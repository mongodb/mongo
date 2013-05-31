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

#pragma once

#include <bitset>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_type.h"

namespace mongo {

    /*
     *  An ActionSet is a bitmask of ActionTypes that represents a set of actions.
     *  These are the actions that a Privilege can grant a user to perform on a resource.
     */
    class ActionSet {
    public:

        ActionSet() : _actions(0) {}

        void addAction(const ActionType& action);
        void addAllActionsFromSet(const ActionSet& actionSet);
        void addAllActions();

        void removeAction(const ActionType& action);
        void removeAllActionsFromSet(const ActionSet& actionSet);
        void removeAllActions();

        bool empty() const { return _actions.none(); }

        bool equals(const ActionSet& other) const { return this->_actions == other._actions; }

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

        // bitmask of actions this privilege grants
        std::bitset<ActionType::NUM_ACTION_TYPES> _actions;
    };

    static inline bool operator==(const ActionSet& lhs, const ActionSet& rhs) {
        return lhs.equals(rhs);
    }

} // namespace mongo
