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

namespace mongo {
namespace resharding_service_test_helpers {
template <class StateEnum, class ReshardingDocument>
class OpObserverForTest;

template <class StateEnum>
class PauseDuringStateTransitions;

template <class StateEnum>
class StateTransitionController;

template <class StateEnum>
class StateTransitionController {
public:
    StateTransitionController() = default;

    void waitUntilStateIsReached(StateEnum state);

private:
    template <class Enum, class ReshardingDocument>
    friend class OpObserverForTest;

    template <class Enum>
    friend class PauseDuringStateTransitions;

    void _setPauseDuringTransition(StateEnum state);

    void _unsetPauseDuringTransition(StateEnum state);

    void _notifyNewStateAndWaitUntilUnpaused(OperationContext* opCtx, StateEnum newState);

    void _resetReachedState();

    Mutex _mutex = MONGO_MAKE_LATCH("StateTransitionController::_mutex");
    stdx::condition_variable _pauseDuringTransitionCond;
    stdx::condition_variable _waitUntilUnpausedCond;

    std::set<StateEnum> _pauseDuringTransition;
    StateEnum _state = StateEnum::kUnused;
};

template <class StateEnum>
class PauseDuringStateTransitions {
public:
    PauseDuringStateTransitions(StateTransitionController<StateEnum>* controller, StateEnum state);
    PauseDuringStateTransitions(StateTransitionController<StateEnum>* controller,
                                std::vector<StateEnum> states);

    ~PauseDuringStateTransitions();

    PauseDuringStateTransitions(const PauseDuringStateTransitions&) = delete;
    PauseDuringStateTransitions& operator=(const PauseDuringStateTransitions&) = delete;

    PauseDuringStateTransitions(PauseDuringStateTransitions&&) = delete;
    PauseDuringStateTransitions& operator=(PauseDuringStateTransitions&&) = delete;

    void wait(StateEnum state);

    void unset(StateEnum state);

private:
    StateTransitionController<StateEnum>* const _controller;
    const std::vector<StateEnum> _states;
};

template <class StateEnum, class ReshardingDocument>
class OpObserverForTest : public OpObserverNoop {
public:
    OpObserverForTest(std::shared_ptr<StateTransitionController<StateEnum>> controller,
                      NamespaceString reshardingDocumentNss);

    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) override;

    virtual StateEnum getState(const ReshardingDocument& reshardingDoc) = 0;

private:
    std::shared_ptr<StateTransitionController<StateEnum>> _controller;
    const NamespaceString _reshardingDocumentNss;
};

}  // namespace resharding_service_test_helpers
}  // namespace mongo
