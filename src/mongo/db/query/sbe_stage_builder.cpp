/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_stage_builder.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/inlined_vector.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/exec/sbe/sort_spec.h"
#include "mongo/db/exec/sbe/stages/agg_project.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/column_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/search_cursor.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/pipeline/abt/field_map_builder.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/search_helper.h"
#include "mongo/db/pipeline/window_function/window_function_first_last_n.h"
#include "mongo/db/query/bind_input_params.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/sbe_stage_builder_abt_helpers.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_impl.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_coll_scan.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr_helpers.h"
#include "mongo/db/query/sbe_stage_builder_window_function.h"
#include "mongo/db/query/shard_filterer_factory_impl.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/id_generator.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {
namespace {
/**
 * Generates an EOF plan. Note that even though this plan will return nothing, it will still define
 * the slots specified by 'reqs'.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateEofPlan(
    PlanNodeId nodeId, const PlanStageReqs& reqs, StageBuilderState& state) {
    PlanStageSlots outputs;

    reqs.forEachReq([&](const std::pair<PlanStageReqs::Type, StringData>& name) {
        auto slot =
            state.env->registerSlot(sbe::value::TypeTags::Nothing, 0, false, state.slotIdGenerator);
        outputs.set(name, slot);
    });

    auto stage = makeLimitCoScanTree(nodeId, 0);
    return {std::move(stage), std::move(outputs)};
}

// Establish the search query cursor and fill in the search slots.
void prepareSearchQueryParameters(PlanStageData* data, const CanonicalQuery& cq) {
    if (cq.cqPipeline().empty()) {
        return;
    }
    auto& searchHelper = getSearchHelpers(cq.getOpCtx()->getServiceContext());
    auto stage = cq.cqPipeline().front()->documentSource();
    if (!searchHelper->isSearchStage(stage) && !searchHelper->isSearchMetaStage(stage)) {
        return;
    }

    // Build a SearchNode in order to retrieve the search info.
    auto sn = searchHelper->getSearchNode(stage);
    auto& env = data->env;

    // TODO: SERVER-78560 handle the second cursor (search metadata cursor).
    auto [cursor, _] = searchHelper->establishSearchQueryCursors(cq.getExpCtxRaw(), sn.get());
    auto firstBatch = cursor.releaseBatch();
    auto cursorId = cursor.getCursorId();

    // Set values for cursorId and first batch slots.
    env->resetSlot(env->getSlot("searchCursorId"_sd),
                   sbe::value::TypeTags::NumberInt64,
                   cursorId,
                   true /* owned */);

    BSONArrayBuilder firstBatchBuilder;
    for (const auto& obj : firstBatch) {
        firstBatchBuilder.append(obj);
    }

    auto [firstBatchTag, firstBatchVal] = stage_builder::makeValue(firstBatchBuilder.arr());
    env->resetSlot(
        env->getSlot("searchFirstBatch"_sd), firstBatchTag, firstBatchVal, true /* owned */);

    // Set value for search query.
    auto [searchQueryTag, searchQueryVal] = stage_builder::makeValue(sn->searchQuery);
    env->resetSlot(
        env->getSlot("searchQuery"_sd), searchQueryTag, searchQueryVal, true /* owned */);

    // Set values for QSN slots.
    if (sn->limit) {
        env->resetSlot(env->getSlot("searchLimit"_sd),
                       sbe::value::TypeTags::NumberInt64,
                       *sn->limit,
                       true /* owned */);
    }

    if (sn->intermediateResultsProtocolVersion) {
        env->resetSlot(env->getSlot("searchProtocolVersion"_sd),
                       sbe::value::TypeTags::NumberInt32,
                       *sn->intermediateResultsProtocolVersion,
                       true /* owned */);
    }

    if (cursor.getVarsField()) {
        auto name = Variables::getBuiltinVariableName(Variables::kSearchMetaId);
        // Variables on the cursor must be an object.
        auto varsObj = cursor.getVarsField()->getField(name);
        if (varsObj.ok()) {
            auto [tag, val] = sbe::bson::convertFrom<false /* View */>(varsObj);
            env->resetSlot(env->getSlot(name), tag, val, true);
            cq.getExpCtx()->variables.setReservedValue(
                Variables::kSearchMetaId, mongo::Value(varsObj), true);
        }
    }
}
}  // namespace

sbe::value::SlotVector getSlotsToForward(const PlanStageReqs& reqs,
                                         const PlanStageSlots& outputs,
                                         const sbe::value::SlotVector& exclude) {
    std::vector<std::pair<PlanStageSlots::Name, sbe::value::SlotId>> pairs;
    if (exclude.empty()) {
        outputs.forEachSlot(reqs, [&](auto&& slot, const PlanStageSlots::Name& name) {
            pairs.emplace_back(name, slot.slotId);
        });
    } else {
        auto excludeSet = sbe::value::SlotSet{exclude.begin(), exclude.end()};
        outputs.forEachSlot(reqs, [&](auto&& slot, const PlanStageSlots::Name& name) {
            if (!excludeSet.count(slot.slotId)) {
                pairs.emplace_back(name, slot.slotId);
            }
        });
    }
    std::sort(pairs.begin(), pairs.end());

    auto outputSlots = sbe::makeSV();
    outputSlots.reserve(pairs.size());
    for (auto&& p : pairs) {
        outputSlots.emplace_back(p.second);
    }
    return outputSlots;
}

/**
 * Performs necessary initialization steps to execute an SBE tree 'root', including binding params
 * from the current query 'cq' into the plan if it was cloned from the SBE plan cache.
 *   root - root node of the execution tree
 *   data - slot metadata (not actual parameter data!) that goes with the execution tree
 *   preparingFromCache - if true, 'root' and 'data' may have come from the SBE plan cache (though
 *     sometimes the caller says true even for non-cached plans). This means current parameters from
 *     'cq' need to be substituted into the execution plan.
 */
void prepareSlotBasedExecutableTree(OperationContext* opCtx,
                                    sbe::PlanStage* root,
                                    PlanStageData* data,
                                    const CanonicalQuery& cq,
                                    const MultipleCollectionAccessor& collections,
                                    PlanYieldPolicySBE* yieldPolicy,
                                    const bool preparingFromCache) {
    tassert(6183502, "PlanStage cannot be null", root);
    tassert(6142205, "PlanStageData cannot be null", data);
    tassert(6142206, "yieldPolicy cannot be null", yieldPolicy);

    root->attachToOperationContext(opCtx);
    root->attachNewYieldPolicy(yieldPolicy);

    // Call markShouldCollectTimingInfo() if appropriate.
    auto expCtx = cq.getExpCtxRaw();
    tassert(6142207, "No expression context", expCtx);
    if (expCtx->explain || expCtx->mayDbProfile) {
        root->markShouldCollectTimingInfo();
    }

    // Register this plan to yield according to the configured policy.
    yieldPolicy->registerPlan(root);

    auto& env = data->env;

    root->prepare(env.ctx);

    // Populate/renew "shardFilterer" if there exists a "shardFilterer" slot. The slot value should
    // be set to Nothing in the plan cache to avoid extending the lifetime of the ownership filter.
    if (auto shardFiltererSlot = env->getSlotIfExists("shardFilterer"_sd)) {
        populateShardFiltererSlot(opCtx, *env, *shardFiltererSlot, collections);
    }

    // Refresh "let" variables in the 'RuntimeEnvironment'.
    auto ids = expCtx->variablesParseState.getDefinedVariableIDs();
    auto& variables = expCtx->variables;
    for (auto id : ids) {
        // Variables defined in "ExpressionContext" may not always be translated into SBE slots.
        if (auto it = data->staticData->variableIdToSlotMap.find(id);
            it != data->staticData->variableIdToSlotMap.end()) {
            auto slotId = it->second;
            auto [tag, val] = sbe::value::makeValue(variables.getValue(id));
            env->resetSlot(slotId, tag, val, true);
        }
    }

    for (auto&& [id, name] : Variables::kIdToBuiltinVarName) {
        // This can happen if the query that created the cache entry had no value for a system
        // variable, whereas the current query has a value for the system variable but does not
        // actually make use of it in the query plan.
        if (id != Variables::kRootId && id != Variables::kRemoveId) {
            if (auto slot = env->getSlotIfExists(name); slot && variables.hasValue(id)) {
                auto [tag, val] = sbe::value::makeValue(variables.getValue(id));
                env->resetSlot(*slot, tag, val, true);
            }
        }
    }

    // This block binds parameters into the main MatchExpression and any additional ones that have
    // been pushed down via 'cq._cqPipeline'. The corresponding SBE plan cache key construction was
    // done in encodeSBE() (canonical_query_encoder.cpp). The main MatchExpression was parameterized
    // in CanonicalQuery::cqInit() and the pushed-down ones in QueryPlanner::extendWithAggPipeline()
    // (query_planner.cpp).
    input_params::bind(cq.getPrimaryMatchExpression(), *data, preparingFromCache);
    for (auto& innerStage : cq.cqPipeline()) {
        auto matchStage = dynamic_cast<DocumentSourceMatch*>(innerStage->documentSource());
        if (matchStage) {
            input_params::bind(matchStage->getMatchExpression(), *data, preparingFromCache);
        }
    }

    interval_evaluation_tree::IndexBoundsEvaluationCache indexBoundsEvaluationCache;
    for (auto&& indexBoundsInfo : data->staticData->indexBoundsEvaluationInfos) {
        input_params::bindIndexBounds(
            cq, indexBoundsInfo, env.runtimeEnv, &indexBoundsEvaluationCache);
    }

    if (preparingFromCache && data->staticData->doSbeClusteredCollectionScan) {
        input_params::bindClusteredCollectionBounds(cq, root, data, env.runtimeEnv);
    }

    prepareSearchQueryParameters(data, cq);
}  // prepareSlotBasedExecutableTree

PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                               sbe::value::SlotIdGenerator* slotIdGenerator) {
    for (const auto& slotName : reqs._slots) {
        _slots[slotName] = TypedSlot{slotIdGenerator->generate(), TypeSignature::kAnyScalarType};
    }
}

namespace {
void getAllNodesByTypeHelper(const QuerySolutionNode* root,
                             StageType type,
                             std::vector<const QuerySolutionNode*>& results) {
    if (root->getType() == type) {
        results.push_back(root);
    }

    for (auto&& child : root->children) {
        getAllNodesByTypeHelper(child.get(), type, results);
    }
}

std::vector<const QuerySolutionNode*> getAllNodesByType(const QuerySolutionNode* root,
                                                        StageType type) {
    std::vector<const QuerySolutionNode*> results;
    getAllNodesByTypeHelper(root, type, results);
    return results;
}

/**
 * Returns pair consisting of:
 *  - First node of the specified type found by pre-order traversal. If node was not found, this
 *    pair element is nullptr.
 *  - Total number of nodes with the specified type in tree.
 */
std::pair<const QuerySolutionNode*, size_t> getFirstNodeByType(const QuerySolutionNode* root,
                                                               StageType type) {
    const QuerySolutionNode* result = nullptr;
    size_t count = 0;
    if (root->getType() == type) {
        result = root;
        count++;
    }

    for (auto&& child : root->children) {
        auto [subTreeResult, subTreeCount] = getFirstNodeByType(child.get(), type);
        if (!result) {
            result = subTreeResult;
        }
        count += subTreeCount;
    }

    return {result, count};
}

std::unique_ptr<fts::FTSMatcher> makeFtsMatcher(OperationContext* opCtx,
                                                const CollectionPtr& collection,
                                                const std::string& indexName,
                                                const fts::FTSQuery* ftsQuery) {
    auto desc = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    tassert(5432209,
            str::stream() << "index descriptor not found for index named '" << indexName
                          << "' in collection '" << collection->ns().toStringForErrorMsg() << "'",
            desc);

    auto entry = collection->getIndexCatalog()->getEntry(desc);
    tassert(5432210,
            str::stream() << "index entry not found for index named '" << indexName
                          << "' in collection '" << collection->ns().toStringForErrorMsg() << "'",
            entry);

    auto accessMethod = static_cast<const FTSAccessMethod*>(entry->accessMethod());
    tassert(5432211,
            str::stream() << "access method is not defined for index named '" << indexName
                          << "' in collection '" << collection->ns().toStringForErrorMsg() << "'",
            accessMethod);

    // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop. In practice, this
    // means that it is illegal to use the StageBuilder on a QuerySolution created by planning a
    // query that contains "no-op" expressions.
    auto query = dynamic_cast<const fts::FTSQueryImpl*>(ftsQuery);
    tassert(5432220, "expected FTSQueryImpl", query);
    return std::make_unique<fts::FTSMatcher>(*query, accessMethod->getSpec());
}
}  // namespace

SlotBasedStageBuilder::SlotBasedStageBuilder(OperationContext* opCtx,
                                             const MultipleCollectionAccessor& collections,
                                             const CanonicalQuery& cq,
                                             const QuerySolution& solution,
                                             PlanYieldPolicySBE* yieldPolicy)
    : BaseType(opCtx, cq, solution),
      _collections(collections),
      _mainNss(cq.nss()),
      _yieldPolicy(yieldPolicy),
      _env(std::make_unique<sbe::RuntimeEnvironment>()),
      _data(std::make_unique<PlanStageStaticData>()),
      _state(_opCtx,
             _env,
             _data.get(),
             _cq.getExpCtxRaw()->variables,
             &_slotIdGenerator,
             &_frameIdGenerator,
             &_spoolIdGenerator,
             &_inListsSet,
             &_collatorMap,
             _cq.getExpCtx(),
             _cq.getExpCtx()->needsMerge,
             _cq.getExpCtx()->allowDiskUse) {
    // Initialize '_data->queryCollator'.
    _data->queryCollator = cq.getCollatorShared();

    // SERVER-52803: In the future if we need to gather more information from the QuerySolutionNode
    // tree, rather than doing one-off scans for each piece of information, we should add a formal
    // analysis pass here.
    // Currently, we assume that each query operates on at most one collection, but a rooted $or
    // queries can have more than one collscan stages with clustered collections.
    auto [node, ct] = getFirstNodeByType(solution.root(), STAGE_COLLSCAN);
    auto [_, orCt] = getFirstNodeByType(solution.root(), STAGE_OR);
    const unsigned long numCollscanStages = ct;
    const unsigned long numOrStages = orCt;
    tassert(7182000,
            str::stream() << "Found " << numCollscanStages << " nodes of type COLLSCAN, and "
                          << numOrStages
                          << " nodes of type OR, expected less than one COLLSCAN nodes or at "
                             "least one OR stage.",
            numCollscanStages <= 1 || numOrStages > 0);

    if (node) {
        auto csn = static_cast<const CollectionScanNode*>(node);

        bool doSbeClusteredCollectionScan = csn->doSbeClusteredCollectionScan();

        _data->shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
        _data->shouldTrackResumeToken = csn->requestResumeToken;
        _data->shouldUseTailableScan = csn->tailable;
        _data->direction = csn->direction;
        _data->doSbeClusteredCollectionScan = doSbeClusteredCollectionScan;

        if (doSbeClusteredCollectionScan) {
            _data->clusterKeyFieldName =
                clustered_util::getClusterKeyFieldName(*(csn->clusteredIndex)).toString();

            const auto& collection = _collections.getMainCollection();
            const CollatorInterface* ccCollator = collection->getDefaultCollator();
            if (ccCollator) {
                _data->ccCollator = ccCollator->cloneShared();
            }
        }
    }
}

SlotBasedStageBuilder::PlanType SlotBasedStageBuilder::build(const QuerySolutionNode* root) {
    // For a given SlotBasedStageBuilder instance, this build() method can only be called once.
    invariant(!_buildHasStarted);
    _buildHasStarted = true;

    const bool needsRecordIdSlot = _data->shouldUseTailableScan || _data->shouldTrackResumeToken ||
        _cq.getForceGenerateRecordId();

    // We always produce a 'resultSlot'.
    PlanStageReqs reqs;
    reqs.set(kResult);

    // We force the root stage to produce a 'recordId' if the iteration can be resumed (via a resume
    // token or a tailable cursor) or if the caller simply expects to be able to read it.
    reqs.setIf(kRecordId, needsRecordIdSlot);

    // Set the target namespace to '_mainNss'. This is necessary as some QuerySolutionNodes that
    // require a collection when stage building do not explicitly name which collection they are
    // targeting.
    reqs.setTargetNamespace(_mainNss);

    // Build the SBE plan stage tree.
    auto [stage, outputs] = build(root, reqs);

    // Assert that we produced a 'resultSlot' and that we produced a 'recordIdSlot' only if it was
    // needed.
    invariant(outputs.has(kResult));
    invariant(reqs.has(kRecordId) == outputs.has(kRecordId));

    _data->resultSlot = outputs.getSlotIfExists(stage_builder::PlanStageSlots::kResult);
    _data->recordIdSlot = outputs.getSlotIfExists(stage_builder::PlanStageSlots::kRecordId);

    return {std::move(stage), PlanStageData(std::move(_env), std::move(_data))};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildCollScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023400, "buildCollScan() does not support kSortKey", !reqs.hasSortKeys());

    auto fields = reqs.getFields();
    auto csn = static_cast<const CollectionScanNode*>(root);
    auto [stage, outputs] = generateCollScan(_state,
                                             getCurrentCollection(reqs),
                                             csn,
                                             std::move(fields),
                                             _yieldPolicy,
                                             reqs.getIsTailableCollScanResumeBranch());

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, TypedSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType});
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      outputs.get(kReturnKey).slotId,
                                      makeFunction("newObj"_sd));
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildVirtualScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    tassert(7182001, "buildVirtualScan() does not support kSortKey", !reqs.hasSortKeys());

    auto vsn = static_cast<const VirtualScanNode*>(root);

    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard inputGuard{inputTag, inputVal};
    auto inputView = sbe::value::getArrayView(inputVal);

    if (vsn->docs.size()) {
        inputView->reserve(vsn->docs.size());
        for (auto& doc : vsn->docs) {
            auto [tag, val] = makeValue(doc);
            inputView->push_back(tag, val);
        }
    }

    inputGuard.reset();
    auto [scanSlots, scanStage] = generateVirtualScanMulti(
        &_slotIdGenerator, vsn->hasRecordId ? 2 : 1, inputTag, inputVal, _yieldPolicy);

    sbe::value::SlotId resultSlot;
    if (vsn->hasRecordId) {
        invariant(scanSlots.size() == 2);
        resultSlot = scanSlots[1];
    } else {
        invariant(scanSlots.size() == 1);
        resultSlot = scanSlots[0];
    }

    PlanStageSlots outputs;

    if (reqs.has(kResult) || reqs.hasFields()) {
        outputs.set(kResult, resultSlot);
    }
    if (reqs.has(kRecordId)) {
        invariant(vsn->hasRecordId);
        invariant(scanSlots.size() == 2);
        outputs.set(kRecordId, scanSlots[0]);
    }

    return {std::move(scanStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildIndexScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto ixn = static_cast<const IndexScanNode*>(root);
    invariant(reqs.has(kReturnKey) || !ixn->addKeyMetadata);

    StringDataSet indexKeyPatternSet;
    for (const auto& elt : ixn->index.keyPattern) {
        indexKeyPatternSet.emplace(elt.fieldNameStringData());
    }

    sbe::IndexKeysInclusionSet fieldBitset, sortKeyBitset;
    auto [fields, additionalFields] = splitVector(
        reqs.getFields(), [&](const std::string& s) { return indexKeyPatternSet.count(s); });
    auto fieldsSet = StringDataSet{fields.begin(), fields.end()};
    size_t i = 0;
    for (const auto& elt : ixn->index.keyPattern) {
        StringData name = elt.fieldNameStringData();
        if (fieldsSet.count(name)) {
            fieldBitset.set(i);
        }
        ++i;
    }

    if (reqs.hasSortKeys()) {
        auto sortKeys = reqs.getSortKeys();
        auto sortKeysSet = StringDataSet{sortKeys.begin(), sortKeys.end()};

        for (auto&& key : sortKeys) {
            tassert(7097208,
                    str::stream() << "Expected sort key '" << key
                                  << "' to be part of index pattern",
                    indexKeyPatternSet.count(key));
        }

        i = 0;
        for (const auto& elt : ixn->index.keyPattern) {
            StringData name = elt.fieldNameStringData();
            if (sortKeysSet.count(name)) {
                sortKeyBitset.set(i);
            }
            ++i;
        }
    }

    if (reqs.has(kReturnKey) || reqs.has(kResult) || !additionalFields.empty()) {
        // If 'reqs' has a kResult or kReturnKey request or if 'additionalFields' is not empty, then
        // we need to get all parts of the index key so that we can create the inflated index key.
        for (int j = 0; j < ixn->index.keyPattern.nFields(); ++j) {
            fieldBitset.set(j);
        }
    }

    // If the slots necessary for performing an index consistency check were not requested in
    // 'reqs', then set 'doIndexConsistencyCheck' to false to avoid generating unnecessary logic.
    bool doIndexConsistencyCheck =
        reqs.has(kSnapshotId) && reqs.has(kIndexIdent) && reqs.has(kIndexKey);

    const auto generateIndexScanFunc =
        ixn->iets.empty() ? generateIndexScan : generateIndexScanWithDynamicBounds;
    auto&& [scanStage, scanOutputs] = generateIndexScanFunc(_state,
                                                            getCurrentCollection(reqs),
                                                            ixn,
                                                            fieldBitset,
                                                            sortKeyBitset,
                                                            _yieldPolicy,
                                                            doIndexConsistencyCheck,
                                                            reqs.has(kIndexKeyPattern));

    auto stage = std::move(scanStage);
    auto outputs = std::move(scanOutputs);

    // Remove the RecordId from the output if we were not requested to produce it.
    if (!reqs.has(PlanStageSlots::kRecordId) && outputs.has(kRecordId)) {
        outputs.clear(kRecordId);
    }

    if (reqs.has(PlanStageSlots::kReturnKey)) {
        sbe::EExpression::Vector args;
        for (auto&& elem : ixn->index.keyPattern) {
            StringData name = elem.fieldNameStringData();
            args.emplace_back(makeStrConstant(name));
            args.emplace_back(
                makeVariable(outputs.get(std::make_pair(PlanStageSlots::kField, name)).slotId));
        }

        auto rawKeyExpr = sbe::makeE<sbe::EFunction>("newObj"_sd, std::move(args));
        outputs.set(PlanStageSlots::kReturnKey,
                    TypedSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType});
        stage = sbe::makeProjectStage(std::move(stage),
                                      ixn->nodeId(),
                                      outputs.get(PlanStageSlots::kReturnKey).slotId,
                                      std::move(rawKeyExpr));
    }

    if (reqs.has(kResult) || !additionalFields.empty()) {
        auto indexKeySlots = sbe::makeSV();
        for (auto&& elem : ixn->index.keyPattern) {
            StringData name = elem.fieldNameStringData();
            indexKeySlots.emplace_back(
                outputs.get(std::make_pair(PlanStageSlots::kField, name)).slotId);
        }

        auto resultSlot = _slotIdGenerator.generate();
        outputs.set(kResult, resultSlot);

        stage = rehydrateIndexKey(
            std::move(stage), ixn->index.keyPattern, ixn->nodeId(), indexKeySlots, resultSlot);
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildCountScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    // COUNT_SCAN node doesn't expected to return index info.
    tassert(5295800, "buildCountScan() does not support kReturnKey", !reqs.has(kReturnKey));
    tassert(5295801, "buildCountScan() does not support kSnapshotId", !reqs.has(kSnapshotId));
    tassert(5295802, "buildCountScan() does not support kIndexIdent", !reqs.has(kIndexIdent));
    tassert(5295803, "buildCountScan() does not support kIndexKey", !reqs.has(kIndexKey));
    tassert(
        5295804, "buildCountScan() does not support kIndexKeyPattern", !reqs.has(kIndexKeyPattern));
    tassert(5295805, "buildCountScan() does not support kSortKey", !reqs.hasSortKeys());

    auto csn = static_cast<const CountScanNode*>(root);

    const auto& collection = getCurrentCollection(reqs);
    auto indexName = csn->index.identifier.catalogName;
    auto indexDescriptor = collection->getIndexCatalog()->findIndexByName(_state.opCtx, indexName);
    auto indexAccessMethod =
        collection->getIndexCatalog()->getEntry(indexDescriptor)->accessMethod()->asSortedData();

    std::unique_ptr<key_string::Value> lowKey, highKey;
    if (csn->iets.empty()) {
        std::tie(lowKey, highKey) =
            makeKeyStringPair(csn->startKey,
                              csn->startKeyInclusive,
                              csn->endKey,
                              csn->endKeyInclusive,
                              indexAccessMethod->getSortedDataInterface()->getKeyStringVersion(),
                              indexAccessMethod->getSortedDataInterface()->getOrdering(),
                              true /* forward */);
    }

    auto [stage, planStageSlots, indexScanBoundsSlots] =
        generateSingleIntervalIndexScan(_state,
                                        collection,
                                        indexName,
                                        indexDescriptor->keyPattern(),
                                        true /* forward */,
                                        std::move(lowKey),
                                        std::move(highKey),
                                        {} /* indexKeysToInclude */,
                                        {} /* indexKeySlots */,
                                        reqs,
                                        _yieldPolicy,
                                        csn->nodeId(),
                                        false /* lowPriority */);

    if (!csn->iets.empty()) {
        tassert(7681500,
                "lowKey and highKey runtime environment slots must be present",
                indexScanBoundsSlots);
        _state.data->indexBoundsEvaluationInfos.emplace_back(IndexBoundsEvaluationInfo{
            csn->index,
            indexAccessMethod->getSortedDataInterface()->getKeyStringVersion(),
            indexAccessMethod->getSortedDataInterface()->getOrdering(),
            1 /* direction */,
            csn->iets,
            {ParameterizedIndexScanSlots::SingleIntervalPlan{indexScanBoundsSlots->first,
                                                             indexScanBoundsSlots->second}}});
    }

    if (csn->index.multikey ||
        (indexDescriptor->getIndexType() == IndexType::INDEX_WILDCARD &&
         indexDescriptor->keyPattern().nFields() > 1)) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage),
            sbe::makeSV(planStageSlots.get(PlanStageSlots::kRecordId).slotId),
            csn->nodeId());
    }

    if (reqs.has(kResult)) {
        // COUNT_SCAN stage doesn't produce any output, make an empty obj for kResult.
        auto resultSlot = _slotIdGenerator.generate();
        planStageSlots.set(kResult, TypedSlot{resultSlot, TypeSignature::kObjectType});
        stage = sbe::makeProjectStage(
            std::move(stage), csn->nodeId(), resultSlot, makeFunction("newObj"));
    }

    planStageSlots.clearNonRequiredSlots(reqs);
    return {std::move(stage), std::move(planStageSlots)};
}

