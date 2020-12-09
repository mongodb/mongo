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
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using FCVP = FeatureCompatibilityVersionParser;
using FeatureCompatibilityParams = ServerGlobalParams::FeatureCompatibility;

namespace {

MONGO_FAIL_POINT_DEFINE(failUpgrading);
MONGO_FAIL_POINT_DEFINE(hangWhileUpgrading);
MONGO_FAIL_POINT_DEFINE(failDowngrading);
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

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
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

        // Ensure that this operation will be killed by the RstlKillOpThread during step-up or
        // stepdown.
        opCtx->setAlwaysInterruptAtStepDownOrUp();

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        invariant(!opCtx->lockState()->isLocked());
        Lock::ExclusiveLock lk(opCtx->lockState(), FeatureCompatibilityVersion::fcvLock);

        auto request = SetFeatureCompatibilityVersion::parse(
            IDLParserErrorContext("setFeatureCompatibilityVersion"), cmdObj);
        const auto requestedVersion = request.getCommandParameter();
        const auto requestedVersionString = FCVP::serializeVersion(requestedVersion);
        FeatureCompatibilityParams::Version actualVersion =
            serverGlobalParams.featureCompatibility.getVersion();
        if (request.getDowngradeOnDiskChanges() &&
            (requestedVersion != FeatureCompatibilityParams::kLastContinuous ||
             actualVersion < requestedVersion)) {
            std::stringstream downgradeOnDiskErrorSS;
            downgradeOnDiskErrorSS
                << "cannot set featureCompatibilityVersion to " << requestedVersionString
                << " with '" << SetFeatureCompatibilityVersion::kDowngradeOnDiskChangesFieldName
                << "' set to true. This is only allowed when downgrading to "
                << FCVP::kLastContinuous;
            uasserted(ErrorCodes::IllegalOperation, downgradeOnDiskErrorSS.str());
        }

        if (actualVersion == requestedVersion) {
            // Set the client's last opTime to the system last opTime so no-ops wait for
            // writeConcern.
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
            return true;
        }

        auto isFromConfigServer = request.getFromConfigServer().value_or(false);
        FeatureCompatibilityVersion::validateSetFeatureCompatibilityVersionRequest(
            actualVersion, requestedVersion, isFromConfigServer);

        if (actualVersion < requestedVersion) {
            checkInitialSyncFinished(opCtx);

            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx, actualVersion, requestedVersion, isFromConfigServer);
            {
                // Take the global lock in S mode to create a barrier for operations taking the
                // global IX or X locks. This ensures that either
                //   - The global IX/X locked operation will start after the FCV change, see the
                //     upgrading to the latest FCV and act accordingly.
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
            }

            // Delete any haystack indexes if we're upgrading to an FCV of 4.9 or higher.
            // TODO SERVER-51871: This block can removed once 5.0 becomes last-lts.
            if (requestedVersion >= FeatureCompatibilityParams::Version::kVersion49) {
                _deleteHaystackIndexesOnUpgrade(opCtx);
            }

            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                if (requestedVersion >= FeatureCompatibilityParams::Version::kVersion49) {
                    // SERVER-52630: Remove once 5.0 becomes the LastLTS
                    ShardingCatalogManager::get(opCtx)->removePre49LegacyMetadata(opCtx);
                }

