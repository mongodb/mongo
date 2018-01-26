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
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keys_collection_document.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/client/shard_registry.h"
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
        : BasicCommand(FeatureCompatibilityVersion::kCommandName) {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(std::stringstream& help) const {
        help << "Set the API version exposed by this node. If set to \""
             << FeatureCompatibilityVersionCommandParser::kVersion34
             << "\", then 3.6 features are disabled. If \""
             << FeatureCompatibilityVersionCommandParser::kVersion36
             << "\", then 3.6 features are enabled, and all nodes in the cluster must be version "
                "3.6. See "
             << feature_compatibility_version::kDochubLink << ".";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(
                    NamespaceString("$setFeatureCompatibilityVersion.version")),
                ActionType::update)) {
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
            Command::appendCommandWCStatus(result, waitForWCStatus, res);
        });

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        Lock::ExclusiveLock lk(opCtx->lockState(), FeatureCompatibilityVersion::fcvLock);

        const auto requestedVersion = uassertStatusOK(
            FeatureCompatibilityVersionCommandParser::extractVersionFromCommand(getName(), cmdObj));

        if (requestedVersion == FeatureCompatibilityVersionCommandParser::kVersion36) {
            uassert(ErrorCodes::IllegalOperation,
                    "cannot initiate featureCompatibilityVersion upgrade while a previous "
                    "featureCompatibilityVersion downgrade has not completed",
                    serverGlobalParams.featureCompatibility.getVersion() !=
                        ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo34);

            if (serverGlobalParams.featureCompatibility.getVersion() ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36) {
                // Set the client's last opTime to the system last opTime so no-ops wait for
                // writeConcern.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            FeatureCompatibilityVersion::setTargetUpgrade(opCtx);

            {
                // Take the global lock in S mode to create a barrier for createCollection
                // execution, which takes a database exclusive lock. This ensures that either
                //   - createCollection will start after the FCV change and see the upgrading to 3.6
                //     FCV and create the collection with a UUID.
                //   - createCollection that began prior to the FCV change, and is creating a
                //     collection without UUID, will finish before the updateUUIDSchemaVersion call
                //     below fetches a list of collections to which to add UUIDs for any without a
                //     UUID.
                // Otherwise createCollection may determine not to add a UUID before the FCV change,
                // but then actually create the collection after the update below identifies all of
                // the databases to update with UUIDs.
                Lock::GlobalLock lk(opCtx, MODE_S, UINT_MAX);
            }

            // First put UUIDs in the storage layer metadata. UUIDs will be generated for unsharded
            // collections; shards will query the config server for sharded collection UUIDs.
            // Remove after 3.4 -> 3.6 upgrade.
            updateUUIDSchemaVersion(opCtx, /*upgrade*/ true);

            // If config server, upgrade shards *after* upgrading self.
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                // Remove after 3.4 -> 3.6 upgrade.
                ShardingCatalogManager::get(opCtx)->generateUUIDsForExistingShardedCollections(
                    opCtx);

                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx,
                        Command::appendMajorityWriteConcern(Command::appendPassthroughFields(
                            cmdObj,
                            BSON(FeatureCompatibilityVersion::kCommandName << requestedVersion)))));
            }

            if (ShardingState::get(opCtx)->enabled()) {
                // Ensure we try reading the keys for signing clusterTime immediately on upgrade.
                // Remove after 3.4 -> 3.6 upgrade.
                LogicalTimeValidator::get(opCtx)->forceKeyRefreshNow(opCtx);
            }

            // Fail after adding UUIDs but before updating the FCV document.
            if (MONGO_FAIL_POINT(featureCompatibilityUpgrade)) {
                exitCleanly(EXIT_CLEAN);
            }

            FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(opCtx, requestedVersion);
        } else {
            invariant(requestedVersion == FeatureCompatibilityVersionCommandParser::kVersion34);

            uassert(ErrorCodes::IllegalOperation,
                    "cannot initiate featureCompatibilityVersion downgrade while a previous "
                    "featureCompatibilityVersion upgrade has not completed",
                    serverGlobalParams.featureCompatibility.getVersion() !=
                        ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo36);

            if (serverGlobalParams.featureCompatibility.getVersion() ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34) {
                // Set the client's last opTime to the system last opTime so no-ops wait for
                // writeConcern.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            FeatureCompatibilityVersion::setTargetDowngrade(opCtx);

            {
                // Take the global lock in S mode to create a barrier for createCollection
                // execution, which takea a database exclusive lock. This ensures that either
                //   - createCollection will start after the FCV change, see downgrading to 3.4 FCV
                //     and not create the collection with a UUID.
                //   - createCollection that began prior to the FCV change, and is creating a
                //     collection with UUID, will finish before the updateUUIDSchemaVersion call
                //     below fetches a list of collections from which to remove UUIDs for any with
                //     a UUID.
                // Otherwise createCollection may determine to add a UUID before the FCV change, but
                // then actually create the collection after the update below identifies all of the
                // databases from which to remove UUIDs.
                Lock::GlobalLock lk(opCtx, MODE_S, UINT_MAX);
            }

            // Fail after updating the FCV document but before removing UUIDs.
            if (MONGO_FAIL_POINT(featureCompatibilityDowngrade)) {
                exitCleanly(EXIT_CLEAN);
            }

            // If config server, downgrade shards *before* downgrading self; and clear the keys
            // collection to ensure all shards will have the same key information on re-upgrade.
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx,
                        Command::appendMajorityWriteConcern(Command::appendPassthroughFields(
                            cmdObj,
                            BSON(FeatureCompatibilityVersion::kCommandName << requestedVersion)))));

                // Stop the background key generator thread from running before trying to drop the
                // collection so we know the key won't just be recreated.
                //
                // Note: no need to restart the key generator if downgrade doesn't succeed at once
                // because downgrade must be completed before upgrade is allowed; and clusterTime
                // auth is inactive unless fully upgraded.
                LogicalTimeValidator::get(opCtx)->enableKeyGenerator(opCtx, false);

                DBDirectClient client(opCtx);
                BSONObj result;
                if (!client.dropCollection(NamespaceString::kSystemKeysCollectionName.toString(),
                                           ShardingCatalogClient::kMajorityWriteConcern,
                                           &result)) {
                    Status status = getStatusFromCommandResult(result);
                    if (status != ErrorCodes::NamespaceNotFound) {
                        uassertStatusOK(status);
                    }
                }
            }

            // Remove after 3.6 -> 3.4 downgrade.
            updateUUIDSchemaVersion(opCtx, /*upgrade*/ false);

            FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(opCtx, requestedVersion);
        }

        return true;
    }

} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
