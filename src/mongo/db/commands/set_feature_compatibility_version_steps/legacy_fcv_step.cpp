/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/commands/set_feature_compatibility_version_steps/legacy_fcv_step.h"

#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/drop_collection_coordinator.h"
#include "mongo/db/global_catalog/ddl/placement_history_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/db/shard_role/ddl/list_collections_gen.h"
#include "mongo/db/shard_role/shard_catalog/coll_mod.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"
#include "mongo/db/shard_role/shard_catalog/collection_options_gen.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/upgrade_downgrade_viewless_timeseries.h"
#include "mongo/db/timeseries/upgrade_downgrade_viewless_timeseries_sharded_cluster.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/user_write_block/write_block_bypass.h"
#include "mongo/logv2/log.h"
#include "mongo/s/migration_blocking_operation/migration_blocking_operation_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {


class LegacyFCVStep : public mongo::FCVStep {
public:
    static LegacyFCVStep* get(ServiceContext* serviceContext);


    inline std::string getStepName() const final {
        return "LegacyFCVStep";
    }

private:
    void prepareToUpgradeActionsBeforeGlobalLock(OperationContext* opCtx,
                                                 FCV originalVersion,
                                                 FCV requestedVersion) final {
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

    void userCollectionsUassertsForUpgrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final {}

    void userCollectionsWorkForUpgrade(OperationContext* opCtx,
                                       FCV originalVersion,
                                       FCV requestedVersion) final {

        if (feature_flags::gRemoveLegacyTimeseriesBucketingParametersHaveChanged
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, originalVersion)) {
            catalog::modifyAllCollectionsMatching(
                opCtx,
                [&](const Collection* collection) {
                    CollMod collModCmd{collection->ns()};
                    collModCmd.set_removeLegacyTimeseriesBucketingParametersHaveChanged(true);

                    BSONObjBuilder responseBuilder;
                    uassertStatusOK(processCollModCommand(
                        opCtx, collection->ns(), collModCmd, nullptr, &responseBuilder));
                },
                [&](const Collection* collection) {
                    return collection->shouldRemoveLegacyTimeseriesBucketingParametersHaveChanged();
                });
        }
    }

    void upgradeServerMetadata(OperationContext* opCtx,
                               FCV originalVersion,
                               FCV requestedVersion) final {
        auto role = ShardingState::get(opCtx)->pollClusterRole();

        // TODO SERVER-116437: Remove once 9.0 becomes last lts.
        if (role && role->has(ClusterRole::ConfigServer)) {
            _removeShardStateFromConfigCollection(opCtx);
        }

        // TODO SERVER-103046: Remove once 9.0 becomes last lts.
        if (role && role->has(ClusterRole::ShardServer) &&
            feature_flags::gTerminateSecondaryReadsUponRangeDeletion
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, originalVersion)) {
            rangedeletionutil::setPreMigrationShardVersionOnRangeDeletionTasks(opCtx);
        }

