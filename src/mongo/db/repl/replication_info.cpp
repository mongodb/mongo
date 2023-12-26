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

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/audit.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/direct_shard_client_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/hello_auth.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/repl/hello_response.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/split_horizon.h"
#include "mongo/db/s/global_user_write_block_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/wire_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {

// Hangs in the beginning of each hello command when set.
MONGO_FAIL_POINT_DEFINE(shardWaitInHello);
// Awaitable hello requests with the proper topologyVersions will sleep for maxAwaitTimeMS on
// standalones. This failpoint will hang right before doing this sleep when set.
MONGO_FAIL_POINT_DEFINE(hangWaitingForHelloResponseOnStandalone);

MONGO_FAIL_POINT_DEFINE(appendHelloOkToHelloResponse);

namespace repl {
namespace {

constexpr auto kHelloString = "hello"_sd;
constexpr auto kCamelCaseIsMasterString = "isMaster"_sd;
constexpr auto kLowerCaseIsMasterString = "ismaster"_sd;

void appendPrimaryOnlyServiceInfo(ServiceContext* serviceContext, BSONObjBuilder* result) {
    auto registry = PrimaryOnlyServiceRegistry::get(serviceContext);
    registry->reportServiceInfoForServerStatus(result);
}

/**
 * Appends replication-related fields to the hello response. Returns the topology version that
 * was included in the response.
 */
TopologyVersion appendReplicationInfo(OperationContext* opCtx,
                                      BSONObjBuilder* result,
                                      bool appendReplicationProcess,
                                      bool useLegacyResponseFields,
                                      boost::optional<TopologyVersion> clientTopologyVersion,
                                      boost::optional<std::int64_t> maxAwaitTimeMS) {
    TopologyVersion topologyVersion;
    ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet()) {
        const auto& horizonParams = SplitHorizon::getParameters(opCtx->getClient());

        boost::optional<Date_t> deadline;
        if (maxAwaitTimeMS) {
            deadline = opCtx->getServiceContext()->getPreciseClockSource()->now() +
                Milliseconds(*maxAwaitTimeMS);
        }
        auto helloResponse =
            replCoord->awaitHelloResponse(opCtx, horizonParams, clientTopologyVersion, deadline);
        result->appendElements(helloResponse->toBSON(useLegacyResponseFields));
        if (appendReplicationProcess) {
            replCoord->appendSecondaryInfoData(result);
        }
        invariant(helloResponse->getTopologyVersion());

        // Only shard servers will respond with the isImplicitDefaultMajorityWC field.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            result->append(HelloCommandReply::kIsImplicitDefaultMajorityWCFieldName,
                           replCoord->getConfig().isImplicitDefaultWriteConcernMajority());

            auto cwwc = ReadWriteConcernDefaults::get(opCtx).getCWWC(opCtx);
            if (cwwc) {
                result->append(HelloCommandReply::kCwwcFieldName, cwwc.value().toBSON());
            }
        }

        return helloResponse->getTopologyVersion().value();
    }

    auto currentTopologyVersion = replCoord->getTopologyVersion();

    if (clientTopologyVersion &&
        clientTopologyVersion->getProcessId() == currentTopologyVersion.getProcessId()) {
        uassert(51764,
                str::stream() << "Received a topology version with counter: "
                              << clientTopologyVersion->getCounter()
                              << " which is greater than the server topology version counter: "
                              << currentTopologyVersion.getCounter(),
                clientTopologyVersion->getCounter() == currentTopologyVersion.getCounter());

        // The topologyVersion never changes on a running standalone process, so just sleep for
        // maxAwaitTimeMS.
        invariant(maxAwaitTimeMS);

        HelloMetrics::get(opCtx)->incrementNumAwaitingTopologyChanges();
        ON_BLOCK_EXIT([&] { HelloMetrics::get(opCtx)->decrementNumAwaitingTopologyChanges(); });
        if (MONGO_unlikely(hangWaitingForHelloResponseOnStandalone.shouldFail())) {
            // Used in tests that wait for this failpoint to be entered to guarantee that the
            // request is waiting and metrics have been updated.
            LOGV2(31462, "Hanging due to hangWaitingForHelloResponseOnStandalone failpoint.");
            hangWaitingForHelloResponseOnStandalone.pauseWhileSet(opCtx);
        }
        opCtx->sleepFor(Milliseconds(*maxAwaitTimeMS));
    }

    result->appendBool((useLegacyResponseFields ? "ismaster" : "isWritablePrimary"),
                       ReplicationCoordinator::get(opCtx)->isWritablePrimaryForReportingPurposes());

    BSONObjBuilder topologyVersionBuilder(result->subobjStart("topologyVersion"));
    currentTopologyVersion.serialize(&topologyVersionBuilder);

    return currentTopologyVersion;
}

