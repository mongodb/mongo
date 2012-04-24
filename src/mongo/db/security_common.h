// security_common.h

/**
*    Copyright (C) 2009 10gen Inc.
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

#include "commands.h"
#include "../util/concurrency/spin_lock.h"

namespace mongo {

    /**
     * Internal secret key info.
     */
    struct AuthInfo {
        AuthInfo() {
            user = "__system";
        }
        string user;
        string pwd;
    };

    // --noauth cmd line option
    extern bool noauth;
    extern AuthInfo internalSecurity;

    /**
     * This method checks the validity of filename as a security key, hashes its
     * contents, and stores it in the internalSecurity variable.  Prints an
     * error message to the logs if there's an error.
     * @param filename the file containing the key
     * @return if the key was successfully stored
     */
    bool setUpSecurityKey(const string& filename);

    class CmdAuthenticate : public Command {
    public:
        virtual bool requiresAuth() { return false; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual LockType locktype() const { return NONE; }
        virtual void help(stringstream& ss) const { ss << "internal"; }
        CmdAuthenticate() : Command("authenticate") {}
        bool run(const string& dbname , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool fromRepl);
        void authenticate(const string& dbname, const string& user, const bool readOnly);
    private:
        bool getUserObj(const string& dbname, const string& user, BSONObj& userObj, string& pwd);
    };
    
    extern CmdAuthenticate cmdAuthenticate;

    class CmdLogout : public Command {
    public:
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        void help(stringstream& h) const { h << "de-authenticate"; }
        virtual LockType locktype() const { return NONE; }
        CmdLogout() : Command("logout") {}
        bool run(const string& dbname , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool fromRepl);
    };

} // namespace mongo
