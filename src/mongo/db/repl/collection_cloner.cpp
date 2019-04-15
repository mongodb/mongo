/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/repl/collection_cloner.h"

#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {
namespace {

using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using executor::RemoteCommandRequest;

constexpr auto kCountResponseDocumentCountFieldName = "n"_sd;

const int kProgressMeterSecondsBetween = 60;
const int kProgressMeterCheckInterval = 128;

}  // namespace

// Failpoint which causes initial sync to hang before establishing its cursor to clone the
// 'namespace' collection.
MONGO_FAIL_POINT_DEFINE(initialSyncHangBeforeCollectionClone);

// Failpoint which causes initial sync to hang when it has cloned 'numDocsToClone' documents to
// collection 'namespace'.
MONGO_FAIL_POINT_DEFINE(initialSyncHangDuringCollectionClone);

// Failpoint which causes initial sync to hang after handling the next batch of results from the
// DBClientConnection, optionally limited to a specific collection.
MONGO_FAIL_POINT_DEFINE(initialSyncHangCollectionClonerAfterHandlingBatchResponse);

// Failpoint which causes initial sync to hang before establishing the cursors (but after
// listIndexes), optionally limited to a specific collection.
MONGO_FAIL_POINT_DEFINE(initialSyncHangCollectionClonerBeforeEstablishingCursor);

BSONObj makeCommandWithUUIDorCollectionName(StringData command,
                                            OptionalCollectionUUID uuid,
                                            const NamespaceString& nss) {
    BSONObjBuilder builder;
    if (uuid)
        uuid->appendToBuilder(&builder, command);
    else
        builder.append(command, nss.coll());
    return builder.obj();
}

