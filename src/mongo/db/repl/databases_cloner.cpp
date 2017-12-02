/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/repl/databases_cloner.h"

#include <algorithm>
#include <iterator>
#include <set>

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameters.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

using Request = executor::RemoteCommandRequest;
using Response = executor::RemoteCommandResponse;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

// The number of attempts for the listDatabases commands.
MONGO_EXPORT_SERVER_PARAMETER(numInitialSyncListDatabasesAttempts, int, 3);

}  // namespace


DatabasesCloner::DatabasesCloner(StorageInterface* si,
                                 executor::TaskExecutor* exec,
                                 OldThreadPool* dbWorkThreadPool,
                                 HostAndPort source,
                                 IncludeDbFilterFn includeDbPred,
                                 OnFinishFn finishFn)
    : _status(ErrorCodes::NotYetInitialized, ""),
      _exec(exec),
      _dbWorkThreadPool(dbWorkThreadPool),
      _source(source),
      _includeDbFn(includeDbPred),
      _finishFn(finishFn),
      _storage(si) {
    uassert(ErrorCodes::InvalidOptions, "storage interface must be provided.", si);
    uassert(ErrorCodes::InvalidOptions, "executor must be provided.", exec);
    uassert(
        ErrorCodes::InvalidOptions, "db worker thread pool must be provided.", dbWorkThreadPool);
    uassert(ErrorCodes::InvalidOptions, "source must be provided.", !source.empty());
    uassert(ErrorCodes::InvalidOptions, "finishFn must be provided.", finishFn);
    uassert(ErrorCodes::InvalidOptions, "includeDbPred must be provided.", includeDbPred);
};

DatabasesCloner::~DatabasesCloner() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

std::string DatabasesCloner::toString() const {
    LockGuard lk(_mutex);
    return str::stream() << "initial sync --"
                         << " active:" << _isActive_inlock() << " status:" << _status.toString()
                         << " source:" << _source.toString()
                         << " db cloners completed:" << _stats.databasesCloned
                         << " db count:" << _databaseCloners.size();
}

void DatabasesCloner::join() {
    if (auto listDatabaseScheduler = _getListDatabasesScheduler()) {
        listDatabaseScheduler->join();
    }

    auto databaseCloners = _getDatabaseCloners();
    for (auto&& cloner : databaseCloners) {
        cloner->join();
    }
}

void DatabasesCloner::shutdown() {
    {
        LockGuard lock(_mutex);
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
    }

    if (auto listDatabaseScheduler = _getListDatabasesScheduler()) {
        listDatabaseScheduler->shutdown();
    }

    auto databaseCloners = _getDatabaseCloners();
    for (auto&& cloner : databaseCloners) {
        cloner->shutdown();
    }
}

bool DatabasesCloner::isActive() {
    LockGuard lk(_mutex);
    return _isActive_inlock();
}

bool DatabasesCloner::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status DatabasesCloner::getStatus() {
    LockGuard lk(_mutex);
    return _status;
}

DatabasesCloner::Stats DatabasesCloner::getStats() const {
    LockGuard lk(_mutex);
    DatabasesCloner::Stats stats = _stats;
    for (auto&& databaseCloner : _databaseCloners) {
        stats.databaseStats.emplace_back(databaseCloner->getStats());
    }
    return stats;
}

std::string DatabasesCloner::Stats::toString() const {
    return toBSON().toString();
}

BSONObj DatabasesCloner::Stats::toBSON() const {
    BSONObjBuilder bob;
    append(&bob);
    return bob.obj();
}

void DatabasesCloner::Stats::append(BSONObjBuilder* builder) const {
    builder->appendNumber("databasesCloned", databasesCloned);
    for (auto&& db : databaseStats) {
        BSONObjBuilder dbBuilder(builder->subobjStart(db.dbname));
        db.append(&dbBuilder);
        dbBuilder.doneFast();
    }
}

Status DatabasesCloner::startup() noexcept {
    LockGuard lk(_mutex);

    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::InternalError, "databases cloner already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress, "databases cloner shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress, "databases cloner completed");
    }

    if (!_status.isOK() && _status.code() != ErrorCodes::NotYetInitialized) {
        return _status;
    }

    // Schedule listDatabase command which will kick off the database cloner per result db. We only
    // retrieve database names since computing & fetching all database stats can be costly on the
    // remote node when there are a large number of collections.
    Request listDBsReq(_source,
                       "admin",
                       BSON("listDatabases" << true << "nameOnly" << true),
                       ReadPreferenceSetting::secondaryPreferredMetadata(),
                       nullptr);
    _listDBsScheduler = stdx::make_unique<RemoteCommandRetryScheduler>(
        _exec,
        listDBsReq,
        stdx::bind(&DatabasesCloner::_onListDatabaseFinish, this, stdx::placeholders::_1),
        RemoteCommandRetryScheduler::makeRetryPolicy(
            numInitialSyncListDatabasesAttempts.load(),
            executor::RemoteCommandRequest::kNoTimeout,
            RemoteCommandRetryScheduler::kAllRetriableErrors));
    _status = _listDBsScheduler->startup();

    if (!_status.isOK()) {
        _state = State::kComplete;
        return _status;
    }

    return Status::OK();
}

