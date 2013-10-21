/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/pch.h"

#include "mongo/db/auth/action_set.h"

#include <bitset>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    void ActionSet::addAction(const ActionType& action) {
        if (action == ActionType::anyAction) {
            addAllActions();
            return;
        }
        _actions.set(action.getIdentifier(), true);
    }

    void ActionSet::addAllActionsFromSet(const ActionSet& actions) {
        if (actions.contains(ActionType::anyAction)) {
            addAllActions();
            return;
        }
        _actions |= actions._actions;
    }

    void ActionSet::addAllActions() {
        _actions = ~std::bitset<ActionType::NUM_ACTION_TYPES>();
    }

    void ActionSet::removeAction(const ActionType& action) {
        _actions.set(action.getIdentifier(), false);
        _actions.set(ActionType::anyAction.getIdentifier(), false);
    }

    void ActionSet::removeAllActionsFromSet(const ActionSet& other) {
        _actions &= ~other._actions;
        if (!other.empty()) {
            _actions.set(ActionType::anyAction.getIdentifier(), false);
        }
    }

    void ActionSet::removeAllActions() {
        _actions = std::bitset<ActionType::NUM_ACTION_TYPES>();
    }

    bool ActionSet::contains(const ActionType& action) const {
        return _actions[action.getIdentifier()];
    }

    bool ActionSet::isSupersetOf(const ActionSet& other) const {
        return (_actions & other._actions) == other._actions;
    }

    Status ActionSet::parseActionSetFromString(const std::string& actionsString,
                                               ActionSet* result) {
        std::vector<std::string> actionsList;
        splitStringDelim(actionsString, &actionsList, ',');
        return parseActionSetFromStringVector(actionsList, result);
    }

    Status ActionSet::parseActionSetFromStringVector(const std::vector<std::string>& actionsVector,
                                                     ActionSet* result) {
        ActionSet actions;
        for (size_t i = 0; i < actionsVector.size(); i++) {
            ActionType action;
            Status status = ActionType::parseActionFromString(actionsVector[i], &action);
            if (status != Status::OK()) {
                ActionSet empty;
                *result = empty;
                return status;
            }
            if (action == ActionType::anyAction) {
                actions.addAllActions();
                break;
            }
            actions.addAction(action);
        }
        *result = actions;
        return Status::OK();
    }

    std::string ActionSet::toString() const {
        if (contains(ActionType::anyAction)) {
            return ActionType::anyAction.toString();
        }
        StringBuilder str;
        bool addedOne = false;
        for (int i = 0; i < ActionType::actionTypeEndValue; i++) {
            ActionType action(i);
            if (contains(action)) {
                if (addedOne) {
                    str << ",";
                }
                str << ActionType::actionToString(action);
                addedOne = true;
            }
        }
        return str.str();
    }

    std::vector<std::string> ActionSet::getActionsAsStrings() const {
        std::vector<std::string> result;
        if (contains(ActionType::anyAction)) {
            result.push_back(ActionType::anyAction.toString());
            return result;
        }
        for (int i = 0; i < ActionType::actionTypeEndValue; i++) {
            ActionType action(i);
            if (contains(action)) {
                result.push_back(ActionType::actionToString(action));
            }
        }
        return result;
    }

} // namespace mongo
