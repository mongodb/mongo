// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/database_name.h"
#include "mongo/db/logical_time.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class BSONObj;

class OperationContext;
class Status;
template <typename T>
class StatusWith;

enum class PrepareConflictBehavior;

namespace repl {
class ReadConcernArgs;
class SpeculativeMajorityReadInfo;
}  // namespace repl

/**
 * Sets the prepare conflict behavior for a command.
 *
 * If the prepareConflictBehavior requested is to ignore prepare conflicts, then readConcernArgs
 * are used to verify if the command is safe to ignore prepare conflicts, and if not, we
 * enforce prepare conflicts.
 */
void setPrepareConflictBehaviorForReadConcern(OperationContext* opCtx,
                                              const repl::ReadConcernArgs& readConcernArgs,
                                              PrepareConflictBehavior prepareConflictBehavior);

/**
 * Given the specified read concern arguments, performs checks that the read concern can actually be
 * satisfied given the current state of the server and if so calls into the replication subsystem to
 * perform the wait. If allowAfterClusterTime is false returns an error if afterClusterTime is
 * set on the readConcernArgs.
 *
 * Note: Callers should use setPrepareConflictBehaviorForReadConcern method to set the desired
 * prepare conflict behavior for their command.
 */
Status waitForReadConcern(OperationContext* opCtx,
                          const repl::ReadConcernArgs& readConcernArgs,
                          const DatabaseName& dbName,
                          bool allowAfterClusterTime);

/*
 * Given a linearizable read command, confirm that
 * current primary is still the true primary of the replica set.
 *
 * A readConcernTimeout of 0 indicates that the operation will block indefinitely waiting for read
 * concern.
 */
Status waitForLinearizableReadConcern(OperationContext* opCtx, Milliseconds readConcernTimeout);

/**
 * Waits to satisfy a "speculative" majority read.
 *
 * This method must only be called if the operation is a speculative majority read.
 */
Status waitForSpeculativeMajorityReadConcern(OperationContext* opCtx,
                                             repl::SpeculativeMajorityReadInfo speculativeReadInfo);

/**
 * Best-effort schedules a no-op write via the `appendOplogNote` command on the primary of this
 * replica set, in order to advance this node's last-written cluster time to at least `clusterTime`.
 */
Status makeNoopWriteToAdvanceClusterTime(OperationContext* opCtx, LogicalTime clusterTime);

}  // namespace mongo
