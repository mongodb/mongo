/* @file rs_mod.cpp code related to modifications of the replica set's configuration 
   such as set initiation
   */

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

#include "pch.h"
#include "../cmdline.h"
#include "../commands.h"
#include "../../util/mmap.h"
#include "health.h"
#include "replset.h"
#include "rs_config.h"

using namespace bson;

namespace mongo { 

    /* throws */ 
    static void checkAllMembersUpAndPreInit(const ReplSetConfig& cfg) {
        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = cfg.members.begin(); i != cfg.members.end(); i++ ) {
            BSONObj res;
            if( !requestHeartbeat(cfg._id, i->h.toString(), res) )
                uasserted(13144, "need all members up to initiate, not ok: " + i->h.toString());
            if( res.getBoolField("mismatch") )
                uasserted(13145, "set names do not match with: " + i->h.toString());
            if( *res.getStringField("set") )
                uasserted(13256, "member " + i->h.toString() + " is already initiated");
        }
    }

    class CmdReplSetInitiate : public Command { 
    public:
        virtual LockType locktype() const { return WRITE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual bool logTheOp() { return false; }
        CmdReplSetInitiate() : Command("replSetInitiate") { }
        virtual void help(stringstream& h) const { 
            h << "Initiate/christen a replica set."; 
            h << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log() << "replSet replSetInitiate admin command received from client" << rsLog;

            if( !replSet ) { 
                errmsg = "server is not running with --replSet";
                return false;
            }
            if( theReplSet ) {
                errmsg = "already initialized";
                result.append("info", "try querying " + rsConfigNs + "");
                return false;
            }            
            if( ReplSet::startupStatus == ReplSet::BADCONFIG ) {
                errmsg = "server already in BADCONFIG state (check logs); not initiating";
                result.append("info", ReplSet::startupStatusMsg);
                return false;
            }
            if( ReplSet::startupStatus != ReplSet::EMPTYCONFIG ) {
                result.append("startupStatus", ReplSet::startupStatus);
                errmsg = "all members and seeds must be reachable to initiate set";
                result.append("info", cmdLine.replSet);
                return false;
            }

            if( cmdObj["replSetInitiate"].type() != Object ) {
                errmsg = "no configuration specified";
                return false;
            }

            ReplSetConfig newConfig(cmdObj["replSetInitiate"].Obj());

            log() << "replSet replSetInitiate config object parses ok, " << newConfig.members.size() << " members specified" << rsLog;

            checkAllMembersUpAndPreInit(newConfig);

            log() << "replSet replSetInitiate all members seem up" << rsLog;

            log() << newConfig.toString() << rsLog;

            MemoryMappedFile::flushAll(true);
            newConfig.save();
            MemoryMappedFile::flushAll(true);

            log() << "replSet replSetInitiate Config now saved locally.  Should come online in about a minute." << rsLog;
            result.append("info", "Config now saved locally.  Should come online in about a minute.");

            return true;
        }
    } cmdReplSetInitiate;

}
