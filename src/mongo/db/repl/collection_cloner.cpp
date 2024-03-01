/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <cstdint>
#include <list>
#include <mutex>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationInitialSync


namespace mongo {
namespace repl {

// Failpoint which causes initial sync to hang when it has cloned 'numDocsToClone' documents to
// collection 'namespace'.
MONGO_FAIL_POINT_DEFINE(initialSyncHangDuringCollectionClone);

// Failpoint which causes initial sync to hang after handling the next batch of results from the
// DBClientConnection, optionally limited to a specific collection.
MONGO_FAIL_POINT_DEFINE(initialSyncHangCollectionClonerAfterHandlingBatchResponse);

CollectionCloner::CollectionCloner(const NamespaceString& sourceNss,
                                   const CollectionOptions& collectionOptions,
                                   InitialSyncSharedData* sharedData,
                                   const HostAndPort& source,
                                   DBClientConnection* client,
                                   StorageInterface* storageInterface,
                                   ThreadPool* dbPool)
    : InitialSyncBaseCloner(
          "CollectionCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _sourceNss(sourceNss),
      _collectionOptions(collectionOptions),
      _sourceDbAndUuid(NamespaceString::kEmpty),
      _collectionClonerBatchSize(collectionClonerBatchSize),
      _countStage("count", this, &CollectionCloner::countStage),
      _listIndexesStage("listIndexes", this, &CollectionCloner::listIndexesStage),
      _createCollectionStage("createCollection", this, &CollectionCloner::createCollectionStage),
      _queryStage("query", this, &CollectionCloner::queryStage),
      _setupIndexBuildersForUnfinishedIndexesStage(
          "setupIndexBuildersForUnfinishedIndexes",
          this,
          &CollectionCloner::setupIndexBuildersForUnfinishedIndexesStage),
      _progressMeter(1U,  // total will be replaced with count command result.
                     kProgressMeterSecondsBetween,
                     kProgressMeterCheckInterval,
                     "documents copied",
                     str::stream() << NamespaceStringUtil::serialize(
                                          _sourceNss, SerializationContext::stateDefault())
                                   << " collection clone progress"),
      _scheduleDbWorkFn([this](executor::TaskExecutor::CallbackFn work) {
          auto task = [this, work = std::move(work)](
                          OperationContext* opCtx,
                          const Status& status) mutable noexcept -> TaskRunner::NextAction {
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
      _dbWorkTaskRunner(dbPool) {
    invariant(sourceNss.isValid());
    invariant(collectionOptions.uuid);
    _sourceDbAndUuid = NamespaceStringOrUUID(sourceNss.dbName(), *collectionOptions.uuid);
    _stats.nss = _sourceNss;
}

BaseCloner::ClonerStages CollectionCloner::getStages() {
    if (_sourceNss.isChangeStreamPreImagesCollection() || _sourceNss.isChangeCollection()) {
        // The change stream pre-images collection and the change collection only need to be created
        // - their documents should not be copied.
        return {&_listIndexesStage,
                &_createCollectionStage,
                &_setupIndexBuildersForUnfinishedIndexesStage};
    }
    return {&_countStage,
            &_listIndexesStage,
            &_createCollectionStage,
            &_queryStage,
            &_setupIndexBuildersForUnfinishedIndexesStage};
}


void CollectionCloner::preStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.start = getSharedData()->getClock()->now();

    BSONObjBuilder b(BSON("collStats" << _sourceNss.coll().toString()));

    BSONObj res;
    getClient()->runCommand(_sourceNss.dbName(), b.obj(), res);
    if (auto status = getStatusFromCommandResult(res); status.isOK()) {
        _stats.bytesToCopy = res.getField("size").safeNumberLong();
        if (_stats.bytesToCopy > 0) {
            // The 'avgObjSize' parameter is only available if 'collStats' returns a 'size' field
            // greater than zero.
            _stats.avgObjSize = res.getField("avgObjSize").safeNumberLong();
        }
    } else {
        LOGV2_DEBUG(4786302,
                    1,
                    "Skipping the recording of some initial sync metrics due to failure in the "
                    "'collStats' command",
                    logAttrs(_sourceNss),
                    "status"_attr = status);
    }
}

void CollectionCloner::postStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.end = getSharedData()->getClock()->now();
}

// Collection cloner stages exit normally if the collection is not found.
BaseCloner::AfterStageBehavior CollectionCloner::CollectionClonerStage::run() {
    try {
        return ClonerStage<CollectionCloner>::run();
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        LOGV2(21132,
              "CollectionCloner stopped because collection was dropped on source",
              logAttrs(getCloner()->getSourceNss()),
              "uuid"_attr = getCloner()->getSourceUuid());
        getCloner()->waitForDatabaseWorkToComplete();
        return kSkipRemainingStages;
    } catch (const DBException&) {
        getCloner()->waitForDatabaseWorkToComplete();
        throw;
    }
}

BaseCloner::AfterStageBehavior CollectionCloner::countStage() {
    auto count = getClient()->count(_sourceDbAndUuid,
                                    {} /* Query */,
                                    QueryOption_SecondaryOk,
                                    0 /* limit */,
                                    0 /* skip */,
                                    ReadConcernArgs::kLocal);

    // The count command may return a negative value after an unclean shutdown,
    // so we set it to zero here to avoid aborting the collection clone.
    // Note that this count value is only used for reporting purposes.
    if (count < 0) {
        LOGV2_WARNING(21142,
                      "Count command returned negative value. Updating to 0 to allow progress "
                      "meter to function properly");
        count = 0;
    }

    _progressMeter.setTotalWhileRunning(static_cast<unsigned long long>(count));
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.documentToCopy = count;
    }
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior CollectionCloner::listIndexesStage() {
    const bool includeBuildUUIDs = true;

    auto indexSpecs =
        getClient()->getIndexSpecs(_sourceDbAndUuid, includeBuildUUIDs, QueryOption_SecondaryOk);
    if (indexSpecs.empty()) {
        LOGV2_WARNING(21143,
                      "No indexes found for collection while cloning",
                      logAttrs(_sourceNss),
                      "source"_attr = getSource());
    }

    const auto storageEngine = getGlobalServiceContext()->getStorageEngine();
    // Parse the index specs into their respective state, ready or unfinished.
    for (auto&& spec : indexSpecs) {
        // Sanitize storage engine options to remove options which might not apply to this node. See
        // SERVER-68122.
        if (auto storageEngineElem = spec.getField(IndexDescriptor::kStorageEngineFieldName)) {
            auto sanitizedStorageEngineOpts =
                storageEngine->getSanitizedStorageOptionsForSecondaryReplication(
                    storageEngineElem.embeddedObject());
            spec = spec.addFields(
                BSON(IndexDescriptor::kStorageEngineFieldName << sanitizedStorageEngineOpts));
        }

        if (spec.hasField("clustered")) {
            invariant(_collectionOptions.clusteredIndex);
            invariant(spec.getBoolField("clustered") == true);
            invariant(clustered_util::formatClusterKeyForListIndexes(
                          _collectionOptions.clusteredIndex.value(),
                          _collectionOptions.collation,
                          _collectionOptions.expireAfterSeconds)
                          .woCompare(spec) == 0);
            // Skip if the spec is for the collection's clusteredIndex.
        } else if (spec.hasField("buildUUID")) {
            _unfinishedIndexSpecs.push_back(spec.getOwned());
        } else if (spec.hasField("name") && spec.getStringField("name") == "_id_"_sd) {
            _idIndexSpec = spec.getOwned();
        } else {
            _readyIndexSpecs.push_back(spec.getOwned());
        }
    }

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.indexes = _readyIndexSpecs.size() + _unfinishedIndexSpecs.size() +
            (_idIndexSpec.isEmpty() ? 0 : 1);
    };

    if (!_idIndexSpec.isEmpty() && _collectionOptions.autoIndexId == CollectionOptions::NO) {
        LOGV2_WARNING(21144,
                      "Found the _id index spec but the collection specified autoIndexId of false",
                      logAttrs(this->_sourceNss));
    }
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior CollectionCloner::createCollectionStage() {
    auto collectionBulkLoader = getStorageInterface()->createCollectionForBulkLoading(
        _sourceNss, _collectionOptions, _idIndexSpec, _readyIndexSpecs);
    uassertStatusOK(collectionBulkLoader.getStatus());
    _collLoader = std::move(collectionBulkLoader.getValue());
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior CollectionCloner::queryStage() {
    runQuery();
    waitForDatabaseWorkToComplete();
    // We want to free the _collLoader regardless of whether the commit succeeds.
    std::unique_ptr<CollectionBulkLoader> loader = std::move(_collLoader);
    uassertStatusOK(loader->commit());
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior CollectionCloner::setupIndexBuildersForUnfinishedIndexesStage() {
    if (_unfinishedIndexSpecs.empty()) {
        return kContinueNormally;
    }

    // Need to group the index specs by 'buildUUID' and start all the index specs with the same
    // 'buildUUID' on the same index builder thread.
    stdx::unordered_map<UUID, std::vector<BSONObj>, UUID::Hash> groupedIndexSpecs;
    for (const auto& unfinishedSpec : _unfinishedIndexSpecs) {
        UUID buildUUID = uassertStatusOK(UUID::parse(unfinishedSpec["buildUUID"]));
        groupedIndexSpecs[buildUUID].push_back(unfinishedSpec["spec"].Obj());
    }

    auto opCtx = cc().makeOperationContext();

    for (const auto& groupedIndexSpec : groupedIndexSpecs) {
        std::vector<std::string> indexNames;
        std::vector<BSONObj> indexSpecs;
        for (const auto& indexSpec : groupedIndexSpec.second) {
            std::string indexName =
                indexSpec.getStringField(IndexDescriptor::kIndexNameFieldName).toString();
            indexNames.push_back(indexName);
            indexSpecs.push_back(indexSpec.getOwned());
        }

        UnreplicatedWritesBlock uwb(opCtx.get());

        // This spawns a new thread and returns immediately once the index build has been
        // registered with the IndexBuildsCoordinator.
        try {
            IndexBuildsCoordinator::get(opCtx.get())
                ->applyStartIndexBuild(opCtx.get(),
                                       IndexBuildsCoordinator::ApplicationMode::kInitialSync,
                                       {getSourceUuid(),
                                        repl::OplogEntry::CommandType::kStartIndexBuild,
                                        "createIndexes",
                                        groupedIndexSpec.first,
                                        std::move(indexNames),
                                        std::move(indexSpecs),
                                        boost::none});
        } catch (const ExceptionFor<ErrorCodes::IndexAlreadyExists>&) {
            // Suppress the IndexAlreadyExists error code.
            // It's possible for the DBDirectClient to return duplicate index specs with different
            // buildUUIDs from the sync source due to getMore() making multiple network calls.
            // In these cases, we can ignore this error as the oplog replay phase will correctly
            // abort and start the appropriate indexes.
            // Example:
            // - listIndexes on the sync source sees x_1 (ready: false) with buildUUID ‘x’.
            // - Sync source aborts the index build with buildUUID ‘x’.
            // - Sync source starts x_1 (ready: false) with buildUUID ‘y’.
            // - getMore on listIndexes sees x_1 with buildUUID 'y'.
        }
    }

    return kContinueNormally;
}

void CollectionCloner::runQuery() {
    FindCommandRequest findCmd{_sourceDbAndUuid};

    if (_resumeToken) {
        // Resume the query from where we left off.
        LOGV2_DEBUG(21133, 1, "Collection cloner will resume the last successful query");
        findCmd.setRequestResumeToken(true);
        findCmd.setResumeAfter(_resumeToken.value());
    } else {
        // New attempt at a resumable query.
        LOGV2_DEBUG(21134, 1, "Collection cloner will run a new query");
        findCmd.setRequestResumeToken(true);
    }

    findCmd.setHint(BSON("$natural" << 1));
    findCmd.setNoCursorTimeout(true);
    findCmd.setReadConcern(ReadConcernArgs::kLocal);
    if (_collectionClonerBatchSize) {
        findCmd.setBatchSize(_collectionClonerBatchSize);
    }

    ExhaustMode exhaustMode = collectionClonerUsesExhaust ? ExhaustMode::kOn : ExhaustMode::kOff;

    if (_collectionOptions.recordIdsReplicated) {
        // The below projection returns a stream of documents in the format
        // {r: <recordId>, d: <original document>}.
        auto projection = BSON("_id" << 0 << "r"
                                     << BSON("$meta"
                                             << "recordId")
                                     << "d"
                                     << "$$ROOT");
        findCmd.setProjection(std::move(projection));
    }

    // We reset this every time we retry or resume a query.
    // We distinguish the first batch from the rest so that we only store the remote cursor id
    // the first time we get it.
    _firstBatchOfQueryRound = true;

    auto cursor = getClient()->find(
        std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryPreferred}, exhaustMode);

    // Process the results of the cursor one batch at a time.
    while (cursor->more()) {
        handleNextBatch(*cursor);
    }
}

void CollectionCloner::handleNextBatch(DBClientCursor& cursor) {
    {
        stdx::lock_guard<InitialSyncSharedData> lk(*getSharedData());
        if (!getSharedData()->getStatus(lk).isOK()) {
            static constexpr char message[] =
                "Collection cloning cancelled due to initial sync failure";
            LOGV2(21136, message, "error"_attr = getSharedData()->getStatus(lk));
            uasserted(ErrorCodes::CallbackCanceled,
                      str::stream() << message << ": " << getSharedData()->getStatus(lk));
        }
    }

    // If this is 'true', it means that something happened to our remote cursor for a reason other
    // than the collection being dropped, all while we were running a non-resumable (4.2) clone.
    // We must abort initial sync in that case.
    if (_lostNonResumableCursor) {
        // This will be caught in runQuery().
        uasserted(ErrorCodes::InitialSyncFailure, "Lost remote cursor");
    }

    if (_firstBatchOfQueryRound) {
        // Store the cursorId of the remote cursor.
        _remoteCursorId = cursor.getCursorId();
    }
    _firstBatchOfQueryRound = false;

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.receivedBatches++;
        while (cursor.moreInCurrentBatch()) {
            _documentsToInsert.emplace_back(cursor.nextSafe());
        }
    }

    // Schedule the next document batch insertion.
    auto&& scheduleResult =
        _scheduleDbWorkFn([=, this](const executor::TaskExecutor::CallbackArgs& cbd) {
            insertDocumentsCallback(cbd);
        });

    if (!scheduleResult.isOK()) {
        Status newStatus = scheduleResult.getStatus().withContext(
            str::stream() << "Error cloning collection '" << _sourceNss.toStringForErrorMsg()
                          << "'");
        // We must throw an exception to terminate query.
        uassertStatusOK(newStatus);
    }

    // Store the resume token for this batch.
    _resumeToken = cursor.getPostBatchResumeToken();

    initialSyncHangCollectionClonerAfterHandlingBatchResponse.executeIf(
        [&](const BSONObj&) {
            while (MONGO_unlikely(
                       initialSyncHangCollectionClonerAfterHandlingBatchResponse.shouldFail()) &&
                   !mustExit()) {
                LOGV2(21137,
                      "initialSyncHangCollectionClonerAfterHandlingBatchResponse fail point "
                      "enabled. Blocking until fail point is disabled",
                      logAttrs(_sourceNss));
                mongo::sleepsecs(1);
            }
        },
        [&](const BSONObj& data) {
            const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "nss"_sd);
            // Only hang when cloning the specified collection, or if no collection was specified.
            return fpNss.isEmpty() || fpNss == _sourceNss;
        });
}

void CollectionCloner::insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& cbd) {
    uassertStatusOK(cbd.status);
    {
        stdx::lock_guard<Latch> lk(_mutex);
        std::vector<BSONObj> docs;
        // Increment 'fetchedBatches' even if no documents were inserted to match the number of
        // 'receivedBatches'.
        ++_stats.fetchedBatches;
        if (_documentsToInsert.size() == 0) {
            LOGV2_WARNING(
                21145, "insertDocumentsCallback, but no documents to insert", logAttrs(_sourceNss));
            return;
        }
        _documentsToInsert.swap(docs);
        _stats.documentsCopied += docs.size();
        _stats.approxBytesCopied = ((long)_stats.documentsCopied) * _stats.avgObjSize;
        _progressMeter.hit(int(docs.size()));
        invariant(_collLoader);

        CollectionBulkLoader::ParseRecordIdAndDocFunc fn = (_collectionOptions.recordIdsReplicated)
            ? ([](const BSONObj& doc) {
                  return std::make_pair(RecordId(doc["r"].Long()), doc["d"].Obj());
              })
            : ([](const BSONObj& doc) { return std::make_pair(RecordId(0), doc); });
        // The insert must be done within the lock, because CollectionBulkLoader is not
        // thread safe.
        uassertStatusOK(_collLoader->insertDocuments(docs.cbegin(), docs.cend(), fn));
    }

    initialSyncHangDuringCollectionClone.executeIf(
        [&](const BSONObj&) {
            LOGV2(21138,
                  "initial sync - initialSyncHangDuringCollectionClone fail point "
                  "enabled. Blocking until fail point is disabled");
            while (MONGO_unlikely(initialSyncHangDuringCollectionClone.shouldFail()) &&
                   !mustExit()) {
                mongo::sleepsecs(1);
            }
        },
        [&](const BSONObj& data) {
            return NamespaceStringUtil::parseFailPointData(data, "namespace") == _sourceNss &&
                static_cast<int>(_stats.documentsCopied) >= data["numDocsToClone"].numberInt();
        });
}

bool CollectionCloner::isMyFailPoint(const BSONObj& data) const {
    const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "nss"_sd);
    return (fpNss.isEmpty() || fpNss == _sourceNss) && BaseCloner::isMyFailPoint(data);
}

void CollectionCloner::waitForDatabaseWorkToComplete() {
    _dbWorkTaskRunner.join();
}

CollectionCloner::Stats CollectionCloner::getStats() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _stats;
}

std::string CollectionCloner::Stats::toString() const {
    return toBSON().toString();
}


BSONObj CollectionCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.append("ns", NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    append(&bob);
    return bob.obj();
}

void CollectionCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber(kDocumentsToCopyFieldName, static_cast<long long>(documentToCopy));
    builder->appendNumber(kDocumentsCopiedFieldName, static_cast<long long>(documentsCopied));
    builder->appendNumber("indexes", static_cast<long long>(indexes));
    builder->appendNumber("fetchedBatches", static_cast<long long>(fetchedBatches));
    builder->appendNumber("bytesToCopy", bytesToCopy);
    if (bytesToCopy) {
        builder->appendNumber("approxBytesCopied", approxBytesCopied);
    }
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

}  // namespace repl
}  // namespace mongo
