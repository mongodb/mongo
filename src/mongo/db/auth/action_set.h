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

#pragma once

#include <bitset>
#include <initializer_list>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_type.h"

namespace mongo {

/*
 *  An ActionSet is a bitmask of ActionTypes that represents a set of actions.
 *  These are the actions that a Privilege can grant a user to perform on a resource.
 *  If the special ActionType::anyAction is granted to this set, it automatically sets all bits
 *  in the bitmask, indicating that it contains all possible actions.
 */
class ActionSet {
public:
    ActionSet() : _actions(0) {}
    ActionSet(std::initializer_list<ActionType> actions);

    void addAction(const ActionType& action);
    void addAllActionsFromSet(const ActionSet& actionSet);
    void addAllActions();

    // Removes action from the set.  Also removes the "anyAction" action, if present.
    // Note: removing the "anyAction" action does *not* remove all other actions.
    void removeAction(const ActionType& action);
    void removeAllActionsFromSet(const ActionSet& actionSet);
    void removeAllActions();

    bool empty() const {
        return _actions.none();
    }

    bool equals(const ActionSet& other) const {
        return this->_actions == other._actions;
    }

    bool contains(const ActionType& action) const;

    // Returns true only if this ActionSet contains all the actions present in the 'other'
    // ActionSet.
    bool isSupersetOf(const ActionSet& other) const;

    // Returns the std::string representation of this ActionSet
    std::string toString() const;

    // Returns a vector of strings representing the actions in the ActionSet.
    std::vector<std::string> getActionsAsStrings() const;

    // Takes a comma-separated std::string of action type std::string representations and returns
    // an int bitmask of the actions.
    static Status parseActionSetFromString(const std::string& actionsString, ActionSet* result);

    // Takes a vector of action type std::string representations and writes into *result an
    // ActionSet of all valid actions encountered.
    // If it encounters any actions that it doesn't recognize, will put those into
    // *unrecognizedActions, while still returning the valid actions in *result, and returning OK.
    static Status parseActionSetFromStringVector(const std::vector<std::string>& actionsVector,
                                                 ActionSet* result,
                                                 std::vector<std::string>* unrecognizedActions);

private:
    // bitmask of actions this privilege grants
    std::bitset<ActionType::NUM_ACTION_TYPES> _actions;
};

static inline bool operator==(const ActionSet& lhs, const ActionSet& rhs) {
    return lhs.equals(rhs);
}

}  // namespace mongo
