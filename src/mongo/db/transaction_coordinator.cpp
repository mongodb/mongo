
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

#include "mongo/db/transaction_coordinator.h"

#include "mongo/db/logical_clock.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {

using Action = TransactionCoordinator::StateMachine::Action;
using Event = TransactionCoordinator::StateMachine::Event;
using State = TransactionCoordinator::StateMachine::State;

//
// Pre-decision
//

Action TransactionCoordinator::recvCoordinateCommit(const std::set<ShardId>& participants) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _participantList.recordFullList(participants);
    return _stateMachine.onEvent(std::move(lk), Event::kRecvParticipantList);
}

Action TransactionCoordinator::madeParticipantListDurable() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stateMachine.onEvent(std::move(lk), Event::kMadeParticipantListDurable);
}

//
// Abort path
//

Action TransactionCoordinator::recvVoteAbort(const ShardId& shardId) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _participantList.recordVoteAbort(shardId);
    return _stateMachine.onEvent(std::move(lk), Event::kRecvVoteAbort);
}

Action TransactionCoordinator::madeAbortDecisionDurable() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stateMachine.onEvent(std::move(lk), Event::kMadeAbortDecisionDurable);
}

Action TransactionCoordinator::recvAbortAck(const ShardId& shardId) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _participantList.recordAbortAck(shardId);
    auto event = _participantList.allParticipantsAckedAbort() ? Event::kRecvFinalAbortAck
                                                              : Event::kRecvAbortAck;
    return _stateMachine.onEvent(std::move(lk), event);
}

//
// Commit path
//

Action TransactionCoordinator::recvVoteCommit(const ShardId& shardId, Timestamp prepareTimestamp) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _participantList.recordVoteCommit(shardId, prepareTimestamp);
    auto event = (_participantList.allParticipantsVotedCommit()) ? Event::kRecvFinalVoteCommit
                                                                 : Event::kRecvVoteCommit;
    if (event == Event::kRecvFinalVoteCommit) {
        const auto maxPrepareTs = _participantList.getHighestPrepareTimestamp();
        _commitTimestamp = Timestamp(maxPrepareTs.getSecs(), maxPrepareTs.getInc() + 1);
        Status s = LogicalClock::get(getGlobalServiceContext())
                       ->advanceClusterTime(LogicalTime(_commitTimestamp.get()));
        if (!s.isOK()) {
            log() << "Coordinator shard failed to advance cluster time to commitTimestamp "
                  << causedBy(s);
        }
    }
    return _stateMachine.onEvent(std::move(lk), event);
}

Action TransactionCoordinator::madeCommitDecisionDurable() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stateMachine.onEvent(std::move(lk), Event::kMadeCommitDecisionDurable);
}

Action TransactionCoordinator::recvCommitAck(const ShardId& shardId) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _participantList.recordCommitAck(shardId);
    auto event = _participantList.allParticipantsAckedCommit() ? Event::kRecvFinalCommitAck
                                                               : Event::kRecvCommitAck;
    return _stateMachine.onEvent(std::move(lk), event);
}

//
// Any time
//

Future<TransactionCoordinator::StateMachine::State> TransactionCoordinator::waitForCompletion() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stateMachine.waitForTransitionTo({State::kCommitted, State::kAborted});
}

Future<TransactionCoordinator::CommitDecision> TransactionCoordinator::waitForDecision() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stateMachine
        .waitForTransitionTo({State::kWaitingForAbortAcks,
                              State::kWaitingForCommitAcks,
                              State::kCommitted,
                              State::kAborted})
        .then([](auto state) {
            switch (state) {
                case TransactionCoordinator::StateMachine::State::kWaitingForAbortAcks:
                case TransactionCoordinator::StateMachine::State::kAborted:
                    return TransactionCoordinator::CommitDecision::kAbort;
                case TransactionCoordinator::StateMachine::State::kWaitingForCommitAcks:
                case TransactionCoordinator::StateMachine::State::kCommitted:
                    return TransactionCoordinator::CommitDecision::kCommit;
                default:
                    MONGO_UNREACHABLE;
            }
        });
}

Action TransactionCoordinator::recvTryAbort() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _stateMachine.onEvent(std::move(lk), Event::kRecvTryAbort);
}

//
// StateMachine
//

