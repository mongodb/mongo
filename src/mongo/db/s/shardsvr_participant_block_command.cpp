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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/participant_block_gen.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/chunk_manager_targeter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardSvrParticipantBlockCommand final : public TypedCommand<ShardSvrParticipantBlockCommand> {
public:
    using Request = ShardsvrParticipantBlock;

    std::string help() const override {
        return "Internal command, which is exported by the shards. Do not call "
               "directly. Blocks CRUD operations.";
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            auto service = ShardingRecoveryService::get(opCtx);
            const auto reason =
                request().getReason().get_value_or(BSON("command"
                                                        << "ShardSvrParticipantBlockCommand"
                                                        << "ns" << ns().toString()));
            auto blockType = request().getBlockType().get_value_or(
                CriticalSectionBlockTypeEnum::kReadsAndWrites);
            switch (blockType) {
                case CriticalSectionBlockTypeEnum::kUnblock:
                    service->releaseRecoverableCriticalSection(
                        opCtx, ns(), reason, ShardingCatalogClient::kLocalWriteConcern);
                    break;
                case CriticalSectionBlockTypeEnum::kWrites:
                    service->acquireRecoverableCriticalSectionBlockWrites(
                        opCtx, ns(), reason, ShardingCatalogClient::kLocalWriteConcern);
                    break;
                default:
                    service->acquireRecoverableCriticalSectionBlockWrites(
                        opCtx, ns(), reason, ShardingCatalogClient::kLocalWriteConcern);
                    service->promoteRecoverableCriticalSectionToBlockAlsoReads(
                        opCtx, ns(), reason, ShardingCatalogClient::kLocalWriteConcern);
            }
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
} shardsvrParticipantBlockCommand;

}  // namespace
}  // namespace mongo
