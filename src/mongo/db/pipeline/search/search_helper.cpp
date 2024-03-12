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

#include <list>
#include <set>
#include <string>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_search_meta.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/plan_sharded_search_gen.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/util/assert_util.h"

namespace mongo {
MONGO_FAIL_POINT_DEFINE(searchReturnEofImmediately);

namespace search_helpers {
namespace {
void prepareSearchPipeline(Pipeline* pipeline, bool applyShardFilter) {
    auto searchStage = pipeline->popFrontWithName(DocumentSourceSearch::kStageName);
    auto& sources = pipeline->getSources();
    if (searchStage) {
        auto desugaredPipeline = dynamic_cast<DocumentSourceSearch*>(searchStage.get())->desugar();
        sources.insert(sources.begin(), desugaredPipeline.begin(), desugaredPipeline.end());
        Pipeline::stitch(&sources);
    }

    auto internalSearchLookupIt = sources.begin();
    // Bail early if the pipeline is not $_internalSearchMongotRemote stage or doesn't need to apply
    // shardFilter.
    if (internalSearchLookupIt == sources.end() || !applyShardFilter ||
        (mongo::DocumentSourceInternalSearchMongotRemote::kStageName !=
             (*internalSearchLookupIt)->getSourceName() &&
         mongo::DocumentSourceVectorSearch::kStageName !=
             (*internalSearchLookupIt)->getSourceName())) {
        return;
    }

    while (internalSearchLookupIt != sources.end()) {
        if (DocumentSourceInternalSearchIdLookUp::kStageName ==
            (*internalSearchLookupIt)->getSourceName()) {
            break;
        }
        internalSearchLookupIt++;
    }

    if (internalSearchLookupIt != sources.end()) {
        auto expCtx = pipeline->getContext();
        if (OperationShardingState::isComingFromRouter(expCtx->opCtx)) {
            // We can only rely on the ownership filter if the operation is coming from the router
            // (i.e. it is versioned).
            auto collectionFilter =
                CollectionShardingState::acquire(expCtx->opCtx, expCtx->ns)
                    ->getOwnershipFilter(
                        expCtx->opCtx,
                        CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup);
            auto doc = new DocumentSourceInternalShardFilter(
                expCtx, std::make_unique<ShardFiltererImpl>(std::move(collectionFilter)));
            internalSearchLookupIt++;
            sources.insert(internalSearchLookupIt, doc);
            Pipeline::stitch(&sources);
        }
    }
}

// Asserts that $$SEARCH_META is accessed correctly, that is, it is set by a prior stage, and is
// not accessed in a subpipline. It is assumed that if there is a
// 'DocumentSourceInternalSearchMongotRemote' then '$$SEARCH_META' will be set at some point in the
// pipeline. Depending on the configuration of the cluster,
// 'DocumentSourceSetVariableFromSubPipeline' could do the actual setting of the variable, but it
// can only be generated alongside a 'DocumentSourceInternalSearchMongotRemote'.
void assertSearchMetaAccessValidHelper(
    const std::vector<const Pipeline::SourceContainer*>& pipelines) {
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
                               boost::optional<BSONObj> sortSpec) {
    auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    auto explainObj = mongot_cursor::getSearchExplainResponse(expCtx, searchQuery, executor.get());
    BSONObjBuilder builder;
    builder << "id" << static_cast<int>(remoteCursorId) << "mongotQuery" << searchQuery << "explain"
            << explainObj;
    if (sortSpec) {
        builder << "sortSpec" << *sortSpec;
    }
    return builder.obj();
}

std::pair<boost::optional<executor::TaskExecutorCursor>,
          boost::optional<executor::TaskExecutorCursor>>
parseMongotResponseCursors(std::vector<executor::TaskExecutorCursor> cursors) {
    // mongot can return zero cursors for an empty collection, one without metadata, or two for
    // results and metadata.
    tassert(7856000, "Expected less than or exactly two cursors from mongot", cursors.size() <= 2);

    if (cursors.size() == 1 && !cursors[0].getType()) {
        return {std::move(cursors[0]), boost::none};
    }

    std::pair<boost::optional<executor::TaskExecutorCursor>,
              boost::optional<executor::TaskExecutorCursor>>
        result;

    for (auto it = cursors.begin(); it != cursors.end(); it++) {
        auto maybeCursorLabel = it->getType();
        // If a cursor is unlabeled mongot does not support metadata cursors. $$SEARCH_META
        // should not be supported in this query.
        tassert(
            7856001, "Expected cursors to be labeled if there are more than one", maybeCursorLabel);

        switch (*maybeCursorLabel) {
            case CursorTypeEnum::DocumentResult:
                result.first.emplace(std::move(*it));
                break;
            case CursorTypeEnum::SearchMetaResult:
                result.second.emplace(std::move(*it));
                break;
        }
    }
    return result;
}
}  // namespace

std::unique_ptr<Pipeline, PipelineDeleter> generateMetadataPipelineAndAttachCursorsForSearch(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const AggregateCommandRequest& request,
    Pipeline* origPipeline,
    boost::optional<UUID> uuid) {
    if (expCtx->explain || !isSearchPipeline(origPipeline)) {
        // $search doesn't return documents or metadata from explain regardless of the verbosity.
        return nullptr;
    }

    auto origSearchStage =
        dynamic_cast<DocumentSourceInternalSearchMongotRemote*>(origPipeline->peekFront());
    tassert(6253727, "Expected search stage", origSearchStage);

    // We expect to receive unmerged metadata documents from mongot if we are not in mongos and have
    // a metadata merge protocol version. However, we can ignore the meta cursor if the pipeline
    // doesn't have a downstream reference to $$SEARCH_META.
    auto expectsMetaCursorFromMongot =
        !expCtx->inMongos && origSearchStage->getIntermediateResultsProtocolVersion();
    auto shouldBuildMetadataPipeline =
        expectsMetaCursorFromMongot && origSearchStage->queryReferencesSearchMeta();

    uassert(
        6253506, "Cannot have exchange specified in a $search pipeline", !request.getExchange());

    // Some tests build $search pipelines without actually setting up a mongot. In this case either
    // return a dummy stage or nothing depending on the environment. Note that in this case we don't
    // actually make any queries, the document source will return eof immediately.
    if (MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        if (shouldBuildMetadataPipeline) {
            // Construct a duplicate ExpressionContext for our cloned pipeline. This is necessary
            // so that the duplicated pipeline and the cloned pipeline do not accidentally
            // share an OperationContext.
            auto newExpCtx = expCtx->copyWith(expCtx->ns, expCtx->uuid);
            return Pipeline::create({origSearchStage->clone(newExpCtx)}, newExpCtx);
        }
        return nullptr;
    }

    // The search stage has not yet established its cursor on mongoT. Establish the cursor for it.
    auto cursors = mongot_cursor::establishSearchCursors(
        expCtx,
        origSearchStage->getSearchQuery(),
        origSearchStage->getTaskExecutor(),
        origSearchStage->getMongotDocsRequested(),
        buildSearchGetMoreFunc([origSearchStage] { return origSearchStage->calcDocsNeeded(); }),
        origSearchStage->getIntermediateResultsProtocolVersion(),
        origSearchStage->getPaginationFlag());

    // mongot can return zero cursors for an empty collection, one without metadata, or two for
    // results and metadata.
    tassert(6253500, "Expected less than or exactly two cursors from mongot", cursors.size() <= 2);

    if (cursors.size() == 0) {
        origSearchStage->markCollectionEmpty();
    }

    std::unique_ptr<Pipeline, PipelineDeleter> newPipeline = nullptr;
    for (auto it = cursors.begin(); it != cursors.end(); it++) {
        auto maybeCursorLabel = it->getType();
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
                origSearchStage->setCursor(std::move(*it));
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
                    auto newExpCtx = expCtx->copyWith(expCtx->ns, expCtx->uuid);

                    // Clone the MongotRemote stage and set the metadata cursor.
                    auto newStage =
                        origSearchStage->copyForAlternateSource(std::move(*it), newExpCtx);

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

InternalSearchMongotRemoteSpec planShardedSearch(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONObj& searchRequest) {
    // Mongos issues the 'planShardedSearch' command rather than 'search' in order to:
    // * Create the merging pipeline.
    // * Get a sortSpec.
    const auto cmdObj = [&]() {
        PlanShardedSearchSpec cmd(std::string(expCtx->ns.coll()) /* planShardedSearch */,
                                  searchRequest /* query */);

        if (expCtx->explain) {
            cmd.setExplain(BSON("verbosity" << ExplainOptions::verbosityString(*expCtx->explain)));
        }

        // Add the searchFeatures field.
        cmd.setSearchFeatures(
            BSON(SearchFeatures_serializer(SearchFeaturesEnum::kShardedSort) << 1));

        return cmd.toBSON();
    }();
    // Send the planShardedSearch to the remote, retrying on network errors.
    auto response = mongot_cursor::runSearchCommandWithRetries(expCtx, cmdObj);

    InternalSearchMongotRemoteSpec remoteSpec(searchRequest.getOwned(),
                                              response.data["protocolVersion"_sd].Int());
    auto parsedPipeline = mongo::Pipeline::parseFromArray(response.data["metaPipeline"], expCtx);
    remoteSpec.setMergingPipeline(parsedPipeline->serializeToBson());
    if (response.data.hasElement("sortSpec")) {
        remoteSpec.setSortSpec(response.data["sortSpec"].Obj().getOwned());
    }

    return remoteSpec;
}

bool hasReferenceToSearchMeta(const DocumentSource& ds) {
    std::set<Variables::Id> refs;
    ds.addVariableRefs(&refs);
    return Variables::hasVariableReferenceTo(refs,
                                             std::set<Variables::Id>{Variables::kSearchMetaId});
}

bool isSearchPipeline(const Pipeline* pipeline) {
    if (!pipeline || pipeline->getSources().empty()) {
        return false;
    }
    return isSearchStage(pipeline->peekFront());
}

bool isSearchMetaPipeline(const Pipeline* pipeline) {
    if (!pipeline || pipeline->getSources().empty()) {
        return false;
    }
    return isSearchMetaStage(pipeline->peekFront());
}

/** Because 'DocumentSourceSearchMeta' inherits from 'DocumentSourceInternalSearchMongotRemote',
 *  to make sure a DocumentSource is a $search stage and not $searchMeta check it is either:
 *    - a 'DocumentSourceSearch'.
 *    - a 'DocumentSourceInternalSearchMongotRemote' and not a 'DocumentSourceSearchMeta'.
 */
bool isSearchStage(DocumentSource* stage) {
    return stage &&
        (dynamic_cast<mongo::DocumentSourceSearch*>(stage) ||
         (dynamic_cast<mongo::DocumentSourceInternalSearchMongotRemote*>(stage) &&
          !dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage)));
}

bool isSearchMetaStage(DocumentSource* stage) {
    return stage && dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage);
}

void assertSearchMetaAccessValid(const Pipeline::SourceContainer& pipeline,
                                 ExpressionContext* expCtx) {
    if (pipeline.empty()) {
        return;
    }

    // If we already validated this pipeline on mongos, no need to do it again on a shard. Check
    // mergeCursors because we could be on a shard doing the merge.
    if ((expCtx->inMongos || !expCtx->needsMerge) &&
        pipeline.front()->getSourceName() != DocumentSourceMergeCursors::kStageName) {
        assertSearchMetaAccessValidHelper({&pipeline});
    }
}

void assertSearchMetaAccessValid(const Pipeline::SourceContainer& shardsPipeline,
                                 const Pipeline::SourceContainer& mergePipeline,
                                 ExpressionContext* expCtx) {
    assertSearchMetaAccessValidHelper({&shardsPipeline, &mergePipeline});
}

void prepareSearchForTopLevelPipeline(Pipeline* pipeline) {
    prepareSearchPipeline(pipeline, true);
}

void prepareSearchForNestedPipeline(Pipeline* pipeline) {
    prepareSearchPipeline(pipeline, false);
}

void establishSearchQueryCursors(boost::intrusive_ptr<ExpressionContext> expCtx,
                                 DocumentSource* stage,
                                 std::unique_ptr<PlanYieldPolicy> yieldPolicy) {
    if (!expCtx->uuid || !isSearchStage(stage) ||
        MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return;
    }
    auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage);
    auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    auto cursors =
        mongot_cursor::establishSearchCursors(expCtx,
                                              searchStage->getSearchQuery(),
                                              executor,
                                              searchStage->getLimit(),
                                              nullptr,
                                              searchStage->getIntermediateResultsProtocolVersion(),
                                              searchStage->getSearchPaginationFlag(),
                                              std::move(yieldPolicy));

