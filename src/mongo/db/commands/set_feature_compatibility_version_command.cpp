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


#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_ddl_coordinators_request_gen.h"
#include "mongo/db/index_names.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/db/shard_role/shard_catalog/drop_indexes.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/write_concern.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

using GenericFCV = multiversion::GenericFCV;
using FCV = multiversion::FeatureCompatibilityVersion;

MONGO_FAIL_POINT_DEFINE(failBeforeTransitioning);
MONGO_FAIL_POINT_DEFINE(failUpgrading);
MONGO_FAIL_POINT_DEFINE(hangWhileUpgrading);
MONGO_FAIL_POINT_DEFINE(failBeforeSendingShardsToDowngradingOrUpgrading);
MONGO_FAIL_POINT_DEFINE(failDowngrading);
MONGO_FAIL_POINT_DEFINE(hangWhileDowngrading);
MONGO_FAIL_POINT_DEFINE(hangBeforeUpdatingFcvDoc);
MONGO_FAIL_POINT_DEFINE(failBeforeUpdatingFcvDoc);
MONGO_FAIL_POINT_DEFINE(failTransitionDuringIsCleaningServerMetadata);
MONGO_FAIL_POINT_DEFINE(hangBeforeTransitioningToDowngraded);
MONGO_FAIL_POINT_DEFINE(hangTransitionBeforeIsCleaningServerMetadata);
MONGO_FAIL_POINT_DEFINE(failAfterReachingTransitioningState);
MONGO_FAIL_POINT_DEFINE(hangAtSetFCVStart);
MONGO_FAIL_POINT_DEFINE(failAfterSendingShardsToDowngradingOrUpgrading);
MONGO_FAIL_POINT_DEFINE(failDowngradeValidationDueToIncompatibleFeature);
MONGO_FAIL_POINT_DEFINE(failUpgradeValidationDueToIncompatibleFeature);
MONGO_FAIL_POINT_DEFINE(immediatelyTimeOutWaitForStaleOFCV);

/**
 * Ensures that only one instance of setFeatureCompatibilityVersion can run at a given time.
 */
ResourceMutex commandMutex("setFCVCommandMutex");

void abortAllReshardCollection(OperationContext* opCtx) {
    auto reshardingCoordinatorService = checked_cast<ReshardingCoordinatorService*>(
        repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
            ->lookupServiceByName(ReshardingCoordinatorService::kServiceName));
    // Skip the quiesce period to avoid blocking the FCV change. Please note that a resharding
    // operation only has a quiesce period if its resharding UUID was provided by the user which is
    // used for retryability.
    reshardingCoordinatorService->abortAllReshardCollection(
        opCtx, {resharding::kFCVChangeAbortReason, resharding::AbortType::kAbortSkipQuiesce});

    PersistentTaskStore<ReshardingCoordinatorDocument> store(
        NamespaceString::kConfigReshardingOperationsNamespace);

    std::vector<std::string> nsWithReshardColl;
    store.forEach(opCtx, {}, [&](const ReshardingCoordinatorDocument& doc) {
        nsWithReshardColl.push_back(NamespaceStringUtil::serialize(
            doc.getSourceNss(), SerializationContext::stateDefault()));
        return true;
    });

    if (!nsWithReshardColl.empty()) {
        std::string nsListStr;
        str::joinStringDelim(nsWithReshardColl, &nsListStr, ',');

        uasserted(
            ErrorCodes::ManualInterventionRequired,
            fmt::format(
                "reshardCollection was not properly cleaned up after attempted abort for these ns: "
                "[{}]. This is sign that the resharding operation was interrupted but not "
                "aborted.",
                nsListStr));
    }
}

void uassertStatusOKIgnoreNSNotFound(Status status) {
    if (status.isOK() || status == ErrorCodes::NamespaceNotFound) {
        return;
    }

    uassertStatusOK(status);
}

void cloneAuthoritativeDatabaseMetadataOnShards(OperationContext* opCtx) {
    // No shards should be added until we have forwarded the clone command to all shards. We use the
    // DDL lock here to serialize with all of add shard and to avoid deadlocks with the DDL blocking
    // used by add/remove shard.
    DDLLockManager::ScopedCollectionDDLLock ddlLock(opCtx,
                                                    NamespaceString::kConfigsvrShardsNamespace,
                                                    "CloneAuthoritativeDatabaseMetadata",
                                                    LockMode::MODE_S);

    // We do a direct read of the shards collection with local readConcern so no shards are missed,
    // but don't go through the ShardRegistry to prevent it from caching data that may be rolled
    // back.
    const auto opTimeWithShards =
        ShardingCatalogManager::get(opCtx)->localCatalogClient()->getAllShards(
            opCtx, repl::ReadConcernLevel::kLocalReadConcern);

    for (const auto& shardType : opTimeWithShards.value) {
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardType.getName());
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto shard = shardStatus.getValue();

        ShardsvrCloneAuthoritativeMetadata request;
        request.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
        request.setDbName(DatabaseName::kAdmin);

        auto response = shard->runCommand(opCtx,
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          DatabaseName::kAdmin,
                                          request.toBSON(),
                                          Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
    }
}

/**
 * Sets the minimum allowed feature compatibility version for the cluster. The cluster should not
 * use any new features introduced in binary versions that are newer than the feature compatibility
 * version set.
 *
 * Format:
 * {
 *   setFeatureCompatibilityVersion: <string version>
 * }
 */
