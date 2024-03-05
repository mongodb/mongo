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
#include "mongo/db/exec/sbe/stages/unwind.h"
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
#include "mongo/db/pipeline/window_function/window_function_first_last_n.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"
#include "mongo/db/pipeline/window_function/window_function_shift.h"
#include "mongo/db/pipeline/window_function/window_function_top_bottom_n.h"
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
    StageBuilderState& state, const PlanStageReqs& reqs, PlanNodeId nodeId) {
    // If the parent is asking for a result, then set materialized result on 'forwardingReqs'.
    PlanStageReqs forwardingReqs = reqs.copyForChild();
    if (reqs.hasResult()) {
        forwardingReqs.setResultObj();
    }

    // Create a new PlanStageSlots and map all the required names to the environment's Nothing slot.
    PlanStageSlots outputs;
    outputs.setMissingRequiredNamedSlotsToNothing(state, forwardingReqs);

    auto stage = makeLimitCoScanTree(nodeId, 0);
    return {std::move(stage), std::move(outputs)};
}

// Fill in the search slots based on initial cursor response from mongot.
void prepareSearchQueryParameters(PlanStageData* data, const CanonicalQuery& cq) {
    if (cq.cqPipeline().empty() || !cq.isSearchQuery() || !cq.getExpCtxRaw()->uuid) {
        return;
    }

    // Build a SearchNode in order to retrieve the search info.
    auto sn = SearchNode::getSearchNode(cq.cqPipeline().front().get());

    auto& env = data->env;

    // Set values for QSN slots.
    if (sn->limit) {
        env->resetSlot(env->getSlot("searchLimit"_sd),
                       sbe::value::TypeTags::NumberInt64,
                       *sn->limit,
                       true /* owned */);
    }

    if (sn->sortSpec) {
        auto sortSpec = std::make_unique<sbe::SortSpec>(*sn->sortSpec, cq.getExpCtx());
        env->resetSlot(env->getSlot("searchSortSpec"_sd),
                       sbe::value::TypeTags::sortSpec,
                       sbe::value::bitcastFrom<sbe::SortSpec*>(sortSpec.release()),
                       true /* owned */);
    }

    if (auto remoteVars = sn->remoteCursorVars) {
        auto name = Variables::getBuiltinVariableName(Variables::kSearchMetaId);
        // Variables on the cursor must be an object.
        auto varsObj = remoteVars->getField(name);
        if (varsObj.ok()) {
            auto [tag, val] = sbe::bson::convertFrom<false /* View */>(varsObj);
            env->resetSlot(env->getSlot(name), tag, val, true /* owned */);
            // Both the SBE and the classic portions of the query can reference the same value,
            // and this is the only place to set the value if using SBE so we don't worry about
            // inconsistency.
            cq.getExpCtx()->variables.setReservedValue(
                Variables::kSearchMetaId, mongo::Value(varsObj), true /* isConstant */);
            if (varsObj.type() == BSONType::Object) {
                auto metaValObj = varsObj.embeddedObject();
                if (metaValObj.hasField("count")) {
                    auto& opDebug = CurOp::get(cq.getOpCtx())->debug();
                    opDebug.mongotCountVal = metaValObj.getField("count").wrap();
                }

                if (metaValObj.hasField(kSlowQueryLogFieldName)) {
                    auto& opDebug = CurOp::get(cq.getOpCtx())->debug();
                    opDebug.mongotSlowQueryLog =
                        metaValObj.getField(kSlowQueryLogFieldName).wrap(kSlowQueryLogFieldName);
                }
            }
        }
    }
}
}  // namespace

sbe::value::SlotVector getSlotsOrderedByName(const PlanStageReqs& reqs,
                                             const PlanStageSlots& outputs) {
    auto requiredNamedSlots = outputs.getRequiredSlotsInOrder(reqs);

    auto outputSlots = sbe::makeSV();
    outputSlots.reserve(requiredNamedSlots.size());
    for (const TypedSlot& slot : requiredNamedSlots) {
        outputSlots.emplace_back(slot.slotId);
    }

    return outputSlots;
}

sbe::value::SlotVector getSlotsToForward(StageBuilderState& state,
                                         const PlanStageReqs& reqs,
                                         const PlanStageSlots& outputs,
                                         const sbe::value::SlotVector& exclude) {
    auto requiredNamedSlots = outputs.getRequiredSlotsUnique(reqs);

    auto slots = state.data->metadataSlots.getSlotVector();

    if (exclude.empty()) {
        for (const TypedSlot& slot : requiredNamedSlots) {
            if (!state.env->isSlotRegistered(slot.slotId)) {
                slots.emplace_back(slot.slotId);
            }
        }
    } else {
        auto excludeSet = sbe::value::SlotSet{exclude.begin(), exclude.end()};

        for (const TypedSlot& slot : requiredNamedSlots) {
            if (!state.env->isSlotRegistered(slot.slotId) && !excludeSet.count(slot.slotId)) {
                slots.emplace_back(slot.slotId);
            }
        }
    }

    return slots;
}

/**
 * Performs necessary initialization steps to execute an SBE tree 'root', including binding params
 * from the current query 'cq' into the plan if it was cloned from the SBE plan cache.
 *   root - root node of the execution tree
 *   data - slot metadata (not actual parameter data!) that goes with the execution tree
 *   preparingFromCache - if true, 'root' and 'data' may have come from the SBE plan cache. This
 *     means current parameters from 'cq' need to be substituted into the execution plan.
 */
void prepareSlotBasedExecutableTree(OperationContext* opCtx,
                                    sbe::PlanStage* root,
                                    PlanStageData* data,
                                    const CanonicalQuery& cq,
                                    const MultipleCollectionAccessor& collections,
                                    PlanYieldPolicySBE* yieldPolicy,
                                    const bool preparingFromCache,
                                    RemoteCursorMap* remoteCursors) {
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
    env.ctx.remoteCursors = remoteCursors;

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
        auto matchStage = dynamic_cast<DocumentSourceMatch*>(innerStage.get());
        if (matchStage) {
            input_params::bind(matchStage->getMatchExpression(), *data, preparingFromCache);
        }
    }

    interval_evaluation_tree::IndexBoundsEvaluationCache indexBoundsEvaluationCache;
    for (auto&& indexBoundsInfo : data->staticData->indexBoundsEvaluationInfos) {
        input_params::bindIndexBounds(
            cq, indexBoundsInfo, env.runtimeEnv, &indexBoundsEvaluationCache);
    }

    if (preparingFromCache && data->staticData->doClusteredCollectionScanSbe) {
        input_params::bindClusteredCollectionBounds(cq, root, data, env.runtimeEnv);
    }

    if (preparingFromCache && cq.shouldParameterizeLimitSkip()) {
        input_params::bindLimitSkipInputSlots(cq, data, env.runtimeEnv);
    }

    prepareSearchQueryParameters(data, cq);
}  // prepareSlotBasedExecutableTree

std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>
buildSearchMetadataExecutorSBE(OperationContext* opCtx,
                               const CanonicalQuery& cq,
                               size_t remoteCursorId,
                               RemoteCursorMap* remoteCursors,
                               PlanYieldPolicySBE* yieldPolicy) {
    auto expCtx = cq.getExpCtxRaw();
    Environment env(std::make_unique<sbe::RuntimeEnvironment>());
    std::unique_ptr<PlanStageStaticData> data(std::make_unique<PlanStageStaticData>());
    sbe::value::SlotIdGenerator slotIdGenerator;
    data->resultSlot = slotIdGenerator.generate();

    auto stage = sbe::SearchCursorStage::createForMetadata(expCtx->ns,
                                                           expCtx->uuid,
                                                           data->resultSlot,
                                                           remoteCursorId,
                                                           yieldPolicy,
                                                           PlanNodeId{} /* planNodeId */);

    env.ctx.remoteCursors = remoteCursors;
    stage->attachToOperationContext(opCtx);
    stage->prepare(env.ctx);
    data->cursorType = CursorTypeEnum::SearchMetaResult;
    return std::make_pair(std::move(stage), PlanStageData(std::move(env), std::move(data)));
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

PlanStageSlots PlanStageSlots::makeMergedPlanStageSlots(StageBuilderState& state,
                                                        PlanNodeId nodeId,
                                                        const PlanStageReqs& reqs,
                                                        std::vector<PlanStageTree>& trees) {
    tassert(8146604, "Expected 'trees' to be non-empty", !trees.empty());

    PlanStageSlots outputs;

    if (reqs.hasResultInfo()) {
        // Merge the childeren's result infos.
        mergeResultInfos(state, nodeId, trees);
    }

    if (reqs.hasResultObj()) {
        // Assert each tree produces a materialized result.
        for (auto& tree : trees) {
            bool hasResultObject = tree.second.hasResultObj();
            tassert(8378200, "Expected child tree to produce a result object", hasResultObject);
        }
    }

    auto& firstTreeOutputs = trees[0].second;

    for (const auto& slotName : firstTreeOutputs.getRequiredNamesInOrder(reqs)) {
        outputs._data->slotNameToIdMap[slotName] = TypedSlot{state.slotId()};
    }

    if (reqs.hasResultInfo()) {
        // If 'reqs' requires ResultInfo, call setResultInfoBaseObj() to properly set a result base
        // object on 'outputs' and then copy over the ResultInfo changes from the first child to the
        // parent.
        outputs.setResultInfoBaseObj(outputs.get(kResult));
        outputs._data->resultInfoChanges.emplace(*trees[0].second._data->resultInfoChanges);
    } else if (reqs.hasResultObj()) {
        tassert(8428006, "Expected result object to be set", outputs.hasResultObj());
    }

    return outputs;
}

std::vector<PlanStageSlots::OwnedSlotName> PlanStageSlots::getRequiredNamesInOrder(
    const PlanStageReqs& reqs) const {
    // Get the required names from 'reqs' and store them into 'names'.
    std::vector<OwnedSlotName> names(reqs._data->slotNameSet.begin(),
                                     reqs._data->slotNameSet.end());

    // Always treat as required, if it present, the slot holding the bitmap with the filtered items
    // of block values.
    if (has(kBlockSelectivityBitmap)) {
        names.emplace_back(kBlockSelectivityBitmap);
    }

    // If this PlanStageSlots has ResultInfo and 'reqs.hasResult()' is true, then we need
    // to get the list of changed fields and add them to 'names'.
    if (reqs.hasResult() && hasResultInfo()) {
        auto modified = _data->resultInfoChanges->getModifiedOrCreatedFieldSet();
        for (auto&& fieldName : modified.getList()) {
            names.emplace_back(OwnedSlotName(kField, fieldName));
        }
    }

    // Sort and de-dup the list, and then return it.
    std::sort(names.begin(), names.end());

    auto newEnd = std::unique(names.begin(), names.end());
    if (newEnd != names.end()) {
        names.erase(newEnd, names.end());
    }

    return names;
}

TypedSlotVector PlanStageSlots::getRequiredSlotsInOrder(const PlanStageReqs& reqs) const {
    auto names = getRequiredNamesInOrder(reqs);

    // Build the list of corresponding slots.
    TypedSlotVector result;
    for (const auto& name : names) {
        auto it = _data->slotNameToIdMap.find(name);
        tassert(8146615,
                str::stream() << "Could not find " << static_cast<int>(name.first) << ":'"
                              << name.second << "' in the slot map, expected slot to exist",
                it != _data->slotNameToIdMap.end());

        result.emplace_back(it->second);
    }

    return result;
}

struct NameTypedSlotPairLt {
    using UnownedSlotName = PlanStageSlots::UnownedSlotName;
    using PairType = std::pair<UnownedSlotName, TypedSlot>;

    bool operator()(const PairType& lhs, const PairType& rhs) const {
        return lhs.first != rhs.first ? lhs.first < rhs.first
                                      : TypedSlot::Less()(lhs.second, rhs.second);
    }
};

struct NameTypedSlotPairEq {
    using UnownedSlotName = PlanStageSlots::UnownedSlotName;
    using PairType = std::pair<UnownedSlotName, TypedSlot>;

    bool operator()(const PairType& lhs, const PairType& rhs) const {
        return lhs.first == rhs.first && TypedSlot::EqualTo()(lhs.second, rhs.second);
    }
};

TypedSlotVector PlanStageSlots::getRequiredSlotsUnique(const PlanStageReqs& reqs) const {
    auto names = getRequiredNamesInOrder(reqs);

    TypedSlotVector result;

    // Build the list of corresponding slots.
    for (const auto& name : names) {
        auto it = _data->slotNameToIdMap.find(name);
        tassert(8146616,
                str::stream() << "Could not find " << static_cast<int>(name.first) << ":'"
                              << name.second << "' in the slot map, expected slot to exist",
                it != _data->slotNameToIdMap.end());

        result.emplace_back(it->second);
    }

    // Sort and de-dup the list by SlotId.
    std::sort(result.begin(), result.end(), TypedSlot::Less());

    auto newEnd = std::unique(result.begin(), result.end(), TypedSlot::EqualTo());
    if (newEnd != result.end()) {
        result.erase(newEnd, result.end());
    }

    return result;
}

std::vector<std::pair<PlanStageSlots::UnownedSlotName, TypedSlot>>
PlanStageSlots::getAllNameSlotPairsInOrder() const {
    std::vector<std::pair<UnownedSlotName, TypedSlot>> nameSlotPairs;
    nameSlotPairs.reserve(_data->slotNameToIdMap.size());

    for (auto& p : _data->slotNameToIdMap) {
        nameSlotPairs.emplace_back(p.first, p.second);
    }

    std::sort(nameSlotPairs.begin(), nameSlotPairs.end(), NameTypedSlotPairLt());

    return nameSlotPairs;
}

TypedSlotVector PlanStageSlots::getAllSlotsInOrder() const {
    TypedSlotVector result;

    auto nameSlotPairs = getAllNameSlotPairsInOrder();

    result.reserve(nameSlotPairs.size());
    for (auto& p : nameSlotPairs) {
        result.emplace_back(p.second);
    }

    return result;
}

void PlanStageSlots::setMissingRequiredNamedSlotsToNothing(StageBuilderState& state,
                                                           const PlanStageReqs& reqs) {
    // If 'reqs' requires a result and we don't have a result, or if 'reqs' requires a materialized
    // result and we don't have a materialized result, then we call setResultObj() to set
    // a materialized result of Nothing on 'outputs'.
    if ((reqs.hasResult() && !hasResult()) || (reqs.hasResultObj() && !hasResultObj())) {
        auto nothingSlot = TypedSlot{state.getNothingSlot()};
        setResultObj(nothingSlot);
    }

    auto names = getRequiredNamesInOrder(reqs);

    for (const auto& name : names) {
        if (!has(name)) {
            auto nothingSlot = TypedSlot{state.getNothingSlot()};
            set(name, nothingSlot);
        }
    }
}

void PlanStageSlots::clearNonRequiredSlots(const PlanStageReqs& reqs, bool saveResultObj) {
    // If 'reqs' doesn't require a result object or ResultInfo, then clear the ResultInfo from
    // this PlanStageSlots if it has one.
    if (!reqs.hasResult() && _data->resultInfoChanges.has_value()) {
        _data->resultInfoChanges.reset();
    }

    auto requiredNames = getRequiredNamesInOrder(reqs);
    auto requiredNameSet = SlotNameSet(requiredNames.begin(), requiredNames.end());

    // If 'saveResultObj' is true, then we add kResult to 'requiredNameSet' so that
    // the kResult slot will not get cleared.
    if (saveResultObj) {
        requiredNameSet.emplace(kResult);
    }

    // Loop over the slot map and remove all slots that are not present in 'requiredNameSet'.
    auto it = _data->slotNameToIdMap.begin();
    while (it != _data->slotNameToIdMap.end()) {
        auto& name = it->first;
        if (requiredNameSet.contains(name)) {
            ++it;
        } else {
            _data->slotNameToIdMap.erase(it++);
        }
    }
}

void PlanStageSlots::mergeResultInfos(StageBuilderState& state,
                                      PlanNodeId nodeId,
                                      std::vector<PlanStageTree>& trees) {
    // Compute the merged ProjectionEffects.
    boost::optional<ProjectionEffects> mergedEffects;

    for (auto&& tree : trees) {
        auto& treeOutputs = tree.second;
        // Assert that each tree produces either a materialized result object or a ResultInfo.
        tassert(8378201,
                "Expected child tree to produce a result object or a ResultInfo",
                treeOutputs.hasResult());
        // If 'tree' has a materialized result object, convert it into a ResultInfo (to make
        // all the trees uniform).
        if (treeOutputs.hasResult()) {
            treeOutputs.setResultInfoBaseObj(treeOutputs.get(kResult));
        }

        const ProjectionEffects& treeEffects = treeOutputs.getResultInfoChanges();
        if (!mergedEffects) {
            mergedEffects.emplace(treeEffects);
        } else {
            *mergedEffects = mergedEffects->merge(treeEffects);
        }
    }

    tassert(8378202,
            "Expected default effect to be Keep or Drop",
            mergedEffects->getDefaultEffect() == ProjectionEffects::kKeep ||
                mergedEffects->getDefaultEffect() == ProjectionEffects::kDrop);

    auto mergedModified = mergedEffects->getModifiedOrCreatedFieldSet();

    // Inspect each 'tree' and populate any slots needed by 'mergedEffects' that are missing.
    for (auto& tree : trees) {
        auto& stage = tree.first;
        auto& treeOutputs = tree.second;

        sbe::SlotExprPairVector projects;

        const ProjectionEffects& treeEffects = treeOutputs.getResultInfoChanges();

        for (auto&& fieldName : mergedModified.getList()) {
            if (!treeOutputs.has(UnownedSlotName(kField, fieldName))) {
                if (treeEffects.isKeep(fieldName)) {
                    auto getFieldExpr =
                        makeFunction("getField"_sd,
                                     makeVariable(treeOutputs.getResultInfoBaseObj()),
                                     makeStrConstant(fieldName));
                    auto slot = state.slotId();

                    projects.emplace_back(std::pair(slot, std::move(getFieldExpr)));
                    treeOutputs.set(std::pair(kField, fieldName), slot);
                } else {
                    tassert(8378203,
                            "Expected field to have Keep effect or Drop effect",
                            treeEffects.isDrop(fieldName));
                    auto nothingSlot = TypedSlot{state.getNothingSlot()};
                    treeOutputs.set(std::pair(kField, fieldName), nothingSlot);
                }
            }
        }

        treeOutputs._data->resultInfoChanges.emplace(*mergedEffects);

        stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);
    }
}

