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

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_databases_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

class ListDatabasesCmd final : public ListDatabasesCmdVersion1Gen<ListDatabasesCmd> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const final {}

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        ListDatabasesReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            auto* as = AuthorizationSession::get(opCtx->getClient());
            auto cmd = request();

            // { nameOnly: bool } - Default false.
            const bool nameOnly = cmd.getNameOnly();

            // { authorizedDatabases: bool } - Dynamic default based on perms.
            const bool authorizedDatabases =
                ([as, tenantId = cmd.getDbName().tenantId()](const boost::optional<bool>& authDB) {
                    const bool mayListAllDatabases = as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(tenantId), ActionType::listDatabases);
                    if (authDB) {
                        uassert(ErrorCodes::Unauthorized,
                                "Insufficient permissions to list all databases",
                                authDB.value() || mayListAllDatabases);
                        return authDB.value();
                    }

                    // By default, list all databases if we can, otherwise
                    // only those we're allowed to find on.
                    return !mayListAllDatabases;
                })(cmd.getAuthorizedDatabases());

            auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

            std::map<std::string, long long> sizes;
            std::map<std::string, std::unique_ptr<BSONObjBuilder>> dbShardInfo;

            auto shardIds = shardRegistry->getAllShardIds(opCtx);
            if (std::find(shardIds.begin(), shardIds.end(), ShardId::kConfigServerId) ==
                shardIds.end()) {
                // The config server may be a shard, so only add if it isn't already in shardIds.
                shardIds.emplace_back(ShardId::kConfigServerId);
            }

            // { filter: matchExpression }.
            auto filteredCmd = applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON({})));

            for (const ShardId& shardId : shardIds) {
                auto shardStatus = shardRegistry->getShard(opCtx, shardId);
                if (!shardStatus.isOK()) {
                    continue;
                }
                const auto s = std::move(shardStatus.getValue());

                auto response = uassertStatusOK(
                    s->runCommandWithFixedRetryAttempts(opCtx,
                                                        ReadPreferenceSetting::get(opCtx),
                                                        DatabaseName::kAdmin,
                                                        filteredCmd,
                                                        Shard::RetryPolicy::kIdempotent));
                uassertStatusOK(response.commandStatus);
                BSONObj x = std::move(response.response);

                BSONObjIterator j(x["databases"].Obj());
                while (j.more()) {
                    BSONObj dbObj = j.next().Obj();

                    const auto name = dbObj["name"].String();

                    // If this is the admin db, only collect its stats from the config servers.
                    if (name == "admin" && !s->isConfig()) {
                        continue;
                    }

                    const long long size = dbObj["sizeOnDisk"].numberLong();

                    long long& sizeSumForDbAcrossShards = sizes[name];
                    if (size == 1) {
                        if (sizeSumForDbAcrossShards <= 1) {
                            sizeSumForDbAcrossShards = 1;
                        }
                    } else {
                        sizeSumForDbAcrossShards += size;
                    }

                    auto& bb = dbShardInfo[name];
                    if (!bb) {
                        bb.reset(new BSONObjBuilder());
                    }

                    bb->append(s->getId().toString(), size);
                }
            }

            // Now that we have aggregated results for all the shards, convert to a response,
            // and compute total sizes.
            long long totalSize = 0;
            std::vector<ListDatabasesReplyItem> items;
            const auto& tenantId = cmd.getDbName().tenantId();
            for (const auto& sizeEntry : sizes) {
                const auto dbname = DatabaseNameUtil::deserialize(
                    tenantId, sizeEntry.first, cmd.getSerializationContext());
                const long long size = sizeEntry.second;

                // Unless this is a listDatabases command on the replica set endpoint (of a
                // single-shard cluster), skip the 'local' database since all shards have their own
                // independent 'local' database.
                if (dbname.isLocalDB() &&
                    (!opCtx->routedByReplicaSetEndpoint() || shardIds.size() > 1)) {
                    continue;
                }

                if (authorizedDatabases && !as->isAuthorizedForAnyActionOnAnyResourceInDB(dbname)) {
                    // We don't have listDatabases on the cluser or find on this database.
                    continue;
                }

                ListDatabasesReplyItem item(sizeEntry.first);
                if (!nameOnly) {
                    item.setSizeOnDisk(size);
                    item.setEmpty(size == 1);
                    item.setShards(dbShardInfo[sizeEntry.first]->obj());

                    uassert(ErrorCodes::BadValue,
                            str::stream() << "Found negative 'sizeOnDisk' in: "
                                          << dbname.toStringForErrorMsg(),
                            size >= 0);

                    totalSize += size;
                }

                items.push_back(std::move(item));
            }

            ListDatabasesReply reply(items);
            if (!nameOnly) {
                reply.setTotalSize(totalSize);
                reply.setTotalSizeMb(totalSize / (1024 * 1024));
            }

            return reply;
        }
    };
};
MONGO_REGISTER_COMMAND(ListDatabasesCmd).forRouter();

}  // namespace
}  // namespace mongo
