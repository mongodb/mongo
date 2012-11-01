/**
*    Copyright (C) 2012 10gen Inc.
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
        _actions.set(action.getIdentifier(), true);
    }

    void ActionSet::addAllActionsFromSet(const ActionSet& actions) {
        _actions |= actions._actions;
    }

    void ActionSet::addAllActions() {
        _actions = ~std::bitset<ActionType::NUM_ACTION_TYPES>();
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
        ActionSet actions;
        for (size_t i = 0; i < actionsList.size(); i++) {
            ActionType action;
            Status status = ActionType::parseActionFromString(actionsList[i], &action);
            if (status != Status::OK()) {
                ActionSet empty;
                *result = empty;
                return status;
            }
            actions.addAction(action);
        }
        *result = actions;
        return Status::OK();
    }

    std::string ActionSet::toString() const {
        StringBuilder str;
        bool addedOne = false;
        for (int i = 0; i < ActionType::ACTION_TYPE_END_VALUE; i++) {
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

} // namespace mongo
