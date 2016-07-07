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

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

// The number of retries for the listIndexes commands.
const size_t numListIndexesRetries = 1;
// The number of retries for the find command, which gets the data.
const size_t numFindRetries = 3;
}  // namespace

CollectionCloner::CollectionCloner(ReplicationExecutor* executor,
                                   const HostAndPort& source,
                                   const NamespaceString& sourceNss,
                                   const CollectionOptions& options,
                                   const CallbackFn& onCompletion,
                                   StorageInterface* storageInterface)
    : _executor(executor),
      _source(source),
      _sourceNss(sourceNss),
      _destNss(_sourceNss),
      _options(options),
      _onCompletion(onCompletion),
      _storageInterface(storageInterface),
      _active(false),
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
                              numListIndexesRetries,
                              executor::RemoteCommandRequest::kNoTimeout,
                              RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _findFetcher(_executor,
                   _source,
                   _sourceNss.db().toString(),
                   BSON("find" << _sourceNss.coll() << "noCursorTimeout" << true),  // SERVER-1387
                   stdx::bind(&CollectionCloner::_findCallback,
                              this,
                              stdx::placeholders::_1,
                              stdx::placeholders::_2,
                              stdx::placeholders::_3),
                   rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
                   RemoteCommandRequest::kNoTimeout,
                   RemoteCommandRetryScheduler::makeRetryPolicy(
                       numFindRetries,
                       executor::RemoteCommandRequest::kNoTimeout,
                       RemoteCommandRetryScheduler::kAllRetriableErrors)),

      _indexSpecs(),
      _documents(),
      _dbWorkCallbackHandle(),
      _scheduleDbWorkFn([this](const ReplicationExecutor::CallbackFn& work) {
          auto status = _executor->scheduleDBWork(work);
          if (status.isOK()) {
              LockGuard lk(_mutex);
              _dbWorkCallbackHandle = status.getValue();
          }
          return status;
      }) {
    uassert(ErrorCodes::BadValue, "null replication executor", executor);
    uassert(ErrorCodes::BadValue,
            "invalid collection namespace: " + sourceNss.ns(),
            sourceNss.isValid());
    uassertStatusOK(options.validate());
    uassert(ErrorCodes::BadValue, "callback function cannot be null", onCompletion);
    uassert(ErrorCodes::BadValue, "null storage interface", storageInterface);
}

CollectionCloner::~CollectionCloner() {
    DESTRUCTOR_GUARD(cancel(); wait(););
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
    output << " active: " << _active;
    output << " listIndexes fetcher: " << _listIndexesFetcher.getDiagnosticString();
    output << " find fetcher: " << _findFetcher.getDiagnosticString();
    output << " database worked callback handle: "
           << (_dbWorkCallbackHandle.isValid() ? "valid" : "invalid");
    return output;
}

bool CollectionCloner::isActive() const {
    LockGuard lk(_mutex);
    return _active;
}

Status CollectionCloner::start() {
    LockGuard lk(_mutex);
    LOG(0) << "CollectionCloner::start called, on ns:" << _destNss;

    if (_active) {
        return Status(ErrorCodes::IllegalOperation, "collection cloner already started");
    }

    _stats.start = _executor->now();
    Status scheduleResult = _listIndexesFetcher.schedule();
    if (!scheduleResult.isOK()) {
        return scheduleResult;
    }

    _active = true;

    return Status::OK();
}

void CollectionCloner::cancel() {
    ReplicationExecutor::CallbackHandle dbWorkCallbackHandle;
    {
        LockGuard lk(_mutex);

        if (!_active) {
            return;
        }

        dbWorkCallbackHandle = _dbWorkCallbackHandle;
    }

    _listIndexesFetcher.cancel();
    _findFetcher.cancel();

    if (dbWorkCallbackHandle.isValid()) {
        _executor->cancel(dbWorkCallbackHandle);
    }
}

CollectionCloner::Stats CollectionCloner::getStats() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stats;
}

void CollectionCloner::wait() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _condition.wait(lk, [this]() { return !_active; });
}

void CollectionCloner::waitForDbWorker() {
    ReplicationExecutor::CallbackHandle dbWorkCallbackHandle;
    {
        LockGuard lk(_mutex);

        if (!_active) {
            return;
        }

        dbWorkCallbackHandle = _dbWorkCallbackHandle;
    }

    if (dbWorkCallbackHandle.isValid()) {
        _executor->wait(dbWorkCallbackHandle);
    }
}

