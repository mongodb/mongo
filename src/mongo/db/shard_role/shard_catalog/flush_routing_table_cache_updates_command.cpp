// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/flush_routing_table_cache_updates_gen.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

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
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(Base::request().getDbName().tenantId()),
                        ActionType::internal));
        }

        void typedRun(OperationContext* opCtx) {
            auto const shardingState = ShardingState::get(opCtx);
            shardingState->assertCanAcceptShardedCommands();

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Can't issue " << Derived::Request::kCommandName
                                  << " from 'eval'",
                    !opCtx->getClient()->isInDirectClient());

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Can't call " << Derived::Request::kCommandName
                                  << " if in read-only mode",
                    !opCtx->readOnly());

            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Can only call " << Derived::Request::kCommandName
                                  << " on collections",
                    !ns().coll().empty());

            if (feature_flags::gAuthoritativeShardsCRUD.isEnabled(
                    kVersionContextIgnored_UNSAFE,
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                // When the `AuthoritativeShardsCRUD` feature flag is enabled, this method
                // should no longer be used, as nodes start relying on the `config.shard.catalog`
                // authoritative collections rather than the config server (primary node) or
                // contacting the primary to replicate filtering metadata in the `config.cache`
                // collections (secondary nodes).
                //
                // However, there is a scenario where a lagging secondary node attempts to contact
                // the primary node of the replica set while the secondary is still operating
                // under 8.0 FCV, but the primary has already transitioned to 9.0 FCV. In this case,
                // the lagging secondary will attempt to refresh using this command as part of the
                // old protocol.
                //
                // In that situation, we will first make the secondary node wait for the latest
                // opTime so that it becomes aware of the FCV change. This is no worse than the
                // current behavior, as the existing protocol also relies on the `config.cache`
                // collections for replication.
                //
                // After that, the command will fail, making the secondary aware that it needs
                // to switch to the authoritative model.

                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);

                uasserted(ErrorCodes::MetadataRefreshCanceledDueToFCVTransition,
                          "This command is deprecated, as shards are authoritative for collection "
                          "metadata. The secondary node must transition to the authoritative "
                          "refresh model.");
            }

            boost::optional<CriticalSectionSignal> criticalSectionSignal;

            {
                // If the primary is in the critical section, secondaries must wait for the commit
                // to finish on the primary in case a secondary's caller has an afterClusterTime
                // inclusive of the commit (and new writes to the committed chunk) that hasn't yet
                // propagated back to this shard. This ensures the read your own writes causal
                // consistency guarantee.
                const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, ns());
                criticalSectionSignal =
                    scopedCsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite);
            }

            if (criticalSectionSignal)
                criticalSectionSignal->get(opCtx);

            if (Base::request().getSyncFromConfig()) {
                LOGV2_DEBUG(21982, 1, "Forcing remote routing table refresh", logAttrs(ns()));
                uassertStatusOK(FilteringMetadataCache::get(opCtx)->onShardVersionMismatch(
                    opCtx, ns(), boost::none));
            }

            FilteringMetadataCache::get(opCtx)->waitForCollectionFlush(opCtx, ns());

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
};
MONGO_REGISTER_COMMAND(FlushRoutingTableCacheUpdatesCmd).forShard();

class FlushRoutingTableCacheUpdatesCmdWithWriteConcern
    : public FlushRoutingTableCacheUpdatesCmdBase<
          FlushRoutingTableCacheUpdatesCmdWithWriteConcern> {
public:
    using Request = FlushRoutingTableCacheUpdatesWithWriteConcern;

    static bool supportsWriteConcern() {
        return true;
    }
};
MONGO_REGISTER_COMMAND(FlushRoutingTableCacheUpdatesCmdWithWriteConcern).forShard();

}  // namespace
}  // namespace mongo
