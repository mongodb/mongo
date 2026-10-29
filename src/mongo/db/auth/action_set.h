// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/util/modules.h"

#include <bitset>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {

/*
 *  An ActionSet is a bitmask of ActionTypes that represents a set of actions.
 *  These are the actions that a Privilege can grant a user to perform on a resource.
 *  If the special ActionType::anyAction is granted to this set, it automatically sets all bits
 *  in the bitmask, indicating that it contains all possible actions.
 */
class [[MONGO_MOD_PUBLIC]] ActionSet {
public:
    ActionSet() = default;
    ActionSet(std::initializer_list<ActionType> actions);

    // Parse a human-readable set of ActionTypes into a bitset of actions.
    // unrecognizedActions will be populated with a copy of any unexpected action, if present.
    static ActionSet parseFromStringVector(const std::vector<std::string_view>& actions,
                                           std::vector<std::string>* unrecognizedActions = nullptr);

    void addAction(ActionType action);
    void addAllActionsFromSet(const ActionSet& actionSet);
    void addAllActions();

    // Removes action from the set.  Also removes the "anyAction" action, if present.
    // Note: removing the "anyAction" action does *not* remove all other actions.
    void removeAction(ActionType action);
    void removeAllActionsFromSet(const ActionSet& actionSet);
    void removeAllActions();

    bool empty() const {
        return _actions.none();
    }

    bool equals(const ActionSet& other) const {
        return this->_actions == other._actions;
    }

    bool contains(ActionType action) const;

    // Returns true if this action set contains the entire other action set
    bool contains(const ActionSet& other) const;

    // Returns true only if this ActionSet contains all the actions present in the 'other'
    // ActionSet.
    bool isSupersetOf(const ActionSet& other) const;

    // Returns the std::string representation of this ActionSet
    std::string toString() const;

    // Returns a vector of strings representing the actions in the ActionSet.
    // The storage for these StringDatas comes from IDL constexpr definitions for
    // ActionTypes and is therefore guaranteed for the life of the process.
    std::vector<std::string_view> getActionsAsStringDatas() const;

    friend bool operator==(const ActionSet& lhs, const ActionSet& rhs) {
        return lhs.equals(rhs);
    }

private:
    // bitmask of actions this privilege grants
    std::bitset<kNumActionTypes> _actions;
};

}  // namespace mongo
