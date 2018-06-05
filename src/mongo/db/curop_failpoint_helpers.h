/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

class CurOpFailpointHelpers {
public:
    /**
     * Helper function which sets the 'msg' field of the opCtx's CurOp to the specified string, and
     * returns the original value of the field.
     */
    static std::string updateCurOpMsg(OperationContext* opCtx, const std::string& newMsg);

    /**
     * This helper function works much like MONGO_FAIL_POINT_PAUSE_WHILE_SET, but additionally
     * calls whileWaiting() at regular intervals. Finally, it also sets the 'msg' field of the
     * opCtx's CurOp to the given string while the failpoint is active.
     *
     * whileWaiting() may be used to do anything the caller needs done while hanging in the
     * failpoint. For example, the caller may use whileWaiting() to release and reacquire locks in
     * order to avoid deadlocks.
     */
    static void waitWhileFailPointEnabled(FailPoint* failPoint,
                                          OperationContext* opCtx,
                                          const std::string& curOpMsg,
                                          const std::function<void(void)>& whileWaiting = nullptr);
};
}
