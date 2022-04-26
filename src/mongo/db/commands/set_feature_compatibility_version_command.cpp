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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <fmt/format.h>

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
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_donor_service.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/s/config/configsvr_coordinator_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/vector_clock.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/pm2423_feature_flags_gen.h"
#include "mongo/s/pm2583_feature_flags_gen.h"
#include "mongo/s/refine_collection_shard_key_coordinator_feature_flags_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/version/releases.h"

using namespace fmt::literals;

namespace mongo {
namespace {

using GenericFCV = multiversion::GenericFCV;

MONGO_FAIL_POINT_DEFINE(failUpgrading);
MONGO_FAIL_POINT_DEFINE(hangWhileUpgrading);
MONGO_FAIL_POINT_DEFINE(failDowngrading);
MONGO_FAIL_POINT_DEFINE(hangWhileDowngrading);
MONGO_FAIL_POINT_DEFINE(hangBeforeUpdatingFcvDoc);
MONGO_FAIL_POINT_DEFINE(hangBeforeDrainingMigrations);

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

void waitForCurrentConfigCommitment(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // Skip the waiting if the current config is from a force reconfig.
    auto oplogWait = replCoord->getConfigTerm() != repl::OpTime::kUninitializedTerm;
    auto status = replCoord->awaitConfigCommitment(opCtx, oplogWait);
    status.addContext("New feature compatibility version is rejected");
    if (status == ErrorCodes::MaxTimeMSExpired) {
        // Convert the error code to be more specific.
        uasserted(ErrorCodes::CurrentConfigNotCommittedYet, status.reason());
    }
    uassertStatusOK(status);
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

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(),
                ActionType::setFeatureCompatibilityVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // Always wait for at least majority writeConcern to ensure all writes involved in the
        // upgrade process cannot be rolled back. There is currently no mechanism to specify a
        // default writeConcern, so we manually call waitForWriteConcern upon exiting this command.
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

        // Ensure that this operation will be killed by the RstlKillOpThread during step-up or
        // stepdown.
        opCtx->setAlwaysInterruptAtStepDownOrUp();

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        Lock::ExclusiveLock setFCVCommandLock(opCtx->lockState(), commandMutex);

        auto request = SetFeatureCompatibilityVersion::parse(
            IDLParserErrorContext("setFeatureCompatibilityVersion"), cmdObj);
        const auto requestedVersion = request.getCommandParameter();
        const auto actualVersion = serverGlobalParams.featureCompatibility.getVersion();
        if (request.getDowngradeOnDiskChanges()) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream()
                        << "Cannot set featureCompatibilityVersion to "
                        << FeatureCompatibilityVersionParser::serializeVersion(requestedVersion)
                        << " with '"
                        << SetFeatureCompatibilityVersion::kDowngradeOnDiskChangesFieldName
                        << "' set to true. This is only allowed when downgrading to "
                        << multiversion::toString(GenericFCV::kLastContinuous),
                    requestedVersion <= actualVersion &&
                        requestedVersion == GenericFCV::kLastContinuous);
        }

        if (requestedVersion == actualVersion) {
            // Set the client's last opTime to the system last opTime so no-ops wait for
            // writeConcern.
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            return true;
        }