CollectionCloner::CollectionCloner(executor::TaskExecutor* executor,
                                   ThreadPool* dbWorkThreadPool,
                                   const HostAndPort& source,
                                   const NamespaceString& sourceNss,
                                   const CollectionOptions& options,
                                   CallbackFn onCompletion,
                                   StorageInterface* storageInterface,
                                   const int batchSize)
    : _executor(executor),
      _dbWorkThreadPool(dbWorkThreadPool),
      _source(source),
      _sourceNss(sourceNss),
      _destNss(_sourceNss),
      _options(options),
      _onCompletion(std::move(onCompletion)),
      _storageInterface(storageInterface),
      _countScheduler(_executor,
                      RemoteCommandRequest(
                          _source,
                          _sourceNss.db().toString(),
                          makeCommandWithUUIDorCollectionName("count", _options.uuid, sourceNss),
                          ReadPreferenceSetting::secondaryPreferredMetadata(),
                          nullptr,
                          RemoteCommandRequest::kNoTimeout),
                      [this](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                          return _countCallback(args);
                      },
                      RemoteCommandRetryScheduler::makeRetryPolicy(
                          numInitialSyncCollectionCountAttempts.load(),
                          executor::RemoteCommandRequest::kNoTimeout,
                          RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _listIndexesFetcher(
          _executor,
          _source,
          _sourceNss.db().toString(),
          makeCommandWithUUIDorCollectionName("listIndexes", _options.uuid, sourceNss),
          [this](const Fetcher::QueryResponseStatus& fetchResult,
                 Fetcher::NextAction * nextAction,
                 BSONObjBuilder * getMoreBob) {
              _listIndexesCallback(fetchResult, nextAction, getMoreBob);
          },
          ReadPreferenceSetting::secondaryPreferredMetadata(),
          RemoteCommandRequest::kNoTimeout /* find network timeout */,
          RemoteCommandRequest::kNoTimeout /* getMore network timeout */,
          RemoteCommandRetryScheduler::makeRetryPolicy(
              numInitialSyncListIndexesAttempts.load(),
              executor::RemoteCommandRequest::kNoTimeout,
              RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _indexSpecs(),
      _documentsToInsert(),
      _dbWorkTaskRunner(_dbWorkThreadPool),
      _scheduleDbWorkFn([this](executor::TaskExecutor::CallbackFn work) {
          auto task = [ this, work = std::move(work) ](
                          OperationContext * opCtx,
                          const Status& status) mutable noexcept->TaskRunner::NextAction {
              try {
                  work(executor::TaskExecutor::CallbackArgs(nullptr, {}, status, opCtx));
              } catch (...) {
                  _finishCallback(exceptionToStatus());
              }
              return TaskRunner::NextAction::kDisposeOperationContext;
          };
          _dbWorkTaskRunner.schedule(std::move(task));
          return executor::TaskExecutor::CallbackHandle();
      }),
      _createClientFn([] { return stdx::make_unique<DBClientConnection>(); }),
      _progressMeter(1U,  // total will be replaced with count command result.
                     kProgressMeterSecondsBetween,
                     kProgressMeterCheckInterval,
                     "documents copied",
                     str::stream() << _sourceNss.toString() << " collection clone progress"),
      _collectionClonerBatchSize(batchSize) {
    // Fetcher throws an exception on null executor.
    invariant(executor);
    uassert(ErrorCodes::BadValue,
            "invalid collection namespace: " + sourceNss.ns(),
            sourceNss.isValid());
    uassertStatusOK(options.validateForStorage());
    uassert(50953,
            "Missing collection UUID in CollectionCloner, collection name: " + sourceNss.ns(),
            _options.uuid);
    uassert(ErrorCodes::BadValue, "callback function cannot be null", _onCompletion);
    uassert(ErrorCodes::BadValue, "storage interface cannot be null", storageInterface);
    uassert(
        50954, "collectionClonerBatchSize must be non-negative.", _collectionClonerBatchSize >= 0);
    _stats.ns = _sourceNss.ns();
}

CollectionCloner::~CollectionCloner() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

const NamespaceString& CollectionCloner::getSourceNamespace() const {
    return _sourceNss;
}

bool CollectionCloner::isActive() const {
    LockGuard lk(_mutex);
    return _isActive_inlock();
}

bool CollectionCloner::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

bool CollectionCloner::_isShuttingDown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return State::kShuttingDown == _state;
}

Status CollectionCloner::startup() noexcept {
    LockGuard lk(_mutex);
    LOG(0) << "CollectionCloner::start called, on ns:" << _destNss;

    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::InternalError, "collection cloner already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress, "collection cloner shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress, "collection cloner completed");
    }

    _stats.start = _executor->now();
    Status scheduleResult = _countScheduler.startup();
    if (!scheduleResult.isOK()) {
        _state = State::kComplete;
        return scheduleResult;
    }

    return Status::OK();
}

void CollectionCloner::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    switch (_state) {
        case State::kPreStart:
            // Transition directly from PreStart to Complete if not started yet.
            _state = State::kComplete;
            return;
        case State::kRunning:
            _state = State::kShuttingDown;
            break;
        case State::kShuttingDown:
        case State::kComplete:
            // Nothing to do if we are already in ShuttingDown or Complete state.
            return;
    }
    _cancelRemainingWork_inlock();
}

void CollectionCloner::_cancelRemainingWork_inlock() {
    _countScheduler.shutdown();
    _listIndexesFetcher.shutdown();
    if (_verifyCollectionDroppedScheduler) {
        _verifyCollectionDroppedScheduler->shutdown();
    }
    if (_queryState == QueryState::kRunning) {
        _queryState = QueryState::kCanceling;
        _clientConnection->shutdownAndDisallowReconnect();
    } else {
        _queryState = QueryState::kFinished;
    }
    _dbWorkTaskRunner.cancel();
}

CollectionCloner::Stats CollectionCloner::getStats() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stats;
}

void CollectionCloner::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _condition.wait(lk, [this]() {
        return (_queryState == QueryState::kNotStarted || _queryState == QueryState::kFinished) &&
            !_isActive_inlock();
    });
}

void CollectionCloner::waitForDbWorker() {
    if (!isActive()) {
        return;
    }
    _dbWorkTaskRunner.join();
}

