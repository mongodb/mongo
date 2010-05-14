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
#include "../../util/mongoutils/str.h"
#include "health.h"
#include "replset.h"
#include "rs_config.h"

using namespace bson;
using namespace mongoutils;

namespace mongo { 

    /* throws */ 
    static void checkAllMembersUpForConfigChange(const ReplSetConfig& cfg) {
        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = cfg.members.begin(); i != cfg.members.end(); i++ ) {
            BSONObj res;
            {
                bool ok = false;
                try { 
                    int theirVersion = -1000;
                    ok = requestHeartbeat(cfg._id, i->h.toString(), res, -1, theirVersion); 
                    if( theirVersion >= cfg.version ) { 
                        stringstream ss;
                        ss << "replSet member " << i->h.toString() << " has too new a config version (" << theirVersion << ") to reconfigure";
                        uasserted(13259, ss.str());
                    }
                }
                catch(...) { }
                if( !ok && !res["rs"].trueValue() ) {
                    if( !res.isEmpty() )
                        log() << "replSet warning " << i->h.toString() << " replied: " << res.toString() << rsLog;
                    uasserted(13144, "need all members up to initiate, not ok: " + i->h.toString());
                }
            }
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
                result.append("startupStatusMsg", ReplSet::startupStatusMsg);
                errmsg = "all members and seeds must be reachable to initiate set";
                result.append("info", cmdLine.replSet);
                return false;
            }

            if( cmdObj["replSetInitiate"].type() != Object ) {
                errmsg = "no configuration specified";
                return false;
            }

            try {
                ReplSetConfig newConfig(cmdObj["replSetInitiate"].Obj());

                log() << "replSet replSetInitiate config object parses ok, " << newConfig.members.size() << " members specified" << rsLog;

                checkAllMembersUpForConfigChange(newConfig);

                log() << "replSet replSetInitiate all members seem up" << rsLog;

                //log() << newConfig.toString() << rsLog;

                string z = str::after("abcde", 'c');

                newConfig.saveConfigEverywhere( theReplSet->memberHostnames() );
            }
            catch( DBException& e ) { 
                log() << "replSet replSetInitiate exception: " << e.what() << rsLog;
                throw;
            }

            log() << "replSet replSetInitiate Config now saved locally.  Should come online in about a minute." << rsLog;
            result.append("info", "Config now saved locally.  Should come online in about a minute.");
            ReplSet::startupStatus = ReplSet::SOON;
            ReplSet::startupStatusMsg = "Received replSetInitiate - should come online shortly.";

            return true;
        }
    } cmdReplSetInitiate;

    /*void ReplSet::receivedNewConfig(BSONObj cfg) { 
        writelock lk("admin.");
        ReplSetConfig c(cfg);
        if( c.version <= config().version ) { 
            log() << "replSet info received new config v" << c.version << " but our v" << config().version << " is already newer" << rsLog;
            return;
        }
    }*/

}
