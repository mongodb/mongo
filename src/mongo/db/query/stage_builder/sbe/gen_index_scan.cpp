/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/query/stage_builder/sbe/gen_index_scan.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/interval_evaluation_tree.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_filter.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <deque>
#include <iterator>
#include <map>

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {
namespace {
/**
 * Returns 'true' if the index bounds in 'intervalLists' can be represented as a number of intervals
 * between low and high keys, which can be statically generated. Inclusivity of each bound is
 * returned through the relevant '*KeyInclusive' parameter. Returns 'false' otherwise.
 */
bool canBeDecomposedIntoSingleIntervals(const std::vector<OrderedIntervalList>& intervalLists,
                                        bool* lowKeyInclusive,
                                        bool* highKeyInclusive) {
    invariant(lowKeyInclusive);
    invariant(highKeyInclusive);

    *lowKeyInclusive = true;
    *highKeyInclusive = true;

    size_t listNum = 0;

    // First, we skip over point intervals.
    for (; listNum < intervalLists.size(); ++listNum) {
        if (!std::all_of(std::begin(intervalLists[listNum].intervals),
                         std::end(intervalLists[listNum].intervals),
                         [](auto&& interval) { return interval.isPoint(); })) {
            break;
        }
    }

    // Bail out early if all our intervals are points.
    if (listNum == intervalLists.size()) {
        return true;
    }

    // After point intervals we can have exactly one non-point interval.
    if (intervalLists[listNum].intervals.size() != 1) {
        return false;
    }

    // Set the inclusivity from the non-point interval.
    *lowKeyInclusive = intervalLists[listNum].intervals[0].startInclusive;
    *highKeyInclusive = intervalLists[listNum].intervals[0].endInclusive;

    // And after the non-point interval we can have any number of "all values" intervals.
    for (++listNum; listNum < intervalLists.size(); ++listNum) {
        if (!(intervalLists[listNum].intervals.size() == 1 &&
              (intervalLists[listNum].intervals[0].isMinToMax() ||
               intervalLists[listNum].intervals[0].isMaxToMin()))) {
            break;
        }
    }

    // If we've reached the end of the interval lists, then we can decompose a multi-interval index
    // bounds into a number of single-interval bounds.
    return listNum == intervalLists.size();
}

/**
 * Decomposes multi-interval index bounds represented as 'intervalLists' into a number of
 * single-interval bounds. Inclusivity of each bound is set through the relevant '*KeyInclusive'
 * parameter. For example, if we've got an index {a: 1, b: 1, c: 1, d: 1} and would issue this
 * query:
 *
 *   {a: {$in: [1,2]}, b: {$in: [10,11]}, c: {$gte: 20}}
 *
 * Then the 'intervalLists' would contain the following multi-interval bounds:
 *
 *   [
 *     [ [1,1], [2,2] ],
 *     [ [10,10], [11,11] ],
 *     [ [20, Inf) ],
 *     [ [MinKey, MaxKey]
 *   ]
 *
 * And it'd be decomposed into the following single-intervals between low and high keys:
 *
 *  {'':1, '':10, '':20, '':MinKey} -> {'':1, '':10, '':Inf, '':MaxKey}
 *  {'':1, '':11, '':20, '':MinKey} -> {'':1, '':11, '':Inf, '':MaxKey}
 *  {'':2, '':10, '':20, '':MinKey} -> {'':2, '':10, '':Inf, '':MaxKey}
 *  {'':2, '':11, '':20, '':MinKey} -> {'':2, '':11, '':Inf, '':MaxKey}
 *
 * TODO SERVER-48485: optimize this function to build and return the intervals as KeyString objects,
 * rather than BSON.
 */
std::vector<std::pair<BSONObj, BSONObj>> decomposeIntoSingleIntervals(
    const std::vector<OrderedIntervalList>& intervalLists,
    bool lowKeyInclusive,
    bool highKeyInclusive) {
    invariant(intervalLists.size() > 0);

    // Appends the 'interval' bounds to the low and high keys and return the updated keys.
    // Inclusivity of each bound is set through the relevant '*KeyInclusive' parameter.
    auto appendInterval = [lowKeyInclusive, highKeyInclusive](const BSONObj& lowKey,
                                                              const BSONObj& highKey,
                                                              const Interval& interval) {
        BSONObjBuilder lowKeyBob{lowKey};
        BSONObjBuilder highKeyBob{highKey};

        if (interval.isMinToMax() || interval.isMaxToMin()) {
            IndexBoundsBuilder::appendTrailingAllValuesInterval(
                interval, lowKeyInclusive, highKeyInclusive, &lowKeyBob, &highKeyBob);
        } else {
            lowKeyBob.append(interval.start);
            highKeyBob.append(interval.end);
        }

        return std::pair(lowKeyBob.obj(), highKeyBob.obj());
    };

    size_t maxStaticIndexScanIntervals =
        internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals.load();
    std::deque<std::pair<BSONObj, BSONObj>> keysQueue{{}};

    // This is an adaptation of the BFS algorithm. The 'keysQueue' is initialized with a pair of
    // empty low/high keys. For each step while traversing the 'intervalLists' we try to append the
    // current interval to each generated pair in 'keysQueue' and then push the updated keys back to
    // the queue.
    for (auto&& list : intervalLists) {
        auto size = keysQueue.size();
        for (size_t ix = 0; ix < size; ++ix) {
            auto [lowKey, highKey] = keysQueue.front();
            keysQueue.pop_front();

            for (auto&& interval : list.intervals) {
                keysQueue.push_back(appendInterval(lowKey, highKey, interval));

                // If the limit of maximum number of static intervals is exceeded, return an empty
                // vector which will cause a fallback to build a generic index scan.
                if (keysQueue.size() > maxStaticIndexScanIntervals) {
                    return {};
                }
            }
        }
    }

    // The 'keysQueue' contains all generated pairs of low/high keys.
    return {keysQueue.begin(), keysQueue.end()};
}

SbIndexInfoType getIndexInfoTypeMask(const PlanStageReqs& reqs) {
    auto indexInfoTypeMask = SbIndexInfoType::kNoInfo;

    if (reqs.has(PlanStageSlots::kIndexIdent)) {
        indexInfoTypeMask = indexInfoTypeMask | SbIndexInfoType::kIndexIdent;
    }
    if (reqs.has(PlanStageSlots::kIndexKey)) {
        indexInfoTypeMask = indexInfoTypeMask | SbIndexInfoType::kIndexKey;
    }
    if (reqs.has(PlanStageSlots::kIndexKeyPattern)) {
        indexInfoTypeMask = indexInfoTypeMask | SbIndexInfoType::kIndexKeyPattern;
    }
    if (reqs.has(PlanStageSlots::kSnapshotId)) {
        indexInfoTypeMask = indexInfoTypeMask | SbIndexInfoType::kSnapshotId;
    }

    return indexInfoTypeMask;
}

PlanStageSlots buildPlanStageSlots(StageBuilderState& state,
                                   const PlanStageReqs& reqs,
                                   const std::string& indexName,
                                   SbSlot recordIdSlot,
                                   const SbIndexInfoSlots& indexInfoSlots) {
    PlanStageSlots outputs;

    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);

    if (reqs.has(PlanStageSlots::kIndexIdent)) {
        const auto& slot = indexInfoSlots.indexIdentSlot;
        tassert(7566702, "Expected 'indexIdentSlot' to be set", slot.has_value());
        outputs.set(PlanStageSlots::kIndexIdent, *slot);
    }
    if (reqs.has(PlanStageSlots::kIndexKey)) {
        const auto& slot = indexInfoSlots.indexKeySlot;
        tassert(7104001, "Expected 'indexKeySlot' to be set", slot.has_value());
        outputs.set(PlanStageSlots::kIndexKey, *slot);
    }
    if (reqs.has(PlanStageSlots::kIndexKeyPattern)) {
        const auto& slot = indexInfoSlots.indexKeyPatternSlot;
        tassert(9405100, "Expected 'indexKeyPatternSlot' to be set", slot.has_value());
        outputs.set(PlanStageSlots::kIndexKeyPattern, *slot);
    }
    if (reqs.has(PlanStageSlots::kSnapshotId)) {
        const auto& slot = indexInfoSlots.snapshotIdSlot;
        tassert(7104000, "Expected 'snapshotIdSlot' to be set", slot.has_value());
        outputs.set(PlanStageSlots::kSnapshotId, *slot);
    }
    if (reqs.has(PlanStageSlots::kPrefetchedResult)) {
        outputs.set(PlanStageSlots::kPrefetchedResult, SbSlot{state.getNothingSlot()});
    }

    return outputs;
}

std::pair<sbe::IndexKeysInclusionSet, sbe::IndexKeysInclusionSet> computeBitsetsForIndexScan(
    const PlanStageReqs& reqs, const BSONObj& keyPattern) {
    sbe::IndexKeysInclusionSet fieldBitset;
    sbe::IndexKeysInclusionSet sortKeyBitset;

    if (reqs.hasSortKeys()) {
        size_t i = 0;
        for (const auto& elt : keyPattern) {
            if (reqs.has(std::pair(PlanStageSlots::kSortKey, elt.fieldNameStringData()))) {
                sortKeyBitset.set(i);
            }
            ++i;
        }
    }

    size_t i = 0;
    for (const auto& elt : keyPattern) {
        if (reqs.has(std::pair(PlanStageSlots::kField, elt.fieldNameStringData()))) {
            fieldBitset.set(i);
        }
        ++i;
    }

    return {fieldBitset, sortKeyBitset};
}

PlanStageSlots setFieldAndSortKeySlots(PlanStageSlots outputs,
                                       const BSONObj& keyPattern,
                                       sbe::IndexKeysInclusionSet fieldBitset,
                                       sbe::IndexKeysInclusionSet sortKeyBitset,
                                       const SbSlotVector& indexKeySlots) {
    size_t i = 0;
    size_t slotIdx = 0;
    for (const auto& elt : keyPattern) {
        bool isFieldOrSortKey = false;

        if (fieldBitset.test(i)) {
            auto name = std::pair(PlanStageSlots::kField, elt.fieldNameStringData());
            outputs.set(name, indexKeySlots[slotIdx]);
            isFieldOrSortKey = true;
        }
        if (sortKeyBitset.test(i)) {
            auto name = std::pair(PlanStageSlots::kSortKey, elt.fieldNameStringData());
            outputs.set(name, indexKeySlots[slotIdx]);
            isFieldOrSortKey = true;
        }

        if (isFieldOrSortKey) {
            ++slotIdx;
        }
        ++i;
    }

    return outputs;
}

/**
 * Constructs an optimized version of an index scan for multi-interval index bounds for the case
 * when the bounds can be decomposed in a number of single-interval bounds. In this case, instead
 * of building a generic index scan to navigate through the index using the 'IndexBoundsChecker',
 * we will construct a subtree with a constant table scan containing all intervals we'd want to
 * scan through. Specifically, we will build the following subtree:
 *
 *   env: { .., boundsSlot = .., keyStringSlot = .., snapshotIdSlot = .., .. }
 *
 *   nlj [] [lowKeySlot, highKeySlot]
 *     left
 *       project [lowKeySlot = getField(unwindSlot, "l"), highKeySlot = getField(unwindSlot, "h")]
 *       unwind unwindSlot unusedIndexSlot boundsSlot false
 *       limit 1
 *       coscan
 *     right
 *       ixseek lowKeySlot highKeySlot keyStringSlot snapshotIdSlot recordIdSlot [] @coll @index
 *
 * In case when the 'intervals' are not specified, 'boundsSlot' will be registered in the runtime
 * environment and returned as a third element of the tuple.
 */
std::tuple<SbStage, PlanStageSlots, boost::optional<SbSlot>>
generateOptimizedMultiIntervalIndexScan(StageBuilderState& state,
                                        const CollectionPtr& collection,
                                        const std::string& indexName,
                                        const BSONObj& keyPattern,
                                        bool forward,
                                        boost::optional<IndexIntervals> intervals,
                                        const PlanStageReqs& reqs,
                                        PlanNodeId nodeId) {
    using namespace std::literals;

    SbBuilder b(state, nodeId);

    auto boundsSlot = [&] {
        if (intervals) {
            auto [boundsTag, boundsVal] = packIndexIntervalsInSbeArray(std::move(*intervals));
            return SbSlot{
                state.env->registerSlot(boundsTag, boundsVal, true, state.slotIdGenerator)};
        } else {
            return SbSlot{state.env->registerSlot(
                sbe::value::TypeTags::Nothing, 0, true, state.slotIdGenerator)};
        }
    }();

    // Project out the constructed array as a constant value if intervals are known at compile time
    // and add an unwind stage on top to flatten the interval bounds array.
    constexpr bool preserveNullAndEmptyArrays = false;
    auto [unwind, unwindSlot, _] =
        b.makeUnwind(b.makeLimitOneCoScanTree(), boundsSlot, preserveNullAndEmptyArrays);

    // Add another project stage to extract low and high keys from each value produced by unwind and
    // bind the keys to the 'lowKeySlot' and 'highKeySlot'.
    auto [project, outSlots] =
        b.makeProject(std::move(unwind),
                      b.makeFunction("getField"_sd, unwindSlot, b.makeStrConstant("l"_sd)),
                      b.makeFunction("getField"_sd, unwindSlot, b.makeStrConstant("h"_sd)));

    auto lowKeySlot = outSlots[0];
    auto highKeySlot = outSlots[1];

    SbIndexInfoType indexInfoTypeMask = getIndexInfoTypeMask(reqs);

    auto [fieldBitset, sortKeyBitset] = computeBitsetsForIndexScan(reqs, keyPattern);
    auto indexKeysToInclude = fieldBitset | sortKeyBitset;

    auto [stage, recordIdSlot, indexKeySlots, indexInfoSlots] =
        b.makeSimpleIndexScan(collection->uuid(),
                              collection->ns().dbName(),
                              indexName,
                              keyPattern,
                              forward,
                              lowKeySlot,
                              highKeySlot,
                              indexKeysToInclude,
                              indexInfoTypeMask);

    auto outputs = buildPlanStageSlots(state, reqs, indexName, recordIdSlot, indexInfoSlots);

    outputs = setFieldAndSortKeySlots(
        std::move(outputs), keyPattern, fieldBitset, sortKeyBitset, indexKeySlots);

    // Finally, get the keys from the outer side and feed them to the inner side (ixscan).
    return {b.makeLoopJoin(std::move(project),
                           std::move(stage),
                           SbSlotVector{},
                           SbExpr::makeSV(lowKeySlot, highKeySlot)),
            std::move(outputs),
            boost::make_optional(!intervals, boundsSlot)};
}

/**
 * Builds a generic multi-interval index scan for the cases when index bounds cannot be represented
 * as valid low/high keys. A 'GenericIndexScanStage' plan will be generated, and it will use either
 * a constant IndexBounds* or a parameterized IndexBounds* from a runtime environment slot.
 * The parameterized IndexBounds* obtained from environment slot can be rebound to a new value upon
 * plan cache recovery.
 *
 * Returns a tuple composed of: (1) a 'GenericIndexScanStage' plan stage; (2) a set of output slots;
 * and (3) boost::none or a runtime environment slot id for index bounds. In case when the 'bounds'
 * are not specified, 'indexBounds' will be registered in the runtime environment and returned in
 * the third element of the tuple.
 */
std::tuple<SbStage, PlanStageSlots, boost::optional<SbSlot>> generateGenericMultiIntervalIndexScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const std::string& indexName,
    const IndexScanNode* ixn,
    const BSONObj& keyPattern,
    key_string::Version version,
    Ordering ordering,
    const PlanStageReqs& reqs) {
    SbBuilder b(state, ixn->nodeId());

    const bool forward = ixn->direction == 1;
    const bool hasDynamicIndexBounds = !ixn->iets.empty();

    boost::optional<SbSlot> boundsSlot;
    SbExpr boundsExpr;

    if (hasDynamicIndexBounds) {
        boundsSlot = SbSlot{state.env->registerSlot(
            sbe::value::TypeTags::Nothing, 0, true /* owned */, state.slotIdGenerator)};
        boundsExpr = boundsSlot;
    } else {
        // 'b.makeConstant()' will take the ownership of the 'IndexBounds' pointer.
        boundsExpr =
            b.makeConstant(sbe::value::TypeTags::indexBounds,
                           sbe::value::bitcastFrom<IndexBounds*>(new IndexBounds(ixn->bounds)));
    }

    SbIndexInfoType indexInfoTypeMask = getIndexInfoTypeMask(reqs);

    auto [fieldBitset, sortKeyBitset] = computeBitsetsForIndexScan(reqs, keyPattern);
    auto indexKeysToInclude = fieldBitset | sortKeyBitset;

    auto [stage, recordIdSlot, indexKeySlots, indexInfoSlots] =
        b.makeGenericIndexScan(collection->uuid(),
                               collection->ns().dbName(),
                               indexName,
                               keyPattern,
                               forward,
                               std::move(boundsExpr),
                               version,
                               ordering,
                               indexKeysToInclude,
                               indexInfoTypeMask);

    auto outputs = buildPlanStageSlots(state, reqs, indexName, recordIdSlot, indexInfoSlots);

    outputs = setFieldAndSortKeySlots(
        std::move(outputs), keyPattern, fieldBitset, sortKeyBitset, indexKeySlots);

    return {std::move(stage), std::move(outputs), boundsSlot};
}