    auto [documentCursor, metaCursor] = parseMongotResponseCursors(std::move(cursors));

    if (documentCursor) {
        searchStage->setRemoteCursorVars(documentCursor->getCursorVars());
        searchStage->setCursor(std::move(*documentCursor));
    }

    if (metaCursor) {
        // If we don't think we're in a sharded environment, mongot should not have sent
        // metadata.
        tassert(7856002,
                "Didn't expect metadata cursor from mongot",
                !expCtx->inMongos && searchStage->getIntermediateResultsProtocolVersion());
        searchStage->setMetadataCursor(std::move(*metaCursor));
    }
}

void establishSearchMetaCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               DocumentSource* stage,
                               std::unique_ptr<PlanYieldPolicy> yieldPolicy) {
    if (!expCtx->uuid || !isSearchMetaStage(stage) ||
        MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return;
    }

    auto searchStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage);
    auto executor = executor::getMongotTaskExecutor(expCtx->opCtx->getServiceContext());
    auto cursors =
        mongot_cursor::establishSearchCursors(expCtx,
                                              searchStage->getSearchQuery(),
                                              executor,
                                              boost::none,
                                              nullptr,
                                              searchStage->getIntermediateResultsProtocolVersion(),
                                              false /* requiresSearchSequenceToken */,
                                              std::move(yieldPolicy));

    auto [documentCursor, metaCursor] = parseMongotResponseCursors(std::move(cursors));

    if (metaCursor) {
        searchStage->setCursor(std::move(*metaCursor));
    } else {
        tassert(7856203,
                "If there's one cursor we expect to get SEARCH_META from the attached vars",
                !searchStage->getIntermediateResultsProtocolVersion() &&
                    !documentCursor->getType() && documentCursor->getCursorVars());
        searchStage->setRemoteCursorVars(documentCursor->getCursorVars());
        searchStage->setCursor(std::move(*documentCursor));
    }
}

