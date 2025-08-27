/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/search/document_source_vector_search.h"

#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/vector_search_helper.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_index_view_validation.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/views/resolved_view.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(vectorSearch,
                                           LiteParsedSearchStage::parse,
                                           DocumentSourceVectorSearch::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           &feature_flags::gFeatureFlagVectorSearchPublicPreview);
ALLOCATE_DOCUMENT_SOURCE_ID(vectorSearch, DocumentSourceVectorSearch::id)

DocumentSourceVectorSearch::DocumentSourceVectorSearch(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    BSONObj originalSpec)
    : DocumentSource(kStageName, expCtx),
      _taskExecutor(taskExecutor),
      _execStatsWrapper(std::make_shared<DSVectorSearchExecStatsWrapper>()),
      _originalSpec(originalSpec.getOwned()) {
    if (auto limitElem = _originalSpec.getField(kLimitFieldName)) {
        uassert(
            8575100, "Expected limit field to be a number in $vectorSearch", limitElem.isNumber());
        _limit = limitElem.safeNumberLong();
        uassert(7912700, "Expected limit to be positive", *_limit > 0);
    }
    if (auto filterElem = _originalSpec.getField(kFilterFieldName)) {
        _filterExpr = uassertStatusOK(MatchExpressionParser::parse(filterElem.Obj(), expCtx));
    }

    initializeOpDebugVectorSearchMetrics();
}

void DocumentSourceVectorSearch::initializeOpDebugVectorSearchMetrics() {
    auto& opDebug = CurOp::get(getExpCtx()->getOperationContext())->debug();
    double numCandidatesLimitRatio = [&] {
        if (!_limit.has_value()) {
            return 0.0;
        }

        auto numCandidatesElem = _originalSpec.getField(kNumCandidatesFieldName);
        if (!numCandidatesElem.isNumber()) {
            return 0.0;
        }

        return numCandidatesElem.safeNumberLong() / static_cast<double>(*_limit);
    }();
    opDebug.vectorSearchMetrics =
        OpDebug::VectorSearchMetrics{.limit = static_cast<long>(_limit.value_or(0LL)),
                                     .numCandidatesLimitRatio = numCandidatesLimitRatio};
}

Value DocumentSourceVectorSearch::serialize(const SerializationOptions& opts) const {
    if (!opts.isKeepingLiteralsUnchanged()) {
        BSONObjBuilder builder;

        // For the query shape we only care about the filter expression and 'index' field. We treat
        // the rest of the input as a black box that we do not introspect. This allows flexibility
        // on the mongot side to change or add parameters. Note that this may make this query shape
        // become outdated, but will not cause server errors.
        if (_filterExpr) {
            builder.append(kFilterFieldName, _filterExpr->serialize(opts));
        }

        if (auto indexElem = _originalSpec.getField(kIndexFieldName)) {
            builder.append(kIndexFieldName,
                           opts.serializeIdentifier(indexElem.valueStringDataSafe()));
        }

        return Value(Document{{kStageName, builder.obj()}});
    }

    // We don't want router to make a remote call to mongot even though it can generate explain
    // output.
    if (!opts.verbosity || getExpCtx()->getInRouter()) {
        return Value(Document{{kStageName, _originalSpec}});
    }

    // If the query is an explain that executed the query, we obtain the explain object from the
    // TaskExecutorCursor. Otherwise, we need to obtain the explain
    // object now.
    // TODO SERVER-107930: Execution stats should be collected in
    // SourceVectorSearch::getExplainOutput() and merged by the owner of the pipelines
    // (Note: deciding when to call 'getVectorSearchExplainResponse()' may not be straightforward).
    boost::optional<BSONObj> explainResponse;
    if (auto wrapper = _execStatsWrapper.lock()) {
        explainResponse = wrapper->getExecStats();
    }

    auto explainSpec = _originalSpec;
    BSONObj explainInfo = explainResponse.value_or_eval([&] {
        // If the request was on a view over a sharded collection, _originalSpec will include the
        // view field. Remove it from explain output as it will be included in $_internalIdLookup
        // and thus redundant.
        if (explainSpec.hasField("view")) {
            explainSpec = explainSpec.removeField("view");
        }
        return search_helpers::getVectorSearchExplainResponse(
            getExpCtx(), explainSpec, _taskExecutor.get());
    });

    auto explainObj = explainSpec.addFields(BSON("explain" << opts.serializeLiteral(explainInfo)));

    // Redact queryVector (embeddings field) if it exists to avoid including all
    // embeddings values and keep explainObj data concise.
    if (opts.isSerializingForExplain() && explainObj.hasField("queryVector")) {
        explainObj = explainObj.addFields(BSON("queryVector" << "redacted"));
    }
    return Value(Document{{kStageName, explainObj}});
}

