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

MONGO_MOD_PRIVATE BSONObj getRedactedCopyForLogging(const Command* command, const BSONObj& cmdObj);

MONGO_MOD_PRIVATE inline void setPrepareConflictBehaviorForReadConcern(
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

MONGO_MOD_PRIVATE void waitForReadConcern(OperationContext* opCtx,
                                          const CommandInvocation* invocation,
                                          const OpMsgRequest& request);

MONGO_MOD_PRIVATE void waitForWriteConcern(OperationContext* opCtx,
                                           const CommandInvocation* invocation,
                                           const repl::OpTime& lastOpBeforeRun,
                                           BSONObjBuilder& commandResponseBuilder);

MONGO_MOD_PRIVATE inline void uassertCommandDoesNotSpecifyWriteConcern(
    const GenericArguments& requestArgs) {
    uassert(ErrorCodes::InvalidOptions,
            "Command does not support writeConcern",
            !commandSpecifiesWriteConcern(requestArgs));
}

MONGO_MOD_PRIVATE void appendReplyMetadata(OperationContext* opCtx,
                                           const GenericArguments& requestArgs,
                                           BSONObjBuilder* metadataBob);

// When handling possible retryable errors, we may have modified the locker state, in particular the
// flags which say if the operation took a write lock or shared lock. This will cause mongod to
// perhaps erroneously check for write concern when no writes were done, or unnecessarily kill a
// read operation. If we re-use the opCtx to retry command execution, we must reset the locker
// state.
MONGO_MOD_PRIVATE inline void resetLockerState(OperationContext* opCtx) {
    // It is necessary to lock the client to change the Locker on the OperationContext.
    ClientLock lk(opCtx->getClient());
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    shard_role_details::swapLocker(opCtx, std::make_unique<Locker>(opCtx->getServiceContext()), lk);
}

MONGO_MOD_PRIVATE void createTransactionCoordinator(
    OperationContext* opCtx,
    TxnNumber clientTxnNumber,
    boost::optional<TxnRetryCounter> clientTxnRetryCounter);

}  // namespace service_entry_point_shard_role_helpers
}  // namespace mongo
