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


#define LOGV2_FOR_HEARTBEATS(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                               \
        ID, DLEVEL, {logv2::LogComponent::kReplicationHeartbeats}, MESSAGE, ##__VA_ARGS__)

#include <cstdint>
#include <cstring>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/parsing_utils.h"
#include "mongo/db/repl/repl_set_command.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace repl {

namespace {
constexpr StringData kInternalIncludeNewlyAddedFieldName = "$_internalIncludeNewlyAdded"_sd;

struct ExecutorMetricPolicy {
    void appendTo(BSONObjBuilder& b, StringData leafName) const {
        ReplicationCoordinator::get(getGlobalServiceContext())->appendDiagnosticBSON(&b, leafName);
    }
};

auto& replExecutorSSM = *CustomMetricBuilder<ExecutorMetricPolicy>{"repl.executor"};
}  // namespace

// Test-only, enabled via command-line. See docs/test_commands.md.
class CmdReplSetTest : public ReplSetCommand {
public:
    std::string help() const override {
        return "Just for tests.\n";
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    CmdReplSetTest() : ReplSetCommand("replSetTest") {}
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        LOGV2(21573, "replSetTest command received", "cmdObj"_attr = cmdObj);

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
            LOGV2(21574,
                  "replSetTest: waiting for member state to become expected state",
                  "timeout"_attr = timeout,
                  "expectedState"_attr = expectedState);

            status = replCoord->waitForMemberState(opCtx, expectedState, timeout);

            uassertStatusOK(status);
            return true;
        } else if (cmdObj.hasElement("getLastStableRecoveryTimestamp")) {
            try {
                ScopedAdmissionPriority<ExecutionAdmissionContext> admissionPriority(
                    opCtx, AdmissionContext::Priority::kExempt);
                // We need to hold the lock so that we don't run when storage is being shutdown.
                Lock::GlobalLock lk(opCtx,
                                    MODE_IS,
                                    Date_t::now() + Milliseconds(5),
                                    Lock::InterruptBehavior::kLeaveUnlocked,
                                    [] {
                                        Lock::GlobalLockOptions options;
                                        options.skipRSTLLock = true;
                                        return options;
                                    }());
                if (lk.isLocked()) {
                    boost::optional<Timestamp> ts =
                        StorageInterface::get(getGlobalServiceContext())
                            ->getLastStableRecoveryTimestamp(getGlobalServiceContext());
                    if (ts) {
                        result.append("lastStableRecoveryTimestamp", ts.value());
                    }
                } else {
                    LOGV2_WARNING(
                        6100700,
                        "Failed to get last stable recovery timestamp due to lock acquire timeout. "
                        "Note this is expected if shutdown is in progress.");
                }
            } catch (const ExceptionFor<ErrorCategory::CancellationError>& ex) {
                LOGV2_WARNING(
                    6100701,
                    "Failed to get last stable recovery timestamp due to cancellation error. Note "
                    "this is expected if shutdown is in progress.",
                    "error"_attr = redact(ex));
            }
            return true;
        } else if (cmdObj.hasElement("restartHeartbeats")) {
            replCoord->restartScheduledHeartbeats_forTest();
            return true;
        }

        Status status = replCoord->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);
        return true;
    }
};

MONGO_REGISTER_COMMAND(CmdReplSetTest).testOnly().forShard();

/** get rollback id.  used to check if a rollback happened during some interval of time.
    as consumed, the rollback id is not in any particular order, it simply changes on each rollback.
    @see incRBID()
*/
class CmdReplSetGetRBID : public ReplSetCommand {
public:
    CmdReplSetGetRBID() : ReplSetCommand("replSetGetRBID") {}
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        result.append("rbid", ReplicationProcess::get(opCtx)->getRollbackID());
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetGetRBID).forShard();

class CmdReplSetGetConfig : public ReplSetCommand {
public:
    std::string help() const override {
        return "Returns the current replica set configuration"
               "{ replSetGetConfig : 1 }\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }
    CmdReplSetGetConfig() : ReplSetCommand("replSetGetConfig") {}
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        bool wantCommitmentStatus;
        uassertStatusOK(bsonExtractBooleanFieldWithDefault(
            cmdObj, "commitmentStatus", false, &wantCommitmentStatus));

        if (cmdObj[kInternalIncludeNewlyAddedFieldName]) {
            uassert(ErrorCodes::InvalidOptions,
                    "The '$_internalIncludeNewlyAdded' option is only supported when testing"
                    " commands are enabled",
                    getTestCommandsEnabled());
        }

        bool includeNewlyAdded;
        uassertStatusOK(bsonExtractBooleanFieldWithDefault(
            cmdObj, kInternalIncludeNewlyAddedFieldName, false, &includeNewlyAdded));

        ReplicationCoordinator::get(opCtx)->processReplSetGetConfig(
            &result, wantCommitmentStatus, includeNewlyAdded);

        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetGetConfig};
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetGetConfig).forShard();

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
    MONGO_verify(!h.empty());
    MONGO_verify(h != "localhost");
    return HostAndPort(h, serverGlobalParams.port);
}
}  // namespace