void CollectionCloner::setScheduleDbWorkFn(const ScheduleDbWorkFn& scheduleDbWorkFn) {
    LockGuard lk(_mutex);

    _scheduleDbWorkFn = [this, scheduleDbWorkFn](const ReplicationExecutor::CallbackFn& work) {
        auto status = scheduleDbWorkFn(work);
        if (status.isOK()) {
            _dbWorkCallbackHandle = status.getValue();
        }
        return status;
    };
}

void CollectionCloner::_listIndexesCallback(const Fetcher::QueryResponseStatus& fetchResult,
                                            Fetcher::NextAction* nextAction,
                                            BSONObjBuilder* getMoreBob) {
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
        Status newStatus{fetchResult.getStatus().code(),
                         str::stream() << "While querying collection '" << _sourceNss.ns()
                                       << "' there was an error '"
                                       << fetchResult.getStatus().reason()
                                       << "'"};
        // TODO: cancel active inserts?
        _finishCallback(newStatus);
        return;
    }

    auto batchData(fetchResult.getValue());
    bool lastBatch = *nextAction == Fetcher::NextAction::kNoAction;
    if (batchData.documents.size() > 0) {
        UniqueLock lk(_mutex);
        _documents.insert(_documents.end(), batchData.documents.begin(), batchData.documents.end());
        lk.unlock();
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
    } else {
        if (batchData.first && !batchData.cursorId) {
            // Empty collection.
            _finishCallback(Status::OK());
        } else {
            warning() << "No documents returned in batch; ns: " << _sourceNss
                      << ", noCursorId:" << batchData.cursorId << ", lastBatch:" << lastBatch;
            _finishCallback({ErrorCodes::IllegalOperation, "Cursor batch returned no documents."});
        }
        return;
    }

    if (!lastBatch) {
        invariant(getMoreBob);
        getMoreBob->append("getMore", batchData.cursorId);
        getMoreBob->append("collection", batchData.nss.coll());
    }
}

void CollectionCloner::_beginCollectionCallback(const ReplicationExecutor::CallbackArgs& cbd) {
    if (!cbd.status.isOK()) {
        _finishCallback(cbd.status);
        return;
    }

    UniqueLock lk(_mutex);
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

void CollectionCloner::_insertDocumentsCallback(const ReplicationExecutor::CallbackArgs& cbd,
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
    _stats.documents += docs.size();
    ++_stats.fetchBatches;
    const auto status = _collLoader->insertDocuments(docs.cbegin(), docs.cend());
    lk.unlock();

    if (!status.isOK()) {
        _finishCallback(status);
        return;
    }

    if (!lastBatch) {
        return;
    }

    _finishCallback(Status::OK());
}

void CollectionCloner::_finishCallback(const Status& status) {
    LOG(1) << "CollectionCloner ns:" << _destNss << " finished with status: " << status;
    // Copy the status so we can change it below if needed.
    auto finalStatus = status;
    bool callCollectionLoader = false;
    UniqueLock lk(_mutex);
    callCollectionLoader = _collLoader.operator bool();
    lk.unlock();
    if (callCollectionLoader) {
        if (finalStatus.isOK()) {
            const auto loaderStatus = _collLoader->commit();
            if (!loaderStatus.isOK()) {
                warning() << "Failed to commit changes to collection " << _destNss.ns() << ": "
                          << loaderStatus;
                finalStatus = loaderStatus;
            }
        }

        // This will release the resources held by the loader.
        _collLoader.reset();
    }
    _onCompletion(finalStatus);
    lk.lock();
    _stats.end = _executor->now();
    _active = false;
    _condition.notify_all();
    LOG(1) << "    collection: " << _destNss << ", stats: " << _stats.toString();
}


std::string CollectionCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj CollectionCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.appendNumber("documents", documents);
    bob.appendNumber("indexes", indexes);
    bob.appendNumber("fetchedBatches", fetchBatches);
    bob.appendDate("start", start);
    bob.appendDate("end", end);
    auto elapsed = end - start;
    long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
    bob.appendNumber("elapsedMillis", elapsedMillis);
    return bob.obj();
}

}  // namespace repl
}  // namespace mongo
