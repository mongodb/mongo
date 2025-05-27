/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/action_type.h"

#include <bitset>
#include <initializer_list>
#include <string>
#include <vector>

namespace mongo {

/*
 *  An ActionSet is a bitmask of ActionTypes that represents a set of actions.
 *  These are the actions that a Privilege can grant a user to perform on a resource.
 *  If the special ActionType::anyAction is granted to this set, it automatically sets all bits
 *  in the bitmask, indicating that it contains all possible actions.
 */
class ActionSet {
public:
    ActionSet() = default;
    ActionSet(std::initializer_list<ActionType> actions);

    // Parse a human-readable set of ActionTypes into a bitset of actions.
    // unrecognizedActions will be populated with a copy of any unexpected action, if present.
    static ActionSet parseFromStringVector(const std::vector<StringData>& actions,
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
    std::vector<StringData> getActionsAsStringDatas() const;

    friend bool operator==(const ActionSet& lhs, const ActionSet& rhs) {
        return lhs.equals(rhs);
    }

private:
    // bitmask of actions this privilege grants
    std::bitset<kNumActionTypes> _actions;
};

}  // namespace mongo
