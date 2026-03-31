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
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/list_databases_common.h"
#include "mongo/db/shard_role/ddl/list_databases_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class ListDatabasesCmd final : public ListDatabasesCmdVersion1Gen<ListDatabasesCmd> {
public:
    static constexpr int kMaxAttempts = 3;

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

        struct ShardDbInfo {
            long long size = 0;
            std::unique_ptr<BSONObjBuilder> shardInfo = nullptr;
        };

        static bool shouldIncludeDatabase(const DatabaseName& dbname,
                                          const bool authorizedDatabases,
                                          AuthorizationSession* as) {
            // skip the 'local' database since all shards have their own
            // independent 'local' database.
            if (dbname.isLocalDB()) {
                return false;
            }

            if (authorizedDatabases && !as->isAuthorizedForAnyActionOnAnyResourceInDB(dbname)) {
                // We don't have listDatabases on the cluster or find on this database.
                return false;
            }

            return true;
        }

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const final {}

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        static bool areDatabaseVersionsConsistent(
            const std::vector<DatabaseType>& databasesSnapshotBefore,
            const std::vector<DatabaseType>& databasesSnapshotAfter) {
            std::map<DatabaseName, DatabaseVersion> dbNameToVersionBefore;
            for (const auto& db : databasesSnapshotBefore) {
                if (db.getDbName().isConfigDB() || db.getDbName().isAdminDB()) {
                    continue;
                }
                dbNameToVersionBefore[db.getDbName()] = db.getVersion();
            }

            // For each database in "after" that intersects "before" by name,
            // return false if the version differs.
            for (const auto& db : databasesSnapshotAfter) {
                auto it = dbNameToVersionBefore.find(db.getDbName());
                if (it == dbNameToVersionBefore.end()) {
                    continue;
                }
                const auto& version = db.getVersion();
                const auto& dbVersionBefore = it->second;
                if (dbVersionBefore.getUuid() == version.getUuid() && dbVersionBefore != version) {
                    LOGV2_DEBUG(11571000,
                                4,
                                "Database version update detected",
                                "dbName"_attr = db.getDbName(),
                                "versionBefore"_attr = dbVersionBefore,
                                "versionAfter"_attr = version);
                    return false;
                }
            }

            return true;
        }

        ListDatabasesReply runNameOnly(OperationContext* opCtx,
                                       bool authorizedDatabases,
                                       AuthorizationSession* as,
                                       RequestType& cmd) {
            std::vector<ListDatabasesReplyItem> items;
            std::vector<DatabaseType> databases = Grid::get(opCtx)->catalogClient()->getAllDBs(
                opCtx, repl::ReadConcernLevel::kSnapshotReadConcern);

            std::unique_ptr<MatchExpression> filter = list_databases::getFilter(cmd, opCtx, ns());
            auto addIfMatches = [&](ListDatabasesReplyItem item) {
                if (!filter || exec::matcher::matchesBSON(filter.get(), item.toBSON())) {
                    items.push_back(std::move(item));
                }
            };

            if (shouldIncludeDatabase(DatabaseName::kAdmin, authorizedDatabases, as)) {
                addIfMatches(DatabaseNameUtil::serialize(DatabaseName::kAdmin,
                                                         cmd.getSerializationContext()));
            }
            if (shouldIncludeDatabase(DatabaseName::kConfig, authorizedDatabases, as)) {
                addIfMatches(DatabaseNameUtil::serialize(DatabaseName::kConfig,
                                                         cmd.getSerializationContext()));
            }

            for (const auto& db : databases) {
                if (shouldIncludeDatabase(db.getDbName(), authorizedDatabases, as)) {
                    auto dbname =
                        DatabaseNameUtil::serialize(db.getDbName(), cmd.getSerializationContext());
                    ListDatabasesReplyItem item(dbname);
                    addIfMatches(std::move(item));
                }
            }

            return ListDatabasesReply(items);
        }

        std::map<std::string, ShardDbInfo> getConsistentDbInfoFromShards(OperationContext* opCtx,
                                                                         RequestType& cmd) {

            std::map<std::string, ShardDbInfo> dbShardInfos;

            // { filter: matchExpression }.
            auto filteredCmd = CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON());

            std::vector<DatabaseType> databasesSnapshotBefore =
                Grid::get(opCtx)->catalogClient()->getAllDBs(
                    opCtx, repl::ReadConcernLevel::kSnapshotReadConcern);

            int attempts = 0;
            std::vector<AsyncRequestsSender::Response> responses;
            bool databasesListIsValid = false;
            while (attempts < kMaxAttempts) {
                attempts++;

                responses = scatterGatherUnversionedTargetConfigServerAndShards(
                    opCtx,
                    DatabaseName::kAdmin,
                    filteredCmd,
                    ReadPreferenceSetting::get(opCtx),
                    Shard::RetryPolicy::kIdempotent);

                std::vector<DatabaseType> databasesSnapshotAfter =
                    Grid::get(opCtx)->catalogClient()->getAllDBs(
                        opCtx, repl::ReadConcernLevel::kSnapshotReadConcern);

                // Broadcasting a `listDatabases` command to all shards can miss a database that is
                // being moved via movePrimary if the movePrimary operation runs concurrently with
                // listDatabases. To ensure that the results collected from shards are consistent,
                // we verify that no database versions changed during the listDatabases
                // scatter-gather to the shards. This is achieved by reading the list of databases
                // with their versions from the config server both before and after querying the
                // shards, and retrying if any version change is detected.
                if (areDatabaseVersionsConsistent(databasesSnapshotBefore,
                                                  databasesSnapshotAfter)) {
                    databasesListIsValid = true;
                    break;
                } else {
                    LOGV2_DEBUG(11571001,
                                4,
                                "Database snapshot mismatch detected, retrying listDatabases cmd",
                                "databasesSnapshotBefore"_attr = databasesSnapshotBefore,
                                "databasesSnapshotAfter"_attr = databasesSnapshotAfter,
                                "attempts"_attr = attempts);
                }

                databasesSnapshotBefore = databasesSnapshotAfter;
            }

            uassert(11571002,
                    "Failed to fetch the databases from all shards of the cluster without a "
                    "concurrent version update",
                    databasesListIsValid);

            auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

            for (const auto& response : responses) {
                const auto& shardId = response.shardId;
                auto shardStatus = shardRegistry->getShard(opCtx, shardId);
                if (!shardStatus.isOK()) {
                    continue;
                }
                const auto s = std::move(shardStatus.getValue());

                const auto shardResponse = uassertStatusOK(response.swResponse);
                uassertStatusOK(shardResponse.status);

                const auto& shardResponseData = shardResponse.data;
                uassertStatusOK(getStatusFromCommandResult(shardResponseData));

                BSONObjIterator j(shardResponseData["databases"].Obj());
                while (j.more()) {
                    BSONObj dbObj = j.next().Obj();

                    const auto name = dbObj["name"].String();

                    // If this is the admin db, only collect its stats from the config servers.
                    if (name == "admin" && !s->isConfig()) {
                        continue;
                    }

                    const long long sizeOnShard = dbObj["sizeOnDisk"].numberLong();

                    auto [it, inserted] = dbShardInfos.try_emplace(name);
                    it->second.size += sizeOnShard;

                    if (!it->second.shardInfo) {
                        it->second.shardInfo = std::make_unique<BSONObjBuilder>();
                    }
                    it->second.shardInfo->append(shardId.toString(), sizeOnShard);
                }
            }

            // Adding empty databases presented only in the config server snapshot but not in the
            // shards, to be consistent with the behavior of the listDatabases command with nameOnly
            // and without a filter.
            // TODO SERVER-121720: the empty databases from the config server are added only when
            // the filter is empty or the filter is name only. If the filter has fields, like empty,
            // size, shards, etc., the empty databases from the config server are not added until
            // the filter is applied to the aggregated result on mongos.
            std::unique_ptr<MatchExpression> filter = list_databases::getFilter(cmd, opCtx, ns());
            const bool filterNameOnly = filter &&
                filter->getCategory() == MatchExpression::MatchCategory::kLeaf &&
                filter->path() == list_databases::kName;
            if (!filter || filterNameOnly) {
                for (const auto& db : databasesSnapshotBefore) {
                    const auto dbname =
                        DatabaseNameUtil::serialize(db.getDbName(), cmd.getSerializationContext());
                    if (dbShardInfos.find(dbname) == dbShardInfos.end()) {
                        if (filterNameOnly &&
                            !exec::matcher::matchesBSON(filter.get(),
                                                        ListDatabasesReplyItem(dbname).toBSON())) {
                            continue;
                        }
                        dbShardInfos.try_emplace(dbname);
                    }
                }
            }

            return dbShardInfos;
        }

        ListDatabasesReply buildReply(std::map<std::string, ShardDbInfo>& dbShardInfos,
                                      bool authorizedDatabases,
                                      AuthorizationSession* as,
                                      const RequestType& cmd) {
            long long totalSize = 0;
            std::vector<ListDatabasesReplyItem> items;
            const auto& tenantId = cmd.getDbName().tenantId();

            for (const auto& dbShardInfo : dbShardInfos) {
                const auto& dbName = dbShardInfo.first;
                const auto& size = dbShardInfo.second.size;
                const auto& shardInfo = dbShardInfo.second.shardInfo;
                const auto databaseName =
                    DatabaseNameUtil::deserialize(tenantId, dbName, cmd.getSerializationContext());

                if (!shouldIncludeDatabase(databaseName, authorizedDatabases, as)) {
                    continue;
                }

                ListDatabasesReplyItem item(dbName);

                item.setSizeOnDisk(size);
                item.setEmpty(size == 0);
                if (shardInfo) {
                    item.setShards(shardInfo->obj());
                }

                uassert(ErrorCodes::BadValue,
                        str::stream() << "Found negative 'sizeOnDisk' in: "
                                      << databaseName.toStringForErrorMsg(),
                        size >= 0);

                totalSize += size;

                items.push_back(std::move(item));
            }

            ListDatabasesReply reply(items);
            reply.setTotalSize(totalSize);
            reply.setTotalSizeMb(totalSize / (1024 * 1024));

            return reply;
        }

        ListDatabasesReply runFullWithSize(OperationContext* opCtx,
                                           bool authorizedDatabases,
                                           AuthorizationSession* as,
                                           RequestType& cmd) {
            auto dbShardInfos = getConsistentDbInfoFromShards(opCtx, cmd);
            // Now that we have aggregated results for all the shards, convert to a response,
            // and compute total sizes.
            return buildReply(dbShardInfos, authorizedDatabases, as, cmd);
        }

        ListDatabasesReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            auto* as = AuthorizationSession::get(opCtx->getClient());
            auto cmd = request();

            setReadWriteConcern(opCtx, cmd, this);

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

            if (nameOnly) {
                return runNameOnly(opCtx, authorizedDatabases, as, cmd);
            }
            return runFullWithSize(opCtx, authorizedDatabases, as, cmd);
        }
    };
};
MONGO_REGISTER_COMMAND(ListDatabasesCmd).forRouter();

}  // namespace
}  // namespace mongo
