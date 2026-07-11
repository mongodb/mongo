// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] TransactionCoordinatorWorkerCurOpRepository {
public:
    TransactionCoordinatorWorkerCurOpRepository() {}
    virtual ~TransactionCoordinatorWorkerCurOpRepository() {}

    enum class [[MONGO_MOD_PRIVATE]] CoordinatorAction {
        kWritingParticipantList,
        kSendingPrepare,
        kWritingDecision,
        kSendingCommit,
        kSendingAbort,
        kWritingEndOfTransaction,
        kDeletingCoordinatorDoc
    };

    /**
     * Associates the two phase commit coordinator transaction information with the
     * OperationContext instance.
     *
     * This method takes the associated client lock from the OperationContext.
     */
    virtual void set(OperationContext* opCtx,
                     const LogicalSessionId& lsid,
                     TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                     CoordinatorAction action) = 0;

    /**
     * Output the state into BSON previously associated with this OperationContext instance.
     */
    virtual void reportState(OperationContext* opCtx, BSONObjBuilder* parent) const = 0;
};

[[MONGO_MOD_PUBLIC]] std::shared_ptr<TransactionCoordinatorWorkerCurOpRepository>
getTransactionCoordinatorWorkerCurOpRepository();

}  // namespace mongo