/**
 * Represents the kinds of interval(s) required for an index scan: a single point interval, a single
 * range interval, or multiple intervals.
 */
enum class IntervalsRequired { EqualityInterval, SingleRangeInterval, MultiInterval };

/**
 * Checks what kind of intervals are required for the index scan plan, i.e., if we can create a
 * single interval index scan plan. Creation of the single interval index scan plans is preferred
 * due to lower query latency as a result of faster plan recovery from the cache. The rule for
 * checking if 'iets' resolve to a single interval is as follows:
 * - an optional sequence of '$eq' or constant point intervals followed by
 * - an optional single interval of a comparison match expression or a constant interval or an
 * intersection of two such nodes followed by
 * - an optional sequence of unbounded intervals [MinKey, MaxKey].
 */
IntervalsRequired canGenerateSingleIntervalIndexScan(
    const std::vector<interval_evaluation_tree::IET>& iets) {
    // Represents different allowed states while checking if the 'iets' could be represented as a
    // single interval.
    enum class State { EqOrConstPoint, ComparisonOrConstRange, UnboundedInterval };
    auto isComparisonOrSingleConst = [&](const interval_evaluation_tree::IET& iet) {
        const auto evalNodePtr = iet.cast<interval_evaluation_tree::EvalNode>();
        const auto constNodePtr = iet.cast<interval_evaluation_tree::ConstNode>();
        const bool isComparison = evalNodePtr &&
            ComparisonMatchExpression::isComparisonMatchExpression(evalNodePtr->matchType());
        const bool isConstSingleInterval = constNodePtr && constNodePtr->oil.intervals.size() == 1;
        return isComparison || isConstSingleInterval;
    };

    auto currentState{State::EqOrConstPoint};
    for (const auto& iet : iets) {
        const auto evalNodePtr = iet.cast<interval_evaluation_tree::EvalNode>();
        const auto constNodePtr = iet.cast<interval_evaluation_tree::ConstNode>();
        const auto intersectNodePtr = iet.cast<interval_evaluation_tree::IntersectNode>();
        const bool isEq = evalNodePtr && evalNodePtr->matchType() == MatchExpression::MatchType::EQ;
        const bool isConstSinglePoint = constNodePtr && constNodePtr->oil.isPoint();
        const bool isSimpleIntersection = intersectNodePtr &&
            isComparisonOrSingleConst(intersectNodePtr->get<0>()) &&
            isComparisonOrSingleConst(intersectNodePtr->get<1>());
        const bool isMinToMax = constNodePtr && constNodePtr->oil.isMinToMax();

        switch (currentState) {
            case State::EqOrConstPoint:
                if (isEq || isConstSinglePoint) {
                    continue;
                } else if (isComparisonOrSingleConst(iet) || isSimpleIntersection) {
                    currentState = State::ComparisonOrConstRange;
                } else {
                    return IntervalsRequired::MultiInterval;
                }
                break;
            case State::ComparisonOrConstRange:
                if (!isMinToMax) {
                    return IntervalsRequired::MultiInterval;
                }

                // Transition to the next state as we allow only one bounded range, after that all
                // remaining fields must be unbounded.
                currentState = State::UnboundedInterval;
                break;
            case State::UnboundedInterval:
                if (!isMinToMax) {
                    return IntervalsRequired::MultiInterval;
                }
                break;
        }
    }

    return currentState == State::EqOrConstPoint ? IntervalsRequired::EqualityInterval
                                                 : IntervalsRequired::SingleRangeInterval;
}
}  // namespace

