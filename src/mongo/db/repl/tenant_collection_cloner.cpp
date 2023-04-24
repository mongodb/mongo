/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_collection_cloner.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {
namespace repl {

namespace {
const int kProgressMeterSecondsBetween = 60;
const int kProgressMeterCheckInterval = 128;
}  // namespace

// Failpoint which causes the tenant database cloner to hang after it has successfully run
// listIndexes and recorded the results and the operationTime.
MONGO_FAIL_POINT_DEFINE(tenantCollectionClonerHangAfterGettingOperationTime);

// Failpoint which causes the tenant collection cloner to hang after createCollection. This
// failpoint doesn't check for cloner exit so we can rely on its timesEntered in tests.
MONGO_FAIL_POINT_DEFINE(tenantCollectionClonerHangAfterCreateCollection);

// Failpoint which causes tenant migration to hang after handling the next batch of results from the
// DBClientConnection, optionally limited to a specific collection.
MONGO_FAIL_POINT_DEFINE(tenantMigrationHangCollectionClonerAfterHandlingBatchResponse);

TenantCollectionCloner::TenantCollectionCloner(const NamespaceString& sourceNss,
                                               const CollectionOptions& collectionOptions,
                                               TenantMigrationSharedData* sharedData,
                                               const HostAndPort& source,
                                               DBClientConnection* client,
                                               StorageInterface* storageInterface,
                                               ThreadPool* dbPool,
                                               StringData tenantId)
    : TenantBaseCloner(
          "TenantCollectionCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _sourceNss(sourceNss),
      _collectionOptions(collectionOptions),
      _sourceDbAndUuid(NamespaceString("UNINITIALIZED")),
      _collectionClonerBatchSize(collectionClonerBatchSize),
      _countStage("count", this, &TenantCollectionCloner::countStage),
      _checkIfDonorCollectionIsEmptyStage(
          "checkIfDonorCollectionIsEmpty",
          this,
          &TenantCollectionCloner::checkIfDonorCollectionIsEmptyStage),
      _listIndexesStage("listIndexes", this, &TenantCollectionCloner::listIndexesStage),
      _createCollectionStage(
          "createCollection", this, &TenantCollectionCloner::createCollectionStage),
      _queryStage("query", this, &TenantCollectionCloner::queryStage),
      _progressMeter(1U,  // total will be replaced with count command result.
                     kProgressMeterSecondsBetween,
                     kProgressMeterCheckInterval,
                     "documents copied",
                     str::stream() << _sourceNss.toString() << " tenant collection clone progress"),
      _tenantId(tenantId) {
    invariant(sourceNss.isValid());
    invariant(ClonerUtils::isNamespaceForTenant(sourceNss, tenantId));
    invariant(collectionOptions.uuid);
    _sourceDbAndUuid = NamespaceStringOrUUID(sourceNss.dbName(), *collectionOptions.uuid);
    _stats.ns = _sourceNss.ns().toString();
}

BaseCloner::ClonerStages TenantCollectionCloner::getStages() {
    return {&_countStage,
            &_checkIfDonorCollectionIsEmptyStage,
            &_listIndexesStage,
            &_createCollectionStage,
            &_queryStage};
}

void TenantCollectionCloner::preStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.start = getSharedData()->getClock()->now();
}

void TenantCollectionCloner::postStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.end = getSharedData()->getClock()->now();
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::TenantCollectionClonerStage::run() {
    try {
        return ClonerStage<TenantCollectionCloner>::run();
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // We can exit this cloner cleanly and move on to the next one.
        LOGV2(5289701,
              "TenantCollectionCloner stopped because collection was dropped on the donor.",
              logAttrs(getCloner()->getSourceNss()),
              "uuid"_attr = getCloner()->getSourceUuid(),
              "tenantId"_attr = getCloner()->getTenantId());
        return kSkipRemainingStages;
    } catch (const DBException&) {
        throw;
    }
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::countStage() {
    auto count =
        getClient()->count(_sourceDbAndUuid,
                           {} /* Query */,
                           QueryOption_SecondaryOk,
                           0 /* limit */,
                           0 /* skip */,
                           ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());

    // The count command may return a negative value after an unclean shutdown,
    // so we set it to zero here to avoid aborting the collection clone.
    // Note that this count value is only used for reporting purposes.
    if (count < 0) {
        LOGV2_WARNING(4884502,
                      "Count command returned negative value. Updating to 0 to allow progress "
                      "meter to function properly",
                      logAttrs(_sourceNss),
                      "tenantId"_attr = _tenantId);
        count = 0;
    }

    BSONObj res;
    getClient()->runCommand(_sourceNss.dbName(), BSON("collStats" << _sourceNss.coll()), res);
    auto status = getStatusFromCommandResult(res);
    if (!status.isOK()) {
        LOGV2_WARNING(5426601,
                      "Skipping recording of data size metrics for collection due to failure in the"
                      " 'collStats' command, tenant migration stats may be inaccurate.",
                      logAttrs(_sourceNss),
                      "migrationId"_attr = getSharedData()->getMigrationId(),
                      "tenantId"_attr = _tenantId,
                      "status"_attr = status);
    }

    _progressMeter.setTotalWhileRunning(static_cast<unsigned long long>(count));
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.documentsToCopyAtStartOfClone = count;
        _stats.approxTotalDataSize = status.isOK() ? res.getField("size").safeNumberLong() : 0;
        _stats.avgObjSize =
            _stats.approxTotalDataSize ? res.getField("avgObjSize").safeNumberLong() : 0;
    }
    return kContinueNormally;
}

// This avoids a race where an index may be created and data inserted after we do listIndexes.
// That would result in doing a createIndexes on a non-empty collection during oplog application.
// Instead, if the collection is empty before listIndexes, we do not clone the data -- it will be
// added during oplog application.
//
// Note we cannot simply use the count() above, because that checks metadata which may not be 100%
// accurate.
BaseCloner::AfterStageBehavior TenantCollectionCloner::checkIfDonorCollectionIsEmptyStage() {
    FindCommandRequest findRequest{_sourceDbAndUuid};
    findRequest.setProjection(BSON("_id" << 1));
    findRequest.setLimit(1);
    findRequest.setReadConcern(
        ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    auto cursor = getClient()->find(std::move(findRequest),
                                    ReadPreferenceSetting{ReadPreference::SecondaryPreferred});
    _donorCollectionWasEmptyBeforeListIndexes = !cursor->more();
    LOGV2_DEBUG(5368500,
                1,
                "Checked if donor collection was empty",
                "wasEmpty"_attr = _donorCollectionWasEmptyBeforeListIndexes,
                logAttrs(_sourceNss),
                "tenantId"_attr = _tenantId);
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::listIndexesStage() {
    // This will be set after a successful listCollections command.
    _operationTime = Timestamp();

    auto indexSpecs = getClient()->getIndexSpecs(
        _sourceDbAndUuid, false /* includeBuildUUIDs */, QueryOption_SecondaryOk);

    // Do a majority read on the sync source to make sure the indexes listed exist on a majority of
    // nodes in the set. We do not check the rollbackId - rollback would lead to the sync source
    // closing connections so the stage would fail.
    _operationTime = getClient()->getOperationTime();

    tenantCollectionClonerHangAfterGettingOperationTime.executeIf(
        [&](const BSONObj&) {
            while (
                MONGO_unlikely(tenantCollectionClonerHangAfterGettingOperationTime.shouldFail()) &&
                !mustExit()) {
                LOGV2(4884509,
                      "tenantCollectionClonerHangAfterGettingOperationTime fail point "
                      "enabled. Blocking until fail point is disabled",
                      logAttrs(_sourceNss),
                      "tenantId"_attr = _tenantId);
                mongo::sleepsecs(1);
            }
        },
        [&](const BSONObj& data) {
            // Only hang when cloning the specified collection, or if no collection was specified.
            auto nss = data["nss"].str();
            return nss.empty() || nss == _sourceNss.toString();
        });

    BSONObj readResult;
    BSONObj cmd = ClonerUtils::buildMajorityWaitRequest(_operationTime);
    getClient()->runCommand(DatabaseName::kAdmin, cmd, readResult, QueryOption_SecondaryOk);
    uassertStatusOKWithContext(
        getStatusFromCommandResult(readResult),
        "TenantCollectionCloner failed to get listIndexes result majority-committed");

    // Process the listIndexes results for finished indexes only.
    if (indexSpecs.empty()) {
        LOGV2_WARNING(4884503,
                      "No indexes found for collection while cloning",
                      logAttrs(_sourceNss),
                      "source"_attr = getSource(),
                      "tenantId"_attr = _tenantId);
    }
    for (auto&& spec : indexSpecs) {
        if (spec.hasField("name") && spec.getStringField("name") == "_id_"_sd) {
            _idIndexSpec = spec.getOwned();
        } else {
            _readyIndexSpecs.push_back(spec.getOwned());
        }
    }
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.indexes = _readyIndexSpecs.size() + (_idIndexSpec.isEmpty() ? 0 : 1);
    };

    // Tenant collections are replicated collections and it's impossible to have an empty _id index
    // and collection options 'autoIndexId' as false except for collections that use clustered
    // index. These are extra sanity checks made on the response received from the remote node.
    uassert(
        ErrorCodes::IllegalOperation,
        str::stream() << "Found empty '_id' index spec but the collection is not specified with "
                         "'autoIndexId' as false, tenantId: "
                      << _tenantId << ", namespace: " << this->_sourceNss.toStringForErrorMsg(),
        _collectionOptions.clusteredIndex || !_idIndexSpec.isEmpty() ||
            _collectionOptions.autoIndexId == CollectionOptions::NO);

    if (!_idIndexSpec.isEmpty() && _collectionOptions.autoIndexId == CollectionOptions::NO) {
        LOGV2_WARNING(4884504,
                      "Found the _id index spec but the collection specified autoIndexId of false",
                      logAttrs(_sourceNss),
                      "tenantId"_attr = _tenantId);
    }
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::createCollectionStage() {
    auto opCtx = cc().makeOperationContext();

    bool skipCreateIndexes = false;

    auto collection =
        CollectionCatalog::get(opCtx.get())->lookupCollectionByUUID(opCtx.get(), getSourceUuid());
    if (collection) {
        uassert(5342500,
                str::stream() << "Collection uuid" << getSourceUuid()
                              << " already exists but does not belong to tenant",
                ClonerUtils::isNamespaceForTenant(collection->ns(), _tenantId));
        uassert(5342501,
                str::stream() << "Collection uuid" << getSourceUuid()
                              << " already exists but does not belong to the same database",
                collection->ns().db() == _sourceNss.db());
        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "Tenant '" << _tenantId << "': collection '"
                              << collection->ns().toStringForErrorMsg()
                              << "' already exists prior to data sync",
                getSharedData()->getResumePhase() == ResumePhase::kDataSync);

        _existingNss = collection->ns();
        LOGV2(5342502,
              "TenantCollectionCloner found collection with same uuid.",
              "existingNamespace"_attr = _existingNss,
              "sourceNamespace"_attr = getSourceNss(),
              "uuid"_attr = getSourceUuid(),
              "migrationId"_attr = getSharedData()->getMigrationId(),
              "tenantId"_attr = getTenantId());

        // We are resuming and the collection already exists.
        DBDirectClient client(opCtx.get());

        // Set the recipient info on the opCtx to bypass the access blocker for local reads.
        tenantMigrationInfo(opCtx.get()) =
            boost::make_optional<TenantMigrationInfo>(getSharedData()->getMigrationId());
        // Reset the recipient info after local reads so oplog entries for future writes
        // (createCollection/createIndex) don't get stamped with the fromTenantMigration field.
        ON_BLOCK_EXIT([&opCtx] { tenantMigrationInfo(opCtx.get()) = boost::none; });

        FindCommandRequest findCmd{*_existingNss};
        findCmd.setSort(BSON("_id" << -1));
        findCmd.setProjection(BSON("_id" << 1));
        _lastDocId = client.findOne(std::move(findCmd));
        if (!_lastDocId.isEmpty()) {
            // The collection is not empty. Skip creating indexes and resume cloning from the last
            // document.
            skipCreateIndexes = true;
            _readyIndexSpecs.clear();
            auto count = client.count(_sourceDbAndUuid);
            {
                stdx::lock_guard<Latch> lk(_mutex);
                _stats.documentsCopied += count;
                _stats.approxTotalBytesCopied = ((long)_stats.documentsCopied) * _stats.avgObjSize;
                _progressMeter.hit(count);
            }
        } else {
            // The collection is still empty. Create indexes that we haven't created. For the
            // indexes that exist locally but not on the donor, we don't need to drop them because
            // oplog application will eventually apply those dropIndex oplog entries.
            const bool includeBuildUUIDs = false;
            const int options = 0;
            auto existingIndexSpecs =
                client.getIndexSpecs(_sourceDbAndUuid, includeBuildUUIDs, options);
            StringMap<bool> existingIndexNames;
            for (const auto& spec : existingIndexSpecs) {
                existingIndexNames[spec.getStringField("name")] = true;
            }
            for (auto it = _readyIndexSpecs.begin(); it != _readyIndexSpecs.end();) {
                if (existingIndexNames[it->getStringField("name")]) {
                    it = _readyIndexSpecs.erase(it);
                } else {
                    it++;
                }
            }
        }
    } else {
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx.get());

        // No collection with the same UUID exists. But if this still fails with NamespaceExists, it
        // means that we have a collection with the same namespace but a different UUID.
        auto status =
            getStorageInterface()->createCollection(opCtx.get(),
                                                    _sourceNss,
                                                    _collectionOptions,
                                                    !_idIndexSpec.isEmpty() /* createIdIndex */,
                                                    _idIndexSpec);
        if (status == ErrorCodes::NamespaceExists &&
            getSharedData()->getResumePhase() == ResumePhase::kDataSync) {
            // If we are resuming from a recipient failover we can get ErrorCodes::NamespaceExists
            // due to following conditions:
            //
            // 1) We have a collection on disk with the same namespace but a different uuid. It
            // means this collection must have been dropped and re-created under a different uuid on
            // the donor during the recipient failover. And the drop and the re-create will be
            // covered by the oplog application phase.
            //
            // 2) We have a [time series] view on disk with the same namespace. It means the view
            // must have dropped and created a regular collection with the namespace same as the
            // dropped view during the recipient failover. The drop view and create collection
            // will be covered by the oplog application phase.
            LOGV2(5767200,
                  "Tenant collection cloner: Skipping cloning this collection.",
                  logAttrs(getSourceNss()),
                  "migrationId"_attr = getSharedData()->getMigrationId(),
                  "tenantId"_attr = getTenantId(),
                  "error"_attr = status);
            return kSkipRemainingStages;
        }
        uassertStatusOKWithContext(status, "Tenant collection cloner: create collection");
    }

    if (!skipCreateIndexes) {
        // This will start building the indexes whose specs we saved last stage.
        auto status = getStorageInterface()->createIndexesOnEmptyCollection(
            opCtx.get(), _existingNss.value_or(_sourceNss), _readyIndexSpecs);

        uassertStatusOKWithContext(status, "Tenant collection cloner: create indexes");
    }

    tenantCollectionClonerHangAfterCreateCollection.pauseWhileSet();
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::queryStage() {
    if (_donorCollectionWasEmptyBeforeListIndexes) {
        LOGV2_WARNING(5368501,
                      "Collection was empty at clone time.",
                      logAttrs(_sourceNss),
                      "tenantId"_attr = _tenantId);
        return kContinueNormally;
    }

    // Sets up tracking the lastVisibleOpTime from response metadata.
    auto requestMetadataWriter = [this](OperationContext* opCtx,
                                        BSONObjBuilder* metadataBob) -> Status {
        *metadataBob << rpc::kReplSetMetadataFieldName << 1;
        return Status::OK();
    };
    auto replyMetadataReader =
        [this](OperationContext* opCtx, const BSONObj& metadataObj, StringData source) -> Status {
        auto readResult = rpc::ReplSetMetadata::readFromMetadata(metadataObj);
        if (readResult.isOK()) {
            stdx::lock_guard<TenantMigrationSharedData> lk(*getSharedData());
            getSharedData()->setLastVisibleOpTime(lk, readResult.getValue().getLastOpVisible());
            return Status::OK();
        }
        if (readResult.getStatus() == ErrorCodes::NoSuchKey) {
            // Some responses may not carry this information (e.g. reconnecting to verify a drop).
            LOGV2_DEBUG(5328200,
                        1,
                        "No repl metadata found in response",
                        "data"_attr = redact(metadataObj));
            return Status::OK();
        }
        return readResult.getStatus().withContext(
            "tenant collection cloner failed to read repl set metadata");
    };
    ScopedMetadataWriterAndReader mwr(getClient(), requestMetadataWriter, replyMetadataReader);

    runQuery();
    return kContinueNormally;
}

void TenantCollectionCloner::runQuery() {
    FindCommandRequest findCmd{_sourceDbAndUuid};

    findCmd.setFilter(
        _lastDocId.isEmpty()
            ? BSONObj{}  // Use $expr and the aggregation version of $gt to avoid type bracketing.
            : BSON("$expr" << BSON("$gt" << BSON_ARRAY("$_id" << _lastDocId["_id"]))));

    if (_collectionOptions.clusteredIndex) {
        findCmd.setHint(BSON("$natural" << 1));
    } else {
        findCmd.setHint(BSON("_id" << 1));
    }

    findCmd.setNoCursorTimeout(true);
    findCmd.setReadConcern(ReadConcernArgs(ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    if (_collectionClonerBatchSize) {
        findCmd.setBatchSize(_collectionClonerBatchSize);
    }

    ExhaustMode exhaustMode = collectionClonerUsesExhaust ? ExhaustMode::kOn : ExhaustMode::kOff;

    auto cursor = getClient()->find(
        std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryPreferred}, exhaustMode);

    // Process the results of the cursor one batch at a time.
    while (cursor->more()) {
        handleNextBatch(*cursor);
    }
}

void TenantCollectionCloner::handleNextBatch(DBClientCursor& cursor) {
    std::vector<BSONObj> docsToInsert;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.receivedBatches++;
        while (cursor.moreInCurrentBatch()) {
            docsToInsert.emplace_back(cursor.nextSafe());
        }
    }

    insertDocuments(std::move(docsToInsert));

    tenantMigrationHangCollectionClonerAfterHandlingBatchResponse.executeIf(
        [&](const BSONObj&) {
            while (
                MONGO_unlikely(
                    tenantMigrationHangCollectionClonerAfterHandlingBatchResponse.shouldFail()) &&
                !mustExit()) {
                LOGV2(4884506,
                      "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse fail point "
                      "enabled. Blocking until fail point is disabled",
                      logAttrs(_sourceNss),
                      "tenantId"_attr = _tenantId);
                mongo::sleepsecs(1);
            }
        },
        [&](const BSONObj& data) {
            // Only hang when cloning the specified collection, or if no collection was specified.
            auto nss = data["nss"].str();
            return nss.empty() || nss == _sourceNss.toString();
        });
}


void TenantCollectionCloner::insertDocuments(std::vector<BSONObj> docsToInsert) {
    invariant(docsToInsert.size(),
              "Document size can't be non-zero:: namespace: {}, tenantId: {}"_format(
                  _sourceNss.toString(), _tenantId));

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.documentsCopied += docsToInsert.size();
        _stats.approxTotalBytesCopied = ((long)_stats.documentsCopied) * _stats.avgObjSize;
        ++_stats.insertedBatches;
        _progressMeter.hit(int(docsToInsert.size()));
    }

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();

    // Disabling the internal document validation for inserts on recipient side as those
    // validations should have already been performed on donor's primary during tenant
    // collection document insertion.
    DisableDocumentValidation documentValidationDisabler(
        opCtx,
        DocumentValidationSettings::kDisableSchemaValidation |
            DocumentValidationSettings::kDisableInternalValidation);

    write_ops::InsertCommandRequest insertOp(_existingNss.value_or(_sourceNss));
    insertOp.setDocuments(std::move(docsToInsert));
    insertOp.setWriteCommandRequestBase([] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(true);
        return wcb;
    }());

    // Set the recipient info on the opCtx to skip checking user permissions in
    // 'write_ops_exec::performInserts()'.
    tenantMigrationInfo(opCtx) =
        boost::make_optional<TenantMigrationInfo>(getSharedData()->getMigrationId());

    // write_ops_exec::PerformInserts() will handle limiting the batch size
    // that gets inserted in a single WUOW.
    auto writeResult = write_ops_exec::performInserts(opCtx, insertOp);
    invariant(!writeResult.results.empty());
    // Since the writes are ordered, it's ok to check just the last writeOp result.
    uassertStatusOKWithContext(writeResult.results.back(),
                               "Tenant collection cloner: insert documents");
}

bool TenantCollectionCloner::isMyFailPoint(const BSONObj& data) const {
    auto nss = data["nss"].str();
    return (nss.empty() || nss == _sourceNss.toString()) && BaseCloner::isMyFailPoint(data);
}

TenantCollectionCloner::Stats TenantCollectionCloner::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _stats;
}

std::string TenantCollectionCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj TenantCollectionCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.append("ns", ns);
    append(&bob);
    return bob.obj();
}

void TenantCollectionCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber(kDocumentsToCopyFieldName,
                          static_cast<long long>(documentsToCopyAtStartOfClone));
    builder->appendNumber(kDocumentsCopiedFieldName, static_cast<long long>(documentsCopied));
    builder->appendNumber("indexes", static_cast<long long>(indexes));
    builder->appendNumber("insertedBatches", static_cast<long long>(insertedBatches));
    if (start != Date_t()) {
        builder->appendDate("start", start);
        if (end != Date_t()) {
            builder->appendDate("end", end);
            auto elapsed = end - start;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("elapsedMillis", elapsedMillis);
        }
    }
    builder->appendNumber("receivedBatches", static_cast<long long>(receivedBatches));
}

Timestamp TenantCollectionCloner::getOperationTime_forTest() {
    return _operationTime;
}


}  // namespace repl
}  // namespace mongo
