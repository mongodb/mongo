// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

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
                                       const UUID& migrationId,
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
    void forceFail(std::string_view errMsg);

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

    void _errorOccurred(std::string_view errMsg);

    const NamespaceString _nss;
    const ShardId _fromShard;
    const MigrationSessionId _migrationSessionId;
    const UUID _migrationId;
    const CancellationToken _cancellationToken;

    stdx::thread _thread;

    // Protects _state and _errMsg.
    std::mutex _mutex;
    State _state = State::NotStarted;
    std::string _errMsg;  // valid only if _state == ErrorOccurred.

    // The number of session oplog entries processed. This is not always equal to the number of
    // session oplog entries comitted because entries may have been processed but not committed
    Atomic<long long> _sessionOplogEntriesMigrated{0};
};

}  // namespace mongo