        _cleanUpIndexCatalogMetadataOnUpgrade(opCtx);
    }

    // TODO SERVER-116437 Remove once 9.0 becomes last lts.
    void _removeShardStateFromConfigCollection(OperationContext* opCtx) {
        // Prevent concurrent add/remove shard operations.
        Lock::ExclusiveLock shardMembershipLock =
            ShardingCatalogManager::get(opCtx)->acquireShardMembershipLockForTopologyChange(opCtx);

        LOGV2(11354300, "Updating 'config.shards' entries to remove 'state' field");

        write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigsvrShardsNamespace);
        updateOp.setUpdates({[&] {
            const auto filter = BSON(ShardType::state << BSON("$exists" << true));
            const auto update = BSON("$unset" << BSON(ShardType::state << ""));
            write_ops::UpdateOpEntry entry;
            entry.setQ(filter);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(false);
            // We want to remove the 'state' field from all entries.
            entry.setMulti(true);
            return entry;
        }()});
        updateOp.setWriteConcern(defaultMajorityWriteConcern());

        DBDirectClient client(opCtx);
        const auto result = client.update(updateOp);
        write_ops::checkWriteErrors(result);

        LOGV2(11354301,
              "Update of 'config.shards' entries succeeded",
              "updateResponse"_attr = result.toBSON());
    }

    // TODO(SERVER-100328): remove after 9.0 is branched.
    // WARNING: do not rely on this method to clean up index metadata that can be created
    // concurrently. It is fine to rely on this only when missing concurrently created collections
    // is fine, when newly created collections no longer use the metadata format we wish to remove.
    void _cleanUpIndexCatalogMetadataOnUpgrade(OperationContext* opCtx) {
        // We bypass the UserWritesBlock mode here in order to not see errors arising from the
        // block. The user already has permission to run FCV at this point and the writes performed
        // here aren't modifying any user data with the exception of fixing up the collection
        // metadata.
        auto originalValue = WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled();
        ON_BLOCK_EXIT([&] { WriteBlockBypass::get(opCtx).set(originalValue); });
        WriteBlockBypass::get(opCtx).set(true);

        catalog::forEachCollectionFromAllDbs(opCtx, MODE_X, [&](const Collection* collection) {
            // Issue a no-op collMod command to each collection to trigger removal of
            // deprecated catalog metadata and to correct any invalid value types previously
            // allowed in metadata.
            BSONObjBuilder responseBuilder;
            uassertStatusOK(processCollModCommand(
                opCtx, collection->ns(), CollMod{collection->ns()}, nullptr, &responseBuilder));
            return true;
        });
    }

    void finalizeUpgrade(OperationContext* opCtx, FCV requestedVersion) final {
        auto role = ShardingState::get(opCtx)->pollClusterRole();

        const bool isConfigsvr = role && role->has(ClusterRole::ConfigServer);
        // Convert viewful timeseries collections to viewless. We do this after the feature flag is
        // enabled, so no new viewful timeseries collections can be created during the conversion.
        // Note that while the conversion is ongoing, CRUD and DDL operations are supported on both
        // the viewful and viewless timeseries collection formats.
        if (gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledOnVersion(requestedVersion)) {
            if (isConfigsvr) {
                timeseries::upgradeDowngradeViewlessTimeseriesInShardedCluster(opCtx,
                                                                               true /*isUpgrade*/);
            } else if (!role) {
                timeseries::upgradeAllTimeseriesToViewless(opCtx);
            }
        }

        // TODO (SERVER-100309): Remove once 9.0 becomes last lts.
        if (isConfigsvr &&
            feature_flags::gSessionsCollectionCoordinatorOnConfigServer.isEnabledOnVersion(
                requestedVersion)) {
            _createConfigSessionsCollectionLocally(opCtx);
        }

        // The content of config.placementHistory needs to be recomputed after ensuring that all
        // shards (including a possible embedded config server) reached the kComplete FCV phase, so
        // that the routine has to be invoked here (rather than embedding it within
        // _upgradeServerMetadata()).
        // Such a choice takes also into account the possibility that a series of config server
        // stepdown events causes the cluster to reach the "FCV upgraded" state without never
        // executing _resetPlacementHistory(): if this occurs, change stream readers will still have
        // the capability of detecting the issue at targeting time and lazily remediate it.
        // TODO (SERVER-98118): Remove once v9.0 become last-lts.
        if (isConfigsvr) {
            _resetPlacementHistory(opCtx, requestedVersion);
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
            service.createQueryShapeRepresentativeQueriesCollection(opCtx);
            service
                .migrateRepresentativeQueriesFromQuerySettingsClusterParameterToDedicatedCollection(
                    opCtx);
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

    // TODO (SERVER-98118): Remove once v9.0 become last-lts.
    void _resetPlacementHistory(OperationContext* opCtx, const FCV requestedVersion) {
        // TODO (SERVER-108188): Avoid resetting config.placementHistory if its initialization
        // metadata already bring the expected version.
        if (!feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting.isEnabledOnVersion(
                requestedVersion)) {
            return;
        }

        ConfigsvrResetPlacementHistory configsvrRequest;
        configsvrRequest.setDbName(DatabaseName::kAdmin);
        configsvrRequest.setWriteConcern(defaultMajorityWriteConcern());

        const auto& configShard = ShardingCatalogManager::get(opCtx)->localConfigShard();
        const auto response =
            configShard->runCommand(opCtx,
                                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                    DatabaseName::kAdmin,
                                    configsvrRequest.toBSON(),
                                    Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
    }


    void prepareToDowngradeActions(OperationContext* opCtx,
                                   FCV originalVersion,
                                   FCV requestedVersion) final {

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

    void userCollectionsUassertsForDowngrade(OperationContext* opCtx,
                                             FCV originalVersion,
                                             FCV requestedVersion) final {

        bool errorAndLogValidationDisabled =
            (gFeatureFlagErrorAndLogValidationAction.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion));
        bool constraintValidationLevelDisabled =
            (gFeatureFlagConstraintValidationLevel.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion));
        bool storageTierDisabled =
            gFeatureFlagCreateSupportsStorageTierOptions
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion);
        if (errorAndLogValidationDisabled || constraintValidationLevelDisabled ||
            storageTierDisabled) {
            catalog::forEachCollectionFromAllDbs(
                opCtx, MODE_IS, [&](const Collection* collection) -> bool {
                    if (errorAndLogValidationDisabled) {
                        uassert(ErrorCodes::CannotDowngrade,
                                fmt::format(
                                    "Cannot downgrade the cluster when there are collections with "
                                    "'errorAndLog' validation action. Please unset the option or "
                                    "drop the collection(s) before downgrading. First detected "
                                    "collection with 'errorAndLog' enabled: {} (UUID: {}).",
                                    collection->ns().toStringForErrorMsg(),
                                    collection->uuid().toString()),
                                collection->getValidationAction() !=
                                    ValidationActionEnum::errorAndLog);
                    }

                    if (constraintValidationLevelDisabled) {
                        uassert(ErrorCodes::CannotDowngrade,
                                fmt::format(
                                    "Cannot downgrade the cluster when there are collections with "
                                    "'constraint' validation level. Please unset the option or "
                                    "drop the collection(s) before downgrading. First detected "
                                    "collection with 'constraint' enabled: {} (UUID: {}).",
                                    collection->ns().toStringForErrorMsg(),
                                    collection->uuid().toString()),
                                collection->getValidationLevel() !=
                                    ValidationLevelEnum::constraint);
                    }

                    if (storageTierDisabled) {
                        auto storageEngine = getGlobalServiceContext()->getStorageEngine();
                        auto storageTier = storageEngine->getStorageTierFromStorageOptions(
                            collection->getCollectionOptions().storageEngine);
                        uassert(
                            ErrorCodes::CannotDowngrade,
                            fmt::format("Cannot downgrade the cluster when there are collections "
                                        "with a cold storage tier. Please drop the collection(s) "
                                        "before downgrading. First detected collection: {} "
                                        "(UUID: {}).",
                                        collection->ns().toStringForErrorMsg(),
                                        collection->uuid().toString()),
                            !storageTier.has_value() ||
                                storageTier.value() != idlSerialize(StorageTierLevelEnum::cold));
                    }

                    return true;
                });
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
            catalog::forEachCollectionFromAllDbs(opCtx, MODE_IS, checkForStringSearchQueryType);
        }

        if (feature_flags::gFeatureFlagEnableReplicasetTransitionToCSRS
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            BSONObj shardIdentityBSON;
            if ([&] {
                    auto coll = acquireCollection(
                        opCtx,
                        CollectionAcquisitionRequest(
                            NamespaceString::kServerConfigurationNamespace,
                            PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
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

        if (repl::feature_flags::gFeatureFlagReplicationUsageOfPriorityPort
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            const auto& replConfig = repl::ReplicationCoordinator::get(opCtx)->getConfig();
            uassert(ErrorCodes::CannotDowngrade,
                    "Cannot downgrade when priority ports are present in the replSetConfig. "
                    "Please run ReplSetReconfig to remove priority ports prior to downgrade",
                    replConfig.getCountOfMembersWithPriorityPort() == 0);
        }

        // Check for v4 2dsphere indexes when downgrading below 8.3
        if (feature_flags::gFeatureFlag2dsphereIndexVersion4
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            static const std::string kIndexVersionFieldName("2dsphereIndexVersion");
            catalog::forEachCollectionFromAllDbs(
                opCtx, MODE_IS, [&](const Collection* collection) -> bool {
                    auto indexCatalog = collection->getIndexCatalog();
                    auto indexIterator =
                        indexCatalog->getIndexIterator(IndexCatalog::InclusionPolicy::kReady |
                                                       IndexCatalog::InclusionPolicy::kUnfinished);
                    while (indexIterator->more()) {
                        const IndexCatalogEntry* entry = indexIterator->next();
                        const IndexDescriptor* descriptor = entry->descriptor();
                        const BSONObj& infoObj = descriptor->infoObj();

                        // Check if this is a 2dsphere index
                        if (descriptor->getAccessMethodName() == IndexNames::GEO_2DSPHERE ||
                            descriptor->getAccessMethodName() == IndexNames::GEO_2DSPHERE_BUCKET) {
                            BSONElement versionElt = infoObj[kIndexVersionFieldName];
                            if (versionElt.isNumber() && versionElt.numberInt() == 4) {
                                uasserted(
                                    ErrorCodes::CannotDowngrade,
                                    fmt::format(
                                        "Cannot downgrade the cluster when there are 2dsphere "
                                        "indexes with version 4. Version 4 indexes require "
                                        "FCV 8.3 or higher. Please drop the index(es) before "
                                        "downgrading. First detected index: {} on collection "
                                        "{} (UUID: {}).",
                                        descriptor->indexName(),
                                        collection->ns().toStringForErrorMsg(),
                                        collection->uuid().toString()));
                            }
                        }
                    }
                    return true;
                });
        }
    }

    void internalServerCleanupForDowngrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        if (!role || role->has(ClusterRole::None) || role->has(ClusterRole::ShardServer)) {
            if (feature_flags::gTSBucketingParametersUnchanged
                    .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                                  originalVersion)) {
                catalog::modifyAllCollectionsMatching(
                    opCtx,
                    [&](const Collection* collection) {
                        // To remove timeseries bucketing parameters from persistent
                        // storage, issue the "collMod" command with none of the parameters
                        // set.
                        BSONObjBuilder responseBuilder;
                        uassertStatusOK(processCollModCommand(opCtx,
                                                              collection->ns(),
                                                              CollMod{collection->ns()},
                                                              nullptr,
                                                              &responseBuilder));
                    },
                    [&](const Collection* collection) {
                        // Only remove the catalog entry flag if it exists. It could've been
                        // removed if the downgrade process was interrupted and is being run
                        // again. The downgrade process cannot be aborted at this point.
                        return collection->getTimeseriesOptions() != boost::none &&
                            collection->timeseriesBucketingParametersHaveChanged();
                    });
            }

            maybeModifyDataOnDowngradeForTest(opCtx, requestedVersion, originalVersion);
        }

        if (!gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledOnVersion(requestedVersion)) {
            if (role && role->has(ClusterRole::ConfigServer)) {
                timeseries::upgradeDowngradeViewlessTimeseriesInShardedCluster(opCtx,
                                                                               false /*isUpgrade*/);
            } else if (!role) {
                timeseries::downgradeAllTimeseriesFromViewless(opCtx);
            }
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

    /*
     * Automatically modifies data on downgrade for testing. This is because in some cases,
     * the server expects the user to modify data themselves. In testing, as there may not
     * actually be a real user, we need to do it ourselves.
     */
    void maybeModifyDataOnDowngradeForTest(OperationContext* opCtx,
                                           const FCV requestedVersion,
                                           const FCV originalVersion) {
        // TODO (SERVER-117265): Revisit the need of this function.
        // We automatically strip the 'recordIdsReplicated' parameter from the collection
        // options when performing an FCV downgrade to a version that doesn't support replicated
        // recordIds.
        if (gFeatureFlagRecordIdsReplicated.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            LOGV2(8700500,
                  "Automatically issuing collMod to strip recordIdsReplicated:true field.");
            catalog::modifyAllCollectionsMatching(
                opCtx,
                [&](const Collection* collection) {
                    BSONObjBuilder responseBuilder;
                    auto collMod = CollMod{collection->ns()};
                    collMod.setRecordIdsReplicated(false);
                    uassertStatusOK(processCollModCommand(
                        opCtx, collection->ns(), collMod, nullptr, &responseBuilder));
                },
                [&](const Collection* collection) { return collection->areRecordIdsReplicated(); });
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
                    PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                    repl::ReadConcernArgs::get(opCtx),
                    AcquisitionPrerequisites::kRead});
            hasUserDocs = Helpers::findOne(opCtx, userColl, BSONObj(), userDoc);
        }

        {
            auto rolesColl = acquireCollectionMaybeLockFree(
                opCtx,
                CollectionAcquisitionRequest{
                    NamespaceString::kAdminRolesNamespace,
                    PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
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


    void finalizeDowngrade(OperationContext* opCtx, FCV requestedVersion) final {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const bool isConfigsvr = role && role->has(ClusterRole::ConfigServer);
        // TODO (SERVER-98118): remove once 9.0 becomes last LTS.
        if (isConfigsvr &&
            !feature_flags::gShardAuthoritativeDbMetadataDDL.isEnabledOnVersion(requestedVersion)) {
            // Dropping the authoritative collections (config.shard.catalog.databases) as the final
            // step of the downgrade ensures that no leftover data remains. This guarantees a clean
            // downgrade and makes it safe to upgrade again.
            dropAuthoritativeDatabaseCollectionOnShards(opCtx);
        }

        // TODO SERVER-94927: Remove once 9.0 becomes last lts.
        const bool isReplSet = !role.has_value();
        if ((isReplSet || isConfigsvr) &&
            !feature_flags::gFeatureFlagPQSBackfill.isEnabledOnVersion(requestedVersion)) {
            auto& service = query_settings::QuerySettingsService::get(opCtx);
            service
                .migrateRepresentativeQueriesFromDedicatedCollectionToQuerySettingsClusterParameter(
                    opCtx);
            service.dropQueryShapeRepresentativeQueriesCollection(opCtx);
        }
    }
};

namespace {

const auto _sampleDecoration = ServiceContext::declareDecoration<LegacyFCVStep>();

const FCVStepRegistry::Registerer<LegacyFCVStep> _LegacyFCVStepRegisterer("LegacyFCVStep");

}  // namespace

LegacyFCVStep* LegacyFCVStep::get(ServiceContext* serviceContext) {
    return &_sampleDecoration(serviceContext);
}

void dropAuthoritativeDatabaseCollectionOnShards(OperationContext* opCtx) {
    // No shards should be added until we have forwarded the command to all shards. We use the DDL
    // lock here to serialize with all of add shard and to avoid deadlocks with the DDL blocking
    // used by add/remove shard.
    DDLLockManager::ScopedCollectionDDLLock ddlLock(opCtx,
                                                    NamespaceString::kConfigsvrShardsNamespace,
                                                    "DropAuthoritativeDatabaseMetadata",
                                                    LockMode::MODE_S);

    const auto opTimeWithShards = Grid::get(opCtx)->catalogClient()->getAllShards(
        opCtx, repl::ReadConcernLevel::kSnapshotReadConcern);

    const auto& nss = NamespaceString::kConfigShardCatalogDatabasesNamespace;

    for (const auto& shardType : opTimeWithShards.value) {
        const auto shardStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardType.getName());
        if (shardStatus == ErrorCodes::ShardNotFound) {
            continue;
        }
        const auto shard = uassertStatusOK(shardStatus);

        // Build the listCollections command to find the collection's UUID.
        ListCollections listCollectionsCmd;
        listCollectionsCmd.setDbName(DatabaseName::kConfig);
        listCollectionsCmd.setFilter(BSON("name" << nss.coll()));

        const auto listCollRes = uassertStatusOK(
            shard->runExhaustiveCursorCommand(opCtx,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              nss.dbName(),
                                              listCollectionsCmd.toBSON(),
                                              Milliseconds(-1)));

        // If the collection doesn't exist, we're done.
        if (listCollRes.docs.empty()) {
            continue;
        }

        // Make noop write to be sure that we are the primary before sending the dropCollection.
        sharding_ddl_util::performNoopMajorityWriteLocally(opCtx);

        auto parsedResponse = ListCollectionsReplyItem::parse(listCollRes.docs[0]);

        // Build and run the drop command using the uuid found as replay protection.
        const auto uuid = parsedResponse.getInfo()->getUuid();
        tassert(10289900,
                "Expected uuid to be set for config.shard.catalog.databases collection",
                uuid.has_value());
        const auto dropCmd = BSON("drop" << nss.coll() << "collectionUUID" << *uuid
                                         << "writeConcern" << BSON("w" << "majority"));

        auto dropResponse =
            shard->runCommand(opCtx,
                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                              NamespaceString::kConfigShardCatalogDatabasesNamespace.dbName(),
                              dropCmd,
                              Shard::RetryPolicy::kIdempotent);

        auto status = Shard::CommandResponse::getEffectiveStatus(dropResponse);

        if (status == ErrorCodes::CollectionUUIDMismatch) {
            // Dropping a collection by UUID isn't idempotent. An old primary may have already
            // dropped it, so re-running the drop can trigger a CollectionUUIDMismatch. This can be
            // safely ignored since the collection is already gone.
            //
            // Another edge case: the collection might have been dropped and re-created with the
            // same name but a different UUID (e.g., during a split-brain scenario). We also ignore
            // this to avoid deleting valid metadata.
            continue;
        }

        uassertStatusOK(status);
    }
}


}  // namespace mongo

