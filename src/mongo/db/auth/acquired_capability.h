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

#include "mongo/db/auth/capability.h"
#include "mongo/db/auth/principal.h"

namespace mongo {

    /**
     * A representation that a given principal has the permission to perform a set of actions on a
     * specific resource.
     * This class does not do any locking/synchronization, the consumer will be responsible for
     * synchronizing access.
     */
    class AcquiredCapability {
    public:

        AcquiredCapability(const Capability& capability, Principal* principal);
        AcquiredCapability(const std::string& resource, Principal* principal, ActionSet actions);
        ~AcquiredCapability() {}

        const Principal* getPrincipal() const;
        const std::string& getResource() const;
        const ActionSet& getActions() const;

        // Checks if the given action is present in the Capability.
        bool includesAction(const ActionType& action) const;

    private:

        Capability _capability;
        Principal* _principal;
    };

} // namespace mongo
