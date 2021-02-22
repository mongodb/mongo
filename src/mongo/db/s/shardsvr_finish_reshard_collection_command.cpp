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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/finish_reshard_collection_gen.h"

namespace mongo {
namespace {

void refreshShardVersion(OperationContext* opCtx, const NamespaceString& nss) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    if (nss.isNamespaceAlwaysUnsharded())
        return;

    auto inRecoverOrRefresh = [&] {
        Lock::DBLock dbLock{opCtx, nss.db(), MODE_IS};
        Lock::CollectionLock collLock{opCtx, nss, MODE_IS};

        const auto csr = CollectionShardingRuntime::get(opCtx, nss);

        auto critSec =
            csr->getCriticalSectionSignal(opCtx, ShardingMigrationCriticalSection::kWrite);

        invariant(critSec);

        boost::optional<CollectionShardingRuntime::CSRLock> csrLock =
            CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);

        auto metadata = csr->getCurrentMetadataIfKnown();

        csrLock.reset();
        csrLock.emplace(CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr));

        // If the shard doesn't yet know its filtering metadata, recovery needs to be run
        bool runRecover = metadata ? false : true;
        csr->setShardVersionRecoverRefreshFuture(
            recoverRefreshShardVersion(opCtx->getServiceContext(), nss, runRecover), *csrLock);
        return csr->getShardVersionRecoverRefreshFuture(opCtx);
    }();

    inRecoverOrRefresh->get(opCtx);
}

class ShardsvrFinishReshardCollectionChangeCmd final
    : public TypedCommand<ShardsvrFinishReshardCollectionChangeCmd> {
public:
    using Request = _shardsvrFinishReshardCollection;

    std::string help() const override {
        return "Internal command used by the resharding coordinator to flush the final state "
               "change to the participant shards while the critical section is active.";
    }

    bool adminOnly() const override {
        return true;
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }

        void typedRun(OperationContext* opCtx) {
            auto const shardingState = ShardingState::get(opCtx);
            uassertStatusOK(shardingState->canAcceptShardedCommands());

            uassert(ErrorCodes::IllegalOperation,
                    "Can't issue _shardsvrFinishReshardCollection from 'eval'",
                    !opCtx->getClient()->isInDirectClient());

            uassert(ErrorCodes::IllegalOperation,
                    "Can't call _shardsvrFinishReshardCollection if in read-only mode",
                    !storageGlobalParams.readOnly);

            refreshShardVersion(opCtx, ns());

            CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, ns());

            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        }
    };
} _shardsvrFinishReshardCollection;

}  // namespace
}  // namespace mongo
