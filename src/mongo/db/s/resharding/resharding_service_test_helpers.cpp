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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_service_test_helpers.h"

#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"

namespace mongo {
namespace resharding_service_test_helpers {

// -----------------------------------------------
//  StateTransitionController
// -----------------------------------------------

template <class StateEnum>
void StateTransitionController<StateEnum>::waitUntilStateIsReached(StateEnum state) {
    stdx::unique_lock lk(_mutex);
    _waitUntilUnpausedCond.wait(lk, [this, state] { return _state == state; });
}

template <class StateEnum>
void StateTransitionController<StateEnum>::_setPauseDuringTransition(StateEnum state) {
    stdx::lock_guard lk(_mutex);
    _pauseDuringTransition.insert(state);
}

template <class StateEnum>
void StateTransitionController<StateEnum>::_unsetPauseDuringTransition(StateEnum state) {
    stdx::lock_guard lk(_mutex);
    _pauseDuringTransition.erase(state);
    _pauseDuringTransitionCond.notify_all();
}

template <class StateEnum>
void StateTransitionController<StateEnum>::_notifyNewStateAndWaitUntilUnpaused(
    OperationContext* opCtx, StateEnum newState) {
    stdx::unique_lock lk(_mutex);
    auto guard = makeGuard([this, prevState = _state] { _state = prevState; });
    _state = newState;
    _waitUntilUnpausedCond.notify_all();
    opCtx->waitForConditionOrInterrupt(_pauseDuringTransitionCond, lk, [this, newState] {
        return _pauseDuringTransition.count(newState) == 0;
    });
    guard.dismiss();
}

template <class StateEnum>
void StateTransitionController<StateEnum>::_resetReachedState() {
    stdx::lock_guard lk(_mutex);
    _state = StateEnum::kUnused;
}

// -----------------------------------------------
//  PauseDuringStateTransitions
// -----------------------------------------------

template <class StateEnum>
PauseDuringStateTransitions<StateEnum>::PauseDuringStateTransitions(
    StateTransitionController<StateEnum>* controller, StateEnum state)
    : PauseDuringStateTransitions<StateEnum>(controller, std::vector<StateEnum>{state}) {}

template <class StateEnum>
PauseDuringStateTransitions<StateEnum>::PauseDuringStateTransitions(
    StateTransitionController<StateEnum>* controller, std::vector<StateEnum> states)
    : _controller{controller}, _states{std::move(states)} {
    _controller->_resetReachedState();
    for (auto state : _states) {
        _controller->_setPauseDuringTransition(state);
    }
}

template <class StateEnum>
PauseDuringStateTransitions<StateEnum>::~PauseDuringStateTransitions() {
    for (auto state : _states) {
        _controller->_unsetPauseDuringTransition(state);
    }
}

template <class StateEnum>
void PauseDuringStateTransitions<StateEnum>::wait(StateEnum state) {
    _controller->waitUntilStateIsReached(state);
}

template <class StateEnum>
void PauseDuringStateTransitions<StateEnum>::unset(StateEnum state) {
    _controller->_unsetPauseDuringTransition(state);
}

// -----------------------------------------------
// OpObserverForTest
// -----------------------------------------------

template <class StateEnum, class ReshardingDocument>
OpObserverForTest<StateEnum, ReshardingDocument>::OpObserverForTest(
    std::shared_ptr<StateTransitionController<StateEnum>> controller,
    NamespaceString reshardingDocumentNss)
    : _controller{std::move(controller)},
      _reshardingDocumentNss{std::move(reshardingDocumentNss)} {}

template <class StateEnum, class ReshardingDocument>
void OpObserverForTest<StateEnum, ReshardingDocument>::onUpdate(OperationContext* opCtx,
                                                                const OplogUpdateEntryArgs& args) {
    if (args.nss != _reshardingDocumentNss) {
        return;
    }

    auto doc = ReshardingDocument::parse({"OpObserverForTest"}, args.updateArgs.updatedDoc);
    _controller->_notifyNewStateAndWaitUntilUnpaused(opCtx, getState(doc));
}

template class StateTransitionController<DonorStateEnum>;
template class StateTransitionController<RecipientStateEnum>;
template class StateTransitionController<CoordinatorStateEnum>;

template class PauseDuringStateTransitions<DonorStateEnum>;
template class PauseDuringStateTransitions<RecipientStateEnum>;
template class PauseDuringStateTransitions<CoordinatorStateEnum>;

template class OpObserverForTest<DonorStateEnum, ReshardingDonorDocument>;
template class OpObserverForTest<RecipientStateEnum, ReshardingRecipientDocument>;
template class OpObserverForTest<CoordinatorStateEnum, ReshardingCoordinatorDocument>;

}  // namespace resharding_service_test_helpers
}  // namespace mongo