bool ietsArePointInterval(const std::vector<interval_evaluation_tree::IET>& iets) {
    return canGenerateSingleIntervalIndexScan(iets) == IntervalsRequired::EqualityInterval;
}

std::pair<SbStage, PlanStageSlots> generateSingleIntervalIndexScanImpl(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const std::string& indexName,
    const BSONObj& keyPattern,
    SbExpr lowKeyExpr,
    SbExpr highKeyExpr,
    const PlanStageReqs& reqs,
    PlanNodeId nodeId,
    bool forward) {
    SbBuilder b(state, nodeId);

    tassert(7856101,
            "lowKeyExpr must be present if highKeyExpr is specified.",
            lowKeyExpr || (!lowKeyExpr && !highKeyExpr));

    SbIndexInfoType indexInfoTypeMask = getIndexInfoTypeMask(reqs);

    auto [fieldBitset, sortKeyBitset] = computeBitsetsForIndexScan(reqs, keyPattern);
    auto indexKeysToInclude = fieldBitset | sortKeyBitset;

    // Scan the index in the range {'lowKeySlot', 'highKeySlot'} (subject to inclusive or
    // exclusive boundaries), and produce a single field recordIdSlot that can be used to
    // position into the collection.
    auto [stage, recordIdSlot, indexKeySlots, indexInfoSlots] =
        b.makeSimpleIndexScan(collection->uuid(),
                              collection->ns().dbName(),
                              indexName,
                              keyPattern,
                              forward,
                              std::move(lowKeyExpr),
                              std::move(highKeyExpr),
                              indexKeysToInclude,
                              indexInfoTypeMask);

    auto outputs = buildPlanStageSlots(state, reqs, indexName, recordIdSlot, indexInfoSlots);

    outputs = setFieldAndSortKeySlots(
        std::move(outputs), keyPattern, fieldBitset, sortKeyBitset, indexKeySlots);

    return {std::move(stage), std::move(outputs)};
}

