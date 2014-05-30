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

#include <boost/thread/mutex.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/stdx/functional.h"

namespace mongo {

    /**
     * Common implementation of AuthzManagerExternalState for systems where role
     * and user information are stored locally.
     */
    class AuthzManagerExternalStateLocal : public AuthzManagerExternalState {
        MONGO_DISALLOW_COPYING(AuthzManagerExternalStateLocal);

    public:
        virtual ~AuthzManagerExternalStateLocal();

        virtual Status initialize();

        virtual Status getStoredAuthorizationVersion(OperationContext* txn, int* outVersion);
        virtual Status getUserDescription(
                            OperationContext* txn, const UserName& userName, BSONObj* result);
        virtual Status getRoleDescription(const RoleName& roleName,
                                          bool showPrivileges,
                                          BSONObj* result);
        virtual Status getRoleDescriptionsForDB(const std::string dbname,
                                                bool showPrivileges,
                                                bool showBuiltinRoles,
                                                std::vector<BSONObj>* result);

        virtual void logOp(
                const char* op,
                const char* ns,
                const BSONObj& o,
                BSONObj* o2,
                bool* b);

    protected:
        AuthzManagerExternalStateLocal();

    private:
        enum RoleGraphState {
            roleGraphStateInitial = 0,
            roleGraphStateConsistent,
            roleGraphStateHasCycle
        };

        /**
         * Initializes the role graph from the contents of the admin.system.roles collection.
         */
        Status _initializeRoleGraph();

        /**
         * Fetches the user document for "userName" from local storage, and stores it into "result".
         */
        virtual Status _getUserDocument(OperationContext* txn,
                                        const UserName& userName,
                                        BSONObj* result) = 0;

        Status _getRoleDescription_inlock(const RoleName& roleName,
                                          bool showPrivileges,
                                          BSONObj* result);
        /**
         * Eventually consistent, in-memory representation of all roles in the system (both
         * user-defined and built-in).  Synchronized via _roleGraphMutex.
         */
        RoleGraph _roleGraph;

        /**
         * State of _roleGraph, one of "initial", "consistent" and "has cycle".  Synchronized via
         * _roleGraphMutex.
         */
        RoleGraphState _roleGraphState;

        /**
         * Guards _roleGraphState and _roleGraph.
         */
        boost::mutex _roleGraphMutex;

        boost::timed_mutex _authzDataUpdateLock;
    };

} // namespace mongo
