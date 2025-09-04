/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/search/search_helper.h"

#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/document_source_list_search_indexes.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_search_meta.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/plan_sharded_search_gen.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/util/assert_util.h"

#include <list>
#include <set>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
MONGO_FAIL_POINT_DEFINE(searchReturnEofImmediately);

namespace search_helpers {
namespace {
void desugarSearchPipeline(Pipeline* pipeline) {
    auto searchStage = pipeline->popFrontWithName(DocumentSourceSearch::kStageName);
    auto& sources = pipeline->getSources();
    if (searchStage) {
        auto desugaredPipeline = dynamic_cast<DocumentSourceSearch*>(searchStage.get())->desugar();
        sources.insert(sources.begin(), desugaredPipeline.begin(), desugaredPipeline.end());
    }
    auto vectorSearchStage = pipeline->popFrontWithName(DocumentSourceVectorSearch::kStageName);
    if (vectorSearchStage) {
        auto desugaredPipeline =
            dynamic_cast<DocumentSourceVectorSearch*>(vectorSearchStage.get())->desugar();
        sources.insert(sources.begin(), desugaredPipeline.begin(), desugaredPipeline.end());
    }
}

// Asserts that $$SEARCH_META is accessed correctly, that is, it is set by a prior stage, and is
// not accessed in a subpipline. It is assumed that if there is a
// 'DocumentSourceInternalSearchMongotRemote' then '$$SEARCH_META' will be set at some point in the
// pipeline. Depending on the configuration of the cluster,
// 'DocumentSourceSetVariableFromSubPipeline' could do the actual setting of the variable, but it
// can only be generated alongside a 'DocumentSourceInternalSearchMongotRemote'.
void assertSearchMetaAccessValidHelper(
    const std::vector<const DocumentSourceContainer*>& pipelines) {
    // Whether or not there was a sub-pipeline stage previously in this pipeline.
    bool subPipeSeen = false;
    bool searchMetaSet = false;

    for (const auto* pipeline : pipelines) {
        for (const auto& source : *pipeline) {
            // Check if this is a stage that sets $$SEARCH_META.
            static constexpr StringData kSetVarName =
                DocumentSourceSetVariableFromSubPipeline::kStageName;
            auto stageName = StringData(source->getSourceName());
            if (stageName == DocumentSourceInternalSearchMongotRemote::kStageName ||
                stageName == DocumentSourceSearch::kStageName || stageName == kSetVarName) {
                searchMetaSet = true;
                if (stageName == kSetVarName) {
                    tassert(6448003,
                            str::stream()
                                << "Expecting all " << kSetVarName << " stages to be setting "
                                << Variables::getBuiltinVariableName(Variables::kSearchMetaId),
                            checked_cast<DocumentSourceSetVariableFromSubPipeline*>(source.get())
                                    ->variableId() == Variables::kSearchMetaId);
                    // $setVariableFromSubPipeline has a "sub pipeline", but it is the exception to
                    // the scoping rule, since it is defining the $$SEARCH_META variable.
                    continue;
                }
            }

            // If this stage has a sub-pipeline, $$SEARCH_META is not allowed after this stage.
            auto thisStageSubPipeline = source->getSubPipeline();
            if (thisStageSubPipeline) {
                subPipeSeen = true;
                if (!thisStageSubPipeline->empty()) {
                    assertSearchMetaAccessValidHelper({thisStageSubPipeline});
                }
            }

            // Check if this stage references $$SEARCH_META.
            std::set<Variables::Id> refs;
            source->addVariableRefs(&refs);
            if (Variables::hasVariableReferenceTo(refs, {Variables::kSearchMetaId})) {
                uassert(6347901,
                        "Can't access $$SEARCH_META after a stage with a sub-pipeline",
                        !subPipeSeen || thisStageSubPipeline);
                uassert(
                    6347902,
                    "Can't access $$SEARCH_META without a $search stage earlier in the pipeline",
                    searchMetaSet);
            }
        }
    }
}

BSONObj getSearchRemoteExplain(const ExpressionContext* expCtx,
                               const BSONObj& searchQuery,
                               size_t remoteCursorId,
                               boost::optional<BSONObj> sortSpec,
                               const mongot_cursor::OptimizationFlags& optimizationFlags) {
    auto executor =
        executor::getMongotTaskExecutor(expCtx->getOperationContext()->getServiceContext());
    auto explainObj = mongot_cursor::getSearchExplainResponse(
        expCtx, searchQuery, executor.get(), optimizationFlags);
    BSONObjBuilder builder;
    builder << "id" << static_cast<int>(remoteCursorId) << "mongotQuery" << searchQuery << "explain"
            << explainObj;
    if (sortSpec) {
        builder << "sortSpec" << *sortSpec;
    }
    return builder.obj();
}

std::pair<std::unique_ptr<executor::TaskExecutorCursor>,
          std::unique_ptr<executor::TaskExecutorCursor>>
parseMongotResponseCursors(std::vector<std::unique_ptr<executor::TaskExecutorCursor>> cursors) {
    // mongot can return zero cursors for an empty collection, one without metadata, or two for
    // results and metadata.
    tassert(7856000, "Expected less than or exactly two cursors from mongot", cursors.size() <= 2);

    if (cursors.size() == 1 && !cursors[0]->getType()) {
        return {std::move(cursors[0]), nullptr};
    }

    std::pair<std::unique_ptr<executor::TaskExecutorCursor>,
              std::unique_ptr<executor::TaskExecutorCursor>>
        result;

    for (auto& cursor : cursors) {
        auto maybeCursorLabel = cursor->getType();
        // If a cursor is unlabeled mongot does not support metadata cursors. $$SEARCH_META
        // should not be supported in this query.
        tassert(
            7856001, "Expected cursors to be labeled if there are more than one", maybeCursorLabel);

        switch (*maybeCursorLabel) {
            case CursorTypeEnum::DocumentResult:
                result.first = std::move(cursor);
                break;
            case CursorTypeEnum::SearchMetaResult:
                result.second = std::move(cursor);
                break;
        }
    }
    return result;
}
}  // namespace

void planShardedSearch(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       InternalSearchMongotRemoteSpec* remoteSpec) {
    LOGV2_DEBUG(9497008,
                5,
                "planShardedSearch",
                "ns"_attr = expCtx->getNamespaceString().coll(),
                "remoteSpec"_attr = redact(remoteSpec->toBSON()));
    // Mongos issues the 'planShardedSearch' command rather than 'search' in order to:
    // * Create the merging pipeline.
    // * Get a sortSpec.
    const auto cmdObj = [&]() {
        PlanShardedSearchSpec cmd(
            std::string(expCtx->getNamespaceString().coll()) /* planShardedSearch */,
            remoteSpec->getMongotQuery() /* query */);

        if (expCtx->getExplain()) {
            cmd.setExplain(
                BSON("verbosity" << ExplainOptions::verbosityString(*expCtx->getExplain())));
        }

        // Add the searchFeatures field.
        cmd.setSearchFeatures(
            BSON(SearchFeatures_serializer(SearchFeaturesEnum::kShardedSort) << 1));

        return cmd.toBSON();
    }();
    // Send the planShardedSearch to the remote, retrying on network errors.
    auto response = mongot_cursor::runSearchCommandWithRetries(expCtx, cmdObj);

    remoteSpec->setMetadataMergeProtocolVersion(response.data["protocolVersion"_sd].Int());
    auto rawPipeline = response.data["metaPipeline"];
    LOGV2_DEBUG(
        9497009, 5, "planShardedSearch response", "mergePipeline"_attr = redact(rawPipeline));
    auto parsedPipeline = mongo::Pipeline::parseFromArray(rawPipeline, expCtx);
    remoteSpec->setMergingPipeline(parsedPipeline->serializeToBson());
    if (response.data.hasElement("sortSpec")) {
        remoteSpec->setSortSpec(response.data["sortSpec"].Obj().getOwned());
    }
}

bool hasReferenceToSearchMeta(const DocumentSource& ds) {
    std::set<Variables::Id> refs;
    ds.addVariableRefs(&refs);
    return Variables::hasVariableReferenceTo(refs,
                                             std::set<Variables::Id>{Variables::kSearchMetaId});
}

bool isSearchPipeline(const Pipeline* pipeline) {
    if (!pipeline || pipeline->empty()) {
        return false;
    }
    return isSearchStage(pipeline->peekFront());
}

bool isSearchMetaPipeline(const Pipeline* pipeline) {
    if (!pipeline || pipeline->empty()) {
        return false;
    }
    return isSearchMetaStage(pipeline->peekFront());
}

void checkAndSetViewOnExpCtx(boost::intrusive_ptr<ExpressionContext> expCtx,
                             const std::vector<mongo::BSONObj> pipelineObj,
                             ResolvedView resolvedView,
                             const NamespaceString& viewName) {
    auto lpp = LiteParsedPipeline(viewName, pipelineObj);

    // Search queries on views behave differently than non-search aggregations on views.
    // When a user pipeline contains a $search/$vectorSearch stage, idLookup will apply the
    // view transforms as part of its subpipeline. In this way, the view stages will always
    // be applied directly after $_internalSearchMongotRemote and before the remaining
    // stages of the user pipeline. This is to ensure the stages following
    // $search/$vectorSearch in the user pipeline will receive the modified documents: when
    // storedSource is disabled, idLookup will retrieve full/unmodified documents during
    // (from the _id values returned by mongot), apply the view's data transforms, and pass
    // said transformed documents through the rest of the user pipeline.
    if (lpp.hasSearchStage() && !resolvedView.getPipeline().empty()) {
        expCtx->setView(boost::make_optional(std::make_pair(viewName, resolvedView.getPipeline())));
    }
}

bool isStoredSource(const Pipeline* pipeline) {
    auto ds = pipeline->peekFront();
    auto searchStage = dynamic_cast<DocumentSourceSearch*>(ds);
    if (searchStage && searchStage->isStoredSource()) {
        return true;
    }

    auto searchStageInternal = dynamic_cast<DocumentSourceInternalSearchMongotRemote*>(ds);
    if (searchStageInternal && searchStageInternal->isStoredSource()) {
        return true;
    }

    return false;
}

bool isMongotPipeline(const Pipeline* pipeline) {
    if (!pipeline || pipeline->empty()) {
        return false;
    }
    return isMongotStage(pipeline->peekFront());
}

/** Because 'DocumentSourceSearchMeta' inherits from 'DocumentSourceInternalSearchMongotRemote',
 *  to make sure a DocumentSource is a $search stage and not $searchMeta check it is either:
 *    - a 'DocumentSourceSearch'.
 *    - a 'DocumentSourceInternalSearchMongotRemote' and not a 'DocumentSourceSearchMeta'.
 */
bool isSearchStage(const DocumentSource* stage) {
    return stage &&
        (dynamic_cast<const mongo::DocumentSourceSearch*>(stage) ||
         (dynamic_cast<const mongo::DocumentSourceInternalSearchMongotRemote*>(stage) &&
          !dynamic_cast<const mongo::DocumentSourceSearchMeta*>(stage)));
}

bool isSearchMetaStage(const DocumentSource* stage) {
    return stage && dynamic_cast<const mongo::DocumentSourceSearchMeta*>(stage);
}

bool isMongotStage(DocumentSource* stage) {
    return stage &&
        (dynamic_cast<mongo::DocumentSourceSearch*>(stage) ||
         dynamic_cast<mongo::DocumentSourceInternalSearchMongotRemote*>(stage) ||
         dynamic_cast<mongo::DocumentSourceVectorSearch*>(stage) ||
         dynamic_cast<mongo::DocumentSourceListSearchIndexes*>(stage) ||
         dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage));
}

