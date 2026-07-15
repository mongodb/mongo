// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/search/internal_search_id_lookup_stage.h"

#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/search/internal_search_id_lookup_local_read_executor.h"
#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"
#include "mongo/db/exec/single_doc_lookup/sbe_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
MONGO_FAIL_POINT_DEFINE(hangBeforeResultsInInternalSearchIdLookup);

namespace {
/**
 * Batch limits for $_internalSearchIdLookup. Only the caching (SBE) executor gains from a batch
 * larger than one -- it reuses its parameterized plan and acquisition across the window -- so the
 * event count uses the knob only when 'shouldBatch'; other paths run batch-of-one. Byte budgets
 * always come from their knobs.
 */
exec::agg::BatchedEnrichmentStage::Limits buildIdLookupLimits(
    const QueryKnobConfiguration& queryKnobsConfig, bool shouldBatch) {
    return exec::agg::BatchedEnrichmentStage::Limits{
        .maxInputEvents =
            shouldBatch ? static_cast<size_t>(queryKnobsConfig.getSearchIdLookupMaxBatchSize()) : 1,
        .maxInputBytes = static_cast<size_t>(queryKnobsConfig.getSearchIdLookupMaxInputBytes()),
        .maxOutputBytes = static_cast<size_t>(queryKnobsConfig.getSearchIdLookupMaxOutputBytes())};
}
}  // namespace

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalSearchIdLookupToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(source.get());

    tassert(10807804, "expected 'DocumentSourceInternalSearchIdLookUp' type", documentSource);

    const auto& expCtx = documentSource->getExpCtx();
    const auto& ifrContext = expCtx->getIfrContext();
    const bool isOptimized = ifrContext &&
        ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagSearchOptimizedIdLookup);

    auto viewPipeline = documentSource->_spec.getViewPipeline();

    // Batching pays behind the caching SBE executor, which serves the optimized, non-view path (see
    // buildIdLookupExecutor). A view forces the per-lookup local-read executor, which gains nothing
    // from a larger batch, so it runs batch-of-one.
    const bool shouldBatch = isOptimized && !viewPipeline;

    auto lookupExecutor = exec::agg::buildIdLookupExecutor(
        expCtx, documentSource->_catalogResourceHandle, std::move(viewPipeline));

    return make_intrusive<exec::agg::InternalSearchIdLookUpStage>(
        documentSource->kStageName,
        documentSource->_spec,
        expCtx,
        documentSource->_catalogResourceHandle,
        documentSource->_searchIdLookupMetrics,
        std::move(lookupExecutor),
        buildIdLookupLimits(expCtx->getQueryKnobConfiguration(), shouldBatch));
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(internalSearchIdLookupStage,
                           DocumentSourceInternalSearchIdLookUp::id,
                           documentSourceInternalSearchIdLookupToStageFn);

std::unique_ptr<SingleDocumentLookupExecutor> buildIdLookupExecutor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle>&
        catalogResourceHandle,
    boost::optional<std::vector<BSONObj>> viewPipeline) {
    const auto& ifrContext = expCtx->getIfrContext();
    const bool optimizedLookupEnabled = ifrContext &&
        ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagSearchOptimizedIdLookup);

    // Optimized fast path (flag on, no view): the SBE point-read executor, reusing the stage's
    // upfront acquisition and caching a parameterized '{_id}' plan across the batch window. The
    // lookup always runs local (search index is on-shard), so AlwaysLocalEligibility; a sharded
    // collection's orphans are dropped by the SHARDING_FILTER the executor adds above the scan.
    if (optimizedLookupEnabled && !viewPipeline) {
        return std::make_unique<SbeSingleDocumentLookupExecutor>(
            std::make_unique<PreAcquiredCollectionAcquirer>(
                catalogResourceHandle->getStasher(),
                catalogResourceHandle->getCollectionForLookupExecutor()),
            std::make_unique<AlwaysLocalEligibility>());
    }

    // Local-read path: `$match`-on-_id (optionally + view pipeline) against the stashed
    // acquisition.
    return std::make_unique<InternalSearchIdLookUpLocalReadExecutor>(catalogResourceHandle,
                                                                     std::move(viewPipeline));
}

InternalSearchIdLookUpStage::InternalSearchIdLookUpStage(
    std::string_view stageName,
    DocumentSourceIdLookupSpec spec,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle>&
        catalogResourceHandle,
    const std::shared_ptr<SearchIdLookupMetrics>& searchIdLookupMetrics,
    std::unique_ptr<SingleDocumentLookupExecutor> lookupExecutor,
    Limits limits)
    : BatchedEnrichmentStage(stageName, expCtx, limits),
      _stageName(stageName),
      _spec(std::move(spec)),
      _catalogResourceHandle(catalogResourceHandle),
      _searchIdLookupMetrics(searchIdLookupMetrics),
      _lookupExecutor(std::move(lookupExecutor)) {
    tassert(13006200, "expected a non-null id lookup executor", _lookupExecutor);
    // Point the installed executor's per-lookup stats at this stage's SpecificStats so explain
    // reports totalDocs/KeysExamined. Executors that don't surface stats no-op this.
    _lookupExecutor->setPlanSummaryStatsSink(&_stats.planSummaryStats);

    // Register metrics on the first batch's OpDebug now; reattachToOperationContext() re-points it
    // on every subsequent getMore.
    registerMetricsOnOpDebug(expCtx->getOperationContext());
}

