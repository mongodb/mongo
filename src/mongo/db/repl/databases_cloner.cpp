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
#include "mongo/db/repl/storage_interface.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
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

const size_t numListDatabasesRetries = 1;

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

std::string DatabasesCloner::toString() const {
    return str::stream() << "initial sync --"
                         << " active:" << _active << " status:" << _status.toString()
                         << " source:" << _source.toString()
                         << " db cloners active:" << _clonersActive
                         << " db count:" << _databaseCloners.size();
}

void DatabasesCloner::join() {
    UniqueLock lk(_mutex);
    if (!_active) {
        return;
    }

    std::vector<std::shared_ptr<DatabaseCloner>> clonersToWaitOn;
    for (auto&& cloner : _databaseCloners) {
        if (cloner && cloner->isActive()) {
            clonersToWaitOn.push_back(cloner);
        }
    }

    lk.unlock();
    for (auto&& cloner : clonersToWaitOn) {
        cloner->wait();
    }
    lk.lock();
}

void DatabasesCloner::shutdown() {
    UniqueLock lk(_mutex);
    if (!_active)
        return;
    _active = false;
    _setStatus_inlock({ErrorCodes::CallbackCanceled, "Initial Sync Cancelled."});
    _cancelCloners_inlock(lk);
}

void DatabasesCloner::_cancelCloners_inlock(UniqueLock& lk) {
    std::vector<std::shared_ptr<DatabaseCloner>> clonersToCancel;
    for (auto&& cloner : _databaseCloners) {
        if (cloner && cloner->isActive()) {
            clonersToCancel.push_back(cloner);
        }
    }

    lk.unlock();
    for (auto&& cloner : clonersToCancel) {
        cloner->cancel();
    }
    lk.lock();
}

bool DatabasesCloner::isActive() {
    LockGuard lk(_mutex);
    return _active;
}

Status DatabasesCloner::getStatus() {
    LockGuard lk(_mutex);
    return _status;
}

Status DatabasesCloner::startup() {
    UniqueLock lk(_mutex);
    invariant(!_active);
    _active = true;

    if (!_status.isOK() && _status.code() != ErrorCodes::NotYetInitialized) {
        return _status;
    }

    _status = Status::OK();

    // Schedule listDatabase command which will kick off the database cloner per result db.
    Request listDBsReq(_source,
                       "admin",
                       BSON("listDatabases" << true),
                       rpc::ServerSelectionMetadata(true, boost::none).toBSON());
    _listDBsScheduler = stdx::make_unique<RemoteCommandRetryScheduler>(
        _exec,
        listDBsReq,
        stdx::bind(&DatabasesCloner::_onListDatabaseFinish, this, stdx::placeholders::_1),
        RemoteCommandRetryScheduler::makeRetryPolicy(
            numListDatabasesRetries,
            executor::RemoteCommandRequest::kNoTimeout,
            RemoteCommandRetryScheduler::kAllRetriableErrors));
    auto s = _listDBsScheduler->startup();
    if (!s.isOK()) {
        _setStatus_inlock(s);
        _failed_inlock(lk);
    }

    return _status;
}

void DatabasesCloner::_onListDatabaseFinish(const CommandCallbackArgs& cbd) {
    Status respStatus = cbd.response.getStatus();
    if (respStatus.isOK()) {
        respStatus = getStatusFromCommandResult(cbd.response.getValue().data);
    }

    UniqueLock lk(_mutex);
    if (!respStatus.isOK()) {
        LOG(1) << "listDatabases failed: " << respStatus;
        _setStatus_inlock(respStatus);
        _failed_inlock(lk);
        return;
    }

    const auto respBSON = cbd.response.getValue().data;
    // There should not be any cloners yet
    invariant(_databaseCloners.size() == 0);
    const auto dbsElem = respBSON["databases"].Obj();
    BSONForEach(arrayElement, dbsElem) {
        const BSONObj dbBSON = arrayElement.Obj();

        // Check to see if we want to exclude this db from the clone.
        if (!_includeDbFn(dbBSON)) {
            LOG(1) << "excluding db: " << dbBSON;
            continue;
        }

        const std::string dbName = dbBSON["name"].str();
        ++_clonersActive;
        std::shared_ptr<DatabaseCloner> dbCloner{nullptr};
        Status startStatus(ErrorCodes::NotYetInitialized,
                           "The DatabasesCloner could not be started.");

        // filters for DatabasesCloner.
        const auto collectionFilterPred = [dbName](const BSONObj& collInfo) {
            const auto collName = collInfo["name"].str();
            const NamespaceString ns(dbName, collName);
            if (ns.isSystem() && !legalClientSystemNS(ns.ns(), true)) {
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
        try {
            dbCloner.reset(new DatabaseCloner(
                _exec,
                _source,
                dbName,
                BSONObj(),  // do not filter collections out during listCollections call.
                collectionFilterPred,
                _storage,  // use storage provided.
                onCollectionFinish,
                onDbFinish));
            // Start database cloner.
            startStatus = dbCloner->start();
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
            _active = false;
            lk.unlock();
            _finishFn(_status);
        } else {
            _failed_inlock(lk);
        }
    }
}

void DatabasesCloner::_onEachDBCloneFinish(const Status& status, const std::string& name) {
    UniqueLock lk(_mutex);
    auto clonersLeft = --_clonersActive;

    if (!status.isOK()) {
        warning() << "database '" << name << "' clone failed due to " << status.toString();
        _setStatus_inlock(status);
        if (clonersLeft == 0) {
            _failed_inlock(lk);
        } else {
            // After cancellation this callback will called until clonersLeft = 0.
            _cancelCloners_inlock(lk);
        }
        return;
    }

    LOG(2) << "Database clone finished: " << name;
    if (StringData(name).equalCaseInsensitive("admin")) {
        LOG(1) << "Finished the 'admin' db, now calling isAdminDbValid.";
        // Do special checks for the admin database because of auth. collections.
        const auto adminStatus = _storage->isAdminDbValid(nullptr /* TODO: wire in txn*/);
        if (!adminStatus.isOK()) {
            _setStatus_inlock(adminStatus);
        }
    }

    if (clonersLeft == 0) {
        _active = false;
        // All cloners are done, trigger event.
        LOG(2) << "All database clones finished, calling _finishFn.";
        lk.unlock();
        _finishFn(_status);
        return;
    }
}

void DatabasesCloner::_failed_inlock(UniqueLock& lk) {
    LOG(3) << "DatabasesCloner::_failed_inlock";
    if (!_active) {
        return;
    }
    _active = false;

    // TODO: shutdown outstanding work, like any cloners active
    auto finish = _finishFn;
    lk.unlock();

    LOG(3) << "calling _finishFn with status: " << _status;
    _finishFn(_status);
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