                // Upgrade shards before config finishes its upgrade.
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx, CommandHelpers::appendMajorityWriteConcern(request.toBSON({}))));

                // Amend metadata created before FCV 4.9. This must be done after all shards have
                // been upgraded to 4.9 in order to guarantee that when the metadata is amended, no
                // new databases or collections with the old version of the metadata will be added.
                // TODO SERVER-53283: Remove once 5.0 has been released.
                if (requestedVersion >= FeatureCompatibilityParams::Version::kVersion49) {
                    try {
                        ShardingCatalogManager::get(opCtx)->upgradeMetadataFor49(opCtx);
                    } catch (const DBException& e) {
                        LOGV2(5276708,
                              "Failed to upgrade sharding metadata: {error}",
                              "error"_attr = e.toString());
                        throw;
                    }
                }
            }

            hangWhileUpgrading.pauseWhileSet(opCtx);
            // Completed transition to requestedVersion.
            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx,
                serverGlobalParams.featureCompatibility.getVersion(),
                requestedVersion,
                isFromConfigServer);
        } else {
            // Time-series collections are only supported in 5.0. If the user tries to downgrade the
            // cluster to an earlier version, they must first remove all time-series collections.
            for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
                auto viewCatalog = DatabaseHolder::get(opCtx)->getViewCatalog(opCtx, dbName);
                if (!viewCatalog) {
                    continue;
                }
                viewCatalog->iterate(opCtx, [](const ViewDefinition& view) {
                    uassert(ErrorCodes::CannotDowngrade,
                            str::stream()
                                << "Cannot downgrade the cluster when there are time-series "
                                   "collections present; drop all time-series collections before "
                                   "downgrading. First detected time-series collection: "
                                << view.name(),
                            !view.timeseries());
                });
            }

            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const bool isReplSet =
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;

            checkInitialSyncFinished(opCtx);

            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx, actualVersion, requestedVersion, isFromConfigServer);

            {
                // Take the global lock in S mode to create a barrier for operations taking the
                // global IX or X locks. This ensures that either
                //   - The global IX/X locked operation will start after the FCV change, see the
                //     downgrading to 4.4 FCV and act accordingly.
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

                if (requestedVersion < FeatureCompatibilityParams::Version::kVersion49) {
                    // SERVER-52632: Remove once 5.0 becomes the LastLTS
                    shardmetadatautil::downgradeShardConfigDatabasesEntriesToPre49(opCtx);
                    shardmetadatautil::downgradeShardConfigCollectionEntriesToPre49(opCtx);
                }


            } else if (isReplSet || serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                // The default rwc document should only be deleted on plain replica sets and the
                // config server replica set, not on shards or standalones.
                deletePersistedDefaultRWConcernDocument(opCtx);
            }

            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                // Downgrade shards before config finishes its downgrade.
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx, CommandHelpers::appendMajorityWriteConcern(request.toBSON({}))));

                // Amend metadata created in FCV 4.9. This must be done after all shards have
                // been downgraded to prior 4.9 in order to guarantee that when the metadata is
                // amended, no new databases or collections with the new version of the metadata
                // will be added.
                // TODO SERVER-53283: Remove once 5.0 has been released.
                if (requestedVersion < FeatureCompatibilityParams::Version::kVersion49) {
                    try {
                        ShardingCatalogManager::get(opCtx)->downgradeMetadataToPre49(opCtx);
                    } catch (const DBException& e) {
                        LOGV2(5276709,
                              "Failed to downgrade sharding metadata: {error}",
                              "error"_attr = e.toString());
                        throw;
                    }
                }
            }

            hangWhileDowngrading.pauseWhileSet(opCtx);
            // Completed transition to requestedVersion.
            FeatureCompatibilityVersion::updateFeatureCompatibilityVersionDocument(
                opCtx,
                serverGlobalParams.featureCompatibility.getVersion(),
                requestedVersion,
                isFromConfigServer);

            if (request.getDowngradeOnDiskChanges()) {
                invariant(requestedVersion == FeatureCompatibilityParams::kLastContinuous);
                _downgradeOnDiskChanges();
                LOGV2(4875603, "Downgrade of on-disk format complete.");
            }
        }

        return true;
    }

private:
    /*
     * Rolls back any upgraded on-disk changes to reflect the disk format of the last-continuous
     * version.
     */
    void _downgradeOnDiskChanges() {
        LOGV2(4975602,
              "Downgrading on-disk format to reflect the last-continuous version.",
              "last_continuous_version"_attr = FCVP::kLastContinuous);
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

                // Construct a dropIndexes command to drop the indexes in 'haystackIndexes'.
                BSONObjBuilder dropIndexesCmd;
                dropIndexesCmd.append("dropIndexes", collName.nss()->coll());
                BSONArrayBuilder indexNames;
                for (auto&& haystackIndex : haystackIndexes) {
                    indexNames.append(haystackIndex->indexName());
                }
                dropIndexesCmd.append("index", indexNames.arr());

                BSONObjBuilder response;  // This response is ignored.
                uassertStatusOK(
                    dropIndexes(opCtx,
                                *collName.nss(),
                                CommandHelpers::appendMajorityWriteConcern(dropIndexesCmd.obj()),
                                &response));
            }
        }
    }

} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
