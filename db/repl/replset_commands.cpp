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
#include "health.h"
#include "rs.h"
#include "rs_config.h"
#include "../dbwebserver.h"
#include "../../util/mongoutils/html.h"
#include "../../client/dbclient.h"

namespace mongo { 

    void checkMembersUpForConfigChange(const ReplSetConfig& cfg, bool initial);

    /* commands in other files:
         replSetHeartbeat - health.cpp
         replSetInitiate  - rs_mod.cpp
    */

    bool replSetBlind = false;

    class CmdReplSetTest : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "Just for testing : do not use.\n";
        }
        CmdReplSetTest() : ReplSetCommand("replSetTest") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) ) 
                return false;
            if( cmdObj.hasElement("blind") ) {
                replSetBlind = cmdObj.getBoolField("blind");
                log() << "replSet info replSetTest command received, replSetBlind=" << replSetBlind << rsLog;
                return true;
            }
            return false;
        }
    } cmdReplSetTest;

    class CmdReplSetGetRBID : public ReplSetCommand {
    public:
        int rbid;
        virtual void help( stringstream &help ) const {
            help << "internal";
        }
        CmdReplSetGetRBID() : ReplSetCommand("replSetGetRBID") { 
            rbid = (int) curTimeMillis();
        }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) ) 
                return false;
            result.append("rbid",rbid);
            return true;
        }
    } cmdReplSetRBID;

    using namespace bson;
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
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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
        virtual bool run(const string& a, BSONObj& b, string& errmsg, BSONObjBuilder& c, bool d) {
            try { 
                rwlock_try_write lk(mutex);
                return _run(a,b,errmsg,c,d);
            }
            catch(rwlock_try_write::exception&) { }
            errmsg = "a replSetReconfig is already in progress";
            return false;
        }
    private:
        bool _run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) ) 
                return false;
            if( !theReplSet->box.getState().primary() ) { 
                errmsg = "replSetReconfig command must be sent to the current replica set primary.";
                return false;
            }

            {
                // just make sure we can get a write lock before doing anything else.  we'll reacquire one 
                // later.  of course it could be stuck then, but this check lowers the risk if weird things 
                // are up - we probably don't want a change to apply 30 minutes after the initial attempt.
                time_t t = time(0);
                writelock lk("");
                if( time(0)-t > 20 ) {
                    errmsg = "took a long time to get write lock, so not initiating.  Initiate when server less busy?";
                    return false;
                }
            }

            if( cmdObj["replSetReconfig"].type() != Object ) {
                errmsg = "no configuration specified";
                return false;
            }

            /** TODO
                Support changes when a majority, but not all, members of a set are up.
                Determine what changes should not be allowed as they would cause erroneous states.
                What should be possible when a majority is not up?
                */
            try {
                ReplSetConfig newConfig(cmdObj["replSetReconfig"].Obj());

                log() << "replSet replSetReconfig config object parses ok, " << newConfig.members.size() << " members specified" << rsLog;

                if( !ReplSetConfig::legalChange(theReplSet->getConfig(), newConfig, errmsg) ) { 
                    return false;
                }

                checkMembersUpForConfigChange(newConfig,false);

                log() << "replSet replSetReconfig [2]" << rsLog;

                theReplSet->haveNewConfig(newConfig, true);
                ReplSet::startupStatusMsg = "replSetReconfig'd";
            }
            catch( DBException& e ) { 
                log() << "replSet replSetReconfig exception: " << e.what() << rsLog;
                throw;
            }

            return true;
        }
    } cmdReplSetReconfig;

    class CmdReplSetFreeze : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "Enable / disable failover for the set - locks current primary as primary even if issues occur.\nFor use during system maintenance.\n";
            help << "{ replSetFreeze : <bool> }";
            help << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }

        CmdReplSetFreeze() : ReplSetCommand("replSetFreeze") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            errmsg = "not yet implemented"; /*TODO*/
            return false;
        }
    } cmdReplSetFreeze;

    class CmdReplSetStepDown: public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "Step down as primary.  Will not try to reelect self or 1 minute.\n";
            help << "(If another member with same priority takes over in the meantime, it will stay primary.)\n";
            help << "http://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }

        CmdReplSetStepDown() : ReplSetCommand("replSetStepDown") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            if( !theReplSet->box.getState().primary() ) {
                errmsg = "not primary so can't step down";
                return false;
            }
            return theReplSet->stepDown();
        }
    } cmdReplSetStepDown;

    using namespace bson;
    using namespace mongoutils::html;
    extern void fillRsLog(stringstream&);

    class ReplSetHandler : public DbWebHandler {
    public:
        ReplSetHandler() : DbWebHandler( "_replSet" , 1 , true ){}

        virtual bool handles( const string& url ) const {
            return startsWith( url , "/_replSet" );
        }

        virtual void handle( const char *rq, string url, 
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ){
            
            string s = str::after(url, "/_replSetOplog?");
            if( !s.empty() )
                responseMsg = _replSetOplog(s);
            else
                responseMsg = _replSet();
            responseCode = 200;
        }


        string _replSetOplog(string parms) { 
            stringstream s;
            string t = "Replication oplog";
            s << start(t);
            s << p(t);

            if( theReplSet == 0 ) { 
                if( cmdLine._replSet.empty() ) 
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://www.mongodb.org/display/DOCS/Replica+Set+Configuration#InitialSetup", "", "initiated") 
                           + ".<br>" + ReplSet::startupStatusMsg);
                }
            }
            else {
                try {
                    theReplSet->getOplogDiagsAsHtml(stringToNum(parms.c_str()), s);
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
                    a("/replSetGetStatus?text", "", "replSetGetStatus") + " | " +
                    a("http://www.mongodb.org/display/DOCS/Replica+Sets", "", "Docs")
                  );

            if( theReplSet == 0 ) { 
                if( cmdLine._replSet.empty() ) 
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://www.mongodb.org/display/DOCS/Replica+Set+Configuration#InitialSetup", "", "initiated") 
                           + ".<br>" + ReplSet::startupStatusMsg);
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
