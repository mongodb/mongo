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

#include <string>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/principal.h"

namespace mongo {

    /**
     * A representation of the permission to perform a set of actions on a specific resource.
     */
    class Capability {
    public:

        Capability(const std::string& resource, ActionSet actions);
        ~Capability() {}

        const std::string& getResource() const {
            return _resource;
        }

        const ActionSet& getActions() const {
            return _actions;
        }

        // Checks if the given action is present in the Capability.
        bool includesAction(const ActionType& action) const;

    private:

        std::string _resource;
        ActionSet _actions; // bitmask of actions this capability grants
    };

} // namespace mongo
