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

#pragma once

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
        bool run(const string& dbname , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool fromRepl);
 
    private:
        bool authenticateCR(const string& dbname, 
                            BSONObj& cmdObj, 
                            string& errmsg, 
                            BSONObjBuilder& result);
        bool authenticateX509(const string& dbname, 
                              BSONObj& cmdObj, 
                              string& errmsg, 
                              BSONObjBuilder& result);
    };

    extern CmdAuthenticate cmdAuthenticate;
}


