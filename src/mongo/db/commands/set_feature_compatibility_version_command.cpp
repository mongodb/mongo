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


#include "mongo/platform/basic.h"

#include <fmt/format.h>

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_impl.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/shard_merge_recipient_service.h"
#include "mongo/db/repl/tenant_migration_donor_service.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/shard_authoritative_catalog_gen.h"
#include "mongo/db/s/sharding_cluster_parameters_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_index_catalog_ddl_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/vector_clock.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


using namespace fmt::literals;

namespace mongo {
namespace {

using GenericFCV = multiversion::GenericFCV;

MONGO_FAIL_POINT_DEFINE(failBeforeTransitioning);
MONGO_FAIL_POINT_DEFINE(failUpgrading);
MONGO_FAIL_POINT_DEFINE(hangWhileUpgrading);
MONGO_FAIL_POINT_DEFINE(failBeforeSendingShardsToDowngradingOrUpgrading);
MONGO_FAIL_POINT_DEFINE(failDowngrading);
MONGO_FAIL_POINT_DEFINE(hangWhileDowngrading);
MONGO_FAIL_POINT_DEFINE(hangBeforeUpdatingFcvDoc);
MONGO_FAIL_POINT_DEFINE(failBeforeUpdatingFcvDoc);
MONGO_FAIL_POINT_DEFINE(failDowngradingDuringIsCleaningServerMetadata);
MONGO_FAIL_POINT_DEFINE(hangBeforeTransitioningToDowngraded);
MONGO_FAIL_POINT_DEFINE(hangDowngradingBeforeIsCleaningServerMetadata);

/**
 * Ensures that only one instance of setFeatureCompatibilityVersion can run at a given time.
 */
Lock::ResourceMutex commandMutex("setFCVCommandMutex");

/**
 * Deletes the persisted default read/write concern document.
 */
void deletePersistedDefaultRWConcernDocument(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    const auto commandResponse = client.runCommand([&] {
        write_ops::DeleteCommandRequest deleteOp(NamespaceString::kConfigSettingsNamespace);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(BSON("_id" << ReadWriteConcernDefaults::kPersistedDocumentId));
            entry.setMulti(false);
            return entry;
        }()});
        return deleteOp.serialize({});
    }());
    uassertStatusOK(getStatusFromWriteCommandReply(commandResponse->getCommandReply()));
}

void abortAllReshardCollection(OperationContext* opCtx) {
    auto reshardingCoordinatorService = checked_cast<ReshardingCoordinatorService*>(
        repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
            ->lookupServiceByName(ReshardingCoordinatorService::kServiceName));
    reshardingCoordinatorService->abortAllReshardCollection(opCtx);

    PersistentTaskStore<ReshardingCoordinatorDocument> store(
        NamespaceString::kConfigReshardingOperationsNamespace);

    std::vector<std::string> nsWithReshardColl;
    store.forEach(opCtx, {}, [&](const ReshardingCoordinatorDocument& doc) {
        nsWithReshardColl.push_back(doc.getSourceNss().ns());
        return true;
    });

    if (!nsWithReshardColl.empty()) {
        std::string nsListStr;
        str::joinStringDelim(nsWithReshardColl, &nsListStr, ',');

        uasserted(
            ErrorCodes::ManualInterventionRequired,
            "reshardCollection was not properly cleaned up after attempted abort for these ns: "
            "[{}]. This is sign that the resharding operation was interrupted but not "
            "aborted."_format(nsListStr));
    }
}

