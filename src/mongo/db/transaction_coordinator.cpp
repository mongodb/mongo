/**
 *    Copyright (C) 2018 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/session_catalog.h"
#include "mongo/db/transaction_coordinator.h"

#include "mongo/db/session.h"

namespace mongo {

using Action = TransactionCoordinator::StateMachine::Action;
using Event = TransactionCoordinator::StateMachine::Event;
using State = TransactionCoordinator::StateMachine::State;

namespace {
const Session::Decoration<boost::optional<TransactionCoordinator>> getTransactionCoordinator =
    Session::declareDecoration<boost::optional<TransactionCoordinator>>();
}  // namespace

boost::optional<TransactionCoordinator>& TransactionCoordinator::get(OperationContext* opCtx) {
    auto session = OperationContextSession::get(opCtx);
    return getTransactionCoordinator(session);
}

void TransactionCoordinator::create(Session* session) {
    invariant(!getTransactionCoordinator(session));
    getTransactionCoordinator(session).emplace();
}

Action TransactionCoordinator::recvCoordinateCommit(const std::set<ShardId>& participants) {
    _participantList.recordFullList(participants);
    return _stateMachine.onEvent(Event::kRecvParticipantList);
}

Action TransactionCoordinator::recvVoteCommit(const ShardId& shardId, int prepareTimestamp) {
    _participantList.recordVoteCommit(shardId, prepareTimestamp);

    auto event = (_participantList.allParticipantsVotedCommit()) ? Event::kRecvFinalVoteCommit
                                                                 : Event::kRecvVoteCommit;
    return _stateMachine.onEvent(event);
}

Action TransactionCoordinator::recvVoteAbort(const ShardId& shardId) {
    _participantList.recordVoteAbort(shardId);
    return _stateMachine.onEvent(Event::kRecvVoteAbort);
}

void TransactionCoordinator::recvCommitAck(const ShardId& shardId) {
    _participantList.recordCommitAck(shardId);
    if (_participantList.allParticipantsAckedCommit()) {
        _stateMachine.onEvent(Event::kRecvFinalCommitAck);
    }
}

void TransactionCoordinator::recvAbortAck(const ShardId& shardId) {
    _participantList.recordAbortAck(shardId);
    if (_participantList.allParticipantsAckedAbort()) {
        _stateMachine.onEvent(Event::kRecvFinalAbortAck);
    }
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
        {State::kWaitingForParticipantList, {
            {Event::kRecvVoteAbort,         {Action::kSendAbort, State::kWaitingForAbortAcks}},
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvParticipantList,   {State::kWaitingForVotes}},
        }},
        {State::kWaitingForVotes, {
            {Event::kRecvVoteAbort,         {Action::kSendAbort, State::kWaitingForAbortAcks}},
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvFinalVoteCommit,   {Action::kSendCommit, State::kWaitingForCommitAcks}},
        }},
        {State::kWaitingForAbortAcks, {
            {Event::kRecvVoteAbort,         {}},
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvFinalAbortAck,     {State::kAborted}},
        }},
        {State::kAborted, {
            {Event::kRecvVoteAbort,         {}},
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvFinalAbortAck,     {}},
        }},
        {State::kWaitingForCommitAcks, {
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvFinalVoteCommit,   {Action::kSendCommit}},
            {Event::kRecvFinalCommitAck,    {State::kCommitted}},
        }},
        {State::kCommitted, {
            {Event::kRecvVoteCommit,        {}},
            {Event::kRecvParticipantList,   {}},
            {Event::kRecvFinalVoteCommit,   {}},
            {Event::kRecvFinalCommitAck,    {}},
        }},
        {State::kBroken, {}},
        // clang-format on
};

Action TransactionCoordinator::StateMachine::onEvent(Event event) {
    const auto legalTransitions = transitionTable.find(_state)->second;
    if (!legalTransitions.count(event)) {
        std::string errmsg = str::stream() << "Transaction coordinator received illegal event '"
                                           << event << "' while in state '" << _state << "'";
        _state = State::kBroken;
        uasserted(ErrorCodes::InternalError, errmsg);
    }

    const auto transition = legalTransitions.find(event)->second;
    if (transition.nextState) {
        _state = *transition.nextState;
    }
    return transition.action;
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
                                                               int prepareTimestamp) {
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
                              << prepareTimestamp
                              << " from participant "
                              << shardId.toString()
                              << " that previously reported prepareTimestamp "
                              << participant.prepareTimestamp,
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

    if (participant.vote == Participant::Vote::kUnknown) {
        participant.vote = Participant::Vote::kAbort;
        participant.ack = Participant::Ack::kAbort;
    }
}

void TransactionCoordinator::ParticipantList::recordCommitAck(const ShardId& shardId) {
    auto it = _participants.find(shardId);
    uassert(
        ErrorCodes::InternalError,
        str::stream() << "Transaction commit coordinator processed 'commit' ack from participant "
                      << shardId.toString()
                      << " not in participant list",
        it != _participants.end());
    it->second.ack = Participant::Ack::kCommit;
}

void TransactionCoordinator::ParticipantList::recordAbortAck(const ShardId& shardId) {
    auto it = _participants.find(shardId);
    uassert(
        ErrorCodes::InternalError,
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

std::set<ShardId> TransactionCoordinator::ParticipantList::getNonAckedAbortParticipants() const {
    std::set<ShardId> nonAckedAbortParticipants;
    for (const auto& kv : _participants) {
        if (kv.second.ack != Participant::Ack::kAbort) {
            invariant(kv.second.ack == Participant::Ack::kNone);
            nonAckedAbortParticipants.insert(kv.first);
        }
    }
    return nonAckedAbortParticipants;
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
