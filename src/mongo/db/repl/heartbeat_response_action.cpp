/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

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