class CmdReplSetInitiate : public ReplSetCommand {
public:
    CmdReplSetInitiate() : ReplSetCommand("replSetInitiate") {}
    std::string help() const override {
        return "Initiate/christen a replica set.\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        BSONObj configObj;
        if (cmdObj["replSetInitiate"].type() == BSONType::object) {
            configObj = cmdObj["replSetInitiate"].Obj();
        }

        const auto& settings = ReplicationCoordinator::get(opCtx)->getSettings();
        if (!settings.isReplSet()) {
            uasserted(ErrorCodes::NoReplicationEnabled,
                      "This node was not started with replication enabled.");
        }

        if (configObj.isEmpty()) {
            std::string replSetString = settings.getReplSetString();

            std::string noConfigMessage =
                "no configuration specified. "
                "Using a default configuration for the set";
            result.append("info2", noConfigMessage);
            LOGV2(
                21577,
                "Initiate: no configuration specified. Using a default configuration for the set");

            ReplicationCoordinatorExternalStateImpl externalState(opCtx->getServiceContext(),
                                                                  StorageInterface::get(opCtx),
                                                                  ReplicationProcess::get(opCtx));
            auto [name, seeds] = parseReplSetSeedList(
                &externalState,
                replSetString);  // May throw on invalid/duplicate seeds, localhost usage, or
                                 // parsing errors (via HostAndPort::parseThrowing).

            BSONObjBuilder b;
            b.append("_id", name);
            b.append("version", 1);
            BSONObjBuilder members;
            HostAndPort me = someHostAndPortForMe();

            auto appendMember =
                [&members, serial = DecimalCounter<uint32_t>()](const HostAndPort& host) mutable {
                    members.append(
                        StringData{serial},
                        BSON("_id" << static_cast<int>(serial) << "host" << host.toString()));
                    ++serial;
                };
            appendMember(me);
            result.append("me", me.toString());
            for (const HostAndPort& seed : seeds) {
                appendMember(seed);
            }
            b.appendArray("members", members.obj());
            configObj = b.obj();
            LOGV2(21578, "Created configuration for initiation", "config"_attr = configObj);
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
};
MONGO_REGISTER_COMMAND(CmdReplSetInitiate).forShard();

class CmdReplSetReconfig : public ReplSetCommand {
public:
    CmdReplSetReconfig() : ReplSetCommand("replSetReconfig") {}

    std::string help() const override {
        return "Adjust configuration of a replica set\n"
               "{ replSetReconfig : config_object }\n"
               "http://dochub.mongodb.org/core/replicasetcommands";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto replCoord = ReplicationCoordinator::get(opCtx);
        uassertStatusOK(replCoord->checkReplEnabledForCommand(&result));

        if (cmdObj["replSetReconfig"].type() != BSONType::object) {
            result.append("errmsg", "no configuration specified");
            return false;
        }

        ReplicationCoordinator::ReplSetReconfigArgs parsedArgs;
        parsedArgs.newConfigObj = cmdObj["replSetReconfig"].Obj();
        parsedArgs.force = cmdObj.hasField("force") && cmdObj["force"].trueValue();

        // For safe reconfig, wait for the current config to be committed before running a new one.
        // We will check again after acquiring the repl mutex in processReplSetReconfig(), in case
        // of concurrent reconfigs.
        if (!parsedArgs.force) {
            // Skip the waiting if the current config is from a force reconfig.
            auto configTerm = replCoord->getConfigTerm();
            auto oplogWait = configTerm != OpTime::kUninitializedTerm;
            auto status = replCoord->awaitConfigCommitment(opCtx, oplogWait, configTerm);
            status.addContext("New config is rejected");
            if (status == ErrorCodes::MaxTimeMSExpired) {
                // Convert the error code to be more specific.
                uasserted(ErrorCodes::CurrentConfigNotCommittedYet, status.reason());
            } else if (status == ErrorCodes::PrimarySteppedDown) {
                // Return NotWritablePrimary since the command has no side effect yet.
                status = {ErrorCodes::NotWritablePrimary, status.reason()};
            }
            uassertStatusOK(status);
        }

        auto status = replCoord->processReplSetReconfig(opCtx, parsedArgs, &result);
        uassertStatusOK(status);

        // Now that the new config has been persisted and installed in memory, wait for the new
        // config to become replicated. For force reconfigs we don't need to do this waiting.
        if (!parsedArgs.force) {
            auto configTerm = replCoord->getConfigTerm();
            auto status = replCoord->awaitConfigCommitment(
                opCtx, false /* waitForOplogCommitment */, configTerm);
            uassertStatusOK(
                status.withContext("Reconfig finished but failed to propagate to a majority"));
        }

        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetConfigure};
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetReconfig).forShard();

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
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        auto secs = cmdObj.firstElement().safeNumberInt();
        uassertStatusOK(ReplicationCoordinator::get(opCtx)->processReplSetFreeze(secs, &result));
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetFreeze).forShard();

namespace {
auto& stepDownCmdsWithForceExecuted =
    *MetricBuilder<Counter64>{"commands.replSetStepDownWithForce.total"}.setRole(
        ClusterRole::ShardServer);
auto& stepDownCmdsWithForceFailed =
    *MetricBuilder<Counter64>{"commands.replSetStepDownWithForce.failed"}.setRole(
        ClusterRole::ShardServer);
}  // namespace

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