void CollectionCloner::setScheduleDbWorkFn_forTest(ScheduleDbWorkFn scheduleDbWorkFn) {
    LockGuard lk(_mutex);
    _scheduleDbWorkFn = std::move(scheduleDbWorkFn);
}

void CollectionCloner::setCreateClientFn_forTest(const CreateClientFn& createClientFn) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _createClientFn = createClientFn;
}

std::vector<BSONObj> CollectionCloner::getDocumentsToInsert_forTest() {
    LockGuard lk(_mutex);
    return _documentsToInsert;
}

void CollectionCloner::_countCallback(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {

    // No need to reword status reason in the case of cancellation.
    if (ErrorCodes::CallbackCanceled == args.response.status) {
        _finishCallback(args.response.status);
        return;
    }

    if (!args.response.status.isOK()) {
        _finishCallback(args.response.status.withContext(
            str::stream() << "Count call failed on collection '" << _sourceNss.ns() << "' from "
                          << _source.toString()));
        return;
    }

    long long count = 0;
    Status commandStatus = getStatusFromCommandResult(args.response.data);
    if (commandStatus == ErrorCodes::NamespaceNotFound) {
        // Querying by a non-existing collection by UUID returns an error. Treat same as
        // behavior of find by namespace and use count == 0.
    } else if (!commandStatus.isOK()) {
        _finishCallback(commandStatus.withContext(
            str::stream() << "Count call failed on collection '" << _sourceNss.ns() << "' from "
                          << _source.toString()));
        return;
    } else {
        auto countStatus = bsonExtractIntegerField(
            args.response.data, kCountResponseDocumentCountFieldName, &count);
        if (!countStatus.isOK()) {
            _finishCallback(countStatus.withContext(
                str::stream() << "There was an error parsing document count from count "
                                 "command result on collection "
                              << _sourceNss.ns()
                              << " from "
                              << _source.toString()));
            return;
        }
    }

    if (count < 0) {
        _finishCallback({ErrorCodes::BadValue,
                         str::stream() << "Count call on collection " << _sourceNss.ns() << " from "
                                       << _source.toString()
                                       << " returned negative document count: "
                                       << count});
        return;
    }

    {
        LockGuard lk(_mutex);
        _stats.documentToCopy = count;
        _progressMeter.setTotalWhileRunning(static_cast<unsigned long long>(count));
    }

    auto scheduleStatus = _listIndexesFetcher.schedule();
    if (!scheduleStatus.isOK()) {
        _finishCallback(scheduleStatus);
        return;
    }
}

void CollectionCloner::_listIndexesCallback(const Fetcher::QueryResponseStatus& fetchResult,
                                            Fetcher::NextAction* nextAction,
                                            BSONObjBuilder* getMoreBob) {
    const bool collectionIsEmpty = fetchResult == ErrorCodes::NamespaceNotFound;
    if (collectionIsEmpty) {
        // Schedule collection creation and finish callback.
        auto&& scheduleResult =
            _scheduleDbWorkFn([this](const executor::TaskExecutor::CallbackArgs& cbd) {
                if (!cbd.status.isOK()) {
                    _finishCallback(cbd.status);
                    return;
                }
                auto opCtx = cbd.opCtx;
                UnreplicatedWritesBlock uwb(opCtx);
                auto&& createStatus =
                    _storageInterface->createCollection(opCtx, _destNss, _options);
                _finishCallback(createStatus);
            });
        if (!scheduleResult.isOK()) {
            _finishCallback(scheduleResult.getStatus());
        }
        return;
    };
    if (!fetchResult.isOK()) {
        _finishCallback(fetchResult.getStatus().withContext(
            str::stream() << "listIndexes call failed on collection '" << _sourceNss.ns() << "'"));
        return;
    }

    auto batchData(fetchResult.getValue());
    auto&& documents = batchData.documents;

    if (documents.empty()) {
        warning() << "No indexes found for collection " << _sourceNss.ns() << " while cloning from "
                  << _source;
    }

    UniqueLock lk(_mutex);
    // When listing indexes by UUID, the sync source may use a different name for the collection
    // as result of renaming or two-phase drop. As the index spec also includes a 'ns' field, this
    // must be rewritten.
    BSONObjBuilder nsFieldReplacementBuilder;
    nsFieldReplacementBuilder.append("ns", _sourceNss.ns());
    BSONElement nsFieldReplacementElem = nsFieldReplacementBuilder.done().firstElement();

    // We may be called with multiple batches leading to a need to grow _indexSpecs.
    _indexSpecs.reserve(_indexSpecs.size() + documents.size());
    for (auto&& doc : documents) {
        // The addField replaces the 'ns' field with the correct name, see above.
        if (StringData("_id_") == doc["name"].str()) {
            _idIndexSpec = doc.addField(nsFieldReplacementElem);
            continue;
        }
        _indexSpecs.push_back(doc.addField(nsFieldReplacementElem));
    }
    lk.unlock();

    // The fetcher will continue to call with kGetMore until an error or the last batch.
    if (*nextAction == Fetcher::NextAction::kGetMore) {
        invariant(getMoreBob);
        getMoreBob->append("getMore", batchData.cursorId);
        getMoreBob->append("collection", batchData.nss.coll());
        return;
    }

    // We have all of the indexes now, so we can start cloning the collection data.
    auto&& scheduleResult = _scheduleDbWorkFn(
        [=](const executor::TaskExecutor::CallbackArgs& cbd) { _beginCollectionCallback(cbd); });
    if (!scheduleResult.isOK()) {
        _finishCallback(scheduleResult.getStatus());
        return;
    }
}

void CollectionCloner::_beginCollectionCallback(const executor::TaskExecutor::CallbackArgs& cbd) {
    if (!cbd.status.isOK()) {
        _finishCallback(cbd.status);
        return;
    }
    MONGO_FAIL_POINT_BLOCK(initialSyncHangCollectionClonerBeforeEstablishingCursor, nssData) {
        const BSONObj& data = nssData.getData();
        auto nss = data["nss"].str();
        // Only hang when cloning the specified collection, or if no collection was specified.
        if (nss.empty() || _destNss.toString() == nss) {
            while (MONGO_FAIL_POINT(initialSyncHangCollectionClonerBeforeEstablishingCursor) &&
                   !_isShuttingDown()) {
                log() << "initialSyncHangCollectionClonerBeforeEstablishingCursor fail point "
                         "enabled for "
                      << _destNss.toString() << ". Blocking until fail point is disabled.";
                mongo::sleepsecs(1);
            }
        }
    }
    if (!_idIndexSpec.isEmpty() && _options.autoIndexId == CollectionOptions::NO) {
        warning()
            << "Found the _id_ index spec but the collection specified autoIndexId of false on ns:"
            << this->_sourceNss;
    }

    auto collectionBulkLoader = _storageInterface->createCollectionForBulkLoading(
        _destNss, _options, _idIndexSpec, _indexSpecs);

    if (!collectionBulkLoader.isOK()) {
        _finishCallback(collectionBulkLoader.getStatus());
        return;
    }

    _stats.indexes = _indexSpecs.size();
    if (!_idIndexSpec.isEmpty()) {
        ++_stats.indexes;
    }

    _collLoader = std::move(collectionBulkLoader.getValue());

    // The query cannot run on the database work thread, because it needs to be able to
    // schedule work on that thread while still running.
    auto runQueryCallback =
        _executor->scheduleWork([this](const executor::TaskExecutor::CallbackArgs& callbackData) {
            // This completion guard invokes _finishCallback on destruction.
            auto cancelRemainingWorkInLock = [this]() { _cancelRemainingWork_inlock(); };
            auto finishCallbackFn = [this](const Status& status) {
                {
                    stdx::lock_guard<stdx::mutex> lock(_mutex);
                    _queryState = QueryState::kFinished;
                    _clientConnection.reset();
                }
                _condition.notify_all();
                _finishCallback(status);
            };
            auto onCompletionGuard =
                std::make_shared<OnCompletionGuard>(cancelRemainingWorkInLock, finishCallbackFn);
            _runQuery(callbackData, onCompletionGuard);
        });
    if (!runQueryCallback.isOK()) {
        _finishCallback(runQueryCallback.getStatus());
        return;
    }
}

void CollectionCloner::_runQuery(const executor::TaskExecutor::CallbackArgs& callbackData,
                                 std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    if (!callbackData.status.isOK()) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, callbackData.status);
        return;
    }
    bool queryStateOK = false;
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        queryStateOK = _queryState == QueryState::kNotStarted;
        if (queryStateOK) {
            _queryState = QueryState::kRunning;
            _clientConnection = _createClientFn();
        } else {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(
                lock, {ErrorCodes::CallbackCanceled, "Collection cloning cancelled."});
            return;
        }
    }

    MONGO_FAIL_POINT_BLOCK(initialSyncHangBeforeCollectionClone, options) {
        const BSONObj& data = options.getData();
        if (data["namespace"].String() == _destNss.ns()) {
            log() << "initial sync - initialSyncHangBeforeCollectionClone fail point "
                     "enabled. Blocking until fail point is disabled.";
            while (MONGO_FAIL_POINT(initialSyncHangBeforeCollectionClone) && !_isShuttingDown()) {
                mongo::sleepsecs(1);
            }
        }
    }

    Status clientConnectionStatus = _clientConnection->connect(_source, StringData());
    if (!clientConnectionStatus.isOK()) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, clientConnectionStatus);
        return;
    }
    if (!replAuthenticate(_clientConnection.get())) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(
            lock,
            {ErrorCodes::AuthenticationFailed,
             str::stream() << "Failed to authenticate to " << _source});
        return;
    }

    // readOnce is available on 4.2 sync sources only.  Initially we don't know FCV, so
    // we won't use the readOnce feature, but once the admin database is cloned we will use it.
    // The admin database is always cloned first, so all user data should use readOnce.
    const bool readOnceAvailable = serverGlobalParams.featureCompatibility.getVersionUnsafe() ==
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42;
    try {
        _clientConnection->query(
            [this, onCompletionGuard](DBClientCursorBatchIterator& iter) {
                _handleNextBatch(onCompletionGuard, iter);
            },
            NamespaceStringOrUUID(_sourceNss.db().toString(), *_options.uuid),
            readOnceAvailable ? QUERY("query" << BSONObj() << "$readOnce" << true) : Query(),
            nullptr /* fieldsToReturn */,
            QueryOption_NoCursorTimeout | QueryOption_SlaveOk |
                (collectionClonerUsesExhaust ? QueryOption_Exhaust : 0),
            _collectionClonerBatchSize);
    } catch (const DBException& e) {
        auto queryStatus = e.toStatus().withContext(str::stream() << "Error querying collection '"
                                                                  << _sourceNss.ns());
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        if (queryStatus.code() == ErrorCodes::OperationFailed ||
            queryStatus.code() == ErrorCodes::CursorNotFound ||
            queryStatus.code() == ErrorCodes::QueryPlanKilled) {
            // With these errors, it's possible the collection was dropped while we were
            // cloning.  If so, we'll execute the drop during oplog application, so it's OK to
            // just stop cloning.
            //
            // A 4.2 node should only ever raise QueryPlanKilled, but an older node could raise
            // OperationFailed or CursorNotFound.
            _verifyCollectionWasDropped(lock, queryStatus, onCompletionGuard);
            return;
        } else if (queryStatus.code() != ErrorCodes::NamespaceNotFound) {
            // NamespaceNotFound means the collection was dropped before we started cloning, so
            // we're OK to ignore the error.  Any other error we must report.
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, queryStatus);
            return;
        }
    }
    waitForDbWorker();
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, Status::OK());
}

