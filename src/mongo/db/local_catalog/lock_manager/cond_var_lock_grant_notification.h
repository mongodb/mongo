/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;

/**
 * Notfication callback, which stores the last notification result and signals a condition
 * variable, which can be waited on.
 */
class CondVarLockGrantNotification final : public LockGrantNotification {
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
    stdx::mutex _mutex;
    stdx::condition_variable _cond;

    // Result from the last call to notify
    LockResult _result{LOCK_INVALID};
};

}  // namespace mongo
