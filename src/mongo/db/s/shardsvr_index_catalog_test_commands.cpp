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
#include "mongo/db/commands.h"
#include "mongo/db/s/sharded_index_catalog_commands_gen.h"
#include "mongo/db/s/sharding_index_catalog_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class ShardsvrRegisterIndexTestCommand final
    : public TypedCommand<ShardsvrRegisterIndexTestCommand> {
public:
    using Request = ShardsvrRegisterIndex;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Example on how to register an index in the "
               "sharding catalog.";
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

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    format(FMT_STRING("{} command not enabled"), definition()->getName()),
                    feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
                        serverGlobalParams.featureCompatibility));
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            auto session = InternalSessionPool::get(opCtx)->acquireSystemSession();
            OperationSessionInfo osi;

            osi.setSessionId(session.getSessionId());
            osi.setTxnNumber(session.getTxnNumber());
            opCtx->setLogicalSessionId(*osi.getSessionId());
            opCtx->setTxnNumber(*osi.getTxnNumber());
            sharding_index_catalog_util::registerIndexCatalogEntry(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                osi,
                ns(),
                request().getName().toString(),
                request().getKeyPattern(),
                request().getOptions(),
                request().getCollectionUUID(),
                request().getIndexCollectionUUID(),
                true);
            // Release the session if the commit is successfull.
            InternalSessionPool::get(opCtx)->release(session);
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };
};
MONGO_REGISTER_TEST_COMMAND(ShardsvrRegisterIndexTestCommand);

class ShardsvrUnregisterIndexTestCommand final
    : public TypedCommand<ShardsvrUnregisterIndexTestCommand> {
public:
    using Request = ShardsvrUnregisterIndex;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly. Example on how to unregister an index in "
               "the sharding catalog.";
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

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    format(FMT_STRING("{} command not enabled"), definition()->getName()),
                    feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
                        serverGlobalParams.featureCompatibility));
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            CommandHelpers::uassertCommandRunWithMajority(Request::kCommandName,
                                                          opCtx->getWriteConcern());

            auto session = InternalSessionPool::get(opCtx)->acquireSystemSession();
            OperationSessionInfo osi;

            osi.setSessionId(session.getSessionId());
            osi.setTxnNumber(session.getTxnNumber());
            opCtx->setLogicalSessionId(*osi.getSessionId());
            opCtx->setTxnNumber(*osi.getTxnNumber());

            sharding_index_catalog_util::unregisterIndexCatalogEntry(
                opCtx,
                Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                osi,
                ns(),
                request().getName().toString(),
                request().getCollectionUUID(),
                true);
            // Release the session if the commit is successfull.
            InternalSessionPool::get(opCtx)->release(session);
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };
};
MONGO_REGISTER_TEST_COMMAND(ShardsvrUnregisterIndexTestCommand);
}  // namespace
}  // namespace mongo