bool encodeSearchForSbeCache(const ExpressionContext* expCtx,
                             DocumentSource* ds,
                             BufBuilder* bufBuilder) {
    if ((!isSearchStage(ds) && !isSearchMetaStage(ds))) {
        return false;
    }
    // Encoding for $search/$searchMeta with its stage name, we also includes storedSource flag
    // for $search as well. We don't need to encode other info from the stage, such as search
    // query, limit, sortSpec, because they are all parameterized into slots.
    bufBuilder->appendStr(ds->getSourceName(), false /* includeEndingNull */);
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
    bufBuilder->appendChar(expCtx->explain ? '1' : '0');
    return true;
}

boost::optional<executor::TaskExecutorCursor> getSearchMetadataCursor(DocumentSource* ds) {
    if (auto search = dynamic_cast<DocumentSourceSearch*>(ds)) {
        return search->getMetadataCursor();
    }
    return boost::none;
}

std::function<void(BSONObjBuilder& bob)> buildSearchGetMoreFunc(
    std::function<boost::optional<long long>()> calcDocsNeeded) {
    if (!calcDocsNeeded) {
        return nullptr;
    }
    return [calcDocsNeeded](BSONObjBuilder& bob) {
        auto docsNeeded = calcDocsNeeded();
        // (Ignore FCV check): This feature is enabled on an earlier FCV.
        if (feature_flags::gFeatureFlagSearchBatchSizeLimit.isEnabledAndIgnoreFCVUnsafe() &&
            docsNeeded.has_value()) {
            BSONObjBuilder cursorOptionsBob(bob.subobjStart(mongot_cursor::kCursorOptionsField));
            cursorOptionsBob.append(mongot_cursor::kDocsRequestedField, docsNeeded.get());
            cursorOptionsBob.doneFast();
        }
    };
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
        cursorMap->insert({searchStage->getRemoteCursorId(),
                           std::make_unique<executor::TaskExecutorCursor>(std::move(*cursor))});
        return cursorMap;
    } else if (auto searchMetaStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage)) {
        auto cursor = searchMetaStage->getCursor();
        if (!cursor) {
            return nullptr;
        }
        auto cursorMap = std::make_unique<RemoteCursorMap>();
        cursorMap->insert({searchMetaStage->getRemoteCursorId(),
                           std::make_unique<executor::TaskExecutorCursor>(std::move(*cursor))});
        return cursorMap;
    }
    return nullptr;
}