namespace {
SbExpr generatePerColumnPredicate(StageBuilderState& state,
                                  const MatchExpression* me,
                                  SbExpr expr) {
    SbExprBuilder b(state);
    switch (me->matchType()) {
        // These are always safe since they will never match documents missing their field, or where
        // the element is an object or array.
        case MatchExpression::REGEX:
            return generateRegexExpr(
                state, checked_cast<const RegexMatchExpression*>(me), std::move(expr));
        case MatchExpression::MOD:
            return generateModExpr(
                state, checked_cast<const ModMatchExpression*>(me), std::move(expr));
        case MatchExpression::BITS_ALL_SET:
            return generateBitTestExpr(state,
                                       checked_cast<const BitTestMatchExpression*>(me),
                                       sbe::BitTestBehavior::AllSet,
                                       std::move(expr));
        case MatchExpression::BITS_ALL_CLEAR:
            return generateBitTestExpr(state,
                                       checked_cast<const BitTestMatchExpression*>(me),
                                       sbe::BitTestBehavior::AllClear,
                                       std::move(expr));
        case MatchExpression::BITS_ANY_SET:
            return generateBitTestExpr(state,
                                       checked_cast<const BitTestMatchExpression*>(me),
                                       sbe::BitTestBehavior::AnySet,
                                       std::move(expr));
        case MatchExpression::BITS_ANY_CLEAR:
            return generateBitTestExpr(state,
                                       checked_cast<const BitTestMatchExpression*>(me),
                                       sbe::BitTestBehavior::AnyClear,
                                       std::move(expr));
        case MatchExpression::EXISTS:
            return b.makeBoolConstant(true);
        case MatchExpression::LT:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::less,
                                          std::move(expr));
        case MatchExpression::GT:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::greater,
                                          std::move(expr));
        case MatchExpression::EQ:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::eq,
                                          std::move(expr));
        case MatchExpression::LTE:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::lessEq,
                                          std::move(expr));
        case MatchExpression::GTE:
            return generateComparisonExpr(state,
                                          checked_cast<const ComparisonMatchExpression*>(me),
                                          sbe::EPrimBinary::greaterEq,
                                          std::move(expr));
        case MatchExpression::MATCH_IN: {
            const auto* ime = checked_cast<const InMatchExpression*>(me);
            tassert(6988583,
                    "Push-down of non-scalar values in $in is not supported.",
                    !ime->hasNonScalarOrNonEmptyValues());
            return generateInExpr(state, ime, std::move(expr));
        }
        case MatchExpression::TYPE_OPERATOR: {
            const auto* tme = checked_cast<const TypeMatchExpression*>(me);
            const MatcherTypeSet& ts = tme->typeSet();
            return b.makeFunction(
                "typeMatch", std::move(expr), b.makeInt32Constant(ts.getBSONTypeMask()));
        }

        default:
            uasserted(6733605,
                      std::string("Expression ") + me->serialize().toString() +
                          " should not be pushed down as a per-column filter");
    }
    MONGO_UNREACHABLE;
}

SbExpr generateLeafExpr(StageBuilderState& state,
                        const MatchExpression* me,
                        sbe::FrameId lambdaFrameId,
                        sbe::value::SlotId inputSlot) {
    auto lambdaParam = makeVariable(lambdaFrameId, 0);
    const MatchExpression::MatchType mt = me->matchType();

    SbExprBuilder b(state);
    if (mt == MatchExpression::NOT) {
        // NOT cannot be pushed into the cell traversal because for arrays, it should behave as
        // conjunction of negated child predicate on each element of the aray, but if we pushed it
        // into the traversal it would become a disjunction.
        const auto& notMe = checked_cast<const NotMatchExpression*>(me);
        uassert(7040601, "Should have exactly one child under $not", notMe->numChildren() == 1);
        const auto child = notMe->getChild(0);
        auto lambdaExpr = b.makeLocalLambda(
            lambdaFrameId, generatePerColumnPredicate(state, child, std::move(lambdaParam)));

        const MatchExpression::MatchType mtChild = child->matchType();
        auto traverserName =
            (mtChild == MatchExpression::EXISTS || mtChild == MatchExpression::TYPE_OPERATOR)
            ? "traverseCsiCellTypes"
            : "traverseCsiCellValues";
        return b.makeNot(
            b.makeFunction(traverserName, b.makeVariable(inputSlot), std::move(lambdaExpr)));
    } else {
        auto lambdaExpr = b.makeLocalLambda(
            lambdaFrameId, generatePerColumnPredicate(state, me, std::move(lambdaParam)));

        auto traverserName = (mt == MatchExpression::EXISTS || mt == MatchExpression::TYPE_OPERATOR)
            ? "traverseCsiCellTypes"
            : "traverseCsiCellValues";
        return b.makeFunction(traverserName, b.makeVariable(inputSlot), std::move(lambdaExpr));
    }
}

SbExpr generatePerColumnLogicalAndExpr(StageBuilderState& state,
                                       const AndMatchExpression* me,
                                       sbe::FrameId lambdaFrameId,
                                       sbe::value::SlotId inputSlot) {
    const auto cTerms = me->numChildren();
    tassert(7072600, "AND should have at least one child", cTerms > 0);

    SbExpr::Vector leaves;
    leaves.reserve(cTerms);
    for (size_t i = 0; i < cTerms; i++) {
        leaves.push_back(generateLeafExpr(state, me->getChild(i), lambdaFrameId, inputSlot));
    }
    SbExprBuilder b(state);
    // Create the balanced binary tree to keep the tree shallow and safe for recursion.
    return b.makeBalancedBooleanOpTree(sbe::EPrimBinary::logicAnd, std::move(leaves));
}

SbExpr generatePerColumnFilterExpr(StageBuilderState& state,
                                   const MatchExpression* me,
                                   sbe::value::SlotId inputSlot) {
    auto lambdaFrameId = state.frameIdGenerator->generate();

    if (me->matchType() == MatchExpression::AND) {
        return generatePerColumnLogicalAndExpr(
            state, checked_cast<const AndMatchExpression*>(me), lambdaFrameId, inputSlot);
    }

    return generateLeafExpr(state, me, lambdaFrameId, inputSlot);
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildColumnScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023403, "buildColumnScan() does not support kSortKey", !reqs.hasSortKeys());

    auto csn = static_cast<const ColumnIndexScanNode*>(root);
    tassert(6312405,
            "Unexpected filter provided for column scan stage. Expected 'filtersByPath' or "
            "'postAssemblyFilter' to be used instead.",
            !csn->filter);

    PlanStageSlots outputs;

    auto reconstructedRecordSlot = _slotIdGenerator.generate();
    outputs.set(kResult, reconstructedRecordSlot);

    boost::optional<sbe::value::SlotId> ridSlot;

    if (reqs.has(kRecordId)) {
        ridSlot = _slotIdGenerator.generate();
        outputs.set(kRecordId, *ridSlot);
    }

    auto fieldSlotIds = _slotIdGenerator.generateMultiple(csn->allFields.size());
    auto rowStoreSlot = _slotIdGenerator.generate();

    // Get all the paths but make sure "_id" comes first (the order of paths given to the
    // column_scan stage defines the order of fields in the reconstructed record).
    std::vector<std::string> paths;
    paths.reserve(csn->allFields.size());
    bool densePathIncludeInFields = false;
    if (csn->allFields.find("_id") != csn->allFields.end()) {
        paths.push_back("_id");
        densePathIncludeInFields = true;
    }
    for (const auto& path : csn->allFields) {
        if (path != "_id") {
            paths.push_back(path);
        }
    }

    // Identify the filtered columns, if any, and create slots/expressions for them.
    std::vector<sbe::ColumnScanStage::PathFilter> filteredPaths;
    filteredPaths.reserve(csn->filtersByPath.size());
    for (size_t i = 0; i < paths.size(); i++) {
        auto itFilter = csn->filtersByPath.find(paths[i]);
        if (itFilter != csn->filtersByPath.end()) {
            auto filterInputSlot = _slotIdGenerator.generate();

            filteredPaths.emplace_back(
                i,
                generatePerColumnFilterExpr(_state, itFilter->second.get(), filterInputSlot)
                    .extractExpr(_state)
                    .expr,
                filterInputSlot);
        }
    }

    // Tag which of the paths should be included into the output.
    std::vector<bool> includeInOutput(paths.size(), false);
    OrderedPathSet fieldsToProject;  // projection when falling back to the row store
    for (size_t i = 0; i < paths.size(); i++) {
        if (csn->outputFields.find(paths[i]) != csn->outputFields.end()) {
            includeInOutput[i] = true;
            fieldsToProject.insert(paths[i]);
        }
    }

    const optimizer::ProjectionName rootStr = getABTVariableName(rowStoreSlot);
    optimizer::FieldMapBuilder builder(rootStr, true);

    // When building its output document (in 'recordSlot'), the 'ColumnStoreStage' should not try to
    // separately project both a document and its sub-fields (e.g., both 'a' and 'a.b'). Compute the
    // the subset of 'csn->allFields' that only includes a field if no other field in
    // 'csn->allFields' is its prefix.
    fieldsToProject = DepsTracker::simplifyDependencies(std::move(fieldsToProject),
                                                        DepsTracker::TruncateToRootLevel::no);
    for (const std::string& field : fieldsToProject) {
        builder.integrateFieldPath(FieldPath(field),
                                   [](const bool isLastElement, optimizer::FieldMapEntry& entry) {
                                       entry._hasLeadingObj = true;
                                       entry._hasKeep = true;
                                   });
    }

    // Generate the expression that is applied to the row store record (in the case when the result
    // cannot be reconstructed from the index).
    std::unique_ptr<sbe::EExpression> rowStoreExpr = nullptr;

    // Avoid generating the row store expression if the projection is not necessary, as indicated by
    // the extraFieldsPermitted flag of the column store node.
    if (boost::optional<optimizer::ABT> abt;
        !csn->extraFieldsPermitted && (abt = builder.generateABT())) {
        // We might get null abt if no paths were added to the builder. It means we should be
        // projecting an empty object.
        tassert(
            6935000, "ABT must be valid if have fields to project", fieldsToProject.empty() || abt);
        rowStoreExpr = abt ? abtToExpr(*abt, _state).expr
                           : sbe::makeE<sbe::EFunction>("newObj", sbe::EExpression::Vector{});
    }

    std::unique_ptr<sbe::PlanStage> stage =
        std::make_unique<sbe::ColumnScanStage>(getCurrentCollection(reqs)->uuid(),
                                               csn->indexEntry.identifier.catalogName,
                                               std::move(paths),
                                               densePathIncludeInFields,
                                               std::move(includeInOutput),
                                               ridSlot,
                                               reconstructedRecordSlot,
                                               rowStoreSlot,
                                               std::move(rowStoreExpr),
                                               std::move(filteredPaths),
                                               _yieldPolicy,
                                               csn->nodeId());

    // Generate post assembly filter.
    if (csn->postAssemblyFilter) {
        auto filterExpr =
            generateFilter(_state,
                           csn->postAssemblyFilter.get(),
                           TypedSlot{reconstructedRecordSlot, TypeSignature::kAnyScalarType},
                           &outputs);

        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(_state).expr, csn->nodeId());
        }
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildFetch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto fn = static_cast<const FetchNode*>(root);

    // The child must produce a kRecordId slot, as well as all the kMeta and kSortKey slots required
    // by the parent of this FetchNode except for 'resultSlot'. Note that the child does _not_ need
    // to produce any kField slots. Any kField requests by the parent will be handled by the logic
    // below.
    auto child = fn->children[0].get();

    auto [sortKeys, additionalSortKeys] =
        splitVector(reqs.getSortKeys(), [&](const std::string& s) {
            if (child->providedSorts().getIgnoredFields().count(s)) {
                return true;
            }
            for (auto&& part : child->providedSorts().getBaseSortPattern()) {
                if (StringData(s) == part.fieldNameStringData()) {
                    return true;
                }
            }
            return false;
        });

    auto forwardingReqs =
        reqs.copy().clear(kResult).clear(kRecordId).clearAllFields().clearAllSortKeys().setSortKeys(
            std::move(sortKeys));

    auto childReqs = forwardingReqs.copy()
                         .set(kRecordId)
                         .set(kSnapshotId)
                         .set(kIndexIdent)
                         .set(kIndexKey)
                         .set(kIndexKeyPattern);

    auto [stage, outputs] = build(child, childReqs);

    uassert(4822880, "RecordId slot is not defined", outputs.has(kRecordId));
    uassert(
        4953600, "ReturnKey slot is not defined", !reqs.has(kReturnKey) || outputs.has(kReturnKey));
    uassert(5290701, "Snapshot id slot is not defined", outputs.has(kSnapshotId));
    uassert(7566701, "Index ident slot is not defined", outputs.has(kIndexIdent));
    uassert(5290711, "Index key slot is not defined", outputs.has(kIndexKey));
    uassert(5113713, "Index key pattern slot is not defined", outputs.has(kIndexKeyPattern));

    auto fields = reqs.getFields();
    sortKeys = std::move(additionalSortKeys);

    auto topLevelFields =
        appendVectorUnique(getTopLevelFields(fields), getTopLevelFields(sortKeys));

    if (fn->filter) {
        DepsTracker deps;
        match_expression::addDependencies(fn->filter.get(), &deps);
        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            topLevelFields =
                appendVectorUnique(std::move(topLevelFields), getTopLevelFields(deps.fields));
        }
    }

    auto childRidSlot = outputs.get(kRecordId).slotId;

    auto resultSlot = _slotIdGenerator.generate();
    auto ridSlot = _slotIdGenerator.generate();
    auto topLevelFieldSlots = _slotIdGenerator.generateMultiple(topLevelFields.size());

    auto relevantSlots = getSlotsToForward(forwardingReqs, outputs);

    stage = makeLoopJoinForFetch(std::move(stage),
                                 resultSlot,
                                 ridSlot,
                                 topLevelFields,
                                 topLevelFieldSlots,
                                 childRidSlot,
                                 outputs.get(kSnapshotId).slotId,
                                 outputs.get(kIndexIdent).slotId,
                                 outputs.get(kIndexKey).slotId,
                                 outputs.get(kIndexKeyPattern).slotId,
                                 getCurrentCollection(reqs),
                                 root->nodeId(),
                                 std::move(relevantSlots));

    outputs.set(kResult, resultSlot);

    // Only propagate kRecordId if requested.
    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, ridSlot);
    } else {
        outputs.clear(kRecordId);
    }

    for (size_t i = 0; i < topLevelFields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, std::move(topLevelFields[i])),
                    topLevelFieldSlots[i]);
    }

    if (fn->filter) {
        auto filterExpr = generateFilter(_state,
                                         fn->filter.get(),
                                         TypedSlot{resultSlot, TypeSignature::kAnyScalarType},
                                         &outputs);
        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(_state).expr, root->nodeId());
        }
    }

    // Keep track of the number of entries in the "fields" vector that represent our output;
    // anything that gets added past this point by appendVectorUnique is coming from the vector of
    // sort keys.
    size_t numOfFields = fields.size();
    auto sortKeysSet = StringSet{sortKeys.begin(), sortKeys.end()};
    auto fieldsAndSortKeys = appendVectorUnique(std::move(fields), std::move(sortKeys));

    auto [outStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                     fieldsAndSortKeys,
                                                     resultSlot,
                                                     root->nodeId(),
                                                     &_slotIdGenerator,
                                                     _state,
                                                     &outputs);
    stage = std::move(outStage);

    auto collatorSlot = _state.getCollatorSlot();

    sbe::SlotExprPairVector projects;
    for (size_t i = 0; i < fieldsAndSortKeys.size(); ++i) {
        auto name = std::move(fieldsAndSortKeys[i]);
        if (sortKeysSet.count(name)) {
            auto slot = _slotIdGenerator.generate();
            auto sortKeyExpr = makeFillEmptyNull(makeVariable(outSlots[i]));
            if (collatorSlot) {
                sortKeyExpr = makeFunction(
                    "collComparisonKey"_sd, std::move(sortKeyExpr), makeVariable(*collatorSlot));
            }
            projects.emplace_back(slot, std::move(sortKeyExpr));
            outputs.set(std::make_pair(PlanStageSlots::kSortKey, name), slot);
        }
        if (i < numOfFields) {
            outputs.set(std::make_pair(PlanStageSlots::kField, std::move(name)), outSlots[i]);
        }
    }

    if (!projects.empty()) {
        stage =
            sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), root->nodeId());
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLimit(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto ln = static_cast<const LimitNode*>(root);
    boost::optional<long long> skip;

    auto [stage, outputs] = [&]() {
        if (ln->children[0]->getType() == StageType::STAGE_SKIP) {
            // If we have both limit and skip stages and the skip stage is beneath the limit, then
            // we can combine these two stages into one.
            const auto sn = static_cast<const SkipNode*>(ln->children[0].get());
            skip = sn->skip;
            return build(sn->children[0].get(), reqs);
        } else {
            return build(ln->children[0].get(), reqs);
        }
    }();

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), ln->limit, skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSkip(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SkipNode*>(root);
    auto [stage, outputs] = build(sn->children[0].get(), reqs);

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), boost::none, sn->skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

