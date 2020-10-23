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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/platform/basic.h"

#include "mongo/base/string_data.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_collection_cloner.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

namespace {
const int kProgressMeterSecondsBetween = 60;
const int kProgressMeterCheckInterval = 128;
}  // namespace

// Failpoint which causes the tenant database cloner to hang after it has successfully run
// listIndexes and recorded the results and the operationTime.
MONGO_FAIL_POINT_DEFINE(tenantCollectionClonerHangAfterGettingOperationTime);

// Failpoint which causes tenant migration to hang after handling the next batch of results from the
// DBClientConnection, optionally limited to a specific collection.
MONGO_FAIL_POINT_DEFINE(tenantMigrationHangCollectionClonerAfterHandlingBatchResponse);

// Failpoint which causes tenant migration to hang when it has cloned 'numDocsToClone' documents to
// collection 'namespace'.
MONGO_FAIL_POINT_DEFINE(tenantMigrationHangDuringCollectionClone);

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
      _listIndexesStage("listIndexes", this, &TenantCollectionCloner::listIndexesStage),
      _createCollectionStage(
          "createCollection", this, &TenantCollectionCloner::createCollectionStage),
      _queryStage("query", this, &TenantCollectionCloner::queryStage),
      _progressMeter(1U,  // total will be replaced with count command result.
                     kProgressMeterSecondsBetween,
                     kProgressMeterCheckInterval,
                     "documents copied",
                     str::stream() << _sourceNss.toString() << " tenant collection clone progress"),
      _scheduleDbWorkFn([this](executor::TaskExecutor::CallbackFn work) {
          auto task = [ this, work = std::move(work) ](
                          OperationContext * opCtx,
                          const Status& status) mutable noexcept->TaskRunner::NextAction {
              try {
                  work(executor::TaskExecutor::CallbackArgs(nullptr, {}, status, opCtx));
              } catch (const DBException& e) {
                  setSyncFailedStatus(e.toStatus());
              }
              return TaskRunner::NextAction::kDisposeOperationContext;
          };
          _dbWorkTaskRunner.schedule(std::move(task));
          return executor::TaskExecutor::CallbackHandle();
      }),
      _dbWorkTaskRunner(dbPool),
      _tenantId(tenantId) {
    invariant(sourceNss.isValid());
    invariant(collectionOptions.uuid);
    _sourceDbAndUuid = NamespaceStringOrUUID(sourceNss.db().toString(), *collectionOptions.uuid);
    _stats.ns = _sourceNss.ns();
}

