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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_set_command.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/initial_sync.h"
#include "mongo/db/repl/old_update_position_args.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_interface.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

using std::string;
using std::stringstream;


class ReplExecutorSSM : public ServerStatusMetric {
public:
    ReplExecutorSSM() : ServerStatusMetric("repl.executor") {}
    virtual void appendAtLeaf(BSONObjBuilder& b) const {
        ReplicationExecutor* exec = getGlobalReplicationCoordinator()->getExecutor();
        b.append("executor", exec->getDiagnosticBSON());
    }
} replExecutorSSM;

// Testing only, enabled via command-line.
class CmdReplSetTest : public ReplSetCommand {
public:
    virtual void help(stringstream& help) const {
        help << "Just for tests.\n";
    }
    // No auth needed because it only works when enabled via command line.
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return Status::OK();
    }
    CmdReplSetTest() : ReplSetCommand("replSetTest") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        log() << "replSetTest command received: " << cmdObj.toString();

        auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());

        if (cmdObj.hasElement("forceInitialSyncFailure")) {
            replSetForceInitialSyncFailure = (unsigned)cmdObj["forceInitialSyncFailure"].Number();
            return true;
        } else if (cmdObj.hasElement("waitForMemberState")) {
            long long stateVal;
            auto status = bsonExtractIntegerField(cmdObj, "waitForMemberState", &stateVal);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            const auto swMemberState = MemberState::create(stateVal);
            if (!swMemberState.isOK()) {
                return appendCommandStatus(result, swMemberState.getStatus());
            }
            const auto expectedState = swMemberState.getValue();

            long long timeoutMillis;
            status = bsonExtractIntegerField(cmdObj, "timeoutMillis", &timeoutMillis);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
            Milliseconds timeout(timeoutMillis);
            log() << "replSetTest: waiting " << timeout << " for member state to become "
                  << expectedState;

            status = replCoord->waitForMemberState(expectedState, timeout);

            return appendCommandStatus(result, status);
        } else if (cmdObj.hasElement("waitForDrainFinish")) {
            long long timeoutMillis;
            auto status = bsonExtractIntegerField(cmdObj, "waitForDrainFinish", &timeoutMillis);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }
            Milliseconds timeout(timeoutMillis);
            log() << "replSetTest: waiting " << timeout << " for applier buffer to finish draining";

            status = replCoord->waitForDrainFinish(timeout);

            return appendCommandStatus(result, status);
        }

        Status status = replCoord->checkReplEnabledForCommand(&result);
        return appendCommandStatus(result, status);
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
    CmdReplSetGetRBID() : ReplSetCommand("replSetGetRBID") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        status = getGlobalReplicationCoordinator()->processReplSetGetRBID(&result);
        return appendCommandStatus(result, status);
    }
} cmdReplSetRBID;

class CmdReplSetGetStatus : public ReplSetCommand {
public:
    virtual void help(stringstream& help) const {
        help << "Report status of a replica set from the POV of this server\n";
        help << "{ replSetGetStatus : 1 }";
        help << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetGetStatus() : ReplSetCommand("replSetGetStatus", true) {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        if (cmdObj["forShell"].trueValue())
            LastError::get(txn->getClient()).disable();

        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        status = getGlobalReplicationCoordinator()->processReplSetGetStatus(&result);
        return appendCommandStatus(result, status);
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetGetStatus};
    }
} cmdReplSetGetStatus;

class CmdReplSetGetConfig : public ReplSetCommand {
public:
    virtual void help(stringstream& help) const {
        help << "Returns the current replica set configuration";
        help << "{ replSetGetConfig : 1 }";
        help << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetGetConfig() : ReplSetCommand("replSetGetConfig", true) {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        getGlobalReplicationCoordinator()->processReplSetGetConfig(&result);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetGetConfig};
    }
} cmdReplSetGetConfig;

