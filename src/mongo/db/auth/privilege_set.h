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
        // Same as above but takes an ActionSet.  The AcquiredPrivilege returned must include
        // permission to perform all the actions in the ActionSet on the given resource.
        const AcquiredPrivilege* getPrivilegeForActions(const std::string& resource,
                                                        const ActionSet& action) const;

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