std::tuple<SbStage, PlanStageSlots, boost::optional<std::pair<SbSlot, SbSlot>>>
generateSingleIntervalIndexScanAndSlotsImpl(StageBuilderState& state,
                                            const CollectionPtr& collection,
                                            const std::string& indexName,
                                            const BSONObj& keyPattern,
                                            bool forward,
                                            std::unique_ptr<key_string::Value> lowKey,
                                            std::unique_ptr<key_string::Value> highKey,
                                            const PlanStageReqs& reqs,
                                            PlanNodeId nodeId,
                                            bool isPointInterval) {
    SbBuilder b(state, nodeId);

    auto slotIdGenerator = state.slotIdGenerator;
    tassert(6584701,
            "Either both lowKey and highKey are specified or none of them are",
            (lowKey && highKey) || (!lowKey && !highKey));
    const bool shouldRegisterLowHighKeyInRuntimeEnv = !lowKey;

    auto lowKeySlot = !lowKey ? boost::make_optional(SbSlot{state.env->registerSlot(
                                    sbe::value::TypeTags::Nothing, 0, true, slotIdGenerator)})
                              : boost::none;
    auto highKeySlot = !highKey ? boost::make_optional(SbSlot{state.env->registerSlot(
                                      sbe::value::TypeTags::Nothing, 0, true, slotIdGenerator)})
                                : boost::none;

    auto lowKeyExpr = !lowKey ? SbExpr{*lowKeySlot}
                              : b.makeConstant(sbe::value::TypeTags::keyString,
                                               sbe::value::makeKeyString(*lowKey).second);
    auto highKeyExpr = !highKey ? SbExpr{*highKeySlot}
                                : b.makeConstant(sbe::value::TypeTags::keyString,
                                                 sbe::value::makeKeyString(*highKey).second);

    auto [stage, outputs] = generateSingleIntervalIndexScanImpl(state,
                                                                collection,
                                                                indexName,
                                                                keyPattern,
                                                                lowKeyExpr.clone(),
                                                                highKeyExpr.clone(),
                                                                reqs,
                                                                nodeId,
                                                                forward);

    // If low and high keys are provided in the runtime environment, then we need to create
    // a cfilter stage on top of project in order to be sure that the single interval
    // exists (the interval may be empty), in which case the index scan plan should simply
    // return EOF. This does not apply when the interval is a point interval, since the interval
    // should always exist in that case.
    if (shouldRegisterLowHighKeyInRuntimeEnv && !isPointInterval) {
        stage =
            b.makeConstFilter(std::move(stage),
                              b.makeBooleanOpTree(abt::Operations::And,
                                                  b.makeFunction("exists", lowKeyExpr.clone()),
                                                  b.makeFunction("exists", highKeyExpr.clone())));
    }

    return {std::move(stage),
            std::move(outputs),
            shouldRegisterLowHighKeyInRuntimeEnv
                ? boost::make_optional(std::pair(*lowKeySlot, *highKeySlot))
                : boost::none};
}

