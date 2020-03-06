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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/fail_point.h"

namespace mongo {

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
     * If checkForInterrupt is false, the field "shouldCheckForInterrupt" may be set to 'true' at
     * runtime to cause this method to uassert on interrupt.
     *
     * The field "shouldContinueOnInterrupt" may be set to 'true' to cause this method to continue
     * on interrupt without asserting, regardless of whether checkForInterrupt or the field
     * "shouldCheckForInterrupt" is set.
     */
    static void waitWhileFailPointEnabled(FailPoint* failPoint,
                                          OperationContext* opCtx,
                                          const std::string& failpointMsg,
                                          const std::function<void()>& whileWaiting = nullptr,
                                          bool checkForInterrupt = false,
                                          boost::optional<NamespaceString> nss = boost::none);
};
}  // namespace mongo