class ReplicationInfoServerStatus : public ServerStatusSection {
public:
    enum class UserWriteBlockState { kUnknown = 0, kDisabled = 1, kEnabled = 2 };

    ReplicationInfoServerStatus() : ServerStatusSection("repl") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
            return BSONObj();
        }

        bool appendReplicationProcess = configElement.numberInt() > 0;

        BSONObjBuilder result;
        appendReplicationInfo(opCtx,
                              &result,
                              appendReplicationProcess,
                              false /* useLegacyResponseFields */,
                              boost::none /* clientTopologyVersion */,
                              boost::none /* maxAwaitTimeMS */);

        appendPrimaryOnlyServiceInfo(opCtx->getServiceContext(), &result);

        auto rbid = ReplicationProcess::get(opCtx)->getRollbackID();
        if (ReplicationProcess::kUninitializedRollbackId != rbid) {
            result.append("rbid", rbid);
        }
        {
            auto state = UserWriteBlockState::kUnknown;
            // Try to lock. If we fail (i.e. lock is already held in write mode), don't read the
            // GlobalUserWriteBlockState and set the userWriteBlockMode field to kUnknown.
            Lock::GlobalLock lk(
                opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kLeaveUnlocked, [] {
                    Lock::GlobalLockSkipOptions options;
                    options.skipRSTLLock = true;
                    return options;
                }());
            if (!lk.isLocked()) {
                LOGV2_DEBUG(6345700, 2, "Failed to retrieve user write block state");
            } else {
                state = GlobalUserWriteBlockState::get(opCtx)->isUserWriteBlockingEnabled(opCtx)
                    ? UserWriteBlockState::kEnabled
                    : UserWriteBlockState::kDisabled;
            }
            result.append("userWriteBlockMode", state);
        }

        return result.obj();
    }

} replicationInfoServerStatus;

class OplogInfoServerStatus : public ServerStatusSection {
public:
    OplogInfoServerStatus() : ServerStatusSection("oplog") {}

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
        if (!replCoord->getSettings().isReplSet()) {
            return BSONObj();
        }

        BSONObjBuilder result;
        result.append("latestOptime", replCoord->getMyLastAppliedOpTime().getTimestamp());

        auto earliestOplogTimestampFetch = [&]() -> Timestamp {
            // Hold reference to the catalog for collection lookup without locks to be safe.
            auto catalog = CollectionCatalog::get(opCtx);
            auto oplog =
                catalog->lookupCollectionByNamespace(opCtx, NamespaceString::kRsOplogNamespace);
            if (!oplog) {
                return Timestamp();
            }

            // Try to get the lock. If it's already locked, immediately return null timestamp.
            Lock::GlobalLock lk(
                opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kLeaveUnlocked, [] {
                    Lock::GlobalLockSkipOptions options;
                    options.skipRSTLLock = true;
                    return options;
                }());
            if (!lk.isLocked()) {
                LOGV2_DEBUG(
                    6294100, 2, "Failed to get global lock for oplog server status section");
                return Timestamp();
            }

            // Try getting earliest oplog timestamp using getEarliestOplogTimestamp
            auto swEarliestOplogTimestamp =
                oplog->getRecordStore()->getEarliestOplogTimestamp(opCtx);

            if (swEarliestOplogTimestamp.getStatus() == ErrorCodes::OplogOperationUnsupported) {
                // Falling back to use getSingleton if the storage engine does not support
                // getEarliestOplogTimestamp.
                // Note that getSingleton will take a global IS lock, but this won't block because
                // we are already holding the global IS lock.
                BSONObj o;
                if (Helpers::getSingleton(opCtx, NamespaceString::kRsOplogNamespace, o)) {
                    return o["ts"].timestamp();
                }
            }
            if (!swEarliestOplogTimestamp.isOK()) {
                return Timestamp();
            }
            return swEarliestOplogTimestamp.getValue();
        }();

        result.append("earliestOptime", earliestOplogTimestampFetch);

        return result.obj();
    }
} oplogInfoServerStatus;

const std::string kAutomationServiceDescriptorFieldName =
    HelloCommandReply::kAutomationServiceDescriptorFieldName.toString();

