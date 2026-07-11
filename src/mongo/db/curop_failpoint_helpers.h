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
                                               const std::string& failpointMsg);

    /**
     * This helper function works much like FailPoint::pauseWhileSet(opCtx), but additionally
     * calls whileWaiting() at regular intervals. Finally, it also sets the 'msg' field of the
     * opCtx's CurOp to the given string while the failpoint is active.
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
     */
    static void waitWhileFailPointEnabled(FailPoint* failPoint,
                                          OperationContext* opCtx,
                                          const std::string& failpointMsg,
                                          const std::function<void()>& whileWaiting = nullptr,
                                          const NamespaceString& nss = {});
};
}  // namespace mongo