void DatabasesCloner::setScheduleDbWorkFn_forTest(const CollectionCloner::ScheduleDbWorkFn& work) {
    LockGuard lk(_mutex);
    _scheduleDbWorkFn = work;
}

StatusWith<std::vector<BSONElement>> DatabasesCloner::parseListDatabasesResponse_forTest(
    BSONObj dbResponse) {
    return _parseListDatabasesResponse(dbResponse);
}

void DatabasesCloner::setAdminAsFirst_forTest(std::vector<BSONElement>& dbsArray) {
    _setAdminAsFirst(dbsArray);
}

StatusWith<std::vector<BSONElement>> DatabasesCloner::_parseListDatabasesResponse(
    BSONObj dbResponse) {
    if (!dbResponse.hasField("databases")) {
        return Status(ErrorCodes::BadValue,
                      "The 'listDatabases' response does not contain a 'databases' field.");
    }
    BSONElement response = dbResponse["databases"];
    try {
        return response.Array();
    } catch (const AssertionException&) {
        return Status(ErrorCodes::BadValue,
                      "The 'listDatabases' response is unable to be transformed into an array.");
    }
}

void DatabasesCloner::_setAdminAsFirst(std::vector<BSONElement>& dbsArray) {
    auto adminIter = std::find_if(dbsArray.begin(), dbsArray.end(), [](BSONElement elem) {
        if (!elem.isABSONObj()) {
            return false;
        }
        auto bsonObj = elem.Obj();
        std::string databaseName = bsonObj.getStringField("name");
        return (databaseName == "admin");
    });
    if (adminIter != dbsArray.end()) {
        std::iter_swap(adminIter, dbsArray.begin());
    }
}

void DatabasesCloner::_onListDatabaseFinish(const CommandCallbackArgs& cbd) {
    Status respStatus = cbd.response.status;
    if (respStatus.isOK()) {
        respStatus = getStatusFromCommandResult(cbd.response.data);
    }

    UniqueLock lk(_mutex);
    if (!respStatus.isOK()) {
        LOG(1) << "'listDatabases' failed: " << respStatus;
        _fail_inlock(&lk, respStatus);
        return;
    }

    // There should not be any cloners yet.
    invariant(_databaseCloners.size() == 0);
    const auto respBSON = cbd.response.data;

    auto databasesArray = _parseListDatabasesResponse(respBSON);
    if (!databasesArray.isOK()) {
        LOG(1) << "'listDatabases' returned a malformed response: "
               << databasesArray.getStatus().toString();
        _fail_inlock(&lk, databasesArray.getStatus());
        return;
    }

    auto dbsArray = databasesArray.getValue();
    // Ensure that the 'admin' database is the first element in the array of databases so that it
    // will be the first to be cloned. This allows users to authenticate against a database while
    // initial sync is occurring.
    _setAdminAsFirst(dbsArray);

    for (BSONElement arrayElement : dbsArray) {
        const BSONObj dbBSON = arrayElement.Obj();

        // Check to see if we want to exclude this db from the clone.
        if (!_includeDbFn(dbBSON)) {
            LOG(1) << "Excluding database from the 'listDatabases' response: " << dbBSON;
            continue;
        }

        if (!dbBSON.hasField("name")) {
            LOG(1) << "Excluding database due to the 'listDatabases' response not containing a "
                      "'name' field for this entry: "
                   << dbBSON;
        }

        const std::string dbName = dbBSON["name"].str();
        std::shared_ptr<DatabaseCloner> dbCloner{nullptr};

        // filters for DatabasesCloner.
        const auto collectionFilterPred = [dbName](const BSONObj& collInfo) {
            const auto collName = collInfo["name"].str();
            const NamespaceString ns(dbName, collName);
            if (ns.isSystem() && !ns.isLegalClientSystemNS()) {
                LOG(1) << "Skipping 'system' collection: " << ns.ns();
                return false;
            }
            if (!ns.isNormal()) {
                LOG(1) << "Skipping non-normal collection: " << ns.ns();
                return false;
            }

            LOG(2) << "Allowing cloning of collectionInfo: " << collInfo;
            return true;
        };
        const auto onCollectionFinish = [](const Status& status, const NamespaceString& srcNss) {
            if (status.isOK()) {
                LOG(1) << "collection clone finished: " << srcNss;
            } else {
                warning() << "collection clone for '" << srcNss << "' failed due to "
                          << status.toString();
            }
        };
        const auto onDbFinish = [this, dbName](const Status& status) {
            _onEachDBCloneFinish(status, dbName);
        };
        Status startStatus = Status::OK();
        try {
            dbCloner.reset(new DatabaseCloner(
                _exec,
                _dbWorkThreadPool,
                _source,
                dbName,
                BSONObj(),  // do not filter collections out during listCollections call.
                collectionFilterPred,
                _storage,  // use storage provided.
                onCollectionFinish,
                onDbFinish));
            if (_scheduleDbWorkFn) {
                dbCloner->setScheduleDbWorkFn_forTest(_scheduleDbWorkFn);
            }
            // Start first database cloner.
            if (_databaseCloners.empty()) {
                startStatus = dbCloner->startup();
            }
        } catch (...) {
            startStatus = exceptionToStatus();
        }

        if (!startStatus.isOK()) {
            std::string err = str::stream() << "could not create cloner for database: " << dbName
                                            << " due to: " << startStatus.toString();
            _setStatus_inlock({ErrorCodes::InitialSyncFailure, err});
            error() << err;
            break;  // exit for_each loop
        }

        // add cloner to list.
        _databaseCloners.push_back(dbCloner);
    }
    if (_databaseCloners.size() == 0) {
        if (_status.isOK()) {
            _succeed_inlock(&lk);
        } else {
            _fail_inlock(&lk, _status);
        }
    }
}