void assertSearchMetaAccessValid(const DocumentSourceContainer& pipeline,
                                 ExpressionContext* expCtx) {
    if (pipeline.empty()) {
        return;
    }

    // If we already validated this pipeline on router, no need to do it again on a shard. Check
    // for $mergeCursors because we could be on a shard doing the merge and only want to validate if
    // we have the whole pipeline.
    bool alreadyValidated = !expCtx->getInRouter() && expCtx->getNeedsMerge();
    if (!alreadyValidated ||
        pipeline.front()->getSourceName() != DocumentSourceMergeCursors::kStageName) {
        assertSearchMetaAccessValidHelper({&pipeline});
    }
}

void assertSearchMetaAccessValid(const DocumentSourceContainer& shardsPipeline,
                                 const DocumentSourceContainer& mergePipeline,
                                 ExpressionContext* expCtx) {
    assertSearchMetaAccessValidHelper({&shardsPipeline, &mergePipeline});
}

std::unique_ptr<Pipeline> prepareSearchForTopLevelPipelineLegacyExecutor(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    Pipeline* origPipeline,
    DocsNeededBounds bounds,
    boost::optional<int64_t> userBatchSize) {
    // First, desuguar $search, and inject shard filterer.
    desugarSearchPipeline(origPipeline);

    // TODO SERVER-94874 Establish mongot cursor for $searchMeta queries too.
    if ((expCtx->getExplain() &&
         !feature_flags::gFeatureFlagSearchExplainExecutionStats.isEnabled()) ||
        expCtx->getExplain() == ExplainOptions::Verbosity::kQueryPlanner ||
        !isSearchPipeline(origPipeline)) {
        // This path is for scenarios where we know we only need one cursor. For $search in
        // queryPlanner verbosity, we don't need both the results and metadata cursors. $searchMeta
        // or $vectorSearch pipelines also won't need an additional pipeline since they only need
        // one cursor.
        return nullptr;
    }

    auto origSearchStage =
        dynamic_cast<DocumentSourceInternalSearchMongotRemote*>(origPipeline->peekFront());
    tassert(6253727, "Expected search stage", origSearchStage);
    origSearchStage->setDocsNeededBounds(bounds);

    // We expect to receive unmerged metadata documents from mongot if we are not in router and have
    // a metadata merge protocol version. However, we can ignore the meta cursor if the pipeline
    // doesn't have a downstream reference to $$SEARCH_META.
    auto expectsMetaCursorFromMongot =
        !expCtx->getInRouter() && origSearchStage->getIntermediateResultsProtocolVersion();
    auto shouldBuildMetadataPipeline =
        expectsMetaCursorFromMongot && origSearchStage->queryReferencesSearchMeta();

    // Some tests build $search pipelines without actually setting up a mongot. In this case either
    // return a dummy stage or nothing depending on the environment. Note that in this case we don't
    // actually make any queries, the document source will return eof immediately.
    if (MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        if (shouldBuildMetadataPipeline) {
            // Construct a duplicate ExpressionContext for our cloned pipeline. This is necessary
            // so that the duplicated pipeline and the cloned pipeline do not accidentally
            // share an OperationContext.
            auto newExpCtx = makeCopyFromExpressionContext(
                expCtx, expCtx->getNamespaceString(), expCtx->getUUID());
            return Pipeline::create({origSearchStage->clone(newExpCtx)}, newExpCtx);
        }
        return nullptr;
    }

    // The search stage has not yet established its cursor on mongoT. Establish the cursor for it.
    auto cursors =
        mongot_cursor::establishCursorsForSearchStage(expCtx,
                                                      origSearchStage->getMongotRemoteSpec(),
                                                      origSearchStage->getTaskExecutor(),
                                                      userBatchSize,
                                                      nullptr,
                                                      origSearchStage->getSearchIdLookupMetrics());

    // mongot can return zero cursors for an empty collection, one without metadata, or two for
    // results and metadata.
    tassert(6253500, "Expected less than or exactly two cursors from mongot", cursors.size() <= 2);

    std::unique_ptr<Pipeline> newPipeline = nullptr;
    for (auto& cursor : cursors) {
        auto maybeCursorLabel = cursor->getType();
        if (!maybeCursorLabel) {
            // If a cursor is unlabeled mongot does not support metadata cursors. $$SEARCH_META
            // should not be supported in this query.
            tassert(6253301,
                    "Expected cursors to be labeled if there are more than one",
                    cursors.size() == 1);
            origSearchStage->setCursor(std::move(cursors.front()));
            return nullptr;
        }
        switch (*maybeCursorLabel) {
            case CursorTypeEnum::DocumentResult:
                origSearchStage->setCursor(std::move(cursor));
                origPipeline->pipelineType = CursorTypeEnum::DocumentResult;
                break;
            case CursorTypeEnum::SearchMetaResult:
                // If we don't think we're in a sharded environment, mongot should not have sent
                // metadata.
                tassert(6253303,
                        "Didn't expect metadata cursor from mongot",
                        expectsMetaCursorFromMongot);
                tassert(6253726,
                        "Expected to not already have created a metadata pipeline",
                        !newPipeline);

                // Only create the new metadata pipeline if the original pipeline needs it. If we
                // don't create this new pipeline, the meta cursor returned from mongot will be
                // killed by the task_executor_cursor destructor when it goes out of scope below.
                if (shouldBuildMetadataPipeline) {
                    // Construct a duplicate ExpressionContext for our cloned pipeline. This is
                    // necessary so that the duplicated pipeline and the cloned pipeline do not
                    // accidentally share an OperationContext.
                    auto newExpCtx = makeCopyFromExpressionContext(
                        expCtx, expCtx->getNamespaceString(), expCtx->getUUID());

                    // Clone the MongotRemote stage and set the metadata cursor.
                    auto newStage =
                        origSearchStage->copyForAlternateSource(std::move(cursor), newExpCtx);

                    // Build a new pipeline with the metadata source as the only stage.
                    newPipeline = Pipeline::create({newStage}, newExpCtx);
                    newPipeline->pipelineType = *maybeCursorLabel;
                }
                break;
        }
    }

    // Can return null if we did not build a metadata pipeline.
    return newPipeline;
}

