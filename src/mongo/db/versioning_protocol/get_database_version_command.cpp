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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/get_database_version_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

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

            auto [dbPrimaryShard, dbVersion] = [&] {
                const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, _targetDb());
                return std::make_pair(scopedDsr->getDbPrimaryShard(), scopedDsr->getDbVersion());
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