/**
 * This table shows the events that are legal to occur (given an asynchronous network) while in each
 * state.
 *
 * For each legal event, it shows the associated action (if any) the coordinator should take, and
 * the next state the coordinator should transition to.
 *
 * Empty ("{}") transitions mean "legal event, but no action to take and no new state to transition
 * to.
 * Missing transitions are illegal.
 */
const std::map<State, std::map<Event, TransactionCoordinator::StateMachine::Transition>>
    TransactionCoordinator::StateMachine::transitionTable = {
        // clang-format off
        {State::kUninitialized, {
            {Event::kRecvParticipantList,   {Action::kWriteParticipantList, State::kMakingParticipantListDurable}},
            {Event::kRecvTryAbort,          {{}, State::kAborted}},
        }},
        {State::kMakingParticipantListDurable, {
            {Event::kRecvParticipantList,          {}},
            {Event::kMadeParticipantListDurable,   {Action::kSendPrepare, State::kWaitingForVotes}},
            {Event::kRecvTryAbort,                 {}},
        }},
        {State::kWaitingForVotes, {
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvVoteAbort,         {Action::kWriteAbortDecision, State::kMakingAbortDecisionDurable}},
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvFinalVoteCommit,   {Action::kWriteCommitDecision, State::kMakingCommitDecisionDurable}},
            {Event::kRecvTryAbort,          {}},
        }},

        // Abort path
        // Note: Can continue to receive votes after abort decision has been made, because an abort
        // decision only requires a single voteAbort.
        {State::kMakingAbortDecisionDurable, {
            {Event::kRecvParticipantList,      {}},
            {Event::kRecvVoteAbort,            {}},
            {Event::kRecvVoteCommit,           {}},
            {Event::kMadeAbortDecisionDurable, {Action::kSendAbort, State::kWaitingForAbortAcks}},
            {Event::kRecvTryAbort,             {}},
        }},
        {State::kWaitingForAbortAcks, {
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvVoteAbort,         {}},
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvAbortAck,          {}},
            {Event::kRecvFinalAbortAck,     {Action::kDone, State::kAborted}},
            {Event::kRecvTryAbort,          {}},

        }},
        {State::kAborted, {
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvVoteAbort,         {}},
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvTryAbort,          {}},
        }},

        // Commit path
        // Note: Cannot continue to receive votes after commit decision has been made, because a
        // commit decision requires all voteCommits.
        {State::kMakingCommitDecisionDurable, {
            {Event::kRecvParticipantList,       {}},
            {Event::kMadeCommitDecisionDurable, {Action::kSendCommit, State::kWaitingForCommitAcks}},
            {Event::kRecvTryAbort,              {}},

        }},
        {State::kWaitingForCommitAcks, {
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvCommitAck,         {}},
            {Event::kRecvFinalCommitAck,    {Action::kDone, State::kCommitted}},
            {Event::kRecvTryAbort,          {}},

        }},
        {State::kCommitted, {
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvTryAbort,          {}},
        }},

        {State::kBroken, {}},
        // clang-format on
};

TransactionCoordinator::StateMachine::~StateMachine() {
    // Coordinators should always reach a terminal state prior to destructing, and all calls to
    // waitForTransitionTo must contain both terminal states, so all outstanding promises should
    // have been triggered prior to this.
    invariant(_stateTransitionPromises.size() == 0);
}

void TransactionCoordinator::StateMachine::_signalAllPromisesWaitingForState(
    stdx::unique_lock<stdx::mutex> lk, State newState) {

    std::list<StateTransitionPromise> promisesToTrigger;
    // Reorder promises so that those which were waiting to be signaled on the current state
    // come first, and those which were not come last.
    auto partitionPoint =
        std::partition(_stateTransitionPromises.begin(),
                       _stateTransitionPromises.end(),
                       [&](const auto& stateTransitionPromise) {
                           return stateTransitionPromise.triggeringStates.find(newState) !=
                               stateTransitionPromise.triggeringStates.end();
                       });
    // Remove all of the promises to trigger from _stateTransitionPromises and put them in
    // promisesToTrigger.
    promisesToTrigger.splice(promisesToTrigger.begin(),
                             _stateTransitionPromises,
                             _stateTransitionPromises.begin(),
                             partitionPoint);

    // Signaling the promises must be done without holding the mutex to avoid deadlock in case
    // signaling the promise triggers a callback that requires locking the mutex.
    lk.unlock();
    for (auto& stateTransitionPromise : promisesToTrigger) {
        stateTransitionPromise.promise.emplaceValue(newState);
    }
}

