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

#include "mongo/db/auth/role_graph.h"

#include <algorithm>
#include <vector>

#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
    PrivilegeVector emptyPrivilegeVector;
    std::vector<RoleName> emptyRoleVector;

    // RoleNameIterator for iterating over an unordered_set of RoleNames.
    class RoleNameSetIterator : public RoleNameIterator::Impl {
        MONGO_DISALLOW_COPYING(RoleNameSetIterator);

    public:
        RoleNameSetIterator(const unordered_set<RoleName>::const_iterator& begin,
                            const unordered_set<RoleName>::const_iterator& end) :
                _begin(begin), _end(end) {}

        virtual ~RoleNameSetIterator() {}

        virtual bool more() const {
            return _begin != _end;
        }

        virtual const RoleName& next() {
            const RoleName& toReturn = get();
            ++_begin;
            return toReturn;
        }


        virtual const RoleName& get() const {
            return *_begin;
        }

    private:
        virtual Impl* doClone() const {
            return new RoleNameSetIterator(_begin, _end);
        }

        unordered_set<RoleName>::const_iterator _begin;
        unordered_set<RoleName>::const_iterator _end;
    };

} // namespace

    RoleGraph::RoleGraph() {};
    RoleGraph::RoleGraph(const RoleGraph& other) : _roleToSubordinates(other._roleToSubordinates),
            _roleToIndirectSubordinates(other._roleToIndirectSubordinates),
            _roleToMembers(other._roleToMembers),
            _directPrivilegesForRole(other._directPrivilegesForRole),
            _allPrivilegesForRole(other._allPrivilegesForRole) {}
    RoleGraph::~RoleGraph() {};

    void RoleGraph::swap(RoleGraph& other) {
        using std::swap;
        swap(this->_roleToSubordinates, other._roleToSubordinates);
        swap(this->_roleToIndirectSubordinates, other._roleToIndirectSubordinates);
        swap(this->_roleToMembers, other._roleToMembers);
        swap(this->_directPrivilegesForRole, other._directPrivilegesForRole);
        swap(this->_allPrivilegesForRole, other._allPrivilegesForRole);
    }

    void swap(RoleGraph& lhs, RoleGraph& rhs) {
        lhs.swap(rhs);
    }

    bool RoleGraph::roleExists(const RoleName& role) {
        _createBuiltinRoleIfNeeded(role);
        return _roleExistsDontCreateBuiltin(role);
    }

    bool RoleGraph::_roleExistsDontCreateBuiltin(const RoleName& role) {
        EdgeSet::const_iterator edgeIt = _roleToSubordinates.find(role);
        if (edgeIt == _roleToSubordinates.end())
            return false;
        edgeIt = _roleToMembers.find(role);
        massert(16825,
                "Role found in forward edges but not all reverse edges map, this should not "
                     "be possible",
                edgeIt != _roleToMembers.end());

        RolePrivilegeMap::const_iterator strIt = _directPrivilegesForRole.find(role);
        if (strIt == _directPrivilegesForRole.end())
            return false;
        strIt = _allPrivilegesForRole.find(role);
        massert(16826,
                "Role found in direct privileges map but not all privileges map, this should not "
                        "be possible",
                strIt != _allPrivilegesForRole.end());
        return true;
    }

    Status RoleGraph::createRole(const RoleName& role) {
        if (roleExists(role)) {
            return Status(ErrorCodes::DuplicateKey,
                          mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                " already exists",
                          0);
        }

        _createRoleDontCheckIfRoleExists(role);
        return Status::OK();
    }

    void RoleGraph::_createRoleDontCheckIfRoleExists(const RoleName& role) {
        // Just reference the role in all the maps so that an entry gets created with empty
        // containers for the value.
        _roleToSubordinates[role];
        _roleToIndirectSubordinates[role];
        _roleToMembers[role];
        _directPrivilegesForRole[role];
        _allPrivilegesForRole[role];
    }

    Status RoleGraph::deleteRole(const RoleName& role) {
        if (!roleExists(role)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                  " does not exist",
                          0);
        }
        if (isBuiltinRole(role)) {
            return Status(ErrorCodes::InvalidRoleModification,
                          mongoutils::str::stream() << "Cannot delete built-in role: " <<
                                  role.getFullName(),
                          0);
        }

        for (std::vector<RoleName>::iterator it = _roleToSubordinates[role].begin();
                it != _roleToSubordinates[role].end(); ++it) {
            _roleToMembers[*it].erase(std::find(_roleToMembers[*it].begin(),
                                                _roleToMembers[*it].end(),
                                                role));
        }
        for (std::vector<RoleName>::iterator it = _roleToMembers[role].begin();
                it != _roleToMembers[role].end(); ++it) {
            _roleToSubordinates[*it].erase(std::find(_roleToSubordinates[*it].begin(),
                                                     _roleToSubordinates[*it].end(),
                                                     role));
        }
        _roleToSubordinates.erase(role);
        _roleToIndirectSubordinates.erase(role);
        _roleToMembers.erase(role);
        _directPrivilegesForRole.erase(role);
        _allPrivilegesForRole.erase(role);
        return Status::OK();
    }

    const std::vector<RoleName>& RoleGraph::getDirectSubordinates(const RoleName& role) {
        if (!roleExists(role))
            return emptyRoleVector;
        return _roleToSubordinates.find(role)->second;
    }

    RoleNameIterator RoleGraph::getIndirectSubordinates(const RoleName& role) {
        if (!roleExists(role))
            return RoleNameIterator(NULL);
        const unordered_set<RoleName>& subs = _roleToIndirectSubordinates.find(role)->second;
        return RoleNameIterator(new RoleNameSetIterator(subs.begin(), subs.end()));
    }

    const std::vector<RoleName>& RoleGraph::getDirectMembers(const RoleName& role) {
        if (!roleExists(role))
            return emptyRoleVector;
        return _roleToMembers.find(role)->second;
    }

    const PrivilegeVector& RoleGraph::getDirectPrivileges(const RoleName& role) {
        if (!roleExists(role))
            return emptyPrivilegeVector;
        return _directPrivilegesForRole.find(role)->second;
    }

    const PrivilegeVector& RoleGraph::getAllPrivileges(const RoleName& role) {
        if (!roleExists(role))
            return emptyPrivilegeVector;
        return _allPrivilegesForRole.find(role)->second;
    }

    Status RoleGraph::addRoleToRole(const RoleName& recipient, const RoleName& role) {
        if (!roleExists(recipient)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << recipient.getFullName() <<
                                " does not exist");
        }
        if (isBuiltinRole(recipient)) {
            return Status(ErrorCodes::InvalidRoleModification,
                          mongoutils::str::stream() << "Cannot grant roles to built-in role: " <<
                                  role.getFullName());
        }
        if (!roleExists(role)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                " does not exist");
        }

        if (std::find(_roleToSubordinates[recipient].begin(),
                      _roleToSubordinates[recipient].end(),
                      role) ==
                _roleToSubordinates[recipient].end()) {
            // Only add role if it's not already present
            _roleToSubordinates[recipient].push_back(role);
            _roleToMembers[role].push_back(recipient);
        }

        return Status::OK();
    }

    Status RoleGraph::removeRoleFromRole(const RoleName& recipient, const RoleName& role) {
        if (!roleExists(recipient)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << recipient.getFullName() <<
                                " does not exist",
                          0);
        }
        if (isBuiltinRole(recipient)) {
            return Status(ErrorCodes::InvalidRoleModification,
                          mongoutils::str::stream() << "Cannot remove roles from built-in role: " <<
                                  role.getFullName(),
                          0);
        }
        if (!roleExists(role)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                " does not exist",
                          0);
        }

        std::vector<RoleName>::iterator itToRm = std::find(_roleToMembers[role].begin(),
                                                           _roleToMembers[role].end(),
                                                           recipient);
        if (itToRm != _roleToMembers[role].end()) {
            _roleToMembers[role].erase(itToRm);
        } else {
            return Status(ErrorCodes::RolesNotRelated,
                          mongoutils::str::stream() << recipient.getFullName() << " is not a member"
                                  " of " << role.getFullName(),
                          0);
        }

        itToRm = std::find(_roleToSubordinates[recipient].begin(),
                           _roleToSubordinates[recipient].end(),
                           role);
        massert(16827,
                mongoutils::str::stream() << role.getFullName() << " is not a subordinate"
                        " of " << recipient.getFullName() << ", even though " <<
                        recipient.getFullName() << " is a member of " << role.getFullName() <<
                        ". This shouldn't be possible",
                itToRm != _roleToSubordinates[recipient].end());
        _roleToSubordinates[recipient].erase(itToRm);
        return Status::OK();
    }

    Status RoleGraph::addPrivilegeToRole(const RoleName& role, const Privilege& privilegeToAdd) {
        if (!roleExists(role)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                " does not exist",
                          0);
        }
        if (isBuiltinRole(role)) {
            return Status(ErrorCodes::InvalidRoleModification,
                          mongoutils::str::stream() << "Cannot grant privileges to built-in role: "
                                  << role.getFullName(),
                          0);
        }

        _addPrivilegeToRoleNoChecks(role, privilegeToAdd);
        return Status::OK();
    }

    void RoleGraph::_addPrivilegeToRoleNoChecks(const RoleName& role,
                                                const Privilege& privilegeToAdd) {
        Privilege::addPrivilegeToPrivilegeVector(&_directPrivilegesForRole[role], privilegeToAdd);
    }

    // NOTE: Current runtime of this is O(n*m) where n is the size of the current PrivilegeVector
    // for the given role, and m is the size of the privilegesToAdd vector.
    // If this was a PrivilegeSet (sorted on resource) rather than a PrivilegeVector, we
    // could do this in O(n+m) instead.
    Status RoleGraph::addPrivilegesToRole(const RoleName& role,
                                          const PrivilegeVector& privilegesToAdd) {
        if (!roleExists(role)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                " does not exist",
                          0);
        }
        if (isBuiltinRole(role)) {
            return Status(ErrorCodes::InvalidRoleModification,
                          mongoutils::str::stream() << "Cannot grant privileges to built-in role: "
                                  << role.getFullName(),
                          0);
        }

        for (PrivilegeVector::const_iterator it = privilegesToAdd.begin();
                it != privilegesToAdd.end(); ++it) {
            Status status = addPrivilegeToRole(role, *it);
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    Status RoleGraph::removePrivilegeFromRole(const RoleName& role,
                                              const Privilege& privilegeToRemove) {
        if (!roleExists(role)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                " does not exist",
                          0);
        }
        if (isBuiltinRole(role)) {
            return Status(
                    ErrorCodes::InvalidRoleModification,
                    mongoutils::str::stream() << "Cannot remove privileges from built-in role: " <<
                            role.getFullName());
        }

        PrivilegeVector& currentPrivileges = _directPrivilegesForRole[role];
        for (PrivilegeVector::iterator it = currentPrivileges.begin();
                it != currentPrivileges.end(); ++it) {

            Privilege& curPrivilege = *it;
            if (curPrivilege.getResourcePattern() == privilegeToRemove.getResourcePattern()) {
                ActionSet curActions = curPrivilege.getActions();

                if (!curActions.isSupersetOf(privilegeToRemove.getActions())) {
                    // Didn't possess all the actions being removed.
                    return Status(ErrorCodes::PrivilegeNotFound,
                                  mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                          " does not contain a privilege on " <<
                                          privilegeToRemove.getResourcePattern().toString() <<
                                          " with actions: " <<
                                          privilegeToRemove.getActions().toString(),
                                  0);
                }

                curPrivilege.removeActions(privilegeToRemove.getActions());
                if (curPrivilege.getActions().empty()) {
                    currentPrivileges.erase(it);
                }
                return Status::OK();
            }
        }
        return Status(ErrorCodes::PrivilegeNotFound,
                      mongoutils::str::stream() << "Role: " << role.getFullName() << " does not "
                      "contain any privileges on " <<
                      privilegeToRemove.getResourcePattern().toString(),
                      0);
    }

    Status RoleGraph::removePrivilegesFromRole(const RoleName& role,
                                               const PrivilegeVector& privilegesToRemove) {
        for (PrivilegeVector::const_iterator it = privilegesToRemove.begin();
                it != privilegesToRemove.end(); ++it) {
            Status status = removePrivilegeFromRole(role, *it);
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    Status RoleGraph::removeAllPrivilegesFromRole(const RoleName& role) {
        if (!roleExists(role)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << role.getFullName() <<
                                " does not exist",
                          0);
        }
        if (isBuiltinRole(role)) {
            return Status(
                    ErrorCodes::InvalidRoleModification,
                    mongoutils::str::stream() << "Cannot remove privileges from built-in role: " <<
                            role.getFullName());
        }
        _directPrivilegesForRole[role].clear();
        return Status::OK();
    }

    Status RoleGraph::recomputePrivilegeData() {
        /*
         * This method is used to recompute the "allPrivileges" vector for each node in the graph,
         * as well as look for cycles.  It is implemented by performing a depth-first traversal of
         * the dependency graph, once for each node.  "visitedRoles" tracks the set of role names
         * ever visited, and it is used to prune each DFS.  A node that has been visited once on any
         * DFS is never visited again.  Complexity of this implementation is O(n+m) where "n" is the
         * number of nodes and "m" is the number of prerequisite edges.  Space complexity is O(n),
         * in both stack space and size of the "visitedRoles" set.
         *
         * "inProgressRoles" is used to detect and report cycles.
         */

        std::vector<RoleName> inProgressRoles;
        unordered_set<RoleName> visitedRoles;

        for (EdgeSet::const_iterator it = _roleToSubordinates.begin();
                it != _roleToSubordinates.end(); ++it) {
            Status status = _recomputePrivilegeDataHelper(it->first, inProgressRoles, visitedRoles);
            if (status != Status::OK()) {
                return status;
            }
        }

        return Status::OK();
    }

    /*
     * Recursive helper method for performing the depth-first traversal of the roles graph.  Called
     * once for every node in the graph by recomputePrivilegeData(), above.
     */
    Status RoleGraph::_recomputePrivilegeDataHelper(const RoleName& currentRole,
                                                    std::vector<RoleName>& inProgressRoles,
                                                    unordered_set<RoleName>& visitedRoles) {

        if (visitedRoles.count(currentRole)) {
            return Status::OK();
        }

        if (!roleExists(currentRole)) {
            return Status(ErrorCodes::RoleNotFound,
                          mongoutils::str::stream() << "Role: " << currentRole.getFullName() <<
                                " does not exist",
                          0);
        }

        // Check for cycles
        std::vector<RoleName>::iterator firstOccurence = std::find(
                inProgressRoles.begin(), inProgressRoles.end(), currentRole);
        if (firstOccurence != inProgressRoles.end()) {
            std::ostringstream os;
            os << "Cycle in dependency graph: ";
            for (std::vector<RoleName>::iterator it = firstOccurence;
                    it != inProgressRoles.end(); ++it) {
                os << it->getFullName() << " -> ";
            }
            os << currentRole.getFullName();
            return Status(ErrorCodes::GraphContainsCycle, os.str());
        }

        inProgressRoles.push_back(currentRole);

        // Need to clear out the "all privileges" vector for the current role, and re-fill it with
        // just the direct privileges for this role.
        PrivilegeVector& currentRoleAllPrivileges = _allPrivilegesForRole[currentRole];
        currentRoleAllPrivileges = _directPrivilegesForRole[currentRole];

        // Need to do the same thing for the indirect roles
        unordered_set<RoleName>& currentRoleIndirectRoles =
                _roleToIndirectSubordinates[currentRole];
        currentRoleIndirectRoles.clear();
        const std::vector<RoleName>& currentRoleDirectRoles = _roleToSubordinates[currentRole];
        for (std::vector<RoleName>::const_iterator it = currentRoleDirectRoles.begin();
                it != currentRoleDirectRoles.end(); ++it) {
            currentRoleIndirectRoles.insert(*it);
        }

        // Recursively add children's privileges to current role's "all privileges" vector, and
        // children's roles to current roles's "indirect roles" vector.
        for (std::vector<RoleName>::const_iterator roleIt = currentRoleDirectRoles.begin();
                roleIt != currentRoleDirectRoles.end(); ++roleIt) {
            const RoleName& childRole = *roleIt;
            Status status = _recomputePrivilegeDataHelper(childRole, inProgressRoles, visitedRoles);
            if (status != Status::OK()) {
                return status;
            }

            // At this point, we know that the "all privilege" set for the child is correct, so
            // add those privileges to our "all privilege" set.
            const PrivilegeVector& childsPrivileges = _allPrivilegesForRole[childRole];
            for (PrivilegeVector::const_iterator privIt = childsPrivileges.begin();
                    privIt != childsPrivileges.end(); ++privIt) {
                Privilege::addPrivilegeToPrivilegeVector(&currentRoleAllPrivileges, *privIt);
            }

            // We also know that the "indirect roles" for the child is also correct, so we can add
            // those roles to our "indirect roles" set.
            const unordered_set<RoleName>& childsRoles = _roleToIndirectSubordinates[childRole];
            for (unordered_set<RoleName>::const_iterator childsRoleIt = childsRoles.begin();
                    childsRoleIt != childsRoles.end(); ++childsRoleIt) {
                currentRoleIndirectRoles.insert(*childsRoleIt);
            }
        }

        if (inProgressRoles.back() != currentRole)
            return Status(ErrorCodes::InternalError, "inProgressRoles stack corrupt");
        inProgressRoles.pop_back();
        visitedRoles.insert(currentRole);
        return Status::OK();
    }


} // namespace mongo
