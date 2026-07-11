// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/sharded_cluster_local_lookup_eligibility.h"

#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/shard_role/initialize_auto_get_helper.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/shard_role/shard_role_loop.h"
#include "mongo/s/query/shard_targeting_helpers.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::exec::agg {

LocalLookupEligibility::Decision ShardedClusterLocalLookupEligibility::decideFromCri(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionRoutingInfo& cri,
    const Document& documentKey,
    const ShardId& localShardId,
    const boost::optional<LogicalTime>& placementConflictTime) {
    try {
        // Targeting collation MUST be kSimpleSpec. Mirrors
        // ShardServerProcessInterface::lookupSingleDocument's behaviour.
        auto targeted = getTargetedShardsForQuery(
            expCtx, cri, documentKey.toBson(), CollationSpec::kSimpleSpec);

        // Local only when exactly one shard is targeted and it is us.
        // A documentKey that does not cover the current shard key -> Unknown.
        if (targeted.size() != 1 || *targeted.begin() != localShardId) {
            return Unknown{};
        }

        // Bundle the version from this cri with the decision so the caller's ScopedSetShardRole
        // uses the same routing snapshot the decision was based on. resolveShardRoleVersions() is
        // the single source of truth for the 'shardVersion' and 'dbVersion' derivation.
        auto [shardVersion, dbVersion] = resolveShardRoleVersions(
            expCtx->getOperationContext(), cri, localShardId, placementConflictTime);
        return Local{.shardVersion = std::move(shardVersion), .dbVersion = std::move(dbVersion)};
    } catch (const DBException& ex) {
        // Interruption/cancellation (killOp, stepdown, ...) must abort the operation promptly, not
        // be masked as Unknown.
        if (ex.isA<ErrorCategory::Interruption>()) {
            throw;
        }

        // Routing/targeting/version-extraction failed against this cri. We can't prove the key is
        // local, so decline and let the fallback re-resolve. (A StaleConfig from the actual local
        // read is raised by the body, not here, and is handled by route()'s refresh + retry.)
        return Unknown{};
    }
}

LocalLookupEligibility::Decision ShardedClusterLocalLookupEligibility::decideFromShardingFilter(
    const ScopedCollectionFilter& filter, const Document& documentKey) {
    try {
        // The documentKey carries the shard-key fields; extract them with the held filter's pattern
        // (handles hashed keys), then ask the authoritative ownership filter. keyBelongsToMe() runs
        // against this shard's pinned filtering metadata, so it inherently answers "is it mine".
        const auto shardKey =
            filter.getShardKeyPattern().extractShardKeyFromDocumentKey(documentKey.toBson());
        if (shardKey.isEmpty() || !filter.keyBelongsToMe(shardKey)) {
            return Unknown{};
        }

        // The held acquisition is reused as-is and is already versioned, so there is nothing for
        // the caller to install: Local with no shard/db version.
        return Local{};
    } catch (const DBException& ex) {
        // Interruption/cancellation (killOp, stepdown, ...) must abort the operation promptly, not
        // be masked as Unknown.
        if (ex.isA<ErrorCategory::Interruption>()) {
            throw;
        }

        // Couldn't prove the key is local from this filter; decline and let the fallback
        // re-resolve.
        return Unknown{};
    }
}

SingleDocumentLookupExecutor::LookupResult ShardedClusterLocalLookupEligibility::run(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const Document& documentKey,
    const AcquisitionState& acquisitionState,
    function_ref<SingleDocumentLookupExecutor::LookupResult(const Decision&)> body) const {
    auto* opCtx = expCtx->getOperationContext();

    return std::visit(
        OverloadedVisitor{
            // Nothing held: route to derive the version, and own the StaleConfig refresh + retry.
            [&](const NoHeldAcquisition&) -> SingleDocumentLookupExecutor::LookupResult {
                // Only set inside a multi-document transaction; resolves to boost::none otherwise.
                // TODO (SERVER-115178): Remove the TransactionRouter access once v9.0 branches out.
                const auto placementConflictTime = [&]() -> boost::optional<LogicalTime> {
                    const auto txnRouter = TransactionRouter::get(opCtx);
                    return txnRouter && txnRouter.isInitialized() &&
                            opCtx->inMultiDocumentTransaction()
                        ? txnRouter.getPlacementConflictTime()
                        : boost::none;
                }();

                sharding::router::CollectionRouter router(opCtx, nss);
                return router.route(
                    "ShardedClusterLocalLookupEligibility",
                    [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                        const auto decision = decideFromCri(
                            expCtx, cri, documentKey, _localShardId, placementConflictTime);
                        // Run the body (which installs ScopedSetShardRole and acquires the
                        // collection) inside the shard-role loop. A StaleConfig caused by stale
                        // shard-side filtering metadata is recovered and retried locally here,
                        // rather than escaping to route() and triggering a useless routing-info
                        // refresh.
                        return shard_role_loop::withStaleShardRetry(opCtx,
                                                                    [&] { return body(decision); });
                    });
            },
            // Held + unsharded: db-primary local for the window. Reuse the acquisition as-is; no
            // routing, no version to install.
            [&](const HeldUnshardedCollectionLocally&)
                -> SingleDocumentLookupExecutor::LookupResult { return body(Local{}); },
            // Held + sharded: authoritative ownership against the pinned filter. Reuse the
            // acquisition; no version to install.
            [&](const HeldShardedCollection& held) -> SingleDocumentLookupExecutor::LookupResult {
                return body(decideFromShardingFilter(held.filter.get(), documentKey));
            },
        },
        acquisitionState);
}

}  // namespace mongo::exec::agg