Action TransactionCoordinator::StateMachine::onEvent(stdx::unique_lock<stdx::mutex> lk,
                                                     Event event) {

    const auto legalTransitions = transitionTable.find(_state)->second;
    if (!legalTransitions.count(event)) {
        std::string errmsg = str::stream() << "Transaction coordinator received illegal event '"
                                           << event << "' while in state '" << _state << "'";
        _state = State::kBroken;
        uasserted(ErrorCodes::InternalError, errmsg);
    }

    const auto transition = legalTransitions.find(event)->second;

    if (transition.nextState) {
        StringBuilder ss;
        ss << "TransactionCoordinator received event " << event << " while in state " << _state
           << " and returning " << transition.action << " and transitioning to "
           << *transition.nextState;
        LOG(3) << ss.str();
        _state = *transition.nextState;
        _signalAllPromisesWaitingForState(std::move(lk), _state);
    } else {
        StringBuilder ss;
        ss << "TransactionCoordinator received event " << event << " while in state " << _state
           << " and returning " << transition.action << " and not transitioning to new state";
        LOG(3) << ss.str();
    }

    return transition.action;
}

Future<State> TransactionCoordinator::StateMachine::waitForTransitionTo(
    const std::set<State>& states) {

    // The set of states waited on MUST include both terminal states of the state machine (committed
    // and aborted). Otherwise it would be possible to wait on a state which is never reached,
    // causing the caller to hang forever.
    invariant(states.find(State::kCommitted) != states.end());
    invariant(states.find(State::kAborted) != states.end());

    // If we're already in one of the states the caller is waiting for, there's no need for a
    // promise so we return immediately.
    if (states.find(_state) != states.end()) {
        return Future<State>::makeReady(_state);
    }

    auto promiseAndFuture = makePromiseFuture<TransactionCoordinator::StateMachine::State>();

    _stateTransitionPromises.emplace_back(std::move(promiseAndFuture.promise), states);

    return std::move(promiseAndFuture.future);
}

//
// ParticipantList
//

void TransactionCoordinator::ParticipantList::recordFullList(
    const std::set<ShardId>& participants) {
    if (!_fullListReceived) {
        for (auto& shardId : participants) {
            _recordParticipant(shardId);
        }
        _fullListReceived = true;
    }
    _validate(participants);
}

void TransactionCoordinator::ParticipantList::recordVoteCommit(const ShardId& shardId,
                                                               Timestamp prepareTimestamp) {
    if (!_fullListReceived) {
        _recordParticipant(shardId);
    }

    auto it = _participants.find(shardId);
    uassert(
        ErrorCodes::InternalError,
        str::stream() << "Transaction commit coordinator received vote 'commit' from participant "
                      << shardId.toString()
                      << " not in participant list",
        it != _participants.end());
    auto& participant = it->second;

    uassert(
        ErrorCodes::InternalError,
        str::stream() << "Transaction commit coordinator received vote 'commit' from participant "
                      << shardId.toString()
                      << " that previously voted to abort",
        participant.vote != Participant::Vote::kAbort);

    if (participant.vote == Participant::Vote::kUnknown) {
        participant.vote = Participant::Vote::kCommit;
        participant.prepareTimestamp = prepareTimestamp;
    } else {
        uassert(ErrorCodes::InternalError,
                str::stream() << "Transaction commit coordinator received prepareTimestamp "
                              << prepareTimestamp.toStringPretty()
                              << " from participant "
                              << shardId.toString()
                              << " that previously reported prepareTimestamp "
                              << participant.prepareTimestamp->toStringPretty(),
                *participant.prepareTimestamp == prepareTimestamp);
    }
}

void TransactionCoordinator::ParticipantList::recordVoteAbort(const ShardId& shardId) {
    if (!_fullListReceived) {
        _recordParticipant(shardId);
    }

    auto it = _participants.find(shardId);
    uassert(
        ErrorCodes::InternalError,
        str::stream() << "Transaction commit coordinator received vote 'abort' from participant "
                      << shardId.toString()
                      << " not in participant list",
        it != _participants.end());
    auto& participant = it->second;

    uassert(
        ErrorCodes::InternalError,
        str::stream() << "Transaction commit coordinator received vote 'abort' from participant "
                      << shardId.toString()
                      << " that previously voted to commit",
        participant.vote != Participant::Vote::kCommit);

    participant.vote = Participant::Vote::kAbort;
    participant.ack = Participant::Ack::kAbort;
}

