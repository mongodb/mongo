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

#include "mongo/db/auth/action_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/action_type_gen.h"

#include <bitset>
#include <cstddef>
#include <string>

namespace mongo {

ActionSet::ActionSet(std::initializer_list<ActionType> actions) {
    for (auto& action : actions) {
        addAction(action);
    }
}

void ActionSet::addAction(ActionType action) {
    if (action == ActionType::anyAction) {
        addAllActions();
        return;
    }
    _actions.set(static_cast<size_t>(action), true);
}

void ActionSet::addAllActionsFromSet(const ActionSet& actions) {
    if (actions.contains(ActionType::anyAction)) {
        addAllActions();
        return;
    }
    _actions |= actions._actions;
}

void ActionSet::addAllActions() {
    _actions.set();
}

void ActionSet::removeAction(ActionType action) {
    _actions.set(static_cast<size_t>(action), false);
    _actions.set(static_cast<size_t>(ActionType::anyAction), false);
}

void ActionSet::removeAllActionsFromSet(const ActionSet& other) {
    _actions &= ~other._actions;
    if (!other.empty()) {
        _actions.set(static_cast<size_t>(ActionType::anyAction), false);
    }
}

void ActionSet::removeAllActions() {
    _actions.reset();
}

bool ActionSet::contains(ActionType action) const {
    return _actions[static_cast<size_t>(action)];
}

bool ActionSet::contains(const ActionSet& other) const {
    return (_actions | other._actions) == _actions;
}

bool ActionSet::isSupersetOf(const ActionSet& other) const {
    return (_actions & other._actions) == other._actions;
}

ActionSet ActionSet::parseFromStringVector(const std::vector<StringData>& actions,
                                           std::vector<std::string>* unrecognizedActions) {
    ActionSet ret;

    for (auto action : actions) {
        auto swActionType = parseActionFromString(action);
        if (!swActionType.isOK()) {
            if ((swActionType.getStatus() == ErrorCodes::FailedToParse) && unrecognizedActions) {
                unrecognizedActions->push_back(std::string{action});
            }
            continue;
        }

        if (swActionType.getValue() == ActionType::anyAction) {
            ret.addAllActions();
            return ret;
        }

        ret.addAction(swActionType.getValue());
    }

    return ret;
}

std::string ActionSet::toString() const {
    if (contains(ActionType::anyAction)) {
        using mongo::toString;
        return toString(ActionType::anyAction);
    }
    std::string str;
    StringData sep;
    for (size_t i = 0; i < kNumActionTypes; ++i) {
        auto action = static_cast<ActionType>(i);
        if (contains(action)) {
            StringData name = toStringData(action);
            str.append(sep.data(), sep.size());
            str.append(name.data(), name.size());
            sep = ","_sd;
        }
    }
    return str;
}

std::vector<StringData> ActionSet::getActionsAsStringDatas() const {
    if (contains(ActionType::anyAction)) {
        return {ActionType_serializer(ActionType::anyAction)};
    }

    std::vector<StringData> result;
    for (size_t i = 0; i < kNumActionTypes; ++i) {
        auto action = static_cast<ActionType>(i);
        if (contains(action)) {
            result.push_back(toStringData(action));
        }
    }
    return result;
}

}  // namespace mongo
