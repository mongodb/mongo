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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/s/transaction_coordinator.h"

#include "mongo/db/logical_clock.h"
#include "mongo/db/s/transaction_coordinator_metrics_observer.h"
#include "mongo/db/server_options.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using CommitDecision = txn::CommitDecision;
using CoordinatorCommitDecision = txn::CoordinatorCommitDecision;
using PrepareVoteConsensus = txn::PrepareVoteConsensus;
using TransactionCoordinatorDocument = txn::TransactionCoordinatorDocument;

}  // namespace

TransactionCoordinator::TransactionCoordinator(ServiceContext* serviceContext,
                                               const LogicalSessionId& lsid,
                                               TxnNumber txnNumber,
                                               std::unique_ptr<txn::AsyncWorkScheduler> scheduler,
                                               Date_t deadline)
    : _serviceContext(serviceContext),
      _lsid(lsid),
      _txnNumber(txnNumber),
      _scheduler(std::move(scheduler)),
      _sendPrepareScheduler(_scheduler->makeChildScheduler()),
      _transactionCoordinatorMetricsObserver(
          std::make_unique<TransactionCoordinatorMetricsObserver>()) {

    auto kickOffCommitPF = makePromiseFuture<void>();
    _kickOffCommitPromise = std::move(kickOffCommitPF.promise);

    // Task, which will fire when the transaction's total deadline has been reached. If the 2PC
    // sequence has not yet started, it will be abandoned altogether.
    auto deadlineFuture =
        _scheduler
            ->scheduleWorkAt(deadline,
                             [this](OperationContext*) {
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
    _transactionCoordinatorMetricsObserver->onCreate(
        ServerTransactionCoordinatorsMetrics::get(_serviceContext),
        _serviceContext->getTickSource(),
        _serviceContext->getPreciseClockSource()->now());

    // Two-phase commit phases chain. Once this chain executes, the 2PC sequence has completed
    // either with success or error and the scheduled deadline task above has been joined.
    std::move(kickOffCommitPF.future)
        .then([this] {
            // Persist the participants, unless they have been made durable already (which would
            // only be the case if this coordinator was created as part of step-up recovery).
            //  Input: _participants
            //         _participantsDurable (optional)
            //  Output: _participantsDurable = true
            {
                stdx::lock_guard<stdx::mutex> lg(_mutex);
                invariant(_participants);

                _step = Step::kWritingParticipantList;

                // TODO: The duration will be meaningless after failover.
                _transactionCoordinatorMetricsObserver->onStartWritingParticipantList(
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());

                if (_participantsDurable)
                    return Future<void>::makeReady();
            }

            return txn::persistParticipantsList(
                       *_sendPrepareScheduler, _lsid, _txnNumber, *_participants)
                .then([this] {
                    stdx::lock_guard<stdx::mutex> lg(_mutex);
                    _participantsDurable = true;
                });
        })
        .then([this] {
            // Send prepare to the participants, unless this has already been done (which would only
            // be the case if this coordinator was created as part of step-up recovery and the
            // recovery document contained a decision).
            //  Input: _participants, _participantsDurable
            //         _decision (optional)
            //  Output: _decision is set
            {
                stdx::lock_guard<stdx::mutex> lg(_mutex);
                invariant(_participantsDurable);

                _step = Step::kWaitingForVotes;

                // TODO: The duration will be meaningless after failover.
                _transactionCoordinatorMetricsObserver->onStartWaitingForVotes(
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());

                if (_decision)
                    return Future<void>::makeReady();
            }

            return txn::sendPrepare(
                       _serviceContext, *_sendPrepareScheduler, _lsid, _txnNumber, *_participants)
                .then([this](PrepareVoteConsensus consensus) mutable {
                    {
                        stdx::lock_guard<stdx::mutex> lg(_mutex);
                        _decision = consensus.decision();
                    }

                    if (_decision->getDecision() == CommitDecision::kCommit) {
                        LOG(3) << "Advancing cluster time to the commit timestamp "
                               << *_decision->getCommitTimestamp() << " for " << _lsid.getId()
                               << ':' << _txnNumber;

                        uassertStatusOK(LogicalClock::get(_serviceContext)
                                            ->advanceClusterTime(
                                                LogicalTime(*_decision->getCommitTimestamp())));
                    }
                });
        })
        .then([this] {
            // Persist the commit decision, unless this has already been done (which would only be
            // the case if this coordinator was created as part of step-up recovery and the recovery
            // document contained a decision).
            //  Input: _decision
            //         _decisionDurable (optional)
            //  Output: _decisionDurable = true
            {
                stdx::lock_guard<stdx::mutex> lg(_mutex);
                invariant(_decision);

                _step = Step::kWritingDecision;

                // TODO: The duration will be meaningless after failover.
                _transactionCoordinatorMetricsObserver->onStartWritingDecision(
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());

                if (_decisionDurable)
                    return Future<void>::makeReady();
            }

            return txn::persistDecision(*_scheduler, _lsid, _txnNumber, *_participants, *_decision)
                .then([this] {
                    stdx::lock_guard<stdx::mutex> lg(_mutex);
                    _decisionDurable = true;
                });
        })
        .then([this] {
            // Send the commit/abort decision to the participants.
            //  Input: _decisionDurable
            //  Output: (none)
            {
                stdx::lock_guard<stdx::mutex> lg(_mutex);
                invariant(_decisionDurable);

                _step = Step::kWaitingForDecisionAcks;

                // TODO: The duration will be meaningless after failover.
                _transactionCoordinatorMetricsObserver->onStartWaitingForDecisionAcks(
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());
            }

            _decisionPromise.emplaceValue(_decision->getDecision());

            switch (_decision->getDecision()) {
                case CommitDecision::kCommit:
                    return txn::sendCommit(_serviceContext,
                                           *_scheduler,
                                           _lsid,
                                           _txnNumber,
                                           *_participants,
                                           *_decision->getCommitTimestamp());
                case CommitDecision::kAbort:
                    return txn::sendAbort(
                        _serviceContext, *_scheduler, _lsid, _txnNumber, *_participants);
                default:
                    MONGO_UNREACHABLE;
            };
        })
        .then([this] {
            // Do a best-effort attempt (i.e., writeConcern w:1) to delete the coordinator's durable
            // state.
            {
                stdx::lock_guard<stdx::mutex> lg(_mutex);

                _step = Step::kDeletingCoordinatorDoc;

                _transactionCoordinatorMetricsObserver->onStartDeletingCoordinatorDoc(
                    ServerTransactionCoordinatorsMetrics::get(_serviceContext),
                    _serviceContext->getTickSource(),
                    _serviceContext->getPreciseClockSource()->now());
            }

            return txn::deleteCoordinatorDoc(*_scheduler, _lsid, _txnNumber);
        })
        .onCompletion([ this, deadlineFuture = std::move(deadlineFuture) ](Status s) mutable {
            // Interrupt this coordinator's scheduler hierarchy and join the deadline task's future
            // in order to guarantee that there are no more threads running within the coordinator.
            _scheduler->shutdown(
                {ErrorCodes::TransactionCoordinatorDeadlineTaskCanceled, "Coordinator completed"});

            return std::move(deadlineFuture).onCompletion([s = std::move(s)](Status) { return s; });
        })
        .getAsync([this](Status s) {
            // Notify all the listeners which are interested in the coordinator's lifecycle. After
            // this call, the coordinator object could potentially get destroyed by its lifetime
            // controller, so there shouldn't be any accesses to `this` after this call.
            _done(s);
        });
}

TransactionCoordinator::~TransactionCoordinator() {
    invariant(_completionPromisesFired);
    invariant(_completionPromises.empty());
}

void TransactionCoordinator::runCommit(std::vector<ShardId> participants) {
    if (!_reserveKickOffCommitPromise())
        return;

    _participants = std::move(participants);
    _kickOffCommitPromise.emplaceValue();
}

void TransactionCoordinator::continueCommit(const TransactionCoordinatorDocument& doc) {
    if (!_reserveKickOffCommitPromise())
        return;

    _participants = std::move(doc.getParticipants());
    if (doc.getDecision()) {
        _participantsDurable = true;
        _decision = std::move(doc.getDecision());
    }

    _kickOffCommitPromise.emplaceValue();
}

SharedSemiFuture<CommitDecision> TransactionCoordinator::getDecision() {
    return _decisionPromise.getFuture();
}

Future<void> TransactionCoordinator::onCompletion() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (_completionPromisesFired)
        return Future<void>::makeReady();

    auto completionPF = makePromiseFuture<void>();
    _completionPromises.emplace_back(std::move(completionPF.promise));

    return std::move(completionPF.future);
}

void TransactionCoordinator::cancelIfCommitNotYetStarted() {
    if (!_reserveKickOffCommitPromise())
        return;

    _kickOffCommitPromise.setError({ErrorCodes::NoSuchTransaction,
                                    "Transaction exceeded deadline or newer transaction started"});
}

bool TransactionCoordinator::_reserveKickOffCommitPromise() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
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
                        str::stream() << "Coordinator " << _lsid.getId() << ':' << _txnNumber
                                      << " stopped due to: "
                                      << status.reason());

    LOG(3) << "Two-phase commit for " << _lsid.getId() << ':' << _txnNumber << " completed with "
           << redact(status);

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    const auto tickSource = _serviceContext->getTickSource();

    _transactionCoordinatorMetricsObserver->onEnd(
        ServerTransactionCoordinatorsMetrics::get(_serviceContext),
        tickSource,
        _serviceContext->getPreciseClockSource()->now(),
        _step,
        _decisionDurable ? _decision : boost::none);

    if (status.isOK() &&
        (shouldLog(logger::LogComponent::kTransaction, logger::LogSeverity::Debug(1)) ||
         _transactionCoordinatorMetricsObserver->getSingleTransactionCoordinatorStats()
                 .getTwoPhaseCommitDuration(tickSource, tickSource->getTicks()) >
             Milliseconds(serverGlobalParams.slowMS))) {
        _logSlowTwoPhaseCommit(*_decision);
    }

    _completionPromisesFired = true;

    if (!_decisionDurable) {
        ul.unlock();
        _decisionPromise.setError(status);
        ul.lock();
    }

    // Trigger the onCompletion promises outside of a lock, because the future handlers indicate to
    // the potential lifetime controller that the object can be destroyed
    auto promisesToTrigger = std::move(_completionPromises);
    ul.unlock();

    for (auto&& promise : promisesToTrigger) {
        promise.emplaceValue();
    }
}

