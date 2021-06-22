/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand
#define LOG_FOR_HEARTBEATS(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kReplicationHeartbeats)

#include "mongo/platform/basic.h"

#include <boost/algorithm/string.hpp>

#include "mongo/db/repl/repl_set_command.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_interface.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

using std::string;
using std::stringstream;

static const std::string kReplSetReconfigNss = "local.replset.reconfig";

class ReplExecutorSSM : public ServerStatusMetric {
public:
    ReplExecutorSSM() : ServerStatusMetric("repl.executor") {}
    virtual void appendAtLeaf(BSONObjBuilder& b) const {
        ReplicationCoordinator::get(getGlobalServiceContext())->appendDiagnosticBSON(&b);
    }
} replExecutorSSM;

// Testing only, enabled via command-line.
class CmdReplSetTest : public ReplSetCommand {
public:
    std::string help() const override {
        return "Just for tests.\n";
    }
    // No auth needed because it only works when enabled via command line.
    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        return Status::OK();
    }
    CmdReplSetTest() : ReplSetCommand("replSetTest") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        log() << "replSetTest command received: " << cmdObj.toString();

        auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());

        if (cmdObj.hasElement("waitForMemberState")) {
            long long stateVal;
            auto status = bsonExtractIntegerField(cmdObj, "waitForMemberState", &stateVal);
            uassertStatusOK(status);

            const auto swMemberState = MemberState::create(stateVal);
            uassertStatusOK(swMemberState.getStatus());
            const auto expectedState = swMemberState.getValue();

            long long timeoutMillis;
            status = bsonExtractIntegerField(cmdObj, "timeoutMillis", &timeoutMillis);
            uassertStatusOK(status);
            Milliseconds timeout(timeoutMillis);
            log() << "replSetTest: waiting " << timeout << " for member state to become "
                  << expectedState;

            status = replCoord->waitForMemberState(expectedState, timeout);

            uassertStatusOK(status);
            return true;
        } else if (cmdObj.hasElement("waitForDrainFinish")) {
            long long timeoutMillis;
            auto status = bsonExtractIntegerField(cmdObj, "waitForDrainFinish", &timeoutMillis);
            uassertStatusOK(status);
            Milliseconds timeout(timeoutMillis);
            log() << "replSetTest: waiting " << timeout << " for applier buffer to finish draining";

            status = replCoord->waitForDrainFinish(timeout);

            uassertStatusOK(status);
            return true;
        } else if (cmdObj.hasElement("getLastStableRecoveryTimestamp")) {
            boost::optional<Timestamp> ts =
                StorageInterface::get(getGlobalServiceContext())
                    ->getLastStableRecoveryTimestamp(getGlobalServiceContext());
            if (ts) {
                result.append("lastStableRecoveryTimestamp", ts.get());
            }
            return true;
        }

        Status status = replCoord->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);
        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(CmdReplSetTest);

/** get rollback id.  used to check if a rollback happened during some interval of time.
    as consumed, the rollback id is not in any particular order, it simply changes on each rollback.
    @see incRBID()
*/
class CmdReplSetGetRBID : public ReplSetCommand {
public:
    CmdReplSetGetRBID() : ReplSetCommand("replSetGetRBID") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        result.append("rbid", ReplicationProcess::get(opCtx)->getRollbackID());
        return true;
    }
} cmdReplSetRBID;

class CmdReplSetGetConfig : public ReplSetCommand {
public:
    std::string help() const override {
        return "Returns the current replica set configuration"
               "{ replSetGetConfig : 1 }\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetGetConfig() : ReplSetCommand("replSetGetConfig") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        ReplicationCoordinator::get(opCtx)->processReplSetGetConfig(&result);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetGetConfig};
    }
} cmdReplSetGetConfig;

