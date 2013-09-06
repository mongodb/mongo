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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/health.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_server_status.h"  // replSettings
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/db/repl/write_concern.h"

using namespace bson;

namespace mongo {

    void checkMembersUpForConfigChange(const ReplSetConfig& cfg, BSONObjBuilder& result, bool initial);

    /* commands in other files:
         replSetHeartbeat - health.cpp
         replSetInitiate  - rs_mod.cpp
    */

    bool replSetBlind = false;
    unsigned replSetForceInitialSyncFailure = 0;

    // Testing only, enabled via command-line.
    class CmdReplSetTest : public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "Just for regression tests.\n";
        }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        CmdReplSetTest() : ReplSetCommand("replSetTest") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            log() << "replSet replSetTest command received: " << cmdObj.toString() << rsLog;

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
    };
    MONGO_INITIALIZER(RegisterReplSetTestCmd)(InitializerContext* context) {
        if (Command::testCommandsEnabled) {
            // Leaked intentionally: a Command registers itself when constructed.
            new CmdReplSetTest();
        }
        return Status::OK();
    }

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
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetGetRBID);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
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
            help << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetGetStatus);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
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
            help << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetReconfig);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
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
                scoped_ptr<ReplSetConfig> newConfig
                        (ReplSetConfig::make(cmdObj["replSetReconfig"].Obj(), force));

                log() << "replSet replSetReconfig config object parses ok, " <<
                        newConfig->members.size() << " members specified" << rsLog;

                if( !ReplSetConfig::legalChange(theReplSet->getConfig(), *newConfig, errmsg) ) {
                    return false;
                }

                checkMembersUpForConfigChange(*newConfig, result, false);

                log() << "replSet replSetReconfig [2]" << rsLog;

                theReplSet->haveNewConfig(*newConfig, true);
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
            help << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetFreeze);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
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
            help << "http://dochub.mongodb.org/core/replicasetcommands";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetStepDown);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
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
            // seconds of this node which can potentially become primary
            if (!force) {
                long long int lastOp = static_cast<long long int>(
                                        theReplSet->lastOpTimeWritten.getSecs());
                long long int closest = static_cast<long long int>(
                                        theReplSet->lastOtherElectableOpTime().getSecs());

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
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetMaintenance);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        CmdReplSetMaintenance() : ReplSetCommand("replSetMaintenance") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;

            if (!theReplSet->setMaintenanceMode(cmdObj["replSetMaintenance"].trueValue())) {
                if (theReplSet->isPrimary()) {
                    errmsg = "primaries can't modify maintenance mode";
                }
                else {
                    errmsg = "already out of maintenance mode";
                }
                return false;
            }

            return true;
        }
    } cmdReplSetMaintenance;

    class CmdReplSetSyncFrom: public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "{ replSetSyncFrom : \"host:port\" }\n";
            help << "Change who this member is syncing from.";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetSyncFrom);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        CmdReplSetSyncFrom() : ReplSetCommand("replSetSyncFrom") { }
        virtual bool run(const string&, 
                         BSONObj& cmdObj, 
                         int, 
                         string& errmsg, 
                         BSONObjBuilder& result, 
                         bool fromRepl) {
            if (!check(errmsg, result)) {
                return false;
            }
            string newTarget = cmdObj["replSetSyncFrom"].valuestrsafe();
            result.append("syncFromRequested", newTarget);
            return theReplSet->forceSyncFrom(newTarget, errmsg, result);
        }
    } cmdReplSetSyncFrom;

    class CmdReplSetUpdatePosition: public ReplSetCommand {
    public:
        virtual void help( stringstream &help ) const {
            help << "internal";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::replSetUpdatePosition);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        CmdReplSetUpdatePosition() : ReplSetCommand("replSetUpdatePosition") { }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg,
                         BSONObjBuilder& result, bool fromRepl) {
            if (!check(errmsg, result))
                return false;

            if (cmdObj.hasField("handshake")) {
                // we have received a handshake, not an update message
                // handshakes are done here to ensure the receiving end supports the update command
                cc().gotHandshake(cmdObj["handshake"].embeddedObject());
                // if we aren't primary, pass the handshake along
                if (!theReplSet->isPrimary() && theReplSet->syncSourceFeedback.supportsUpdater()) {
                    theReplSet->syncSourceFeedback.forwardSlaveHandshake();
                }
                return true;
            }

            uassert(16888, "optimes field should be an array with an object for each secondary",
                    cmdObj["optimes"].type() == Array);
            BSONArray newTimes = BSONArray(cmdObj["optimes"].Obj());
            updateSlaveLocations(newTimes);

            return true;
        }
    } cmdReplSetUpdatePosition;

}