std::unique_ptr<RemoteExplainVector> getSearchRemoteExplains(
    const ExpressionContext* expCtx,
    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline) {
    if (cqPipeline.empty() || !expCtx->explain ||
        MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return nullptr;
    }
    // We currently only put the first search stage explain into RemoteExplainVector since only one
    // search is possible in the pipeline and sub-pipeline is in separate PlanExecutorSBE. In the
    // future we will need to recursively check search in every pipeline.
    auto stage = cqPipeline.front().get();
    if (auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(stage)) {
        auto explainMap = std::make_unique<RemoteExplainVector>();
        explainMap->push_back(getSearchRemoteExplain(expCtx,
                                                     searchStage->getSearchQuery(),
                                                     searchStage->getRemoteCursorId(),
                                                     searchStage->getSortSpec()));
        return explainMap;
    } else if (auto searchMetaStage = dynamic_cast<mongo::DocumentSourceSearchMeta*>(stage)) {
        auto explainMap = std::make_unique<RemoteExplainVector>();
        explainMap->push_back(getSearchRemoteExplain(expCtx,
                                                     searchMetaStage->getSearchQuery(),
                                                     searchMetaStage->getRemoteCursorId(),
                                                     boost::none /* sortSpec */));
        return explainMap;
    }
    return nullptr;
}
}  // namespace search_helpers
}  // namespace mongo
