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

namespace mongo { 

    void checkAllMembersUpForConfigChange(const ReplSetConfig& cfg, bool initial);

    /* commands in other files:
         replSetHeartbeat - health.cpp
         replSetInitiate  - rs_mod.cpp
    */

    using namespace bson;

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
    public:
        virtual void help( stringstream &help ) const {
            help << "Adjust configuration of a replica set\n";
            help << "{ replSetReconfig : config_object }";
            help << "\nhttp://www.mongodb.org/display/DOCS/Replica+Set+Commands";
        }
        CmdReplSetReconfig() : ReplSetCommand("replSetReconfig") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) ) 
                return false;
            if( !theReplSet->isPrimary() ) { 
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

                checkAllMembersUpForConfigChange(newConfig,false);

                log() << "replSet replSetReconfig all members seem up" << rsLog;

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

        CmdReplSetFreeze() : ReplSetCommand("replSetFreeze", true) { }
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

        CmdReplSetStepDown() : ReplSetCommand("replSetStepDown", true) { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            if( !theReplSet->isPrimary() ) {
                errmsg = "not primary so can't step down";
                return false;
            }
            return theReplSet->stepDown();
        }
    } cmdReplSetStepDown;

}