class CmdHello : public BasicCommandWithReplyBuilderInterface {
public:
    CmdHello() : CmdHello(kHelloString, {}) {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    bool requiresAuth() const final {
        return false;
    }

    HandshakeRole handshakeRole() const final {
        return HandshakeRole::kHello;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Check if this server is primary for a replica set\n"
               "{ hello : 1 }";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const final {
        static const Status kReadConcernNotSupported{ErrorCodes::InvalidOptions,
                                                     "read concern not supported"};
        static const Status kDefaultReadConcernNotPermitted{
            ErrorCodes::InvalidOptions, "cluster wide default read concern not permitted"};

        static const Status kImplicitDefaultReadConcernNotPermitted{
            ErrorCodes::InvalidOptions, "implicit default read concern not permitted"};
        return {{level != repl::ReadConcernLevel::kLocalReadConcern, kReadConcernNotSupported},
                {kDefaultReadConcernNotPermitted},
                {kImplicitDefaultReadConcernNotPermitted}};
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();  // No auth required
    }

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) final {
        // Critical to monitoring and observability, categorize the command as immediate priority.
        ScopedAdmissionPriorityForLock skipAdmissionControl(shard_role_details::getLocker(opCtx),
                                                            AdmissionContext::Priority::kImmediate);

        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        const bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);
        const auto vts = auth::ValidatedTenancyScope::get(opCtx);
        const auto sc = vts != boost::none
            ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
            : SerializationContext::stateCommandRequest();
        auto cmd = HelloCommand::parse(
            IDLParserContext("hello", apiStrict, vts, dbName.tenantId(), sc), cmdObj);

        shardWaitInHello.execute(
            [&](const BSONObj& customArgs) { _handleHelloFailPoint(customArgs, opCtx, cmdObj); });

        /* currently request to arbiter is (somewhat arbitrarily) an ismaster request that is not
           authenticated.
        */
        if (cmd.getForShell()) {
            NotPrimaryErrorTracker::get(opCtx->getClient()).disable();
        }

        Client::TagMask connectionTagsToSet = 0;
        Client::TagMask connectionTagsToUnset = 0;

        // Tag connections to avoid closing them on stepdown.
        if (!cmd.getHangUpOnStepDown()) {
            connectionTagsToSet |= Client::kKeepOpen;
        }

        // Negotiate compressors before logging metadata so we can include the result in the log
        // line.
        auto result = replyBuilder->getBodyBuilder();
        if (opCtx->getClient()->session()) {
            MessageCompressorManager::forSession(opCtx->getClient()->session())
                .serverNegotiate(cmd.getCompression(), &result);
        }

        auto client = opCtx->getClient();
        const auto internalClient = cmd.getInternalClient();
        const bool isInternalClient = internalClient.has_value();

        if (ClientMetadata::tryFinalize(client)) {
            // This is the first hello for this client.
            audit::logClientMetadata(client);
            if (!isInternalClient) {
                DirectShardClientTracker::trackClient(client);
            }

            // Set split horizon parameters.
            auto sniName = client->getSniNameForSession();
            SplitHorizon::setParameters(client, std::move(sniName));
        }

        // Parse the optional 'internalClient' field. This is provided by incoming connections from
        // mongod and mongos.
        if (internalClient) {
            connectionTagsToUnset |= Client::kExternalClientKeepOpen;

            // All incoming connections from mongod/mongos of earlier versions should be
            // closed if the featureCompatibilityVersion is bumped to 3.6.
            if (internalClient->getMaxWireVersion() >=
                WireSpec::getWireSpec(opCtx->getServiceContext())
                    .get()
                    ->incomingExternalClient.maxWireVersion) {
                connectionTagsToSet |= Client::kLatestVersionInternalClientKeepOpen;
            } else {
                connectionTagsToUnset |= Client::kLatestVersionInternalClientKeepOpen;
            }
        } else {
            connectionTagsToUnset |= Client::kLatestVersionInternalClientKeepOpen;
            connectionTagsToSet |= Client::kExternalClientKeepOpen;
        }

        if (opCtx->getClient()->session()) {
            opCtx->getClient()->mutateTags(
                [connectionTagsToSet, connectionTagsToUnset, opCtx](Client::TagMask originalTags) {
                    // After a mongos sends the initial "isMaster" command with its mongos client
                    // information, it sometimes sends another "isMaster" command that is forwarded
                    // from its client. Once kInternalClient has been set, we assume that any future
                    // "isMaster" commands are forwarded in this manner, and we do not update the
                    // session tags.
                    if (!opCtx->getClient()->isInternalClient()) {
                        return (originalTags | connectionTagsToSet) & ~connectionTagsToUnset;
                    } else {
                        return originalTags;
                    }
                });
            if (!opCtx->getClient()->isInternalClient()) {
                opCtx->getClient()->setIsInternalClient(isInternalClient);
            }
        }

        // If a client is following the awaitable hello protocol, maxAwaitTimeMS should be
        // present if and only if topologyVersion is present in the request.
        auto clientTopologyVersion = cmd.getTopologyVersion();
        auto maxAwaitTimeMS = cmd.getMaxAwaitTimeMS();
        auto curOp = CurOp::get(opCtx);
        boost::optional<ScopeGuard<std::function<void()>>> timerGuard;
        if (clientTopologyVersion && maxAwaitTimeMS) {
            uassert(31372,
                    "topologyVersion must have a non-negative counter",
                    clientTopologyVersion->getCounter() >= 0);

            LOGV2_DEBUG(23904,
                        3,
                        "Using maxAwaitTimeMS for awaitable hello protocol",
                        "maxAwaitTimeMS"_attr = maxAwaitTimeMS.value());

            curOp->pauseTimer();
            timerGuard.emplace([curOp]() { curOp->resumeTimer(); });
        } else {
            uassert(31368,
                    (clientTopologyVersion
                         ? "A request with a 'topologyVersion' must include 'maxAwaitTimeMS'"
                         : "A request with 'maxAwaitTimeMS' must include a 'topologyVersion'"),
                    !clientTopologyVersion && !maxAwaitTimeMS);
        }

        // Try to parse the optional 'helloOk' field. This should be provided on the initial
        // handshake for an incoming connection if the client supports the hello command. Clients
        // that specify 'helloOk' do not rely on "not master" error message parsing, which means
        // that we can safely return "not primary" error messages instead.
        if (auto helloOk = cmd.getHelloOk()) {
            // If the hello request contains a "helloOk" field, set _supportsHello on the Client
            // to the value.
            client->setSupportsHello(*helloOk);
            // Attach helloOk: true to the response so that the client knows the server supports
            // the hello command.
            result.append(HelloCommandReply::kHelloOkFieldName, true);
        }

        if (MONGO_unlikely(appendHelloOkToHelloResponse.shouldFail())) {
            result.append(HelloCommandReply::kClientSupportsHelloFieldName,
                          client->supportsHello());
        }

        auto currentTopologyVersion = appendReplicationInfo(
            opCtx, &result, 0, useLegacyResponseFields(), clientTopologyVersion, maxAwaitTimeMS);

        timerGuard.reset();  // Resume curOp timer.

        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            constexpr int kConfigServerModeNumber = 2;
            result.append(HelloCommandReply::kConfigsvrFieldName, kConfigServerModeNumber);
        }

