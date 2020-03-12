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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplicationInitialSync

#include "mongo/platform/basic.h"

#include "mongo/base/string_data.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/collection_bulk_loader.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

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
    : BaseCloner("CollectionCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _sourceNss(sourceNss),
      _collectionOptions(collectionOptions),
      _sourceDbAndUuid(NamespaceString("UNINITIALIZED")),
      _collectionClonerBatchSize(collectionClonerBatchSize),
      _countStage("count", this, &CollectionCloner::countStage),
      _listIndexesStage("listIndexes", this, &CollectionCloner::listIndexesStage),
      _createCollectionStage("createCollection", this, &CollectionCloner::createCollectionStage),
      _queryStage("query", this, &CollectionCloner::queryStage),
      _progressMeter(1U,  // total will be replaced with count command result.
                     kProgressMeterSecondsBetween,
                     kProgressMeterCheckInterval,
                     "documents copied",
                     str::stream() << _sourceNss.toString() << " collection clone progress"),
      _scheduleDbWorkFn([this](executor::TaskExecutor::CallbackFn work) {
          auto task = [ this, work = std::move(work) ](
                          OperationContext * opCtx,
                          const Status& status) mutable noexcept->TaskRunner::NextAction {
              try {
                  work(executor::TaskExecutor::CallbackArgs(nullptr, {}, status, opCtx));
              } catch (const DBException& e) {
                  setInitialSyncFailedStatus(e.toStatus());
              }
              return TaskRunner::NextAction::kDisposeOperationContext;
          };
          _dbWorkTaskRunner.schedule(std::move(task));
          return executor::TaskExecutor::CallbackHandle();
      }),
      _dbWorkTaskRunner(dbPool) {
    invariant(sourceNss.isValid());
    invariant(collectionOptions.uuid);
    _sourceDbAndUuid = NamespaceStringOrUUID(sourceNss.db().toString(), *collectionOptions.uuid);
    _stats.ns = _sourceNss.ns();

    // Find out whether the sync source supports resumable queries.
    _resumeSupported = (getClient()->getMaxWireVersion() == WireVersion::RESUMABLE_INITIAL_SYNC);
}

BaseCloner::ClonerStages CollectionCloner::getStages() {
    return {&_countStage, &_listIndexesStage, &_createCollectionStage, &_queryStage};
}


void CollectionCloner::preStage() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.start = getSharedData()->getClock()->now();
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
              "CollectionCloner ns: '{namespace}' uuid: "
              "UUID(\"{uuid}\") stopped because collection was dropped on source.",
              "CollectionCloner stopped because collection was dropped on source",
              "namespace"_attr = getCloner()->getSourceNss(),
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
                                    QueryOption_SlaveOk,
                                    0 /* limit */,
                                    0 /* skip */,
                                    ReadConcernArgs::kImplicitDefault);

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
    auto indexSpecs = IndexBuildsCoordinator::supportsTwoPhaseIndexBuild()
        ? getClient()->getReadyIndexSpecs(_sourceDbAndUuid, QueryOption_SlaveOk)
        : getClient()->getIndexSpecs(_sourceDbAndUuid, QueryOption_SlaveOk);
    if (indexSpecs.empty()) {
        LOGV2_WARNING(21143,
                      "No indexes found for collection {namespace} while cloning from {source}",
                      "No indexes found for collection while cloning",
                      "namespace"_attr = _sourceNss.ns(),
                      "source"_attr = getSource());
    }
    for (auto&& spec : indexSpecs) {
        if (spec["name"].str() == "_id_"_sd) {
            _idIndexSpec = spec.getOwned();
        } else {
            _indexSpecs.push_back(spec.getOwned());
        }
    }
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.indexes = _indexSpecs.size() + (_idIndexSpec.isEmpty() ? 0 : 1);
    };

    if (!_idIndexSpec.isEmpty() && _collectionOptions.autoIndexId == CollectionOptions::NO) {
        LOGV2_WARNING(21144,
                      "Found the _id_ index spec but the collection specified autoIndexId of false "
                      "on ns:{namespace}",
                      "Found the _id index spec but the collection specified autoIndexId of false",
                      "namespace"_attr = this->_sourceNss);
    }
    return kContinueNormally;
}