// TODO SERVER-68551: Remove once 7.0 becomes last-lts
void dropDistLockCollections(OperationContext* opCtx) {
    LOGV2(6589100, "Dropping deprecated distributed locks collections");
    static const std::vector<NamespaceString> collectionsToDrop{
        NamespaceString::kLockpingsNamespace, NamespaceString::kDistLocksNamepsace};

    for (const auto& nss : collectionsToDrop) {
        DropReply dropReply;
        const auto dropStatus =
            dropCollection(opCtx,
                           nss,
                           &dropReply,
                           DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
        if (dropStatus != ErrorCodes::NamespaceNotFound) {
            uassertStatusOKWithContext(
                dropStatus,
                str::stream() << "Failed to drop deprecated distributed locks collection " << nss);
        }
    }
}

void uassertStatusOKIgnoreNSNotFound(Status status) {
    if (status.isOK() || status == ErrorCodes::NamespaceNotFound) {
        return;
    }

    uassertStatusOK(status);
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
          << feature_compatibility_version_documentation::kCompatibilityLink << ".";
        return h.str();
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::setFeatureCompatibilityVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // Ensure that this operation will be killed by the RstlKillOpThread during step-up or
        // stepdown.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto request = SetFeatureCompatibilityVersion::parse(
            IDLParserContext("setFeatureCompatibilityVersion"), cmdObj);
        auto isFromConfigServer = request.getFromConfigServer().value_or(false);

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        Lock::ExclusiveLock setFCVCommandLock(opCtx, commandMutex);

        const auto requestedVersion = request.getCommandParameter();
        const auto actualVersion = serverGlobalParams.featureCompatibility.getVersion();

        auto isConfirmed = request.getConfirm().value_or(false);
        // TODO (SERVER-74398): Remove this flag once 7.0 is last LTS.
        if (mongo::repl::requireConfirmInSetFcv) {
            const auto upgradeMsg =
                "Once you have upgraded to {}, you will not be able to downgrade FCV and binary version without support assistance. Please re-run this command with 'confirm: true' to acknowledge this and continue with the FCV upgrade."_format(
                    multiversion::toString(requestedVersion));
            const auto downgradeMsg =
                "Once you have downgraded the FCV, if you choose to downgrade the binary version, "
                "it will require support assistance. Please re-run this command with 'confirm: "
                "true' to acknowledge this and continue with the FCV downgrade.";
            uassert(7369100,
                    (requestedVersion > actualVersion ? upgradeMsg : downgradeMsg),
                    // If the request is from a config svr, skip requiring the 'confirm: true'
                    // parameter.
                    (isFromConfigServer || isConfirmed));
        }

        // Always wait for at least majority writeConcern to ensure all writes involved in the
        // upgrade/downgrade process cannot be rolled back. There is currently no mechanism to
        // specify a default writeConcern, so we manually call waitForWriteConcern upon exiting this
        // command.
        //
        // TODO SERVER-25778: replace this with the general mechanism for specifying a default
        // writeConcern.
        ON_BLOCK_EXIT([&] {
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

            // TODO SERVER-72796: Remove once gGlobalIndexesShardingCatalog is enabled.
            if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
                feature_flags::gGlobalIndexesShardingCatalog.isEnabledOnVersion(requestedVersion)) {
                ShardingDDLCoordinatorService::getService(opCtx)
                    ->waitForCoordinatorsOfGivenTypeToComplete(
                        opCtx, DDLCoordinatorTypeEnum::kRenameCollectionPre63Compatible);
            }
            // TODO SERVER-73627: Remove once 7.0 becomes last LTS.
            if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
                feature_flags::gDropCollectionHoldingCriticalSection.isEnabledOnVersion(
                    requestedVersion)) {
                ShardingDDLCoordinatorService::getService(opCtx)
                    ->waitForCoordinatorsOfGivenTypeToComplete(
                        opCtx, DDLCoordinatorTypeEnum::kDropCollectionPre70Compatible);

                ShardingDDLCoordinatorService::getService(opCtx)
                    ->waitForCoordinatorsOfGivenTypeToComplete(
                        opCtx, DDLCoordinatorTypeEnum::kDropDatabasePre70Compatible);
            }

            // TODO SERVER-68373: Remove once 7.0 becomes last LTS
            if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
                mongo::gFeatureFlagFLE2CompactForProtocolV2.isEnabledOnVersion(requestedVersion)) {
                ShardingDDLCoordinatorService::getService(opCtx)
                    ->waitForCoordinatorsOfGivenTypeToComplete(
                        opCtx,
                        DDLCoordinatorTypeEnum::kCompactStructuredEncryptionDataPre70Compatible);
                ShardingDDLCoordinatorService::getService(opCtx)
                    ->waitForCoordinatorsOfGivenTypeToComplete(
                        opCtx,
                        DDLCoordinatorTypeEnum::kCompactStructuredEncryptionDataPre61Compatible);
            }

            return true;
        }

        const auto upgradeOrDowngrade = requestedVersion > actualVersion ? "upgrade" : "downgrade";
        const auto server_type = serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)
            ? "config server"
            : (request.getPhase() ? "shard server" : "replica set/standalone");

        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kStart) {
            LOGV2(6744300,
                  "setFeatureCompatibilityVersion command called",
                  "upgradeOrDowngrade"_attr = upgradeOrDowngrade,
                  "serverType"_attr = server_type,
                  "fromVersion"_attr = actualVersion,
                  "toVersion"_attr = requestedVersion);
        }

        const boost::optional<Timestamp> changeTimestamp = getChangeTimestamp(opCtx, request);

        FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
            opCtx, request, actualVersion);

        uassert(5563600,
                "'phase' field is only valid to be specified on shards",
                !request.getPhase() ||
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kStart) {
            {
                // Start transition to 'requestedVersion' by updating the local FCV document to a
                // 'kUpgrading' or 'kDowngrading' state, respectively.
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

                // If catalogShard is enabled and there is an entry in config.shards with _id:
                // ShardId::kConfigServerId then the config server is a catalog shard.
                auto isCatalogShard =
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
                    !ShardingCatalogManager::get(opCtx)
                         ->findOneConfigDocument(opCtx,
                                                 NamespaceString::kConfigsvrShardsNamespace,
                                                 BSON("_id" << ShardId::kConfigServerId.toString()))
                         .isEmpty();

                uassert(ErrorCodes::CannotDowngrade,
                        "Cannot downgrade featureCompatibilityVersion to {} "
                        "with a catalog shard as it is not supported in earlier versions. "
                        "Please transition the config server to dedicated mode using the "
                        "transitionToDedicatedConfigServer command."_format(
                            multiversion::toString(requestedVersion)),
                        !isCatalogShard ||
                            gFeatureFlagCatalogShard.isEnabledOnVersion(requestedVersion));

                uassert(ErrorCodes::Error(6744303),
                        "Failing setFeatureCompatibilityVersion before reaching the FCV "
                        "transitional stage due to 'failBeforeTransitioning' failpoint set",
                        !failBeforeTransitioning.shouldFail());

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
                      "serverType"_attr = server_type,
                      "fromVersion"_attr = actualVersion,
                      "toVersion"_attr = requestedVersion);
            }

            if (request.getPhase() == SetFCVPhaseEnum::kStart) {
                invariant(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

                // This helper function is only for any actions that should be done specifically on
                // shard servers during phase 1 of the 2-phase setFCV protocol for sharded clusters.
                // For example, before completing phase 1, we must wait for backward incompatible
                // ShardingDDLCoordinators to finish.
                // We do not expect any other feature-specific work to be done in the 'start' phase.
                _shardServerPhase1Tasks(opCtx, requestedVersion);

                // If we are only running the 'start' phase, then we are done.
                return true;
            }
        }

        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());

        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kPrepare) {
            if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                uassert(ErrorCodes::Error(6794600),
                        "Failing downgrade due to "
                        "'failBeforeSendingShardsToDowngradingOrUpgrading' failpoint set",
                        !failBeforeSendingShardsToDowngradingOrUpgrading.shouldFail());
                // Tell the shards to enter 'start' phase of setFCV (transition to kDowngrading).
                _sendSetFCVRequestToShards(
                    opCtx, request, changeTimestamp, SetFCVPhaseEnum::kStart);

                // (Ignore FCV check): This feature flag is intentional to only check if it is
                // enabled on this binary so the config server can be a shard.
                if (gFeatureFlagCatalogShard.isEnabledAndIgnoreFCVUnsafe()) {
                    // The config server may also be a shard, so have it run any shard server tasks.
                    // Run this after sending the first phase to shards so they enter the transition
                    // state even if this throws.
                    _shardServerPhase1Tasks(opCtx, requestedVersion);
                }
            }

            // Any checks and actions that need to be performed before being able to downgrade needs
            // to be placed on the _prepareToUpgrade and _prepareToDowngrade functions. After the
            // prepare function complete, a node is not allowed to refuse to upgrade/downgrade.
            if (requestedVersion > actualVersion) {
                _prepareToUpgrade(opCtx, request, changeTimestamp);
            } else {
                _prepareToDowngrade(opCtx, request, changeTimestamp);
            }

            if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                // Tell the shards to enter the 'prepare' phase of setFCV (check that they will be
                // able to upgrade or downgrade).
                _sendSetFCVRequestToShards(
                    opCtx, request, changeTimestamp, SetFCVPhaseEnum::kPrepare);
            }

            if (request.getPhase() == SetFCVPhaseEnum::kPrepare) {
                invariant(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
                // If we are only running the 'prepare' phase, then we are done
                return true;
            }
        }

        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());
        invariant(!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kComplete);

        // All feature-specific FCV upgrade or downgrade code should go into the respective
        // _runUpgrade and _runDowngrade functions. Each of them have their own helper functions
        // where all feature-specific upgrade/downgrade code should be placed. Please read the
        // comments on the helper functions for more details on where to place the code.
        if (requestedVersion > actualVersion) {
            _runUpgrade(opCtx, request, changeTimestamp);
        } else {
            _runDowngrade(opCtx, request, changeTimestamp);
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
                serverGlobalParams.featureCompatibility.getVersion(),
                requestedVersion,
                isFromConfigServer,
                changeTimestamp,
                false /* setTargetVersion */,
                false /* setIsCleaningServerMetadata */);
        }

        // TODO SERVER-72796: Remove once gGlobalIndexesShardingCatalog is enabled.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
            requestedVersion > actualVersion &&
            feature_flags::gGlobalIndexesShardingCatalog
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, actualVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kRenameCollectionPre63Compatible);
        }

        // TODO SERVER-73627: Remove once 7.0 becomes last LTS.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
            requestedVersion > actualVersion &&
            feature_flags::gDropCollectionHoldingCriticalSection
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, actualVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kDropCollectionPre70Compatible);
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kDropDatabasePre70Compatible);
        }

        // TODO SERVER-68373: Remove once 7.0 becomes last LTS
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
            requestedVersion > actualVersion &&
            mongo::gFeatureFlagFLE2CompactForProtocolV2
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, actualVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kCompactStructuredEncryptionDataPre70Compatible);
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kCompactStructuredEncryptionDataPre61Compatible);
        }

        LOGV2(6744302,
              "setFeatureCompatibilityVersion succeeded",
              "upgradeOrDowngrade"_attr = upgradeOrDowngrade,
              "serverType"_attr = server_type,
              "fromVersion"_attr = actualVersion,
              "toVersion"_attr = requestedVersion);

        return true;
    }