        result.appendNumber(HelloCommandReply::kMaxBsonObjectSizeFieldName, BSONObjMaxUserSize);
        result.appendNumber(HelloCommandReply::kMaxMessageSizeBytesFieldName,
                            static_cast<long long>(MaxMessageSizeBytes));
        result.appendNumber(HelloCommandReply::kMaxWriteBatchSizeFieldName,
                            static_cast<long long>(write_ops::kMaxWriteBatchSize));
        result.appendDate(HelloCommandReply::kLocalTimeFieldName, jsTime());
        result.append(HelloCommandReply::kLogicalSessionTimeoutMinutesFieldName,
                      localLogicalSessionTimeoutMinutes);
        result.appendNumber(HelloCommandReply::kConnectionIdFieldName,
                            opCtx->getClient()->getConnectionId());


        if (auto wireSpec = WireSpec::getWireSpec(opCtx->getServiceContext()).get();
            cmd.getInternalClient()) {
            result.append(HelloCommandReply::kMinWireVersionFieldName,
                          wireSpec->incomingInternalClient.minWireVersion);
            result.append(HelloCommandReply::kMaxWireVersionFieldName,
                          wireSpec->incomingInternalClient.maxWireVersion);
        } else {
            result.append(HelloCommandReply::kMinWireVersionFieldName,
                          wireSpec->incomingExternalClient.minWireVersion);
            result.append(HelloCommandReply::kMaxWireVersionFieldName,
                          wireSpec->incomingExternalClient.maxWireVersion);
        }

        result.append(HelloCommandReply::kReadOnlyFieldName, opCtx->readOnly());

        if (auto param = ServerParameterSet::getNodeParameterSet()->getIfExists(
                kAutomationServiceDescriptorFieldName)) {
            param->append(opCtx, &result, kAutomationServiceDescriptorFieldName, boost::none);
        }