        boost::optional<Timestamp> changeTimestamp;
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // The Config Server creates a new ID (i.e., timestamp) when it receives an upgrade or
            // downgrade request. Alternatively, the request refers to a previously aborted
            // operation for which the local FCV document must contain the ID to be reused.
            if (!serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
                const auto now = VectorClock::get(opCtx)->getTime();
                changeTimestamp = now.clusterTime().asTimestamp();
            } else {
                auto fcvObj =
                    FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(opCtx);
                auto fcvDoc = FeatureCompatibilityVersionDocument::parse(
                    IDLParserErrorContext("featureCompatibilityVersionDocument"), fcvObj.get());
                changeTimestamp = fcvDoc.getChangeTimestamp();
                uassert(5722800,
                        "The 'changeTimestamp' field is missing in the FCV document persisted by "
                        "the Config Server. This may indicate that this document has been "
                        "explicitly amended causing an internal data inconsistency.",
                        changeTimestamp);
            }
        } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
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

        FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
            opCtx, request, actualVersion);

        uassert(5563600,
                "'phase' field is only valid to be specified on shards",
                !request.getPhase() || serverGlobalParams.clusterRole == ClusterRole::ShardServer);

        auto isFromConfigServer = request.getFromConfigServer().value_or(false);

        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kStart) {
            {
                boost::optional<MigrationBlockingGuard> drainNewMoveChunks;

                // Drain moveChunks if the actualVersion relies on the new migration protocol but
                // the requestedVersion uses the old one (downgrading).
                if ((feature_flags::gFeatureFlagMigrationRecipientCriticalSection
                         .isEnabledOnVersion(actualVersion) &&
                     !feature_flags::gFeatureFlagMigrationRecipientCriticalSection
                          .isEnabledOnVersion(requestedVersion)) ||
                    (feature_flags::gFeatureFlagNewPersistedChunkVersionFormat.isEnabledOnVersion(
                         actualVersion) &&
                     !feature_flags::gFeatureFlagNewPersistedChunkVersionFormat.isEnabledOnVersion(
                         requestedVersion))) {
                    drainNewMoveChunks.emplace(opCtx, "setFeatureCompatibilityVersionDowngrade");

                    // At this point, because we are holding the MigrationBlockingGuard, no new
                    // migrations can start and there are no active ongoing ones. Still, there could
                    // be migrations pending recovery. Drain them.
                    migrationutil::drainMigrationsPendingRecovery(opCtx);
                }

                // Start transition to 'requestedVersion' by updating the local FCV document to a
                // 'kUpgrading' or 'kDowngrading' state, respectively.
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

                if (!gFeatureFlagUserWriteBlocking.isEnabledOnVersion(requestedVersion)) {
                    // TODO SERVER-65010 Remove this scope once 6.0 has branched out

                    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                        uassert(
                            ErrorCodes::CannotDowngrade,
                            "Cannot downgrade while user write blocking is being changed",
                            ConfigsvrCoordinatorService::getService(opCtx)
                                ->areAllCoordinatorsOfTypeFinished(
                                    opCtx, ConfigsvrCoordinatorTypeEnum::kSetUserWriteBlockMode));
                    }

                    DBDirectClient client(opCtx);

                    const bool isBlockingUserWrites =
                        client.count(NamespaceString::kUserWritesCriticalSectionsNamespace) != 0;

                    uassert(ErrorCodes::CannotDowngrade,
                            "Cannot downgrade while user write blocking is enabled.",
                            !isBlockingUserWrites);
                }

                // TODO (SERVER-65572): Remove setClusterParameter serialization and collection
                // drop after this is backported to 6.0.
                if (!gFeatureFlagClusterWideConfig.isEnabledOnVersion(requestedVersion)) {
                    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                        uassert(ErrorCodes::CannotDowngrade,
                                "Cannot downgrade while cluster server parameter is being set",
                                ConfigsvrCoordinatorService::getService(opCtx)
                                    ->areAllCoordinatorsOfTypeFinished(
                                        opCtx, ConfigsvrCoordinatorTypeEnum::kSetClusterParameter));
                    }

                    DropReply dropReply;
                    const auto dropStatus = dropCollection(
                        opCtx,
                        NamespaceString::kClusterParametersNamespace,
                        &dropReply,
                        DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
                    uassert(
                        dropStatus.code(),
                        str::stream() << "Failed to drop the cluster server parameters collection"
                                      << causedBy(dropStatus.reason()),
                        dropStatus.isOK() || dropStatus.code() == ErrorCodes::NamespaceNotFound);
                }

                FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                    opCtx,
                    actualVersion,
                    requestedVersion,
                    isFromConfigServer,
                    changeTimestamp,
                    true /* setTargetVersion */);
            }

            if (request.getPhase() == SetFCVPhaseEnum::kStart) {
                invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
                if (actualVersion > requestedVersion &&
                    !feature_flags::gOrphanTracking.isEnabledOnVersion(requestedVersion)) {
                    BalancerStatsRegistry::get(opCtx)->terminate();
                    ScopedRangeDeleterLock rangeDeleterLock(opCtx);
                    clearOrphanCountersFromRangeDeletionTasks(opCtx);
                }

                // TODO (SERVER-62325): Remove collMod draining mechanism after 6.0 branching.
                if (actualVersion > requestedVersion &&
                    requestedVersion < multiversion::FeatureCompatibilityVersion::kVersion_6_0) {
                    // No more collMod coordinators will start because we have already switched
                    // the FCV value to kDowngrading. Wait for the ongoing collMod coordinators to
                    // finish.
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kCollMod);
                }

                // TODO SERVER-62850 Remove when 6.0 branches-out
                if (actualVersion > requestedVersion &&
                    !feature_flags::gFeatureFlagRecoverableRefineCollectionShardKeyCoordinator
                         .isEnabledOnVersion(requestedVersion)) {
                    // No more (recoverable) ReshardCollectionCoordinators will start because we
                    // have already switched the FCV value to kDowngrading. Wait for the ongoing
                    // RefineCollectionCoordinators to finish.
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kRefineCollectionShardKey);
                }

                // TODO SERVER-65077: Remove FCV check once 6.0 is released
                if (actualVersion > requestedVersion &&
                    !gFeatureFlagFLE2.isEnabledOnVersion(requestedVersion)) {
                    // No more (recoverable) CompactStructuredEncryptionDataCoordinator will start
                    // because we have already switched the FCV value to kDowngrading. Wait for the
                    // ongoing CompactStructuredEncryptionDataCoordinator to finish.
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kCompactStructuredEncryptionData);
                }

                // If we are only running phase-1, then we are done
                return true;
            }
        }

        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());
        invariant(!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kComplete);

        if (requestedVersion > actualVersion) {
            _runUpgrade(opCtx, request, changeTimestamp);
        } else {
            _runDowngrade(opCtx, request, changeTimestamp);
        }

        hangBeforeDrainingMigrations.pauseWhileSet();
        {
            boost::optional<MigrationBlockingGuard> drainOldMoveChunks;

            bool orphanTrackingCondition =
                serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
                !feature_flags::gOrphanTracking.isEnabledOnVersion(actualVersion) &&
                feature_flags::gOrphanTracking.isEnabledOnVersion(requestedVersion);
            // Drain moveChunks if the actualVersion relies on the old migration protocol but the
            // requestedVersion uses the new one (upgrading), we're persisting the new chunk
            // version format, or we are adding the numOrphans field to range deletion documents.
            if ((!feature_flags::gFeatureFlagMigrationRecipientCriticalSection.isEnabledOnVersion(
                     actualVersion) &&
                 feature_flags::gFeatureFlagMigrationRecipientCriticalSection.isEnabledOnVersion(
                     requestedVersion)) ||
                (!feature_flags::gFeatureFlagNewPersistedChunkVersionFormat.isEnabledOnVersion(
                     actualVersion) &&
                 feature_flags::gFeatureFlagNewPersistedChunkVersionFormat.isEnabledOnVersion(
                     requestedVersion)) ||
                orphanTrackingCondition) {
                drainOldMoveChunks.emplace(opCtx, "setFeatureCompatibilityVersionUpgrade");

                // At this point, because we are holding the MigrationBlockingGuard, no new
                // migrations can start and there are no active ongoing ones. Still, there could
                // be migrations pending recovery. Drain them.
                migrationutil::drainMigrationsPendingRecovery(opCtx);

                if (orphanTrackingCondition) {
                    setOrphanCountersOnRangeDeletionTasks(opCtx);
                    BalancerStatsRegistry::get(opCtx)->initializeAsync(opCtx);
                }
            }

            // Complete transition by updating the local FCV document to the fully upgraded or
            // downgraded requestedVersion.
            const auto fcvChangeRegion(FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

            hangBeforeUpdatingFcvDoc.pauseWhileSet();

            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx,
                serverGlobalParams.featureCompatibility.getVersion(),
                requestedVersion,
                isFromConfigServer,
                changeTimestamp,
                false /* setTargetVersion */);
        }

        return true;
    }