    bool shouldCheckoutSession() const final {
        return false;
    }

    CmdReplSetStepDown() : ReplSetCommand("replSetStepDown") {}

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const bool force = cmdObj["force"].trueValue();

        if (force) {
            stepDownCmdsWithForceExecuted.increment();
        }

        ScopeGuard onExitGuard([&] {
            if (force) {
                stepDownCmdsWithForceFailed.increment();
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

        LOGV2(21579, "Attempting to step down in response to replSetStepDown command");

        ReplicationCoordinator::get(opCtx)->stepDown(
            opCtx, force, Seconds(secondaryCatchUpPeriodSecs), Seconds(stepDownForSecs));

        LOGV2(21580, "replSetStepDown command completed");

        onExitGuard.dismiss();
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetStepDown).forShard();

class CmdReplSetMaintenance : public ReplSetCommand {
public:
    std::string help() const override {
        return "{ replSetMaintenance : bool }\n"
               "Enable or disable maintenance mode.";
    }
    CmdReplSetMaintenance() : ReplSetCommand("replSetMaintenance") {}
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
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
};
MONGO_REGISTER_COMMAND(CmdReplSetMaintenance).forShard();

class CmdReplSetSyncFrom : public ReplSetCommand {
public:
    std::string help() const override {
        return "{ replSetSyncFrom : \"host:port\" }\n"
               "Change who this member is syncing from. Note: This will interrupt and restart an "
               "in-progress initial sync.";
    }
    CmdReplSetSyncFrom() : ReplSetCommand("replSetSyncFrom") {}
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        HostAndPort targetHostAndPort;
        status = targetHostAndPort.initialize(cmdObj.getStringField("replSetSyncFrom"));
        uassertStatusOK(status);

        uassertStatusOK(ReplicationCoordinator::get(opCtx)->processReplSetSyncFrom(
            opCtx, targetHostAndPort, &result));
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetSyncFrom).forShard();

class CmdReplSetUpdatePosition : public ReplSetCommand {
public:
    CmdReplSetUpdatePosition() : ReplSetCommand("replSetUpdatePosition") {}
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());

        Status status = replCoord->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        // accept and ignore handshakes sent from old (3.0-series) nodes without erroring to
        // enable mixed-version operation, since we no longer use the handshakes
        if (cmdObj.hasField("handshake"))
            return true;

        auto metadataResult = rpc::ReplSetMetadata::readFromMetadata(cmdObj);
        if (metadataResult.isOK()) {
            // New style update position command has metadata, which may inform the
            // upstream of a higher term.
            const auto& metadata = metadataResult.getValue();
            if (metadata.hasReplicaSetId()) {
                auto config = replCoord->getConfig();
                uassert(ErrorCodes::InconsistentReplicaSetNames,
                        "The current replicaSetId does not match the one in replSetMetaData",
                        !config.isInitialized() ||
                            config.getReplicaSetId() == metadata.getReplicaSetId());
            }

            replCoord->processReplSetMetadata(metadata);
        }

        UpdatePositionArgs args;

        status = args.initialize(cmdObj);
        if (status.isOK()) {
            status = replCoord->processReplSetUpdatePosition(args);
            return CommandHelpers::appendCommandStatusNoThrow(result, status);
        } else {
            // Parsing error from UpdatePositionArgs.
            uassertStatusOK(status);
        }
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetUpdatePosition).forShard();

namespace {
/**
 * Returns true if there is no data on this server. Useful when starting replication.
 * The "local" database does NOT count except for "rs.oplog" collection.
 * Used to set the hasData field on replset heartbeat command response.
 */
bool replHasDatabases(OperationContext* opCtx) {
    std::vector<DatabaseName> dbNames = catalog::listDatabases();

    if (dbNames.size() >= 2)
        return true;
    if (dbNames.size() == 1) {
        if (!dbNames[0].isLocalDB())
            return true;

        // we have a local database.  return true if oplog isn't empty
        BSONObj o;
        if (Helpers::getSingleton(opCtx, NamespaceString::kRsOplogNamespace, o)) {
            return true;
        }
    }
    return false;
}

}  // namespace

/* { replSetHeartbeat : <setname> } */
class CmdReplSetHeartbeat : public ReplSetCommand {
public:
    CmdReplSetHeartbeat() : ReplSetCommand("replSetHeartbeat") {}
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        LOGV2_FOR_HEARTBEATS(24095,
                             2,
                             "Received heartbeat request",
                             "from"_attr = cmdObj.getStringField("from"),
                             "cmdObj"_attr = cmdObj);

        Status status = Status(ErrorCodes::InternalError, "status not set in heartbeat code");
        /* we don't call ReplSetCommand::check() here because heartbeat
           checks many things that are pre-initialization. */
        if (!ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
            status = Status(ErrorCodes::NoReplicationEnabled, "not running using replication");
            uassertStatusOK(status);
        }

        ReplSetHeartbeatArgsV1 args;
        uassertStatusOK(args.initialize(cmdObj));

        ReplSetHeartbeatResponse response;
        status = ReplicationCoordinator::get(opCtx)->processHeartbeatV1(args, &response);
        if (status.isOK())
            response.addToBSON(&result);

        LOGV2_FOR_HEARTBEATS(24097,
                             2,
                             "Generated heartbeat response",
                             "from"_attr = cmdObj.getStringField("from"),
                             "response"_attr = response);
        uassertStatusOK(status);
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetHeartbeat).forShard();

class CmdReplSetStepUp : public ReplSetCommand {
public:
    CmdReplSetStepUp() : ReplSetCommand("replSetStepUp") {}

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);

        LOGV2(21581, "Received replSetStepUp request");

        const bool skipDryRun = cmdObj["skipDryRun"].trueValue();
        status = ReplicationCoordinator::get(opCtx)->stepUpIfEligible(skipDryRun);

        if (!status.isOK()) {
            LOGV2(21582, "replSetStepUp request failed", "error"_attr = causedBy(status));
        }

        uassertStatusOK(status);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetStepUp).forShard();

class CmdReplSetAbortPrimaryCatchUp : public ReplSetCommand {
public:
    std::string help() const override {
        return "{ CmdReplSetAbortPrimaryCatchUp : 1 }\n"
               "Abort primary catch-up mode; immediately finish the transition to primary "
               "without fetching any further unreplicated writes from any other online nodes";
    }

    CmdReplSetAbortPrimaryCatchUp() : ReplSetCommand("replSetAbortPrimaryCatchUp") {}

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        Status status = ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result);
        uassertStatusOK(status);
        LOGV2(21583, "Received replSetAbortPrimaryCatchUp request");

        status = ReplicationCoordinator::get(opCtx)->abortCatchupIfNeeded(
            ReplicationCoordinator::PrimaryCatchUpConclusionReason::
                kFailedWithReplSetAbortPrimaryCatchUpCmd);
        if (!status.isOK()) {
            LOGV2(21584,
                  "replSetAbortPrimaryCatchUp request failed",
                  "error"_attr = causedBy(status));
        }
        uassertStatusOK(status);
        return true;
    }

private:
    ActionSet getAuthActionSet() const override {
        return ActionSet{ActionType::replSetStateChange};
    }
};
MONGO_REGISTER_COMMAND(CmdReplSetAbortPrimaryCatchUp).forShard();

}  // namespace repl
}  // namespace mongo
