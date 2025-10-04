/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/util/modules.h"

namespace MONGO_MOD_PUB mongo {
namespace repl {
/**
 * An interface for the Initial Syncer that declares functions that are used by
 * ReplicationCoordinatorImpl for initial sync.
 */
class InitialSyncerInterface {
public:
    /**
     * Type of function to create a database client
     *
     * Used for testing only.
     */
    using CreateClientFn = std::function<std::unique_ptr<DBClientConnection>()>;
    struct Options {
        /** Function to return optime of last operation applied on this node */
        using GetMyLastOptimeFn = std::function<OpTime()>;

        /** Function to update optime of last operation applied on this node */
        using SetMyLastOptimeFn = std::function<void(const OpTimeAndWallTime&)>;

        /** Function to reset all optimes on this node (e.g. applied & durable). */
        using ResetOptimesFn = std::function<void()>;

        /** Function to set this node into a specific follower mode. */
        using SetFollowerModeFn = std::function<bool(const MemberState&)>;

        // Retry values
        Milliseconds syncSourceRetryWait{1000};
        Milliseconds initialSyncRetryWait{1000};

        // InitialSyncer waits this long before retrying getApplierBatchCallback() if there are
        // currently no operations available to apply or if the 'rsSyncApplyStop' failpoint is
        // active. This default value is based on the duration in OplogApplierBatcher::run().
        Milliseconds getApplierBatchCallbackRetryWait{1000};

        GetMyLastOptimeFn getMyLastOptime;
        SetMyLastOptimeFn setMyLastOptime;
        ResetOptimesFn resetOptimes;

        SyncSourceSelector* syncSourceSelector = nullptr;

        // The oplog fetcher will restart the oplog tailing query this many times on
        // non-cancellation failures.
        std::uint32_t oplogFetcherMaxFetcherRestarts = 0;
    };
    /**
     * Callback function to report last applied optime of initial sync.
     */
    typedef std::function<void(const StatusWith<OpTimeAndWallTime>& lastApplied)> OnCompletionFn;

    virtual ~InitialSyncerInterface() = default;

    /**
     * Starts initial sync process, with the provided number of attempts
     */
    virtual Status startup(OperationContext* opCtx, std::uint32_t maxAttempts) noexcept = 0;

    /**
     * Shuts down replication if "start" has been called, and blocks until shutdown has completed.
     * Must be called before the executor that the initial syncer uses is shutdown.
     */
    virtual Status shutdown() = 0;

    /**
     * Block until inactive.
     */
    virtual void join() = 0;

    /**
     * Returns stats about the progress of initial sync. If initial sync is not in progress it
     * returns an empty BSON object.
     */
    virtual BSONObj getInitialSyncProgress() const = 0;

    /**
     * Cancels the current initial sync attempt if the initial syncer is active.
     */
    virtual void cancelCurrentAttempt() = 0;

    /**
     * Returns the initial sync method that this initial syncer is using.
     */
    virtual std::string getInitialSyncMethod() const = 0;

    /**
     * Returns whether user access to db 'local' should be allowed during this initial sync.
     * Should be constant, and must not take any locks.
     */
    virtual bool allowLocalDbAccess() const = 0;
};

}  // namespace repl
}  // namespace MONGO_MOD_PUB mongo
