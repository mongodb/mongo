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
#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/drop_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/shardsvr_join_ddl_coordinators_request_gen.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/coll_mod.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/local_catalog/drop_indexes.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/user_write_block/write_block_bypass.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/write_concern.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/s/migration_blocking_operation/migration_blocking_operation_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"
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
MONGO_FAIL_POINT_DEFINE(automaticallyCollmodToRecordIdsReplicatedFalse);
MONGO_FAIL_POINT_DEFINE(setFCVPauseAfterReadingConfigDropPedingDBs);
MONGO_FAIL_POINT_DEFINE(failDowngradeValidationDueToIncompatibleFeature);
MONGO_FAIL_POINT_DEFINE(failUpgradeValidationDueToIncompatibleFeature);


/**
 * Ensures that only one instance of setFeatureCompatibilityVersion can run at a given time.
 */
Lock::ResourceMutex commandMutex("setFCVCommandMutex");

void abortAllReshardCollection(OperationContext* opCtx) {
    auto reshardingCoordinatorService = checked_cast<ReshardingCoordinatorService*>(
        repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
            ->lookupServiceByName(ReshardingCoordinatorService::kServiceName));
    reshardingCoordinatorService->abortAllReshardCollection(opCtx);

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

void _setShardedClusterCardinalityParameter(OperationContext* opCtx, const FCV requestedVersion) {
    // If the replica set endpoint is not active, then it isn't safe to allow direct connections
    // again after a second shard has been added. The replica set endpoint requires the cluster
    // parameter to be correct (go back to false when the second shard is removed) so we will need
    // to update the cluster parameter whenever replica set endpoint is enabled.
    if (feature_flags::gFeatureFlagReplicaSetEndpoint.isEnabledOnVersion(requestedVersion)) {
        uassertStatusOK(
            ShardingCatalogManager::get(opCtx)->updateClusterCardinalityParameterIfNeeded(opCtx));
    }
}

void uassertStatusOKIgnoreNSNotFound(Status status) {
    if (status.isOK() || status == ErrorCodes::NamespaceNotFound) {
        return;
    }

    uassertStatusOK(status);
}

/*
 * Automatically modifies data on downgrade for testing. This is because in some cases,
 * the server expects the user to modify data themselves. In testing, as there may not
 * actually be a real user, we need to do it ourselves.
 */
void maybeModifyDataOnDowngradeForTest(OperationContext* opCtx,
                                       const FCV requestedVersion,
                                       const FCV originalVersion) {
    if (MONGO_unlikely(automaticallyCollmodToRecordIdsReplicatedFalse.shouldFail())) {
        // If the test-only failpoint 'automaticallyCollmodToRecordIdsReplicatedFalse' is set,
        // we automatically strip the 'recordIdsReplicated' parameter from the collection
        // options when performing an FCV downgrade to a version that doesn't support replicated
        // recordIds. Normally this is not the case: when a collection with
        // recordIdsReplicated:true is found, we complain.
        if (gFeatureFlagRecordIdsReplicated.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            LOGV2(8700500,
                  "Automatically issuing collMod to strip recordIdsReplicated:true field.");
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    dbName,
                    MODE_X,
                    [&](const Collection* collection) {
                        BSONObjBuilder responseBuilder;
                        auto collMod = CollMod{collection->ns()};
                        collMod.setRecordIdsReplicated(false);
                        uassertStatusOK(processCollModCommand(
                            opCtx, collection->ns(), collMod, nullptr, &responseBuilder));
                        return true;
                    },
                    [&](const Collection* collection) {
                        return collection->areRecordIdsReplicated();
                        ;
                    });
            }
        }
    }
}