namespace {
HostAndPort someHostAndPortForMe() {
    const char* ips = serverGlobalParams.bind_ip.c_str();
    while (*ips) {
        std::string ip;
        const char* comma = strchr(ips, ',');
        if (comma) {
            ip = std::string(ips, comma - ips);
            ips = comma + 1;
        } else {
            ip = std::string(ips);
            ips = "";
        }
        HostAndPort h = HostAndPort(ip, serverGlobalParams.port);
        if (!h.isLocalHost()) {
            return h;
        }
    }

    std::string h = getHostName();
    verify(!h.empty());
    verify(h != "localhost");
    return HostAndPort(h, serverGlobalParams.port);
}

void parseReplSetSeedList(ReplicationCoordinatorExternalState* externalState,
                          const std::string& replSetString,
                          std::string* setname,
                          std::vector<HostAndPort>* seeds) {
    const char* p = replSetString.c_str();
    const char* slash = strchr(p, '/');
    std::set<HostAndPort> seedSet;
    if (slash) {
        *setname = string(p, slash - p);
    } else {
        *setname = p;
    }

    if (slash == 0) {
        return;
    }

    p = slash + 1;
    while (1) {
        const char* comma = strchr(p, ',');
        if (comma == 0) {
            comma = strchr(p, 0);
        }
        if (p == comma) {
            break;
        }
        HostAndPort m;
        try {
            m = HostAndPort(string(p, comma - p));
        } catch (...) {
            uassert(13114, "bad --replSet seed hostname", false);
        }
        uassert(13096, "bad --replSet command line config string - dups?", seedSet.count(m) == 0);
        seedSet.insert(m);
        // uassert(13101, "can't use localhost in replset host list", !m.isLocalHost());
        if (externalState->isSelf(m, getGlobalServiceContext())) {
            LOG(1) << "ignoring seed " << m.toString() << " (=self)";
        } else {
            seeds->push_back(m);
        }
        if (*comma == 0) {
            break;
        }
        p = comma + 1;
    }
}
}  // namespace

class CmdReplSetInitiate : public ReplSetCommand {
public:
    CmdReplSetInitiate() : ReplSetCommand("replSetInitiate") {}
    virtual void help(stringstream& h) const {
        h << "Initiate/christen a replica set.";
        h << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
    }
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        BSONObj configObj;
        if (cmdObj["replSetInitiate"].type() == Object) {
            configObj = cmdObj["replSetInitiate"].Obj();
        }

        std::string replSetString =
            ReplicationCoordinator::get(txn)->getSettings().getReplSetString();
        if (replSetString.empty()) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::NoReplicationEnabled,
                                              "This node was not started with the replSet option"));
        }

        if (configObj.isEmpty()) {
            string noConfigMessage =
                "no configuration specified. "
                "Using a default configuration for the set";
            result.append("info2", noConfigMessage);
            log() << "initiate : " << noConfigMessage;

            ReplicationCoordinatorExternalStateImpl externalState(StorageInterface::get(txn));
            std::string name;
            std::vector<HostAndPort> seeds;
            parseReplSetSeedList(&externalState, replSetString, &name, &seeds);  // may throw...

            BSONObjBuilder b;
            b.append("_id", name);
            b.append("version", 1);
            BSONObjBuilder members;
            HostAndPort me = someHostAndPortForMe();
            members.append("0", BSON("_id" << 0 << "host" << me.toString()));
            result.append("me", me.toString());
            for (unsigned i = 0; i < seeds.size(); i++) {
                members.append(BSONObjBuilder::numStr(i + 1),
                               BSON("_id" << i + 1 << "host" << seeds[i].toString()));
            }
            b.appendArray("members", members.obj());
            configObj = b.obj();
            log() << "created this configuration for initiation : " << configObj.toString();
        }

        if (configObj.getField("version").eoo()) {
            // Missing version field defaults to version 1.
            BSONObjBuilder builder;
            builder.appendElements(configObj);
            builder.append("version", 1);
            configObj = builder.obj();
        }

        Status status =
            getGlobalReplicationCoordinator()->processReplSetInitiate(txn, configObj, &result);
        return appendCommandStatus(result, status);
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetConfigure};
    }
} cmdReplSetInitiate;

