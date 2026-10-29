// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class CurOpFailpointHelpers {
public:
    /**
     * Helper function which sets the 'msg' field of the opCtx's CurOp to the specified string, and
     * returns the original value of the field.
     */
    static std::string updateCurOpFailPointMsg(OperationContext* opCtx,
                                               std::string_view failpointMsg);

    /**
     * This helper function works much like FailPoint::pauseWhileSet(opCtx), but additionally
     * calls whileWaiting() at regular intervals. Finally, it also sets the 'msg' field of the
     * opCtx's CurOp to the given string while the failpoint is active. Counts as one entry into the
     * FailPoint.
     *
     * whileWaiting() may be used to do anything the caller needs done while hanging in the
     * failpoint. For example, the caller may use whileWaiting() to release and reacquire locks in
     * order to avoid deadlocks.
     *
     * The field "shouldCheckForInterrupt" may be set to 'true' at runtime to cause this method to
     * uassert on interrupt.
     *
     * The field "shouldContinueOnInterrupt" may be set to 'true' to cause this method to continue
     * on interrupt without asserting, regardless of whether the field "shouldCheckForInterrupt" is
     * set.
     *
     * The failpoint's data may scope the failpoint to a subset of the operations reaching the
     * callsite: "extraPred" must return true if present, a "comment" field matches only operations
     * carrying an equal comment, and an "nss" field matches only operations on that namespace.
     * "comment" takes precedence over "nss". A callsite which may be targeted by "nss" must pass
     * 'nss'; supplying "nss" in the data without it uasserts, rather than silently matching every
     * operation.
     */
    static void waitWhileFailPointEnabled(
        FailPoint* failPoint,
        OperationContext* opCtx,
        std::string_view failpointMsg,
        const std::function<void()>& whileWaiting = {},
        const NamespaceString& nss = {},
        const std::function<bool(const BSONObj&)>& extraPred = {}) {
        invariant(failPoint);
        // `scopedIf` returns a single LockHandle and counts as exactly one activation. Because that
        // handle is held for the whole impl wait loop, a concurrent setMode(off) -- e.g. a
        // `configureFailPoint` command disabling this failpoint -- blocks until the loop observes
        // the disable and returns. Callers rely on that handshake: when their disable command
        // returns, the paused thread has provably resumed past the failpoint.
        if (auto fpHandle = failPoint->scopedIf(
                [&](const BSONObj& data) { return _shouldExecute(data, opCtx, nss, extraPred); });
            MONGO_unlikely(fpHandle.isActive())) {
            _waitWhileFailPointEnabledImpl(fpHandle, opCtx, failpointMsg, whileWaiting);
        }
    }

private:
    /**
     * Returns true if the failpoint should execute given the provided data and opCtx. A
     * non-callable`extraPred` has no effect, but a callable one must return true for this function
     * to return true. If `opCtx` has a comment, it must match the "comment" field of `data`, and
     * otherwise if `nss` is provided it must match `data`'s "nss" field.
     */
    static bool _shouldExecute(const BSONObj& data,
                               OperationContext* opCtx,
                               const NamespaceString& nss,
                               const std::function<bool(const BSONObj&)>& extraPred);

    /**
     * Continuously executes "whileWaiting" until the failpoint is disabled or the loop is
     * interrupted (if interruptible). Requires that `fpHandle.isActive()` is true.
     */
    static void _waitWhileFailPointEnabledImpl(FailPoint::LockHandle& fpHandle,
                                               OperationContext* opCtx,
                                               std::string_view failpointMsg,
                                               const std::function<void()>& whileWaiting);
};
}  // namespace mongo