void handleDropPendingDBsGarbage(OperationContext* parentOpCtx) {
    // We are using DBDirectClient with specific read/write concerns, so open up an alternative
    // client region.
    auto newClient = parentOpCtx->getService()->makeClient(
        "setFeatureCompatibilityVersion handleDropPendingDBsGarbage");
    const AlternativeClientRegion clientRegion{newClient};
    const auto opCtxShared = cc().makeOperationContext();
    auto* const opCtx = opCtxShared.get();

    const auto kVersionTimestampFieldName = std::string{} + DatabaseType::kVersionFieldName + "." +
        DatabaseVersion::kTimestampFieldName;

    const auto& configShard = ShardingCatalogManager::get(opCtx)->localConfigShard();

    const auto getTimestampFromDropPendingDBs = [&](int order) {
        boost::optional<Timestamp> timestamp;

        AggregateCommandRequest request{
            NamespaceString::kConfigDropPendingDBsNamespace,
            {
                BSON("$sort" << BSON(kVersionTimestampFieldName << order)),
                BSON("$limit" << 1),
            },
        };
        request.setReadConcern(repl::ReadConcernArgs::kMajority);

        uassertStatusOK(configShard->runAggregation(
            opCtx,
            request,
            [&](const std::vector<BSONObj>& batch, const boost::optional<BSONObj>&) {
                invariant(batch.size() == 1);
                const auto bsonVersion = batch[0][DatabaseType::kVersionFieldName];
                tassert(10291400,
                        "The version field is expected to exist and be an object",
                        bsonVersion.type() == BSONType::object);
                const BSONElement bsonTimestamp = bsonVersion[DatabaseVersion::kTimestampFieldName];
                tassert(10291401,
                        "The timestamp field is expected to exist and be a timestamp",
                        bsonTimestamp.type() == BSONType::timestamp);
                timestamp = bsonTimestamp.timestamp();
                return true;
            }));

        return timestamp;
    };

    const auto getLatestTimestampFromDropPendingDBs = [&] {
        return getTimestampFromDropPendingDBs(-1);
    };
    const auto getEarliestTimestampFromDropPendingDBs = [&] {
        return getTimestampFromDropPendingDBs(1);
    };

    // 1. Get the latest timestamp in config.dropPendingDBs.

    const auto latestTimestamp = getLatestTimestampFromDropPendingDBs();

    // If there's none, we're done.
    if (!latestTimestamp) {
        return;
    }

    if (MONGO_unlikely(setFCVPauseAfterReadingConfigDropPedingDBs.shouldFail())) {
        setFCVPauseAfterReadingConfigDropPedingDBs.pauseWhileSet();
    }

    // 2. Drain drop database coordinators in all shards.

    // The list of shards is stable during the execution of this function, since it is called during
    // FCV upgrade.
    const auto opTimeWithShards = Grid::get(opCtx)->catalogClient()->getAllShards(
        opCtx, repl::ReadConcernLevel::kSnapshotReadConcern);
    for (const auto& shardType : opTimeWithShards.value) {
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardType.getName());
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto shard = shardStatus.getValue();

        ShardsvrJoinDDLCoordinators request;
        request.setDbName(DatabaseName::kAdmin);
        request.setTypes({{DDLCoordinatorType_serializer(DDLCoordinatorTypeEnum::kDropDatabase)}});

        const auto response = shard->runCommand(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                DatabaseName::kAdmin,
                                                request.toBSON(),
                                                Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
    }

    // 3. Get new earliest timestamp. After draining all coordinators, the new earliest timestamp,
    // if there's any, should be strictly later than the latest timestamp we got before the drain.

    const auto newEarliestTimestamp = getEarliestTimestampFromDropPendingDBs();

    if (!newEarliestTimestamp || *newEarliestTimestamp > *latestTimestamp) {
        return;
    }

    // 4. If there's still a timestamp and it is equal or lower than the latest timestamp we got
    // before the drain, then, assuming that all binaries are upgraded, we know that all entries
    // with that timestamp or lower are garbage, and we can safely delete them. Since this function
    // is called during setFeatureCompatibilityVersion, by the documented upgrade protocol, all
    // binaries should be upgraded by this point.
    DBDirectClient dbClient{opCtx};
    write_ops::checkWriteErrors(dbClient.remove([&] {
        write_ops::DeleteCommandRequest request{
            NamespaceString::kConfigDropPendingDBsNamespace,
            {
                {BSON(kVersionTimestampFieldName << BSON("$lte" << *latestTimestamp)),
                 true /* multi */},
            }};
        request.setWriteConcern(defaultMajorityWriteConcern());
        return request;
    }()));
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

        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kStart) {
            LOGV2(6744300,
                  "setFeatureCompatibilityVersion command called",
                  "upgradeOrDowngrade"_attr = upgradeOrDowngrade,
                  "serverType"_attr = serverType,
                  "fromVersion"_attr = actualVersion,
                  "toVersion"_attr = requestedVersion);
        }

        const boost::optional<Timestamp> changeTimestamp = getChangeTimestamp(opCtx, request);

        FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
            opCtx, request, actualVersion);

        uassert(5563600,
                "'phase' field is only valid to be specified on shards",
                !request.getPhase() || (role && role->has(ClusterRole::ShardServer)));
        // TODO (SERVER-97816): Remove feature flag check once 9.0 becomes last lts.
        uassert(
            1034131,
            "'phase' field must be present on shards",
            !feature_flags::gUseTopologyChangeCoordinators.isEnabledOnVersion(requestedVersion) ||
                request.getPhase() || (!role || !role->hasExclusively(ClusterRole::ShardServer)));

        if (isDryRun) {
            processDryRun(opCtx, request, requestedVersion, actualVersion);
            return true;
        }

        // Automatic dryRun processing only if skipDryRun is false and the role is either the config
        // server or not part of a sharded cluster
        if (repl::feature_flags::gFeatureFlagSetFcvDryRunMode.isEnabled() && !skipDryRun &&
            (!role || !role->isShardOnly())) {
            processDryRun(opCtx, request, requestedVersion, actualVersion);
        }


        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kStart) {
            {
                // TODO (SERVER-100309): Remove once 9.0 becomes last lts.
                if (role && role->has(ClusterRole::ConfigServer) &&
                    feature_flags::gSessionsCollectionCoordinatorOnConfigServer
                        .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion,
                                                                      actualVersion)) {
                    // Checks that the config server exists as sharded and throws CannotUpgrade
                    // otherwise. We do this before setting the FCV to kUpgrading so that we don't
                    // trap the user in a transitional phase and before entering the FCV change
                    // region because this may create config.system.sessions and we don't want to
                    // hold the FCV lock for a long time.
                    _validateSessionsCollectionSharded(opCtx);
                }

                if (role && role->has(ClusterRole::ConfigServer) &&
                    requestedVersion > actualVersion) {
                    _fixConfigShardsTopologyTime(opCtx);
                }

                if (role && role->has(ClusterRole::ConfigServer)) {
                    // Waiting for recovery here to avoid waiting for recovery while holding the
                    // fcvChangeRegion
                    ShardingDDLCoordinatorService::getService(opCtx)->waitForRecovery(opCtx);
                }

                // Start transition to 'requestedVersion' by updating the local FCV document to a
                // 'kUpgrading' or 'kDowngrading' state, respectively.
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

                uassert(ErrorCodes::Error(6744303),
                        "Failing setFeatureCompatibilityVersion before reaching the FCV "
                        "transitional stage due to 'failBeforeTransitioning' failpoint set",
                        !failBeforeTransitioning.shouldFail());

                // TODO (SERVER-103458): Remove once 9.0 becomes last lts.
                if (role && role->has(ClusterRole::ConfigServer) &&
                    feature_flags::gCheckInvalidDatabaseInGlobalCatalog
                        .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion,
                                                                      actualVersion)) {
                    // Remove reference to the admin database from the config, it is leftover from
                    // an old version.
                    DBDirectClient client(opCtx);
                    write_ops::checkWriteErrors(client.remove(write_ops::DeleteCommandRequest(
                        NamespaceString::kConfigDatabasesNamespace,
                        {{BSON(DatabaseType::kDbNameFieldName << "admin"), false /* multi */}})));
                }

                if (role && role->has(ClusterRole::ConfigServer)) {
                    uassert(
                        ErrorCodes::ConflictingOperationInProgress,
                        "Failed to start FCV change because an addShardCoordinator is in progress",
                        ShardingDDLCoordinatorService::getService(opCtx)
                            ->areAllCoordinatorsOfTypeFinished(opCtx,
                                                               DDLCoordinatorTypeEnum::kAddShard));
                }

                // If this is a config server, then there must be no active
                // SetClusterParameterCoordinator instances active when downgrading.
                if (role && role->has(ClusterRole::ConfigServer)) {
                    uassert(ErrorCodes::CannotDowngrade,
                            "Cannot downgrade while cluster server parameters are being set",
                            (requestedVersion > actualVersion ||
                             ConfigsvrCoordinatorService::getService(opCtx)
                                 ->areAllCoordinatorsOfTypeFinished(
                                     opCtx, ConfigsvrCoordinatorTypeEnum::kSetClusterParameter)));
                }

                // TODO (SERVER-94362) Remove once create database coordinator becomes last lts.
                if (role && role->has(ClusterRole::ConfigServer) &&
                    feature_flags::gCreateDatabaseDDLCoordinator
                        .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion,
                                                                      actualVersion)) {
                    // Drain drop database coordinators and remove possible garbage from
                    // config.dropPendingDBs.
                    handleDropPendingDBsGarbage(opCtx);
                }

                // TODO (SERVER-100309): Remove once 9.0 becomes last lts.
                if (role && role->has(ClusterRole::ConfigServer) &&
                    feature_flags::gSessionsCollectionCoordinatorOnConfigServer
                        .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion,
                                                                      actualVersion)) {
                    // Checks that the config server exists as sharded and throws CannotUpgrade
                    // otherwise. We do this before setting the FCV to kUpgrading so that we don't
                    // trap the user in a transitional phase and after entering the FCV change
                    // region to ensure we don't transition to/from being a config shard during the
                    // checks.
                    _validateSessionsCollectionSharded(opCtx);
                }

                // We pass boost::none as the setIsCleaningServerMetadata argument in order to
                // indicate that we don't want to override the existing isCleaningServerMetadata FCV
                // doc field. This is to protect against the case where a previous FCV downgrade
                // failed in the isCleaningServerMetadata phase, and the user runs setFCV again. In
                // that case we do not want to remove the existing isCleaningServerMetadata FCV doc
                // field because it would not be safe to upgrade the FCV.
                FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                    opCtx,
                    actualVersion,
                    requestedVersion,
                    isFromConfigServer,
                    changeTimestamp,
                    true /* setTargetVersion */,
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

            if (request.getPhase() == SetFCVPhaseEnum::kStart) {
                invariant(role);
                invariant(role->has(ClusterRole::ShardServer));

                // This helper function is only for any actions that should be done specifically on
                // shard servers during phase 1 of the 3-phase setFCV protocol for sharded clusters.
                // For example, before completing phase 1, we must wait for backward incompatible
                // ShardingDDLCoordinators to finish.
                // We do not expect any other feature-specific work to be done in the 'start' phase.
                _shardServerPhase1Tasks(opCtx, requestedVersion);

                // If we are only running the 'start' phase, then we are done.
                return true;
            }
        }

        invariant(serverGlobalParams.featureCompatibility.acquireFCVSnapshot()
                      .isUpgradingOrDowngrading());

        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kPrepare) {
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

            if (request.getPhase() == SetFCVPhaseEnum::kPrepare) {
                invariant(role);
                invariant(role->has(ClusterRole::ShardServer));
                // If we are only running the 'prepare' phase, then we are done
                return true;
            }
        }

        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        invariant(!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kComplete);


        const bool isDowngradeTransition = requestedVersion < actualVersion;
        if (isDowngradeTransition ||
            repl::feature_flags::gFeatureFlagUpgradingToDowngrading.isEnabled()) {

            hangTransitionBeforeIsCleaningServerMetadata.pauseWhileSet(opCtx);
            // Set the isCleaningServerMetadata field to true. This prohibits the upgradingTo
            // Downgrading/ downgradingToUpgrading transition until the isCleaningServerMetadata is
            // unset when we successfully finish the FCV upgrade/downgrade and transition to the
            // upgraded/downgraded state.
            {
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));
                FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                    opCtx,
                    fcvSnapshot.getVersion(),
                    requestedVersion,
                    isFromConfigServer,
                    changeTimestamp,
                    true /* setTargetVersion */,
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
            const auto fcvChangeRegion(FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

            uassert(ErrorCodes::Error(6794601),
                    "Failing downgrade due to 'failBeforeUpdatingFcvDoc' failpoint set",
                    !failBeforeUpdatingFcvDoc.shouldFail());

            hangBeforeUpdatingFcvDoc.pauseWhileSet();

            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx,
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion(),
                requestedVersion,
                isFromConfigServer,
                changeTimestamp,
                false /* setTargetVersion */,
                false /* setIsCleaningServerMetadata */);
        }

        // _finalizeUpgrade/_finalizeDowngrade are only for any tasks that must be done to fully
        // complete the FCV change AFTER the FCV document has already been updated to the requested
        // FCV. This is because there are feature flags that only change value once the FCV document
        // is on the requested value. Everything in these functions **must** be
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

        return true;
    }

