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

#include "mongo/pch.h"

#include "mongo/db/auth/capability_set.h"

#include <map>
#include <string>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/capability.h"
#include "mongo/db/auth/principal.h"

namespace mongo {

    void CapabilitySet::grantCapability(const Capability& capability) {
        _capabilities.insert(std::make_pair(capability.getResource(), capability));
    }

    const Capability* CapabilitySet::getCapabilityForAction(const std::string& resource,
                                                            const ActionType& action) const {
        CapabilitySetConstRange range;
        CapabilityRangeConstIterator it;

        range = _capabilities.equal_range(resource);
        for (it = range.first; it != range.second; ++it) {
            const Capability& capability = it->second;
            if (capability.includesAction(action)) {
                return &capability;
            }
        }
        return NULL;
    }

    void CapabilitySet::revokeCapabilitiesFromPrincipal(Principal* principal) {
        CapabilityRangeIterator it = _capabilities.begin();

        while (it != _capabilities.end()) {
            CapabilityRangeIterator current = it;
            ++it;  // Must advance now because erase will invalidate the iterator
            Capability& capability = current->second;
            if (capability.getPrincipal() == principal) {
                _capabilities.erase(current);
            }
        }
    }

} // namespace mongo