void prepareSearchForNestedPipelineLegacyExecutor(Pipeline* pipeline) {
    desugarSearchPipeline(pipeline);
    // TODO SERVER-94874 Establish mongot cursor here, like is done in
    // prepareSearchForTopLevelPipelineLegacyExecutor.
}

void establishSearchCursorsSBE(boost::intrusive_ptr<ExpressionContext> expCtx,
                               DocumentSource* stage,
                               std::unique_ptr<PlanYieldPolicy> yieldPolicy) {
    if (!expCtx->getUUID() || !isSearchStage(stage) ||
        MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return;
    }
    auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage);
    auto executor =
        executor::getMongotTaskExecutor(expCtx->getOperationContext()->getServiceContext());

    auto cursors = mongot_cursor::establishCursorsForSearchStage(
        expCtx, searchStage->getMongotRemoteSpec(), executor, boost::none, std::move(yieldPolicy));

    auto [documentCursor, metaCursor] = parseMongotResponseCursors(std::move(cursors));

    if (documentCursor) {
        searchStage->setRemoteCursorVars(documentCursor->getCursorVars());
        searchStage->setCursor(std::move(documentCursor));
    }

    if (metaCursor) {
        // If we don't think we're in a sharded environment, mongot should not have sent
        // metadata.
        tassert(7856002,
                "Didn't expect metadata cursor from mongot",
                !expCtx->getInRouter() && searchStage->getIntermediateResultsProtocolVersion());
        searchStage->setMetadataCursor(std::move(metaCursor));
    }
}

