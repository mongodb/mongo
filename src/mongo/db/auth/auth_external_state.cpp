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
*/

#include "mongo/db/auth/auth_external_state.h"

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"

namespace mongo {

    AuthExternalState::AuthExternalState() {}
    AuthExternalState::~AuthExternalState() {}

    Status AuthExternalState::getPrivilegeDocumentOverConnection(DBClientBase* conn,
                                                                 const std::string& dbname,
                                                                 const std::string& principalName,
                                                                 BSONObj* result) {
        if (principalName == internalSecurity.user) {
            if (internalSecurity.pwd.empty()) {
                return Status(ErrorCodes::UserNotFound,
                              "key file must be used to log in with internal user",
                              15889);
            }
            *result = BSON("user" << principalName << "pwd" << internalSecurity.pwd).getOwned();
            return Status::OK();
        }

        std::string usersNamespace = dbname + ".system.users";

        BSONObj userBSONObj;
        BSONObj query = BSON("user" << principalName);
        userBSONObj = conn->findOne(usersNamespace, query, 0, QueryOption_SlaveOk);
        if (userBSONObj.isEmpty()) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream() << "auth: couldn't find user " << principalName
                                                    << ", " << usersNamespace,
                          0);
        }

        *result = userBSONObj.getOwned();
        return Status::OK();
    }

}  // namespace mongo
