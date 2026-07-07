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
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {
namespace {
/**
 * Builds the SingleDocumentLookupExecutor for the updateLookup stage.
 */
std::unique_ptr<exec::agg::SingleDocumentLookupExecutor> buildUpdateLookupExecutor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    using namespace exec::agg;
    auto aggExecutor = std::make_unique<AggregationSingleDocumentLookupExecutor>(
        exec::SingleDocumentLookupStatsRecorder::makeUpdateLookupAggregationRecorder());

    const auto& ifrContext = expCtx->getIfrContext();
    const bool optimizedLookupEnabled = ifrContext &&
        ifrContext->getSavedFlagValue(feature_flags::gFeatureFlagChangeStreamOptimizedUpdateLookup);
    if (!optimizedLookupEnabled) {
        return aggExecutor;
    }

    OperationContext* opCtx = expCtx->getOperationContext();

    // Record the effective wiring on this operation so it rides onto the ClientCursor. The getMore
    // precondition uses it to kill-and-resume the cursor if the flag is later turned off.
    CurOp::get(opCtx)->debug().usesOptimizedUpdateLookup = true;

    LocalLookupEligibilityFactoryImpl eligibilityFactory;
    auto expressExecutor = std::make_unique<ExpressSingleDocumentLookupExecutor>(
        std::make_unique<OnDemandCollectionAcquirer>(),
        eligibilityFactory.makeLocalLookupEligibility(opCtx));
    return std::make_unique<PrimaryWithFallbackSingleDocumentLookupExecutor>(
        std::move(expressExecutor), std::move(aggExecutor));
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
        return make_intrusive<exec::agg::ChangeStreamUpdateLookupStage>(
            changeStreamAddPostImageDS->kStageName,
            changeStreamAddPostImageDS->getExpCtx(),
            buildUpdateLookupExecutor(changeStreamAddPostImageDS->getExpCtx()));
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
