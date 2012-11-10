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

#include <map>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/acquired_privilege.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/principal.h"

namespace mongo {

    /**
     * A collection of privileges describing which authenticated principals bestow the client
     * the ability to perform various actions on specific resources.  Since every privilege
     * comes from an authenticated principal, removing that principal can remove all privileges
     * that that principal granted.
     * This class does not do any locking/synchronization, the consumer will be responsible for
     * synchronizing access.
     */
    class PrivilegeSet {
        MONGO_DISALLOW_COPYING(PrivilegeSet);
    public:
        PrivilegeSet(){}
        ~PrivilegeSet(){}

        void grantPrivilege(const AcquiredPrivilege& privilege);
        void revokePrivilegesFromPrincipal(Principal* principal);

        // Returns the first privilege found that grants the given action on the given resource.
        // Returns NULL if there is no such privilege.
        // Ownership of the returned Privilege remains with the PrivilegeSet.  The pointer
        // returned is only guaranteed to remain valid until the next non-const method is called
        // on the PrivilegeSet.
        const AcquiredPrivilege* getPrivilegeForAction(const std::string& resource,
                                                       const ActionType& action) const;

    private:

        // Key is the resource the privilege is on.
        typedef std::multimap<const std::string, AcquiredPrivilege> PrivilegeMap;
        typedef PrivilegeMap::iterator PrivilegeRangeIterator;
        typedef std::pair<PrivilegeRangeIterator, PrivilegeRangeIterator> PrivilegeSetRange;
        typedef PrivilegeMap::const_iterator PrivilegeRangeConstIterator;
        typedef std::pair<PrivilegeRangeConstIterator, PrivilegeRangeConstIterator>
                PrivilegeSetConstRange;

        // Maps resource to privileges
        PrivilegeMap _privileges;
    };

} // namespace mongo
