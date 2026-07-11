// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"

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
        std::unique_lock lk(_mutex);
        _waitUntilUnpausedCond.wait(lk, [this, state] { return _state == state; });
    }

private:
    template <class Enum, class ReshardingDocument>
    friend class StateTransitionControllerOpObserver;

    template <class Enum>
    friend class PauseDuringStateTransitions;

    void _setPauseDuringTransition(StateEnum state) {
        std::lock_guard lk(_mutex);
        _pauseDuringTransition.insert(state);
    }

    void _unsetPauseDuringTransition(StateEnum state) {
        std::lock_guard lk(_mutex);
        _pauseDuringTransition.erase(state);
        _pauseDuringTransitionCond.notify_all();
    }

    void _notifyNewStateAndWaitUntilUnpaused(OperationContext* opCtx, StateEnum newState) {
        std::unique_lock lk(_mutex);
        ScopeGuard guard([this, prevState = _state] { _state = prevState; });
        _state = newState;
        _waitUntilUnpausedCond.notify_all();
        opCtx->waitForConditionOrInterrupt(_pauseDuringTransitionCond, lk, [this, newState] {
            return _pauseDuringTransition.count(newState) == 0;
        });
        guard.dismiss();
    }

    void _resetReachedState() {
        std::lock_guard lk(_mutex);
        _state = StateEnum::kUnused;
    }

    std::mutex _mutex;
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
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override {
        if (coll->ns() != _stateDocumentNss) {
            return;
        }

        auto doc = StateDocument::parse(
            begin->doc, IDLParserContext{"StateTransitionControllerOpObserver::onInserts"});
        _controller->_notifyNewStateAndWaitUntilUnpaused(opCtx, _getState(doc));
        invariant(++begin == end);  // No support for inserting more than one state document yet.
    }

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override {
        if (args.coll->ns() != _stateDocumentNss) {
            return;
        }

        auto doc =
            StateDocument::parse(args.updateArgs->updatedDoc,
                                 IDLParserContext{"StateTransitionControllerOpObserver::onUpdate"});
        _controller->_notifyNewStateAndWaitUntilUnpaused(opCtx, _getState(doc));
    }

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override {
        if (coll->ns() != _stateDocumentNss) {
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
