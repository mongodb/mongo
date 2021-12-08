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

#include <vector>

#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/functional.h"
#include "mongo/util/str.h"

namespace mongo {
namespace process_health {

// Note: this class provides no internal synchronization. If used in a multithreaded context callers
// must provide their own concurrency control.
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

    StateMachine(State initialState) : _started(false), _initial(initialState), _current(nullptr){};

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
    void validTransition(State from, State to) noexcept {
        stdx::lock_guard<stdx::recursive_mutex> lk(_mutex);
        tassertNotStarted();
        auto& context = _states[from];
        context.validTransitions.insert(to);
    }

    // Define valid transitions.
    // Must be called prior to starting the state machine.
    void validTransitions(const TransitionsContainer& transitions) noexcept {
        for (auto [from, toStates] : transitions) {
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
        virtual boost::optional<State> accept(const OptionalMessageType& message) noexcept = 0;

        // The state this handler is defined for
        State state() const {
            return _state;
        }

        StateEventRegistryPtr enter(StateCallback&& cb) {
            _onEnter.push_back(std::move(cb));
            return this;
        }

        void fireEnter(State previous, const OptionalMessageType& m) noexcept {
            for (auto& cb : _onEnter)
                cb(previous, _state, m);
        }

        StateEventRegistryPtr exit(StateCallback&& cb) {
            _onExit.push_back(std::move(cb));
            return this;
        }

        void fireExit(State newState, const OptionalMessageType& message) noexcept {
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

        boost::optional<State> accept(const OptionalMessageType& m) noexcept override {
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
