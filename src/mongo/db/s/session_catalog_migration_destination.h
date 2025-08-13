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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"

#include <memory>
#include <string>

namespace mongo {

class ConnectionString;
class ServiceContext;
class OperationContext;

/**
 * Provides infrastructure for retrieving session information that needs to be migrated from
 * the source migration shard.
 */
class SessionCatalogMigrationDestination {
    SessionCatalogMigrationDestination(const SessionCatalogMigrationDestination&) = delete;
    SessionCatalogMigrationDestination& operator=(const SessionCatalogMigrationDestination&) =
        delete;

public:
    enum class State {
        NotStarted,
        Migrating,
        ReadyToCommit,
        Committing,
        ErrorOccurred,
        Done,
    };

    struct ProcessOplogResult {
        LogicalSessionId sessionId;
        TxnNumber txnNum{kUninitializedTxnNumber};

        repl::OpTime oplogTime;
        bool isPrePostImage = false;
    };

    SessionCatalogMigrationDestination(NamespaceString nss,
                                       ShardId fromShard,
                                       MigrationSessionId migrationSessionId,
                                       CancellationToken cancellationToken);
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
     * Returns true if the thread to initiate the session info transfer has been spawned and is
     * therefore joinable.
     */
    bool joinable() const;

    /**
     * Joins the spawned thread called by start(). Should only be called after finish()
     * was called.
     */
    void join();

    /**
     * Forces this into an error state which will also stop session transfer thread.
     */
    void forceFail(StringData errMsg);

    /**
     * Returns the session id for the migration.
     */
    MigrationSessionId getMigrationSessionId() const;

    /**
     * Returns the current state.
     */
    State getState();

    /**
     * Returns the error message stored. This is only valid when getState() == ErrorOccurred.
     */
    std::string getErrMsg();

    /**
     * Returns the number of session oplog entries processed by the _processSessionOplog() method
     */
    long long getSessionOplogEntriesMigrated();

private:
    void _retrieveSessionStateFromSource(ServiceContext* service);

    ProcessOplogResult _processSessionOplog(const BSONObj& oplogBSON,
                                            const ProcessOplogResult& lastResult,
                                            ServiceContext* serviceContext,
                                            CancellationToken cancellationToken);

    void _errorOccurred(StringData errMsg);

    const NamespaceString _nss;
    const ShardId _fromShard;
    const MigrationSessionId _migrationSessionId;
    const CancellationToken _cancellationToken;

    stdx::thread _thread;

    // Protects _state and _errMsg.
    stdx::mutex _mutex;
    State _state = State::NotStarted;
    std::string _errMsg;  // valid only if _state == ErrorOccurred.

    // The number of session oplog entries processed. This is not always equal to the number of
    // session oplog entries comitted because entries may have been processed but not committed
    AtomicWord<long long> _sessionOplogEntriesMigrated{0};
};

}  // namespace mongo
