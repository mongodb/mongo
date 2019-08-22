/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/db/s/transaction_coordinator_worker_curop_info.h"

#include "mongo/base/shim.h"

namespace mongo {
const auto getTransactionCoordinatorWorkerCurOpInfo =
    OperationContext::declareDecoration<boost::optional<TransactionCoordinatorWorkerCurOpInfo>>();

class MongoDTransactionCoordinatorWorkerCurOpRepository final
    : public TransactionCoordinatorWorkerCurOpRepository {
public:
    MongoDTransactionCoordinatorWorkerCurOpRepository() {}

    void set(OperationContext* opCtx,
             const LogicalSessionId& lsid,
             const TxnNumber txnNumber,
             const CoordinatorAction action) override {
        auto startTime = opCtx->getServiceContext()->getPreciseClockSource()->now();
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        getTransactionCoordinatorWorkerCurOpInfo(opCtx).emplace(
            TransactionCoordinatorWorkerCurOpInfo(lsid, txnNumber, startTime, action));
    }

    /**
     * Caller should hold the client lock associated with the OperationContext.
     */
    void reportState(OperationContext* opCtx, BSONObjBuilder* parent) const override {
        if (auto info = getTransactionCoordinatorWorkerCurOpInfo(opCtx)) {
            info->reportState(parent);
        }
    }
};

const auto _transactionCoordinatorWorkerCurOpRepository =
    std::make_shared<MongoDTransactionCoordinatorWorkerCurOpRepository>();

MONGO_REGISTER_SHIM(getTransactionCoordinatorWorkerCurOpRepository)
()->std::shared_ptr<TransactionCoordinatorWorkerCurOpRepository> {
    return _transactionCoordinatorWorkerCurOpRepository;
}

TransactionCoordinatorWorkerCurOpInfo::TransactionCoordinatorWorkerCurOpInfo(
    LogicalSessionId lsid, TxnNumber txnNumber, Date_t startTime, CoordinatorAction action)
    : _lsid(lsid), _txnNumber(txnNumber), _startTime(startTime), _action(action) {}


const std::string TransactionCoordinatorWorkerCurOpInfo::toString(CoordinatorAction action) {
    switch (action) {
        case CoordinatorAction::kSendingPrepare:
            return "sendingPrepare";
        case CoordinatorAction::kSendingCommit:
            return "sendingCommit";
        case CoordinatorAction::kSendingAbort:
            return "sendingAbort";
        case CoordinatorAction::kWritingParticipantList:
            return "writingParticipantList";
        case CoordinatorAction::kWritingDecision:
            return "writingDecision";
        case CoordinatorAction::kDeletingCoordinatorDoc:
            return "deletingCoordinatorDoc";
        default:
            MONGO_UNREACHABLE
    }
}

void TransactionCoordinatorWorkerCurOpInfo::reportState(BSONObjBuilder* parent) const {
    invariant(parent);
    BSONObjBuilder twoPhaseCoordinatorBuilder;
    BSONObjBuilder lsidBuilder(twoPhaseCoordinatorBuilder.subobjStart("lsid"));
    _lsid.serialize(&lsidBuilder);
    lsidBuilder.doneFast();
    twoPhaseCoordinatorBuilder.append("txnNumber", _txnNumber);
    twoPhaseCoordinatorBuilder.append("action", toString(_action));
    twoPhaseCoordinatorBuilder.append("startTime", dateToISOStringUTC(_startTime));
    parent->append("twoPhaseCommitCoordinator", twoPhaseCoordinatorBuilder.obj());
}
}  // namespace mongo
