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
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
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
#include "mongo/db/repl/tenant_migration_donor_service.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
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
#include "mongo/s/catalog/type_index_catalog_gen.h"
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
MONGO_FAIL_POINT_DEFINE(failBeforeSendingShardsToDowngrading);
MONGO_FAIL_POINT_DEFINE(failDowngrading);
MONGO_FAIL_POINT_DEFINE(hangWhileDowngrading);
MONGO_FAIL_POINT_DEFINE(hangBeforeUpdatingFcvDoc);
MONGO_FAIL_POINT_DEFINE(failBeforeUpdatingFcvDoc);

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
             const DatabaseName&,
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
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        Lock::ExclusiveLock setFCVCommandLock(opCtx->lockState(), commandMutex);

        auto request = SetFeatureCompatibilityVersion::parse(
            IDLParserContext("setFeatureCompatibilityVersion"), cmdObj);
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

        const auto upgradeOrDowngrade = requestedVersion > actualVersion ? "upgrade" : "downgrade";
        const auto server_type = serverGlobalParams.clusterRole == ClusterRole::ConfigServer
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
                !request.getPhase() || serverGlobalParams.clusterRole == ClusterRole::ShardServer);

        auto isFromConfigServer = request.getFromConfigServer().value_or(false);

        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kStart) {
            {
                // Start transition to 'requestedVersion' by updating the local FCV document to a
                // 'kUpgrading' or 'kDowngrading' state, respectively.
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

                uassert(ErrorCodes::Error(6744303),
                        "Failing setFeatureCompatibilityVersion before reaching the FCV "
                        "transitional stage due to 'failBeforeTransitioning' failpoint set",
                        !failBeforeTransitioning.shouldFail());

                FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                    opCtx,
                    actualVersion,
                    requestedVersion,
                    isFromConfigServer,
                    changeTimestamp,
                    true /* setTargetVersion */);

                LOGV2(6744301,
                      "setFeatureCompatibilityVersion has set the FCV to the transitional state",
                      "upgradeOrDowngrade"_attr = upgradeOrDowngrade,
                      "serverType"_attr = server_type,
                      "fromVersion"_attr = actualVersion,
                      "toVersion"_attr = requestedVersion);
            }

            if (request.getPhase() == SetFCVPhaseEnum::kStart) {
                invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);

                // TODO SERVER-68008: Remove collMod draining mechanism after 7.0 becomes last LTS.
                if (actualVersion > requestedVersion &&
                    !feature_flags::gCollModCoordinatorV3.isEnabledOnVersion(requestedVersion)) {
                    // Drain all running collMod v3 coordinator because they produce backward
                    // incompatible on disk metadata
                    ShardingDDLCoordinatorService::getService(opCtx)
                        ->waitForCoordinatorsOfGivenTypeToComplete(
                            opCtx, DDLCoordinatorTypeEnum::kCollMod);
                }

                // TODO SERVER-68373 remove once 7.0 becomes last LTS
                if (actualVersion > requestedVersion) {
                    // Drain the QE compact coordinator because it persists state that is
                    // not backwards compatible with earlier versions.
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

        // If we are downgrading to a version that doesn't support implicit translation of
        // Timeseries collection in sharding DDL Coordinators we need to drain all ongoing
        // coordinators
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer &&
            !feature_flags::gImplicitDDLTimeseriesNssTranslation.isEnabledOnVersion(
                requestedVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)->waitForOngoingCoordinatorsToFinish(
                opCtx);
        }

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
                false /* setTargetVersion */);
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
    // This helper function is for any actions that should be done before taking the FCV full
    // transition lock in S mode. It is required that the code in this helper function is idempotent
    // and could be done after _runDowngrade even if it failed at any point in the middle of
    // _userCollectionsDowngradeCleanup or _internalServerDowngradeCleanup.
    void _prepareForUpgrade(OperationContext* opCtx) {
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            return;
        } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            return;
        } else {
            _cancelServerlessMigrations(opCtx);
        }
    }

    // This helper function is for any user collections uasserts, creations, or deletions that need
    // to happen during the upgrade. It is required that the code in this helper function is
    // idempotent and could be done after _runDowngrade even if it failed at any point in the middle
    // of _userCollectionsDowngradeCleanup or _internalServerDowngradeCleanup.
    void _userCollectionsUpgradeUassertsAndUpdates() {
        return;
    }

    // This helper function is for updating metadata to make sure the new features in the
    // upgraded version work for sharded and non-sharded clusters. It is required that the code
    // in this helper function is idempotent and could be done after _runDowngrade even if it
    // failed at any point in the middle of _userCollectionsDowngradeCleanup or
    // _internalServerDowngradeCleanup.
    void _completeUpgrade(OperationContext* opCtx,
                          const multiversion::FeatureCompatibilityVersion requestedVersion) {
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            _createGlobalIndexesIndexes(opCtx, requestedVersion);
        } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            _createGlobalIndexesIndexes(opCtx, requestedVersion);
        } else {
            return;
        }
    }

    void _createGlobalIndexesIndexes(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        // TODO SERVER-67392: Remove once FCV 7.0 becomes last-lts.
        if (feature_flags::gGlobalIndexesShardingCatalog.isEnabledOnVersion(requestedVersion)) {
            uassertStatusOK(sharding_util::createGlobalIndexesIndexes(opCtx));
            if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
                uassertStatusOK(sharding_util::createShardCollectionCatalogIndexes(opCtx));
            }
        }
    }

    void _runUpgrade(OperationContext* opCtx,
                     const SetFeatureCompatibilityVersion& request,
                     boost::optional<Timestamp> changeTimestamp) {
        const auto requestedVersion = request.getCommandParameter();

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // Tell the shards to enter phase-1 of setFCV
            _sendSetFCVRequestToShards(opCtx, request, changeTimestamp, SetFCVPhaseEnum::kStart);
        }

        // This helper function is for any actions that should be done before taking the FCV full
        // transition lock in S mode. It is required that the code in this helper function is
        // idempotent and could be done after _runDowngrade even if it failed at any point in the
        // middle of _userCollectionsDowngradeCleanup or _internalServerDowngradeCleanup.
        _prepareForUpgrade(opCtx);

        {
            // Take the FCV full transition lock in S mode to create a barrier for operations taking
            // the global IX or X locks, which implicitly take the FCV full transition lock in IX
            // mode (aside from those which explicitly opt out). This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::ResourceLock lk(
                opCtx, opCtx->lockState(), resourceIdFeatureCompatibilityVersion, MODE_S);
        }

        // This helper function is for any user collections uasserts, creations, or deletions that
        // need to happen during the upgrade. It is required that the code in this helper function
        // is idempotent and could be done after _runDowngrade even if it failed at any point in the
        // middle of _userCollectionsDowngradeCleanup or _internalServerDowngradeCleanup.
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
            serverGlobalParams.clusterRole == ClusterRole::None) {
            _userCollectionsUpgradeUassertsAndUpdates();
        }

        uassert(ErrorCodes::Error(549180),
                "Failing upgrade due to 'failUpgrading' failpoint set",
                !failUpgrading.shouldFail());

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {

            // Always abort the reshardCollection regardless of version to ensure that it will run
            // on a consistent version from start to finish. This will ensure that it will be able
            // to apply the oplog entries correctly.
            abortAllReshardCollection(opCtx);

            // TODO SERVER-68551: Remove once 7.0 becomes last-lts
            dropDistLockCollections(opCtx);

            // Tell the shards to enter phase-2 of setFCV (fully upgraded)
            _sendSetFCVRequestToShards(opCtx, request, changeTimestamp, SetFCVPhaseEnum::kComplete);
        }

        // This helper function is for updating metadata to make sure the new features in the
        // upgraded version work for sharded and non-sharded clusters. It is required that the code
        // in this helper function is idempotent and could be done after _runDowngrade even if it
        // failed at any point in the middle of _userCollectionsDowngradeCleanup or
        // _internalServerDowngradeCleanup.
        _completeUpgrade(opCtx, requestedVersion);

        hangWhileUpgrading.pauseWhileSet(opCtx);
    }

    // This helper function is for any actions that should be done before taking the FCV full
    // transition lock in S mode.
    void _prepareForDowngrade(OperationContext* opCtx) {
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            return;
        } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            return;
        } else {
            _cancelServerlessMigrations(opCtx);
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
    void _uassertUserDataAndSettingsReadyForDowngrade(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            return;
        } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
                   serverGlobalParams.clusterRole == ClusterRole::None) {
            _userCollectionsUasserts(opCtx, requestedVersion);
        }
    }

    void _userCollectionsUasserts(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        if (!feature_flags::gTimeseriesScalabilityImprovements.isEnabledOnVersion(
                requestedVersion)) {
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    dbName,
                    MODE_S,
                    [&](const CollectionPtr& collection) {
                        invariant(collection->getTimeseriesOptions());

                        auto indexCatalog = collection->getIndexCatalog();
                        auto indexIt = indexCatalog->getIndexIterator(
                            opCtx,
                            IndexCatalog::InclusionPolicy::kReady |
                                IndexCatalog::InclusionPolicy::kUnfinished);

                        while (indexIt->more()) {
                            auto indexEntry = indexIt->next();
                            // Fail to downgrade if the time-series collection has a partial, TTL
                            // index.
                            if (indexEntry->descriptor()->isPartial()) {
                                // TODO (SERVER-67659): Remove partial, TTL index check once FCV 7.0
                                // becomes last-lts.
                                uassert(
                                    ErrorCodes::CannotDowngrade,
                                    str::stream()
                                        << "Cannot downgrade the cluster when there are secondary "
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

                        return true;
                    },
                    [&](const CollectionPtr& collection) {
                        return collection->getTimeseriesOptions() != boost::none;
                    });
            }

            // Block downgrade for collections with encrypted fields
            // TODO SERVER-67760: Remove once FCV 7.0 becomes last-lts.
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx, dbName, MODE_X, [&](const CollectionPtr& collection) {
                        auto& efc = collection->getCollectionOptions().encryptedFieldConfig;

                        uassert(
                            ErrorCodes::CannotDowngrade,
                            str::stream()
                                << "Cannot downgrade the cluster as collection " << collection->ns()
                                << " has 'encryptedFields' with range indexes",
                            !(efc.has_value() && hasQueryType(efc.get(), QueryTypeEnum::Range)));
                        return true;
                    });
            }
        }

        if (!feature_flags::gfeatureFlagCappedCollectionsRelaxedSize.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
                catalog::forEachCollectionFromDb(
                    opCtx,
                    dbName,
                    MODE_S,
                    [&](const CollectionPtr& collection) {
                        uasserted(
                            ErrorCodes::CannotDowngrade,
                            str::stream()
                                << "Cannot downgrade the cluster when there are capped "
                                   "collection with a size that is non multiple of 256 bytes. "
                                   "Drop or resize the following collection: '"
                                << collection->ns() << "'");
                        return true;
                    },
                    [&](const CollectionPtr& collection) {
                        return collection->isCapped() && collection->getCappedMaxSize() % 256 != 0;
                    });
            }
        }
    }

    // This helper function is for any internal server downgrade cleanup, such as dropping
    // collections or aborting. This cleanup will happen after user collection downgrade
    // cleanup. The code in this helper function is required to be idempotent in case the node
    // crashes or downgrade fails in a way that the user has to run setFCV again. It also cannot
    // fail for a non-retryable reason since at this point user data has already been cleaned up.
    // This helper function can only fail with some transient error that can be retried (like
    // InterruptedDueToReplStateChange), ManualInterventionRequired, or fasserts. For any
    // non-retryable error in this helper function, it should error either with an uassert with
    // ManualInterventionRequired as the error code (indicating a server bug but that all the data
    // is consistent on disk and for reads/writes) or with an fassert (indicating a server bug and
    // that the data is corrupted). ManualInterventionRequired and fasserts are errors that are not
    // expected to occur in practice, but if they did, they would turn into a Support case.
    void _internalServerDowngradeCleanup(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            _dropInternalGlobalIndexesCollection(opCtx, requestedVersion);

            // Always abort the reshardCollection regardless of version to ensure that it will
            // run on a consistent version from start to finish. This will ensure that it will
            // be able to apply the oplog entries correctly.
            abortAllReshardCollection(opCtx);
        } else if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            _dropInternalGlobalIndexesCollection(opCtx, requestedVersion);
        } else {
            return;
        }
    }

    void _dropInternalGlobalIndexesCollection(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        // TODO SERVER-67392: Remove when 7.0 branches-out.
        // Coordinators that commits indexes to the csrs must be drained before this point. Older
        // FCV's must not find cluster-wide indexes.
        if (requestedVersion == GenericFCV::kLastLTS) {
            NamespaceString indexCatalogNss;
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                indexCatalogNss = NamespaceString::kConfigsvrIndexCatalogNamespace;
            } else {
                indexCatalogNss = NamespaceString::kShardIndexCatalogNamespace;
            }
            LOGV2(6280502, "Droping global indexes collection", "nss"_attr = indexCatalogNss);
            DropReply dropReply;
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

            if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
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
            } else {
                LOGV2(6711906,
                      "Unset index version field in config.collections",
                      "nss"_attr = CollectionType::ConfigNS);
                DBDirectClient client(opCtx);
                write_ops::UpdateCommandRequest update(CollectionType::ConfigNS);
                update.setUpdates({[&]() {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(
                        BSON(CollectionType::kIndexVersionFieldName << BSON("$exists" << true)));
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                        BSON("$unset" << BSON(CollectionType::kIndexVersionFieldName << true))));
                    entry.setMulti(true);
                    return entry;
                }()});
                client.update(update);
            }
        }
    }

    void _runDowngrade(OperationContext* opCtx,
                       const SetFeatureCompatibilityVersion& request,
                       boost::optional<Timestamp> changeTimestamp) {
        const auto requestedVersion = request.getCommandParameter();
        // TODO  SERVER-65332 remove logic bound to this future object when v7.0 branches out
        boost::optional<SharedSemiFuture<void>> chunkResizeAsyncTask;

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            uassert(ErrorCodes::Error(6794600),
                    "Failing downgrade due to 'failBeforeSendingShardsToDowngrading' failpoint set",
                    !failBeforeSendingShardsToDowngrading.shouldFail());
            // Tell the shards to enter phase-1 of setFCV
            _sendSetFCVRequestToShards(opCtx, request, changeTimestamp, SetFCVPhaseEnum::kStart);
        }

        // Any actions that should be done before taking the FCV full transition lock in S mode
        // should go in this function.
        _prepareForDowngrade(opCtx);

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
            requestedVersion == GenericFCV::kLastLTS) {
            // As data size aware balancing is supported starting from v6.1, chunks resizing is
            // required only when downgrading to v6.0
            chunkResizeAsyncTask =
                Balancer::get(opCtx)->applyLegacyChunkSizeConstraintsOnClusterData(opCtx);
        }

        {
            // Take the FCV full transition lock in S mode to create a barrier for operations taking
            // the global IX or X locks, which implicitly take the FCV full transition lock in IX
            // mode (aside from those which explicitly opt out). This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::ResourceLock lk(
                opCtx, opCtx->lockState(), resourceIdFeatureCompatibilityVersion, MODE_S);
        }

        uassert(ErrorCodes::Error(549181),
                "Failing downgrade due to 'failDowngrading' failpoint set",
                !failDowngrading.shouldFail());

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
        _uassertUserDataAndSettingsReadyForDowngrade(opCtx, requestedVersion);

        // This helper function is for any internal server downgrade cleanup, such as dropping
        // collections or aborting. This cleanup will happen after user collection downgrade
        // cleanup. The code in this helper function is required to be idempotent in case the node
        // crashes or downgrade fails in a way that the user has to run setFCV again. It also cannot
        // fail for a non-retryable reason since at this point user data has already been cleaned
        // up.
        // This helper function can only fail with some transient error that can be retried
        // (like InterruptedDueToReplStateChange), ManualInterventionRequired, or fasserts. For any
        // non-retryable error in this helper function, it should error either with an uassert with
        // ManualInterventionRequired as the error code (indicating a server bug but that all the
        // data is consistent on disk and for reads/writes) or with an fassert (indicating a server
        // bug and that the data is corrupted). ManualInterventionRequired and fasserts are errors
        // that are not expected to occur in practice, but if they did, they would turn into a
        // Support case.
        _internalServerDowngradeCleanup(opCtx, requestedVersion);

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // Tell the shards to enter phase-2 of setFCV (fully downgraded)
            _sendSetFCVRequestToShards(opCtx, request, changeTimestamp, SetFCVPhaseEnum::kComplete);

            if (requestedVersion == GenericFCV::kLastLTS) {
                // chunkResizeAsyncTask is only used by config servers as part of internal server
                // downgrade cleanup. Waiting for the task to complete is put at the end of
                // _runDowngrade instead of inside _internalServerDowngradeCleanup because the task
                // might take a long time to complete.
                invariant(chunkResizeAsyncTask.has_value());
                LOGV2(6417108, "Waiting for cluster chunks resize process to complete.");
                uassertStatusOKWithContext(
                    chunkResizeAsyncTask->getNoThrow(opCtx),
                    "Failed to enforce chunk size constraint during FCV downgrade");
                LOGV2(6417109, "Cluster chunks resize process completed.");
            }
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
     * Abort all serverless migrations active on this node, for both donors and recipients.
     * Called after reaching an upgrading or downgrading state.
     */
    void _cancelServerlessMigrations(OperationContext* opCtx) {
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

            if (getGlobalReplSettings().isServerless()) {
                auto splitDonorService = checked_cast<ShardSplitDonorService*>(
                    repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                        ->lookupServiceByName(ShardSplitDonorService::kServiceName));
                splitDonorService->abortAllSplits(opCtx);
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
        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // The Config Server always creates a new ID (i.e., timestamp) when it receives an
            // upgrade or downgrade request.
            const auto now = VectorClock::get(opCtx)->getTime();
            changeTimestamp = now.clusterTime().asTimestamp();
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
        return changeTimestamp;
    }

} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
