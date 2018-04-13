/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/collection_cloner.h"

#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/server_parameters.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

constexpr auto kCountResponseDocumentCountFieldName = "n"_sd;

const int kProgressMeterSecondsBetween = 60;
const int kProgressMeterCheckInterval = 128;

// The number of attempts for the count command, which gets the document count.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncCollectionCountAttempts, int, 3);
// The number of attempts for the listIndexes commands.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncListIndexesAttempts, int, 3);
// The number of attempts for the find command, which gets the data.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncCollectionFindAttempts, int, 3);
}  // namespace

// Failpoint which causes initial sync to hang before establishing its cursor to clone the
// 'namespace' collection.
MONGO_FP_DECLARE(initialSyncHangBeforeCollectionClone);

// Failpoint which causes initial sync to hang when it has cloned 'numDocsToClone' documents to
// collection 'namespace'.
MONGO_FP_DECLARE(initialSyncHangDuringCollectionClone);

// Failpoint which causes initial sync to hang after handling the next batch of results from the
// 'AsyncResultsMerger', optionally limited to a specific collection.
MONGO_FP_DECLARE(initialSyncHangCollectionClonerAfterHandlingBatchResponse);

// Failpoint which causes initial sync to hang before establishing the cursors (but after
// listIndexes), optionally limited to a specific collection.
MONGO_FP_DECLARE(initialSyncHangCollectionClonerBeforeEstablishingCursor);

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
                                   OldThreadPool* dbWorkThreadPool,
                                   const HostAndPort& source,
                                   const NamespaceString& sourceNss,
                                   const CollectionOptions& options,
                                   const CallbackFn& onCompletion,
                                   StorageInterface* storageInterface,
                                   const int batchSize,
                                   const int maxNumClonerCursors)
    : _executor(executor),
      _dbWorkThreadPool(dbWorkThreadPool),
      _source(source),
      _sourceNss(sourceNss),
      _destNss(_sourceNss),
      _options(options),
      _onCompletion(onCompletion),
      _storageInterface(storageInterface),
      _countScheduler(_executor,
                      RemoteCommandRequest(
                          _source,
                          _sourceNss.db().toString(),
                          makeCommandWithUUIDorCollectionName("count", _options.uuid, sourceNss),
                          ReadPreferenceSetting::secondaryPreferredMetadata(),
                          nullptr,
                          RemoteCommandRequest::kNoTimeout),
                      stdx::bind(&CollectionCloner::_countCallback, this, stdx::placeholders::_1),
                      RemoteCommandRetryScheduler::makeRetryPolicy(
                          numInitialSyncCollectionCountAttempts.load(),
                          executor::RemoteCommandRequest::kNoTimeout,
                          RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _listIndexesFetcher(
          _executor,
          _source,
          _sourceNss.db().toString(),
          makeCommandWithUUIDorCollectionName("listIndexes", _options.uuid, sourceNss),
          stdx::bind(&CollectionCloner::_listIndexesCallback,
                     this,
                     stdx::placeholders::_1,
                     stdx::placeholders::_2,
                     stdx::placeholders::_3),
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
      _scheduleDbWorkFn([this](const executor::TaskExecutor::CallbackFn& work) {
          auto task = [ this, work ](OperationContext * opCtx,
                                     const Status& status) noexcept->TaskRunner::NextAction {
              try {
                  work(executor::TaskExecutor::CallbackArgs(nullptr, {}, status, opCtx));
              } catch (...) {
                  _finishCallback(exceptionToStatus());
              }
              return TaskRunner::NextAction::kDisposeOperationContext;
          };
          _dbWorkTaskRunner.schedule(task);
          return executor::TaskExecutor::CallbackHandle();
      }),
      _progressMeter(1U,  // total will be replaced with count command result.
                     kProgressMeterSecondsBetween,
                     kProgressMeterCheckInterval,
                     "documents copied",
                     str::stream() << _sourceNss.toString() << " collection clone progress"),
      _collectionCloningBatchSize(batchSize),
      _maxNumClonerCursors(maxNumClonerCursors) {
    // Fetcher throws an exception on null executor.
    invariant(executor);
    uassert(ErrorCodes::BadValue,
            "invalid collection namespace: " + sourceNss.ns(),
            sourceNss.isValid());
    uassertStatusOK(options.validateForStorage());
    uassert(ErrorCodes::BadValue, "callback function cannot be null", onCompletion);
    uassert(ErrorCodes::BadValue, "storage interface cannot be null", storageInterface);
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
    if (_arm) {
        // This method can be called from a callback from either a TaskExecutor or a TaskRunner. The
        // TaskExecutor should never have an OperationContext attached to the Client, and the
        // TaskRunner should always have an OperationContext attached. Unfortunately, we don't know
        // which situation we're in, so have to handle both.
        auto& client = cc();
        if (auto opCtx = client.getOperationContext()) {
            _killArmHandle = _arm->kill(opCtx);
        } else {
            auto newOpCtx = client.makeOperationContext();
            _killArmHandle = _arm->kill(newOpCtx.get());
        }
    }
    _countScheduler.shutdown();
    _listIndexesFetcher.shutdown();
    if (_establishCollectionCursorsScheduler) {
        _establishCollectionCursorsScheduler->shutdown();
    }
    if (_verifyCollectionDroppedScheduler) {
        _verifyCollectionDroppedScheduler->shutdown();
    }
    _dbWorkTaskRunner.cancel();
}

CollectionCloner::Stats CollectionCloner::getStats() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stats;
}

void CollectionCloner::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_killArmHandle) {
        _executor->waitForEvent(_killArmHandle);
    }
    _condition.wait(lk, [this]() { return !_isActive_inlock(); });
}

