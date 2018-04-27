/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_command_parser.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

MONGO_FP_DECLARE(featureCompatibilityDowngrade);
MONGO_FP_DECLARE(featureCompatibilityUpgrade);
/**
 * Sets the minimum allowed version for the cluster. If it is 3.4, then the node should not use 3.6
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
        std::stringstream h;
        h << "Set the API version exposed by this node. If set to \""
          << FeatureCompatibilityVersionParser::kVersion36
          << "\", then 4.0 features are disabled. If \""
          << FeatureCompatibilityVersionParser::kVersion40
          << "\", then 4.0 features are enabled, and all nodes in the cluster must be binary "
             "version 4.0. See "
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
                WriteConcernOptions(
                    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, timeout),
                &res);
            CommandHelpers::appendCommandWCStatus(result, waitForWCStatus, res);
        });

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        invariant(!opCtx->lockState()->isLocked());
        Lock::ExclusiveLock lk(opCtx->lockState(), FeatureCompatibilityVersion::fcvLock);

        const auto requestedVersion = uassertStatusOK(
            FeatureCompatibilityVersionCommandParser::extractVersionFromCommand(getName(), cmdObj));
        ServerGlobalParams::FeatureCompatibility::Version actualVersion =
            serverGlobalParams.featureCompatibility.getVersion();

        if (requestedVersion == FeatureCompatibilityVersionParser::kVersion40) {
            uassert(ErrorCodes::IllegalOperation,
                    "cannot initiate featureCompatibilityVersion upgrade to 4.0 while a previous "
                    "featureCompatibilityVersion downgrade to 3.6 has not completed. Finish "
                    "downgrade to 3.6, then upgrade to 4.0.",
                    actualVersion !=
                        ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo36);

            if (actualVersion ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
                // Set the client's last opTime to the system last opTime so no-ops wait for
                // writeConcern.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            FeatureCompatibilityVersion::setTargetUpgrade(opCtx);

            {
                // Take the global lock in S mode to create a barrier for operations taking the
                // global IX or X locks. This ensures that either
                //   - The global IX/X locked operation will start after the FCV change, see the
                //     upgrading to 4.0 FCV and act accordingly.
                //   - The global IX/X locked operation began prior to the FCV change, is acting on
                //     that assumption and will finish before upgrade procedures begin right after
                //     this.
                Lock::GlobalLock lk(opCtx, MODE_S);
            }

            updateUniqueIndexesOnUpgrade(opCtx);

            // Upgrade shards before config finishes its upgrade.
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                auto allDbs = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getAllDBs(
                                                  opCtx, repl::ReadConcernLevel::kLocalReadConcern))
                                  .value;

                // The 'config' dataabase contains the sharded 'config.system.sessions' collection,
                // but does not have an entry in config.databases.
                allDbs.emplace_back("config", ShardId("config"), true);

                auto clusterTime = LogicalClock::get(opCtx)->getClusterTime().asTimestamp();
                for (const auto& db : allDbs) {
                    const auto dbVersion = databaseVersion::makeNew();

                    uassertStatusOK(Grid::get(opCtx)->catalogClient()->updateConfigDocument(
                        opCtx,
                        DatabaseType::ConfigNS,
                        BSON(DatabaseType::name(db.getName())),
                        BSON("$set" << BSON(DatabaseType::version(dbVersion.toBSON()))),
                        false,
                        ShardingCatalogClient::kLocalWriteConcern));

                    // Enumerate all collections
                    auto collections =
                        uassertStatusOK(Grid::get(opCtx)->catalogClient()->getCollections(
                            opCtx,
                            &db.getName(),
                            nullptr,
                            repl::ReadConcernLevel::kLocalReadConcern));

                    for (const auto& coll : collections) {
                        if (!coll.getDropped()) {
                            uassertStatusOK(
                                ShardingCatalogManager::get(opCtx)->upgradeChunksHistory(
                                    opCtx, coll.getNs(), coll.getEpoch(), clusterTime));
                        }
                    }
                }

                // Now that new metadata are written out to disk flush the local in memory state.
                Grid::get(opCtx)->catalogCache()->purgeAllDatabases();

                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx,
                        CommandHelpers::appendMajorityWriteConcern(
                            CommandHelpers::appendPassthroughFields(
                                cmdObj,
                                BSON(FeatureCompatibilityVersionCommandParser::kCommandName
                                     << requestedVersion)))));
            }

            FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(opCtx, requestedVersion);
        } else if (requestedVersion == FeatureCompatibilityVersionParser::kVersion36) {
            uassert(ErrorCodes::IllegalOperation,
                    "cannot initiate setting featureCompatibilityVersion to 3.6 while a previous "
                    "featureCompatibilityVersion upgrade to 4.0 has not completed.",
                    actualVersion !=
                        ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40);

            if (actualVersion ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo36) {
                // Set the client's last opTime to the system last opTime so no-ops wait for
                // writeConcern.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            FeatureCompatibilityVersion::setTargetDowngrade(opCtx);

            {
                // Take the global lock in S mode to create a barrier for operations taking the
                // global IX or X locks. This ensures that either
                //   - The global IX/X locked operation will start after the FCV change, see the
                //     downgrading to 3.6 FCV and act accordingly.
                //   - The global IX/X locked operation began prior to the FCV change, is acting on
                //     that assumption and will finish before downgrade procedures begin right after
                //     this.
                Lock::GlobalLock lk(opCtx, MODE_S);
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

                auto allDbs = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getAllDBs(
                                                  opCtx, repl::ReadConcernLevel::kLocalReadConcern))
                                  .value;

                // The 'config' dataabase contains the sharded 'config.system.sessions' collection,
                // but does not have an entry in config.databases.
                allDbs.emplace_back("config", ShardId("config"), true);

                for (const auto& db : allDbs) {
                    uassertStatusOK(Grid::get(opCtx)->catalogClient()->updateConfigDocument(
                        opCtx,
                        DatabaseType::ConfigNS,
                        BSON(DatabaseType::name(db.getName())),
                        BSON("$unset" << BSON("version"
                                              << "")),
                        false,
                        ShardingCatalogClient::kLocalWriteConcern));

                    // Enumerate all collections
                    auto collections =
                        uassertStatusOK(Grid::get(opCtx)->catalogClient()->getCollections(
                            opCtx,
                            &db.getName(),
                            nullptr,
                            repl::ReadConcernLevel::kLocalReadConcern));

                    for (const auto& coll : collections) {
                        if (!coll.getDropped()) {
                            uassertStatusOK(
                                ShardingCatalogManager::get(opCtx)->downgradeChunksHistory(
                                    opCtx, coll.getNs(), coll.getEpoch()));
                        }
                    }
                }
                // Now that new metadata are written out to disk flush the local in memory state.
                Grid::get(opCtx)->catalogCache()->purgeAllDatabases();
            }

            FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(opCtx, requestedVersion);
        }

        return true;
    }

} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
