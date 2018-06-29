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
#include <iostream>
#include <map>

#include "mongo/base/disallow_copying.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class Session;

/**
 * A state machine that coordinates a distributed transaction commit with the transaction
 * coordinator.
 */
class TransactionParticipant {
    MONGO_DISALLOW_COPYING(TransactionParticipant);

public:
    TransactionParticipant() = default;
    ~TransactionParticipant() = default;

    static boost::optional<TransactionParticipant>& get(Session* session);
    static void create(Session* session);

    class StateMachine {
    public:
        friend class TransactionParticipant;

        // Note: We must differentiate the 'committed/aborted' and 'committed/aborted after prepare'
        // states, because it is illegal to receive, for example, a prepare request after a
        // commit/abort if no prepare was received before the commit/abort.
        enum class State {
            kUnprepared,
            kAborted,
            kCommitted,
            kWaitingForDecision,
            kAbortedAfterPrepare,
            kCommittedAfterPrepare,

            // The state machine transitions to this state when a message that is considered illegal
            // to receive in a particular state is received. This indicates either a byzantine
            // message, or that the transition table does not accurately reflect an asynchronous
            // network.
            kBroken,
        };

        // State machine inputs
        enum class Event {
            kRecvPrepare,
            kVoteCommitRejected,
            kRecvAbort,
            kRecvCommit,
        };

        // State machine outputs
        enum class Action {
            kNone,
            kPrepare,
            kAbort,
            kCommit,
            kSendCommitAck,
            kSendAbortAck,
        };

        Action onEvent(Event e);

        State state() {
            return _state;
        }

    private:
        struct Transition {
            Transition() : action(Action::kNone) {}
            Transition(Action action) : action(action) {}
            Transition(Action action, State state) : action(action), nextState(state) {}

            Action action;
            boost::optional<State> nextState;
        };

        static const std::map<State, std::map<Event, Transition>> transitionTable;
        State _state{State::kUnprepared};
    };

private:
    StateMachine _stateMachine;
};

inline StringBuilder& operator<<(StringBuilder& sb,
                                 const TransactionParticipant::StateMachine::State& state) {
    using State = TransactionParticipant::StateMachine::State;
    switch (state) {
        // clang-format off
        case State::kUnprepared:                return sb << "Unprepared";
        case State::kAborted:                   return sb << "Aborted";
        case State::kCommitted:                 return sb << "Committed";
        case State::kWaitingForDecision:        return sb << "WaitingForDecision";
        case State::kAbortedAfterPrepare:       return sb << "AbortedAfterPrepare";
        case State::kCommittedAfterPrepare:     return sb << "CommittedAfterPrepare";
        case State::kBroken:                    return sb << "Broken";
        // clang-format on
        default:
            MONGO_UNREACHABLE;
    };
}

inline std::ostream& operator<<(std::ostream& os,
                                const TransactionParticipant::StateMachine::State& state) {
    StringBuilder sb;
    sb << state;
    return os << sb.str();
}

inline StringBuilder& operator<<(StringBuilder& sb,
                                 const TransactionParticipant::StateMachine::Event& event) {
    using Event = TransactionParticipant::StateMachine::Event;
    switch (event) {
        // clang-format off
        case Event::kRecvPrepare:               return sb << "RecvPrepare";
        case Event::kVoteCommitRejected:        return sb << "VoteCommitRejected";
        case Event::kRecvAbort:                 return sb << "RecvAbort";
        case Event::kRecvCommit:                return sb << "RecvCommit";
        // clang-format on
        default:
            MONGO_UNREACHABLE;
    };
}

inline std::ostream& operator<<(std::ostream& os,
                                const TransactionParticipant::StateMachine::Event& event) {
    StringBuilder sb;
    sb << event;
    return os << sb.str();
}

inline StringBuilder& operator<<(StringBuilder& sb,
                                 const TransactionParticipant::StateMachine::Action& action) {
    using Action = TransactionParticipant::StateMachine::Action;
    switch (action) {
        // clang-format off
        case Action::kNone:                     return sb << "None";
        case Action::kPrepare:                  return sb << "Prepare";
        case Action::kAbort:                    return sb << "Abort";
        case Action::kCommit:                   return sb << "Commit";
        case Action::kSendCommitAck:            return sb << "SendCommitAck";
        case Action::kSendAbortAck:             return sb << "SendAbortAck";
        // clang-format on
        default:
            MONGO_UNREACHABLE;
    };
}

inline std::ostream& operator<<(std::ostream& os,
                                const TransactionParticipant::StateMachine::Action& action) {
    StringBuilder sb;
    sb << action;
    return os << sb.str();
}

}  // namespace mongo