void CollectionCloner::waitForDbWorker() {
    if (!isActive()) {
        return;
    }
    _dbWorkTaskRunner.join();
}

void CollectionCloner::setScheduleDbWorkFn_forTest(const ScheduleDbWorkFn& scheduleDbWorkFn) {
    LockGuard lk(_mutex);
    _scheduleDbWorkFn = scheduleDbWorkFn;
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
        _finishCallback({args.response.status.code(),
                         str::stream() << "During count call on collection '" << _sourceNss.ns()
                                       << "' from "
                                       << _source.toString()
                                       << ", there was an error '"
                                       << args.response.status.reason()
                                       << "'"});
        return;
    }

    long long count = 0;
    Status commandStatus = getStatusFromCommandResult(args.response.data);
    if (commandStatus == ErrorCodes::NamespaceNotFound && _options.uuid) {
        // Querying by a non-existing collection by UUID returns an error. Treat same as
        // behavior of find by namespace and use count == 0.
    } else if (!commandStatus.isOK()) {
        _finishCallback({commandStatus.code(),
                         str::stream() << "During count call on collection '" << _sourceNss.ns()
                                       << "' from "
                                       << _source.toString()
                                       << ", there was a command error '"
                                       << commandStatus.reason()
                                       << "'"});
        return;
    } else {
        auto countStatus = bsonExtractIntegerField(
            args.response.data, kCountResponseDocumentCountFieldName, &count);
        if (!countStatus.isOK()) {
            _finishCallback({countStatus.code(),
                             str::stream()
                                 << "There was an error parsing document count from count "
                                    "command result on collection "
                                 << _sourceNss.ns()
                                 << " from "
                                 << _source.toString()
                                 << ": "
                                 << countStatus.reason()});
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
        Status newStatus{fetchResult.getStatus().code(),
                         str::stream() << "During listIndexes call on collection '"
                                       << _sourceNss.ns()
                                       << "' there was an error '"
                                       << fetchResult.getStatus().reason()
                                       << "'"};

        _finishCallback(newStatus);
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
        stdx::bind(&CollectionCloner::_beginCollectionCallback, this, stdx::placeholders::_1));
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

    BSONObjBuilder cmdObj;
    EstablishCursorsCommand cursorCommand;
    // The 'find' command is used when the number of cloning cursors is 1 to ensure
    // the correctness of the collection cloning process until 'parallelCollectionScan'
    // can be tested more extensively in context of initial sync.
    if (_maxNumClonerCursors == 1) {
        cmdObj.appendElements(
            makeCommandWithUUIDorCollectionName("find", _options.uuid, _sourceNss));
        cmdObj.append("noCursorTimeout", true);
        // Set batchSize to be 0 to establish the cursor without fetching any documents,
        // similar to the response format of 'parallelCollectionScan'.
        cmdObj.append("batchSize", 0);
        cursorCommand = Find;
    } else {
        cmdObj.appendElements(makeCommandWithUUIDorCollectionName(
            "parallelCollectionScan", _options.uuid, _sourceNss));
        cmdObj.append("numCursors", _maxNumClonerCursors);
        cursorCommand = ParallelCollScan;
    }

    Client::initThreadIfNotAlready();
    auto opCtx = cc().getOperationContext();

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

    _establishCollectionCursorsScheduler = stdx::make_unique<RemoteCommandRetryScheduler>(
        _executor,
        RemoteCommandRequest(_source,
                             _sourceNss.db().toString(),
                             cmdObj.obj(),
                             ReadPreferenceSetting::secondaryPreferredMetadata(),
                             opCtx,
                             RemoteCommandRequest::kNoTimeout),
        stdx::bind(&CollectionCloner::_establishCollectionCursorsCallback,
                   this,
                   stdx::placeholders::_1,
                   cursorCommand),
        RemoteCommandRetryScheduler::makeRetryPolicy(
            numInitialSyncCollectionFindAttempts.load(),
            executor::RemoteCommandRequest::kNoTimeout,
            RemoteCommandRetryScheduler::kAllRetriableErrors));
    auto scheduleStatus = _establishCollectionCursorsScheduler->startup();
    LOG(1) << "Attempting to establish cursors with maxNumClonerCursors: " << _maxNumClonerCursors;

    if (!scheduleStatus.isOK()) {
        _establishCollectionCursorsScheduler.reset();
        _finishCallback(scheduleStatus);
        return;
    }
}

