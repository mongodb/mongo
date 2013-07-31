/*
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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authz_manager_external_state.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    /**
     * Mock of the AuthzManagerExternalState class used only for testing.
     */
    class AuthzManagerExternalStateMock : public AuthzManagerExternalState {
        MONGO_DISALLOW_COPYING(AuthzManagerExternalStateMock);

    public:

        AuthzManagerExternalStateMock() {};

        // no-op for the mock
        virtual Status insertPrivilegeDocument(const std::string& dbname,
                                               const BSONObj& userObj) const;

        // no-op for the mock
        virtual Status updatePrivilegeDocument(const UserName& user,
                                               const BSONObj& updateObj) const;

        // no-op for the mock
        virtual Status removePrivilegeDocuments(const std::string& dbname,
                                                const BSONObj& query) const;

        // Non-const version that puts document into a vector that can be accessed later
        Status insertPrivilegeDocument(const std::string& dbname, const BSONObj& userObj);

        void clearPrivilegeDocuments();

        virtual Status getAllDatabaseNames(std::vector<std::string>* dbnames) const;

        virtual Status getAllV1PrivilegeDocsForDB(const std::string& dbname,
                                                  std::vector<BSONObj>* privDocs) const;

        virtual Status _findUser(const std::string& usersNamespace,
                                 const BSONObj& query,
                                 BSONObj* result) const;


    private:
        unordered_map<std::string, std::vector<BSONObj> > _userDocuments; // dbname to user docs
    };

} // namespace mongo
