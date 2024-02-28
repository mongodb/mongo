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


#include <absl/container/flat_hash_set.h>
#include <algorithm>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <iterator>
#include <mutex>
#include <tuple>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/server_transaction_coordinators_metrics.h"
#include "mongo/db/s/single_transaction_coordinator_stats.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/s/transaction_coordinator_metrics_observer.h"
#include "mongo/db/s/transaction_coordinator_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/tick_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {
namespace {

using CommitDecision = txn::CommitDecision;
using CoordinatorCommitDecision = txn::CoordinatorCommitDecision;
using PrepareVoteConsensus = txn::PrepareVoteConsensus;
using TransactionCoordinatorDocument = txn::TransactionCoordinatorDocument;

MONGO_FAIL_POINT_DEFINE(hangBeforeWaitingForParticipantListWriteConcern);
MONGO_FAIL_POINT_DEFINE(hangBeforeWaitingForDecisionWriteConcern);

ExecutorFuture<void> waitForMajorityWithHangFailpoint(
    ServiceContext* service,
    FailPoint& failpoint,
    const std::string& failPointName,
    repl::OpTime opTime,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
    const CancellationToken& cancelToken) {
    auto executor = Grid::get(service)->getExecutorPool()->getFixedExecutor();
    auto waitForWC = [service, executor, cancelToken](repl::OpTime opTime) {
        return WaitForMajorityService::get(service)
            .waitUntilMajorityForWrite(service, opTime, cancelToken)
            .thenRunOn(executor);
    };

    if (auto sfp = failpoint.scoped(); MONGO_unlikely(sfp.isActive())) {
        const BSONObj& data = sfp.getData();
        LOGV2(22445,
              "Hit {failPointName} failpoint",
              "failPointName"_attr = failPointName,
              "lsid"_attr = lsid,
              "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

        // Run the hang failpoint asynchronously on a different thread to avoid self deadlocks.
        return ExecutorFuture<void>(executor).then(
            [service, &failpoint, failPointName, data, waitForWC, opTime] {
                if (!data["useUninterruptibleSleep"].eoo()) {
                    failpoint.pauseWhileSet();
                } else {
                    ThreadClient tc(failPointName, service->getService(ClusterRole::ShardServer));

                    // TODO(SERVER-74658): Please revisit if this thread could be made killable.
                    {
                        stdx::lock_guard<Client> lk(*tc.get());
                        tc.get()->setSystemOperationUnkillableByStepdown(lk);
                    }

                    auto opCtx = tc->makeOperationContext();
                    failpoint.pauseWhileSet(opCtx.get());
                }

                return waitForWC(opTime);
            });
    }
    return waitForWC(std::move(opTime));
}

void setDecisionPromise(const txn::CoordinatorCommitDecision& decision,
                        SharedPromise<txn::CommitDecision>& promise) {
    switch (decision.getDecision()) {
        case CommitDecision::kCommit: {
            promise.emplaceValue(CommitDecision::kCommit);
            return;
        }
        case CommitDecision::kAbort: {
            promise.setError(*decision.getAbortStatus());
            return;
        }
    };
    MONGO_UNREACHABLE;
}

}  // namespace

TransactionCoordinator::TransactionCoordinator(
    OperationContext* operationContext,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
    std::unique_ptr<txn::AsyncWorkScheduler> scheduler,
    Date_t deadline,
    const CancellationToken& cancelToken)
    : _serviceContext(operationContext->getServiceContext()),
      _lsid(lsid),
      _txnNumberAndRetryCounter(txnNumberAndRetryCounter),
      _scheduler(std::move(scheduler)),
      _sendPrepareScheduler(_scheduler->makeChildScheduler()),
      _transactionCoordinatorMetricsObserver(
          std::make_unique<TransactionCoordinatorMetricsObserver>()),
      _deadline(deadline),
      _cancelToken(cancelToken) {
    invariant(_txnNumberAndRetryCounter.getTxnRetryCounter());

    auto apiParams = APIParameters::get(operationContext);
    auto kickOffCommitPF = makePromiseFuture<void>();
    _kickOffCommitPromise = std::move(kickOffCommitPF.promise);

    // Task, which will fire when the transaction's total deadline has been reached. If the 2PC
    // sequence has not yet started, it will be abandoned altogether.
    auto deadlineFuture =
        _scheduler
            ->scheduleWorkAt(deadline,
                             [this](OperationContext*) {
                                 LOGV2_DEBUG(5047000,
                                             1,
                                             "TransactionCoordinator deadline reached",
                                             "sessionId"_attr = _lsid,
                                             "txnNumberAndRetryCounter"_attr =
                                                 _txnNumberAndRetryCounter);
                                 cancelIfCommitNotYetStarted();

                                 // See the comments for sendPrepare about the purpose of this
                                 // cancellation code
                                 _sendPrepareScheduler->shutdown(
                                     {ErrorCodes::TransactionCoordinatorReachedAbortDecision,
                                      "Transaction exceeded deadline"});
                             })
            .tapError([this](Status s) {
                if (_reserveKickOffCommitPromise()) {
                    _kickOffCommitPromise.setError(std::move(s));
                }
            });

    // TODO: The duration will be meaningless after failover.
    _updateAssociatedClient(operationContext->getClient());
    _transactionCoordinatorMetricsObserver->onCreate(
        ServerTransactionCoordinatorsMetrics::get(_serviceContext),
        _serviceContext->getTickSource(),
        _serviceContext->getPreciseClockSource()->now());

    // Two-phase commit phases chain. Once this chain executes, the 2PC sequence has completed
    // either with success or error and the scheduled deadline task above has been joined.
    std::move(kickOffCommitPF.future)
        .then([this] {
            return VectorClockMutable::get(_serviceContext)->waitForDurableTopologyTime();
        })
        .thenRunOn(_scheduler->getExecutor())
        .then([this] {
            // Persist the participants, unless they have been made durable already (which would
            // only be the case if this coordinator was created as part of step-up recovery).
            //  Input: _participants
            //         _participantsDurable (optional)
            //  Output: _participantsDurable = true
            {
                stdx::lock_guard<Latch> lg(_mutex);
                invariant(_participants);

                _step = Step::kWritingParticipantList;

                _transactionCoordinatorMetricsObserver->onStartStep(
                    TransactionCoordinator::Step::kWritingParticipantList,
                    TransactionCoordinator::Step::kInactive,
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());
                if (_participantsDurable)
                    return Future<repl::OpTime>::makeReady(repl::OpTime());
            }
            return txn::persistParticipantsList(
                *_sendPrepareScheduler, _lsid, _txnNumberAndRetryCounter, *_participants);
        })
        .then([this](repl::OpTime opTime) {
            return waitForMajorityWithHangFailpoint(
                _serviceContext,
                hangBeforeWaitingForParticipantListWriteConcern,
                "hangBeforeWaitingForParticipantListWriteConcern",
                std::move(opTime),
                _lsid,
                _txnNumberAndRetryCounter,
                _cancelToken);
        })
        .thenRunOn(_scheduler->getExecutor())
        .then([this, apiParams] {
            {
                stdx::lock_guard<Latch> lg(_mutex);
                _participantsDurable = true;
            }

            // Send prepare to the participants, unless this has already been done (which would only
            // be the case if this coordinator was created as part of step-up recovery and the
            // recovery document contained a decision).
            //  Input: _participants, _participantsDurable
            //         _decision (optional)
            //  Output: _decision is set
            {
                stdx::lock_guard<Latch> lg(_mutex);
                invariant(_participantsDurable);

                auto previousStep = _step;
                _step = Step::kWaitingForVotes;

                _transactionCoordinatorMetricsObserver->onStartStep(
                    TransactionCoordinator::Step::kWaitingForVotes,
                    previousStep,
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());

                if (_decision)
                    return Future<void>::makeReady();
            }

            return txn::sendPrepare(_serviceContext,
                                    *_sendPrepareScheduler,
                                    _lsid,
                                    _txnNumberAndRetryCounter,
                                    apiParams,
                                    *_participants)
                .then([this](PrepareVoteConsensus consensus) mutable {
                    {
                        stdx::lock_guard<Latch> lg(_mutex);
                        _decision = consensus.decision();
                    }

                    if (_decision->getDecision() == CommitDecision::kCommit) {
                        auto affectedNamespacesSet = consensus.releaseAffectedNamespaces();
                        _affectedNamespaces.reserve(affectedNamespacesSet.size());
                        std::move(affectedNamespacesSet.begin(),
                                  affectedNamespacesSet.end(),
                                  std::back_inserter(_affectedNamespaces));
                        LOGV2_DEBUG(22446,
                                    3,
                                    "Advancing cluster time to the commit timestamp",
                                    "sessionId"_attr = _lsid,
                                    "txnNumberAndRetryCounter"_attr = _txnNumberAndRetryCounter,
                                    "commitTimestamp"_attr = *_decision->getCommitTimestamp());

                        VectorClockMutable::get(_serviceContext)
                            ->tickClusterTimeTo(LogicalTime(*_decision->getCommitTimestamp()));
                    }
                });
        })
        .onError<ErrorCodes::TransactionCoordinatorReachedAbortDecision>(
            [this, lsid, txnNumberAndRetryCounter](const Status& status) {
                // Timeout happened, propagate the decision to abort the transaction to replicas
                // and convert the internal error code to the public one.
                LOGV2(5047001,
                      "Transaction coordinator made abort decision",
                      "sessionId"_attr = lsid,
                      "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter,
                      "status"_attr = redact(status));
                stdx::lock_guard<Latch> lg(_mutex);
                _decision = txn::PrepareVote::kAbort;
                _decision->setAbortStatus(Status(ErrorCodes::NoSuchTransaction, status.reason()));
            })
        .then([this] {
            // Persist the commit decision, unless this has already been done (which would only be
            // the case if this coordinator was created as part of step-up recovery and the recovery
            // document contained a decision).
            //  Input: _decision
            //         _decisionDurable (optional)
            //  Output: _decisionDurable = true
            {
                stdx::lock_guard<Latch> lg(_mutex);
                invariant(_decision);

                auto previousStep = _step;
                _step = Step::kWritingDecision;

                _transactionCoordinatorMetricsObserver->onStartStep(
                    TransactionCoordinator::Step::kWritingDecision,
                    previousStep,
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());

                if (_decisionDurable)
                    return Future<repl::OpTime>::makeReady(repl::OpTime());
            }

            return txn::persistDecision(*_scheduler,
                                        _lsid,
                                        _txnNumberAndRetryCounter,
                                        *_participants,
                                        *_decision,
                                        _affectedNamespaces);
        })
        .then([this](repl::OpTime opTime) {
            setDecisionPromise(*_decision, _decisionPromise);
            return waitForMajorityWithHangFailpoint(_serviceContext,
                                                    hangBeforeWaitingForDecisionWriteConcern,
                                                    "hangBeforeWaitingForDecisionWriteConcern",
                                                    std::move(opTime),
                                                    _lsid,
                                                    _txnNumberAndRetryCounter,
                                                    _cancelToken);
        })
        .then([this, apiParams] {
            {
                stdx::lock_guard<Latch> lg(_mutex);
                _decisionDurable = true;
            }

            // Send the commit/abort decision to the participants.
            //  Input: _decisionDurable
            //  Output: (none)
            {
                stdx::lock_guard<Latch> lg(_mutex);
                invariant(_decisionDurable);

                auto previousStep = _step;
                _step = Step::kWaitingForDecisionAcks;

                _transactionCoordinatorMetricsObserver->onStartStep(
                    TransactionCoordinator::Step::kWaitingForDecisionAcks,
                    previousStep,
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());
            }

            switch (_decision->getDecision()) {
                case CommitDecision::kCommit: {
                    return txn::sendCommit(_serviceContext,
                                           *_scheduler,
                                           _lsid,
                                           _txnNumberAndRetryCounter,
                                           apiParams,
                                           *_participants,
                                           *_decision->getCommitTimestamp());
                }
                case CommitDecision::kAbort: {
                    return txn::sendAbort(_serviceContext,
                                          *_scheduler,
                                          _lsid,
                                          _txnNumberAndRetryCounter,
                                          apiParams,
                                          *_participants);
                }
                default:
                    MONGO_UNREACHABLE;
            };
        })
        .then([this, apiParams] {
            setDecisionPromise(*_decision, _decisionAcknowledgedPromise);

            {
                stdx::lock_guard<Latch> lg(_mutex);

                auto previousStep = _step;
                _step = Step::kWritingEndOfTransaction;

                _transactionCoordinatorMetricsObserver->onStartStep(
                    TransactionCoordinator::Step::kWritingEndOfTransaction,
                    previousStep,
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());
            }

            // If transaction was commited, write endOfTransaction oplog entries
            if (_decision->getDecision() != CommitDecision::kCommit) {
                return Future<void>::makeReady();
            }
            return txn::writeEndOfTransaction(
                *_scheduler, _lsid, _txnNumberAndRetryCounter, _affectedNamespaces);
        })
        .then([this] {
            // Do a best-effort attempt (i.e., writeConcern w:1) to delete the coordinator's durable
            // state.
            {
                stdx::lock_guard<Latch> lg(_mutex);

                auto previousStep = _step;
                _step = Step::kDeletingCoordinatorDoc;

                _transactionCoordinatorMetricsObserver->onStartStep(
                    TransactionCoordinator::Step::kDeletingCoordinatorDoc,
                    previousStep,
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());
            }
            return txn::deleteCoordinatorDoc(*_scheduler, _lsid, _txnNumberAndRetryCounter);
        })
        .getAsync([this, deadlineFuture = std::move(deadlineFuture)](Status s) mutable {
            // Interrupt this coordinator's scheduler hierarchy and join the deadline task's future
            // in order to guarantee that there are no more threads running within the coordinator.
            _scheduler->shutdown(
                {ErrorCodes::TransactionCoordinatorDeadlineTaskCanceled, "Coordinator completed"});

            return std::move(deadlineFuture).getAsync([this, s = std::move(s)](Status) {
                // Notify all the listeners which are interested in the coordinator's lifecycle.
                // After this call, the coordinator object could potentially get destroyed by its
                // lifetime controller, so there shouldn't be any accesses to `this` after this
                // call.
                _done(s);
            });
        });
}

TransactionCoordinator::~TransactionCoordinator() {
    invariant(_completionPromise.getFuture().isReady());
}

void TransactionCoordinator::runCommit(OperationContext* opCtx, std::vector<ShardId> participants) {
    if (!_reserveKickOffCommitPromise())
        return;
    invariant(opCtx != nullptr && opCtx->getClient() != nullptr);
    _updateAssociatedClient(opCtx->getClient());
    _participants = std::move(participants);
    _kickOffCommitPromise.emplaceValue();
}

void TransactionCoordinator::continueCommit(const TransactionCoordinatorDocument& doc) {
    if (!_reserveKickOffCommitPromise())
        return;

    _transactionCoordinatorMetricsObserver->onRecoveryFromFailover();

    _participants = doc.getParticipants();
    if (doc.getDecision()) {
        _participantsDurable = true;
        _decision = doc.getDecision();
    }
    _affectedNamespaces = doc.getAffectedNamespaces().get_value_or({});

    _kickOffCommitPromise.emplaceValue();
}

SharedSemiFuture<CommitDecision> TransactionCoordinator::getDecision() const {
    return _decisionPromise.getFuture();
}

SharedSemiFuture<void> TransactionCoordinator::onCompletion() const {
    return _completionPromise.getFuture();
}

SharedSemiFuture<CommitDecision> TransactionCoordinator::onDecisionAcknowledged() const {
    return _decisionAcknowledgedPromise.getFuture();
}

void TransactionCoordinator::cancelIfCommitNotYetStarted() {
    if (!_reserveKickOffCommitPromise())
        return;

    _kickOffCommitPromise.setError({ErrorCodes::TransactionCoordinatorCanceled,
                                    "Transaction exceeded deadline or newer transaction started"});
}

bool TransactionCoordinator::_reserveKickOffCommitPromise() {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_kickOffCommitPromiseSet)
        return false;

