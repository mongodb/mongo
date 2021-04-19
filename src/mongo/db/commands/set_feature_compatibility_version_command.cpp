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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
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
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/server_options.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using FCVP = FeatureCompatibilityVersionParser;
using FeatureCompatibility = ServerGlobalParams::FeatureCompatibility;

MONGO_FAIL_POINT_DEFINE(failUpgrading);
MONGO_FAIL_POINT_DEFINE(hangWhileUpgrading);
MONGO_FAIL_POINT_DEFINE(hangAfterStartingFCVTransition);
MONGO_FAIL_POINT_DEFINE(failDowngrading);
MONGO_FAIL_POINT_DEFINE(hangWhileDowngrading);

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

void checkInitialSyncFinished(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool isReplSet =
        replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot upgrade/downgrade the cluster when the replica set config "
                          << "contains 'newlyAdded' members; wait for those members to "
                          << "finish its initial sync procedure",
            !(isReplSet && replCoord->replSetContainsNewlyAddedMembers()));


    // We should make sure the current config w/o 'newlyAdded' members got replicated
    // to all nodes.
    LOGV2(4637904, "Waiting for the current replica set config to propagate to all nodes.");
    // If a write concern is given, we'll use its wTimeout. It's kNoTimeout by default.
    WriteConcernOptions writeConcern(repl::ReplSetConfig::kConfigAllWriteConcernName,
                                     WriteConcernOptions::SyncMode::NONE,
                                     opCtx->getWriteConcern().wTimeout);
    writeConcern.checkCondition = WriteConcernOptions::CheckCondition::Config;
    repl::OpTime fakeOpTime(Timestamp(1, 1), replCoord->getTerm());
    uassertStatusOKWithContext(
        replCoord->awaitReplication(opCtx, fakeOpTime, writeConcern).status,
        "Failed to wait for the current replica set config to propagate to all nodes");
    LOGV2(4637905, "The current replica set config has been propagated to all nodes.");
}

void waitForCurrentConfigCommitment(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // Skip the waiting if the current config is from a force reconfig.
    auto oplogWait = replCoord->getConfig().getConfigTerm() != repl::OpTime::kUninitializedTerm;
    auto status = replCoord->awaitConfigCommitment(opCtx, oplogWait);
    status.addContext("New feature compatibility version is rejected");
    if (status == ErrorCodes::MaxTimeMSExpired) {
        // Convert the error code to be more specific.
        uasserted(ErrorCodes::CurrentConfigNotCommittedYet, status.reason());
    }
    uassertStatusOK(status);
}

void removeTimeseriesEntriesFromConfigTransactions(OperationContext* opCtx,
                                                   const std::vector<LogicalSessionId>& sessions) {
    DBDirectClient dbClient(opCtx);
    for (const auto& session : sessions) {
        dbClient.remove(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                        QUERY(LogicalSessionRecord::kIdFieldName << session.toBSON()));
    }
}

