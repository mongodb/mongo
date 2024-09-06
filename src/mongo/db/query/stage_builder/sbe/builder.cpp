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

#include "mongo/db/query/stage_builder/sbe/builder.h"

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
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/query/shard_filterer_factory_impl.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/query/stage_builder/sbe/abt_holder_impl.h"
#include "mongo/db/query/stage_builder/sbe/gen_abt_helpers.h"
#include "mongo/db/query/stage_builder/sbe/gen_accumulator.h"
#include "mongo/db/query/stage_builder/sbe/gen_coll_scan.h"
#include "mongo/db/query/stage_builder/sbe/gen_expression.h"
#include "mongo/db/query/stage_builder/sbe/gen_filter.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/gen_index_scan.h"
#include "mongo/db/query/stage_builder/sbe/gen_projection.h"
#include "mongo/db/query/stage_builder/sbe/gen_window_function.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
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

                if (metaValObj.hasField(mongot_cursor::kSlowQueryLogFieldName)) {
                    auto& opDebug = CurOp::get(cq.getOpCtx())->debug();
                    opDebug.mongotSlowQueryLog =
                        metaValObj.getField(mongot_cursor::kSlowQueryLogFieldName)
                            .wrap(mongot_cursor::kSlowQueryLogFieldName);
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
    for (const SbSlot& slot : requiredNamedSlots) {
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
        for (const SbSlot& slot : requiredNamedSlots) {
            if (!state.env->isSlotRegistered(slot.slotId)) {
                slots.emplace_back(slot.slotId);
            }
        }
    } else {
        auto excludeSet = sbe::value::SlotSet{exclude.begin(), exclude.end()};

        for (const SbSlot& slot : requiredNamedSlots) {
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

PlanStageSlots PlanStageSlots::makeMergedPlanStageSlots(
    StageBuilderState& state,
    PlanNodeId nodeId,
    const PlanStageReqs& reqs,
    std::vector<PlanStageTree>& trees,
    const std::vector<const FieldSet*>& postimageAllowedFieldSets) {
    tassert(8146604, "Expected 'trees' to be non-empty", !trees.empty());

    PlanStageSlots outputs;

    if (reqs.hasResultInfo()) {
        // Merge the childeren's result infos.
        mergeResultInfos(state, nodeId, reqs, trees, postimageAllowedFieldSets);
    }

    if (reqs.hasResultObj()) {
        // Assert each tree produces a materialized result.
        for (auto& tree : trees) {
            bool hasResultObject = tree.second.hasResultObj();
            tassert(8378200, "Expected child tree to produce a result object", hasResultObject);
        }
    }

    auto& firstTreeOutputs = trees.front().second;

    for (const auto& slotName : firstTreeOutputs.getRequiredNamesInOrder(reqs)) {
        outputs._data->slotNameToIdMap[slotName] = SbSlot{state.slotId()};
    }

    if (reqs.hasResultInfo()) {
        // If 'reqs' requires ResultInfo, call setResultInfoBaseObj() to properly set a
        // ResultInfo base object on 'outputs'.
        outputs.setResultInfoBaseObj(outputs.get(kResult));

        // Copy over the ResultInfo changes from the first child to 'outputs'.
        outputs._data->resultInfoChanges.emplace(*firstTreeOutputs._data->resultInfoChanges);
    } else if (reqs.hasResultObj()) {
        tassert(8428006, "Expected result object to be set", outputs.hasResultObj());
    }

    return outputs;
}

void PlanStageSlots::addEffectsToResultInfo(StageBuilderState& state,
                                            const PlanStageReqs& reqs,
                                            const FieldEffects& newEffectsIn) {
    tassert(8323500, "Expected ResultInfo to be set", hasResultInfo());
    tassert(8323501, "Expected ResultInfo requirement to be set", reqs.hasResultInfo());

    FieldEffects newEffects = newEffectsIn;
    newEffects.narrow(reqs.getResultInfoTrackedFieldSet());

    // Compose 'resultInfoChanges' with 'newEffects' and store the result back into
    // 'resultInfoChanges'.
    _data->resultInfoChanges->compose(newEffects);
}

namespace {
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

std::vector<ProjectNode> getTransformedNodesForCoveredProjection(
    const std::vector<std::string>& paths, const PlanStageSlots& outputs) {
    // Build a vector of SbExpr ProjectNodes that refer directly to the corresponding kField slots.
    std::vector<ProjectNode> newNodes;
    newNodes.reserve(paths.size());
    for (const auto& path : paths) {
        newNodes.emplace_back(outputs.get(std::pair(PlanStageSlots::kField, StringData{path})));
    }

    return newNodes;
}
}  // namespace

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

    // If this PlanStageSlots has ResultInfo and 'reqs.hasResult()' is true, check if there are
    // any additional names that should be added to 'names'.
    if (reqs.hasResult() && hasResultInfo()) {
        // Get the list of changed fields and add them to 'names'.
        auto changed = _data->resultInfoChanges->getChangedFields();
        tassert(8323503,
                "Expected FieldSet to be closed",
                changed.getScope() == FieldListScope::kClosed);

        for (auto&& fieldName : changed.getList()) {
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

SbSlotVector PlanStageSlots::getRequiredSlotsInOrder(const PlanStageReqs& reqs) const {
    auto names = getRequiredNamesInOrder(reqs);

    // Build the list of corresponding slots.
    SbSlotVector result;
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

struct NameSbSlotPairLt {
    using UnownedSlotName = PlanStageSlots::UnownedSlotName;
    using PairType = std::pair<UnownedSlotName, SbSlot>;

    bool operator()(const PairType& lhs, const PairType& rhs) const {
        return lhs.first != rhs.first ? lhs.first < rhs.first
                                      : SbSlot::Less()(lhs.second, rhs.second);
    }
};

struct NameSbSlotPairEq {
    using UnownedSlotName = PlanStageSlots::UnownedSlotName;
    using PairType = std::pair<UnownedSlotName, SbSlot>;

    bool operator()(const PairType& lhs, const PairType& rhs) const {
        return lhs.first == rhs.first && SbSlot::EqualTo()(lhs.second, rhs.second);
    }
};

SbSlotVector PlanStageSlots::getRequiredSlotsUnique(const PlanStageReqs& reqs) const {
    auto names = getRequiredNamesInOrder(reqs);

    SbSlotVector result;

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
    std::sort(result.begin(), result.end(), SbSlot::Less());

    auto newEnd = std::unique(result.begin(), result.end(), SbSlot::EqualTo());
    if (newEnd != result.end()) {
        result.erase(newEnd, result.end());
    }

    return result;
}

std::vector<std::pair<PlanStageSlots::UnownedSlotName, SbSlot>>
PlanStageSlots::getAllNameSlotPairsInOrder() const {
    std::vector<std::pair<UnownedSlotName, SbSlot>> nameSlotPairs;
    nameSlotPairs.reserve(_data->slotNameToIdMap.size());

    for (auto& p : _data->slotNameToIdMap) {
        nameSlotPairs.emplace_back(p.first, p.second);
    }

    std::sort(nameSlotPairs.begin(), nameSlotPairs.end(), NameSbSlotPairLt());

    return nameSlotPairs;
}

SbSlotVector PlanStageSlots::getAllSlotsInOrder() const {
    SbSlotVector result;

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
        auto nothingSlot = SbSlot{state.getNothingSlot()};
        setResultObj(nothingSlot);
    }

    auto names = getRequiredNamesInOrder(reqs);

    for (const auto& name : names) {
        if (!has(name)) {
            auto nothingSlot = SbSlot{state.getNothingSlot()};
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

void PlanStageSlots::mergeResultInfos(
    StageBuilderState& state,
    PlanNodeId nodeId,
    const PlanStageReqs& reqs,
    std::vector<PlanStageTree>& trees,
    const std::vector<const FieldSet*>& postimageAllowedFieldSets) {
    tassert(8323504, "Expected 'trees' to be non-empty", !trees.empty());

    SbBuilder b(state, nodeId);

    std::vector<FieldEffects> treesEffects;
    treesEffects.reserve(trees.size());

    for (size_t i = 0; i < trees.size(); ++i) {
        auto& outputs = trees[i].second;

        // Assert that each tree produces either a materialized result object or a ResultInfo.
        tassert(8378201,
                "Expected child tree to produce a result object or a ResultInfo",
                outputs.hasResult());

        // If this tree has a materialized result object, convert it into a ResultInfo with
        // default effects (all "Keeps").
        if (outputs.hasResult()) {
            outputs.setResultInfoBaseObj(outputs.get(kResult));
        }

        auto effects = outputs.getResultInfoChanges();
        effects.narrow(reqs.getResultInfoTrackedFieldSet());

        treesEffects.emplace_back(std::move(effects));
    }

    auto applyEffectsToResultBaseObjectForTree = [&](size_t i) {
        if (!treesEffects[i].hasDroppedOrChangedFields()) {
            // If 'treesEffects[i]' is all "Keeps", then there's nothing to do.
            return;
        }

        auto& stage = trees[i].first;
        auto& outputs = trees[i].second;

        // Get the current kResult slot, and get the current 'resultInfoChanges' composed
        // with the 'trackedFieldSet' from 'reqs'.
        auto resultBaseSlot = outputs.getResultInfoBaseObj();
        auto effects = outputs.getResultInfoChanges();
        effects.compose(FieldEffects(reqs.getResultInfoTrackedFieldSet()));

        // Project the updated result object base to a new slot, and then pass this new slot
        // to setResultInfoBaseObj(). This will set kResult to point to the new slot, and it
        // will also reset 'resultInfoChanges' to contain all "Keep" effects.
        auto [outStage, outSlots] =
            b.makeProject(std::move(stage),
                          generateProjectionFromEffects(state, effects, resultBaseSlot, outputs));
        stage = std::move(outStage);

        outputs.setResultInfoBaseObj(outSlots[0]);

        // Store a default FieldEffects (all "Keeps") into 'treeEffects' for this tree.
        treesEffects[i] = FieldEffects();
    };

    // For each tree, if 'effects.hasCreatedFields()' is true or if 'effects' drops an infinite
    // number of fields, then we need to materialize the result object for the tree before merging.
    for (size_t i = 0; i < treesEffects.size(); ++i) {
        const auto& effects = treesEffects[i];

        if (effects.hasCreatedFields() || effects.getDefaultEffect() == FieldEffect::kDrop) {
            applyEffectsToResultBaseObjectForTree(i);
        }
    }

    // Merge the effects of all of the trees.
    boost::optional<FieldEffects> mergedEffects = mergeEffects(treesEffects);

    // If mergeEffects() was unable to merge the effects of all trees, then materialize the result
    // object for all trees.
    if (!mergedEffects) {
        for (size_t i = 0; i < treesEffects.size(); ++i) {
            applyEffectsToResultBaseObjectForTree(i);
        }

        mergedEffects.emplace(FieldEffects());
    }

    auto mergedChangedFieldSet = mergedEffects->getChangedFields();

    tassert(8378202,
            "Expected default effect to be Keep",
            mergedEffects->getDefaultEffect() == FieldEffect::kKeep);

    tassert(8323516,
            "Expected changed field set to have closed scope",
            mergedChangedFieldSet.getScope() == FieldListScope::kClosed);

    // Inspect each 'tree' and populate any slots needed by 'mergedEffects' that are missing.
    for (size_t i = 0; i < trees.size(); ++i) {
        auto& stage = trees[i].first;
        auto& outputs = trees[i].second;
        const FieldEffects& treeEffects = outputs.getResultInfoChanges();
        std::vector<std::string> keptFieldsMissing;
        std::vector<std::string> droppedFieldsMissing;

        for (const auto& fieldName : mergedChangedFieldSet.getList()) {
            if (!outputs.has(UnownedSlotName(kField, fieldName))) {
                bool isFieldAllowed = postimageAllowedFieldSets[i]->count(fieldName);
                auto effect = isFieldAllowed ? treeEffects.get(fieldName) : FieldEffect::kDrop;

                if (effect == FieldEffect::kKeep) {
                    keptFieldsMissing.emplace_back(fieldName);
                } else if (effect == FieldEffect::kDrop) {
                    droppedFieldsMissing.emplace_back(fieldName);
                } else {
                    tasserted(8378203, "Expected field to have Keep effect or Drop effect");
                }
            }
        }

        if (!keptFieldsMissing.empty()) {
            SbExprOptSbSlotVector projects;
            for (const auto& fieldName : keptFieldsMissing) {
                auto getFieldExpr = b.makeFunction(
                    "getField"_sd, outputs.getResultInfoBaseObj(), b.makeStrConstant(fieldName));
                projects.emplace_back(std::move(getFieldExpr), boost::none);
            }

            auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(projects));
            stage = std::move(outStage);

            invariant(keptFieldsMissing.size() == outSlots.size());

            for (size_t i = 0; i < keptFieldsMissing.size(); ++i) {
                outputs.set(std::pair(kField, std::move(keptFieldsMissing[i])), outSlots[i]);
            }
        }

        if (!droppedFieldsMissing.empty()) {
            auto nothingSlot = SbSlot{state.getNothingSlot()};
            for (auto& fieldName : droppedFieldsMissing) {
                outputs.set(std::pair(kField, std::move(fieldName)), nothingSlot);
            }
        }

        outputs._data->resultInfoChanges.emplace(*mergedEffects);
    }
}


FieldSet PlanStageReqs::getNeededFieldSet() const {
    if (hasResultObj()) {
        return FieldSet::makeUniverseSet();
    } else {
        auto result = FieldSet::makeClosedSet(getFields());
        if (_data->trackedFieldSet) {
            result.setUnion(*_data->trackedFieldSet);
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
             &_inListsMap,
             &_collatorsMap,
             &_sortSpecMap,
             _cq.getExpCtx(),
             _cq.getExpCtx()->needsMerge,
             _cq.getExpCtx()->allowDiskUse) {
    // Initialize '_data->queryCollator'.
    _data->queryCollator = cq.getCollatorShared();

    _data->runtimePlanningRootNodeId = solution.unextendedRootId();

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

    // Analyze the QSN tree point to by 'root' and store the results of analysis in '_qsnAnalysis'.
    analyzeTree();

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
        outputs.set(kReturnKey, SbSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType});
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
    auto [scanSlots, stage] = generateVirtualScanMulti(&_slotIdGenerator,
                                                       vsn->hasRecordId ? 2 : 1,
                                                       inputTag,
                                                       inputVal,
                                                       _yieldPolicy,
                                                       vsn->nodeId());

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
        auto filterExpr = generateFilter(
            _state, vsn->filter.get(), SbSlot{resultSlot, TypeSignature::kAnyScalarType}, outputs);
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

    // Create a new PlanStageReqs object to pass the relevant reqs to the "generateIndexScan"
    // function.
    PlanStageReqs indexScanReqs;
    indexScanReqs.set(PlanStageSlots::kRecordId)
        .setIf(PlanStageSlots::kSnapshotId, reqs.has(PlanStageSlots::kSnapshotId))
        .setIf(PlanStageSlots::kIndexIdent, reqs.has(PlanStageSlots::kIndexIdent))
        .setIf(PlanStageSlots::kIndexKey, reqs.has(PlanStageSlots::kIndexKey))
        .setIf(PlanStageSlots::kIndexKeyPattern, reqs.has(PlanStageSlots::kIndexKeyPattern))
        .setIf(PlanStageSlots::kPrefetchedResult, reqs.has(PlanStageSlots::kPrefetchedResult));

    const auto generateIndexScanFunc =
        ixn->iets.empty() ? generateIndexScan : generateIndexScanWithDynamicBounds;
    auto&& [scanStage, scanOutputs] = generateIndexScanFunc(_state,
                                                            getCurrentCollection(reqs),
                                                            ixn,
                                                            fieldBitset,
                                                            sortKeyBitset,
                                                            _yieldPolicy,
                                                            indexScanReqs);

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
                    SbSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType});
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
    tassert(9081802,
            "buildCountScan() does not support kPrefetchedResult",
            !reqs.has(kPrefetchedResult));
    tassert(5295805, "buildCountScan() does not support kSortKey", !reqs.hasSortKeys());

    auto csn = static_cast<const CountScanNode*>(root);

    auto collection = getCurrentCollection(reqs);
    auto indexName = csn->index.identifier.catalogName;
    auto indexDescriptor = collection->getIndexCatalog()->findIndexByName(_state.opCtx, indexName);
    auto indexAccessMethod =
        collection->getIndexCatalog()->getEntry(indexDescriptor)->accessMethod()->asSortedData();

    std::unique_ptr<key_string::Value> lowKey, highKey;
    bool isPointInterval = false;
    if (csn->iets.empty()) {
        std::tie(lowKey, highKey) =
            makeKeyStringPair(csn->startKey,
                              csn->startKeyInclusive,
                              csn->endKey,
                              csn->endKeyInclusive,
                              indexAccessMethod->getSortedDataInterface()->getKeyStringVersion(),
                              indexAccessMethod->getSortedDataInterface()->getOrdering(),
                              true /* forward */);
        isPointInterval = *lowKey == *highKey;
    } else {
        isPointInterval = ietsArePointInterval(csn->iets);
    }

    auto [stage, planStageSlots, indexScanBoundsSlots] =
        generateSingleIntervalIndexScanAndSlots(_state,
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
                                                false /* lowPriority */,
                                                isPointInterval);

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
        planStageSlots.setResultObj(SbSlot{resultSlot, TypeSignature::kObjectType});
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

    auto coll = getCurrentCollection(reqs);
    std::unique_ptr<sbe::PlanStage> stage =
        std::make_unique<sbe::ColumnScanStage>(coll->uuid(),
                                               coll->ns().dbName(),
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
            _state, csn->postAssemblyFilter.get(), SbSlot{reconstructedRecordSlot}, outputs);

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

    // Check if this FETCH stage has any descendants that are FETCHs or COLLSCANs.
    boost::optional<UnfetchedIxscans> unfetchedIxns = getUnfetchedIxscans(root);
    const bool mayHaveFetchOrCollScanDescendants =
        !unfetchedIxns || unfetchedIxns->hasFetchesOrCollScans;

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

    if (mayHaveFetchOrCollScanDescendants) {
        childReqs.set(kPrefetchedResult);
    }

    auto [stage, outputs] = build(child, childReqs);

    uassert(4822880, "RecordId slot is not defined", outputs.has(kRecordId));
    uassert(
        4953600, "ReturnKey slot is not defined", !reqs.has(kReturnKey) || outputs.has(kReturnKey));
    uassert(5290701, "Snapshot id slot is not defined", outputs.has(kSnapshotId));
    uassert(7566701, "Index ident slot is not defined", outputs.has(kIndexIdent));
    uassert(5290711, "Index key slot is not defined", outputs.has(kIndexKey));
    uassert(5113713, "Index key pattern slot is not defined", outputs.has(kIndexKeyPattern));
    uassert(9081801,
            "Prefetched result slot is not defined",
            !mayHaveFetchOrCollScanDescendants || outputs.has(kPrefetchedResult));

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

    auto relevantSlots = getSlotsToForward(_state, forwardingReqs, outputs);

    auto recordIdSlot = outputs.get(kRecordId);
    auto snapshotIdSlot = outputs.get(kSnapshotId);
    auto indexIndexSlot = outputs.get(kIndexIdent);
    auto indexKeySlot = outputs.get(kIndexKey);
    auto indexKeyPatternSlot = outputs.get(kIndexKeyPattern);
    auto prefetchedResultSlot = mayHaveFetchOrCollScanDescendants
        ? boost::make_optional(outputs.get(kPrefetchedResult))
        : boost::none;

    auto [outStage, resultSlot, ridSlot, fieldSlots] =
        makeLoopJoinForFetch(std::move(stage),
                             fields,
                             recordIdSlot,
                             snapshotIdSlot,
                             indexIndexSlot,
                             indexKeySlot,
                             indexKeyPatternSlot,
                             prefetchedResultSlot,
                             getCurrentCollection(reqs),
                             _state,
                             root->nodeId(),
                             std::move(relevantSlots));

    stage = std::move(outStage);

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

    auto [projectStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                         fieldsAndSortKeys,
                                                         resultSlot,
                                                         root->nodeId(),
                                                         &_slotIdGenerator,
                                                         _state,
                                                         &outputs);
    stage = std::move(projectStage);

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

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSort(
    const QuerySolutionNode* root, const PlanStageReqs& reqsIn) {
    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    tassert(5037001,
            "QueryPlannerAnalysis should not produce a SortNode with an empty sort pattern",
            sortPattern.size() > 0);

    SbExprBuilder b(_state);

    // getExecutor() should never call into buildSlotBasedExecutableTree() when the query
    // contains $meta, so this assertion should always be true.
    for (const auto& part : sortPattern) {
        tassert(5037002, "Sort with $meta is not supported in SBE", part.fieldPath);
    }

    auto child = sn->children[0].get();

    if (auto [ixn, ct] = root->getFirstNodeByType(STAGE_IXSCAN);
        !sn->fetched() && !reqsIn.hasResult() && ixn && ct >= 1) {
        return buildSortCovered(root, reqsIn);
    }

    // When sort is followed by a limit the overhead of tracking the kField slots during sorting is
    // greater compared to the overhead of retrieving the necessary kFields from the materialized
    // result object after the sorting is done.
    boost::optional<PlanStageReqs> updatedReqs;
    if (reqsIn.getHasLimit()) {
        updatedReqs.emplace(reqsIn);
        updatedReqs->setResultObj().clearAllFields();
    }

    const auto& reqs = updatedReqs ? *updatedReqs : reqsIn;

    auto forwardingReqs = reqs.copyForChild();
    auto childReqs = reqs.copyForChild();

    // When there's no limit on the sort, the dominating factor is number of comparisons
    // (nlogn). A sort with a limit of k requires only nlogk comparisons. When k is small, the
    // number of key generations (n) can actually dominate the runtime. So for all top-k sorts
    // we use a "cheap" sort key: it's cheaper to construct but more expensive to compare. The
    // assumption here is that k << n.
    bool allowCallGenCheapSortKey = sn->limit != 0;

    BuildSortKeysPlan plan = makeSortKeysPlan(sortPattern, allowCallGenCheapSortKey);

    if (plan.type == BuildSortKeysPlan::kTraverseFields) {
        childReqs.setFields(std::move(plan.fieldsForSortKeys));
    } else if (plan.type == BuildSortKeysPlan::kCallGenSortKey ||
               plan.type == BuildSortKeysPlan::kCallGenCheapSortKey) {
        childReqs.setResultObj();
    } else {
        MONGO_UNREACHABLE;
    }

    auto [stage, childOutputs] = build(child, childReqs);
    auto outputs = std::move(childOutputs);

    auto sortKeys = buildSortKeys(_state, plan, sortPattern, outputs);

    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;

    if (plan.type == BuildSortKeysPlan::kTraverseFields) {
        // Handle the case where we are using a materialized result object and there are no common
        // prefixes.
        orderBy.reserve(sortPattern.size());

        sbe::SlotExprPairVector projects;

        if (sortKeys.parallelArraysCheckExpr) {
            auto parallelArraysError =
                b.makeFail(ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            auto failOnParallelArraysExpr =
                b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                               std::move(sortKeys.parallelArraysCheckExpr),
                               std::move(parallelArraysError));

            projects.emplace_back(_state.slotId(), failOnParallelArraysExpr.extractExpr(_state));
        }

        for (size_t i = 0; i < sortPattern.size(); ++i) {
            const auto& part = sortPattern[i];
            SbExpr& sortKeyExpr = sortKeys.keyExprs[i];

            sbe::value::SlotId sortKeySlot = _state.slotId();
            projects.emplace_back(sortKeySlot, sortKeyExpr.extractExpr(_state));

            orderBy.push_back(sortKeySlot);
            direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                                 : sbe::value::SortDirection::Descending);
        }

        stage =
            sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), root->nodeId());
    } else if (plan.type == BuildSortKeysPlan::kCallGenSortKey ||
               plan.type == BuildSortKeysPlan::kCallGenCheapSortKey) {
        auto& sortKeyExpr = sortKeys.fullKeyExpr;

        const auto fullSortKeySlot = _slotIdGenerator.generate();
        stage = sbe::makeProjectStage(
            std::move(stage), root->nodeId(), fullSortKeySlot, sortKeyExpr.extractExpr(_state));

        if (plan.type == BuildSortKeysPlan::kCallGenSortKey) {
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
    } else {
        MONGO_UNREACHABLE;
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

    auto postimageAllowedFieldSets = getPostimageAllowedFieldSets(mergeSortNode->children);

    auto outputs = PlanStageSlots::makeMergedPlanStageSlots(
        _state, root->nodeId(), childReqs, inputStagesAndSlots, postimageAllowedFieldSets);

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
    SbSlot childResultSlot = outputs.getResultObj();

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
    SbSlot getFieldSlot = outSlots[0];

    // Continue building the unwind and projection to results.
    return buildOnlyUnwind(un, reqs, stage, outputs, childResultSlot.getId(), getFieldSlot.getId());
}  // buildUnwind

namespace {
/**
 * This function produces an updated document expression by taking an input document ('docExpr')
 * and applying '{$addFields: {<outputPath>: <outputExpr>}}' to it with traversalDepth=0.
 */
SbExpr projectUnwindOutputs(StageBuilderState& state,
                            SbExpr docExpr,
                            std::string outputPath,
                            SbExpr outputExpr) {
    constexpr int32_t traversalDepth = 0;

    std::vector<std::string> paths;
    std::vector<ProjectNode> nodes;

    paths.emplace_back(std::move(outputPath));
    nodes.emplace_back(std::move(outputExpr));

    return generateProjection(state,
                              projection_ast::ProjectType::kAddition,
                              std::move(paths),
                              std::move(nodes),
                              std::move(docExpr),
                              nullptr,
                              traversalDepth);
}

/**
 * This function produces an updated document expression by taking an input document ('docExpr'),
 * applying '{$addFields: {<firstOutputPath>: <firstOutputExpr>}}' to it with traversalDepth=0,
 * and then applying '{$addFields: {<secondOutputPath>: <secondOutputExpr>}}' to it with
 * traversalDepth=0.
 *
 * As an optimization, if the two ouput paths do not conflict with each other, then the two
 * $addFields operations will be combined into a single $addFields operation like so:
 *   {$addFields: {<firstOutputPath>: <firstOutputExpr>, <secondOutputPath>: <secondOutputExpr>}}
 */
SbExpr projectUnwindOutputs(StageBuilderState& state,
                            SbExpr docExpr,
                            std::string firstOutputPath,
                            SbExpr firstOutputExpr,
                            std::string secondOutputPath,
                            SbExpr secondOutputExpr) {
    constexpr int32_t traversalDepth = 0;

    bool hasConflictingPaths = pathsAreConflicting(firstOutputPath, secondOutputPath);

    if (!hasConflictingPaths) {
        // If the first path and second path don't conflict with each other, then we can do a
        // single projection to update both paths.
        std::vector<std::string> paths;
        std::vector<ProjectNode> nodes;

        paths.emplace_back(std::move(firstOutputPath));
        nodes.emplace_back(std::move(firstOutputExpr));

        paths.emplace_back(std::move(secondOutputPath));
        nodes.emplace_back(std::move(secondOutputExpr));

        docExpr = generateProjection(state,
                                     projection_ast::ProjectType::kAddition,
                                     std::move(paths),
                                     std::move(nodes),
                                     std::move(docExpr),
                                     nullptr,
                                     traversalDepth);
    } else {
        // If the first path and second path conflict, then we do 2 projections: one projection
        // to update the first path, and another projection to update the second path.
        docExpr = projectUnwindOutputs(
            state, std::move(docExpr), std::move(firstOutputPath), std::move(firstOutputExpr));
        docExpr = projectUnwindOutputs(
            state, std::move(docExpr), std::move(secondOutputPath), std::move(secondOutputExpr));
    }

    return docExpr;
}
}  // namespace

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
    using namespace std::literals::string_literals;

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

    // The projection expression that adds the index and/or unwind values to the result doc.
    std::unique_ptr<sbe::EExpression> finalProjectExpr;

    bool hasIndexPath = un->indexPath.has_value();
    const std::string& unwindPath = un->fieldPath.fullPath();
    const std::string& indexOutputPath = hasIndexPath ? un->indexPath->fullPath() : ""s;

    if (hasIndexPath) {
        // "includeArrayIndex" option (Cases 1-3). The index is always projected in these.

        // Case 1: index Null, unwind val //////////////////////////////////////////////////////////
        SbExpr indexNullUnwindValProjExpr[2];

        for (int copy = 0; copy < 2; ++copy) {
            indexNullUnwindValProjExpr[copy] =
                projectUnwindOutputs(_state,
                                     SbExpr{childResultSlot},
                                     unwindPath,
                                     SbExpr{unwindSlot},
                                     indexOutputPath,
                                     SbExpr{makeConstant(sbe::value::TypeTags::Null, 0)});
        }

        // Case 2: index val, unwind val ///////////////////////////////////////////////////////////
        SbExpr indexValUnwindValProjExpr = projectUnwindOutputs(_state,
                                                                SbExpr{childResultSlot},
                                                                unwindPath,
                                                                SbExpr{unwindSlot},
                                                                indexOutputPath,
                                                                SbExpr{arrayIndexSlot});

        // Case 3: index Null //////////////////////////////////////////////////////////////////////
        SbExpr indexNullProjExpr =
            projectUnwindOutputs(_state,
                                 SbExpr{childResultSlot},
                                 indexOutputPath,
                                 SbExpr{makeConstant(sbe::value::TypeTags::Null, 0)});

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
        SbExpr unwindValProjExpr =
            projectUnwindOutputs(_state, SbExpr{childResultSlot}, unwindPath, SbExpr{unwindSlot});

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
    SbSlot resultSlot = SbSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType};
    stage = makeProjectStage(std::move(stage),
                             un->nodeId(),
                             resultSlot.slotId,  // output result document
                             std::move(finalProjectExpr));

    // Clear the kField slots affected by our updates to 'unwindPath' and 'indexOutputPath'.
    outputs.clearAffectedFields(unwindPath);
    if (hasIndexPath) {
        outputs.clearAffectedFields(indexOutputPath);
    }

    // If we didn't project the index or if 'unwindPath' and 'indexOutputPath' don't conflict with
    // each other, then update the kField slots in 'outputs' for 'unwindPath' and 'indexOutputPath'
    // if our parent requested them. Otherwise, don't set the kField slots for 'unwindPath' and
    // 'indexOutputPath' and let our parent just retrieve them from the result object if needed.
    if (!hasIndexPath || !pathsAreConflicting(unwindPath, indexOutputPath)) {
        auto unwindFieldName = std::make_pair(PlanStageSlots::kField, unwindPath);
        if (reqs.has(unwindFieldName)) {
            outputs.set(std::move(unwindFieldName), unwindSlot);
        }

        if (hasIndexPath) {
            auto indexOutputFieldName = std::make_pair(PlanStageSlots::kField, indexOutputPath);
            if (reqs.has(indexOutputFieldName)) {
                outputs.set(std::move(indexOutputFieldName), arrayIndexSlot);
            }
        }
    }

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
using NothingPassthruUpdatedAndResultPaths = std::tuple<std::vector<std::string>,
                                                        std::vector<std::string>,
                                                        std::vector<std::string>,
                                                        std::vector<std::string>>;

NothingPassthruUpdatedAndResultPaths mapRequiredFieldsToProjectionOutputs(
    const std::vector<std::string>& reqFields,
    bool isInclusion,
    const FieldSet& preimageAllowedFields,
    const std::vector<std::string>& paths,
    const std::vector<ProjectNode::Type>& nodeTypes,
    const std::vector<std::string>& trackedChangedFields) {
    using NodeType = ProjectNode::Type;

    // Fast path for when 'reqFields' is empty and 'trackedChangedFields' is empty.
    if (reqFields.empty() && trackedChangedFields.empty()) {
        return {};
    }

    // Scan the ProjectNodes and build various path sets.
    StringDataSet keepDropPathSet;
    StringDataSet changedPathSet;
    StringDataSet createdPathSet;

    StringDataSet pathPrefixSet;
    StringDataSet createdPathPrefixSet;

    for (size_t i = 0; i < nodeTypes.size(); ++i) {
        auto& nodeType = nodeTypes[i];
        auto& path = paths[i];

        if (nodeType == NodeType::kBool) {
            keepDropPathSet.insert(path);
            addPrefixesToSet(path, pathPrefixSet);
        } else if (nodeType == NodeType::kExpr || nodeType == NodeType::kSbExpr) {
            changedPathSet.insert(path);
            addPrefixesToSet(path, pathPrefixSet);

            createdPathSet.insert(path);
            addPrefixesToSet(path, createdPathPrefixSet);
        } else if (nodeType == NodeType::kSlice) {
            changedPathSet.insert(path);
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
        bool pathAllowed = preimageAllowedFields.count(getTopLevelField(path));

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
            // We already checked 'keepDropPathSet' above. Here we check 'changedPathSet' and
            // 'pathPrefixSet' (or, if 'pathAllowed' is false, we check 'createdPathSet' and
            // 'createdPathPrefixSet').
            const auto& prefixSet = pathAllowed ? pathPrefixSet : createdPathPrefixSet;
            const auto& pathSet = pathAllowed ? changedPathSet : createdPathSet;

            if (prefixSet.count(path) || prefixIsInSet(path, pathSet)) {
                resultPaths.emplace_back(path);
            } else if (pathAllowed && !isInclusion) {
                passthruPaths.emplace_back(path);
            } else {
                nothingPaths.emplace_back(path);
            }
        }
    }

    if (!trackedChangedFields.empty()) {
        auto fieldSet = StringSet(reqFields.begin(), reqFields.end());
        for (const auto& field : trackedChangedFields) {
            if (fieldSet.insert(field).second) {
                if (createdPathSet.count(field)) {
                    updatedPaths.emplace_back(field);
                } else {
                    resultPaths.emplace_back(field);
                }
            }
        }
    }

    return std::tuple(std::move(nothingPaths),
                      std::move(passthruPaths),
                      std::move(updatedPaths),
                      std::move(resultPaths));
}

boost::optional<std::vector<std::string>> getFixedPlanFieldOrder(const FieldEffects& effects) {
    // Get the post-image allowed field set.
    auto allowedFields = effects.getAllowedFields();

    // If the field set is open, then return boost::none.
    if (allowedFields.getScope() != FieldListScope::kClosed) {
        return boost::none;
    }

    // Check if there is a fixed order that can be used for this projection.
    std::vector<std::string> result;
    std::vector<std::string> adds;

    size_t numPreimageFieldPositionsObserved = 0;
    for (const auto& field : allowedFields.getList()) {
        auto effect = effects.get(field);

        if (effect == FieldEffect::kAdd) {
            // Record all Adds that we see in order.
            adds.emplace_back(field);
        } else if (isNonSpecificEffect(effect)) {
            // If we encounter an unsupported effect, we can't use a fixed order for
            // this projection, so we return boost::none.
            return boost::none;
        } else {
            // Increment the count of input fields whose positions we need to observe.
            ++numPreimageFieldPositionsObserved;

            // If we need to observe the position of more than one input field, then we
            // can't use a fixed order for this projection, so we return boost::none.
            if (numPreimageFieldPositionsObserved > 1) {
                return boost::none;
            }

            // Append the field to the end of 'result'.
            result.emplace_back(field);
        }
    }

    // Append all the Adds that we saw to the end of 'result' and return 'result'.
    result.insert(result.end(), adds.begin(), adds.end());

    return std::move(result);
}

std::vector<const QuerySolutionNode*> getProjectionDescendants(const QuerySolutionNode* root) {
    using DfsItem = std::pair<const QuerySolutionNode*, size_t>;

    absl::InlinedVector<DfsItem, 64> dfs;
    std::vector<const QuerySolutionNode*> descendants;

    for (const auto& child : root->children) {
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

inline bool effectIsDropOrAdd(FieldEffect e) {
    return e == FieldEffect::kDrop || e == FieldEffect::kAdd;
}
}  // namespace

std::unique_ptr<SlotBasedStageBuilder::ResultPlan> SlotBasedStageBuilder::getResultPlan(
    const QuerySolutionNode* qsNode, const PlanStageReqs& reqs) {
    auto effects = getQsnInfo(qsNode).effects;

    // If this isn't a projection, or if getQsnInfo().effects returned boost::none, or if 'reqs'
    // doesn't contain a result object req and it doesn't contain an result info req, then there's
    // no need to make any changes to 'reqs', so we return null.
    if (!reqs.hasResult() || !hasProjectionInfo(qsNode) || !effects) {
        return {};
    }

    // If this projection can participate with the result info req, then there's no need to make
    // any changes to 'reqs', so we return null.
    if (reqs.hasResultInfo()) {
        // Narrow 'effects' so that it only has effects applicable to fields in the result
        // info's "tracked field" set.
        effects->narrow(reqs.getResultInfoTrackedFieldSet());

        if (composeEffectsForResultInfo(*effects, reqs.getResultInfoEffects())) {
            return {};
        }
    }

    // If we reach here, 'qsNode' must be a projection and 'reqs' must contain either a result
    // object req or a result info req that this projection can't participate with, and therefore
    // this projection must produce a result object.

    const auto& projectionInfo = getProjectionInfo(qsNode);
    if (projectionInfo.isCoveredProjection || projectionInfo.needWholeDocument) {
        // If this projection is a covered projection or if it requires its child to produce a
        // result object, then attempting to use a "fixed" plan or a "ResultInfo" based plan will
        // not produce a better SBE tree (and might produce a worse SBE tree), so for such cases
        // we bail out early and just return null.
        return {};
    }

    // See if we can used a "fixed" plan to materialize the result object.
    auto fixedPlanFields = getFixedPlanFieldOrder(*effects);

    if (fixedPlanFields) {
        // If it's possible to use a fixed plan, then return the fixed plan.
        const auto& postimageAllowedFields = getPostimageAllowedFields(qsNode);

        auto [passthruPaths, nothingPaths] =
            splitVector(reqs.getFields(), [&](const std::string& p) {
                return postimageAllowedFields.count(getTopLevelField(p));
            });

        PlanStageReqs planReqs = reqs;
        planReqs.clearResult()
            .clearAllFields()
            .setFields(std::move(passthruPaths))
            .setFields(*fixedPlanFields)
            .setCanProcessBlockValues(true);

        auto plan = std::make_unique<ResultPlan>(ResultPlan::kUseFixedPlan, std::move(planReqs));

        plan->fixedPlanFields = std::move(*fixedPlanFields);
        plan->nothingPaths = std::move(nothingPaths);

        return plan;
    }

    // Try using a "ResultInfo"-based plan to materialize the result object, and see if doing
    // so would produce a more efficient SBE tree.

    // Compute what the tracked field set would be for a ResultInfo req. We need to track all
    // fields except those with Drop or Add effects.
    auto reqTrackedFieldSet =
        effects->getFieldsWithEffects([](auto e) { return !effectIsDropOrAdd(e); });

    auto reqEffects = *effects;

    // Narrow 'reqEffects' so that it only has effects applicable to fields in 'reqTrackedFieldSet'.
    reqEffects.narrow(reqTrackedFieldSet);

    // Loop over this projection's descendants and see if at least one descendant could participate
    // with the ResultInfo req.
    bool hasParticipatingDescendant = false;
    for (auto desc : getProjectionDescendants(qsNode)) {
        if (auto descEffects = getQsnInfo(desc).effects) {
            // Narrow 'descEffects' so that it only has effects applicable to fields in
            // 'reqTrackedFieldSet'.
            descEffects->narrow(reqTrackedFieldSet);

            if (composeEffectsForResultInfo(*descEffects, reqEffects)) {
                // If 'descEffects' can be composed with 'reqEffects', then 'desc' could participate
                // with a result info req.
                hasParticipatingDescendant = true;
                break;
            }
        }
    }

    if (hasParticipatingDescendant) {
        // If we can use a ResultInfo-based plan, return the ResultInfo-based plan.
        const auto& postimageAllowedFields = getPostimageAllowedFields(qsNode);

        auto [passthruPaths, nothingPaths] =
            splitVector(reqs.getFields(), [&](const std::string& p) {
                return postimageAllowedFields.count(getTopLevelField(p));
            });

        PlanStageReqs planReqs = reqs;
        planReqs.clearResult()
            .clearAllFields()
            .setFields(std::move(passthruPaths))
            .setResultInfo(postimageAllowedFields, FieldEffects())
            .setCanProcessBlockValues(false);

        auto plan = std::make_unique<ResultPlan>(ResultPlan::kUseResultInfo, std::move(planReqs));

        plan->nothingPaths = std::move(nothingPaths);

        return plan;
    }

    // If we reach here, that means it wasn't possible to produce a better SBE tree using either
    // a fixed plan or a ResultInfo-based plan. There's no need to make any changes to 'reqs', so
    // we return null.
    return {};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::makeResultUsingPlan(std::unique_ptr<sbe::PlanStage> stage,
                                           PlanStageSlots outputs,
                                           const QuerySolutionNode* qsNode,
                                           std::unique_ptr<ResultPlan> plan) {
    SbBuilder b(_state, qsNode->nodeId());

    // If hasBlockOutput() is true, terminate the block processing section of the pipeline here.
    if (outputs.hasBlockOutput()) {
        stage = buildBlockToRow(std::move(stage), _state, outputs);
    }

    // Update the kField slots in 'outputs' that correspond to 'nothingPaths'.
    if (!plan->nothingPaths.empty()) {
        auto nothingSlot = _state.getNothingSlot();
        for (auto& nothingPath : plan->nothingPaths) {
            outputs.set(std::make_pair(PlanStageSlots::kField, std::move(nothingPath)),
                        nothingSlot);
        }
    }

    SbExpr resultExpr;

    switch (plan->type) {
        case ResultPlan::kUseResultInfo: {
            // Compose 'resultInfoChanges' with qsNode's allowed field set.
            FieldEffects resultInfoChanges = outputs.getResultInfoChanges();
            resultInfoChanges.compose(FieldEffects(getPostimageAllowedFields(qsNode)));

            // Using 'resultInfoChanges', generate a projection that will materialize the
            // result object.
            resultExpr = generateProjectionFromEffects(
                _state, resultInfoChanges, outputs.getResultInfoBaseObj(), outputs);
            break;
        }
        case ResultPlan::kUseFixedPlan: {
            // Generate a call to newBsonObj() that will materialize the result object.
            SbExpr::Vector args;
            for (const auto& field : plan->fixedPlanFields) {
                args.emplace_back(b.makeStrConstant(field));
                args.emplace_back(SbExpr{outputs.get(std::pair(kField, field))});
            }

            resultExpr = b.makeFunction("newBsonObj"_sd, std::move(args));
            break;
        }
        default:
            MONGO_UNREACHABLE_TASSERT(8323509);
    }

    auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(resultExpr));
    stage = std::move(outStage);

    outputs.setResultObj(outSlots[0]);

    return {std::move(stage), std::move(outputs)};
}

std::unique_ptr<ProjectionPlan> SlotBasedStageBuilder::makeProjectionPlan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto& projectionInfo = getProjectionInfo(root);

    bool isInclusion = projectionInfo.isInclusion;
    auto paths = projectionInfo.paths;
    auto nodes = cloneProjectNodes(projectionInfo.nodes);

    // Check if this projection is a "covered projection". We handle covered projections differently
    // in a number of ways (vs. normal projections).
    bool isCoveredProjection = projectionInfo.isCoveredProjection;

    StringMap<Expression*> pathExprMap;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto& path = paths[i];
        if (node.isExpr()) {
            pathExprMap.emplace(path, node.getExpr());
        }
    }

    // Get the parent's requirements.
    bool reqResultObj = reqs.hasResultObj();
    bool reqResultInfo = reqs.hasResultInfo();
    auto reqFields = reqs.getFields();

    boost::optional<FieldSet> reqTrackedFieldSetForChild;
    boost::optional<FieldEffects> reqEffectsForChild;
    boost::optional<FieldEffects> effects;

    // If there is a ResultInfo req, check if this projection can participate with it.
    if (reqResultInfo) {
        const auto& reqTrackedFieldSet = reqs.getResultInfoTrackedFieldSet();
        const auto& reqEffects = reqs.getResultInfoEffects();

        // Get the effects of this projection.
        effects = getQsnInfo(root).effects;

        bool canParticipate = false;
        if (effects) {
            // Narrow 'effects' so that it only has effects applicable to fields in
            // 'reqTrackedFieldSet'.
            effects->narrow(reqTrackedFieldSet);

            if (auto composedEffects = composeEffectsForResultInfo(*effects, reqEffects)) {
                // If 'effects' can be composed with 'reqEffects', then this projection can
                // participate with the result info req. Set 'canParticipate' to true, and
                // initialize 'reqTrackedFieldSetForChild' and 'reqEffectsForChild'.
                canParticipate = true;

                // Update the tracked field set for the ResultInfo req. We need to continue to
                // track all the fields we were tracking before except those with Drop or Add
                // effects.
                reqTrackedFieldSetForChild.emplace(reqTrackedFieldSet);
                reqTrackedFieldSetForChild->setIntersect(
                    effects->getFieldsWithEffects([](auto e) { return !effectIsDropOrAdd(e); }));

                reqEffectsForChild.emplace(std::move(*composedEffects));
                reqEffectsForChild->narrow(*reqTrackedFieldSetForChild);
            }
        }

        if (!canParticipate) {
            // If this projection can't participate with the result info req, then it must produce
            // a result object instead.
            reqResultObj = true;
            reqResultInfo = false;
        }
    }

    // If this projection isn't participating with a result info req, set 'effects' to all Keeps.
    if (!reqResultInfo) {
        effects = FieldEffects();
    }

    std::vector<ProjectNode::Type> nodeTypes = !isCoveredProjection
        ? ProjectNode::getNodeTypes(nodes)
        : getTransformedNodeTypesForCoveredProjection(paths.size());

    const auto& preimageAllowedFields = !isCoveredProjection
        ? getPostimageAllowedFields(root->children[0].get())
        : QsnAnalysis::kEmptyFieldSet;

    std::vector<std::string> trackedChangedFields = effects->getChangedFields().getList();

    auto [nothingPaths, passthruPaths, updatedPaths, resultPaths] =
        mapRequiredFieldsToProjectionOutputs(
            reqFields, isInclusion, preimageAllowedFields, paths, nodeTypes, trackedChangedFields);

    std::vector<std::string> resultFields;
    StringSet resultFieldSet;
    for (const auto& p : resultPaths) {
        auto f = getTopLevelField(p);
        auto [_, inserted] = resultFieldSet.insert(f.toString());
        if (inserted) {
            resultFields.emplace_back(f.toString());
        }
    }

    bool childMakeResult =
        (reqResultObj && !isCoveredProjection) || projectionInfo.needWholeDocument;

    if (isCoveredProjection) {
        // We don't actually have MQL expressions corresponding to the paths in 'updatedPaths'
        // for covered projections, so instead we treat these paths as "passthru paths".
        passthruPaths = appendVectorUnique(std::move(passthruPaths), std::move(updatedPaths));
        updatedPaths = std::vector<std::string>{};
    }

    // Start preparing the requirements for our child.
    auto childReqs = reqs.copyForChild().clearResult().clearAllFields();

    if (childMakeResult) {
        // If 'childMakeResult' is true, add a result object requirement to 'childReqs'.
        childReqs.setResultObj();
    } else if (reqResultInfo && !isCoveredProjection) {
        // If there is a ResultInfo req that we are participating with and this isn't a covered
        // projection, then set a ResultInfo req on 'childReqs'.
        childReqs.setResultInfo(*reqTrackedFieldSetForChild, *reqEffectsForChild);
    }

    // Start computing the list of fields that we need to request from our child. We begin
    // with the paths from 'passthruPaths' and the paths from 'projectionInfo.depFields'.
    auto fields = appendVectorUnique(std::move(passthruPaths), projectionInfo.depFields);

    // Add all the fields needed as input for calls to generateSingleFieldProjection().
    if (!reqResultObj) {
        fields = appendVectorUnique(std::move(fields), filterVector(resultFields, [&](auto&& f) {
                                        return preimageAllowedFields.count(f);
                                    }));
    }

    childReqs.setFields(std::move(fields));

    // Indicate we can work on block values, if we are not requested to produce a result object.
    childReqs.setCanProcessBlockValues(!childReqs.hasResult());

    return std::make_unique<ProjectionPlan>(ProjectionPlan{std::move(childReqs),
                                                           reqResultObj,
                                                           reqResultInfo,
                                                           isCoveredProjection,
                                                           isInclusion,
                                                           std::move(paths),
                                                           std::move(nodes),
                                                           std::move(nothingPaths),
                                                           std::move(updatedPaths),
                                                           std::move(resultPaths),
                                                           std::move(pathExprMap),
                                                           std::move(*effects)});
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildProjection(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(8146605, "buildProjection() does not support kSortKey", !reqs.hasSortKeys());

    // Build a plan for this projection.
    std::unique_ptr<ProjectionPlan> plan = makeProjectionPlan(root, reqs);

    // Call build() on the child.
    auto [childStage, childOutputs] = build(root->children[0].get(), plan->childReqs);

    // Call buildProjectionImpl() to generate all SBE expressions and stages needed for this
    // projection.
    auto [stage, outputs] = buildProjectionImpl(
        root, reqs, std::move(plan), std::move(childStage), std::move(childOutputs));

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionImpl(const QuerySolutionNode* root,
                                           const PlanStageReqs& reqs,
                                           std::unique_ptr<ProjectionPlan> plan,
                                           std::unique_ptr<sbe::PlanStage> stage,
                                           PlanStageSlots outputs) {
    const bool isCoveredProjection = plan->isCoveredProjection;

    const bool isInclusion = plan->isInclusion;
    std::vector<std::string>& paths = plan->paths;
    std::vector<ProjectNode>& nodes = plan->nodes;

    // If 'isCoveredProjection' is true, transform 'nodes' appropriately.
    if (isCoveredProjection) {
        tassert(8146606, "Expected 'isInclusion' to be true", isInclusion);

        nodes = getTransformedNodesForCoveredProjection(paths, outputs);
    }

    const auto& preimageAllowedFields = !isCoveredProjection
        ? getPostimageAllowedFields(root->children[0].get())
        : QsnAnalysis::kEmptyFieldSet;

    SbBuilder b(_state, root->nodeId());

    std::vector<std::string> resultFields;
    StringSet resultFieldSet;
    for (const auto& p : plan->resultPaths) {
        auto f = getTopLevelField(p);
        auto [_, inserted] = resultFieldSet.insert(f.toString());
        if (inserted) {
            resultFields.emplace_back(f.toString());
        }
    }

    // Evaluate the MQL expressions needed by this projection. We begin by making a list of all
    // the relevant paths.
    std::vector<std::string> exprPaths;
    StringMap<SbSlot> exprPathSlotMap;

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].isExpr()) {
            exprPaths.push_back(paths[i]);
        }
    }

    exprPaths = appendVectorUnique(std::move(exprPaths), plan->updatedPaths);

    // Define a helper lambda for processing MQL expressions that we will invoke later.
    auto generateProjExpressions = [&](bool vectorizeExprs = false) {
        // Call the generateExpression() lambda to evaluate each MQL expression, and then use a
        // ProjectStage to project the output values to slots.
        auto resultObjSlot = outputs.getResultObjIfExists();
        SbExprOptSbSlotVector projects;
        std::vector<std::string> processedPaths;

        // Visit all the paths in 'exprPaths' that are not in 'exprPathSlotMap' yet.
        for (const auto& exprPath : exprPaths) {
            if (!exprPathSlotMap.count(exprPath)) {
                const auto* expr = plan->pathExprMap[exprPath];
                SbExpr e = generateExpression(_state, expr, resultObjSlot, outputs);

                if (vectorizeExprs && !e.isSlotExpr()) {
                    // Attempt to vectorize.
                    auto blockResult = buildVectorizedExpr(_state, std::move(e), outputs, false);
                    if (blockResult) {
                        // If vectorization is successful, store the result to 'projects'.
                        projects.emplace_back(std::move(blockResult), boost::none);
                        processedPaths.emplace_back(exprPath);
                    }
                } else {
                    // If we're not attempting to vectorize or if 'e' is just a slot variable,
                    // then add 'e' to 'projects'.
                    projects.emplace_back(std::move(e), boost::none);
                    processedPaths.emplace_back(exprPath);
                }
            }
        }

        if (!projects.empty()) {
            // Create the ProjectStage and get the list of slots that were projected to.
            auto [outStage, outSlots] =
                b.makeProject(std::move(stage), buildVariableTypes(outputs), std::move(projects));
            stage = std::move(outStage);

            size_t outSlotsIdx = 0;
            for (const auto& processedPath : processedPaths) {
                // Store the slot into 'exprPathSlotMap'.
                auto slot = outSlots[outSlotsIdx++];
                exprPathSlotMap[processedPath] = slot;
            }
        }
    };

    // If hasBlockOutput() is true, then we will attempt to vectorize the MQL expressions. (If
    // vectorization fails then we will fall back to using the original "scalar" MQL expressions.)
    // We may also have to generate a BlockToRow stage if this projection or this projection's
    // parent can't handle the blocks being produced by this projection's child.
    if (outputs.hasBlockOutput()) {
        if (!exprPaths.empty()) {
            // Call generateProjExpressions() with 'vectorizeExprs' set to true.
            constexpr bool vectorizeExprs = true;
            generateProjExpressions(vectorizeExprs);
        }

        // Terminate the block processing section of the pipeline if either: (1) there are
        // expressions that are not compatible with block processing; or (2) the parent stage
        // doesn't support block values; or (3) we're planning to call generateProjection() or
        // generateSingleFieldProjection() below.
        if (exprPaths.size() != exprPathSlotMap.size() || !reqs.getCanProcessBlockValues() ||
            plan->reqResultObj || !resultFields.empty()) {
            // Store all the slots from 'exprPathSlotMap' into the 'individualSlots' vector.
            SbSlotVector individualSlots;
            for (const auto& exprPath : exprPaths) {
                if (exprPathSlotMap.count(exprPath)) {
                    individualSlots.push_back(exprPathSlotMap[exprPath]);
                }
            }

            // Create a BlockToRowStage.
            auto [outStage, outSlots] =
                buildBlockToRow(std::move(stage), _state, outputs, std::move(individualSlots));
            stage = std::move(outStage);

            // For each slot that was in 'exprPathSlotMap', replace all occurrences of the original
            // slot with the corresponding new slot produced by the BlockToRow stage.
            size_t outSlotsIdx = 0;
            for (const auto& exprPath : exprPaths) {
                if (exprPathSlotMap.count(exprPath)) {
                    // Update the slot in 'exprPathSlotMap' corresponding to 'exprPath'.
                    auto slot = outSlots[outSlotsIdx++];
                    exprPathSlotMap[exprPath] = slot;
                }
            }
        }
    }

    // Call the generateProjExpressions() helper lambda. If 'outputs.hasBlockOutput()' was false
    // above, then this will be the first time we're invoking the lambda, otherwise it will be
    // the second time we're invoking the lambda.
    generateProjExpressions();

    // generateProjExpressions() just projected all of the MQL Expressions to slots, so now
    // we need to update 'nodes' to refer to these slots.
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        const auto& path = paths[i];

        if (auto it = exprPathSlotMap.find(path); it != exprPathSlotMap.end()) {
            auto slot = it->second;
            node = ProjectNode(SbExpr{slot});
        }
    }

    auto projectType = isInclusion ? projection_ast::ProjectType::kInclusion
                                   : projection_ast::ProjectType::kExclusion;

    boost::optional<SbSlotVector> resultFieldSlots;

    // Generate any single-field projections that are needed for 'resultFields'.
    if (!resultFields.empty() && !plan->reqResultObj) {
        SbExprOptSbSlotVector projects;

        for (const auto& field : resultFields) {
            auto expr = preimageAllowedFields.count(field)
                ? SbExpr{outputs.get(std::pair(kField, field))}
                : SbExpr{_state.getNothingSlot()};

            projects.emplace_back(
                generateSingleFieldProjection(
                    _state, projectType, paths, nodes, std::move(expr), nullptr, field),
                boost::none);
        }

        auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(projects));
        stage = std::move(outStage);

        resultFieldSlots.emplace(std::move(outSlots));
    }

    // Invalidate the kField slots in 'outputs' that correspond to 'resultPaths'.
    for (const auto& resultPath : plan->resultPaths) {
        outputs.clear(std::make_pair(PlanStageSlots::kField, resultPath));
    }

    // Update the kField slots in 'outputs' that correspond to 'nothingPaths'.
    if (!plan->nothingPaths.empty()) {
        auto nothingSlot = _state.getNothingSlot();
        for (auto& nothingPath : plan->nothingPaths) {
            outputs.set(std::make_pair(PlanStageSlots::kField, std::move(nothingPath)),
                        nothingSlot);
        }
    }

    // Update the kField slots in 'outputs' that correspond to 'updatedPaths'.
    for (auto& updatedPath : plan->updatedPaths) {
        auto slot = exprPathSlotMap[updatedPath];
        outputs.set(std::make_pair(PlanStageSlots::kField, std::move(updatedPath)), slot);
    }

    // If 'resultFieldSlots' is set, then update the kField slots in 'outputs' that correspond
    // to 'resultFields'.
    if (resultFieldSlots) {
        size_t i = 0;
        for (const auto& field : resultFields) {
            outputs.set(std::pair(PlanStageSlots::kField, field), (*resultFieldSlots)[i]);
            ++i;
        }
    }

    // Produce a materialized result object if needed.
    if (plan->reqResultObj) {
        // Use the child's result object as the input, or for covered projections use null as input.
        auto inputObj =
            !isCoveredProjection ? SbExpr{outputs.getResultObj()} : b.makeNullConstant();

        SbExpr resultExpr = generateProjection(
            _state, projectType, std::move(paths), std::move(nodes), std::move(inputObj));

        auto [outStage, outSlots] = b.makeProject(std::move(stage), std::move(resultExpr));
        stage = std::move(outStage);

        outputs.setResultObj(outSlots[0]);
    }

    // When 'reqResultInfo' is true and 'isCoveredProjection' is true, we don't ask our child for
    // a result base object. Instead, we set the result base object to be an empty object.
    if (plan->reqResultInfo && isCoveredProjection) {
        outputs.setResultInfoBaseObj(SbSlot{_state.getEmptyObjSlot()});
    }

    // For each path in 'resultsPaths' that is not present in 'outputs', we read the path's value
    // from 'outputs.getResultObj()' and project it to a slot and store the path/slot pair into
    // 'outputs'.
    auto resultPathsToSet = filterVector(
        std::move(plan->resultPaths), [&](auto&& p) { return !outputs.has(std::pair(kField, p)); });

    if (!resultPathsToSet.empty()) {
        auto resultSlot = outputs.getIfExists(PlanStageSlots::kResult);

        auto [outStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                         resultPathsToSet,
                                                         resultSlot,
                                                         root->nodeId(),
                                                         &_slotIdGenerator,
                                                         _state,
                                                         &outputs);
        stage = std::move(outStage);

        for (size_t i = 0; i < resultPathsToSet.size(); ++i) {
            auto& p = resultPathsToSet[i];
            auto slot = outSlots[i];
            outputs.set(std::make_pair(PlanStageSlots::kField, std::move(p)), slot);
        }
    }

    // If 'outputs' has a materialized result and 'reqs' was expecting ResultInfo, then convert
    // the materialized result into a ResultInfo.
    if (outputs.hasResultObj() && plan->reqResultInfo) {
        outputs.setResultInfoBaseObj(outputs.getResultObj());
    }

    // If our parent requires ResultInfo, then we take this projection's effects (excluding
    // fields dropped by 'reqs.getResultInfoTrackedFieldSet()') and we add these effects to
    // 'resultInfoChanges' in 'outputs'.
    if (plan->reqResultInfo) {
        outputs.addEffectsToResultInfo(_state, reqs, plan->newEffects);
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

    auto postimageAllowedFieldSets = getPostimageAllowedFieldSets(orn->children);

    auto outputs = PlanStageSlots::makeMergedPlanStageSlots(
        _state, root->nodeId(), childReqs, inputStagesAndSlots, postimageAllowedFieldSets);

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
    auto coll = getCurrentCollection(reqs);
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
        outputs.set(kReturnKey, SbSlot{_slotIdGenerator.generate(), TypeSignature::kObjectType});
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
    // Note: This implementation is incomplete and doesn't support all the possible cases that
    // the query planner could potentially generate. This implementation is only enabled when
    // 'internalQueryForceIntersectionPlans' is set to true.

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
    auto innerPrefetchedResultSlot = innerOutputs.getIfExists(kPrefetchedResult);

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
    if (reqs.has(kPrefetchedResult) && innerPrefetchedResultSlot) {
        auto slot = *innerPrefetchedResultSlot;
        innerProjectSlots.push_back(slot.slotId);
        outputs.set(kPrefetchedResult, slot);
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
    // Note: This implementation is incomplete and doesn't support all the possible cases that
    // the query planner could potentially generate. This implementation is only enabled when
    // 'internalQueryForceIntersectionPlans' is set to true.

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
                              .clear(kIndexKeyPattern)
                              .clear(kPrefetchedResult);
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
    if (reqs.has(kPrefetchedResult)) {
        auto innerPrefetchedResultSlot = innerOutputs.get(kPrefetchedResult);
        innerProjectSlots.push_back(innerPrefetchedResultSlot.slotId);
        outputs.set(kPrefetchedResult, innerPrefetchedResultSlot);
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

    std::vector<const FieldSet*> postimageAllowedFieldSets;
    postimageAllowedFieldSets.reserve(2);
    postimageAllowedFieldSets.push_back(&getPostimageAllowedFields(root));
    postimageAllowedFieldSets.push_back(&getPostimageAllowedFields(root));

    auto outputs = PlanStageSlots::makeMergedPlanStageSlots(
        _state, root->nodeId(), reqs, inputStagesAndSlots, postimageAllowedFieldSets);

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
const Expression* getNExprFromAccumulatorN(const WindowFunctionStatement& wfStmt) {
    auto opName = wfStmt.expr->getOpName();
    if (opName == AccumulatorTop::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionTop, AccumulatorTop>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorBottom::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionBottom, AccumulatorBottom>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorTopN::getName()) {
        return dynamic_cast<window_function::ExpressionN<WindowFunctionTopN, AccumulatorTopN>*>(
                   wfStmt.expr.get())
            ->nExpr.get();
    } else if (opName == AccumulatorBottomN::getName()) {
        return dynamic_cast<
                   window_function::ExpressionN<WindowFunctionBottomN, AccumulatorBottomN>*>(
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

SbExpr getDefaultValueExpr(StageBuilderState& state, const WindowFunctionStatement& wfStmt) {
    SbExprBuilder b(state);

    if (wfStmt.expr->getOpName() == "$shift") {
        auto defaultVal =
            dynamic_cast<window_function::ExpressionShift*>(wfStmt.expr.get())->defaultVal();

        if (defaultVal) {
            auto val = sbe::value::makeValue(*defaultVal);
            return b.makeConstant(val.first, val.second);
        } else {
            return b.makeNullConstant();
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
    using BuildOutput =
        std::tuple<SbStage, std::vector<std::string>, SbSlotVector, StringMap<SbSlot>>;

    WindowStageBuilder(StageBuilderState& state,
                       const PlanStageReqs& forwardingReqs,
                       const PlanStageSlots& outputs,
                       const WindowNode* wn,
                       bool allowDiskUse,
                       SbSlotVector currSlotsIn)
        : state(state),
          forwardingReqs(forwardingReqs),
          outputs(outputs),
          windowNode(wn),
          allowDiskUse(allowDiskUse),
          b(state, wn->nodeId()),
          currSlots(std::move(currSlotsIn)) {
        // Initialize 'boundTestingSlots'.
        boundTestingSlots.reserve(currSlots.size());
        for (size_t i = 0; i < currSlots.size(); ++i) {
            boundTestingSlots.emplace_back(SbSlot{state.slotId()});
        }
    }

    BuildOutput build(SbStage stage);

    size_t ensureSlotInBuffer(SbSlot slot) {
        for (size_t i = 0; i < currSlots.size(); i++) {
            if (slot.getId() == currSlots[i].getId()) {
                return i;
            }
        }
        currSlots.push_back(slot);
        boundTestingSlots.push_back(SbSlot{state.slotId()});
        for (auto& frameFirstSlots : windowFrameFirstSlots) {
            frameFirstSlots.push_back(SbSlot{state.slotId()});
        }
        for (auto& frameLastSlots : windowFrameLastSlots) {
            frameLastSlots.push_back(SbSlot{state.slotId()});
        }
        return currSlots.size() - 1;
    }

    size_t registerFrameFirstSlots() {
        windowFrameFirstSlots.push_back(SbSlotVector{});
        auto& frameFirstSlots = windowFrameFirstSlots.back();
        frameFirstSlots.clear();
        for (size_t i = 0; i < currSlots.size(); i++) {
            frameFirstSlots.push_back(SbSlot{state.slotId()});
        }
        return windowFrameFirstSlots.size() - 1;
    }

    size_t registerFrameLastSlots() {
        windowFrameLastSlots.push_back(SbSlotVector{});
        auto& frameLastSlots = windowFrameLastSlots.back();
        frameLastSlots.clear();
        for (size_t i = 0; i < currSlots.size(); i++) {
            frameLastSlots.push_back(SbSlot{state.slotId()});
        }
        return windowFrameLastSlots.size() - 1;
    }

    std::pair<SbStage, size_t> generatePartitionExpr(SbStage stage) {
        // Get stages for partition by.
        size_t partitionSlotCount = 0;
        if (windowNode->partitionBy) {
            auto partitionSlot = SbSlot{state.slotId()};
            ensureSlotInBuffer(partitionSlot);
            partitionSlotCount++;
            auto rootSlotOpt = outputs.getResultObjIfExists();
            auto partitionExpr =
                generateExpression(state, windowNode->partitionBy->get(), rootSlotOpt, outputs);

            // Assert partition slot is not an array.
            auto frameId = state.frameId();
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

            auto [outStage, _] =
                b.makeProject(std::move(stage), std::pair(std::move(partitionExpr), partitionSlot));
            stage = std::move(outStage);
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
    std::tuple<SbStage, SbSlot, SbSlot> getDocumentBoundSlot(SbStage stage) {
        if (!documentBoundSlot) {
            documentBoundSlot = SbSlot{state.slotId()};

            SbAggExprVector sbAggExprs;
            sbAggExprs.emplace_back(
                SbAggExpr{SbExpr{} /*init*/,
                          SbExpr{} /*blockAgg*/,
                          b.makeFunction("sum", b.makeInt32Constant(1)) /*agg*/},
                *documentBoundSlot);

            auto [outStage, _] = b.makeAggProject(std::move(stage), std::move(sbAggExprs));
            stage = std::move(outStage);
        }
        auto documentBoundSlotIdx = ensureSlotInBuffer(*documentBoundSlot);
        return {std::move(stage), *documentBoundSlot, boundTestingSlots[documentBoundSlotIdx]};
    }

    std::tuple<SbStage, SbSlot, SbSlot> getSortBySlot(SbStage stage) {
        if (!sortBySlot) {
            sortBySlot = SbSlot{state.slotId()};
            tassert(7914602,
                    "Expected to have a single sort component",
                    windowNode->sortBy && windowNode->sortBy->size() == 1);

            FieldPath fp("CURRENT." + windowNode->sortBy->front().fieldPath->fullPath());

            auto rootSlotOpt = outputs.getResultObjIfExists();
            auto sortByExpr =
                generateExpressionFieldPath(state, fp, boost::none, rootSlotOpt, outputs);

            auto [outStage, _] =
                b.makeProject(std::move(stage), std::pair(std::move(sortByExpr), *sortBySlot));
            stage = std::move(outStage);
        }

        auto sortBySlotIdx = ensureSlotInBuffer(*sortBySlot);
        return {std::move(stage), *sortBySlot, boundTestingSlots[sortBySlotIdx]};
    }

    // Calculate slot for range and time range based window bounds
    std::tuple<SbStage, SbSlot, SbSlot> getRangeBoundSlot(SbStage stage,
                                                          boost::optional<TimeUnit> unit) {
        auto projectRangeBoundSlot = [&](StringData typeCheckFn, SbExpr failExpr) {
            auto slot = state.slotId();
            auto [outStage, sortBySlot, _] = getSortBySlot(std::move(stage));
            stage = std::move(outStage);

            auto frameId = state.frameIdGenerator->generate();
            auto sortByVar = SbVar{frameId, 0};
            auto binds = SbExpr::makeSeq(b.makeFillEmptyNull(b.makeVariable(sortBySlot)));

            auto checkType = b.makeLet(
                frameId,
                std::move(binds),
                b.makeIf(b.makeFunction(typeCheckFn, sortByVar), sortByVar, std::move(failExpr)));

            auto [projectStage, __] =
                b.makeProject(std::move(stage), std::pair(std::move(checkType), slot));
            stage = std::move(projectStage);

            return slot;
        };

        if (unit) {
            if (!timeRangeBoundSlot) {
                timeRangeBoundSlot = projectRangeBoundSlot(
                    "isDate",
                    b.makeFail(ErrorCodes::Error{7956500},
                               "Invalid range: Expected the sortBy field to be a date"));
            }
            auto timeRangeBoundSlotIdx = ensureSlotInBuffer(*timeRangeBoundSlot);
            return {
                std::move(stage), *timeRangeBoundSlot, boundTestingSlots[timeRangeBoundSlotIdx]};
        } else {
            if (!rangeBoundSlot) {
                rangeBoundSlot = projectRangeBoundSlot(
                    "isNumber",
                    b.makeFail(ErrorCodes::Error{7993103},
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

    SbExpr convertSbExprToArgExpr(SbExpr argExpr) {
        if (argExpr.isSlotExpr()) {
            ensureSlotInBuffer(argExpr.toSlot());
            return argExpr;
        } else if (argExpr.isConstantExpr()) {
            return argExpr;
        } else {
            auto argSlot = SbSlot{state.slotId()};
            windowArgProjects.emplace_back(std::move(argExpr), argSlot);
            ensureSlotInBuffer(argSlot);
            return SbExpr{argSlot};
        }
    }

    SbExpr getArgExpr(Expression* arg) {
        auto rootSlotOpt = outputs.getResultObjIfExists();
        auto argExpr = generateExpression(state, arg, rootSlotOpt, outputs);

        return convertSbExprToArgExpr(std::move(argExpr));
    }

    std::tuple<SbStage, AccumInputsPtr, AccumInputsPtr> generateArgs(
        SbStage stage, const WindowFunctionStatement& outputField, bool removable) {
        auto rootSlotOpt = outputs.getResultObjIfExists();
        auto collatorSlot = state.getCollatorSlot();

        // Get init expression arg for relevant functions
        auto getUnitArg = [&](window_function::ExpressionWithUnit* expr) {
            auto unit = expr->unitInMillis();
            if (unit) {
                return b.makeInt64Constant(*unit);
            } else {
                return b.makeNullConstant();
            }
        };

        auto opName = outputField.expr->getOpName();

        AccumInputsPtr initInputs;

        if (opName == AccumulatorExpMovingAvg::kName) {
            auto emaExpr =
                dynamic_cast<window_function::ExpressionExpMovingAvg*>(outputField.expr.get());

            SbExpr alpha;
            if (auto n = emaExpr->getN()) {
                alpha = b.makeDecimalConstant(
                    Decimal128(2).divide(Decimal128(n.get()).add(Decimal128(1))));
            } else {
                alpha = b.makeDecimalConstant(emaExpr->getAlpha().get());
            }

            initInputs = std::make_unique<InitExpMovingAvgInputs>(std::move(alpha));
        } else if (opName == AccumulatorIntegral::kName) {
            SbExpr unitExpr = getUnitArg(
                dynamic_cast<window_function::ExpressionWithUnit*>(outputField.expr.get()));

            initInputs = std::make_unique<InitIntegralInputs>(std::move(unitExpr));
        } else if (isAccumulatorN(opName)) {
            auto nExprPtr = getNExprFromAccumulatorN(outputField);
            auto maxSizeExpr = generateExpression(state, nExprPtr, rootSlotOpt, outputs);

            initInputs = std::make_unique<InitAccumNInputs>(std::move(maxSizeExpr),
                                                            b.makeBoolConstant(false));
        }

        AccumInputsPtr addRemoveInputs;

        if (opName == "$covarianceSamp" || opName == "$covariancePop") {
            if (auto expr = dynamic_cast<ExpressionArray*>(outputField.expr->input().get());
                expr && expr->getChildren().size() == 2) {
                auto argX = expr->getChildren()[0].get();
                auto argY = expr->getChildren()[1].get();

                addRemoveInputs =
                    std::make_unique<AddCovarianceInputs>(getArgExpr(argX), getArgExpr(argY));
            } else if (auto expr =
                           dynamic_cast<ExpressionConstant*>(outputField.expr->input().get());
                       expr && expr->getValue().isArray() &&
                       expr->getValue().getArray().size() == 2) {
                auto array = expr->getValue().getArray();
                auto bson = BSON("x" << array[0] << "y" << array[1]);
                auto [argXTag, argXVal] =
                    sbe::bson::convertFrom<false /* View */>(bson.getField("x"));
                auto [argYTag, argYVal] =
                    sbe::bson::convertFrom<false /* View */>(bson.getField("y"));

                addRemoveInputs = std::make_unique<AddCovarianceInputs>(
                    b.makeConstant(argXTag, argXVal), b.makeConstant(argYTag, argYVal));
            } else {
                addRemoveInputs = std::make_unique<AddCovarianceInputs>(b.makeNullConstant(),
                                                                        b.makeNullConstant());
            }
        } else if (opName == "$integral") {
            auto [outStage, sortBySlot, _] = getSortBySlot(std::move(stage));
            stage = std::move(outStage);

            addRemoveInputs = std::make_unique<AddIntegralInputs>(
                getArgExpr(outputField.expr->input().get()), b.makeVariable(sortBySlot));
        } else if (opName == "$linearFill") {
            auto [outStage, sortBySlot, _] = getSortBySlot(std::move(stage));
            stage = std::move(outStage);

            addRemoveInputs = std::make_unique<AddLinearFillInputs>(
                getArgExpr(outputField.expr->input().get()), b.makeVariable(sortBySlot));
        } else if (opName == "$rank" || opName == "$denseRank") {
            auto isAscending = windowNode->sortBy->front().isAscending;

            addRemoveInputs = std::make_unique<AddRankInputs>(
                getArgExpr(outputField.expr->input().get()), b.makeBoolConstant(isAscending));
        } else if (isTopBottomN(outputField)) {
            tassert(8155715, "Root slot should be set", rootSlotOpt);

            SbExpr valueExpr;

            if (auto expObj = dynamic_cast<ExpressionObject*>(outputField.expr->input().get())) {
                for (auto& [key, value] : expObj->getChildExpressions()) {
                    if (key == AccumulatorN::kFieldNameOutput) {
                        auto outputExpr =
                            generateExpression(state, value.get(), rootSlotOpt, outputs);
                        valueExpr =
                            convertSbExprToArgExpr(b.makeFillEmptyNull(std::move(outputExpr)));
                        break;
                    }
                }
            } else if (auto expConst =
                           dynamic_cast<ExpressionConstant*>(outputField.expr->input().get())) {
                auto objConst = expConst->getValue();
                tassert(8155716,
                        str::stream() << opName << " window funciton must have an object argument",
                        objConst.isObject());
                auto objBson = objConst.getDocument().toBson();
                auto outputField = objBson.getField(AccumulatorN::kFieldNameOutput);
                if (outputField.ok()) {
                    auto [outputTag, outputVal] =
                        sbe::bson::convertFrom<false /* View */>(outputField);
                    auto outputExpr = b.makeConstant(outputTag, outputVal);
                    valueExpr = b.makeFillEmptyNull(std::move(outputExpr));
                }
            } else {
                tasserted(8155717,
                          str::stream()
                              << opName << " window function must have an object argument");
            }

            tassert(8155718,
                    str::stream() << opName
                                  << " window function must have an output field in the argument",
                    !valueExpr.isNull());

            SbExpr sortByExpr;
            auto sortSpecExpr = b.makeVariable(state.getSortSpecSlot(&outputField));

            if (removable) {
                auto key = collatorSlot ? b.makeFunction("generateSortKey",
                                                         sortSpecExpr.clone(),
                                                         b.makeVariable(*rootSlotOpt),
                                                         b.makeVariable(*collatorSlot))
                                        : b.makeFunction("generateSortKey",
                                                         sortSpecExpr.clone(),
                                                         b.makeVariable(*rootSlotOpt));

                sortByExpr = convertSbExprToArgExpr(std::move(key));
            } else {
                auto key = collatorSlot ? b.makeFunction("generateCheapSortKey",
                                                         sortSpecExpr.clone(),
                                                         b.makeVariable(*rootSlotOpt),
                                                         b.makeVariable(*collatorSlot))
                                        : b.makeFunction("generateCheapSortKey",
                                                         sortSpecExpr.clone(),
                                                         b.makeVariable(*rootSlotOpt));

                sortByExpr = b.makeFunction("sortKeyComponentVectorToArray", std::move(key));
            }

            addRemoveInputs = std::make_unique<AddTopBottomNInputs>(
                std::move(valueExpr), std::move(sortByExpr), std::move(sortSpecExpr));
        } else {
            addRemoveInputs =
                std::make_unique<AddSingleInput>(getArgExpr(outputField.expr->input().get()));
        }

        return {std::move(stage), std::move(initInputs), std::move(addRemoveInputs)};
    }

    SbStage generateInitsAddsAndRemoves(SbStage stage,
                                        const WindowFunctionStatement& outputField,
                                        const WindowOp& windowOp,
                                        bool removable,
                                        AccumInputsPtr initInputs,
                                        AccumInputsPtr addRemoveInputs,
                                        SbWindow& window) {
        // Create init/add/remove expressions.
        if (removable) {
            AccumInputsPtr addInputs =
                addRemoveInputs ? addRemoveInputs->clone() : AccumInputsPtr{};
            AccumInputsPtr removeInputs = std::move(addRemoveInputs);

            window.initExprs = windowOp.buildInitialize(state, std::move(initInputs));
            window.addExprs = windowOp.buildAddAggs(state, std::move(addInputs));
            window.removeExprs = windowOp.buildRemoveAggs(state, std::move(removeInputs));
        } else {
            auto accOp = AccumOp{windowOp.getOpName()};

            // Call buildInitialize() to generate the accum initialize expressions.
            window.initExprs = accOp.buildInitialize(state, std::move(initInputs));

            // Call buildAddExprs() to generate the accum exprs, and then call buildAddAggs()
            // to generate the accum aggs and store the result into 'window.addExprs'.
            auto accArgs = accOp.buildAddExprs(state, std::move(addRemoveInputs));
            window.addExprs = accOp.buildAddAggs(state, std::move(accArgs));

            // Populate 'window.removeExprs' with null SbExprs.
            window.removeExprs = SbExpr::Vector{};
            window.removeExprs.resize(window.addExprs.size());
        }

        for (size_t i = 0; i < window.initExprs.size(); i++) {
            window.windowExprSlots.emplace_back(SbSlot{state.slotId()});
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
                               SbWindow& window) {
        auto makeOffsetBoundExpr = [&](SbSlot boundSlot,
                                       std::pair<sbe::value::TypeTags, sbe::value::Value> offset =
                                           {sbe::value::TypeTags::Nothing, 0},
                                       boost::optional<TimeUnit> unit = boost::none) {
            if (offset.first == sbe::value::TypeTags::Nothing) {
                return b.makeVariable(boundSlot);
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
                return b.makeFunction("dateAdd",
                                      b.makeVariable(*state.getTimeZoneDBSlot()),
                                      b.makeVariable(boundSlot),
                                      b.makeConstant(unitTag, unitVal),
                                      b.makeConstant(longOffsetTag, longOffsetVal),
                                      b.makeConstant(timezoneTag, timezoneVal));
            } else {
                return b.makeBinaryOp(sbe::EPrimBinary::add,
                                      b.makeVariable(boundSlot),
                                      b.makeConstant(offset.first, offset.second));
            }
        };
        auto makeLowBoundExpr = [&](SbSlot boundSlot,
                                    SbSlot boundTestingSlot,
                                    std::pair<sbe::value::TypeTags, sbe::value::Value> offset =
                                        {sbe::value::TypeTags::Nothing, 0},
                                    boost::optional<TimeUnit> unit = boost::none) {
            // Use three way comparison to compare special values like NaN.
            return b.makeBinaryOp(sbe::EPrimBinary::greaterEq,
                                  b.makeBinaryOp(sbe::EPrimBinary::cmp3w,
                                                 b.makeVariable(boundTestingSlot),
                                                 makeOffsetBoundExpr(boundSlot, offset, unit)),
                                  b.makeInt32Constant(0));
        };
        auto makeHighBoundExpr = [&](SbSlot boundSlot,
                                     SbSlot boundTestingSlot,
                                     std::pair<sbe::value::TypeTags, sbe::value::Value> offset =
                                         {sbe::value::TypeTags::Nothing, 0},
                                     boost::optional<TimeUnit> unit = boost::none) {
            // Use three way comparison to compare special values like NaN.
            return b.makeBinaryOp(sbe::EPrimBinary::lessEq,
                                  b.makeBinaryOp(sbe::EPrimBinary::cmp3w,
                                                 b.makeVariable(boundTestingSlot),
                                                 makeOffsetBoundExpr(boundSlot, offset, unit)),
                                  b.makeInt32Constant(0));
        };
        auto makeLowUnboundedExpr = [&](const WindowBounds::Unbounded&) {
            window.lowBoundExpr = SbExpr{};
        };
        auto makeHighUnboundedExpr = [&](const WindowBounds::Unbounded&) {
            window.highBoundExpr = SbExpr{};
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
                b.makeFunction("aggLinearFillCanAdd", b.makeVariable(window.windowExprSlots[0]));
        }

        return stage;
    }

    std::pair<SbStage, SbExpr> generateFinalExpr(SbStage stage,
                                                 const WindowFunctionStatement& outputField,
                                                 const WindowOp& windowOp,
                                                 bool removable,
                                                 SbWindow& window) {
        using ExpressionWithUnit = window_function::ExpressionWithUnit;

        // Build extra arguments for finalize expressions.
        auto getModifiedExpr = [&](SbExpr argExpr, SbSlotVector& newSlots) {
            if (argExpr.isSlotExpr()) {
                auto idx = ensureSlotInBuffer(argExpr.toSlot());
                return b.makeVariable(newSlots[idx]);
            } else if (argExpr.isConstantExpr()) {
                return argExpr.clone();
            } else {
                MONGO_UNREACHABLE;
            }
        };

        AccumInputsPtr finalizeInputs;

        if (outputField.expr->getOpName() == "$derivative") {
            auto [outStage, sortBySlot, _] = getSortBySlot(std::move(stage));
            stage = std::move(outStage);

            auto inputExpr = getArgExpr(outputField.expr->input().get());
            auto sortByExpr = b.makeVariable(sortBySlot);

            auto u = dynamic_cast<ExpressionWithUnit*>(outputField.expr.get())->unitInMillis();
            auto unit = u ? b.makeInt64Constant(*u) : b.makeNullConstant();

            auto& frameFirstSlots = windowFrameFirstSlots[*windowFrameFirstSlotIdx.back()];
            auto& frameLastSlots = windowFrameLastSlots[*windowFrameLastSlotIdx.back()];
            auto frameFirstInput = getModifiedExpr(inputExpr.clone(), frameFirstSlots);
            auto frameLastInput = getModifiedExpr(std::move(inputExpr), frameLastSlots);
            auto frameFirstSortBy = getModifiedExpr(sortByExpr.clone(), frameFirstSlots);
            auto frameLastSortBy = getModifiedExpr(std::move(sortByExpr), frameLastSlots);

            finalizeInputs = std::make_unique<FinalizeDerivativeInputs>(std::move(unit),
                                                                        std::move(frameFirstInput),
                                                                        std::move(frameFirstSortBy),
                                                                        std::move(frameLastInput),
                                                                        std::move(frameLastSortBy));
        } else if (outputField.expr->getOpName() == "$linearFill") {
            auto [outStage, sortBySlot, _] = getSortBySlot(std::move(stage));
            stage = std::move(outStage);

            auto sortByExpr = b.makeVariable(sortBySlot);

            finalizeInputs = std::make_unique<FinalizeLinearFillInputs>(std::move(sortByExpr));
        } else if (outputField.expr->getOpName() == "$first" && removable) {
            auto inputExpr = getArgExpr(outputField.expr->input().get());
            auto& frameFirstSlots = windowFrameFirstSlots[*windowFrameFirstSlotIdx.back()];

            auto frameFirstInput = getModifiedExpr(std::move(inputExpr), frameFirstSlots);

            finalizeInputs = std::make_unique<FinalizeWindowFirstLastInputs>(
                std::move(frameFirstInput), b.makeNullConstant());
        } else if (outputField.expr->getOpName() == "$last" && removable) {
            auto inputExpr = getArgExpr(outputField.expr->input().get());
            auto& frameLastSlots = windowFrameLastSlots[*windowFrameLastSlotIdx.back()];

            auto frameLastInput = getModifiedExpr(std::move(inputExpr), frameLastSlots);

            finalizeInputs = std::make_unique<FinalizeWindowFirstLastInputs>(
                std::move(frameLastInput), b.makeNullConstant());
        } else if (outputField.expr->getOpName() == "$shift") {
            // The window bounds of $shift is DocumentBounds{shiftByPos, shiftByPos}, so it is a
            // window frame of size 1. So $shift is equivalent to $first or $last on the window
            // bound.
            tassert(8293501, "$shift is expected to be removable", removable);

            auto inputExpr = getArgExpr(outputField.expr->input().get());
            auto& frameFirstSlots = windowFrameFirstSlots[*windowFrameFirstSlotIdx.back()];

            auto frameFirstInput = getModifiedExpr(std::move(inputExpr), frameFirstSlots);

            finalizeInputs = std::make_unique<FinalizeWindowFirstLastInputs>(
                std::move(frameFirstInput), getDefaultValueExpr(state, outputField));
        } else if (isTopBottomN(outputField)) {
            finalizeInputs = std::make_unique<FinalizeTopBottomNInputs>(
                b.makeVariable(state.getSortSpecSlot(&outputField)));
        }

        // Build finalize.
        SbExpr finalExpr;

        if (removable) {
            finalExpr =
                windowOp.buildFinalize(state, std::move(finalizeInputs), window.windowExprSlots);
        } else {
            auto accOp = AccumOp{windowOp.getOpName()};
            finalExpr =
                accOp.buildFinalize(state, std::move(finalizeInputs), window.windowExprSlots);
        }

        // Deal with empty window for finalize expressions.
        auto emptyWindowExpr = [&] {
            StringData opName = outputField.expr->getOpName();

            if (opName == "$sum") {
                return b.makeInt32Constant(0);
            } else if (opName == "$push" || opName == AccumulatorAddToSet::kName) {
                auto [tag, val] = sbe::value::makeNewArray();
                return b.makeConstant(tag, val);
            } else if (opName == "$shift") {
                return getDefaultValueExpr(state, outputField);
            } else {
                return b.makeNullConstant();
            }
        }();

        if (finalExpr) {
            finalExpr =
                b.makeIf(b.makeFunction("exists", b.makeVariable(window.windowExprSlots[0])),
                         std::move(finalExpr),
                         std::move(emptyWindowExpr));
        } else {
            finalExpr = b.makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                                       b.makeVariable(window.windowExprSlots[0]),
                                       std::move(emptyWindowExpr));
        }

        return {std::move(stage), std::move(finalExpr)};
    }

private:
    StageBuilderState& state;
    const PlanStageReqs& forwardingReqs;
    const PlanStageSlots& outputs;
    const WindowNode* windowNode;
    const bool allowDiskUse;
    SbBuilder b;

    SbSlotVector currSlots;
    SbSlotVector boundTestingSlots;
    std::vector<SbSlotVector> windowFrameFirstSlots;
    std::vector<SbSlotVector> windowFrameLastSlots;

    // Calculate slot for document position based window bounds, and add corresponding stages.
    boost::optional<SbSlot> documentBoundSlot;

    // Calculate sort-by slot, and add corresponding stages.
    boost::optional<SbSlot> sortBySlot;

    // Calculate slot for range and time range based window bounds
    boost::optional<SbSlot> rangeBoundSlot;
    boost::optional<SbSlot> timeRangeBoundSlot;

    std::vector<boost::optional<size_t>> windowFrameFirstSlotIdx;
    std::vector<boost::optional<size_t>> windowFrameLastSlotIdx;

    // We project window function input arguments in order to avoid repeated evaluation
    // for both add and remove expressions.
    SbExprOptSbSlotVector windowArgProjects;

    SbExpr::Vector windowFinalExprs;
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
    std::vector<SbWindow> windows;

    for (size_t i = 0; i < windowNode->outputFields.size(); i++) {
        auto& outputField = windowNode->outputFields[i];

        WindowBounds windowBounds = outputField.expr->bounds();

        // Check whether window is removable or not.
        bool removable = isWindowRemovable(windowBounds);

        auto [genArgsStage, initInputs, addRemoveInputs] =
            generateArgs(std::move(stage), outputField, removable);
        stage = std::move(genArgsStage);

        SbWindow window{};
        auto windowOp = WindowOp{outputField.expr->getOpName()};

        // Create init/add/remove expressions.
        stage = generateInitsAddsAndRemoves(std::move(stage),
                                            outputField,
                                            windowOp,
                                            removable,
                                            std::move(initInputs),
                                            std::move(addRemoveInputs),
                                            window);

        // Create frame first and last slots if the window requires.
        createFrameFirstAndLastSlots(outputField, removable);

        // Build bound expressions.
        stage = generateBoundExprs(std::move(stage), outputField, windowBounds, window);

        // Build extra arguments for finalize expressions.
        auto [genFinalExprStage, finalExpr] =
            generateFinalExpr(std::move(stage), outputField, windowOp, removable, window);
        stage = std::move(genFinalExprStage);

        windowFinalExprs.emplace_back(std::move(finalExpr));

        // Append the window definition to the end of the 'windows' vector.
        windows.emplace_back(std::move(window));
    }

    if (windowArgProjects.size() > 0) {
        auto [outStage, _] = b.makeProject(std::move(stage), std::move(windowArgProjects));
        stage = std::move(outStage);
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
    stage = b.makeWindow(std::move(stage),
                         std::move(currSlots),
                         std::move(boundTestingSlots),
                         partitionSlotCount,
                         std::move(windows),
                         state.getCollatorSlot(),
                         allowDiskUse);

    SbExprOptSbSlotVector windowFinalProjects;
    for (auto& expr : windowFinalExprs) {
        windowFinalProjects.emplace_back(std::move(expr), boost::none);
    }

    // Get final window outputs.
    auto [finalProjectStage, windowFinalSlots] =
        b.makeProject(std::move(stage), std::move(windowFinalProjects));

    // Build 'outputPathMap'.
    StringMap<SbSlot> outputPathMap;
    for (size_t i = 0; i < windowNode->outputFields.size(); ++i) {
        // If 'outputField' is not a dotted path, add 'outputField' and its corresponding slot
        // to 'outputPathMap'.
        auto& outputField = windowNode->outputFields[i];
        if (outputField.fieldName.find('.') == std::string::npos) {
            outputPathMap.emplace(outputField.fieldName, windowFinalSlots[i]);
        }
    }

    return {std::move(finalProjectStage),
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

    // Initially we populate 'currSlots' with the slots from '_data->metadataSlots'.
    SbSlotVector currSlots;
    for (auto slotId : _data->metadataSlots.getSlotVector()) {
        currSlots.emplace_back(SbSlot{slotId});
    }

    const bool allowDiskUse = _cq.getExpCtx()->allowDiskUse;

    // Create a WindowStageBuilder and call the build() method on it. This will generate all
    // the SBE expressions and SBE stages needed to implement the window stage.
    WindowStageBuilder builder(
        _state, forwardingReqs, outputs, windowNode, allowDiskUse, std::move(currSlots));

    auto [outStage, windowFields, windowFinalSlots, outputPathMap] =
        builder.build(std::move(stage));
    stage = std::move(outStage);

    // Update the kField slots in 'outputs' to reflect the effects of this stage.
    for (auto&& windowField : windowFields) {
        outputs.clearAffectedFields(windowField);
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

    auto collection = getCurrentCollection(reqs);
    auto expCtx = _cq.getExpCtxRaw();

    // Register search query parameter slots.
    auto limitSlot = _env->registerSlot("searchLimit"_sd,
                                        sbe::value::TypeTags::Nothing,
                                        0 /* val */,
                                        false /* owned */,
                                        &_slotIdGenerator);

    auto sortSpecSlot = _env->registerSlot(
        "searchSortSpec"_sd, sbe::value::TypeTags::Nothing, 0 /* val */, false, &_slotIdGenerator);

    bool isStoredSource = sn->searchQuery.getBoolField(mongot_cursor::kReturnStoredSourceArg);

    auto topLevelFields = getTopLevelFields(reqs.getFields());

    PlanStageSlots outputs;

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

        auto topLevelFieldSlots = _slotIdGenerator.generateMultiple(topLevelFields.size());

        auto stage = sbe::SearchCursorStage::createForStoredSource(expCtx->ns,
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
                                                                   sn->nodeId());

        for (size_t i = 0; i < topLevelFields.size(); ++i) {
            outputs.set(std::make_pair(PlanStageSlots::kField, topLevelFields[i]),
                        topLevelFieldSlots[i]);
        }

        return {std::move(stage), std::move(outputs)};
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

    // Join the idx scan stage with fetch stage.
    auto [outStage, outputDocSlot, _, topLevelFieldSlots] =
        makeLoopJoinForFetch(std::move(idxScanStage),
                             topLevelFields,
                             idxOutputs.get(kRecordId),
                             idxOutputs.get(kSnapshotId),
                             idxOutputs.get(kIndexIdent),
                             idxOutputs.get(kIndexKey),
                             idxOutputs.get(kIndexKeyPattern),
                             boost::none /* prefetchedResultSlot */,
                             collection,
                             _state,
                             sn->nodeId(),
                             sbe::makeSV() /* slotsToForward */);
    auto fetchStage = std::move(outStage);

    // Slot stores the resulting document.
    outputs.setResultObj(outputDocSlot);

    for (size_t i = 0; i < topLevelFields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, topLevelFields[i]),
                    topLevelFieldSlots[i]);
    }

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
                        SbSlot{bitmapSlotId,
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

CollectionPtr SlotBasedStageBuilder::getCurrentCollection(const PlanStageReqs& reqs) const {
    auto nss = reqs.getTargetNamespace();
    const auto coll = _collections.lookupCollection(nss);
    tassert(7922500,
            str::stream() << "No collection found that matches namespace '"
                          << nss.toStringForErrorMsg() << "'",
            coll != CollectionPtr::null);
    return CollectionPtr{coll.get()};
}

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::build(
    const QuerySolutionNode* root, const PlanStageReqs& reqsIn) {
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
                !reqsIn.getIsBuildingUnionForTailableCollScan()) {
                auto reqs = reqsIn;
                reqs.setIsBuildingUnionForTailableCollScan(true);
                return makeUnionForTailableCollScan(root, reqs);
            }
            [[fallthrough]];
        default:
            break;
    }

    // If 'root->fetched()' is true and 'reqsIn' has a kPrefetchedResult req, then we drop the
    // kPrefetchedResult req (as well as the kIndexKey, kIndexKeyPattern, kIndexIdent, and
    // kSnapshotId reqs) and we add a result object req.
    //
    // After we call build() on 'root' we will fix up the PlanStageSlots object returned by build().
    bool moveResultToPrefetchedResultSlot = false;
    boost::optional<PlanStageReqs> localReqs;

    if (reqsIn.has(kPrefetchedResult) && root->fetched()) {
        localReqs.emplace(reqsIn);
        localReqs->clear(kPrefetchedResult)
            .clear(kIndexKey)
            .clear(kIndexKeyPattern)
            .clear(kIndexIdent)
            .clear(kSnapshotId);
        localReqs->setResultObj();
        moveResultToPrefetchedResultSlot = true;
    }

    const PlanStageReqs& reqs = localReqs ? *localReqs : reqsIn;

    std::unique_ptr<ResultPlan> resultPlan;
    if (reqs.hasResult()) {
        resultPlan = getResultPlan(root, reqs);
    }

    // If 'resultPlan' is not null, set 'reqs' to refer to 'resultPlan->reqs'. Otherwise,
    // set 'reqs' to refer to 'reqsIn'.
    const PlanStageReqs& childReqs = resultPlan ? resultPlan->reqs : reqs;

    // Build the child.
    auto [childStage, childOutputs] = (this->*(kStageBuilders.at(stageType)))(root, childReqs);
    auto stage = std::move(childStage);
    auto outputs = std::move(childOutputs);

    // If 'reqsIn' had a result object requirement and we dropped that requirement and replaced
    // it with other requirements above, call makeResultUsingPlan() to create the materialized
    // result object.
    if (resultPlan) {
        auto [outStage, outSlots] =
            makeResultUsingPlan(std::move(stage), std::move(outputs), root, std::move(resultPlan));
        stage = std::move(outStage);
        outputs = std::move(outSlots);
    }

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
            for (const auto& f : missingFields) {
                tassert(6023424,
                        str::stream()
                            << "Expected build() for " << stageTypeToString(stageType)
                            << " to either satisfy all kField reqs, provide a materialized "
                            << "result object, or provide a compatible result base object",
                        reqs.hasResultInfo() && reqs.getResultInfoTrackedFieldSet().count(f) &&
                            outputs.hasResultInfo() &&
                            outputs.getResultInfoChanges().get(f) == FieldEffect::kKeep);
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

    // If 'moveResultToPrefetchedResultSlot' is true, then we need to fix up 'outputs'.
    if (moveResultToPrefetchedResultSlot) {
        auto nothingSlot = SbSlot{_state.getNothingSlot()};

        // Set kIndexKey to point to the slot holding the result object and set kIndexKey,
        // kIndexKeyPattern, kIndexIdent, and kSnapshotId to point to the Nothing slot.
        outputs.set(kPrefetchedResult, outputs.getResultObj());
        outputs.set(kIndexKey, nothingSlot);
        outputs.set(kIndexKeyPattern, nothingSlot);
        outputs.set(kIndexIdent, nothingSlot);
        outputs.set(kSnapshotId, nothingSlot);
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

        outputs.clearNonRequiredSlots(reqsIn, saveResultObj);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
