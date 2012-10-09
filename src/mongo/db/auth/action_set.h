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

#include <stdint.h>

#include "mongo/base/status.h"

namespace mongo {

    /*
     *  An ActionSet is a bitmask of ActionTypes that represents a set of actions.
     *  These are the actions that a Capability can grant a principal to perform on a resource.
     */
    class ActionSet {
    public:

        ActionSet() : _actions(0) {}

        // This describes the different types of actions that a principle can have the capability
        // to perform on a resource
        enum ActionType {
            NONE = 0x0,
            READ = 0x1, // Can query data, run read-only commands.
            WRITE = 0x2, // Can read or write data, run any non-admin command, can create indexes.
            USER_ADMIN = 0x4, // Can read and write system.users.
            PRODUCTION_ADMIN = 0x8, // Can run single-db admin commands.
            SUPER_ADMIN = 0x10, // Can run cluster-wide admin commands.
        };

        void addAction(const ActionType& action);

        bool contains(const ActionType& action) const;

        // Returns true only if this ActionSet contains all the actions present in the 'other'
        // ActionSet.
        bool isSupersetOf(const ActionSet& other) const; // TODO: test

        // Takes the string representation of a single action type and returns the corresponding
        // ActionType enum.
        static Status parseActionFromString(const std::string& actionString,
                                            ActionSet::ActionType* result);

        // Takes a comma-separate string of action type string representations and returns
        // an int bitmask of the actions.
        static Status parseActionSetFromString(const std::string& actionsString, ActionSet* result);

        // Takes an ActionType and returns the string representation
        static std::string actionToString(const ActionSet::ActionType& action); // TODO

        // Takes an ActionSet and returns the string representation
        static std::string actionSetToString(const ActionSet& actionSet); // TODO

    private:

        uint64_t _actions; // bitmask of actions this capability grants
    };

} // namespace mongo