void establishSearchMetaCursorSBE(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  DocumentSource* stage,
                                  std::unique_ptr<PlanYieldPolicy> yieldPolicy) {
    if (!expCtx->getUUID() || !isSearchMetaStage(stage) ||
        MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return;
    }

    auto searchStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage);
    auto executor =
        executor::getMongotTaskExecutor(expCtx->getOperationContext()->getServiceContext());
    auto cursors = mongot_cursor::establishCursorsForSearchMetaStage(
        expCtx,
        searchStage->getSearchQuery(),
        executor,
        searchStage->getIntermediateResultsProtocolVersion(),
        std::move(yieldPolicy));

    auto [documentCursor, metaCursor] = parseMongotResponseCursors(std::move(cursors));

    if (metaCursor) {
        searchStage->setCursor(std::move(metaCursor));
    } else {
        tassert(7856203,
                "If there's one cursor we expect to get SEARCH_META from the attached vars",
                !searchStage->getIntermediateResultsProtocolVersion() &&
                    !documentCursor->getType() && documentCursor->getCursorVars());
        searchStage->setRemoteCursorVars(documentCursor->getCursorVars());
        searchStage->setCursor(std::move(documentCursor));
    }
}

bool encodeSearchForSbeCache(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             DocumentSource* ds,
                             BufBuilder* bufBuilder) {
    if ((!isSearchStage(ds) && !isSearchMetaStage(ds))) {
        return false;
    }
    // Encoding for $search/$searchMeta with its stage name, we also includes storedSource flag
    // for $search as well. We don't need to encode other info from the stage, such as search
    // query, limit, sortSpec, because they are all parameterized into slots.
    bufBuilder->appendStrBytes(ds->getSourceName());
    if (auto searchStage = dynamic_cast<DocumentSourceSearch*>(ds)) {
        bufBuilder->appendChar(searchStage->isStoredSource() ? '1' : '0');
        // The remoteCursorId is the offset of the cursor in opCtx, we expect it to be same across
        // query runs, but we encode it in the key for safety. Currently the id is fixed to be '0'
        // because there is only one possible cursor in an executor.
        bufBuilder->appendNum(searchStage->getRemoteCursorId());
    } else if (auto searchStage = dynamic_cast<DocumentSourceSearchMeta*>(ds)) {
        // See comment above for DocumentSourceSearch.
        bufBuilder->appendNum(searchStage->getRemoteCursorId());
    } else {
        MONGO_UNREACHABLE;
    }
    // We usually don't cache explain query, except inside $lookup sub-pipeline.
    bufBuilder->appendChar(expCtx->getExplain() ? '1' : '0');
    return true;
}

