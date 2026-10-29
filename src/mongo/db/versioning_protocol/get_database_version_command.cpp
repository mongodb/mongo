// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/catalog_cache_diagnostics_helpers.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/get_database_version_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

void appendFilteringMetadataCacheInfo(OperationContext* opCtx,
                                      rpc::ReplyBuilderInterface* result,
                                      const DatabaseName& dbName) {
    auto [dbPrimaryShard, dbVersion] = [&] {
        const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, dbName);

        // GetDatabaseVersion command can bypass the critical section to read database
        // metadata as it is a command used for troubleshooting and inspect the insights of
        // the DatabaseShardingRuntime.
        BypassDatabaseMetadataAccess bypassDbMetadataAccess(
            opCtx, BypassDatabaseMetadataAccess::Type::kReadOnly);  // NOLINT

        return std::make_pair(scopedDsr->getDbPrimaryShard(opCtx), scopedDsr->getDbVersion(opCtx));
    }();

    if (!dbVersion) {
        result->getBodyBuilder().append("dbVersion", BSONObj());
        return;
    }

    result->getBodyBuilder().append("dbVersion", dbVersion->toBSON());

    if (dbPrimaryShard && ShardingState::get(opCtx)->shardId() == *dbPrimaryShard) {
        result->getBodyBuilder().append("isPrimaryShardForDb", true);
    }
}

}  // namespace

class GetDatabaseVersionCmd final : public TypedCommand<GetDatabaseVersionCmd> {
public:
    using Request = GetDatabaseVersion;

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        // The command parameter happens to be string so it's historically been interpreted
        // by parseNs as a collection. Continuing to do so here for unexamined compatibility.
        NamespaceString ns() const override {
            const auto& cmd = request();
            return NamespaceStringUtil::deserialize(cmd.getDbName(), cmd.getCommandParameter());
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forDatabaseName(_targetDb()),
                            ActionType::getDatabaseVersion));
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << definition()->getName() << " can only be run on shard servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
            if (request().getLatestCached()) {
                auto builder = result->getBodyBuilder();
                catalog_cache_diagnostics_helpers::appendLatestCachedDbInfo(
                    opCtx, &builder, _targetDb());
                builder.done();
            } else {
                appendFilteringMetadataCacheInfo(opCtx, result, _targetDb());
            }
        }

        DatabaseName _targetDb() const {
            const auto& cmd = request();
            return DatabaseNameUtil::deserialize(cmd.getDbName().tenantId(),
                                                 cmd.getCommandParameter(),
                                                 cmd.getSerializationContext());
        }
    };

    std::string help() const override {
        return " example: { getDatabaseVersion : 'foo'  } ";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(GetDatabaseVersionCmd).forShard();

}  // namespace mongo
