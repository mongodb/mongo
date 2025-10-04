/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrValidateShardKeyCandidateCommand final
    : public TypedCommand<ShardsvrValidateShardKeyCandidateCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the primary sharding server. Do not call "
               "directly. Validates a collection shard key candidate.";
    }

    using Request = ShardsvrValidateShardKeyCandidate;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {

            const ShardKeyPattern keyPattern(request().getKey());
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            {
                const auto coll =
                    acquireCollection(opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx, ns(), AcquisitionPrerequisites::kRead),
                                      MODE_IS);

                uassert(ErrorCodes::NamespaceNotSharded,
                        str::stream()
                            << "Can't execute " << Request::kCommandName
                            << "on unsharded collection " << redact(ns().toStringForErrorMsg()),
                        coll.getShardingDescription().isSharded());

                shardkeyutil::validateShardKeyIndexExistsOrCreateIfPossible(
                    opCtx,
                    ns(),
                    keyPattern,
                    boost::none,
                    coll.getShardingDescription().isUniqueShardKey(),
                    request().getEnforceUniquenessCheck().value_or(true),
                    shardkeyutil::ValidationBehaviorsLocalRefineShardKey(opCtx,
                                                                         coll.getCollectionPtr()),
                    coll.getCollectionPtr()->getTimeseriesOptions());
            }
            shardkeyutil::validateShardKeyIsNotEncrypted(opCtx, ns(), keyPattern);
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrValidateShardKeyCandidateCommand).forShard();

}  // namespace
}  // namespace mongo
