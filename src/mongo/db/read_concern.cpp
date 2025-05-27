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

#include "mongo/db/read_concern.h"

#include "mongo/base/shim.h"
#include "mongo/db/repl/speculative_majority_read_info.h"

#include <string>

namespace mongo {

void setPrepareConflictBehaviorForReadConcern(OperationContext* opCtx,
                                              const repl::ReadConcernArgs& readConcernArgs,
                                              PrepareConflictBehavior prepareConflictBehavior) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(setPrepareConflictBehaviorForReadConcern);
    return w(opCtx, readConcernArgs, prepareConflictBehavior);
}

Status waitForReadConcern(OperationContext* opCtx,
                          const repl::ReadConcernArgs& readConcernArgs,
                          const DatabaseName& dbName,
                          bool allowAfterClusterTime) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(waitForReadConcern);
    return w(opCtx, readConcernArgs, dbName, allowAfterClusterTime);
}

Status waitForLinearizableReadConcern(OperationContext* opCtx, Milliseconds readConcernTimeout) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(waitForLinearizableReadConcern);
    return w(opCtx, readConcernTimeout);
}

Status waitForSpeculativeMajorityReadConcern(
    OperationContext* opCtx, repl::SpeculativeMajorityReadInfo speculativeReadInfo) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(waitForSpeculativeMajorityReadConcern);
    return w(opCtx, speculativeReadInfo);
}

}  // namespace mongo
