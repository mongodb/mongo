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
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"

namespace mongo {
namespace {

template <typename Derived>
class FlushRoutingTableCacheUpdatesCmdBase : public TypedCommand<Derived> {
public:
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command which waits for any pending routing table cache updates for a "
               "particular namespace to be written locally. The operationTime returned in the "
               "response metadata is guaranteed to be at least as late as the last routing table "
               "cache update to the local disk. Takes a 'forceRemoteRefresh' option to make this "
               "node refresh its cache from the config server before waiting for the last refresh "
               "to be persisted.";
    }

    bool adminOnly() const override {
        return true;
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public TypedCommand<Derived>::InvocationBase {
    public:
        using Base = typename TypedCommand<Derived>::InvocationBase;
        using Base::Base;

        bool supportsWriteConcern() const override {
            return Derived::supportsWriteConcern();
        }

        NamespaceString ns() const override {
            return Base::request().getCommandParameter();
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
                    str::stream() << "Can't issue " << Derived::Request::kCommandName
                                  << " from 'eval'",
                    !opCtx->getClient()->isInDirectClient());

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Can't call " << Derived::Request::kCommandName
                                  << " if in read-only mode",
                    !storageGlobalParams.readOnly);

            auto& oss = OperationShardingState::get(opCtx);

            {
                AutoGetCollection autoColl(opCtx, ns(), MODE_IS);

                // If the primary is in the critical section, secondaries must wait for the commit
                // to finish on the primary in case a secondary's caller has an afterClusterTime
                // inclusive of the commit (and new writes to the committed chunk) that hasn't yet
                // propagated back to this shard. This ensures the read your own writes causal
                // consistency guarantee.
                auto const csr = CollectionShardingRuntime::get(opCtx, ns());
                auto criticalSectionSignal =
                    csr->getCriticalSectionSignal(opCtx, ShardingMigrationCriticalSection::kWrite);
                if (criticalSectionSignal) {
                    oss.setMigrationCriticalSectionSignal(criticalSectionSignal);
                }
            }

            oss.waitForMigrationCriticalSectionSignal(opCtx);

            if (Base::request().getSyncFromConfig()) {
                LOGV2_DEBUG(21982,
                            1,
                            "Forcing remote routing table refresh for {namespace}",
                            "Forcing remote routing table refresh",
                            "namespace"_attr = ns());
                onShardVersionMismatch(opCtx, ns(), boost::none);
            }

            CatalogCacheLoader::get(opCtx).waitForCollectionFlush(opCtx, ns());

            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        }
    };
};

class FlushRoutingTableCacheUpdatesCmd
    : public FlushRoutingTableCacheUpdatesCmdBase<FlushRoutingTableCacheUpdatesCmd> {
public:
    using Request = FlushRoutingTableCacheUpdates;

    static bool supportsWriteConcern() {
        return false;
    }

} _flushRoutingTableCacheUpdatesCmd;

class FlushRoutingTableCacheUpdatesCmdWithWriteConcern
    : public FlushRoutingTableCacheUpdatesCmdBase<
          FlushRoutingTableCacheUpdatesCmdWithWriteConcern> {
public:
    using Request = FlushRoutingTableCacheUpdatesWithWriteConcern;

    static bool supportsWriteConcern() {
        return true;
    }

} _flushRoutingTableCacheUpdatesWithWriteConcernCmd;

}  // namespace
}  // namespace mongo
