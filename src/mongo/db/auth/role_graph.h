/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <algorithm>
#include <set>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

class OperationContext;

/**
 * A graph of role and privilege relationships.
 *
 * This structure is used to store an in-memory representation of the admin.system.roledata
 * collection, specifically the graph of which roles are members of other roles and what
 * privileges each role has, both directly and transitively through membership in other roles.
 * There are some restrictions on calls to getAllPrivileges(), specifically, one must call
 * recomputePrivilegeData() before calling getAllPrivileges() if any of the mutation methods
 * have been called on the instance since the later of its construction or the last call to
 * recomputePrivilegeData() on the object.
 */
class RoleGraph {
public:
    RoleGraph() = default;

    // Explicitly make RoleGraph movable
    RoleGraph(RoleGraph&&) = default;
    RoleGraph& operator=(RoleGraph&&) = default;

    /**
     * Adds to "privileges" the privileges associated with the named built-in role, and returns
     * true. Returns false if "role" does not name a built-in role, and does not modify
     * "privileges".  Addition of new privileges is done as with
     * Privilege::addPrivilegeToPrivilegeVector.
     */
    static bool addPrivilegesForBuiltinRole(const RoleName& role, PrivilegeVector* privileges);

    // Built-in roles for backwards compatibility with 2.2 and prior
    static const std::string BUILTIN_ROLE_V0_READ;
    static const std::string BUILTIN_ROLE_V0_READ_WRITE;
    static const std::string BUILTIN_ROLE_V0_ADMIN_READ;
    static const std::string BUILTIN_ROLE_V0_ADMIN_READ_WRITE;

    // Swaps the contents of this RoleGraph with those of "other"
    void swap(RoleGraph& other);

    /**
     * Adds to "privileges" the necessary privileges to do absolutely anything on the system.
     */
    static void generateUniversalPrivileges(PrivilegeVector* privileges);

    /**
     * Returns an iterator over the RoleNames of the "members" of the given role.
     * Members of a role are roles that have been granted this role directly (roles that are
     * members transitively through another role are not included).  These are the "parents" of
     * this node in the graph.
     */
    RoleNameIterator getDirectMembers(const RoleName& role);

    /**
     * Returns an iterator over the RoleNames of the "subordinates" of the given role.
     * Subordinate roles are the roles that this role has been granted directly (roles
     * that have been granted transitively through another role are not included).  These are
     * the "children" of this node in the graph.
     */
    RoleNameIterator getDirectSubordinates(const RoleName& role);

    /**
     * Returns an iterator that can be used to get a full list of roles that this role inherits
     * privileges from.  This includes its direct subordinate roles as well as the subordinates
     * of its subordinates, and so on.
     */
    RoleNameIterator getIndirectSubordinates(const RoleName& role);

    /**
     * Returns an iterator that can be used to get a full list of roles (in lexicographical
     * order) that are defined on the given database.
     */
    RoleNameIterator getRolesForDatabase(const std::string& dbname);

    /**
     * Returns a vector of the privileges that the given role has been directly granted.
     * Privileges that have been granted transitively through this role's subordinate roles are
     * not included.
     */
    const PrivilegeVector& getDirectPrivileges(const RoleName& role);

    /**
     * Returns a vector of all privileges that the given role contains.  This includes both the
     * privileges that have been granted to this role directly, as well as any privileges
     * inherited from the role's subordinate roles.
     */
    const PrivilegeVector& getAllPrivileges(const RoleName& role);

    /**
     * Returns the RestrictionDocument (if any) attached to the given role.
     * Restrictions applied transitively through this role's subordinate roles
     * are not included.
     */
    const SharedRestrictionDocument& getDirectAuthenticationRestrictions(const RoleName& role) {
        return _directRestrictionsForRole[role];
    }

    /**
     * Returns a vector of all restriction documents that the given role contains.
     * This includes both the restrictions set on this role directly,
     * as well as any restrictions inherited from the role's subordinate roles.
     */
    const std::vector<SharedRestrictionDocument>& getAllAuthenticationRestrictions(
        const RoleName& role) {
        return _allRestrictionsForRole[role];
    }