std::unique_ptr<executor::TaskExecutorCursor> getSearchMetadataCursor(DocumentSource* ds) {
    if (auto search = dynamic_cast<DocumentSourceSearch*>(ds)) {
        return search->getMetadataCursor();
    }
    return nullptr;
}

std::unique_ptr<RemoteCursorMap> getSearchRemoteCursors(
    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline) {
    if (cqPipeline.empty() || MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return nullptr;
    }
    // We currently only put the first search stage into RemoteCursorMap since only one search
    // is possible in the pipeline and sub-pipeline is in separate PlanExecutorSBE. In the future we
    // will need to recursively check search in every pipeline.
    auto stage = cqPipeline.front().get();
    if (auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage)) {
        auto cursor = searchStage->getCursor();
        if (!cursor) {
            return nullptr;
        }
        auto cursorMap = std::make_unique<RemoteCursorMap>();
        cursorMap->insert({searchStage->getRemoteCursorId(), std::move(cursor)});
        return cursorMap;
    } else if (auto searchMetaStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage)) {
        auto cursor = searchMetaStage->getCursor();
        if (!cursor) {
            return nullptr;
        }
        auto cursorMap = std::make_unique<RemoteCursorMap>();
        cursorMap->insert({searchMetaStage->getRemoteCursorId(), std::move(cursor)});
        return cursorMap;
    }
    return nullptr;
}

