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

    const AcquiredPrivilege* PrivilegeSet::getPrivilegeForActions(const std::string& resource,
                                                                  const ActionSet& actions) const {
        PrivilegeSetConstRange range;
        PrivilegeRangeConstIterator it;

        range = _privileges.equal_range(resource);
        for (it = range.first; it != range.second; ++it) {
            const AcquiredPrivilege& privilege = it->second;
            if (privilege.getPrivilege().includesActions(actions)) {
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