namespace {
HostAndPort someHostAndPortForMe() {
    const auto& addrs = serverGlobalParams.bind_ips;
    const auto& bind_port = serverGlobalParams.port;
    const auto& af = IPv6Enabled() ? AF_UNSPEC : AF_INET;
    bool localhost_only = true;

    for (const auto& addr : addrs) {
        // Get all addresses associated with each named bind host.
        // If we find any that are valid external identifiers,
        // then go ahead and use the first one.
        const auto& socks = SockAddr::createAll(addr, bind_port, af);
        for (const auto& sock : socks) {
            if (!sock.isLocalHost()) {
                if (!sock.isDefaultRoute()) {
                    // Return the hostname as passed rather than the resolved address.
                    return HostAndPort(addr, bind_port);
                }
                localhost_only = false;
            }
        }
    }

    if (localhost_only) {
        // We're only binding localhost-type interfaces.
        // Use one of those by name if available,
        // otherwise fall back on "localhost".
        return HostAndPort(addrs.size() ? addrs[0] : "localhost", bind_port);
    }

    // Based on the above logic, this is only reached for --bind_ip '0.0.0.0'.
    // We are listening externally, but we don't have a definite hostname.
    // Ask the OS.
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
    std::string help() const override {
        return "Initiate/christen a replica set.\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        BSONObj configObj;
        if (cmdObj["replSetInitiate"].type() == Object) {
            configObj = cmdObj["replSetInitiate"].Obj();
        }

        std::string replSetString =
            ReplicationCoordinator::get(opCtx)->getSettings().getReplSetString();
        if (replSetString.empty()) {
            uasserted(ErrorCodes::NoReplicationEnabled,
                      "This node was not started with the replSet option");
        }

        if (configObj.isEmpty()) {
            string noConfigMessage =
                "no configuration specified. "
                "Using a default configuration for the set";
            result.append("info2", noConfigMessage);
            log() << "initiate : " << noConfigMessage;

            ReplicationCoordinatorExternalStateImpl externalState(
                opCtx->getServiceContext(),
                DropPendingCollectionReaper::get(opCtx),
                StorageInterface::get(opCtx),
                ReplicationProcess::get(opCtx));
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
            BSONObjBuilder builder(std::move(configObj));
            builder.append("version", 1);
            configObj = builder.obj();
        }

        Status status =
            ReplicationCoordinator::get(opCtx)->processReplSetInitiate(opCtx, configObj, &result);
        uassertStatusOK(status);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetConfigure};
    }
} cmdReplSetInitiate;

class CmdReplSetReconfig : public ReplSetCommand {
public:
    CmdReplSetReconfig() : ReplSetCommand("replSetReconfig") {}

    std::string help() const override {
        return "Adjust configuration of a replica set\n"
               "{ replSetReconfig : config_object }\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto replCoord = ReplicationCoordinator::get(opCtx);
        uassertStatusOK(replCoord->checkReplEnabledForCommand(&result));

        if (cmdObj["replSetReconfig"].type() != Object) {
            result.append("errmsg", "no configuration specified");
            return false;
        }

        ReplicationCoordinator::ReplSetReconfigArgs parsedArgs;
        parsedArgs.newConfigObj = cmdObj["replSetReconfig"].Obj();
        parsedArgs.force = cmdObj.hasField("force") && cmdObj["force"].trueValue();
        auto status = replCoord->processReplSetReconfig(opCtx, parsedArgs, &result);

        if (status.isOK() && !parsedArgs.force) {
            const auto service = opCtx->getServiceContext();
            Lock::GlobalLock globalLock(opCtx, MODE_IX);
            writeConflictRetry(opCtx, "replSetReconfig", kReplSetReconfigNss, [&] {
                WriteUnitOfWork wuow(opCtx);
                // Users must not be allowed to provide their own contents for the o2 field.
                // o2 field of no-ops is supposed to be used internally.

                service->getOpObserver()->onOpMessage(opCtx,
                                                      BSON("msg"
                                                           << "Reconfig set"
                                                           << "version"
                                                           << parsedArgs.newConfigObj["version"]));
                wuow.commit();
            });
        }

        uassertStatusOK(status);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetConfigure};
    }
} cmdReplSetReconfig;