namespace {
/**
 * Given a field path, this function will return an expression that will be true if evaluating the
 * field path involves array traversal at any level of the path (including the leaf field).
 */
SbExpr generateArrayCheckForSort(StageBuilderState& state,
                                 SbExpr inputExpr,
                                 const FieldPath& fp,
                                 FieldIndex level,
                                 sbe::value::FrameIdGenerator* frameIdGenerator,
                                 boost::optional<TypedSlot> fieldSlot = boost::none) {
    invariant(level < fp.getPathLength());

    tassert(8102000,
            "Expected either 'inputExpr' or 'fieldSlot' to be defined",
            !inputExpr.isNull() || fieldSlot.has_value());

    SbExprBuilder b(state);
    auto resultExpr = [&] {
        auto fieldExpr = fieldSlot ? b.makeVariable(fieldSlot->slotId)
                                   : b.makeFunction("getField"_sd,
                                                    std::move(inputExpr),
                                                    b.makeStrConstant(fp.getFieldName(level)));
        if (level == fp.getPathLength() - 1u) {
            return b.makeFunction("isArray"_sd, std::move(fieldExpr));
        }
        sbe::FrameId frameId = frameIdGenerator->generate();
        return b.makeLet(
            frameId,
            SbExpr::makeSeq(std::move(fieldExpr)),
            b.makeBinaryOp(
                sbe::EPrimBinary::logicOr,
                b.makeFunction("isArray"_sd, b.makeVariable(frameId, 0)),
                generateArrayCheckForSort(
                    state, b.makeVariable(frameId, 0), fp, level + 1, frameIdGenerator)));
    }();

    if (level == 0) {
        resultExpr = b.makeFillEmptyFalse(std::move(resultExpr));
    }

    return resultExpr;
}

/**
 * Given a field path, this function recursively builds an expression tree that will produce the
 * corresponding sort key for that path.
 */
std::unique_ptr<sbe::EExpression> generateSortTraverse(
    std::unique_ptr<sbe::EVariable> inputVar,
    bool isAscending,
    boost::optional<sbe::value::SlotId> collatorSlot,
    const FieldPath& fp,
    size_t level,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    boost::optional<sbe::value::SlotId> fieldSlot = boost::none) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    tassert(8102001,
            "Expected either 'inputVar' or 'fieldSlot' to be defined",
            inputVar || fieldSlot.has_value());

    StringData helperFn = isAscending ? "_internalLeast"_sd : "_internalGreatest"_sd;

    // Generate an expression to read a sub-field at the current nested level.
    auto fieldExpr = fieldSlot
        ? makeVariable(*fieldSlot)
        : makeFunction("getField"_sd, std::move(inputVar), makeStrConstant(fp.getFieldName(level)));

    if (level == fp.getPathLength() - 1) {
        // For the last level, we can just return the field slot without the need for a
        // traverse expression.
        auto frameId = fieldSlot ? boost::optional<sbe::FrameId>{}
                                 : boost::make_optional(frameIdGenerator->generate());
        auto var = fieldSlot ? fieldExpr->clone() : makeVariable(*frameId, 0);
        auto moveVar = fieldSlot ? std::move(fieldExpr) : makeMoveVariable(*frameId, 0);

        auto helperArgs = sbe::makeEs(moveVar->clone());
        if (collatorSlot) {
            helperArgs.emplace_back(makeVariable(*collatorSlot));
        }

        // According to MQL's sorting semantics, when a leaf field is an empty array we
        // should use Undefined as the sort key.
        auto resultExpr = sbe::makeE<sbe::EIf>(
            makeFillEmptyFalse(makeFunction("isArray"_sd, std::move(var))),
            makeFillEmptyUndefined(sbe::makeE<sbe::EFunction>(helperFn, std::move(helperArgs))),
            makeFillEmptyNull(std::move(moveVar)));

        if (!fieldSlot) {
            resultExpr = sbe::makeE<sbe::ELocalBind>(
                *frameId, sbe::makeEs(std::move(fieldExpr)), std::move(resultExpr));
        }
        return resultExpr;
    }

    // Prepare a lambda expression that will navigate to the next component of the field path.
    auto lambdaFrameId = frameIdGenerator->generate();
    auto lambdaExpr = sbe::makeE<sbe::ELocalLambda>(
        lambdaFrameId,
        generateSortTraverse(std::make_unique<sbe::EVariable>(lambdaFrameId, 0),
                             isAscending,
                             collatorSlot,
                             fp,
                             level + 1,
                             frameIdGenerator));

    // Generate the traverse expression for the current nested level.
    // Be sure to invoke the least/greatest fold expression only if the current nested level is an
    // array.
    auto frameId = frameIdGenerator->generate();
    auto var = fieldSlot ? makeVariable(*fieldSlot) : makeVariable(frameId, 0);
    auto resultVar = makeMoveVariable(frameId, fieldSlot ? 0 : 1);

    auto binds = sbe::makeEs();
    if (!fieldSlot) {
        binds.emplace_back(std::move(fieldExpr));
    }
    binds.emplace_back(makeFunction(
        "traverseP", var->clone(), std::move(lambdaExpr), makeInt32Constant(1) /* maxDepth */));

    auto helperArgs = sbe::makeEs(resultVar->clone());
    if (collatorSlot) {
        helperArgs.emplace_back(makeVariable(*collatorSlot));
    }

    return sbe::makeE<sbe::ELocalBind>(
        frameId,
        std::move(binds),
        // According to MQL's sorting semantics, when a non-leaf field is an empty array or
        // doesn't exist we should use Null as the sort key.
        makeFillEmptyNull(
            sbe::makeE<sbe::EIf>(makeFillEmptyFalse(makeFunction("isArray"_sd, var->clone())),
                                 sbe::makeE<sbe::EFunction>(helperFn, std::move(helperArgs)),
                                 resultVar->clone())));
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSort(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    tassert(5037001,
            "QueryPlannerAnalysis should not produce a SortNode with an empty sort pattern",
            sortPattern.size() > 0);

    auto child = sn->children[0].get();

    if (auto [ixn, ct] = getFirstNodeByType(root, STAGE_IXSCAN);
        !sn->fetched() && !reqs.has(kResult) && ixn && ct >= 1) {
        return buildSortCovered(root, reqs);
    }

    // getExecutor() should never call into buildSlotBasedExecutableTree() when the query
    // contains $meta, so this assertion should always be true.
    for (const auto& part : sortPattern) {
        tassert(5037002, "Sort with $meta is not supported in SBE", part.fieldPath);
    }

    const bool hasPartsWithCommonPrefix = sortPatternHasPartsWithCommonPrefix(sortPattern);
    auto fields = reqs.getFields();

    if (!hasPartsWithCommonPrefix) {
        DepsTracker deps;
        sortPattern.addDependencies(&deps);
        // If the sort pattern doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            auto topLevelFields = getTopLevelFields(deps.fields);
            fields = appendVectorUnique(std::move(fields), std::move(topLevelFields));
        }
    }

    auto childReqs = reqs.copy().setIf(kResult, hasPartsWithCommonPrefix).setFields(fields);
    auto [stage, childOutputs] = build(child, childReqs);
    auto outputs = std::move(childOutputs);

    auto collatorSlot = _state.getCollatorSlot();

    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;

    if (!hasPartsWithCommonPrefix) {
        // Handle the case where we are using kResult and there are no common prefixes.
        orderBy.reserve(sortPattern.size());

        // Sorting has a limitation where only one of the sort patterns can involve arrays.
        // If there are at least two sort patterns, check the data for this possibility.
        auto failOnParallelArrays = [&]() -> SbExpr {
            SbExprBuilder b(_state);
            auto parallelArraysError =
                b.makeFail(ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            if (sortPattern.size() < 2) {
                // If the sort pattern only has one part, we don't need to generate a "parallel
                // arrays" check.
                return {};
            } else if (sortPattern.size() == 2) {
                // If the sort pattern has two parts, we can generate a simpler expression to
                // perform the "parallel arrays" check.
                auto makeIsNotArrayCheck = [&](const FieldPath& fp) {
                    return b.makeNot(generateArrayCheckForSort(
                        _state,
                        SbExpr{},
                        fp,
                        0 /* level */,
                        &_frameIdGenerator,
                        outputs.getIfExists(
                            std::make_pair(PlanStageSlots::kField, fp.getFieldName(0)))));
                };

                return b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                                      makeIsNotArrayCheck(*sortPattern[0].fieldPath),
                                      b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                     makeIsNotArrayCheck(*sortPattern[1].fieldPath),
                                                     std::move(parallelArraysError)));
            } else {
                // If the sort pattern has three or more parts, we generate an expression to
                // perform the "parallel arrays" check that works (and scales well) for an
                // arbitrary number of sort pattern parts.
                auto makeIsArrayCheck = [&](const FieldPath& fp) {
                    return b.makeBinaryOp(
                        sbe::EPrimBinary::cmp3w,
                        generateArrayCheckForSort(_state,
                                                  SbExpr{},
                                                  fp,
                                                  0,
                                                  &_frameIdGenerator,
                                                  outputs.getIfExists(std::make_pair(
                                                      PlanStageSlots::kField, fp.getFieldName(0)))),
                        b.makeBoolConstant(false));
                };

                auto numArraysExpr = makeIsArrayCheck(*sortPattern[0].fieldPath);
                for (size_t idx = 1; idx < sortPattern.size(); ++idx) {
                    numArraysExpr = b.makeBinaryOp(sbe::EPrimBinary::add,
                                                   std::move(numArraysExpr),
                                                   makeIsArrayCheck(*sortPattern[idx].fieldPath));
                }

                return b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                                      b.makeBinaryOp(sbe::EPrimBinary::lessEq,
                                                     std::move(numArraysExpr),
                                                     b.makeInt32Constant(1)),
                                      std::move(parallelArraysError));
            }
        }();

        if (!failOnParallelArrays.isNull()) {
            stage = sbe::makeProjectStage(std::move(stage),
                                          root->nodeId(),
                                          _slotIdGenerator.generate(),
                                          failOnParallelArrays.extractExpr(_state).expr);
        }

        sbe::SlotExprPairVector sortExpressions;

        for (const auto& part : sortPattern) {
            auto topLevelFieldSlot =
                outputs.get(std::make_pair(PlanStageSlots::kField, part.fieldPath->getFieldName(0)))
                    .slotId;

            std::unique_ptr<sbe::EExpression> sortExpr = generateSortTraverse(nullptr,
                                                                              part.isAscending,
                                                                              collatorSlot,
                                                                              *part.fieldPath,
                                                                              0,
                                                                              &_frameIdGenerator,
                                                                              topLevelFieldSlot);

            // Apply the transformation required by the collation, if specified.
            if (collatorSlot) {
                sortExpr = makeFunction(
                    "collComparisonKey"_sd, std::move(sortExpr), makeVariable(*collatorSlot));
            }
            sbe::value::SlotId sortKeySlot = _slotIdGenerator.generate();
            sortExpressions.emplace_back(sortKeySlot, std::move(sortExpr));

            orderBy.push_back(sortKeySlot);
            direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                                 : sbe::value::SortDirection::Descending);
        }
        stage = sbe::makeS<sbe::ProjectStage>(
            std::move(stage), std::move(sortExpressions), root->nodeId());

    } else {
        // When there's no limit on the sort, the dominating factor is number of comparisons
        // (nlogn). A sort with a limit of k requires only nlogk comparisons. When k is small, the
        // number of key generations (n) can actually dominate the runtime. So for all top-k sorts
        // we use a "cheap" sort key: it's cheaper to construct but more expensive to compare. The
        // assumption here is that k << n.

        const sbe::value::SlotId childResultSlotId = outputs.get(kResult).slotId;

        StringData sortKeyGenerator = sn->limit ? "generateCheapSortKey" : "generateSortKey";

        auto sortSpec = std::make_unique<sbe::SortSpec>(sn->pattern);
        auto sortSpecExpr =
            makeConstant(sbe::value::TypeTags::sortSpec,
                         sbe::value::bitcastFrom<sbe::SortSpec*>(sortSpec.release()));

        const auto fullSortKeySlot = _slotIdGenerator.generate();

        // generateSortKey() will handle the parallel arrays check and sort key traversal for us,
        // so we don't need to generate our own sort key traversal logic in the SBE plan.
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      fullSortKeySlot,
                                      collatorSlot ? makeFunction(sortKeyGenerator,
                                                                  std::move(sortSpecExpr),
                                                                  makeVariable(childResultSlotId),
                                                                  makeVariable(*collatorSlot))
                                                   : makeFunction(sortKeyGenerator,
                                                                  std::move(sortSpecExpr),
                                                                  makeVariable(childResultSlotId)));

        if (sortKeyGenerator == "generateSortKey") {
            // In this case generateSortKey() produces a mem-comparable KeyString so we use for
            // the comparison. We always sort in ascending order because the KeyString takes the
            // ordering into account.
            orderBy = {fullSortKeySlot};
            direction = {sbe::value::SortDirection::Ascending};
        } else {
            // Generate the cheap sort key represented as an array then extract each component into
            // a slot:
            //
            // sort [s1, s2] [asc, dsc] ...
            // project s1=getElement(fullSortKey,0), s2=getElement(fullSortKey,1)
            // project fullSortKey=generateSortKeyCheap(bson)
            sbe::SlotExprPairVector projects;

            int i = 0;
            for (const auto& part : sortPattern) {
                auto sortKeySlot = _slotIdGenerator.generate();

                orderBy.push_back(sortKeySlot);
                direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                                     : sbe::value::SortDirection::Descending);

                projects.emplace_back(sortKeySlot,
                                      makeFunction("sortKeyComponentVectorGetElement",
                                                   makeVariable(fullSortKeySlot),
                                                   makeInt32Constant(i)));
                ++i;
            }
            stage = sbe::makeS<sbe::ProjectStage>(
                std::move(stage), std::move(projects), root->nodeId());
        }
    }

    // Slots for sort stage to forward to parent stage. Values in these slots are not used during
    // sorting.
    auto forwardedSlots = getSlotsToForward(reqs, outputs);

    outputs.clearNonRequiredSlots(reqs);
    if (!reqs.has(kResult)) {
        outputs.clear(kResult);
    }

    stage =
        sbe::makeS<sbe::SortStage>(std::move(stage),
                                   std::move(orderBy),
                                   std::move(direction),
                                   std::move(forwardedSlots),
                                   sn->limit ? sn->limit : std::numeric_limits<std::size_t>::max(),
                                   sn->maxMemoryUsageBytes,
                                   _cq.getExpCtx()->allowDiskUse,
                                   root->nodeId());

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSortCovered(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023404, "buildSortCovered() does not support kResult", !reqs.has(kResult));

    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    tassert(7047600,
            "QueryPlannerAnalysis should not produce a SortNode with an empty sort pattern",
            sortPattern.size() > 0);
    tassert(6023422, "buildSortCovered() expected 'sn' to not be fetched", !sn->fetched());

    auto child = sn->children[0].get();

    // The child must produce all of the slots required by the parent of this SortNode.
    auto childReqs = reqs.copy();

    std::vector<std::string> fields;
    StringDataSet sortPathsSet;
    for (const auto& part : sortPattern) {
        const auto& field = part.fieldPath->fullPath();
        fields.emplace_back(field);
        sortPathsSet.emplace(field);
    }

    childReqs.setFields(std::move(fields));

    auto [stage, outputs] = build(child, childReqs);

    auto collatorSlot = _state.getCollatorSlot();

    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;
    orderBy.reserve(sortPattern.size());
    direction.reserve(sortPattern.size());
    for (const auto& part : sortPattern) {
        // getExecutor() should never call into buildSlotBasedExecutableTree() when the query
        // contains $meta, so this assertion should always be true.
        tassert(7047602, "Sort with $meta is not supported in SBE", part.fieldPath);

        orderBy.push_back(
            outputs.get(std::make_pair(PlanStageSlots::kField, part.fieldPath->fullPath())).slotId);
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    sbe::SlotExprPairVector projects;
    auto makeSortKey = [&](sbe::value::SlotId inputSlot) {
        auto sortKeyExpr = makeFillEmptyNull(makeVariable(inputSlot));
        if (collatorSlot) {
            // If a collation is set, wrap 'sortKeyExpr' with a call to collComparisonKey(). The
            // "comparison keys" returned by collComparisonKey() will be used in 'orderBy' instead
            // of the fields' actual values.
            sortKeyExpr = makeFunction(
                "collComparisonKey"_sd, std::move(sortKeyExpr), makeVariable(*collatorSlot));
        }
        return sortKeyExpr;
    };

    for (size_t idx = 0; idx < orderBy.size(); ++idx) {
        auto sortKeySlot{_slotIdGenerator.generate()};
        projects.emplace_back(sortKeySlot, makeSortKey(orderBy[idx]));
        orderBy[idx] = sortKeySlot;
    }

    stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), root->nodeId());

    // Slots for sort stage to forward to parent stage. Values in these slots are not used during
    // sorting.
    auto forwardedSlots = getSlotsToForward(childReqs, outputs, orderBy);

    stage =
        sbe::makeS<sbe::SortStage>(std::move(stage),
                                   std::move(orderBy),
                                   std::move(direction),
                                   std::move(forwardedSlots),
                                   sn->limit ? sn->limit : std::numeric_limits<std::size_t>::max(),
                                   sn->maxMemoryUsageBytes,
                                   _cq.getExpCtx()->allowDiskUse,
                                   root->nodeId());

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildSortKeyGenerator(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    uasserted(4822883, "Sort key generator in not supported in SBE yet");
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSortMerge(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    auto mergeSortNode = static_cast<const MergeSortNode*>(root);

    const auto sortPattern = SortPattern{mergeSortNode->sort, _cq.getExpCtx()};
    std::vector<sbe::value::SortDirection> direction;

    for (const auto& part : sortPattern) {
        uassert(4822881, "Sorting by expression not supported", !part.expression);
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputKeys;
    std::vector<sbe::value::SlotVector> inputVals;

    std::vector<std::string> sortKeys;
    StringSet sortPatternSet;
    for (auto&& sortPart : sortPattern) {
        sortPatternSet.emplace(sortPart.fieldPath->fullPath());
        sortKeys.emplace_back(sortPart.fieldPath->fullPath());
    }

    // Children must produce all of the slots required by the parent of this SortMergeNode. In
    // addition, children must always produce a 'recordIdSlot' if the 'dedup' flag is true, and
    // they must produce kField slots for each part of the sort pattern.
    auto childReqs =
        reqs.copy().setIf(kRecordId, mergeSortNode->dedup).setSortKeys(std::move(sortKeys));

    for (auto&& child : mergeSortNode->children) {
        sbe::value::SlotVector inputKeysForChild;

        // Children must produce a 'resultSlot' if they produce fetched results.
        auto [stage, outputs] = build(child.get(), childReqs);

        tassert(5184301,
                "SORT_MERGE node must receive a RecordID slot as input from child stage"
                " if the 'dedup' flag is set",
                !mergeSortNode->dedup || outputs.has(kRecordId));

        for (const auto& part : sortPattern) {
            inputKeysForChild.push_back(
                outputs.get(std::make_pair(PlanStageSlots::kSortKey, part.fieldPath->fullPath()))
                    .slotId);
        }

        inputKeys.push_back(std::move(inputKeysForChild));
        inputStages.push_back(std::move(stage));

        auto sv = getSlotsToForward(childReqs, outputs);

        inputVals.push_back(std::move(sv));
    }

    PlanStageSlots outputs(childReqs, &_slotIdGenerator);

    auto outputVals = getSlotsToForward(childReqs, outputs);

    auto stage = sbe::makeS<sbe::SortedMergeStage>(std::move(inputStages),
                                                   std::move(inputKeys),
                                                   std::move(direction),
                                                   std::move(inputVals),
                                                   std::move(outputVals),
                                                   root->nodeId());

    if (mergeSortNode->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId).slotId), root->nodeId());
        // Stop propagating the RecordId output if none of our ancestors are going to use it.
        if (!reqs.has(kRecordId)) {
            outputs.clear(kRecordId);
        }
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildMatch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const MatchNode* mn = static_cast<const MatchNode*>(root);

    bool needChildResultDoc = false;

    std::vector<std::string> fields = reqs.getFields();
    if (mn->filter) {
        DepsTracker filterDeps;
        match_expression::addDependencies(mn->filter.get(), &filterDeps);

        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!filterDeps.needWholeDocument) {
            fields = appendVectorUnique(std::move(fields), getTopLevelFields(filterDeps.fields));
        }

        needChildResultDoc = filterDeps.needWholeDocument;
    }

    // The child must produce all of the slots required by the parent of this MatchNode. Also, if
    // the filter needs the whole document, the child must produce 'kResult' as well.
    PlanStageReqs childReqs = reqs.copy().setIf(kResult, needChildResultDoc);

    childReqs.setFields(std::move(fields));

    auto [stage, outputs] = build(mn->children[0].get(), childReqs);
    if (mn->filter) {
        auto childResultSlot =
            needChildResultDoc ? boost::make_optional(outputs.get(kResult)) : boost::none;

        SbExpr filterExpr = generateFilter(_state, mn->filter.get(), childResultSlot, &outputs);

        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(_state).expr, root->nodeId());
        }
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

