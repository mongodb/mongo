// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo {

class OperationContext;

/**
 * Notfication callback, which stores the last notification result and signals a condition
 * variable, which can be waited on.
 */
class [[MONGO_MOD_PRIVATE]] CondVarLockGrantNotification final : public LockGrantNotification {
public:
    CondVarLockGrantNotification() = default;

    /**
     * Uninterruptible blocking method, which waits for the notification to fire.
     *
     * @param timeout How many milliseconds to wait before returning LOCK_TIMEOUT.
     */
    LockResult wait(Milliseconds timeout);

    /**
     * Interruptible blocking method, which waits for the notification to fire or an interrupt from
     * the operation context.
     *
     * @param opCtx OperationContext to wait on for an interrupt.
     * @param timeout How many milliseconds to wait before returning LOCK_TIMEOUT.
     */
    LockResult wait(OperationContext* opCtx, Milliseconds timeout);

    /**
     * Clears the object so it can be reused.
     */
    void clear() {
        _result = LOCK_INVALID;
    }

    void notify(ResourceId resId, LockResult result) override;

private:
    // These two go together to implement the conditional variable pattern.
    std::mutex _mutex;
    stdx::condition_variable _cond;

    // Result from the last call to notify
    LockResult _result{LOCK_INVALID};
};

}  // namespace mongo
