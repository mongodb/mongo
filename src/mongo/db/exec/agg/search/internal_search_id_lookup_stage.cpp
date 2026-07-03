/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/search/internal_search_id_lookup_stage.h"

#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/search/internal_search_id_lookup_local_read_executor.h"
#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/express_single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
MONGO_FAIL_POINT_DEFINE(hangBeforeResultsInInternalSearchIdLookup);

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalSearchIdLookupToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(source.get());

    tassert(10807804, "expected 'DocumentSourceInternalSearchIdLookUp' type", documentSource);

    auto lookupExecutor = exec::agg::buildIdLookupExecutor(documentSource->getExpCtx(),
                                                           documentSource->_catalogResourceHandle,
                                                           documentSource->_spec.getViewPipeline());

    return make_intrusive<exec::agg::InternalSearchIdLookUpStage>(
        documentSource->kStageName,
        documentSource->_spec,
        documentSource->getExpCtx(),
        documentSource->_catalogResourceHandle,
        documentSource->_searchIdLookupMetrics,
        std::move(lookupExecutor));
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

    // Express fast path: flag on and no view. idLookup is always local, so Express uses
    // AlwaysLocalEligibility, drops orphans via the acquisition's shard filter, and reuses the
    // stage's upfront acquisition through PreAcquiredCollectionAcquirer.
    if (optimizedLookupEnabled && !viewPipeline) {
        return std::make_unique<ExpressSingleDocumentLookupExecutor>(
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
    std::unique_ptr<SingleDocumentLookupExecutor> lookupExecutor)
    : Stage(stageName, expCtx),
      _stageName(stageName),
      _spec(std::move(spec)),
      _catalogResourceHandle(catalogResourceHandle),
      _searchIdLookupMetrics(searchIdLookupMetrics),
      _lookupExecutor(std::move(lookupExecutor)) {
    tassert(13006200, "expected a non-null id lookup executor", _lookupExecutor);
    // Point the installed executor's per-lookup stats at this stage's SpecificStats so explain
    // reports totalDocs/KeysExamined. Executors that don't surface stats no-op this.
    _lookupExecutor->setPlanSummaryStatsSink(&_stats.planSummaryStats);
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

GetNextResult InternalSearchIdLookUpStage::doGetNext() {
    // Register the IdLookup's metrics on OpDebug. It's important that we check this on each
    // doGetNext(), since the OpDebug metrics will get reset on each new getMore.
    auto& opDebug = CurOp::get(pExpCtx->getOperationContext())->debug();
    if (!opDebug.searchIdLookupMetrics) {
        opDebug.searchIdLookupMetrics = _searchIdLookupMetrics;
    }

    // Opens a non-ticketed interval exactly once (idempotent), to cover any subsequent in-memory
    // aggregation work (e.g. $sort, $group) that runs after the search source is exhausted.
    auto openIntervalForSubsequentWork = [&]() {
        auto opCtx = pExpCtx->getOperationContext();
        auto& tracker = getAggNonTicketedIntervalTracker(opCtx);
        if (!tracker.hasIntervalStart) {
            tracker.openInterval(opCtx->tickSource().getTicks());
        }
    };

    boost::optional<Document> result;
    Document inputDoc;
    if (auto limit = _spec.getLimit();
        limit && *limit != 0 && _searchIdLookupMetrics->getDocsReturnedByIdLookup() >= *limit) {
        openIntervalForSubsequentWork();
        return GetNextResult::makeEOF();
    }

    while (!result) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            if (nextInput.isEOF()) {
                openIntervalForSubsequentWork();
            }
            return nextInput;
        }

        _searchIdLookupMetrics->incrementDocsSeenByIdLookup();
        inputDoc = nextInput.releaseDocument();
        auto documentId = inputDoc["_id"];

        if (!documentId.missing()) {
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

            // Resolve the _id via the installed strategy. documentKey always carries an _id, so the
            // result is found or not found, never kNotHandled; a miss is dropped like an empty
            // read.
            using HandledStatus = SingleDocumentLookupExecutor::LookupResult::HandledStatus;
            auto lookupResult = _lookupExecutor->performLookup(pExpCtx,
                                                               pExpCtx->getNamespaceString(),
                                                               pExpCtx->getUUID(),
                                                               documentKey,
                                                               boost::none /* afterClusterTime */);
            tassert(13006201,
                    "$_internalSearchIdLookup executor did not handle an _id lookup",
                    lookupResult.status != HandledStatus::kNotHandled);
            if (lookupResult.status == HandledStatus::kDocumentFound) {
                result = std::move(lookupResult.document);
            }
        }
    }

    // Result must be populated here - EOF returns above.
    invariant(result);
    MutableDocument output(*result);

    // Transfer searchScore metadata from inputDoc to the result.
    output.copyMetaDataFrom(inputDoc);
    _searchIdLookupMetrics->incrementDocsReturnedByIdLookup();
    return output.freeze();
}

}  // namespace exec::agg
}  // namespace mongo