    /**
     * Returns whether or not the given role exists in the role graph.  Will implicitly
     * add the role to the graph if it is a built-in role and isn't already in the graph.
     */
    bool roleExists(const RoleName& role);

    /**
     * Returns whether the given role corresponds to a built-in role.
     */
    static bool isBuiltinRole(const RoleName& role);

    // Mutation functions

    /**
     * Puts an entry into the RoleGraph for the given RoleName.
     * Returns DuplicateKey if the role already exists.
     */
    Status createRole(const RoleName& role);

    /**
     * Deletes the given role by first removing it from the members/subordinates arrays for
     * all other roles, and then by removing its own entries in the 4 member maps.
     * Returns RoleNotFound if the role doesn't exist.
     * Returns InvalidRoleModification if "role" is a built-in role.
     */
    Status deleteRole(const RoleName& role);

    /**
     * Grants "role" to "recipient". This leaves "recipient" as a member of "role" and "role"
     * as a subordinate of "recipient".
     * Returns RoleNotFound if either of "role" or "recipient" doesn't exist in
     * the RoleGraph.
     * Returns InvalidRoleModification if "recipient" is a built-in role.
     */
    Status addRoleToRole(const RoleName& recipient, const RoleName& role);

    /**
     * Revokes "role" from "recipient".
     * Returns RoleNotFound if either of "role" or "recipient" doesn't exist in
     * the RoleGraph.  Returns RolesNotRelated if "recipient" is not currently a
     * member of "role".
     * Returns InvalidRoleModification if "role" is a built-in role.
     */
    Status removeRoleFromRole(const RoleName& recipient, const RoleName& role);

    /**
     * Removes all roles held by "victim".
     * Returns RoleNotFound if "victim" doesn't exist in the role graph.
     * Returns InvalidRoleModification if "victim" is a built-in role.
     */
    Status removeAllRolesFromRole(const RoleName& victim);

    /**
     * Grants "privilegeToAdd" to "role".
     * Returns RoleNotFound if "role" doesn't exist in the role graph.
     * Returns InvalidRoleModification if "role" is a built-in role.
     */
    Status addPrivilegeToRole(const RoleName& role, const Privilege& privilegeToAdd);

    /**
     * Grants Privileges from "privilegesToAdd" to "role".
     * Returns RoleNotFound if "role" doesn't exist in the role graph.
     * Returns InvalidRoleModification if "role" is a built-in role.
     */
    Status addPrivilegesToRole(const RoleName& role, const PrivilegeVector& privilegesToAdd);

    /**
     * Removes "privilegeToRemove" from "role".
     * Returns RoleNotFound if "role" doesn't exist in the role graph.
     * Returns PrivilegeNotFound if "role" doesn't contain the full privilege being removed.
     * Returns InvalidRoleModification if "role" is a built-in role.
     */
    Status removePrivilegeFromRole(const RoleName& role, const Privilege& privilegeToRemove);

    /**
     * Removes all privileges in the "privilegesToRemove" vector from "role".
     * Returns RoleNotFound if "role" doesn't exist in the role graph.
     * Returns InvalidRoleModification if "role" is a built-in role.
     * Returns PrivilegeNotFound if "role" is missing any of the privileges being removed.  If
     * PrivilegeNotFound is returned then the graph may be in an inconsistent state and needs to
     * be abandoned.
     */
    Status removePrivilegesFromRole(const RoleName& role,
                                    const PrivilegeVector& privilegesToRemove);

    /**
     * Removes all privileges from "role".
     * Returns RoleNotFound if "role" doesn't exist in the role graph.
     * Returns InvalidRoleModification if "role" is a built-in role.
     */
    Status removeAllPrivilegesFromRole(const RoleName& role);

    /**
     * Replace all restrictions on a role with a new Document
     * Returns RoleNotFound if "role" doesn't exist in the role graph.
     * Returns InvalidRoleModification if "role" is a built-in role.
     */
    Status replaceRestrictionsForRole(const RoleName& role, SharedRestrictionDocument restrictions);

