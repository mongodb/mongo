// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/user_write_block/user_writes_block_reason_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <array>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] GlobalUserWriteBlockState {
public:
    GlobalUserWriteBlockState() = default;

    static GlobalUserWriteBlockState* get(ServiceContext* serviceContext);
    static GlobalUserWriteBlockState* get(OperationContext* opCtx);

    /**
     * Methods to control the global user write blocking state.
     */
    void enableUserWriteBlocking(OperationContext* opCtx, UserWritesBlockReasonEnum reason);
    void disableUserWriteBlocking(OperationContext* opCtx);

    /**
     * Gets the reason why the user writes are blocked globally.
     */
    UserWritesBlockReasonEnum getUserWriteBlockingReason(OperationContext* opCtx) const {
        return _globalUserWritesBlockedReason.load();
    }

    /**
     * Checks that user writes are allowed on the specified namespace. Callers must hold the
     * GlobalLock in any mode. Throws UserWritesBlocked if user writes are disallowed.
     */
    void checkUserWritesAllowed(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Returns whether user write blocking is enabled, disregarding a specific namespace and the
     * state of WriteBlockBypass. Used for serverStatus.
     */
    bool isUserWriteBlockingEnabled(OperationContext* opCtx) const;

    /**
     * Reports the user write blocking counters.
     */
    void appendUserWriteBlockModeCounters(BSONObjBuilder& bob) const;

    /**
     * Methods to enable/disable blocking new sharded DDL operations.
     */
    void enableUserShardedDDLBlocking(OperationContext* opCtx);
    void disableUserShardedDDLBlocking(OperationContext* opCtx);

    /**
     * Checks that new sharded DDL operations are allowed to start. Throws UserWritesBlocked if
     * starting new sharded DDL operations is disallowed.
     */
    void checkShardedDDLAllowedToStart(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Methods to enable/disable blocking new user index builds.
     */
    void enableUserIndexBuildBlocking(OperationContext* opCtx);
    void disableUserIndexBuildBlocking(OperationContext* opCtx);

    /**
     * Checks that an index build is allowed to start on the specified namespace. Returns
     * UserWritesBlocked if user index builds are disallowed, OK otherwise.
     */
    Status checkIfIndexBuildAllowedToStart(OperationContext* opCtx,
                                           const NamespaceString& nss) const;


private:
    Atomic<bool> _globalUserWritesBlocked{false};
    Atomic<UserWritesBlockReasonEnum> _globalUserWritesBlockedReason{
        UserWritesBlockReasonEnum::kUnspecified};
    std::array<Atomic<size_t>, idlEnumCount<UserWritesBlockReasonEnum>>
        _globalUserWriteBlockCounters{};
    Atomic<bool> _userShardedDDLBlocked{false};
    Atomic<bool> _userIndexBuildsBlocked{false};
};

}  // namespace mongo
