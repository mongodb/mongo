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
#include "../repl.h"
#include "health.h"
#include "rs.h"
#include "rs_config.h"
#include "../dbwebserver.h"
#include "../../util/mongoutils/html.h"
#include "../repl_block.h"

using namespace bson;

namespace mongo {

    void checkMembersUpForConfigChange(const ReplSetConfig& cfg, BSONObjBuilder& result, bool initial);

    /* commands in other files:
         replSetHeartbeat - health.cpp
         replSetInitiate  - rs_mod.cpp
    */

    bool replSetBlind = false;
    unsigned replSetForceInitialSyncFailure = 0;

    class CmdReplSetTest : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "Just for regression tests.\n";
        }
        CmdReplSetTest() : ReplSetCommand("replSetTest") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log() << "replSet replSetTest command received: " << cmdObj.toString() << rsLog;

            if (!checkAuth(errmsg, result)) {
                return false;
            }

            if( cmdObj.hasElement("forceInitialSyncFailure") ) {
                replSetForceInitialSyncFailure = (unsigned) cmdObj["forceInitialSyncFailure"].Number();
                return true;
            }

            if( !check(errmsg, result) )
                return false;

            if( cmdObj.hasElement("blind") ) {
                replSetBlind = cmdObj.getBoolField("blind");
                return true;
            }

            if (cmdObj.hasElement("sethbmsg")) {
                replset::sethbmsg(cmdObj["sethbmsg"].String());
                return true;
            }

            return false;
        }
    } cmdReplSetTest;

    /** get rollback id.  used to check if a rollback happened during some interval of time.
        as consumed, the rollback id is not in any particular order, it simply changes on each rollback.
        @see incRBID()
    */
    class CmdReplSetGetRBID : public ReplSetCommand {
    public:
        /* todo: ideally this should only change on rollbacks NOT on mongod restarts also. fix... */
        int rbid;
        virtual void help( stringstream &help ) const {
            help << "internal";
        }
        CmdReplSetGetRBID() : ReplSetCommand("replSetGetRBID") {
            // this is ok but micros or combo with some rand() and/or 64 bits might be better --
            // imagine a restart and a clock correction simultaneously (very unlikely but possible...)
            rbid = (int) curTimeMillis64();
        }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            result.append("rbid",rbid);
            return true;
        }
    } cmdReplSetRBID;

    /** we increment the rollback id on every rollback event. */
    void incRBID() {
        cmdReplSetRBID.rbid++;
    }

    /** helper to get rollback id from another server. */
    int getRBID(DBClientConnection *c) {
        bo info;
        c->simpleCommand("admin", &info, "replSetGetRBID");
        return info["rbid"].numberInt();
    }

    class CmdReplSetGetStatus : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "Report status of a replica set from the POV of this server\n";
            help << "{ replSetGetStatus : 1 }";
            help << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }
        CmdReplSetGetStatus() : ReplSetCommand("replSetGetStatus", true) { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if ( cmdObj["forShell"].trueValue() )
                lastError.disableForCommand();

            if( !check(errmsg, result) )
                return false;
            theReplSet->summarizeStatus(result);
            return true;
        }
    } cmdReplSetGetStatus;

    class CmdReplSetReconfig : public ReplSetCommand {
        RWLock mutex; /* we don't need rw but we wanted try capability. :-( */
    public:
        virtual void help( stringstream &help ) const {
            help << "Adjust configuration of a replica set\n";
            help << "{ replSetReconfig : config_object }";
            help << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }
        CmdReplSetReconfig() : ReplSetCommand("replSetReconfig"), mutex("rsreconfig") { }
        virtual bool run(const string& a, BSONObj& b, int e, string& errmsg, BSONObjBuilder& c, bool d) {
            try {
                rwlock_try_write lk(mutex);
                return _run(a,b,e,errmsg,c,d);
            }
            catch(rwlock_try_write::exception&) { }
            errmsg = "a replSetReconfig is already in progress";
            return false;
        }
    private:
        bool _run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if ( !checkAuth(errmsg, result) ) {
                return false;
            }

            if( cmdObj["replSetReconfig"].type() != Object ) {
                errmsg = "no configuration specified";
                return false;
            }

            bool force = cmdObj.hasField("force") && cmdObj["force"].trueValue();
            if( force && !theReplSet ) {
                replSettings.reconfig = cmdObj["replSetReconfig"].Obj().getOwned();
                result.append("msg", "will try this config momentarily, try running rs.conf() again in a few seconds");
                return true;
            }

            if ( !check(errmsg, result) ) {
                return false;
            }

            if( !force && !theReplSet->box.getState().primary() ) {
                errmsg = "replSetReconfig command must be sent to the current replica set primary.";
                return false;
            }

            {
                // just make sure we can get a write lock before doing anything else.  we'll reacquire one
                // later.  of course it could be stuck then, but this check lowers the risk if weird things
                // are up - we probably don't want a change to apply 30 minutes after the initial attempt.
                time_t t = time(0);
                Lock::GlobalWrite lk;
                if( time(0)-t > 20 ) {
                    errmsg = "took a long time to get write lock, so not initiating.  Initiate when server less busy?";
                    return false;
                }
            }

            try {
                ReplSetConfig newConfig(cmdObj["replSetReconfig"].Obj(), force);

                log() << "replSet replSetReconfig config object parses ok, " << newConfig.members.size() << " members specified" << rsLog;

                if( !ReplSetConfig::legalChange(theReplSet->getConfig(), newConfig, errmsg) ) {
                    return false;
                }

                checkMembersUpForConfigChange(newConfig, result, false);

                log() << "replSet replSetReconfig [2]" << rsLog;

                theReplSet->haveNewConfig(newConfig, true);
                ReplSet::startupStatusMsg.set("replSetReconfig'd");
            }
            catch( DBException& e ) {
                log() << "replSet replSetReconfig exception: " << e.what() << rsLog;
                throw;
            }
            catch( string& se ) {
                log() << "replSet reconfig exception: " << se << rsLog;
                errmsg = se;
                return false;
            }

            resetSlaveCache();
            return true;
        }
    } cmdReplSetReconfig;

    class CmdReplSetFreeze : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "{ replSetFreeze : <seconds> }";
            help << "'freeze' state of member to the extent we can do that.  What this really means is that\n";
            help << "this node will not attempt to become primary until the time period specified expires.\n";
            help << "You can call again with {replSetFreeze:0} to unfreeze sooner.\n";
            help << "A process restart unfreezes the member also.\n";
            help << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }

        CmdReplSetFreeze() : ReplSetCommand("replSetFreeze") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            int secs = (int) cmdObj.firstElement().numberInt();
            if( theReplSet->freeze(secs) ) {
                if( secs == 0 )
                    result.append("info","unfreezing");
            }
            if( secs == 1 )
                result.append("warning", "you really want to freeze for only 1 second?");
            return true;
        }
    } cmdReplSetFreeze;

    class CmdReplSetStepDown: public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "{ replSetStepDown : <seconds> }\n";
            help << "Step down as primary.  Will not try to reelect self for the specified time period (1 minute if no numeric secs value specified).\n";
            help << "(If another member with same priority takes over in the meantime, it will stay primary.)\n";
            help << "http://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }

        CmdReplSetStepDown() : ReplSetCommand("replSetStepDown") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            if( !theReplSet->box.getState().primary() ) {
                errmsg = "not primary so can't step down";
                return false;
            }

            bool force = cmdObj.hasField("force") && cmdObj["force"].trueValue();

            // only step down if there is another node synced to within 10
            // seconds of this node
            if (!force) {
                long long int lastOp = (long long int)theReplSet->lastOpTimeWritten.getSecs();
                long long int closest = (long long int)theReplSet->lastOtherOpTime().getSecs();

                long long int diff = lastOp - closest;
                result.append("closest", closest);
                result.append("difference", diff);

                if (diff < 0) {
                    // not our problem, but we'll wait until thing settle down
                    errmsg = "someone is ahead of the primary?";
                    return false;
                }

                if (diff > 10) {
                    errmsg = "no secondaries within 10 seconds of my optime";
                    return false;
                }
            }

            int secs = (int) cmdObj.firstElement().numberInt();
            if( secs == 0 )
                secs = 60;
            return theReplSet->stepDown(secs);
        }
    } cmdReplSetStepDown;

    class CmdReplSetMaintenance: public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "{ replSetMaintenance : bool }\n";
            help << "Enable or disable maintenance mode.";
        }

        CmdReplSetMaintenance() : ReplSetCommand("replSetMaintenance") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            if( theReplSet->box.getState().primary() ) {
                errmsg = "primaries can't modify maintenance mode";
                return false;
            }

            theReplSet->setMaintenanceMode(cmdObj["replSetMaintenance"].trueValue());
            return true;
        }
    } cmdReplSetMaintenance;

    class CmdReplSetSyncFrom: public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "{ replSetSyncFrom : \"host:port\" }\n";
            help << "Change who this member is syncing from.";
        }

        CmdReplSetSyncFrom() : ReplSetCommand("replSetSyncFrom") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if (!checkAuth(errmsg, result) || !check(errmsg, result)) {
                return false;
            }

            string newTarget = cmdObj["replSetSyncFrom"].valuestrsafe();
            result.append("syncFromRequested", newTarget);
            return theReplSet->forceSyncFrom(newTarget, errmsg, result);
        }
    } cmdReplSetSyncFrom;

    using namespace bson;
    using namespace mongoutils::html;
    extern void fillRsLog(stringstream&);

    class ReplSetHandler : public DbWebHandler {
    public:
        ReplSetHandler() : DbWebHandler( "_replSet" , 1 , true ) {}

        virtual bool handles( const string& url ) const {
            return startsWith( url , "/_replSet" );
        }

        virtual void handle( const char *rq, string url, BSONObj params,
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ) {

            if( url == "/_replSetOplog" ) {
                responseMsg = _replSetOplog(params);
            }
            else
                responseMsg = _replSet();
            responseCode = 200;
        }

        string _replSetOplog(bo parms) {
            int _id = (int) str::toUnsigned( parms["_id"].String() );

            stringstream s;
            string t = "Replication oplog";
            s << start(t);
            s << p(t);

            if( theReplSet == 0 ) {
                if( cmdLine._replSet.empty() )
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://www.mongodb.org/display/DOCS/Replica+Set+Configuration#InitialSetup", "", "initiated")
                           + ".<br>" + ReplSet::startupStatusMsg.get());
                }
            }
            else {
                try {
                    theReplSet->getOplogDiagsAsHtml(_id, s);
                }
                catch(std::exception& e) {
                    s << "error querying oplog: " << e.what() << '\n';
                }
            }

            s << _end();
            return s.str();
        }

        /* /_replSet show replica set status in html format */
        string _replSet() {
            stringstream s;
            s << start("Replica Set Status " + prettyHostName());
            s << p( a("/", "back", "Home") + " | " +
                    a("/local/system.replset/?html=1", "", "View Replset Config") + " | " +
                    a("/replSetGetStatus?text=1", "", "replSetGetStatus") + " | " +
                    a("http://www.mongodb.org/display/DOCS/Replica+Sets", "", "Docs")
                  );

            if( theReplSet == 0 ) {
                if( cmdLine._replSet.empty() )
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://www.mongodb.org/display/DOCS/Replica+Set+Configuration#InitialSetup", "", "initiated")
                           + ".<br>" + ReplSet::startupStatusMsg.get());
                }
            }
            else {
                try {
                    theReplSet->summarizeAsHtml(s);
                }
                catch(...) { s << "error summarizing replset status\n"; }
            }
            s << p("Recent replset log activity:");
            fillRsLog(s);
            s << _end();
            return s.str();
        }



    } replSetHandler;

}
