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

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"

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

        auto&& [arrSetTag, arrSetVal, hasArray, hasObject, hasNull] =
            stage_builder::convertInExpressionEqualities(expr, _data);
        bindParam(*slotId, true /*owned*/, arrSetTag, arrSetVal);

        // Auto-parameterization should not kick in if the $in's list of equalities includes any
        // arrays, objects or null values. Asserted after bind to avoid leaking memory allocated in
        // 'stage_builder::convertInExpressionEqualities()'.
        tassert(6988502, "Should not auto-parameterize $in with an array value", !hasArray);
        tassert(6988503, "Should not auto-parameterize $in with a null value", !hasNull);
        tassert(6988504, "Should not auto-parameterize $in with an object value", !hasObject);
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
                sbe::value::makeNewPcreRegex(expr->getString(), expr->getFlags());
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

        // The bitmask representing the set of types is a 32-bit unsigned integer. In order to avoid
        // converting a 32-bit unsigned number that is larger than INT_MAX to a 32-bit signed
        // number, we use NumberInt64 rather than NumberInt32 as the destination SBE type.
        auto value = sbe::value::bitcastFrom<int64_t>(expr->typeSet().getBSONTypeMask());
        tassert(
            6279506, "type mask cannot be negative", sbe::value::bitcastTo<int64_t>(value) >= 0);
        bindParam(*slotId, true /*owned*/, sbe::value::TypeTags::NumberInt64, value);
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
            // access to this match expression tree. Furthermore, after all input parameters are
            // bound the match expression tree is no longer used.
            bindParam(*slotId,
                      true /*owned*/,
                      sbe::value::TypeTags::jsFunction,
                      sbe::value::bitcastFrom<JsFunction*>(
                          const_cast<WhereMatchExpression*>(expr)->extractPredicate().release()));
        } else {
            auto [typeTag, value] = sbe::value::makeCopyJsFunction(expr->getPredicate());
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
        auto it = _data.inputParamToSlotMap.find(paramId);
        if (it != _data.inputParamToSlotMap.end()) {
            return it->second;
        }
        return boost::none;
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
        stdx::get<mongo::stage_builder::ParameterizedIndexScanSlots::SingleIntervalPlan>(
            indexBoundsInfo.slots.slots);
    runtimeEnvironment->resetSlot(singleInterval.lowKey,
                                  sbe::value::TypeTags::ksValue,
                                  sbe::value::bitcastFrom<KeyString::Value*>(lowKey.release()),
                                  /* owned */ true);

    runtimeEnvironment->resetSlot(singleInterval.highKey,
                                  sbe::value::TypeTags::ksValue,
                                  sbe::value::bitcastFrom<KeyString::Value*>(highKey.release()),
                                  /* owned */ true);
}

void bindGenericPlanSlots(const stage_builder::IndexBoundsEvaluationInfo& indexBoundsInfo,
                          stage_builder::IndexIntervals intervals,
                          std::unique_ptr<IndexBounds> bounds,
                          sbe::RuntimeEnvironment* runtimeEnvironment) {
    const auto indexSlots =
        stdx::get<mongo::stage_builder::ParameterizedIndexScanSlots::GenericPlan>(
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
}  // namespace

void bind(const CanonicalQuery& canonicalQuery,
          stage_builder::PlanStageData& data,
          const bool bindingCachedPlan) {
    MatchExpressionParameterBindingVisitor visitor{data, bindingCachedPlan};
    MatchExpressionParameterBindingWalker walker{&visitor};
    tree_walker::walk<true, MatchExpression>(canonicalQuery.root(), &walker);
}

void bindIndexBounds(
    const CanonicalQuery& cq,
    const stage_builder::IndexBoundsEvaluationInfo& indexBoundsInfo,
    sbe::RuntimeEnvironment* runtimeEnvironment,
    interval_evaluation_tree::IndexBoundsEvaluationCache* indexBoundsEvaluationCache) {
    auto bounds = makeIndexBounds(indexBoundsInfo, cq, indexBoundsEvaluationCache);
    auto intervals = stage_builder::makeIntervalsFromIndexBounds(*bounds,
                                                                 indexBoundsInfo.direction == 1,
                                                                 indexBoundsInfo.keyStringVersion,
                                                                 indexBoundsInfo.ordering);
    const bool isSingleIntervalSolution = stdx::holds_alternative<
        mongo::stage_builder::ParameterizedIndexScanSlots::SingleIntervalPlan>(
        indexBoundsInfo.slots.slots);
    if (isSingleIntervalSolution) {
        bindSingleIntervalPlanSlots(indexBoundsInfo, std::move(intervals), runtimeEnvironment);
    } else {
        bindGenericPlanSlots(
            indexBoundsInfo, std::move(intervals), std::move(bounds), runtimeEnvironment);
    }
}
}  // namespace mongo::input_params
