/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/process_health/state_machine.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace process_health {

class StateMachineTestFixture : public unittest::Test {
public:
    enum class MachineState { A, B, C, NoHandlerDefined };
    static constexpr MachineState kInitialState = MachineState::A;
    struct Message {
        boost::optional<MachineState> nextState;
    };


    using SM = StateMachine<Message, MachineState>;
    class StateMachineTest : public SM {
        using SM::StateMachine;

    public:
        StateEventRegistryPtr on(MachineState s) {
            return SM::on(s);
        }
    };

    class NextStateHandler : public SM::StateHandler {
    public:
        using StateMachineTest::StateHandler::StateHandler;

        boost::optional<MachineState> accept(const SM::OptionalMessageType& m) noexcept override {
            return m->nextState;
        }
    };

    StateMachineTest* subject() {
        return _sm.get();
    }

    void setUp() override {
        _sm = std::make_unique<StateMachineTest>(kInitialState);
        auto allStates =
            std::vector<MachineState>{MachineState::A, MachineState::B, MachineState::C};
        for (MachineState state : allStates) {
            _sm->registerHandler(std::make_unique<NextStateHandler>(state));
        }
        _sm->validTransitions(
            {{MachineState::A, {MachineState::B}}, {MachineState::B, {MachineState::A}}});
    }

    void tearDown() override {}

protected:
    std::unique_ptr<StateMachineTest> _sm;
};


TEST_F(StateMachineTestFixture, RegistersMessageHandlerAndStateHooksFluently) {
    int cnt = 0;
    auto hook = [&cnt]() {
        return
            [&cnt](MachineState oldState, MachineState newState, const SM::OptionalMessageType& m) {
                cnt++;
            };
    };

    subject()
        ->registerHandler(MachineState::B,
                          [](const SM::OptionalMessageType& m) { return m->nextState; })
        // hooks registerd for state B
        ->enter(hook())
        ->exit(hook());

    subject()->start();
    subject()->accept(Message{MachineState::B});
    ASSERT_EQUALS(1, cnt);
    subject()->accept(Message{MachineState::A});
    ASSERT_EQUALS(2, cnt);
}

TEST_F(StateMachineTestFixture, InitialStateCorrect) {
    subject()->start();
    ASSERT_TRUE(subject()->state() == kInitialState);
}

TEST_F(StateMachineTestFixture, ValidTransition) {
    subject()->start();
    subject()->accept(Message{MachineState::B});
    ASSERT_TRUE(subject()->state() == MachineState::B);
}

TEST_F(StateMachineTestFixture, FiresEnterTransitionHooks) {
    int cnt = 0;
    SM::OptionalMessageType message;
    auto hook = [&cnt, &message](MachineState oldState,
                                 MachineState newState,
                                 const SM::OptionalMessageType& m) {
        ASSERT_TRUE(MachineState::A == oldState);
        ASSERT_TRUE(MachineState::B == newState);
        cnt++;
        message = m;
    };
    subject()->on(MachineState::B)->enter(hook);
    subject()->start();
    subject()->accept(Message{MachineState::B});
    ASSERT_TRUE(MachineState::B == subject()->state());
    ASSERT_EQUALS(1, cnt);
    ASSERT(MachineState::B == message->nextState);
}

TEST_F(StateMachineTestFixture, DoesNotFireHooksOrTransitionIfAcceptReturnsNone) {
    int cnt = 0;
    auto hook = [&cnt]() {
        return
            [&cnt](MachineState oldState, MachineState newState, const SM::OptionalMessageType& m) {
                cnt++;
            };
    };
    subject()->on(MachineState::A)->enter(hook())->exit(hook());
    subject()->on(MachineState::B)->enter(hook())->exit(hook());
    subject()->start();

    // enter A
    ASSERT_EQUALS(1, cnt);

    subject()->accept(Message{boost::none});

    // no additional transition or hooks called
    ASSERT_TRUE(MachineState::A == subject()->state());
    ASSERT_EQUALS(1, cnt);
}

TEST_F(StateMachineTestFixture, FiresExitTransitionHooks) {
    int cnt = 0;
    SM::OptionalMessageType message;
    auto hook = [&cnt, &message](MachineState oldState,
                                 MachineState newState,
                                 const SM::OptionalMessageType& m) {
        ASSERT_TRUE(MachineState::A == oldState);
        ASSERT_TRUE(MachineState::B == newState);
        cnt++;
        message = m;
    };
    subject()->on(MachineState::A)->exit(hook);
    subject()->start();
    subject()->accept(Message{MachineState::B});
    ASSERT_TRUE(MachineState::B == subject()->state());
    ASSERT_EQUALS(1, cnt);
    ASSERT(MachineState::B == message->nextState);
}

TEST_F(StateMachineTestFixture, FiresInitialTransitionHooks) {
    int cnt = 0;
    SM::OptionalMessageType message;
    auto hook = [&cnt, &message](MachineState oldState,
                                 MachineState newState,
                                 const SM::OptionalMessageType& m) {
        ASSERT_TRUE(MachineState::A == oldState);
        ASSERT_TRUE(MachineState::A == newState);
        cnt++;
        message = m;
    };
    subject()->on(MachineState::A)->enter(hook);
    subject()->start();
    ASSERT_TRUE(MachineState::A == subject()->state());
    ASSERT_EQUALS(1, cnt);
    ASSERT(boost::none == message);
}

TEST_F(StateMachineTestFixture, DoesNotTransitionStateOrFireHooksIfAcceptReturnsNone) {
    auto hook = [](MachineState oldState, MachineState newState, const SM::OptionalMessageType& m) {
        ASSERT(false);
    };
    subject()->on(MachineState::B)->exit(hook);
    subject()->start();
    subject()->accept(Message{MachineState::B});
    subject()->accept(Message{});
    ASSERT_TRUE(MachineState::B == subject()->state());
}

TEST_F(StateMachineTestFixture, MoveWorks) {
    StateMachineTest stateMachine(MachineState::A);
    stateMachine.validTransition(MachineState::A, MachineState::B);
    stateMachine.registerHandler(std::make_unique<NextStateHandler>(MachineState::A));
    stateMachine.registerHandler(std::make_unique<NextStateHandler>(MachineState::B));

    int cnt = 0;
    auto hook = [&cnt](MachineState oldState,
                       MachineState newState,
                       const SM::OptionalMessageType& m) { cnt++; };
    stateMachine.on(MachineState::B)->enter(hook);

    StateMachineTest stateMachine2 = std::move(stateMachine);
    stateMachine2.start();
    stateMachine2.accept(Message{MachineState::B});
    ASSERT_EQUALS(1, cnt);
    ASSERT_TRUE(MachineState::B == stateMachine2.state());
}

DEATH_TEST_F(StateMachineTestFixture, CrashIfHooksAreRegisteredAfterStart, "5936505") {
    auto hook = []() {
        return
            [](MachineState oldState, MachineState newState, const SM::OptionalMessageType& m) {};
    };
    subject()->start();
    subject()->on(MachineState::B)->enter(hook())->exit(hook());
}

DEATH_TEST_F(StateMachineTestFixture, CrashOnInvalidTransition, "5936506") {
    subject()->start();
    subject()->accept(Message{MachineState::C});
}

DEATH_TEST_F(StateMachineTestFixture, CrashOnUndefinedHandler, "invariant") {
    auto hook = [](MachineState oldState, MachineState newState, const SM::OptionalMessageType& m) {
    };
    subject()->on(MachineState::NoHandlerDefined)->enter(hook);
    subject()->start();
}

DEATH_TEST_F(StateMachineTestFixture, CrashIfHookThrowsException, "fatal") {
    auto hook = [](MachineState oldState, MachineState newState, const SM::OptionalMessageType& m) {
        throw std::runtime_error("exception");
    };
    subject()->on(MachineState::A)->enter(hook);
    subject()->start();
}
}  // namespace process_health
}  // namespace mongo