/**
 * Create a ProjectStage that evalutes the "newRoot" expression from a $replaceRoot pipeline stage
 * and append it to the root of the SBE plan.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildReplaceRoot(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {

    const ReplaceRootNode* rrn = static_cast<const ReplaceRootNode*>(root);

    DepsTracker newRootDeps;
    expression::addDependencies(rrn->newRoot.get(), &newRootDeps);

    // The $replaceRoot operation only ever needs 'kResult' if there are operations in the 'newRoot'
    // expression that need the whole document.
    PlanStageReqs childReqs = newRootDeps.needWholeDocument
        ? reqs.copy().clearAllFields().set(kResult)
        : reqs.copy().clearAllFields().clear(kResult).setFields(
              getTopLevelFields(newRootDeps.fields));

    auto [stage, outputs] = build(rrn->children[0].get(), childReqs);

    // MQL semantics require $replaceRoot to fail if newRoot expression does not evaluate to an
    // object. We fill empty results with null and wrap the generated expression in an if statement
    // that fails if it does not evaluate to an object.
    auto newRootVar = getABTLocalVariableName(_state.frameId(), 0);
    auto newRootABT = abt::unwrap(
        generateExpression(_state, rrn->newRoot.get(), outputs.getIfExists(kResult), &outputs)
            .extractABT());
    auto validatedNewRootABT = optimizer::make<optimizer::Let>(
        newRootVar,
        makeFillEmptyNull(std::move(newRootABT)),
        optimizer::make<optimizer::If>(
            generateABTNonObjectCheck(newRootVar),
            makeABTFail(ErrorCodes::Error{8105800},
                        "Expression in $replaceRoot/$replaceWith must evaluate to an object"_sd),
            makeVariable(newRootVar)));
    auto validatedNewRootExpression = abtToExpr(validatedNewRootABT, _state);

    // The wrapper checks that we add to 'validatedNewRootExpression' ensure that we will only ever
    // output a result with an object type, even if the type checker does not narrow down the set of
    // possible types that far.
    auto newRootType =
        validatedNewRootExpression.typeSignature.intersect(TypeSignature::kObjectType);
    tassert(8105801,
            str::stream() << "Invalid type deduction from lowered $replaceRoot expression: "
                          << validatedNewRootExpression.typeSignature.typesMask,
            newRootType.typesMask != 0);

    auto resultSlot = _state.slotId();
    stage = makeProjectStage(
        std::move(stage), rrn->nodeId(), resultSlot, std::move(validatedNewRootExpression.expr));

    outputs.set(kResult, {resultSlot, newRootType});
    outputs.clearAllFields();
    outputs.clearNonRequiredSlots(reqs);
    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionSimple(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    using namespace std::literals;
    tassert(6023405, "buildProjectionSimple() does not support kSortKey", !reqs.hasSortKeys());

    auto pn = static_cast<const ProjectionNodeSimple*>(root);

    auto [fields, additionalFields] = splitVector(reqs.getFields(), [&](const std::string& s) {
        return pn->proj.type() == projection_ast::ProjectType::kInclusion
            ? pn->proj.getRequiredFields().count(s)
            : !pn->proj.getExcludedPaths().count(s);
    });

    auto childReqs = reqs.copy().clearAllFields().setFields(std::move(fields));
    if (!additionalFields.empty()) {
        childReqs.set(kResult);
    }

    auto [stage, childOutputs] = build(pn->children[0].get(), childReqs);
    auto outputs = std::move(childOutputs);

    if (reqs.has(kResult) || !additionalFields.empty()) {
        const auto childResult = outputs.get(kResult).slotId;

        sbe::MakeBsonObjStage::FieldBehavior behaviour;
        const OrderedPathSet* fields;
        if (pn->proj.type() == projection_ast::ProjectType::kInclusion) {
            behaviour = sbe::MakeBsonObjStage::FieldBehavior::keep;
            fields = &pn->proj.getRequiredFields();
        } else {
            behaviour = sbe::MakeBsonObjStage::FieldBehavior::drop;
            fields = &pn->proj.getExcludedPaths();
        }

        outputs.set(kResult, TypedSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType});
        stage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(stage),
                                                  outputs.get(kResult).slotId,
                                                  childResult,
                                                  behaviour,
                                                  *fields,
                                                  OrderedPathSet{},
                                                  sbe::makeSV(),
                                                  true,
                                                  false,
                                                  root->nodeId());
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionCovered(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    using namespace std::literals;
    tassert(6023406, "buildProjectionCovered() does not support kSortKey", !reqs.hasSortKeys());

    auto pn = static_cast<const ProjectionNodeCovered*>(root);
    invariant(pn->proj.isSimple());

    tassert(5037301,
            str::stream() << "Can't build covered projection for fetched sub-plan: "
                          << root->toString(),
            !pn->children[0]->fetched());

    // This is a ProjectionCoveredNode, so we will be pulling all the data we need from one index.
    // pn->coveredKeyObj is the "index.keyPattern" from the child (which is either an IndexScanNode
    // or DistinctNode). pn->coveredKeyObj lists all the fields that the index can provide, not the
    // fields that the projection wants. 'pn->proj.getRequiredFields()' lists all of the fields
    // that the projection needs. Since this is a simple covered projection, we're guaranteed that
    // 'pn->proj.getRequiredFields()' is a subset of pn->coveredKeyObj.

    // List out the projected fields in the order they appear in 'coveredKeyObj'.
    std::vector<std::string> fields;
    StringDataSet fieldsSet;
    for (auto&& elt : pn->coveredKeyObj) {
        std::string field(elt.fieldNameStringData());
        if (pn->proj.getRequiredFields().count(field)) {
            fields.emplace_back(std::move(field));
            fieldsSet.emplace(elt.fieldNameStringData());
        }
    }

    // The child must produce all of the slots required by the parent of this ProjectionNodeSimple,
    // except for 'resultSlot' which will be produced by the MakeBsonObjStage below if requested by
    // the caller. In addition to that, the child must produce the index key slots that are needed
    // by this covered projection.
    auto childReqs = reqs.copy().clear(kResult).clearAllFields().setFields(fields);
    auto [stage, childOutputs] = build(pn->children[0].get(), childReqs);
    auto outputs = std::move(childOutputs);

    auto additionalFields =
        filterVector(reqs.getFields(), [&](const std::string& s) { return !fieldsSet.count(s); });

    if (reqs.has(kResult) || !additionalFields.empty()) {
        auto slots = sbe::makeSV();
        std::vector<std::string> names;

        if (fieldsSet.count("_id"_sd)) {
            names.emplace_back("_id"_sd);
            slots.emplace_back(
                outputs.get(std::make_pair(PlanStageSlots::kField, "_id"_sd)).slotId);
        }

        for (const auto& field : fields) {
            if (field != "_id"_sd) {
                names.emplace_back(field);
                slots.emplace_back(
                    outputs.get(std::make_pair(PlanStageSlots::kField, StringData(field))).slotId);
            }
        }

        auto resultSlot = _slotIdGenerator.generate();
        stage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(stage),
                                                  resultSlot,
                                                  boost::none,
                                                  boost::none,
                                                  std::vector<std::string>{},
                                                  std::move(names),
                                                  std::move(slots),
                                                  true,
                                                  false,
                                                  root->nodeId());

        outputs.set(kResult, TypedSlot{resultSlot, TypeSignature::kObjectType});
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionDefault(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    tassert(6023407, "buildProjectionDefault() does not support kSortKey", !reqs.hasSortKeys());

    auto pn = static_cast<const ProjectionNodeDefault*>(root);
    const auto& projection = pn->proj;

    if (const auto [ixn, ct] = getFirstNodeByType(root, STAGE_IXSCAN);
        !pn->fetched() && projection.isInclusionOnly() && ixn && ct >= 1) {
        return buildProjectionDefaultCovered(root, reqs);
    }

    // If the projection doesn't need the whole document, then we take all the top-level fields
    // referenced by expressions in the projection and we add them to 'fields'. At present, we
    // intentionally ignore any basic inclusions that are part of the projection (ex. {a:1})
    // for the purposes of populating 'fields'.
    DepsTracker deps;
    auto [_, nodes] = getProjectionNodes(projection);
    for (auto&& node : nodes) {
        if (node.isExpr()) {
            expression::addDependencies(node.getExpr(), &deps);
        }
    }

    auto fields =
        !deps.needWholeDocument ? getTopLevelFields(deps.fields) : std::vector<std::string>{};

    // The child must produce all of the slots required by the parent of this ProjectionNodeDefault.
    // In addition to that, the child must always produce 'kResult' because it's needed by the
    // projection logic below.
    auto childReqs = reqs.copy().set(kResult).clearAllFields().setFields(fields);

    auto [stage, outputs] = build(pn->children[0].get(), childReqs);

    auto inputSlot = outputs.get(kResult);
    auto projectionExpr =
        generateProjection(_state, &projection, inputSlot.slotId, inputSlot, &outputs)
            .extractExpr(_state);

    auto resultSlot = _state.slotId();
    auto resultStage =
        makeProject(std::move(stage), root->nodeId(), resultSlot, std::move(projectionExpr.expr));

    stage = std::move(resultStage);
    outputs.set(kResult, TypedSlot{resultSlot, projectionExpr.typeSignature});

    outputs.clearAllFields();
    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionDefaultCovered(const QuerySolutionNode* root,
                                                     const PlanStageReqs& reqs) {
    tassert(
        6023408, "buildProjectionDefaultCovered() does not support kSortKey", !reqs.hasSortKeys());

    auto pn = static_cast<const ProjectionNodeDefault*>(root);
    const auto& projection = pn->proj;

    tassert(7055402,
            "buildProjectionDefaultCovered() expected 'pn' to be an inclusion-only projection",
            projection.isInclusionOnly());
    tassert(
        7055403, "buildProjectionDefaultCovered() expected 'pn' to not be fetched", !pn->fetched());

    auto [paths, _] = getProjectionNodes(projection);
    auto fields = std::move(paths);
    auto pathTreeRoot = buildPathTree<boost::optional<sbe::value::SlotId>>(
        fields, BuildPathTreeMode::AssertNoConflictingPaths);

    auto fieldsSet = StringDataSet{fields.begin(), fields.end()};
    auto additionalFields =
        filterVector(reqs.getFields(), [&](const std::string& s) { return !fieldsSet.count(s); });

    auto childReqs = reqs.copy().clear(kResult).clearAllFields().setFields(fields);

    auto [stage, outputs] = build(pn->children[0].get(), childReqs);

    for (size_t i = 0; i < fields.size(); ++i) {
        auto slot = outputs.get(std::make_pair(PlanStageSlots::kField, StringData(fields[i])));
        outputs.set(std::make_pair(PlanStageSlots::kField, fields[i]), slot);
    }

    if (reqs.has(kResult) || !additionalFields.empty()) {
        // Extract slots corresponding to each of the projection field paths.
        for (size_t i = 0; i < fields.size(); i++) {
            auto matchPath = sbe::MatchPath{fields[i]};
            auto node = pathTreeRoot->findLeafNode(matchPath);
            tassert(7580700,
                    str::stream() << "Expected to find '" << fields[i] << "' in the path tree",
                    node != nullptr);

            node->value =
                outputs.get(std::make_pair(PlanStageSlots::kField, StringData(fields[i]))).slotId;
        }
        // Build the expression to create object with requested projection field paths.
        auto resultSlot = _slotIdGenerator.generate();
        outputs.set(kResult, TypedSlot{resultSlot, TypeSignature::kObjectType});

        stage = sbe::makeProjectStage(
            std::move(stage), root->nodeId(), resultSlot, buildNewObjExpr(pathTreeRoot.get()));
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildOr(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto orn = static_cast<const OrNode*>(root);

    bool needChildResultDoc = false;

    auto fields = reqs.getFields();

    if (orn->filter) {
        DepsTracker deps;
        match_expression::addDependencies(orn->filter.get(), &deps);
        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            fields = appendVectorUnique(std::move(fields), getTopLevelFields(deps.fields));
        }

        needChildResultDoc = deps.needWholeDocument;
    }

    // Children must produce all of the slots required by the parent of this OrNode. In addition
    // to that, children must always produce a 'recordIdSlot' if the 'dedup' flag is true, and
    // children must produce a 'resultSlot' if 'filter' needs the whole document.
    auto childReqs = reqs.copy().setIf(kResult, needChildResultDoc).setIf(kRecordId, orn->dedup);

    childReqs.setFields(std::move(fields));

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputSlots;
    for (auto&& child : orn->children) {
        auto [stage, outputs] = build(child.get(), childReqs);

        inputStages.emplace_back(std::move(stage));
        inputSlots.emplace_back(getSlotsToForward(childReqs, outputs));
    }

    // Construct a union stage whose branches are translated children of the 'Or' node.
    PlanStageSlots outputs(childReqs, &_slotIdGenerator);
    auto unionOutputSlots = getSlotsToForward(childReqs, outputs);

    auto stage = sbe::makeS<sbe::UnionStage>(
        std::move(inputStages), std::move(inputSlots), std::move(unionOutputSlots), root->nodeId());

    if (orn->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId).slotId), root->nodeId());
        // Stop propagating the RecordId output if none of our ancestors are going to use it.
        if (!reqs.has(kRecordId)) {
            outputs.clear(kRecordId);
        }
    }

    if (orn->filter) {
        auto resultSlot = outputs.getIfExists(kResult);

        auto filterExpr = generateFilter(_state, orn->filter.get(), resultSlot, &outputs);

        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(_state).expr, root->nodeId());
        }
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildTextMatch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto textNode = static_cast<const TextMatchNode*>(root);
    const auto& coll = getCurrentCollection(reqs);
    tassert(5432212, "no collection object", coll);
    tassert(6023410, "buildTextMatch() does not support kSortKey", !reqs.hasSortKeys());
    tassert(5432215,
            str::stream() << "text match node must have one child, but got "
                          << root->children.size(),
            root->children.size() == 1);
    // TextMatchNode guarantees to produce a fetched sub-plan, but it doesn't fetch itself. Instead,
    // its child sub-plan must be fully fetched, and a text match plan is constructed under this
    // assumption.
    tassert(5432216, "text match input must be fetched", root->children[0]->fetched());

    auto childReqs = reqs.copy().set(kResult);
    auto [stage, outputs] = build(textNode->children[0].get(), childReqs);
    tassert(5432217, "result slot is not produced by text match sub-plan", outputs.has(kResult));

    // Create an FTS 'matcher' to apply 'ftsQuery' to matching documents.
    auto matcher = makeFtsMatcher(
        _opCtx, coll, textNode->index.identifier.catalogName, textNode->ftsQuery.get());

    // Build an 'ftsMatch' expression to match a document stored in the 'kResult' slot using the
    // 'matcher' instance.
    auto ftsMatch =
        makeFunction("ftsMatch",
                     makeConstant(sbe::value::TypeTags::ftsMatcher,
                                  sbe::value::bitcastFrom<fts::FTSMatcher*>(matcher.release())),
                     makeVariable(outputs.get(kResult).slotId));

    // Wrap the 'ftsMatch' expression into an 'if' expression to ensure that it can be applied only
    // to a document.
    auto filter =
        sbe::makeE<sbe::EIf>(makeFunction("isObject", makeVariable(outputs.get(kResult).slotId)),
                             std::move(ftsMatch),
                             sbe::makeE<sbe::EFail>(ErrorCodes::Error{4623400},
                                                    "textmatch requires input to be an object"));

    // Add a filter stage to apply 'ftsQuery' to matching documents and discard documents which do
    // not match.
    stage =
        sbe::makeS<sbe::FilterStage<false>>(std::move(stage), std::move(filter), root->nodeId());

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, TypedSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType});
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      outputs.get(kReturnKey).slotId,
                                      makeFunction("newObj"));
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildReturnKey(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023411, "buildReturnKey() does not support kSortKey", !reqs.hasSortKeys());

    // TODO SERVER-49509: If the projection includes {$meta: "sortKey"}, the result of this stage
    // should also include the sort key. Everything else in the projection is ignored.
    auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);

    // The child must produce all of the slots required by the parent of this ReturnKeyNode except
    // for 'resultSlot'. In addition to that, the child must always produce a 'returnKeySlot'.
    // After build() returns, we take the 'returnKeySlot' produced by the child and store it into
    // 'resultSlot' for the parent of this ReturnKeyNode to consume.
    auto childReqs = reqs.copy().clear(kResult).clearAllFields().set(kReturnKey);
    auto [stage, outputs] = build(returnKeyNode->children[0].get(), childReqs);

    outputs.set(kResult, outputs.get(kReturnKey));
    outputs.clear(kReturnKey);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildEof(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    return generateEofPlan(root->nodeId(), reqs, _state);
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndHash(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto andHashNode = static_cast<const AndHashNode*>(root);

    tassert(6023412, "buildAndHash() does not support kSortKey", !reqs.hasSortKeys());
    tassert(5073711, "need at least two children for AND_HASH", andHashNode->children.size() >= 2);

    auto childReqs = reqs.copy().set(kResult).set(kRecordId).clearAllFields();

    auto outerChild = andHashNode->children[0].get();
    auto innerChild = andHashNode->children[1].get();

    auto [outerStage, outerOutputs] = build(outerChild, childReqs);
    auto outerIdSlot = outerOutputs.get(kRecordId).slotId;
    auto outerResultSlot = outerOutputs.get(kResult).slotId;
    auto outerCondSlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);
    tassert(5073712, "innerOutputs must contain kRecordId slot", innerOutputs.has(kRecordId));
    tassert(5073713, "innerOutputs must contain kResult slot", innerOutputs.has(kResult));
    auto innerIdSlot = innerOutputs.get(kRecordId).slotId;
    auto innerResultSlot = innerOutputs.get(kResult).slotId;
    auto innerSnapshotIdSlot = innerOutputs.getIfExists(kSnapshotId);
    auto innerIndexIdentSlot = innerOutputs.getIfExists(kIndexIdent);
    auto innerIndexKeySlot = innerOutputs.getIfExists(kIndexKey);
    auto innerIndexKeyPatternSlot = innerOutputs.getIfExists(kIndexKeyPattern);

    auto innerCondSlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    auto collatorSlot = _state.getCollatorSlot();

    // Designate outputs.
    PlanStageSlots outputs;

    outputs.set(kResult, innerResultSlot);

    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, innerIdSlot);
    }
    if (reqs.has(kSnapshotId) && innerSnapshotIdSlot) {
        auto slot = *innerSnapshotIdSlot;
        innerProjectSlots.push_back(slot.slotId);
        outputs.set(kSnapshotId, slot);
    }
    if (reqs.has(kIndexIdent) && innerIndexIdentSlot) {
        auto slot = *innerIndexIdentSlot;
        innerProjectSlots.push_back(slot.slotId);
        outputs.set(kIndexIdent, slot);
    }
    if (reqs.has(kIndexKey) && innerIndexKeySlot) {
        auto slot = *innerIndexKeySlot;
        innerProjectSlots.push_back(slot.slotId);
        outputs.set(kIndexKey, slot);
    }
    if (reqs.has(kIndexKeyPattern) && innerIndexKeyPatternSlot) {
        auto slot = *innerIndexKeyPatternSlot;
        innerProjectSlots.push_back(slot.slotId);
        outputs.set(kIndexKeyPattern, slot);
    }

    auto stage = sbe::makeS<sbe::HashJoinStage>(std::move(outerStage),
                                                std::move(innerStage),
                                                outerCondSlots,
                                                outerProjectSlots,
                                                innerCondSlots,
                                                innerProjectSlots,
                                                collatorSlot,
                                                root->nodeId());

    // If there are more than 2 children, iterate all remaining children and hash
    // join together.
    for (size_t i = 2; i < andHashNode->children.size(); i++) {
        auto [childStage, outputs] = build(andHashNode->children[i].get(), childReqs);
        tassert(5073714, "outputs must contain kRecordId slot", outputs.has(kRecordId));
        tassert(5073715, "outputs must contain kResult slot", outputs.has(kResult));
        auto idSlot = outputs.get(kRecordId).slotId;
        auto resultSlot = outputs.get(kResult).slotId;
        auto condSlots = sbe::makeSV(idSlot);
        auto projectSlots = sbe::makeSV(resultSlot);

        // The previous HashJoinStage is always set as the inner stage, so that we can reuse the
        // innerIdSlot and innerResultSlot that have been designated as outputs.
        stage = sbe::makeS<sbe::HashJoinStage>(std::move(childStage),
                                               std::move(stage),
                                               condSlots,
                                               projectSlots,
                                               innerCondSlots,
                                               innerProjectSlots,
                                               collatorSlot,
                                               root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndSorted(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023413, "buildAndSorted() does not support kSortKey", !reqs.hasSortKeys());

    auto andSortedNode = static_cast<const AndSortedNode*>(root);

    // Need at least two children.
    tassert(
        5073706, "need at least two children for AND_SORTED", andSortedNode->children.size() >= 2);

    auto childReqs = reqs.copy().set(kResult).set(kRecordId).clearAllFields();

    auto outerChild = andSortedNode->children[0].get();
    auto innerChild = andSortedNode->children[1].get();

    auto outerChildReqs = childReqs.copy()
                              .clear(kSnapshotId)
                              .clear(kIndexIdent)
                              .clear(kIndexKey)
                              .clear(kIndexKeyPattern);
    auto [outerStage, outerOutputs] = build(outerChild, outerChildReqs);

    auto outerIdSlot = outerOutputs.get(kRecordId).slotId;
    auto outerResultSlot = outerOutputs.get(kResult).slotId;

    auto outerKeySlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);
    tassert(5073707, "innerOutputs must contain kRecordId slot", innerOutputs.has(kRecordId));
    tassert(5073708, "innerOutputs must contain kResult slot", innerOutputs.has(kResult));
    auto innerIdSlot = innerOutputs.get(kRecordId).slotId;
    auto innerResultSlot = innerOutputs.get(kResult).slotId;

    auto innerKeySlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    // Designate outputs.
    PlanStageSlots outputs;

    outputs.set(kResult, innerResultSlot);

    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, innerIdSlot);
    }
    if (reqs.has(kSnapshotId)) {
        auto innerSnapshotSlot = innerOutputs.get(kSnapshotId);
        innerProjectSlots.push_back(innerSnapshotSlot.slotId);
        outputs.set(kSnapshotId, innerSnapshotSlot);
    }
    if (reqs.has(kIndexIdent)) {
        auto innerIndexIdentSlot = innerOutputs.get(kIndexIdent);
        innerProjectSlots.push_back(innerIndexIdentSlot.slotId);
        outputs.set(kIndexIdent, innerIndexIdentSlot);
    }
    if (reqs.has(kIndexKey)) {
        auto innerIndexKeySlot = innerOutputs.get(kIndexKey);
        innerProjectSlots.push_back(innerIndexKeySlot.slotId);
        outputs.set(kIndexKey, innerIndexKeySlot);
    }
    if (reqs.has(kIndexKeyPattern)) {
        auto innerIndexKeyPatternSlot = innerOutputs.get(kIndexKeyPattern);
        innerProjectSlots.push_back(innerIndexKeyPatternSlot.slotId);
        outputs.set(kIndexKeyPattern, innerIndexKeyPatternSlot);
    }

    std::vector<sbe::value::SortDirection> sortDirs(outerKeySlots.size(),
                                                    sbe::value::SortDirection::Ascending);

    auto stage = sbe::makeS<sbe::MergeJoinStage>(std::move(outerStage),
                                                 std::move(innerStage),
                                                 outerKeySlots,
                                                 outerProjectSlots,
                                                 innerKeySlots,
                                                 innerProjectSlots,
                                                 sortDirs,
                                                 root->nodeId());

    // If there are more than 2 children, iterate all remaining children and merge
    // join together.
    for (size_t i = 2; i < andSortedNode->children.size(); i++) {
        auto [childStage, outputs] = build(andSortedNode->children[i].get(), childReqs);
        tassert(5073709, "outputs must contain kRecordId slot", outputs.has(kRecordId));
        tassert(5073710, "outputs must contain kResult slot", outputs.has(kResult));
        auto idSlot = outputs.get(kRecordId).slotId;
        auto resultSlot = outputs.get(kResult).slotId;
        auto keySlots = sbe::makeSV(idSlot);
        auto projectSlots = sbe::makeSV(resultSlot);

        stage = sbe::makeS<sbe::MergeJoinStage>(std::move(childStage),
                                                std::move(stage),
                                                keySlots,
                                                projectSlots,
                                                innerKeySlots,
                                                innerProjectSlots,
                                                sortDirs,
                                                root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

namespace {
template <typename F>
struct FieldPathAndCondPreVisitor : public SelectiveConstExpressionVisitorBase {
    // To avoid overloaded-virtual warnings.
    using SelectiveConstExpressionVisitorBase::visit;

    explicit FieldPathAndCondPreVisitor(const F& fn) : _fn(fn) {}

    void visit(const ExpressionFieldPath* expr) final {
        _fn(expr);
    }

    F _fn;
};

/**
 * Walks through the 'expr' expression tree and whenever finds an 'ExpressionFieldPath', calls
 * the 'fn' function. Type requirement for 'fn' is it must have a const 'ExpressionFieldPath'
 * pointer parameter.
 */