BaseCloner::AfterStageBehavior CollectionCloner::createCollectionStage() {
    auto collectionBulkLoader = getStorageInterface()->createCollectionForBulkLoading(
        _sourceNss, _collectionOptions, _idIndexSpec, _indexSpecs);
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

void CollectionCloner::runQuery() {
    // Non-resumable query.
    Query query = QUERY("query" << BSONObj() << "$readOnce" << true);

    if (_resumeSupported) {
        if (_resumeToken) {
            // Resume the query from where we left off.
            LOGV2_DEBUG(21133, 1, "Collection cloner will resume the last successful query");
            query = QUERY("query" << BSONObj() << "$readOnce" << true << "$_requestResumeToken"
                                  << true << "$_resumeAfter" << _resumeToken.get());
        } else {
            // New attempt at a resumable query.
            LOGV2_DEBUG(21134, 1, "Collection cloner will run a new query");
            query = QUERY("query" << BSONObj() << "$readOnce" << true << "$_requestResumeToken"
                                  << true);
        }
        query.hint(BSON("$natural" << 1));
    }

    // We reset this every time we retry or resume a query.
    // We distinguish the first batch from the rest so that we only store the remote cursor id
    // the first time we get it.
    _firstBatchOfQueryRound = true;

    try {
        getClient()->query([this](DBClientCursorBatchIterator& iter) { handleNextBatch(iter); },
                           _sourceDbAndUuid,
                           query,
                           nullptr /* fieldsToReturn */,
                           QueryOption_NoCursorTimeout | QueryOption_SlaveOk |
                               (collectionClonerUsesExhaust ? QueryOption_Exhaust : 0),
                           _collectionClonerBatchSize,
                           ReadConcernArgs::kImplicitDefault);
    } catch (...) {
        auto status = exceptionToStatus();

        // If the collection was dropped at any point, we can just move on to the next cloner.
        // This applies to both resumable (4.4) and non-resumable (4.2) queries.
        if (status == ErrorCodes::NamespaceNotFound) {
            throw;  // This will re-throw the NamespaceNotFound, resulting in a clean exit.
        }

        // Wire version 4.2 only.
        if (!_resumeSupported) {
            // If we lost our cursor last round, the only time we can can continue is if we find out
            // this round that the collection was dropped on the source (that scenario is covered
            // right above). If that is not the case, then the cloner would have more work to do,
            // but since we cannot resume the query, we must abort initial sync.
            if (_lostNonResumableCursor) {
                abortNonResumableClone(status);
            }

            // Collection has changed upstream. This will trigger the code block above next round,
            // (unless we find out the collection was dropped via getting a NamespaceNotFound).
            if (_queryStage.isCursorError(status)) {
                LOGV2(21135,
                      "Lost cursor during non-resumable query: {error}",
                      "Lost cursor during non-resumable query",
                      "error"_attr = status);
                _lostNonResumableCursor = true;
                throw;
            }
            // Any other errors (including network errors, but excluding NamespaceNotFound) result
            // in immediate failure.
            abortNonResumableClone(status);
        }

        // Re-throw all query errors for resumable (4.4) queries.
        throw;
    }
}

void CollectionCloner::handleNextBatch(DBClientCursorBatchIterator& iter) {
    {
        stdx::lock_guard<InitialSyncSharedData> lk(*getSharedData());
        if (!getSharedData()->getInitialSyncStatus(lk).isOK()) {
            static constexpr char message[] =
                "Collection cloning cancelled due to initial sync failure";
            LOGV2(21136, message, "error"_attr = getSharedData()->getInitialSyncStatus(lk));
            uasserted(ErrorCodes::CallbackCanceled,
                      str::stream()
                          << message << ": " << getSharedData()->getInitialSyncStatus(lk));
        }
    }

    // If this is 'true', it means that something happened to our remote cursor for a reason other
    // than the collection being dropped, all while we were running a non-resumable (4.2) clone.
    // We must abort initial sync in that case.
    if (_lostNonResumableCursor) {
        // This will be caught in runQuery().
        uasserted(ErrorCodes::InitialSyncFailure, "Lost remote cursor");
    }

    if (_firstBatchOfQueryRound && _resumeSupported) {
        // Store the cursorId of the remote cursor.
        _remoteCursorId = iter.getCursorId();
    }
    _firstBatchOfQueryRound = false;

    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.receivedBatches++;
        while (iter.moreInCurrentBatch()) {
            _documentsToInsert.emplace_back(iter.nextSafe());
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

    if (_resumeSupported) {
        // Store the resume token for this batch.
        _resumeToken = iter.getPostBatchResumeToken();
    }

    initialSyncHangCollectionClonerAfterHandlingBatchResponse.executeIf(
        [&](const BSONObj&) {
            while (MONGO_unlikely(
                       initialSyncHangCollectionClonerAfterHandlingBatchResponse.shouldFail()) &&
                   !mustExit()) {
                LOGV2(21137,
                      "initialSyncHangCollectionClonerAfterHandlingBatchResponse fail point "
                      "enabled for {namespace}. Blocking until fail point is disabled.",
                      "initialSyncHangCollectionClonerAfterHandlingBatchResponse fail point "
                      "enabled. Blocking until fail point is disabled",
                      "namespace"_attr = _sourceNss.toString());
                mongo::sleepsecs(1);
            }
        },
        [&](const BSONObj& data) {
            // Only hang when cloning the specified collection, or if no collection was specified.
            auto nss = data["nss"].str();
            return nss.empty() || nss == _sourceNss.toString();
        });
}

void CollectionCloner::insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& cbd) {
    uassertStatusOK(cbd.status);

    {
        stdx::lock_guard<Latch> lk(_mutex);
        std::vector<BSONObj> docs;
        if (_documentsToInsert.size() == 0) {
            LOGV2_WARNING(21145,
                          "insertDocumentsCallback, but no documents to insert for ns:{namespace}",
                          "insertDocumentsCallback, but no documents to insert",
                          "namespace"_attr = _sourceNss);
            return;
        }
        _documentsToInsert.swap(docs);
        _stats.documentsCopied += docs.size();
        ++_stats.fetchedBatches;
        _progressMeter.hit(int(docs.size()));
        invariant(_collLoader);

        // The insert must be done within the lock, because CollectionBulkLoader is not
        // thread safe.
        uassertStatusOK(_collLoader->insertDocuments(docs.cbegin(), docs.cend()));
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
            return data["namespace"].String() == _sourceNss.ns() &&
                static_cast<int>(_stats.documentsCopied) >= data["numDocsToClone"].numberInt();
        });
}

bool CollectionCloner::isMyFailPoint(const BSONObj& data) const {
    auto nss = data["nss"].str();
    return (nss.empty() || nss == _sourceNss.toString()) && BaseCloner::isMyFailPoint(data);
}

void CollectionCloner::waitForDatabaseWorkToComplete() {
    _dbWorkTaskRunner.join();
}

// Throws.
void CollectionCloner::abortNonResumableClone(const Status& status) {
    invariant(!_resumeSupported);
    LOGV2(21141,
          "Error during non-resumable clone: {error}",
          "Error during non-resumable clone",
          "error"_attr = status);
    std::string message = str::stream()
        << "Collection clone failed and is not resumable. nss: " << _sourceNss;
    uasserted(ErrorCodes::InitialSyncFailure, message);
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
    bob.append("ns", ns);
    append(&bob);
    return bob.obj();
}

void CollectionCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber(kDocumentsToCopyFieldName, documentToCopy);
    builder->appendNumber(kDocumentsCopiedFieldName, documentsCopied);
    builder->appendNumber("indexes", indexes);
    builder->appendNumber("fetchedBatches", fetchedBatches);
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

}  // namespace repl
}  // namespace mongo