Status CollectionCloner::_parseCursorResponse(BSONObj response,
                                              std::vector<CursorResponse>* cursors,
                                              EstablishCursorsCommand cursorCommand) {
    switch (cursorCommand) {
        case Find: {
            StatusWith<CursorResponse> findResponse = CursorResponse::parseFromBSON(response);
            if (!findResponse.isOK()) {
                Status errorStatus{findResponse.getStatus().code(),
                                   str::stream()
                                       << "While parsing the 'find' query against collection '"
                                       << _sourceNss.ns()
                                       << "' there was an error '"
                                       << findResponse.getStatus().reason()
                                       << "'"};
                return errorStatus;
            }
            cursors->push_back(std::move(findResponse.getValue()));
            break;
        }
        case ParallelCollScan: {
            auto cursorElements = _parseParallelCollectionScanResponse(response);
            if (!cursorElements.isOK()) {
                return cursorElements.getStatus();
            }
            std::vector<BSONElement> cursorsArray;
            cursorsArray = cursorElements.getValue();
            // Parse each BSONElement into a 'CursorResponse' object.
            for (BSONElement cursor : cursorsArray) {
                if (!cursor.isABSONObj()) {
                    Status errorStatus(
                        ErrorCodes::FailedToParse,
                        "The 'cursor' field in the list of cursor responses is not a "
                        "valid BSON Object");
                    return errorStatus;
                }
                const BSONObj cursorObj = cursor.Obj().getOwned();
                StatusWith<CursorResponse> parallelCollScanResponse =
                    CursorResponse::parseFromBSON(cursorObj);
                if (!parallelCollScanResponse.isOK()) {
                    return parallelCollScanResponse.getStatus();
                }
                cursors->push_back(std::move(parallelCollScanResponse.getValue()));
            }
            break;
        }
        default: {
            Status errorStatus(
                ErrorCodes::FailedToParse,
                "The command used to establish the collection cloner cursors is not valid.");
            return errorStatus;
        }
    }
    return Status::OK();
}

