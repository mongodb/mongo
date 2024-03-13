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

#include "mongo/db/query/bind_input_params.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/js_function.h"
#include "mongo/db/exec/sbe/util/pcre.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/assert_util.h"

namespace mongo::input_params {
namespace {

class MatchExpressionParameterBindingVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionParameterBindingVisitor(stage_builder::PlanStageData& data,
                                           bool bindingCachedPlan)
        : _data(data), _bindingCachedPlan(bindingCachedPlan) {}

    void visit(const BitsAllClearMatchExpression* expr) final {
        visitBitTestExpression(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        visitBitTestExpression(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        visitBitTestExpression(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        visitBitTestExpression(expr);
    }

    void visit(const EqualityMatchExpression* expr) final {
        visitComparisonMatchExpression(expr);
    }
    void visit(const GTEMatchExpression* expr) final {
        visitComparisonMatchExpression(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        visitComparisonMatchExpression(expr);
    }
    void visit(const LTEMatchExpression* expr) final {
        visitComparisonMatchExpression(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        visitComparisonMatchExpression(expr);
    }

    void visit(const InMatchExpression* expr) final {
        auto slotId = getSlotId(expr->getInputParamId());
        if (!slotId) {
            return;
        }

        // The parameterization logic upstream should not have added a parameter marker if the $in
        // contains any regexes.
        tassert(6279503, "Unexpected parameter marker for $in with regexes", !expr->hasRegex());

        // Prepare the inList. We also store a shared_ptr pointing to the InListData object inside
        // 'data' to ensure the InListData object stays alive even if 'expr' gets mutated in the
        // future and drops its reference to the InListData.
        InListData* l = prepareInList(_data, expr->getInList());

        auto listTag = sbe::value::TypeTags::inListData;
        auto listVal = sbe::value::bitcastFrom<InListData*>(l);

        bindParam(*slotId, false, listTag, listVal);

        // Auto-parameterization should not kick in if the $in's list of equalities includes any
        // arrays, objects or null values.
        tassert(6988502, "Should not auto-parameterize $in with an array value", !l->hasArray());
        tassert(6988503, "Should not auto-parameterize $in with a null value", !l->hasNull());
        tassert(6988504, "Should not auto-parameterize $in with an object value", !l->hasObject());
    }

    void visit(const ModMatchExpression* expr) final {
        // Either both input parameter ids should be present, or neither should.
        auto divisorParam = expr->getDivisorInputParamId();
        auto remainderParam = expr->getRemainderInputParamId();
        if (!divisorParam) {
            tassert(6279507, "$mod had remainder param but not divisor param", !remainderParam);
            return;
        }
        tassert(6279508, "$mod had divisor param but not remainder param", remainderParam);

        if (auto slotId = getSlotId(*divisorParam)) {
            auto value = sbe::value::bitcastFrom<int64_t>(expr->getDivisor());
            bindParam(*slotId, true /*owned*/, sbe::value::TypeTags::NumberInt64, value);
        }

        if (auto slotId = getSlotId(*remainderParam)) {
            auto value = sbe::value::bitcastFrom<int64_t>(expr->getRemainder());
            bindParam(*slotId, true /*owned*/, sbe::value::TypeTags::NumberInt64, value);
        }
    }

    void visit(const RegexMatchExpression* expr) final {
        auto sourceRegexParam = expr->getSourceRegexInputParamId();
        auto compiledRegexParam = expr->getCompiledRegexInputParamId();
        if (!sourceRegexParam) {
            tassert(6279509, "$regex had compiled param but not source param", !compiledRegexParam);
            return;
        }
        tassert(6279510, "$regex had source param but not compiled param", compiledRegexParam);

        if (auto slotId = getSlotId(*sourceRegexParam)) {
            auto&& [bsonRegexTag, bsonRegexVal] =
                sbe::value::makeNewBsonRegex(expr->getString(), expr->getFlags());
            bindParam(*slotId, true /*owned*/, bsonRegexTag, bsonRegexVal);
        }

        if (auto slotId = getSlotId(*compiledRegexParam)) {
            auto&& [compiledRegexTag, compiledRegexVal] =
                sbe::makeNewPcreRegex(expr->getString(), expr->getFlags());
            bindParam(*slotId, true /*owned*/, compiledRegexTag, compiledRegexVal);
        }
    }

    void visit(const SizeMatchExpression* expr) final {
        auto slotId = getSlotId(expr->getInputParamId());
        if (!slotId) {
            return;
        }

        auto value = sbe::value::bitcastFrom<int32_t>(expr->getData());
        bindParam(*slotId, true /*owned*/, sbe::value::TypeTags::NumberInt32, value);
    }

    void visit(const TypeMatchExpression* expr) final {
        auto slotId = getSlotId(expr->getInputParamId());
        if (!slotId) {
            return;
        }

        auto value = sbe::value::bitcastFrom<int32_t>(expr->typeSet().getBSONTypeMask());

        bindParam(*slotId, true /*owned*/, sbe::value::TypeTags::NumberInt32, value);
    }

    void visit(const WhereMatchExpression* expr) final {
        auto slotId = getSlotId(expr->getInputParamId());
        if (!slotId) {
            return;
        }

        if (_bindingCachedPlan) {
            // Generally speaking, this visitor is non-destructive and does not mutate the
            // MatchExpression tree. However, in order to apply an optimization to avoid making a
            // copy of the 'JsFunction' object stored within 'WhereMatchExpression', we can transfer
            // its ownership from the match expression node into the SBE runtime environment. Hence,
            // we need to drop the const qualifier. This is a safe operation only when the plan is
            // being recovered from the SBE plan cache -- in this case, the visitor has exclusive
            // access to this match expression tree. However, in case of replanning, we
            // need to call recoverWhereExprPredicate to move predicates back to
            // WhereMatchExpressions.
            bindParam(*slotId,
                      true /*owned*/,
                      sbe::value::TypeTags::jsFunction,
                      sbe::value::bitcastFrom<JsFunction*>(
                          const_cast<WhereMatchExpression*>(expr)->extractPredicate().release()));
        } else {
            auto [typeTag, value] = std::pair(
                sbe::value::TypeTags::jsFunction,
                sbe::value::bitcastFrom<JsFunction*>(new JsFunction(expr->getPredicate())));

            bindParam(*slotId, true /*owned*/, typeTag, value);
        }
    }

    /**
     * These match expressions cannot contain parameter marks themselves (though their children
     * can).
     */
    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {}
    void visit(const ElemMatchValueMatchExpression* matchExpr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}
    void visit(const InternalEqHashedKey* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    void visitComparisonMatchExpression(const ComparisonMatchExpressionBase* expr) {
        auto slotId = getSlotId(expr->getInputParamId());
        if (!slotId) {
            return;
        }
        // This is an unowned value which is a view into the BSON owned by the MatchExpression. This
        // is acceptable because the 'MatchExpression' is held by the 'CanonicalQuery', and the
        // 'CanonicalQuery' lives for the lifetime of the query.
        auto&& [typeTag, val] = sbe::bson::convertFrom<true>(expr->getData());
        bindParam(*slotId, false /*owned*/, typeTag, val);
    }

    void visitBitTestExpression(const BitTestMatchExpression* expr) {
        auto bitPositionsParam = expr->getBitPositionsParamId();
        auto bitMaskParam = expr->getBitMaskParamId();
        if (!bitPositionsParam) {
            tassert(6279501,
                    "bit-test expression had bitmask param but not bit positions param",
                    !bitMaskParam);
            return;
        }
        tassert(6279502,
                "bit-test expression had bit positions param but not bitmask param",
                bitMaskParam);

        if (auto slotId = getSlotId(*bitPositionsParam)) {
            auto&& [bitPosTag, bitPosVal] = stage_builder::convertBitTestBitPositions(expr);
            bindParam(*slotId, true /*owned*/, bitPosTag, bitPosVal);
        }

        if (auto slotId = getSlotId(*bitMaskParam)) {
            auto val = sbe::value::bitcastFrom<uint64_t>(expr->getBitMask());
            bindParam(*slotId, true /*owned*/, sbe::value::TypeTags::NumberInt64, val);
        }
    }

    void bindParam(sbe::value::SlotId slotId,
                   bool owned,
                   sbe::value::TypeTags typeTag,
                   sbe::value::Value value) {
        boost::optional<sbe::value::ValueGuard> guard;
        if (owned) {
            guard.emplace(typeTag, value);
        }
        auto accessor = _data.env->getAccessor(slotId);
        if (owned) {
            guard->reset();
        }
        accessor->reset(owned, typeTag, value);
    }

    boost::optional<sbe::value::SlotId> getSlotId(
        boost::optional<MatchExpression::InputParamId> paramId) const {
        return paramId ? getSlotId(*paramId) : boost::none;
    }

    boost::optional<sbe::value::SlotId> getSlotId(MatchExpression::InputParamId paramId) const {
        auto it = _data.staticData->inputParamToSlotMap.find(paramId);
        if (it != _data.staticData->inputParamToSlotMap.end()) {
            return it->second;
        }
        return boost::none;
    }

    InListData* prepareInList(stage_builder::PlanStageData& data,
                              const std::shared_ptr<InListData>& inList) const {
        InListData* l = inList.get();
        if (!l->isPrepared()) {
            l->prepare();
        }

        if (data.inListsSet.insert(l).second) {
            data.inLists.emplace_back(inList);
        }

        return l;
    }

    stage_builder::PlanStageData& _data;
    // True if the plan for which we are binding parameter values is being recovered from the SBE
    // plan cache.
    const bool _bindingCachedPlan;
};

class MatchExpressionParameterBindingWalker {
public:
    MatchExpressionParameterBindingWalker(MatchExpressionParameterBindingVisitor* visitor)
        : _visitor{visitor} {
        invariant(_visitor);
    }

    void preVisit(const MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(const MatchExpression* expr) {}
    void inVisit(long count, const MatchExpression* expr) {}

private:
    MatchExpressionParameterBindingVisitor* const _visitor;
};

/**
 * Evaluates IndexBounds from the given IntervalEvaluationTrees for the given query.
 * 'indexBoundsInfo' contains the interval evaluation trees.
 *
 * Returns the built index bounds.
 */
std::unique_ptr<IndexBounds> makeIndexBounds(
    const stage_builder::IndexBoundsEvaluationInfo& indexBoundsInfo,
    const CanonicalQuery& cq,
    interval_evaluation_tree::IndexBoundsEvaluationCache* indexBoundsEvaluationCache) {
    auto bounds = std::make_unique<IndexBounds>();
    bounds->fields.reserve(indexBoundsInfo.iets.size());

    tassert(6335200,
            "IET list size must be equal to the number of fields in the key pattern",
            static_cast<size_t>(indexBoundsInfo.index.keyPattern.nFields()) ==
                indexBoundsInfo.iets.size());

    BSONObjIterator it{indexBoundsInfo.index.keyPattern};
    BSONElement keyElt = it.next();
    for (auto&& iet : indexBoundsInfo.iets) {
        auto oil =
            interval_evaluation_tree::evaluateIntervals(iet,
                                                        cq.getInputParamIdToMatchExpressionMap(),
                                                        keyElt,
                                                        indexBoundsInfo.index,
                                                        indexBoundsEvaluationCache);
        bounds->fields.emplace_back(std::move(oil));
        keyElt = it.next();
    }

    IndexBoundsBuilder::alignBounds(bounds.get(),
                                    indexBoundsInfo.index.keyPattern,
                                    indexBoundsInfo.index.collator != nullptr,
                                    indexBoundsInfo.direction);
    return bounds;
}

void bindSingleIntervalPlanSlots(const stage_builder::IndexBoundsEvaluationInfo& indexBoundsInfo,
                                 stage_builder::IndexIntervals intervals,
                                 sbe::RuntimeEnvironment* runtimeEnvironment) {
    // If there are no intervals, it means that the solution will be empty and will return EOF
    // without executing the index scan.
    if (intervals.empty()) {
        return;
    }

    tassert(6584700, "Can only bind a single index interval", intervals.size() == 1);
    auto&& [lowKey, highKey] = intervals[0];
    const auto singleInterval =
        get<mongo::stage_builder::ParameterizedIndexScanSlots::SingleIntervalPlan>(
            indexBoundsInfo.slots.slots);
    runtimeEnvironment->resetSlot(singleInterval.lowKey,
                                  sbe::value::TypeTags::ksValue,
                                  sbe::value::bitcastFrom<key_string::Value*>(lowKey.release()),
                                  /* owned */ true);

    runtimeEnvironment->resetSlot(singleInterval.highKey,
                                  sbe::value::TypeTags::ksValue,
                                  sbe::value::bitcastFrom<key_string::Value*>(highKey.release()),
                                  /* owned */ true);
}

void bindGenericPlanSlots(const stage_builder::IndexBoundsEvaluationInfo& indexBoundsInfo,
                          stage_builder::IndexIntervals intervals,
                          std::unique_ptr<IndexBounds> bounds,
                          sbe::RuntimeEnvironment* runtimeEnvironment) {
    const auto indexSlots = get<mongo::stage_builder::ParameterizedIndexScanSlots::GenericPlan>(
        indexBoundsInfo.slots.slots);
    const bool isGenericScan = intervals.empty();
    runtimeEnvironment->resetSlot(indexSlots.isGenericScan,
                                  sbe::value::TypeTags::Boolean,
                                  sbe::value::bitcastFrom<bool>(isGenericScan),
                                  /*owned*/ true);
    if (isGenericScan) {
        runtimeEnvironment->resetSlot(indexSlots.indexBounds,
                                      sbe::value::TypeTags::indexBounds,
                                      sbe::value::bitcastFrom<IndexBounds*>(bounds.release()),
                                      /*owned*/ true);
    } else {
        auto [boundsTag, boundsVal] =
            stage_builder::packIndexIntervalsInSbeArray(std::move(intervals));
        runtimeEnvironment->resetSlot(indexSlots.lowHighKeyIntervals,
                                      boundsTag,
                                      boundsVal,
                                      /*owned*/ true);
    }
}

/**
 * This mutable MatchExpression visitor will update the JS function predicate in each $where
 * expression by recovering it from the SBE runtime environment. The predicate was previously
 * was put there during the input parameter binding in process, after it was extracted from the
 * $where expression as an optimization.
 */
class WhereMatchExpressionVisitor final : public SelectiveMatchExpressionVisitorBase<false> {
public:
    explicit WhereMatchExpressionVisitor(stage_builder::PlanStageData& data) : _data(data) {}

    // To avoid overloaded-virtual warnings.
    using SelectiveMatchExpressionVisitorBase<false>::visit;

    void visit(WhereMatchExpression* expr) final {
        auto paramId = expr->getInputParamId();
        if (!paramId) {
            return;
        }

        auto it = _data.staticData->inputParamToSlotMap.find(*paramId);
        if (it == _data.staticData->inputParamToSlotMap.end()) {
            return;
        }

        auto accessor = _data.env->getAccessor(it->second);
        auto [type, value] = accessor->copyOrMoveValue();
        const auto valueType = type;  // a workaround for a compiler bug
        tassert(8415201,
                str::stream() << "Unexpected value type: " << valueType,
                type == sbe::value::TypeTags::jsFunction);
        expr->setPredicate(std::unique_ptr<JsFunction>(sbe::value::bitcastTo<JsFunction*>(value)));
    }

private:
    stage_builder::PlanStageData& _data;
};

/**
 * A match expression tree walker to visit the tree nodes with the 'WhereMatchExpressionVisitor'.
 */
class WhereMatchExpressionWalker {
public:
    explicit WhereMatchExpressionWalker(WhereMatchExpressionVisitor* visitor) : _visitor{visitor} {
        invariant(_visitor);
    }

    void preVisit(MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(MatchExpression* expr) {}
    void inVisit(long count, MatchExpression* expr) {}

private:
    WhereMatchExpressionVisitor* _visitor;
};
}  // namespace

void bind(const MatchExpression* matchExpr,
          stage_builder::PlanStageData& data,
          const bool bindingCachedPlan) {
    MatchExpressionParameterBindingVisitor visitor{data, bindingCachedPlan};
    MatchExpressionParameterBindingWalker walker{&visitor};
    tree_walker::walk<true, MatchExpression>(matchExpr, &walker);
}

void bindIndexBounds(
    const CanonicalQuery& cq,
    const stage_builder::IndexBoundsEvaluationInfo& indexBoundsInfo,
    sbe::RuntimeEnvironment* runtimeEnvironment,
    interval_evaluation_tree::IndexBoundsEvaluationCache* indexBoundsEvaluationCache) {
    std::unique_ptr<IndexBounds> bounds =
        makeIndexBounds(indexBoundsInfo, cq, indexBoundsEvaluationCache);
    stage_builder::IndexIntervals intervals =
        stage_builder::makeIntervalsFromIndexBounds(*bounds,
                                                    indexBoundsInfo.direction == 1,
                                                    indexBoundsInfo.keyStringVersion,
                                                    indexBoundsInfo.ordering);
    const bool isSingleIntervalSolution =
        holds_alternative<mongo::stage_builder::ParameterizedIndexScanSlots::SingleIntervalPlan>(
            indexBoundsInfo.slots.slots);
    if (isSingleIntervalSolution) {
        bindSingleIntervalPlanSlots(indexBoundsInfo, std::move(intervals), runtimeEnvironment);
    } else {
        bindGenericPlanSlots(
            indexBoundsInfo, std::move(intervals), std::move(bounds), runtimeEnvironment);
    }
}

void bindClusteredCollectionBounds(const CanonicalQuery& cq,
                                   const sbe::PlanStage* root,
                                   const stage_builder::PlanStageData* data,
                                   sbe::RuntimeEnvironment* runtimeEnvironment) {
    // Arguments needed to mimic the original build-time bounds setting from the current query.
    auto clusteredBoundInfos = data->staticData->clusteredCollBoundsInfos;
    const MatchExpression* conjunct = cq.getPrimaryMatchExpression();  // this is csn->filter
    bool minAndMaxEmpty = cq.getFindCommandRequest().getMin().isEmpty() &&
        cq.getFindCommandRequest().getMax().isEmpty();

    // Caching OR queries with collection scans is restricted, since it is challenging to determine
    // which match expressions from the input query require a clustered collection scan. Therefore,
    // we cannot correctly calculate the correct bounds for the query using the cached plan.
    tassert(6125900,
            "OR queries with clustered collection scans are not supported by the SBE cache.",
            cq.getPrimaryMatchExpression()->matchType() != MatchExpression::OR || !minAndMaxEmpty);

    tassert(7228000,
            "We only expect to cache plans with one clustered collection scan.",
            1 == clusteredBoundInfos.size());

    const CollatorInterface* queryCollator = cq.getCollator();  // current query's desired collator

    for (const auto& clusteredBoundInfo : clusteredBoundInfos) {
        // The outputs produced by the QueryPlannerAccess APIs below (passed by reference).
        // Scan start/end bounds.
        RecordIdRange recordRange;

        // Cast the return value to void since we are not building a CollectionScanNode here so do
        // not need to set it in its 'hasCompatibleCollation' member.
        static_cast<void>(
            QueryPlannerAccess::handleRIDRangeScan(conjunct,
                                                   queryCollator,
                                                   data->staticData->ccCollator.get(),
                                                   data->staticData->clusterKeyFieldName,
                                                   recordRange));
        QueryPlannerAccess::handleRIDRangeMinMax(cq,
                                                 data->staticData->direction,
                                                 queryCollator,
                                                 data->staticData->ccCollator.get(),
                                                 recordRange);
        // Bind the scan bounds to input slots.
        const auto& minRecord = recordRange.getMin();
        if (minRecord) {
            boost::optional<sbe::value::SlotId> minRecordId = clusteredBoundInfo.minRecord;
            tassert(7571500, "minRecordId slot missing", minRecordId.has_value());
            auto [tag, val] = sbe::value::makeCopyRecordId(minRecord->recordId());
            runtimeEnvironment->resetSlot(minRecordId.value(), tag, val, true);
        }
        const auto& maxRecord = recordRange.getMax();
        if (maxRecord) {
            boost::optional<sbe::value::SlotId> maxRecordId = clusteredBoundInfo.maxRecord;
            tassert(7571501, "maxRecordId slot missing", maxRecordId.has_value());
            auto [tag, val] = sbe::value::makeCopyRecordId(maxRecord->recordId());
            runtimeEnvironment->resetSlot(maxRecordId.value(), tag, val, true);
        }
    }
}  // bindClusteredCollectionBounds

void bindLimitSkipInputSlots(const CanonicalQuery& cq,
                             const stage_builder::PlanStageData* data,
                             sbe::RuntimeEnvironment* runtimeEnvironment) {
    auto setLimitSkipInputSlot = [&](boost::optional<sbe::value::SlotId> slot,
                                     boost::optional<int64_t> amount) {
        if (slot) {
            tassert(8349202, "Slot is present, but amount is not present", amount);
            runtimeEnvironment->resetSlot(*slot,
                                          sbe::value::TypeTags::NumberInt64,
                                          sbe::value::bitcastFrom<int64_t>(*amount),
                                          false);
        }
    };

    setLimitSkipInputSlot(data->staticData->limitSkipSlots.limit,
                          cq.getFindCommandRequest().getLimit());
    setLimitSkipInputSlot(data->staticData->limitSkipSlots.skip,
                          cq.getFindCommandRequest().getSkip());
}

void recoverWhereExprPredicate(MatchExpression* filter, stage_builder::PlanStageData& data) {
    WhereMatchExpressionVisitor visitor{data};
    WhereMatchExpressionWalker walker{&visitor};
    tree_walker::walk<false, MatchExpression>(filter, &walker);
}

}  // namespace mongo::input_params