Document InternalSearchIdLookUpStage::getExplainOutput(
    const query_shape::SerializationOptions& opts) const {
    const PlanSummaryStats& stats = _stats.planSummaryStats;
    MutableDocument output(Stage::getExplainOutput(opts));
    // Create sub-document with the stage name. The QO side of the explain output has a similar
    // sub-document, and they are going to be merged together by the owner of the pipelines.
    MutableDocument doc;
    doc["totalDocsExamined"] = Value(static_cast<long long>(stats.totalDocsExamined));
    doc["totalKeysExamined"] = Value(static_cast<long long>(stats.totalKeysExamined));
    doc["numDocsFilteredByIdLookup"] = opts.serializeLiteral(
        Value((long long)(_searchIdLookupMetrics->getDocsSeenByIdLookup() -
                          _searchIdLookupMetrics->getDocsReturnedByIdLookup())));
    output[_stageName] = doc.freezeToValue();
    return output.freeze();
}

void InternalSearchIdLookUpStage::beginBatch() {
    // Open the per-batch resource scope over the lookup executor; closeBatch() releases it.
    _batch.emplace(*_lookupExecutor);
}

void InternalSearchIdLookUpStage::reattachToOperationContext(OperationContext* opCtx) {
    // Re-bind the base's memory tracker to the new operation first.
    BatchedEnrichmentStage::reattachToOperationContext(opCtx);
    // OpDebug is reset on each new getMore; re-point it at this stage's metrics when the operation
    // reattaches (once per getMore) instead of on every enrich sub-batch.
    registerMetricsOnOpDebug(opCtx);
}

void InternalSearchIdLookUpStage::registerMetricsOnOpDebug(OperationContext* opCtx) {
    if (!opCtx) {
        return;
    }
    auto& opDebug = CurOp::get(opCtx)->debug();
    if (!opDebug.searchIdLookupMetrics) {
        opDebug.searchIdLookupMetrics = _searchIdLookupMetrics;
    }
}

void InternalSearchIdLookUpStage::closeBatch() noexcept {
    _batch.reset();
}

boost::optional<size_t> InternalSearchIdLookUpStage::remainingDocumentsToEmit() const {
    // Honour the spec's 'limit' (0 or unset means no cap): the remaining allowance is the limit
    // minus what has already been returned. The base treats 0 as "stop" and uses a positive value
    // to cap the upstream pull, so a batch never advances mongot past what the limit needs.
    auto limit = _spec.getLimit();
    if (!limit || *limit == 0) {
        return boost::none;
    }
    auto returned = _searchIdLookupMetrics->getDocsReturnedByIdLookup();
    return returned >= *limit ? 0u : static_cast<size_t>(*limit - returned);
}

void InternalSearchIdLookUpStage::onExhausted() {
    // Open a non-ticketed interval exactly once (idempotent), to cover any subsequent in-memory
    // aggregation work (e.g. $sort, $group) that runs after the search source is exhausted.
    auto opCtx = pExpCtx->getOperationContext();
    auto& tracker = getAggNonTicketedIntervalTracker(opCtx);
    if (!tracker.hasIntervalStart) {
        tracker.openInterval(opCtx->tickSource().getTicks());
    }
}

boost::optional<Document> InternalSearchIdLookUpStage::enrich(Document event) {
    _searchIdLookupMetrics->incrementDocsSeenByIdLookup();

    auto documentId = event["_id"];
    if (documentId.missing()) {
        // Inputs without an _id are skipped (dropped).
        return boost::none;
    }

    if (MONGO_unlikely(hangBeforeResultsInInternalSearchIdLookup.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangBeforeResultsInInternalSearchIdLookup,
            pExpCtx->getOperationContext(),
            "hangBeforeResultsInInternalSearchIdLookup",
            []() {
                LOGV2(11147700,
                      "Hanging aggregation due to "
                      "'hangBeforeResultsInInternalSearchIdLookup' "
                      "failpoint");
            });
    }

    auto documentKey = Document({{"_id", documentId}});

    tassert(31052,
            "Collection should exist when using $_internalSearchIdLookup",
            pExpCtx->getUUID().has_value());

    // Resolve the _id. The executor always handles a bare _id lookup, so the result is found or
    // not-found (a miss -- deleted doc or orphan -- is dropped below), never kNotHandled.
    using HandledStatus = SingleDocumentLookupExecutor::LookupResult::HandledStatus;
    auto lookupResult = _lookupExecutor->performLookup(pExpCtx,
                                                       pExpCtx->getNamespaceString(),
                                                       pExpCtx->getUUID(),
                                                       documentKey,
                                                       boost::none /* afterClusterTime */);
    tassert(13006201,
            "$_internalSearchIdLookup executor did not handle an _id lookup",
            lookupResult.status != HandledStatus::kNotHandled);
    if (lookupResult.status != HandledStatus::kDocumentFound) {
        return boost::none;
    }

    // Transfer searchScore metadata from the input event to the resolved document.
    MutableDocument output(std::move(*lookupResult.document));
    output.copyMetaDataFrom(event);
    _searchIdLookupMetrics->incrementDocsReturnedByIdLookup();
    return output.freeze();
}

}  // namespace exec::agg
}  // namespace mongo
