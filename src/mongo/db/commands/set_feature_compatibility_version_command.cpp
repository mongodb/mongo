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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/set_feature_compatibility_version_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/shard_merge_recipient_service.h"
#include "mongo/db/repl/tenant_migration_donor_service.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/s/config/configsvr_coordinator_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/shard_authoritative_catalog_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_index_catalog_ddl_util.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_collection_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"
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
MONGO_FAIL_POINT_DEFINE(failAfterReachingTransitioningState);
MONGO_FAIL_POINT_DEFINE(hangAtSetFCVStart);
MONGO_FAIL_POINT_DEFINE(failAfterSendingShardsToDowngradingOrUpgrading);
MONGO_FAIL_POINT_DEFINE(hangAfterBlockingIndexBuildsForFcvDowngrade);

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
        nsWithReshardColl.push_back(NamespaceStringUtil::serialize(
            doc.getSourceNss(), SerializationContext::stateDefault()));
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

// TODO SERVER-78330 remove this.
void deleteShardingStateRecoveryDoc(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    const auto commandResponse = client.runCommand([&] {
        write_ops::DeleteCommandRequest deleteOp(NamespaceString::kServerConfigurationNamespace);
        deleteOp.setDeletes(
            {[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON("_id"
                                << "minOpTimeRecovery"));
                entry.setMulti(false);
                return entry;
            }()});
        return deleteOp.serialize({});
    }());
    uassertStatusOK(getStatusFromWriteCommandReply(commandResponse->getCommandReply()));
}