FieldSet PlanStageReqs::getNeededFieldSet() const {
    if (hasResultObj()) {
        return FieldSet::makeUniverseSet();
    } else {
        auto result = FieldSet::makeClosedSet(getFields());
        if (_data->allowedSet) {
            result.setUnion(*_data->allowedSet);
        }
        return result;
    }
}

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
             _yieldPolicy,
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
    auto [node, ct] = solution.getFirstNodeByType(STAGE_COLLSCAN);
    auto [_, orCt] = solution.getFirstNodeByType(STAGE_OR);
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

        bool doClusteredCollectionScanSbe = csn->doClusteredCollectionScanSbe();

        _data->shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
        _data->shouldTrackResumeToken = csn->requestResumeToken;
        _data->shouldUseTailableScan = csn->tailable;
        _data->direction = csn->direction;
        _data->doClusteredCollectionScanSbe = doClusteredCollectionScanSbe;

        if (doClusteredCollectionScanSbe) {
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

void SlotBasedStageBuilder::analyzeTree(const QuerySolutionNode* root) {
    using DfsItem = std::pair<const QuerySolutionNode*, size_t>;

    absl::InlinedVector<DfsItem, 32> dfs;
    dfs.emplace_back(DfsItem(root, 0));

    while (!dfs.empty()) {
        auto& dfsBack = dfs.back();
        auto node = dfsBack.first;

        // Skip if already analyzed this subtree.
        if (_analysis.count(node)) {
            dfs.pop_back();
            continue;
        }

        auto childIdx = dfsBack.second;

        if (childIdx < node->children.size()) {
            dfsBack.second++;
            dfs.emplace_back(DfsItem(node->children[childIdx].get(), 0));
        } else {
            _analysis.emplace(node, analyze(node));
            dfs.pop_back();
        }
    }
}

QsnAnalysis SlotBasedStageBuilder::analyze(const QuerySolutionNode* node) {
    switch (node->getType()) {
        case STAGE_LIMIT:
        case STAGE_SKIP:
        case STAGE_MATCH:
        case STAGE_SHARDING_FILTER:
        case STAGE_SORT_SIMPLE:
        case STAGE_SORT_DEFAULT: {
            // Get the FieldSet produced by this node's child and return it.
            return QsnAnalysis{getAnalysis(node->children[0]).allowedFieldSet};
        }
        case STAGE_OR:
        case STAGE_AND_HASH:
        case STAGE_AND_SORTED:
        case STAGE_SORT_MERGE: {
            // Union the FieldSets produced by all this node's children and return it.
            auto allowedFields = getAnalysis(node->children[0]).allowedFieldSet;
            for (size_t i = 1; i < node->children.size(); ++i) {
                allowedFields.setUnion(getAnalysis(node->children[i]).allowedFieldSet);
            }
            return QsnAnalysis{std::move(allowedFields)};
        }
        case STAGE_IXSCAN: {
            std::vector<std::string> result;
            StringSet resultSet;
            // Loop over the parts of the index's keyPattern and add each top-level field
            // that is referenced to 'result', and then return 'result'.
            auto ixn = static_cast<const IndexScanNode*>(node);
            BSONObjIterator it(ixn->index.keyPattern);
            while (it.more()) {
                auto f = getTopLevelField(it.next().fieldNameStringData());
                if (!resultSet.count(f)) {
                    auto str = f.toString();
                    resultSet.emplace(str);
                    result.emplace_back(std::move(str));
                }
            }
            return QsnAnalysis{FieldSet::makeClosedSet(std::move(result))};
        }
        case STAGE_GROUP: {
            std::vector<std::string> result;
            result.emplace_back("_id");
            // Loop over thel fields produced by the GroupNode, add them to 'result', and then
            // return 'result'.
            auto groupNode = static_cast<const GroupNode*>(node);
            auto& accStmts = groupNode->accumulators;
            for (size_t i = 0; i < accStmts.size(); ++i) {
                result.emplace_back(accStmts[i].fieldName);
            }
            return QsnAnalysis{FieldSet::makeClosedSet(std::move(result))};
        }
        case STAGE_PROJECTION_DEFAULT:
        case STAGE_PROJECTION_COVERED:
        case STAGE_PROJECTION_SIMPLE: {
            auto pn = static_cast<const ProjectionNode*>(node);
            auto [paths, nodes] = getProjectNodes(pn->proj);
            bool isInclusion = pn->proj.type() == projection_ast::ProjectType::kInclusion;

            // Get the FieldSet produced by this node's child, update it with the effects of
            // this projection, and return it.
            auto allowedFields = getAnalysis(node->children[0]).allowedFieldSet;
            allowedFields.setIntersect(makeAllowedFieldSet(isInclusion, paths, nodes));
            allowedFields.setUnion(makeCreatedFieldSet(isInclusion, paths, nodes));

            return QsnAnalysis{std::move(allowedFields)};
        }
        default: {
            return {};
        }
    }
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildTree() {
    const bool needsRecordIdSlot = _data->shouldUseTailableScan || _data->shouldTrackResumeToken ||
        _cq.getForceGenerateRecordId();

    // We always produce a 'resultSlot'.
    PlanStageReqs reqs;
    reqs.setResultObj();

    // We force the root stage to produce a 'recordId' if the iteration can be resumed (via a resume
    // token or a tailable cursor) or if the caller simply expects to be able to read it.
    reqs.setIf(kRecordId, needsRecordIdSlot);

    // Set the target namespace to '_mainNss'. This is necessary as some QuerySolutionNodes that
    // require a collection when stage building do not explicitly name which collection they are
    // targeting.
    reqs.setTargetNamespace(_mainNss);

    // Build the SBE plan stage tree and return it.
    return build(_root, reqs);
}

SlotBasedStageBuilder::PlanType SlotBasedStageBuilder::build(const QuerySolutionNode* root) {
    // For a given SlotBasedStageBuilder instance, this build() method can only be called once.
    invariant(!_buildHasStarted);
    _buildHasStarted = true;

    _root = root;
    ON_BLOCK_EXIT([&] { _root = nullptr; });

    auto [stage, outputs] = buildTree();

    // Assert that we produced a 'resultSlot' and that we produced a 'recordIdSlot' if it was
    // needed.
    invariant(outputs.hasResultObj());

    const bool needsRecordIdSlot = _data->shouldUseTailableScan || _data->shouldTrackResumeToken ||
        _cq.getForceGenerateRecordId();
    if (needsRecordIdSlot) {
        invariant(outputs.has(kRecordId));
    }

    _data->resultSlot = outputs.getResultObjSlotIfExists();
    _data->recordIdSlot = outputs.getSlotIfExists(stage_builder::PlanStageSlots::kRecordId);

    return {std::move(stage), PlanStageData(std::move(_env), std::move(_data))};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildCollScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023400, "buildCollScan() does not support kSortKey", !reqs.hasSortKeys());

    auto csn = static_cast<const CollectionScanNode*>(root);
    auto fields = reqs.getFields();

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
    auto [scanSlots, stage] = generateVirtualScanMulti(
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

    if (reqs.hasResult() || reqs.hasFields()) {
        outputs.setResultObj(resultSlot);
    }
    if (reqs.has(kRecordId)) {
        invariant(vsn->hasRecordId);
        invariant(scanSlots.size() == 2);
        outputs.set(kRecordId, scanSlots[0]);
    }

    if (vsn->filter) {
        auto filterExpr = generateFilter(_state,
                                         vsn->filter.get(),
                                         TypedSlot{resultSlot, TypeSignature::kAnyScalarType},
                                         outputs);
        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(_state), vsn->nodeId());
        }
    }

    return {std::move(stage), std::move(outputs)};
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

    bool reqResult = reqs.hasResult() || !additionalFields.empty();

    if (reqs.has(kReturnKey) || reqResult) {
        // If 'reqs' requires result or kReturnKey, or if 'additionalFields' is not empty, then we
        // need to get all parts of the index key so that we can create the inflated index key.
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
                makeVariable(outputs.get(std::make_pair(PlanStageSlots::kField, name))));
        }

        auto rawKeyExpr = sbe::makeE<sbe::EFunction>("newObj"_sd, std::move(args));
        outputs.set(PlanStageSlots::kReturnKey,
                    TypedSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType});
        stage = sbe::makeProjectStage(std::move(stage),
                                      ixn->nodeId(),
                                      outputs.get(PlanStageSlots::kReturnKey).slotId,
                                      std::move(rawKeyExpr));
    }

    if (reqResult) {
        auto indexKeySlots = sbe::makeSV();
        for (auto&& elem : ixn->index.keyPattern) {
            StringData name = elem.fieldNameStringData();
            indexKeySlots.emplace_back(
                outputs.get(std::make_pair(PlanStageSlots::kField, name)).slotId);
        }

        auto resultSlot = _slotIdGenerator.generate();
        outputs.setResultObj(resultSlot);

        stage = rehydrateIndexKey(
            std::move(stage), ixn->index.keyPattern, ixn->nodeId(), indexKeySlots, resultSlot);
    }

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

    if (reqs.hasResult() || reqs.hasFields()) {
        // COUNT_SCAN stage doesn't produce any output, make an empty object for the result.
        auto resultSlot = _slotIdGenerator.generate();
        planStageSlots.setResultObj(TypedSlot{resultSlot, TypeSignature::kObjectType});
        stage = sbe::makeProjectStage(
            std::move(stage), csn->nodeId(), resultSlot, makeFunction("newObj"));
    }

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
    outputs.setResultObj(reconstructedRecordSlot);

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
                    .extractExpr(_state),
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
        rowStoreExpr = abt ? SbExpr{abt::wrap(std::move(*abt))}.extractExpr(_state)
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
        auto filterExpr = generateFilter(
            _state, csn->postAssemblyFilter.get(), TypedSlot{reconstructedRecordSlot}, outputs);

        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(_state), csn->nodeId());
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

    auto forwardingReqs = reqs.copyForChild()
                              .clearResult()
                              .clear(kRecordId)
                              .clearAllFields()
                              .clearAllSortKeys()
                              .setSortKeys(std::move(sortKeys));

    auto childReqs = forwardingReqs.copyForChild()
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

    sortKeys = std::move(additionalSortKeys);

    auto fields =
        appendVectorUnique(getTopLevelFields(reqs.getFields()), getTopLevelFields(sortKeys));

    if (fn->filter) {
        DepsTracker deps;
        match_expression::addDependencies(fn->filter.get(), &deps);
        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            fields = appendVectorUnique(std::move(fields), getTopLevelFields(deps.fields));
        }
    }

    auto resultSlot = TypedSlot{_slotIdGenerator.generate()};
    auto ridSlot = _slotIdGenerator.generate();
    auto fieldSlots = _slotIdGenerator.generateMultiple(fields.size());

    auto relevantSlots = getSlotsToForward(_state, forwardingReqs, outputs);

    stage = makeLoopJoinForFetch(std::move(stage),
                                 resultSlot,
                                 ridSlot,
                                 fields,
                                 fieldSlots,
                                 outputs.get(kRecordId),
                                 outputs.get(kSnapshotId),
                                 outputs.get(kIndexIdent),
                                 outputs.get(kIndexKey),
                                 outputs.get(kIndexKeyPattern),
                                 getCurrentCollection(reqs),
                                 root->nodeId(),
                                 std::move(relevantSlots));

    outputs.setResultObj(resultSlot);

    // Only propagate kRecordId if requested.
    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, ridSlot);
    } else {
        outputs.clear(kRecordId);
    }

    for (size_t i = 0; i < fields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, fields[i]), fieldSlots[i]);
    }

    if (fn->filter) {
        auto filterExpr = generateFilter(_state, fn->filter.get(), resultSlot, outputs);
        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(_state), root->nodeId());
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

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLimit(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto ln = static_cast<const LimitNode*>(root);
    std::unique_ptr<sbe::EExpression> skip;

    auto childReqs = reqs.copyForChild();
    childReqs.setHasLimit(true);

    auto [stage, outputs] = [&]() {
        if (ln->children[0]->getType() == StageType::STAGE_SKIP) {
            // If we have both limit and skip stages and the skip stage is beneath the limit, then
            // we can combine these two stages into one.
            const auto sn = static_cast<const SkipNode*>(ln->children[0].get());
            skip = buildLimitSkipAmountExpression(
                sn->canBeParameterized, sn->skip, _data->limitSkipSlots.skip);
            return build(sn->children[0].get(), childReqs);
        } else {
            return build(ln->children[0].get(), childReqs);
        }
    }();

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage),
            buildLimitSkipAmountExpression(
                ln->canBeParameterized, ln->limit, _data->limitSkipSlots.limit),
            std::move(skip),
            root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSkip(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SkipNode*>(root);
    auto [stage, outputs] = build(sn->children[0].get(), reqs);

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage),
            nullptr,
            buildLimitSkipAmountExpression(
                sn->canBeParameterized, sn->skip, _data->limitSkipSlots.skip),
            root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::unique_ptr<sbe::EExpression> SlotBasedStageBuilder::buildLimitSkipAmountExpression(
    LimitSkipParameterization canBeParameterized,
    long long amount,
    boost::optional<sbe::value::SlotId>& slot) {
    if (canBeParameterized == LimitSkipParameterization::Disabled) {
        return makeConstant(sbe::value::TypeTags::NumberInt64,
                            sbe::value::bitcastFrom<long long>(amount));
    }

    if (!slot) {
        slot = _env.runtimeEnv->registerSlot(sbe::value::TypeTags::NumberInt64,
                                             sbe::value::bitcastFrom<long long>(amount),
                                             false,
                                             &_slotIdGenerator);
    } else {
        const auto& slotAmount = _env.runtimeEnv->getAccessor(*slot)->getViewOfValue();
        tassert(8349204,
                str::stream() << "Inconsistent value in limit or skip slot " << *slot
                              << ". Value in slot: " << sbe::value::print(slotAmount)
                              << ". Incoming value: " << amount,
                slotAmount.first == sbe::value::TypeTags::NumberInt64 &&
                    sbe::value::bitcastTo<long long>(slotAmount.second) == amount);
    }
    return makeVariable(*slot);
}

