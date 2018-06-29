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

#include "mongo/db/transaction_participant.h"

#include "mongo/db/session.h"

namespace mongo {

using Action = TransactionParticipant::StateMachine::Action;
using Event = TransactionParticipant::StateMachine::Event;
using State = TransactionParticipant::StateMachine::State;

namespace {
const Session::Decoration<boost::optional<TransactionParticipant>> getTransactionParticipant =
    Session::declareDecoration<boost::optional<TransactionParticipant>>();
}  // namespace

boost::optional<TransactionParticipant>& TransactionParticipant::get(Session* session) {
    return getTransactionParticipant(session);
}

void TransactionParticipant::create(Session* session) {
    invariant(!getTransactionParticipant(session));
    getTransactionParticipant(session).emplace();
}

//
// StateMachine
//

/**
 * This table shows the events that are legal to occur (given an asynchronous network) while in each
 * state.
 *
 * For each legal event, it shows the associated action (if any) the participant should take, and
 * the next state the participant should transition to.
 *
 * Empty ("{}") transitions mean "legal event, but no action to take and no new state to transition
 * to.
 * Missing transitions are illegal.
 */
const std::map<State, std::map<Event, TransactionParticipant::StateMachine::Transition>>
    TransactionParticipant::StateMachine::transitionTable = {
        // clang-format off
        {State::kUnprepared, {
            {Event::kRecvPrepare,           {Action::kPrepare, State::kWaitingForDecision}},
            {Event::kRecvCommit,            {Action::kCommit, State::kCommitted}},
            {Event::kRecvAbort,             {Action::kAbort, State::kAborted}},
        }},
        {State::kAborted, {
            {Event::kRecvAbort,             {}},
        }},
        {State::kCommitted, {
            {Event::kRecvCommit,            {}},
        }},
        {State::kWaitingForDecision, {
            {Event::kRecvPrepare,           {}},
            {Event::kVoteCommitRejected,    {Action::kAbort, State::kAbortedAfterPrepare}},
            {Event::kRecvCommit,            {Action::kCommit, State::kCommittedAfterPrepare}},
            {Event::kRecvAbort,             {Action::kAbort, State::kAbortedAfterPrepare}},
        }},
        {State::kAbortedAfterPrepare, {
            {Event::kRecvPrepare,           {}},
            {Event::kVoteCommitRejected,    {}},
            {Event::kRecvAbort,             {}},
        }},
        {State::kCommittedAfterPrepare, {
            {Event::kRecvPrepare,           {}},
            {Event::kRecvCommit,            {}},
        }},
        {State::kBroken, {}},
        // clang-format on
};

Action TransactionParticipant::StateMachine::onEvent(Event event) {
    const auto legalTransitions = transitionTable.find(_state)->second;
    if (!legalTransitions.count(event)) {
        _state = State::kBroken;
        uasserted(ErrorCodes::InternalError,
                  str::stream() << "Transaction participant received illegal event '" << event
                                << "' while in state '"
                                << _state
                                << "'");
    }

    const auto transition = legalTransitions.find(event)->second;
    if (transition.nextState) {
        _state = *transition.nextState;
    }
    return transition.action;
}

}  // namespace mongo
