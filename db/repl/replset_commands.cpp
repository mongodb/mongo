/**
*    Copyright (C) 2008 10gen Inc.
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

#include "stdafx.h"
#include "../cmdline.h"
#include "replset.h"
#include "health.h"
#include "../commands.h"

namespace mongo { 

    /* commands in other files:
         replSetHeartbeat - health.cpp
         */

    class CmdReplSetGetStatus : public Command {
    public:
        virtual bool slaveOk() {
            return true;
        }
        virtual bool adminOnly() {
            return true;
        }
        virtual bool logTheOp() {
            return false;   
        }
        virtual LockType locktype(){ return NONE; }
        CmdReplSetGetStatus() : Command("replSetGetStatus") { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            return true;
        }
    } cmdReplSetGetStatus;

}
