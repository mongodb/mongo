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
#include "mongo/db/repl/initial_sync_common.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/server_parameters.h"
#include "mongo/rpc/get_status_from_command_result.h"
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

// The batchSize to use for the query to get all documents from the collection.
// 16MB max batch size / 12 byte min doc size * 10 (for good measure) = batchSize to use.
const auto batchSize = (16 * 1024 * 1024) / 12 * 10;
// The number of attempts for the count command, which gets the document count.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncCollectionCountAttempts, int, 3);
// The number of attempts for the listIndexes commands.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncListIndexesAttempts, int, 3);
// The number of attempts for the find command, which gets the data.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncCollectionFindAttempts, int, 3);
}  // namespace

CollectionCloner::CollectionCloner(executor::TaskExecutor* executor,
                                   OldThreadPool* dbWorkThreadPool,
                                   const HostAndPort& source,
                                   const NamespaceString& sourceNss,
                                   const CollectionOptions& options,
                                   const CallbackFn& onCompletion,
                                   StorageInterface* storageInterface)
    : _executor(executor),
      _dbWorkThreadPool(dbWorkThreadPool),
      _source(source),
      _sourceNss(sourceNss),
      _destNss(_sourceNss),
      _options(options),
      _onCompletion(onCompletion),
      _storageInterface(storageInterface),
      _countScheduler(_executor,
                      RemoteCommandRequest(_source,
                                           _sourceNss.db().toString(),
                                           BSON("count" << _sourceNss.coll()),
                                           rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
                                           nullptr,
                                           RemoteCommandRequest::kNoTimeout),
                      stdx::bind(&CollectionCloner::_countCallback, this, stdx::placeholders::_1),
                      RemoteCommandRetryScheduler::makeRetryPolicy(
                          numInitialSyncCollectionCountAttempts,
                          executor::RemoteCommandRequest::kNoTimeout,
                          RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _listIndexesFetcher(_executor,
                          _source,
                          _sourceNss.db().toString(),
                          BSON("listIndexes" << _sourceNss.coll()),
                          stdx::bind(&CollectionCloner::_listIndexesCallback,
                                     this,
                                     stdx::placeholders::_1,
                                     stdx::placeholders::_2,
                                     stdx::placeholders::_3),
                          rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
                          RemoteCommandRequest::kNoTimeout,
                          RemoteCommandRetryScheduler::makeRetryPolicy(
                              numInitialSyncListIndexesAttempts,
                              executor::RemoteCommandRequest::kNoTimeout,
                              RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _findFetcher(
          _executor,
          _source,
          _sourceNss.db().toString(),
          // noCursorTimeout true, large batchSize (for older server versions to get larger batch)
          BSON("find" << _sourceNss.coll() << "noCursorTimeout" << true << "batchSize"
                      << batchSize),
          stdx::bind(&CollectionCloner::_findCallback,
                     this,
                     stdx::placeholders::_1,
                     stdx::placeholders::_2,
                     stdx::placeholders::_3),
          rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
          RemoteCommandRequest::kNoTimeout,
          RemoteCommandRetryScheduler::makeRetryPolicy(
              numInitialSyncCollectionFindAttempts,
              executor::RemoteCommandRequest::kNoTimeout,
              RemoteCommandRetryScheduler::kAllRetriableErrors)),

      _indexSpecs(),
      _documents(),
      _dbWorkTaskRunner(_dbWorkThreadPool),
      _scheduleDbWorkFn([this](const executor::TaskExecutor::CallbackFn& work) {
          auto task = [work](OperationContext* txn,
                             const Status& status) -> TaskRunner::NextAction {
              work(executor::TaskExecutor::CallbackArgs(nullptr, {}, status, txn));
              return TaskRunner::NextAction::kDisposeOperationContext;
          };
          _dbWorkTaskRunner.schedule(task);
          return executor::TaskExecutor::CallbackHandle();
      }),
      _progressMeter(1U,  // total will be replaced with count command result.
                     kProgressMeterSecondsBetween,
                     kProgressMeterCheckInterval,
                     "documents copied",
                     str::stream() << _sourceNss.toString() << " collection clone progress") {
    // Fetcher throws an exception on null executor.
    invariant(executor);
    uassert(ErrorCodes::BadValue,
            "invalid collection namespace: " + sourceNss.ns(),
            sourceNss.isValid());
    uassertStatusOK(options.validate());
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

std::string CollectionCloner::getDiagnosticString() const {
    LockGuard lk(_mutex);
    str::stream output;
    output << "CollectionCloner";
    output << " executor: " << _executor->getDiagnosticString();
    output << " source: " << _source.toString();
    output << " source namespace: " << _sourceNss.toString();
    output << " destination namespace: " << _destNss.toString();
    output << " collection options: " << _options.toBSON();
    output << " active: " << _isActive_inlock();
    output << " listIndexes fetcher: " << _listIndexesFetcher.getDiagnosticString();
    output << " find fetcher: " << _findFetcher.getDiagnosticString();
    return output;
}

bool CollectionCloner::isActive() const {
    LockGuard lk(_mutex);
    return _isActive_inlock();
}

bool CollectionCloner::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
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

    _countScheduler.shutdown();
    _listIndexesFetcher.shutdown();
    _findFetcher.shutdown();
    _dbWorkTaskRunner.cancel();
}

CollectionCloner::Stats CollectionCloner::getStats() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stats;
}

void CollectionCloner::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
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

    Status commandStatus = getStatusFromCommandResult(args.response.data);
    if (!commandStatus.isOK()) {
        _finishCallback({commandStatus.code(),
                         str::stream() << "During count call on collection '" << _sourceNss.ns()
                                       << "' from "
                                       << _source.toString()
                                       << ", there was a command error '"
                                       << commandStatus.reason()
                                       << "'"});
        return;
    }

    long long count = 0;
    auto countStatus =
        bsonExtractIntegerField(args.response.data, kCountResponseDocumentCountFieldName, &count);
    if (!countStatus.isOK()) {
        _finishCallback({countStatus.code(),
                         str::stream() << "There was an error parsing document count from count "
                                          "command result on collection "
                                       << _sourceNss.ns()
                                       << " from "
                                       << _source.toString()
                                       << ": "
                                       << countStatus.reason()});
        return;
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
                auto txn = cbd.txn;
                txn->setReplicatedWrites(false);
                auto&& createStatus = _storageInterface->createCollection(txn, _destNss, _options);
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
    // We may be called with multiple batches leading to a need to grow _indexSpecs.
    _indexSpecs.reserve(_indexSpecs.size() + documents.size());
    for (auto&& doc : documents) {
        if (StringData("_id_") == doc["name"].str()) {
            _idIndexSpec = doc;
            continue;
        }
        _indexSpecs.push_back(doc);
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

void CollectionCloner::_findCallback(const StatusWith<Fetcher::QueryResponse>& fetchResult,
                                     Fetcher::NextAction* nextAction,
                                     BSONObjBuilder* getMoreBob) {
    if (!fetchResult.isOK()) {
        // Wait for active inserts to complete.
        waitForDbWorker();

        Status newStatus{fetchResult.getStatus().code(),
                         str::stream() << "While querying collection '" << _sourceNss.ns()
                                       << "' there was an error '"
                                       << fetchResult.getStatus().reason()
                                       << "'"};
        _finishCallback(newStatus);
        return;
    }

    auto batchData(fetchResult.getValue());
    bool lastBatch = *nextAction == Fetcher::NextAction::kNoAction;
    if (batchData.documents.size() > 0) {
        LockGuard lk(_mutex);
        _documents.insert(_documents.end(), batchData.documents.begin(), batchData.documents.end());
    } else if (!batchData.first) {
        warning() << "No documents returned in batch; ns: " << _sourceNss
                  << ", cursorId:" << batchData.cursorId << ", isLastBatch:" << lastBatch;
    }

    auto&& scheduleResult = _scheduleDbWorkFn(stdx::bind(
        &CollectionCloner::_insertDocumentsCallback, this, stdx::placeholders::_1, lastBatch));
    if (!scheduleResult.isOK()) {
        Status newStatus{scheduleResult.getStatus().code(),
                         str::stream() << "While cloning collection '" << _sourceNss.ns()
                                       << "' there was an error '"
                                       << scheduleResult.getStatus().reason()
                                       << "'"};
        _finishCallback(newStatus);
        return;
    }

    if (!lastBatch) {
        invariant(getMoreBob);
        getMoreBob->append("getMore", batchData.cursorId);
        getMoreBob->append("collection", batchData.nss.coll());
    }
}

void CollectionCloner::_beginCollectionCallback(const executor::TaskExecutor::CallbackArgs& cbd) {
    if (!cbd.status.isOK()) {
        _finishCallback(cbd.status);
        return;
    }

    UniqueLock lk(_mutex);
    if (!_idIndexSpec.isEmpty() && _options.autoIndexId == CollectionOptions::NO) {
        warning()
            << "Found the _id_ index spec but the collection specified autoIndexId of false on ns:"
            << this->_sourceNss;
    }

    auto status = _storageInterface->createCollectionForBulkLoading(
        _destNss, _options, _idIndexSpec, _indexSpecs);

    if (!status.isOK()) {
        lk.unlock();
        _finishCallback(status.getStatus());
        return;
    }

    _stats.indexes = _indexSpecs.size();
    if (!_idIndexSpec.isEmpty()) {
        ++_stats.indexes;
    }

    _collLoader = std::move(status.getValue());

    Status scheduleStatus = _findFetcher.schedule();
    if (!scheduleStatus.isOK()) {
        lk.unlock();
        _finishCallback(scheduleStatus);
        return;
    }
}

void CollectionCloner::_insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& cbd,
                                                bool lastBatch) {
    if (!cbd.status.isOK()) {
        _finishCallback(cbd.status);
        return;
    }

    std::vector<BSONObj> docs;
    UniqueLock lk(_mutex);
    if (_documents.size() == 0) {
        warning() << "_insertDocumentsCallback, but no documents to insert for ns:" << _destNss;

        if (lastBatch) {
            lk.unlock();
            _finishCallback(Status::OK());
        }
        return;
    }

    _documents.swap(docs);
    _stats.documentsCopied += docs.size();
    ++_stats.fetchBatches;
    _progressMeter.hit(int(docs.size()));
    invariant(_collLoader);
    const auto status = _collLoader->insertDocuments(docs.cbegin(), docs.cend());
    lk.unlock();

    if (!status.isOK()) {
        _finishCallback(status);
        return;
    }

    MONGO_FAIL_POINT_BLOCK(initialSyncHangDuringCollectionClone, options) {
        const BSONObj& data = options.getData();
        if (data["namespace"].String() == _destNss.ns() &&
            static_cast<int>(_stats.documentsCopied) >= data["numDocsToClone"].numberInt()) {
            log() << "initial sync - initialSyncHangDuringCollectionClone fail point "
                     "enabled. Blocking until fail point is disabled.";
            while (MONGO_FAIL_POINT(initialSyncHangDuringCollectionClone)) {
                mongo::sleepsecs(1);
            }
        }
    }

    if (!lastBatch) {
        return;
    }

    // Done with last batch and time to call _finshCallback with Status::OK().
    _finishCallback(Status::OK());
}

void CollectionCloner::_finishCallback(const Status& status) {
    LOG(1) << "CollectionCloner ns:" << _destNss << " finished with status: " << redact(status);
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
                warning() << "Failed to commit changes to collection " << _destNss.ns() << ": "
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