void _setShardedClusterCardinalityParameter(
    OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
    // The config.shards collection is stable during FCV changes, so query that to discover the
    // current number of shards.
    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{NamespaceString::kConfigsvrShardsNamespace};
    findRequest.setLimit(2);
    auto numShards = client.find(std::move(findRequest))->itcount();

    // Prior to 7.3, the cluster parameter 'hasTwoOrMoreShards' gets set to true when the number
    // of shards goes from 1 to 2 but doesn't get set to false when the number of shards goes down
    // to 1.
    if (numShards >= 2) {
        return;
    }

    uassertStatusOK(
        ShardingCatalogManager::get(opCtx)->updateClusterCardinalityParameter(opCtx, numShards));
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

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        hangAtSetFCVStart.pauseWhileSet(opCtx);

        // Ensure that this operation will be killed by the RstlKillOpThread during step-up or
        // stepdown.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        auto request = SetFeatureCompatibilityVersion::parse(
            IDLParserContext("setFeatureCompatibilityVersion"), cmdObj);
        auto isFromConfigServer = request.getFromConfigServer().value_or(false);

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        Lock::ExclusiveLock setFCVCommandLock(opCtx, commandMutex);

        const auto requestedVersion = request.getCommandParameter();
        const auto actualVersion =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();

        auto isConfirmed = request.getConfirm().value_or(false);
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

            // _finalizeUpgrade is only for any tasks that must be done to fully complete the FCV
            // upgrade AFTER the FCV document has already been updated to the UPGRADED FCV.
            // We call it here because it's possible that during an FCV upgrade, the
            // replset/shard server/config server undergoes failover AFTER the FCV document has
            // already been updated to the UPGRADED FCV, but before the cluster has completed
            // _finalizeUpgrade. In this case, since the cluster failed over, the user/client may
            // retry sending the setFCV command to the cluster, but the cluster is already in the
            // requestedVersion (i.e. requestedVersion == actualVersion). However, the cluster
            // should retry/complete the tasks from _finalizeUpgrade before sending ok:1 back to the
            // user/client. Therefore, these tasks **must** be idempotent/retryable.
            _finalizeUpgrade(opCtx, requestedVersion);
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

                ScopedPostFCVDocumentUpdateActions postUpdateAction =
                    _prepareTransitionalState(opCtx, actualVersion, requestedVersion);

                // If this is a config server, then there must be no active
                // SetClusterParameterCoordinator instances active when downgrading.
                if (role && role->has(ClusterRole::ConfigServer)) {
                    uassert(ErrorCodes::CannotDowngrade,
                            "Cannot downgrade while cluster server parameters are being set",
                            ConfigsvrCoordinatorService::getService(opCtx)
                                ->areAllCoordinatorsOfTypeFinished(
                                    opCtx, ConfigsvrCoordinatorTypeEnum::kSetClusterParameter));
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
                invariant(role && role->has(ClusterRole::ShardServer));

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
                // Tell the shards to enter 'start' phase of setFCV (transition to kDowngrading).
                _sendSetFCVRequestToShards(
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
                _sendSetFCVRequestToShards(
                    opCtx, request, changeTimestamp, SetFCVPhaseEnum::kPrepare);
            }

            if (request.getPhase() == SetFCVPhaseEnum::kPrepare) {
                invariant(role && role->has(ClusterRole::ShardServer));
                // If we are only running the 'prepare' phase, then we are done
                return true;
            }
        }

        invariant(serverGlobalParams.featureCompatibility.acquireFCVSnapshot()
                      .isUpgradingOrDowngrading());
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
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion(),
                requestedVersion,
                isFromConfigServer,
                changeTimestamp,
                false /* setTargetVersion */,
                false /* setIsCleaningServerMetadata */);
        }

        // _finalizeUpgrade is only for any tasks that must be done to fully complete the FCV
        // upgrade AFTER the FCV document has already been updated to the UPGRADED FCV.
        // This is because during _runUpgrade, the FCV is still in the transitional state (which
        // behaves like the downgraded FCV), so certain tasks cannot be done yet until the FCV is
        // fully upgraded.
        // Everything in this function **must** be idempotent/retryable.
        if (requestedVersion > actualVersion) {
            _finalizeUpgrade(opCtx, requestedVersion);
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
    void _shardServerPhase1Tasks(OperationContext* opCtx,
                                 multiversion::FeatureCompatibilityVersion requestedVersion) {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto& [originalVersion, _] = getTransitionFCVFromAndTo(fcvSnapshot.getVersion());
        const auto isDowngrading = originalVersion > requestedVersion;
        const auto isUpgrading = originalVersion < requestedVersion;

        // TODO SERVER-67392: Remove once gGlobalIndexesShardingCatalog is enabled.
        if (isDowngrading &&
            feature_flags::gGlobalIndexesShardingCatalog
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kRenameCollection);
        }

        // TODO SERVER-77915: Remove once v8.0 branches out
        if ((isUpgrading &&
             feature_flags::gTrackUnshardedCollectionsUponCreation
                 .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion,
                                                               originalVersion)) ||
            (isDowngrading &&
             feature_flags::gTrackUnshardedCollectionsUponCreation
                 .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                               originalVersion))) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kRenameCollection);
        }

        // TODO SERVER-79064: Remove once 8.0 becomes last LTS.
        if (isDowngrading &&
            feature_flags::gAuthoritativeRefineCollectionShardKey
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kRefineCollectionShardKey);
        }

        // TODO SERVER-79304 Remove once shardCollection authoritative version becomes LTS
        // TODO (SERVER-77915): Remove once 8.0 (trackUnshardedCollections) becomes lastLTS.
        if (isDowngrading &&
            (feature_flags::gAuthoritativeShardCollection
                 .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                               originalVersion))) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kCreateCollection);
        }

        // We also wait for Create Collection coordinators to drain if downgrading and the flag for
        // validateAndDefaultValuesForShardedTimeseries gets disabled since it involves a durable
        // DDL document change.
        if (isDowngrading &&
            gFeatureFlagValidateAndDefaultValuesForShardedTimeseries
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kCreateCollection);
        }

        // TODO SERVER-87119 remove the following scope once v8.0 branches out
        if (isDowngrading &&
            feature_flags::gConvertToCappedCoordinator.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kConvertToCapped);
        }

        // TODO SERVER-77915: Remove once trackUnshardedCollections becomes lastLTS.
        if ((isUpgrading &&
             feature_flags::gTrackUnshardedCollectionsUponCreation
                 .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion,
                                                               originalVersion)) ||
            (isDowngrading &&
             feature_flags::gTrackUnshardedCollectionsUponCreation
                 .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion,
                                                               originalVersion))) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(opCtx, DDLCoordinatorTypeEnum::kCollMod);
        }

        // TODO (SERVER-76436) Remove once global balancing becomes last lts.
        if (isDowngrading &&
            feature_flags::gBalanceUnshardedCollections
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(opCtx,
                                                           DDLCoordinatorTypeEnum::kMovePrimary);
        }

        if (isUpgrading) {
            _createShardingIndexCatalogIndexes(
                opCtx, requestedVersion, NamespaceString::kShardIndexCatalogNamespace);
        }
    }

    // This helper function is for any actions that should be done before taking the global lock in
    // S mode. It is required that the code in this helper function is idempotent and could be done
    // after _runDowngrade even if it failed at any point in the middle of
    // _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
    void _prepareToUpgradeActions(OperationContext* opCtx,
                                  const multiversion::FeatureCompatibilityVersion requestedVersion,
                                  boost::optional<Timestamp> changeTimestamp) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        if (!role || role->has(ClusterRole::None)) {
            if (repl::ReplicationCoordinator::get(opCtx)->getSettings().isServerless()) {
                _cancelServerlessMigrations(opCtx);
            }
            _maybeMigrateAuditConfig(opCtx, requestedVersion, changeTimestamp);
            return;
        }

        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (role && role->has(ClusterRole::ConfigServer)) {
            // Config server role actions.
            _maybeMigrateAuditConfig(opCtx, requestedVersion, changeTimestamp);
            _dropReshardingCoordinatorUniqueIndex(opCtx, requestedVersion);
        }

        if (role && role->has(ClusterRole::ShardServer)) {
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

    void _maybeRemoveOldAuditConfig(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        if (feature_flags::gFeatureFlagAuditConfigClusterParameter.isEnabledOnVersion(
                requestedVersion) &&
            audit::removeOldConfig) {
            LOGV2_DEBUG(7193000,
                        3,
                        "Upgraded to FCV with audit config cluster parameter enabled, removing old "
                        "config.");
            audit::removeOldConfig(opCtx);
        }
    }

    // This helper function is for updating server metadata to make sure the new features in the
    // upgraded version work for sharded and non-sharded clusters. It is required that the code
    // in this helper function is idempotent and could be done after _runDowngrade even if it
    // failed at any point in the middle of _userCollectionsUassertsForDowngrade or
    // _internalServerCleanupForDowngrade.
    void _upgradeServerMetadata(OperationContext* opCtx,
                                const multiversion::FeatureCompatibilityVersion requestedVersion) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        if (role && role->has(ClusterRole::ShardServer)) {
            // Delete any possible leftover ShardingStateRecovery document.
            // TODO SERVER-78330 remove this.
            deleteShardingStateRecoveryDoc(opCtx);
        }

        if (role && role->has(ClusterRole::ConfigServer)) {
            _setShardedClusterCardinalityParameter(opCtx, requestedVersion);
            // TODO (SERVER-83264): Remove once 8.0 becomes last LTS.
            _upgradeConfigSettingsSchema(opCtx, requestedVersion);
            _initializePlacementHistory(opCtx, requestedVersion);
        }

        // TODO SERVER-80490: Remove this once 8.0 is released.
        // Sanitizes the wiredTiger.creationString option from the durable catalog. Removes the
        // encryption config options since they are ephemeral in nature.
        _sanitizeCreationConfigString(opCtx, requestedVersion);
    }

    void _upgradeConfigSettingsSchema(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        if (feature_flags::gBalancerSettingsSchema.isEnabledOnVersion(requestedVersion)) {
            LOGV2(8260900, "Updating schema on config.settings");
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->upgradeDowngradeConfigSettings(opCtx));
        }
    }

    // TODO (SERVER-83704): Remove this function once 8.0 becomes last LTS.
    void _initializePlacementHistory(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        const auto& [originalVersion, _] = getTransitionFCVFromAndTo(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion());
        if (feature_flags::gPlacementHistoryPostFCV3.isEnabledOnTargetFCVButDisabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            ShardingCatalogManager::get(opCtx)->initializePlacementHistory(opCtx);
        }
    }

    // TODO SERVER-80490: Remove this method once 8.0 is released.
    void _sanitizeCreationConfigString(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        // We bypass the UserWritesBlock mode here in order to not see errors arising from the
        // block. The user already has permission to run FCV at this point and the writes performed
        // here aren't modifying any user data with the exception of fixing up the collection
        // metadata.
        auto originalValue = WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled();
        ON_BLOCK_EXIT([&] { WriteBlockBypass::get(opCtx).set(originalValue); });
        WriteBlockBypass::get(opCtx).set(true);

        auto curop = CurOp::get(opCtx);
        for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
            Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
            catalog::forEachCollectionFromDb(
                opCtx,
                dbName,
                MODE_X,
                [&](const Collection* collection) {
                    NamespaceStringOrUUID nsOrUUID(dbName, collection->uuid());
                    CollMod collModCmd(collection->ns());

                    // Nested CurOp for collMod. Namespace should ideally be
                    // the command namespace for 'dbName' but collMod internally
                    // overrides the CurOp namespace (through OldClientContext)
                    // with the namespace of the collection being modified.
                    CurOp collModCurOp;
                    collModCurOp.push(opCtx);
                    collModCurOp.setGenericOpRequestDetails(collection->ns(),
                                                            curop->getCommand(),
                                                            collModCmd.toBSON({}),
                                                            NetworkOp::dbMsg);

                    BSONObjBuilder unusedBuilder;
                    uassertStatusOK(
                        processCollModCommand(opCtx, nsOrUUID, collModCmd, &unusedBuilder));

                    try {
                        // Logs the collMod statistics if it took longer than the server
                        // parameter `slowMs` to complete.
                        collModCurOp.completeAndLogOperation(
                            {MONGO_LOGV2_DEFAULT_COMPONENT, toLogService(opCtx->getService())},
                            CollectionCatalog::get(opCtx)
                                ->getDatabaseProfileSettings(dbName)
                                .filter);
                    } catch (const DBException& e) {
                        LOGV2_WARNING(8592500,
                                      "unable to log collMod operation during setFCV upgrade",
                                      "dbName"_attr = dbName,
                                      "error"_attr = e);
                    }
                    return true;
                },
                [&](const Collection* coll) {
                    // Performing sanitisation on node local collections is unnecessary since by
                    // definition they can use configuration specific to this node.
                    //
                    // We also only focus on normal collections that are created by the user.
                    const auto ns = coll->ns();
                    return ns.isReplicated() && ns.isNormalCollection() && !ns.isOnInternalDb();
                });
        }
    }

    void _maybeMigrateAuditConfig(OperationContext* opCtx,
                                  const multiversion::FeatureCompatibilityVersion requestedVersion,
                                  boost::optional<Timestamp> changeTimestamp) {
        const auto& [fromVersion, _] = getTransitionFCVFromAndTo(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion());
        if (feature_flags::gFeatureFlagAuditConfigClusterParameter
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, fromVersion) &&
            audit::migrateOldToNew) {
            LOGV2_DEBUG(7193001,
                        3,
                        "Upgrading to FCV wth audit config cluster parameter enabled, migrating "
                        "audit config to cluster parameter.");
            audit::migrateOldToNew(opCtx, changeTimestamp);
        }
    }

    void _createReshardingCoordinatorUniqueIndex(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion originalVersion) {
        // We're guaranteed that if the resharding metadata collection exists, it is empty;
        // if it were not we would have already aborted with ManualInterventionRequired.
        if (resharding::gFeatureFlagReshardingImprovements
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            LOGV2(7760407,
                  "Downgrading to FCV wth resharding improvements parameter disabled, "
                  "creating resharding coordinator unique index.");
            AutoGetCollection autoColl(
                opCtx, NamespaceString::kConfigReshardingOperationsNamespace, MODE_X);
            const Collection* collection = autoColl.getCollection().get();
            // This could only happen if we got a downgrade command before the service initialized;
            // in that case the collection and index will be created on initialization.
            if (!collection) {
                LOGV2_DEBUG(7760408,
                            2,
                            "The reshardingOperations collection did not exist during downgrade");
                return;
            }
            writeConflictRetry(
                opCtx,
                "createIndexOnConfigCollection",
                NamespaceString::kConfigReshardingOperationsNamespace,
                [&] {
                    WriteUnitOfWork wunit(opCtx);
                    CollectionWriter collWriter(opCtx, collection->uuid());
                    try {
                        IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                            opCtx,
                            collWriter,
                            {BSON("key" << BSON("active" << 1) << "name"
                                        << "ReshardingCoordinatorActiveIndex"
                                        << "v" << int(IndexDescriptor::kLatestIndexVersion)
                                        << "unique" << true)},
                            false /*fromMigrate*/);
                    } catch (const DBException& e) {
                        // The uassert should never happen, but it does not indicate corruption if
                        // it does.
                        uassert(ErrorCodes::ManualInterventionRequired,
                                str::stream() << "Unable to create 'active' index on "
                                                 "'config.reshardingOperations'.  Consider "
                                                 "dropping 'config.reshardingOperations' and "
                                                 "trying again.  Original exception "
                                              << e.toString(),
                                e.code() == ErrorCodes::IndexAlreadyExists);
                        LOGV2_DEBUG(7760409,
                                    2,
                                    "The 'active' unique index on the reshardingOperations "
                                    "collection already existed during downgrade");
                        return;
                    }
                    wunit.commit();
                });
        }
    }

    void _downgradeConfigSettingsSchema(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion originalVersion) {
        if (feature_flags::gBalancerSettingsSchema.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
            LOGV2(8260901, "Updating schema on config.settings");
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->upgradeDowngradeConfigSettings(opCtx));
        }
    }

    void _dropReshardingCoordinatorUniqueIndex(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        // There is no need to re-create this index on upgrade, as the index is no longer
        // needed to ensure resharding operations are unique.
        const auto& [fromVersion, _] = getTransitionFCVFromAndTo(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion());
        if (resharding::gFeatureFlagReshardingImprovements
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, fromVersion)) {
            LOGV2(7760401,
                  "Upgrading to FCV wth resharding improvements parameter enabled, "
                  "dropping resharding coordinator unique index.");
            try {
                // The index name is included literally here to avoid creating a
                // construction-order catastrophe with the static std::string
                // kReshardingCoordinatorActiveIndexName
                auto reply = dropIndexes(opCtx,
                                         NamespaceString::kConfigReshardingOperationsNamespace,
                                         boost::none,
                                         "ReshardingCoordinatorActiveIndex");
                LOGV2_DEBUG(
                    7760402, 2, "Dropped resharding coordinator index", "reply"_attr = reply);
            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                LOGV2_DEBUG(
                    7760403, 2, "Resharding coordinator collection did not exist during upgrade.");
            } catch (ExceptionFor<ErrorCodes::IndexNotFound>&) {
                LOGV2_DEBUG(7760404,
                            2,
                            "Resharding coordinator 'active' index did not exist during upgrade.");
            }
        }
    }

    void _createShardingIndexCatalogIndexes(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const NamespaceString& indexCatalogNss) {
        // TODO SERVER-67392: Remove once gGlobalIndexesShardingCatalog is enabled.
        const auto actualVersion =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
        if (feature_flags::gGlobalIndexesShardingCatalog
                .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, actualVersion)) {
            uassertStatusOK(
                sharding_util::createShardingIndexCatalogIndexes(opCtx, indexCatalogNss));
            if (indexCatalogNss == NamespaceString::kShardIndexCatalogNamespace) {
                uassertStatusOK(sharding_util::createShardCollectionCatalogIndexes(opCtx));
            }
        }
    }

    // _prepareToUpgrade performs all actions and checks that need to be done before proceeding to
    // make any metadata changes as part of FCV upgrade. Any new feature specific upgrade code
    // should be placed in the _prepareToUpgrade helper functions:
    //  * _prepareToUpgradeActions: for any upgrade actions that should be done before taking the
    //  global lock in S mode
    //  * _userCollectionsWorkForUpgrade: for any user collections uasserts, creations, or deletions
    //    that need to happen during the upgrade. This happens after the global lock.
    // Please read the comments on those helper functions for more details on what should be placed
    // in each function.
    void _prepareToUpgrade(OperationContext* opCtx,
                           const SetFeatureCompatibilityVersion& request,
                           boost::optional<Timestamp> changeTimestamp) {
        // This helper function is for any actions that should be done before taking the global
        // lock in S mode. It is required that the code in this helper function is idempotent and
        // could be done after _runDowngrade even if it failed at any point in the middle of
        // _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
        const auto requestedVersion = request.getCommandParameter();
        _prepareToUpgradeActions(opCtx, requestedVersion, changeTimestamp);

        {
            // Take the global lock in S mode to create a barrier for operations taking the global
            // IX or X locks. This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::GlobalLock lk(opCtx, MODE_S);
        }
        auto role = ShardingState::get(opCtx)->pollClusterRole();

        // This helper function is for any user collections uasserts, creations, or deletions that
        // need to happen during the upgrade. It is required that the code in this helper function
        // is idempotent and could be done after _runDowngrade even if it failed at any point in the
        // middle of _userCollectionsUassertsForDowngrade or _internalServerCleanupForDowngrade.
        if (!role || role->has(ClusterRole::None) || role->has(ClusterRole::ShardServer)) {
            _userCollectionsWorkForUpgrade();
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

            // Always abort the reshardCollection regardless of version to ensure that it will run
            // on a consistent version from start to finish. This will ensure that it will be able
            // to apply the oplog entries correctly.
            abortAllReshardCollection(opCtx);

            _createShardingIndexCatalogIndexes(
                opCtx, requestedVersion, NamespaceString::kConfigsvrIndexCatalogNamespace);

            // Tell the shards to complete setFCV (transition to fully upgraded)
            _sendSetFCVRequestToShards(opCtx, request, changeTimestamp, SetFCVPhaseEnum::kComplete);
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
    void _prepareToDowngradeActions(OperationContext* opCtx) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        if ((!role || role->has(ClusterRole::None)) &&
            repl::ReplicationCoordinator::get(opCtx)->getSettings().isServerless()) {
            _cancelServerlessMigrations(opCtx);
            return;
        }

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
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto& [originalVersion, _] = getTransitionFCVFromAndTo(fcvSnapshot.getVersion());

        if (feature_flags::gFeatureFlagAuditConfigClusterParameter
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            // Ensure audit config cluster parameter is unset on disk.
            AutoGetCollection clusterParametersColl(
                opCtx, NamespaceString::kClusterParametersNamespace, MODE_IS);
            BSONObj _result;
            if (Helpers::findOne(opCtx,
                                 clusterParametersColl.getCollection(),
                                 BSON("_id"
                                      << "auditConfig"),
                                 _result)) {
                uasserted(ErrorCodes::CannotDowngrade,
                          "Cannot downgrade the cluster when the auditConfig cluster parameter is "
                          "set. Drop the auditConfig document from the config.clusterParameters "
                          "collection before downgrading.");
            }
        }

        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (role && role->has(ClusterRole::ConfigServer)) {
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
                            << collDoc[CollectionType::kNssFieldName].String() << "'",
                        !hasShardingIndexCatalogEntries);
            }
        }

        if (gFeatureFlagRecordIdsReplicated.isDisabledOnTargetFCVButEnabledOnOriginalFCV(
                requestedVersion, originalVersion)) {
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
    }

    // Remove cluster parameters from the clusterParameters collections which are not enabled on
    // requestedVersion.
    void _cleanUpClusterParameters(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto& [fromVersion, _] = getTransitionFCVFromAndTo(fcvSnapshot.getVersion());

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

    void _updateAuditConfigOnDowngrade(
        OperationContext* opCtx, const multiversion::FeatureCompatibilityVersion requestedVersion) {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto& [fromVersion, _] = getTransitionFCVFromAndTo(fcvSnapshot.getVersion());

        if (feature_flags::gFeatureFlagAuditConfigClusterParameter
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, fromVersion)) {
            if (audit::updateAuditConfigOnDowngrade) {
                audit::updateAuditConfigOnDowngrade(opCtx);
            }
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
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        invariant(fcvSnapshot.isUpgradingOrDowngrading());
        const auto& [originalVersion, _] = getTransitionFCVFromAndTo(fcvSnapshot.getVersion());

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
        }

        _cleanUpClusterParameters(opCtx, requestedVersion);
        // Note the config server is also considered a shard, so the ConfigServer and ShardServer
        // roles aren't mutually exclusive.
        if (role && role->has(ClusterRole::ConfigServer)) {
            _updateAuditConfigOnDowngrade(opCtx, requestedVersion);
            _dropInternalShardingIndexCatalogCollection(
                opCtx,
                requestedVersion,
                originalVersion,
                NamespaceString::kConfigsvrIndexCatalogNamespace);
            // Always abort the reshardCollection regardless of version to ensure that it will
            // run on a consistent version from start to finish. This will ensure that it will
            // be able to apply the oplog entries correctly.
            abortAllReshardCollection(opCtx);
            _createReshardingCoordinatorUniqueIndex(opCtx, requestedVersion, originalVersion);
            // TODO (SERVER-83264): Remove once 8.0 becomes last LTS.
            _downgradeConfigSettingsSchema(opCtx, requestedVersion, originalVersion);
        }

        if (role && role->has(ClusterRole::ShardServer)) {
            _dropInternalShardingIndexCatalogCollection(
                opCtx,
                requestedVersion,
                originalVersion,
                NamespaceString::kShardIndexCatalogNamespace);
        } else {
            _updateAuditConfigOnDowngrade(opCtx, requestedVersion);
        }
    }

    void _dropInternalShardingIndexCatalogCollection(
        OperationContext* opCtx,
        const multiversion::FeatureCompatibilityVersion requestedVersion,
        const multiversion::FeatureCompatibilityVersion originalVersion,
        const NamespaceString& indexCatalogNss) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        // TODO SERVER-67392: Remove when 7.0 branches-out.
        // Coordinators that commits indexes to the csrs must be drained before this point. Older
        // FCV's must not find cluster-wide indexes.
        DropReply dropReply;
        if (feature_flags::gGlobalIndexesShardingCatalog
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
            // Note the config server is also considered a shard, so the ConfigServer and
            // ShardServer roles aren't mutually exclusive.
            if (role && role->has(ClusterRole::ShardServer)) {
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
                                             .toStringForErrorMsg()
                                      << causedBy(dropStatus.reason()),
                        dropStatus.isOK() || dropStatus.code() == ErrorCodes::NamespaceNotFound);
            }

            if (role && role->has(ClusterRole::ConfigServer)) {
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

            LOGV2(6280502, "Dropping global indexes collection", "nss"_attr = indexCatalogNss);
            const auto deletionStatus =
                dropCollection(opCtx,
                               indexCatalogNss,
                               &dropReply,
                               DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
            uassert(deletionStatus.code(),
                    str::stream() << "Failed to drop " << indexCatalogNss.toStringForErrorMsg()
                                  << causedBy(deletionStatus.reason()),
                    deletionStatus.isOK() ||
                        deletionStatus.code() == ErrorCodes::NamespaceNotFound);
        }
    }

    /**
     * May contain actions to perfom after the FCV document update. Execution occurs when the object
     * goes out of scope.
     */
    using ScopedPostFCVDocumentUpdateActions = ScopeGuard<std::function<void()>>;

    /**
     * Actions to be performed before the FCV document is set into upgrading or downgrading
     * transitional state. The returned object may contain post-update actions which are executed
     * when it goes out of scope, so it must be properly scoped to expire after the FCV document has
     * been updated. The assumption is that the provided opCtx is still valid by the time the action
     * is executed.
     */
    ScopedPostFCVDocumentUpdateActions _prepareTransitionalState(
        OperationContext* opCtx,
        multiversion::FeatureCompatibilityVersion actualVersion,
        multiversion::FeatureCompatibilityVersion requestedVersion) {

        // Any actions to be performed post-update must also be performed in case of interruption
        // during this function.
        std::vector<std::function<void()>> postUpdateActions;
        ScopeGuard postUpdateActionsGuard([&postUpdateActions]() {
            for (const auto& action : postUpdateActions) {
                action();
            }
        });

        // TODO (SERVER-68290): Remove index build abort due to FCV downgrade once the
        // feature flag is removed.
        if (feature_flags::gIndexBuildGracefulErrorHandling
                .isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, actualVersion)) {
            invariant(requestedVersion < actualVersion);
            const auto reason = fmt::format("FCV downgrade in progress, from {} to {}.",
                                            toString(actualVersion),
                                            toString(requestedVersion));

            const auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
            // Block new index builds before writing the transitional FCV state, which will cause
            // new feature flag checks to consider it disabled.
            indexBuildsCoord->setNewIndexBuildsBlocked(true, reason);
            // New index builds will be unblocked after ScopedPostFCVDocumentUpdateActions goes out
            // of scope once the FCV document has been updated.
            postUpdateActions.push_back(
                [indexBuildsCoord] { indexBuildsCoord->setNewIndexBuildsBlocked(false); });

            if (hangAfterBlockingIndexBuildsForFcvDowngrade.shouldFail()) {
                LOGV2(7738704, "Hanging for failpoint hangAfterBlockingIndexBuildsForFcvDowngrade");
                hangAfterBlockingIndexBuildsForFcvDowngrade.pauseWhileSet(opCtx);
            }

            // While new index builds are blocked, abort all existing index builds and wait for
            // them.
            indexBuildsCoord->abortAllIndexBuildsWithReason(opCtx, reason);
            // Some index builds might already be committing or aborting, in which case the above
            // call does not wait for them. Wait for the rest of the index builds.
            indexBuildsCoord->waitForAllIndexBuildsToStop(opCtx);
        }

        postUpdateActionsGuard.dismiss();
        const auto runAllActions = [postUpdateActions = std::move(postUpdateActions)]() {
            for (const auto& action : postUpdateActions) {
                action();
            }
        };

        return {runAllActions};
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
        _prepareToDowngradeActions(opCtx);

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
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        const auto requestedVersion = request.getCommandParameter();
        const auto actualVersion =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
        auto isFromConfigServer = request.getFromConfigServer().value_or(false);

        hangDowngradingBeforeIsCleaningServerMetadata.pauseWhileSet(opCtx);
        // Set the isCleaningServerMetadata field to true. This prohibits the downgrading to
        // upgrading transition until the isCleaningServerMetadata is unset when we successfully
        // finish the FCV downgrade and transition to the DOWNGRADED state.
        {
            const auto fcvChangeRegion(FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));
            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx,
                actualVersion,
                requestedVersion,
                isFromConfigServer,
                changeTimestamp,
                true /* setTargetVersion */,
                true /* setIsCleaningServerMetadata*/);
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

        if (role && role->has(ClusterRole::ConfigServer)) {
            // Tell the shards to complete setFCV (transition to fully downgraded).
            _sendSetFCVRequestToShards(opCtx, request, changeTimestamp, SetFCVPhaseEnum::kComplete);
        }

        hangBeforeTransitioningToDowngraded.pauseWhileSet(opCtx);
    }

    /**
     * Abort all serverless migrations active on this node, for both donors and recipients.
     * Called after reaching an upgrading or downgrading state for nodes with ClusterRole::None.
     * Must only be called in serverless mode.
     */
    void _cancelServerlessMigrations(OperationContext* opCtx) {
        invariant(repl::ReplicationCoordinator::get(opCtx)->getSettings().isServerless());
        invariant(serverGlobalParams.featureCompatibility.acquireFCVSnapshot()
                      .isUpgradingOrDowngrading());

        auto donorService = checked_cast<TenantMigrationDonorService*>(
            repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                ->lookupServiceByName(TenantMigrationDonorService::kServiceName));
        donorService->abortAllMigrations(opCtx);

        auto recipientService = checked_cast<repl::TenantMigrationRecipientService*>(
            repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                ->lookupServiceByName(
                    repl::TenantMigrationRecipientService::kTenantMigrationRecipientServiceName));
        recipientService->abortAllMigrations(opCtx);

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

    void _assertNoCollectionsHaveChangeStreamsPrePostImages(OperationContext* opCtx) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        invariant(role && role->has(ClusterRole::ConfigServer));

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
                                          << collection->ns().toStringForErrorMsg()
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
    void _finalizeUpgrade(OperationContext* opCtx,
                          const multiversion::FeatureCompatibilityVersion requestedVersion) {
        auto role = ShardingState::get(opCtx)->pollClusterRole();
        // TODO SERVER-79064: Remove once 8.0 becomes last LTS.
        if (role && role->has(ClusterRole::ShardServer) &&
            feature_flags::gAuthoritativeRefineCollectionShardKey.isEnabledOnVersion(
                requestedVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kRefineCollectionShardKeyPre71Compatible);
        }

        // TODO SERVER-79304 Remove once shardCollection authoritative version becomes LTS
        if (role && role->has(ClusterRole::ShardServer) &&
            feature_flags::gAuthoritativeShardCollection.isEnabledOnVersion(requestedVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kCreateCollectionPre80Compatible);
        }

        // We perform a drain of create coordinators so that we don't end up with a confusing
        // ConflictingOperationInProgress error. For more details see SERVER-83114.
        //
        // This can be removed once 8.0 becomes last LTS.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
            gFeatureFlagValidateAndDefaultValuesForShardedTimeseries.isEnabledOnVersion(
                requestedVersion)) {
            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kCreateCollection);
        }

        _maybeRemoveOldAuditConfig(opCtx, requestedVersion);

        // TODO SERVER-80266 remove once 8.0 becomes last lts
        if (role && role->has(ClusterRole::ConfigServer)) {
            ShardingCatalogManager::get(opCtx)->deleteMaxSizeMbFromShardEntries(opCtx);
        }
    }
};
MONGO_REGISTER_COMMAND(SetFeatureCompatibilityVersionCommand).forShard();

}  // namespace
}  // namespace mongo