void TransactionCoordinator::ParticipantList::recordCommitAck(const ShardId& shardId) {
    auto it = _participants.find(shardId);
    uassert(
        50989,
        str::stream() << "Transaction commit coordinator processed 'commit' ack from participant "
                      << shardId.toString()
                      << " not in participant list",
        it != _participants.end());
    it->second.ack = Participant::Ack::kCommit;
}

void TransactionCoordinator::ParticipantList::recordAbortAck(const ShardId& shardId) {
    auto it = _participants.find(shardId);
    uassert(
        50990,
        str::stream() << "Transaction commit coordinator processed 'abort' ack from participant "
                      << shardId.toString()
                      << " not in participant list",
        it != _participants.end());
    it->second.ack = Participant::Ack::kAbort;
}

bool TransactionCoordinator::ParticipantList::allParticipantsVotedCommit() const {
    return _fullListReceived && std::all_of(_participants.begin(),
                                            _participants.end(),
                                            [](const std::pair<ShardId, Participant>& i) {
                                                return i.second.vote == Participant::Vote::kCommit;
                                            });
}

bool TransactionCoordinator::ParticipantList::allParticipantsAckedAbort() const {
    return std::all_of(
        _participants.begin(), _participants.end(), [](const std::pair<ShardId, Participant>& i) {
            return i.second.ack == Participant::Ack::kAbort;
        });
}

bool TransactionCoordinator::ParticipantList::allParticipantsAckedCommit() const {
    invariant(_fullListReceived);
    return std::all_of(
        _participants.begin(), _participants.end(), [](const std::pair<ShardId, Participant>& i) {
            return i.second.ack == Participant::Ack::kCommit;
        });
}

Timestamp TransactionCoordinator::ParticipantList::getHighestPrepareTimestamp() const {
    invariant(_fullListReceived);
    Timestamp highestPrepareTimestamp = Timestamp::min();
    for (const auto& participant : _participants) {
        invariant(participant.second.prepareTimestamp);
        if (*participant.second.prepareTimestamp > highestPrepareTimestamp) {
            highestPrepareTimestamp = *participant.second.prepareTimestamp;
        }
    }
    return highestPrepareTimestamp;
}

std::set<ShardId> TransactionCoordinator::ParticipantList::getParticipants() const {
    std::set<ShardId> participants;
    for (const auto& kv : _participants) {
        participants.insert(kv.first);
    }
    return participants;
}

std::set<ShardId> TransactionCoordinator::ParticipantList::getNonAckedCommitParticipants() const {
    std::set<ShardId> nonAckedCommitParticipants;
    for (const auto& kv : _participants) {
        if (kv.second.ack != Participant::Ack::kCommit) {
            invariant(kv.second.ack == Participant::Ack::kNone);
            nonAckedCommitParticipants.insert(kv.first);
        }
    }
    return nonAckedCommitParticipants;
}

std::set<ShardId> TransactionCoordinator::ParticipantList::getNonVotedAbortParticipants() const {
    std::set<ShardId> nonVotedAbortParticipants;
    for (const auto& kv : _participants) {
        if (kv.second.vote != Participant::Vote::kAbort) {
            invariant(kv.second.ack == Participant::Ack::kNone);
            nonVotedAbortParticipants.insert(kv.first);
        }
    }
    return nonVotedAbortParticipants;
}

void TransactionCoordinator::ParticipantList::_recordParticipant(const ShardId& shardId) {
    if (_participants.find(shardId) == _participants.end()) {
        _participants[shardId] = {};
    }
}

void TransactionCoordinator::ParticipantList::_validate(const std::set<ShardId>& participants) {
    // Ensure that the participant list received contained only participants that we already know
    // about.
    for (auto& shardId : participants) {
        uassert(ErrorCodes::InternalError,
                str::stream() << "Transaction commit coordinator received a participant list with "
                                 "unexpected participant "
                              << shardId,
                _participants.find(shardId) != _participants.end());
    }

    // Ensure that the participant list received is not missing a participant we already heard from.
    for (auto& it : _participants) {
        auto& shardId = it.first;
        uassert(ErrorCodes::InternalError,
                str::stream() << "Transaction commit coordinator received a participant list "
                                 "missing expected participant "
                              << shardId.toString(),
                participants.find(shardId) != participants.end());
    }
}

}  // namespace mongo
