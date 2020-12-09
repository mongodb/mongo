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
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_command_parser.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/active_shard_collection_registry.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(featureCompatibilityDowngrade);
MONGO_FAIL_POINT_DEFINE(featureCompatibilityUpgrade);
MONGO_FAIL_POINT_DEFINE(pauseBeforeDowngradingConfigMetadata);  // TODO SERVER-44034: Remove.
MONGO_FAIL_POINT_DEFINE(pauseBeforeUpgradingConfigMetadata);    // TODO SERVER-44034: Remove.
MONGO_FAIL_POINT_DEFINE(failUpgrading);
MONGO_FAIL_POINT_DEFINE(hangWhileUpgrading);
MONGO_FAIL_POINT_DEFINE(failDowngrading);
MONGO_FAIL_POINT_DEFINE(allowFCVDowngradeWithCompoundHashedShardKey);
MONGO_FAIL_POINT_DEFINE(hangWhileDowngrading);

/**
 * Deletes the persisted default read/write concern document.
 */
void deletePersistedDefaultRWConcernDocument(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    const auto commandResponse = client.runCommand([&] {
        write_ops::Delete deleteOp(NamespaceString::kConfigSettingsNamespace);
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

/**
 * Sets the minimum allowed version for the cluster. If it is 4.2, then the node should not use 4.4
 * features.
 *
 * Format:
 * {
 *   setFeatureCompatibilityVersion: <string version>
 * }
 */
class SetFeatureCompatibilityVersionCommand : public BasicCommand {
public:
    SetFeatureCompatibilityVersionCommand()
        : BasicCommand(FeatureCompatibilityVersionCommandParser::kCommandName) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        using FCVP = FeatureCompatibilityVersionParser;
        std::stringstream h;
        h << "Set the API version exposed by this node. If set to '" << FCVP::kVersion42
          << "', then " << FCVP::kVersion44 << " features are disabled. If set to '"
          << FCVP::kVersion44 << "', then " << FCVP::kVersion44
          << " features are enabled, and all nodes in the cluster must be binary version "
          << FCVP::kVersion44 << ". See "
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
             BSONObjBuilder& result) {
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

        {
            // Acquire the global IX lock and then immediately release it to ensure this operation
            // will be killed by the RstlKillOpThread during step-up or stepdown. Note that the
            // RstlKillOpThread kills any operations on step-up or stepdown for which
            // Locker::wasGlobalLockTakenInModeConflictingWithWrites() returns true.
            Lock::GlobalLock lk(opCtx, MODE_IX);
        }

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        invariant(!opCtx->lockState()->isLocked());
        Lock::ExclusiveLock lk(opCtx->lockState(), FeatureCompatibilityVersion::fcvLock);

        MigrationBlockingGuard migrationBlockingGuard(opCtx, "setFeatureCompatibilityVersion");

        const auto requestedVersion = uassertStatusOK(
            FeatureCompatibilityVersionCommandParser::extractVersionFromCommand(getName(), cmdObj));
        ServerGlobalParams::FeatureCompatibility::Version actualVersion =
            serverGlobalParams.featureCompatibility.getVersion();

        if (requestedVersion == FeatureCompatibilityVersionParser::kVersion44) {
            uassert(ErrorCodes::IllegalOperation,
                    "cannot initiate featureCompatibilityVersion upgrade to 4.4 while a previous "
                    "featureCompatibilityVersion downgrade to 4.2 has not completed. Finish "
                    "downgrade to 4.2, then upgrade to 4.4.",
                    actualVersion !=
                        ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo42);

            if (actualVersion ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {
                // Set the client's last opTime to the system last opTime so no-ops wait for
                // writeConcern.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            FeatureCompatibilityVersion::setTargetUpgrade(opCtx);

            // Force reconfig to add a 'term' field to the config.
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const bool isReplSet =
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
            if (isReplSet) {
                auto getNewConfig = [&](const repl::ReplSetConfig& oldConfig, long long term) {
                    auto newConfig = oldConfig;
                    newConfig.setConfigTerm(term);
                    newConfig.setConfigVersion(newConfig.getConfigVersion() + 1);
                    return newConfig;
                };

                // "force" reconfig in order to skip safety checks. This is safe since the content
                // of config is the same.
                LOGV2(4718900, "Upgrading replica set config.");
                auto status = replCoord->doReplSetReconfig(opCtx, getNewConfig, true /* force */);
                uassertStatusOKWithContext(status, "Failed to upgrade the replica set config");

                LOGV2(4718901,
                      "Waiting for the upgraded replica set config to propagate to a majority");
                // If a write concern is given, we'll use its wTimeout. It's kNoTimeout by default.
                WriteConcernOptions writeConcern(
                    repl::ReplSetConfig::kConfigMajorityWriteConcernModeName,
                    WriteConcernOptions::SyncMode::NONE,
                    opCtx->getWriteConcern().wTimeout);
                writeConcern.checkCondition = WriteConcernOptions::CheckCondition::Config;
                repl::OpTime fakeOpTime(Timestamp(1, 1), replCoord->getTerm());
                uassertStatusOKWithContext(
                    replCoord->awaitReplication(opCtx, fakeOpTime, writeConcern).status,
                    "Failed to wait for the upgraded replica set config to propagate to a "
                    "majority");
                LOGV2(4718902, "The upgraded replica set config has been propagated to a majority");
            }

            {
                // Take the global lock in S mode to create a barrier for operations taking the
                // global IX or X locks. This ensures that either
                //   - The global IX/X locked operation will start after the FCV change, see the
                //     upgrading to 4.4 FCV and act accordingly.
                //   - The global IX/X locked operation began prior to the FCV change, is acting on
                //     that assumption and will finish before upgrade procedures begin right after
                //     this.
                Lock::GlobalLock lk(opCtx, MODE_S);
            }

            if (failUpgrading.shouldFail())
                return false;

            if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
                const auto shardingState = ShardingState::get(opCtx);
                if (shardingState->enabled()) {
                    LOGV2(20500, "Upgrade: submitting orphaned ranges for cleanup");
                    migrationutil::submitOrphanRangesForCleanup(opCtx);
                }

                // The primary shard sharding a collection will write the initial chunks for a
                // collection directly to the config server, so wait for all shard collections to
                // complete to guarantee no chunks are missed by the update on the config server.
                ActiveShardCollectionRegistry::get(opCtx).waitForActiveShardCollectionsToComplete(
                    opCtx);
            }

            // Upgrade shards before config finishes its upgrade.
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx,
                        CommandHelpers::appendMajorityWriteConcern(
                            CommandHelpers::appendPassthroughFields(
                                cmdObj,
                                BSON(FeatureCompatibilityVersionCommandParser::kCommandName
                                     << requestedVersion)))));

                if (MONGO_unlikely(pauseBeforeUpgradingConfigMetadata.shouldFail())) {
                    LOGV2(20501, "Hit pauseBeforeUpgradingConfigMetadata");
                    pauseBeforeUpgradingConfigMetadata.pauseWhileSet(opCtx);
                }
                ShardingCatalogManager::get(opCtx)->upgradeOrDowngradeChunksAndTags(
                    opCtx, ShardingCatalogManager::ConfigUpgradeType::kUpgrade);
            }

            hangWhileUpgrading.pauseWhileSet(opCtx);
            FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(opCtx, requestedVersion);
        } else if (requestedVersion == FeatureCompatibilityVersionParser::kVersion42) {
            uassert(ErrorCodes::IllegalOperation,
                    "cannot initiate setting featureCompatibilityVersion to 4.2 while a previous "
                    "featureCompatibilityVersion upgrade to 4.4 has not completed.",
                    actualVersion !=
                        ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo44);

            if (actualVersion ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo42) {
                // Set the client's last opTime to the system last opTime so no-ops wait for
                // writeConcern.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            // Compound hashed shard keys are only allowed in 4.4. If the user tries to downgrade
            // the cluster to FCV42, they must first drop all the collections with compound hashed
            // shard key. If we find any existing collections, we uassert.
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                const auto grid = Grid::get(opCtx);
                auto allDbs = uassertStatusOK(grid->catalogClient()->getAllDBs(
                                                  opCtx, repl::ReadConcernLevel::kLocalReadConcern))
                                  .value;
                for (const auto& db : allDbs) {
                    auto collections = uassertStatusOK(grid->catalogClient()->getCollections(
                        opCtx, &db.getName(), nullptr, repl::ReadConcernLevel::kLocalReadConcern));
                    for (const auto& coll : collections) {
                        if (coll.getDropped()) {
                            continue;
                        }
                        auto shardKeyPattern = coll.getKeyPattern().toBSON();
                        uassert(31411,
                                str::stream()
                                    << "Cannot downgrade the cluster when there is an existing "
                                       "collection with compound hashed shard key. Please drop the "
                                       "collection "
                                    << coll.getNs() << " and re-initiate the downgrade process",
                                allowFCVDowngradeWithCompoundHashedShardKey.shouldFail() ||
                                    !ShardKeyPattern::extractHashedField(shardKeyPattern) ||
                                    shardKeyPattern.nFields() == 1);
                    }
                }
            }

            // Two phase index builds are only supported in 4.4. If the user tries to downgrade the
            // cluster to FCV42, they must first wait for all index builds to run to completion, or
            // abort the index builds (using the dropIndexes command).
            if (auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx)) {
                auto numIndexBuilds = indexBuildsCoord->getActiveIndexBuildCount(opCtx);
                uassert(
                    ErrorCodes::ConflictingOperationInProgress,
                    str::stream()
                        << "Cannot downgrade the cluster when there are index builds in progress: "
                        << numIndexBuilds,
                    numIndexBuilds == 0U);
            }

            // Cannot downgrade to FCV 4.2 when long collection names are present.
            const std::vector<std::string> dbNames = CollectionCatalog::get(opCtx).getAllDbNames();
            for (const auto& dbName : dbNames) {
                Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
                catalog::forEachCollectionFromDb(
                    opCtx, dbName, MODE_IS, [&](const Collection* collection) {
                        const auto collNss = collection->ns();
                        uassert(ErrorCodes::InvalidNamespace,
                                str::stream()
                                    << "Cannot downgrade the cluster when there are long "
                                    << "collection names present. FCV 4.2 limit: "
                                    << NamespaceString::MaxNSCollectionLenFCV42
                                    << ". Found: " << collNss
                                    << ", but there may be more. Rename or drop the collection",
                                collNss.size() <= NamespaceString::MaxNSCollectionLenFCV42);
                        return true;
                    });
            }


            FeatureCompatibilityVersion::setTargetDowngrade(opCtx);

            // Safe reconfig introduces a new "term" field in the config document. If the user tries
            // to downgrade the replset to FCV42, the primary will initiate a reconfig without the
            // term and wait for it to be replicated on all nodes.
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const bool isReplSet =
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
            if (isReplSet &&
                replCoord->getConfig().getConfigTerm() != repl::OpTime::kUninitializedTerm) {
                // Force reconfig with term -1 to remove the 4.2 incompatible "term" field.
                auto getNewConfig = [&](const repl::ReplSetConfig& oldConfig, long long term) {
                    auto newConfig = oldConfig;
                    newConfig.setConfigTerm(repl::OpTime::kUninitializedTerm);
                    newConfig.setConfigVersion(newConfig.getConfigVersion() + 1);
                    return newConfig;
                };

                // "force" reconfig in order to skip safety checks. This is safe since the content
                // of config is the same.
                LOGV2(4628800, "Downgrading replica set config.");
                auto status = replCoord->doReplSetReconfig(opCtx, getNewConfig, true /* force */);
                uassertStatusOKWithContext(status, "Failed to downgrade the replica set config");

                LOGV2(4628801,
                      "Waiting for the downgraded replica set config to propagate to all nodes");
                // If a write concern is given, we'll use its wTimeout. It's kNoTimeout by default.
                WriteConcernOptions writeConcern(repl::ReplSetConfig::kConfigAllWriteConcernName,
                                                 WriteConcernOptions::SyncMode::NONE,
                                                 opCtx->getWriteConcern().wTimeout);
                writeConcern.checkCondition = WriteConcernOptions::CheckCondition::Config;
                repl::OpTime fakeOpTime(Timestamp(1, 1), replCoord->getTerm());
                uassertStatusOKWithContext(
                    replCoord->awaitReplication(opCtx, fakeOpTime, writeConcern).status,
                    "Failed to wait for the downgraded replica set config to propagate to all "
                    "nodes");
                LOGV2(4628802,
                      "The downgraded replica set config has been propagated to all nodes");
            }

            {
                // Take the global lock in S mode to create a barrier for operations taking the
                // global IX or X locks. This ensures that either
                //   - The global IX/X locked operation will start after the FCV change, see the
                //     downgrading to 4.2 FCV and act accordingly.
                //   - The global IX/X locked operation began prior to the FCV change, is acting on
                //     that assumption and will finish before downgrade procedures begin right after
                //     this.
                Lock::GlobalLock lk(opCtx, MODE_S);
            }

            if (failDowngrading.shouldFail())
                return false;

            if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
                LOGV2(20502, "Downgrade: dropping config.rangeDeletions collection");
                migrationutil::dropRangeDeletionsCollection(opCtx);

                // The primary shard sharding a collection will write the initial chunks for a
                // collection directly to the config server, so wait for all shard collections to
                // complete to guarantee no chunks are missed by the update on the config server.
                ActiveShardCollectionRegistry::get(opCtx).waitForActiveShardCollectionsToComplete(
                    opCtx);
            } else if (isReplSet || serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                // The default rwc document should only be deleted on plain replica sets and the
                // config server replica set, not on shards or standalones.
                deletePersistedDefaultRWConcernDocument(opCtx);
            }

            // Downgrade shards before config finishes its downgrade.
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx,
                        CommandHelpers::appendMajorityWriteConcern(
                            CommandHelpers::appendPassthroughFields(
                                cmdObj,
                                BSON(FeatureCompatibilityVersionCommandParser::kCommandName
                                     << requestedVersion)))));

                if (MONGO_unlikely(pauseBeforeDowngradingConfigMetadata.shouldFail())) {
                    LOGV2(20503, "Hit pauseBeforeDowngradingConfigMetadata");
                    pauseBeforeDowngradingConfigMetadata.pauseWhileSet(opCtx);
                }
                ShardingCatalogManager::get(opCtx)->upgradeOrDowngradeChunksAndTags(
                    opCtx, ShardingCatalogManager::ConfigUpgradeType::kDowngrade);
            }

            hangWhileDowngrading.pauseWhileSet(opCtx);
            FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(opCtx, requestedVersion);
        }

        return true;
    }

} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
