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
#pragma once

#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/str.h"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth
namespace mongo {
namespace process_health {

/**
 * Overview
 * --------
 * `StateMachine<InputMessage, State>` implements a state machine, where states are a subset of
 * `State` values, and transitions between states are driven by the receipt of
 * `boost::optional<InputMessage>` values.
 *
 * Associated with each state is a message handler. When the state machine `accept`s a message,
 * the message handler of the current state is invoked. The message handler returns a
 * `boost::optional<State>` indicating to which state, if any, the machine will transition as a
 * result of accepting the message. If the message handler returns `boost::none`, then the state
 * machine remains in the same state.
 *
 * The set of legal state transitions (e.g. "`A` can transition into `A` and `C` only) is
 * configured by the
 * `void StateMachine::validTransitions(const TransitionsContainer& transitions)` member
 * function.
 *
 * When the state machine undergoes a state transition on account of an accepted message,
 * optionally registered state transition callbacks are invoked. Associated with each state is a
 * set of callbacks invoked when the state machine transitions out of that state, and another
 * set of callbacks invoked when the state machine transitions into that state. Note that a
 * state can transition into itself if its message handler returns the current state, and this
 * is distinct from returning `boost::none`, which indicates that the state machine remain in
 * the same state but invoke no state transition callbacks.
 *
 * For example, in the diagram below, the state machine is in state `C`. When
 * `State StateMachine::accept(const OptionalMessageType&)` is invoked, the message handler
 * registered with state `C` will be invoked.
 *
 * - If the handler returns `boost::none`, then the state machine remains in state `C` and no
 *   transition callbacks are invoked.
 * - If the handler returns `C`, then the state machine transitions from state `C` into state
 *   `C` (itself). If any exit callbacks were registered with state `C`, then they are invoked.
 *   Then, if any enter callbacks were registered with state `C`, they are invoked.
 * - If instead the handler returns `B`, then the state machine transitions from state `C` into
 *   state `B`. `C`'s exit callbacks are invoked, if any, and then `B`'s enter callbacks are
 *   invoked, if any.
 * - If instead the handler returns `A`, etc.
 * - If instead the handler returns any other value, then the state machine has encountered an
 *   illegal state transition. Illegal state transitions will trigger a failure within a test
 *   suite, but have no effect outside of testing.
 *
 * Whichever state the state machine ends up in, the message handler registered with that state
 * will be invoked the next time `accept` is invoked on the state machine.
 *
 * A state machine is constructed with an initial state. If any enter callbacks are associated
 * with that state, then they will be invoked as soon as the state machine is `start`ed (see the
 * arrow entering `D` from above in the diagram).
 *
 *                                   │
 *                                   │ onEnter(D)
 *                                   ▼
 * ┌─────────────┐  onExit(D)      ┌─────────────────┐
 * │      B      │  onEnter(B)     │        D        │
 * │             │ ◀────────────── │ (initial state) │
 * └─────────────┘                 └─────────────────┘
 *   ▲                               │
 *   │ onExit(C)                     │ onExit(D)
 *   │ onEnter(B)                    │ onEnter(C)
 *   │                               ▼
 *   │                             ┌────────────────────────────────┐   onExit(C)
 *   │                             │                                │   onEnter(C)
 *   │                             │               C                │ ─────────────┐
 *   │                             │        (current state)         │              │
 *   └──────────────────────────── │                                │ ◀────────────┘
 *                                 └────────────────────────────────┘
 *                                   │                  ▲
 *                                   │ onExit(C)        │ onExit(A)
 *                                   │ onEnter(A)       │ onEnter(C)
 *                                   ▼                  │
 *                    onExit(A)    ┌─────────────────┐  │
 *                    onEnter(A)   │                 │  │
 *                  ┌───────────── │        A        │  │
 *                  │              │                 │  │
 *                  └────────────▶ │                 │ ─┘
 *                                 └─────────────────┘
 *
 * When registering a state's message handler, the state may be marked "transient." A transient
 * state's message handler is invoked when the state machine transitions into the state, in
 * addition to when a message is `accept`ed while in the state.
 *
 * For example, in the diagram above, if `A` were marked transient, then after a message
 * accepted in state `C` causes a transition into state `A`, `A`'s message handler would be
 * invoked with the same message. `A`'s message handler will be invoked recursively until either
 * `A`'s message handler's return value indicates a different state, or is `boost::none`. The
 * idea is to transition out of the transient state, hence the name.
 *
 * By default, states are not marked "transient."
 *
 * Intended Usage
 * --------------
 * `StateMachine` has a two stage lifecycle: "before `start`" and "after `start`." Intended
 * usage is the following:
 *
 * - Construct a `StateMachine<InputMessage, State>`, specifying its initial `State`.
 * - Configure the legal state transitions by calling `validTransitions` with a table of
 *   adjacency lists.
 * - Configure state message handlers and enter/exit callbacks by calling `registerHandler`
 *   for each state. Use the following "fluent" syntax:
 *   ```
 *   stateMachine.registerHandler(State::A, onMessageInStateA)
 *       ->enter(onEnterStateACallback1)
 *       ->enter(onEnterStateACallback2)
 *       // ...
 *       ->exit(onExitStateACallback1)
 *       // ...
 *       ->exit(onExitStateACallbackN);
 *   ```
 * - Call `void StateMachine::start()`. This will transition the state machine into its initial
 *   state. It might invoke enter callbacks, but will not invoke any message handlers or exit
 *   callbacks.
 *   Now that the state machine is started, `registerHandler` and `validTransitions` cannot be
 *   called again.
 * - The following two operations can be called arbitrarily many times in any order.
 * - Call `State StateMachine::accept(const OptionalMessageType&)`. It returns the resulting
 *   `State`.
 * - Query the current state by calling `State StateMachine::state() const`.
 * - ...
 * - Destroy the `StateMachine`.
 *
 * Concurrency
 * -----------
 * `StateMachine` owns a `stdx::recursive_mutex` that it holds a lock on throughout
 * `accept(...)` and `state()`. This has implications for multi-threaded use of `StateMachine`.
 * See the comment above the definition of `accept(...)`.
 *
 */

template <class InputMessage, typename State>
class StateMachine {
public:
    using StateMachineType = StateMachine<InputMessage, State>;
    using MessageType = InputMessage;
    using OptionalMessageType = boost::optional<InputMessage>;

