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

#include "mongo/db/repl/database_cloner.h"

#include <algorithm>
#include <iterator>
#include <set>

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

const char* kNameFieldName = "name";
const char* kOptionsFieldName = "options";

// The number of retries for the listCollections commands.
const int numListCollectionsRetries = 1;

/**
 * Default listCollections predicate.
 */
bool acceptAllPred(const BSONObj&) {
    return true;
}

/**
 * Creates a listCollections command obj with an optional filter.
 */
BSONObj createListCollectionsCommandObject(const BSONObj& filter) {
    BSONObjBuilder output;
    output.append("listCollections", 1);
    if (!filter.isEmpty()) {
        output.append("filter", filter);
    }
    return output.obj();
}

}  // namespace

DatabaseCloner::DatabaseCloner(executor::TaskExecutor* executor,
                               const HostAndPort& source,
                               const std::string& dbname,
                               const BSONObj& listCollectionsFilter,
                               const ListCollectionsPredicateFn& listCollectionsPred,
                               StorageInterface* si,
                               const CollectionCallbackFn& collWork,
                               const CallbackFn& onCompletion)
    : _executor(executor),
      _source(source),
      _dbname(dbname),
      _listCollectionsFilter(listCollectionsFilter),
      _listCollectionsPredicate(listCollectionsPred ? listCollectionsPred : acceptAllPred),
      _storageInterface(si),
      _collectionWork(collWork),
      _onCompletion(onCompletion),
      _listCollectionsFetcher(_executor,
                              _source,
                              _dbname,
                              createListCollectionsCommandObject(_listCollectionsFilter),
                              stdx::bind(&DatabaseCloner::_listCollectionsCallback,
                                         this,
                                         stdx::placeholders::_1,
                                         stdx::placeholders::_2,
                                         stdx::placeholders::_3),
                              rpc::ServerSelectionMetadata(true, boost::none).toBSON(),
                              RemoteCommandRequest::kNoTimeout,
                              RemoteCommandRetryScheduler::makeRetryPolicy(
                                  numListCollectionsRetries,
                                  executor::RemoteCommandRequest::kNoTimeout,
                                  RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _scheduleDbWorkFn([this](const executor::TaskExecutor::CallbackFn& work) {
          return _executor->scheduleWork(work);
      }),
      _startCollectionCloner([](CollectionCloner& cloner) { return cloner.start(); }) {
    // Fetcher throws an exception on null executor.
    invariant(executor);
    uassert(ErrorCodes::BadValue, "empty database name", !dbname.empty());
    uassert(ErrorCodes::BadValue, "storage interface cannot be null", si);
    uassert(ErrorCodes::BadValue, "collection callback function cannot be null", collWork);
    uassert(ErrorCodes::BadValue, "callback function cannot be null", onCompletion);
}

DatabaseCloner::~DatabaseCloner() {
    DESTRUCTOR_GUARD(cancel(); wait(););
}

const std::vector<BSONObj>& DatabaseCloner::getCollectionInfos() const {
    LockGuard lk(_mutex);
    return _collectionInfos;
}

std::string DatabaseCloner::getDiagnosticString() const {
    LockGuard lk(_mutex);
    return _getDiagnosticString_inlock();
}

std::string DatabaseCloner::_getDiagnosticString_inlock() const {
    str::stream output;
    output << "DatabaseCloner";
    output << " executor: " << _executor->getDiagnosticString();
    output << " source: " << _source.toString();
    output << " database: " << _dbname;
    output << " listCollections filter" << _listCollectionsFilter;
    output << " active: " << _active;
    output << " collection info objects (empty if listCollections is in progress): "
           << _collectionInfos.size();
    return output;
}

bool DatabaseCloner::isActive() const {
    LockGuard lk(_mutex);
    return _active;
}

Status DatabaseCloner::start() {
    LockGuard lk(_mutex);

    if (_active) {
        return Status(ErrorCodes::IllegalOperation, "database cloner already started");
    }

    _stats.start = _executor->now();
    LOG(1) << "Scheduling listCollections call for database: " << _dbname;
    Status scheduleResult = _listCollectionsFetcher.schedule();
    if (!scheduleResult.isOK()) {
        error() << "Error scheduling listCollections for database: " << _dbname
                << ", error:" << scheduleResult;
        return scheduleResult;
    }

    _active = true;

    return Status::OK();
}

void DatabaseCloner::cancel() {
    {
        LockGuard lk(_mutex);

        if (!_active) {
            return;
        }
    }

    _listCollectionsFetcher.cancel();
}

DatabaseCloner::Stats DatabaseCloner::getStats() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stats;
}

void DatabaseCloner::wait() {
    UniqueLock lk(_mutex);
    _condition.wait(lk, [this]() { return !_active; });
}

void DatabaseCloner::setScheduleDbWorkFn(const CollectionCloner::ScheduleDbWorkFn& work) {
    LockGuard lk(_mutex);

    _scheduleDbWorkFn = work;
}

void DatabaseCloner::setStartCollectionClonerFn(
    const StartCollectionClonerFn& startCollectionCloner) {
    _startCollectionCloner = startCollectionCloner;
}

void DatabaseCloner::_listCollectionsCallback(const StatusWith<Fetcher::QueryResponse>& result,
                                              Fetcher::NextAction* nextAction,
                                              BSONObjBuilder* getMoreBob) {
    if (!result.isOK()) {
        _finishCallback({result.getStatus().code(),
                         str::stream() << "While issuing listCollections on db '" << _dbname
                                       << "' (host:"
                                       << _source.toString()
                                       << ") there was an error '"
                                       << result.getStatus().reason()
                                       << "'"});
        return;
    }

    auto batchData(result.getValue());
    auto&& documents = batchData.documents;

    UniqueLock lk(_mutex);
    // We may be called with multiple batches leading to a need to grow _collectionInfos.
    _collectionInfos.reserve(_collectionInfos.size() + documents.size());
    _stats.collections += documents.size();
    std::copy_if(documents.begin(),
                 documents.end(),
                 std::back_inserter(_collectionInfos),
                 _listCollectionsPredicate);

    // The fetcher will continue to call with kGetMore until an error or the last batch.
    if (*nextAction == Fetcher::NextAction::kGetMore) {
        invariant(getMoreBob);
        getMoreBob->append("getMore", batchData.cursorId);
        getMoreBob->append("collection", batchData.nss.coll());
        return;
    }

    // Nothing to do for an empty database.
    if (_collectionInfos.empty()) {
        _finishCallback_inlock(lk, Status::OK());
        return;
    }

    _collectionNamespaces.reserve(_collectionInfos.size());
    std::set<std::string> seen;
    for (auto&& info : _collectionInfos) {
        BSONElement nameElement = info.getField(kNameFieldName);
        if (nameElement.eoo()) {
            _finishCallback_inlock(
                lk,
                {ErrorCodes::FailedToParse,
                 str::stream() << "collection info must contain '" << kNameFieldName << "' "
                               << "field : "
                               << info});
            return;
        }
        if (nameElement.type() != mongo::String) {
            _finishCallback_inlock(
                lk,
                {ErrorCodes::TypeMismatch,
                 str::stream() << "'" << kNameFieldName << "' field must be a string: " << info});
            return;
        }
        const std::string collectionName = nameElement.String();
        if (seen.find(collectionName) != seen.end()) {
            _finishCallback_inlock(lk,
                                   {ErrorCodes::DuplicateKey,
                                    str::stream()
                                        << "collection info contains duplicate collection name "
                                        << "'"
                                        << collectionName
                                        << "': "
                                        << info});
            return;
        }

        BSONElement optionsElement = info.getField(kOptionsFieldName);
        if (optionsElement.eoo()) {
            _finishCallback_inlock(
                lk,
                {ErrorCodes::FailedToParse,
                 str::stream() << "collection info must contain '" << kOptionsFieldName << "' "
                               << "field : "
                               << info});
            return;
        }
        if (!optionsElement.isABSONObj()) {
            _finishCallback_inlock(lk,
                                   Status(ErrorCodes::TypeMismatch,
                                          str::stream() << "'" << kOptionsFieldName
                                                        << "' field must be an object: "
                                                        << info));
            return;
        }
        const BSONObj optionsObj = optionsElement.Obj();
        CollectionOptions options;
        Status parseStatus = options.parse(optionsObj);
        if (!parseStatus.isOK()) {
            _finishCallback_inlock(lk, parseStatus);
            return;
        }
        seen.insert(collectionName);

        _collectionNamespaces.emplace_back(_dbname, collectionName);
        auto&& nss = *_collectionNamespaces.crbegin();

        try {
            _collectionCloners.emplace_back(
                _executor,
                _source,
                nss,
                options,
                stdx::bind(
                    &DatabaseCloner::_collectionClonerCallback, this, stdx::placeholders::_1, nss),
                _storageInterface);
        } catch (const UserException& ex) {
            _finishCallback_inlock(lk, ex.toStatus());
            return;
        }
    }

    for (auto&& collectionCloner : _collectionCloners) {
        collectionCloner.setScheduleDbWorkFn(_scheduleDbWorkFn);
    }

    // Start first collection cloner.
    _currentCollectionClonerIter = _collectionCloners.begin();

    LOG(1) << "    cloning collection " << _currentCollectionClonerIter->getSourceNamespace();

    Status startStatus = _startCollectionCloner(*_currentCollectionClonerIter);
    if (!startStatus.isOK()) {
        LOG(1) << "    failed to start collection cloning on "
               << _currentCollectionClonerIter->getSourceNamespace() << ": " << startStatus;
        _finishCallback_inlock(lk, startStatus);
        return;
    }
}

void DatabaseCloner::_collectionClonerCallback(const Status& status, const NamespaceString& nss) {
    auto newStatus = status;

    UniqueLock lk(_mutex);
    if (!status.isOK()) {
        newStatus = {status.code(),
                     str::stream() << "While cloning collection '" << nss.toString()
                                   << "' there was an error '"
                                   << status.reason()
                                   << "'"};
        _failedNamespaces.push_back({newStatus, nss});
    }
    ++_stats.clonedCollections;

    // Forward collection cloner result to caller.
    // Failure to clone a collection does not stop the database cloner
    // from cloning the rest of the collections in the listCollections result.
    lk.unlock();
    _collectionWork(newStatus, nss);
    lk.lock();
    _currentCollectionClonerIter++;

    if (_currentCollectionClonerIter != _collectionCloners.end()) {
        Status startStatus = _startCollectionCloner(*_currentCollectionClonerIter);
        if (!startStatus.isOK()) {
            LOG(1) << "    failed to start collection cloning on "
                   << _currentCollectionClonerIter->getSourceNamespace() << ": " << startStatus;
            _finishCallback_inlock(lk, startStatus);
            return;
        }
        return;
    }

    Status finalStatus(Status::OK());
    if (_failedNamespaces.size() > 0) {
        finalStatus = {ErrorCodes::InitialSyncFailure,
                       str::stream() << "Failed to clone " << _failedNamespaces.size()
                                     << " collection(s) in '"
                                     << _dbname
                                     << "' from "
                                     << _source.toString()};
    }
    _finishCallback_inlock(lk, finalStatus);
}

void DatabaseCloner::_finishCallback(const Status& status) {
    _onCompletion(status);
    LockGuard lk(_mutex);
    _active = false;
    _condition.notify_all();
    _stats.end = _executor->now();
    LOG(1) << "    database: " << _dbname << ", stats: " << _stats.toString();
}

void DatabaseCloner::_finishCallback_inlock(UniqueLock& lk, const Status& status) {
    if (lk.owns_lock()) {
        lk.unlock();
    }
    _finishCallback(status);
}


std::string DatabaseCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj DatabaseCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.appendNumber("collections", collections);
    bob.appendNumber("clonedCollections", clonedCollections);
    bob.appendDate("start", start);
    bob.appendDate("end", end);
    auto elapsed = end - start;
    long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
    bob.appendNumber("elapsedMillis", elapsedMillis);
    return bob.obj();
}

}  // namespace repl
}  // namespace mongo
