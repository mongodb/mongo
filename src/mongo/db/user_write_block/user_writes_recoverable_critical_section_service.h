/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/user_write_block/user_writes_block_reason_gen.h"

#include <string>

namespace mongo {

/**
 * Represents the 'user writes blocking' critical section. The critical section status is persisted
 * on disk and it's in-memory representation is kept in sync with the persisted state through an
 * OpObserver that reacts to the inserts/updates/deletes on the persisted document.
 *
 * On replicaSets, enable blocking is depicted by transition (1); and disable blocking is depicted
 * by transition (2):
 *
 *             (1) acquireRecoverableCriticalSectionBlockingUserWrites()
 *             ---------------------------------------------------------
 *            |                                                         |
 *            |                                                         v
 * + --------------------+                                   + --------------------+
 * | User writes allowed |                                   | User writes blocked |
 * + --------------------+                                   + --------------------+
 *            ^                                                         ^
 *            |                                                         |
 *             ---------------------------------------------------------
 *                      (2) releaseRecoverableCriticalSection()
 *
 * On sharded clusters, blocking/unblocking happens as a two-phase protocol. Enable blocking is
 * depicted by transitions (1) and (2); and disable blocking is depicted by (3) and (4):
 *
 *                                          (2) promoteRecoverableCriticalSectionToBlockUserWrites()
 *                                                             ------------------------
 *                                                            |                        |
 *  (1) acquireRecoverableCriticalSectionBlockNewShardedDDL() |                        |
 *             ---------------------------------              |                        |
 *            |                                 |             |                        |
 *            |                                 V             |                        v
 * + -------------------------+      + -------------------------+       + -------------------------+
 * | User writes allowed,     |      | User writes allowed,     |       | User writes blocked      |
 * | User sharded DDL allowed |      | User sharded DDL blocked |       | User sharded DDL blocked |
 * + -------------------------+      + -------------------------+       + -------------------------+
 *             ^                                         ^                             |
 *             |                                |        |                             |
 *              --------------------------------         |                             |
 *          (4) releaseRecoverableCriticalSection()      |                             |
 *                                                       |                             |
 *                                                        -----------------------------
 *                                   (3) demoteRecoverableCriticalSectionToNoLongerBlockUserWrites()
 *
 */
class UserWritesRecoverableCriticalSectionService
    : public ReplicaSetAwareService<UserWritesRecoverableCriticalSectionService> {
public:
    static const NamespaceString kGlobalUserWritesNamespace;

    UserWritesRecoverableCriticalSectionService() = default;

    static UserWritesRecoverableCriticalSectionService* get(ServiceContext* serviceContext);
    static UserWritesRecoverableCriticalSectionService* get(OperationContext* opCtx);

    bool shouldRegisterReplicaSetAwareService() const override;

    /**
     * Acquires the user writes critical section blocking user writes. This should be used only on
     * replica sets.
     */
    void acquireRecoverableCriticalSectionBlockingUserWrites(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<UserWritesBlockReasonEnum> reason = boost::none);

    /**
     * Acquires the user writes critical section blocking only new sharded DDL operations, but not
     * user writes nor local DDL. This is a 'prepare' state before user writes and local DDL can be
     * blocked on sharded clusters.
     * Note: This method does not wait for ongoing DDL coordinators to finish. If the caller wishes
     * to ensure that any ongoing DDL operation has seen the new blocking state, the caller should
     * wait for the ShardingDDLCoordinators to drain itself.
     */
    void acquireRecoverableCriticalSectionBlockNewShardedDDL(OperationContext* opCtx,
                                                             const NamespaceString& nss);

    /**
     * Promotes a user writes critical section that is in the 'prepare' state (i.e. only blocking
     * sharded DDL) to start blocking also user writes. This should be run only after all shards in
     * the cluster have entered the 'prepare' state.
     */
    void promoteRecoverableCriticalSectionToBlockUserWrites(OperationContext* opCtx,
                                                            const NamespaceString& nss);

    /**
     * Demotes a user writes critical section that is blocking both sharded DDL and user writes to
     * only block sharded DDL. This is a preparation step before allowing user writes again on
     * sharded clusters.
     */
    void demoteRecoverableCriticalSectionToNoLongerBlockUserWrites(OperationContext* opCtx,
                                                                   const NamespaceString& nss);

    /**
     * Releases the user writes critical section, allowing user writes again. On sharded clusters,
     * before this method is called all shards must have first demoted their critical sections to no
     * longer block user writes.
     */
    void releaseRecoverableCriticalSection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<UserWritesBlockReasonEnum> reason = boost::none);

    /**
     * This method is called when we have to mirror the state on disk of the recoverable critical
     * section to memory (on startUp or on rollback).
     */
    void recoverRecoverableCriticalSections(OperationContext* opCtx);

private:
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) final {
        // TODO (SERVER-91505): Determine if we should reload in-memory states on rollback.
        if (isRollback) {
            return;
        }
        recoverRecoverableCriticalSections(opCtx);
    }

    void onStartup(OperationContext* opCtx) final {}
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onShutdown() final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final {}
    void onStepUpComplete(OperationContext* opCtx, long long term) final {}
    void onStepDown() final {}
    void onRollbackBegin() final {}
    void onBecomeArbiter() final {}
    inline std::string getServiceName() const final {
        return "UserWritesRecoverableCriticalSectionService";
    }
};

}  // namespace mongo
