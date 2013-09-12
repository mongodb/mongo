/*    Copyright 2013 10gen Inc.

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

#include "mongo/db/auth/user.h"

#include <vector>

#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "db/auth/role_name.h"

namespace mongo {

    User::User(const UserName& name) : _name(name), _refCount(0), _isValid(1) {}
    User::~User() {
        dassert(_refCount == 0);
    }

    const UserName& User::getName() const {
        return _name;
    }

    const User::RoleDataMap& User::getRoles() const {
        return _roles;
    }

    const User::CredentialData& User::getCredentials() const {
        return _credentials;
    }

    bool User::isValid() const {
        return _isValid.loadRelaxed() == 1;
    }

    uint32_t User::getRefCount() const {
        return _refCount;
    }

    const ActionSet User::getActionsForResource(const std::string& resource) const {
        unordered_map<string, Privilege>::const_iterator it = _privileges.find(resource);
        if (it == _privileges.end()) {
            return ActionSet();
        }
        return it->second.getActions();
    }

    void User::copyFrom(const User& other) {
        _name = other._name;
        _privileges = other._privileges;
        _roles = other._roles;
        _credentials = other._credentials;
        _refCount = other._refCount;
        _isValid= other._isValid;
    }

    void User::setCredentials(const CredentialData& credentials) {
        _credentials = credentials;
    }

    void User::addRole(const RoleName& roleName) {
        RoleData& role = _roles[roleName];
        if (role.name.empty()) {
            role.name = roleName;
        }
        role.hasRole = true;
    }

    void User::addRoles(const std::vector<RoleName>& roles) {
        for (std::vector<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            addRole(*it);
        }
    }

    void User::addDelegatableRole(const RoleName& roleName) {
        RoleData& role = _roles[roleName];
        if (role.name.empty()) {
            role.name = roleName;
        }
        role.canDelegate = true;
    }

    void User::addDelegatableRoles(const std::vector<RoleName>& roles) {
        for (std::vector<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
            addDelegatableRole(*it);
        }
    }

    void User::addPrivilege(const Privilege& privilegeToAdd) {
        ResourcePrivilegeMap::iterator it = _privileges.find(privilegeToAdd.getResource());
        if (it == _privileges.end()) {
            // No privilege exists yet for this resource
            _privileges.insert(std::make_pair(privilegeToAdd.getResource(), privilegeToAdd));
        } else {
            dassert(it->first == privilegeToAdd.getResource());
            it->second.addActions(privilegeToAdd.getActions());
        }
    }

    void User::addPrivileges(const PrivilegeVector& privileges) {
        for (PrivilegeVector::const_iterator it = privileges.begin();
                it != privileges.end(); ++it) {
            addPrivilege(*it);
        }
    }

    void User::invalidate() {
        _isValid.store(0);
    }

    void User::incrementRefCount() {
        ++_refCount;
    }

    void User::decrementRefCount() {
        dassert(_refCount > 0);
        --_refCount;
    }
} // namespace mongo