std::vector<std::shared_ptr<DatabaseCloner>> DatabasesCloner::_getDatabaseCloners() const {
    LockGuard lock(_mutex);
    return _databaseCloners;
}

RemoteCommandRetryScheduler* DatabasesCloner::_getListDatabasesScheduler() const {
    LockGuard lock(_mutex);
    return _listDBsScheduler.get();
}

void DatabasesCloner::_onEachDBCloneFinish(const Status& status, const std::string& name) {
    UniqueLock lk(_mutex);
    if (!status.isOK()) {
        warning() << "database '" << name << "' (" << (_stats.databasesCloned + 1) << " of "
                  << _databaseCloners.size() << ") clone failed due to " << status.toString();
        _fail_inlock(&lk, status);
        return;
    }

    if (StringData(name).equalCaseInsensitive("admin")) {
        LOG(1) << "Finished the 'admin' db, now calling isAdminDbValid.";
        // Do special checks for the admin database because of auth. collections.
        auto adminStatus = Status(ErrorCodes::NotYetInitialized, "");
        {
            // TODO: Move isAdminDbValid() out of the collection/database cloner code paths.
            OperationContext* opCtx = cc().getOperationContext();
            ServiceContext::UniqueOperationContext opCtxPtr;
            if (!opCtx) {
                opCtxPtr = cc().makeOperationContext();
                opCtx = opCtxPtr.get();
            }
            adminStatus = _storage->isAdminDbValid(opCtx);
        }
        if (!adminStatus.isOK()) {
            LOG(1) << "Validation failed on 'admin' db due to " << adminStatus;
            _fail_inlock(&lk, adminStatus);
            return;
        }
    }

    _stats.databasesCloned++;

    if (_stats.databasesCloned == _databaseCloners.size()) {
        _succeed_inlock(&lk);
        return;
    }

    // Start next database cloner.
    auto&& dbCloner = _databaseCloners[_stats.databasesCloned];
    auto startStatus = dbCloner->startup();
    if (!startStatus.isOK()) {
        warning() << "failed to schedule database '" << name << "' ("
                  << (_stats.databasesCloned + 1) << " of " << _databaseCloners.size()
                  << ") due to " << startStatus.toString();
        _fail_inlock(&lk, startStatus);
        return;
    }
}

void DatabasesCloner::_fail_inlock(UniqueLock* lk, Status status) {
    LOG(3) << "DatabasesCloner::_fail_inlock called";
    if (!_isActive_inlock()) {
        return;
    }

    _setStatus_inlock(status);
    // TODO: shutdown outstanding work, like any cloners active
    invariant(_finishFn);
    auto finish = _finishFn;
    _finishFn = {};
    lk->unlock();

    LOG(3) << "DatabasesCloner - calling _finishFn with status: " << _status;
    finish(status);

    // Release any resources that might be held by the '_finishFn' (moved to 'finish') function
    // object.
    finish = OnFinishFn();

    lk->lock();
    invariant(_state != State::kComplete);
    _state = State::kComplete;
}

void DatabasesCloner::_succeed_inlock(UniqueLock* lk) {
    LOG(3) << "DatabasesCloner::_succeed_inlock called";
    const auto status = Status::OK();
    _setStatus_inlock(status);
    invariant(_finishFn);
    auto finish = _finishFn;
    _finishFn = OnFinishFn();
    lk->unlock();

    LOG(3) << "DatabasesCloner - calling _finishFn with status OK";
    finish(status);

    // Release any resources that might be held by the '_finishFn' (moved to 'finish') function
    // object.
    finish = OnFinishFn();

    lk->lock();
    invariant(_state != State::kComplete);
    _state = State::kComplete;
}

void DatabasesCloner::_setStatus_inlock(Status s) {
    // Only set the first time called, all subsequent failures are not recorded --only first.
    if (!s.isOK() && (_status.isOK() || _status == ErrorCodes::NotYetInitialized)) {
        LOG(1) << "setting DatabasesCloner status to " << s;
        _status = s;
    }
}

}  // namespace repl
}  // namespace mongo
