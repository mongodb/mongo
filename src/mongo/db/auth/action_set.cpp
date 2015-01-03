/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#define MONGO_PCH_WHITELISTED
#include "mongo/platform/basic.h"
#include "mongo/pch.h"
#undef MONGO_PCH_WHITELISTED

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