std::unique_ptr<RemoteExplainVector> getSearchRemoteExplains(
    const ExpressionContext* expCtx,
    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline) {
    if (cqPipeline.empty() || !expCtx->getExplain() ||
        MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return nullptr;
    }
    // We currently only put the first search stage explain into RemoteExplainVector since only one
    // search is possible in the pipeline and sub-pipeline is in separate PlanExecutorSBE. In the
    // future we will need to recursively check search in every pipeline.
    auto stage = cqPipeline.front().get();
    if (auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage)) {
        auto explainMap = std::make_unique<RemoteExplainVector>();
        explainMap->push_back(
            getSearchRemoteExplain(expCtx,
                                   searchStage->getSearchQuery(),
                                   searchStage->getRemoteCursorId(),
                                   searchStage->getSortSpec(),
                                   mongot_cursor::getOptimizationFlagsForSearch()));
        return explainMap;
    } else if (auto searchMetaStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage)) {
        auto explainMap = std::make_unique<RemoteExplainVector>();
        explainMap->push_back(
            getSearchRemoteExplain(expCtx,
                                   searchMetaStage->getSearchQuery(),
                                   searchMetaStage->getRemoteCursorId(),
                                   boost::none /* sortSpec */,
                                   mongot_cursor::getOptimizationFlagsForSearchMeta()));
        return explainMap;
    }
    return nullptr;
}

boost::optional<SearchQueryViewSpec> getViewFromExpCtx(
    boost::intrusive_ptr<ExpressionContext> expCtx) {
    if (expCtx->getView()) {
        const auto& expCtxView = *expCtx->getView();
        return boost::make_optional(
            SearchQueryViewSpec(std::string(expCtxView.first.coll()), expCtxView.second));
    }

    return boost::none;
}

boost::optional<SearchQueryViewSpec> getViewFromBSONObj(const BSONObj& spec) {
    if (spec.hasField(kViewFieldName) && spec[kViewFieldName].type() == BSONType::object) {
        return SearchQueryViewSpec::parse(spec[kViewFieldName].embeddedObject(),
                                          IDLParserContext("unpack SearchQueryViewSpec"));
    }

    return boost::none;
}