class CmdReplSetFreeze : public ReplSetCommand {
public:
    std::string help() const override {
        return "{ replSetFreeze : <seconds> }\n"
               "'freeze' state of member to the extent we can do that.  What this really means is "
               "that\n"
               "this node will not attempt to become primary until the time period specified "
               "expires.\n"
               "You can call again with {replSetFreeze:0} to unfreeze sooner.\n"
               "A process restart unfreezes the member also.\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetFreeze() : ReplSetCommand("replSetFreeze") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        int secs = (int)cmdObj.firstElement().numberInt();
        uassertStatusOK(ReplicationCoordinator::get(opCtx)->processReplSetFreeze(secs, &result));
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetFreeze;

class CmdReplSetStepDown : public ReplSetCommand {
public:
    std::string help() const override {
        return "{ replSetStepDown : <seconds> }\n"
               "Step down as primary.  Will not try to reelect self for the specified time period "
               "(1 minute if no numeric secs value specified, or secs is 0).\n"
               "(If another member with same priority takes over in the meantime, it will stay "
               "primary.)\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetStepDown()
        : ReplSetCommand("replSetStepDown"),
          _stepDownCmdsWithForceExecutedMetric("commands.replSetStepDownWithForce.total",
                                               &_stepDownCmdsWithForceExecuted),
          _stepDownCmdsWithForceFailedMetric("commands.replSetStepDownWithForce.failed",
                                             &_stepDownCmdsWithForceFailed) {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const bool force = cmdObj["force"].trueValue();

        if (force) {
            _stepDownCmdsWithForceExecuted.increment();
        }

        auto onExitGuard = makeGuard([&] {
            if (force) {
                _stepDownCmdsWithForceFailed.increment();
            }
        });

        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        long long stepDownForSecs = cmdObj.firstElement().numberLong();
        if (stepDownForSecs == 0) {
            stepDownForSecs = 60;
        } else if (stepDownForSecs < 0) {
            status = Status(ErrorCodes::BadValue, "stepdown period must be a positive integer");
            uassertStatusOK(status);
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
            uassertStatusOK(status);
        }

        if (secondaryCatchUpPeriodSecs < 0) {
            status = Status(ErrorCodes::BadValue,
                            "secondaryCatchUpPeriodSecs period must be a positive or absent");
            uassertStatusOK(status);
        }

        if (stepDownForSecs < secondaryCatchUpPeriodSecs) {
            status = Status(ErrorCodes::BadValue,
                            "stepdown period must be longer than secondaryCatchUpPeriodSecs");
            uassertStatusOK(status);
        }

        log() << "Attempting to step down in response to replSetStepDown command";

        ReplicationCoordinator::get(opCtx)->stepDown(
            opCtx, force, Seconds(secondaryCatchUpPeriodSecs), Seconds(stepDownForSecs));

        onExitGuard.dismiss();
        return true;
    }

private:
    mutable Counter64 _stepDownCmdsWithForceExecuted;
    mutable Counter64 _stepDownCmdsWithForceFailed;
    ServerStatusMetricField<Counter64> _stepDownCmdsWithForceExecutedMetric;
    ServerStatusMetricField<Counter64> _stepDownCmdsWithForceFailedMetric;

    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetStepDown;

class CmdReplSetMaintenance : public ReplSetCommand {
public:
    std::string help() const override {
        return "{ replSetMaintenance : bool }\n"
               "Enable or disable maintenance mode.";
    }
    CmdReplSetMaintenance() : ReplSetCommand("replSetMaintenance") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        uassertStatusOK(ReplicationCoordinator::get(opCtx)->setMaintenanceMode(
            opCtx, cmdObj["replSetMaintenance"].trueValue()));
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetMaintenance;

class CmdReplSetSyncFrom : public ReplSetCommand {
public:
    std::string help() const override {
        return "{ replSetSyncFrom : \"host:port\" }\n"
               "Change who this member is syncing from. Note: This will interrupt and restart an "
               "in-progress initial sync.";
    }
    CmdReplSetSyncFrom() : ReplSetCommand("replSetSyncFrom") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        HostAndPort targetHostAndPort;
        status = targetHostAndPort.initialize(cmdObj["replSetSyncFrom"].valuestrsafe());
        uassertStatusOK(status);

        uassertStatusOK(ReplicationCoordinator::get(opCtx)->processReplSetSyncFrom(
            opCtx, targetHostAndPort, &result));
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetSyncFrom;

class CmdReplSetUpdatePosition : public ReplSetCommand {
public:
    CmdReplSetUpdatePosition() : ReplSetCommand("replSetUpdatePosition") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());

        Status status = replCoord->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        // accept and ignore handshakes sent from old (3.0-series) nodes without erroring to
        // enable mixed-version operation, since we no longer use the handshakes
        if (cmdObj.hasField("handshake"))
            return true;

        // Wall clock times are required in ReplSetMetadata when FCV is 4.2. Arbiters trivially
        // have FCV equal to 4.2, so they are excluded from this check.
        bool isArbiter = replCoord->getMemberState() == MemberState::RS_ARBITER;
        bool requireWallTime =
            (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
             serverGlobalParams.featureCompatibility.getVersion() ==
                 ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42 &&
             !isArbiter);

        auto metadataResult = rpc::ReplSetMetadata::readFromMetadata(cmdObj, requireWallTime);
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

        // re-check requireWallTime
        requireWallTime =
            (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
             serverGlobalParams.featureCompatibility.getVersion() ==
                 ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42 &&
             !isArbiter);
        status = args.initialize(cmdObj, requireWallTime);
        if (status.isOK()) {
            status = replCoord->processReplSetUpdatePosition(args, &configVersion);

            if (status == ErrorCodes::InvalidReplicaSetConfig) {
                result.append("configVersion", configVersion);
            }
            // TODO convert to uassertStatusOK once SERVER-34806 is done.
            return CommandHelpers::appendCommandStatusNoThrow(result, status);
        } else {
            // Parsing error from UpdatePositionArgs.
            uassertStatusOK(status);
        }
        return true;
    }
} cmdReplSetUpdatePosition;

namespace {
/**
 * Returns true if there is no data on this server. Useful when starting replication.
 * The "local" database does NOT count except for "rs.oplog" collection.
 * Used to set the hasData field on replset heartbeat command response.
 */
bool replHasDatabases(OperationContext* opCtx) {
    StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
    std::vector<string> names = storageEngine->listDatabases();

    if (names.size() >= 2)
        return true;
    if (names.size() == 1) {
        if (names[0] != "local")
            return true;

        // we have a local database.  return true if oplog isn't empty
        BSONObj o;
        if (Helpers::getSingleton(opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), o)) {
            return true;
        }
    }
    return false;
}

}  // namespace

MONGO_FAIL_POINT_DEFINE(rsDelayHeartbeatResponse);

/* { replSetHeartbeat : <setname> } */
class CmdReplSetHeartbeat : public ReplSetCommand {
public:
    CmdReplSetHeartbeat() : ReplSetCommand("replSetHeartbeat") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        MONGO_FAIL_POINT_BLOCK(rsDelayHeartbeatResponse, delay) {
            const BSONObj& data = delay.getData();
            sleepsecs(data["delay"].numberInt());
        }

        LOG_FOR_HEARTBEATS(2) << "Received heartbeat request from " << cmdObj.getStringField("from")
                              << ", " << cmdObj;

        Status status = Status(ErrorCodes::InternalError, "status not set in heartbeat code");
        /* we don't call ReplSetCommand::check() here because heartbeat
           checks many things that are pre-initialization. */
        if (!ReplicationCoordinator::get(opCtx)->getSettings().usingReplSets()) {
            status = Status(ErrorCodes::NoReplicationEnabled, "not running with --replSet");
            uassertStatusOK(status);
        }

        ReplSetHeartbeatArgsV1 args;
        uassertStatusOK(args.initialize(cmdObj));

        LOG_FOR_HEARTBEATS(2) << "Processing heartbeat request from "
                              << cmdObj.getStringField("from") << ", " << cmdObj;
        ReplSetHeartbeatResponse response;
        status = ReplicationCoordinator::get(opCtx)->processHeartbeatV1(args, &response);
        if (status.isOK())
            response.addToBSON(&result);

        LOG_FOR_HEARTBEATS(2) << "Generated heartbeat response to  "
                              << cmdObj.getStringField("from") << ", " << response;
        uassertStatusOK(status);
        return true;
    }
} cmdReplSetHeartbeat;

class CmdReplSetStepUp : public ReplSetCommand {
public:
    CmdReplSetStepUp() : ReplSetCommand("replSetStepUp") {}

    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        log() << "Received replSetStepUp request";

        const bool skipDryRun = cmdObj["skipDryRun"].trueValue();
        status = ReplicationCoordinator::get(opCtx)->stepUpIfEligible(skipDryRun);

        if (!status.isOK()) {
            log() << "replSetStepUp request failed" << causedBy(status);
        }

        uassertStatusOK(status);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetStepUp;

class CmdReplSetAbortPrimaryCatchUp : public ReplSetCommand {
public:
    std::string help() const override {
        return "{ CmdReplSetAbortPrimaryCatchUp : 1 }\n"
               "Abort primary catch-up mode; immediately finish the transition to primary "
               "without fetching any further unreplicated writes from any other online nodes";
    }

    CmdReplSetAbortPrimaryCatchUp() : ReplSetCommand("replSetAbortPrimaryCatchUp") {}

    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) override {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);
        log() << "Received replSetAbortPrimaryCatchUp request";

        status = ReplicationCoordinator::get(opCtx)->abortCatchupIfNeeded(
            ReplicationCoordinator::PrimaryCatchUpConclusionReason::
                kFailedWithReplSetAbortPrimaryCatchUpCmd);
        if (!status.isOK()) {
            log() << "replSetAbortPrimaryCatchUp request failed" << causedBy(status);
        }
        uassertStatusOK(status);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
} cmdReplSetAbortPrimaryCatchUp;

}  // namespace repl
}  // namespace mongo
