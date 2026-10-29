// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/action_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/action_type_gen.h"

#include <bitset>
#include <cstddef>
#include <string>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

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

ActionSet ActionSet::parseFromStringVector(const std::vector<std::string_view>& actions,
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
    std::string_view sep;
    for (size_t i = 0; i < kNumActionTypes; ++i) {
        auto action = static_cast<ActionType>(i);
        if (contains(action)) {
            std::string_view name = toStringData(action);
            str.append(sep.data(), sep.size());
            str.append(name.data(), name.size());
            sep = ","sv;
        }
    }
    return str;
}

std::vector<std::string_view> ActionSet::getActionsAsStringDatas() const {
    if (contains(ActionType::anyAction)) {
        return {idl::serialize(ActionType::anyAction)};
    }

    std::vector<std::string_view> result;
    for (size_t i = 0; i < kNumActionTypes; ++i) {
        auto action = static_cast<ActionType>(i);
        if (contains(action)) {
            result.push_back(toStringData(action));
        }
    }
    return result;
}

}  // namespace mongo