class SetFeatureCompatibilityVersionCommand : public BasicCommand {
public:
    SetFeatureCompatibilityVersionCommand()
        : BasicCommand(SetFeatureCompatibilityVersion::kCommandName) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        std::stringstream h;
        h << "Set the featureCompatibilityVersion used by this cluster. If set to '"
          << multiversion::toString(GenericFCV::kLastLTS)
          << "', then features introduced in versions greater than '"
          << multiversion::toString(GenericFCV::kLastLTS) << "' will be disabled";
        if (GenericFCV::kLastContinuous != GenericFCV::kLastLTS) {
            h << " If set to '" << multiversion::toString(GenericFCV::kLastContinuous)
              << "', then features introduced in '" << multiversion::toString(GenericFCV::kLatest)
              << "' will be disabled.";
        }
        h << " If set to '" << multiversion::toString(GenericFCV::kLatest) << "', then '"
          << multiversion::toString(GenericFCV::kLatest)
          << "' features are enabled, and all nodes in the cluster must be binary version "
          << multiversion::toString(GenericFCV::kLatest) << ". See "
          << feature_compatibility_version_documentation::compatibilityLink() << ".";
        return h.str();
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forClusterResource(dbName.tenantId()),
                     ActionType::setFeatureCompatibilityVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    void checkBlockFCVChanges(OperationContext* opCtx,
                              const SetFeatureCompatibilityVersion& request) {
        DBDirectClient dbClient(opCtx);
        FindCommandRequest findRequest{NamespaceString::kBlockFCVChangesNamespace};
        const auto response = dbClient.findOne(std::move(findRequest));
        if (!response.isEmpty()) {
            // The block is skipped if the request is from a config server
            const auto fromConfigServer = request.getFromConfigServer().value_or(false);
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "setFeatureCompatibilityVersion command is blocked. It might be that the "
                    "replica set is being added to a sharded cluster. If this is the case, the "
                    "command cannot be run via direct replica set connection.",
                    fromConfigServer);
        }
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        hangAtSetFCVStart.pauseWhileSet(opCtx);

        uassert(ErrorCodes::IllegalOperation,
                "Changing FCV is prohibited with --replicaSetConfigShardMaintenanceMode",
                !serverGlobalParams.replicaSetConfigShardMaintenanceMode);

