/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#pragma once

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/s/shard_id.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

class ConnectionString;
class ServiceContext;
class OperationContext;

/**
 * Provides infrastructure for retrieving session information that needs to be migrated from
 * the source migration shard.
 */
class SessionCatalogMigrationDestination {
    MONGO_DISALLOW_COPYING(SessionCatalogMigrationDestination);

public:
    enum class State {
        NotStarted,
        Migrating,
        ReadyToCommit,
        Committing,
        ErrorOccurred,
        Done,
    };

    static const char kSessionMigrateOplogTag[];

    SessionCatalogMigrationDestination(ShardId fromShard, MigrationSessionId migrationSessionId);
    ~SessionCatalogMigrationDestination();

    /**
     * Spawns a separate thread to initiate the session info transfer to this shard.
     */
    void start(ServiceContext* service);

    /**
     * Signals to this object that the source shard will no longer create generate new
     * session info to transfer. In other words, once the source shard returns an empty
     * result for session info to transfer after this call, it is safe for this to stop.
     */
    void finish();

    /**
     * Joins the spawned thread called by start(). Should only be called after finish()
     * was called.
     */
    void join();

    /**
     * Forces this into an error state which will also stop session transfer thread.
     */
    void forceFail(std::string& errMsg);

    /**
     * Returns the current state.
     */
    State getState();

    /**
     * Returns the error message stored. This is only valid when getState() == ErrorOccurred.
     */
    std::string getErrMsg();

private:
    void _retrieveSessionStateFromSource(ServiceContext* service);

    void _errorOccurred(StringData errMsg);

    const ShardId _fromShard;
    const MigrationSessionId _migrationSessionId;

    stdx::thread _thread;

    // Protects _state and _errMsg.
    stdx::mutex _mutex;
    stdx::condition_variable _isStateChanged;
    State _state = State::NotStarted;
    std::string _errMsg;  // valid only if _state == ErrorOccurred.
};

}  // namespace mongo