PlanStageReqs computeReqsForIndexScan(const PlanStageReqs& reqs,
                                      const BSONObj& keyPattern,
                                      const MatchExpression* filter = nullptr) {
    PlanStageReqs ixScanReqs;
    ixScanReqs.set(PlanStageSlots::kRecordId)
        .setIf(PlanStageSlots::kSnapshotId, reqs.has(PlanStageSlots::kSnapshotId))
        .setIf(PlanStageSlots::kIndexIdent, reqs.has(PlanStageSlots::kIndexIdent))
        .setIf(PlanStageSlots::kIndexKey, reqs.has(PlanStageSlots::kIndexKey))
        .setIf(PlanStageSlots::kIndexKeyPattern, reqs.has(PlanStageSlots::kIndexKeyPattern))
        .setIf(PlanStageSlots::kPrefetchedResult, reqs.has(PlanStageSlots::kPrefetchedResult));
    ;

    StringDataSet keyPatternSet;
    for (const auto& elt : keyPattern) {
        keyPatternSet.emplace(elt.fieldNameStringData());
    }

    // Copy kSortKey reqs to 'ixScanReqs', and assert if any sort key isn't part of the key pattern.
    if (reqs.hasSortKeys()) {
        auto sortKeys = reqs.getSortKeys();
        for (auto&& key : sortKeys) {
            tassert(7097208,
                    str::stream() << "Expected sort key '" << key
                                  << "' to be part of index pattern",
                    keyPatternSet.count(key));

            ixScanReqs.set(std::pair(PlanStageSlots::kSortKey, key));
        }
    }

    bool reqAllKeyPatternParts = reqs.hasResult() || reqs.has(PlanStageSlots::kReturnKey);

    if (!reqAllKeyPatternParts) {
        // Determine which parts of the index key pattern are needed by the filter expression
        // and add these fields to 'ixScanReqs'.
        if (filter) {
            DepsTracker deps;
            dependency_analysis::addDependencies(filter, &deps);
            for (auto&& elt : keyPattern) {
                if (deps.fields.count(elt.fieldName())) {
                    StringData name = elt.fieldNameStringData();
                    ixScanReqs.set(std::pair(PlanStageSlots::kField, name));
                }
            }
        }

        // Visit each kField req and check if it's present in the key pattern.
        auto fields = reqs.getFields();
        for (auto&& field : fields) {
            if (keyPatternSet.count(field)) {
                // If 'field' is part of the key pattern, copy the kField req to 'ixScanReqs'.
                ixScanReqs.set(std::pair(PlanStageSlots::kField, field));
            } else {
                // If 'field' is not part of the key pattern, then we will need all parts of the
                // key pattern. Set 'reqAllKeyPatternParts' to true and break out of this loop.
                reqAllKeyPatternParts = true;
                break;
            }
        }
    }

    // If 'reqAllKeyPatternParts' is true, then we need to get all parts of the index key pattern.
    if (reqAllKeyPatternParts) {
        for (const auto& elt : keyPattern) {
            StringData name = elt.fieldNameStringData();
            ixScanReqs.set(std::pair(PlanStageSlots::kField, name));
        }
    }

    return ixScanReqs;
}