    /**
     * Updates the RoleGraph by adding the role named "roleName", with the given role
     * memberships, privileges, and authentication restrictions.
     * If the name "roleName" already exists, it is replaced.
     * Any subordinate roles mentioned in role.roles are created, if needed,
     * with empty privilege, restriction, and subordinate role lists.
     *
     * Should _only_ fail if the role to replace is a builtin role, in which
     * case it will return ErrorCodes::InvalidRoleModification.
     */
    Status replaceRole(const RoleName& roleName,
                       const std::vector<RoleName>& roles,
                       const PrivilegeVector& privileges,
                       SharedRestrictionDocument restrictions);

    /**
     * Adds the role described in "doc" the role graph.
     */
    Status addRoleFromDocument(const BSONObj& doc);

    /**
     * Applies to the RoleGraph the oplog operation described by the parameters.
     *
     * Returns Status::OK() on success, ErrorCodes::OplogOperationUnsupported if the oplog
     * operation is not supported, and other codes (typically BadValue) if the oplog operation
     * is ill-described.
     */
    Status handleLogOp(OperationContext* opCtx,
                       const char* op,
                       const NamespaceString& ns,
                       const BSONObj& o,
                       const BSONObj* o2);

    /**
     * Recomputes the indirect (getAllPrivileges) data for this graph.
     *
     * Must be called between calls to any of the mutation functions and calls
     * to getAllPrivileges().
     *
     * Returns Status::OK() on success.  If a cycle is detected, returns
     * ErrorCodes::GraphContainsCycle, and the status message reveals the cycle.
     */
    Status recomputePrivilegeData();

private:
    // Helper method doing a topological DFS to compute the indirect privilege
    // data and look for cycles
    Status _recomputePrivilegeDataHelper(const RoleName& currentRole,
                                         unordered_set<RoleName>& visitedRoles);

    /**
     * If the role name given is not a built-in role, or it is but it's already in the role
     * graph, then this does nothing.  If it *is* a built-in role and this is the first time
     * this function has been called for this role, it will add the role into the role graph.
     */
    void _createBuiltinRoleIfNeeded(const RoleName& role);

    /**
     * Adds the built-in roles for the given database name to the role graph if they aren't
     * already present.
     */
    void _createBuiltinRolesForDBIfNeeded(const std::string& dbname);

    /**
     * Returns whether or not the given role exists strictly within the role graph.
     */
    bool _roleExistsDontCreateBuiltin(const RoleName& role);

    /**
     * Just creates the role in the role graph, without checking whether or not the role already
     * exists.
     */
    void _createRoleDontCheckIfRoleExists(const RoleName& role);

    /**
     * Grants "privilegeToAdd" to "role".
     * Doesn't do any checking as to whether the role exists or is a built-in role.
     */
    void _addPrivilegeToRoleNoChecks(const RoleName& role, const Privilege& privilegeToAdd);


    // Represents all the outgoing edges to other roles from any given role.
    using EdgeSet = unordered_map<RoleName, std::vector<RoleName>>;
    // Maps a role name to a list of privileges associated with that role.
    using RolePrivilegeMap = unordered_map<RoleName, PrivilegeVector>;

    // Maps a role name to a restriction document.
    using RestrictionDocumentMap = stdx::unordered_map<RoleName, SharedRestrictionDocument>;
    // Maps a role name to all restriction documents from self and subordinates.
    using RestrictionDocumentsMap =
        stdx::unordered_map<RoleName, std::vector<SharedRestrictionDocument>>;

    EdgeSet _roleToSubordinates;
    unordered_map<RoleName, unordered_set<RoleName>> _roleToIndirectSubordinates;
    EdgeSet _roleToMembers;
    RolePrivilegeMap _directPrivilegesForRole;
    RolePrivilegeMap _allPrivilegesForRole;
    RestrictionDocumentMap _directRestrictionsForRole;
    RestrictionDocumentsMap _allRestrictionsForRole;
    std::set<RoleName> _allRoles;
};

inline void swap(RoleGraph& lhs, RoleGraph& rhs) {
    lhs.swap(rhs);
}

}  // namespace mongo
