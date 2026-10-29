// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

Status makeNoopWriteToAdvanceClusterTime(OperationContext* opCtx, LogicalTime clusterTime) {
    static auto w = MONGO_WEAK_FUNCTION_DEFINITION(makeNoopWriteToAdvanceClusterTime);
    return w(opCtx, clusterTime);
}

}  // namespace mongo
