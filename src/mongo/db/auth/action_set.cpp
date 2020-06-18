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

#include "mongo/db/auth/action_set.h"

#include <bitset>
#include <string>

#include "mongo/base/status.h"
#include "mongo/util/str.h"

namespace mongo {

ActionSet::ActionSet(std::initializer_list<ActionType> actions) {
    for (auto& action : actions) {
        addAction(action);
    }
}

void ActionSet::addAction(const ActionType& action) {
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

void ActionSet::removeAction(const ActionType& action) {
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

bool ActionSet::contains(const ActionType& action) const {
    return _actions[static_cast<size_t>(action)];
}

bool ActionSet::isSupersetOf(const ActionSet& other) const {
    return (_actions & other._actions) == other._actions;
}

Status ActionSet::parseActionSetFromString(const std::string& actionsString, ActionSet* result) {
    std::vector<std::string> actionsList;
    str::splitStringDelim(actionsString, &actionsList, ',');
    std::vector<std::string> unrecognizedActions;
    Status status = parseActionSetFromStringVector(actionsList, result, &unrecognizedActions);
    invariant(status);
    if (unrecognizedActions.empty()) {
        return Status::OK();
    }
    std::string unrecognizedActionsString;
    str::joinStringDelim(unrecognizedActions, &unrecognizedActionsString, ',');
    return Status(ErrorCodes::FailedToParse,
                  str::stream() << "Unrecognized action privilege strings: "
                                << unrecognizedActionsString);
}

Status ActionSet::parseActionSetFromStringVector(const std::vector<std::string>& actionsVector,
                                                 ActionSet* result,
                                                 std::vector<std::string>* unrecognizedActions) {
    result->removeAllActions();
    for (StringData actionName : actionsVector) {
        auto parseResult = parseActionFromString(actionName);
        if (!parseResult.isOK()) {
            const auto& status = parseResult.getStatus();
            if (status == ErrorCodes::FailedToParse) {
                unrecognizedActions->push_back(std::string{actionName});
            } else {
                invariant(status);
            }
        } else {
            const auto& action = parseResult.getValue();
            if (action == ActionType::anyAction) {
                result->addAllActions();
                return Status::OK();
            }
            result->addAction(action);
        }
    }
    return Status::OK();
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
            str.append(sep.rawData(), sep.size());
            str.append(name.rawData(), name.size());
            sep = ","_sd;
        }
    }
    return str;
}

std::vector<std::string> ActionSet::getActionsAsStrings() const {
    using mongo::toString;
    std::vector<std::string> result;
    if (contains(ActionType::anyAction)) {
        result.push_back(toString(ActionType::anyAction));
        return result;
    }
    for (size_t i = 0; i < kNumActionTypes; ++i) {
        auto action = static_cast<ActionType>(i);
        if (contains(action)) {
            result.push_back(toString(action));
        }
    }
    return result;
}

}  // namespace mongo
