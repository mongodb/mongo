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

#include <string>

#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    void ActionSet::addAction(const ActionType& action) {
        _actions |= action;
    }

    bool ActionSet::contains(const ActionType& action) const {
        return _actions & action;
    }

    bool ActionSet::isSupersetOf(const ActionSet& other) const {
        return (_actions & other._actions) == other._actions;
    }


    Status ActionSet::parseActionFromString(const std::string& action,
                                            ActionSet::ActionType* result) {
        if(action == "r") {
            *result = ActionSet::READ;
            return Status::OK();
        } else if (action == "w") {
            *result = ActionSet::WRITE;
            return Status::OK();
        } else if (action == "u") {
            *result = ActionSet::USER_ADMIN;
            return Status::OK();
        } else if (action == "p") {
            *result = ActionSet::PRODUCTION_ADMIN;
            return Status::OK();
        } else if (action == "a") {
            *result = ActionSet::SUPER_ADMIN;
            return Status::OK();
        } else {
            *result = ActionSet::NONE;
            return Status(ErrorCodes::FailedToParse,
                          mongoutils::str::stream() << "Unrecognized action capability string: "
                                                    << action,
                          0);
        }
    }

    Status ActionSet::parseActionSetFromString(const std::string& actionsString,
                                               ActionSet* result) {
        std::vector<std::string> actionsList;
        splitStringDelim(actionsString, &actionsList, ',');
        ActionSet actions;
        for (size_t i = 0; i < actionsList.size(); i++) {
            ActionSet::ActionType action;
            Status status = parseActionFromString(actionsList[i], &action);
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

    // Takes an ActionType and returns the string representation
    std::string ActionSet::actionToString(const ActionSet::ActionType& action) {
        return ""; // TODO
    }

    // Takes an ActionSet and returns the string representation
    std::string ActionSet::actionSetToString(const ActionSet& actionSet) {
        return ""; // TODO
    }

} // namespace mongo
