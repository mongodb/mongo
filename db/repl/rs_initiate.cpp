/* @file rs_initiate.cpp
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
#include "rs.h"
#include "rs_config.h"
#include "../dbhelpers.h"

using namespace bson;
using namespace mongoutils;

namespace mongo { 

    /* called on a reconfig AND on initiate
       throws 
       @param initial true when initiating
    */
    void checkMembersUpForConfigChange(const ReplSetConfig& cfg, bool initial) {
        int failures = 0;
        int me = 0;
        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = cfg.members.begin(); i != cfg.members.end(); i++ ) {
            if( ListeningSockets::listeningOn(i->h) ) {
                me++;
                if( !i->potentiallyHot() ) { 
                    uasserted(13420, "initiation and reconfiguration of a replica set must be sent to a node that can become primary");
                }
            }
        }
        uassert(13278, "bad config - dups?", me <= 1); // dups?
        uassert(13279, "can't find self in the replset config", me == 1);

        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = cfg.members.begin(); i != cfg.members.end(); i++ ) {
            BSONObj res;
            {
                bool ok = false;
                try {
                    int theirVersion = -1000;
                    ok = requestHeartbeat(cfg._id, "", i->h.toString(), res, -1, theirVersion, initial/*check if empty*/); 
                    if( theirVersion >= cfg.version ) { 
                        stringstream ss;
                        ss << "replSet member " << i->h.toString() << " has too new a config version (" << theirVersion << ") to reconfigure";
                        uasserted(13259, ss.str());
                    }
                }
                catch(DBException& e) { 
                    log() << "replSet cmufcc requestHeartbeat " << i->h.toString() << " : " << e.toString() << rsLog;
                }
                catch(...) { 
                    log() << "replSet cmufcc error exception in requestHeartbeat?" << rsLog;
                }
                if( res.getBoolField("mismatch") )
                    uasserted(13145, "set name does not match the set name host " + i->h.toString() + " expects");
                if( *res.getStringField("set") ) {
                    if( cfg.version <= 1 ) {
                        // this was to be initiation, no one shoudl be initiated already.
                        uasserted(13256, "member " + i->h.toString() + " is already initiated");
                    }
                    else {
                        // Assure no one has a newer config.
                        if( res["v"].Int() >= cfg.version ) {
                            uasserted(13341, "member " + i->h.toString() + " has a config version >= to the new cfg version; cannot change config");
                        }
                    }
                }
                if( !ok && !res["rs"].trueValue() ) {
                    if( !res.isEmpty() ) {
                        /* strange.  got a response, but not "ok". log it. */
                        log() << "replSet warning " << i->h.toString() << " replied: " << res.toString() << rsLog;
                    }

                    bool allowFailure = false;
                    failures++;
                    if( res.isEmpty() && !initial && failures == 1 ) {
                        /* for now we are only allowing 1 node to be down on a reconfig.  this can be made to be a minority
                           trying to keep change small as release is near.
                           */
                        const Member* m = theReplSet->findById( i->_id );
                        if( m ) { 
                            // ok, so this was an existing member (wouldn't make sense to add to config a new member that is down)
                            assert( m->h().toString() == i->h.toString() );
                            allowFailure = true;
                        }
                    }

                    if( !allowFailure ) {
                        string msg = string("need members up to initiate, not ok : ") + i->h.toString();
                        if( !initial )
                            msg = string("need most members up to reconfigure, not ok : ") + i->h.toString();
                        uasserted(13144, msg);
                    }
                }
            }
            if( initial ) {
                bool hasData = res["hasData"].Bool();
                uassert(13311, "member " + i->h.toString() + " has data already, cannot initiate set.  All members except initiator must be empty.", 
                    !hasData || ListeningSockets::listeningOn(i->h));
            }
        }
    }

    class CmdReplSetInitiate : public ReplSetCommand { 
    public:
        virtual LockType locktype() const { return NONE; }
        CmdReplSetInitiate() : ReplSetCommand("replSetInitiate") { }
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
                result.append("info", "try querying " + rsConfigNs + " to see current configuration");
                return false;
            }

            {
                // just make sure we can get a write lock before doing anything else.  we'll reacquire one 
                // later.  of course it could be stuck then, but this check lowers the risk if weird things 
                // are up.
                time_t t = time(0);
                writelock lk("");
                if( time(0)-t > 10 ) { 
                    errmsg = "took a long time to get write lock, so not initiating.  Initiate when server less busy?";
                    return false;
                }

                /* check that we don't already have an oplog.  that could cause issues.
                   it is ok if the initiating member has *other* data than that.
                   */
                BSONObj o;
                if( Helpers::getFirst(rsoplog, o) ) { 
                    errmsg = rsoplog + string(" is not empty on the initiating member.  cannot initiate.");
                    return false;
                }
            }

            if( ReplSet::startupStatus == ReplSet::BADCONFIG ) {
                errmsg = "server already in BADCONFIG state (check logs); not initiating";
                result.append("info", ReplSet::startupStatusMsg);
                return false;
            }
            if( ReplSet::startupStatus != ReplSet::EMPTYCONFIG ) {
                result.append("startupStatus", ReplSet::startupStatus);
                errmsg = "all members and seeds must be reachable to initiate set";
                result.append("info", cmdLine._replSet);
                return false;
            }

            BSONObj configObj;

            if( cmdObj["replSetInitiate"].type() != Object ) {
                result.append("info2", "no configuration explicitly specified -- making one");
                log() << "replSet info initiate : no configuration specified.  Using a default configuration for the set" << rsLog;

                string name;
                vector<HostAndPort> seeds;
                set<HostAndPort> seedSet;
                parseReplsetCmdLine(cmdLine._replSet, name, seeds, seedSet); // may throw...

                bob b;
                b.append("_id", name);
                bob members;
                members.append("0", BSON( "_id" << 0 << "host" << HostAndPort::Me().toString() ));
                for( unsigned i = 0; i < seeds.size(); i++ )
                    members.append(bob::numStr(i+1), BSON( "_id" << i+1 << "host" << seeds[i].toString()));
                b.appendArray("members", members.obj());
                configObj = b.obj();
                log() << "replSet created this configuration for initiation : " << configObj.toString() << rsLog;
            }
            else { 
                configObj = cmdObj["replSetInitiate"].Obj();
            }

            bool parsed = false;
            try {
                ReplSetConfig newConfig(configObj);
                parsed = true;

                if( newConfig.version > 1 ) { 
                    errmsg = "can't initiate with a version number greater than 1";
                    return false;
                }

                log() << "replSet replSetInitiate config object parses ok, " << newConfig.members.size() << " members specified" << rsLog;

                checkMembersUpForConfigChange(newConfig, true);

                log() << "replSet replSetInitiate all members seem up" << rsLog;

                writelock lk("");
                bo comment = BSON( "msg" << "initiating set");
                newConfig.saveConfigLocally(comment);
                log() << "replSet replSetInitiate config now saved locally.  Should come online in about a minute." << rsLog;
                result.append("info", "Config now saved locally.  Should come online in about a minute.");
                ReplSet::startupStatus = ReplSet::SOON;
                ReplSet::startupStatusMsg = "Received replSetInitiate - should come online shortly.";
            }
            catch( DBException& e ) { 
                log() << "replSet replSetInitiate exception: " << e.what() << rsLog;
                if( !parsed ) 
                    errmsg = string("couldn't parse cfg object ") + e.what();
                else
                    errmsg = string("couldn't initiate : ") + e.what();
                return false;
            }

            return true;
        }
    } cmdReplSetInitiate;

}