    _kickOffCommitPromiseSet = true;
    return true;
}

void TransactionCoordinator::_done(Status status) {
    // TransactionCoordinatorSteppingDown indicates the *sending* node (that is, *this* node) is
    // stepping down. Active coordinator tasks are interrupted with this code instead of
    // InterruptedDueToReplStateChange, because InterruptedDueToReplStateChange indicates the
    // *receiving* node was stepping down.
    if (status == ErrorCodes::TransactionCoordinatorSteppingDown)
        status = Status(ErrorCodes::InterruptedDueToReplStateChange,
                        str::stream()
                            << "Coordinator " << _lsid << ':' << _txnNumberAndRetryCounter.toBSON()
                            << " stopped due to: " << status.reason());

    LOGV2_DEBUG(22447,
                3,
                "Two-phase commit completed",
                "sessionId"_attr = _lsid,
                "txnNumberAndRetryCounter"_attr = _txnNumberAndRetryCounter,
                "status"_attr = redact(status));

    stdx::unique_lock<Latch> ul(_mutex);

    const auto tickSource = _serviceContext->getTickSource();

    _transactionCoordinatorMetricsObserver->onEnd(
        ServerTransactionCoordinatorsMetrics::get(_serviceContext),
        tickSource,
        _serviceContext->getPreciseClockSource()->now(),
        _step,
        _decisionDurable ? _decision : boost::none);

    if (status.isOK() &&
        (shouldLog(logv2::LogComponent::kTransaction, logv2::LogSeverity::Debug(1)) ||
         _transactionCoordinatorMetricsObserver->getSingleTransactionCoordinatorStats()
                 .getTwoPhaseCommitDuration(tickSource, tickSource->getTicks()) >
             Milliseconds(serverGlobalParams.slowMS.load()))) {
        _logSlowTwoPhaseCommit(*_decision);
    }

    ul.unlock();

    if (!_decisionPromise.getFuture().isReady()) {
        _decisionPromise.setError(status);
    }
    if (!_decisionAcknowledgedPromise.getFuture().isReady()) {
        _decisionAcknowledgedPromise.setError(status);
    }

    if (!status.isOK()) {
        _completionPromise.setError(status);
    } else {
        _completionPromise.setFrom(_decisionPromise.getFuture().getNoThrow().getStatus());
    }
}