void CollectionCloner::_handleNextBatch(std::shared_ptr<OnCompletionGuard> onCompletionGuard,
                                        DBClientCursorBatchIterator& iter) {
    _stats.receivedBatches++;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        uassert(ErrorCodes::CallbackCanceled,
                "Collection cloning cancelled.",
                _queryState != QueryState::kCanceling);
        while (iter.moreInCurrentBatch()) {
            BSONObj o = iter.nextSafe();
            _documentsToInsert.emplace_back(std::move(o));
        }
    }

    // Schedule the next document batch insertion.
    auto&& scheduleResult = _scheduleDbWorkFn([=](const executor::TaskExecutor::CallbackArgs& cbd) {
        _insertDocumentsCallback(cbd, onCompletionGuard);
    });

    if (!scheduleResult.isOK()) {
        Status newStatus = scheduleResult.getStatus().withContext(
            str::stream() << "Error cloning collection '" << _sourceNss.ns() << "'");
        // We must throw an exception to terminate query.
        uassertStatusOK(newStatus);
    }

    MONGO_FAIL_POINT_BLOCK(initialSyncHangCollectionClonerAfterHandlingBatchResponse, nssData) {
        const BSONObj& data = nssData.getData();
        auto nss = data["nss"].str();
        // Only hang when cloning the specified collection, or if no collection was specified.
        if (nss.empty() || _destNss.toString() == nss) {
            while (MONGO_FAIL_POINT(initialSyncHangCollectionClonerAfterHandlingBatchResponse) &&
                   !_isShuttingDown()) {
                log() << "initialSyncHangCollectionClonerAfterHandlingBatchResponse fail point "
                         "enabled for "
                      << _destNss.toString() << ". Blocking until fail point is disabled.";
                mongo::sleepsecs(1);
            }
        }
    }
}

