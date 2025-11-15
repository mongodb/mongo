/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/s/request_types/repair_sharded_collection_chunks_history_gen.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding


namespace mongo {
namespace {

class RepairShardedCollectionChunksHistoryCommand
    : public TypedCommand<RepairShardedCollectionChunksHistoryCommand> {
public:
    using Request = RepairShardedCollectionChunksHistory;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            BSONObjBuilder cmdBuilder;
            ConfigsvrRepairShardedCollectionChunksHistory cmd(ns());
            cmd.setForce(request().getForce());
            cmd.setDbName(DatabaseName::kAdmin);
            cmd.serialize(&cmdBuilder);

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        DatabaseName::kAdmin,
                                        CommandHelpers::appendMajorityWriteConcern(
                                            cmdBuilder.obj(), opCtx->getWriteConcern()),
                                        Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(cmdResponse.commandStatus);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        // The command intentionally uses the permission control of split/mergeChunks since it only
        // modifies the contents of chunk entries and increments the collection/shard placement
        // versions without causing any data placement changes
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::splitChunk));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Administrative command to repair the effects of SERVER-62065. If the collection "
               "has been upgraded through a cluster comprised of binaries which do not contain "
               "this command, the chunks cache collections on the shards will miss history "
               "entries. This command will correct that and will mark such collections as "
               "correctly repaired, so that a subsequent invocation will not cause any changes to "
               "the routing information. In rare cases where the history entries are missing due "
               "to corrupted restore, the 'force:true' parameter can be passed which will force "
               "all history entries to be re-added.";
    }
};
MONGO_REGISTER_COMMAND(RepairShardedCollectionChunksHistoryCommand).forRouter();

}  // namespace
}  // namespace mongo