std::pair<SbStage, PlanStageSlots> setResultAndAdditionalFieldSlots(SbStage stage,
                                                                    PlanStageSlots outputs,
                                                                    const BSONObj& keyPattern,
                                                                    const PlanStageReqs& reqs,
                                                                    StageBuilderState& state,
                                                                    PlanNodeId nodeId) {
    SbBuilder b(state, nodeId);

    StringDataSet keyPatternSet;
    for (const auto& elt : keyPattern) {
        keyPatternSet.emplace(elt.fieldNameStringData());
    }

    std::vector<std::string> additionalFields =
        filterVector(reqs.getFields(), [&](auto&& f) { return !keyPatternSet.count(f); });

    if (reqs.hasResult() || !additionalFields.empty()) {
        SbSlotVector indexKeySlots;
        for (auto&& elem : keyPattern) {
            StringData name = elem.fieldNameStringData();
            indexKeySlots.emplace_back(outputs.get(std::pair(PlanStageSlots::kField, name)));
        }

        auto [outStage, outSlots] =
            b.makeProject(std::move(stage), rehydrateIndexKey(state, keyPattern, indexKeySlots));
        stage = std::move(outStage);

        auto resultSlot = outSlots[0];
        outputs.setResultObj(resultSlot);

        if (!additionalFields.empty()) {
            auto [outStage, outSlots] = projectFieldsToSlots(std::move(stage),
                                                             additionalFields,
                                                             resultSlot,
                                                             nodeId,
                                                             state.slotIdGenerator,
                                                             state,
                                                             &outputs);
            stage = std::move(outStage);

            for (size_t i = 0; i < additionalFields.size(); ++i) {
                auto& field = additionalFields[i];
                auto slot = outSlots[i];
                outputs.set(std::pair(PlanStageSlots::kField, std::move(field)), slot);
            }
        }
    }

    if (reqs.has(PlanStageSlots::kReturnKey)) {
        SbExpr::Vector args;
        for (auto&& elem : keyPattern) {
            StringData name = elem.fieldNameStringData();
            args.emplace_back(b.makeStrConstant(name));
            args.emplace_back(outputs.get(std::pair(PlanStageSlots::kField, name)));
        }

        auto [outStage, outSlots] =
            b.makeProject(std::move(stage), b.makeFunction("newObj"_sd, std::move(args)));
        stage = std::move(outStage);

        outputs.set(PlanStageSlots::kReturnKey, outSlots[0]);
    }

    return {std::move(stage), std::move(outputs)};
}

std::tuple<SbStage, PlanStageSlots, boost::optional<std::pair<SbSlot, SbSlot>>>
generateSingleIntervalIndexScanAndSlots(StageBuilderState& state,
                                        const CollectionPtr& collection,
                                        const std::string& indexName,
                                        const BSONObj& keyPattern,
                                        bool forward,
                                        std::unique_ptr<key_string::Value> lowKey,
                                        std::unique_ptr<key_string::Value> highKey,
                                        const PlanStageReqs& reqs,
                                        PlanNodeId nodeId,
                                        bool isPointInterval) {
    PlanStageReqs ixScanReqs = computeReqsForIndexScan(reqs, keyPattern);

    auto [stage, outputs, boundsSlots] =
        generateSingleIntervalIndexScanAndSlotsImpl(state,
                                                    collection,
                                                    indexName,
                                                    keyPattern,
                                                    forward,
                                                    std::move(lowKey),
                                                    std::move(highKey),
                                                    ixScanReqs,
                                                    nodeId,
                                                    isPointInterval);

    auto [finalStage, finalOutputs] = setResultAndAdditionalFieldSlots(
        std::move(stage), std::move(outputs), keyPattern, reqs, state, nodeId);

    return {std::move(finalStage), std::move(finalOutputs), std::move(boundsSlots)};
}

std::pair<SbStage, PlanStageSlots> generateSingleIntervalIndexScan(StageBuilderState& state,
                                                                   const CollectionPtr& collection,
                                                                   const std::string& indexName,
                                                                   const BSONObj& keyPattern,
                                                                   bool forward,
                                                                   SbExpr lowKeyExpr,
                                                                   SbExpr highKeyExpr,
                                                                   const PlanStageReqs& reqs,
                                                                   PlanNodeId nodeId) {
    PlanStageReqs ixScanReqs = computeReqsForIndexScan(reqs, keyPattern);

    auto [stage, outputs] = generateSingleIntervalIndexScanImpl(state,
                                                                collection,
                                                                indexName,
                                                                keyPattern,
                                                                std::move(lowKeyExpr),
                                                                std::move(highKeyExpr),
                                                                ixScanReqs,
                                                                nodeId,
                                                                forward);

    return setResultAndAdditionalFieldSlots(
        std::move(stage), std::move(outputs), keyPattern, reqs, state, nodeId);
}

