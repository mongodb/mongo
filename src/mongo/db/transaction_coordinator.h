/*
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

#pragma once

#include <boost/optional.hpp>
#include <map>
#include <set>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/timestamp.h"
#include "mongo/s/shard_id.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class OperationContext;
class Session;

/**
 * A state machine that coordinates a distributed transaction commit with the transaction
 * participants.
 */
class TransactionCoordinator {
    MONGO_DISALLOW_COPYING(TransactionCoordinator);

public:
    TransactionCoordinator() = default;
    ~TransactionCoordinator() = default;

    /**
     * The internal state machine, or "brain", used by the TransactionCoordinator to determine what
     * to do in response to an "event" (receiving a request or hearing back a response).
     */
    class StateMachine {
        friend class TransactionCoordinator;

    public:
        enum class State {
            kWaitingForParticipantList,
            kWaitingForVotes,
            kAborted,
            kWaitingForCommitAcks,
            kCommitted,

            // The state machine transitions to this state when a message that is considered illegal
            // to receive in a particular state is received. This indicates either a byzantine
            // message, or that the transition table does not accurately reflect an asynchronous
            // network.
            kBroken,
        };

        // State machine inputs
        enum class Event {
            kRecvVoteAbort,
            kRecvVoteCommit,
            kRecvParticipantList,
            kRecvFinalVoteCommit,
            kRecvFinalCommitAck,
            kRecvTryAbort,
        };

        // State machine outputs
        enum class Action { kNone, kSendCommit, kSendAbort };

        Action onEvent(Event e);

        State state() const {
            return _state;
        }

    private:
        struct Transition {
            Transition(Action action, State nextState) : action(action), nextState(nextState) {}
            Transition(State nextState) : Transition(Action::kNone, nextState) {}
            Transition(Action action) : action(action) {}
            Transition() : Transition(Action::kNone) {}

            Action action;
            boost::optional<State> nextState;
        };

        static const std::map<State, std::map<Event, Transition>> transitionTable;
        State _state{State::kWaitingForParticipantList};
    };

    /**
     * The coordinateCommit command contains the full participant list that this node is responsible
     * for coordinating the commit across.
     *
     * Stores the participant list.
     *
     * Throws if any participants that this node has already heard a vote from are not in the list.
     */
    StateMachine::Action recvCoordinateCommit(const std::set<ShardId>& participants);

    /**
     * A participant sends a voteCommit command with its prepareTimestamp if it succeeded in
     * preparing the transaction.
     *
     * Stores the participant's vote.
     *
     * Throws if the full participant list has been received and this shard is not one of the
     * participants.
     */
    StateMachine::Action recvVoteCommit(const ShardId& shardId, Timestamp prepareTimestamp);

    /**
     * A participant sends a voteAbort command if it failed to prepare the transaction.
     *
     * Stores the participant's vote and causes the coordinator to decide to abort the transaction.
     *
     * Throws if the full participant list has been received and this shard is not one of the
     * participants.
     */
    StateMachine::Action recvVoteAbort(const ShardId& shardId);

    /**
     * A tryAbort event is received by the coordinator when a transaction is implicitly aborted when
     * a new transaction is received for the same session with a higher transaction number.
     */
    StateMachine::Action recvTryAbort();

    /**
     * Marks this participant as having completed committing the transaction.
     */
    void recvCommitAck(const ShardId& shardId);

    std::set<ShardId> getNonAckedCommitParticipants() const {
        return _participantList.getNonAckedCommitParticipants();
    }

    std::set<ShardId> getNonVotedAbortParticipants() const {
        return _participantList.getNonVotedAbortParticipants();
    }

    Timestamp getCommitTimestamp() const {
        return _participantList.getHighestPrepareTimestamp();
    }

    StateMachine::State state() const {
        return _stateMachine.state();
    }

    class ParticipantList {
    public:
        void recordFullList(const std::set<ShardId>& participants);
        void recordVoteCommit(const ShardId& shardId, Timestamp prepareTimestamp);
        void recordVoteAbort(const ShardId& shardId);
        void recordCommitAck(const ShardId& shardId);
        void recordAbortAck(const ShardId& shardId);

        bool allParticipantsVotedCommit() const;
        bool allParticipantsAckedAbort() const;
        bool allParticipantsAckedCommit() const;

        Timestamp getHighestPrepareTimestamp() const;

        std::set<ShardId> getNonAckedCommitParticipants() const;
        std::set<ShardId> getNonVotedAbortParticipants() const;

        class Participant {
        public:
            enum class Vote { kUnknown, kAbort, kCommit };
            enum class Ack { kNone, kCommit };

            Vote vote{Vote::kUnknown};
            Ack ack{Ack::kNone};
            boost::optional<Timestamp> prepareTimestamp{boost::none};
        };

    private:
        void _recordParticipant(const ShardId& shardId);
        void _validate(const std::set<ShardId>& participants);

        bool _fullListReceived{false};
        std::map<ShardId, Participant> _participants;
    };

private:
    stdx::mutex _mutex;
    ParticipantList _participantList;
    StateMachine _stateMachine;
};

inline StringBuilder& operator<<(StringBuilder& sb,
                                 const TransactionCoordinator::StateMachine::State& state) {
    using State = TransactionCoordinator::StateMachine::State;
    switch (state) {
        // clang-format off
        case State::kWaitingForParticipantList:     return sb << "kWaitingForParticipantlist";
        case State::kWaitingForVotes:               return sb << "kWaitingForVotes";
        case State::kAborted:                       return sb << "kAborted";
        case State::kWaitingForCommitAcks:          return sb << "kWaitingForCommitAcks";
        case State::kCommitted:                     return sb << "kCommitted";
        case State::kBroken:                        return sb << "kBroken";
        // clang-format on
        default:
            MONGO_UNREACHABLE;
    };
}

inline std::ostream& operator<<(std::ostream& os,
                                const TransactionCoordinator::StateMachine::State& state) {
    StringBuilder sb;
    sb << state;
    return os << sb.str();
}

inline StringBuilder& operator<<(StringBuilder& sb,
                                 const TransactionCoordinator::StateMachine::Event& event) {
    using Event = TransactionCoordinator::StateMachine::Event;
    switch (event) {
        // clang-format off
        case Event::kRecvVoteAbort:         return sb << "kRecvVoteAbort";
        case Event::kRecvVoteCommit:        return sb << "kRecvVoteCommit";
        case Event::kRecvParticipantList:   return sb << "kRecvParticipantList";
        case Event::kRecvFinalVoteCommit:   return sb << "kRecvFinalVoteCommit";
        case Event::kRecvFinalCommitAck:    return sb << "kRecvFinalCommitAck";
        // clang-format on
        default:
            MONGO_UNREACHABLE;
    };
}

inline std::ostream& operator<<(std::ostream& os,
                                const TransactionCoordinator::StateMachine::Event& event) {
    StringBuilder sb;
    sb << event;
    return os << sb.str();
}

}  // namespace mongo
