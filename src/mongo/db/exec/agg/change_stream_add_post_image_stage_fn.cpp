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

#include "mongo/db/curop.h"
#include "mongo/db/exec/agg/change_stream_add_post_image_stage.h"
#include "mongo/db/exec/agg/change_stream_update_lookup_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/single_doc_lookup/aggregation_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/express_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility_factory_impl.h"
#include "mongo/db/exec/single_doc_lookup/sbe_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <memory>

namespace mongo {
namespace {
/**
 * Computes the batch limits for the updateLookup stage. Batching only pays off behind a caching
 * executor, which is the collection-level SBE path; database- and cluster-wide streams run Express
 * (acquires per event, so a larger batch buys nothing) and stay batch-of-one. With the optimization
 * off the stage runs batch-of-one too, so its resume and latency behaviour stays byte-for-byte
 * master regardless of the batch-size knob (raised only by tests). The byte budgets always come
 * from their knobs.
 */
exec::agg::BatchedEnrichmentStage::Limits buildUpdateLookupLimits(
    const QueryKnobConfiguration& queryKnobsConfig, bool isOptimized, bool isCollectionStream) {
    const bool shouldBatch = isOptimized && isCollectionStream;
    return exec::agg::BatchedEnrichmentStage::Limits{
        .maxInputEvents = shouldBatch
            ? static_cast<size_t>(queryKnobsConfig.getChangeStreamUpdateLookupMaxBatchSize())
            : 1,
        .maxInputBytes =
            static_cast<size_t>(queryKnobsConfig.getChangeStreamUpdateLookupMaxInputBytes()),
        .maxOutputBytes =
            static_cast<size_t>(queryKnobsConfig.getChangeStreamUpdateLookupMaxOutputBytes())};
}

/**
 * Builds the SingleDocumentLookupExecutor for the updateLookup stage.
 *
 * With the optimization on, the primary strategy depends on the change-stream scope:
 * - Collection-level streams have a fixed lookup namespace, so they use the SBE executor: it
 *   compiles a parameterized '{_id: <value>}' plan once and reuses it across events (rebinding the
 *   _id slot), with the acquisition cached across the batch window.
 * - Database- and cluster-wide streams look up a different namespace per event, so a cached plan
 *   buys nothing; they use the Express point-read executor.
 * Both primaries fall back to the routed Aggregation executor when they decline.
 */
std::unique_ptr<exec::agg::SingleDocumentLookupExecutor> buildUpdateLookupExecutor(
    OperationContext* opCtx, bool isOptimized, bool isCollectionStream) {
    using namespace exec::agg;
    auto aggExecutor = std::make_unique<AggregationSingleDocumentLookupExecutor>(
        exec::SingleDocumentLookupStatsRecorder::makeUpdateLookupAggregationRecorder());

    if (!isOptimized) {
        return aggExecutor;
    }

    // Record the effective wiring on this operation so it rides onto the ClientCursor. The getMore
    // precondition uses it to kill-and-resume the cursor if the flag is later turned off.
    CurOp::get(opCtx)->debug().usesOptimizedUpdateLookup = true;

    LocalLookupEligibilityFactoryImpl eligibilityFactory;
    std::unique_ptr<SingleDocumentLookupExecutor> primary;
    if (isCollectionStream) {
        primary = std::make_unique<SbeSingleDocumentLookupExecutor>(
            std::make_unique<OnDemandCollectionAcquirer>(),
            eligibilityFactory.makeLocalLookupEligibility(opCtx));
    } else {
        primary = std::make_unique<ExpressSingleDocumentLookupExecutor>(
            std::make_unique<OnDemandCollectionAcquirer>(),
            eligibilityFactory.makeLocalLookupEligibility(opCtx),
            exec::SingleDocumentLookupStatsRecorder::makeUpdateLookupExpressRecorder());
    }

    return std::make_unique<PrimaryWithFallbackSingleDocumentLookupExecutor>(
        std::move(primary), std::move(aggExecutor));
}
}  // namespace

/**
 * Builds the execution stage for DocumentSourceChangeStreamAddPostImage. The post-image work is
 * split across two single-responsibility stages selected by the 'fullDocument' mode:
 * - 'updateLookup'                -> ChangeStreamUpdateLookupStage (look up the current document).
 * - 'required' / 'whenAvailable'  -> ChangeStreamAddPostImageStage (compute the post-image from the
 *                                    pre-image plus the oplog update modification).
 */
boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamAddPostImageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamAddPostImageDS =
        dynamic_cast<DocumentSourceChangeStreamAddPostImage*>(documentSource.get());
    tassert(10561301,
            "expected 'DocumentSourceChangeStreamAddPostImage' type",
            changeStreamAddPostImageDS);

    if (changeStreamAddPostImageDS->isUpdateLookup()) {
        const auto& expCtx = changeStreamAddPostImageDS->getExpCtx();
        auto ifrCtx = expCtx->getIfrContext();
        const bool isOptimized = ifrCtx &&
            ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagChangeStreamOptimizedUpdateLookup);
        const bool isCollectionStream =
            ChangeStream::getChangeStreamType(expCtx->getNamespaceString()) ==
            ChangeStreamType::kCollection;

        return make_intrusive<exec::agg::ChangeStreamUpdateLookupStage>(
            changeStreamAddPostImageDS->kStageName,
            expCtx,
            buildUpdateLookupExecutor(
                expCtx->getOperationContext(), isOptimized, isCollectionStream),
            buildUpdateLookupLimits(
                expCtx->getQueryKnobConfiguration(), isOptimized, isCollectionStream));
    }

    return make_intrusive<exec::agg::ChangeStreamAddPostImageStage>(
        changeStreamAddPostImageDS->kStageName,
        changeStreamAddPostImageDS->getExpCtx(),
        changeStreamAddPostImageDS->getFullDocument());
}

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamAddPostImage,
                           DocumentSourceChangeStreamAddPostImage::id,
                           documentSourceChangeStreamAddPostImageToStageFn)

}  // namespace exec::agg
}  // namespace mongo