void CollectionCloner::_establishCollectionCursorsCallback(const RemoteCommandCallbackArgs& rcbd,
                                                           EstablishCursorsCommand cursorCommand) {
    if (_state == State::kShuttingDown) {
        Status shuttingDownStatus{ErrorCodes::CallbackCanceled, "Cloner shutting down."};
        _finishCallback(shuttingDownStatus);
        return;
    }
    auto response = rcbd.response;
    if (!response.isOK()) {
        _finishCallback(response.status);
        return;
    }
    Status commandStatus = getStatusFromCommandResult(response.data);
    if (commandStatus == ErrorCodes::NamespaceNotFound) {
        _finishCallback(Status::OK());
        return;
    }
    if (!commandStatus.isOK()) {
        Status newStatus{commandStatus.code(),
                         str::stream() << "While querying collection '" << _sourceNss.ns()
                                       << "' there was an error '"
                                       << commandStatus.reason()
                                       << "'"};
        _finishCallback(commandStatus);
        return;
    }

    std::vector<CursorResponse> cursorResponses;
    Status parseResponseStatus =
        _parseCursorResponse(response.data, &cursorResponses, cursorCommand);
    if (!parseResponseStatus.isOK()) {
        _finishCallback(parseResponseStatus);
        return;
    }
    LOG(1) << "Collection cloner running with " << cursorResponses.size()
           << " cursors established.";

    // Initialize the 'AsyncResultsMerger'(ARM).
    std::vector<ClusterClientCursorParams::RemoteCursor> remoteCursors;
    for (auto&& cursorResponse : cursorResponses) {
        // A placeholder 'ShardId' is used until the ARM is made less sharding specific.
        remoteCursors.emplace_back(
            ShardId("CollectionClonerSyncSource"), _source, std::move(cursorResponse));
    }

    // An empty list of authenticated users is passed into the cluster parameters
    // as user information is not used in the ARM in context of collection cloning.
    _clusterClientCursorParams =
        stdx::make_unique<ClusterClientCursorParams>(_sourceNss, UserNameIterator());
    _clusterClientCursorParams->remotes = std::move(remoteCursors);
    if (_collectionCloningBatchSize > 0)
        _clusterClientCursorParams->batchSize = _collectionCloningBatchSize;
    // Client::initThreadIfNotAlready();
    auto opCtx = cc().makeOperationContext();
    _arm = stdx::make_unique<AsyncResultsMerger>(
        opCtx.get(), _executor, _clusterClientCursorParams.get());
    _arm->detachFromOperationContext();
    opCtx.reset();

    // This completion guard invokes _finishCallback on destruction.
    auto cancelRemainingWorkInLock = [this]() { _cancelRemainingWork_inlock(); };
    auto finishCallbackFn = [this](const Status& status) { _finishCallback(status); };
    auto onCompletionGuard =
        std::make_shared<OnCompletionGuard>(cancelRemainingWorkInLock, finishCallbackFn);

    // Lock guard must be declared after completion guard. If there is an error in this function
    // that will cause the destructor of the completion guard to run, the destructor must be run
    // outside the mutex. This is a necessary condition to invoke _finishCallback.
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    Status scheduleStatus = _scheduleNextARMResultsCallback(onCompletionGuard);
    if (!scheduleStatus.isOK()) {
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lock, scheduleStatus);
        return;
    }
}

StatusWith<std::vector<BSONElement>> CollectionCloner::_parseParallelCollectionScanResponse(
    BSONObj resp) {
    if (!resp.hasField("cursors")) {
        return Status(ErrorCodes::CursorNotFound,
                      "The 'parallelCollectionScan' response does not contain a 'cursors' field.");
    }
    BSONElement response = resp["cursors"];
    if (response.type() == BSONType::Array) {
        return response.Array();
    } else {
        return Status(
            ErrorCodes::FailedToParse,
            "The 'parallelCollectionScan' response is unable to be transformed into an array.");
    }
}

Status CollectionCloner::_bufferNextBatchFromArm(WithLock lock) {
    // We expect this callback to execute in a thread from a TaskExecutor which will not have an
    // OperationContext populated. We must make one ourselves.
    auto opCtx = cc().makeOperationContext();
    _arm->reattachToOperationContext(opCtx.get());
    while (_arm->ready()) {
        auto armResultStatus = _arm->nextReady();
        if (!armResultStatus.getStatus().isOK()) {
            return armResultStatus.getStatus();
        }
        if (armResultStatus.getValue().isEOF()) {
            // We have reached the end of the batch.
            break;
        } else {
            auto queryResult = armResultStatus.getValue().getResult();
            _documentsToInsert.push_back(std::move(*queryResult));
        }
    }
    _arm->detachFromOperationContext();

    return Status::OK();
}

Status CollectionCloner::_scheduleNextARMResultsCallback(
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    // We expect this callback to execute in a thread from a TaskExecutor which will not have an
    // OperationContext populated. We must make one ourselves.
    auto opCtx = cc().makeOperationContext();
    _arm->reattachToOperationContext(opCtx.get());
    auto nextEvent = _arm->nextEvent();
    _arm->detachFromOperationContext();
    if (!nextEvent.isOK()) {
        return nextEvent.getStatus();
    }
    auto event = nextEvent.getValue();
    auto handleARMResultsOnNextEvent =
        _executor->onEvent(event,
                           stdx::bind(&CollectionCloner::_handleARMResultsCallback,
                                      this,
                                      stdx::placeholders::_1,
                                      onCompletionGuard));
    return handleARMResultsOnNextEvent.getStatus();
}