class CmdReplSetReconfig : public ReplSetCommand {
public:
    virtual void help(stringstream& help) const {
        help << "Adjust configuration of a replica set\n";
        help << "{ replSetReconfig : config_object }";
        help << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetReconfig() : ReplSetCommand("replSetReconfig") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        if (cmdObj["replSetReconfig"].type() != Object) {
            errmsg = "no configuration specified";
            return false;
        }

        ReplicationCoordinator::ReplSetReconfigArgs parsedArgs;
        parsedArgs.newConfigObj = cmdObj["replSetReconfig"].Obj();
        parsedArgs.force = cmdObj.hasField("force") && cmdObj["force"].trueValue();
        status =
            getGlobalReplicationCoordinator()->processReplSetReconfig(txn, parsedArgs, &result);

        ScopedTransaction scopedXact(txn, MODE_X);
        Lock::GlobalWrite globalWrite(txn->lockState());

        WriteUnitOfWork wuow(txn);
        if (status.isOK() && !parsedArgs.force) {
            getGlobalServiceContext()->getOpObserver()->onOpMessage(
                txn,
                BSON("msg"
                     << "Reconfig set"
                     << "version"
                     << parsedArgs.newConfigObj["version"]));
        }
        wuow.commit();

        return appendCommandStatus(result, status);
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetConfigure};
    }
} cmdReplSetReconfig;

