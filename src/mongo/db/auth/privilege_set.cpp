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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/principal.h"
#include "mongo/util/map_util.h"

namespace mongo {

    const std::string PrivilegeSet::WILDCARD_RESOURCE = "*";

    PrivilegeSet::PrivilegeSet() {}
    PrivilegeSet::~PrivilegeSet() {}

    void PrivilegeSet::grantPrivilege(const Privilege& privilege,
                                      const PrincipalName& authorizingPrincipal) {
        grantPrivileges(std::vector<Privilege>(1, privilege), authorizingPrincipal);
    }

    void PrivilegeSet::grantPrivileges(const std::vector<Privilege>& privileges,
                                       const PrincipalName& authorizingPrincipal) {
        StringMap<ActionSet>& byResourceForPrincipal = _byPrincipal[authorizingPrincipal];
        for (std::vector<Privilege>::const_iterator iter = privileges.begin(),
                 end = privileges.end();
             iter != end; ++iter) {

            byResourceForPrincipal[iter->getResource()].addAllActionsFromSet(iter->getActions());

            ResourcePrivilegeCacheEntry* entry = _lookupOrInsertEntry(iter->getResource());
            entry->actions.addAllActionsFromSet(iter->getActions());
        }
    }

    void PrivilegeSet::revokePrivilegesFromPrincipal(const PrincipalName& principal) {
        PrincipalPrivilegeMap::iterator principalEntry = _byPrincipal.find(principal);
        if (principalEntry == _byPrincipal.end())
            return;

        // For every resource that "principal" authorizes, mark its entry in the _byResource table
        // as dirty, so that it will be rebuilt on next consultation.
        for (StringMap<ActionSet>::const_iterator resourceEntry = principalEntry->second.begin(),
                 end = principalEntry->second.end();
             resourceEntry != end; ++resourceEntry) {

            _lookupOrInsertEntry(resourceEntry->first)->dirty = true;
        }

        // Remove the princiapl from the _byPrincipal table.
        _byPrincipal.erase(principalEntry);
    }

    bool PrivilegeSet::hasPrivilege(const Privilege& desiredPrivilege) {
        if (desiredPrivilege.getActions().empty())
            return true;

        StringData resourceSearchList[2];
        resourceSearchList[0] = WILDCARD_RESOURCE;
        resourceSearchList[1] = desiredPrivilege.getResource();

        ActionSet unmetRequirements = desiredPrivilege.getActions();
        for (int i = 0; i < boost::size(resourceSearchList); ++i) {
            ResourcePrivilegeCacheEntry* entry = _lookupEntry(resourceSearchList[i]);
            if (NULL == entry)
                continue;
            if (entry->dirty)
                _rebuildEntry(resourceSearchList[i], entry);
            unmetRequirements.removeAllActionsFromSet(entry->actions);
            if (unmetRequirements.empty())
                return true;
        }
        return false;
    }

    bool PrivilegeSet::hasPrivileges(const std::vector<Privilege>& desiredPrivileges) {
        for (std::vector<Privilege>::const_iterator iter = desiredPrivileges.begin(),
                 end = desiredPrivileges.end();
             iter != end; ++iter) {

            if (!hasPrivilege(*iter))
                return false;
        }
        return true;
    }

    void PrivilegeSet::_rebuildEntry(const StringData& resource,
                                     ResourcePrivilegeCacheEntry* entry) {
        const ActionSet emptyActionSet;
        entry->actions.removeAllActions();

        for (PrincipalPrivilegeMap::const_iterator iter = _byPrincipal.begin(),
                 end = _byPrincipal.end();
             iter != end; ++iter) {

            entry->actions.addAllActionsFromSet(
                    mapFindWithDefault(iter->second, resource, emptyActionSet));
        }

        entry->dirty = false;
    }

    PrivilegeSet::ResourcePrivilegeCacheEntry* PrivilegeSet::_lookupEntry(
            const StringData& resource) {

        if (resource == WILDCARD_RESOURCE)
            return &_globalPrivilegeEntry;

        ResourcePrivilegeCache::const_iterator iter = _byResource.find(resource);
        if (iter != _byResource.end()) {
            // StringMap doesn't have non-const iterators, so there is no way to lookup without
            // inserting and get a mutable value, without const-cast.
            return const_cast<ResourcePrivilegeCacheEntry*>(&iter->second);
        }
        return NULL;
    }

    PrivilegeSet::ResourcePrivilegeCacheEntry* PrivilegeSet::_lookupOrInsertEntry(
            const StringData& resource) {

        if (resource == WILDCARD_RESOURCE)
            return &_globalPrivilegeEntry;
        return &_byResource[resource];
    }

} // namespace mongo