void removeTimeseriesEntriesFromConfigTransactions(OperationContext* opCtx) {
    DBDirectClient dbClient(opCtx);

    std::vector<LogicalSessionId> sessions;

    static constexpr StringData kLastWriteOpFieldName = "lastWriteOpTime"_sd;
    auto cursor = dbClient.query(NamespaceString::kSessionTransactionsTableNamespace, Query{});
    while (cursor->more()) {
        auto entry = TransactionHistoryIterator{repl::OpTime::parse(
                                                    cursor->next()[kLastWriteOpFieldName].Obj())}
                         .next(opCtx);
        if (entry.getNss().isTimeseriesBucketsCollection()) {
            sessions.push_back(*entry.getSessionId());
        }
    }

    if (sessions.empty()) {
        return;
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet &&
        opCtx->getLogicalSessionId()) {
        // Perform the deletes using a separate client because direct writes to config.transactions
        // are not allowed in sessions.
        auto client =
            getGlobalServiceContext()->makeClient("removeTimeseriesEntriesFromConfigTransactions");
        AlternativeClientRegion acr(client);

        removeTimeseriesEntriesFromConfigTransactions(cc().makeOperationContext().get(), sessions);
    } else {
        removeTimeseriesEntriesFromConfigTransactions(opCtx, sessions);
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
          << FCVP::kLastLTS << "', then features introduced in versions greater than '"
          << FCVP::kLastLTS << "' will be disabled";
        if (FCVP::kLastContinuous != FCVP::kLastLTS) {
            h << " If set to '" << FCVP::kLastContinuous << "', then features introduced in '"
              << FCVP::kLatest << "' will be disabled.";
        }
        h << " If set to '" << FCVP::kLatest << "', then '" << FCVP::kLatest
          << "' features are enabled, and all nodes in the cluster must be binary version "
          << FCVP::kLatest << ". See "
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
            // Propagate the user's wTimeout if one was given.
            auto timeout =
                opCtx->getWriteConcern().usedDefault ? INT_MAX : opCtx->getWriteConcern().wTimeout;
            WriteConcernResult res;
            auto waitForWCStatus = waitForWriteConcern(
                opCtx,
                repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                WriteConcernOptions(repl::ReplSetConfig::kMajorityWriteConcernModeName,
                                    WriteConcernOptions::SyncMode::UNSET,
                                    timeout),
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
            uassert(
                ErrorCodes::IllegalOperation,
                str::stream() << "Cannot set featureCompatibilityVersion to "
                              << FCVP::serializeVersion(requestedVersion) << " with '"
                              << SetFeatureCompatibilityVersion::kDowngradeOnDiskChangesFieldName
                              << "' set to true. This is only allowed when downgrading to "
                              << FCVP::kLastContinuous,
                requestedVersion <= actualVersion &&
                    requestedVersion == FeatureCompatibility::kLastContinuous);
        }

        if (requestedVersion == actualVersion) {
            // Set the client's last opTime to the system last opTime so no-ops wait for
            // writeConcern.
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            return true;
        }

        auto isFromConfigServer = request.getFromConfigServer().value_or(false);
        FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
            actualVersion, requestedVersion, isFromConfigServer);

        uassert(5563600,
                "'phase' field is only valid to be specified on shards",
                !request.getPhase() || serverGlobalParams.clusterRole == ClusterRole::ShardServer);

        if (!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kStart) {
            {
                // Start transition to 'requestedVersion' by updating the local FCV document to a
                // 'kUpgrading' or 'kDowngrading' state, respectively.
                const auto fcvChangeRegion(
                    FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

                checkInitialSyncFinished(opCtx);

                FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                    opCtx,
                    actualVersion,
                    requestedVersion,
                    isFromConfigServer,
                    true /* setTargetVersion */);
            }

            if (request.getPhase() == SetFCVPhaseEnum::kStart) {
                invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
                if (actualVersion > requestedVersion) {
                    // Downgrading
                    // TODO SERVER-55898: Release fcvChangeRegion and wait for DDLCoordinators to
                    // drain
                }
                // If we are only running phase-1, then we are done
                return true;
            }
        }

        invariant(!request.getPhase() || request.getPhase() == SetFCVPhaseEnum::kComplete);

        uassert(5563601,
                "Cannot transition to fully upgraded or fully downgraded state if we are not in "
                "kUpgrading or kDowngrading state",
                serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());

        hangAfterStartingFCVTransition.pauseWhileSet(opCtx);

        if (requestedVersion > actualVersion) {
            _runUpgrade(opCtx, request);
        } else {
            _runDowngrade(opCtx, request);
        }

        {
            // Complete transition by updating the local FCV document to the fully upgraded or
            // downgraded requestedVersion.
            const auto fcvChangeRegion(FeatureCompatibilityVersion::enterFCVChangeRegion(opCtx));

            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx,
                serverGlobalParams.featureCompatibility.getVersion(),
                requestedVersion,
                isFromConfigServer,
                false /* setTargetVersion */);
        }

        return true;
    }