        // Ensure that this operation will be killed by the RstlKillOpThread during step-up or
        // stepdown.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto request = SetFeatureCompatibilityVersion::parse(
            cmdObj, IDLParserContext("setFeatureCompatibilityVersion"));

        auto isDryRun = request.getDryRun().value_or(false);

        uassert(ErrorCodes::InvalidOptions,
                "dry-run mode is not enabled",
                !request.getDryRun().has_value() ||
                    repl::feature_flags::gFeatureFlagSetFcvDryRunMode.isEnabled());

        auto skipDryRun = request.getSkipDryRun().value_or(false);

        uassert(ErrorCodes::InvalidOptions,
                "skipDryRun can not be used because dry-run mode is not enabled",
                !request.getSkipDryRun().has_value() ||
                    repl::feature_flags::gFeatureFlagSetFcvDryRunMode.isEnabled());

        // Ensure that `dryRun` and `skipDryRun` are not both set to `true`
        uassert(ErrorCodes::InvalidOptions,
                "The 'dryRun' and 'skipDryRun' options cannot both be set to true.",
                !(isDryRun && skipDryRun));

        auto isFromConfigServer = request.getFromConfigServer().value_or(false);

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        Lock::ExclusiveLock setFCVCommandLock(opCtx, commandMutex);

        // Check there is no block on setFeatureCompatibilityVersion.
        checkBlockFCVChanges(opCtx, request);

        const auto requestedVersion = request.getCommandParameter();
        const auto actualVersion =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();

        auto isConfirmed = request.getConfirm().value_or(false);
        const auto upgradeMsg = fmt::format(
            "Once you have upgraded to {}, you will not be able to downgrade FCV and binary "
            "version without support assistance. Please re-run this command with 'confirm: true' "
            "to acknowledge this and continue with the FCV upgrade.",
            multiversion::toString(requestedVersion));
        const auto downgradeMsg =
            "Once you have downgraded the FCV, if you choose to downgrade the binary version, "
            "it will require support assistance. Please re-run this command with 'confirm: "
            "true' to acknowledge this and continue with the FCV downgrade.";
        uassert(7369100,
                (requestedVersion > actualVersion ? upgradeMsg : downgradeMsg),
                // If the request is from a config svr, skip requiring the 'confirm: true'
                // parameter.
                (isFromConfigServer || isConfirmed || isDryRun));

        // Always wait for at least majority writeConcern to ensure all writes involved in the
        // upgrade/downgrade process cannot be rolled back. There is currently no mechanism to
        // specify a default writeConcern, so we manually call waitForWriteConcern upon exiting this
        // command.
        //
        // TODO SERVER-25778: replace this with the general mechanism for specifying a default
        // writeConcern.
        ON_BLOCK_EXIT([&] {
            if (isDryRun) {
                return;
            }
            WriteConcernResult res;
            auto waitForWCStatus = waitForWriteConcern(
                opCtx,
                repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                WriteConcernOptions(
                    repl::ReplSetConfig::kMajorityWriteConcernModeName,
                    WriteConcernOptions::SyncMode::UNSET,
                    // Propagate the user's wTimeout if one was given. Default is kNoTimeout.
                    opCtx->getWriteConcern().wTimeout),
                &res);
            CommandHelpers::appendCommandWCStatus(result, waitForWCStatus, res);
        });

        if (requestedVersion == actualVersion) {
            // Set the client's last opTime to the system last opTime so no-ops wait for
            // writeConcern. This will wait for any previous setFCV disk writes to be majority
            // committed before returning to the user, if the previous setFCV command had updated
            // the FCV but encountered failover afterwards.
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

            // _finalizeUpgrade and _finalizeDowngrade are only for any tasks that must be done to
            // fully complete the FCV upgrade AFTER the FCV document has already been updated to the
            // UPGRADED/DOWNGRADED FCV. We call it here because it's possible that during an FCV
            // upgrade/downgrade, the replset/shard server/config server undergoes failover AFTER
            // the FCV document has already been updated to the UPGRADED/DOWNGRADED FCV, but before
            // the cluster has completed _finalize*. In this case, since the cluster failed over,
            // the user/client may retry sending the setFCV command to the cluster, but the cluster
            // is already in the requestedVersion (i.e. requestedVersion == actualVersion). However,
            // the cluster should retry/complete the tasks from _finalize* before sending ok:1
            // back to the user/client. Therefore, these tasks **must** be idempotent/retryable.
            if (!isDryRun) {
                _finalizeUpgrade(opCtx, requestedVersion);
                _finalizeDowngrade(opCtx, requestedVersion);
            }
            return true;
        }

        const auto upgradeOrDowngrade = requestedVersion > actualVersion ? "upgrade" : "downgrade";
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const auto serverType = !role || role->has(ClusterRole::None)
            ? "replica set/maintenance mode"
            : (role->has(ClusterRole::ConfigServer) ? "config server" : "shard server");

        if ((!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kStart) && !isDryRun) {
            LOGV2(6744300,
                  "setFeatureCompatibilityVersion command called",
                  "upgradeOrDowngrade"_attr = upgradeOrDowngrade,
                  "serverType"_attr = serverType,
                  "fromVersion"_attr = actualVersion,
                  "toVersion"_attr = requestedVersion);
        }

        auto resolvedTransition =
            FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
                opCtx, request, actualVersion);
        const boost::optional<Timestamp>& changeTimestamp = resolvedTransition.changeTimestamp;

        uassert(5563600,
                "'phase' field is only valid to be specified on shards",
                !request.getPhase() || (role && role->has(ClusterRole::ShardServer)));
        // TODO (SERVER-97816): Remove feature flag check once 9.0 becomes last lts.
        uassert(
            1034131,
            "'phase' field must be present on shards",
            !feature_flags::gUseTopologyChangeCoordinators.isEnabledOnVersion(requestedVersion) ||
                isDryRun || request.getPhase() || (!role || !role->isShardOnly()));

        if (isDryRun) {
            processDryRun(opCtx, request, requestedVersion, actualVersion);
            return true;
        }

        // ---------- kStart phase (Enter transitional FCV) ----------
        if (resolvedTransition.shouldRun(SetFCVPhaseEnum::kStart)) {
            // Automatic dryRun processing only if skipDryRun is false and the role is either the
            // config server or not part of a sharded cluster
            if (repl::feature_flags::gFeatureFlagSetFcvDryRunMode.isEnabled() && !skipDryRun &&
                (!role || !role->isShardOnly())) {
                processDryRun(opCtx, request, requestedVersion, actualVersion);
            }

            FCVStepRegistry::get(opCtx->getServiceContext())
                .beforeStartWithoutFCVLock(opCtx, actualVersion, requestedVersion);

            {
                // Start transition to 'requestedVersion' by updating the local FCV document to a
                // 'kUpgrading' or 'kDowngrading' state, respectively.
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

                uassert(ErrorCodes::Error(6744303),
                        "Failing setFeatureCompatibilityVersion before reaching the FCV "
                        "transitional stage due to 'failBeforeTransitioning' failpoint set",
                        !failBeforeTransitioning.shouldFail());

                if (role && role->has(ClusterRole::ConfigServer)) {
                    uassert(
                        ErrorCodes::ConflictingOperationInProgress,
                        "Failed to start FCV change because an addShardCoordinator is in progress",
                        ShardingCoordinatorService::getService(opCtx)
                            ->areAllCoordinatorsOfTypeFinished(opCtx,
                                                               CoordinatorTypeEnum::kAddShard));
                }

                // If this is a config server, then there must be no active
                // SetClusterParameterCoordinator instances active when downgrading.
                if (role && role->has(ClusterRole::ConfigServer) &&
                    requestedVersion < actualVersion) {
                    uassert(ErrorCodes::ConflictingOperationInProgress,
                            "Cannot downgrade while cluster server parameters are being set",
                            (ConfigsvrCoordinatorService::getService(opCtx)
                                 ->areAllCoordinatorsOfTypeFinished(
                                     opCtx, ConfigsvrCoordinatorTypeEnum::kSetClusterParameter)));
                }

                FCVStepRegistry::get(opCtx->getServiceContext())
                    .beforeStartWithFCVLock(opCtx, actualVersion, requestedVersion);

                // We pass boost::none as the setIsCleaningServerMetadata argument in order to
                // indicate that we don't want to override the existing isCleaningServerMetadata FCV
                // doc field. This is to protect against the case where a previous FCV transition
                // failed in the isCleaningServerMetadata phase, and the user runs setFCV again. In
                // that case we do not want to remove the existing isCleaningServerMetadata FCV doc
                // field because it would not be safe to upgrade the FCV.
                FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                    opCtx,
                    resolvedTransition.transitionalVersion,
                    SetFCVPhaseEnum::kStart,
                    changeTimestamp,
                    boost::none /* setIsCleaningServerMetadata */);

                LOGV2(6744301,
                      "setFeatureCompatibilityVersion has set the FCV to the transitional state",
                      "upgradeOrDowngrade"_attr = upgradeOrDowngrade,
                      "serverType"_attr = serverType,
                      "fromVersion"_attr = actualVersion,
                      "toVersion"_attr = requestedVersion);
            }

            uassert(ErrorCodes::Error(7555200),
                    "Failing upgrade or downgrade due to 'failAfterReachingTransitioningState' "
                    "failpoint set",
                    !failAfterReachingTransitioningState.shouldFail());

            if (role && role->has(ClusterRole::ShardServer)) {
                // This helper function is only for any actions that should be done specifically on
                // shard servers during phase 1 of the 3-phase setFCV protocol for sharded clusters.
                // For example, before completing phase 1, we must wait for backward incompatible
                // ShardingCoordinators to finish.
                // We do not expect any other feature-specific work to be done in the 'start' phase.
                _shardServerPhase1Tasks(opCtx, requestedVersion);
            }

            if (role && role->has(ClusterRole::ConfigServer)) {
                uassert(ErrorCodes::Error(6794600),
                        "Failing downgrade due to "
                        "'failBeforeSendingShardsToDowngradingOrUpgrading' failpoint set",
                        !failBeforeSendingShardsToDowngradingOrUpgrading.shouldFail());

                // Always abort the reshardCollection regardless of version to ensure that it
                // will run on a consistent version from start to finish. This will ensure that
                // it will be able to apply the oplog entries correctly.
                abortAllReshardCollection(opCtx);

                // Tell the shards to enter 'start' phase of setFCV (transition to kDowngrading).
                _sendEnterSetFCVPhaseRequestToShard(
                    opCtx, request, changeTimestamp, SetFCVPhaseEnum::kStart);

                // The config server may also be a shard, so have it run any shard server tasks.
                // Run this after sending the first phase to shards so they enter the transition
                // state even if this throws.
                _shardServerPhase1Tasks(opCtx, requestedVersion);
            }
        }

        // ---------- kPrepare phase (Feasibility Check) ----------
        if (resolvedTransition.shouldRun(SetFCVPhaseEnum::kPrepare)) {
            invariant(serverGlobalParams.featureCompatibility.acquireFCVSnapshot()
                          .isUpgradingOrDowngrading());

            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx,
                resolvedTransition.transitionalVersion,
                SetFCVPhaseEnum::kPrepare,
                changeTimestamp,
                boost::none /* setIsCleaningServerMetadata*/);

            uassert(ErrorCodes::Error(7555202),
                    "Failing downgrade due to "
                    "'failAfterSendingShardsToDowngradingOrUpgrading' failpoint set",
                    !failAfterSendingShardsToDowngradingOrUpgrading.shouldFail());

            // Any checks and actions that need to be performed before being able to downgrade needs
            // to be placed on the _prepareToUpgrade and _prepareToDowngrade functions. After the
            // prepare function complete, a node is not allowed to refuse to upgrade/downgrade.
            if (requestedVersion > actualVersion) {
                _prepareToUpgrade(opCtx, request, changeTimestamp);
            } else {
                _prepareToDowngrade(opCtx, request, changeTimestamp);
            }

            if (role && role->has(ClusterRole::ConfigServer)) {
                // Tell the shards to enter the 'prepare' phase of setFCV (check that they will be
                // able to upgrade or downgrade).
                _sendEnterSetFCVPhaseRequestToShard(
                    opCtx, request, changeTimestamp, SetFCVPhaseEnum::kPrepare);
            }
        }

        // ---------- kComplete phase (Metadata changes & Transition to Target FCV) ----------
        if (resolvedTransition.shouldRun(SetFCVPhaseEnum::kComplete)) {
            invariant(serverGlobalParams.featureCompatibility.acquireFCVSnapshot()
                          .isUpgradingOrDowngrading());

            const bool isDowngradeTransition = requestedVersion < actualVersion;
            if (isDowngradeTransition ||
                repl::feature_flags::gFeatureFlagUpgradingToDowngrading.isEnabled()) {

                hangTransitionBeforeIsCleaningServerMetadata.pauseWhileSet(opCtx);
                // Set the isCleaningServerMetadata field to true. This prohibits the upgradingTo
                // Downgrading/ downgradingToUpgrading transition until the isCleaningServerMetadata
                // is unset when we successfully finish the FCV upgrade/downgrade and transition to
                // the upgraded/downgraded state.
                {
                    const auto fcvChangeRegion(
                        FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));
                    FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                        opCtx,
                        resolvedTransition.transitionalVersion,
                        SetFCVPhaseEnum::kComplete,
                        changeTimestamp,
                        true /* setIsCleaningServerMetadata*/);
                }

                uassert(ErrorCodes::Error(10778000),
                        "Failing transition due to 'failTransitionDuringIsCleaningServerMetadata' "
                        "failpoint set",
                        !failTransitionDuringIsCleaningServerMetadata.shouldFail());
            }

            // All feature-specific FCV upgrade or downgrade code should go into the respective
            // _runUpgrade and _runDowngrade functions. Each of them have their own helper functions
            // where all feature-specific upgrade/downgrade code should be placed. Please read the
            // comments on the helper functions for more details on where to place the code.
            if (isDowngradeTransition) {
                _runDowngrade(opCtx, request, changeTimestamp);
            } else {
                _runUpgrade(opCtx, request, changeTimestamp);
            }

            {
                // Complete transition by updating the local FCV document to the fully upgraded or
                // downgraded requestedVersion.
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

                uassert(ErrorCodes::Error(6794601),
                        "Failing downgrade due to 'failBeforeUpdatingFcvDoc' failpoint set",
                        !failBeforeUpdatingFcvDoc.shouldFail());

                hangBeforeUpdatingFcvDoc.pauseWhileSet();

                FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                    opCtx,
                    requestedVersion,
                    boost::none, /* phase */
                    changeTimestamp,
                    false /* setIsCleaningServerMetadata */);
            }

            // _finalizeUpgrade/_finalizeDowngrade are only for any tasks that must be done to fully
            // complete the FCV change AFTER the FCV document has already been updated to the
            // requested FCV. This is because there are feature flags that only change value once
            // the FCV document is on the requested value. Everything in these functions **must** be
            // idempotent/retryable.
            if (requestedVersion > actualVersion) {
                _finalizeUpgrade(opCtx, requestedVersion);
            } else {
                _finalizeDowngrade(opCtx, requestedVersion);
            }

            LOGV2(6744302,
                  "setFeatureCompatibilityVersion succeeded",
                  "upgradeOrDowngrade"_attr = upgradeOrDowngrade,
                  "serverType"_attr = serverType,
                  "fromVersion"_attr = actualVersion,
                  "toVersion"_attr = requestedVersion);
        }

        return true;
    }