private:
    void _runUpgrade(OperationContext* opCtx,
                     const SetFeatureCompatibilityVersion& request,
                     boost::optional<Timestamp> changeTimestamp) {
        const auto requestedVersion = request.getCommandParameter();

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // Tell the shards to enter phase-1 of setFCV
            auto requestPhase1 = request;
            requestPhase1.setFromConfigServer(true);
            requestPhase1.setPhase(SetFCVPhaseEnum::kStart);
            requestPhase1.setChangeTimestamp(changeTimestamp);
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                    opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase1.toBSON({}))));
        }

        _cancelTenantMigrations(opCtx);

        {
            // Take the global lock in S mode to create a barrier for operations taking the global
            // IX or X locks. This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::GlobalLock lk(opCtx, MODE_S);
        }

        // (Generic FCV reference): TODO SERVER-60912: When kLastLTS is 6.0, remove this FCV-gated
        // upgrade code.
        if (requestedVersion == multiversion::GenericFCV::kLatest) {
            for (const auto& tenantDbName : DatabaseHolder::get(opCtx)->getNames()) {
                const auto& dbName = tenantDbName.dbName();
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    tenantDbName,
                    MODE_X,
                    [&](const CollectionPtr& collection) {
                        if (collection->getTimeseriesBucketsMayHaveMixedSchemaData()) {
                            // The catalog entry flag has already been added. This can happen if the
                            // upgrade process was interrupted and is being run again, or if there
                            // was a time-series collection created during the upgrade. The upgrade
                            // process cannot be aborted at this point.
                            return true;
                        }

                        NamespaceStringOrUUID nsOrUUID(dbName, collection->uuid());
                        CollMod collModCmd(collection->ns());
                        BSONObjBuilder unusedBuilder;
                        Status status =
                            processCollModCommand(opCtx, nsOrUUID, collModCmd, &unusedBuilder);

                        if (!status.isOK()) {
                            LOGV2_FATAL(
                                6057503,
                                "Failed to add catalog entry during upgrade",
                                "error"_attr = status,
                                "timeseriesBucketsMayHaveMixedSchemaData"_attr =
                                    collection->getTimeseriesBucketsMayHaveMixedSchemaData(),
                                logAttrs(collection->ns()),
                                logAttrs(collection->uuid()));
                        }

                        return true;
                    },
                    [&](const CollectionPtr& collection) {
                        return collection->getTimeseriesOptions() != boost::none;
                    });
            }
        }

        uassert(ErrorCodes::Error(549180),
                "Failing upgrade due to 'failUpgrading' failpoint set",
                !failUpgrading.shouldFail());

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // Always abort the reshardCollection regardless of version to ensure that it will run
            // on a consistent version from start to finish. This will ensure that it will be able
            // to apply the oplog entries correctly.
            abortAllReshardCollection(opCtx);

            // Tell the shards to enter phase-2 of setFCV (fully upgraded)
            auto requestPhase2 = request;
            requestPhase2.setFromConfigServer(true);
            requestPhase2.setPhase(SetFCVPhaseEnum::kComplete);
            requestPhase2.setChangeTimestamp(changeTimestamp);
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                    opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase2.toBSON({}))));
        }

        // Create the pre-images collection if the feature flag is enabled on the requested version.
        // TODO SERVER-61770: Remove once FCV 6.0 becomes last-lts.
        if (feature_flags::gFeatureFlagChangeStreamPreAndPostImages.isEnabledOnVersion(
                requestedVersion)) {
            createChangeStreamPreImagesCollection(opCtx);
        }

        hangWhileUpgrading.pauseWhileSet(opCtx);
    }

    void _runDowngrade(OperationContext* opCtx,
                       const SetFeatureCompatibilityVersion& request,
                       boost::optional<Timestamp> changeTimestamp) {
        const auto requestedVersion = request.getCommandParameter();
        const bool preImagesFeatureFlagDisabledOnDowngradeVersion =
            !feature_flags::gFeatureFlagChangeStreamPreAndPostImages.isEnabledOnVersion(
                requestedVersion);

        // TODO SERVER-62693: remove the following scope once 6.0 branches out
        if (requestedVersion == multiversion::GenericFCV::kLastLTS) {
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer ||
                serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
                sharding_util::downgradeCollectionBalancingFieldsToPre53(opCtx);
            }
        }

        // TODO  SERVER-65332 remove logic bound to this future object When kLastLTS is 6.0
        boost::optional<SharedSemiFuture<void>> chunkResizeAsyncTask;
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // Tell the shards to enter phase-1 of setFCV
            auto requestPhase1 = request;
            requestPhase1.setFromConfigServer(true);
            requestPhase1.setPhase(SetFCVPhaseEnum::kStart);
            requestPhase1.setChangeTimestamp(changeTimestamp);
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                    opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase1.toBSON({}))));

            chunkResizeAsyncTask =
                Balancer::get(opCtx)->applyLegacyChunkSizeConstraintsOnClusterData(opCtx);
        }

        _cancelTenantMigrations(opCtx);

        {
            // Take the global lock in S mode to create a barrier for operations taking the global
            // IX or X locks. This ensures that either
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     downgrading to the last-lts or last-continuous FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before downgrade procedures begin right after this.
            Lock::GlobalLock lk(opCtx, MODE_S);
        }

        // (Generic FCV reference): TODO SERVER-60912: When kLastLTS is 6.0, remove this FCV-gated
        // downgrade code.
        if (requestedVersion == multiversion::GenericFCV::kLastLTS) {
            for (const auto& tenantDbName : DatabaseHolder::get(opCtx)->getNames()) {
                const auto& dbName = tenantDbName.dbName();
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    tenantDbName,
                    MODE_X,
                    [&](const CollectionPtr& collection) {
                        invariant(collection->getTimeseriesOptions());

                        auto indexCatalog = collection->getIndexCatalog();
                        auto indexIt = indexCatalog->getIndexIterator(
                            opCtx, /*includeUnfinishedIndexes=*/true);

                        while (indexIt->more()) {
                            auto indexEntry = indexIt->next();
                            // Secondary indexes on time-series measurements are only supported
                            // in 5.2 and up. If the user tries to downgrade the cluster to an
                            // earlier version, they must first remove all incompatible secondary
                            // indexes on time-series measurements.
                            uassert(
                                ErrorCodes::CannotDowngrade,
                                str::stream()
                                    << "Cannot downgrade the cluster when there are secondary "
                                       "indexes on time-series measurements present, or when there "
                                       "are partial indexes on a time-series collection. Drop all "
                                       "secondary indexes on time-series measurements, and all "
                                       "partial indexes on time-series collections, before "
                                       "downgrading. First detected incompatible index name: '"
                                    << indexEntry->descriptor()->indexName() << "' on collection: '"
                                    << collection->ns().getTimeseriesViewNamespace() << "'",
                                timeseries::isBucketsIndexSpecCompatibleForDowngrade(
                                    *collection->getTimeseriesOptions(),
                                    indexEntry->descriptor()->infoObj()));

                            if (auto filter = indexEntry->getFilterExpression()) {
                                auto status = IndexCatalogImpl::checkValidFilterExpressions(
                                    filter,
                                    /*timeseriesMetricIndexesFeatureFlagEnabled*/ false);
                                uassert(ErrorCodes::CannotDowngrade,
                                        str::stream()
                                            << "Cannot downgrade the cluster when there are "
                                               "secondary indexes with partial filter expressions "
                                               "that contain $in/$or/$geoWithin or an $and that is "
                                               "not top level. Drop all indexes containing these "
                                               "partial filter elements before downgrading. First "
                                               "detected incompatible index name: '"
                                            << indexEntry->descriptor()->indexName()
                                            << "' on collection: '"
                                            << collection->ns().getTimeseriesViewNamespace() << "'",
                                        status.isOK());
                            }
                        }

                        if (!collection->getTimeseriesBucketsMayHaveMixedSchemaData()) {
                            // The catalog entry flag has already been removed. This can happen if
                            // the downgrade process was interrupted and is being run again. The
                            // downgrade process cannot be aborted at this point.
                            return true;
                        }
                        NamespaceStringOrUUID nsOrUUID(dbName, collection->uuid());
                        CollMod collModCmd(collection->ns());
                        BSONObjBuilder unusedBuilder;
                        Status status =
                            processCollModCommand(opCtx, nsOrUUID, collModCmd, &unusedBuilder);

                        if (!status.isOK()) {
                            LOGV2_FATAL(
                                6057600,
                                "Failed to remove catalog entry during downgrade",
                                "error"_attr = status,
                                "timeseriesBucketsMayHaveMixedSchemaData"_attr =
                                    collection->getTimeseriesBucketsMayHaveMixedSchemaData(),
                                logAttrs(collection->ns()),
                                logAttrs(collection->uuid()));
                        }

                        return true;
                    },
                    [&](const CollectionPtr& collection) {
                        return collection->getTimeseriesOptions() != boost::none;
                    });
            }
        }

        if (serverGlobalParams.featureCompatibility
                .isFCVDowngradingOrAlreadyDowngradedFromLatest()) {
            for (const auto& tenantDbName : DatabaseHolder::get(opCtx)->getNames()) {
                const auto& dbName = tenantDbName.dbName();
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    tenantDbName,
                    MODE_X,
                    [&](const CollectionPtr& collection) {
                        // Fail to downgrade if there exists a collection with
                        // 'changeStreamPreAndPostImages' enabled.
                        // TODO SERVER-61770: Remove once FCV 6.0 becomes last-lts.
                        uassert(ErrorCodes::CannotDowngrade,
                                str::stream() << "Cannot downgrade the cluster as collection "
                                              << collection->ns()
                                              << " has 'changeStreamPreAndPostImages' enabled",
                                preImagesFeatureFlagDisabledOnDowngradeVersion &&
                                    !collection->isChangeStreamPreAndPostImagesEnabled());
                        return true;
                    },
                    [&](const CollectionPtr& collection) {
                        // TODO SERVER-61770: Remove 'changeStreamPreAndPostImages' check once
                        // FCV 6.0 becomes last-lts.
                        return preImagesFeatureFlagDisabledOnDowngradeVersion &&
                            collection->isChangeStreamPreAndPostImagesEnabled();
                    });
            }

            // TODO SERVER-63564: Remove once FCV 6.0 becomes last-lts.
            for (const auto& tenantDbName : DatabaseHolder::get(opCtx)->getNames()) {
                const auto& dbName = tenantDbName.dbName();
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx, tenantDbName, MODE_X, [&](const CollectionPtr& collection) {
                        auto indexCatalog = collection->getIndexCatalog();
                        auto indexIt = indexCatalog->getIndexIterator(
                            opCtx, true /* includeUnfinishedIndexes */);
                        while (indexIt->more()) {
                            auto indexEntry = indexIt->next();
                            uassert(
                                ErrorCodes::CannotDowngrade,
                                fmt::format(
                                    "Cannot downgrade the cluster when there are indexes that have "
                                    "the 'prepareUnique' field. Use listIndexes to find "
                                    "them and drop "
                                    "the indexes or use collMod to manually set it to false to "
                                    "remove the field "
                                    "before downgrading. First detected incompatible index name: "
                                    "'{}' on collection: '{}'",
                                    indexEntry->descriptor()->indexName(),
                                    collection->ns().toString()),
                                !indexEntry->descriptor()->prepareUnique());
                        }
                        return true;
                    });
            }

            // Drop the pre-images collection if 'changeStreamPreAndPostImages' feature flag is not
            // enabled on the downgrade version.
            // TODO SERVER-61770: Remove once FCV 6.0 becomes last-lts.
            if (preImagesFeatureFlagDisabledOnDowngradeVersion) {
                DropReply dropReply;
                const auto deletionStatus =
                    dropCollection(opCtx,
                                   NamespaceString::kChangeStreamPreImagesNamespace,
                                   &dropReply,
                                   DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
                uassert(deletionStatus.code(),
                        str::stream() << "Failed to drop the change stream pre-images collection"
                                      << causedBy(deletionStatus.reason()),
                        deletionStatus.isOK() ||
                            deletionStatus.code() == ErrorCodes::NamespaceNotFound);
            }

            // Block downgrade for collections with encrypted fields
            // TODO SERVER-65077: Remove once FCV 6.0 becomes last-lts.
            for (const auto& tenantDbName : DatabaseHolder::get(opCtx)->getNames()) {
                const auto& dbName = tenantDbName.dbName();
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx, tenantDbName, MODE_X, [&](const CollectionPtr& collection) {
                        uassert(
                            ErrorCodes::CannotDowngrade,
                            str::stream() << "Cannot downgrade the cluster as collection "
                                          << collection->ns() << " has 'encryptedFields'",
                            !collection->getCollectionOptions().encryptedFieldConfig.has_value());
                        return true;
                    });
            }
        }

        {

            LOGV2(5876100, "Starting removal of internal sessions from config.transactions.");

            // Due to the possibility that the shell or drivers have implicit sessions enabled, we
            // cannot write to the config.transactions collection while we're in a session. So we
            // construct a temporary client to as a work around.
            auto newClient = opCtx->getServiceContext()->makeClient("InternalSessionsCleanup");

            {
                stdx::lock_guard<Client> lk(*newClient.get());
                newClient->setSystemOperationKillableByStepdown(lk);
            }

            AlternativeClientRegion acr(newClient);

            auto setFcvCancellationThreadPool([] {
                ThreadPool::Options options;
                options.poolName = "SetFcvDowngradeCancellableOpCtxPool";
                options.minThreads = 1;
                options.maxThreads = 1;

                auto threadPool = std::make_shared<ThreadPool>(std::move(options));
                threadPool->startup();
                return threadPool;
            }());

            CancelableOperationContextFactory factory(opCtx->getCancellationToken(),
                                                      setFcvCancellationThreadPool);

            // We use a CancelableOperationContext in order to stop cleanup if the original opCtx
            // has been interrupted.
            auto newOpCtxPtr = factory.makeOperationContext(&cc());
            auto newOpCtx = newOpCtxPtr.get();

            _cleanupInternalSessions(newOpCtx);

            LOGV2(5876101, "Completed removal of internal sessions from config.transactions.");
        }

        // TODO: SERVER-62375 Remove upgrade/downgrade code for internal transactions.
        // We want to wait for all of the transaction coordinator entries related to internal
        // transactions to be removed. This is because the corresponding coordinator document
        // contains has a special lsid which downgraded binaries cannot properly parse.
        if (serverGlobalParams.clusterRole != ClusterRole::None) {
            auto coordinatorService = TransactionCoordinatorService::get(opCtx);
            for (const auto& future :
                 coordinatorService->getAllRemovalFuturesForCoordinatorsForInternalTransactions(
                     opCtx)) {
                auto status = future.getNoThrow(opCtx);
                uassertStatusOKWithContext(status,
                                           str::stream()
                                               << "Unable to remove all "
                                               << NamespaceString::kTransactionCoordinatorsNamespace
                                               << " documents for internal transactions");
            }
        }

        // TODO SERVER-64720 Remove when 6.0 becomes last LTS
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kCreateCollection);
        }

        // TODO SERVER-62338 Remove when 6.0 branches-out
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
            !resharding::gFeatureFlagRecoverableShardsvrReshardCollectionCoordinator
                 .isEnabledOnVersion(requestedVersion)) {
            // No more (recoverable) ReshardCollectionCoordinators will start because we
            // have already switched the FCV value to kDowngrading. Wait for the ongoing
            // ReshardCollectionCoordinators to finish. The fact that the the configsvr has already
            // executed 'abortAllReshardCollection' after switching to kDowngrading FCV ensures that
            // ReshardCollectionCoordinators will finish (Interrupted) promptly.
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kReshardCollection);
        }

        uassert(ErrorCodes::Error(549181),
                "Failing downgrade due to 'failDowngrading' failpoint set",
                !failDowngrading.shouldFail());

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // Always abort the reshardCollection regardless of version to ensure that it will run
            // on a consistent version from start to finish. This will ensure that it will be able
            // to apply the oplog entries correctly.
            abortAllReshardCollection(opCtx);

            // Tell the shards to enter phase-2 of setFCV (fully downgraded)
            auto requestPhase2 = request;
            requestPhase2.setFromConfigServer(true);
            requestPhase2.setPhase(SetFCVPhaseEnum::kComplete);
            requestPhase2.setChangeTimestamp(changeTimestamp);
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                    opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase2.toBSON({}))));
        }

        if (chunkResizeAsyncTask.has_value()) {
            LOGV2(6417108, "Waiting for cluster chunks resize process to complete.");
            uassertStatusOKWithContext(
                chunkResizeAsyncTask->getNoThrow(opCtx),
                "Failed to enforce chunk size constraint during FCV downgrade");
            LOGV2(6417109, "Cluster chunks resize process completed.");
        }
        hangWhileDowngrading.pauseWhileSet(opCtx);

        if (request.getDowngradeOnDiskChanges()) {
            invariant(requestedVersion == GenericFCV::kLastContinuous);
            _downgradeOnDiskChanges();
            LOGV2(4875603, "Downgrade of on-disk format complete.");
        }
    }

    /**
     * Rolls back any upgraded on-disk changes to reflect the disk format of the last-continuous
     * version.
     */
    void _downgradeOnDiskChanges() {
        LOGV2(4975602,
              "Downgrading on-disk format to reflect the last-continuous version.",
              "last_continuous_version"_attr = multiversion::toString(GenericFCV::kLastContinuous));
    }

    /**
     * Kills all tenant migrations active on this node, for both donors and recipients.
     * Called after reaching an upgrading or downgrading state.
     */
    void _cancelTenantMigrations(OperationContext* opCtx) {
        invariant(serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());
        if (serverGlobalParams.clusterRole == ClusterRole::None) {
            auto donorService = checked_cast<TenantMigrationDonorService*>(
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName));
            donorService->abortAllMigrations(opCtx);
            auto recipientService = checked_cast<repl::TenantMigrationRecipientService*>(
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(repl::TenantMigrationRecipientService::
                                              kTenantMigrationRecipientServiceName));
            recipientService->abortAllMigrations(opCtx);
        }
    }

    /**
     * Removes all child sessions from the config.transactions collection and updates the parent
     * sessions to have the highest txnNumber of either itself or its child sessions.
     */
    void _cleanupInternalSessions(OperationContext* opCtx) {
        _updateSessionDocuments(opCtx, _constructParentLsidToTxnNumberMap(opCtx));
        _deleteChildSessionDocuments(opCtx);
    }

    /**
     * Constructs a map consisting of a mapping between the parent session and the highest
     * txnNumber of its child sessions.
     */
    LogicalSessionIdMap<TxnNumber> _constructParentLsidToTxnNumberMap(OperationContext* opCtx) {
        DBDirectClient client(opCtx);

        LogicalSessionIdMap<TxnNumber> parentLsidToTxnNum;
        FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
        findRequest.setFilter(BSON("parentLsid" << BSON("$exists" << true)));
        findRequest.setProjection(BSON("_id" << 1 << "parentLsid" << 1));
        auto cursor = client.find(std::move(findRequest));

        while (cursor->more()) {
            auto doc = cursor->next();
            auto lsid = LogicalSessionId::parse(
                IDLParserErrorContext("parse lsid for session document modification"),
                doc.getField("_id").Obj());
            auto parentLsid = LogicalSessionId::parse(
                IDLParserErrorContext("parse parentLsid for session document modification"),
                doc.getField("parentLsid").Obj());
            auto txnNum = lsid.getTxnNumber();
            if (auto it = parentLsidToTxnNum.find(parentLsid); it != parentLsidToTxnNum.end()) {
                it->second = std::max(*txnNum, it->second);
            } else {
                parentLsidToTxnNum[parentLsid] = *txnNum;
            }
        }

        return parentLsidToTxnNum;
    }

    /**
     * Update each parent session's txnNumber to the highest txnNumber seen for that session
     * (including child sessions). We do this to account for the case where a child session
     * ran a transaction with a higher txnNumber than the last recorded txnNumber for a
     * parent session. The parent session should know what the most recent txnNumber sent by
     * the driver is.
     */
    void _updateSessionDocuments(OperationContext* opCtx,
                                 const LogicalSessionIdMap<TxnNumber>& parentLsidToTxnNum) {
        DBDirectClient client(opCtx);
        write_ops::UpdateCommandRequest updateOp(
            NamespaceString::kSessionTransactionsTableNamespace);
        std::vector<write_ops::UpdateOpEntry> updates;
        for (const auto& [lsid, txnNumber] : parentLsidToTxnNum) {
            SessionTxnRecord modifiedDoc;
            bool parentSessionExists = false;
            FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
            findRequest.setFilter(BSON("_id" << lsid.toBSON()));
            auto cursor = client.find(std::move(findRequest));
            if ((parentSessionExists = cursor->more())) {
                modifiedDoc = SessionTxnRecord::parse(
                    IDLParserErrorContext("parse transaction document to modify"), cursor->next());

                // We do not want to override the transaction state of a parent session with a
                // greater txnNumber than that of its child sessions.
                if (modifiedDoc.getTxnNum() > txnNumber) {
                    continue;
                }
            }

            // Upsert a new transaction document for a parent session if it doesn't already
            // exist in the config.transactions collection.
            if (!parentSessionExists) {
                modifiedDoc.setSessionId(lsid);
            }

            modifiedDoc.setLastWriteDate(Date_t::now());
            modifiedDoc.setTxnNum(txnNumber);

            // We set this timestamp to ensure that retry attempts fail with
            // IncompleteTransactionHistory. This is to stop us from double applying an
            // operation.
            modifiedDoc.setLastWriteOpTime(repl::OpTime(Timestamp(1, 0), 1));

            write_ops::UpdateOpEntry updateEntry;
            updateEntry.setQ(BSON("_id" << lsid.toBSON()));
            updateEntry.setU(
                write_ops::UpdateModification::parseFromClassicUpdate(modifiedDoc.toBSON()));
            updateEntry.setUpsert(true);
            updates.push_back(updateEntry);

            if (updates.size() == write_ops::kMaxWriteBatchSize) {
                updateOp.setUpdates(updates);
                auto response = client.runCommand(updateOp.serialize({}));
                uassertStatusOK(getStatusFromWriteCommandReply(response->getCommandReply()));
                updates.clear();
            }
        }

        if (updates.size() > 0) {
            updateOp.setUpdates(updates);
            auto response = client.runCommand(updateOp.serialize({}));
            uassertStatusOK(getStatusFromWriteCommandReply(response->getCommandReply()));
        }
    }

    /**
     * Delete the remaining child sessions from the config.transactions collection.
     */
    void _deleteChildSessionDocuments(OperationContext* opCtx) {
        DBDirectClient client(opCtx);

        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::kSessionTransactionsTableNamespace);
        write_ops::DeleteOpEntry deleteEntry;
        deleteEntry.setQ(BSON("_id.txnUUID" << BSON("$exists" << true)));
        deleteEntry.setMulti(true);
        deleteOp.setDeletes({deleteEntry});

        auto response = client.runCommand(deleteOp.serialize({}));
        uassertStatusOK(getStatusFromWriteCommandReply(response->getCommandReply()));
    }
} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