void TransactionCoordinator::_logSlowTwoPhaseCommit(
    const txn::CoordinatorCommitDecision& decision) {
    log() << _twoPhaseCommitInfoForLog(decision);
}

std::string TransactionCoordinator::_twoPhaseCommitInfoForLog(
    const txn::CoordinatorCommitDecision& decision) const {
    StringBuilder s;

    s << "two-phase commit";

    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _lsid.serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", _txnNumber);

    s << " parameters:" << parametersBuilder.obj().toString();

    switch (decision.getDecision()) {
        case txn::CommitDecision::kCommit:
            s << ", terminationCause:committed";
            s << ", commitTimestamp: " << decision.getCommitTimestamp()->toString();
            break;
        case txn::CommitDecision::kAbort:
            s << ", terminationCause:aborted";
            s << ", terminationDetails: " << *decision.getAbortStatus();
            break;
        default:
            MONGO_UNREACHABLE;
    };

    s << ", numParticipants:" << _participants->size();

    auto tickSource = _serviceContext->getTickSource();
    auto curTick = tickSource->getTicks();
    const auto& singleTransactionCoordinatorStats =
        _transactionCoordinatorMetricsObserver->getSingleTransactionCoordinatorStats();

    BSONObjBuilder stepDurations;
    stepDurations.append("writingParticipantListMicros",
                         durationCount<Microseconds>(
                             singleTransactionCoordinatorStats.getWritingParticipantListDuration(
                                 tickSource, curTick)));
    stepDurations.append(
        "waitingForVotesMicros",
        durationCount<Microseconds>(
            singleTransactionCoordinatorStats.getWaitingForVotesDuration(tickSource, curTick)));
    stepDurations.append(
        "writingDecisionMicros",
        durationCount<Microseconds>(
            singleTransactionCoordinatorStats.getWritingDecisionDuration(tickSource, curTick)));
    stepDurations.append("waitingForDecisionAcksMicros",
                         durationCount<Microseconds>(
                             singleTransactionCoordinatorStats.getWaitingForDecisionAcksDuration(
                                 tickSource, curTick)));
    stepDurations.append("deletingCoordinatorDocMicros",
                         durationCount<Microseconds>(
                             singleTransactionCoordinatorStats.getDeletingCoordinatorDocDuration(
                                 tickSource, curTick)));
    s << ", stepDurations:" << stepDurations.obj();

    // Total duration of the commit coordination. Logged at the end of the line for consistency with
    // slow command logging.
    // Note that this is reported in milliseconds while the step durations are reported in
    // microseconds.
    s << " "
      << duration_cast<Milliseconds>(
             singleTransactionCoordinatorStats.getTwoPhaseCommitDuration(tickSource, curTick));

    return s.str();
}

}  // namespace mongo
