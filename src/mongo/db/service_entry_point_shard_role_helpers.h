// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/write_concern.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace service_entry_point_shard_role_helpers {

[[MONGO_MOD_PRIVATE]] BSONObj getRedactedCopyForLogging(const Command* command,
                                                        const BSONObj& cmdObj);

[[MONGO_MOD_PRIVATE]] inline void setPrepareConflictBehaviorForReadConcern(
    OperationContext* opCtx, const CommandInvocation* invocation) {
    // Some read commands can safely ignore prepare conflicts by default because they do not
    // require snapshot isolation and do not conflict with concurrent writes. We also give these
    // operations permission to write, as this may be required for queries that spill using the
    // storage engine. The kIgnoreConflictsAllowWrites setting suppresses an assertion in the
    // storage engine that prevents operations that ignore prepare conflicts from also writing.
    const auto prepareConflictBehavior = invocation->canIgnorePrepareConflicts()
        ? PrepareConflictBehavior::kIgnoreConflictsAllowWrites
        : PrepareConflictBehavior::kEnforce;
    mongo::setPrepareConflictBehaviorForReadConcern(
        opCtx, repl::ReadConcernArgs::get(opCtx), prepareConflictBehavior);
}

[[MONGO_MOD_PRIVATE]] void waitForReadConcern(OperationContext* opCtx,
                                              const CommandInvocation* invocation,
                                              const OpMsgRequest& request);

[[MONGO_MOD_PRIVATE]] void waitForWriteConcern(OperationContext* opCtx,
                                               const CommandInvocation* invocation,
                                               const repl::OpTime& lastOpBeforeRun,
                                               BSONObjBuilder& commandResponseBuilder);

[[MONGO_MOD_PRIVATE]] inline void uassertCommandDoesNotSpecifyWriteConcern(
    const GenericArguments& requestArgs) {
    uassert(ErrorCodes::InvalidOptions,
            "Command does not support writeConcern",
            !commandSpecifiesWriteConcern(requestArgs));
}

[[MONGO_MOD_PRIVATE]] void appendReplyMetadata(OperationContext* opCtx,
                                               const GenericArguments& requestArgs,
                                               BSONObjBuilder* metadataBob);

// When handling possible retryable errors, we may have modified the locker state, in particular the
// flags which say if the operation took a write lock or shared lock. This will cause mongod to
// perhaps erroneously check for write concern when no writes were done, or unnecessarily kill a
// read operation. If we re-use the opCtx to retry command execution, we must reset the locker
// state.
[[MONGO_MOD_PRIVATE]] inline void resetLockerState(OperationContext* opCtx) {
    // It is necessary to lock the client to change the Locker on the OperationContext.
    ClientLock lk(opCtx->getClient());
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    shard_role_details::swapLocker(opCtx, std::make_unique<Locker>(opCtx->getServiceContext()), lk);
}

[[MONGO_MOD_PRIVATE]] void createTransactionCoordinator(
    OperationContext* opCtx,
    TxnNumber clientTxnNumber,
    boost::optional<TxnRetryCounter> clientTxnRetryCounter);

}  // namespace service_entry_point_shard_role_helpers
}  // namespace mongo
