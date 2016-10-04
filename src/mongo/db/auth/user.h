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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

/**
 * Represents a MongoDB user.  Stores information about the user necessary for access control
 * checks and authentications, such as what privileges this user has, as well as what roles
 * the user belongs to.
 *
 * Every User object is owned by an AuthorizationManager.  The AuthorizationManager is the only
 * one that should construct, modify, or delete a User object.  All other consumers of User must
 * use only the const methods.  The AuthorizationManager is responsible for maintaining the
 * reference count on all User objects it gives out and must not mutate any User objects with
 * a non-zero reference count (except to call invalidate()).  Any consumer of a User object
 * should check isInvalidated() before using it, and if it has been invalidated, it should
 * return the object to the AuthorizationManager and fetch a new User object instance for this
 * user from the AuthorizationManager.
 */
class User {
    MONGO_DISALLOW_COPYING(User);

public:
    struct SCRAMCredentials {
        SCRAMCredentials() : iterationCount(0), salt(""), serverKey(""), storedKey("") {}

        int iterationCount;
        std::string salt;
        std::string serverKey;
        std::string storedKey;
    };
    struct CredentialData {
        CredentialData() : password(""), scram(), isExternal(false) {}

        std::string password;
        SCRAMCredentials scram;
        bool isExternal;
    };

    typedef unordered_map<ResourcePattern, Privilege> ResourcePrivilegeMap;

    explicit User(const UserName& name);
    ~User();

    /**
     * Returns the user name for this user.
     */
    const UserName& getName() const;

    /**
     * Returns an iterator over the names of the user's direct roles
     */
    RoleNameIterator getRoles() const;

    /**
     * Returns an iterator over the names of the user's indirect roles
     */
    RoleNameIterator getIndirectRoles() const;

    /**
     * Returns true if this user is a member of the given role.
     */
    bool hasRole(const RoleName& roleName) const;

    /**
     * Returns a reference to the information about the user's privileges.
     */
    const ResourcePrivilegeMap& getPrivileges() const {
        return _privileges;
    }

    /**
     * Returns the CredentialData for this user.
     */
    const CredentialData& getCredentials() const;

    /**
     * Gets the set of actions this user is allowed to perform on the given resource.
     */
    const ActionSet getActionsForResource(const ResourcePattern& resource) const;

    /**
     * Returns true if this copy of information about this user is still valid. If this returns
     * false, this object should no longer be used and should be returned to the
     * AuthorizationManager and a new User object for this user should be requested.
     */
    bool isValid() const;

    /**
     * This returns the reference count for this User.  The AuthorizationManager should be the
     * only caller of this.
     */
    uint32_t getRefCount() const;

    /**
     * Clones this user into a new, valid User object with refcount of 0.
     */
    User* clone() const;

    // Mutators below.  Mutation functions should *only* be called by the AuthorizationManager

    /**
     * Sets this user's authentication credentials.
     */
    void setCredentials(const CredentialData& credentials);

    /**
     * Replaces any existing user role membership information with the roles from "roles".
     */
    void setRoles(RoleNameIterator roles);

    /**
     * Replaces any existing indirect user role membership information with the roles from
     * "indirectRoles".
     */
    void setIndirectRoles(RoleNameIterator indirectRoles);

    /**
     * Replaces any existing user privilege information with "privileges".
     */
    void setPrivileges(const PrivilegeVector& privileges);

    /**
     * Adds the given role name to the list of roles of which this user is a member.
     */
    void addRole(const RoleName& role);

    /**
     * Adds the given role names to the list of roles that this user belongs to.
     */
    void addRoles(const std::vector<RoleName>& roles);

    /**
     * Adds the given privilege to the list of privileges this user is authorized for.
     */
    void addPrivilege(const Privilege& privilege);

    /**
     * Adds the given privileges to the list of privileges this user is authorized for.
     */
    void addPrivileges(const PrivilegeVector& privileges);

    /**
     * Marks this instance of the User object as invalid, most likely because information about
     * the user has been updated and needs to be reloaded from the AuthorizationManager.
     *
     * This method should *only* be called by the AuthorizationManager.
     */
    void invalidate();

    /**
     * Increments the reference count for this User object, which records how many threads have
     * a reference to it.
     *
     * This method should *only* be called by the AuthorizationManager.
     */
    void incrementRefCount();

    /**
     * Decrements the reference count for this User object, which records how many threads have
     * a reference to it.  Once the reference count goes to zero, the AuthorizationManager is
     * allowed to destroy this instance.
     *
     * This method should *only* be called by the AuthorizationManager.
     */
    void decrementRefCount();

private:
    UserName _name;

    // Maps resource name to privilege on that resource
    ResourcePrivilegeMap _privileges;

    // Roles the user has privileges from
    unordered_set<RoleName> _roles;

    // Roles that the user indirectly has privileges from, due to role inheritance.
    std::vector<RoleName> _indirectRoles;

    // Credential information.
    CredentialData _credentials;

    // _refCount and _isInvalidated are modified exclusively by the AuthorizationManager
    // _isInvalidated can be read by any consumer of User, but _refCount can only be
    // meaningfully read by the AuthorizationManager, as _refCount is guarded by the AM's _lock
    uint32_t _refCount;
    AtomicUInt32 _isValid;  // Using as a boolean
};

}  // namespace mongo