std::pair<SbStage, PlanStageSlots> generateIndexScanImpl(StageBuilderState& state,
                                                         const CollectionPtr& collection,
                                                         const IndexScanNode* ixn,
                                                         const PlanStageReqs& reqs) {
    SbBuilder b(state, ixn->nodeId());

    const auto& keyPattern = ixn->index.keyPattern;
    auto indexName = ixn->index.identifier.catalogName;
    auto descriptor = collection->getIndexCatalog()->findIndexByName(state.opCtx, indexName);
    tassert(5483200,
            str::stream() << "failed to find index in catalog named: "
                          << ixn->index.identifier.catalogName,
            descriptor);

    // Find the IndexAccessMethod which corresponds to the 'indexName'.
    auto accessMethod =
        collection->getIndexCatalog()->getEntry(descriptor)->accessMethod()->asSortedData();
    auto intervals =
        makeIntervalsFromIndexBounds(ixn->bounds,
                                     ixn->direction == 1,
                                     accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                                     accessMethod->getSortedDataInterface()->getOrdering());

    PlanStageSlots outputs;

    SbStage stage;
    if (intervals.size() == 1) {
        // If we have just a single interval, we can construct a simplified sub-tree.
        auto&& [lowKey, highKey] = intervals[0];

        const bool isPointInterval = *lowKey == *highKey;
        std::tie(stage, outputs, std::ignore) =
            generateSingleIntervalIndexScanAndSlotsImpl(state,
                                                        collection,
                                                        indexName,
                                                        keyPattern,
                                                        ixn->direction == 1,
                                                        std::move(lowKey),
                                                        std::move(highKey),
                                                        reqs,
                                                        ixn->nodeId(),
                                                        isPointInterval);
    } else if (intervals.size() > 1) {
        // If we were able to decompose multi-interval index bounds into a number of single-interval
        // bounds, we can also built an optimized sub-tree to perform an index scan.
        std::tie(stage, outputs, std::ignore) =
            generateOptimizedMultiIntervalIndexScan(state,
                                                    collection,
                                                    indexName,
                                                    keyPattern,
                                                    ixn->direction == 1,
                                                    std::move(intervals),
                                                    reqs,
                                                    ixn->nodeId());
    } else {
        // Generate a generic index scan for multi-interval index bounds.
        std::tie(stage, outputs, std::ignore) = generateGenericMultiIntervalIndexScan(
            state,
            collection,
            indexName,
            ixn,
            keyPattern,
            accessMethod->getSortedDataInterface()->getKeyStringVersion(),
            accessMethod->getSortedDataInterface()->getOrdering(),
            reqs);
    }

    if (ixn->shouldDedup) {
        if (collection->isClustered()) {
            stage = b.makeUnique(std::move(stage), outputs.get(PlanStageSlots::kRecordId));
        } else {
            stage = b.makeUniqueRoaring(std::move(stage), outputs.get(PlanStageSlots::kRecordId));
        }
    }

    if (ixn->filter) {
        const bool isOverIxscan = true;
        auto filterExpr = generateFilter(state, ixn->filter.get(), {}, outputs, isOverIxscan);
        if (!filterExpr.isNull()) {
            stage = b.makeFilter(std::move(stage), std::move(filterExpr));
        }
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<SbStage, PlanStageSlots> generateIndexScan(StageBuilderState& state,
                                                     const CollectionPtr& collection,
                                                     const IndexScanNode* ixn,
                                                     const PlanStageReqs& reqs) {
    const auto& keyPattern = ixn->index.keyPattern;
    PlanNodeId nodeId = ixn->nodeId();

    PlanStageReqs ixScanReqs =
        computeReqsForIndexScan(reqs, ixn->index.keyPattern, ixn->filter.get());

    auto [stage, outputs] = generateIndexScanImpl(state, collection, ixn, ixScanReqs);

    return setResultAndAdditionalFieldSlots(
        std::move(stage), std::move(outputs), keyPattern, reqs, state, nodeId);
}

IndexIntervals makeIntervalsFromIndexBounds(const IndexBounds& bounds,
                                            bool forward,
                                            key_string::Version version,
                                            Ordering ordering) {
    auto lowKeyInclusive{IndexBounds::isStartIncludedInBound(bounds.boundInclusion)};
    auto highKeyInclusive{IndexBounds::isEndIncludedInBound(bounds.boundInclusion)};
    auto intervals = [&]() -> std::vector<std::pair<BSONObj, BSONObj>> {
        auto lowKey = bounds.startKey;
        auto highKey = bounds.endKey;
        if (bounds.isSimpleRange ||
            IndexBoundsBuilder::isSingleInterval(
                bounds, &lowKey, &lowKeyInclusive, &highKey, &highKeyInclusive)) {
            return {{lowKey, highKey}};
        } else if (canBeDecomposedIntoSingleIntervals(
                       bounds.fields, &lowKeyInclusive, &highKeyInclusive)) {
            return decomposeIntoSingleIntervals(bounds.fields, lowKeyInclusive, highKeyInclusive);
        } else {
            // Index bounds cannot be represented as valid low/high keys.
            return {};
        }
    }();

    LOGV2_DEBUG(
        4742905, 5, "Number of generated interval(s) for ixscan", "num"_attr = intervals.size());
    IndexIntervals result;
    for (auto&& [lowKey, highKey] : intervals) {
        LOGV2_DEBUG(4742906,
                    5,
                    "Generated interval [lowKey, highKey]",
                    "lowKey"_attr = redact(lowKey),
                    "highKey"_attr = redact(highKey));
        result.push_back(makeKeyStringPair(
            lowKey, lowKeyInclusive, highKey, highKeyInclusive, version, ordering, forward));
    }
    return result;
}

std::pair<sbe::value::TypeTags, sbe::value::Value> packIndexIntervalsInSbeArray(
    IndexIntervals intervals) {
    auto [boundsTag, boundsVal] = sbe::value::makeNewArray();
    auto arr = sbe::value::getArrayView(boundsVal);
    sbe::value::ValueGuard boundsGuard{boundsTag, boundsVal};
    arr->reserve(intervals.size());
    for (auto&& [lowKey, highKey] : intervals) {
        auto [tag, val] = sbe::value::makeNewObject();
        auto obj = sbe::value::getObjectView(val);
        sbe::value::ValueGuard guard{tag, val};
        obj->reserve(2);
        obj->push_back(
            "l"_sd, sbe::value::TypeTags::keyString, sbe::value::makeKeyString(*lowKey).second);
        obj->push_back(
            "h"_sd, sbe::value::TypeTags::keyString, sbe::value::makeKeyString(*highKey).second);
        guard.reset();
        arr->push_back(tag, val);
    }
    boundsGuard.reset();
    return {boundsTag, boundsVal};
}

std::pair<SbStage, PlanStageSlots> generateIndexScanWithDynamicBoundsImpl(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const IndexScanNode* ixn,
    const PlanStageReqs& reqs) {
    SbBuilder b(state, ixn->nodeId());

    const auto& keyPattern = ixn->index.keyPattern;
    const bool forward = ixn->direction == 1;
    auto indexName = ixn->index.identifier.catalogName;
    auto descriptor = collection->getIndexCatalog()->findIndexByName(state.opCtx, indexName);
    tassert(6335101,
            str::stream() << "failed to find index in catalog named: "
                          << ixn->index.identifier.catalogName,
            descriptor);

    // Find the IndexAccessMethod which corresponds to the 'indexName'.
    auto accessMethod = descriptor->getEntry()->accessMethod()->asSortedData();

    SbStage stage;
    ParameterizedIndexScanSlots parameterizedScanSlots;

    PlanStageSlots outputs;

    // Whenever possible we should prefer building simplified single interval index scan plans in
    // order to get the best performance.
    if (auto intervalsRequired = canGenerateSingleIntervalIndexScan(ixn->iets);
        intervalsRequired != IntervalsRequired::MultiInterval) {
        boost::optional<std::pair<SbSlot, SbSlot>> indexScanBoundsSlots;
        std::tie(stage, outputs, indexScanBoundsSlots) =
            generateSingleIntervalIndexScanAndSlotsImpl(
                state,
                collection,
                indexName,
                keyPattern,
                forward,
                nullptr,
                nullptr,
                reqs,
                ixn->nodeId(),
                intervalsRequired == IntervalsRequired::EqualityInterval /* isPointInterval */);
        tassert(6484702,
                "lowKey and highKey runtime environment slots must be present",
                indexScanBoundsSlots);
        parameterizedScanSlots = {ParameterizedIndexScanSlots::SingleIntervalPlan{
            indexScanBoundsSlots->first.getId(), indexScanBoundsSlots->second.getId()}};
    } else {
        // Generate a generic index scan for multi-interval index bounds.
        auto [genericStage, genericOutputs, genericBoundsSlot] =
            generateGenericMultiIntervalIndexScan(
                state,
                collection,
                indexName,
                ixn,
                keyPattern,
                accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                accessMethod->getSortedDataInterface()->getOrdering(),
                reqs);
        tassert(6335203, "bounds slot for generic index scan is undefined", genericBoundsSlot);

        // If we were able to decompose multi-interval index bounds into a number of
        // single-interval bounds, we can also built an optimized sub-tree to perform an index
        // scan.
        auto [optimizedStage, optimizedOutputs, optimizedBoundsSlot] =
            generateOptimizedMultiIntervalIndexScan(state,
                                                    collection,
                                                    indexName,
                                                    keyPattern,
                                                    forward,
                                                    boost::none,
                                                    reqs,
                                                    ixn->nodeId());
        tassert(6335204, "bounds slot for index scan is undefined", optimizedBoundsSlot);

        // Generate a branch stage that will either execute an optimized or a generic index scan
        // based on the condition in the slot 'isGenericScanSlot'.
        auto isGenericScanSlot = SbSlot{state.env->registerSlot(
            sbe::value::TypeTags::Nothing, 0, true /* owned */, state.slotIdGenerator)};

        std::vector<std::pair<SbStage, PlanStageSlots>> stagesAndSlots;
        stagesAndSlots.emplace_back(std::move(genericStage), std::move(genericOutputs));
        stagesAndSlots.emplace_back(std::move(optimizedStage), std::move(optimizedOutputs));

        // Define a lambda for creating a BranchStage.
        auto makeBranchStage = [&](sbe::PlanStage::Vector inputStages,
                                   std::vector<SbSlotVector> inputSlots) {
            return b.makeBranch(std::move(inputStages[0]),
                                std::move(inputStages[1]),
                                SbExpr{isGenericScanSlot},
                                inputSlots[0],
                                inputSlots[1]);
        };

        // Call makeMergedPlanStageSlots(), passing in our 'makeBranchStage' lambda.
        auto [branchStage, branchOutputs] = PlanStageSlots::makeMergedPlanStageSlots(
            state, ixn->nodeId(), reqs, std::move(stagesAndSlots), makeBranchStage);
        stage = std::move(branchStage);

        outputs = std::move(branchOutputs);

        parameterizedScanSlots = {ParameterizedIndexScanSlots::GenericPlan{
            isGenericScanSlot.getId(), genericBoundsSlot->getId(), optimizedBoundsSlot->getId()}};
    }

    if (ixn->shouldDedup) {
        if (collection->isClustered()) {
            stage = b.makeUnique(std::move(stage), outputs.get(PlanStageSlots::kRecordId));
        } else {
            stage = b.makeUniqueRoaring(std::move(stage), outputs.get(PlanStageSlots::kRecordId));
        }
    }

    if (ixn->filter) {
        const bool isOverIxscan = true;
        auto filterExpr = generateFilter(state, ixn->filter.get(), {}, outputs, isOverIxscan);
        if (!filterExpr.isNull()) {
            stage = b.makeFilter(std::move(stage), std::move(filterExpr));
        }
    }

    state.data->indexBoundsEvaluationInfos.emplace_back(
        IndexBoundsEvaluationInfo{ixn->index,
                                  accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                                  accessMethod->getSortedDataInterface()->getOrdering(),
                                  ixn->direction,
                                  ixn->iets,
                                  std::move(parameterizedScanSlots)});

    return {std::move(stage), std::move(outputs)};
}

std::pair<SbStage, PlanStageSlots> generateIndexScanWithDynamicBounds(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const IndexScanNode* ixn,
    const PlanStageReqs& reqs) {
    const auto& keyPattern = ixn->index.keyPattern;
    PlanNodeId nodeId = ixn->nodeId();

    PlanStageReqs ixScanReqs =
        computeReqsForIndexScan(reqs, ixn->index.keyPattern, ixn->filter.get());

    auto [stage, outputs] =
        generateIndexScanWithDynamicBoundsImpl(state, collection, ixn, ixScanReqs);

    return setResultAndAdditionalFieldSlots(
        std::move(stage), std::move(outputs), keyPattern, reqs, state, nodeId);
}
}  // namespace mongo::stage_builder
