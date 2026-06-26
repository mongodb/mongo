/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

private:
    const ShardId _localShardId;
};

}  // namespace mongo::exec::agg