private:
    void _runUpgrade(OperationContext* opCtx, const SetFeatureCompatibilityVersion& request) {
        const auto requestedVersion = request.getCommandParameter();

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // Tell the shards to enter phase-1 of setFCV
            auto requestPhase1 = request;
            requestPhase1.setPhase(SetFCVPhaseEnum::kStart);
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                    opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase1.toBSON({}))));
        }

        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        const bool isReplSet =
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

        // If the 'useSecondaryDelaySecs' feature flag is enabled in the upgraded FCV, issue a
        // reconfig to change the 'slaveDelay' field to 'secondaryDelaySecs'.
        if (repl::feature_flags::gUseSecondaryDelaySecs.isEnabledAndIgnoreFCV() && isReplSet &&
            requestedVersion >= repl::feature_flags::gUseSecondaryDelaySecs.getVersion()) {
            // Wait for the current config to be committed before starting a new reconfig.
            waitForCurrentConfigCommitment(opCtx);

            auto getNewConfig = [&](const repl::ReplSetConfig& oldConfig, long long term) {
                auto newConfig = oldConfig.getMutable();
                newConfig.setConfigVersion(newConfig.getConfigVersion() + 1);
                for (auto mem = oldConfig.membersBegin(); mem != oldConfig.membersEnd(); mem++) {
                    newConfig.useSecondaryDelaySecsFieldName(mem->getId());
                }
                return repl::ReplSetConfig(std::move(newConfig));
            };
            auto status = replCoord->doReplSetReconfig(opCtx, getNewConfig, false /* force */);
            uassertStatusOKWithContext(status, "Failed to upgrade the replica set config");

            uassertStatusOKWithContext(
                replCoord->awaitConfigCommitment(opCtx, true /* waitForOplogCommitment */),
                "The upgraded replica set config failed to propagate to a majority");
            LOGV2(5042302, "The upgraded replica set config has been propagated to a majority");
        }

        {
            // Take the global lock in S mode to create a barrier for operations taking the global
            // IX or X locks. This ensures that either:
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     upgrading to the latest FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before upgrade procedures begin right after this.
            Lock::GlobalLock lk(opCtx, MODE_S);
        }

        uassert(ErrorCodes::Error(549180),
                "Failing upgrade due to 'failUpgrading' failpoint set",
                !failUpgrading.shouldFail());

        // Delete any haystack indexes if we're upgrading to an FCV of 4.9 or higher.
        //
        // TODO SERVER-51871: This block can removed once 5.0 becomes last-lts.
        if (requestedVersion >= FeatureCompatibility::Version::kVersion49) {
            _deleteHaystackIndexesOnUpgrade(opCtx);
        }

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // TODO SERVER-53283: This block can removed once 5.0 becomes last-lts.
            // TODO SERVER-53774: Replace kLatest by the version defined in the feature flag IDL
            if (requestedVersion >= FeatureCompatibility::kLatest) {
                ShardingCatalogManager::get(opCtx)->upgradeMetadataFor50Phase1(opCtx);
            }

            // Tell the shards to enter phase-2 of setFCV (fully upgraded)
            auto requestPhase2 = request;
            requestPhase2.setPhase(SetFCVPhaseEnum::kComplete);
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                    opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase2.toBSON({}))));

            // TODO SERVER-53283: This block can removed once 5.0 becomes last-lts.
            // TODO SERVER-53774: Replace kLatest by the version defined in the feature flag IDL
            if (requestedVersion >= FeatureCompatibility::kLatest) {
                ShardingCatalogManager::get(opCtx)->upgradeMetadataFor50Phase2(opCtx);
            }
        }

        hangWhileUpgrading.pauseWhileSet(opCtx);
    }

    /**
     * Removes all haystack indexes from the catalog.
     *
     * TODO SERVER-51871: This method can be removed once 5.0 becomes last-lts.
     */
    void _deleteHaystackIndexesOnUpgrade(OperationContext* opCtx) {
        auto collCatalog = CollectionCatalog::get(opCtx);
        for (const auto& db : collCatalog->getAllDbNames()) {
            for (auto collIt = collCatalog->begin(opCtx, db); collIt != collCatalog->end(opCtx);
                 ++collIt) {
                NamespaceStringOrUUID collName(
                    collCatalog->lookupNSSByUUID(opCtx, collIt.uuid().get()).get());
                AutoGetCollectionForRead coll(opCtx, collName);
                auto idxCatalog = coll->getIndexCatalog();
                std::vector<const IndexDescriptor*> haystackIndexes;
                idxCatalog->findIndexByType(opCtx, IndexNames::GEO_HAYSTACK, haystackIndexes);

                // Continue if 'coll' has no haystack indexes.
                if (haystackIndexes.empty()) {
                    continue;
                }

                std::vector<std::string> indexNames;
                for (auto&& haystackIndex : haystackIndexes) {
                    indexNames.emplace_back(haystackIndex->indexName());
                }
                dropIndexes(opCtx, *collName.nss(), indexNames);
            }
        }
    }

    void _runDowngrade(OperationContext* opCtx, const SetFeatureCompatibilityVersion& request) {
        const auto requestedVersion = request.getCommandParameter();

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // Tell the shards to enter phase-1 of setFCV
            auto requestPhase1 = request;
            requestPhase1.setPhase(SetFCVPhaseEnum::kStart);
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                    opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase1.toBSON({}))));
        }

        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        const bool isReplSet =
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

        // Time-series collections are only supported in 5.0. If the user tries to downgrade the
        // cluster to an earlier version, they must first remove all time-series collections.
        // TODO (SERVER-56171): Remove once 5.0 is last-lts.
        for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
            auto viewCatalog = DatabaseHolder::get(opCtx)->getViewCatalog(opCtx, dbName);
            if (!viewCatalog) {
                continue;
            }
            viewCatalog->iterate([](const ViewDefinition& view) {
                uassert(ErrorCodes::CannotDowngrade,
                        str::stream()
                            << "Cannot downgrade the cluster when there are time-series "
                               "collections present; drop all time-series collections before "
                               "downgrading. First detected time-series collection: "
                            << view.name(),
                        !view.timeseries());
                return true;
            });
        }

        // TODO (SERVER-56171): Remove once 5.0 is last-lts.
        removeTimeseriesEntriesFromConfigTransactions(opCtx);

        // If the 'useSecondaryDelaySecs' feature flag is disabled in the downgraded FCV, issue a
        // reconfig to change the 'secondaryDelaySecs' field to 'slaveDelay'.
        if (isReplSet && repl::feature_flags::gUseSecondaryDelaySecs.isEnabledAndIgnoreFCV() &&
            requestedVersion < repl::feature_flags::gUseSecondaryDelaySecs.getVersion()) {
            // Wait for the current config to be committed before starting a new reconfig.
            waitForCurrentConfigCommitment(opCtx);

            auto getNewConfig = [&](const repl::ReplSetConfig& oldConfig, long long term) {
                auto newConfig = oldConfig.getMutable();
                newConfig.setConfigVersion(newConfig.getConfigVersion() + 1);
                for (auto mem = oldConfig.membersBegin(); mem != oldConfig.membersEnd(); mem++) {
                    newConfig.useSlaveDelayFieldName(mem->getId());
                }

                return repl::ReplSetConfig(std::move(newConfig));
            };

            auto status = replCoord->doReplSetReconfig(opCtx, getNewConfig, false /* force */);
            uassertStatusOKWithContext(status, "Failed to downgrade the replica set config");

            uassertStatusOKWithContext(
                replCoord->awaitConfigCommitment(opCtx, true /* waitForOplogCommitment */),
                "The downgraded replica set config failed to propagate to a majority");
            LOGV2(5042304, "The downgraded replica set config has been propagated to a majority");
        }

        {
            // Take the global lock in S mode to create a barrier for operations taking the global
            // IX or X locks. This ensures that either
            //   - The global IX/X locked operation will start after the FCV change, see the
            //     downgrading to the last-lts or last-continuous FCV and act accordingly.
            //   - The global IX/X locked operation began prior to the FCV change, is acting on that
            //     assumption and will finish before downgrade procedures begin right after this.
            Lock::GlobalLock lk(opCtx, MODE_S);
        }

        uassert(ErrorCodes::Error(549181),
                "Failing upgrade due to 'failDowngrading' failpoint set",
                !failDowngrading.shouldFail());

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            // TODO SERVER-53283: This block can removed once 5.0 becomes last-lts.
            // TODO SERVER-53774: Replace kLatest by the version defined in the feature flag IDL
            if (requestedVersion < FeatureCompatibility::kLatest) {
                ShardingCatalogManager::get(opCtx)->downgradeMetadataToPre50Phase1(opCtx);
            }

            // Tell the shards to enter phase-2 of setFCV (fully downgraded)
            auto requestPhase2 = request;
            requestPhase2.setPhase(SetFCVPhaseEnum::kComplete);
            uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                    opCtx, CommandHelpers::appendMajorityWriteConcern(requestPhase2.toBSON({}))));

            // TODO SERVER-53283: This block can removed once 5.0 becomes last-lts.
            // TODO SERVER-53774: Replace kLatest by the version defined in the feature flag IDL
            if (requestedVersion < FeatureCompatibility::kLatest) {
                ShardingCatalogManager::get(opCtx)->downgradeMetadataToPre50Phase2(opCtx);
            }
        }

        hangWhileDowngrading.pauseWhileSet(opCtx);

        if (request.getDowngradeOnDiskChanges()) {
            invariant(requestedVersion == FeatureCompatibility::kLastContinuous);
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
              "last_continuous_version"_attr = FCVP::kLastContinuous);
    }

} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