void CollectionCloner::_handleARMResultsCallback(
    const executor::TaskExecutor::CallbackArgs& cbd,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard) {
    auto setResultAndCancelRemainingWork = [this](std::shared_ptr<OnCompletionGuard> guard,
                                                  Status status) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        guard->setResultAndCancelRemainingWork_inlock(lock, status);
        return;
    };

    if (!cbd.status.isOK()) {
        // Wait for active inserts to complete.
        waitForDbWorker();
        Status newStatus{cbd.status.code(),
                         str::stream() << "While querying collection '" << _sourceNss.ns()
                                       << "' there was an error '"
                                       << cbd.status.reason()
                                       << "'"};
        setResultAndCancelRemainingWork(onCompletionGuard, cbd.status);
        return;
    }

    // Pull the documents from the ARM into a buffer until the entire batch has been processed.
    bool lastBatch;
    {
        UniqueLock lk(_mutex);
        auto nextBatchStatus = _bufferNextBatchFromArm(lk);
        if (!nextBatchStatus.isOK()) {
            if (_options.uuid && (nextBatchStatus.code() == ErrorCodes::OperationFailed ||
                                  nextBatchStatus.code() == ErrorCodes::CursorNotFound)) {
                // With these errors, it's possible the collection was dropped while we were
                // cloning.  If so, we'll execute the drop during oplog application, so it's OK to
                // just stop cloning.  This is only safe if cloning by UUID; if we are cloning by
                // name, we have no way to detect if the collection was dropped and another
                // collection with the same name created in the interim.
                _verifyCollectionWasDropped(lk, nextBatchStatus, onCompletionGuard, cbd.opCtx);
            } else {
                onCompletionGuard->setResultAndCancelRemainingWork_inlock(lk, nextBatchStatus);
            }
            return;
        }

        // Check if this is the last batch of documents to clone.
        lastBatch = _arm->remotesExhausted();
    }

    // Schedule the next document batch insertion.
    auto&& scheduleResult =
        _scheduleDbWorkFn(stdx::bind(&CollectionCloner::_insertDocumentsCallback,
                                     this,
                                     stdx::placeholders::_1,
                                     lastBatch,
                                     onCompletionGuard));
    if (!scheduleResult.isOK()) {
        Status newStatus{scheduleResult.getStatus().code(),
                         str::stream() << "While cloning collection '" << _sourceNss.ns()
                                       << "' there was an error '"
                                       << scheduleResult.getStatus().reason()
                                       << "'"};
        setResultAndCancelRemainingWork(onCompletionGuard, scheduleResult.getStatus());
        return;
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

    // If the remote cursors are not exhausted, schedule this callback again to handle
    // the impending cursor response.
    if (!lastBatch) {
        Status scheduleStatus = _scheduleNextARMResultsCallback(onCompletionGuard);
        if (!scheduleStatus.isOK()) {
            setResultAndCancelRemainingWork(onCompletionGuard, scheduleStatus);
            return;
        }
    }
}

void CollectionCloner::_verifyCollectionWasDropped(
    const stdx::unique_lock<stdx::mutex>& lk,
    Status batchStatus,
    std::shared_ptr<OnCompletionGuard> onCompletionGuard,
    OperationContext* opCtx) {
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
                             opCtx,
                             RemoteCommandRequest::kNoTimeout),
        [this, opCtx, batchStatus, onCompletionGuard](const RemoteCommandCallbackArgs& args) {
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
    bool lastBatch,
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
        if (lastBatch) {
            onCompletionGuard->setResultAndCancelRemainingWork_inlock(lk, Status::OK());
        }
        return;
    }
    _documentsToInsert.swap(docs);
    _stats.documentsCopied += docs.size();
    ++_stats.fetchBatches;
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

    if (lastBatch) {
        // Clean up resources once the last batch has been copied over and set the status to OK.
        onCompletionGuard->setResultAndCancelRemainingWork_inlock(lk, Status::OK());
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
    builder->appendNumber("fetchedBatches", fetchBatches);
    if (start != Date_t()) {
        builder->appendDate("start", start);
        if (end != Date_t()) {
            builder->appendDate("end", end);
            auto elapsed = end - start;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("elapsedMillis", elapsedMillis);
        }
    }
}
}  // namespace repl
}  // namespace mongo
