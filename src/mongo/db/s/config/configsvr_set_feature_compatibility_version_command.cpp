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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_command_parser.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace mongo {

using CollectionUUID = UUID;

namespace {

/**
 * Internal sharding command run on config servers to set featureCompatibilityVersion on all shards.
 *
 * Format:
 * {
 *   _configsvrSetFeatureCompatibilityVersion: <string version>
 * }
 */
class ConfigSvrSetFeatureCompatibilityVersionCommand : public BasicCommand {
public:
    ConfigSvrSetFeatureCompatibilityVersionCommand()
        : BasicCommand("_configsvrSetFeatureCompatibilityVersion") {}

    void help(std::stringstream& help) const override {
        help << "Internal command, which is exported by the sharding config server. Do not call "
                "directly. Sets featureCompatibilityVersion on all shards. See "
             << feature_compatibility_version::kDochubLink << ".";
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& unusedDbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto version = uassertStatusOK(
            FeatureCompatibilityVersionCommandParser::extractVersionFromCommand(getName(), cmdObj));

        uassert(ErrorCodes::IllegalOperation,
                str::stream() << getName() << " can only be run on config servers. See "
                              << feature_compatibility_version::kDochubLink
                              << ".",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

        // Remove after 3.4 -> 3.6 upgrade.
        if (version == FeatureCompatibilityVersionCommandParser::kVersion36) {
            _generateUUIDsForExistingShardedCollections(opCtx);
        }

        // Forward to all shards.
        uassertStatusOK(ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
            opCtx, version));

        // On success, set featureCompatibilityVersion on self.
        FeatureCompatibilityVersion::set(opCtx, version);

        return true;
    }

private:
    /**
     * Iterates through each entry in config.collections that does not have a UUID, generates a UUID
     * for the collection, and updates the entry with the generated UUID.
     *
     * Remove after 3.4 -> 3.6 upgrade.
     */
    void _generateUUIDsForExistingShardedCollections(OperationContext* opCtx) {
        // Retrieve all collections in config.collections that do not have a UUID. Some collections
        // may already have a UUID if an earlier upgrade attempt failed after making some progress.
        auto shardedColls =
            uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    repl::ReadConcernLevel::kLocalReadConcern,
                    NamespaceString(CollectionType::ConfigNS),
                    BSON(CollectionType::uuid.name() << BSON("$exists" << false)),  // query
                    BSONObj(),                                                      // sort
                    boost::none                                                     // limit
                    ))
                .docs;

        // Generate and persist a new UUID for each collection that did not have a UUID.
        LOG(0) << "generating UUIDs for all sharded collections that do not yet have one";
        for (auto& coll : shardedColls) {
            auto collType = uassertStatusOK(CollectionType::fromBSON(coll));
            invariant(!collType.getUUID());

            auto uuid = CollectionUUID::gen();
            collType.setUUID(uuid);

            uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
                opCtx, collType.getNs().ns(), collType, false /* upsert */));
            LOG(2) << "updated entry in config.collections for sharded collection "
                   << collType.getNs() << " with generated UUID " << uuid;
        }
    }
} configsvrSetFeatureCompatibilityVersionCmd;

}  // namespace
}  // namespace mongo