template <typename F>
void walkAndActOnFieldPaths(Expression* expr, const F& fn) {
    FieldPathAndCondPreVisitor<F> preVisitor(fn);
    ExpressionWalker walker(&preVisitor, nullptr /*inVisitor*/, nullptr /*postVisitor*/);
    expression_walker::walk(expr, &walker);
}

// Return true iff 'accStmt' is a $topN or $bottomN operator.
bool isTopBottomN(const AccumulationStatement& accStmt) {
    return accStmt.expr.name == AccumulatorTopBottomN<kTop, true>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kBottom, true>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kTop, false>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kBottom, false>::getName();
}

// Return true iff 'accStmt' is one of $topN, $bottomN, $minN, $maxN, $firstN or $lastN.
bool isAccumulatorN(const AccumulationStatement& accStmt) {
    return accStmt.expr.name == AccumulatorTopBottomN<kTop, true>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kBottom, true>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kTop, false>::getName() ||
        accStmt.expr.name == AccumulatorTopBottomN<kBottom, false>::getName() ||
        accStmt.expr.name == AccumulatorMinN::getName() ||
        accStmt.expr.name == AccumulatorMaxN::getName() ||
        accStmt.expr.name == AccumulatorFirstN::getName() ||
        accStmt.expr.name == AccumulatorLastN::getName();
}

// Compute what values 'groupNode' will need from its child node in order to build expressions for
// the group-by key ("_id") and the accumulators.
MONGO_COMPILER_NOINLINE
PlanStageReqs computeChildReqsForGroup(const PlanStageReqs& reqs, const GroupNode& groupNode) {
    auto childReqs = reqs.copy().set(PlanStageSlots::kResult).clearAllFields();

    // If the group node references any top level fields, we take all of them and add them to
    // 'childReqs'. Note that this happens regardless of whether we need the whole document because
    // it can be the case that this stage references '$$ROOT' as well as some top level fields.
    if (auto topLevelFields = getTopLevelFields(groupNode.requiredFields);
        !topLevelFields.empty()) {
        childReqs.setFields(std::move(topLevelFields));
    }

    if (!groupNode.needWholeDocument) {
        // Tracks whether we need to request kResult.
        bool rootDocIsNeeded = false;
        bool sortKeyIsNeeded = false;
        auto referencesRoot = [&](const ExpressionFieldPath* fieldExpr) {
            rootDocIsNeeded = rootDocIsNeeded || fieldExpr->isROOT();
        };

        // Walk over all field paths involved in this $group stage.
        walkAndActOnFieldPaths(groupNode.groupByExpression.get(), referencesRoot);
        for (const auto& accStmt : groupNode.accumulators) {
            walkAndActOnFieldPaths(accStmt.expr.argument.get(), referencesRoot);
            if (isTopBottomN(accStmt)) {
                sortKeyIsNeeded = true;
            }
        }

        // If any accumulator requires generating sort key, we cannot clear the kResult.
        if (!sortKeyIsNeeded) {
            const auto& childNode = *groupNode.children[0];

            // If the group node doesn't have any dependency (e.g. $count) or if the dependency can
            // be satisfied by the child node (e.g. covered index scan), we can clear the kResult
            // requirement for the child.
            if (groupNode.requiredFields.empty() || !rootDocIsNeeded) {
                childReqs.clear(PlanStageSlots::kResult);
            } else if (childNode.getType() == StageType::STAGE_PROJECTION_COVERED) {
                auto& pn = static_cast<const ProjectionNodeCovered&>(childNode);
                std::set<std::string> providedFieldSet;
                for (auto&& elt : pn.coveredKeyObj) {
                    providedFieldSet.emplace(elt.fieldNameStringData());
                }
                if (std::all_of(groupNode.requiredFields.begin(),
                                groupNode.requiredFields.end(),
                                [&](const std::string& f) { return providedFieldSet.count(f); })) {
                    childReqs.clear(PlanStageSlots::kResult);
                }
            }
        }
    }

    return childReqs;
}

// Search the group-by ('_id') and accumulator expressions of a $group for field path expressions,
// and populate a slot in 'childOutputs' for each path found. Each slot is bound via a ProjectStage
// to an EExpression that evaluates the path traversal.
//
// This function also adds each path it finds to the 'groupFieldSet' output.
MONGO_COMPILER_NOINLINE
SbStage projectPathTraversalsForGroupBy(StageBuilderState& state,
                                        const GroupNode& groupNode,
                                        const PlanStageReqs& childReqs,
                                        SbStage childStage,
                                        PlanStageSlots& childOutputs,
                                        StringSet& groupFieldSet) {
    // Slot to EExpression map that tracks path traversal expressions. Note that this only contains
    // expressions corresponding to paths which require traversals (that is, if there exists a
    // top level field slot corresponding to a field, we take care not to add it to 'projects' to
    // avoid rebinding a slot).
    sbe::SlotExprPairVector projects;

    // Lambda which populates 'projects' and 'childOutputs' with an expression and/or a slot,
    // respectively, corresponding to the value of 'fieldExpr'.
    auto accumulateFieldPaths = [&](const ExpressionFieldPath* fieldExpr) {
        // We optimize neither a field path for the top-level document itself nor a field path
        // that refers to a variable instead.
        if (fieldExpr->getFieldPath().getPathLength() == 1 || fieldExpr->isVariableReference()) {
            return;
        }

        // Don't generate an expression if we have one already.
        std::string fp = fieldExpr->getFieldPathWithoutCurrentPrefix().fullPath();
        if (groupFieldSet.count(fp)) {
            return;
        }

        // Mark 'fp' as being seen and either find a slot corresponding to it or generate an
        // expression for it and bind it to a slot.
        groupFieldSet.insert(fp);
        TypedSlot slot = [&]() -> TypedSlot {
            // Special case: top level fields which already have a slot.
            if (fieldExpr->getFieldPath().getPathLength() == 2) {
                return childOutputs.get({PlanStageSlots::kField, StringData(fp)});
            } else {
                // General case: we need to generate a path traversal expression.
                auto result = stage_builder::generateExpression(
                    state,
                    fieldExpr,
                    childOutputs.getIfExists(PlanStageSlots::kResult),
                    &childOutputs);

                if (result.hasSlot()) {
                    return TypedSlot{*result.getSlot(), TypeSignature::kAnyScalarType};
                } else {
                    auto newSlot = state.slotId();
                    auto expr = result.extractExpr(state);
                    projects.emplace_back(newSlot, std::move(expr.expr));
                    return TypedSlot{newSlot, expr.typeSignature};
                }
            }
        }();

        childOutputs.set(std::make_pair(PlanStageSlots::kPathExpr, std::move(fp)), slot);
    };

    // Walk over all field paths involved in this $group stage.
    walkAndActOnFieldPaths(groupNode.groupByExpression.get(), accumulateFieldPaths);
    for (const auto& accStmt : groupNode.accumulators) {
        walkAndActOnFieldPaths(accStmt.expr.argument.get(), accumulateFieldPaths);
    }

    if (!projects.empty()) {
        childStage = makeProject(std::move(childStage), std::move(projects), groupNode.nodeId());
    }

    return childStage;
}

MONGO_COMPILER_NOINLINE
std::tuple<sbe::value::SlotVector, SbStage, std::unique_ptr<sbe::EExpression>> generateGroupByKey(
    StageBuilderState& state,
    const boost::intrusive_ptr<Expression>& idExpr,
    const PlanStageSlots& outputs,
    SbStage stage,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    auto rootSlot = outputs.getIfExists(PlanStageSlots::kResult);

    if (auto idExprObj = dynamic_cast<ExpressionObject*>(idExpr.get()); idExprObj) {
        sbe::value::SlotVector slots;
        sbe::EExpression::Vector exprs;

        sbe::SlotExprPairVector projects;

        for (auto&& [fieldName, fieldExpr] : idExprObj->getChildExpressions()) {
            auto expr = generateExpression(state, fieldExpr.get(), rootSlot, &outputs);

            auto slot = state.slotId();
            projects.emplace_back(slot, expr.extractExpr(state).expr);

            slots.push_back(slot);
            exprs.emplace_back(makeStrConstant(fieldName));
            exprs.emplace_back(makeVariable(slot));
        }

        if (!projects.empty()) {
            stage = makeProject(std::move(stage), std::move(projects), nodeId);
        }

        // When there's only one field in the document _id expression, 'Nothing' is converted to
        // 'Null'.
        // TODO SERVER-21992: Remove the following block because this block emulates the classic
        // engine's buggy behavior. With index that can handle 'Nothing' and 'Null' differently,
        // SERVER-21992 issue goes away and the distinct scan should be able to return 'Nothing' and
        // 'Null' separately.
        if (slots.size() == 1) {
            auto slot = state.slotId();
            stage =
                makeProject(std::move(stage), nodeId, slot, makeFillEmptyNull(std::move(exprs[1])));

            slots[0] = slot;
            exprs[1] = makeVariable(slots[0]);
        }

        // Composes the _id document and assigns a slot to the result using 'newObj' function if _id
        // should produce a document. For example, resultSlot = newObj(field1, slot1, ..., fieldN,
        // slotN)
        return {slots, std::move(stage), sbe::makeE<sbe::EFunction>("newObj"_sd, std::move(exprs))};
    }

    auto groupByExpr =
        generateExpression(state, idExpr.get(), rootSlot, &outputs).extractExpr(state).expr;

    if (auto groupByExprConstant = groupByExpr->as<sbe::EConstant>(); groupByExprConstant) {
        // When the group id is Nothing (with $$REMOVE for example), we use null instead.
        auto tag = groupByExprConstant->getConstant().first;
        if (tag == sbe::value::TypeTags::Nothing) {
            groupByExpr = makeNullConstant();
        }
        return {sbe::value::SlotVector{}, std::move(stage), std::move(groupByExpr)};
    } else {
        // The group-by field may end up being 'Nothing' and in that case _id: null will be
        // returned. Calling 'makeFillEmptyNull' for the group-by field takes care of that.
        auto fillEmptyNullExpr = makeFillEmptyNull(std::move(groupByExpr));

        auto slot = state.slotId();
        stage = makeProject(std::move(stage), nodeId, slot, std::move(fillEmptyNullExpr));

        return {sbe::value::SlotVector{slot}, std::move(stage), nullptr};
    }
}

template <TopBottomSense sense, bool single>
std::unique_ptr<sbe::EExpression> getSortSpecFromTopBottomN(
    const AccumulatorTopBottomN<sense, single>* acc) {
    tassert(5807013, "Accumulator state must not be null", acc);
    auto sortPattern =
        acc->getSortPattern().serialize(SortPattern::SortKeySerialization::kForExplain).toBson();
    auto sortSpec = std::make_unique<sbe::SortSpec>(sortPattern);
    auto sortSpecExpr = makeConstant(sbe::value::TypeTags::sortSpec,
                                     sbe::value::bitcastFrom<sbe::SortSpec*>(sortSpec.release()));
    return sortSpecExpr;
}

std::unique_ptr<sbe::EExpression> getSortSpecFromTopBottomN(const AccumulationStatement& accStmt) {
    auto acc = accStmt.expr.factory();
    if (accStmt.expr.name == AccumulatorTopBottomN<kTop, true>::getName()) {
        return getSortSpecFromTopBottomN(
            dynamic_cast<AccumulatorTopBottomN<kTop, true>*>(acc.get()));
    } else if (accStmt.expr.name == AccumulatorTopBottomN<kBottom, true>::getName()) {
        return getSortSpecFromTopBottomN(
            dynamic_cast<AccumulatorTopBottomN<kBottom, true>*>(acc.get()));
    } else if (accStmt.expr.name == AccumulatorTopBottomN<kTop, false>::getName()) {
        return getSortSpecFromTopBottomN(
            dynamic_cast<AccumulatorTopBottomN<kTop, false>*>(acc.get()));
    } else if (accStmt.expr.name == AccumulatorTopBottomN<kBottom, false>::getName()) {
        return getSortSpecFromTopBottomN(
            dynamic_cast<AccumulatorTopBottomN<kBottom, false>*>(acc.get()));
    } else {
        MONGO_UNREACHABLE;
    }
}

sbe::value::SlotVector generateAccumulator(StageBuilderState& state,
                                           const AccumulationStatement& accStmt,
                                           const PlanStageSlots& outputs,
                                           sbe::value::SlotIdGenerator* slotIdGenerator,
                                           sbe::AggExprVector& aggSlotExprs,
                                           boost::optional<TypedSlot> initializerRootSlot) {
    auto rootSlot = outputs.getIfExists(PlanStageSlots::kResult);
    auto collatorSlot = state.getCollatorSlot();

    // One accumulator may be translated to multiple accumulator expressions. For example, The
    // $avg will have two accumulators expressions, a sum(..) and a count which is implemented
    // as sum(1).
    auto accExprs = [&]() {
        // $topN/$bottomN accumulators require multiple arguments to the accumulator builder.
        if (isTopBottomN(accStmt)) {
            StringDataMap<std::unique_ptr<sbe::EExpression>> accArgs;
            auto sortSpecExpr = getSortSpecFromTopBottomN(accStmt);
            accArgs.emplace(AccArgs::kTopBottomNSortSpec, sortSpecExpr->clone());

            // Build the key expression for the accumulator.
            tassert(5807014,
                    str::stream() << accStmt.expr.name
                                  << " accumulator must have the root slot set",
                    rootSlot);
            auto key = collatorSlot ? makeFunction("generateCheapSortKey",
                                                   std::move(sortSpecExpr),
                                                   makeVariable(rootSlot->slotId),
                                                   makeVariable(*collatorSlot))
                                    : makeFunction("generateCheapSortKey",
                                                   std::move(sortSpecExpr),
                                                   makeVariable(rootSlot->slotId));
            accArgs.emplace(AccArgs::kTopBottomNKey,
                            makeFunction("sortKeyComponentVectorToArray", std::move(key)));

            // Build the value expression for the accumulator.
            if (auto expObj = dynamic_cast<ExpressionObject*>(accStmt.expr.argument.get())) {
                for (auto& [key, value] : expObj->getChildExpressions()) {
                    if (key == AccumulatorN::kFieldNameOutput) {
                        auto outputExpr =
                            generateExpression(state, value.get(), rootSlot, &outputs);
                        accArgs.emplace(AccArgs::kTopBottomNValue,
                                        makeFillEmptyNull(outputExpr.extractExpr(state).expr));
                        break;
                    }
                }
            } else if (auto expConst =
                           dynamic_cast<ExpressionConstant*>(accStmt.expr.argument.get())) {
                auto objConst = expConst->getValue();
                tassert(7767100,
                        str::stream()
                            << accStmt.expr.name << " accumulator must have an object argument",
                        objConst.isObject());
                auto objBson = objConst.getDocument().toBson();
                auto outputField = objBson.getField(AccumulatorN::kFieldNameOutput);
                if (outputField.ok()) {
                    auto [outputTag, outputVal] =
                        sbe::bson::convertFrom<false /* View */>(outputField);
                    auto outputExpr = makeConstant(outputTag, outputVal);
                    accArgs.emplace(AccArgs::kTopBottomNValue,
                                    makeFillEmptyNull(std::move(outputExpr)));
                }
            } else {
                tasserted(5807015,
                          str::stream()
                              << accStmt.expr.name << " accumulator must have an object argument");
            }
            tassert(5807016,
                    str::stream() << accStmt.expr.name
                                  << " accumulator must have an output field in the argument",
                    accArgs.find(AccArgs::kTopBottomNValue) != accArgs.end());

            auto accExprs = stage_builder::buildAccumulator(
                accStmt, std::move(accArgs), collatorSlot, *state.frameIdGenerator);

            return accExprs;
        } else {
            auto argExpr =
                generateExpression(state, accStmt.expr.argument.get(), rootSlot, &outputs);
            auto accExprs = stage_builder::buildAccumulator(
                accStmt, argExpr.extractExpr(state).expr, collatorSlot, *state.frameIdGenerator);
            return accExprs;
        }
    }();

    auto initExprs = [&]() {
        StringDataMap<std::unique_ptr<sbe::EExpression>> initExprArgs;
        if (isAccumulatorN(accStmt)) {
            initExprArgs.emplace(
                AccArgs::kMaxSize,
                generateExpression(
                    state, accStmt.expr.initializer.get(), initializerRootSlot, nullptr)
                    .extractExpr(state)
                    .expr);
            initExprArgs.emplace(
                AccArgs::kIsGroupAccum,
                makeConstant(sbe::value::TypeTags::Boolean, sbe::value::bitcastFrom<bool>(true)));
        } else {
            initExprArgs.emplace(
                "",
                generateExpression(
                    state, accStmt.expr.initializer.get(), initializerRootSlot, nullptr)
                    .extractExpr(state)
                    .expr);
        }
        return initExprArgs;
    }();
    auto accInitExprs = [&]() {
        if (initExprs.size() == 1) {
            return stage_builder::buildInitialize(
                accStmt, std::move(initExprs.begin()->second), *state.frameIdGenerator);
        } else {
            return stage_builder::buildInitialize(
                accStmt, std::move(initExprs), *state.frameIdGenerator);
        }
    }();

    tassert(7567301,
            "The accumulation and initialization expression should have the same length",
            accExprs.size() == accInitExprs.size());
    sbe::value::SlotVector aggSlots;
    for (size_t i = 0; i < accExprs.size(); i++) {
        auto slot = slotIdGenerator->generate();
        aggSlots.push_back(slot);
        aggSlotExprs.push_back(std::make_pair(
            slot, sbe::AggExprPair{std::move(accInitExprs[i]), std::move(accExprs[i])}));
    }

    return aggSlots;
}

/**
 * Generate a vector of (inputSlot, mergingExpression) pairs. The slot (whose id is allocated by
 * this function) will be used to store spilled partial aggregate values that have been recovered
 * from disk and deserialized. The merging expression is an agg function which combines these
 * partial aggregates.
 *
 * Usually the returned vector will be of length 1, but in some cases the MQL accumulation statement
 * is implemented by calculating multiple separate aggregates in the SBE plan, which are finalized
 * by a subsequent project stage to produce the ultimate value.
 */
sbe::SlotExprPairVector generateMergingExpressions(StageBuilderState& state,
                                                   const AccumulationStatement& accStmt,
                                                   int numInputSlots) {
    tassert(7039555, "'numInputSlots' must be positive", numInputSlots > 0);
    auto slotIdGenerator = state.slotIdGenerator;
    tassert(7039556, "expected non-null 'slotIdGenerator' pointer", slotIdGenerator);
    auto frameIdGenerator = state.frameIdGenerator;
    tassert(7039557, "expected non-null 'frameIdGenerator' pointer", frameIdGenerator);

    auto spillSlots = slotIdGenerator->generateMultiple(numInputSlots);
    auto collatorSlot = state.getCollatorSlot();

    auto mergingExprs = [&]() {
        if (isTopBottomN(accStmt)) {
            StringDataMap<std::unique_ptr<sbe::EExpression>> mergeArgs;
            mergeArgs.emplace(AccArgs::kTopBottomNSortSpec, getSortSpecFromTopBottomN(accStmt));
            return buildCombinePartialAggregates(
                accStmt, spillSlots, std::move(mergeArgs), collatorSlot, *frameIdGenerator);
        } else {
            return buildCombinePartialAggregates(
                accStmt, spillSlots, collatorSlot, *frameIdGenerator);
        }
    }();

    // Zip the slot vector and expression vector into a vector of pairs.
    tassert(7039550,
            "expected same number of slots and input exprs",
            spillSlots.size() == mergingExprs.size());
    sbe::SlotExprPairVector result;
    result.reserve(spillSlots.size());
    for (size_t i = 0; i < spillSlots.size(); ++i) {
        result.push_back({spillSlots[i], std::move(mergingExprs[i])});
    }
    return result;
}

// Given a sequence 'groupBySlots' of slot ids, return a new sequence that contains all slots ids in
// 'groupBySlots' but without any duplicate ids.
sbe::value::SlotVector dedupGroupBySlots(const sbe::value::SlotVector& groupBySlots) {
    stdx::unordered_set<sbe::value::SlotId> uniqueSlots;
    sbe::value::SlotVector dedupedGroupBySlots;

    for (auto slot : groupBySlots) {
        if (!uniqueSlots.contains(slot)) {
            dedupedGroupBySlots.emplace_back(slot);
            uniqueSlots.insert(slot);
        }
    }

    return dedupedGroupBySlots;
}