void validateViewNotSetByUser(boost::intrusive_ptr<ExpressionContext> expCtx, const BSONObj& spec) {
    // During $rankFusion parsing, if there's more than 1 mongot input pipeline, a view key will be
    // injected during the parsing of that mongot stage (ex: $search, $vectorSearch, $searchMeta).
    // Since $rankFusion passes the serialized version of the parsed pipeline to a
    // DocumentSource::UnionWith(..) constructor, that serialized pipeline eventually gets reparsed.
    // Because the view key already exists in the pipeline and the internal client flag is not set,
    // this internal client error gets thrown.

    // To avoid that, the isHybridSearch flag is only set after the initial parsing of the
    // user-provided $rankFusion/$scoreFusion pipeline and its value is checked here to avoid
    // throwing an internal client error.
    if (spec.hasField(kViewFieldName) && !expCtx->isHybridSearch()) {
        assertAllowedInternalIfRequired(
            expCtx->getOperationContext(), kViewFieldName, AllowedWithClientType::kInternal);
    }
}

void validateMongotIndexedViewsFF(boost::intrusive_ptr<ExpressionContext> expCtx,
                                  const std::vector<BSONObj>& effectivePipeline) {
    // Queries on views with empty effective pipelines (i.e. identity views) are treated as queries
    // on the underlying collection, therefore allowed on all FCV versions that support search.
    uassert(ErrorCodes::OptionNotSupportedOnView,
            "search stages are unsupported on views",
            effectivePipeline.empty() || expCtx->isFeatureFlagMongotIndexedViewsEnabled());
}

void promoteStoredSourceOrAddIdLookup(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::list<boost::intrusive_ptr<DocumentSource>>& desugaredPipeline,
    bool isStoredSource,
    long long limit,
    boost::optional<SearchQueryViewSpec> view) {
    // If 'returnStoredSource' is true, we don't want to do idLookup. Instead, promote the fields in
    // 'storedSource' to root.
    if (isStoredSource) {
        // {$replaceRoot: {newRoot: {$ifNull: ["$storedSource", "$$ROOT"]}}
        // 'storedSource' is not always present in the document from mongot. If it's present, use it
        // as the root. Otherwise keep the original document.
        BSONObj replaceRootSpec =
            BSON("$replaceRoot" << BSON(
                     "newRoot" << BSON(
                         "$ifNull" << BSON_ARRAY("$" + kProtocolStoredFieldsName << "$$ROOT"))));
        desugaredPipeline.push_back(
            DocumentSourceReplaceRoot::createFromBson(replaceRootSpec.firstElement(), expCtx));
        // Note: intentionally not including a shard filtering operator here. The isolation
        // semantics are already weaker here so this was deemed OK. Potentially part of that
        // conversation: the documents are not guaranteed to have the shard key, and we don't have
        // an idLookup to go get it.
    } else {
        auto shardFilterer = DocumentSourceInternalShardFilter::buildIfNecessary(expCtx);
        // idLookup must always be immediately after the first stage in the desugared pipeline
        auto idLookupStage = make_intrusive<DocumentSourceInternalSearchIdLookUp>(
            expCtx, limit, buildExecShardFilterPolicy(shardFilterer), view);
        desugaredPipeline.insert(std::next(desugaredPipeline.begin()), idLookupStage);
        if (shardFilterer) {
            desugaredPipeline.push_back(std::move(shardFilterer));
        }

        // Check if the first stage in the pipeline is a mongotRemoteStage (only exists for
        // $search).
        auto mongotRemoteStage = dynamic_cast<DocumentSourceInternalSearchMongotRemote*>(
            desugaredPipeline.front().get());
        if (mongotRemoteStage) {
            // Connect the shared search state of these two stages of the pipeline for batch size
            // tuning.
            mongotRemoteStage->setSearchIdLookupMetrics(idLookupStage->getSearchIdLookupMetrics());
        }
    }
}

std::unique_ptr<SearchNode> getSearchNode(DocumentSource* stage) {
    if (search_helpers::isSearchStage(stage)) {
        auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage);
        auto node = std::make_unique<SearchNode>(false,
                                                 searchStage->getSearchQuery(),
                                                 searchStage->getLimit(),
                                                 searchStage->getSortSpec(),
                                                 searchStage->getRemoteCursorId(),
                                                 searchStage->getRemoteCursorVars());
        return node;
    } else if (search_helpers::isSearchMetaStage(stage)) {
        auto searchStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage);
        return std::make_unique<SearchNode>(true,
                                            searchStage->getSearchQuery(),
                                            boost::none /* limit */,
                                            boost::none /* sortSpec */,
                                            searchStage->getRemoteCursorId(),
                                            searchStage->getRemoteCursorVars());
    } else {
        tasserted(7855801, str::stream() << "Unknown stage type" << stage->getSourceName());
    }
}

}  // namespace search_helpers
}  // namespace mongo