void CollectionCloner::_verifyCollectionWasDropped(
    const stdx::unique_lock<stdx::mutex>& lk,
    Status batchStatus,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    // If we already have a _verifyCollectionDroppedScheduler, just return; the existing
    // scheduler will take care of cleaning up.
    if (_verifyCollectionDroppedScheduler) {
        return;
    }
    BSONObjBuilder cmdObj;
    _options.uuid->appendToBuilder(&cmdObj, "find");
    cmdObj.append("batchSize", 0);
    _verifyCollectionDroppedScheduler = stdx::make_unique<RemoteCommandRetryScheduler>(
        _executor,
        RemoteCommandRequest(_source,
                             _sourceNss.db().toString(),
                             cmdObj.obj(),
                             ReadPreferenceSetting::secondaryPreferredMetadata(),
                             nullptr /* No OperationContext require for replication commands */,
                             RemoteCommandRequest::kNoTimeout),
        [this, batchStatus, onCompletionGuard](const RemoteCommandCallbackArgs& args) {
            // If the attempt to determine if the collection was dropped fails for any reason other
            // than NamespaceNotFound, return the original error code.
            //
            // Otherwise, if the collection was dropped, either the error will be NamespaceNotFound,
            // or it will be a drop-pending collection and the find will succeed and give us a
            // collection with a drop-pending name.
            UniqueLock lk(_mutex);
            Status finalStatus(batchStatus);
            if (args.response.isOK()) {
                auto response = CursorResponse::parseFromBSON(args.response.data);
                if (response.getStatus().code() == ErrorCodes::NamespaceNotFound ||
                    (response.isOK() && response.getValue().getNSS().isDropPendingNamespace())) {
                    log() << "CollectionCloner ns: '" << _sourceNss.ns() << "' uuid: UUID(\""
                          << *_options.uuid << "\") stopped because collection was dropped.";
                    finalStatus = Status::OK();
                } else if (!response.isOK()) {
                    log() << "CollectionCloner received an unexpected error when verifying drop of "
                             "ns: '"
                          << _sourceNss.ns() << "' uuid: UUID(\"" << *_options.uuid
                          << "\"), status " << response.getStatus();
                }
            } else {
                log() << "CollectionCloner is unable to verify drop of ns: '" << _sourceNss.ns()
                      << "' uuid: UUID(\"" << *_options.uuid << "\"), status "
                      << args.response.status;
            }
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lk, finalStatus);
        },
        RemoteCommandRetryScheduler::makeNoRetryPolicy());

    auto status = _verifyCollectionDroppedScheduler->startup();
    if (!status.isOK()) {
        log() << "CollectionCloner is unable to start verification of ns: '" << _sourceNss.ns()
              << "' uuid: UUID(\"" << *_options.uuid << "\"), status " << status;
        // If we can't run the command, assume this wasn't a drop and just use the original error.
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lk, batchStatus);
    }
}

