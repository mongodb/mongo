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
#include "mongo/db/commands.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/replset_commands.h"
#include "mongo/db/repl/rs_config.h"
#include "mongo/db/repl/write_concern.h"

namespace mongo {
namespace repl {

    /* commands in other files:
         replSetHeartbeat - health.cpp
         replSetInitiate  - rs_mod.cpp
    */

    bool ReplSetCommand::check(string& errmsg, BSONObjBuilder& result) {
        if (!getGlobalReplicationCoordinator()->getSettings().usingReplSets()) {
            errmsg = "not running with --replSet";
            if (serverGlobalParams.configsvr) {
                result.append("info", "configsvr"); // for shell prompt
            }
            return false;
        }

        if( theReplSet == 0 ) {
            result.append("startupStatus", ReplSet::startupStatus);
            string s;
            errmsg = ReplSet::startupStatusMsg.empty() ?
                        "replset unknown error 2" : ReplSet::startupStatusMsg.get();
            if( ReplSet::startupStatus == 3 )
                result.append("info", "run rs.initiate(...) if not yet done for the set");
            return false;
        }

        return true;
    }

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
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
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
                sethbmsg(cmdObj["sethbmsg"].String());
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
        virtual void help( stringstream &help ) const {
            help << "internal";
        }
        CmdReplSetGetRBID() : ReplSetCommand("replSetGetRBID") {}
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::internal);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            Status status = getGlobalReplicationCoordinator()->processReplSetGetRBID(&result);
            return appendCommandStatus(result, status);
        }
    } cmdReplSetRBID;

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
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        CmdReplSetGetStatus() : ReplSetCommand("replSetGetStatus", true) { }
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if ( cmdObj["forShell"].trueValue() )
                lastError.disableForCommand();

            if (!check(errmsg, result))
                return false;

            getGlobalReplicationCoordinator()->processReplSetGetStatus(&result);

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
            actions.addAction(ActionType::replSetConfigure);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        CmdReplSetReconfig() : ReplSetCommand("replSetReconfig"), mutex("rsreconfig") { }
        virtual bool run(OperationContext* txn, const string& a, BSONObj& b, int e, string& errmsg, BSONObjBuilder& c, bool d) {
            try {
                rwlock_try_write lk(mutex);
                return _run(txn, a,b,e,errmsg,c,d);
            }
            catch(rwlock_try_write::exception&) { }
            errmsg = "a replSetReconfig is already in progress";
            return false;
        }
    private:
        bool _run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( cmdObj["replSetReconfig"].type() != Object ) {
                errmsg = "no configuration specified";
                return false;
            }

            ReplicationCoordinator::ReplSetReconfigArgs parsedArgs;
            parsedArgs.newConfigObj =  cmdObj["replSetReconfig"].Obj();
            parsedArgs.force = cmdObj.hasField("force") && cmdObj["force"].trueValue();
            Status status = getGlobalReplicationCoordinator()->processReplSetReconfig(txn,
                                                                                      parsedArgs,
                                                                                      &result);
            return appendCommandStatus(result, status);
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
            actions.addAction(ActionType::replSetStateChange);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        CmdReplSetFreeze() : ReplSetCommand("replSetFreeze") { }
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            int secs = (int) cmdObj.firstElement().numberInt();
            return appendCommandStatus(
                    result,
                    getGlobalReplicationCoordinator()->processReplSetFreeze(secs, &result));
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
            actions.addAction(ActionType::replSetStateChange);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        CmdReplSetStepDown() : ReplSetCommand("replSetStepDown") { }
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;

            bool force = cmdObj.hasField("force") && cmdObj["force"].trueValue();
            int secs = (int) cmdObj.firstElement().numberInt();
            if( secs == 0 )
                secs = 60;

            Status status = getGlobalReplicationCoordinator()->stepDown(
                    txn,
                    force,
                    ReplicationCoordinator::Milliseconds(0),
                    ReplicationCoordinator::Milliseconds(secs * 1000));
            return appendCommandStatus(result, status);
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
            actions.addAction(ActionType::replSetStateChange);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        CmdReplSetMaintenance() : ReplSetCommand("replSetMaintenance") { }
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            return appendCommandStatus(
                    result,
                    getGlobalReplicationCoordinator()->processReplSetMaintenance(
                            txn,
                            cmdObj["replSetMaintenance"].trueValue(),
                            &result));
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
            actions.addAction(ActionType::replSetStateChange);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        CmdReplSetSyncFrom() : ReplSetCommand("replSetSyncFrom") { }
        virtual bool run(OperationContext* txn, const string&, 
                         BSONObj& cmdObj, 
                         int, 
                         string& errmsg, 
                         BSONObjBuilder& result, 
                         bool fromRepl) {
            string newTarget = cmdObj["replSetSyncFrom"].valuestrsafe();
            return appendCommandStatus(
                    result,
                    getGlobalReplicationCoordinator()->processReplSetSyncFrom(newTarget,
                                                                              &result));
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
            actions.addAction(ActionType::internal);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        CmdReplSetUpdatePosition() : ReplSetCommand("replSetUpdatePosition") { }
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg,
                         BSONObjBuilder& result, bool fromRepl) {
            if (!check(errmsg, result))
                return false;

            if (cmdObj.hasField("handshake")) {
                // we have received a handshake, not an update message
                // handshakes are done here to ensure the receiving end supports the update command

                return appendCommandStatus(
                        result,
                        getGlobalReplicationCoordinator()->processReplSetUpdatePositionHandshake(
                                cmdObj["handshake"].embeddedObject(),
                                &result));
            }

            uassert(16888, "optimes field should be an array with an object for each secondary",
                    cmdObj["optimes"].type() == Array);

            return appendCommandStatus(
                    result,
                    getGlobalReplicationCoordinator()->processReplSetUpdatePosition(
                            BSONArray(cmdObj["optimes"].Obj()),
                            &result));
        }
    } cmdReplSetUpdatePosition;

} // namespace repl
} // namespace mongo
