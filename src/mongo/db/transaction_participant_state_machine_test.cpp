/**
 *    Copyright (C) 2018 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <deque>

#include "mongo/db/transaction_participant.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::deque;
using std::map;

using StateMachine = TransactionParticipant::StateMachine;
using State = StateMachine::State;
using Event = StateMachine::Event;
using Action = StateMachine::Action;

using Schedule = deque<Event>;

void runSchedule(StateMachine& participant, Schedule& schedule) {
    while (!schedule.empty()) {
        participant.onEvent(schedule.front());
        schedule.pop_front();
    }
}

void expectScheduleSucceeds(Schedule schedule, State expectedEndState) {
    StateMachine participant;
    runSchedule(participant, schedule);
    ASSERT_EQ(expectedEndState, participant.state());
}

void expectScheduleThrows(Schedule schedule) {
    StateMachine participant;
    ASSERT_THROWS(runSchedule(participant, schedule), AssertionException);
    ASSERT_EQ(State::kBroken, participant.state());
}

TEST(ParticipantStateMachine, DirectCommitSucceeds) {
    expectScheduleSucceeds({Event::kRecvCommit}, State::kCommitted);
}

TEST(ParticipantStateMachine, TwoPhaseCommitSucceeds) {
    expectScheduleSucceeds({Event::kRecvPrepare, Event::kRecvCommit},
                           State::kCommittedAfterPrepare);
}

TEST(ParticipantStateMachine, DirectAbortSucceeds) {
    expectScheduleSucceeds({Event::kRecvAbort}, State::kAborted);
}

TEST(ParticipantStateMachine, TwoPhaseAbortSucceeds) {
    expectScheduleSucceeds({Event::kRecvPrepare, Event::kRecvAbort}, State::kAbortedAfterPrepare);
}

TEST(ParticipantStateMachine, PrepareAfterDirectAbortThrows) {
    expectScheduleThrows({Event::kRecvAbort, Event::kRecvPrepare});
}

TEST(ParticipantStateMachine, PrepareAfterDirectCommitThrows) {
    expectScheduleThrows({Event::kRecvCommit, Event::kRecvPrepare});
}

TEST(ParticipantStateMachine, CommitAfterAbortThrows) {
    expectScheduleThrows({Event::kRecvCommit, Event::kRecvAbort});
    expectScheduleThrows({Event::kRecvPrepare, Event::kRecvCommit, Event::kRecvAbort});
}

TEST(ParticipantStateMachine, AbortAfterCommitThrows) {
    expectScheduleThrows({Event::kRecvAbort, Event::kRecvCommit});
    expectScheduleThrows({Event::kRecvPrepare, Event::kRecvAbort, Event::kRecvCommit});
}

}  // namespace mongo