void CollectionCloner::_insertDocumentsCallback(
    const executor::TaskExecutor::CallbackArgs& cbd,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    if (!cbd.status.isOK()) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, cbd.status);
        return;
    }

    UniqueLock lk(_mutex);
    std::vector<BSONObj> docs;
    if (_documentsToInsert.size() == 0) {
        warning() << "_insertDocumentsCallback, but no documents to insert for ns:" << _destNss;
        return;
    }
    _documentsToInsert.swap(docs);
    _stats.documentsCopied += docs.size();
    ++_stats.fetchedBatches;
    _progressMeter.hit(int(docs.size()));
    invariant(_collLoader);
    const auto status = _collLoader->insertDocuments(docs.cbegin(), docs.cend());
    if (!status.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lk, status);
        return;
    }

    MONGO_FAIL_POINT_BLOCK(initialSyncHangDuringCollectionClone, options) {
        const BSONObj& data = options.getData();
        if (data["namespace"].String() == _destNss.ns() &&
            static_cast<int>(_stats.documentsCopied) >= data["numDocsToClone"].numberInt()) {
            lk.unlock();
            log() << "initial sync - initialSyncHangDuringCollectionClone fail point "
                     "enabled. Blocking until fail point is disabled.";
            while (MONGO_FAIL_POINT(initialSyncHangDuringCollectionClone) && !_isShuttingDown()) {
                mongo::sleepsecs(1);
            }
            lk.lock();
        }
    }
}

