/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <list>
#include <set>
#include <vector>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_write.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

class ShardCollectionCmd : public BasicCommand {
public:
    ShardCollectionCmd() : BasicCommand("shardCollection", "shardcollection") {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "Shard a collection. Requires key. Optional unique."
             << " Sharding must already be enabled for the database.\n"
             << "   { enablesharding : \"<dbname>\" }\n";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::enableSharding)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        auto shardCollRequest =
            ShardCollection::parse(IDLParserErrorContext("ShardCollection"), cmdObj);

        ConfigsvrShardCollectionRequest configShardCollRequest;
        configShardCollRequest.set_configsvrShardCollection(nss);
        configShardCollRequest.setKey(shardCollRequest.getKey());
        configShardCollRequest.setUnique(shardCollRequest.getUnique());
        configShardCollRequest.setNumInitialChunks(shardCollRequest.getNumInitialChunks());
        configShardCollRequest.setCollation(shardCollRequest.getCollation());

        // Invalidate the routing table cache entry for this collection so that we reload the
        // collection the next time it's accessed, even if we receive a failure, e.g. NetworkError.
        ON_BLOCK_EXIT(
            [opCtx, nss] { Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss); });

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            Command::appendMajorityWriteConcern(
                Command::appendPassthroughFields(cmdObj, configShardCollRequest.toBSON())),
            Shard::RetryPolicy::kIdempotent));

        Command::filterCommandReplyForPassthrough(cmdResponse.response, &result);
        return true;
    }

} shardCollectionCmd;

}  // namespace
}  // namespace mongo