BaseCloner::ClonerStages TenantCollectionCloner::getStages() {
    return {&_countStage, &_listIndexesStage, &_createCollectionStage, &_queryStage};
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
    } catch (const DBException&) {
        getCloner()->waitForDatabaseWorkToComplete();
        throw;
    }
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::countStage() {
    auto count = getClient()->count(_sourceDbAndUuid,
                                    {} /* Query */,
                                    QueryOption_SlaveOk,
                                    0 /* limit */,
                                    0 /* skip */,
                                    ReadConcernArgs::kImplicitDefault);

    // The count command may return a negative value after an unclean shutdown,
    // so we set it to zero here to avoid aborting the collection clone.
    // Note that this count value is only used for reporting purposes.
    if (count < 0) {
        LOGV2_WARNING(4884502,
                      "Count command returned negative value. Updating to 0 to allow progress "
                      "meter to function properly",
                      "namespace"_attr = _sourceNss.ns(),
                      "tenantId"_attr = _tenantId);
        count = 0;
    }

    _progressMeter.setTotalWhileRunning(static_cast<unsigned long long>(count));
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.documentToCopy = count;
    }
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::listIndexesStage() {
    // This will be set after a successful listCollections command.
    _operationTime = Timestamp();

    auto indexSpecs = getClient()->getIndexSpecs(
        _sourceDbAndUuid, false /* includeBuildUUIDs */, QueryOption_SlaveOk);

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
                      "namespace"_attr = _sourceNss.toString(),
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
    getClient()->runCommand("admin", cmd, readResult, QueryOption_SlaveOk);
    uassertStatusOKWithContext(
        getStatusFromCommandResult(readResult),
        "TenantCollectionCloner failed to get listIndexes result majority-committed");

    // Process the listIndexes results for finished indexes only.
    if (indexSpecs.empty()) {
        LOGV2_WARNING(4884503,
                      "No indexes found for collection while cloning",
                      "namespace"_attr = _sourceNss.ns(),
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

    if (!_idIndexSpec.isEmpty() && _collectionOptions.autoIndexId == CollectionOptions::NO) {
        LOGV2_WARNING(4884504,
                      "Found the _id index spec but the collection specified autoIndexId of false",
                      "namespace"_attr = this->_sourceNss,
                      "tenantId"_attr = _tenantId);
    }
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::createCollectionStage() {
    auto opCtx = cc().makeOperationContext();

    auto status =
        getStorageInterface()->createCollection(opCtx.get(), _sourceNss, _collectionOptions);
    if (status == ErrorCodes::NamespaceExists) {
        uassert(4884501,
                "Collection exists but does not belong to tenant",
                ClonerUtils::isNamespaceForTenant(_sourceNss, _tenantId));
    } else {
        uassertStatusOKWithContext(status, "Tenant collection cloner: create collection");
    }

    // This will start building the indexes whose specs we saved last stage.
    status = getStorageInterface()->createIndexesOnEmptyCollection(
        opCtx.get(), _sourceNss, _readyIndexSpecs);

    uassertStatusOKWithContext(status, "Tenant collection cloner: create indexes");

    return kContinueNormally;
}

BaseCloner::AfterStageBehavior TenantCollectionCloner::queryStage() {
    // Sets up tracking the lastVisibleOpTime from response metadata.
    auto requestMetadataWriter = [this](OperationContext* opCtx,
                                        BSONObjBuilder* metadataBob) -> Status {
        *metadataBob << rpc::kReplSetMetadataFieldName << 1;
        return Status::OK();
    };
    auto replyMetadataReader =
        [this](OperationContext* opCtx, const BSONObj& metadataObj, StringData source) -> Status {
        auto readResult = rpc::ReplSetMetadata::readFromMetadata(metadataObj);
        if (!readResult.isOK()) {
            return readResult.getStatus().withContext(
                "tenant collection cloner failed to read repl set metadata");
        }
        stdx::lock_guard<TenantMigrationSharedData> lk(*getSharedData());
        getSharedData()->setLastVisibleOpTime(lk, readResult.getValue().getLastOpVisible());
        return Status::OK();
    };
    ScopedMetadataWriterAndReader mwr(getClient(), requestMetadataWriter, replyMetadataReader);

    runQuery();
    waitForDatabaseWorkToComplete();
    return kContinueNormally;
}

void TenantCollectionCloner::runQuery() {
    auto query = QUERY("query" << BSONObj());
    query.hint(BSON("_id" << 1));

    getClient()->query([this](DBClientCursorBatchIterator& iter) { handleNextBatch(iter); },
                       _sourceDbAndUuid,
                       query,
                       nullptr /* fieldsToReturn */,
                       QueryOption_NoCursorTimeout | QueryOption_SlaveOk |
                           (collectionClonerUsesExhaust ? QueryOption_Exhaust : 0),
                       _collectionClonerBatchSize);
    _dbWorkTaskRunner.join();
}

void TenantCollectionCloner::handleNextBatch(DBClientCursorBatchIterator& iter) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.receivedBatches++;
        while (iter.moreInCurrentBatch()) {
            _documentsToInsert.emplace_back(InsertStatement(iter.nextSafe()));
        }
    }

    // Schedule the next document batch insertion.
    auto&& scheduleResult = _scheduleDbWorkFn(
        [=](const executor::TaskExecutor::CallbackArgs& cbd) { insertDocumentsCallback(cbd); });

    if (!scheduleResult.isOK()) {
        Status newStatus = scheduleResult.getStatus().withContext(
            str::stream() << "Error cloning collection '" << _sourceNss.ns() << "'");
        // We must throw an exception to terminate query.
        uassertStatusOK(newStatus);
    }

    tenantMigrationHangCollectionClonerAfterHandlingBatchResponse.executeIf(
        [&](const BSONObj&) {
            while (
                MONGO_unlikely(
                    tenantMigrationHangCollectionClonerAfterHandlingBatchResponse.shouldFail()) &&
                !mustExit()) {
                LOGV2(4884506,
                      "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse fail point "
                      "enabled. Blocking until fail point is disabled",
                      "namespace"_attr = _sourceNss.toString(),
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


void TenantCollectionCloner::insertDocumentsCallback(
    const executor::TaskExecutor::CallbackArgs& cbd) {
    uassertStatusOK(cbd.status);
    std::vector<InsertStatement> docs;

    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (_documentsToInsert.size() == 0) {
            LOGV2_WARNING(4884507,
                          "insertDocumentsCallback, but no documents to insert",
                          "namespace"_attr = _sourceNss,
                          "tenantId"_attr = _tenantId);
            return;
        }
        _documentsToInsert.swap(docs);
        _stats.documentsCopied += docs.size();
        ++_stats.insertedBatches;
        _progressMeter.hit(int(docs.size()));
    }

    uassertStatusOK(getStorageInterface()->insertDocuments(cbd.opCtx, _sourceDbAndUuid, docs));

    tenantMigrationHangDuringCollectionClone.executeIf(
        [&](const BSONObj&) {
            LOGV2(4884508,
                  "initial sync - tenantMigrationHangDuringCollectionClone fail point "
                  "enabled. Blocking until fail point is disabled",
                  "namespace"_attr = _sourceNss.ns(),
                  "tenantId"_attr = _tenantId);
            while (MONGO_unlikely(tenantMigrationHangDuringCollectionClone.shouldFail()) &&
                   !mustExit()) {
                mongo::sleepsecs(1);
            }
        },
        [&](const BSONObj& data) {
            return data["namespace"].String() == _sourceNss.ns() &&
                static_cast<int>(_stats.documentsCopied) >= data["numDocsToClone"].numberInt();
        });
}

void TenantCollectionCloner::waitForDatabaseWorkToComplete() {
    _dbWorkTaskRunner.join();
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
    builder->appendNumber(kDocumentsToCopyFieldName, documentToCopy);
    builder->appendNumber(kDocumentsCopiedFieldName, documentsCopied);
    builder->appendNumber("indexes", indexes);
    builder->appendNumber("insertedBatches", insertedBatches);
    if (start != Date_t()) {
        builder->appendDate("start", start);
        if (end != Date_t()) {
            builder->appendDate("end", end);
            auto elapsed = end - start;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("elapsedMillis", elapsedMillis);
        }
    }
    builder->appendNumber("receivedBatches", receivedBatches);
}

Timestamp TenantCollectionCloner::getOperationTime_forTest() {
    return _operationTime;
}


}  // namespace repl
}  // namespace mongo