    // handlers accept oldState, newState, and input message as parameters.
    // Should not throw exceptions.
    using StateCallback = unique_function<void(State, State, const OptionalMessageType&)>;

    // State machine accepts InputMessage and optionally transitions to state in the return value
    using MessageHandler = unique_function<boost::optional<State>(const OptionalMessageType&)>;

    using TransitionsContainer = stdx::unordered_map<State, std::vector<State>>;

    StateMachine() = delete;
    StateMachine(const StateMachineType&) = delete;
    StateMachineType& operator=(const StateMachineType&) = delete;

    StateMachine(State initialState)
        : _started(false), _initial(initialState), _current(nullptr) {};

    StateMachine(StateMachineType&& sm) {
        *this = std::move(sm);
    };

    StateMachine& operator=(StateMachineType&& other) {
        if (this != &other) {
            _started = other._started;
            _initial = other._initial;
            _states = std::move(other._states);

            if (other._current) {
                _current = &_states.at(other._current->state());
            }

            other._started = false;
            other._current = nullptr;
        }
        return *this;
    }

    void tassertNotStarted() const {
        tassert(
            5936505, "operation cannot be performed after the state machine is started", !_started);
    }

    void tassertStarted() const {
        tassert(
            5936508, "operation cannot be performed before the state machine is started", _started);
    }

    // Transitions the state machine into the initial state.
    // Can only be called once.
    void start() {
        stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
        tassertNotStarted();
        _started = true;

        auto& initialState = getContextOrFatal(_initial);
        _current = &initialState;
        auto& handler = initialState.stateHandler;
        if (handler)
            handler->fireEnter(_current->state(), boost::none);
    }

    // Define a valid transition.
    // Must be called prior to starting the state machine.
    void validTransition(State from, State to) {
        try {
            stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
            tassertNotStarted();
            auto& context = _states[from];
            context.validTransitions.insert(to);
        } catch (const std::exception& e) {
            LOGV2_FATAL(9894900,
                        "Terminating process on invalid state transition",
                        "except"_attr = e.what());
        }
    }

    // Define valid transitions.
    // Must be called prior to starting the state machine.
    void validTransitions(const TransitionsContainer& transitions) {
        for (const auto& [from, toStates] : transitions) {
            for (auto to : toStates) {
                validTransition(from, to);
            }
        }
    }

    // Accept message m, transition the state machine, and return the resulting state.
    // Upon the transition to the new state the state machine will call any registered hooks.
    //
    // In order to avoid deadlock while calling this function, authors should ensure
    // that:
    //	1. A recursive call only occurs from the current thread; or
    //	2. For any hooks run as a result of accepting this message, no blocking calls are made
    // involving shared resources with another thread that may call this function.
    State accept(const OptionalMessageType& m) {
        stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
        tassertStarted();

        auto& handler = _current->stateHandler;

        auto result = handler->accept(m);
        if (result) {
            setState(*result, m);
        }
        return _current->state();
    }

    // Return the current state.
    State state() const {
        stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
        tassertStarted();
        invariant(_current);
        return _current->state();
    }

