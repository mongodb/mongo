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

#include <boost/optional.hpp>

#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/task_executor.h"
#include "tenant_migration_access_blocker.h"

namespace mongo {


/**
 * The TenantMigrationRecipientAccessBlocker is used to reject tenant reads at a point-in-time
 * before a recipient node reaches the consistent state after a tenant migration.
 *
 * When data cloning is finished (and therefore a consistent donor optime established) an opObserver
 * that is observing the recipient state document will create a
 * TenantMigrationRecipientAccessBlocker in state `kReject` that will reject all reads (with
 * SnapshotTooOld) for that tenant.
 *
 * When oplog application reaches this consistent point, the recipient primary will wait for
 * the earlier state document write to be committed on all recipient nodes before doing the state
 * machine write for the consistent state. The TenantMigrationRecipientAccessBlocker, upon see the
 * write for the consistent state, will transition to `kRejectBefore` state with the
 * `rejectBeforeTimestamp` set to the recipient consistent timestamp and will start allowing reads
 * for read concerns which read the latest snapshot, and "atClusterTime" or "majority" read concerns
 * which are after the `rejectBeforeTimestamp`. Reads for older snapshots, except "majority" until
 * the majority snapshot for a node reaches the `rejectBeforeTimestamp`, will be rejected with
 * SnapshotTooOld. Reads for "majority" when the majority snapshot is before the
 * `rejectBeforeTimestamp` will be blocked until the majority committed snapshot reaches that
 * timestamp.
 *
 * To ensure atClusterTime and afterClusterTime reads are consistent, when the recipient receives a
 * recipientSyncData command with a returnAfterReachingTimestamp after the consistent point, the
 * `rejectBeforeTimestamp` will be advanced to the given returnAfterReachingTimestamp.
 *
 * Blocker excludes all operations with 'tenantMigrationInfo' decoration set, as they are
 * internal.
 */
class TenantMigrationRecipientAccessBlocker
    : public std::enable_shared_from_this<TenantMigrationRecipientAccessBlocker>,
      public TenantMigrationAccessBlocker {
public:
    TenantMigrationRecipientAccessBlocker(ServiceContext* serviceContext, const UUID& migrationId);

    //
    // Called by all writes and reads against the database.
    //

    Status checkIfCanWrite(Timestamp writeTs) final;
    Status waitUntilCommittedOrAborted(OperationContext* opCtx) final;

    Status checkIfLinearizableReadWasAllowed(OperationContext* opCtx) final;
    SharedSemiFuture<void> getCanReadFuture(OperationContext* opCtx, StringData command) final;

    //
    // Called by index build user threads before acquiring an index build slot, and again right
    // after registering the build.
    //
    Status checkIfCanBuildIndex() final;

    // @return true if TTL is blocked
    bool checkIfShouldBlockTTL() const final;

    // Clear TTL blocker once the state doc is garbage collectable.
    void stopBlockingTTL();

    /**
     * Called when an optime is majority committed.
     */
    void onMajorityCommitPointUpdate(repl::OpTime opTime) final;

    void appendInfoForServerStatus(BSONObjBuilder* builder) const final;

    void recordTenantMigrationError(Status status) final{};

    //
    // Called as a recipient to reject reads before the `timestamp`.
    //
    void startRejectingReadsBefore(const Timestamp& timestamp);

    bool inStateReject() const {
        return _state.isReject();
    }

private:
    /**
     * The access states of an mtab.
     */
    class BlockerState {
    public:
        void transitionToRejectBefore() {
            _state = State::kRejectBefore;
        }

        bool isReject() const {
            return _state == State::kReject;
        }

        bool isRejectBefore() const {
            return _state == State::kRejectBefore;
        }

        std::string toString() const;

    private:
        enum class State { kReject, kRejectBefore };

        State _state = State::kReject;
    };

    ServiceContext* _serviceContext;

    // Protects the state below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationRecipientAccessBlocker::_mutex");

    BlockerState _state;

    boost::optional<Timestamp> _rejectBeforeTimestamp;

    // Start with blocked TTL, unblock when the migration document is marked as
    // garbage collectable.
    bool _ttlIsBlocked = true;
};

}  // namespace mongo