void TransactionCoordinator::_logSlowTwoPhaseCommit(
    const txn::CoordinatorCommitDecision& decision) {
    logv2::DynamicAttributes attrs;

    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _lsid.serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", _txnNumberAndRetryCounter.getTxnNumber());
    parametersBuilder.append("txnRetryCounter", *_txnNumberAndRetryCounter.getTxnRetryCounter());

    attrs.add("parameters", parametersBuilder.obj());

    std::string decisionTemp;
    switch (decision.getDecision()) {
        case txn::CommitDecision::kCommit:
            attrs.add("terminationCause", "committed");
            attrs.add("commitTimestamp", decision.getCommitTimestamp()->toBSON());
            break;
        case txn::CommitDecision::kAbort:
            attrs.add("terminationCause", "aborted");
            attrs.add("terminationDetails", *decision.getAbortStatus());
            break;
        default:
            MONGO_UNREACHABLE;
    };

    attrs.add("numParticipants", _participants->size());

    auto tickSource = _serviceContext->getTickSource();
    auto curTick = tickSource->getTicks();
    const auto& singleTransactionCoordinatorStats =
        _transactionCoordinatorMetricsObserver->getSingleTransactionCoordinatorStats();

    BSONObjBuilder stepDurations;
    stepDurations.append(
        "writingParticipantListMicros",
        durationCount<Microseconds>(singleTransactionCoordinatorStats.getStepDuration(
            Step::kWritingParticipantList, tickSource, curTick)));
    stepDurations.append(
        "waitingForVotesMicros",
        durationCount<Microseconds>(singleTransactionCoordinatorStats.getStepDuration(
            Step::kWaitingForVotes, tickSource, curTick)));
    stepDurations.append(
        "writingDecisionMicros",
        durationCount<Microseconds>(singleTransactionCoordinatorStats.getStepDuration(
            Step::kWritingDecision, tickSource, curTick)));
    stepDurations.append(
        "waitingForDecisionAcksMicros",
        durationCount<Microseconds>(singleTransactionCoordinatorStats.getStepDuration(
            Step::kWaitingForDecisionAcks, tickSource, curTick)));
    stepDurations.append(
        "writingEndOfTransactionMicros",
        durationCount<Microseconds>(singleTransactionCoordinatorStats.getStepDuration(
            Step::kWritingEndOfTransaction, tickSource, curTick)));
    stepDurations.append(
        "deletingCoordinatorDocMicros",
        durationCount<Microseconds>(singleTransactionCoordinatorStats.getStepDuration(
            Step::kDeletingCoordinatorDoc, tickSource, curTick)));

    attrs.add("stepDurations", stepDurations.obj());

    // Total duration of the commit coordination. Logged at the end of the line for consistency
    // with slow command logging. Note that this is reported in milliseconds while the step
    // durations are reported in microseconds.
    attrs.add(
        "duration",
        duration_cast<Milliseconds>(
            singleTransactionCoordinatorStats.getTwoPhaseCommitDuration(tickSource, curTick)));

    LOGV2(51804, "two-phase commit", attrs);
}

