// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/heartbeat_response_action.h"

namespace mongo {
namespace repl {

HeartbeatResponseAction HeartbeatResponseAction::makeNoAction() {
    return HeartbeatResponseAction();
}

HeartbeatResponseAction HeartbeatResponseAction::makeReconfigAction() {
    HeartbeatResponseAction result;
    result._action = Reconfig;
    return result;
}

HeartbeatResponseAction HeartbeatResponseAction::makePriorityTakeoverAction() {
    HeartbeatResponseAction result;
    result._action = PriorityTakeover;
    return result;
}

HeartbeatResponseAction HeartbeatResponseAction::makeCatchupTakeoverAction() {
    HeartbeatResponseAction result;
    result._action = CatchupTakeover;
    return result;
}

HeartbeatResponseAction HeartbeatResponseAction::makeRetryReconfigAction() {
    HeartbeatResponseAction result;
    result._action = RetryReconfig;
    return result;
}

HeartbeatResponseAction HeartbeatResponseAction::makeStepDownSelfAction(int primaryIndex) {
    HeartbeatResponseAction result;
    result._action = StepDownSelf;
    result._primaryIndex = primaryIndex;
    return result;
}

HeartbeatResponseAction HeartbeatResponseAction::makeRestartHeartbeatsAction() {
    HeartbeatResponseAction result;
    result._action = RestartHeartbeats;
    return result;
}

HeartbeatResponseAction::HeartbeatResponseAction() : _action(NoAction), _primaryIndex(-1) {}

void HeartbeatResponseAction::setNextHeartbeatStartDate(Date_t when) {
    _nextHeartbeatStartDate = when;
}

void HeartbeatResponseAction::setAdvancedOpTimeOrUpdatedConfig(bool advancedOrUpdated) {
    _advancedOpTimeOrUpdatedConfig = advancedOrUpdated;
}

void HeartbeatResponseAction::setBecameElectable(bool becameElectable) {
    _becameElectable = becameElectable;
}

void HeartbeatResponseAction::setChangedMemberState(bool changedMemberState) {
    _changedMemberState = changedMemberState;
}

}  // namespace repl
}  // namespace mongo
