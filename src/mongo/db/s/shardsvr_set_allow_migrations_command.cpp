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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_allow_migrations_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {
namespace {

class ShardsvrSetAllowMigrationsCommand final
    : public TypedCommand<ShardsvrSetAllowMigrationsCommand> {
public:
    using Request = ShardsvrSetAllowMigrations;

    std::string help() const override {
        return "Internal command. Do not call directly. Enable/disable migrations in a collection.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool isCollectionSharded(OperationContext* opCtx, const NamespaceString& nss) {
            try {
                uassertStatusOK(Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss));
                return true;
            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // The collection is unsharded or doesn't exist
                return false;
            }
        }

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            opCtx->setAlwaysInterruptAtStepDownOrUp();

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " must be called with majority writeConcern, got "
                                  << request().toBSON(BSONObj()),
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            const auto& nss = ns();

            uassert(ErrorCodes::NamespaceNotSharded,
                    "Collection must be sharded so migrations can be blocked",
                    isCollectionSharded(opCtx, nss));

            const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            BatchedCommandRequest updateRequest([&]() {
                write_ops::Update updateOp(CollectionType::ConfigNS);
                updateOp.setUpdates({[&] {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(BSON(CollectionType::fullNs << nss.ns()));
                    if (request().getAllowMigrations()) {
                        entry.setU(write_ops::UpdateModification(BSON(
                            "$unset" << BSON(CollectionType::permitMigrations.name() << true))));
                    } else {
                        entry.setU(write_ops::UpdateModification(BSON(
                            "$set" << BSON(CollectionType::permitMigrations.name() << false))));
                    }
                    entry.setMulti(false);
                    return entry;
                }()});
                return updateOp;
            }());

            updateRequest.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

            auto response = configShard->runBatchWriteCommand(opCtx,
                                                              Shard::kDefaultConfigCommandTimeout,
                                                              updateRequest,
                                                              Shard::RetryPolicy::kIdempotent);

            uassertStatusOK(response.toStatus());
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrSetAllowMigrationsCommand;

}  // namespace
}  // namespace mongo
