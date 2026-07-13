// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/functional.h"

namespace mongo::exec::agg {

/**
 * Eligibility for a sharded cluster: decides whether this shard ('localShardId') owns the
 * documentKey for 'nss'. If so it returns Local{version}; otherwise Unknown (defer to the
 * fallback).
 *
 * run() drives the body through CollectionRouter::route(), which fetches the CollectionRoutingInfo
 * and refreshes + retries on a StaleConfig thrown by the body's local read. The body stays
 * sharding-agnostic.
 */
class ShardedClusterLocalLookupEligibility final : public LocalLookupEligibility {
public:
    explicit ShardedClusterLocalLookupEligibility(ShardId localShardId)
        : _localShardId(std::move(localShardId)) {}

    SingleDocumentLookupExecutor::LookupResult run(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey,
        const AcquisitionState& acquisitionState,
        function_ref<SingleDocumentLookupExecutor::LookupResult(const Decision&)> body)
        const override;

    /**
     * Pure decision over a given CollectionRoutingInfo, bundling the routing version it was based
     * on into the Local arm so the caller installs a matching ScopedSetShardRole. Used on the
     * NoHeldAcquisition arm, where the caller will acquire fresh.
     *
     * 'placementConflictTime' is the transaction's placement-conflict timestamp (boost::none
     * outside a multi-document transaction). It is forwarded to resolveShardRoleVersions() so the
     * bundled version matches what createScopedShardRoles would produce.
     */
    static Decision decideFromCri(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  const CollectionRoutingInfo& cri,
                                  const Document& documentKey,
                                  const ShardId& localShardId,
                                  const boost::optional<LogicalTime>& placementConflictTime);

    /**
     * Pure decision over a held sharded acquisition's ownership filter. Returns Local with no
     * version (the held acquisition is reused as-is, nothing to install) when keyBelongsToMe()
     * confirms the documentKey's shard key is owned by this shard, Unknown otherwise. Used on the
     * HeldShardedCollection arm.
     */
    static Decision decideFromShardingFilter(const ScopedCollectionFilter& filter,
                                             const Document& documentKey);

    /**
     * The sharded decision arms (decideFromCri targeting, decideFromShardingFilter keyBelongsToMe)
     * resolve locality from the documentKey's shard key, so a Local decision from either already
     * confirms ownership. The unsharded HeldUnshardedCollectionLocally arm returns Local without a
     * shard key to check, but that arm only ever runs against an unsharded collection, where there
     * is no shard filter for the executor to skip so this can unconditionally return true.
     */
    bool checksShardKeyOwnership() const override {
        return true;
    }

private:
    const ShardId _localShardId;
};

}  // namespace mongo::exec::agg