std::unique_ptr<sbe::EExpression> SlotBasedStageBuilder::buildLimitSkipSumExpression(
    LimitSkipParameterization canBeParameterized, size_t limitSkipSum) {
    if (canBeParameterized == LimitSkipParameterization::Disabled) {
        // SBE doesn't have unsigned 64-bit integers, so we cap the limit at
        // std::numeric_limits<int64_t>::max() to handle the pathological edge case where the
        // unsigned value is larger than than the maximum possible signed value.
        return makeInt64Constant(
            std::min(limitSkipSum, static_cast<size_t>(std::numeric_limits<int64_t>::max())));
    }

    boost::optional<int64_t> limit = _cq.getFindCommandRequest().getLimit();
    boost::optional<int64_t> skip = _cq.getFindCommandRequest().getSkip();
    tassert(8349207, "expected limit to be present", limit);
    size_t sum = static_cast<size_t>(*limit) + static_cast<size_t>(skip.value_or(0));
    tassert(8349208,
            str::stream() << "expected sum of find command request limit and skip parameters to be "
                             "equal to the provided value. Limit: "
                          << limit << ", skip: " << skip << ", sum: " << sum
                          << ", provided value: " << limitSkipSum,
            sum == limitSkipSum);
    if (!skip) {
        return buildLimitSkipAmountExpression(
            canBeParameterized, *limit, _data->limitSkipSlots.limit);
    }

    auto sumExpr = makeBinaryOp(
        sbe::EPrimBinary::add,
        buildLimitSkipAmountExpression(canBeParameterized, *limit, _data->limitSkipSlots.limit),
        buildLimitSkipAmountExpression(canBeParameterized, *skip, _data->limitSkipSlots.skip));

    // SBE promotes to double on int64 overflow. We need to return int64 max value in that case,
    // since the SBE sort stage expects the limit to always be a 64-bit integer.
    return makeLocalBind(
        &_frameIdGenerator,
        [](sbe::EVariable sum) {
            return sbe::makeE<sbe::EIf>(
                makeFunction(
                    "typeMatch",
                    sum.clone(),
                    makeInt32Constant(MatcherTypeSet{BSONType::NumberLong}.getBSONTypeMask())),
                sum.clone(),
                makeInt64Constant(std::numeric_limits<int64_t>::max()));
        },
        std::move(sumExpr));
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
        auto fieldExpr = fieldSlot ? SbExpr{*fieldSlot}
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
    boost::optional<TypedSlot> fieldSlot = boost::none) {
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

    if (auto [ixn, ct] = root->getFirstNodeByType(STAGE_IXSCAN);
        !sn->fetched() && !reqs.hasResult() && ixn && ct >= 1) {
        return buildSortCovered(root, reqs);
    }

    // getExecutor() should never call into buildSlotBasedExecutableTree() when the query
    // contains $meta, so this assertion should always be true.
    for (const auto& part : sortPattern) {
        tassert(5037002, "Sort with $meta is not supported in SBE", part.fieldPath);
    }

    const bool hasPartsWithCommonPrefix = sortPatternHasPartsWithCommonPrefix(sortPattern);
    std::vector<std::string> fields;

    if (!hasPartsWithCommonPrefix) {
        DepsTracker deps;
        sortPattern.addDependencies(&deps);
        // If the sort pattern doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            fields = getTopLevelFields(deps.fields);
        }
    }

    auto forwardingReqs = reqs.copyForChild();
    if (reqs.getHasLimit()) {
        // When sort is followed by a limit the overhead of tracking the kField slots during
        // sorting is greater compared to the overhead of retrieving the necessary kFields from
        // the materialized result object after the sorting is done.
        forwardingReqs.clearAllFields().setResultObj();
    }

    if (hasPartsWithCommonPrefix) {
        forwardingReqs.setResultObj();
    }

    auto childReqs = forwardingReqs.copyForChild().setFields(fields);

    auto [stage, childOutputs] = build(child, childReqs);
    auto outputs = std::move(childOutputs);

    auto collatorSlot = _state.getCollatorSlot();

    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;

    if (!hasPartsWithCommonPrefix) {
        // Handle the case where we are using a materialized result object and there are no common
        // prefixes.
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
                                          failOnParallelArrays.extractExpr(_state));
        }

        sbe::SlotExprPairVector sortExpressions;

        for (const auto& part : sortPattern) {
            auto topLevelFieldSlot = outputs.get(
                std::make_pair(PlanStageSlots::kField, part.fieldPath->getFieldName(0)));

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

        const TypedSlot childResultSlotId = outputs.getResultObj();

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
    auto forwardedSlots = getSlotsToForward(_state, forwardingReqs, outputs);

    stage = sbe::makeS<sbe::SortStage>(
        std::move(stage),
        std::move(orderBy),
        std::move(direction),
        std::move(forwardedSlots),
        sn->limit ? buildLimitSkipSumExpression(sn->canBeParameterized, sn->limit) : nullptr,
        sn->maxMemoryUsageBytes,
        _cq.getExpCtx()->allowDiskUse,
        _yieldPolicy,
        root->nodeId());

    if (reqs.getHasLimit()) {
        // Project the fields that the parent requested using the result object.
        auto resultSlot = outputs.getResultObjIfExists();
        tassert(8312200, "Result object should be set", resultSlot);
        auto fields = reqs.getFields();
        // Clear from outputs everything that is not found in forwardingReqs and project from
        // result object every required field not already in outputs.
        outputs.clearNonRequiredSlots(forwardingReqs);
        auto [outStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                         fields,
                                                         *resultSlot,
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

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSortCovered(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(6023404,
            "buildSortCovered() does not support 'reqs' with a result requirement",
            !reqs.hasResult());

    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    tassert(7047600,
            "QueryPlannerAnalysis should not produce a SortNode with an empty sort pattern",
            sortPattern.size() > 0);
    tassert(6023422, "buildSortCovered() expected 'sn' to not be fetched", !sn->fetched());

    auto child = sn->children[0].get();

    // The child must produce all of the slots required by the parent of this SortNode.
    auto childReqs = reqs.copyForChild();

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
    auto forwardedSlots = getSlotsToForward(_state, childReqs, outputs, orderBy);

    stage = sbe::makeS<sbe::SortStage>(
        std::move(stage),
        std::move(orderBy),
        std::move(direction),
        std::move(forwardedSlots),
        sn->limit ? buildLimitSkipSumExpression(sn->canBeParameterized, sn->limit) : nullptr,
        sn->maxMemoryUsageBytes,
        _cq.getExpCtx()->allowDiskUse,
        _yieldPolicy,
        root->nodeId());

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

    std::vector<sbe::value::SlotVector> inputKeys;

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
        reqs.copyForChild().setIf(kRecordId, mergeSortNode->dedup).setSortKeys(std::move(sortKeys));

    std::vector<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>> inputStagesAndSlots;

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

        inputStagesAndSlots.emplace_back(std::pair(std::move(stage), std::move(outputs)));
    }

    auto outputs = PlanStageSlots::makeMergedPlanStageSlots(
        _state, root->nodeId(), childReqs, inputStagesAndSlots);

    auto outputVals = getSlotsOrderedByName(childReqs, outputs);

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputVals;
    for (auto& p : inputStagesAndSlots) {
        auto& stage = p.first;
        auto& outputs = p.second;
        inputStages.push_back(std::move(stage));
        inputVals.push_back(getSlotsOrderedByName(childReqs, outputs));
    }

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

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildMatch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const MatchNode* mn = static_cast<const MatchNode*>(root);

    bool needChildResultDoc = false;

    std::vector<std::string> fields;
    if (mn->filter) {
        DepsTracker filterDeps;
        match_expression::addDependencies(mn->filter.get(), &filterDeps);

        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!filterDeps.needWholeDocument) {
            fields = getTopLevelFields(filterDeps.fields);
        }

        needChildResultDoc = filterDeps.needWholeDocument;
    }

    // The child must produce all of the slots required by the parent of this MatchNode.
    PlanStageReqs childReqs = reqs.copyForChild().setFields(std::move(fields));

    // If the filter needs the whole document, the child must produce a materialized result object
    // as well.
    if (needChildResultDoc) {
        childReqs.setResultObj();
    }

    // Indicate we can work on block values, if we are not requested to produce a result object.
    childReqs.setCanProcessBlockValues(!childReqs.hasResult());

    auto [stage, outputs] = build(mn->children[0].get(), childReqs);

    if (mn->filter) {
        auto childResultSlot =
            needChildResultDoc ? boost::make_optional(outputs.getResultObj()) : boost::none;

        SbExpr filterExpr = generateFilter(_state, mn->filter.get(), childResultSlot, outputs);

        if (!filterExpr.isNull()) {
            // Try to vectorize if the stage received blocked input from children.
            if (outputs.hasBlockOutput()) {
                auto [newStage, isVectorised] = buildVectorizedFilterExpr(
                    std::move(stage), reqs, std::move(filterExpr), outputs, root->nodeId());

                stage = std::move(newStage);

                if (!isVectorised) {
                    // The last step was to convert the block to row. Generate the filter expression
                    // again to use the scalar slots instead of the block slots.
                    SbExpr filterScalarExpr =
                        generateFilter(_state, mn->filter.get(), childResultSlot, outputs);
                    VariableTypes varTypes = buildVariableTypes(outputs);
                    stage = sbe::makeS<sbe::FilterStage<false>>(
                        std::move(stage),
                        filterScalarExpr.extractExpr(_state, &varTypes),
                        root->nodeId());
                }
            } else {
                // Did not receive block input. Continue the scalar execution.
                VariableTypes varTypes = buildVariableTypes(outputs);
                stage = sbe::makeS<sbe::FilterStage<false>>(
                    std::move(stage), filterExpr.extractExpr(_state, &varTypes), root->nodeId());
            }
        }
    }

    // Ensure that we are not forwarding block values that the caller cannot handle.
    if (outputs.hasBlockOutput() && (reqs.hasResult() || !reqs.getCanProcessBlockValues())) {
        stage = buildBlockToRow(std::move(stage), _state, outputs);
    }

    return {std::move(stage), std::move(outputs)};
}

/**
 * Builds the execution stage for an $unwind aggregation stage that has been pushed down to SBE.
 * This also builds a child project stage to get the field to be unwound, and ancestor project
 * stage(s) to add the $unwind outputs (value and optionally array index) to the result document.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildUnwind(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const UnwindNode* un = static_cast<const UnwindNode*>(root);
    const FieldPath& fp = un->fieldPath;

    //
    // Build the execution subtree for the child plan subtree.
    //

    // The child must produce all of the slots required by the parent of this UnwindNode, plus this
    // node needs to produce the result slot.
    PlanStageReqs childReqs = reqs.copyForChild().setResultObj();
    auto [stage, outputs] = build(un->children[0].get(), childReqs);

    // Clear the root of the original field being unwound so later plan stages do not reference it.
    outputs.clearField(fp.getSubpath(0));
    const sbe::value::SlotId childResultSlot = outputs.getResultObj().slotId;

    //
    // Build a project execution child node to get the field to be unwound. This gets the value of
    // the field at the end of the full FieldPath out of the doc produced by the child and puts it
    // into 'getFieldSlot'. projectFieldsToSlots() is used instead of makeProjectStage() because
    // only the former supports dotted paths.
    //
    std::vector<std::string> fields;
    fields.emplace_back(fp.fullPath());
    auto [outStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                     fields,
                                                     childResultSlot,
                                                     root->nodeId(),
                                                     &_slotIdGenerator,
                                                     _state,
                                                     &outputs);
    stage = std::move(outStage);
    sbe::value::SlotId getFieldSlot = outSlots[0].getId();

    // Continue building the unwind and projection to results.
    return buildOnlyUnwind(un, reqs, stage, outputs, childResultSlot, getFieldSlot);
}  // buildUnwind

/**
 * Builds only the unwind and project results part of an $unwind stage, allowing an $LU stage to
 * invoke just these parts of building its absorbed $unwind. For stand-alone $unwind this method is
 * conceptually just the "bottom two thirds" of buildUnwind(). Used for the special case of a
 * nonexistent foreign collection, where the $lookup result array is empty and thus its
 * materialization is not a performance or memory problem.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildOnlyUnwind(
    const UnwindNode* un,
    const PlanStageReqs& reqs,
    std::unique_ptr<sbe::PlanStage>& stage,
    PlanStageSlots& outputs,
    const sbe::value::SlotId childResultSlot,
    const sbe::value::SlotId getFieldSlot) {
    const FieldPath& fp = un->fieldPath;

    //
    // Build the unwind execution node itself. This will unwind the value in 'getFieldSlot' into
    // 'unwindSlot' and place the array index value into 'arrayIndexSlot'.
    //
    sbe::value::SlotId unwindSlot = _slotIdGenerator.generate();
    sbe::value::SlotId arrayIndexSlot = _slotIdGenerator.generate();
    stage = sbe::makeS<sbe::UnwindStage>(std::move(stage),
                                         getFieldSlot /* inField */,
                                         unwindSlot /* outField */,
                                         arrayIndexSlot /* outIndex */,
                                         un->preserveNullAndEmptyArrays,
                                         un->nodeId(),
                                         nullptr /* yieldPolicy */,
                                         true /* participateInTrialRunTracking */);

    //
    // Build project parent node to add the unwind and/or index outputs to the result doc. Since
    // docs are immutable in SBE, doing this the simpler way via separate ProjectStages for each
    // output leads to an extra result doc copy if both unwind and index get projected. To avoid
    // this, we build a single ProjectStage that handles all possible combinations of needed
    // projections. This is simplified slightly by the fact that we know whether the index output
    // was requested or not, so we can wire only the relevant combinations.
    //

    // Paths in result document to project to.
    std::vector<std::string> fieldPaths;

    // Variables whose values are to be projected into the result document.
    std::vector<ProjectNode> projectionNodes;

    // The projection expression that adds the index and/or unwind values to the result doc.
    std::unique_ptr<sbe::EExpression> finalProjectExpr;

    if (un->indexPath) {
        // "includeArrayIndex" option (Cases 1-3). The index is always projected in these.

        // If our parent wants the array index field, set our outputs to point it to that slot.
        if (reqs.has({PlanStageSlots::SlotType::kField, un->indexPath->fullPath()})) {
            outputs.set(PlanStageSlots::OwnedSlotName{PlanStageSlots::SlotType::kField,
                                                      un->indexPath->fullPath()},
                        arrayIndexSlot);
        }

        // Case 1: index Null, unwind val //////////////////////////////////////////////////////////
        // We need two copies of the Case 1 expression as it is used twice, but the copy constructor
        // is deleted so we are forced to std::move it.
        SbExpr indexNullUnwindValProjExpr[2];

        for (int copy = 0; copy < 2; ++copy) {
            fieldPaths.clear();
            projectionNodes.clear();

            // Index output
            fieldPaths.emplace_back(un->indexPath->fullPath());
            projectionNodes.emplace_back(SbExpr{makeConstant(sbe::value::TypeTags::Null, 0)});

            // Unwind output
            fieldPaths.emplace_back(fp.fullPath());
            projectionNodes.emplace_back(SbExpr{unwindSlot});

            indexNullUnwindValProjExpr[copy] = generateProjection(
                _state,
                projection_ast::ProjectType::kAddition,
                std::move(fieldPaths),
                std::move(projectionNodes),
                SbExpr{childResultSlot});  // current result doc: updated by the projection
        }

        // Case 2: index val, unwind val ///////////////////////////////////////////////////////////
        fieldPaths.clear();
        projectionNodes.clear();

        // Index output
        fieldPaths.emplace_back(un->indexPath->fullPath());
        projectionNodes.emplace_back(SbExpr{arrayIndexSlot});

        // Unwind output
        fieldPaths.emplace_back(fp.fullPath());
        projectionNodes.emplace_back(SbExpr{unwindSlot});

        SbExpr indexValUnwindValProjExpr = generateProjection(
            _state,
            projection_ast::ProjectType::kAddition,
            std::move(fieldPaths),
            std::move(projectionNodes),
            SbExpr{childResultSlot});  // current result doc: updated by the projection

        // Case 3: index Null //////////////////////////////////////////////////////////////////////
        fieldPaths.clear();
        projectionNodes.clear();

        // Index output
        fieldPaths.emplace_back(un->indexPath->fullPath());
        projectionNodes.emplace_back(SbExpr{makeConstant(sbe::value::TypeTags::Null, 0)});

        SbExpr indexNullProjExpr = generateProjection(
            _state,
            projection_ast::ProjectType::kAddition,
            std::move(fieldPaths),
            std::move(projectionNodes),
            SbExpr{childResultSlot});  // current result document: updated by the projection

        // Wrap the above projection subexpressions in conditionals that correctly handle quirky MQL
        // edge cases:
        //   if isNull(index)
        //      then if exists(unwind)
        //              then project {Null, unwind}
        //              else project {Null,       }
        //      else if index >= 0
        //              then project {index, unwind}
        //              else project {Null,  unwind}
        SbExprBuilder bldI{_state};
        finalProjectExpr =
            /* outer if */ bldI
                .makeIf(bldI.makeFunction("isNull", bldI.makeVariable(arrayIndexSlot)),
                        /* outer then */
                        bldI.makeIf(
                            /* inner1 if */ bldI.makeFunction(
                                "exists", sbe::makeE<sbe::EVariable>(unwindSlot)),
                            /* inner1 then */ std::move(indexNullUnwindValProjExpr[0]),
                            /* inner1 else */ std::move(indexNullProjExpr)),
                        /* outer else */
                        bldI.makeIf(
                            /* inner2 if */ bldI.makeBinaryOp(
                                sbe::EPrimBinary::greaterEq,
                                bldI.makeVariable(arrayIndexSlot),
                                makeConstant(sbe::value::TypeTags::NumberInt64, 0)),
                            /* inner2 then */ std::move(indexValUnwindValProjExpr),
                            /* inner2 else */ std::move(indexNullUnwindValProjExpr[1])))
                .extractExpr(_state);
    } else {
        // No "includeArrayIndex" option (Cases 4-5). The index is never projected in these.

        // Case 4: unwind val //////////////////////////////////////////////////////////////////////
        fieldPaths.clear();
        projectionNodes.clear();

        // Unwind output
        fieldPaths.emplace_back(fp.fullPath());
        projectionNodes.emplace_back(SbExpr{unwindSlot});

        SbExpr unwindValProjExpr = generateProjection(
            _state,
            projection_ast::ProjectType::kAddition,
            std::move(fieldPaths),
            std::move(projectionNodes),
            SbExpr{childResultSlot});  // current result document: updated by the projection

        // Case 5: NO-OP - original doc ////////////////////////////////////////////////////////////
        // Does not need a generateProjection() call as it will be handled in the wrapper logic.

        // Wrap 'unwindValProjExpr' in a conditional that correctly handles the quirky MQL edge
        // cases. If the unwind field was not an array (indicated by 'arrayIndexSlot' containing
        // Null), we avoid projecting its value to the result, as if it is Nothing (instead of a
        // singleton) this would incorrectly create the dotted path above that value in the result
        // document. We don't need to project in the singleton case either as the result doc already
        // has that singleton at the unwind field location.
        SbExprBuilder bldU{_state};
        finalProjectExpr =
            bldU.makeIf(
                    /* if */ bldU.makeFunction("isNull", bldU.makeVariable(arrayIndexSlot)),
                    /* then no-op */ bldU.makeVariable(childResultSlot),
                    /* else project */ std::move(unwindValProjExpr))
                .extractExpr(_state);
    }  // else no "includeArrayIndex"

    // Create the ProjectStage that adds the output(s) to the result doc via 'finalProjectExpr'.
    TypedSlot resultSlot = TypedSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType};
    stage = makeProjectStage(std::move(stage),
                             un->nodeId(),
                             resultSlot.slotId,  // output result document
                             std::move(finalProjectExpr));

    outputs.setResultObj(resultSlot);
    return {std::move(stage), std::move(outputs)};
}  // buildOnlyUnwind

