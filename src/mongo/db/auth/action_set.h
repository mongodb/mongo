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

#pragma once

#include <bitset>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_type.h"

namespace mongo {

    /*
     *  An ActionSet is a bitmask of ActionTypes that represents a set of actions.
     *  These are the actions that a Capability can grant a principal to perform on a resource.
     */
    class ActionSet {
    public:

        ActionSet() : _actions(0) {}

        void addAction(const ActionType& action);
        void addAllActionsFromSet(const ActionSet& actionSet);
        void addAllActions();

        bool contains(const ActionType& action) const;

        // Returns true only if this ActionSet contains all the actions present in the 'other'
        // ActionSet.
        bool isSupersetOf(const ActionSet& other) const;

        // Returns the string representation of this ActionSet
        std::string toString() const;

        // Takes a comma-separated string of action type string representations and returns
        // an int bitmask of the actions.
        static Status parseActionSetFromString(const std::string& actionsString, ActionSet* result);

    private:

        // bitmask of actions this capability grants
        std::bitset<ActionType::NUM_ACTION_TYPES> _actions;
    };

} // namespace mongo