    // Allows registering multiple callbacks through chained calls to enter/exit.
    class StateEventRegistry {
    public:
        virtual StateEventRegistry* enter(StateCallback&& cb) = 0;
        virtual StateEventRegistry* exit(StateCallback&& cb) = 0;
    };
    using StateEventRegistryPtr = StateEventRegistry*;


    // Defines the transition function for each state and maintains the list of callbacks
    // used when a state is entered or exited.
    class StateHandler : public StateEventRegistry {
    public:
        StateHandler() = delete;
        StateHandler(State state) : _state(state) {}
        virtual ~StateHandler() {}

        // Accepts input message m when state machine is in state _state. Optionally, the
        // state machine transitions to the state specified in the return value. Entry and exit
        // hooks will not fire if this method returns boost::none.
        virtual boost::optional<State> accept(const OptionalMessageType& message) = 0;

        // The state this handler is defined for
        State state() const {
            return _state;
        }

        StateEventRegistryPtr enter(StateCallback&& cb) override {
            _onEnter.push_back(std::move(cb));
            return this;
        }

        void fireEnter(State previous, const OptionalMessageType& m) {
            for (auto& cb : _onEnter)
                try {
                    cb(previous, _state, m);
                } catch (const std::exception& e) {
                    LOGV2_FATAL(9894901,
                                "Process health transition handler threw during execution",
                                "except"_attr = e.what());
                }
        }

        StateEventRegistryPtr exit(StateCallback&& cb) override {
            _onExit.push_back(std::move(cb));
            return this;
        }

        void fireExit(State newState, const OptionalMessageType& message) {
            for (auto& cb : _onExit)
                cb(_state, newState, message);
        }

        bool _isTransient = false;

    protected:
        // The state we are handling
        const State _state;

        // Callbacks are called inline when we enter/exit _state
        std::vector<StateCallback> _onEnter;
        std::vector<StateCallback> _onExit;
    };
    using StateHandlerPtr = std::unique_ptr<StateHandler>;


    class LambdaStateHandler : public StateHandler {
    public:
        LambdaStateHandler(State state, MessageHandler&& m)
            : StateHandler(state), _messageHandler(std::move(m)) {}
        ~LambdaStateHandler() override {}

        boost::optional<State> accept(const OptionalMessageType& m) override {
            return _messageHandler(m);
        }

    protected:
        MessageHandler _messageHandler;
    };

    StateEventRegistryPtr registerHandler(StateHandlerPtr handler) {
        stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
        tassertNotStarted();
        auto& context = _states[handler->state()];
        context.stateHandler = std::move(handler);
        return context.stateHandler.get();
    }

    StateEventRegistryPtr registerHandler(State s, MessageHandler&& handler, bool isTransient) {
        stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
        tassertNotStarted();

        auto& context = _states[s];
        context.stateHandler = std::make_unique<LambdaStateHandler>(s, std::move(handler));
        if (isTransient) {
            context.stateHandler->_isTransient = true;
        }

        return context.stateHandler.get();
    }

    StateEventRegistryPtr registerHandler(State s, MessageHandler&& handler) {
        return registerHandler(s, std::move(handler), false);
    }

protected:
    struct StateContext {
        StateHandlerPtr stateHandler;
        stdx::unordered_set<State> validTransitions;
        State state() {
            return stateHandler->state();
        }
    };

    using StateContexts = stdx::unordered_map<State, StateContext>;

    void setState(State s, const OptionalMessageType& message) {
        stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
        tassertStarted();

        invariant(_current);
        auto& previousContext = *_current;

        auto& transitions = previousContext.validTransitions;
        auto it = transitions.find(s);
        tassert(5936506, "invalid state transition", it != transitions.end());

        // in production, an illegal transition is a noop
        if (it == transitions.end())
            return;

        // switch to new state
        _current = &getContextOrFatal(s);

        // fire exit hooks for previous state
        previousContext.stateHandler->fireExit(s, message);

        // fire entry hooks for new state
        _current->stateHandler->fireEnter(previousContext.state(), message);

        if (_current->stateHandler->_isTransient) {
            accept(message);
        }
    }

    StateHandler* getHandlerOrFatal(State s) {
        auto& handler = getContextOrFatal(s).stateHandler;
        invariant(handler, "state handler is not defined");
        return handler.get();
    }

    StateContext& getContextOrFatal(State s) {
        try {
            return _states.at(s);
        } catch (const std::out_of_range& ex) {
            invariant(false, str::stream() << "state context is not defined: " << ex.what());
            MONGO_UNREACHABLE;
        }
    }

    StateEventRegistryPtr on(State s) {
        tassertNotStarted();
        return getHandlerOrFatal(s);
    }

    mutable stdx::recursive_mutex _mutex;
    bool _started;
    State _initial;
    StateContext* _current = nullptr;
    StateContexts _states;
};
}  // namespace process_health
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