        if (opCtx->isExhaust()) {
            LOGV2_DEBUG(23905, 3, "Using exhaust for isMaster or hello protocol");

            uassert(51756,
                    "An isMaster or hello request with exhaust must specify 'maxAwaitTimeMS'",
                    maxAwaitTimeMS);
            invariant(clientTopologyVersion);

            InExhaustHello::get(opCtx->getClient()->session().get())
                ->setInExhaust(true /* inExhaust */, getName());

            if (clientTopologyVersion->getProcessId() == currentTopologyVersion.getProcessId() &&
                clientTopologyVersion->getCounter() == currentTopologyVersion.getCounter()) {
                // Indicate that an exhaust message should be generated and the previous BSONObj
                // command parameters should be reused as the next BSONObj command parameters.
                replyBuilder->setNextInvocation(boost::none);
            } else {
                BSONObjBuilder niBuilder;
                for (const auto& elem : cmdObj) {
                    if (elem.fieldNameStringData() == HelloCommand::kTopologyVersionFieldName) {
                        BSONObjBuilder tvBuilder(
                            niBuilder.subobjStart(HelloCommand::kTopologyVersionFieldName));
                        currentTopologyVersion.serialize(&tvBuilder);
                    } else {
                        niBuilder.append(elem);
                    }
                }
                replyBuilder->setNextInvocation(niBuilder.obj());
            }
        }

        handleHelloAuth(opCtx, dbName, cmd, &result);

        if (getTestCommandsEnabled()) {
            validateResult(&result);
        }
        return true;
    }

    void validateResult(BSONObjBuilder* result) {
        auto ret = result->asTempObj();
        if (ret[ErrorReply::kErrmsgFieldName].eoo()) {
            // Nominal success case, parse the object as-is.
            HelloCommandReply::parse(IDLParserContext{"hello.reply"}, ret);
        } else {
            // Something went wrong, still try to parse, but accept a few ignorable fields.
            StringDataSet ignorable({ErrorReply::kCodeFieldName, ErrorReply::kErrmsgFieldName});
            HelloCommandReply::parse(IDLParserContext{"hello.reply"}, ret.removeFields(ignorable));
        }
    }

protected:
    CmdHello(const StringData cmdName, const std::initializer_list<StringData>& alias)
        : BasicCommandWithReplyBuilderInterface(cmdName, alias) {}

    virtual bool useLegacyResponseFields() const {
        return false;
    }

private:
    // Fail point for Hello command, it blocks until disabled. Supported arguments:
    //   internalClient:  enabled only for internal clients
    //   notInternalClient: enabled only for non-internal clients
    static void _handleHelloFailPoint(const BSONObj& args,
                                      OperationContext* opCtx,
                                      const BSONObj& cmdObj) {
        if (args.hasElement("internalClient") && !cmdObj.hasElement("internalClient")) {
            LOGV2(5648901, "Fail point Hello is disabled for external client", "cmd"_attr = cmdObj);
            return;  // Filtered out not internal client.
        }
        if (args.hasElement("notInternalClient") && cmdObj.hasElement("internalClient")) {
            LOGV2(5648902, "Fail point Hello is disabled for internal client");
            return;  // Filtered out internal client.
        }
        if (args.hasElement("delayMillis")) {
            Milliseconds delay{args["delayMillis"].safeNumberLong()};
            LOGV2(6724102,
                  "Fail point delays Hello processing",
                  "cmd"_attr = cmdObj,
                  "client"_attr = opCtx->getClient()->clientAddress(true),
                  "desc"_attr = opCtx->getClient()->desc(),
                  "delay"_attr = delay);
            opCtx->sleepFor(delay);
            return;
        }
        // Default action is sleep.
        LOGV2(5648903,
              "Fail point blocks Hello response until removed",
              "cmd"_attr = cmdObj,
              "client"_attr = opCtx->getClient()->clientAddress(true),
              "desc"_attr = opCtx->getClient()->desc());
        shardWaitInHello.pauseWhileSet(opCtx);
    }
};
MONGO_REGISTER_COMMAND(CmdHello).forShard();

class CmdIsMaster : public CmdHello {
public:
    CmdIsMaster() : CmdHello(kCamelCaseIsMasterString, {kLowerCaseIsMasterString}) {}

    const std::set<std::string>& apiVersions() const final {
        return kNoApiVersions;
    }

    std::string help() const final {
        return "Check if this server is primary for a replica set\n"
               "{ isMaster : 1 }";
    }

protected:
    // Parse the command name, which should be one of the following: hello, isMaster, or
    // ismaster. If the command is "hello", we must attach an "isWritablePrimary" response field
    // instead of "ismaster" and "secondaryDelaySecs" response field instead of "slaveDelay".
    bool useLegacyResponseFields() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdIsMaster).forShard();

OpCounterServerStatusSection replOpCounterServerStatusSection("opcountersRepl", &replOpCounters);

}  // namespace

}  // namespace repl
}  // namespace mongo
