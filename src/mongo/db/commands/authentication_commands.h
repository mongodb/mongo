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

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands.h"

namespace mongo {

    class CmdAuthenticate : public Command {
    public:
        static void disableCommand();

        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual LockType locktype() const { return NONE; }
        virtual void help(stringstream& ss) const { ss << "internal"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        CmdAuthenticate() : Command("authenticate") {}
        bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl);

    private:
        /**
         * Completes the authentication of "user" using "mechanism" and parameters from "cmdObj".
         *
         * Returns Status::OK() on success.  All other statuses indicate failed authentication.  The
         * entire status returned here may always be used for logging.  However, if the code is
         * AuthenticationFailed, the "reason" field of the return status may contain information
         * that should not be revealed to the connected client.
         *
         * Other than AuthenticationFailed, common returns are BadValue, indicating unsupported
         * mechanism, and ProtocolError, indicating an error in the use of the authentication
         * protocol.
         */
        Status _authenticate(const std::string& mechanism,
                             const UserName& user,
                             const BSONObj& cmdObj);
        Status _authenticateCR(const UserName& user, const BSONObj& cmdObj);
        Status _authenticateX509(const UserName& user, const BSONObj& cmdObj);
    };

    extern CmdAuthenticate cmdAuthenticate;
}


