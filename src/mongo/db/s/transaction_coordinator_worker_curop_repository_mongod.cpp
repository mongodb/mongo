// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/shim.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/transaction_coordinator_worker_curop_repository.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class TransactionCoordinatorWorkerCurOpInfo {
public:
    using CoordinatorAction = TransactionCoordinatorWorkerCurOpRepository::CoordinatorAction;

    TransactionCoordinatorWorkerCurOpInfo(LogicalSessionId lsid,
                                          TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                          Date_t startTime,
                                          CoordinatorAction action);

    TransactionCoordinatorWorkerCurOpInfo() = delete;

    void reportState(BSONObjBuilder* parent) const;

private:
    static std::string toString(CoordinatorAction action);

    LogicalSessionId _lsid;
    TxnNumberAndRetryCounter _txnNumberAndRetryCounter;
    Date_t _startTime;
    CoordinatorAction _action;
};

const auto getTransactionCoordinatorWorkerCurOpInfo =
    OperationContext::declareDecoration<boost::optional<TransactionCoordinatorWorkerCurOpInfo>>();

class MongoDTransactionCoordinatorWorkerCurOpRepository final
    : public TransactionCoordinatorWorkerCurOpRepository {
public:
    MongoDTransactionCoordinatorWorkerCurOpRepository() {}

    void set(OperationContext* opCtx,
             const LogicalSessionId& lsid,
             const TxnNumberAndRetryCounter txnNumberAndRetryCounter,
             const CoordinatorAction action) override {
        auto startTime = opCtx->getServiceContext()->getPreciseClockSource()->now();
        std::lock_guard<Client> lk(*opCtx->getClient());
        getTransactionCoordinatorWorkerCurOpInfo(opCtx).emplace(
            TransactionCoordinatorWorkerCurOpInfo(
                lsid, txnNumberAndRetryCounter, startTime, action));
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

std::shared_ptr<TransactionCoordinatorWorkerCurOpRepository>
getTransactionCoordinatorWorkerCurOpRepositoryImpl() {
    return _transactionCoordinatorWorkerCurOpRepository;
}

auto getTransactionCoordinatorWorkerCurOpRepositoryRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(getTransactionCoordinatorWorkerCurOpRepository,
                                     getTransactionCoordinatorWorkerCurOpRepositoryImpl);

TransactionCoordinatorWorkerCurOpInfo::TransactionCoordinatorWorkerCurOpInfo(
    LogicalSessionId lsid,
    TxnNumberAndRetryCounter txnNumberAndRetryCounter,
    Date_t startTime,
    CoordinatorAction action)
    : _lsid(lsid),
      _txnNumberAndRetryCounter(txnNumberAndRetryCounter),
      _startTime(startTime),
      _action(action) {}


std::string TransactionCoordinatorWorkerCurOpInfo::toString(CoordinatorAction action) {
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
        case CoordinatorAction::kWritingEndOfTransaction:
            return "writingEndOfTransaction";
        case CoordinatorAction::kDeletingCoordinatorDoc:
            return "deletingCoordinatorDoc";
    }
    MONGO_UNREACHABLE;
}

void TransactionCoordinatorWorkerCurOpInfo::reportState(BSONObjBuilder* parent) const {
    invariant(parent);
    BSONObjBuilder twoPhaseCoordinatorBuilder;
    BSONObjBuilder lsidBuilder(twoPhaseCoordinatorBuilder.subobjStart("lsid"));
    _lsid.serialize(&lsidBuilder);
    lsidBuilder.doneFast();
    twoPhaseCoordinatorBuilder.append("txnNumber", _txnNumberAndRetryCounter.getTxnNumber());
    twoPhaseCoordinatorBuilder.append("txnRetryCounter",
                                      *_txnNumberAndRetryCounter.getTxnRetryCounter());
    twoPhaseCoordinatorBuilder.append("action", toString(_action));
    twoPhaseCoordinatorBuilder.append("startTime", dateToISOStringUTC(_startTime));
    parent->append("twoPhaseCommitCoordinator", twoPhaseCoordinatorBuilder.obj());
}

}  // namespace
}  // namespace mongo