TransactionCoordinator::Step TransactionCoordinator::getStep() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _step;
}

void TransactionCoordinator::reportState(OperationContext* opCtx, BSONObjBuilder& parent) const {
    BSONObjBuilder doc;
    TickSource* tickSource = _serviceContext->getTickSource();
    TickSource::Tick currentTick = tickSource->getTicks();

    stdx::lock_guard<Latch> lk(_mutex);

    BSONObjBuilder lsidBuilder(doc.subobjStart("lsid"));
    _lsid.serialize(&lsidBuilder);
    lsidBuilder.doneFast();
    doc.append("txnNumber", _txnNumberAndRetryCounter.getTxnNumber());
    doc.append("txnRetryCounter", *_txnNumberAndRetryCounter.getTxnRetryCounter());

    if (_participants) {
        doc.append("numParticipants", static_cast<long long>(_participants->size()));
    }

    doc.append("state", toString(_step));

    const auto& singleStats =
        _transactionCoordinatorMetricsObserver->getSingleTransactionCoordinatorStats();
    singleStats.reportMetrics(doc, tickSource, currentTick);
    singleStats.reportLastClient(opCtx, parent);

    if (_decision)
        doc.append("decision", _decision->toBSON());

    doc.append("deadline", _deadline);

    parent.append("desc", "transaction coordinator");
    parent.append("twoPhaseCommitCoordinator", doc.obj());
}

std::string TransactionCoordinator::toString(Step step) {
    switch (step) {
        case Step::kInactive:
            return "inactive";
        case Step::kWritingParticipantList:
            return "writingParticipantList";
        case Step::kWaitingForVotes:
            return "waitingForVotes";
        case Step::kWritingDecision:
            return "writingDecision";
        case Step::kWaitingForDecisionAcks:
            return "waitingForDecisionAcks";
        case Step::kWritingEndOfTransaction:
            return "writingEndOfTransaction";
        case Step::kDeletingCoordinatorDoc:
            return "deletingCoordinatorDoc";
    }
    MONGO_UNREACHABLE;
}

void TransactionCoordinator::_updateAssociatedClient(Client* client) {
    stdx::lock_guard<Latch> lk(_mutex);
    _transactionCoordinatorMetricsObserver->updateLastClientInfo(client);
}

}  // namespace mongo