std::tuple<std::vector<std::string>, sbe::value::SlotVector, SbStage> generateGroupFinalStage(
    StageBuilderState& state,
    SbStage groupStage,
    sbe::value::SlotVector groupOutSlots,
    std::unique_ptr<sbe::EExpression> idFinalExpr,
    sbe::value::SlotVector dedupedGroupBySlots,
    const std::vector<AccumulationStatement>& accStmts,
    const std::vector<sbe::value::SlotVector>& aggSlotsVec,
    PlanNodeId nodeId) {
    sbe::SlotExprPairVector projects;
    // To passthrough the output slots of accumulators with trivial finalizers, we need to find
    // their slot ids. We can do this by sorting 'groupStage.outSlots' because the slot ids
    // correspond to the order in which the accumulators were translated (that is, the order in
    // which they are listed in 'accStmts'). Note, that 'groupStage.outSlots' contains deduped
    // group-by slots at the front and the accumulator slots at the back.
    std::sort(groupOutSlots.begin() + dedupedGroupBySlots.size(), groupOutSlots.end());

    tassert(5995100,
            "The _id expression must either produce an expression or a scalar value",
            idFinalExpr || dedupedGroupBySlots.size() == 1);

    auto finalGroupBySlot = [&]() {
        if (!idFinalExpr) {
            return dedupedGroupBySlots[0];
        } else {
            auto slot = state.slotId();
            projects.emplace_back(slot, std::move(idFinalExpr));
            return slot;
        }
    }();

    auto collatorSlot = state.getCollatorSlot();
    auto finalSlots{sbe::value::SlotVector{finalGroupBySlot}};
    std::vector<std::string> fieldNames{"_id"};
    size_t idxAccFirstSlot = dedupedGroupBySlots.size();
    for (size_t idxAcc = 0; idxAcc < accStmts.size(); ++idxAcc) {
        // Gathers field names for the output object from accumulator statements.
        fieldNames.push_back(accStmts[idxAcc].fieldName);

        auto finalExpr = [&]() {
            const auto& accStmt = accStmts[idxAcc];
            if (isTopBottomN(accStmt)) {
                StringDataMap<std::unique_ptr<sbe::EExpression>> finalArgs;
                finalArgs.emplace(AccArgs::kTopBottomNSortSpec, getSortSpecFromTopBottomN(accStmt));
                return buildFinalize(state,
                                     accStmts[idxAcc],
                                     aggSlotsVec[idxAcc],
                                     std::move(finalArgs),
                                     collatorSlot,
                                     *state.frameIdGenerator);
            } else {
                return buildFinalize(state,
                                     accStmts[idxAcc],
                                     aggSlotsVec[idxAcc],
                                     collatorSlot,
                                     *state.frameIdGenerator);
            }
        }();

        // The final step may not return an expression if it's trivial. For example, $first and
        // $last's final steps are trivial.
        if (finalExpr) {
            auto outSlot = state.slotId();
            finalSlots.push_back(outSlot);
            projects.emplace_back(outSlot, std::move(finalExpr));
        } else {
            finalSlots.push_back(groupOutSlots[idxAccFirstSlot]);
        }

        // Some accumulator(s) like $avg generate multiple expressions and slots. So, need to
        // advance this index by the number of those slots for each accumulator.
        idxAccFirstSlot += aggSlotsVec[idxAcc].size();
    }

    // Gathers all accumulator results. If there're no project expressions, does not add a project
    // stage.
    auto retStage = projects.empty()
        ? std::move(groupStage)
        : makeProject(std::move(groupStage), std::move(projects), nodeId);

    return {std::move(fieldNames), std::move(finalSlots), std::move(retStage)};
}

// Generate the accumulator expressions and HashAgg operator used to compute a $group pipeline
// stage.
MONGO_COMPILER_NOINLINE
std::tuple<std::vector<std::string>, sbe::value::SlotVector, SbStage> buildGroupAggregation(
    StageBuilderState& state,
    const GroupNode& groupNode,
    bool allowDiskUse,
    std::unique_ptr<sbe::EExpression> idFinalExpr,
    const PlanStageSlots& childOutputs,
    SbStage groupByStage,
    sbe::value::SlotVector& groupBySlots) {
    auto nodeId = groupNode.nodeId();

    auto initializerRootSlot = [&]() {
        bool isVariableGroupInitializer = false;
        for (const auto& accStmt : groupNode.accumulators) {
            isVariableGroupInitializer = isVariableGroupInitializer ||
                !ExpressionConstant::isNullOrConstant(accStmt.expr.initializer);
        }
        if (!isVariableGroupInitializer) {
            return boost::optional<TypedSlot>{};
        }

        sbe::value::SlotId idSlot;
        // We materialize the groupId before the group stage to provide it as root to
        // initializer expression
        if (idFinalExpr) {
            auto slot = state.slotId();
            groupByStage =
                makeProject(std::move(groupByStage), nodeId, slot, std::move(idFinalExpr));

            groupBySlots.clear();
            groupBySlots.push_back(slot);
            idFinalExpr = nullptr;
            idSlot = slot;
        } else {
            idSlot = groupBySlots[0];
        }

        // As per the mql semantics add a project expression 'isObject(id) ? id : {}'
        // which will be provided as root to initializer expression
        auto [emptyObjTag, emptyObjVal] = sbe::value::makeNewObject();
        auto isObjectExpr = sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EFunction>("isObject"_sd, sbe::makeEs(makeVariable(idSlot))),
            makeVariable(idSlot),
            makeConstant(emptyObjTag, emptyObjVal));

        auto isObjSlot = state.slotId();
        groupByStage =
            makeProject(std::move(groupByStage), nodeId, isObjSlot, std::move(isObjectExpr));

        return boost::optional<TypedSlot>(TypedSlot{isObjSlot, TypeSignature::kObjectType});
    }();

    // Translates accumulators which are executed inside the group stage and gets slots for
    // accumulators.
    auto currentStage = std::move(groupByStage);
    sbe::AggExprVector aggSlotExprs;
    std::vector<sbe::value::SlotVector> aggSlotsVec;
    // Since partial accumulator state may be spilled to disk and then merged, we must construct not
    // only the basic agg expressions for each accumulator, but also agg expressions that are used
    // to combine partial aggregates that have been spilled to disk.
    sbe::SlotExprPairVector mergingExprs;
    for (const auto& accStmt : groupNode.accumulators) {
        sbe::value::SlotVector curAggSlots = generateAccumulator(
            state, accStmt, childOutputs, state.slotIdGenerator, aggSlotExprs, initializerRootSlot);

        sbe::SlotExprPairVector curMergingExprs =
            generateMergingExpressions(state, accStmt, curAggSlots.size());

        aggSlotsVec.emplace_back(std::move(curAggSlots));
        mergingExprs.insert(mergingExprs.end(),
                            std::make_move_iterator(curMergingExprs.begin()),
                            std::make_move_iterator(curMergingExprs.end()));
    }

    // There might be duplicated expressions and slots. Dedup them before creating a HashAgg
    // because it would complain about duplicated slots and refuse to be created, which is
    // reasonable because duplicated expressions would not contribute to grouping.
    auto dedupedGroupBySlots = dedupGroupBySlots(groupBySlots);

    auto groupOutSlots = dedupedGroupBySlots;
    for (auto& [slot, _] : aggSlotExprs) {
        groupOutSlots.push_back(slot);
    }

    // Builds a group stage with accumulator expressions and group-by slot(s).
    currentStage = makeHashAgg(std::move(currentStage),
                               dedupedGroupBySlots,
                               std::move(aggSlotExprs),
                               state.getCollatorSlot(),
                               allowDiskUse,
                               std::move(mergingExprs),
                               nodeId);

    tassert(
        5851603,
        "Group stage's output slots must include deduped slots for group-by keys and slots for all "
        "accumulators",
        groupOutSlots.size() ==
            std::accumulate(aggSlotsVec.begin(),
                            aggSlotsVec.end(),
                            dedupedGroupBySlots.size(),
                            [](int sum, const auto& aggSlots) { return sum + aggSlots.size(); }));
    tassert(
        5851604,
        "Group stage's output slots must contain the deduped groupBySlots at the front",
        std::equal(dedupedGroupBySlots.begin(), dedupedGroupBySlots.end(), groupOutSlots.begin()));


    // Builds the final stage(s) over the collected accumulators.
    return generateGroupFinalStage(state,
                                   std::move(currentStage),
                                   std::move(groupOutSlots),
                                   std::move(idFinalExpr),
                                   dedupedGroupBySlots,
                                   groupNode.accumulators,
                                   aggSlotsVec,
                                   nodeId);
}
}  // namespace

