// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/batched_enrichment_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/single_doc_lookup/scoped_batched_lookup.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * Execution stage for the change stream 'fullDocument: "updateLookup"' enrichment. For each update
 * event it looks up the current majority-committed version of the document (read at or after the
 * event's clusterTime) and attaches it as the 'fullDocument' field. Its construction is based on
 * DocumentSourceChangeStreamAddPostImage (in updateLookup mode), which handles the optimization
 * part.
 *
 * Built on BatchedEnrichmentStage: events are buffered and enriched in batches inside a single
 * resource scope, so the lookup executor never holds catalog state across upstream getNext() calls.
 * The production batch size is governed by the factory (batch-of-1 unless a caching executor makes
 * batching pay), so behaviour is byte-for-byte the per-event path; batching only changes when.
 */
class ChangeStreamUpdateLookupStage final : public BatchedEnrichmentStage {
public:
    ChangeStreamUpdateLookupStage(std::string_view stageName,
                                  const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                  std::unique_ptr<SingleDocumentLookupExecutor> lookupExecutor,
                                  Limits limits);

    /**
     * Test-only: returns the injected SingleDocumentLookupExecutor so wiring tests can assert the
     * concrete strategy type via dynamic_cast.
     */
    const SingleDocumentLookupExecutor* getLookupExecutor_forTest() const {
        return _lookupExecutor.get();
    }

private:
    /**
     * Opens / closes the per-batch resource scope over the lookup executor chain. closeBatch()
     * releases any attached catalog state (idempotent; a no-op for Express, which caches nothing).
     */
    void beginBatch() override;
    void closeBatch() noexcept override;

    /**
     * Enriches one buffered event: for an update event, attaches the looked-up post-image as
     * 'fullDocument'; any other event passes through unchanged.
     */
    Document enrich(Document event) override;

    /**
     * Extracts the lookup parameters from the update event and invokes the injected executor.
     * Returns the post-image document or boost::none.
     */
    boost::optional<Document> performPostImageLookup(const Document& updateOp);

    /**
     * Executor responsible for fetching the latest majority-committed version of the document.
     */
    std::unique_ptr<SingleDocumentLookupExecutor> _lookupExecutor;

    /**
     * The change stream this lookup is part of. Used to resolve the lookup namespace cheaply for
     * collection-level streams (fixed namespace, no per-event parse).
     */
    ChangeStream _changeStream;

    /**
     * The active per-batch resource scope, present only while a batch is being enriched.
     */
    boost::optional<ScopedBatchedLookup> _batch;
};
}  // namespace mongo::exec::agg
