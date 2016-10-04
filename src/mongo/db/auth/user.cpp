/*    Copyright 2013 10gen Inc.

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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/auth/user.h"

#include <vector>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/sequence_util.h"

namespace mongo {

User::User(const UserName& name) : _name(name), _refCount(0), _isValid(1) {}

User::~User() {
    dassert(_refCount == 0);
}

const UserName& User::getName() const {
    return _name;
}

RoleNameIterator User::getRoles() const {
    return makeRoleNameIteratorForContainer(_roles);
}

RoleNameIterator User::getIndirectRoles() const {
    return makeRoleNameIteratorForContainer(_indirectRoles);
}

bool User::hasRole(const RoleName& roleName) const {
    return _roles.count(roleName);
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

const ActionSet User::getActionsForResource(const ResourcePattern& resource) const {
    unordered_map<ResourcePattern, Privilege>::const_iterator it = _privileges.find(resource);
    if (it == _privileges.end()) {
        return ActionSet();
    }
    return it->second.getActions();
}

User* User::clone() const {
    std::unique_ptr<User> result(new User(_name));
    result->_privileges = _privileges;
    result->_roles = _roles;
    result->_credentials = _credentials;
    return result.release();
}

void User::setCredentials(const CredentialData& credentials) {
    _credentials = credentials;
}

void User::setRoles(RoleNameIterator roles) {
    _roles.clear();
    while (roles.more()) {
        _roles.insert(roles.next());
    }
}

void User::setIndirectRoles(RoleNameIterator indirectRoles) {
    _indirectRoles.clear();
    while (indirectRoles.more()) {
        _indirectRoles.push_back(indirectRoles.next());
    }
}

void User::setPrivileges(const PrivilegeVector& privileges) {
    _privileges.clear();
    for (size_t i = 0; i < privileges.size(); ++i) {
        const Privilege& privilege = privileges[i];
        _privileges[privilege.getResourcePattern()] = privilege;
    }
}

void User::addRole(const RoleName& roleName) {
    _roles.insert(roleName);
}

void User::addRoles(const std::vector<RoleName>& roles) {
    for (std::vector<RoleName>::const_iterator it = roles.begin(); it != roles.end(); ++it) {
        addRole(*it);
    }
}

void User::addPrivilege(const Privilege& privilegeToAdd) {
    ResourcePrivilegeMap::iterator it = _privileges.find(privilegeToAdd.getResourcePattern());
    if (it == _privileges.end()) {
        // No privilege exists yet for this resource
        _privileges.insert(std::make_pair(privilegeToAdd.getResourcePattern(), privilegeToAdd));
    } else {
        dassert(it->first == privilegeToAdd.getResourcePattern());
        it->second.addActions(privilegeToAdd.getActions());
    }
}

void User::addPrivileges(const PrivilegeVector& privileges) {
    for (PrivilegeVector::const_iterator it = privileges.begin(); it != privileges.end(); ++it) {
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
}  // namespace mongo
