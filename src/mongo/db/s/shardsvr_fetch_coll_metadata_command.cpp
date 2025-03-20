/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrFetchCollMetadataCommand final
    : public TypedCommand<ShardsvrFetchCollMetadataCommand> {
public:
    using Request = ShardsvrFetchCollMetadata;

    bool skipApiVersionCheck() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command. This command aims to fetch collection and chunks metadata, for a "
               "specific namespace, from the global catalog and persist it locally in the "
               "shard-local catalog";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Ensure shard is ready to accept sharded commands
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            // Ensure interruption on step down/up
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            const auto nss = ns();

            // Assert that migrations are disabled.
            uassert(10140200,
                    "_shardsvrFetchCollMetadata can only run when migrations are disabled",
                    !sharding_ddl_util::checkAllowMigrations(opCtx, nss));

            // Fetch the collection and chunk metadata from the config server.
            auto collAndChunks = _fetchCollectionAndChunks(opCtx, nss);

            // Persist the collection metadata locally.
            const auto serializedNs =
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());

            DBDirectClient dbClient(opCtx);
            dbClient.update(NamespaceString::kConfigShardCollectionsNamespace,
                            BSON(CollectionType::kNssFieldName << serializedNs),
                            collAndChunks.first.toBSON(),
                            true /* upsert */,
                            false /* multi */);

            // TODO (SERVER-102400): Persist chunks metadata.

            LOGV2(10140202, "Persisted metadata locally on shard", "ns"_attr = nss);
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        auto _fetchCollectionAndChunks(OperationContext* opCtx, const NamespaceString& nss)
            -> std::pair<CollectionType, std::vector<ChunkType>> {

            const auto readConcern = [&]() -> repl::ReadConcernArgs {
                const auto vcTime = VectorClock::get(opCtx)->getTime();
                return {vcTime.configTime(), repl::ReadConcernLevel::kSnapshotReadConcern};
            }();

            return Grid::get(opCtx)->catalogClient()->getCollectionAndChunks(
                opCtx, nss, ChunkVersion::IGNORED(), readConcern);
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrFetchCollMetadataCommand).forShard();

}  // namespace
}  // namespace mongo
