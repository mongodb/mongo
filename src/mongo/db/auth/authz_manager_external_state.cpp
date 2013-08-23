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

#include "mongo/db/auth/authz_manager_external_state.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AuthzManagerExternalState::AuthzManagerExternalState() {}
    AuthzManagerExternalState::~AuthzManagerExternalState() {}

    Status AuthzManagerExternalState::getPrivilegeDocument(const UserName& userName,
                                                           int authzVersion,
                                                           BSONObj* result) {
        if (userName == internalSecurity.user->getName()) {
            return Status(ErrorCodes::InternalError,
                          "Requested privilege document for the internal user");
        }

        StringData dbname = userName.getDB();

        // Make sure the dbname is actually a database
        if (dbname == StringData("$external", StringData::LiteralTag()) ||
            dbname == AuthorizationManager::SERVER_RESOURCE_NAME ||
            dbname == AuthorizationManager::CLUSTER_RESOURCE_NAME) {
            return Status(ErrorCodes::UserNotFound,
                          mongoutils::str::stream() << "No privilege documents stored in the " <<
                          dbname << " user source.");
        }

        if (!NamespaceString::validDBName(dbname)) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Bad database name \"" << dbname << "\"");
        }

        // Build the query needed to get the privilege document
        std::string usersNamespace;
        BSONObjBuilder queryBuilder;
        if (authzVersion == 1) {
            usersNamespace = mongoutils::str::stream() << dbname << ".system.users";
            queryBuilder.append(AuthorizationManager::V1_USER_NAME_FIELD_NAME, userName.getUser());
            queryBuilder.appendNull(AuthorizationManager::V1_USER_SOURCE_FIELD_NAME);
        } else if (authzVersion == 2) {
            usersNamespace = "admin.system.users";
            queryBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME, userName.getUser());
            queryBuilder.append(AuthorizationManager::USER_SOURCE_FIELD_NAME, userName.getDB());
        } else {
            return Status(ErrorCodes::UnsupportedFormat,
                          mongoutils::str::stream() <<
                                  "Unrecognized authorization format version: " << authzVersion);
        }

        // Query for the privilege document
        BSONObj userBSONObj;
        Status found = _findUser(usersNamespace, queryBuilder.obj(), &userBSONObj);
        if (!found.isOK()) {
            if (found.code() == ErrorCodes::UserNotFound) {
                // Return more detailed status that includes user name.
                return Status(ErrorCodes::UserNotFound,
                              mongoutils::str::stream() << "auth: couldn't find user " <<
                                      userName.toString() << ", " << usersNamespace,
                              0);
            } else {
                return found;
            }
        }

        *result = userBSONObj.getOwned();
        return Status::OK();
    }

    bool AuthzManagerExternalState::hasAnyPrivilegeDocuments() {
        std::string usersNamespace = "admin.system.users";

        BSONObj userBSONObj;
        BSONObj query;
        return _findUser(usersNamespace, query, &userBSONObj).isOK();
    }

}  // namespace mongo
