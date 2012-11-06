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

#include "mongo/db/auth/privilege_set.h"

#include <map>
#include <string>

#include "mongo/db/auth/acquired_privilege.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/principal.h"

namespace mongo {

    void PrivilegeSet::grantPrivilege(const AcquiredPrivilege& privilege) {
        _privileges.insert(std::make_pair(privilege.getPrivilege().getResource(), privilege));
    }

    const AcquiredPrivilege* PrivilegeSet::getPrivilegeForAction(const std::string& resource,
                                                                 const ActionType& action) const {
        PrivilegeSetConstRange range;
        PrivilegeRangeConstIterator it;

        range = _privileges.equal_range(resource);
        for (it = range.first; it != range.second; ++it) {
            const AcquiredPrivilege& privilege = it->second;
            if (privilege.getPrivilege().includesAction(action)) {
                return &privilege;
            }
        }
        return NULL;
    }

    void PrivilegeSet::revokePrivilegesFromPrincipal(Principal* principal) {
        PrivilegeRangeIterator it = _privileges.begin();

        while (it != _privileges.end()) {
            PrivilegeRangeIterator current = it;
            ++it;  // Must advance now because erase will invalidate the iterator
            AcquiredPrivilege& privilege = current->second;
            if (privilege.getPrincipal() == principal) {
                _privileges.erase(current);
            }
        }
    }

} // namespace mongo