/**
 * Translates a 'GroupNode' QSN into a sbe::PlanStage tree. This translation logic assumes that the
 * only child of the 'GroupNode' must return an Object (or 'BSONObject') and the translated sub-tree
 * must return 'BSONObject'. The returned 'BSONObject' will always have an "_id" field for the group
 * key and zero or more field(s) for accumulators.
 *
 * For example, a QSN tree: GroupNode(nodeId=2) over a CollectionScanNode(nodeId=1), we would have
 * the following translated sbe::PlanStage tree. In this example, we assume that the $group pipeline
 * spec is {"_id": "$a", "x": {"$min": "$b"}, "y": {"$first": "$b"}}.
 *
 * [2] mkbson s12 [_id = s8, x = s11, y = s10] true false
 * [2] project [s11 = (s9 ?: null)]
 * [2] group [s8] [s9 = min(
 *   let [
 *      l1.0 = s5
 *  ]
 *  in
 *      if (typeMatch(l1.0, 1088ll) ?: true)
 *      then Nothing
 *      else l1.0
 * ), s10 = first((s5 ?: null))]
 * [2] project [s8 = (s4 ?: null)]
 * [1] scan s6 s7 none none none none [s4 = a, s5 = b] @<collUuid> true false
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildGroup(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023414, "buildGroup() does not support kSortKey", !reqs.hasSortKeys());

    auto groupNode = static_cast<const GroupNode*>(root);
    auto nodeId = groupNode->nodeId();
    const auto& idExpr = groupNode->groupByExpression;

    tassert(
        5851600, "should have one and only one child for GROUP", groupNode->children.size() == 1);
    tassert(5851601, "GROUP should have had group-by key expression", idExpr);
    tassert(
        6360401,
        "GROUP cannot propagate a record id slot, but the record id was requested by the parent",
        !reqs.has(kRecordId));

    const auto& childNode = groupNode->children[0].get();
    const auto& accStmts = groupNode->accumulators;

    // Builds the child and gets the child result slot.
    auto childReqs = computeChildReqsForGroup(reqs, *groupNode);
    auto [childStage, childOutputs] = build(childNode, childReqs);

    // Set of field paths referenced by group. Useful for de-duplicating fields and clearing the
    // slots corresponding to fields in 'childOutputs' so that they are not mistakenly referenced by
    // parent stages.
    StringSet groupFieldSet;
    childStage = projectPathTraversalsForGroupBy(
        _state, *groupNode, childReqs, std::move(childStage), childOutputs, groupFieldSet);

    sbe::value::SlotVector groupBySlots;
    SbStage groupByStage;
    std::unique_ptr<sbe::EExpression> idFinalExpr;

    std::tie(groupBySlots, groupByStage, idFinalExpr) = generateGroupByKey(
        _state, idExpr, childOutputs, std::move(childStage), nodeId, &_slotIdGenerator);

    auto [fieldNames, finalSlots, outStage] = buildGroupAggregation(_state,
                                                                    *groupNode,
                                                                    _cq.getExpCtx()->allowDiskUse,
                                                                    std::move(idFinalExpr),
                                                                    childOutputs,
                                                                    std::move(groupByStage),
                                                                    groupBySlots);

    tassert(5851605,
            "The number of final slots must be as 1 (the final group-by slot) + the number of acc "
            "slots",
            finalSlots.size() == 1 + accStmts.size());

    // Clear all fields needed by this group stage from 'childOutputs' to avoid references to
    // ExpressionFieldPath values that are no longer visible.
    for (const auto& groupField : groupFieldSet) {
        childOutputs.clear({PlanStageSlots::kPathExpr, StringData(groupField)});
    }

    auto fieldNamesSet = StringDataSet{fieldNames.begin(), fieldNames.end()};
    auto [fields, additionalFields] =
        splitVector(reqs.getFields(), [&](const std::string& s) { return fieldNamesSet.count(s); });
    auto fieldsSet = StringDataSet{fields.begin(), fields.end()};

    PlanStageSlots outputs;
    for (size_t i = 0; i < fieldNames.size(); ++i) {
        if (fieldsSet.count(fieldNames[i])) {
            outputs.set(std::make_pair(PlanStageSlots::kField, fieldNames[i]), finalSlots[i]);
        }
    };

    // Builds a stage to create a result object out of a group-by slot and gathered accumulator
    // result slots if the parent node requests so.
    if (reqs.has(kResult) || !additionalFields.empty()) {
        auto resultSlot = _slotIdGenerator.generate();
        outputs.set(kResult, TypedSlot{resultSlot, TypeSignature::kObjectType});
        // This mkbson stage combines 'finalSlots' into a bsonObject result slot which has
        // 'fieldNames' fields.
        if (groupNode->shouldProduceBson) {
            outStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(outStage),
                                                         resultSlot,   // objSlot
                                                         boost::none,  // rootSlot
                                                         boost::none,  // fieldBehavior
                                                         std::vector<std::string>{},  // fields
                                                         std::move(fieldNames),  // projectFields
                                                         std::move(finalSlots),  // projectVars
                                                         true,                   // forceNewObject
                                                         false,                  // returnOldObject
                                                         nodeId);
        } else {
            outStage = sbe::makeS<sbe::MakeObjStage>(std::move(outStage),
                                                     resultSlot,                  // objSlot
                                                     boost::none,                 // rootSlot
                                                     boost::none,                 // fieldBehavior
                                                     std::vector<std::string>{},  // fields
                                                     std::move(fieldNames),       // projectFields
                                                     std::move(finalSlots),       // projectVars
                                                     true,                        // forceNewObject
                                                     false,                       // returnOldObject
                                                     nodeId);
        }
    }

    return {std::move(outStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::makeUnionForTailableCollScan(const QuerySolutionNode* root,
                                                    const PlanStageReqs& reqs) {
    using namespace std::literals;
    tassert(
        6023415, "makeUnionForTailableCollScan() does not support kSortKey", !reqs.hasSortKeys());

    // Register a SlotId in the global environment which would contain a recordId to resume a
    // tailable collection scan from. A PlanStage executor will track the last seen recordId and
    // will reset a SlotAccessor for the resumeRecordIdSlot with this recordId.
    auto resumeRecordIdSlot = _env->registerSlot(
        "resumeRecordId"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    // For tailable collection scan we need to build a special union sub-tree consisting of two
    // branches:
    //   1) An anchor branch implementing an initial collection scan before the first EOF is hit.
    //   2) A resume branch implementing all consecutive collection scans from a recordId which was
    //      seen last.
    //
    // The 'makeStage' parameter is used to build a PlanStage tree which is served as a root stage
    // for each of the union branches. The same mechanism is used to build each union branch, and
    // the special logic which needs to be triggered depending on which branch we build is
    // controlled by setting the isTailableCollScanResumeBranch flag in PlanStageReqs.
    auto makeUnionBranch = [&](bool isTailableCollScanResumeBranch)
        -> std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> {
        auto childReqs = reqs;
        childReqs.setIsTailableCollScanResumeBranch(isTailableCollScanResumeBranch);
        auto [branch, outputs] = build(root, childReqs);

        auto branchSlots = getSlotsToForward(childReqs, outputs);

        return {std::move(branchSlots), std::move(branch)};
    };

    // Build an anchor branch of the union and add a constant filter on top of it, so that it would
    // only execute on an initial collection scan, that is, when resumeRecordId is not available
    // yet.
    auto&& [anchorBranchSlots, anchorBranch] = makeUnionBranch(false);
    anchorBranch = sbe::makeS<sbe::FilterStage<true>>(
        std::move(anchorBranch),
        makeNot(makeFunction("exists"_sd, sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    // Build a resume branch of the union and add a constant filter on op of it, so that it would
    // only execute when we resume a collection scan from the resumeRecordId.
    auto&& [resumeBranchSlots, resumeBranch] = makeUnionBranch(true);
    resumeBranch = sbe::makeS<sbe::FilterStage<true>>(
        sbe::makeS<sbe::LimitSkipStage>(std::move(resumeBranch), boost::none, 1, root->nodeId()),
        sbe::makeE<sbe::EFunction>("exists"_sd,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    invariant(anchorBranchSlots.size() == resumeBranchSlots.size());

    // A vector of the output slots for each union branch.
    auto branchSlots = makeVector<sbe::value::SlotVector>(std::move(anchorBranchSlots),
                                                          std::move(resumeBranchSlots));

    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    auto unionOutputSlots = getSlotsToForward(reqs, outputs);

    // Branch output slots become the input slots to the union.
    auto unionStage =
        sbe::makeS<sbe::UnionStage>(sbe::makeSs(std::move(anchorBranch), std::move(resumeBranch)),
                                    branchSlots,
                                    unionOutputSlots,
                                    root->nodeId());

    return {std::move(unionStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildShardFilterCovered(const QuerySolutionNode* root,
                                               const PlanStageReqs& reqs) {
    // Constructs an optimized SBE plan for 'filterNode' in the case that the fields of the
    // 'shardKeyPattern' are provided by 'child'. In this case, the SBE tree for 'child' will
    // fill out slots for the necessary components of the index key. These slots can be read
    // directly in order to determine the shard key that should be passed to the
    // 'shardFiltererSlot'.
    const auto filterNode = static_cast<const ShardingFilterNode*>(root);
    auto child = filterNode->children[0].get();
    tassert(6023416,
            "buildShardFilterCovered() expects ixscan below shard filter",
            child->getType() == STAGE_IXSCAN || child->getType() == STAGE_VIRTUAL_SCAN);

    // Extract the child's key pattern.
    BSONObj indexKeyPattern = child->getType() == STAGE_IXSCAN
        ? static_cast<const IndexScanNode*>(child)->index.keyPattern
        : static_cast<const VirtualScanNode*>(child)->indexKeyPattern;

    auto childReqs = reqs.copy();

    // If we're sharded make sure that we don't return data that isn't owned by the shard. This
    // situation can occur when pending documents from in-progress migrations are inserted and when
    // there are orphaned documents from aborted migrations. To check if the document is owned by
    // the shard, we need to own a 'ShardFilterer', and extract the document's shard key as a
    // BSONObj.
    auto shardKeyPattern = _collections.getMainCollection().getShardKeyPattern().toBSON();
    // We register the "shardFilterer" slot but we don't construct the ShardFilterer here, because
    // once constructed the ShardFilterer will prevent orphaned documents from being deleted. We
    // will construct the ShardFilterer later while preparing the SBE tree for execution.
    auto shardFiltererSlot = _env->registerSlot(
        "shardFilterer"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    for (auto&& shardKeyElt : shardKeyPattern) {
        childReqs.set(std::make_pair(PlanStageSlots::kField, shardKeyElt.fieldNameStringData()));
    }

    auto [stage, outputs] = build(child, childReqs);

    // Maps from key name to a bool that indicates whether the key is hashed.
    StringDataMap<bool> indexKeyPatternMap;
    for (auto&& ixPatternElt : indexKeyPattern) {
        indexKeyPatternMap.emplace(ixPatternElt.fieldNameStringData(),
                                   ShardKeyPattern::isHashedPatternEl(ixPatternElt));
    }

    // Build expressions to create shard key fields and deal with hashed shard keys.
    std::vector<std::string> projectFields;
    sbe::EExpression::Vector projectValues;
    for (auto&& shardKeyPatternElt : shardKeyPattern) {
        auto it = indexKeyPatternMap.find(shardKeyPatternElt.fieldNameStringData());
        tassert(5562303, "Could not find element", it != indexKeyPatternMap.end());
        const auto ixKeyEltHashed = it->second;
        const auto slotId = outputs
                                .get(std::make_pair(PlanStageSlots::kField,
                                                    shardKeyPatternElt.fieldNameStringData()))
                                .slotId;

        // Get the value stored in the index for this component of the shard key. We may have to
        // hash it.
        auto elem = makeVariable(slotId);

        // Handle the case where the index key or shard key is hashed.
        const bool shardKeyEltHashed = ShardKeyPattern::isHashedPatternEl(shardKeyPatternElt);
        if (ixKeyEltHashed) {
            // If the index stores hashed data, then we know the shard key field is hashed as
            // well. Nothing to do here. We can apply shard filtering with no other changes.
            tassert(6023421,
                    "Index key is hashed, expected corresponding shard key to be hashed",
                    shardKeyEltHashed);
        } else if (shardKeyEltHashed) {
            // The shard key field is hashed but the index stores unhashed data. We must apply
            // the hash function before passing this off to the shard filter.
            elem = makeFunction("shardHash"_sd, std::move(elem));
        }

        projectFields.push_back(shardKeyPatternElt.fieldName());
        projectValues.push_back(std::move(elem));
    }

    auto shardKeyExpression = makeNewBsonObject(std::move(projectFields), std::move(projectValues));
    auto shardFilterExpression = makeFunction("shardFilter",
                                              sbe::makeE<sbe::EVariable>(shardFiltererSlot),
                                              std::move(shardKeyExpression));

    outputs.clearNonRequiredSlots(reqs);

    return {sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), std::move(shardFilterExpression), root->nodeId()),
            std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildShardFilter(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto child = root->children[0].get();
    bool childIsIndexScan = child->getType() == STAGE_IXSCAN ||
        (child->getType() == STAGE_VIRTUAL_SCAN &&
         !static_cast<const VirtualScanNode*>(child)->indexKeyPattern.isEmpty());

    // If we're not required to fill out the 'kResult' slot, then instead we can request a slot from
    // the child for each of the fields which constitute the shard key. This allows us to avoid
    // materializing an intermediate object for plans where shard filtering can be performed based
    // on the contents of index keys.
    //
    // We only apply this optimization in the special case that the child QSN is an IXSCAN, since in
    // this case we can request exactly the fields we need according to their position in the index
    // key pattern.
    if (!reqs.has(kResult) && childIsIndexScan) {
        return buildShardFilterCovered(root, reqs);
    }

    auto childReqs = reqs.copy();

    // If we're sharded make sure that we don't return data that isn't owned by the shard. This
    // situation can occur when pending documents from in-progress migrations are inserted and when
    // there are orphaned documents from aborted migrations. To check if the document is owned by
    // the shard, we need to own a 'ShardFilterer', and extract the document's shard key as a
    // BSONObj.
    auto shardKeyPattern = _collections.getMainCollection().getShardKeyPattern().toBSON();
    // We register the "shardFilterer" slot but we don't construct the ShardFilterer here, because
    // once constructed the ShardFilterer will prevent orphaned documents from being deleted. We
    // will construct the ShardFilterer later while preparing the SBE tree for execution.
    auto shardFiltererSlot = _env->registerSlot(
        "shardFilterer"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    // Request slots for top level shard key fields and cache parsed key path.
    std::vector<sbe::MatchPath> shardKeyPaths;
    std::vector<bool> shardKeyHashed;
    for (auto&& shardKeyElt : shardKeyPattern) {
        shardKeyPaths.emplace_back(shardKeyElt.fieldNameStringData());
        shardKeyHashed.push_back(ShardKeyPattern::isHashedPatternEl(shardKeyElt));
        childReqs.set(std::make_pair(PlanStageSlots::kField, shardKeyPaths.back().getPart(0)));
    }

    auto [stage, outputs] = build(child, childReqs);
    auto shardFilterExpression = makeFunction(
        "shardFilter",
        sbe::makeE<sbe::EVariable>(shardFiltererSlot),
        makeShardKeyFunctionForPersistedDocuments(shardKeyPaths, shardKeyHashed, outputs));

    outputs.clearNonRequiredSlots(reqs);
    return {sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), std::move(shardFilterExpression), root->nodeId()),
            std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildWindow(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto windowNode = static_cast<const WindowNode*>(root);

    auto child = root->children[0].get();
    auto childReqs = reqs.copy();
    childReqs.setFields(getTopLevelFields(windowNode->partitionByRequiredFields));
    childReqs.setFields(getTopLevelFields(windowNode->sortByRequiredFields));
    auto outputRequiredFields = getTopLevelFields(windowNode->outputRequiredFields);
    childReqs.setFields(outputRequiredFields);

    auto childStageOutput = build(child, childReqs);
    auto stage = std::move(childStageOutput.first);
    auto outputs = std::move(childStageOutput.second);
    auto rootSlotOpt = outputs.getIfExists(kResult);

    // Create a tuple of slots for each new slot added.
    sbe::value::SlotVector currSlots;
    sbe::value::SlotVector boundTestingSlots;
    std::vector<sbe::value::SlotVector> windowFrameFirstSlots;
    std::vector<sbe::value::SlotVector> windowFrameLastSlots;
    auto ensureSlotInBuffer = [&](sbe::value::SlotId slot) {
        for (size_t i = 0; i < currSlots.size(); i++) {
            if (slot == currSlots[i]) {
                return i;
            }
        }
        currSlots.push_back(slot);
        boundTestingSlots.push_back(_slotIdGenerator.generate());
        for (auto& frameFirstSlots : windowFrameFirstSlots) {
            frameFirstSlots.push_back(_slotIdGenerator.generate());
        }
        for (auto& frameLastSlots : windowFrameLastSlots) {
            frameLastSlots.push_back(_slotIdGenerator.generate());
        }
        return currSlots.size() - 1;
    };
    auto registerFrameFirstSlots = [&]() {
        windowFrameFirstSlots.push_back(sbe::value::SlotVector());
        auto& frameFirstSlots = windowFrameFirstSlots.back();
        frameFirstSlots.clear();
        for (size_t i = 0; i < currSlots.size(); i++) {
            frameFirstSlots.push_back(_slotIdGenerator.generate());
        }
        return windowFrameFirstSlots.size() - 1;
    };
    auto registerFrameLastSlots = [&]() {
        windowFrameLastSlots.push_back(sbe::value::SlotVector());
        auto& frameLastSlots = windowFrameLastSlots.back();
        frameLastSlots.clear();
        for (size_t i = 0; i < currSlots.size(); i++) {
            frameLastSlots.push_back(_slotIdGenerator.generate());
        }
        return windowFrameLastSlots.size() - 1;
    };

    auto collatorSlot = _state.getCollatorSlot();

    // Get stages for partition by.
    size_t partitionSlotCount = 0;
    if (windowNode->partitionBy) {
        auto partitionSlot = _slotIdGenerator.generate();
        ensureSlotInBuffer(partitionSlot);
        partitionSlotCount++;
        auto partitionABT = abt::unwrap(
            generateExpression(_state, windowNode->partitionBy->get(), rootSlotOpt, &outputs)
                .extractABT());
        auto partitionName = getABTLocalVariableName(_state.frameId(), 0);
        // Assert partition slot is not an array.
        partitionABT = optimizer::make<optimizer::Let>(
            partitionName,
            makeFillEmptyNull(std::move(partitionABT)),
            optimizer::make<optimizer::If>(
                makeABTFunction("isArray"_sd, makeVariable(partitionName)),
                makeABTFail(
                    ErrorCodes::TypeMismatch,
                    "An expression used to partition cannot evaluate to value of type array"),
                makeVariable(partitionName)));
        auto partitionExpr = abtToExpr(partitionABT, _state).expr;
        stage = sbe::makeProjectStage(
            std::move(stage), root->nodeId(), partitionSlot, std::move(partitionExpr));
    }

    // Calculate list of forward slots.
    for (auto forwardSlot : getSlotsToForward(reqs, outputs)) {
        ensureSlotInBuffer(forwardSlot);
    }

    // Calculate slot for document position based window bounds, and add corresponding stages.
    boost::optional<sbe::value::SlotId> documentBoundSlot;
    auto getDocumentBoundSlot = [&]() -> std::pair<sbe::value::SlotId, sbe::value::SlotId> {
        if (!documentBoundSlot) {
            documentBoundSlot = _slotIdGenerator.generate();
            sbe::value::SlotMap<sbe::AggExprPair> aggExprPairs;
            aggExprPairs.emplace(
                *documentBoundSlot,
                sbe::AggExprPair{nullptr, makeFunction("sum", makeInt32Constant(1))});
            stage = sbe::makeS<sbe::AggProjectStage>(
                std::move(stage), std::move(aggExprPairs), windowNode->nodeId());
        }
        auto documentBoundSlotIdx = ensureSlotInBuffer(*documentBoundSlot);
        return {*documentBoundSlot, boundTestingSlots[documentBoundSlotIdx]};
    };

    // Calculate sort-by slot, and add corresponding stages.
    boost::optional<sbe::value::SlotId> sortBySlot;
    auto getSortBySlot = [&]() -> std::pair<sbe::value::SlotId, sbe::value::SlotId> {
        if (!sortBySlot) {
            sortBySlot = _slotIdGenerator.generate();
            tassert(7914602,
                    "Expected to have a single sort component",
                    windowNode->sortBy && windowNode->sortBy->size() == 1);
            const auto& part = windowNode->sortBy->front();
            auto expCtx = _cq.getExpCtxRaw();
            auto fieldPathExpr = ExpressionFieldPath::createPathFromString(
                expCtx, part.fieldPath->fullPath(), expCtx->variablesParseState);
            auto sortByExpr = generateExpression(_state, fieldPathExpr.get(), rootSlotOpt, &outputs)
                                  .extractExpr(_state)
                                  .expr;
            stage = makeProjectStage(
                std::move(stage), windowNode->nodeId(), *sortBySlot, std::move(sortByExpr));
        }
        auto sortBySlotIdx = ensureSlotInBuffer(*sortBySlot);
        return {*sortBySlot, boundTestingSlots[sortBySlotIdx]};
    };

    // Calculate slot for range and time range based window bounds
    boost::optional<sbe::value::SlotId> rangeBoundSlot;
    boost::optional<sbe::value::SlotId> timeRangeBoundSlot;
    auto getRangeBoundSlot =
        [&](boost::optional<TimeUnit> unit) -> std::pair<sbe::value::SlotId, sbe::value::SlotId> {
        auto projectRangeBoundSlot = [&](StringData typeCheckFn,
                                         std::unique_ptr<sbe::EExpression> failExpr) {
            auto slot = _slotIdGenerator.generate();
            auto sortBySlot = getSortBySlot().first;

            auto checkType = makeLocalBind(
                &_frameIdGenerator,
                [&](sbe::EVariable input) {
                    return sbe::makeE<sbe::EIf>(makeFunction(typeCheckFn, input.clone()),
                                                input.clone(),
                                                std::move(failExpr));
                },
                makeFillEmptyNull(makeVariable(sortBySlot)));

            stage = makeProjectStage(
                std::move(stage), windowNode->nodeId(), slot, std::move(checkType));
            return slot;
        };
        if (unit) {
            if (!timeRangeBoundSlot) {
                timeRangeBoundSlot = projectRangeBoundSlot(
                    "isDate",
                    sbe::makeE<sbe::EFail>(
                        ErrorCodes::Error{7956500},
                        "Invalid range: Expected the sortBy field to be a date"));
            }
            auto timeRangeBoundSlotIdx = ensureSlotInBuffer(*timeRangeBoundSlot);
            return {*timeRangeBoundSlot, boundTestingSlots[timeRangeBoundSlotIdx]};
        } else {
            if (!rangeBoundSlot) {
                rangeBoundSlot = projectRangeBoundSlot(
                    "isNumber",
                    sbe::makeE<sbe::EFail>(
                        ErrorCodes::Error{7993103},
                        "Invalid range: Expected the sortBy field to be a number"));
            }
            auto rangeBoundSlotIdx = ensureSlotInBuffer(*rangeBoundSlot);
            return {*rangeBoundSlot, boundTestingSlots[rangeBoundSlotIdx]};
        }
    };

    // Create window function input arguments and project them in order to avoid repeated evaluation
    // for both add and remove expressions.
    std::vector<StringDataMap<std::unique_ptr<sbe::EExpression>>> windowArgExprs;
    sbe::SlotExprPairVector windowArgProjects;
    for (auto& outputField : windowNode->outputFields) {
        auto accName = outputField.expr->getOpName();
        StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs;
        auto getArgExpr = [&](Expression* arg) {
            auto argExpr =
                generateExpression(_state, arg, rootSlotOpt, &outputs).extractExpr(_state).expr;
            if (auto varExpr = argExpr->as<sbe::EVariable>(); varExpr) {
                ensureSlotInBuffer(varExpr->getSlotId());
                return argExpr;
            } else if (argExpr->as<sbe::EConstant>()) {
                return argExpr;
            } else {
                auto argSlot = _slotIdGenerator.generate();
                windowArgProjects.emplace_back(argSlot, std::move(argExpr));
                ensureSlotInBuffer(argSlot);
                return makeVariable(argSlot);
            }
        };
        if (accName == "$covarianceSamp" || accName == "$covariancePop") {
            auto expr = dynamic_cast<ExpressionArray*>(outputField.expr->input().get());
            tassert(7820818,
                    "Covariance argument should be an array of two elements",
                    expr && expr->getChildren().size() == 2);
            auto argX = expr->getChildren()[0].get();
            auto argY = expr->getChildren()[1].get();
            argExprs.emplace(AccArgs::kCovarianceX, getArgExpr(argX));
            argExprs.emplace(AccArgs::kCovarianceY, getArgExpr(argY));
        } else if (accName == "$integral" || accName == "$derivative" || accName == "$linearFill") {
            argExprs.emplace(AccArgs::kInput, getArgExpr(outputField.expr->input().get()));
            argExprs.emplace(AccArgs::kSortBy, makeVariable(getSortBySlot().first));
        } else {
            argExprs.emplace("", getArgExpr(outputField.expr->input().get()));
        }
        windowArgExprs.emplace_back(std::move(argExprs));
    }
    if (windowArgProjects.size() > 0) {
        stage = sbe::makeS<sbe::ProjectStage>(
            std::move(stage), std::move(windowArgProjects), windowNode->nodeId());
    }

    // Creating window definitions, including the slots and expressions for the bounds and
    // accumulators.
    std::vector<sbe::WindowStage::Window> windows;
    std::vector<std::string> windowFields;
    sbe::value::SlotVector windowFinalSlots;
    sbe::SlotExprPairVector windowFinalProjects;
    std::vector<boost::optional<size_t>> windowFrameFirstSlotIdx;
    std::vector<boost::optional<size_t>> windowFrameLastSlotIdx;
    for (size_t i = 0; i < windowNode->outputFields.size(); i++) {
        sbe::WindowStage::Window window{};
        auto& outputField = windowNode->outputFields[i];
        windowFields.push_back(outputField.fieldName);

        // Check whether window is removable or not.
        auto isUnboundedBoundRemovable = [](const WindowBounds::Unbounded&) {
            return false;
        };
        auto isCurrentBoundRemovable = [](const WindowBounds::Current&) {
            return true;
        };
        auto isDocumentWindowRemovable = [&](const WindowBounds::DocumentBased& document) {
            auto isValueBoundRemovable = [](const int&) {
                return true;
            };
            return stdx::visit(OverloadedVisitor{isUnboundedBoundRemovable,
                                                 isCurrentBoundRemovable,
                                                 isValueBoundRemovable},
                               document.lower);
        };
        auto isRangeWindowRemovable = [&](const WindowBounds::RangeBased& range) {
            auto isValueBoundRemovable = [](const Value&) {
                return true;
            };
            return stdx::visit(OverloadedVisitor{isUnboundedBoundRemovable,
                                                 isCurrentBoundRemovable,
                                                 isValueBoundRemovable},
                               range.lower);
        };
        const auto& windowBounds = outputField.expr->bounds();
        auto removable =
            stdx::visit(OverloadedVisitor{isDocumentWindowRemovable, isRangeWindowRemovable},
                        windowBounds.bounds);

        // Create a fake accumulation statement for non-removable window bounds.
        auto accStmt = createFakeAccumulationStatement(_state, outputField);

        // Get init expression arg for relevant functions
        auto getUnitArg = [&](window_function::ExpressionWithUnit* expr) {
            auto unit = expr->unitInMillis();
            if (unit) {
                return makeInt64Constant(*unit);
            } else {
                return makeNullConstant();
            }
        };
        auto initExprArgs = [&]() {
            StringDataMap<std::unique_ptr<sbe::EExpression>> initExprArgs;
            if (outputField.expr->getOpName() == AccumulatorExpMovingAvg::kName) {
                auto alpha = [&]() {
                    auto emaExpr = dynamic_cast<window_function::ExpressionExpMovingAvg*>(
                        outputField.expr.get());
                    if (auto N = emaExpr->getN(); N) {
                        return Decimal128(2).divide(Decimal128(N.get()).add(Decimal128(1)));
                    } else {
                        return emaExpr->getAlpha().get();
                    }
                }();
                initExprArgs.emplace("", makeDecimalConstant(alpha));
            } else if (outputField.expr->getOpName() == AccumulatorIntegral::kName) {
                initExprArgs.emplace("",
                                     getUnitArg(dynamic_cast<window_function::ExpressionWithUnit*>(
                                         outputField.expr.get())));
            } else if (outputField.expr->getOpName() == "$firstN") {
                auto nExprPtr =
                    dynamic_cast<
                        window_function::ExpressionN<WindowFunctionFirstN, AccumulatorFirstN>*>(
                        outputField.expr.get())
                        ->nExpr.get();
                initExprArgs.emplace(AccArgs::kMaxSize,
                                     generateExpression(_state, nExprPtr, rootSlotOpt, &outputs)
                                         .extractExpr(_state)
                                         .expr);
                initExprArgs.emplace(AccArgs::kIsGroupAccum,
                                     makeConstant(sbe::value::TypeTags::Boolean,
                                                  sbe::value::bitcastFrom<bool>(false)));
            } else if (outputField.expr->getOpName() == "$lastN") {
                auto nExprPtr =
                    dynamic_cast<
                        window_function::ExpressionN<WindowFunctionLastN, AccumulatorLastN>*>(
                        outputField.expr.get())
                        ->nExpr.get();
                initExprArgs.emplace(AccArgs::kMaxSize,
                                     generateExpression(_state, nExprPtr, rootSlotOpt, &outputs)
                                         .extractExpr(_state)
                                         .expr);
                initExprArgs.emplace(AccArgs::kIsGroupAccum,
                                     makeConstant(sbe::value::TypeTags::Boolean,
                                                  sbe::value::bitcastFrom<bool>(false)));
            } else {
                initExprArgs.emplace("", std::unique_ptr<mongo::sbe::EExpression>(nullptr));
            }
            return initExprArgs;
        }();

        // Create init/add/remove expressions.
        auto argExprs = std::move(windowArgExprs[i]);
        auto cloneExprMap = [](const StringDataMap<std::unique_ptr<sbe::EExpression>>& exprMap) {
            StringDataMap<std::unique_ptr<sbe::EExpression>> exprMapClone;
            for (auto& [argName, argExpr] : exprMap) {
                exprMapClone.emplace(argName, argExpr->clone());
            }
            return exprMapClone;
        };
        if (removable) {
            if (initExprArgs.size() == 1) {
                window.initExprs =
                    buildWindowInit(_state, outputField, std::move(initExprArgs.begin()->second));
            } else {
                window.initExprs = buildWindowInit(_state, outputField, std::move(initExprArgs));
            }
            if (argExprs.size() == 1) {
                window.addExprs =
                    buildWindowAdd(_state, outputField, argExprs.begin()->second->clone());
                window.removeExprs =
                    buildWindowRemove(_state, outputField, argExprs.begin()->second->clone());
            } else {
                window.addExprs =
                    buildWindowAdd(_state, outputField, cloneExprMap(argExprs), collatorSlot);
                window.removeExprs = buildWindowRemove(_state, outputField, cloneExprMap(argExprs));
            }
        } else {
            if (initExprArgs.size() == 1) {
                window.initExprs = buildInitialize(
                    accStmt, std::move(initExprArgs.begin()->second), _frameIdGenerator);
            } else {
                window.initExprs =
                    buildInitialize(accStmt, std::move(initExprArgs), _frameIdGenerator);
            }
            if (argExprs.size() == 1) {
                window.addExprs = buildAccumulator(
                    accStmt, argExprs.begin()->second->clone(), collatorSlot, _frameIdGenerator);
            } else {
                window.addExprs = buildAccumulator(
                    accStmt, cloneExprMap(argExprs), collatorSlot, _frameIdGenerator);
            }
            window.removeExprs =
                std::vector<std::unique_ptr<sbe::EExpression>>{window.addExprs.size()};
        }

        for (size_t i = 0; i < window.initExprs.size(); i++) {
            window.windowExprSlots.push_back(_slotIdGenerator.generate());
        }

        tassert(7914601,
                "Init/add/remove expressions of a window function should be of the same size",
                window.initExprs.size() == window.addExprs.size() &&
                    window.addExprs.size() == window.removeExprs.size() &&
                    window.removeExprs.size() == window.windowExprSlots.size());


        // Build bound expressions and create window definitions.

        // Create frame first and last slots if the window requires.
        bool frameFirstLastAccumulators = true;
        if (outputField.expr->getOpName() == "$derivative") {
            windowFrameFirstSlotIdx.push_back(registerFrameFirstSlots());
            windowFrameLastSlotIdx.push_back(registerFrameLastSlots());
        } else if (outputField.expr->getOpName() == "$first" && removable) {
            windowFrameFirstSlotIdx.push_back(registerFrameFirstSlots());
            windowFrameLastSlotIdx.push_back(boost::none);
        } else if (outputField.expr->getOpName() == "$last" && removable) {
            windowFrameFirstSlotIdx.push_back(boost::none);
            windowFrameLastSlotIdx.push_back(registerFrameLastSlots());
        } else {
            frameFirstLastAccumulators = false;
            windowFrameFirstSlotIdx.push_back(boost::none);
            windowFrameLastSlotIdx.push_back(boost::none);
        }

        auto makeOffsetBoundExpr = [&](sbe::value::SlotId boundSlot,
                                       std::pair<sbe::value::TypeTags, sbe::value::Value> offset =
                                           {sbe::value::TypeTags::Nothing, 0},
                                       boost::optional<TimeUnit> unit = boost::none) {
            if (offset.first == sbe::value::TypeTags::Nothing) {
                return makeVariable(boundSlot);
            }
            if (unit) {
                auto [unitTag, unitVal] = sbe::value::makeNewString(serializeTimeUnit(*unit));
                sbe::value::ValueGuard unitGuard{unitTag, unitVal};
                auto [timezoneTag, timezoneVal] = sbe::value::makeNewString("UTC");
                sbe::value::ValueGuard timezoneGuard{timezoneTag, timezoneVal};
                auto [longOffsetOwned, longOffsetTag, longOffsetVal] = genericNumConvert(
                    offset.first, offset.second, sbe::value::TypeTags::NumberInt64);
                unitGuard.reset();
                timezoneGuard.reset();
                return makeFunction("dateAdd",
                                    makeVariable(*_state.getTimeZoneDBSlot()),
                                    makeVariable(boundSlot),
                                    makeConstant(unitTag, unitVal),
                                    makeConstant(longOffsetTag, longOffsetVal),
                                    makeConstant(timezoneTag, timezoneVal));
            } else {
                return makeBinaryOp(sbe::EPrimBinary::add,
                                    makeVariable(boundSlot),
                                    makeConstant(offset.first, offset.second));
            }
        };
        auto makeLowBoundExpr = [&](sbe::value::SlotId boundSlot,
                                    sbe::value::SlotId boundTestingSlot,
                                    std::pair<sbe::value::TypeTags, sbe::value::Value> offset =
                                        {sbe::value::TypeTags::Nothing, 0},
                                    boost::optional<TimeUnit> unit = boost::none) {
            return makeBinaryOp(sbe::EPrimBinary::greaterEq,
                                makeVariable(boundTestingSlot),
                                makeOffsetBoundExpr(boundSlot, offset, unit));
        };
        auto makeHighBoundExpr = [&](sbe::value::SlotId boundSlot,
                                     sbe::value::SlotId boundTestingSlot,
                                     std::pair<sbe::value::TypeTags, sbe::value::Value> offset =
                                         {sbe::value::TypeTags::Nothing, 0},
                                     boost::optional<TimeUnit> unit = boost::none) {
            return makeBinaryOp(sbe::EPrimBinary::lessEq,
                                makeVariable(boundTestingSlot),
                                makeOffsetBoundExpr(boundSlot, offset, unit));
        };
        auto makeLowUnboundedExpr = [&](const WindowBounds::Unbounded&) {
            window.lowBoundExpr = nullptr;
        };
        auto makeHighUnboundedExpr = [&](const WindowBounds::Unbounded&) {
            window.highBoundExpr = nullptr;
        };
        auto makeLowCurrentExpr = [&](const WindowBounds::Current&) {
            auto [lowBoundSlot, lowBoundTestingSlot] = getDocumentBoundSlot();
            window.lowBoundExpr = makeLowBoundExpr(lowBoundSlot, lowBoundTestingSlot);
        };
        auto makeHighCurrentExpr = [&](const WindowBounds::Current&) {
            auto [highBoundSlot, highBoundTestingSlot] = getDocumentBoundSlot();
            window.highBoundExpr = makeHighBoundExpr(highBoundSlot, highBoundTestingSlot);
        };
        auto documentCase = [&](const WindowBounds::DocumentBased& document) {
            auto makeLowValueExpr = [&](const int& v) {
                auto [lowBoundSlot, lowBoundTestingSlot] = getDocumentBoundSlot();
                window.lowBoundExpr = makeLowBoundExpr(
                    lowBoundSlot,
                    lowBoundTestingSlot,
                    {sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(v)});
            };
            auto makeHighValueExpr = [&](const int& v) {
                auto [highBoundSlot, highBoundTestingSlot] = getDocumentBoundSlot();
                window.highBoundExpr = makeHighBoundExpr(
                    highBoundSlot,
                    highBoundTestingSlot,
                    {sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(v)});
            };
            stdx::visit(
                OverloadedVisitor{makeLowUnboundedExpr, makeLowCurrentExpr, makeLowValueExpr},
                document.lower);
            stdx::visit(
                OverloadedVisitor{makeHighUnboundedExpr, makeHighCurrentExpr, makeHighValueExpr},
                document.upper);
        };
        auto rangeCase = [&](const WindowBounds::RangeBased& range) {
            auto rangeBoundSlot = getRangeBoundSlot(range.unit).first;
            auto rangeBoundTestingSlot = getRangeBoundSlot(range.unit).second;
            auto makeLowValueExpr = [&](const Value& v) {
                window.lowBoundExpr = makeLowBoundExpr(
                    rangeBoundSlot, rangeBoundTestingSlot, sbe::value::makeValue(v), range.unit);
            };
            auto makeHighValueExpr = [&](const Value& v) {
                window.highBoundExpr = makeHighBoundExpr(
                    rangeBoundSlot, rangeBoundTestingSlot, sbe::value::makeValue(v), range.unit);
            };
            stdx::visit(
                OverloadedVisitor{makeLowUnboundedExpr, makeLowCurrentExpr, makeLowValueExpr},
                range.lower);
            stdx::visit(
                OverloadedVisitor{makeHighUnboundedExpr, makeHighCurrentExpr, makeHighValueExpr},
                range.upper);
        };

        stdx::visit(OverloadedVisitor{documentCase, rangeCase}, windowBounds.bounds);

        if (outputField.expr->getOpName() == "$linearFill") {
            tassert(7971215, "expected a single initExpr", window.initExprs.size() == 1);
            window.highBoundExpr =
                makeFunction("aggLinearFillCanAdd", makeVariable(window.windowExprSlots[0]));
        }

        // Build extra arguments for finalize expressions.
        auto getModifiedExpr = [&](std::unique_ptr<sbe::EExpression> argExpr,
                                   sbe::value::SlotVector& newSlots) {
            if (auto varExpr = argExpr->as<sbe::EVariable>(); varExpr) {
                auto idx = ensureSlotInBuffer(varExpr->getSlotId());
                return makeVariable(newSlots[idx]);
            } else if (argExpr->as<sbe::EConstant>()) {
                return argExpr->clone();
            } else {
                MONGO_UNREACHABLE;
            }
        };

        StringDataMap<std::unique_ptr<sbe::EExpression>> finalArgExprs;
        if (outputField.expr->getOpName() == "$derivative") {
            auto unit = getUnitArg(
                dynamic_cast<window_function::ExpressionWithUnit*>(outputField.expr.get()));
            auto it = argExprs.find(AccArgs::kInput);
            tassert(7993401,
                    str::stream() << "Window function expects '" << AccArgs::kInput << "' argument",
                    it != argExprs.end());
            auto inputExpr = it->second->clone();
            it = argExprs.find(AccArgs::kSortBy);
            tassert(7993402,
                    str::stream() << "Window function expects '" << AccArgs::kSortBy
                                  << "' argument",
                    it != argExprs.end());
            auto sortByExpr = it->second->clone();

            auto& frameFirstSlots = windowFrameFirstSlots[*windowFrameFirstSlotIdx.back()];
            auto& frameLastSlots = windowFrameLastSlots[*windowFrameLastSlotIdx.back()];
            auto frameFirstInput = getModifiedExpr(inputExpr->clone(), frameFirstSlots);
            auto frameLastInput = getModifiedExpr(inputExpr->clone(), frameLastSlots);
            auto frameFirstSortBy = getModifiedExpr(sortByExpr->clone(), frameFirstSlots);
            auto frameLastSortBy = getModifiedExpr(sortByExpr->clone(), frameLastSlots);
            finalArgExprs.emplace(AccArgs::kUnit, std::move(unit));
            finalArgExprs.emplace(AccArgs::kDerivativeInputFirst, std::move(frameFirstInput));
            finalArgExprs.emplace(AccArgs::kDerivativeInputLast, std::move(frameLastInput));
            finalArgExprs.emplace(AccArgs::kDerivativeSortByFirst, std::move(frameFirstSortBy));
            finalArgExprs.emplace(AccArgs::kDerivativeSortByLast, std::move(frameLastSortBy));
        } else if (outputField.expr->getOpName() == "$first" && removable) {
            tassert(8085502,
                    str::stream() << "Window function $first expects 1 argument",
                    argExprs.size() == 1);
            auto it = argExprs.begin();
            auto& frameFirstSlots = windowFrameFirstSlots[*windowFrameFirstSlotIdx.back()];
            auto inputExpr = it->second->clone();
            auto frameFirstInput = getModifiedExpr(inputExpr->clone(), frameFirstSlots);
            finalArgExprs.emplace(AccArgs::kInput, std::move(frameFirstInput));
        } else if (outputField.expr->getOpName() == "$last" && removable) {
            tassert(8085503,
                    str::stream() << "Window function $last expects 1 argument",
                    argExprs.size() == 1);
            auto it = argExprs.begin();
            auto inputExpr = it->second->clone();
            auto& frameLastSlots = windowFrameLastSlots[*windowFrameLastSlotIdx.back()];
            auto frameLastInput = getModifiedExpr(inputExpr->clone(), frameLastSlots);
            finalArgExprs.emplace(AccArgs::kInput, std::move(frameLastInput));
        } else if (outputField.expr->getOpName() == "$linearFill") {
            finalArgExprs = std::move(argExprs);
        }

        // Build finalize expressions.
        std::unique_ptr<sbe::EExpression> finalExpr;
        if (removable) {
            finalExpr = finalArgExprs.size() > 0
                ? buildWindowFinalize(_state,
                                      outputField,
                                      window.windowExprSlots,
                                      std::move(finalArgExprs),
                                      collatorSlot)
                : buildWindowFinalize(_state, outputField, window.windowExprSlots, collatorSlot);
        } else {
            finalExpr = finalArgExprs.size() > 0
                ? buildFinalize(_state,
                                accStmt,
                                window.windowExprSlots,
                                std::move(finalArgExprs),
                                collatorSlot,
                                _frameIdGenerator)
                : buildFinalize(
                      _state, accStmt, window.windowExprSlots, collatorSlot, _frameIdGenerator);
        }

        // Deal with empty window for finalize expressions.
        if (!frameFirstLastAccumulators) {
            auto emptyWindowExpr = [](StringData accExprName) {
                if (accExprName == "$sum") {
                    return makeConstant(sbe::value::TypeTags::NumberInt32, 0);
                } else if (accExprName == "$push") {
                    auto [tag, val] = sbe::value::makeNewArray();
                    return makeConstant(tag, val);
                } else {
                    return makeConstant(sbe::value::TypeTags::Null, 0);
                }
            }(outputField.expr->getOpName());
            if (finalExpr) {
                finalExpr = sbe::makeE<sbe::EIf>(
                    makeFunction("exists", makeVariable(window.windowExprSlots[0])),
                    std::move(finalExpr),
                    std::move(emptyWindowExpr));
            } else {
                finalExpr = makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                                         makeVariable(window.windowExprSlots[0]),
                                         std::move(emptyWindowExpr));
            }
        }
        auto finalSlot = _slotIdGenerator.generate();
        windowFinalProjects.emplace_back(finalSlot, std::move(finalExpr));
        windowFinalSlots.push_back(finalSlot);
        windows.emplace_back(std::move(window));
    }

    // Assign frame first/last slots to window definitions.
    for (size_t windowIdx = 0; windowIdx < windows.size(); ++windowIdx) {
        if (windowFrameFirstSlotIdx[windowIdx]) {
            windows[windowIdx].frameFirstSlots =
                std::move(windowFrameFirstSlots[*windowFrameFirstSlotIdx[windowIdx]]);
        }
        if (windowFrameLastSlotIdx[windowIdx]) {
            windows[windowIdx].frameLastSlots =
                std::move(windowFrameLastSlots[*windowFrameLastSlotIdx[windowIdx]]);
        }
    }

    // Calculate sliding window.
    stage = sbe::makeS<sbe::WindowStage>(std::move(stage),
                                         std::move(currSlots),
                                         std::move(boundTestingSlots),
                                         partitionSlotCount,
                                         std::move(windows),
                                         collatorSlot,
                                         _cq.getExpCtx()->allowDiskUse,
                                         windowNode->nodeId());

    // Get final window outputs.
    stage = sbe::makeS<sbe::ProjectStage>(
        std::move(stage), std::move(windowFinalProjects), windowNode->nodeId());

    if (reqs.has(kResult)) {
        std::vector<ProjectionNode> nodes;
        for (size_t i = 0; i < windowFields.size(); ++i) {
            nodes.emplace_back(SbExpr{windowFinalSlots[i]});
        }

        auto resultSlot = outputs.get(kResult);
        auto projType = projection_ast::ProjectType::kAddition;
        auto projectionExpr = generateProjection(_state,
                                                 projType,
                                                 std::move(windowFields),
                                                 std::move(nodes),
                                                 resultSlot.slotId,
                                                 resultSlot)
                                  .extractExpr(_state);

        auto outResultSlot = _state.slotId();
        auto outStage = makeProject(
            std::move(stage), windowNode->nodeId(), outResultSlot, std::move(projectionExpr.expr));

        stage = std::move(outStage);
        outputs.set(kResult, TypedSlot{outResultSlot, projectionExpr.typeSignature});
    }

    outputs.clearNonRequiredSlots(reqs);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSearch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto sn = static_cast<const SearchNode*>(root);
    const auto& collection = getCurrentCollection(reqs);
    auto expCtx = _cq.getExpCtxRaw();

    // Register search query parameter slots.
    auto cursorIdSlot = _env->registerSlot("searchCursorId"_sd,
                                           sbe::value::TypeTags::Nothing,
                                           0 /* val */,
                                           false /* owned */,
                                           &_slotIdGenerator);
    auto firstBatchSlot = _env->registerSlot("searchFirstBatch"_sd,
                                             sbe::value::TypeTags::Nothing,
                                             0 /* val */,
                                             false /* owned */,
                                             &_slotIdGenerator);
    auto searchQuerySlot = _env->registerSlot("searchQuery"_sd,
                                              sbe::value::TypeTags::Nothing,
                                              0 /* val */,
                                              false /* owned */,
                                              &_slotIdGenerator);
    auto limitSlot = _env->registerSlot("searchLimit"_sd,
                                        sbe::value::TypeTags::Nothing,
                                        0 /* val */,
                                        false /* owned */,
                                        &_slotIdGenerator);

    auto protocolVersionSlot = _env->registerSlot("searchProtocolVersion"_sd,
                                                  sbe::value::TypeTags::Nothing,
                                                  0 /* val */,
                                                  false /* owned */,
                                                  &_slotIdGenerator);

    bool isStoredSource = sn->searchQuery.getBoolField(kReturnStoredSourceArg);

    auto topLevelFields = getTopLevelFields(reqs.getFields());
    auto topLevelFieldSlots = _slotIdGenerator.generateMultiple(topLevelFields.size());

    PlanStageSlots outputs;

    for (size_t i = 0; i < topLevelFields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, topLevelFields[i]),
                    topLevelFieldSlots[i]);
    }

    // Search cursor stage output slots
    auto searchResultSlot = isStoredSource && reqs.has(kResult)
        ? boost::make_optional(_slotIdGenerator.generate())
        : boost::none;
    // Register the $$SEARCH_META slot.
    _state.getBuiltinVarSlot(Variables::kSearchMetaId);

    std::vector<std::string> metadataNames = {Document::metaFieldSearchScore.toString(),
                                              Document::metaFieldSearchHighlights.toString(),
                                              Document::metaFieldSearchScoreDetails.toString(),
                                              Document::metaFieldSearchSortValues.toString()};
    auto metadataSlots = _slotIdGenerator.generateMultiple(metadataNames.size());
    // We have to generate all search metadata slots until we have migrate everything to SBE, this
    // is because the metadata usage may depends on post-SBE DocumentSources in the pipeline, the
    // SBE plan cache won't work in this case.
    _data->metadataSlots.searchScoreSlot = metadataSlots[0];
    _data->metadataSlots.searchHighlightsSlot = metadataSlots[1];
    _data->metadataSlots.searchDetailsSlot = metadataSlots[2];
    _data->metadataSlots.searchSortValuesSlot = metadataSlots[3];

    std::vector<std::string> fieldNames =
        isStoredSource ? topLevelFields : std::vector<std::string>{"_id"};
    auto fieldSlots =
        isStoredSource ? topLevelFieldSlots : _slotIdGenerator.generateMultiple(fieldNames.size());

    auto searchCursorStage = sbe::makeS<sbe::SearchCursorStage>(expCtx->ns,
                                                                expCtx->uuid,
                                                                searchResultSlot,
                                                                metadataNames,
                                                                metadataSlots,
                                                                fieldNames,
                                                                fieldSlots,
                                                                cursorIdSlot,
                                                                firstBatchSlot,
                                                                searchQuerySlot,
                                                                boost::none /* sortSpecSlot */,
                                                                limitSlot,
                                                                protocolVersionSlot,
                                                                expCtx->explain,
                                                                _yieldPolicy,
                                                                sn->nodeId());
    if (isStoredSource) {
        if (searchResultSlot) {
            outputs.set(kResult, searchResultSlot.value());
        }
        outputs.clearNonRequiredSlots(reqs);
        return {std::move(searchCursorStage), std::move(outputs)};
    }

    auto searchCursorStagePtr = dynamic_cast<sbe::SearchCursorStage*>(searchCursorStage.get());

    // Make a project stage to convert '_id' field value into keystring.
    auto catalog = collection->getIndexCatalog();
    auto indexDescriptor = catalog->findIndexByName(_state.opCtx, kIdIndexName);
    auto indexAccessMethod = catalog->getEntry(indexDescriptor)->accessMethod()->asSortedData();
    auto sortedData = indexAccessMethod->getSortedDataInterface();
    auto version = sortedData->getKeyStringVersion();
    auto ordering = sortedData->getOrdering();

    auto collatorSlot = _state.getCollatorSlot();
    auto makeNewKeyFunc = [&](key_string::Discriminator discriminator) {
        StringData functionName = collatorSlot ? "collKs" : "ks";
        sbe::EExpression::Vector args;
        args.emplace_back(makeInt64Constant(static_cast<int64_t>(version)));
        args.emplace_back(makeInt32Constant(ordering.getBits()));
        args.emplace_back(makeVariable(fieldSlots[0]));
        args.emplace_back(makeInt64Constant(static_cast<int64_t>(discriminator)));
        if (collatorSlot) {
            args.emplace_back(makeVariable(*collatorSlot));
        }
        return makeE<sbe::EFunction>(functionName, std::move(args));
    };

    auto childReqs = reqs.copy()
                         .set(kRecordId)
                         .set(kSnapshotId)
                         .set(kIndexIdent)
                         .set(kIndexKey)
                         .set(kIndexKeyPattern);
    auto [idxScanStage, idxOutputs] =
        generateSingleIntervalIndexScan(_state,
                                        collection,
                                        kIdIndexName.toString(),
                                        indexDescriptor->keyPattern(),
                                        makeNewKeyFunc(key_string::Discriminator::kExclusiveBefore),
                                        makeNewKeyFunc(key_string::Discriminator::kExclusiveAfter),
                                        {} /* indexKeysToInclude */,
                                        {} /* indexKeySlots */,
                                        childReqs,
                                        _yieldPolicy,
                                        sn->nodeId(),
                                        true /* forward */,
                                        false /* lowPriority */);

    // Slot stores the resulting document.
    auto outputDocSlot = _slotIdGenerator.generate();
    outputs.set(kResult, outputDocSlot);
    // Slot stores rid if it it found, in our case same as seekRecordIdSlot.
    auto ridSlot = _slotIdGenerator.generate();

    // Join the idx scan stage with fetch stage.
    auto fetchStage = makeLoopJoinForFetch(std::move(idxScanStage),
                                           outputDocSlot,
                                           ridSlot,
                                           topLevelFields,
                                           topLevelFieldSlots,
                                           idxOutputs.get(kRecordId).slotId,
                                           idxOutputs.get(kSnapshotId).slotId,
                                           idxOutputs.get(kIndexIdent).slotId,
                                           idxOutputs.get(kIndexKey).slotId,
                                           idxOutputs.get(kIndexKeyPattern).slotId,
                                           collection,
                                           sn->nodeId(),
                                           sbe::makeSV() /* slotsToForward */);

    // Join the search_cursor+project stage with idx_scan+fetch stage.
    auto stage = sbe::makeS<sbe::LoopJoinStage>(
        std::move(searchCursorStage),
        sbe::makeS<sbe::LimitSkipStage>(
            std::move(fetchStage), 1, boost::none /* skip */, sn->nodeId()),
        metadataSlots,
        sbe::makeSV(fieldSlots[0]),
        nullptr /* predicate */,
        sn->nodeId());

    // Use the most outer nlj stage stats to track how many documents is returned.
    // TODO: SERVER-80648 for a better solution.
    searchCursorStagePtr->setDocsReturnedStats(stage->getCommonStats());

    outputs.clearNonRequiredSlots(reqs);
    return {std::move(stage), std::move(outputs)};
}