private:
    // This helper function is only for any actions that should be done specifically on
    // shard servers during phase 1 of the 3-phase setFCV protocol for sharded clusters.
    // For example, before completing phase 1, we must wait for backward incompatible
    // ShardingCoordinators to finish. This is important in order to ensure that no
    // shard that is currently a participant of such a backward-incompatible
    // ShardingCoordinator can transition to the fully downgraded state (and thus,
    // possibly downgrade its binary) while the coordinator is still in progress.
    // The fact that the FCV has already transitioned to kDowngrading ensures that no
    // new backward-incompatible ShardingCoordinators can start.
    // We do not expect any other feature-specific work to be done in the 'start' phase.
    void _shardServerPhase1Tasks(OperationContext* opCtx, FCV requestedVersion) {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto originalVersion = getTransitionFCVInfo(fcvSnapshot.getVersion()).from;
        const auto isDowngrading = originalVersion > requestedVersion;
        const auto isUpgrading = originalVersion < requestedVersion;

        if (isDowngrading) {
            FCVStepRegistry::get(opCtx->getServiceContext())
                .drainingOnDowngrade(opCtx, originalVersion, requestedVersion);

            // TODO SERVER-99655: update once gSnapshotFCVInDDLCoordinators is enabled
            // on the lastLTS
            if (feature_flags::gSnapshotFCVInDDLCoordinators.isEnabledOnVersion(originalVersion)) {
                ShardingCoordinatorService::getService(opCtx)
                    ->waitForCoordinatorsOfGivenOfcvToComplete(
                        opCtx, [originalVersion](boost::optional<FCV> ofcv) -> bool {
                            return ofcv == originalVersion;
                        });
            } else {
                // TODO SERVER-77915: Remove once v8.0 branches out
                if (feature_flags::gTrackUnshardedCollectionsUponMoveCollection
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, CoordinatorTypeEnum::kRenameCollection);
                }

                // TODO (SERVER-100309): Remove once 9.0 becomes last lts.
                if (feature_flags::gSessionsCollectionCoordinatorOnConfigServer
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, CoordinatorTypeEnum::kCreateCollection);
                }

                // TODO SERVER-77915: Remove once v8.0 branches out.
                if (feature_flags::gTrackUnshardedCollectionsUponMoveCollection
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(opCtx,
                                                                   CoordinatorTypeEnum::kCollMod);
                }

                // TODO (SERVER-97816): Remove once 9.0 becomes last lts.
                if (feature_flags::gUseTopologyChangeCoordinators
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, CoordinatorTypeEnum::kRemoveShardCommit);
                }
            }
        }

        if (isUpgrading) {
            if (feature_flags::gSnapshotFCVInDDLCoordinators.isEnabledOnVersion(requestedVersion)) {
                // Wait until all sharding coordinators that run are on the kUpgrading* FCV
                ShardingCoordinatorService::getService(opCtx)
                    ->waitForCoordinatorsOfGivenOfcvToComplete(
                        opCtx, [fcvSnapshot](boost::optional<FCV> ofcv) -> bool {
                            return ofcv != fcvSnapshot.getVersion();
                        });
            } else {
                // TODO (SERVER-98118): remove once 9.0 becomes last LTS.
                if (feature_flags::gShardAuthoritativeDbMetadataDDL
                        .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    // Since we have a feature flag changing value in kUpgrading, we need to drain
                    // coordinators that started in FCV 8.0. waitForOngoingCoordinatorsToFinish may
                    // also wait for coordinators that started AFTER the transition to kUpgrading.
                    // That's OK, it's a performance penalty, but there is no correctness issue.
                    ShardingCoordinatorService::getService(opCtx)
                        ->waitForOngoingCoordinatorsToFinish(
                            opCtx, [](const ShardingCoordinator& coordinatorInstance) -> bool {
                                static constexpr std::array drainCoordinatorTypes{
                                    CoordinatorTypeEnum::kMovePrimary,
                                    CoordinatorTypeEnum::kDropDatabase,
                                    CoordinatorTypeEnum::kCreateDatabase,
                                };
                                const auto opType = coordinatorInstance.operationType();
                                return std::ranges::any_of(drainCoordinatorTypes, [&](auto&& type) {
                                    return opType == type;
                                });
                            });
                }
            }
        }
    }

    // This helper function is for any validation in user collections to ensure compatibility with
    // the requested FCV before an upgrade. This function must not modify any user data or system
    // state; it only performs precondition checks. Any validation failure would result in transient
    // errors that can be retried (like InterruptedDueToReplStateChange) or an
    // ErrorCode::CannotUpgrade, which requires manual cleanup of user data before retrying the
    // upgrade.  The code added/modified in this helper function should not leave the server in an
    // inconsistent state if the actions in this function failed part way through. The code in this
    // helper function is required to be idempotent in case the node crashes or upgrade fails in a
    // way that the user has to run setFCV again.
    void _userCollectionsUassertsForUpgrade(OperationContext* opCtx,
                                            const FCV requestedVersion,
                                            const FCV originalVersion) {

        invariant(!ServerGlobalParams::FCVSnapshot::isUpgradingOrDowngrading(originalVersion));
        auto role = ShardingState::get(opCtx)->pollClusterRole();

        if (MONGO_unlikely(failUpgradeValidationDueToIncompatibleFeature.shouldFail())) {
            uasserted(ErrorCodes::CannotUpgrade,
                      "Simulated dry-run validation failure via fail point.");
        }

        FCVStepRegistry::get(opCtx->getServiceContext())
            .userCollectionsUassertsForUpgrade(opCtx, originalVersion, requestedVersion);
    }

    // This helper function is for any actions that should be done before taking the global lock in
    // S mode. It is required that the code in this helper function is idempotent and could be done
    // after _runDowngrade even if it failed at any point in the middle of
    // _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
    void _prepareToUpgradeActionsBeforeGlobalLock(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        boost::optional<Timestamp> changeTimestamp) {
        const auto originalVersion =
            getTransitionFCVInfo(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
                .from;

        FCVStepRegistry::get(opCtx->getServiceContext())
            .prepareToUpgradeActionsBeforeGlobalLock(opCtx, originalVersion, requestedVersion);
    }

    // This helper function is for any user collections creations, changes or deletions that need
    // to happen during the upgrade. It is required that the code in this helper function is
    // idempotent and could be done after _runDowngrade even if it failed at any point in the middle
    // of _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
    void _userCollectionsWorkForUpgrade(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto originalVersion = getTransitionFCVInfo(fcvSnapshot.getVersion()).from;

        FCVStepRegistry::get(opCtx->getServiceContext())
            .userCollectionsWorkForUpgrade(opCtx, originalVersion, requestedVersion);
    }

    // This helper function is for updating server metadata to make sure the new features in the
    // upgraded version work for sharded and non-sharded clusters. It is required that the code
    // in this helper function is idempotent and could be done after _runDowngrade even if it
    // failed at any point in the middle of _userCollectionsUassertsForDowngrade or
    // _internalServerCleanupForDowngrade.
    void _upgradeServerMetadata(OperationContext* opCtx, const FCV requestedVersion) {
        const auto originalVersion =
            getTransitionFCVInfo(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
                .from;

        FCVStepRegistry::get(opCtx->getServiceContext())
            .upgradeServerMetadata(opCtx, originalVersion, requestedVersion);
    }

    // _prepareToUpgrade performs all actions and checks that need to be done before proceeding to
    // make any metadata changes as part of FCV upgrade. Any new feature specific upgrade code
    // should be placed in the _prepareToUpgrade helper functions:
    //  * _prepareToUpgradeActionsBeforeGlobalLock: for any actions that need to be done before
    //  acquiring the global lock
    //  * _userCollectionsUassertsForUpgrade: for any checks on user data or settings that will
    //    uassert with the `CannotUpgrade` code if users need to manually clean up.
    //  * _userCollectionsWorkForUpgrade: for any user collections creations, changes or deletions
    //    that need to happen during the upgrade. This happens after the global lock.
    // Please read the comments on those helper functions for more details on what should be placed
    // in each function.
    void _prepareToUpgrade(OperationContext* opCtx,
                           const SetFeatureCompatibilityVersion& request,
                           boost::optional<Timestamp> changeTimestamp) {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto originalVersion = getTransitionFCVInfo(fcvSnapshot.getVersion()).from;
        const auto requestedVersion = request.getCommandParameter();

        _prepareToUpgradeActionsBeforeGlobalLock(opCtx, requestedVersion, changeTimestamp);

        // This wait serves as a barrier to guarantee that, from now on:
        // - No operations with an OFCV lower than the upgrading OFCV will be running
        // - All operations acquiring the global lock in X/IX mode see the 'kUpgrading' FCV state
        _waitForOperationsRelyingOnStaleFcvToComplete(opCtx, fcvSnapshot.getVersion());

        _userCollectionsUassertsForUpgrade(opCtx, requestedVersion, originalVersion);

        auto role = ShardingState::get(opCtx)->pollClusterRole();

        // This helper function is for any user collections creations, changes or deletions that
        // need to happen during the upgrade. It is required that the code in this helper function
        // is idempotent and could be done after _runDowngrade even if it failed at any point in the
        // middle of _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
        if (!role || role->has(ClusterRole::None) || role->has(ClusterRole::ShardServer)) {
            _userCollectionsWorkForUpgrade(opCtx, requestedVersion);
        }

        // Run the authoritative clone phase on ALL shards (including the config
        // server if it's also a shard).
        if (role && role->has(ClusterRole::ConfigServer)) {
            if (feature_flags::gShardAuthoritativeDbMetadataDDL
                    .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion,
                                                                  originalVersion)) {
                cloneAuthoritativeDatabaseMetadataOnShards(opCtx);
            }
        }

        uassert(ErrorCodes::Error(549180),
                "Failing upgrade due to 'failUpgrading' failpoint set",
                !failUpgrading.shouldFail());
    }

    // _runUpgrade performs all the metadata-changing actions of an FCV upgrade. Any new feature
    // specific upgrade code should be placed in the _runUpgrade helper functions:
    //  * _upgradeServerMetadata: for updating server metadata to make sure the new features in the
    //  upgraded
    //    version work for sharded and non-sharded clusters
    // Please read the comments on those helper functions for more details on what should be placed
    // in each function.
    void _runUpgrade(OperationContext* opCtx,
                     const SetFeatureCompatibilityVersion& request,
                     boost::optional<Timestamp> changeTimestamp) {
        const auto requestedVersion = request.getCommandParameter();
        auto role = ShardingState::get(opCtx)->pollClusterRole();

        if (role && role->has(ClusterRole::ConfigServer)) {
            // Tell the shards to complete setFCV (transition to fully upgraded)
            _sendEnterSetFCVPhaseRequestToShard(
                opCtx, request, changeTimestamp, SetFCVPhaseEnum::kComplete);
        }

        // This helper function is for updating server metadata to make sure the new features in the
        // upgraded version work for sharded and non-sharded clusters. It is required that the code
        // in this helper function is idempotent and could be done after _runDowngrade even if it
        // failed at any point in the middle of _userCollectionsUassertsForDowngrade or
        // _internalServerCleanupForDowngrade.
        _upgradeServerMetadata(opCtx, requestedVersion);

        hangWhileUpgrading.pauseWhileSet(opCtx);
    }

    // This helper function is for any actions that should be done before taking the global lock in
    // S mode.
    void _prepareToDowngradeActions(OperationContext* opCtx, const FCV requestedVersion) {
        const auto originalVersion =
            getTransitionFCVInfo(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
                .from;

        FCVStepRegistry::get(opCtx->getServiceContext())
            .prepareToDowngradeActions(opCtx, originalVersion, requestedVersion);
    }

    /**
     * This function:
     * - Waits for operations using a stale OFCV to complete
     * - Acquires the global lock in shared mode to make sure that:
     * --- All operations that may potentially have started on an old FCV complete
     * --- All new operations are guaranteed to see at least the current FCV state
     */
    void _waitForOperationsRelyingOnStaleFcvToComplete(OperationContext* opCtx, FCV version) {
        auto waitForStaleOFcvDeadline = Date_t::max();
        if (MONGO_unlikely(immediatelyTimeOutWaitForStaleOFCV.shouldFail())) {
            waitForStaleOFcvDeadline = Date_t::now();
        }
        waitForOperationsNotMatchingVersionContextToComplete(
            opCtx, VersionContext(version), waitForStaleOFcvDeadline);

        // Take the global lock in S mode to create a barrier for operations taking the global
        // IX or X locks. This ensures that either:
        //   - The global IX/X locked operation will start after the FCV change, see the
        //     updated server FCV value and act accordingly.
        //   - The global IX/X locked operation began prior to the FCV change, is acting on that
        //     assumption and will finish before upgrade/downgrade metadata cleanup procedures done
        //     right after this barrier.
        Lock::GlobalLock lk(opCtx, MODE_S);
    }

    // Tell the shards to enter phase-1 or phase-2 of setFCV.
    void _sendEnterSetFCVPhaseRequestToShard(OperationContext* opCtx,
                                             const SetFeatureCompatibilityVersion& request,
                                             boost::optional<Timestamp> changeTimestamp,
                                             enum mongo::SetFCVPhaseEnum phase) {
        auto requestPhase = request;
        requestPhase.setFromConfigServer(true);
        requestPhase.setPhase(phase);
        requestPhase.setChangeTimestamp(changeTimestamp);
        generic_argument_util::setMajorityWriteConcern(requestPhase);
        uassertStatusOK(ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
            opCtx, CommandHelpers::filterCommandRequestForPassthrough(requestPhase.toBSON())));
    }

    // This helper function is for any uasserts for users to clean up user collections. Uasserts for
    // users to change settings or wait for settings to change should also happen here. These
    // uasserts happen before the internal server downgrade cleanup. This function must not modify
    // any user data or system state. It only checks preconditions for downgrades and fails if they
    // are unmet. The code in this helper function is required to be idempotent in case the node
    // crashes or downgrade fails in a way that the user has to run setFCV again. The code
    // added/modified in this helper function should not leave the server in an inconsistent state
    // if the actions in this function failed part way through. This helper function can only fail
    // with some transient error that can be retried (like InterruptedDueToReplStateChange) or
    // ErrorCode::CannotDowngrade. The uasserts added to this helper function can only have the
    // CannotDowngrade error code indicating that the user must manually clean up some user data in
    // order to retry the FCV downgrade.
    void _userCollectionsUassertsForDowngrade(OperationContext* opCtx,
                                              const FCV requestedVersion,
                                              const FCV originalVersion) {

        invariant(!ServerGlobalParams::FCVSnapshot::isUpgradingOrDowngrading(originalVersion));
        auto role = ShardingState::get(opCtx)->pollClusterRole();

        if (MONGO_unlikely(failDowngradeValidationDueToIncompatibleFeature.shouldFail())) {
            uasserted(ErrorCodes::CannotDowngrade,
                      "Simulated dry-run validation failure via fail point.");
        }
        FCVStepRegistry::get(opCtx->getServiceContext())
            .userCollectionsUassertsForDowngrade(opCtx, originalVersion, requestedVersion);
    }

    // This helper function is for any internal server downgrade cleanup, such as dropping
    // collections or aborting. This cleanup will happen after user collection downgrade
    // cleanup.
    // The code in this helper function is required to be IDEMPOTENT and RETRYABLE in case the
    // node crashes or downgrade fails in a way that the user has to run setFCV again. It cannot
    // fail for a non-retryable reason since at this point user data has already been cleaned
    // up.
    // It also MUST be able to be rolled back. This is because we cannot guarantee the safety of
    // any server metadata that is not replicated in the event of a rollback.
    //
    // This helper function can only fail with some transient error that can be retried
    // (like InterruptedDueToReplStateChange), ManualInterventionRequired, or fasserts. For
    // any non-retryable error in this helper function, it should error either with an
    // uassert with ManualInterventionRequired as the error code (indicating a server bug
    // but that all the data is consistent on disk and for reads/writes) or with an fassert
    // (indicating a server bug and that the data is corrupted). ManualInterventionRequired
    // and fasserts are errors that are not expected to occur in practice, but if they did,
    // they would turn into a Support case.
    void _internalServerCleanupForDowngrade(OperationContext* opCtx,
                                            const FCV originalVersion,
                                            const FCV requestedVersion) {

        FCVStepRegistry::get(opCtx->getServiceContext())
            .internalServerCleanupForDowngrade(opCtx, originalVersion, requestedVersion);
    }

    // _prepareToDowngrade performs all actions and checks that need to be done before proceeding to
    // make any metadata changes as part of FCV downgrade. Any new feature specific downgrade code
    // should be placed in the helper functions:
    // * _prepareToDowngradeActions: Any downgrade actions that should be done before taking the FCV
    // global lock in S mode should go in this function.
    // * _userCollectionsUassertsForDowngrade: for any checks on user data or settings that will
    // uassert if users need to manually clean up user data or settings.
    // When doing feature flag checking for downgrade, we should check the feature flag is enabled
    // on current FCV and will be disabled after downgrade by using
    // isDisabledOnTargetFCVButEnabledOnOriginalFCV(targetFCV, originalFCV) Please read the comments
    // on those helper functions for more details on what should be placed in each function.
    void _prepareToDowngrade(OperationContext* opCtx,
                             const SetFeatureCompatibilityVersion& request,
                             boost::optional<Timestamp> changeTimestamp) {
        const auto requestedVersion = request.getCommandParameter();

        // Any actions that should be done before taking the global lock in S mode should go in
        // this function.
        _prepareToDowngradeActions(opCtx, requestedVersion);

        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());

        // This wait serves as a barrier to gurantee that, from now on:
        // - No operations with an OFCV greater than the downgrading OFCV will be running
        // - All operations acquiring the global lock in X/IX mode see the 'kDowngrading' FCV state
        _waitForOperationsRelyingOnStaleFcvToComplete(opCtx, fcvSnapshot.getVersion());

        uassert(ErrorCodes::Error(549181),
                "Failing downgrade due to 'failDowngrading' failpoint set",
                !failDowngrading.shouldFail());
        hangWhileDowngrading.pauseWhileSet(opCtx);

        // This helper function is for any uasserts for users to clean up user collections. Uasserts
        // for users to change settings or wait for settings to change should also happen here.
        // These uasserts happen before the internal server downgrade cleanup. The code in this
        // helper function is required to be idempotent in case the node crashes or downgrade fails
        // in a way that the user has to run setFCV again. The code added/modified in this helper
        // function should not leave the server in an inconsistent state if the actions in this
        // function failed part way through.
        // This helper function can only fail with some transient error that can be retried (like
        // InterruptedDueToReplStateChange) or ErrorCode::CannotDowngrade. The uasserts added to
        // this helper function can only have the CannotDowngrade error code indicating that the
        // user must manually clean up some user data in order to retry the FCV downgrade.

        const auto originalVersion = getTransitionFCVInfo(fcvSnapshot.getVersion()).from;
        _userCollectionsUassertsForDowngrade(opCtx, requestedVersion, originalVersion);
    }

    // _runDowngrade performs all the metadata-changing actions of an FCV downgrade. Any new feature
    // specific downgrade code should be placed in the _runDowngrade helper functions:
    // * _internalServerCleanupForDowngrade: for any internal server downgrade cleanup
    // When doing feature flag checking for downgrade, we should check the feature flag is enabled
    // on current FCV and will be disabled after downgrade by using
    // isDisabledOnTargetFCVButEnabledOnOriginalFCV(targetFCV, originalFCV)
    // Please read the comments on those helper functions for more details on what should be placed
    // in each function.
    void _runDowngrade(OperationContext* opCtx,
                       const SetFeatureCompatibilityVersion& request,
                       boost::optional<Timestamp> changeTimestamp) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        const auto requestedVersion = request.getCommandParameter();

        // This helper function is for any internal server downgrade cleanup, such as dropping
        // collections or aborting. This cleanup will happen after user collection downgrade
        // cleanup.
        // The code in this helper function is required to be IDEMPOTENT and RETRYABLE in case the
        // node crashes or downgrade fails in a way that the user has to run setFCV again. It cannot
        // fail for a non-retryable reason since at this point user data has already been cleaned
        // up.
        // It also MUST be able to be rolled back. This is because we cannot guarantee the safety of
        // any server metadata that is not replicated in the event of a rollback.
        //
        // This helper function can only fail with some transient error that can be retried
        // (like InterruptedDueToReplStateChange), ManualInterventionRequired, or fasserts. For
        // any non-retryable error in this helper function, it should error either with an
        // uassert with ManualInterventionRequired as the error code (indicating a server bug
        // but that all the data is consistent on disk and for reads/writes) or with an fassert
        // (indicating a server bug and that the data is corrupted). ManualInterventionRequired
        // and fasserts are errors that are not expected to occur in practice, but if they did,
        // they would turn into a Support case.
        _internalServerCleanupForDowngrade(
            opCtx, getTransitionFCVInfo(fcvSnapshot.getVersion()).from, requestedVersion);

        if (role && role->has(ClusterRole::ConfigServer)) {
            // Tell the shards to complete setFCV (transition to fully downgraded).
            _sendEnterSetFCVPhaseRequestToShard(
                opCtx, request, changeTimestamp, SetFCVPhaseEnum::kComplete);
        }

        hangBeforeTransitioningToDowngraded.pauseWhileSet(opCtx);
    }

    // _finalizeUpgrade is only for any tasks that must be done to fully complete the FCV upgrade
    // AFTER the FCV document has already been updated to the UPGRADED FCV.
    // This is because during _runUpgrade, the FCV is still in the transitional state (which behaves
    // like the downgraded FCV), so certain tasks cannot be done yet until the FCV is fully
    // upgraded.
    // Additionally, it's possible that during an FCV upgrade, the replset/shard server/config
    // server undergoes failover AFTER the FCV document has already been updated to the UPGRADED
    // FCV, but before the cluster has completed _finalizeUpgrade. In this case, since the cluster
    // failed over, the user/client may retry sending the setFCV command to the cluster, but the
    // cluster is already in the requestedVersion (i.e. requestedVersion == actualVersion). However,
    // the cluster should retry/complete the tasks from _finalizeUpgrade before sending ok:1
    // back to the user/client. Therefore, these tasks **must** be idempotent/retryable.
    void _finalizeUpgrade(OperationContext* opCtx, const FCV requestedVersion) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();

        // Drain outstanding DDL operations that are incompatible with the target FCV.
        if (role && role->has(ClusterRole::ShardServer)) {
            // TODO SERVER-99655: remove the comment below.
            // The draining logic relies on the OFCV infrastructure, which has been introduced in
            // FCV 8.2 and may behave sub-optimally when requestedVersion is lower than 8.2.
            ShardingCoordinatorService::getService(opCtx)->waitForCoordinatorsOfGivenOfcvToComplete(
                opCtx, [requestedVersion](boost::optional<FCV> ofcv) -> bool {
                    return ofcv != requestedVersion;
                });
        }

        // This wait serves as a barrier to gurantee that, from now on:
        // - No operations with OFCV lower than the target version will be running
        // - All operations acquiring the global lock in X/IX mode see the fully upgraded FCV state
        _waitForOperationsRelyingOnStaleFcvToComplete(opCtx, requestedVersion);

        FCVStepRegistry::get(opCtx->getServiceContext()).finalizeUpgrade(opCtx, requestedVersion);
    }

    // _finalizeDowngrade is analogous to _finalizeUpgrade, but runs on downgrade. As with
    // _finalizeUpgrade, all tasks in this function **must** be idempotent/retryable.
    void _finalizeDowngrade(OperationContext* opCtx,
                            const multiversion::FeatureCompatibilityVersion requestedVersion) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const bool isShardsvr = role && role->has(ClusterRole::ShardServer);

        if (isShardsvr) {
            // TODO SERVER-99655: always use requestedVersion as expectedOfcv - and remove the note
            // above about sub-optimal behavior.
            auto expectedOfcv =
                feature_flags::gSnapshotFCVInDDLCoordinators.isEnabledOnVersion(requestedVersion)
                ? boost::make_optional(requestedVersion)
                : boost::none;
            ShardingCoordinatorService::getService(opCtx)->waitForCoordinatorsOfGivenOfcvToComplete(
                opCtx,
                [expectedOfcv](boost::optional<FCV> ofcv) -> bool { return ofcv != expectedOfcv; });
        }

        // This wait serves as a barrier to guarantee that, from now on:
        // - No operations with OFCV higher than the target version will be running
        // - All operations acquiring the global lock in X/IX mode see the fully downgraded FCV
        // state
        _waitForOperationsRelyingOnStaleFcvToComplete(opCtx, requestedVersion);

        FCVStepRegistry::get(opCtx->getServiceContext()).finalizeDowngrade(opCtx, requestedVersion);
    }
    void _forwardDryRunRequestToShards(OperationContext* opCtx,
                                       const SetFeatureCompatibilityVersion& request) {
        auto requestDryRun = request;
        requestDryRun.setFromConfigServer(true);
        requestDryRun.setDryRun(true);
        uassertStatusOK(ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
            opCtx, CommandHelpers::filterCommandRequestForPassthrough(requestDryRun.toBSON())));
    }

    void processDryRun(OperationContext* opCtx,
                       const SetFeatureCompatibilityVersion& request,
                       FCV requestedVersion,
                       FCV actualVersion) {

        // Derive the original version if the actual version is in a transitional state.
        const FCV originalVersion = multiversion::isStandardFCV(actualVersion)
            ? actualVersion
            : multiversion::getTransitionFCVInfo(actualVersion).from;
        LOGV2(10710700,
              "Executing dry-run validation of setFeatureCompatibilityVersion command",
              "requestedVersion"_attr = requestedVersion,
              "actualVersion"_attr = actualVersion);

        if (requestedVersion > actualVersion) {
            _userCollectionsUassertsForUpgrade(opCtx, requestedVersion, originalVersion);
        } else if (requestedVersion < actualVersion) {
            _userCollectionsUassertsForDowngrade(opCtx, requestedVersion, originalVersion);
        }

        auto role = ShardingState::get(opCtx)->pollClusterRole();
        if (role && role->has(ClusterRole::ConfigServer)) {
            _forwardDryRunRequestToShards(opCtx, request);
        }

        LOGV2(10710701,
              "Dry-run validation of setFeatureCompatibilityVersion command completed successfully",
              "requestedVersion"_attr = requestedVersion,
              "actualVersion"_attr = actualVersion);
    }
};
MONGO_REGISTER_COMMAND(SetFeatureCompatibilityVersionCommand).forShard();

}  // namespace
}  // namespace mongo