private:
    // This helper function is only for any actions that should be done specifically on
    // shard servers during phase 1 of the 2-phase setFCV protocol for sharded clusters.
    // For example, before completing phase 1, we must wait for backward incompatible
    // ShardingDDLCoordinators to finish. This is important in order to ensure that no
    // shard that is currently a participant of such a backward-incompatible
    // ShardingDDLCoordinator can transition to the fully downgraded state (and thus,
    // possibly downgrade its binary) while the coordinator is still in progress.
    // The fact that the FCV has already transitioned to kDowngrading ensures that no
    // new backward-incompatible ShardingDDLCoordinators can start.
    // We do not expect any other feature-specific work to be done in the 'start' phase.
    void _shardServerPhase1Tasks(OperationContext* opCtx,
                                 multiversion::FeatureCompatibilityVersion requestedVersion) {
        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());
        const auto& [originalVersion, _] =
            getTransitionFCVFromAndTo(serverGlobalParams.featureCompatibility.getVersion());
        const auto isDowngrading = originalVersion > requestedVersion;
        const auto isUpgrading = originalVersion < requestedVersion;
        // TODO (SERVER-71309): Remove once 7.0 becomes last LTS.
        if (isDowngrading &&
            feature_flags::gResilientMovePrimary.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(opCtx,
                                                           DDLCoordinatorTypeEnum::kMovePrimary);
        }

        // TODO SERVER-68008: Remove collMod draining mechanism after 7.0 becomes last LTS.
        if (isDowngrading &&
            feature_flags::gCollModCoordinatorV3.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            // Drain all running collMod v3 coordinator because they produce backward
            // incompatible on disk metadata
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(opCtx, DDLCoordinatorTypeEnum::kCollMod);
        }

        // TODO SERVER-72796: Remove once gGlobalIndexesShardingCatalog is enabled.
        if (isDowngrading &&
            feature_flags::gGlobalIndexesShardingCatalog
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kRenameCollection);
        }

        // TODO SERVER-73627: Remove once 7.0 becomes last LTS.
        if (isDowngrading &&
            feature_flags::gDropCollectionHoldingCriticalSection
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(opCtx,
                                                           DDLCoordinatorTypeEnum::kDropCollection);
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(opCtx,
                                                           DDLCoordinatorTypeEnum::kDropDatabase);
        }

        // TODO SERVER-68373 remove once 7.0 becomes last LTS
        if (isDowngrading &&
            mongo::gFeatureFlagFLE2CompactForProtocolV2
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            // Drain the QE compact coordinator because it persists state that is
            // not backwards compatible with earlier versions.
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kCompactStructuredEncryptionData);
        }

        if (isUpgrading) {
            _createShardingIndexCatalogIndexes(opCtx, requestedVersion);
        }
    }

    // This helper function is for any actions that should be done before taking the FCV full
    // transition lock in S mode. It is required that the code in this helper function is idempotent
    // and could be done after _runDowngrade even if it failed at any point in the middle of
    // _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
    void _prepareToUpgradeActions(OperationContext* opCtx) {
        if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            _cancelServerlessMigrations(opCtx);
            return;
        }

        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            // Config server role actions.
        }

        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            // Shard server role actions.
        }
    }

    // This helper function is for any user collections uasserts, creations, or deletions that need
    // to happen during the upgrade. It is required that the code in this helper function is
    // idempotent and could be done after _runDowngrade even if it failed at any point in the middle
    // of _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
    void _userCollectionsWorkForUpgrade() {
        return;
    }

    // This helper function is for updating metadata to make sure the new features in the
    // upgraded version work for sharded and non-sharded clusters. It is required that the code
    // in this helper function is idempotent and could be done after _runDowngrade even if it
    // failed at any point in the middle of _userCollectionsUassertsForDowngrade or
    // _internalServerCleanupForDowngrade.
    void _completeUpgrade(OperationContext* opCtx,
                          const multiversion::FeatureCompatibilityVersion requestedVersion) {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            const auto actualVersion = serverGlobalParams.featureCompatibility.getVersion();
            _cleanupConfigVersionOnUpgrade(opCtx, requestedVersion, actualVersion);
            _createSchemaOnConfigSettings(opCtx, requestedVersion, actualVersion);
            _setOnCurrentShardSinceFieldOnChunks(opCtx, requestedVersion, actualVersion);
            // Depends on _setOnCurrentShardSinceFieldOnChunks()
            _initializePlacementHistory(opCtx, requestedVersion, actualVersion);
            _dropConfigMigrationsCollection(opCtx);
            _setShardedClusterCardinalityParam(opCtx, requestedVersion);
        }

        _removeRecordPreImagesCollectionOption(opCtx);
    }

    // TODO SERVER-68889 remove once 7.0 becomes last LTS
    void _cleanupConfigVersionOnUpgrade(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion actualVersion) {
        if (feature_flags::gStopUsingConfigVersion.isEnabledOnTargetFCVButDisabledOnOriginalFCV(
                requestedVersion, actualVersion)) {
            LOGV2(6888800, "Removing deprecated fields from config.version collection");
            static const std::vector<StringData> deprecatedFields{
                "excluding"_sd,
                "upgradeId"_sd,
                "upgradeState"_sd,
                StringData{VersionType::currentVersion.name()},
                StringData{VersionType::minCompatibleVersion.name()},
            };

            const auto updateObj = [&] {
                BSONObjBuilder updateBuilder;
                BSONObjBuilder unsetBuilder(updateBuilder.subobjStart("$unset"));
                for (const auto deprecatedField : deprecatedFields) {
                    unsetBuilder.append(deprecatedField.toString(), true);
                }
                unsetBuilder.doneFast();
                return updateBuilder.obj();
            }();

            DBDirectClient client(opCtx);
            write_ops::UpdateCommandRequest update(VersionType::ConfigNS);
            update.setUpdates({[&]() {
                write_ops::UpdateOpEntry entry;
                entry.setQ({});
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(updateObj));
                entry.setMulti(true);
                entry.setUpsert(false);
                return entry;
            }()});
            client.update(update);
        }
    }

    // TODO SERVER-68889 remove once 7.0 becomes last LTS
    void _updateConfigVersionOnDowngrade(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion originalVersion) {
        if (feature_flags::gStopUsingConfigVersion.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            LOGV2(6888801, "Restoring removed fields in config.version collection");
            const auto updateObj = [&] {
                BSONObjBuilder updateBuilder;
                BSONObjBuilder unsetBuilder(updateBuilder.subobjStart("$set"));
                unsetBuilder.append(VersionType::minCompatibleVersion.name(),
                                    VersionType::MIN_COMPATIBLE_CONFIG_VERSION);
                unsetBuilder.append(VersionType::currentVersion.name(),
                                    VersionType::CURRENT_CONFIG_VERSION);
                unsetBuilder.doneFast();
                return updateBuilder.obj();
            }();

            DBDirectClient client(opCtx);
            write_ops::UpdateCommandRequest update(VersionType::ConfigNS);
            update.setUpdates({[&]() {
                write_ops::UpdateOpEntry entry;
                entry.setQ({});
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(updateObj));
                entry.setMulti(true);
                entry.setUpsert(false);
                return entry;
            }()});
            client.update(update);
        }
    }

    // TODO SERVER-69106 remove once v7.0 becomes last-lts
    void _initializePlacementHistory(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion actualVersion) {
        if (feature_flags::gHistoricalPlacementShardingCatalog
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, actualVersion)) {
            ShardingCatalogManager::get(opCtx)->initializePlacementHistory(opCtx);
        }
    }

    void _createShardingIndexCatalogIndexes(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        // TODO SERVER-67392: Remove once gGlobalIndexesShardingCatalog is enabled.
        const auto actualVersion = serverGlobalParams.featureCompatibility.getVersion();
        if (feature_flags::gGlobalIndexesShardingCatalog
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, actualVersion)) {
            uassertStatusOK(sharding_util::createShardingIndexCatalogIndexes(opCtx));
            if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
                uassertStatusOK(sharding_util::createShardCollectionCatalogIndexes(opCtx));
            }
        }
    }

    // TODO (SERVER-70763): Remove once FCV 7.0 becomes last-lts.
    void _createSchemaOnConfigSettings(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion actualVersion) {
        if (feature_flags::gConfigSettingsSchema.isEnabledOnTargetFCVButDisabledOnOriginalFCV(
                requestedVersion, actualVersion)) {
            LOGV2(6885200, "Creating schema on config.settings");
            uassertStatusOK(ShardingCatalogManager::get(opCtx)->upgradeConfigSettings(opCtx));
        }
    }

    void _setShardedClusterCardinalityParam(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        if (feature_flags::gClusterCardinalityParameter.isEnabledOnVersion(requestedVersion)) {
            // Get current cluster parameter value so that we don't run SetClusterParameter
            // extraneously
            auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
            auto* clusterCardinalityParam =
                clusterParameters->get<ClusterParameterWithStorage<ShardedClusterCardinalityParam>>(
                    "shardedClusterCardinalityForDirectConns");
            auto currentValue =
                clusterCardinalityParam->getValue(boost::none).getHasTwoOrMoreShards();

            // config.shards is stable during FCV changes, so query that to discover the current
            // number of shards.
            DBDirectClient client(opCtx);
            FindCommandRequest findRequest{NamespaceString::kConfigsvrShardsNamespace};
            findRequest.setLimit(2);
            auto numShards = client.find(std::move(findRequest))->itcount();
            bool expectedValue = numShards >= 2;

            if (expectedValue == currentValue) {
                return;
            }

            ConfigsvrSetClusterParameter configsvrSetClusterParameter(
                BSON("shardedClusterCardinalityForDirectConns"
                     << BSON("hasTwoOrMoreShards" << expectedValue)));
            configsvrSetClusterParameter.setDbName(DatabaseName(boost::none, "admin"));

            const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
            const auto cmdResponse =
                uassertStatusOK(shardRegistry->getConfigShard()->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    DatabaseName::kAdmin.toString(),
                    configsvrSetClusterParameter.toBSON({}),
                    Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(std::move(cmdResponse)));
        }
    }

    // TODO (SERVER-72791): Remove once FCV 7.0 becomes last-lts.
    void _setOnCurrentShardSinceFieldOnChunks(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion actualVersion) {
        if (feature_flags::gAutoMerger.isEnabledOnTargetFCVButDisabledOnOriginalFCV(
                requestedVersion, actualVersion)) {
            ShardingCatalogManager::get(opCtx)->setOnCurrentShardSinceFieldOnChunks(opCtx);
        }
    }

    // Removes collection option "recordPreImages" from all collection definitions.
    // TODO SERVER-74036: Remove once FCV 7.0 becomes last-LTS.
    void _removeRecordPreImagesCollectionOption(OperationContext* opCtx) {
        for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
            Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
            catalog::forEachCollectionFromDb(
                opCtx,
                dbName,
                MODE_X,
                [&](const Collection* collection) {
                    // To remove collection option "recordPreImages" from persistent storage, issue
                    // the "collMod" command with none of the parameters set.
                    BSONObjBuilder responseBuilder;
                    uassertStatusOK(processCollModCommand(
                        opCtx, collection->ns(), CollMod{collection->ns()}, &responseBuilder));
                    LOGV2(7383300,
                          "Removed 'recordPreImages' collection option",
                          "ns"_attr = collection->ns(),
                          "collModResponse"_attr = responseBuilder.obj());
                    return true;
                },
                [&](const Collection* collection) {
                    return collection->getCollectionOptions().recordPreImagesOptionUsed;
                });
        }
    }

    // TODO SERVER-75080 get rid of `_dropConfigMigrationsCollection` once v7.0 branches out
    void _dropConfigMigrationsCollection(OperationContext* opCtx) {
        // Dropping potential leftover `config.migrations` collection as it is unused since v6.0
        DropReply dropReply;
        const auto deletionStatus =
            dropCollection(opCtx,
                           NamespaceString::kMigrationsNamespace,
                           &dropReply,
                           DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
        uassert(deletionStatus.code(),
                str::stream() << "Failed to drop " << NamespaceString::kMigrationsNamespace
                              << causedBy(deletionStatus.reason()),
                deletionStatus.isOK() || deletionStatus.code() == ErrorCodes::NamespaceNotFound);
    }

    // _prepareToUpgrade performs all actions and checks that need to be done before proceeding to
    // make any metadata changes as part of FCV upgrade. Any new feature specific upgrade code
    // should be placed in the _prepareToUpgrade helper functions:
    //  * _prepareToUpgradeActions: for any upgrade actions that should be done before taking the
    //  FCV full transition lock in S mode
    //  * _userCollectionsWorkForUpgrade: for any user collections uasserts, creations, or deletions
    //    that need to happen during the upgrade. This happens after the FCV full transition lock.
    // Please read the comments on those helper functions for more details on what should be placed
    // in each function.
    void _prepareToUpgrade(OperationContext* opCtx,
                           const SetFeatureCompatibilityVersion& request,
                           boost::optional<Timestamp> changeTimestamp) {
        // This helper function is for any actions that should be done before taking the FCV full
        // transition lock in S mode. It is required that the code in this helper function is
        // idempotent and could be done after _runDowngrade even if it failed at any point in the
        // middle of _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
        _prepareToUpgradeActions(opCtx);

        {
            // Take the FCV full transition lock in S mode to create a barrier for operations taking
            // the global IX or X locks, which implicitly take the FCV full transition lock in IX
            // mode (aside from those which explicitly opt out). This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::ResourceLock lk(opCtx, resourceIdFeatureCompatibilityVersion, MODE_S);
        }

        // This helper function is for any user collections uasserts, creations, or deletions that
        // need to happen during the upgrade. It is required that the code in this helper function
        // is idempotent and could be done after _runDowngrade even if it failed at any point in the
        // middle of _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            _userCollectionsWorkForUpgrade();
        }

        uassert(ErrorCodes::Error(549180),
                "Failing upgrade due to 'failUpgrading' failpoint set",
                !failUpgrading.shouldFail());
    }

    // _runUpgrade performs all the metadata-changing actions of an FCV upgrade. Any new feature
    // specific upgrade code should be placed in the _runUpgrade helper functions:
    //  * _completeUpgrade: for updating metadata to make sure the new features in the upgraded
    //    version work for sharded and non-sharded clusters
    // Please read the comments on those helper functions for more details on what should be placed
    // in each function.
    void _runUpgrade(OperationContext* opCtx,
                     const SetFeatureCompatibilityVersion& request,
                     boost::optional<Timestamp> changeTimestamp) {
        const auto requestedVersion = request.getCommandParameter();

        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {

            // Always abort the reshardCollection regardless of version to ensure that it will run
            // on a consistent version from start to finish. This will ensure that it will be able
            // to apply the oplog entries correctly.
            abortAllReshardCollection(opCtx);

            // TODO SERVER-68551: Remove once 7.0 becomes last-lts
            dropDistLockCollections(opCtx);

            _createShardingIndexCatalogIndexes(opCtx, requestedVersion);

            // Tell the shards to complete setFCV (transition to fully upgraded)
            _sendSetFCVRequestToShards(opCtx, request, changeTimestamp, SetFCVPhaseEnum::kComplete);
        }

        // This helper function is for updating metadata to make sure the new features in the
        // upgraded version work for sharded and non-sharded clusters. It is required that the code
        // in this helper function is idempotent and could be done after _runDowngrade even if it
        // failed at any point in the middle of _userCollectionsUassertsForDowngrade or
        // _internalServerCleanupForDowngrade.
        _completeUpgrade(opCtx, requestedVersion);

        hangWhileUpgrading.pauseWhileSet(opCtx);
    }

    // This helper function is for any actions that should be done before taking the FCV full
    // transition lock in S mode.
    void _prepareToDowngradeActions(OperationContext* opCtx) {
        if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            _cancelServerlessMigrations(opCtx);
            return;
        }

        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            // Config server role actions.
        }

        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            // Shard server role actions.
        }
    }

    // Tell the shards to enter phase-1 or phase-2 of setFCV.
    void _sendSetFCVRequestToShards(OperationContext* opCtx,
                                    const SetFeatureCompatibilityVersion& request,
                                    boost::optional<Timestamp> changeTimestamp,
                                    enum mongo::SetFCVPhaseEnum phase) {
        auto requestPhase = request;
        requestPhase.setFromConfigServer(true);
        requestPhase.setPhase(phase);
        requestPhase.setChangeTimestamp(changeTimestamp);
        uassertStatusOK(ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
            opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase.toBSON({}))));
    }

    // This helper function is for any uasserts for users to clean up user collections. Uasserts for
    // users to change settings or wait for settings to change should also happen here. These
    // uasserts happen before the internal server downgrade cleanup. The code in this helper
    // function is required to be idempotent in case the node crashes or downgrade fails in a way
    // that the user has to run setFCV again. The code added/modified in this helper function should
    // not leave the server in an inconsistent state if the actions in this function failed part way
    // through.
    // This helper function can only fail with some transient error that can be retried (like
    // InterruptedDueToReplStateChange) or ErrorCode::CannotDowngrade. The uasserts added to this
    // helper function can only have the CannotDowngrade error code indicating that the user must
    // manually clean up some user data in order to retry the FCV downgrade.
    void _userCollectionsUassertsForDowngrade(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());
        const auto& [originalVersion, _] =
            getTransitionFCVFromAndTo(serverGlobalParams.featureCompatibility.getVersion());

        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            if (gFeatureFlagCatalogShard.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                    requestedVersion, originalVersion)) {
                _assertNoCollectionsHaveChangeStreamsPrePostImages(opCtx);
            }

            if (feature_flags::gGlobalIndexesShardingCatalog
                    .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                  originalVersion)) {
                bool hasShardingIndexCatalogEntries;
                BSONObj indexDoc, collDoc;
                {
                    AutoGetCollection indexesColl(
                        opCtx, NamespaceString::kConfigsvrIndexCatalogNamespace, MODE_IS);
                    hasShardingIndexCatalogEntries =
                        Helpers::findOne(opCtx, indexesColl.getCollection(), BSONObj(), indexDoc);
                }
                if (hasShardingIndexCatalogEntries) {
                    auto uuid = uassertStatusOK(
                        UUID::parse(indexDoc[IndexCatalogType::kCollectionUUIDFieldName]));
                    AutoGetCollection collsColl(
                        opCtx, NamespaceString::kConfigsvrCollectionsNamespace, MODE_IS);
                    Helpers::findOne(opCtx,
                                     collsColl.getCollection(),
                                     BSON(CollectionType::kUuidFieldName << uuid),
                                     collDoc);
                }
                uassert(ErrorCodes::CannotDowngrade,
                        str::stream()
                            << "Cannot downgrade the cluster when there are global indexes "
                               "present. Drop all global indexes before downgrading. First "
                               "detected global index name: '"
                            << indexDoc[IndexCatalogType::kNameFieldName].String()
                            << "' on collection '"
                            << NamespaceString(collDoc[CollectionType::kNssFieldName].String())
                            << "'",
                        !hasShardingIndexCatalogEntries);
            }
        }

        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            if (feature_flags::gTimeseriesScalabilityImprovements
                    .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                  originalVersion)) {
                for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                    catalog::forEachCollectionFromDb(
                        opCtx,
                        dbName,
                        MODE_S,
                        [&](const Collection* collection) {
                            auto tsOptions = collection->getTimeseriesOptions();
                            invariant(tsOptions);

                            auto indexCatalog = collection->getIndexCatalog();
                            auto indexIt = indexCatalog->getIndexIterator(
                                opCtx,
                                IndexCatalog::InclusionPolicy::kReady |
                                    IndexCatalog::InclusionPolicy::kUnfinished);

                            // Check and fail to downgrade if the time-series collection has a
                            // partial, TTL index.
                            while (indexIt->more()) {
                                auto indexEntry = indexIt->next();
                                if (indexEntry->descriptor()->isPartial()) {
                                    // TODO (SERVER-67659): Remove partial, TTL index check once
                                    // FCV 7.0 becomes last-lts.
                                    uassert(
                                        ErrorCodes::CannotDowngrade,
                                        str::stream()
                                            << "Cannot downgrade the cluster when there are "
                                               "secondary "
                                               "TTL indexes with partial filters on time-series "
                                               "collections. Drop all partial, TTL indexes on "
                                               "time-series collections before downgrading. First "
                                               "detected incompatible index name: '"
                                            << indexEntry->descriptor()->indexName()
                                            << "' on collection: '"
                                            << collection->ns().getTimeseriesViewNamespace() << "'",
                                        !indexEntry->descriptor()->infoObj().hasField(
                                            IndexDescriptor::kExpireAfterSecondsFieldName));
                                }
                            }

                            // Check the time-series options for a default granularity. Fail the
                            // downgrade if the bucketing parameters are custom values.
                            uassert(
                                ErrorCodes::CannotDowngrade,
                                str::stream()
                                    << "Cannot downgrade the cluster when there are time-series "
                                       "collections with custom bucketing parameters. In order to "
                                       "downgrade, the time-series collection(s) must be updated "
                                       "with a granularity of 'seconds', 'minutes' or 'hours'. "
                                       "First detected incompatible collection: '"
                                    << collection->ns().getTimeseriesViewNamespace() << "'",
                                tsOptions->getGranularity().has_value());

                            return true;
                        },
                        [&](const Collection* collection) {
                            return collection->getTimeseriesOptions() != boost::none;
                        });
                }
            }

            // Block downgrade for collections with encrypted fields
            // TODO SERVER-67760: Remove once FCV 7.0 becomes last-lts.
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx, dbName, MODE_X, [&](const Collection* collection) {
                        auto& efc = collection->getCollectionOptions().encryptedFieldConfig;

                        uassert(ErrorCodes::CannotDowngrade,
                                str::stream() << "Cannot downgrade the cluster as collection "
                                              << collection->ns()
                                              << " has 'encryptedFields' with range indexes",
                                !(efc.has_value() &&
                                  hasQueryType(efc.get(), QueryTypeEnum::RangePreview)));
                        return true;
                    });
            }

            if (feature_flags::gfeatureFlagCappedCollectionsRelaxedSize
                    .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                  originalVersion)) {
                for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                    catalog::forEachCollectionFromDb(
                        opCtx,
                        dbName,
                        MODE_S,
                        [&](const Collection* collection) {
                            uasserted(
                                ErrorCodes::CannotDowngrade,
                                str::stream()
                                    << "Cannot downgrade the cluster when there are capped "
                                       "collection with a size that is non multiple of 256 bytes. "
                                       "Drop or resize the following collection: '"
                                    << collection->ns() << "'");
                            return true;
                        },
                        [&](const Collection* collection) {
                            return collection->isCapped() &&
                                collection->getCappedMaxSize() % 256 != 0;
                        });
                }
            }
        }
    }

    // Remove cluster parameters from the clusterParameters collections which are not enabled on
    // requestedVersion.
    void _cleanUpClusterParameters(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());
        const auto& [fromVersion, _] =
            getTransitionFCVFromAndTo(serverGlobalParams.featureCompatibility.getVersion());

        auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
        std::vector<write_ops::DeleteOpEntry> deletes;
        for (const auto& [name, sp] : clusterParameters->getMap()) {
            if (sp->isEnabledOnVersion(fromVersion) && !sp->isEnabledOnVersion(requestedVersion)) {
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
    void _internalServerCleanupForDowngrade(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());
        const auto& [originalVersion, _] =
            getTransitionFCVFromAndTo(serverGlobalParams.featureCompatibility.getVersion());
        _cleanUpClusterParameters(opCtx, requestedVersion);
        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            _dropInternalShardingIndexCatalogCollection(opCtx, requestedVersion, originalVersion);
            _removeSchemaOnConfigSettings(opCtx, requestedVersion, originalVersion);
            // Always abort the reshardCollection regardless of version to ensure that it will
            // run on a consistent version from start to finish. This will ensure that it will
            // be able to apply the oplog entries correctly.
            abortAllReshardCollection(opCtx);
            _updateConfigVersionOnDowngrade(opCtx, requestedVersion, originalVersion);
        }

        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            // If we are downgrading to a version that doesn't support implicit translation of
            // Timeseries collection in sharding DDL Coordinators we need to drain all ongoing
            // coordinators
            if (feature_flags::gImplicitDDLTimeseriesNssTranslation
                    .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                  originalVersion)) {
                ShardingDDLCoordinatorService::getService(opCtx)
                    ->waitForOngoingCoordinatorsToFinish(opCtx);
            }
            _dropInternalShardingIndexCatalogCollection(opCtx, requestedVersion, originalVersion);
        }
    }

    void _dropInternalShardingIndexCatalogCollection(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion originalVersion) {
        // TODO SERVER-67392: Remove when 7.0 branches-out.
        // Coordinators that commits indexes to the csrs must be drained before this point. Older
        // FCV's must not find cluster-wide indexes.
        DropReply dropReply;
        if (feature_flags::gGlobalIndexesShardingCatalog
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            // Note the config server is also considered a shard, so the ConfigServer and
            // ShardServer roles aren't mutually exclusive.
            if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
                // There cannot be any global indexes at this point, but calling
                // clearCollectionShardingIndexCatalog removes the index version from
                // config.shard.collections and the csr transactionally.
                LOGV2(7013200, "Clearing global indexes for all collections");
                DBDirectClient client(opCtx);
                FindCommandRequest findCmd{NamespaceString::kShardCollectionCatalogNamespace};
                findCmd.setFilter(BSON(ShardAuthoritativeCollectionType::kIndexVersionFieldName
                                       << BSON("$exists" << true)));
                auto cursor = client.find(std::move(findCmd));
                while (cursor->more()) {
                    const auto collectionDoc = cursor->next();
                    auto collection = ShardAuthoritativeCollectionType::parse(
                        IDLParserContext("FCVDropIndexCatalogCtx"), collectionDoc);
                    clearCollectionShardingIndexCatalog(
                        opCtx, collection.getNss(), collection.getUuid());
                }

                LOGV2(6711905,
                      "Droping collection catalog collection",
                      "nss"_attr = NamespaceString::kShardCollectionCatalogNamespace);
                const auto dropStatus =
                    dropCollection(opCtx,
                                   NamespaceString::kShardCollectionCatalogNamespace,
                                   &dropReply,
                                   DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
                uassert(dropStatus.code(),
                        str::stream() << "Failed to drop "
                                      << NamespaceString::kShardCollectionCatalogNamespace
                                      << causedBy(dropStatus.reason()),
                        dropStatus.isOK() || dropStatus.code() == ErrorCodes::NamespaceNotFound);
            }

            if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                LOGV2(6711906,
                      "Unset index version field in config.collections",
                      "nss"_attr = CollectionType::ConfigNS);
                DBDirectClient client(opCtx);
                write_ops::UpdateCommandRequest update(CollectionType::ConfigNS);
                update.setUpdates({[&]() {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(BSON(ShardAuthoritativeCollectionType::kIndexVersionFieldName
                                    << BSON("$exists" << true)));
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON(
                        "$unset" << BSON(ShardAuthoritativeCollectionType::kIndexVersionFieldName
                                         << true))));
                    entry.setMulti(true);
                    return entry;
                }()});
                update.getWriteCommandRequestBase().setOrdered(false);
                client.update(update);
            }

            // TODO SERVER-75274: Drop both collections on a catalog shard enabled config server.
            NamespaceString indexCatalogNss;
            if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
                indexCatalogNss = NamespaceString::kConfigsvrIndexCatalogNamespace;
            } else {
                indexCatalogNss = NamespaceString::kShardIndexCatalogNamespace;
            }
            LOGV2(6280502, "Dropping global indexes collection", "nss"_attr = indexCatalogNss);
            const auto deletionStatus =
                dropCollection(opCtx,
                               indexCatalogNss,
                               &dropReply,
                               DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
            uassert(deletionStatus.code(),
                    str::stream() << "Failed to drop " << indexCatalogNss
                                  << causedBy(deletionStatus.reason()),
                    deletionStatus.isOK() ||
                        deletionStatus.code() == ErrorCodes::NamespaceNotFound);
        }
    }

    // TODO (SERVER-70763): Remove once FCV 7.0 becomes last-lts.
    void _removeSchemaOnConfigSettings(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion originalVersion) {
        if (feature_flags::gConfigSettingsSchema.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            LOGV2(6885201, "Removing schema on config.settings");
            CollMod collModCmd{NamespaceString::kConfigSettingsNamespace};
            collModCmd.getCollModRequest().setValidator(BSONObj());
            collModCmd.getCollModRequest().setValidationLevel(ValidationLevelEnum::off);
            BSONObjBuilder builder;
            uassertStatusOKIgnoreNSNotFound(processCollModCommand(
                opCtx, {NamespaceString::kConfigSettingsNamespace}, collModCmd, &builder));
        }
    }

    // _prepareToDowngrade performs all actions and checks that need to be done before proceeding to
    // make any metadata changes as part of FCV downgrade. Any new feature specific downgrade
    // code should be placed in the helper functions:
    // * _prepareToDowngradeActions: Any downgrade actions that should be done before taking the FCV
    // full transition lock in S mode should go in this function.
    // * _userCollectionsUassertsForDowngrade: for any checks on user data or settings that will
    // uassert if users need to manually clean up user data or settings.
    // When doing feature flag checking for downgrade, we should check the feature flag is enabled
    // on current FCV and will be disabled after downgrade by using
    // isDisabledOnTargetFCVButEnabledOnOriginalFCV(targetFCV, originalFCV)
    // Please read the comments on those helper functions for more details on what should be placed
    // in each function.
    void _prepareToDowngrade(OperationContext* opCtx,
                             const SetFeatureCompatibilityVersion& request,
                             boost::optional<Timestamp> changeTimestamp) {
        const auto requestedVersion = request.getCommandParameter();

        // Any actions that should be done before taking the FCV full transition lock in S mode
        // should go in this function.
        _prepareToDowngradeActions(opCtx);

        {
            // Take the FCV full transition lock in S mode to create a barrier for operations taking
            // the global IX or X locks, which implicitly take the FCV full transition lock in IX
            // mode (aside from those which explicitly opt out). This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::ResourceLock lk(opCtx, resourceIdFeatureCompatibilityVersion, MODE_S);
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
        _userCollectionsUassertsForDowngrade(opCtx, requestedVersion);
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
        const auto requestedVersion = request.getCommandParameter();
        const auto actualVersion = serverGlobalParams.featureCompatibility.getVersion();
        auto isFromConfigServer = request.getFromConfigServer().value_or(false);

        hangDowngradingBeforeIsCleaningServerMetadata.pauseWhileSet(opCtx);
        // Set the isCleaningServerMetadata field to true. This prohibits the downgrading to
        // upgrading transition until the isCleaningServerMetadata is unset when we successfully
        // finish the FCV downgrade and transition to the DOWNGRADED state.
        // (Ignore FCV check): This is intentional because we want to use this feature even if we
        // are in downgrading fcv state.
        if (repl::feature_flags::gDowngradingToUpgrading.isEnabledAndIgnoreFCVUnsafe()) {
            {
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));
                FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                    opCtx,
                    actualVersion,
                    requestedVersion,
                    isFromConfigServer,
                    changeTimestamp,
                    true /* setTargetVersion */,
                    true /* setIsCleaningServerMetadata*/);
            }
        }

        uassert(ErrorCodes::Error(7428201),
                "Failing downgrade due to 'failDowngradingDuringIsCleaningServerMetadata' "
                "failpoint set",
                !failDowngradingDuringIsCleaningServerMetadata.shouldFail());

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
        _internalServerCleanupForDowngrade(opCtx, requestedVersion);

        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            // Tell the shards to complete setFCV (transition to fully downgraded).
            _sendSetFCVRequestToShards(opCtx, request, changeTimestamp, SetFCVPhaseEnum::kComplete);
        }

        hangBeforeTransitioningToDowngraded.pauseWhileSet(opCtx);
    }

    /**
     * Abort all serverless migrations active on this node, for both donors and recipients.
     * Called after reaching an upgrading or downgrading state.
     */
    void _cancelServerlessMigrations(OperationContext* opCtx) {
        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());
        if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            auto donorService = checked_cast<TenantMigrationDonorService*>(
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName));
            donorService->abortAllMigrations(opCtx);

            auto recipientService = checked_cast<repl::TenantMigrationRecipientService*>(
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(repl::TenantMigrationRecipientService::
                                              kTenantMigrationRecipientServiceName));
            recipientService->abortAllMigrations(opCtx);

            if (getGlobalReplSettings().isServerless()) {
                auto splitDonorService = checked_cast<ShardSplitDonorService*>(
                    repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                        ->lookupServiceByName(ShardSplitDonorService::kServiceName));
                splitDonorService->abortAllSplits(opCtx);

                auto mergeRecipientService = checked_cast<repl::ShardMergeRecipientService*>(
                    repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                        ->lookupServiceByName(
                            repl::ShardMergeRecipientService::kShardMergeRecipientServiceName));
                mergeRecipientService->abortAllMigrations(opCtx);
            }
        }
    }

    /**
     * For sharded cluster servers:
     *  Generate a new changeTimestamp if change fcv is called on config server,
     *  otherwise retrieve changeTimestamp from the Config Server request.
     */
    boost::optional<Timestamp> getChangeTimestamp(mongo::OperationContext* opCtx,
                                                  mongo::SetFeatureCompatibilityVersion request) {
        boost::optional<Timestamp> changeTimestamp;
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            // The Config Server always creates a new ID (i.e., timestamp) when it receives an
            // upgrade or downgrade request.
            const auto now = VectorClock::get(opCtx)->getTime();
            changeTimestamp = now.clusterTime().asTimestamp();
        } else if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
                   request.getPhase()) {
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

    void _assertNoCollectionsHaveChangeStreamsPrePostImages(OperationContext* opCtx) {
        invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

        // Config servers only started allowing collections with changeStreamPreAndPostImages
        // in 7.0, so don't allow downgrading with such a collection.
        for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
            Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
            catalog::forEachCollectionFromDb(
                opCtx,
                dbName,
                MODE_S,
                [&](const Collection* collection) {
                    uassert(ErrorCodes::CannotDowngrade,
                            str::stream() << "Cannot downgrade the config server as collection "
                                          << collection->ns()
                                          << " has 'changeStreamPreAndPostImages' enabled. Please "
                                             "unset the option or drop the collection.",
                            !collection->isChangeStreamPreAndPostImagesEnabled());
                    return true;
                },
                [&](const Collection* collection) {
                    return collection->isChangeStreamPreAndPostImagesEnabled();
                });
        }
    }

} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