intrusive_ptr<DocumentSource> DocumentSourceVectorSearch::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    mongot_cursor::throwIfNotRunningWithMongotHostConfigured(expCtx);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " value must be an object. Found: " << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = elem.embeddedObject();

    // Validate the source of the view if it exists on the spec, otherwise check expCtx for the
    // view.
    boost::optional<SearchQueryViewSpec> view = search_helpers::getViewFromBSONObj(spec);
    if (view) {
        search_helpers::validateViewNotSetByUser(expCtx, spec);
    } else if ((view = search_helpers::getViewFromExpCtx(expCtx))) {
        spec = spec.addField(BSON(kViewFieldName << view->toBSON()).firstElement());
    }

    if (view) {
        search_helpers::validateMongotIndexedViewsFF(expCtx, view->getEffectivePipeline());
        search_index_view_validation::validate(*view);
    }

    auto serviceContext = expCtx->getOperationContext()->getServiceContext();
    return make_intrusive<DocumentSourceVectorSearch>(
        expCtx, executor::getMongotTaskExecutor(serviceContext), spec.getOwned());
}


std::list<intrusive_ptr<DocumentSource>> DocumentSourceVectorSearch::desugar() {
    auto executor =
        executor::getMongotTaskExecutor(getExpCtx()->getOperationContext()->getServiceContext());

    std::list<intrusive_ptr<DocumentSource>> desugaredPipeline = {
        make_intrusive<DocumentSourceVectorSearch>(
            getExpCtx(), executor, _originalSpec.getOwned())};

    search_helpers::promoteStoredSourceOrAddIdLookup(
        getExpCtx(),
        desugaredPipeline,
        isStoredSource(),
        _limit.value_or(0),
        search_helpers::getViewFromBSONObj(_originalSpec));

    return desugaredPipeline;
}

std::pair<DocumentSourceContainer::iterator, bool>
DocumentSourceVectorSearch::_attemptSortAfterVectorSearchOptimization(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    auto isSortOnVectorSearchMeta = [](const SortPattern& sortPattern) -> bool {
        return isSortOnSingleMetaField(sortPattern,
                                       (1 << DocumentMetadataFields::MetaType::kVectorSearchScore));
    };
    auto optItr = std::next(itr);
    if (optItr != container->end()) {
        if (auto sortStage = dynamic_cast<DocumentSourceSort*>(optItr->get())) {
            // A $sort stage has been found directly after this stage.
            // $vectorSearch results are always sorted by 'vectorSearchScore',
            // so if the $sort stage is also sorted by 'vectorSearchScore', the $sort stage
            // is redundant and can safely be removed.
            if (isSortOnVectorSearchMeta(sortStage->getSortKeyPattern())) {
                // Optimization successful.
                container->remove(*optItr);
                return {itr, true};  // Return the same pointer in case there are other
                                     // optimizations to still be applied.
            }
        }
    }

    // Optimization not possible.
    return {itr, false};
}

DocumentSourceContainer::iterator DocumentSourceVectorSearch::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // Attempt to remove a $sort on metadata after this $vectorSearch stage.
    {
        const auto&& [returnItr, optimizationSucceeded] =
            _attemptSortAfterVectorSearchOptimization(itr, container);
        if (optimizationSucceeded) {
            return returnItr;
        }
    }

    auto stageItr = std::next(itr);
    // Only attempt to get the limit from the query if there are further stages in the pipeline.
    if (stageItr != container->end()) {
        // Move past the $internalSearchIdLookup stage, if it is next.
        auto nextIdLookup = dynamic_cast<DocumentSourceInternalSearchIdLookUp*>(stageItr->get());
        if (nextIdLookup) {
            ++stageItr;
        }
        // Calculate the extracted limit without modifying the rest of the pipeline.
        if (auto userLimit = getUserLimit(stageItr, container)) {
            _limit = _limit ? std::min(*_limit, *userLimit) : *userLimit;
        }
    }

    return std::next(itr);
}

}  // namespace mongo
