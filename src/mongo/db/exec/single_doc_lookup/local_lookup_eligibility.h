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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/functional.h"

#include <functional>
#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo {
class ScopedCollectionFilter;
}  // namespace mongo

namespace mongo::exec::agg {

/**
 * Decides whether a single-document lookup of 'documentKey' against 'nss' can be safely executed
 * against this node's local catalog without remote routing.
 *
 * A "yes" decision must be paired with the shard/db version it was based on, so the caller can
 * install ScopedSetShardRole before its acquisition. If the version installed at acquisition time
 * disagrees with what the decision was based on, the staleness signal (StaleConfig) is thrown.
 * LocalLookupEligibility will attempt to refresh the catalog and will retry.
 */
class LocalLookupEligibility {
public:
    /**
     * "Yes, run locally" variant. If shardVersion or dbVersion is set, the caller MUST install
     *   ScopedSetShardRole(opCtx, nss, shardVersion, dbVersion)
     * for the scope of the acquisition + executor run, so the acquisition is versioned against the
     * same routing snapshot this decision was based on.
     *
     * Conventions:
     *   - sharded collection on this shard:  shardVersion set, dbVersion unset.
     *   - unsharded collections on this primary: shardVersion unset, dbVersion set.
     *   - replica set / search / unversioned: both unset.
     */
    struct Local {
        boost::optional<ShardVersion> shardVersion;
        boost::optional<DatabaseVersion> dbVersion;
    };

    /**
     * "Can't confirm this is local, route via the remote-fallback strategy" variant. Covers both
     * "the key targets another shard" and "we couldn't determine targeting".
     */
    struct Unknown {};

    /**
     * The two outcomes are mutually exclusive at the type level: it is impossible to construct an
     * "unknown with a shardVersion" value, or to read versions from an Unknown decision.
     */
    using Decision = std::variant<Local, Unknown>;

    /**
     * Convenience: did the decision say local?
     */
    static bool isLocal(const Decision& d) {
        return std::holds_alternative<Local>(d);
    }

    /**
     * What acquisition (if any) the caller already holds, passed to run() so eligibility can skip
     * routing when ownership is already determinable from a held acquisition.
     *
     * The arms differ in how a Local decision relates to placement staleness:
     *   - HeldUnshardedCollectionLocally / HeldShardedCollection: the decision reflects the
     *     placement observed when the held acquisition was taken (some time T). The caller installs
     *     nothing further.
     *   - NoHeldAcquisition: the decision is based on the latest, possibly stale, cached routing
     *     information. A Local decision therefore carries the shard/db version it was based on, and
     *     the caller MUST take a versioned acquisition on the namespace. If placement changed since
     *     the decision, that acquisition throws StaleConfig, which run() refreshes and retries.
     *
     * Arm -> decision:
     *   NoHeldAcquisition              -> Use routing information to determine if the document
     *                                     belongs to this shard. Returns a version to attach to
     *                                     ScopedSetShardRole before acquiring the collection.
     *   HeldUnshardedCollectionLocally -> Collection is unsharded on the local shard.
     *                                     Nothing to install as collection is already acquired.
     *   HeldShardedCollection          -> Consult keyBelongsToMe() on the ScopedCollectionFilter.
     *                                     Nothing to install as collection is already acquired.
     */
    struct NoHeldAcquisition {};
    struct HeldUnshardedCollectionLocally {};
    struct HeldShardedCollection {
        std::reference_wrapper<const ScopedCollectionFilter> filter;
    };
    using AcquisitionState =
        std::variant<NoHeldAcquisition, HeldUnshardedCollectionLocally, HeldShardedCollection>;

    virtual ~LocalLookupEligibility() = default;

    /**
     * Computes the Decision for (nss, documentKey) given what the caller currently holds
     * 'acquisitionState', invokes 'body' with it, and returns body's result. On the
     * NoHeldAcquisition arm the eligibility owns catalog-and-routing (CAR) error handling, so
     * 'body' stays sharding-agnostic.
     */
    virtual SingleDocumentLookupExecutor::LookupResult run(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey,
        const AcquisitionState& acquisitionState,
        function_ref<SingleDocumentLookupExecutor::LookupResult(const Decision&)> body) const = 0;
};

/**
 * Eligibility implementation that always returns Local{} with no version to install. Use on:
 *   - Replica sets (no sharding, every lookup is local by construction).
 *   - $search idLookup (search index resides on-shard, lookups are always local by design).
 *   - Tests that want to force-local behaviour without spinning up sharding state.
 */
class AlwaysLocalEligibility final : public LocalLookupEligibility {
public:
    SingleDocumentLookupExecutor::LookupResult run(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey,
        const AcquisitionState& acquisitionState,
        function_ref<SingleDocumentLookupExecutor::LookupResult(const Decision&)> body)
        const override {
        return body(Local{});
    }
};

/**
 * Eligibility implementation that always returns Unknown{} (decline).
 */
class AlwaysUnknownEligibility final : public LocalLookupEligibility {
public:
    SingleDocumentLookupExecutor::LookupResult run(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const Document& documentKey,
        const AcquisitionState& acquisitionState,
        function_ref<SingleDocumentLookupExecutor::LookupResult(const Decision&)> body)
        const override {
        return body(Unknown{});
    }
};

/**
 * Builds a ScopedSetShardRole so any subsequent acquisition on `opCtx` is shard-versioned against
 * the same routing snapshot the eligibility predicate based its Local decision on.
 *
 * Returns boost::none when neither shardVersion nor dbVersion is set. Returns a populated optional
 * otherwise.
 *
 * Shared between the Express and SBE single-document lookup executors.
 */
boost::optional<ScopedSetShardRole> createScopedShardRole(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const LocalLookupEligibility::Local& local);

}  // namespace mongo::exec::agg
