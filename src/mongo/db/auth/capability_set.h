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
#include "mongo/db/auth/acquired_capability.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/capability.h"
#include "mongo/db/auth/principal.h"

namespace mongo {

    /**
     * A collection of capabilities describing which authenticated principals bestow the client
     * the ability to perform various actions on specific resources.  Since every capability
     * comes from an authenticated principal, removing that principal can remove all capabilities
     * that that principal granted.
     * This class does not do any locking/synchronization, the consumer will be responsible for
     * synchronizing access.
     */
    class CapabilitySet {
        MONGO_DISALLOW_COPYING(CapabilitySet);
    public:
        CapabilitySet(){}
        ~CapabilitySet(){}

        void grantCapability(const AcquiredCapability& capability);
        void revokeCapabilitiesFromPrincipal(Principal* principal);

        // Returns the first capability found that grants the given action on the given resource.
        // Returns NULL if there is no such capability.
        // Ownership of the returned Capability remains with the CapabilitySet.  The pointer
        // returned is only guaranteed to remain valid until the next non-const method is called
        // on the CapabilitySet.
        const AcquiredCapability* getCapabilityForAction(const std::string& resource,
                                                         const ActionType& action) const;

    private:

        // Key is the resource the capability is on.
        typedef std::multimap<const std::string, AcquiredCapability> CapabilityMap;
        typedef CapabilityMap::iterator CapabilityRangeIterator;
        typedef std::pair<CapabilityRangeIterator, CapabilityRangeIterator> CapabilitySetRange;
        typedef CapabilityMap::const_iterator CapabilityRangeConstIterator;
        typedef std::pair<CapabilityRangeConstIterator, CapabilityRangeConstIterator>
                CapabilitySetConstRange;

        // Maps resource to capabilities
        CapabilityMap _capabilities;
    };

} // namespace mongo