void CollectionCloner::_finishCallback(const Status& status) {
    log() << "CollectionCloner ns:" << _destNss
          << " finished cloning with status: " << redact(status);
    // Copy the status so we can change it below if needed.
    auto finalStatus = status;
    bool callCollectionLoader = false;
    decltype(_onCompletion) onCompletion;
    {
        LockGuard lk(_mutex);
        invariant(_state != State::kComplete);

        callCollectionLoader = _collLoader.operator bool();

        invariant(_onCompletion);
        std::swap(_onCompletion, onCompletion);
    }
    if (callCollectionLoader) {
        if (finalStatus.isOK()) {
            const auto loaderStatus = _collLoader->commit();
            if (!loaderStatus.isOK()) {
                warning() << "Failed to commit collection indexes " << _destNss.ns() << ": "
                          << redact(loaderStatus);
                finalStatus = loaderStatus;
            }
        }

        // This will release the resources held by the loader.
        _collLoader.reset();
    }
    onCompletion(finalStatus);

    // This will release the resources held by the callback function object. '_onCompletion' is
    // already cleared at this point and 'onCompletion' is the remaining reference to the callback
    // function (with any implicitly held resources). To avoid any issues with destruction logic
    // in the function object's resources accessing this CollectionCloner, we release this function
    // object outside the lock.
    onCompletion = {};

    LockGuard lk(_mutex);
    _stats.end = _executor->now();
    _progressMeter.finished();
    _state = State::kComplete;
    _condition.notify_all();
    LOG(1) << "    collection: " << _destNss << ", stats: " << _stats.toString();
}

constexpr StringData CollectionCloner::Stats::kDocumentsToCopyFieldName;
constexpr StringData CollectionCloner::Stats::kDocumentsCopiedFieldName;

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
