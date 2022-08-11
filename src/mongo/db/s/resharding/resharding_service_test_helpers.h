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

#include "mongo/platform/basic.h"

#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace resharding_service_test_helpers {

/**
 * This contains the logic for pausing/unpausing when a state is reached.
 *
 * Template param StateEnum must have kUnused and kDone.
 */
template <class StateEnum>
class StateTransitionController {
public:
    StateTransitionController() = default;

    void waitUntilStateIsReached(StateEnum state) {
        stdx::unique_lock lk(_mutex);
        _waitUntilUnpausedCond.wait(lk, [this, state] { return _state == state; });
    }

private:
    template <class Enum, class ReshardingDocument>
    friend class StateTransitionControllerOpObserver;

    template <class Enum>
    friend class PauseDuringStateTransitions;

    void _setPauseDuringTransition(StateEnum state) {
        stdx::lock_guard lk(_mutex);
        _pauseDuringTransition.insert(state);
    }

    void _unsetPauseDuringTransition(StateEnum state) {
        stdx::lock_guard lk(_mutex);
        _pauseDuringTransition.erase(state);
        _pauseDuringTransitionCond.notify_all();
    }

    void _notifyNewStateAndWaitUntilUnpaused(OperationContext* opCtx, StateEnum newState) {
        stdx::unique_lock lk(_mutex);
        ScopeGuard guard([this, prevState = _state] { _state = prevState; });
        _state = newState;
        _waitUntilUnpausedCond.notify_all();
        opCtx->waitForConditionOrInterrupt(_pauseDuringTransitionCond, lk, [this, newState] {
            return _pauseDuringTransition.count(newState) == 0;
        });
        guard.dismiss();
    }

    void _resetReachedState() {
        stdx::lock_guard lk(_mutex);
        _state = StateEnum::kUnused;
    }

    Mutex _mutex = MONGO_MAKE_LATCH("StateTransitionController::_mutex");
    stdx::condition_variable _pauseDuringTransitionCond;
    stdx::condition_variable _waitUntilUnpausedCond;

    std::set<StateEnum> _pauseDuringTransition;
    StateEnum _state = StateEnum::kUnused;
};

template <class StateEnum>
class PauseDuringStateTransitions {
public:
    PauseDuringStateTransitions(StateTransitionController<StateEnum>* controller, StateEnum state)
        : PauseDuringStateTransitions<StateEnum>(controller, std::vector<StateEnum>{state}) {}

    PauseDuringStateTransitions(StateTransitionController<StateEnum>* controller,
                                std::vector<StateEnum> states)
        : _controller{controller}, _states{std::move(states)} {
        _controller->_resetReachedState();
        for (auto state : _states) {
            _controller->_setPauseDuringTransition(state);
        }
    }

    ~PauseDuringStateTransitions() {
        for (auto state : _states) {
            _controller->_unsetPauseDuringTransition(state);
        }
    }

    PauseDuringStateTransitions(const PauseDuringStateTransitions&) = delete;
    PauseDuringStateTransitions& operator=(const PauseDuringStateTransitions&) = delete;

    PauseDuringStateTransitions(PauseDuringStateTransitions&&) = delete;
    PauseDuringStateTransitions& operator=(PauseDuringStateTransitions&&) = delete;

    void wait(StateEnum state) {
        _controller->waitUntilStateIsReached(state);
    }

    void unset(StateEnum state) {
        _controller->_unsetPauseDuringTransition(state);
    }

private:
    StateTransitionController<StateEnum>* const _controller;
    const std::vector<StateEnum> _states;
};

template <class StateEnum, class StateDocument>
class StateTransitionControllerOpObserver : public OpObserverNoop {
public:
    using GetStateFunc = std::function<StateEnum(const StateDocument&)>;

    StateTransitionControllerOpObserver(
        std::shared_ptr<StateTransitionController<StateEnum>> controller,
        NamespaceString stateDocumentNss,
        GetStateFunc getStateFunc)
        : _controller{std::move(controller)},
          _stateDocumentNss{std::move(stateDocumentNss)},
          _getState{std::move(getStateFunc)} {}

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override {
        if (coll->ns() != _stateDocumentNss) {
            return;
        }

        auto doc = StateDocument::parse(
            IDLParserContext{"StateTransitionControllerOpObserver::onInserts"}, begin->doc);
        _controller->_notifyNewStateAndWaitUntilUnpaused(opCtx, _getState(doc));
        invariant(++begin == end);  // No support for inserting more than one state document yet.
    }

    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) override {
        if (args.nss != _stateDocumentNss) {
            return;
        }

        auto doc =
            StateDocument::parse(IDLParserContext{"StateTransitionControllerOpObserver::onUpdate"},
                                 args.updateArgs->updatedDoc);
        _controller->_notifyNewStateAndWaitUntilUnpaused(opCtx, _getState(doc));
    }

    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const UUID& uuid,
                  StmtId stmtId,
                  const OplogDeleteEntryArgs& args) override {
        if (nss != _stateDocumentNss) {
            return;
        }

        _controller->_notifyNewStateAndWaitUntilUnpaused(opCtx, StateEnum::kDone);
    }

private:
    std::shared_ptr<StateTransitionController<StateEnum>> _controller;
    const NamespaceString _stateDocumentNss;
    GetStateFunc _getState;
};

}  // namespace resharding_service_test_helpers
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