const CollectionPtr& SlotBasedStageBuilder::getCurrentCollection(const PlanStageReqs& reqs) const {
    auto nss = reqs.getTargetNamespace();
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "No collection found that matches namespace '"
                          << nss.toStringForErrorMsg() << "'",
            _collections.lookupCollection(nss) != CollectionPtr::null);
    return _collections.lookupCollection(nss);
}

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::build(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    typedef std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> (
        SlotBasedStageBuilder::*builderCallback)(const QuerySolutionNode* root,
                                                 const PlanStageReqs& reqs);
    static const stdx::unordered_map<StageType, builderCallback> kStageBuilders = {
        {STAGE_COLLSCAN, &SlotBasedStageBuilder::buildCollScan},
        {STAGE_COUNT_SCAN, &SlotBasedStageBuilder::buildCountScan},
        {STAGE_VIRTUAL_SCAN, &SlotBasedStageBuilder::buildVirtualScan},
        {STAGE_IXSCAN, &SlotBasedStageBuilder::buildIndexScan},
        {STAGE_COLUMN_SCAN, &SlotBasedStageBuilder::buildColumnScan},
        {STAGE_FETCH, &SlotBasedStageBuilder::buildFetch},
        {STAGE_LIMIT, &SlotBasedStageBuilder::buildLimit},
        {STAGE_MATCH, &SlotBasedStageBuilder::buildMatch},
        {STAGE_REPLACE_ROOT, &SlotBasedStageBuilder::buildReplaceRoot},
        {STAGE_SKIP, &SlotBasedStageBuilder::buildSkip},
        {STAGE_SORT_SIMPLE, &SlotBasedStageBuilder::buildSort},
        {STAGE_SORT_DEFAULT, &SlotBasedStageBuilder::buildSort},
        {STAGE_SORT_KEY_GENERATOR, &SlotBasedStageBuilder::buildSortKeyGenerator},
        {STAGE_PROJECTION_SIMPLE, &SlotBasedStageBuilder::buildProjectionSimple},
        {STAGE_PROJECTION_DEFAULT, &SlotBasedStageBuilder::buildProjectionDefault},
        {STAGE_PROJECTION_COVERED, &SlotBasedStageBuilder::buildProjectionCovered},
        {STAGE_OR, &SlotBasedStageBuilder::buildOr},
        // In SBE TEXT_OR behaves like a regular OR. All the work to support "textScore"
        // metadata is done outside of TEXT_OR, unlike the legacy implementation.
        {STAGE_TEXT_OR, &SlotBasedStageBuilder::buildOr},
        {STAGE_TEXT_MATCH, &SlotBasedStageBuilder::buildTextMatch},
        {STAGE_RETURN_KEY, &SlotBasedStageBuilder::buildReturnKey},
        {STAGE_EOF, &SlotBasedStageBuilder::buildEof},
        {STAGE_AND_HASH, &SlotBasedStageBuilder::buildAndHash},
        {STAGE_AND_SORTED, &SlotBasedStageBuilder::buildAndSorted},
        {STAGE_SORT_MERGE, &SlotBasedStageBuilder::buildSortMerge},
        {STAGE_GROUP, &SlotBasedStageBuilder::buildGroup},
        {STAGE_EQ_LOOKUP, &SlotBasedStageBuilder::buildLookup},
        {STAGE_SHARDING_FILTER, &SlotBasedStageBuilder::buildShardFilter},
        {STAGE_SEARCH, &SlotBasedStageBuilder::buildSearch},
        {STAGE_WINDOW, &SlotBasedStageBuilder::buildWindow},
        {STAGE_UNPACK_TS_BUCKET, &SlotBasedStageBuilder::buildUnpackTsBucket}};

    tassert(4822884,
            str::stream() << "Unsupported QSN in SBE stage builder: " << root->toString(),
            kStageBuilders.find(root->getType()) != kStageBuilders.end());

    // If this plan is for a tailable cursor scan, and we're not already in the process of building
    // a special union sub-tree implementing such scans, then start building a union sub-tree. Note
    // that LIMIT or SKIP stage is used as a splitting point of the two union branches, if present,
    // because we need to apply limit (or skip) only in the initial scan (in the anchor branch), and
    // the resume branch should not have it.
    switch (root->getType()) {
        case STAGE_COLLSCAN:
        case STAGE_LIMIT:
        case STAGE_SKIP:
            if (_cq.getFindCommandRequest().getTailable() &&
                !reqs.getIsBuildingUnionForTailableCollScan()) {
                auto childReqs = reqs;
                childReqs.setIsBuildingUnionForTailableCollScan(true);
                return makeUnionForTailableCollScan(root, childReqs);
            }
            [[fallthrough]];
        default:
            break;
    }

    auto [stage, slots] = (this->*(kStageBuilders.at(root->getType())))(root, reqs);
    auto outputs = std::move(slots);

    auto fields = filterVector(reqs.getFields(), [&](const std::string& s) {
        return !outputs.has(std::make_pair(PlanStageSlots::kField, StringData(s)));
    });

    if (!fields.empty()) {
        tassert(6023424,
                str::stream() << "Expected build() for " << stageTypeToString(root->getType())
                              << " to either produce a kResult slot or to satisfy all kField reqs",
                outputs.has(PlanStageSlots::kResult));

        auto resultSlot = outputs.get(PlanStageSlots::kResult);
        auto [outStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                         fields,
                                                         resultSlot.slotId,
                                                         root->nodeId(),
                                                         &_slotIdGenerator,
                                                         _state,
                                                         &outputs);
        stage = std::move(outStage);

        for (size_t i = 0; i < fields.size(); ++i) {
            outputs.set(std::make_pair(PlanStageSlots::kField, std::move(fields[i])), outSlots[i]);
        }
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
