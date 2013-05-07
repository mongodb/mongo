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
 */

#pragma once

#include <algorithm>
#include <set>

#include "mongo/base/status.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

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
        RoleGraph();
        RoleGraph(const RoleGraph& other);
        ~RoleGraph();

        // Swaps the contents of this RoleGraph with those of "other"
        void swap(RoleGraph& other);

        /**
         * Returns an iterator that can be used to get a list of the members of the given role.
         * Members of a role are roles that have been granted this role directly (roles that are
         * members transitively through another role are not included).  These are the "parents" of
         * this node in the graph. The iterator is valid until the next call to addRole or
         * removeRole.
         */
        RoleNameIterator getDirectMembers(const RoleName& role) const;

        /**
         * Returns an iterator that can be used to get a list of "subordinate" roles of the given
         * role.  Subordinate roles are the roles that this role has been granted directly (roles
         * that have been granted transitively through another role are not included).  These are
         * the "children" of this node in the graph. The iterator is valid until the next call to
         * addRole or removeRole.
         */
        RoleNameIterator getDirectSubordinates(const RoleName& role) const;

        /**
         * Returns a vector of the privileges that the given role has been directly granted.
         * Privileges that have been granted transitively through this role's subordinate roles are
         * not included.
         */
        const PrivilegeVector& getDirectPrivileges(const RoleName& role) const;

        /**
         * Returns a vector of all privileges that the given role contains.  This includes both the
         * privileges that have been granted to this role directly, as well as any privileges
         * inherited from the role's subordinate roles.
         */
        const PrivilegeVector& getAllPrivileges(const RoleName& role) const;

        /**
         * Returns whether or not the given role exists in the role graph.
         */
        bool roleExists(const RoleName& role) const;

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
         */
        Status deleteRole(const RoleName& role);

        /**
         * Grants "role" to "recipient". This leaves "recipient" as a member of "role" and "role"
         * as a subordinate of "recipient".
         * Returns RoleNotFound if either of "role" or "recipient" doesn't exist in
         * the RoleGraph.
         */
        Status addRoleToRole(const RoleName& recipient, const RoleName& role);

        /**
         * Revokes "role" from "recipient".
         * Returns RoleNotFound if either of "role" or "recipient" doesn't exist in
         * the RoleGraph.  Returns RolesNotRelated if "recipient" is not currently a
         * member of "role".
         */
        Status removeRoleFromRole(const RoleName& recipient, const RoleName& role);

        /**
         * Grants "privilegeToAdd" to "role".
         * Returns RoleNotFound if "role" doesn't exist in the role graph.
         */
        Status addPrivilegeToRole(const RoleName& role, const Privilege& privilegeToAdd);

        /**
         * Grants Privileges from "privilegesToAdd" to "role".
         * Returns RoleNotFound if "role" doesn't exist in the role graph.
         */
        Status addPrivilegesToRole(const RoleName& role, const PrivilegeVector& privilegesToAdd);

        /**
         * Removes "privilegeToRemove" from "role".
         * Returns RoleNotFound if "role" doesn't exist in the role graph.
         * Returns PrivilegeNotFound if "role" doesn't contain the full privilege being removed.
         */
        Status removePrivilegeFromRole(const RoleName& role,
                                       const Privilege& privilegeToRemove);

        /**
         * Removes all privileges in the "privilegesToRemove" vector from "role".
         * Returns RoleNotFound if "role" doesn't exist in the role graph.
         * Returns PrivilegeNotFound if "role" is missing any of the privileges being removed.  If
         * PrivilegeNotFound is returned then the graph may be in an inconsistent state and needs to
         * be abandoned.
         */
        Status removePrivilegesFromRole(const RoleName& role,
                                        const PrivilegeVector& privilegesToRemove);

        /**
         * Removes all privileges from "role".
         * Returns RoleNotFound if "role" doesn't exist in the role graph.
         */
        Status removeAllPrivilegesFromRole(const RoleName& role);

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
        // Helper method for recursively doing a topological DFS to compute the indirect privilege
        // data and look for cycles
        Status _recomputePrivilegeDataHelper(const RoleName& currentRole,
                                             std::vector<RoleName>& inProgressRoles,
                                             unordered_set<RoleName>& visitedRoles);

        // Represents all the outgoing edges to other roles from any given role.
        typedef unordered_map<RoleName, unordered_set<RoleName> > EdgeSet;
        // Maps a role name to a list of privileges associated with that role.
        typedef unordered_map<RoleName, PrivilegeVector> RolePrivilegeMap;

        EdgeSet _roleToSubordinates;
        EdgeSet _roleToMembers;
        RolePrivilegeMap _directPrivilegesForRole;
        RolePrivilegeMap _allPrivilegesForRole;
    };

    void swap(RoleGraph& lhs, RoleGraph& rhs);

}  // namespace mongo
