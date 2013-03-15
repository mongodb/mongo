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
 */

#include <mongo/pch.h>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/commands.h"

namespace mongo {
    class CmdConnectionStatus : public Command {
    public:
        CmdConnectionStatus() : Command("connectionStatus") {}
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required

        void help(stringstream& h) const {
            h << "Returns connection-specific information such as logged-in users";
        }

        bool run(const string&, BSONObj& cmdObj, int, string& errmsg,
                 BSONObjBuilder& result, bool fromRepl) {
            AuthorizationManager* authMgr = ClientBasic::getCurrent()->getAuthorizationManager();

            BSONObjBuilder authInfo(result.subobjStart("authInfo"));
            {
                BSONArrayBuilder authenticatedUsers(authInfo.subarrayStart("authenticatedUsers"));

                PrincipalSet::NameIterator nameIter = authMgr->getAuthenticatedPrincipalNames();
                for ( ; nameIter.more(); nameIter.next()) {
                    BSONObjBuilder principal(authenticatedUsers.subobjStart());
                    principal.append("user", nameIter->getUser());
                    principal.append("userSource", nameIter->getDB());
                    principal.doneFast();
                }
                authenticatedUsers.doneFast();
            }
            authInfo.doneFast();

            return true;
        }
    } cmdConnectionStatus;
}
