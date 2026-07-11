// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"
#include "mongo/db/query/plan_summary_stats.h"

#include <memory>

namespace mongo::exec::agg {

/**
 * Fast-path lookup strategy: serves local _id point lookups via the Express executor.
 *
 * Eligibility-gated by LocalLookupEligibility: it serves the lookup only when the document is
 * decided local, otherwise it declines (kNotHandled) so the caller's fallback strategy handles it.
 * See performLookup() for the result-status contract.
 */
class ExpressSingleDocumentLookupExecutor : public SingleDocumentLookupExecutor {
public:
    ExpressSingleDocumentLookupExecutor(
        std::unique_ptr<CollectionAcquirer> collectionAcquirer,
        std::unique_ptr<LocalLookupEligibility> localEligibility,
        boost::optional<SingleDocumentLookupStatsRecorder> recorder = boost::none)
        : _collectionAcquirer(std::move(collectionAcquirer)),
          _localEligibility(std::move(localEligibility)),
          _recorder(std::move(recorder)) {
        tassert(12841300,
                "ExpressSingleDocumentLookupExecutor requires a non-null collection acquirer",
                _collectionAcquirer);
        tassert(12841301,
                "ExpressSingleDocumentLookupExecutor requires a non-null eligibility policy",
                _localEligibility);
    }

    /**
     * Runs the lookup body under LocalLookupEligibility::run(), which supplies the Decision and (in
     * the routing-aware impl) owns catalog-and-routing refresh + retry. On a Local decision the
     * body installs ScopedSetShardRole(sv, dv), acquires the collection, and runs an Express _id
     * plan. Returns:
     *   - kNotHandled: Unknown decision, no _id in the documentKey, or no Express _id plan.
     *   - kDocumentFound: the document, read locally under the installed shard role.
     *   - kDocumentNotFound: _id absent, or the namespace / collection UUID no longer exists
     *     (matching master's null-fullDocument DDL semantic).
     *
     * Catalog-and-routing errors (StaleConfig / StaleDbVersion / ShardCannotRefreshDueToLocksHeld)
     * are not caught here: they propagate so the eligibility's CollectionRouter::route() can
     * refresh and retry.
     */
    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const NamespaceString& nss,
                               boost::optional<UUID> collectionUUID,
                               const Document& documentKey,
                               boost::optional<Timestamp> afterClusterTime) override;

    void setPlanSummaryStatsSink(PlanSummaryStats* sink) override {
        _planSummaryStatsSink = sink;
    }

private:
    std::unique_ptr<CollectionAcquirer> _collectionAcquirer;
    std::unique_ptr<LocalLookupEligibility> _localEligibility;
    boost::optional<SingleDocumentLookupStatsRecorder> _recorder;

    // Non-owning; owned by the caller. Stats are accumulated here when set.
    PlanSummaryStats* _planSummaryStatsSink = nullptr;
};

}  // namespace mongo::exec::agg