private:
    // This helper function is only for any actions that should be done specifically on
    // shard servers during phase 1 of the 3-phase setFCV protocol for sharded clusters.
    // For example, before completing phase 1, we must wait for backward incompatible
    // ShardingDDLCoordinators to finish. This is important in order to ensure that no
    // shard that is currently a participant of such a backward-incompatible
    // ShardingDDLCoordinator can transition to the fully downgraded state (and thus,
    // possibly downgrade its binary) while the coordinator is still in progress.
    // The fact that the FCV has already transitioned to kDowngrading ensures that no
    // new backward-incompatible ShardingDDLCoordinators can start.
    // We do not expect any other feature-specific work to be done in the 'start' phase.
    void _shardServerPhase1Tasks(OperationContext* opCtx, FCV requestedVersion) {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto originalVersion = getTransitionFCVInfo(fcvSnapshot.getVersion()).from;
        const auto isDowngrading = originalVersion > requestedVersion;
        const auto isUpgrading = originalVersion < requestedVersion;

        if (isDowngrading) {
            // TODO SERVER-103838 Remove this code block once 9.0 becomes LTS.
            if (feature_flags::gPersistRecipientPlacementInfoInMigrationRecoveryDoc
                    .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                  originalVersion)) {
                migrationutil::drainMigrationsOnFcvDowngrade(opCtx);
            }

            // TODO SERVER-99655: update once gSnapshotFCVInDDLCoordinators is enabled
            // on the lastLTS
            if (feature_flags::gSnapshotFCVInDDLCoordinators.isEnabledOnVersion(originalVersion)) {
                ShardingDDLCoordinatorService::getService(opCtx)
                    ->waitForCoordinatorsOfGivenOfcvToComplete(
                        opCtx, [originalVersion](boost::optional<FCV> ofcv) -> bool {
                            return ofcv == originalVersion;
                        });
            } else {
                // TODO SERVER-77915: Remove once v8.0 branches out
                if (feature_flags::gTrackUnshardedCollectionsUponMoveCollection
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kRenameCollection);
                }

                // TODO (SERVER-100309): Remove once 9.0 becomes last lts.
                if (feature_flags::gSessionsCollectionCoordinatorOnConfigServer
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kCreateCollection);
                }

                // TODO SERVER-77915: Remove once v8.0 branches out.
                if (feature_flags::gTrackUnshardedCollectionsUponMoveCollection
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kCollMod);
                }

                // TODO (SERVER-76436) Remove once global balancing becomes last lts.
                if (feature_flags::gBalanceUnshardedCollections
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kMovePrimary);
                }

                // TODO (SERVER-97816): Remove once 9.0 becomes last lts.
                if (feature_flags::gUseTopologyChangeCoordinators
                        .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                      originalVersion)) {
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kRemoveShardCommit);
                }
            }
        }

        if (isUpgrading) {
            if (feature_flags::gSnapshotFCVInDDLCoordinators.isEnabledOnVersion(requestedVersion)) {
                // Wait until all DDL coordinators that run are on the kUpgrading* FCV
                ShardingDDLCoordinatorService::getService(opCtx)
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
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForOngoingCoordinatorsToFinish(
                            opCtx, [](const ShardingDDLCoordinator& coordinatorInstance) -> bool {
                                static constexpr std::array drainCoordinatorTypes{
                                    DDLCoordinatorTypeEnum::kMovePrimary,
                                    DDLCoordinatorTypeEnum::kDropDatabase,
                                    DDLCoordinatorTypeEnum::kCreateDatabase,
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
    }

    // This helper function is for any user collections uasserts, creations, or deletions that need
    // to happen during the upgrade. It is required that the code in this helper function is
    // idempotent and could be done after _runDowngrade even if it failed at any point in the middle
    // of _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
    void _userCollectionsWorkForUpgrade(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto originalVersion = getTransitionFCVInfo(fcvSnapshot.getVersion()).from;

        if (feature_flags::gRemoveLegacyTimeseriesBucketingParametersHaveChanged
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, originalVersion)) {
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    dbName,
                    MODE_X,
                    [&](const Collection* collection) {
                        CollMod collModCmd{collection->ns()};
                        collModCmd.set_removeLegacyTimeseriesBucketingParametersHaveChanged(true);

                        BSONObjBuilder responseBuilder;
                        uassertStatusOK(processCollModCommand(
                            opCtx, collection->ns(), collModCmd, nullptr, &responseBuilder));
                        return true;
                    },
                    [&](const Collection* collection) {
                        return collection->getTimeseriesOptions() != boost::none;
                    });
            }
        }
    }

    // This helper function is for updating server metadata to make sure the new features in the
    // upgraded version work for sharded and non-sharded clusters. It is required that the code
    // in this helper function is idempotent and could be done after _runDowngrade even if it
    // failed at any point in the middle of _userCollectionsUassertsForDowngrade or
    // _internalServerCleanupForDowngrade.
    void _upgradeServerMetadata(OperationContext* opCtx, const FCV requestedVersion) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const auto originalVersion =
            getTransitionFCVInfo(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
                .from;

        if (role && role->has(ClusterRole::ConfigServer)) {
            _setShardedClusterCardinalityParameter(opCtx, requestedVersion);
        }

        // TODO SERVER-103046: Remove once 9.0 becomes last lts.
        if (role && role->has(ClusterRole::ShardServer) &&
            feature_flags::gTerminateSecondaryReadsUponRangeDeletion
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, originalVersion)) {
            rangedeletionutil::setPreMigrationShardVersionOnRangeDeletionTasks(opCtx);
        }

        _cleanUpDeprecatedCatalogMetadata(opCtx);
    }

    // TODO(SERVER-100328): remove after 9.0 is branched.
    // WARNING: do not rely on this method to clean up metadata that can be created concurrently. It
    // is fine to rely on this only when missing concurrently created collections is fine, when
    // newly created collections no longer use the metadata format we wish to remove.
    void _cleanUpDeprecatedCatalogMetadata(OperationContext* opCtx) {
        // We bypass the UserWritesBlock mode here in order to not see errors arising from the
        // block. The user already has permission to run FCV at this point and the writes performed
        // here aren't modifying any user data with the exception of fixing up the collection
        // metadata.
        auto originalValue = WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled();
        ON_BLOCK_EXIT([&] { WriteBlockBypass::get(opCtx).set(originalValue); });
        WriteBlockBypass::get(opCtx).set(true);

        for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
            Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
            catalog::forEachCollectionFromDb(
                opCtx, dbName, MODE_X, [&](const Collection* collection) {
                    // To remove deprecated catalog metadata, issue a collmod with no other options
                    // set.
                    BSONObjBuilder responseBuilder;
                    uassertStatusOK(processCollModCommand(opCtx,
                                                          collection->ns(),
                                                          CollMod{collection->ns()},
                                                          nullptr,
                                                          &responseBuilder));
                    return true;
                });
        }
    }

    void _dropSessionsCollectionLocally(OperationContext* opCtx,
                                        const FCV requestedVersion,
                                        const FCV originalVersion) {
        if (feature_flags::gSessionsCollectionCoordinatorOnConfigServer
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            // Only drop the collection locally if the config server is not acting as a shard. Since
            // addShard (transitionFromDedicated) cannot run on transitional FCV, we cannot drop
            // this when we shouldn't. If we transition to dedicated after this check, then
            // transition to dedicated will drop the collection.
            const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
            bool amIAConfigShard =
                std::find(allShardIds.begin(),
                          allShardIds.end(),
                          ShardingState::get(opCtx)->shardId()) != allShardIds.end();
            if (!amIAConfigShard) {
                DropCollectionCoordinator::dropCollectionLocally(
                    opCtx,
                    NamespaceString::kLogicalSessionsNamespace,
                    true /* fromMigrate */,
                    true /* dropSystemCollections */,
                    boost::none,
                    false /* requireCollectionEmpty */);
            }
        }
    }

    // _prepareToUpgrade performs all actions and checks that need to be done before proceeding to
    // make any metadata changes as part of FCV upgrade. Any new feature specific upgrade code
    // should be placed in the _prepareToUpgrade helper functions:
    //  * _userCollectionsWorkForUpgrade: for any user collections uasserts, creations, or deletions
    //    that need to happen during the upgrade. This happens after the global lock.
    // Please read the comments on those helper functions for more details on what should be placed
    // in each function.
    void _prepareToUpgrade(OperationContext* opCtx,
                           const SetFeatureCompatibilityVersion& request,
                           boost::optional<Timestamp> changeTimestamp) {
        {
            // Take the global lock in S mode to create a barrier for operations taking the global
            // IX or X locks. This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::GlobalLock lk(opCtx, MODE_S);
        }

        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto originalVersion = getTransitionFCVInfo(fcvSnapshot.getVersion()).from;
        const auto requestedVersion = request.getCommandParameter();

        _userCollectionsUassertsForUpgrade(opCtx, requestedVersion, originalVersion);

        auto role = ShardingState::get(opCtx)->pollClusterRole();

        // This helper function is for any user collections uasserts, creations, or deletions that
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
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->runCloneAuthoritativeMetadataOnShards(
                        opCtx));
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
        if (!feature_flags::gFeatureFlagEnableReplicasetTransitionToCSRS.isEnabledOnVersion(
                requestedVersion)) {
            BSONObj shardIdentityBSON;
            if ([&] {
                    auto coll = acquireCollection(
                        opCtx,
                        CollectionAcquisitionRequest(
                            NamespaceString::kServerConfigurationNamespace,
                            PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                            repl::ReadConcernArgs::get(opCtx),
                            AcquisitionPrerequisites::kRead),
                        LockMode::MODE_IS);
                    return Helpers::findOne(
                        opCtx, coll, BSON("_id" << ShardIdentityType::IdName), shardIdentityBSON);
                }()) {
                auto shardIdentity = uassertStatusOK(
                    ShardIdentityType::fromShardIdentityDocument(shardIdentityBSON));
                if (shardIdentity.getDeferShardingInitialization().has_value()) {
                    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                        uasserted(ErrorCodes::CannotDowngrade,
                                  "Downgrading FCV is prohibited during promotion to sharded "
                                  "cluster. Please finish the promotion before proceeding.");
                    }
                    uasserted(ErrorCodes::CannotDowngrade,
                              "Downgrading FCV is prohibited during promotion to sharded cluster. "
                              "Please remove the shard identity document before proceeding.");
                }
            }
        }

        auto role = ShardingState::get(opCtx)->pollClusterRole();
        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (role && role->has(ClusterRole::ConfigServer)) {
            // Config server role actions.
        }

        if (role && role->has(ClusterRole::ShardServer)) {
            // Shard server role actions.
        }
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

        if (gFeatureFlagRecordIdsReplicated.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion) &&
            MONGO_likely(!automaticallyCollmodToRecordIdsReplicatedFalse.shouldFail())) {
            // so don't allow downgrading with such a collection.
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    dbName,
                    MODE_IS,
                    [&](const Collection* collection) -> bool {
                        uasserted(
                            ErrorCodes::CannotDowngrade,
                            fmt::format(
                                "Cannot downgrade the cluster when there are collections with "
                                "'recordIdsReplicated' enabled. Please unset the option or "
                                "drop the collection(s) before downgrading. First detected "
                                "collection with 'recordIdsReplicated' enabled: {} (UUID: {}).",
                                collection->ns().toStringForErrorMsg(),
                                collection->uuid().toString()));
                    },
                    [&](const Collection* collection) {
                        return collection->areRecordIdsReplicated();
                    });
            }
        }

        if (gFeatureFlagErrorAndLogValidationAction.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    dbName,
                    MODE_IS,
                    [&](const Collection* collection) -> bool {
                        uasserted(
                            ErrorCodes::CannotDowngrade,
                            fmt::format(
                                "Cannot downgrade the cluster when there are collections with "
                                "'errorAndLog' validation action. Please unset the option or "
                                "drop the collection(s) before downgrading. First detected "
                                "collection with 'errorAndLog' enabled: {} (UUID: {}).",
                                collection->ns().toStringForErrorMsg(),
                                collection->uuid().toString()));
                    },
                    [&](const Collection* collection) {
                        return collection->getValidationAction() ==
                            ValidationActionEnum::errorAndLog;
                    });
            }
        }

        if (gFeatureFlagQETextSearchPreview.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            auto checkForStringSearchQueryType = [](const Collection* collection) {
                const auto& encryptedFields =
                    collection->getCollectionOptions().encryptedFieldConfig;
                if (encryptedFields &&
                    hasQueryTypeMatching(encryptedFields.get(), isFLE2TextQueryType)) {
                    uasserted(ErrorCodes::CannotDowngrade,
                              fmt::format(
                                  "Collection {} (UUID: {}) has an encrypted field with query type "
                                  "substringPreview, suffixPreview, or prefixPreview, which "
                                  "are not compatible with the target FCV. Please drop this "
                                  "collection before trying to downgrade FCV.",
                                  collection->ns().toStringForErrorMsg(),
                                  collection->uuid().toString()));
                }
                return true;
            };
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
                catalog::forEachCollectionFromDb(
                    opCtx, dbName, MODE_IS, checkForStringSearchQueryType);
            }
        }
    }

    // Remove cluster parameters from the clusterParameters collections which are not enabled on
    // requestedVersion.
    void _cleanUpClusterParameters(OperationContext* opCtx,
                                   const FCV originalVersion,
                                   const FCV requestedVersion) {
        auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
        std::vector<write_ops::DeleteOpEntry> deletes;
        for (const auto& [name, sp] : clusterParameters->getMap()) {
            auto parameterState = sp->getState();
            if (sp->isEnabledOnVersion(parameterState, originalVersion) &&
                !sp->isEnabledOnVersion(parameterState, requestedVersion)) {
                deletes.emplace_back(
                    write_ops::DeleteOpEntry(BSON("_id" << name), false /*multi*/));
            }
        }
        if (deletes.size() > 0) {
            DBDirectClient client(opCtx);
            // We never downgrade with multitenancy enabled, so assume we have just the none tenant.
            write_ops::DeleteCommandRequest deleteOp(NamespaceString::kClusterParametersNamespace);
            deleteOp.setDeletes(deletes);
            write_ops::checkWriteErrors(client.remove(deleteOp));
        }
    }

    // Insert the authorization schema document in admin.system.version if there are any user or
    // role documents on-disk. This must be performed on FCV downgrade since lower-version binaries
    // assert that this document exists when users and/or roles exist during initial sync.
    void _createAuthzSchemaVersionDocIfNeeded(OperationContext* opCtx) {
        // Check if any user or role documents exist on-disk.
        bool hasUserDocs, hasRoleDocs = false;
        BSONObj userDoc, roleDoc;
        {
            auto userColl = acquireCollectionMaybeLockFree(
                opCtx,
                CollectionAcquisitionRequest{
                    NamespaceString::kAdminUsersNamespace,
                    PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                    repl::ReadConcernArgs::get(opCtx),
                    AcquisitionPrerequisites::kRead});
            hasUserDocs = Helpers::findOne(opCtx, userColl, BSONObj(), userDoc);
        }

        {
            auto rolesColl = acquireCollectionMaybeLockFree(
                opCtx,
                CollectionAcquisitionRequest{
                    NamespaceString::kAdminRolesNamespace,
                    PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                    repl::ReadConcernArgs::get(opCtx),
                    AcquisitionPrerequisites::kRead});
            hasRoleDocs = Helpers::findOne(opCtx, rolesColl, BSONObj(), roleDoc);
        }

        // If they do, write an authorization schema document to disk set to schemaVersionSCRAM28.
        if (hasUserDocs || hasRoleDocs) {
            DBDirectClient client(opCtx);
            auto result = client.update([&] {
                write_ops::UpdateCommandRequest updateOp(
                    NamespaceString::kServerConfigurationNamespace);
                updateOp.setUpdates({[&] {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(AuthorizationManager::versionDocumentQuery);
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                        BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName
                                            << AuthorizationManager::schemaVersion28SCRAM))));
                    entry.setMulti(false);
                    entry.setUpsert(true);
                    return entry;
                }()});
                return updateOp;
            }());

            write_ops::checkWriteErrors(result);
        }
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
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        if (!role || role->has(ClusterRole::None) || role->has(ClusterRole::ShardServer)) {
            if (feature_flags::gTSBucketingParametersUnchanged
                    .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                  originalVersion)) {
                for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                    catalog::forEachCollectionFromDb(
                        opCtx,
                        dbName,
                        MODE_X,
                        [&](const Collection* collection) {
                            // Only remove the catalog entry flag if it exists. It could've been
                            // removed if the downgrade process was interrupted and is being run
                            // again. The downgrade process cannot be aborted at this point.
                            if (collection->timeseriesBucketingParametersHaveChanged()) {
                                // To remove timeseries bucketing parameters from persistent
                                // storage, issue the "collMod" command with none of the parameters
                                // set.
                                BSONObjBuilder responseBuilder;
                                uassertStatusOK(processCollModCommand(opCtx,
                                                                      collection->ns(),
                                                                      CollMod{collection->ns()},
                                                                      nullptr,
                                                                      &responseBuilder));
                                return true;
                            }
                            return true;
                        },
                        [&](const Collection* collection) {
                            return collection->getTimeseriesOptions() != boost::none;
                        });
                }
            }

            maybeModifyDataOnDowngradeForTest(opCtx, requestedVersion, originalVersion);
        }

        _cleanUpClusterParameters(opCtx, originalVersion, requestedVersion);
        _createAuthzSchemaVersionDocIfNeeded(opCtx);
        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (role && role->has(ClusterRole::ConfigServer)) {
            _dropSessionsCollectionLocally(opCtx, requestedVersion, originalVersion);
        }

        if (role && role->has(ClusterRole::ShardServer)) {
            abortAllMultiUpdateCoordinators(opCtx, requestedVersion, originalVersion);
        }
    }

    void abortAllMultiUpdateCoordinators(OperationContext* opCtx,
                                         const FCV requestedVersion,
                                         const FCV originalVersion) {
        if (!migration_blocking_operation::gFeatureFlagPauseMigrationsDuringMultiUpdatesAvailable
                 .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            return;
        }
        MultiUpdateCoordinatorService::abortAndWaitForAllInstances(
            opCtx,
            {ErrorCodes::IllegalOperation,
             fmt::format("FCV downgrading to {} and pauseMigrationsDuringMultiUpdates is not "
                         "supported on this version",
                         toString(requestedVersion))});
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

        {
            // Take the global lock in S mode to create a barrier for operations taking the global
            // IX or X locks. This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::GlobalLock lk(opCtx, MODE_S);
        }

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

        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
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

    /**
     * For sharded cluster servers:
     *  Generate a new changeTimestamp if change fcv is called on config server,
     *  otherwise retrieve changeTimestamp from the Config Server request.
     */
    boost::optional<Timestamp> getChangeTimestamp(mongo::OperationContext* opCtx,
                                                  mongo::SetFeatureCompatibilityVersion request) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        boost::optional<Timestamp> changeTimestamp;
        if (role && role->has(ClusterRole::ConfigServer)) {
            // The Config Server always creates a new ID (i.e., timestamp) when it receives an
            // upgrade or downgrade request.
            const auto now = VectorClock::get(opCtx)->getTime();
            changeTimestamp = now.clusterTime().asTimestamp();
        } else if (role && role->has(ClusterRole::ShardServer) && request.getPhase()) {
            // Shards receive the timestamp from the Config Server's request.
            changeTimestamp = request.getChangeTimestamp();
            uassert(5563500,
                    "The 'changeTimestamp' field is missing even though the node is running as a "
                    "shard. This may indicate that the 'setFeatureCompatibilityVersion' command "
                    "was invoked directly against the shard or that the config server has not been "
                    "upgraded to at least version 5.0.",
                    changeTimestamp);
        }
        return changeTimestamp;
    }

    /**
     * For sharded clusters created before 5.0, it is possible that entries in config.shards do not
     * contain a topologyTime field. To make all entries consistent with the behaviour on 5.0+, we
     * insert a topologyTime in any entries which do not have it. This will simplify reasoning about
     * the topologyTime and ShardRegistry. Moreover, this has another objective, which is to heal
     * clusters affected by SERVER-63742, which caused a corrupted topologyTime to be persisted and
     * gossiped in the cluster. Note that this healing is only possible when config.shards doesn't
     * contain any topologyTime, as this is known to be benign. The case where config.shards does
     * have some topologyTime, but an inconsistent $topologyTime which is greater is gossiped, is
     * disallowed by the ShardRegistry, and requires manual intervention. This latter case would
     * trigger a tassert and force user intervention, and thus we do not need to explicitly check
     * for it here.
     *
     * The new topologyTime will make it into the vector clock following the usual
     * ConfigServerOpObserver mechanism.
     *
     * TODO (SERVER-102087): remove after 9.0 is branched.
     */
    void _fixConfigShardsTopologyTime(OperationContext* opCtx) {
        // Prevent concurrent add/remove shard operations.
        Lock::ExclusiveLock shardMembershipLock =
            ShardingCatalogManager::get(opCtx)->acquireShardMembershipLockForTopologyChange(opCtx);

        const auto time = VectorClock::get(opCtx)->getTime();
        const auto newTopologyTime = time.configTime().asTimestamp();

        LOGV2(10216200,
              "Updating 'config.shards' entries which do not have a topologyTime field with the "
              "current $configTime",
              "newTopologyTime"_attr = newTopologyTime);

        write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigsvrShardsNamespace);
        updateOp.setUpdates({[&] {
            // Filter by $exists to prevent modifying entries which already have a topologyTime.
            const auto filter = BSON(ShardType::topologyTime << BSON("$exists" << false));
            const auto update = BSON("$set" << BSON(ShardType::topologyTime << newTopologyTime));
            write_ops::UpdateOpEntry entry;
            entry.setQ(filter);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(false);
            // We want all entries to contain a topologyTime field.
            entry.setMulti(true);
            return entry;
        }()});
        updateOp.setWriteConcern(defaultMajorityWriteConcernDoNotUse());

        DBDirectClient client(opCtx);
        const auto result = client.update(updateOp);
        write_ops::checkWriteErrors(result);

        LOGV2(10216201,
              "Update of 'config.shards' entries succeeded",
              "updateResponse"_attr = result.toBSON());
    }

    void _validateSessionsCollectionSharded(OperationContext* opCtx) {
        const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

        // If there are no shards in the cluster the collection cannot be sharded.
        if (allShardIds.empty()) {
            return;
        }

        auto cm = uassertStatusOK(
            RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(
                opCtx, NamespaceString::kLogicalSessionsNamespace));

        if (!cm.isSharded()) {
            auto status = LogicalSessionCache::get(opCtx)->refreshNow(opCtx);
            uassert(
                ErrorCodes::CannotUpgrade,
                str::stream() << "Collection "
                              << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                              << " must be created as sharded before upgrading. If the collection "
                                 "exists as unsharded, please contact support for assistance.",
                status.isOK());
        }
    }

    void _validateSessionsCollectionOutsideConfigServer(OperationContext* opCtx) {
        const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

        // If there are no shards in the cluster the collection cannot be sharded.
        if (allShardIds.empty()) {
            return;
        }

        auto cm = uassertStatusOK(
            RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(
                opCtx, NamespaceString::kLogicalSessionsNamespace));

        // If we are on a dedicated config server, make sure that there is not a chunk on the
        // config server. This is prevented by SERVER-97338, but prior to its fixing this could
        // be possible.
        bool amIAConfigShard = std::find(allShardIds.begin(),
                                         allShardIds.end(),
                                         ShardingState::get(opCtx)->shardId()) != allShardIds.end();
        if (!amIAConfigShard) {
            std::set<ShardId> shardsOwningChunks;
            cm.getAllShardIds(&shardsOwningChunks);
            uassert(ErrorCodes::CannotUpgrade,
                    str::stream()
                        << "Collection "
                        << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                        << " has a range located on the config server. Please move this range to "
                           "any other shard using the `moveRange` command before upgrading.",
                    !shardsOwningChunks.contains(ShardingState::get(opCtx)->shardId()));
        }
    }

    void _createConfigSessionsCollectionLocally(OperationContext* opCtx) {
        ShardsvrCreateCollection shardsvrCollRequest(NamespaceString::kLogicalSessionsNamespace);
        ShardsvrCreateCollectionRequest requestParamsObj;
        requestParamsObj.setShardKey(BSON("_id" << 1));
        shardsvrCollRequest.setShardsvrCreateCollectionRequest(std::move(requestParamsObj));
        shardsvrCollRequest.setDbName(NamespaceString::kLogicalSessionsNamespace.dbName());

        try {
            cluster::createCollection(opCtx, std::move(shardsvrCollRequest));
        } catch (const ExceptionFor<ErrorCodes::IllegalOperation>& ex) {
            LOGV2(8694900,
                  "Failed to create config.system.sessions on upgrade",
                  "error"_attr = redact(ex));
        }
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
        const bool isConfigsvr = role && role->has(ClusterRole::ConfigServer);

        // Drain outstanding DDL operations that are incompatible with the target FCV.
        if (role && role->has(ClusterRole::ShardServer)) {
            // TODO SERVER-99655: remove the comment below.
            // The draining logic relies on the OFCV infrastructure, which has been introduced in
            // FCV 8.2 and may behave sub-optimally when requestedVersion is lower than 8.2.
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenOfcvToComplete(
                    opCtx, [requestedVersion](boost::optional<FCV> ofcv) -> bool {
                        return ofcv != requestedVersion;
                    });
        }

        // TODO (SERVER-100309): Remove once 9.0 becomes last lts.
        if (isConfigsvr &&
            feature_flags::gSessionsCollectionCoordinatorOnConfigServer.isEnabledOnVersion(
                requestedVersion)) {
            _createConfigSessionsCollectionLocally(opCtx);
        }

        // TODO (SERVER-97816): Remove once 9.0 becomes last lts.
        if (isConfigsvr &&
            feature_flags::gUseTopologyChangeCoordinators.isEnabledOnVersion(requestedVersion)) {
            // The old remove shard is not a coordinator, so we can only drain this operation by
            // acquiring the same DDL lock that it acquires (config.shards).
            DDLLockManager::ScopedCollectionDDLLock ddlLock(
                opCtx,
                NamespaceString::kConfigsvrShardsNamespace,
                "setFCVFinalizeUpgrade",
                LockMode::MODE_IX);
        }

        // TODO SERVER-94927: Remove once 9.0 becomes last lts.
        const bool isReplSet = !role.has_value();
        if ((isReplSet || isConfigsvr) &&
            feature_flags::gFeatureFlagPQSBackfill.isEnabledOnVersion(requestedVersion)) {
            auto& service = query_settings::QuerySettingsService::get(opCtx);
            try {
                service.createQueryShapeRepresentativeQueriesCollection(opCtx);
                service
                    .migrateRepresentativeQueriesFromQuerySettingsClusterParameterToDedicatedCollection(
                        opCtx);
            } catch (const ExceptionFor<ErrorCodes::Interrupted>&) {
                throw;
            } catch (const DBException& ex) {
                uasserted(ErrorCodes::TemporarilyUnavailable,
                          str::stream()
                              << "Cannot upgrade to the new FCV due to QuerySettingsService issue: "
                              << ex.reason());
            }
        }
    }

    // _finalizeDowngrade is analogous to _finalizeUpgrade, but runs on downgrade. As with
    // _finalizeUpgrade, all tasks in this function **must** be idempotent/retryable.
    void _finalizeDowngrade(OperationContext* opCtx,
                            const multiversion::FeatureCompatibilityVersion requestedVersion) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const bool isConfigsvr = role && role->has(ClusterRole::ConfigServer);
        const bool isShardsvr = role && role->has(ClusterRole::ShardServer);

        if (isShardsvr) {
            // TODO SERVER-99655: always use requestedVersion as expectedOfcv - and remove the note
            // above about sub-optimal behavior.
            auto expectedOfcv =
                feature_flags::gSnapshotFCVInDDLCoordinators.isEnabledOnVersion(requestedVersion)
                ? boost::make_optional(requestedVersion)
                : boost::none;
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenOfcvToComplete(
                    opCtx, [expectedOfcv](boost::optional<FCV> ofcv) -> bool {
                        return ofcv != expectedOfcv;
                    });
        }

        // TODO (SERVER-98118): remove once 9.0 becomes last LTS.
        if (isShardsvr &&
            !feature_flags::gShardAuthoritativeDbMetadataDDL.isEnabledOnVersion(requestedVersion)) {
            // Dropping the authoritative collections (config.shard.catalog.X) as the final step of
            // the downgrade ensures that no leftover data remains. This guarantees a clean
            // downgrade and makes it safe to upgrade again.
            DropCollectionCoordinator::dropCollectionLocally(
                opCtx,
                NamespaceString::kConfigShardCatalogDatabasesNamespace,
                true /* fromMigrate */,
                false /* dropSystemCollections */,
                boost::none,
                false /* requireCollectionEmpty */);
        }

        // TODO SERVER-94927: Remove once 9.0 becomes last lts.
        const bool isReplSet = !role.has_value();
        if ((isReplSet || isConfigsvr) &&
            !feature_flags::gFeatureFlagPQSBackfill.isEnabledOnVersion(requestedVersion)) {
            auto& service = query_settings::QuerySettingsService::get(opCtx);
            try {
                service
                    .migrateRepresentativeQueriesFromDedicatedCollectionToQuerySettingsClusterParameter(
                        opCtx);
                service.dropQueryShapeRepresentativeQueriesCollection(opCtx);
            } catch (const ExceptionFor<ErrorCodes::Interrupted>&) {
                throw;
            } catch (const DBException& ex) {
                uasserted(
                    ErrorCodes::TemporarilyUnavailable,
                    str::stream()
                        << "Cannot downgrade to the old FCV due to QuerySettingsService issue: "
                        << ex.reason());
            }
        }
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
