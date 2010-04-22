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

    class CmdReplSetInitiate : public Command { 
    public:
        virtual LockType locktype(){ return WRITE; }
        virtual bool slaveOk() { return true; }
        virtual bool adminOnly() { return true; }
        virtual bool logTheOp() { return false; }
        CmdReplSetInitiate() : Command("replSetInitiate") { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !replSet ) { 
                errmsg = "not running with --replSet";
                return false;
            }
            if( theReplSet ) {
                errmsg = "already initialized";
                return false;
            }            
            if( ReplSet::startupStatus == ReplSet::BADCONFIG ) {
                errmsg = "config already exists, but is bad";
                return false;
            }
            if( ReplSet::startupStatus != ReplSet::EMPTYCONFIG ) {
                result.append("startupStatus", ReplSet::startupStatus);
                errmsg = "all seed hosts must be reachable to initiate set";
                return false;
            }

            return true;
        }
    } cmdReplSetInitiate;

    /* commands in other files:
         replSetHeartbeat - health.cpp
         */

    class CmdReplSetGetStatus : public Command {
    public:
        virtual bool slaveOk() { return true; }
        virtual bool adminOnly() { return true; }
        virtual bool logTheOp() { return false; }
        virtual LockType locktype(){ return NONE; }
        CmdReplSetGetStatus() : Command("replSetGetStatus", true) { }
        virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !replSet ) { 
                errmsg = "not running with --replSet";
                return false;
            }
            if( theReplSet == 0 ) {
                result.append("startupState", ReplSet::startupStatus);
                errmsg = ReplSet::startupStatusMsg.empty() ? 
                    errmsg = "replset unknown error 1" : ReplSet::startupStatusMsg;
                return false;
            }

            theReplSet->summarizeStatus(result);

            return true;
        }
    } cmdReplSetGetStatus;

}
