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
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameters.h"
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
const char* kInfoFieldName = "info";
const char* kUUIDFieldName = "uuid";

// The number of attempts for the listCollections commands.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncListCollectionsAttempts, int, 3);

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
                               OldThreadPool* dbWorkThreadPool,
                               const HostAndPort& source,
                               const std::string& dbname,
                               const BSONObj& listCollectionsFilter,
                               const ListCollectionsPredicateFn& listCollectionsPred,
                               StorageInterface* si,
                               const CollectionCallbackFn& collWork,
                               const CallbackFn& onCompletion)
    : _executor(executor),
      _dbWorkThreadPool(dbWorkThreadPool),
      _source(source),
      _dbname(dbname),
      _listCollectionsFilter(
          listCollectionsFilter.isEmpty()
              ? ListCollectionsFilter::makeTypeCollectionFilter()
              : ListCollectionsFilter::addTypeCollectionFilter(listCollectionsFilter)),
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
                                  numInitialSyncListCollectionsAttempts.load(),
                                  executor::RemoteCommandRequest::kNoTimeout,
                                  RemoteCommandRetryScheduler::kAllRetriableErrors)),
      _startCollectionCloner([](CollectionCloner& cloner) { return cloner.startup(); }) {
    // Fetcher throws an exception on null executor.
    invariant(executor);
    uassert(ErrorCodes::BadValue, "db worker thread pool cannot be null", dbWorkThreadPool);
    uassert(ErrorCodes::BadValue, "empty database name", !dbname.empty());
    uassert(ErrorCodes::BadValue, "storage interface cannot be null", si);
    uassert(ErrorCodes::BadValue, "collection callback function cannot be null", collWork);
    uassert(ErrorCodes::BadValue, "callback function cannot be null", onCompletion);

    _stats.dbname = _dbname;
}

DatabaseCloner::~DatabaseCloner() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

const std::vector<BSONObj>& DatabaseCloner::getCollectionInfos_forTest() const {
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
    output << " active: " << _isActive_inlock();
    output << " collection info objects (empty if listCollections is in progress): "
           << _collectionInfos.size();
    return output;
}

bool DatabaseCloner::isActive() const {
    LockGuard lk(_mutex);
    return _isActive_inlock();
}

bool DatabaseCloner::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status DatabaseCloner::startup() noexcept {
    LockGuard lk(_mutex);

    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::InternalError, "database cloner already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress, "database cloner shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress, "database cloner completed");
    }

    _stats.start = _executor->now();
    LOG(1) << "Scheduling listCollections call for database: " << _dbname;
    Status scheduleResult = _listCollectionsFetcher.schedule();
    if (!scheduleResult.isOK()) {
        error() << "Error scheduling listCollections for database: " << _dbname
                << ", error:" << scheduleResult;
        _state = State::kComplete;
        return scheduleResult;
    }

    return Status::OK();
}

void DatabaseCloner::shutdown() {
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

    for (auto&& collectionCloner : _collectionCloners) {
        collectionCloner.shutdown();
    }

    _listCollectionsFetcher.shutdown();
}

DatabaseCloner::Stats DatabaseCloner::getStats() const {
    LockGuard lk(_mutex);
    DatabaseCloner::Stats stats = _stats;
    for (auto&& collectionCloner : _collectionCloners) {
        stats.collectionStats.emplace_back(collectionCloner.getStats());
    }
    return stats;
}

void DatabaseCloner::join() {
    UniqueLock lk(_mutex);
    _condition.wait(lk, [this]() { return !_isActive_inlock(); });
}

void DatabaseCloner::setScheduleDbWorkFn_forTest(const CollectionCloner::ScheduleDbWorkFn& work) {
    LockGuard lk(_mutex);

    _scheduleDbWorkFn = work;
}

void DatabaseCloner::setStartCollectionClonerFn(
    const StartCollectionClonerFn& startCollectionCloner) {
    _startCollectionCloner = startCollectionCloner;
}

DatabaseCloner::State DatabaseCloner::getState_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _state;
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
    std::copy_if(documents.begin(),
                 documents.end(),
                 std::back_inserter(_collectionInfos),
                 _listCollectionsPredicate);
    _stats.collections += _collectionInfos.size();

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
        Status parseStatus = options.parse(optionsObj, CollectionOptions::parseForCommand);
        if (!parseStatus.isOK()) {
            _finishCallback_inlock(lk, parseStatus);
            return;
        }

        BSONElement infoElement = info.getField(kInfoFieldName);
        if (infoElement.isABSONObj()) {
            BSONElement uuidElement = infoElement[kUUIDFieldName];
            if (!uuidElement.eoo()) {
                auto res = CollectionUUID::parse(uuidElement);
                if (!res.isOK()) {
                    _finishCallback_inlock(lk, res.getStatus());
                    return;
                }
                options.uuid = res.getValue();
            }
        }
        // TODO(SERVER-27994): Ensure UUID present when FCV >= "3.6".

        seen.insert(collectionName);

        _collectionNamespaces.emplace_back(_dbname, collectionName);
        auto&& nss = *_collectionNamespaces.crbegin();

        try {
            _collectionCloners.emplace_back(
                _executor,
                _dbWorkThreadPool,
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

    if (_scheduleDbWorkFn) {
        for (auto&& collectionCloner : _collectionCloners) {
            collectionCloner.setScheduleDbWorkFn_forTest(_scheduleDbWorkFn);
        }
    }

    // Start first collection cloner.
    _currentCollectionClonerIter = _collectionCloners.begin();

    LOG(1) << "    cloning collection " << _currentCollectionClonerIter->getSourceNamespace();

    Status startStatus = _startCollectionCloner(*_currentCollectionClonerIter);
    if (!startStatus.isOK()) {
        LOG(1) << "    failed to start collection cloning on "
               << _currentCollectionClonerIter->getSourceNamespace() << ": " << redact(startStatus);
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
                   << _currentCollectionClonerIter->getSourceNamespace() << ": "
                   << redact(startStatus);
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
    invariant(_state != State::kComplete);
    _state = State::kComplete;
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

std::string DatabaseCloner::getDBName() const {
    return _dbname;
}

std::string DatabaseCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj DatabaseCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.append("dbname", dbname);
    append(&bob);
    return bob.obj();
}

void DatabaseCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("collections", collections);
    builder->appendNumber("clonedCollections", clonedCollections);
    if (start != Date_t()) {
        builder->appendDate("start", start);
        if (end != Date_t()) {
            builder->appendDate("end", end);
            auto elapsed = end - start;
            long long elapsedMillis = duration_cast<Milliseconds>(elapsed).count();
            builder->appendNumber("elapsedMillis", elapsedMillis);
        }
    }

    for (auto&& collection : collectionStats) {
        BSONObjBuilder collectionBuilder(builder->subobjStart(collection.ns));
        collection.append(&collectionBuilder);
        collectionBuilder.doneFast();
    }
}

std::ostream& operator<<(std::ostream& os, const DatabaseCloner::State& state) {
    switch (state) {
        case DatabaseCloner::State::kPreStart:
            return os << "PreStart";
        case DatabaseCloner::State::kRunning:
            return os << "Running";
        case DatabaseCloner::State::kShuttingDown:
            return os << "ShuttingDown";
        case DatabaseCloner::State::kComplete:
            return os << "Complete";
    }
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