class CmdReplSetFreeze : public ReplSetCommand {
public:
    virtual void help(stringstream& help) const {
        help << "{ replSetFreeze : <seconds> }";
        help << "'freeze' state of member to the extent we can do that.  What this really means is "
                "that\n";
        help << "this node will not attempt to become primary until the time period specified "
                "expires.\n";
        help << "You can call again with {replSetFreeze:0} to unfreeze sooner.\n";
        help << "A process restart unfreezes the member also.\n";
        help << "\nhttp://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetFreeze() : ReplSetCommand("replSetFreeze") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        int secs = (int)cmdObj.firstElement().numberInt();
        return appendCommandStatus(
            result, getGlobalReplicationCoordinator()->processReplSetFreeze(secs, &result));
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetFreeze;

class CmdReplSetStepDown : public ReplSetCommand {
public:
    virtual void help(stringstream& help) const {
        help << "{ replSetStepDown : <seconds> }\n";
        help << "Step down as primary.  Will not try to reelect self for the specified time period "
                "(1 minute if no numeric secs value specified, or secs is 0).\n";
        help << "(If another member with same priority takes over in the meantime, it will stay "
                "primary.)\n";
        help << "http://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetStepDown() : ReplSetCommand("replSetStepDown") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        const bool force = cmdObj["force"].trueValue();

        long long stepDownForSecs = cmdObj.firstElement().numberLong();
        if (stepDownForSecs == 0) {
            stepDownForSecs = 60;
        } else if (stepDownForSecs < 0) {
            status = Status(ErrorCodes::BadValue, "stepdown period must be a positive integer");
            return appendCommandStatus(result, status);
        }

        long long secondaryCatchUpPeriodSecs;
        status = bsonExtractIntegerField(
            cmdObj, "secondaryCatchUpPeriodSecs", &secondaryCatchUpPeriodSecs);
        if (status.code() == ErrorCodes::NoSuchKey) {
            // if field is absent, default values
            if (force) {
                secondaryCatchUpPeriodSecs = 0;
            } else {
                secondaryCatchUpPeriodSecs = 10;
            }
        } else if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        if (secondaryCatchUpPeriodSecs < 0) {
            status = Status(ErrorCodes::BadValue,
                            "secondaryCatchUpPeriodSecs period must be a positive or absent");
            return appendCommandStatus(result, status);
        }

        if (stepDownForSecs < secondaryCatchUpPeriodSecs) {
            status = Status(ErrorCodes::BadValue,
                            "stepdown period must be longer than secondaryCatchUpPeriodSecs");
            return appendCommandStatus(result, status);
        }

        log() << "Attempting to step down in response to replSetStepDown command";

        status = getGlobalReplicationCoordinator()->stepDown(
            txn, force, Seconds(secondaryCatchUpPeriodSecs), Seconds(stepDownForSecs));
        return appendCommandStatus(result, status);
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetStepDown;

class CmdReplSetMaintenance : public ReplSetCommand {
public:
    virtual void help(stringstream& help) const {
        help << "{ replSetMaintenance : bool }\n";
        help << "Enable or disable maintenance mode.";
    }
    CmdReplSetMaintenance() : ReplSetCommand("replSetMaintenance") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        return appendCommandStatus(result,
                                   getGlobalReplicationCoordinator()->setMaintenanceMode(
                                       cmdObj["replSetMaintenance"].trueValue()));
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetMaintenance;

class CmdReplSetSyncFrom : public ReplSetCommand {
public:
    virtual void help(stringstream& help) const {
        help << "{ replSetSyncFrom : \"host:port\" }\n";
        help << "Change who this member is syncing from.";
    }
    CmdReplSetSyncFrom() : ReplSetCommand("replSetSyncFrom") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        HostAndPort targetHostAndPort;
        status = targetHostAndPort.initialize(cmdObj["replSetSyncFrom"].valuestrsafe());
        if (!status.isOK())
            return appendCommandStatus(result, status);

        return appendCommandStatus(
            result,
            getGlobalReplicationCoordinator()->processReplSetSyncFrom(targetHostAndPort, &result));
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetSyncFrom;

class CmdReplSetUpdatePosition : public ReplSetCommand {
public:
    CmdReplSetUpdatePosition() : ReplSetCommand("replSetUpdatePosition") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        auto replCoord = repl::ReplicationCoordinator::get(txn->getClient()->getServiceContext());

        Status status = replCoord->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        // accept and ignore handshakes sent from old (3.0-series) nodes without erroring to
        // enable mixed-version operation, since we no longer use the handshakes
        if (cmdObj.hasField("handshake"))
            return true;

        auto metadataResult = rpc::ReplSetMetadata::readFromMetadata(cmdObj);
        if (metadataResult.isOK()) {
            // New style update position command has metadata, which may inform the
            // upstream of a higher term.
            auto metadata = metadataResult.getValue();
            replCoord->processReplSetMetadata(metadata);
        }

        // In the case of an update from a member with an invalid replica set config,
        // we return our current config version.
        long long configVersion = -1;

        UpdatePositionArgs args;

        status = args.initialize(cmdObj);
        if (status.isOK()) {
            // v3.2.4+ style replSetUpdatePosition command.
            status = replCoord->processReplSetUpdatePosition(args, &configVersion);

            if (status == ErrorCodes::InvalidReplicaSetConfig) {
                result.append("configVersion", configVersion);
            }
            return appendCommandStatus(result, status);
        } else if (status == ErrorCodes::NoSuchKey) {
            // Pre-3.2.4 style replSetUpdatePosition command.
            OldUpdatePositionArgs oldArgs;
            status = oldArgs.initialize(cmdObj);
            if (!status.isOK())
                return appendCommandStatus(result, status);

            status = replCoord->processReplSetUpdatePosition(oldArgs, &configVersion);

            if (status == ErrorCodes::InvalidReplicaSetConfig) {
                result.append("configVersion", configVersion);
            }
            return appendCommandStatus(result, status);
        } else {
            // Parsing error from UpdatePositionArgs.
            return appendCommandStatus(result, status);
        }
    }
} cmdReplSetUpdatePosition;

namespace {
/**
 * Returns true if there is no data on this server. Useful when starting replication.
 * The "local" database does NOT count except for "rs.oplog" collection.
 * Used to set the hasData field on replset heartbeat command response.
 */
bool replHasDatabases(OperationContext* txn) {
    std::vector<string> names;
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    storageEngine->listDatabases(&names);

    if (names.size() >= 2)
        return true;
    if (names.size() == 1) {
        if (names[0] != "local")
            return true;

        // we have a local database.  return true if oplog isn't empty
        BSONObj o;
        if (Helpers::getSingleton(txn, repl::rsOplogName.c_str(), o)) {
            return true;
        }
    }
    return false;
}

const std::string kHeartbeatConfigVersion = "configVersion";

bool isHeartbeatRequestV1(const BSONObj& cmdObj) {
    return cmdObj.hasField(kHeartbeatConfigVersion);
}

}  // namespace

MONGO_FP_DECLARE(rsDelayHeartbeatResponse);

/* { replSetHeartbeat : <setname> } */
class CmdReplSetHeartbeat : public ReplSetCommand {
public:
    CmdReplSetHeartbeat() : ReplSetCommand("replSetHeartbeat") {}
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        MONGO_FAIL_POINT_BLOCK(rsDelayHeartbeatResponse, delay) {
            const BSONObj& data = delay.getData();
            sleepsecs(data["delay"].numberInt());
        }

        Status status = Status(ErrorCodes::InternalError, "status not set in heartbeat code");
        /* we don't call ReplSetCommand::check() here because heartbeat
           checks many things that are pre-initialization. */
        if (!getGlobalReplicationCoordinator()->getSettings().usingReplSets()) {
            status = Status(ErrorCodes::NoReplicationEnabled, "not running with --replSet");
            return appendCommandStatus(result, status);
        }

        /* we want to keep heartbeat connections open when relinquishing primary.
           tag them here. */
        transport::Session::TagMask originalTag = 0;
        transport::Session* session = txn->getClient()->session();
        if (session) {
            originalTag = session->getTags();
            session->replaceTags(originalTag | transport::Session::kKeepOpen);
        }

        // Unset the tag on block exit
        ON_BLOCK_EXIT([session, originalTag]() {
            if (session) {
                session->replaceTags(originalTag);
            }
        });

        // Process heartbeat based on the version of request. The missing fields in mismatched
        // version will be empty.
        if (isHeartbeatRequestV1(cmdObj)) {
            ReplSetHeartbeatArgsV1 args;
            status = args.initialize(cmdObj);
            if (status.isOK()) {
                ReplSetHeartbeatResponse response;
                status = getGlobalReplicationCoordinator()->processHeartbeatV1(args, &response);
                if (status.isOK())
                    response.addToBSON(&result, true);
                return appendCommandStatus(result, status);
            }
            // else: fall through to old heartbeat protocol as it is likely that
            // a new node just joined the set
        }

        ReplSetHeartbeatArgs args;
        status = args.initialize(cmdObj);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        // ugh.
        if (args.getCheckEmpty()) {
            result.append("hasData", replHasDatabases(txn));
        }

        ReplSetHeartbeatResponse response;
        status = getGlobalReplicationCoordinator()->processHeartbeat(args, &response);
        if (status.isOK())
            response.addToBSON(&result, false);
        return appendCommandStatus(result, status);
    }
} cmdReplSetHeartbeat;

/** the first cmd called by a node seeking election and it's a basic sanity
    test: do any of the nodes it can reach know that it can't be the primary?
    */
class CmdReplSetFresh : public ReplSetCommand {
public:
    CmdReplSetFresh() : ReplSetCommand("replSetFresh") {}

    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        ReplicationCoordinator::ReplSetFreshArgs parsedArgs;
        parsedArgs.id = cmdObj["id"].Int();
        parsedArgs.setName = cmdObj["set"].String();
        parsedArgs.who = HostAndPort(cmdObj["who"].String());
        BSONElement cfgverElement = cmdObj["cfgver"];
        uassert(28525,
                str::stream() << "Expected cfgver argument to replSetFresh command to have "
                                 "numeric type, but found "
                              << typeName(cfgverElement.type()),
                cfgverElement.isNumber());
        parsedArgs.cfgver = cfgverElement.safeNumberLong();
        parsedArgs.opTime = Timestamp(cmdObj["opTime"].Date());

        status = getGlobalReplicationCoordinator()->processReplSetFresh(parsedArgs, &result);
        return appendCommandStatus(result, status);
    }
} cmdReplSetFresh;

class CmdReplSetElect : public ReplSetCommand {
public:
    CmdReplSetElect() : ReplSetCommand("replSetElect") {}

private:
    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        DEV log() << "received elect msg " << cmdObj.toString();
        else LOG(2) << "received elect msg " << cmdObj.toString();

        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        ReplicationCoordinator::ReplSetElectArgs parsedArgs;
        parsedArgs.set = cmdObj["set"].String();
        parsedArgs.whoid = cmdObj["whoid"].Int();
        BSONElement cfgverElement = cmdObj["cfgver"];
        uassert(28526,
                str::stream() << "Expected cfgver argument to replSetElect command to have "
                                 "numeric type, but found "
                              << typeName(cfgverElement.type()),
                cfgverElement.isNumber());
        parsedArgs.cfgver = cfgverElement.safeNumberLong();
        parsedArgs.round = cmdObj["round"].OID();

        status = getGlobalReplicationCoordinator()->processReplSetElect(parsedArgs, &result);
        return appendCommandStatus(result, status);
    }
} cmdReplSetElect;

class CmdReplSetStepUp : public ReplSetCommand {
public:
    CmdReplSetStepUp() : ReplSetCommand("replSetStepUp") {}

    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        Status status = getGlobalReplicationCoordinator()->checkReplEnabledForCommand(&result);
        if (!status.isOK())
            return appendCommandStatus(result, status);

        status = getGlobalReplicationCoordinator()->stepUpIfEligible();

        return appendCommandStatus(result, status);
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetStepUp;

}  // namespace repl
}  // namespace mongo
