/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"

namespace mongo {

using std::string;
using std::stringstream;

class CmdConnectionStatus : public Command {
public:
    CmdConnectionStatus() : Command("connectionStatus") {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}  // No auth required

    void help(stringstream& h) const {
        h << "Returns connection-specific information such as logged-in users and their roles";
    }

    bool run(OperationContext* txn,
             const string&,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        AuthorizationSession* authSession = AuthorizationSession::get(ClientBasic::getCurrent());

        bool showPrivileges;
        Status status =
            bsonExtractBooleanFieldWithDefault(cmdObj, "showPrivileges", false, &showPrivileges);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        BSONObjBuilder authInfo(result.subobjStart("authInfo"));
        {
            BSONArrayBuilder authenticatedUsers(authInfo.subarrayStart("authenticatedUsers"));
            UserNameIterator nameIter = authSession->getAuthenticatedUserNames();

            for (; nameIter.more(); nameIter.next()) {
                BSONObjBuilder userInfoBuilder(authenticatedUsers.subobjStart());
                userInfoBuilder.append(AuthorizationManager::USER_NAME_FIELD_NAME,
                                       nameIter->getUser());
                userInfoBuilder.append(AuthorizationManager::USER_DB_FIELD_NAME, nameIter->getDB());
            }
        }
        {
            BSONArrayBuilder authenticatedRoles(authInfo.subarrayStart("authenticatedUserRoles"));
            RoleNameIterator roleIter = authSession->getAuthenticatedRoleNames();

            for (; roleIter.more(); roleIter.next()) {
                BSONObjBuilder roleInfoBuilder(authenticatedRoles.subobjStart());
                roleInfoBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME,
                                       roleIter->getRole());
                roleInfoBuilder.append(AuthorizationManager::ROLE_DB_FIELD_NAME, roleIter->getDB());
            }
        }
        if (showPrivileges) {
            BSONArrayBuilder authenticatedPrivileges(
                authInfo.subarrayStart("authenticatedUserPrivileges"));

            // Create a unified map of resources to privileges, to avoid duplicate
            // entries in the connection status output.
            User::ResourcePrivilegeMap unifiedResourcePrivilegeMap;
            UserNameIterator nameIter = authSession->getAuthenticatedUserNames();

            for (; nameIter.more(); nameIter.next()) {
                User* authUser = authSession->lookupUser(*nameIter);
                const User::ResourcePrivilegeMap& resourcePrivilegeMap = authUser->getPrivileges();
                for (User::ResourcePrivilegeMap::const_iterator it = resourcePrivilegeMap.begin();
                     it != resourcePrivilegeMap.end();
                     ++it) {
                    if (unifiedResourcePrivilegeMap.find(it->first) ==
                        unifiedResourcePrivilegeMap.end()) {
                        unifiedResourcePrivilegeMap[it->first] = it->second;
                    } else {
                        unifiedResourcePrivilegeMap[it->first].addActions(it->second.getActions());
                    }
                }
            }

            for (User::ResourcePrivilegeMap::const_iterator it =
                     unifiedResourcePrivilegeMap.begin();
                 it != unifiedResourcePrivilegeMap.end();
                 ++it) {
                authenticatedPrivileges << it->second.toBSON();
            }
        }

        authInfo.doneFast();

        return true;
    }
} cmdConnectionStatus;
}