/**
 * Create a ProjectStage that evalutes the "newRoot" expression from a $replaceRoot pipeline stage
 * and append it to the root of the SBE plan.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildReplaceRoot(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    SbBuilder b(_state, root->nodeId());

    const ReplaceRootNode* rrn = static_cast<const ReplaceRootNode*>(root);

    DepsTracker newRootDeps;
    expression::addDependencies(rrn->newRoot.get(), &newRootDeps);

    // The $replaceRoot operation only ever needs a materialized result object if there are
    // operations in the 'newRoot' expression that need the whole document.
    PlanStageReqs childReqs = newRootDeps.needWholeDocument
        ? reqs.copyForChild().clearAllFields().setResultObj()
        : reqs.copyForChild().clearAllFields().clearResult().setFields(
              getTopLevelFields(newRootDeps.fields));

    auto [stage, outputs] = build(rrn->children[0].get(), childReqs);

    // MQL semantics require $replaceRoot to fail if newRoot expression does not evaluate to an
    // object. We fill empty results with null and wrap the generated expression in an if statement
    // that fails if it does not evaluate to an object.
    auto frameId = _state.frameId();
    auto newRootVar = SbVar{frameId, 0};

    auto newRootABT =
        generateExpression(_state, rrn->newRoot.get(), outputs.getResultObjIfExists(), outputs);

    auto validatedNewRootExpr = b.makeLet(
        frameId,
        SbExpr::makeSeq(b.makeFillEmptyNull(std::move(newRootABT))),
        b.makeIf(
            b.generateNonObjectCheck(newRootVar),
            b.makeFail(ErrorCodes::Error{8105800},
                       "Expression in $replaceRoot/$replaceWith must evaluate to an object"_sd),
            newRootVar));

    auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(validatedNewRootExpr));
    stage = std::move(outStage);

    auto resultSlot = outSlots[0];

    // We just generated a check that will throw at run time if 'resultSlot' is set to Nothing
    // or to a non-object value. Therefore, if we reach this point in SBE tree, we know that
    // 'resultSlot' must be an object. Set the type signature on 'resultSlot' accordingly.
    resultSlot.setTypeSignature(TypeSignature::kObjectType);

    outputs.setResultObj(resultSlot);

    // The result object has changed, so clear all field slots.
    outputs.clearAllFields();

    return {std::move(stage), std::move(outputs)};
}  // buildReplaceRoot

namespace {
std::pair<std::vector<const QuerySolutionNode*>, bool> getUnfetchedIxscans(
    const QuerySolutionNode* root) {
    using DfsItem = std::pair<const QuerySolutionNode*, size_t>;

    absl::InlinedVector<DfsItem, 64> dfs;
    std::vector<const QuerySolutionNode*> results;
    bool hasFetchesOrScans = false;

    for (auto&& child : root->children) {
        dfs.emplace_back(DfsItem(child.get(), 0));
    }

    while (!dfs.empty()) {
        auto& dfsBack = dfs.back();
        auto node = dfsBack.first;
        auto childIdx = dfsBack.second;
        bool popDfs = true;

        auto visitNextChild = [&] {
            if (childIdx < node->children.size()) {
                popDfs = false;
                dfsBack.second++;
                dfs.emplace_back(DfsItem(node->children[childIdx].get(), 0));
                return true;
            }
            return false;
        };

        switch (node->getType()) {
            case STAGE_IXSCAN: {
                results.push_back(node);
                break;
            }
            case STAGE_LIMIT:
            case STAGE_SKIP:
            case STAGE_MATCH:
            case STAGE_SHARDING_FILTER:
            case STAGE_SORT_SIMPLE:
            case STAGE_SORT_DEFAULT:
            case STAGE_OR:
            case STAGE_AND_HASH:
            case STAGE_AND_SORTED:
            case STAGE_SORT_MERGE: {
                visitNextChild();
                break;
            }
            case STAGE_COLLSCAN:
            case STAGE_VIRTUAL_SCAN:
            case STAGE_COLUMN_SCAN:
            case STAGE_FETCH: {
                hasFetchesOrScans = true;
                break;
            }
            default: {
                return {{}, false};
            }
        }

        if (popDfs) {
            dfs.pop_back();
        }
    }

    return {std::move(results), hasFetchesOrScans};
}

template <typename SetT>
bool prefixIsInSet(StringData str, const SetT& s) {
    for (;;) {
        if (s.count(str)) {
            return true;
        }

        size_t pos = str.rfind('.');
        if (pos == std::string::npos) {
            break;
        }

        str = str.substr(0, pos);
    }

    return false;
};

void addPrefixesToSet(StringData str, StringDataSet& s) {
    for (;;) {
        auto [_, inserted] = s.insert(str);
        if (!inserted) {
            break;
        }

        size_t pos = str.rfind('.');
        if (pos == std::string::npos) {
            break;
        }

        str = str.substr(0, pos);
    }
};

void addPrefixesToSet(StringData str, StringSet& s) {
    for (;;) {
        auto [_, inserted] = s.insert(str.toString());
        if (!inserted) {
            break;
        }

        size_t pos = str.rfind('.');
        if (pos == std::string::npos) {
            break;
        }

        str = str.substr(0, pos);
    }
};

using NothingPassthruUpdatedAndResultPaths = std::tuple<std::vector<std::string>,
                                                        std::vector<std::string>,
                                                        std::vector<std::string>,
                                                        std::vector<std::string>>;

NothingPassthruUpdatedAndResultPaths mapRequiredFieldsToProjectionOutputs(
    const std::vector<std::string>& reqFields,
    bool isInclusion,
    const FieldSet& childAllowedFields,
    const std::vector<std::string>& paths,
    const std::vector<ProjectNode>& nodes,
    const std::vector<std::string>& resultInfoDrops,
    const std::vector<std::string>& resultInfoModifys) {
    // Fast path for when 'reqFields', 'resultInfoDrops', and 'resultInfoModifys' are all empty.
    if (reqFields.empty() && resultInfoDrops.empty() && resultInfoModifys.empty()) {
        return {};
    }

    // Scan the ProjectNodes and build various path sets.
    StringDataSet keepDropPathSet;
    StringDataSet modifiedOrCreatedPathSet;
    StringDataSet createdPathSet;

    StringDataSet pathPrefixSet;
    StringDataSet createdPathPrefixSet;

    for (size_t i = 0; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        auto& path = paths[i];

        if (node.isBool()) {
            keepDropPathSet.insert(path);
            addPrefixesToSet(path, pathPrefixSet);
        } else if (node.isExpr()) {
            modifiedOrCreatedPathSet.insert(path);
            addPrefixesToSet(path, pathPrefixSet);

            createdPathSet.insert(path);
            addPrefixesToSet(path, createdPathPrefixSet);
        } else if (node.isSlice()) {
            modifiedOrCreatedPathSet.insert(path);
            addPrefixesToSet(path, pathPrefixSet);
        }
    }

    // Scan the required fields and determine post-projection which paths will be Nothing
    // ('nothingPaths'), which paths will be unchanged ('passthruPaths'), which paths are
    // being assigned exprs ('updatedPaths'), and which paths will need to be read from
    // the result object ('resultPaths').
    std::vector<std::string> nothingPaths;
    std::vector<std::string> passthruPaths;
    std::vector<std::string> updatedPaths;
    std::vector<std::string> resultPaths;

    for (const auto& path : reqFields) {
        bool pathAllowed = childAllowedFields.count(getTopLevelField(path));

        bool matchesKeepOrDrop = pathAllowed && prefixIsInSet(path, keepDropPathSet);
        bool matchesKeep = matchesKeepOrDrop & isInclusion;
        bool matchesDrop = matchesKeepOrDrop & !isInclusion;

        if (matchesKeep) {
            passthruPaths.emplace_back(path);
        } else if (matchesDrop) {
            nothingPaths.emplace_back(path);
        } else if (createdPathSet.count(path)) {
            updatedPaths.emplace_back(path);
        } else {
            // We already checked 'keepDropPathSet' above. Here we check 'modifiedOrCreatedPathSet'
            // and 'pathPrefixSet' (or, if 'pathAllowed' is false, we check 'createdPathSet' and
            // 'createdPathPrefixSet').
            const auto& prefixSet = pathAllowed ? pathPrefixSet : createdPathPrefixSet;
            const auto& pathSet = pathAllowed ? modifiedOrCreatedPathSet : createdPathSet;

            if (prefixSet.count(path) || prefixIsInSet(path, pathSet)) {
                resultPaths.emplace_back(path);
            } else if (pathAllowed && !isInclusion) {
                passthruPaths.emplace_back(path);
            } else {
                nothingPaths.emplace_back(path);
            }
        }
    }

    if (!resultInfoDrops.empty() || !resultInfoModifys.empty()) {
        auto fieldSet = StringSet(reqFields.begin(), reqFields.end());

        for (auto&& field : resultInfoModifys) {
            if (fieldSet.insert(field).second) {
                if (createdPathSet.count(field)) {
                    updatedPaths.emplace_back(field);
                } else {
                    resultPaths.emplace_back(field);
                }
            }
        }

        for (auto&& field : resultInfoDrops) {
            if (fieldSet.insert(field).second) {
                nothingPaths.emplace_back(field);
            }
        }
    }

    return std::tuple(std::move(nothingPaths),
                      std::move(passthruPaths),
                      std::move(updatedPaths),
                      std::move(resultPaths));
}

bool canUseCoveredProjection(const QuerySolutionNode* root,
                             bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes) {
    // The "covered projection" optimization can be used for this projection if and only if
    // all of the following conditions are met:
    //   0) 'root' is a projection.
    //   1) 'isInclusion' is true.
    //   2) The 'nodes' vector contains Keep nodes only.
    //   3) The 'root' subtree has at least one unfetched IXSCAN node. (A corollary of this
    //      condition is that 'root->fetched()' must also be false.)
    //   4) For each unfetched IXSCAN in the root's subtree, the IXSCAN's pattern contains
    //      all of the paths needed by the 'root' projection to materialize the result object.
    //   5) For each top-level field needed by the 'root' projection, the order of its subfields
    //      is statically determinable. (This condition is trivially met if there are no common
    //      prefixes among the projection paths needed by root's parent.)
    if (root->fetched() || !isInclusion ||
        !std::all_of(nodes.begin(), nodes.end(), [](auto&& n) { return n.isBool(); })) {
        return false;
    }

    auto [ixNodes, hasFetchesOrScans] = getUnfetchedIxscans(root);
    if (ixNodes.empty() || hasFetchesOrScans) {
        return false;
    }

    // Build a set of the projection paths, build a prefix set of projection paths, and
    // check if there are two or more paths with a common prefix.
    StringSet projPathSet;
    StringSet projPathPrefixSet;
    bool projPathsHaveCommonPrefix = false;

    for (size_t i = 0; i < paths.size(); ++i) {
        auto& path = paths[i];

        if (!projPathsHaveCommonPrefix && projPathPrefixSet.count(getTopLevelField(path))) {
            projPathsHaveCommonPrefix = true;
        }

        projPathSet.insert(path);
        addPrefixesToSet(path, projPathPrefixSet);
    }

    if (projPathsHaveCommonPrefix && hasFetchesOrScans) {
        // If two of more paths needed by root's parent have a common prefix and the 'root' subtree
        // contains one or more FETCHs, then it's not possible to statically determine the subfield
        // order for all the top-level fields needed by root's parent. In this case, we cannot use
        // the "covered projection" optimization because we can't meet condition #4 from above.
        return false;
    }

    std::vector<StringData> patternWithTopLevelSorted;

    for (auto& ixNode : ixNodes) {
        auto ixn = static_cast<const IndexScanNode*>(ixNode);

        StringDataSet patternPartSet;
        std::vector<std::pair<std::pair<StringData, size_t>, StringData>> patternData;

        // Read the pattern for this IXSCAN and build a part set. If the projection
        // paths have a common prefix, we also initialize the 'patternData' vector.
        BSONObjIterator it(ixn->index.keyPattern);
        size_t i = 0;
        while (it.more()) {
            auto part = it.next().fieldNameStringData();

            patternPartSet.insert(part);

            if (projPathsHaveCommonPrefix &&
                (prefixIsInSet(part, projPathSet) || projPathPrefixSet.count(part))) {
                auto topLevelField = getTopLevelField(part);
                patternData.emplace_back(std::pair(topLevelField, i), part);
                ++i;
            }
        }

        // If this pattern does not contain all of the projection paths needed by
        // root's parent, then we can't use the "covered projection" optimization.
        for (const auto& path : paths) {
            if (!patternPartSet.count(path)) {
                return false;
            }
        }

        // If the projection paths have a common prefix, determine the subfield order
        // for each top-level field produced by this IXSCAN.
        //
        // If this is the first IXSCAN in 'ixNodes', store its subfield order info into
        // 'patternWithTopLevelSorted'. If this is not the first IXSCAN in 'ixNodes',
        // compare its subfield order with what's stored in 'patternWithTopLevelSorted'.
        if (projPathsHaveCommonPrefix) {
            std::vector<StringData> pattern;
            pattern.reserve(patternData.size());

            std::sort(patternData.begin(), patternData.end());
            for (auto& part : patternData) {
                pattern.emplace_back(part.second);
            }

            if (patternWithTopLevelSorted.empty()) {
                patternWithTopLevelSorted = std::move(pattern);
            } else {
                if (pattern != patternWithTopLevelSorted) {
                    // If there are two or more IXSCANs whose patterns differ with respect to
                    // how subfields are ordered, then we can't use the "covered projection"
                    // optimization in this case because we can't meet condition #4 above.
                    return false;
                }
            }
        }
    }

    return true;
}

bool canUseCoveredProjection(const QuerySolutionNode* root) {
    if (root->getType() != STAGE_PROJECTION_DEFAULT &&
        root->getType() != STAGE_PROJECTION_COVERED && root->getType() != STAGE_PROJECTION_SIMPLE) {
        return false;
    }

    auto pn = static_cast<const ProjectionNode*>(root);
    const auto& projection = pn->proj;

    bool isInclusion = projection.type() == projection_ast::ProjectType::kInclusion;
    auto [paths, nodes] = getProjectNodes(projection);

    return canUseCoveredProjection(root, isInclusion, paths, nodes);
}

boost::optional<std::vector<std::string>> projectionOutputsFieldOrderIsFixed(
    bool isInclusion,
    const std::vector<std::string>& paths,
    const std::vector<ProjectNode>& nodes,
    const FieldSet& childAllowedFields) {
    // Compute the post-image allowed field set.
    auto allowedFields = childAllowedFields;
    allowedFields.setIntersect(makeAllowedFieldSet(isInclusion, paths, nodes));
    allowedFields.setUnion(makeCreatedFieldSet(isInclusion, paths, nodes));
    // If the field set is open, then return boost::none.
    if (allowedFields.getScope() != FieldListScope::kClosed) {
        return boost::none;
    }
    // If the field set has 2 or more fields that are not "_id", then return boost::none.
    const auto& allowedList = allowedFields.getList();
    bool hasId = allowedFields.count("_id"_sd);
    size_t n = allowedList.size();

    if (n > (hasId ? 2 : 1)) {
        return boost::none;
    }
    // Build the result and return it.
    boost::optional<std::vector<std::string>> result;
    result.emplace();
    result->reserve(n);
    if (hasId) {
        result->emplace_back("_id"_sd);
    }

    if (n == (hasId ? 2 : 1)) {
        size_t idx = allowedList[0] == "_id"_sd ? 1 : 0;
        result->emplace_back(allowedList[idx]);
    }

    return result;
}

std::vector<const QuerySolutionNode*> getProjectionDescendants(const QuerySolutionNode* root) {
    using DfsItem = std::pair<const QuerySolutionNode*, size_t>;

    absl::InlinedVector<DfsItem, 64> dfs;
    std::vector<const QuerySolutionNode*> descendants;

    for (auto&& child : root->children) {
        dfs.emplace_back(DfsItem(child.get(), 0));
    }

    while (!dfs.empty()) {
        auto& dfsBack = dfs.back();
        auto node = dfsBack.first;
        auto childIdx = dfsBack.second;
        bool popDfs = true;

        auto visitNextChild = [&] {
            if (childIdx < node->children.size()) {
                popDfs = false;
                dfsBack.second++;
                dfs.emplace_back(DfsItem(node->children[childIdx].get(), 0));
                return true;
            }
            return false;
        };

        switch (node->getType()) {
            case STAGE_LIMIT:
            case STAGE_SKIP:
            case STAGE_MATCH:
            case STAGE_SHARDING_FILTER:
            case STAGE_SORT_SIMPLE:
            case STAGE_SORT_DEFAULT:
            case STAGE_OR:
            case STAGE_SORT_MERGE:
                visitNextChild();
                break;
            default:
                descendants.push_back(node);
                break;
        }

        if (popDfs) {
            dfs.pop_back();
        }
    }

    return descendants;
}
}  // namespace

std::unique_ptr<BuildProjectionPlan> SlotBasedStageBuilder::makeBuildProjectionPlan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto pn = static_cast<const ProjectionNode*>(root);
    const auto& projection = pn->proj;

    bool isInclusion = projection.type() == projection_ast::ProjectType::kInclusion;

    const auto& childAllowedFields = getAllowedFieldSet(root->children[0].get());

    // Get the ProjectNodes.
    auto [projPaths, projNodes] = getProjectNodes(projection);
    auto paths = std::move(projPaths);
    auto nodes = std::move(projNodes);

    // Get the parent's requirements.
    auto reqFields = reqs.getFields();

    bool reqResultObj = reqs.hasResultObj();
    bool reqResultInfo = reqs.hasResultInfo();

    std::vector<std::string> resultInfoDrops;
    std::vector<std::string> resultInfoModifys;
    boost::optional<FieldSet> updatedAllowedSet;

    // Check if this projection can use the "covered projection" optimization.
    bool isCoveredProjection = canUseCoveredProjection(root, isInclusion, paths, nodes);

    if (reqResultInfo) {
        bool resultInfoReqIsCompatible = false;

        if (!isCoveredProjection) {
            const auto& reqAllowedSet = reqs.getResultInfoAllowedSet();

            auto allowedSet = makeAllowedFieldSet(isInclusion, paths, nodes);
            allowedSet.setIntersect(childAllowedFields);

            bool scopesDiffer = reqAllowedSet.getScope() == FieldListScope::kOpen &&
                allowedSet.getScope() == FieldListScope::kClosed;

            auto createdSet = makeCreatedFieldSet(isInclusion, paths, nodes);
            createdSet.setIntersect(reqAllowedSet);

            if (createdSet.getList().empty() && createdSet.getScope() == FieldListScope::kClosed &&
                !scopesDiffer) {
                resultInfoReqIsCompatible = true;

                auto effects = ProjectionEffects(isInclusion, paths, nodes);
                effects.compose(ProjectionEffects(childAllowedFields));

                auto effectsAllowedSet = effects.getAllowedFieldSet();
                auto effectsModifiedSet = effects.getModifiedOrCreatedFieldSet();

                auto reqAllowedSetComplement = reqAllowedSet;
                reqAllowedSetComplement.setComplement();

                effectsAllowedSet.setUnion(reqAllowedSetComplement);
                effectsModifiedSet.setIntersect(reqAllowedSet);

                tassert(8378204,
                        "Expected effectsAllowedSet scope to be kOpen",
                        effectsAllowedSet.getScope() == FieldListScope::kOpen);

                resultInfoDrops = effectsAllowedSet.getList();
                resultInfoModifys = effectsModifiedSet.getList();

                // Initialized 'updatedAllowedSet'.
                updatedAllowedSet.emplace(reqAllowedSet);
                updatedAllowedSet->setIntersect(allowedSet);
            }
        }

        if (!resultInfoReqIsCompatible) {
            reqResultObj = true;
            reqResultInfo = false;
        }
    }

    // Map the parent's required fields onto the outputs of this projection.
    auto [nothingPaths, passthruPaths, updatedPaths, resultPaths] =
        mapRequiredFieldsToProjectionOutputs(reqFields,
                                             isInclusion,
                                             childAllowedFields,
                                             paths,
                                             nodes,
                                             resultInfoDrops,
                                             resultInfoModifys);

    // Eliminate parts of the projection that are known to be no-ops.
    if (!childAllowedFields.getList().empty() ||
        childAllowedFields.getScope() == FieldListScope::kClosed) {
        size_t outIdx = 0;

        for (size_t idx = 0; idx < nodes.size(); ++idx) {
            bool pathAllowed = childAllowedFields.count(getTopLevelField(paths[idx]));

            if (nodes[idx].isExpr() || pathAllowed) {
                if (outIdx != idx) {
                    nodes[outIdx] = std::move(nodes[idx]);
                    paths[outIdx] = std::move(paths[idx]);
                }
                ++outIdx;
            }
        }

        if (outIdx < nodes.size()) {
            nodes.resize(outIdx);
            paths.resize(outIdx);
        }
    }

    StringSet updatedPathSet(updatedPaths.begin(), updatedPaths.end());
    StringMap<Expression*> updatedPathsExprMap;
    if (!updatedPathSet.empty()) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (updatedPathSet.count(paths[i])) {
                updatedPathsExprMap.emplace(paths[i], nodes[i].getExpr());
            }
        }
    }

    std::vector<std::string> resultFields;
    StringSet resultFieldSet;
    for (auto&& p : resultPaths) {
        auto f = getTopLevelField(p);
        auto [_, inserted] = resultFieldSet.insert(f.toString());
        if (inserted) {
            resultFields.emplace_back(f.toString());
        }
    }

    // Compute the dependencies of any expressions in this projection that we need.
    DepsTracker deps;
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        auto& path = paths[i];
        if (node.isExpr()) {
            auto f = getTopLevelField(path);
            if (reqResultObj || updatedPathSet.count(path) || resultFieldSet.count(f)) {
                expression::addDependencies(node.getExpr(), &deps);
            }
        }
    }

    bool childMakeResult = deps.needWholeDocument;

    bool makeResult = reqResultObj || !resultPaths.empty();

    auto allowedSet = makeAllowedFieldSet(isInclusion, paths, nodes);
    allowedSet.setIntersect(childAllowedFields);

    // If we don't need to produce a materialized result object, then we can eliminate the parts of
    // the projection that are not in 'resultFieldSet'.
    if (!reqResultObj) {
        size_t outIdx = 0;
        for (size_t idx = 0; idx < nodes.size(); ++idx) {
            if (resultFieldSet.count(getTopLevelField(paths[idx]))) {
                if (outIdx != idx) {
                    nodes[outIdx] = std::move(nodes[idx]);
                    paths[outIdx] = std::move(paths[idx]);
                }
                ++outIdx;
            }
        }

        if (outIdx < nodes.size()) {
            nodes.resize(outIdx);
            paths.resize(outIdx);
        }
    }

    // Determine what 'planType' should be set to.
    //
    // If 'reqResultInfo' is false, we will eagerly initialize 'projInputFields' and
    // 'projNothingInputFields' when we set 'planType' (except when we set 'planType' to
    // 'kUseChildResultInfo').
    //
    // If 'reqResultInfo' is true or if we choose the 'kUseChildResultInfo' plan type,
    // then we will handle initializing 'projInputFields' and 'projNothingInputFields'
    // later.
    boost::optional<BuildProjectionPlan::Type> planType;

    std::vector<std::string> projInputFields;
    std::vector<std::string> projNothingInputFields;
    boost::optional<std::vector<std::string>> inputPlanSingleFields;

    if (!makeResult) {
        // We don't have to materialize a result object, and we don't have to create a temporary
        // object either.
        planType.emplace(BuildProjectionPlan::kDoNotMakeResult);
    }

    if (!planType && childMakeResult) {
        // Materialize the result object (or temporary object) using the child's materialized
        // result object as input.
        planType.emplace(BuildProjectionPlan::kUseChildResultObj);
    }

    if (!planType && isCoveredProjection) {
        tassert(8378205, "Expected ResultInfo requirement to not be set", !reqResultInfo);

        // Materialize the result object (or temporary object) using the "covered projection"
        // technique.
        //
        // For covered projections, we eagerly initialize 'projInputFields' to 'paths'. (We know
        // that 'reqResultInfo' is always false when 'isCoveredProjection' is true.)
        planType.emplace(BuildProjectionPlan::kUseCoveredProjection);

        auto ixNode = getUnfetchedIxscans(root).first[0];
        auto ixn = static_cast<const IndexScanNode*>(ixNode);

        auto fields = getTopLevelFields(paths);
        auto fieldSet = StringSet(fields.begin(), fields.end());
        bool hasId = fieldSet.count("_id"_sd);

        // Clear 'fields'. We're going to re-build the list of fields to match the order of
        // ixn's key pattern.
        fields.clear();

        // If 'fields' contains "_id", handle that first.
        if (hasId) {
            fields.push_back("_id");
            // Remove "_id" from 'fieldSet' so that the loop below doesn't add another occurrence.
            fieldSet.erase("_id");
        }

        // Loop over ixn's key pattern and re-build the list of fields.
        BSONObjIterator it(ixn->index.keyPattern);
        while (it.more()) {
            auto part = it.next().fieldNameStringData();
            auto f = getTopLevelField(part);

            if (fieldSet.count(f)) {
                std::string str = f.toString();
                fieldSet.erase(str);
                fields.push_back(std::move(str));
            }
        }

        projInputFields = paths;
    }

    // If our parent doesn't actually need the full materialized result object and we haven't
    // determined 'planType' yet, then we can use a fixed plan without a resultBase.
    if (!planType && !reqResultObj) {
        planType.emplace(BuildProjectionPlan::kUseInputPlanWithoutObj);

        // If 'reqResultInfo' is true, initialize 'projInputFields' eagerly.
        if (!reqResultInfo) {
            projInputFields = resultFields;
        }

        inputPlanSingleFields.emplace(resultFields);
    }

    // If our parent needs the full materialized result object and we can prove the materialized
    // result object either will have a single field only or will have "_id" and one other field
    // only, then we can use a fixed plan.
    if (!planType && allowedSet.getScope() == FieldListScope::kClosed) {
        tassert(8378206, "Expected result object requirement to be set", reqResultObj);

        auto fields =
            projectionOutputsFieldOrderIsFixed(isInclusion, paths, nodes, childAllowedFields);
        if (fields) {
            planType.emplace(BuildProjectionPlan::kUseInputPlanWithoutObj);

            // Initialize 'projInputFields' eagerly.
            projInputFields = *fields;

            inputPlanSingleFields.emplace(std::move(*fields));
        }
    }

    // If our parent needs the full materialized result object and we haven't determined 'planType'
    // yet, figure out if this projection can ask its descendants for ResultInfo.
    if (!planType) {
        tassert(8378207, "Expected result object requirement to be set", reqResultObj);

        bool someDescendantsCanProduceResultInfo = false;

        for (auto desc : getProjectionDescendants(root)) {
            // Skip non-projection descendants.
            bool isProjection = desc->getType() == STAGE_PROJECTION_DEFAULT ||
                desc->getType() == STAGE_PROJECTION_COVERED ||
                desc->getType() == STAGE_PROJECTION_SIMPLE;
            if (!isProjection) {
                continue;
            }
            // Decode the projection.
            auto descPn = static_cast<const ProjectionNode*>(desc);
            auto [descPaths, descNodes] = getProjectNodes(descPn->proj);
            bool descIsInclusion = descPn->proj.type() == projection_ast::ProjectType::kInclusion;
            // Skip covered projections.
            if (canUseCoveredProjection(desc, descIsInclusion, descPaths, descNodes)) {
                continue;
            }
            // Compute the the allowed set for the descendant projection.
            auto descAllowedSet = makeAllowedFieldSet(descIsInclusion, descPaths, descNodes);
            // Check if the descendant would be able to "participate" with a ResultInfo req
            // from the ancestor projection.
            bool scopesDiffer = allowedSet.getScope() == FieldListScope::kOpen &&
                descAllowedSet.getScope() == FieldListScope::kClosed;

            auto descCreatedSet = makeCreatedFieldSet(descIsInclusion, descPaths, descNodes);
            descCreatedSet.setIntersect(allowedSet);

            // If 'descCreatedSet' is the empty set and 'scopesDiffer' is false, then the descendant
            // can "participate".
            if (descCreatedSet.getList().empty() &&
                descCreatedSet.getScope() == FieldListScope::kClosed && !scopesDiffer) {
                someDescendantsCanProduceResultInfo = true;
            }
        }
        // If none of the descendants could participate with ResultInfo req, then we won't bother
        // trying to ask our child for ResultInfo.
        if (someDescendantsCanProduceResultInfo) {
            planType.emplace(BuildProjectionPlan::kUseChildResultInfo);
        }
    }

    // If our parent needs the full materialized result object and we haven't determined 'planType'
    // yet, then we need to ask our child to produce a materialized result object.
    if (!planType) {
        tassert(8378208, "Expected result object requirement to be set", reqResultObj);

        childMakeResult = true;
        planType.emplace(BuildProjectionPlan::kUseChildResultObj);
    }

    // If there is a ResultInfo req that we are participating with, or if we are using the
    // 'kUseChildResultInfo' plan type, then we need to populate 'projInputFields' and
    // 'projNothingInputFields' with the necessary inputs.
    if (reqResultInfo) {
        projInputFields = resultFields;
    } else if (planType == BuildProjectionPlan::kUseChildResultInfo) {
        auto modifiedOrCreated = makeModifiedOrCreatedFieldSet(isInclusion, paths, nodes).getList();

        for (const auto& field : modifiedOrCreated) {
            if (childAllowedFields.count(field)) {
                projInputFields.emplace_back(field);
            } else {
                projNothingInputFields.emplace_back(field);
            }
        }
    }

    // Start preparing the requirements for our child.
    auto childReqs = reqs.copyForChild().clearResult().clearAllFields();

    if (childMakeResult) {
        // If 'childMakeResult' is true, add a result object requirement to 'childReqs'.
        childReqs.setResultObj();
    } else if (planType == BuildProjectionPlan::kUseChildResultInfo && reqResultObj) {
        // If our parent asked for a materialized result and if we've decided to ask our child
        // for ResultInfo, then set a ResultInfo req on 'childReqs'.
        childReqs.setResultInfo(allowedSet);
    } else if (reqResultInfo) {
        // Otherwise, if there is a ResultInfo req that we are participating with, then set a
        // ResultInfo req on 'childReqs' with an updated ProjectionEffects that reflect the
        // effects of this projection.
        childReqs.setResultInfo(*updatedAllowedSet);
    }

    // Compute the list of fields that we need to request from our child.
    auto fields = std::move(passthruPaths);
    fields = appendVectorUnique(std::move(fields), std::move(projInputFields));
    fields = appendVectorUnique(std::move(fields), getTopLevelFields(deps.fields));

    childReqs.setFields(std::move(fields));

    // Indicate we can work on block values, if we are not requested to produce a result object.
    childReqs.setCanProcessBlockValues(!childReqs.hasResult());

    bool produceResultObj = reqResultObj;

    return std::make_unique<BuildProjectionPlan>(
        BuildProjectionPlan{std::move(childReqs),
                            *planType,
                            reqResultInfo,
                            produceResultObj,
                            isInclusion,
                            std::move(paths),
                            std::move(nodes),
                            std::move(nothingPaths),
                            std::move(resultPaths),
                            std::move(updatedPaths),
                            std::move(updatedPathsExprMap),
                            std::move(resultInfoDrops),
                            std::move(resultInfoModifys),
                            std::move(projNothingInputFields),
                            std::move(inputPlanSingleFields)});
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildProjection(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(8146605, "buildProjection() does not support kSortKey", !reqs.hasSortKeys());

    // Build a plan for this projection.
    std::unique_ptr<BuildProjectionPlan> plan = makeBuildProjectionPlan(root, reqs);

    // Call build() on the child.
    auto [stage, outputs] = build(root->children[0].get(), plan->childReqs);

    // Call buildProjectionImpl() to generate all SBE expressions and stages needed for this
    // projection.
    return buildProjectionImpl(root, reqs, std::move(plan), std::move(stage), std::move(outputs));
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionImpl(const QuerySolutionNode* root,
                                           const PlanStageReqs& reqs,
                                           std::unique_ptr<BuildProjectionPlan> plan,
                                           std::unique_ptr<sbe::PlanStage> stage,
                                           PlanStageSlots outputs) {
    BuildProjectionPlan::Type planType = plan->type;

    const bool isInclusion = plan->isInclusion;
    std::vector<std::string>& paths = plan->paths;
    std::vector<ProjectNode>& nodes = plan->nodes;

    // Update 'outputs' so that the kField slot for each field in 'plan->projNothingInputFields'
    // is set to Nothing.
    if (!plan->projNothingInputFields.empty()) {
        auto nothingSlot = _state.getNothingSlot();
        for (auto&& name : plan->projNothingInputFields) {
            outputs.set(std::make_pair(PlanStageSlots::kField, name), nothingSlot);
        }
    }

    std::unique_ptr<sbe::MakeObjInputPlan> inputPlan;

    if (planType == BuildProjectionPlan::kUseInputPlanWithoutObj) {
        auto allowedSet = FieldSet::makeClosedSet(*plan->inputPlanSingleFields);

        inputPlan = std::make_unique<sbe::MakeObjInputPlan>(std::move(*plan->inputPlanSingleFields),
                                                            std::move(allowedSet));
    } else if (planType == BuildProjectionPlan::kUseChildResultInfo) {
        tassert(8378209, "Expected ResultInfo requirement to be set", outputs.hasResultInfo());

        const auto& childAllowedFields = getAllowedFieldSet(root->children[0].get());

        // Store this projection's effects into 'effects', and then compose 'effects' with
        // 'childAllowedFields'.
        auto effects = ProjectionEffects(isInclusion, paths, nodes);
        effects.compose(ProjectionEffects(childAllowedFields));

        // Compose 'effects' with our descendants' collective ProjectionEffects.
        effects.compose(outputs.getResultInfoChanges());

        tassert(8378210,
                "Expected default effect to be Keep or Drop",
                effects.getDefaultEffect() == ProjectionEffects::kKeep ||
                    effects.getDefaultEffect() == ProjectionEffects::kDrop);

        if (effects.hasEffect(ProjectionEffects::kModify) ||
            effects.hasEffect(ProjectionEffects::kCreate) ||
            (!isInclusion && effects.getDefaultEffect() == ProjectionEffects::kDrop)) {
            // If 'effects' has at least one Modify or Create, or if this is an exclusion
            // projection and 'effects.defaultEffect' is Drop, then we need to make a
            // MakeObjInputPlan.
            inputPlan = std::make_unique<sbe::MakeObjInputPlan>(
                effects.getModifiedOrCreatedFieldSet().getList(), effects.getAllowedFieldSet());
        } else {
            // If there are no Create/Modify effects and either this is an inclusion or
            // 'effects.defaultEffect' is not Drop, then we can take the result base object
            // from our child and pass it to makeBsonObj() without using a MakeObjInputPlan.

            // If this is an exclusion projection, take all of the fields this projection
            // promised it would drop (via its ResultInfo req) and manually add "drop" actions
            // to 'paths' / 'nodes' for each of these fields.
            if (!isInclusion) {
                auto fields = getTopLevelFields(paths);
                auto fieldSet = StringSet(fields.begin(), fields.end());

                for (const auto& field : effects.getFieldList()) {
                    if (effects.isDrop(field) && !fieldSet.count(field)) {
                        paths.emplace_back(field);
                        nodes.emplace_back(ProjectNode::Drop{});
                    }
                }
            }
        }
    } else if (planType == BuildProjectionPlan::kUseCoveredProjection) {
        tassert(8146606,
                "Cannot generate a covered projection when 'isInclusion' is false",
                isInclusion);
        tassert(8146607,
                "Expected 'updatedPathsExprMap' to be empty",
                plan->updatedPathsExprMap.empty());

        auto ixNode = getUnfetchedIxscans(root).first[0];
        auto ixn = static_cast<const IndexScanNode*>(ixNode);

        // When doing a covered projection, we need to re-order 'paths' so that it matches the order
        // of the ixn's pattern, and we need to replace all the "Keep" nodes with "SbExpr" nodes.
        std::vector<std::string> newPaths;
        std::vector<ProjectNode> newNodes;
        auto pathSet = StringSet(paths.begin(), paths.end());

        // Do two passes, collecting all paths that start with "_id" during the first pass and
        // collecting all other paths during the second pass.
        for (bool firstPass : {true, false}) {
            BSONObjIterator it(ixn->index.keyPattern);

            while (it.more()) {
                auto part = it.next().fieldNameStringData();

                if (pathSet.count(part)) {
                    auto slot = outputs.get({kField, part});
                    bool topLevelFieldIsId = getTopLevelField(part) == "_id"_sd;
                    if (topLevelFieldIsId == firstPass) {
                        newPaths.emplace_back(part.toString());
                        newNodes.emplace_back(SbExpr{slot});
                    }
                }
            }
        }

        paths = std::move(newPaths);
        nodes = std::move(newNodes);
    }

    SbBuilder b(_state, root->nodeId());

    // Prepare the list of MQL expressions we need to evaluate.
    std::vector<const Expression*> exprs;
    std::vector<std::string> exprPaths;
    std::vector<boost::optional<size_t>> exprNodeIdxs;
    std::vector<boost::optional<TypedSlot>> exprSlots;
    StringSet exprPathSet;
    StringMap<TypedSlot> updatedPathsSlotMap;

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].isExpr() && exprPathSet.emplace(paths[i]).second) {
            exprs.push_back(nodes[i].getExpr());
            exprPaths.push_back(paths[i]);
            exprNodeIdxs.push_back(i);
            exprSlots.push_back(boost::none);
        }
    }
    for (auto& path : plan->updatedPaths) {
        if (plan->updatedPathsExprMap.count(path) && exprPathSet.emplace(path).second) {
            exprs.push_back(plan->updatedPathsExprMap[path]);
            exprPaths.push_back(path);
            exprNodeIdxs.push_back(boost::none);
            exprSlots.push_back(boost::none);
        }
    }

    const size_t numExprs = exprPaths.size();
    size_t numExprsProcessed = 0;

    auto generateProjExpressions = [&](bool vectorizeExprs = false) {
        // Call generateExpression() to evaluate each MQL expression, and then use a ProjectStage
        // to project the output values to slots.
        auto resultObjSlot = outputs.getResultObjIfExists();
        SbExprOptSbSlotVector projects;
        std::vector<uint8_t> processed;
        processed.resize(numExprs, 0);

        for (size_t i = 0; i < numExprs; ++i) {
            if (!exprSlots[i].has_value()) {
                SbExpr e = generateExpression(_state, exprs[i], resultObjSlot, outputs);

                if (vectorizeExprs && !e.isSlotExpr()) {
                    // Attempt to vectorize.
                    auto blockResult = buildVectorizedExpr(_state, std::move(e), outputs, false);
                    if (blockResult) {
                        // If vectorization is successful, store the result to 'projects'.
                        projects.emplace_back(std::move(blockResult), boost::none);
                        processed[i] = 1;
                    }
                } else {
                    // If we're not attempting to vectorize or if 'e' is just a slot variable,
                    // then add 'e' to 'projects'.
                    projects.emplace_back(std::move(e), boost::none);
                    processed[i] = 1;
                }
            }
        }

        // If the loop above didn't add anything to 'projects', then there's nothing to do and
        // we can return early.
        if (projects.empty()) {
            return;
        }

        // Create the ProjectStage and get the list of slots that were projected to.
        auto [outStage, outSlots] =
            b.makeProject(std::move(stage), buildVariableTypes(outputs), std::move(projects));
        stage = std::move(outStage);

        size_t outSlotsIdx = 0;
        for (size_t i = 0; i < numExprs; ++i) {
            if (processed[i]) {
                // Store the slot into 'exprSlots[i]'. Doing this will prevent us from trying
                // process this expression again if generateProjExpressions() gets invoked
                // multiple times.
                auto slot = outSlots[outSlotsIdx++];
                exprSlots[i].emplace(slot);

                // Update 'nodes' and 'updatedPathsSlotMap' and increment 'numExprsProcessed'.
                if (auto nodeIdx = exprNodeIdxs[i]) {
                    nodes[*nodeIdx] = ProjectNode(SbExpr{slot});
                }
                if (plan->updatedPathsExprMap.count(exprPaths[i])) {
                    updatedPathsSlotMap[exprPaths[i]] = slot;
                }

                ++numExprsProcessed;
            }
        }
    };

    // If hasBlockOutput() returns true, then we attempt to vectorize the MQL expressions. We may
    // also have to generate a BlockToRow stage if this projection or this projection's parent can't
    // handle the blocks being produced by this projection's child.
    if (outputs.hasBlockOutput()) {
        // Call generateProjExpressions() with 'vectorizeExprs' set to true.
        if (!exprPaths.empty()) {
            constexpr bool vectorizeExprs = true;
            generateProjExpressions(vectorizeExprs);
        }

        // Terminate the block processing section of the pipeline if there are expressions
        // that are not compatible with block processing, the parent stage doesn't support
        // block values or if we need to build a scalar result document.
        if (numExprsProcessed != numExprs || !reqs.getCanProcessBlockValues() ||
            planType != BuildProjectionPlan::kDoNotMakeResult) {
            // Store all the slots from 'exprSlots' into the 'individualSlots' vector.
            TypedSlotVector individualSlots;
            for (size_t i = 0; i < numExprs; ++i) {
                if (exprSlots[i]) {
                    individualSlots.push_back(*exprSlots[i]);
                }
            }

            // Create a BlockToRowStage.
            auto [outStage, outSlots] =
                buildBlockToRow(std::move(stage), _state, outputs, std::move(individualSlots));
            stage = std::move(outStage);

            // For each slot that was in 'exprSlots', replace all occurrences of the original
            // slot with the corresponding new slot produced by the BlockToRow stage.
            size_t outSlotsIdx = 0;
            for (size_t i = 0; i < numExprs; ++i) {
                if (exprSlots[i]) {
                    // Update the slot at 'exprSlots[i]'.
                    auto slot = outSlots[outSlotsIdx++];
                    exprSlots[i] = slot;

                    // Update 'nodes' and 'updatedPathsSlotMap'.
                    if (auto nodeIdx = exprNodeIdxs[i]) {
                        nodes[*nodeIdx] = ProjectNode(SbExpr{slot});
                    }
                    if (plan->updatedPathsExprMap.count(exprPaths[i])) {
                        updatedPathsSlotMap[exprPaths[i]] = slot;
                    }
                }
            }
        }
    }

    // Evaluate the MQL expressions needed by this projection.
    if (!exprPaths.empty()) {
        generateProjExpressions();
    }

    auto projectType = isInclusion ? projection_ast::ProjectType::kInclusion
                                   : projection_ast::ProjectType::kExclusion;

    boost::optional<TypedSlot> projOutputSlot;

    // Produce a materialized result object (or a temporary result object) if needed.
    if (planType != BuildProjectionPlan::kDoNotMakeResult) {
        auto projectionExpr = [&] {
            if (planType == BuildProjectionPlan::kUseChildResultObj) {
                return generateProjection(_state,
                                          projectType,
                                          std::move(paths),
                                          std::move(nodes),
                                          outputs.getResultObj(),
                                          &outputs);
            } else if (planType == BuildProjectionPlan::kUseCoveredProjection) {
                return generateProjection(_state,
                                          projectType,
                                          std::move(paths),
                                          std::move(nodes),
                                          b.makeNullConstant(),
                                          &outputs);
            } else if (planType == BuildProjectionPlan::kUseInputPlanWithoutObj) {
                return generateProjectionWithInputFields(_state,
                                                         projectType,
                                                         std::move(paths),
                                                         std::move(nodes),
                                                         {} /* resultBase */,
                                                         *inputPlan,
                                                         &outputs);
            } else if (planType == BuildProjectionPlan::kUseChildResultInfo && !inputPlan) {
                SbExpr resultBase = SbExpr{outputs.getResultInfoBaseObj()};

                return generateProjection(_state,
                                          projectType,
                                          std::move(paths),
                                          std::move(nodes),
                                          std::move(resultBase),
                                          &outputs);
            } else if (planType == BuildProjectionPlan::kUseChildResultInfo && inputPlan) {
                SbExpr resultBase = SbExpr{outputs.getResultInfoBaseObj()};

                return generateProjectionWithInputFields(_state,
                                                         projectType,
                                                         std::move(paths),
                                                         std::move(nodes),
                                                         std::move(resultBase),
                                                         *inputPlan,
                                                         &outputs);
            } else {
                MONGO_UNREACHABLE_TASSERT(8146608);
            }
        }();

        auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(projectionExpr));
        stage = std::move(outStage);

        projOutputSlot.emplace(outSlots[0]);

        if (plan->produceResultObj) {
            outputs.setResultObj(*projOutputSlot);
        }
    }

    // Update kField slots in 'outputs' as appropriate.
    for (auto&& updatedPath : plan->updatedPaths) {
        auto slot = updatedPathsSlotMap[updatedPath];
        outputs.set(std::make_pair(PlanStageSlots::kField, std::move(updatedPath)), slot);
    }

    // Set kField slots to Nothing as appropriate.
    if (!plan->nothingPaths.empty()) {
        auto nothingSlot = _state.getNothingSlot();
        for (auto&& nothingPath : plan->nothingPaths) {
            outputs.set(std::make_pair(PlanStageSlots::kField, nothingPath), nothingSlot);
        }
    }

    // Assign values retrieved from the result object to kField slots as appropriate.
    if (!plan->resultPaths.empty()) {
        for (auto&& resultPath : plan->resultPaths) {
            outputs.clear(std::make_pair(PlanStageSlots::kField, resultPath));
        }

        auto [outStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                         plan->resultPaths,
                                                         *projOutputSlot,
                                                         root->nodeId(),
                                                         &_slotIdGenerator,
                                                         _state,
                                                         &outputs);
        stage = std::move(outStage);

        for (size_t i = 0; i < plan->resultPaths.size(); ++i) {
            auto& resultPath = plan->resultPaths[i];
            auto slot = outSlots[i];
            outputs.set(std::make_pair(PlanStageSlots::kField, std::move(resultPath)), slot);
        }
    }

    if (plan->reqResultInfo) {
        tassert(8378211, "Expected 'outputs' to have a result", outputs.hasResult());
        // If 'outputs' has a materialized result and 'reqs' was expecting ResultInfo, then
        // convert the materialized result into a ResultInfo.
        if (outputs.hasResultObj()) {
            outputs.setResultInfoBaseObj(outputs.getResultObj());
        }
        // For changes (drops/modifys/creates) from this projection on fields that are not
        // dropped by 'reqs.getResultInfoAllowedSet()', record the changes in 'outputs'.
        outputs.addResultInfoChanges(plan->resultInfoDrops, plan->resultInfoModifys);
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildOr(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto orn = static_cast<const OrNode*>(root);

    bool needChildResultDoc = false;

    std::vector<std::string> fields;

    if (orn->filter) {
        DepsTracker deps;
        match_expression::addDependencies(orn->filter.get(), &deps);
        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            fields = getTopLevelFields(deps.fields);
        }

        needChildResultDoc = deps.needWholeDocument;
    }

    // Children must produce all of the slots required by the parent of this OrNode. In addition
    // to that, children must always produce a 'recordIdSlot' if the 'dedup' flag is true, and
    // children must produce a 'resultSlot' if 'filter' needs the whole document.
    auto childReqs = reqs.copyForChild().setIf(kRecordId, orn->dedup).setFields(std::move(fields));

    if (needChildResultDoc) {
        childReqs.setResultObj();
    }

    std::vector<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>> inputStagesAndSlots;
    for (auto&& child : orn->children) {
        auto [stage, outputs] = build(child.get(), childReqs);
        inputStagesAndSlots.emplace_back(std::pair(std::move(stage), std::move(outputs)));
    }

    auto outputs = PlanStageSlots::makeMergedPlanStageSlots(
        _state, root->nodeId(), childReqs, inputStagesAndSlots);

    auto unionOutputSlots = getSlotsOrderedByName(childReqs, outputs);

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputSlots;
    for (auto& p : inputStagesAndSlots) {
        auto& stage = p.first;
        auto& outputs = p.second;
        inputStages.push_back(std::move(stage));
        inputSlots.push_back(getSlotsOrderedByName(childReqs, outputs));
    }

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
        auto resultSlot = outputs.getResultObjIfExists();

        auto filterExpr = generateFilter(_state, orn->filter.get(), resultSlot, outputs);

        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(_state), root->nodeId());
        }
    }

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

    auto childReqs = reqs.copyForChild().setResultObj();
    auto [stage, outputs] = build(textNode->children[0].get(), childReqs);
    tassert(5432217, "result slot is not produced by text match sub-plan", outputs.hasResultObj());

    // Create an FTS 'matcher' to apply 'ftsQuery' to matching documents.
    auto matcher = makeFtsMatcher(
        _opCtx, coll, textNode->index.identifier.catalogName, textNode->ftsQuery.get());

    // Build an 'ftsMatch' expression to match against the result object using the 'matcher'
    // instance.
    auto ftsMatch =
        makeFunction("ftsMatch",
                     makeConstant(sbe::value::TypeTags::ftsMatcher,
                                  sbe::value::bitcastFrom<fts::FTSMatcher*>(matcher.release())),
                     makeVariable(outputs.getResultObj()));

    // Wrap the 'ftsMatch' expression into an 'if' expression to ensure that it can be applied only
    // to a document.
    auto filter =
        sbe::makeE<sbe::EIf>(makeFunction("isObject", makeVariable(outputs.getResultObj())),
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
    auto childReqs = reqs.copyForChild().clearResult().clearAllFields().set(kReturnKey);
    auto [stage, outputs] = build(returnKeyNode->children[0].get(), childReqs);

    outputs.setResultObj(outputs.get(kReturnKey));
    outputs.clear(kReturnKey);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildEof(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    return generateEofPlan(_state, reqs, root->nodeId());
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndHash(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto andHashNode = static_cast<const AndHashNode*>(root);

    tassert(6023412, "buildAndHash() does not support kSortKey", !reqs.hasSortKeys());
    tassert(5073711, "need at least two children for AND_HASH", andHashNode->children.size() >= 2);

    auto childReqs = reqs.copyForChild().setResultObj().set(kRecordId).clearAllFields();

    auto outerChild = andHashNode->children[0].get();
    auto innerChild = andHashNode->children[1].get();

    auto [outerStage, outerOutputs] = build(outerChild, childReqs);
    auto outerIdSlot = outerOutputs.get(kRecordId).slotId;
    auto outerResultSlot = outerOutputs.getResultObj().slotId;
    auto outerCondSlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);

    auto innerIdSlot = innerOutputs.get(kRecordId).slotId;
    auto innerResultSlot = innerOutputs.getResultObj().slotId;
    auto innerSnapshotIdSlot = innerOutputs.getIfExists(kSnapshotId);
    auto innerIndexIdentSlot = innerOutputs.getIfExists(kIndexIdent);
    auto innerIndexKeySlot = innerOutputs.getIfExists(kIndexKey);
    auto innerIndexKeyPatternSlot = innerOutputs.getIfExists(kIndexKeyPattern);

    auto innerCondSlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    auto collatorSlot = _state.getCollatorSlot();

    // Designate outputs.
    PlanStageSlots outputs;

    outputs.setResultObj(innerResultSlot);

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
                                                _yieldPolicy,
                                                root->nodeId());

    // If there are more than 2 children, iterate all remaining children and hash
    // join together.
    for (size_t i = 2; i < andHashNode->children.size(); i++) {
        auto [childStage, outputs] = build(andHashNode->children[i].get(), childReqs);

        auto idSlot = outputs.get(kRecordId).slotId;
        auto resultSlot = outputs.getResultObj().slotId;
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
                                               _yieldPolicy,
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

    auto childReqs = reqs.copyForChild().setResultObj().set(kRecordId).clearAllFields();

    auto outerChild = andSortedNode->children[0].get();
    auto innerChild = andSortedNode->children[1].get();

    auto outerChildReqs = childReqs.copyForChild()
                              .clear(kSnapshotId)
                              .clear(kIndexIdent)
                              .clear(kIndexKey)
                              .clear(kIndexKeyPattern);
    auto [outerStage, outerOutputs] = build(outerChild, outerChildReqs);

    auto outerIdSlot = outerOutputs.get(kRecordId).slotId;
    auto outerResultSlot = outerOutputs.getResultObj().slotId;

    auto outerKeySlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);

    auto innerIdSlot = innerOutputs.get(kRecordId).slotId;
    auto innerResultSlot = innerOutputs.getResultObj().slotId;

    auto innerKeySlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    // Designate outputs.
    PlanStageSlots outputs;

    outputs.setResultObj(innerResultSlot);

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

        auto idSlot = outputs.get(kRecordId).slotId;
        auto resultSlot = outputs.getResultObj().slotId;
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
    auto makeUnionBranch = [&](bool isTailableCollScanResumeBranch) {
        auto childReqs = reqs;
        childReqs.setIsTailableCollScanResumeBranch(isTailableCollScanResumeBranch);
        return build(root, childReqs);
    };

    std::vector<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>> inputStagesAndSlots;

    // Build the anchor branch and resume branch of the union.
    inputStagesAndSlots.emplace_back(makeUnionBranch(false));
    inputStagesAndSlots.emplace_back(makeUnionBranch(true));

    // Add a constant filter on top of the anchor branch, so that it would only execute on an
    // initial collection scan, that is, when resumeRecordId is not available yet.
    auto& [anchorBranch, _] = inputStagesAndSlots[0];
    anchorBranch = sbe::makeS<sbe::FilterStage<true>>(
        std::move(anchorBranch),
        makeNot(makeFunction("exists"_sd, sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    // Add a constant filter on top of the resume branch, so that it would only execute when we
    // resume a collection scan from the resumeRecordId.
    auto& [resumeBranch, __] = inputStagesAndSlots[1];
    resumeBranch = sbe::makeS<sbe::FilterStage<true>>(
        sbe::makeS<sbe::LimitSkipStage>(
            std::move(resumeBranch), nullptr, makeInt64Constant(1), root->nodeId()),
        sbe::makeE<sbe::EFunction>("exists"_sd,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    auto outputs =
        PlanStageSlots::makeMergedPlanStageSlots(_state, root->nodeId(), reqs, inputStagesAndSlots);

    auto unionOutputSlots = getSlotsOrderedByName(reqs, outputs);

    sbe::PlanStage::Vector inputStages;
    std::vector<sbe::value::SlotVector> inputSlots;
    for (auto& p : inputStagesAndSlots) {
        auto& stage = p.first;
        auto& outputs = p.second;
        inputStages.push_back(std::move(stage));
        inputSlots.push_back(getSlotsOrderedByName(reqs, outputs));
    }

    // Branch output slots become the input slots to the union.
    auto unionStage = sbe::makeS<sbe::UnionStage>(
        std::move(inputStages), std::move(inputSlots), std::move(unionOutputSlots), root->nodeId());

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

    auto childReqs = reqs.copyForChild();

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
        const auto slotId = outputs.get(
            std::make_pair(PlanStageSlots::kField, shardKeyPatternElt.fieldNameStringData()));

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

    // If we're not required to produce a result object, then instead we can request a slot from
    // the child for each of the fields which constitute the shard key. This allows us to avoid
    // materializing an intermediate object for plans where shard filtering can be performed based
    // on the contents of index keys.
    //
    // We only apply this optimization in the special case that the child QSN is an IXSCAN, since in
    // this case we can request exactly the fields we need according to their position in the index
    // key pattern.
    if (!reqs.hasResult() && childIsIndexScan) {
        return buildShardFilterCovered(root, reqs);
    }

    auto childReqs = reqs.copyForChild();

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

    return {sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), std::move(shardFilterExpression), root->nodeId()),
            std::move(outputs)};
}

namespace {
// Return true iff 'wfStmt' is a $topN or $bottomN operator.
bool isTopBottomN(const WindowFunctionStatement& wfStmt) {
    auto opName = wfStmt.expr->getOpName();
    return opName == AccumulatorTopBottomN<kTop, true>::getName() ||
        opName == AccumulatorTopBottomN<kBottom, true>::getName() ||
        opName == AccumulatorTopBottomN<kTop, false>::getName() ||
        opName == AccumulatorTopBottomN<kBottom, false>::getName();
}

// Return true iff 'wfStmt' is one of $topN, $bottomN, $minN, $maxN, $firstN or $lastN.
bool isAccumulatorN(const WindowFunctionStatement& wfStmt) {
    auto opName = wfStmt.expr->getOpName();
    return opName == AccumulatorTopBottomN<kTop, true>::getName() ||
        opName == AccumulatorTopBottomN<kBottom, true>::getName() ||
        opName == AccumulatorTopBottomN<kTop, false>::getName() ||
        opName == AccumulatorTopBottomN<kBottom, false>::getName() ||
        opName == AccumulatorFirstN::getName() || opName == AccumulatorLastN::getName() ||
        opName == AccumulatorMaxN::getName() || opName == AccumulatorMinN::getName();
}

const Expression* getNExprFromAccumulatorN(const WindowFunctionStatement& wfStmt) {
    auto opName = wfStmt.expr->getOpName();
    if (opName == AccumulatorTopBottomN<kTop, true>::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionTop,
                                                         AccumulatorTopBottomN<kTop, true>>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorTopBottomN<kBottom, true>::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionBottom,
                                                         AccumulatorTopBottomN<kBottom, true>>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorTopBottomN<kTop, false>::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionTopN,
                                                         AccumulatorTopBottomN<kTop, false>>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorTopBottomN<kBottom, false>::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionBottomN,
                                                         AccumulatorTopBottomN<kBottom, false>>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorFirstN::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionFirstN, AccumulatorFirstN>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorLastN::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionLastN, AccumulatorLastN>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorMaxN::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionMaxN, AccumulatorMaxN>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorMinN::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionMinN, AccumulatorMinN>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else {
        MONGO_UNREACHABLE;
    }
}

std::unique_ptr<sbe::EExpression> getSortSpecFromSortPattern(SortPattern sortPattern) {
    auto serialisedSortPattern =
        sortPattern.serialize(SortPattern::SortKeySerialization::kForExplain).toBson();
    auto sortSpec = std::make_unique<sbe::SortSpec>(serialisedSortPattern);
    auto sortSpecExpr = makeConstant(sbe::value::TypeTags::sortSpec,
                                     sbe::value::bitcastFrom<sbe::SortSpec*>(sortSpec.release()));
    return sortSpecExpr;
}

std::unique_ptr<sbe::EExpression> getSortSpecFromTopBottomN(const WindowFunctionStatement& wfStmt) {
    if (wfStmt.expr->getOpName() == AccumulatorTopBottomN<kTop, true>::getName()) {
        return getSortSpecFromSortPattern(
            *dynamic_cast<window_function::ExpressionN<WindowFunctionTop,
                                                       AccumulatorTopBottomN<kTop, true>>*>(
                 wfStmt.expr.get())
                 ->sortPattern);
    } else if (wfStmt.expr->getOpName() == AccumulatorTopBottomN<kBottom, true>::getName()) {
        return getSortSpecFromSortPattern(
            *dynamic_cast<window_function::ExpressionN<WindowFunctionBottom,
                                                       AccumulatorTopBottomN<kBottom, true>>*>(
                 wfStmt.expr.get())
                 ->sortPattern);
    } else if (wfStmt.expr->getOpName() == AccumulatorTopBottomN<kTop, false>::getName()) {
        return getSortSpecFromSortPattern(
            *dynamic_cast<window_function::ExpressionN<WindowFunctionTopN,
                                                       AccumulatorTopBottomN<kTop, false>>*>(
                 wfStmt.expr.get())
                 ->sortPattern);
    } else if (wfStmt.expr->getOpName() == AccumulatorTopBottomN<kBottom, false>::getName()) {
        return getSortSpecFromSortPattern(
            *dynamic_cast<window_function::ExpressionN<WindowFunctionBottomN,
                                                       AccumulatorTopBottomN<kBottom, false>>*>(
                 wfStmt.expr.get())
                 ->sortPattern);
    } else {
        MONGO_UNREACHABLE;
    }
}

std::unique_ptr<sbe::EExpression> getDefaultValueExpr(const WindowFunctionStatement& wfStmt) {
    if (wfStmt.expr->getOpName() == "$shift") {
        auto defaultVal =
            dynamic_cast<window_function::ExpressionShift*>(wfStmt.expr.get())->defaultVal();

        if (defaultVal) {
            auto val = sbe::value::makeValue(*defaultVal);
            return makeConstant(val.first, val.second);
        } else {
            return makeNullConstant();
        }
    } else {
        MONGO_UNREACHABLE;
    }
}

std::tuple<bool, PlanStageReqs, PlanStageReqs> computeChildReqsForWindow(
    const PlanStageReqs& reqs, const WindowNode* windowNode) {
    auto reqFields = reqs.getFields();
    bool reqFieldsHasDottedPaths = std::any_of(reqFields.begin(), reqFields.end(), [](auto&& f) {
        return f.find('.') != std::string::npos;
    });
    if (reqFieldsHasDottedPaths) {
        reqFields = filterVector(std::move(reqFields),
                                 [](auto&& f) { return f.find('.') == std::string::npos; });
    }

    // If the parent requires result, or if reqs.getFields() contained a dotted path, or if
    // 'windowNode->outputFields' contains a dotted path P where 'getTopLevelField(P)' is in
    // 'reqFieldSet', then we need to materialize the result object.
    bool reqResult = [&] {
        if (reqs.hasResult() || reqFieldsHasDottedPaths) {
            return true;
        }
        auto reqFieldSet = StringDataSet(reqFields.begin(), reqFields.end());
        for (const auto& outputField : windowNode->outputFields) {
            const auto& path = outputField.fieldName;
            if (path.find('.') != std::string::npos && reqFieldSet.count(getTopLevelField(path))) {
                return true;
            }
            if (isTopBottomN(outputField)) {
                // We need the materialized result object to generate sort keys for $topN/$bottomN.
                return true;
            }
        }
        return false;
    }();

    auto childReqs =
        reqResult ? reqs.copyForChild().setResultObj() : reqs.copyForChild().clearResult();

    auto forwardingReqs = childReqs.copyForChild();

    childReqs.setFields(getTopLevelFields(windowNode->partitionByRequiredFields));
    childReqs.setFields(getTopLevelFields(windowNode->sortByRequiredFields));
    childReqs.setFields(getTopLevelFields(windowNode->outputRequiredFields));

    return {reqResult, std::move(childReqs), std::move(forwardingReqs)};
}

class WindowStageBuilder {
public:
    using SlotId = sbe::value::SlotId;
    using SlotVector = sbe::value::SlotVector;
    using StringDataEExprMap = StringDataMap<std::unique_ptr<sbe::EExpression>>;
    using BuildOutput =
        std::tuple<SbStage, std::vector<std::string>, SlotVector, StringMap<SlotId>>;

    WindowStageBuilder(StageBuilderState& state,
                       const PlanStageReqs& forwardingReqs,
                       const PlanStageSlots& outputs,
                       const WindowNode* wn,
                       const CanonicalQuery& cq,
                       boost::optional<SlotId> collatorSlot,
                       SlotVector sv)
        : _state(state),
          _slotIdGenerator(*state.slotIdGenerator),
          forwardingReqs(forwardingReqs),
          outputs(outputs),
          rootSlotOpt(outputs.getResultObjIfExists()),
          windowNode(wn),
          _cq(cq),
          collatorSlot(collatorSlot),
          b(state, wn->nodeId()),
          currSlots(std::move(sv)),
          boundTestingSlots(_slotIdGenerator.generateMultiple(currSlots.size())) {}

    BuildOutput build(SbStage stage);

    size_t ensureSlotInBuffer(SlotId slot) {
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
    }

    size_t registerFrameFirstSlots() {
        windowFrameFirstSlots.push_back(SlotVector());
        auto& frameFirstSlots = windowFrameFirstSlots.back();
        frameFirstSlots.clear();
        for (size_t i = 0; i < currSlots.size(); i++) {
            frameFirstSlots.push_back(_slotIdGenerator.generate());
        }
        return windowFrameFirstSlots.size() - 1;
    }

    size_t registerFrameLastSlots() {
        windowFrameLastSlots.push_back(SlotVector());
        auto& frameLastSlots = windowFrameLastSlots.back();
        frameLastSlots.clear();
        for (size_t i = 0; i < currSlots.size(); i++) {
            frameLastSlots.push_back(_slotIdGenerator.generate());
        }
        return windowFrameLastSlots.size() - 1;
    }

    std::pair<SbStage, size_t> generatePartitionExpr(SbStage stage) {
        // Get stages for partition by.
        size_t partitionSlotCount = 0;
        if (windowNode->partitionBy) {
            auto partitionSlot = _slotIdGenerator.generate();
            ensureSlotInBuffer(partitionSlot);
            partitionSlotCount++;
            auto partitionExpr =
                generateExpression(_state, windowNode->partitionBy->get(), rootSlotOpt, outputs);

            // Assert partition slot is not an array.
            auto frameId = _state.frameId();
            auto partitionName = SbVar{frameId, 0};
            partitionExpr = b.makeLet(
                frameId,
                SbExpr::makeSeq(b.makeFillEmptyNull(std::move(partitionExpr))),
                b.makeIf(
                    b.makeFunction("isArray"_sd, partitionName),
                    b.makeFail(
                        ErrorCodes::TypeMismatch,
                        "An expression used to partition cannot evaluate to value of type array"),
                    partitionName));

            stage = sbe::makeProjectStage(std::move(stage),
                                          windowNode->nodeId(),
                                          partitionSlot,
                                          partitionExpr.extractExpr(_state));
        }

        return {std::move(stage), partitionSlotCount};
    }

    void ensureForwardSlotsInBuffer() {
        // Calculate list of forward slots.
        for (auto forwardSlot : getSlotsOrderedByName(forwardingReqs, outputs)) {
            ensureSlotInBuffer(forwardSlot);
        }
    }

    // Calculate slot for document position based window bounds, and add corresponding stages.
    std::tuple<SbStage, SlotId, SlotId> getDocumentBoundSlot(SbStage stage) {
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
        return {std::move(stage), *documentBoundSlot, boundTestingSlots[documentBoundSlotIdx]};
    }

    std::tuple<SbStage, SlotId, SlotId> getSortBySlot(SbStage stage) {
        if (!sortBySlot) {
            sortBySlot = _slotIdGenerator.generate();
            tassert(7914602,
                    "Expected to have a single sort component",
                    windowNode->sortBy && windowNode->sortBy->size() == 1);
            const auto& part = windowNode->sortBy->front();
            auto expCtx = _cq.getExpCtxRaw();
            auto fieldPathExpr = ExpressionFieldPath::createPathFromString(
                expCtx, part.fieldPath->fullPath(), expCtx->variablesParseState);
            auto sortByExpr = generateExpression(_state, fieldPathExpr.get(), rootSlotOpt, outputs)
                                  .extractExpr(_state);
            stage = makeProjectStage(
                std::move(stage), windowNode->nodeId(), *sortBySlot, std::move(sortByExpr));
        }
        auto sortBySlotIdx = ensureSlotInBuffer(*sortBySlot);
        return {std::move(stage), *sortBySlot, boundTestingSlots[sortBySlotIdx]};
    }

    // Calculate slot for range and time range based window bounds
    std::tuple<SbStage, SlotId, SlotId> getRangeBoundSlot(SbStage stage,
                                                          boost::optional<TimeUnit> unit) {
        auto projectRangeBoundSlot = [&](StringData typeCheckFn,
                                         std::unique_ptr<sbe::EExpression> failExpr) {
            auto slot = _slotIdGenerator.generate();
            auto [outStage, sortBySlot, _] = getSortBySlot(std::move(stage));
            stage = std::move(outStage);

            auto checkType = makeLocalBind(
                _state.frameIdGenerator,
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
            return {
                std::move(stage), *timeRangeBoundSlot, boundTestingSlots[timeRangeBoundSlotIdx]};
        } else {
            if (!rangeBoundSlot) {
                rangeBoundSlot = projectRangeBoundSlot(
                    "isNumber",
                    sbe::makeE<sbe::EFail>(
                        ErrorCodes::Error{7993103},
                        "Invalid range: Expected the sortBy field to be a number"));
            }
            auto rangeBoundSlotIdx = ensureSlotInBuffer(*rangeBoundSlot);
            return {std::move(stage), *rangeBoundSlot, boundTestingSlots[rangeBoundSlotIdx]};
        }
    }

    std::vector<std::string> getWindowOutputPaths() {
        std::vector<std::string> windowFields;

        for (size_t i = 0; i < windowNode->outputFields.size(); i++) {
            auto& outputField = windowNode->outputFields[i];
            windowFields.push_back(outputField.fieldName);
        }
        return windowFields;
    }

    bool isWindowRemovable(const WindowBounds& windowBounds) const {
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
            return visit(OverloadedVisitor{isUnboundedBoundRemovable,
                                           isCurrentBoundRemovable,
                                           isValueBoundRemovable},
                         document.lower);
        };
        auto isRangeWindowRemovable = [&](const WindowBounds::RangeBased& range) {
            auto isValueBoundRemovable = [](const Value&) {
                return true;
            };
            return visit(OverloadedVisitor{isUnboundedBoundRemovable,
                                           isCurrentBoundRemovable,
                                           isValueBoundRemovable},
                         range.lower);
        };
        bool removable = visit(OverloadedVisitor{isDocumentWindowRemovable, isRangeWindowRemovable},
                               windowBounds.bounds);
        return removable;
    }

    std::tuple<SbStage, StringDataEExprMap, StringDataEExprMap> generateArgs(
        SbStage stage, const WindowFunctionStatement& outputField, bool removable) {
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
            } else if (isAccumulatorN(outputField)) {
                auto nExprPtr = getNExprFromAccumulatorN(outputField);
                initExprArgs.emplace(
                    AccArgs::kMaxSize,
                    generateExpression(_state, nExprPtr, rootSlotOpt, outputs).extractExpr(_state));
                initExprArgs.emplace(AccArgs::kIsGroupAccum,
                                     makeConstant(sbe::value::TypeTags::Boolean,
                                                  sbe::value::bitcastFrom<bool>(false)));
            } else {
                initExprArgs.emplace("", std::unique_ptr<mongo::sbe::EExpression>(nullptr));
            }
            return initExprArgs;
        }();

        StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs;
        auto accName = outputField.expr->getOpName();

        auto getArgExprFromSBEExpression = [&](std::unique_ptr<sbe::EExpression> argExpr) {
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
        auto getArgExpr = [&](Expression* arg) {
            auto argExpr =
                generateExpression(_state, arg, rootSlotOpt, outputs).extractExpr(_state);
            return getArgExprFromSBEExpression(std::move(argExpr));
        };
        if (accName == "$covarianceSamp" || accName == "$covariancePop") {
            if (auto expr = dynamic_cast<ExpressionArray*>(outputField.expr->input().get());
                expr && expr->getChildren().size() == 2) {
                auto argX = expr->getChildren()[0].get();
                auto argY = expr->getChildren()[1].get();
                argExprs.emplace(AccArgs::kCovarianceX, getArgExpr(argX));
                argExprs.emplace(AccArgs::kCovarianceY, getArgExpr(argY));
            } else if (auto expr =
                           dynamic_cast<ExpressionConstant*>(outputField.expr->input().get());
                       expr && expr->getValue().isArray() &&
                       expr->getValue().getArray().size() == 2) {
                auto array = expr->getValue().getArray();
                auto bson = BSON("x" << array[0] << "y" << array[1]);
                auto [argXTag, argXVal] =
                    sbe::bson::convertFrom<false /* View */>(bson.getField("x"));
                argExprs.emplace(AccArgs::kCovarianceX, makeConstant(argXTag, argXVal));
                auto [argYTag, argYVal] =
                    sbe::bson::convertFrom<false /* View */>(bson.getField("y"));
                argExprs.emplace(AccArgs::kCovarianceY, makeConstant(argYTag, argYVal));
            } else {
                argExprs.emplace(AccArgs::kCovarianceX,
                                 makeConstant(sbe::value::TypeTags::Null, 0));
                argExprs.emplace(AccArgs::kCovarianceY,
                                 makeConstant(sbe::value::TypeTags::Null, 0));
            }
        } else if (accName == "$integral" || accName == "$derivative" || accName == "$linearFill") {
            auto [outStage, sortBySlot, _] = getSortBySlot(std::move(stage));
            stage = std::move(outStage);

            argExprs.emplace(AccArgs::kInput, getArgExpr(outputField.expr->input().get()));
            argExprs.emplace(AccArgs::kSortBy, makeVariable(sortBySlot));
        } else if (accName == "$rank" || accName == "$denseRank") {
            auto isAscending = windowNode->sortBy->front().isAscending;
            argExprs.emplace(AccArgs::kInput, getArgExpr(outputField.expr->input().get()));
            argExprs.emplace(AccArgs::kRankIsAscending,
                             makeConstant(sbe::value::TypeTags::Boolean,
                                          sbe::value::bitcastFrom<bool>(isAscending)));
        } else if (isTopBottomN(outputField)) {
            tassert(8155715, "Root slot should be set", rootSlotOpt);

            auto sortSpecExpr = getSortSpecFromTopBottomN(outputField);
            if (removable) {
                auto key = collatorSlot ? makeFunction("generateSortKey",
                                                       std::move(sortSpecExpr),
                                                       makeVariable(*rootSlotOpt),
                                                       makeVariable(*collatorSlot))
                                        : makeFunction("generateSortKey",
                                                       std::move(sortSpecExpr),
                                                       makeVariable(*rootSlotOpt));

                argExprs.emplace(AccArgs::kTopBottomNKey,
                                 getArgExprFromSBEExpression(std::move(key)));
            } else {
                argExprs.emplace(AccArgs::kTopBottomNSortSpec, sortSpecExpr->clone());

                // Build the key expression
                auto key = collatorSlot ? makeFunction("generateCheapSortKey",
                                                       std::move(sortSpecExpr),
                                                       makeVariable(*rootSlotOpt),
                                                       makeVariable(*collatorSlot))
                                        : makeFunction("generateCheapSortKey",
                                                       std::move(sortSpecExpr),
                                                       makeVariable(*rootSlotOpt));
                argExprs.emplace(AccArgs::kTopBottomNKey,
                                 makeFunction("sortKeyComponentVectorToArray", std::move(key)));
            }

            if (auto expObj = dynamic_cast<ExpressionObject*>(outputField.expr->input().get())) {
                for (auto& [key, value] : expObj->getChildExpressions()) {
                    if (key == AccumulatorN::kFieldNameOutput) {
                        auto outputExpr =
                            generateExpression(_state, value.get(), rootSlotOpt, outputs);
                        argExprs.emplace(AccArgs::kTopBottomNValue,
                                         getArgExprFromSBEExpression(
                                             makeFillEmptyNull(outputExpr.extractExpr(_state))));
                        break;
                    }
                }
            } else if (auto expConst =
                           dynamic_cast<ExpressionConstant*>(outputField.expr->input().get())) {
                auto objConst = expConst->getValue();
                tassert(8155716,
                        str::stream() << accName << " window funciton must have an object argument",
                        objConst.isObject());
                auto objBson = objConst.getDocument().toBson();
                auto outputField = objBson.getField(AccumulatorN::kFieldNameOutput);
                if (outputField.ok()) {
                    auto [outputTag, outputVal] =
                        sbe::bson::convertFrom<false /* View */>(outputField);
                    auto outputExpr = makeConstant(outputTag, outputVal);
                    argExprs.emplace(AccArgs::kTopBottomNValue,
                                     makeFillEmptyNull(std::move(outputExpr)));
                }
            } else {
                tasserted(8155717,
                          str::stream()
                              << accName << " window function must have an object argument");
            }
            tassert(8155718,
                    str::stream() << accName
                                  << " window function must have an output field in the argument",
                    argExprs.find(AccArgs::kTopBottomNValue) != argExprs.end());

        } else {
            argExprs.emplace("", getArgExpr(outputField.expr->input().get()));
        }

        return {std::move(stage), std::move(initExprArgs), std::move(argExprs)};
    }

    SbStage generateInitsAddsAndRemoves(SbStage stage,
                                        const WindowFunctionStatement& outputField,
                                        const AccumulationStatement& accStmt,
                                        bool removable,
                                        StringDataEExprMap initExprArgs,
                                        const StringDataEExprMap& argExprs,
                                        sbe::WindowStage::Window& window) {
        // Create init/add/remove expressions.
        auto cloneExprMap = [](const StringDataMap<std::unique_ptr<sbe::EExpression>>& exprMap) {
            StringDataMap<std::unique_ptr<sbe::EExpression>> exprMapClone;
            for (auto& [argName, argExpr] : exprMap) {
                exprMapClone.emplace(argName, argExpr->clone());
            }
            return exprMapClone;
        };
        if (removable) {
            if (initExprArgs.size() == 1) {
                window.initExprs = buildWindowInit(
                    _state, outputField, std::move(initExprArgs.begin()->second), collatorSlot);
            } else {
                window.initExprs =
                    buildWindowInit(_state, outputField, std::move(initExprArgs), collatorSlot);
            }
            if (argExprs.size() == 1) {
                window.addExprs = buildWindowAdd(
                    _state, outputField, argExprs.begin()->second->clone(), collatorSlot);
                window.removeExprs = buildWindowRemove(
                    _state, outputField, argExprs.begin()->second->clone(), collatorSlot);
            } else {
                window.addExprs =
                    buildWindowAdd(_state, outputField, cloneExprMap(argExprs), collatorSlot);
                window.removeExprs = buildWindowRemove(_state, outputField, cloneExprMap(argExprs));
            }
        } else {
            if (initExprArgs.size() == 1) {
                window.initExprs =
                    buildInitialize(accStmt, std::move(initExprArgs.begin()->second), _state);
            } else {
                window.initExprs = buildInitialize(accStmt, std::move(initExprArgs), _state);
            }
            if (argExprs.size() == 1) {
                window.addExprs = buildAccumulator(
                    accStmt, argExprs.begin()->second->clone(), collatorSlot, _state);
            } else {
                window.addExprs =
                    buildAccumulator(accStmt, cloneExprMap(argExprs), collatorSlot, _state);
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

        return stage;
    }

    void createFrameFirstAndLastSlots(const WindowFunctionStatement& outputField, bool removable) {
        // Create frame first and last slots if the window requires.
        if (outputField.expr->getOpName() == "$derivative") {
            windowFrameFirstSlotIdx.push_back(registerFrameFirstSlots());
            windowFrameLastSlotIdx.push_back(registerFrameLastSlots());
        } else if (outputField.expr->getOpName() == "$first" && removable) {
            windowFrameFirstSlotIdx.push_back(registerFrameFirstSlots());
            windowFrameLastSlotIdx.push_back(boost::none);
        } else if (outputField.expr->getOpName() == "$last" && removable) {
            windowFrameFirstSlotIdx.push_back(boost::none);
            windowFrameLastSlotIdx.push_back(registerFrameLastSlots());
        } else if (outputField.expr->getOpName() == "$shift") {
            windowFrameFirstSlotIdx.push_back(registerFrameFirstSlots());
            windowFrameLastSlotIdx.push_back(boost::none);
        } else {
            windowFrameFirstSlotIdx.push_back(boost::none);
            windowFrameLastSlotIdx.push_back(boost::none);
        }
    }

    SbStage generateBoundExprs(SbStage stage,
                               const WindowFunctionStatement& outputField,
                               const WindowBounds& windowBounds,
                               sbe::WindowStage::Window& window) {
        auto makeOffsetBoundExpr = [&](SlotId boundSlot,
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
        auto makeLowBoundExpr = [&](SlotId boundSlot,
                                    SlotId boundTestingSlot,
                                    std::pair<sbe::value::TypeTags, sbe::value::Value> offset =
                                        {sbe::value::TypeTags::Nothing, 0},
                                    boost::optional<TimeUnit> unit = boost::none) {
            // Use three way comparison to compare special values like NaN.
            return makeBinaryOp(sbe::EPrimBinary::greaterEq,
                                makeBinaryOp(sbe::EPrimBinary::cmp3w,
                                             makeVariable(boundTestingSlot),
                                             makeOffsetBoundExpr(boundSlot, offset, unit)),
                                makeConstant(sbe::value::TypeTags::NumberInt32, 0));
        };
        auto makeHighBoundExpr = [&](SlotId boundSlot,
                                     SlotId boundTestingSlot,
                                     std::pair<sbe::value::TypeTags, sbe::value::Value> offset =
                                         {sbe::value::TypeTags::Nothing, 0},
                                     boost::optional<TimeUnit> unit = boost::none) {
            // Use three way comparison to compare special values like NaN.
            return makeBinaryOp(sbe::EPrimBinary::lessEq,
                                makeBinaryOp(sbe::EPrimBinary::cmp3w,
                                             makeVariable(boundTestingSlot),
                                             makeOffsetBoundExpr(boundSlot, offset, unit)),
                                makeConstant(sbe::value::TypeTags::NumberInt32, 0));
        };
        auto makeLowUnboundedExpr = [&](const WindowBounds::Unbounded&) {
            window.lowBoundExpr = nullptr;
        };
        auto makeHighUnboundedExpr = [&](const WindowBounds::Unbounded&) {
            window.highBoundExpr = nullptr;
        };
        auto makeLowCurrentExpr = [&](const WindowBounds::Current&) {
            auto [outStage, lowBoundSlot, lowBoundTestingSlot] =
                getDocumentBoundSlot(std::move(stage));
            stage = std::move(outStage);

            window.lowBoundExpr = makeLowBoundExpr(lowBoundSlot, lowBoundTestingSlot);
        };
        auto makeHighCurrentExpr = [&](const WindowBounds::Current&) {
            auto [outStage, highBoundSlot, highBoundTestingSlot] =
                getDocumentBoundSlot(std::move(stage));
            stage = std::move(outStage);

            window.highBoundExpr = makeHighBoundExpr(highBoundSlot, highBoundTestingSlot);
        };
        auto documentCase = [&](const WindowBounds::DocumentBased& document) {
            auto makeLowValueExpr = [&](const int& v) {
                auto [outStage, lowBoundSlot, lowBoundTestingSlot] =
                    getDocumentBoundSlot(std::move(stage));
                stage = std::move(outStage);

                window.lowBoundExpr = makeLowBoundExpr(
                    lowBoundSlot,
                    lowBoundTestingSlot,
                    {sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(v)});
            };
            auto makeHighValueExpr = [&](const int& v) {
                auto [outStage, highBoundSlot, highBoundTestingSlot] =
                    getDocumentBoundSlot(std::move(stage));
                stage = std::move(outStage);

                window.highBoundExpr = makeHighBoundExpr(
                    highBoundSlot,
                    highBoundTestingSlot,
                    {sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(v)});
            };
            visit(OverloadedVisitor{makeLowUnboundedExpr, makeLowCurrentExpr, makeLowValueExpr},
                  document.lower);
            visit(OverloadedVisitor{makeHighUnboundedExpr, makeHighCurrentExpr, makeHighValueExpr},
                  document.upper);
        };
        auto rangeCase = [&](const WindowBounds::RangeBased& range) {
            auto [outStage, outRbSlot, outRbTestingSlot] =
                getRangeBoundSlot(std::move(stage), range.unit);

            stage = std::move(outStage);
            auto rangeBoundSlot = std::move(outRbSlot);
            auto rangeBoundTestingSlot = std::move(outRbTestingSlot);

            auto makeLowValueExpr = [&](const Value& v) {
                window.lowBoundExpr = makeLowBoundExpr(
                    rangeBoundSlot, rangeBoundTestingSlot, sbe::value::makeValue(v), range.unit);
            };
            auto makeHighValueExpr = [&](const Value& v) {
                window.highBoundExpr = makeHighBoundExpr(
                    rangeBoundSlot, rangeBoundTestingSlot, sbe::value::makeValue(v), range.unit);
            };
            visit(OverloadedVisitor{makeLowUnboundedExpr, makeLowCurrentExpr, makeLowValueExpr},
                  range.lower);
            visit(OverloadedVisitor{makeHighUnboundedExpr, makeHighCurrentExpr, makeHighValueExpr},
                  range.upper);
        };

        visit(OverloadedVisitor{documentCase, rangeCase}, windowBounds.bounds);

        if (outputField.expr->getOpName() == "$linearFill") {
            tassert(7971215, "expected a single initExpr", window.initExprs.size() == 1);
            window.highBoundExpr =
                makeFunction("aggLinearFillCanAdd", makeVariable(window.windowExprSlots[0]));
        }

        return stage;
    }

    SlotId generateFinalExpr(const WindowFunctionStatement& outputField,
                             const AccumulationStatement& accStmt,
                             bool removable,
                             StringDataEExprMap argExprs,
                             const sbe::WindowStage::Window& window) {
        using ExpressionWithUnit = window_function::ExpressionWithUnit;

        // Build extra arguments for finalize expressions.
        auto getModifiedExpr = [&](std::unique_ptr<sbe::EExpression> argExpr,
                                   SlotVector& newSlots) {
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
            auto u = dynamic_cast<ExpressionWithUnit*>(outputField.expr.get())->unitInMillis();
            auto unit = u ? makeInt64Constant(*u) : makeNullConstant();

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
            finalArgExprs.emplace(AccArgs::kDefaultVal, makeNullConstant());
        } else if (outputField.expr->getOpName() == "$last" && removable) {
            tassert(8085503,
                    str::stream() << "Window function $last expects 1 argument",
                    argExprs.size() == 1);
            auto it = argExprs.begin();
            auto inputExpr = it->second->clone();
            auto& frameLastSlots = windowFrameLastSlots[*windowFrameLastSlotIdx.back()];
            auto frameLastInput = getModifiedExpr(inputExpr->clone(), frameLastSlots);
            finalArgExprs.emplace(AccArgs::kInput, std::move(frameLastInput));
            finalArgExprs.emplace(AccArgs::kDefaultVal, makeNullConstant());
        } else if (outputField.expr->getOpName() == "$linearFill") {
            finalArgExprs = std::move(argExprs);
        } else if (outputField.expr->getOpName() == "$shift") {
            // The window bounds of $shift is DocumentBounds{shiftByPos, shiftByPos}, so it is a
            // window frame of size 1. So $shift is equivalent to $first or $last on the window
            // bound.
            tassert(8293500,
                    str::stream() << "Window function $shift expects 1 argument",
                    argExprs.size() == 1);
            tassert(8293501, "$shift is expected to be removable", removable);
            auto it = argExprs.begin();
            auto& frameFirstSlots = windowFrameFirstSlots[*windowFrameFirstSlotIdx.back()];
            auto inputExpr = it->second->clone();
            auto frameFirstInput = getModifiedExpr(inputExpr->clone(), frameFirstSlots);
            finalArgExprs.emplace(AccArgs::kInput, std::move(frameFirstInput));
            finalArgExprs.emplace(AccArgs::kDefaultVal, getDefaultValueExpr(outputField));
        } else if (isTopBottomN(outputField) && !removable) {
            finalArgExprs.emplace(AccArgs::kTopBottomNSortSpec,
                                  getSortSpecFromTopBottomN(outputField));
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
                                collatorSlot)
                      .extractExpr(_state)
                : buildFinalize(_state, accStmt, window.windowExprSlots, collatorSlot)
                      .extractExpr(_state);
        }

        // Deal with empty window for finalize expressions.
        auto emptyWindowExpr = [&] {
            StringData accExprName = outputField.expr->getOpName();

            if (accExprName == "$sum") {
                return makeConstant(sbe::value::TypeTags::NumberInt32, 0);
            } else if (accExprName == "$push" || accExprName == AccumulatorAddToSet::kName) {
                auto [tag, val] = sbe::value::makeNewArray();
                return makeConstant(tag, val);
            } else if (accExprName == "$shift") {
                return getDefaultValueExpr(outputField);
            } else {
                return makeConstant(sbe::value::TypeTags::Null, 0);
            }
        }();

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
        auto finalSlot = _slotIdGenerator.generate();
        windowFinalProjects.emplace_back(finalSlot, std::move(finalExpr));
        windowFinalSlots.push_back(finalSlot);

        return finalSlot;
    }

private:
    StageBuilderState& _state;
    sbe::value::SlotIdGenerator& _slotIdGenerator;
    const PlanStageReqs& forwardingReqs;
    const PlanStageSlots& outputs;
    boost::optional<TypedSlot> rootSlotOpt;
    const WindowNode* windowNode;
    const CanonicalQuery& _cq;
    boost::optional<SlotId> collatorSlot;
    SbBuilder b;

    SlotVector currSlots;
    SlotVector boundTestingSlots;
    std::vector<SlotVector> windowFrameFirstSlots;
    std::vector<SlotVector> windowFrameLastSlots;

    // Calculate slot for document position based window bounds, and add corresponding stages.
    boost::optional<SlotId> documentBoundSlot;

    // Calculate sort-by slot, and add corresponding stages.
    boost::optional<SlotId> sortBySlot;

    // Calculate slot for range and time range based window bounds
    boost::optional<SlotId> rangeBoundSlot;
    boost::optional<SlotId> timeRangeBoundSlot;

    std::vector<boost::optional<size_t>> windowFrameFirstSlotIdx;
    std::vector<boost::optional<size_t>> windowFrameLastSlotIdx;

    // We project window function input arguments in order to avoid repeated evaluation
    // for both add and remove expressions.
    sbe::SlotExprPairVector windowArgProjects;

    sbe::value::SlotVector windowFinalSlots;
    sbe::SlotExprPairVector windowFinalProjects;
};

WindowStageBuilder::BuildOutput WindowStageBuilder::build(SbStage stage) {
    // Get stages for partition by.
    auto [outStage, partitionSlotCount] = generatePartitionExpr(std::move(stage));
    stage = std::move(outStage);

    // Calculate list of forward slots.
    ensureForwardSlotsInBuffer();

    // Generate list of the window output paths.
    auto windowFields = getWindowOutputPaths();

    // Creating window definitions, including the slots and expressions for the bounds and
    // accumulators.
    std::vector<sbe::WindowStage::Window> windows;
    StringMap<SlotId> outputPathMap;

    for (size_t i = 0; i < windowNode->outputFields.size(); i++) {
        auto& outputField = windowNode->outputFields[i];

        WindowBounds windowBounds = outputField.expr->bounds();

        // Check whether window is removable or not.
        bool removable = isWindowRemovable(windowBounds);

        // Create a fake accumulation statement for non-removable window bounds.
        auto accStmt = createFakeAccumulationStatement(_state, outputField);

        auto [outStage, initExprArgs, argExprs] =
            generateArgs(std::move(stage), outputField, removable);
        stage = std::move(outStage);

        sbe::WindowStage::Window window{};

        // Create init/add/remove expressions.
        stage = generateInitsAddsAndRemoves(std::move(stage),
                                            outputField,
                                            accStmt,
                                            removable,
                                            std::move(initExprArgs),
                                            argExprs,
                                            window);

        // Create frame first and last slots if the window requires.
        createFrameFirstAndLastSlots(outputField, removable);

        // Build bound expressions.
        stage = generateBoundExprs(std::move(stage), outputField, windowBounds, window);

        // Build extra arguments for finalize expressions.
        auto finalSlot =
            generateFinalExpr(outputField, accStmt, removable, std::move(argExprs), window);

        // Append the window definition to the end of the 'windows' vector.
        windows.emplace_back(std::move(window));

        // If 'outputField' is not a dotted path, add 'outputField' and its corresponding slot
        // to 'outputPathMap'.
        if (outputField.fieldName.find('.') == std::string::npos) {
            outputPathMap.emplace(outputField.fieldName, finalSlot);
        }
    }

    if (windowArgProjects.size() > 0) {
        stage = sbe::makeS<sbe::ProjectStage>(
            std::move(stage), std::move(windowArgProjects), windowNode->nodeId());
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

    return {std::move(stage),
            std::move(windowFields),
            std::move(windowFinalSlots),
            std::move(outputPathMap)};
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildWindow(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto windowNode = static_cast<const WindowNode*>(root);

    auto [reqResult, childReqs, forwardingReqs] = computeChildReqsForWindow(reqs, windowNode);

    auto child = root->children[0].get();
    auto childStageOutput = build(child, childReqs);
    auto stage = std::move(childStageOutput.first);
    auto outputs = std::move(childStageOutput.second);

    auto collatorSlot = _state.getCollatorSlot();

    auto sv = _data->metadataSlots.getSlotVector();

    // Create a WindowStageBuilder and call the build() method on it. This will generate all
    // the SBE expressions and SBE stages needed to implement the window stage.
    WindowStageBuilder builder(
        _state, forwardingReqs, outputs, windowNode, _cq, collatorSlot, std::move(sv));

    auto [outStage, windowFields, windowFinalSlots, outputPathMap] =
        builder.build(std::move(stage));
    stage = std::move(outStage);

    // Update the kField slots in 'outputs' to reflect the effects of this stage.
    for (auto&& windowField : windowFields) {
        outputs.clearFieldAndAllPrefixes(windowField);
    }
    for (const auto& [field, slot] : outputPathMap) {
        outputs.set(std::make_pair(PlanStageSlots::kField, field), slot);
    }

    // Produce a materialized result object if needed.
    if (reqResult) {
        SbBuilder b(_state, windowNode->nodeId());

        std::vector<ProjectNode> nodes;
        for (size_t i = 0; i < windowFields.size(); ++i) {
            nodes.emplace_back(SbExpr{windowFinalSlots[i]});
        }

        // Call generateProjection() to produce the output object.
        auto projType = projection_ast::ProjectType::kAddition;
        auto projectionExpr = generateProjection(
            _state, projType, std::move(windowFields), std::move(nodes), outputs.getResultObj());

        auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(projectionExpr));
        stage = std::move(outStage);

        outputs.setResultObj(outSlots[0]);
    }

    return {std::move(stage), std::move(outputs)};
}  // buildWindow

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSearchMeta(
    const SearchNode* root,
    StageBuilderState& state,
    const CanonicalQuery& cq,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    Environment& env,
    PlanYieldPolicySBE* const yieldPolicy) {
    auto expCtx = cq.getExpCtxRaw();
    PlanStageSlots outputs;

    if (!expCtx->needsMerge) {
        auto searchMetaSlot = state.getBuiltinVarSlot(Variables::kSearchMetaId);
        auto stage = sbe::makeS<sbe::FilterStage<true>>(
            makeLimitCoScanTree(root->nodeId(), 1),
            makeFunction("exists"_sd, makeVariable(*searchMetaSlot)),
            root->nodeId());
        outputs.setResultObj(*searchMetaSlot);
        return {std::move(stage), std::move(outputs)};
    }

    auto searchResultSlot = slotIdGenerator->generate();

    auto stage = sbe::SearchCursorStage::createForMetadata(expCtx->ns,
                                                           expCtx->uuid,
                                                           searchResultSlot,
                                                           root->remoteCursorId,
                                                           yieldPolicy,
                                                           root->nodeId());
    outputs.setResultObj(searchResultSlot);
    state.data->cursorType = CursorTypeEnum::SearchMetaResult;

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::vector<std::string>, sbe::value::SlotVector>
SlotBasedStageBuilder::buildSearchMetadataSlots() {
    std::vector<std::string> metadataNames;
    sbe::value::SlotVector metadataSlots;

    const QueryMetadataBitSet& metadataBit = _cq.searchMetadata();
    if (metadataBit.test(DocumentMetadataFields::MetaType::kSearchScore) || _state.needsMerge) {
        metadataNames.push_back(Document::metaFieldSearchScore.toString());
        metadataSlots.push_back(_slotIdGenerator.generate());
        _data->metadataSlots.searchScoreSlot = metadataSlots.back();
    }

    if (metadataBit.test(DocumentMetadataFields::MetaType::kSearchHighlights) ||
        _state.needsMerge) {
        metadataNames.push_back(Document::metaFieldSearchHighlights.toString());
        metadataSlots.push_back(_slotIdGenerator.generate());
        _data->metadataSlots.searchHighlightsSlot = metadataSlots.back();
    }

    if (metadataBit.test(DocumentMetadataFields::MetaType::kSearchScoreDetails) ||
        _state.needsMerge) {
        metadataNames.push_back(Document::metaFieldSearchScoreDetails.toString());
        metadataSlots.push_back(_slotIdGenerator.generate());
        _data->metadataSlots.searchDetailsSlot = metadataSlots.back();
    }

    if (metadataBit.test(DocumentMetadataFields::MetaType::kSearchSortValues) ||
        _state.needsMerge) {
        metadataNames.push_back(Document::metaFieldSearchSortValues.toString());
        metadataSlots.push_back(_slotIdGenerator.generate());
        _data->metadataSlots.searchSortValuesSlot = metadataSlots.back();
    }

    if (metadataBit.test(DocumentMetadataFields::MetaType::kSearchSequenceToken) ||
        _state.needsMerge) {
        metadataNames.push_back(Document::metaFieldSearchSequenceToken.toString());
        metadataSlots.push_back(_slotIdGenerator.generate());
        _data->metadataSlots.searchSequenceToken = metadataSlots.back();
    }

    return {metadataNames, metadataSlots};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSearch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto sn = static_cast<const SearchNode*>(root);
    if (sn->isSearchMeta) {
        return buildSearchMeta(sn, _state, _cq, &_slotIdGenerator, _env, _yieldPolicy);
    }

    const auto& collection = getCurrentCollection(reqs);
    auto expCtx = _cq.getExpCtxRaw();

    // Register search query parameter slots.
    auto limitSlot = _env->registerSlot("searchLimit"_sd,
                                        sbe::value::TypeTags::Nothing,
                                        0 /* val */,
                                        false /* owned */,
                                        &_slotIdGenerator);

    auto sortSpecSlot = _env->registerSlot(
        "searchSortSpec"_sd, sbe::value::TypeTags::Nothing, 0 /* val */, false, &_slotIdGenerator);

    bool isStoredSource = sn->searchQuery.getBoolField(kReturnStoredSourceArg);

    auto topLevelFields = getTopLevelFields(reqs.getFields());
    auto topLevelFieldSlots = _slotIdGenerator.generateMultiple(topLevelFields.size());

    PlanStageSlots outputs;

    for (size_t i = 0; i < topLevelFields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, topLevelFields[i]),
                    topLevelFieldSlots[i]);
    }

    // Search cursor stage output slots
    auto searchResultSlot = isStoredSource && reqs.hasResult()
        ? boost::make_optional(_slotIdGenerator.generate())
        : boost::none;
    // Register the $$SEARCH_META slot.
    _state.getBuiltinVarSlot(Variables::kSearchMetaId);

    auto [metadataNames, metadataSlots] = buildSearchMetadataSlots();

    _data->metadataSlots.sortKeySlot = _slotIdGenerator.generate();

    auto collatorSlot = _state.getCollatorSlot();
    if (isStoredSource) {
        if (searchResultSlot) {
            outputs.setResultObj(searchResultSlot.value());
        }

        return {sbe::SearchCursorStage::createForStoredSource(expCtx->ns,
                                                              expCtx->uuid,
                                                              searchResultSlot,
                                                              metadataNames,
                                                              metadataSlots,
                                                              topLevelFields,
                                                              topLevelFieldSlots,
                                                              sn->remoteCursorId,
                                                              sortSpecSlot,
                                                              limitSlot,
                                                              _data->metadataSlots.sortKeySlot,
                                                              collatorSlot,
                                                              _yieldPolicy,
                                                              sn->nodeId()),
                std::move(outputs)};
    }

    auto idSlot = _slotIdGenerator.generate();
    auto searchCursorStage =
        sbe::SearchCursorStage::createForNonStoredSource(expCtx->ns,
                                                         expCtx->uuid,
                                                         idSlot,
                                                         metadataNames,
                                                         metadataSlots,
                                                         sn->remoteCursorId,
                                                         sortSpecSlot,
                                                         limitSlot,
                                                         _data->metadataSlots.sortKeySlot,
                                                         collatorSlot,
                                                         _yieldPolicy,
                                                         sn->nodeId());

    // Make a project stage to convert '_id' field value into keystring.
    auto catalog = collection->getIndexCatalog();
    auto indexDescriptor = catalog->findIndexByName(_state.opCtx, kIdIndexName);
    auto indexAccessMethod = catalog->getEntry(indexDescriptor)->accessMethod()->asSortedData();
    auto sortedData = indexAccessMethod->getSortedDataInterface();
    auto version = sortedData->getKeyStringVersion();
    auto ordering = sortedData->getOrdering();

    auto makeNewKeyFunc = [&](key_string::Discriminator discriminator) {
        StringData functionName = collatorSlot ? "collKs" : "ks";
        sbe::EExpression::Vector args;
        args.emplace_back(makeInt64Constant(static_cast<int64_t>(version)));
        args.emplace_back(makeInt32Constant(ordering.getBits()));
        args.emplace_back(makeVariable(idSlot));
        args.emplace_back(makeInt64Constant(static_cast<int64_t>(discriminator)));
        if (collatorSlot) {
            args.emplace_back(makeVariable(*collatorSlot));
        }
        return makeE<sbe::EFunction>(functionName, std::move(args));
    };

    auto childReqs = reqs.copyForChild()
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
    outputs.setResultObj(outputDocSlot);
    // Slot stores rid if it it found, in our case same as seekRecordIdSlot.
    auto ridSlot = _slotIdGenerator.generate();

    // Join the idx scan stage with fetch stage.
    auto fetchStage = makeLoopJoinForFetch(std::move(idxScanStage),
                                           outputDocSlot,
                                           ridSlot,
                                           topLevelFields,
                                           topLevelFieldSlots,
                                           idxOutputs.get(kRecordId),
                                           idxOutputs.get(kSnapshotId),
                                           idxOutputs.get(kIndexIdent),
                                           idxOutputs.get(kIndexKey),
                                           idxOutputs.get(kIndexKeyPattern),
                                           collection,
                                           sn->nodeId(),
                                           sbe::makeSV() /* slotsToForward */);

    // Join the search_cursor+project stage with idx_scan+fetch stage.
    auto outerProjVec = metadataSlots;
    outerProjVec.push_back(*_data->metadataSlots.sortKeySlot);
    auto stage = sbe::makeS<sbe::LoopJoinStage>(
        std::move(searchCursorStage),
        sbe::makeS<sbe::LimitSkipStage>(
            std::move(fetchStage), makeInt64Constant(1), nullptr /* skip */, sn->nodeId()),
        std::move(outerProjVec),
        sbe::makeSV(idSlot),
        nullptr /* predicate */,
        sn->nodeId());

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, bool> SlotBasedStageBuilder::buildVectorizedFilterExpr(
    std::unique_ptr<sbe::PlanStage> stage,
    const PlanStageReqs& reqs,
    SbExpr scalarFilterExpression,
    PlanStageSlots& outputs,
    PlanNodeId nodeId) {
    // Attempt to vectorize the filter expression.
    auto vectorizedFilterExpression =
        buildVectorizedExpr(_state, std::move(scalarFilterExpression), outputs, true);

    if (vectorizedFilterExpression) {
        // Vectorisation was possible.
        auto typeSig = vectorizedFilterExpression.getTypeSignature();

        if (vectorizedFilterExpression.isConstantExpr()) {
            auto [tag, val] = vectorizedFilterExpression.getConstantValue();
            // The expression is a scalar constant, it must be a boolean value.
            tassert(8333500,
                    "Expected true or false value for filter",
                    tag == sbe::value::TypeTags::Boolean);
            if (sbe::value::bitcastTo<bool>(val)) {
                LOGV2_DEBUG(8333501, 1, "Trivially true boolean expression is ignored");
            } else {
                stage = makeS<sbe::FilterStage<true>>(
                    std::move(stage),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                               sbe::value::bitcastFrom<bool>(false)),
                    nodeId);
            }
        } else if (typeSig &&
                   TypeSignature::kBlockType.include(TypeSignature::kBooleanType)
                       .isSubset(*typeSig)) {
            // The vectorised filter expression should return a block of boolean values. We will
            // project this block in a slot with a special type
            // (PlanStageSlots::kBlockSelectivityBitmap) so that later stages know where to find it.

            // Add a project stage to project the boolean block to a slot.
            sbe::value::SlotId bitmapSlotId = _state.slotId();
            sbe::SlotExprPairVector projects;

            auto incomingBitmapSlotId =
                outputs.getSlotIfExists(PlanStageSlots::kBlockSelectivityBitmap);
            if (incomingBitmapSlotId) {
                SbExprBuilder sb(_state);
                auto sbExpr = sb.makeFunction("valueBlockLogicalAnd"_sd,
                                              sb.makeVariable(SbVar{*incomingBitmapSlotId}),
                                              std::move(vectorizedFilterExpression));

                projects.emplace_back(bitmapSlotId, sbExpr.extractExpr(_state));
            } else {
                projects.emplace_back(bitmapSlotId, vectorizedFilterExpression.extractExpr(_state));
            }

            stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);

            // Use the result as the bitmap for the BlockToRow stage.
            outputs.set(PlanStageSlots::kBlockSelectivityBitmap,
                        TypedSlot{bitmapSlotId,
                                  TypeSignature::kBlockType.include(TypeSignature::kBooleanType)});

            // Add a filter stage that pulls new data if there isn't at least one 'true' value
            // in the produced bitmap.
            SbExprBuilder b(_state);
            auto filterSbExpr = b.makeNot(b.makeFunction("valueBlockNone"_sd,
                                                         b.makeVariable(SbVar{bitmapSlotId}),
                                                         b.makeBoolConstant(true)));
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterSbExpr.extractExpr(_state), nodeId);
        } else {
            // The vectorised expression returns a scalar result.
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), vectorizedFilterExpression.extractExpr(_state), nodeId);
        }

        // The vectorised execution should stop if the caller cannot process blocks or the stage
        // needs to return a scalar result document.
        if (reqs.hasResult() || !reqs.getCanProcessBlockValues()) {
            stage = buildBlockToRow(std::move(stage), _state, outputs);
        }

        return {std::move(stage), true};
    } else {
        // It is not possible to create the vectorised expression. Convert block to row and
        // continue with the scalar filter expression.
        stage = buildBlockToRow(std::move(stage), _state, outputs);
        return {std::move(stage), false};
    }
}

const CollectionPtr& SlotBasedStageBuilder::getCurrentCollection(const PlanStageReqs& reqs) const {
    auto nss = reqs.getTargetNamespace();
    const auto& coll = _collections.lookupCollection(nss);
    tassert(7922500,
            str::stream() << "No collection found that matches namespace '"
                          << nss.toStringForErrorMsg() << "'",
            coll != CollectionPtr::null);
    return coll;
}

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::build(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    // Define the 'builderCallback' typedef.
    typedef std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> (
        SlotBasedStageBuilder::*builderCallback)(const QuerySolutionNode*, const PlanStageReqs&);

    static const stdx::unordered_map<StageType, builderCallback> kStageBuilders = {
        {STAGE_COLLSCAN, &SlotBasedStageBuilder::buildCollScan},
        {STAGE_COUNT_SCAN, &SlotBasedStageBuilder::buildCountScan},
        {STAGE_VIRTUAL_SCAN, &SlotBasedStageBuilder::buildVirtualScan},
        {STAGE_IXSCAN, &SlotBasedStageBuilder::buildIndexScan},
        {STAGE_COLUMN_SCAN, &SlotBasedStageBuilder::buildColumnScan},
        {STAGE_FETCH, &SlotBasedStageBuilder::buildFetch},
        {STAGE_LIMIT, &SlotBasedStageBuilder::buildLimit},
        {STAGE_MATCH, &SlotBasedStageBuilder::buildMatch},
        {STAGE_UNWIND, &SlotBasedStageBuilder::buildUnwind},
        {STAGE_REPLACE_ROOT, &SlotBasedStageBuilder::buildReplaceRoot},
        {STAGE_SKIP, &SlotBasedStageBuilder::buildSkip},
        {STAGE_SORT_SIMPLE, &SlotBasedStageBuilder::buildSort},
        {STAGE_SORT_DEFAULT, &SlotBasedStageBuilder::buildSort},
        {STAGE_SORT_KEY_GENERATOR, &SlotBasedStageBuilder::buildSortKeyGenerator},
        {STAGE_PROJECTION_SIMPLE, &SlotBasedStageBuilder::buildProjection},
        {STAGE_PROJECTION_DEFAULT, &SlotBasedStageBuilder::buildProjection},
        {STAGE_PROJECTION_COVERED, &SlotBasedStageBuilder::buildProjection},
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
        {STAGE_EQ_LOOKUP, &SlotBasedStageBuilder::buildEqLookup},
        {STAGE_EQ_LOOKUP_UNWIND, &SlotBasedStageBuilder::buildEqLookupUnwind},
        {STAGE_SHARDING_FILTER, &SlotBasedStageBuilder::buildShardFilter},
        {STAGE_SEARCH, &SlotBasedStageBuilder::buildSearch},
        {STAGE_WINDOW, &SlotBasedStageBuilder::buildWindow},
        {STAGE_UNPACK_TS_BUCKET, &SlotBasedStageBuilder::buildUnpackTsBucket}};

    tassert(4822884,
            str::stream() << "Unsupported QSN in SBE stage builder: " << root->toString(),
            kStageBuilders.find(root->getType()) != kStageBuilders.end());

    auto stageType = root->getType();

    // If this plan is for a tailable cursor scan, and we're not already in the process of building
    // a special union sub-tree implementing such scans, then start building a union sub-tree. Note
    // that LIMIT or SKIP stage is used as a splitting point of the two union branches, if present,
    // because we need to apply limit (or skip) only in the initial scan (in the anchor branch), and
    // the resume branch should not have it.
    switch (stageType) {
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

    auto [stage, slots] = (this->*(kStageBuilders.at(stageType)))(root, reqs);
    auto outputs = std::move(slots);

    bool reqResultInfo = reqs.hasResultInfo();

    if (reqResultInfo) {
        tassert(8146611,
                str::stream() << "Expected build() for " << stageTypeToString(stageType)
                              << " to produce a result object or ResultInfo",
                outputs.hasResult());
    }

    // Check if there are any required kField slots that are missing.
    std::vector<std::string> missingFields;

    auto names = outputs.getRequiredNamesInOrder(reqs);

    for (const auto& name : names) {
        if (name.first == kField && !outputs.has(name)) {
            missingFields.emplace_back(name.second);
        }
    }

    // If any required kFields slots are missing, populate them (or throw an error if there is not
    // a materialized result object or a compatible result base object to retrieve them from).
    if (!missingFields.empty()) {
        if (!outputs.hasResultObj()) {
            for (auto&& f : missingFields) {
                tassert(6023424,
                        str::stream()
                            << "Expected build() for " << stageTypeToString(stageType)
                            << " to either satisfy all kField reqs, provide a materialized "
                            << "result object, or provide a compatible result base object",
                        reqs.hasResultInfo() && reqs.getResultInfoAllowedSet().count(f) &&
                            outputs.hasResultInfo() && outputs.getResultInfoChanges().isKeep(f));
            }
        }

        auto [outStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                         missingFields,
                                                         outputs.get(PlanStageSlots::kResult),
                                                         root->nodeId(),
                                                         &_slotIdGenerator,
                                                         _state,
                                                         &outputs);

        stage = std::move(outStage);

        for (size_t i = 0; i < missingFields.size(); ++i) {
            outputs.set(std::make_pair(PlanStageSlots::kField, std::move(missingFields[i])),
                        outSlots[i]);
        }
    }

    if (reqResultInfo) {
        // If 'outputs' has a materialized result and 'reqs' was expecting ResultInfo,
        // then convert the materialized result into a ResultInfo.
        if (!outputs.hasResultInfo()) {
            outputs.setResultInfoBaseObj(outputs.getResultObj());
        }
    }

    if (root->metadataExhausted()) {
        // Metadata is exhausted by current node, later nodes/stages won't see metadata from input.
        _data->metadataSlots.reset();
    }

    // Clear non-required slots (excluding ~10 stages to preserve legacy behavior for now),
    // and also clear ResultInfo if it's not required.
    bool clearSlots = stageType != STAGE_VIRTUAL_SCAN && stageType != STAGE_COLUMN_SCAN &&
        stageType != STAGE_LIMIT && stageType != STAGE_SKIP && stageType != STAGE_TEXT_MATCH &&
        stageType != STAGE_RETURN_KEY && stageType != STAGE_AND_HASH &&
        stageType != STAGE_AND_SORTED && stageType != STAGE_GROUP && stageType != STAGE_SEARCH &&
        stageType != STAGE_UNPACK_TS_BUCKET;

    if (clearSlots) {
        // To preserve legacy behavior, in some cases we unconditionally retain the result object.
        bool saveResultObj = stageType != STAGE_SORT_SIMPLE && stageType != STAGE_SORT_DEFAULT &&
            stageType != STAGE_PROJECTION_SIMPLE && stageType != STAGE_PROJECTION_COVERED &&
            stageType != STAGE_PROJECTION_DEFAULT;

        outputs.clearNonRequiredSlots(reqs, saveResultObj);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
