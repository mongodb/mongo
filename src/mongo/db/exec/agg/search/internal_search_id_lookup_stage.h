// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/batched_enrichment_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/single_doc_lookup/scoped_batched_lookup.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * Execution stage for $search / $vectorSearch '$_internalSearchIdLookup'. For each _id emitted by
 * mongot it resolves the local document and forwards it (carrying the search-score metadata),
 * dropping results whose _id no longer resolves or that are orphans, and honouring the spec's
 * 'limit'.
 *
 * Built on BatchedEnrichmentStage: results are buffered and resolved in batches inside a single
 * resource scope, so the lookup executor never holds catalog state across upstream getNext() calls.
 * The production batch size is governed by the factory (batch-of-one unless a caching executor
 * makes batching pay), so behaviour is byte-for-byte the previous per-result path; only the batch
 * size changes once an SBE executor is wired in (SERVER-130900).
 */
class InternalSearchIdLookUpStage final : public BatchedEnrichmentStage {
public:
    using SearchIdLookupMetrics = DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics;

    InternalSearchIdLookUpStage(
        std::string_view stageName,
        DocumentSourceIdLookupSpec spec,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle>&
            catalogResourceHandle,
        const std::shared_ptr<SearchIdLookupMetrics>& searchIdLookupMetrics,
        std::unique_ptr<SingleDocumentLookupExecutor> lookupExecutor,
        Limits limits);

    const SpecificStats* getSpecificStats() const override {
        return &_stats;
    }

    Document getExplainOutput(const query_shape::SerializationOptions& opts =
                                  query_shape::SerializationOptions{}) const override;

    /**
     * Test-only accessor for the single-document lookup executor held by this stage, so wiring
     * tests can assert the concrete strategy installed by the stage factory via dynamic_cast.
     */
    const SingleDocumentLookupExecutor* getLookupExecutor_forTest() const {
        return _lookupExecutor.get();
    }

private:
    /**
     * Opens / closes the per-batch resource scope over the lookup executor. closeBatch() releases
     * any attached catalog state (idempotent; a no-op for the executors used today, which cache
     * nothing across lookups).
     */
    void beginBatch() override;
    void closeBatch() noexcept override;

    /**
     * Re-points this operation's OpDebug at the stage's idLookup metrics on each getMore. OpDebug
     * is per-command and reset on each getMore, so pointing happens here (once per getMore) plus
     * once at construction to cover the first batch, rather than on every enrich sub-batch.
     */
    void reattachToOperationContext(OperationContext* opCtx) override;

    /**
     * Points 'opCtx''s OpDebug at this stage's idLookup metrics so explain / profiler can report
     * them, unless already set. A no-op if 'opCtx' is null.
     */
    void registerMetricsOnOpDebug(OperationContext* opCtx);

    /**
     * Resolves one buffered search result to its local document (with search-score metadata carried
     * over), or boost::none to drop it (missing _id, no local document, or an orphan).
     */
    boost::optional<Document> enrich(Document event) override;

    /**
     * Caps output at the spec's 'limit' (0 or unset means no cap): stops emitting once that many
     * documents have been returned, even though mongot may still have results to hand up.
     */
    bool shouldStopEmittingDocuments() const override;

    /**
     * Opens the non-ticketed interval for post-search in-memory work once the search source is
     * exhausted, so time in subsequent stages (e.g. $sort, $group) is attributed correctly.
     */
    void onExhausted() override;

    const std::string _stageName;
    const DocumentSourceIdLookupSpec _spec;

    // Handle on catalog state that can be acquired and released during a lookup. Also contains the
    // collection needed for execution.
    boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle> _catalogResourceHandle;

    std::shared_ptr<SearchIdLookupMetrics> _searchIdLookupMetrics;
    DocumentSourceIdLookupStats _stats;

    // Lookup strategy enrich() drives for every _id (never null): the Express fast path or the
    // local-read executor, chosen by buildIdLookupExecutor().
    std::unique_ptr<SingleDocumentLookupExecutor> _lookupExecutor;

    // The active per-batch resource scope, present only while a batch is being enriched.
    boost::optional<ScopedBatchedLookup> _batch;
};

/**
 * Builds the executor InternalSearchIdLookUpStage drives for every _id. idLookup has no remote
 * lookup, so there is no fallback. Returns the Express fast path when the flag is on and there is
 * no view (a view is not a pure _id point lookup); otherwise the local-read executor. The stage
 * wires its explain stats sink through the executor interface, so no separate handle is returned.
 */
std::unique_ptr<SingleDocumentLookupExecutor> buildIdLookupExecutor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle>&
        catalogResourceHandle,
    boost::optional<std::vector<BSONObj>> viewPipeline);

}  // namespace mongo::exec::agg
