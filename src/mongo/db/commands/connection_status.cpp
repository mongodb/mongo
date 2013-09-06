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

#include <mongo/pch.h>

#include "mongo/db/auth/authorization_session.h"
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
            AuthorizationSession* authSession =
                    ClientBasic::getCurrent()->getAuthorizationSession();

            BSONObjBuilder authInfo(result.subobjStart("authInfo"));
            {
                BSONArrayBuilder authenticatedUsers(authInfo.subarrayStart("authenticatedUsers"));

                UserSet::NameIterator nameIter = authSession->getAuthenticatedUserNames();
                for ( ; nameIter.more(); nameIter.next()) {
                    BSONObjBuilder userInfoBuilder(authenticatedUsers.subobjStart());
                    userInfoBuilder.append("user", nameIter->getUser());
                    userInfoBuilder.append("userSource", nameIter->getDB());
                    userInfoBuilder.doneFast();
                }
                authenticatedUsers.doneFast();
            }
            authInfo.doneFast();

            return true;
        }
    } cmdConnectionStatus;
}
