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


#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_index_scan.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/check_bounds.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/logv2/log.h"
#include "mongo/util/overloaded_visitor.h"
#include <boost/optional.hpp>

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

        return std::make_pair(lowKeyBob.obj(), highKeyBob.obj());
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

/**
 * Constructs an optimized version of an index scan for multi-interval index bounds for the case
 * when the bounds can be decomposed in a number of single-interval bounds. In this case, instead
 * of building a generic index scan to navigate through the index using the 'IndexBoundsChecker',
 * we will construct a subtree with a constant table scan containing all intervals we'd want to
 * scan through. Specifically, we will build the following subtree:
 *
 *         nlj [indexIdSlot] [lowKeySlot, highKeySlot]
 *              left
 *                  project [indexIdSlot = <indexName>,
 *                           indexKeyPatternSlot = <index key pattern>,
 *                           lowKeySlot = getField (unwindSlot, "l"),
 *                           highKeySlot = getField (unwindSlot, "h")]
 *                  unwind unwindSlot indexSlot boundsSlot false
 *                  project [boundsSlot = [{"l" : KS(...), "h" : KS(...)},
 *                                         {"l" : KS(...), "h" : KS(...)}, ...]]
 *                  limit 1
 *                  coscan
 *               right
 *                  ixseek lowKeySlot highKeySlot keyStringSlot snapshotIdSlot recordIdSlot []
 *                  @coll @index
 *
 * This subtree is similar to the single-interval subtree with the only difference that instead
 * of projecting a single pair of the low/high keys, we project an array of such pairs and then
 * use the unwind stage to flatten the array and generate multiple input intervals to the ixscan.
 *
 * In case when the 'intervals' are not specified, 'boundsSlot' will be registered in the runtime
 * environment and returned as a third element of the tuple.
 */
std::tuple<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>, boost::optional<sbe::value::SlotId>>
generateOptimizedMultiIntervalIndexScan(StageBuilderState& state,
                                        const CollectionPtr& collection,
                                        const std::string& indexName,
                                        const BSONObj& keyPattern,
                                        bool forward,
                                        boost::optional<IndexIntervals> intervals,
                                        sbe::IndexKeysInclusionSet indexKeysToInclude,
                                        sbe::value::SlotVector indexKeySlots,
                                        boost::optional<sbe::value::SlotId> snapshotIdSlot,
                                        boost::optional<sbe::value::SlotId> indexIdSlot,
                                        boost::optional<sbe::value::SlotId> recordSlot,
                                        boost::optional<sbe::value::SlotId> indexKeyPatternSlot,
                                        PlanYieldPolicy* yieldPolicy,
                                        PlanNodeId planNodeId) {
    using namespace std::literals;

    auto slotIdGenerator = state.slotIdGenerator;
    auto recordIdSlot = slotIdGenerator->generate();
    auto lowKeySlot = slotIdGenerator->generate();
    auto highKeySlot = slotIdGenerator->generate();

    auto limitStage = sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId);
    auto&& [boundsSlot, boundsStage] =
        [&]() -> std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> {
        if (intervals) {
            auto [boundsTag, boundsVal] = packIndexIntervalsInSbeArray(std::move(*intervals));
            const auto boundsSlot = slotIdGenerator->generate();
            return {boundsSlot,
                    sbe::makeProjectStage(std::move(limitStage),
                                          planNodeId,
                                          boundsSlot,
                                          makeConstant(boundsTag, boundsVal))};
        }

        // If the key intervals are not specified, they will be provided in the
        // 'lowHighKeyIntervalsSlot'.
        return {state.data->env->registerSlot(
                    sbe::value::TypeTags::Nothing, 0, true /* owned */, state.slotIdGenerator),
                std::move(limitStage)};
    }();

    // Project out the constructed array as a constant value if intervals are known at compile time
    // and add an unwind stage on top to flatten the interval bounds array.
    auto unwindSlot = slotIdGenerator->generate();
    auto unwind = sbe::makeS<sbe::UnwindStage>(
        std::move(boundsStage),
        boundsSlot,
        unwindSlot,
        slotIdGenerator->generate(), /* We don't need an index slot but must to provide it. */
        false /* Preserve null and empty arrays, in our case it cannot be empty anyway. */,
        planNodeId);

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    projects.emplace(lowKeySlot,
                     makeFunction("getField"_sd, makeVariable(unwindSlot), makeConstant("l"_sd)));
    projects.emplace(highKeySlot,
                     makeFunction("getField"_sd, makeVariable(unwindSlot), makeConstant("h"_sd)));
    if (indexIdSlot) {
        // Construct a copy of 'indexName' to project for use in the index consistency check.
        projects.emplace(*indexIdSlot, makeConstant(indexName));
    }

    if (indexKeyPatternSlot) {
        auto [bsonObjTag, bsonObjVal] =
            sbe::value::copyValue(sbe::value::TypeTags::bsonObject,
                                  sbe::value::bitcastFrom<const char*>(keyPattern.objdata()));
        projects.emplace(*indexKeyPatternSlot, makeConstant(bsonObjTag, bsonObjVal));
    }

    // Add another project stage to extract low and high keys from each value produced by unwind and
    // bind the keys to the 'lowKeySlot' and 'highKeySlot'.
    auto project =
        sbe::makeS<sbe::ProjectStage>(std::move(unwind), std::move(projects), planNodeId);

    // Whereas 'snapshotIdSlot' is used by the caller to inspect the snapshot id of the latest index
    // key, 'indexSnapshotSlot' is updated by the IndexScan below during yield to obtain the latest
    // snapshot id.
    boost::optional<sbe::value::SlotId> indexSnapshotSlot;
    if (snapshotIdSlot) {
        indexSnapshotSlot = slotIdGenerator->generate();
    }

    auto stage = sbe::makeS<sbe::IndexScanStage>(collection->uuid(),
                                                 indexName,
                                                 forward,
                                                 recordSlot,
                                                 recordIdSlot,
                                                 indexSnapshotSlot,
                                                 indexKeysToInclude,
                                                 std::move(indexKeySlots),
                                                 makeVariable(lowKeySlot),
                                                 makeVariable(highKeySlot),
                                                 yieldPolicy,
                                                 planNodeId);

    // Add a project on top of the index scan to remember the snapshotId of the most recent index
    // key returned by the IndexScan above. Otherwise, the index key's snapshot id would be
    // overwritten during yield.
    if (snapshotIdSlot) {
        stage = sbe::makeProjectStage(
            std::move(stage), planNodeId, *snapshotIdSlot, makeVariable(*indexSnapshotSlot));
    }

    auto outerSv = sbe::makeSV();
    if (indexIdSlot) {
        outerSv.push_back(*indexIdSlot);
    }

    if (indexKeyPatternSlot) {
        outerSv.push_back(*indexKeyPatternSlot);
    }

    // Finally, get the keys from the outer side and feed them to the inner side (ixscan).
    return {recordIdSlot,
            sbe::makeS<sbe::LoopJoinStage>(std::move(project),
                                           std::move(stage),
                                           std::move(outerSv),
                                           sbe::makeSV(lowKeySlot, highKeySlot),
                                           nullptr,
                                           planNodeId),
            boost::make_optional(!intervals, boundsSlot)};
}

/**
 * Builds an anchor sub-tree of the recursive index scan CTE to seed the result set with the initial
 * 'startKey' for the index scan.
 *
 * In case when the 'startKey' is not specified, 'boundsSlot' will be registered in the runtime
 * environment and returned as a third element of the tuple.
 */
std::tuple<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>, boost::optional<sbe::value::SlotId>>
makeAnchorBranchForGenericIndexScan(StageBuilderState& state,
                                    boost::optional<std::unique_ptr<KeyString::Value>> startKey,
                                    const sbe::value::SlotVector& unusedSlots,
                                    PlanNodeId planNodeId) {
    // Just project out the 'startKey' KeyString. We must bind a slot for each field requested from
    // index keys, but we don't expect these values to ever get used, so we bind them to Nothing.
    auto startKeySlot = state.slotId();
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    boost::optional<sbe::value::SlotId> boundsSlot;
    if (startKey) {
        projects.insert(
            {startKeySlot,
             makeConstant(sbe::value::TypeTags::ksValue,
                          sbe::value::bitcastFrom<KeyString::Value*>(startKey->release()))});
    } else {
        // If the 'startKey' is not specified, it will be provided in the 'boundsSlot'.
        boundsSlot = state.data->env->registerSlot(
            sbe::value::TypeTags::Nothing, 0, true /* owned */, state.slotIdGenerator);
        projects.insert({startKeySlot, makeVariable(*boundsSlot)});
    }

    for (auto&& unusedSlot : unusedSlots) {
        projects.insert({unusedSlot, makeConstant(sbe::value::TypeTags::Nothing, 0)});
    }
    return {startKeySlot,
            sbe::makeS<sbe::ProjectStage>(
                sbe::makeS<sbe::LimitSkipStage>(
                    sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId),
                std::move(projects),
                planNodeId),
            boundsSlot};
}

/**
 * Builds a recursive sub-tree of the recursive CTE to generate the remainder of the result set
 * consisting of valid recordId's and index seek keys to restart the index scan from.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
makeRecursiveBranchForGenericIndexScan(const CollectionPtr& collection,
                                       const std::string& indexName,
                                       sbe::CheckBoundsParams params,
                                       sbe::SpoolId spoolId,
                                       sbe::IndexKeysInclusionSet indexKeysToInclude,
                                       sbe::value::SlotVector savedIndexKeySlots,
                                       boost::optional<sbe::value::SlotId> snapshotIdSlot,
                                       boost::optional<sbe::value::SlotId> indexIdSlot,
                                       boost::optional<sbe::value::SlotId> indexKeySlot,
                                       boost::optional<sbe::value::SlotId> indexKeyPatternSlot,
                                       sbe::value::SlotIdGenerator* slotIdGenerator,
                                       PlanYieldPolicy* yieldPolicy,
                                       PlanNodeId planNodeId) {
    // The IndexScanStage in this branch will always produce a KeyString. As such, we use
    // 'indexKeySlot' if is defined and generate a new slot otherwise.
    auto resultSlot = indexKeySlot ? *indexKeySlot : slotIdGenerator->generate();
    auto recordIdSlot = slotIdGenerator->generate();
    auto seekKeySlot = slotIdGenerator->generate();

    // Build a standard index scan nested loop join with the outer branch producing a low key
    // to be fed into the index scan. The low key is taken from the 'seekKeySlot' which would
    // contain a value from the stack spool. See below for details.
    auto stage = sbe::makeS<sbe::IndexScanStage>(collection->uuid(),
                                                 indexName,
                                                 params.direction == 1,
                                                 resultSlot,
                                                 recordIdSlot,
                                                 snapshotIdSlot,
                                                 indexKeysToInclude,
                                                 std::move(savedIndexKeySlots),
                                                 makeVariable(seekKeySlot) /* seekKeyLow */,
                                                 nullptr /* seekKeyHigh */,
                                                 yieldPolicy,
                                                 planNodeId);

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    if (indexIdSlot) {
        // Construct a copy of 'indexName' to project for use in the index consistency check.
        projects.emplace(*indexIdSlot, makeConstant(indexName));
    }

    if (indexKeyPatternSlot) {
        auto [bsonObjTag, bsonObjVal] = sbe::value::copyValue(
            sbe::value::TypeTags::bsonObject,
            sbe::value::bitcastFrom<const char*>(params.keyPattern.objdata()));
        projects.emplace(*indexKeyPatternSlot, makeConstant(bsonObjTag, bsonObjVal));
    }

    if (!projects.empty()) {
        stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), planNodeId);
    }

    auto spoolValsSV = sbe::makeSV(seekKeySlot);
    if (indexIdSlot) {
        spoolValsSV.push_back(*indexIdSlot);
    }

    if (indexKeyPatternSlot) {
        spoolValsSV.push_back(*indexKeyPatternSlot);
    }

    auto correlatedSv = spoolValsSV;

    // Inject a nested loop join with the outer branch being a stack spool, and the inner branch
    // being the index scan subtree ('stage'). The stack spool is populated from the values
    // generated by the index scan above, and passed through the check bounds stage, which would
    // produce either a valid recordId to be consumed by the stage sitting above the index scan
    // sub-tree, or a seek key to restart the index scan from. The spool will only store the seek
    // keys, passing through valid recordId's.
    auto checkBoundsSlot = slotIdGenerator->generate();
    return {checkBoundsSlot,
            sbe::makeS<sbe::LoopJoinStage>(sbe::makeS<sbe::SpoolConsumerStage<true>>(
                                               spoolId, std::move(spoolValsSV), planNodeId),
                                           sbe::makeS<sbe::CheckBoundsStage>(std::move(stage),
                                                                             std::move(params),
                                                                             resultSlot,
                                                                             recordIdSlot,
                                                                             checkBoundsSlot,
                                                                             planNodeId),
                                           sbe::makeSV(),
                                           std::move(correlatedSv),
                                           nullptr,
                                           planNodeId)};
}

/**
 * Builds a generic multi-interval index scan for the cases when index bounds cannot be represented
 * as valid low/high keys. In this case we will build a recursive sub-tree and will use the
 * 'CheckBoundsStage' to navigate through the index. The recursive sub-tree is built using a union
 * stage in conjunction with the stack spool:
 *
 *         filter {isNumber(resultSlot)}
 *         lspool [resultSlot, varSlots...] {!isNumber(resultSlot)}
 *         union [resultSlot, varSlots...]
 *            [anchorSlot, unusedSlots]
 *                project [startKeySlot = KS(...), unusedVarSlot0 = Nothing, ...]
 *                limit 1
 *                coscan
 *            [checkBoundsSlot, savedSlots...]
 *                 nlj [] [seekKeySlot, indexIdSlot, indexKeyPatternSlot]
 *                     left
 *                         sspool [seekKeySlot, indexIdSlot, indexKeyPatternSlot]
 *                     right
 *                         chkbounds resultSlot recordIdSlot checkBoundsSlot
 *                         project [indexIdSlot = <indexName>,
 *                                  indexKeyPatternSlot = <index key pattern>]
 *                         ixseek seekKeySlot resultSlot recordIdSlot
 *                                snapshotIdSlot savedIndexKeySlots []
 *                                @coll @index
 *
 *   - The anchor union branch is the starting point of the recursive subtree. It pushes the
 *     starting index into the lspool stage. The lspool has a filter predicate to ensure that
 *     only index keys will be stored in the spool.
 *   - There is a filter stage at the top of the subtree to ensure that we will only produce
 *     recordId values.
 *   - The recursive union branch does the remaining job. It has a nested loop join with the outer
 *     branch being a stack spool, which reads data from the lspool above.
 *        1. The outer branch reads next seek key from sspool.
 *               * If the spool is empty, we're done with the scan.
 *        2. The seek key is passed to the inner branch.
 *        3. The inner branch execution starts with the ixscan.
 *        4. Two slots produced by the ixscan, 'resultSlot' and 'recordIdSlot', are passed to
 *            the chkbounds stage. Note that 'resultSlot' would contain the index key.
 *        5. The chkbounds stage can produce one of the following values:
 *               * The recordId value, taken from the ixscan stage, if the index key is within the
 *                 bounds.
 *               * A seek key the ixscan will have to restart from if the key is not within the
 *                 bounds, but has not exceeded the maximum value.
 *                    - At this point the chkbounds stage returns ADVANCED state, to propagate the
 *                      seek key point, but on the next call to getNext will return EOF to signal
 *                      that we've done with the current interval and need to continue from a new
 *                      seek point.
 *               * If the key is past the bound, no value is produced and EOF state is returned.
 *        6. If chkbounds returns EOF, the process repeats from step 1.
 *        7. Otherwise, the produces values is pulled up to the lspool stage and either is
 *           stored in the buffer, if it was a seek key, or just propagated to the upper stage as a
 *           valid recordId, and the process continues from step 4 by fetching the next key from the
 *           index.
 *   - The recursion is terminated when the sspool becomes empty.
 *
 * In case when the 'bounds' are not specified, 'initialStartKey' and 'indexBounds' will be
 * registered in the runtime environment and returned as a pair in the third element of the tuple.
 */
std::tuple<sbe::value::SlotId,
           std::unique_ptr<sbe::PlanStage>,
           boost::optional<std::pair<sbe::value::SlotId, sbe::value::SlotId>>>
generateGenericMultiIntervalIndexScan(StageBuilderState& state,
                                      const CollectionPtr& collection,
                                      const IndexScanNode* ixn,
                                      KeyString::Version version,
                                      Ordering ordering,
                                      sbe::IndexKeysInclusionSet indexKeysToInclude,
                                      sbe::value::SlotVector indexKeySlots,
                                      boost::optional<sbe::value::SlotId> snapshotIdSlot,
                                      boost::optional<sbe::value::SlotId> indexIdSlot,
                                      boost::optional<sbe::value::SlotId> indexKeySlot,
                                      boost::optional<sbe::value::SlotId> indexKeyPatternSlot,
                                      sbe::value::SlotIdGenerator* slotIdGenerator,
                                      sbe::value::SpoolIdGenerator* spoolIdGenerator,
                                      PlanYieldPolicy* yieldPolicy) {
    using namespace std::literals;

    auto resultSlot = slotIdGenerator->generate();
    IndexSeekPoint seekPoint;
    const bool hasDynamicIndexBounds = !ixn->iets.empty();
    const bool didGetStartSeekPoint = !hasDynamicIndexBounds &&
        IndexBoundsChecker{&ixn->bounds, ixn->index.keyPattern, ixn->direction}.getStartSeekPoint(
            &seekPoint);

    // Get the start seek key for our recursive scan. If there are no possible index entries that
    // match the bounds and we cannot generate a start seek key, inject an EOF sub-tree an exit
    // straight away - this index scan won't emit any results.
    if (!hasDynamicIndexBounds && !didGetStartSeekPoint) {
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
        projects.emplace(resultSlot, makeConstant(sbe::value::TypeTags::Nothing, 0));

        for (auto slot : indexKeySlots) {
            projects.emplace(slot, makeConstant(sbe::value::TypeTags::Nothing, 0));
        }

        if (snapshotIdSlot) {
            projects.emplace(*snapshotIdSlot, makeConstant(sbe::value::TypeTags::Nothing, 0));
        }

        if (indexIdSlot) {
            projects.emplace(*indexIdSlot, makeConstant(sbe::value::TypeTags::Nothing, 0));
        }

        if (indexKeySlot) {
            projects.emplace(*indexKeySlot, makeConstant(sbe::value::TypeTags::Nothing, 0));
        }

        if (indexKeyPatternSlot) {
            projects.emplace(*indexKeyPatternSlot, makeConstant(sbe::value::TypeTags::Nothing, 0));
        }

        return {resultSlot,
                sbe::makeS<sbe::ProjectStage>(
                    sbe::makeS<sbe::LimitSkipStage>(
                        sbe::makeS<sbe::CoScanStage>(ixn->nodeId()), 0, boost::none, ixn->nodeId()),
                    std::move(projects),
                    ixn->nodeId()),
                boost::none};
    }

    sbe::value::SlotVector unionOutputSlots;
    for (auto&& indexKey : indexKeySlots) {
        unionOutputSlots.push_back(indexKey);
    }

    if (snapshotIdSlot) {
        unionOutputSlots.push_back(*snapshotIdSlot);
    }

    if (indexIdSlot) {
        unionOutputSlots.push_back(*indexIdSlot);
    }

    if (indexKeySlot) {
        unionOutputSlots.push_back(*indexKeySlot);
    }

    if (indexKeyPatternSlot) {
        unionOutputSlots.push_back(*indexKeyPatternSlot);
    }

    // Build the anchor branch of the union.
    auto unusedSlots = slotIdGenerator->generateMultiple(unionOutputSlots.size());
    auto startKey = !hasDynamicIndexBounds
        ? boost::make_optional(std::make_unique<KeyString::Value>(
              IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                  seekPoint, version, ordering, ixn->direction == 1)))
        : boost::none;
    auto [anchorSlot, anchorBranch, initialStartKeySlot] =
        makeAnchorBranchForGenericIndexScan(state, std::move(startKey), unusedSlots, ixn->nodeId());

    auto spoolId = spoolIdGenerator->generate();

    // Build the recursive branch of the union.
    sbe::value::SlotVector savedSlots;
    auto savedIndexKeySlots = slotIdGenerator->generateMultiple(indexKeySlots.size());
    for (auto&& slot : savedIndexKeySlots) {
        savedSlots.push_back(slot);
    }

    boost::optional<sbe::value::SlotId> savedSnapshot;
    if (snapshotIdSlot) {
        savedSnapshot = slotIdGenerator->generate();
        savedSlots.push_back(*savedSnapshot);
    }

    boost::optional<sbe::value::SlotId> savedIndexId;
    if (indexIdSlot) {
        savedIndexId = slotIdGenerator->generate();
        savedSlots.push_back(*savedIndexId);
    }

    boost::optional<sbe::value::SlotId> savedKeyString;
    if (indexKeySlot) {
        savedKeyString = slotIdGenerator->generate();
        savedSlots.push_back(*savedKeyString);
    }

    boost::optional<sbe::value::SlotId> savedKeyPattern;
    if (indexKeyPatternSlot) {
        savedKeyPattern = slotIdGenerator->generate();
        savedSlots.push_back(*savedKeyPattern);
    }

    // Pass IndexBounds to the recursive branch of the index scan. In case the 'bounds' are not
    // defined during the function call, register a slot in the runtime environment, where
    // IndexBounds will be defined.
    auto indexBounds = [&]() -> sbe::CheckBoundsParams::IndexBoundsType {
        if (!hasDynamicIndexBounds) {
            return ixn->bounds;
        }

        const auto boundsSlot = state.data->env->registerSlot(
            sbe::value::TypeTags::Nothing, 0, true /* owned */, state.slotIdGenerator);
        return boundsSlot;
    }();
    const auto indexBoundsSlot = stdx::holds_alternative<sbe::value::SlotId>(indexBounds)
        ? boost::make_optional(stdx::get<sbe::value::SlotId>(indexBounds))
        : boost::none;
    auto [recursiveSlot, recursiveBranch] = makeRecursiveBranchForGenericIndexScan(
        collection,
        ixn->index.identifier.catalogName,
        {std::move(indexBounds), ixn->index.keyPattern, ixn->direction, version, ordering},
        spoolId,
        indexKeysToInclude,
        savedIndexKeySlots,
        savedSnapshot,
        savedIndexId,
        savedKeyString,
        savedKeyPattern,
        slotIdGenerator,
        yieldPolicy,
        ixn->nodeId());

    // Construct a union stage from the two branches.
    auto makeSlotVector = [](sbe::value::SlotId headSlot, const sbe::value::SlotVector& varSlots) {
        sbe::value::SlotVector sv;
        sv.reserve(1 + varSlots.size());
        sv.push_back(headSlot);
        sv.insert(sv.end(), varSlots.begin(), varSlots.end());
        return sv;
    };
    auto unionStage = sbe::makeS<sbe::UnionStage>(
        sbe::makeSs(std::move(anchorBranch), std::move(recursiveBranch)),
        std::vector<sbe::value::SlotVector>{makeSlotVector(anchorSlot, unusedSlots),
                                            makeSlotVector(recursiveSlot, savedSlots)},
        makeSlotVector(resultSlot, unionOutputSlots),
        ixn->nodeId());

    // Stick in a lazy producer spool on top. The specified predicate will ensure that we will only
    // store the seek key values in the spool (that is, if the value type is not a number, or not
    // a recordId).
    auto spool = sbe::makeS<sbe::SpoolLazyProducerStage>(
        std::move(unionStage),
        spoolId,
        makeSlotVector(resultSlot, std::move(unionOutputSlots)),
        makeNot(makeFunction("isRecordId"_sd, sbe::makeE<sbe::EVariable>(resultSlot))),
        ixn->nodeId());

    // Finally, add a filter stage on top to filter out seek keys and return only recordIds.
    return {
        resultSlot,
        sbe::makeS<sbe::FilterStage<false>>(std::move(spool),
                                            makeFunction("isRecordId"_sd, makeVariable(resultSlot)),
                                            ixn->nodeId()),
        hasDynamicIndexBounds
            ? boost::make_optional(std::pair(*initialStartKeySlot, *indexBoundsSlot))
            : boost::none};
}

/**
 * Checks if we can create a single interval index scan plan. Creation of the single interval index
 * scan plans is preferred due to lower query latency as a result of faster plan recovery from the
 * cache. The rule for checking if 'iets' resolve to a single interval is as follows:
 * - an optional sequence of '$eq' or constant point intervals followed by
 * - an optional single interval of a comparison match expression or a constant interval or an
 * intersection of two such nodes followed by
 * - an optional sequence of unbounded intervals [MinKey, MaxKey].
 */
bool canGenerateSingleIntervalIndexScan(const std::vector<interval_evaluation_tree::IET>& iets) {
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
                    return false;
                }
                break;
            case State::ComparisonOrConstRange:
                if (!isMinToMax) {
                    return false;
                }

                // Transition to the next state as we allow only one bounded range, after that all
                // remaining fields must be unbounded.
                currentState = State::UnboundedInterval;
                break;
            case State::UnboundedInterval:
                if (!isMinToMax) {
                    return false;
                }
                break;
        }
    }

    return true;
}
}  // namespace

/**
 * Constructs the most simple version of an index scan from the single interval index bounds. The
 * generated subtree will have the following form:
 *
 *         nlj [indexIdSlot, keyPatternSlot] []
 *              left
 *                  project [indexIdSlot = <indexName>, keyPatternSlot = <index key pattern>]
 *                  limit 1
 *                  coscan
 *               right
 *                  ixseek lowKeySlot highKeySlot recordIdSlot [] @coll @index
 *
 * The inner branch of the nested loop join produces a single row with indexName and index key
 * pattern to be consumed above for the index key consistency check done when we do a fetch. In
 * case when the 'lowKey' and 'highKey' are not specified, slots will be registered for them in the
 * runtime environment and their slot ids returned as a pair in the third element of the tuple.
 *
 * If 'recordSlot' is provided, than the corresponding slot will be filled out with each KeyString
 * in the index.
 */
std::tuple<sbe::value::SlotId,
           std::unique_ptr<sbe::PlanStage>,
           boost::optional<std::pair<sbe::value::SlotId, sbe::value::SlotId>>>
generateSingleIntervalIndexScan(StageBuilderState& state,
                                const CollectionPtr& collection,
                                const std::string& indexName,
                                const BSONObj& keyPattern,
                                bool forward,
                                std::unique_ptr<KeyString::Value> lowKey,
                                std::unique_ptr<KeyString::Value> highKey,
                                sbe::IndexKeysInclusionSet indexKeysToInclude,
                                sbe::value::SlotVector indexKeySlots,
                                boost::optional<sbe::value::SlotId> snapshotIdSlot,
                                boost::optional<sbe::value::SlotId> indexIdSlot,
                                boost::optional<sbe::value::SlotId> recordSlot,
                                boost::optional<sbe::value::SlotId> indexKeyPatternSlot,
                                PlanYieldPolicy* yieldPolicy,
                                PlanNodeId planNodeId) {
    auto slotIdGenerator = state.slotIdGenerator;
    auto recordIdSlot = slotIdGenerator->generate();
    tassert(6584701,
            "Either both lowKey and highKey are specified or none of them are",
            (lowKey && highKey) || (!lowKey && !highKey));
    const bool shouldRegisterLowHighKeyInRuntimeEnv = !lowKey;

    // This helper function returns a pair of 'EExpression' and an optional slotId depending on the
    // presence of the 'key' argument. If the 'key' argument is present, we return a copy of the
    // 'key' as wrapped in an 'EConstant' and 'boost::none' for the second part of the
    // pair. The 'EConstant' can be embedded into the ixscan stage and eliminates the need for a
    // 'LoopJoinStage' to feed the 'lowKey' and 'highKey' slots into the ixscan. Otherwise, a
    // slot is generated in the runtime environment and this function returns the slot wrapped in
    // an 'EVariable' as well as the slotId itself.
    auto makeKeyExpr = [&](std::unique_ptr<KeyString::Value> key)
        -> std::pair<std::unique_ptr<sbe::EExpression>, boost::optional<sbe::value::SlotId>> {
        if (key) {
            return std::pair{
                makeConstant(sbe::value::TypeTags::ksValue,
                             sbe::value::bitcastFrom<KeyString::Value*>(key.release())),
                boost::none};
        } else {
            auto keySlot = state.data->env->registerSlot(
                sbe::value::TypeTags::Nothing, 0, true /* owned */, slotIdGenerator);
            return std::pair{makeVariable(keySlot), keySlot};
        }
    };

    std::unique_ptr<sbe::EExpression> lowKeyExpr;
    boost::optional<sbe::value::SlotId> lowKeySlot;
    std::tie(lowKeyExpr, lowKeySlot) = makeKeyExpr(std::move(lowKey));

    std::unique_ptr<sbe::EExpression> highKeyExpr;
    boost::optional<sbe::value::SlotId> highKeySlot;
    std::tie(highKeyExpr, highKeySlot) = makeKeyExpr(std::move(highKey));

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;
    if (indexIdSlot) {
        // Construct a copy of 'indexName' to project for use in the index consistency check.
        projects.emplace(*indexIdSlot, makeConstant(indexName));
    }

    if (indexKeyPatternSlot) {
        auto [bsonObjTag, bsonObjVal] =
            sbe::value::copyValue(sbe::value::TypeTags::bsonObject,
                                  sbe::value::bitcastFrom<const char*>(keyPattern.objdata()));
        projects.emplace(*indexKeyPatternSlot, makeConstant(bsonObjTag, bsonObjVal));
    }

    auto lowHighKeyBranch = [&]() {
        auto childStage = [&]() {
            auto limitStage = sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), 1, boost::none, planNodeId);
            if (projects.empty()) {
                return limitStage;
            }

            return sbe::makeS<sbe::ProjectStage>(
                std::move(limitStage), std::move(projects), planNodeId);
        }();

        // If low and high keys are provided in the runtime environment, then we need to create
        // a cfilter stage on top of project in order to be sure that the single interval
        // exists (the interval may be empty), in which case the index scan plan should simply
        // return EOF.
        if (!shouldRegisterLowHighKeyInRuntimeEnv) {
            return childStage;
        }
        return sbe::makeS<sbe::FilterStage</* IsConst */ true, /* IsEof */ false>>(
            std::move(childStage),
            makeBinaryOp(sbe::EPrimBinary::logicAnd,
                         makeFunction("exists", lowKeyExpr->clone()),
                         makeFunction("exists", highKeyExpr->clone())),
            planNodeId);
    }();

    // Whereas 'snapshotIdSlot' is used by the caller to inspect the snapshot id of the latest index
    // key, 'indexSnapshotSlot' is updated by the IndexScan below during yield to obtain the latest
    // snapshot id.
    boost::optional<sbe::value::SlotId> indexSnapshotSlot;
    if (snapshotIdSlot) {
        indexSnapshotSlot = slotIdGenerator->generate();
    }

    // Scan the index in the range {'lowKeySlot', 'highKeySlot'} (subject to inclusive or
    // exclusive boundaries), and produce a single field recordIdSlot that can be used to
    // position into the collection.
    auto stage = sbe::makeS<sbe::IndexScanStage>(collection->uuid(),
                                                 indexName,
                                                 forward,
                                                 recordSlot,
                                                 recordIdSlot,
                                                 indexSnapshotSlot,
                                                 indexKeysToInclude,
                                                 std::move(indexKeySlots),
                                                 std::move(lowKeyExpr),
                                                 std::move(highKeyExpr),
                                                 yieldPolicy,
                                                 planNodeId);

    // Add a project on top of the index scan to remember the snapshotId of the most recent index
    // key returned by the IndexScan above. Otherwise, the index key's snapshot id would be
    // overwritten during yield.
    if (snapshotIdSlot) {
        stage = sbe::makeProjectStage(
            std::move(stage), planNodeId, *snapshotIdSlot, makeVariable(*indexSnapshotSlot));
    }

    auto outerSv = sbe::makeSV();
    if (indexIdSlot) {
        outerSv.push_back(*indexIdSlot);
    }

    if (indexKeyPatternSlot) {
        outerSv.push_back(*indexKeyPatternSlot);
    }

    // Finally, get the keys from the outer side and feed them to the inner side.
    return {recordIdSlot,
            sbe::makeS<sbe::LoopJoinStage>(std::move(lowHighKeyBranch),
                                           std::move(stage),
                                           std::move(outerSv),
                                           sbe::makeSV(),
                                           nullptr,
                                           planNodeId),
            shouldRegisterLowHighKeyInRuntimeEnv
                ? boost::make_optional(std::pair(*lowKeySlot, *highKeySlot))
                : boost::none};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateIndexScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const IndexScanNode* ixn,
    const sbe::IndexKeysInclusionSet& originalIndexKeyBitset,
    PlanYieldPolicy* yieldPolicy,
    StringMap<const IndexAccessMethod*>* iamMap,
    bool needsCorruptionCheck) {

    auto indexName = ixn->index.identifier.catalogName;
    auto descriptor = collection->getIndexCatalog()->findIndexByName(state.opCtx, indexName);
    tassert(5483200,
            str::stream() << "failed to find index in catalog named: "
                          << ixn->index.identifier.catalogName,
            descriptor);

    auto keyPattern = descriptor->keyPattern();

    // Find the IndexAccessMethod which corresponds to the 'indexName'.
    auto accessMethod =
        collection->getIndexCatalog()->getEntry(descriptor)->accessMethod()->asSortedData();
    auto intervals =
        makeIntervalsFromIndexBounds(ixn->bounds,
                                     ixn->direction == 1,
                                     accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                                     accessMethod->getSortedDataInterface()->getOrdering());

    std::unique_ptr<sbe::PlanStage> stage;
    PlanStageSlots outputs;

    // Determine the set of fields from the index required to apply the filter and union those with
    // the set of fields from the index required by the parent stage.
    auto [indexFilterKeyBitset, indexFilterKeyFields] = [&]() {
        if (ixn->filter) {
            DepsTracker tracker;
            match_expression::addDependencies(ixn->filter.get(), &tracker);
            return makeIndexKeyInclusionSet(ixn->index.keyPattern, tracker.fields);
        }
        return std::make_pair(sbe::IndexKeysInclusionSet{}, std::vector<std::string>{});
    }();
    auto indexKeyBitset = originalIndexKeyBitset | indexFilterKeyBitset;
    auto indexKeySlots = state.slotIdGenerator->generateMultiple(indexKeyBitset.count());
    sbe::value::SlotVector relevantSlots;

    // Generate the relevant slots and add the access method corresponding to 'indexName' to
    // 'iamMap' if a parent stage needs to execute a consistency check.
    boost::optional<sbe::value::SlotId> snapshotIdSlot;
    boost::optional<sbe::value::SlotId> indexIdSlot;
    boost::optional<sbe::value::SlotId> indexKeySlot;

    if (iamMap) {
        iamMap->insert({indexName, accessMethod});

        snapshotIdSlot = state.slotId();
        outputs.set(PlanStageSlots::kSnapshotId, *snapshotIdSlot);
        relevantSlots.push_back(*snapshotIdSlot);

        indexIdSlot = state.slotId();
        outputs.set(PlanStageSlots::kIndexId, *indexIdSlot);
        relevantSlots.push_back(*indexIdSlot);

        indexKeySlot = state.slotId();
        outputs.set(PlanStageSlots::kIndexKey, *indexKeySlot);
        relevantSlots.push_back(*indexKeySlot);
    }

    // Generate a slot for an index key pattern if a parent stage needs to execute a corruption
    // check.
    boost::optional<sbe::value::SlotId> indexKeyPatternSlot;
    if (needsCorruptionCheck) {
        indexKeyPatternSlot = state.slotId();
        outputs.set(PlanStageSlots::kIndexKeyPattern, *indexKeyPatternSlot);
        relevantSlots.push_back(*indexKeyPatternSlot);
    }

    if (intervals.size() == 1) {
        // If we have just a single interval, we can construct a simplified sub-tree.
        auto&& [lowKey, highKey] = intervals[0];
        sbe::value::SlotId recordIdSlot;

        std::tie(recordIdSlot, stage, std::ignore) =
            generateSingleIntervalIndexScan(state,
                                            collection,
                                            indexName,
                                            keyPattern,
                                            ixn->direction == 1,
                                            std::move(lowKey),
                                            std::move(highKey),
                                            indexKeyBitset,
                                            indexKeySlots,
                                            snapshotIdSlot,
                                            indexIdSlot,
                                            indexKeySlot,
                                            indexKeyPatternSlot,
                                            yieldPolicy,
                                            ixn->nodeId());

        outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    } else if (intervals.size() > 1) {
        // If we were able to decompose multi-interval index bounds into a number of single-interval
        // bounds, we can also built an optimized sub-tree to perform an index scan.
        sbe::value::SlotId recordIdSlot;
        std::tie(recordIdSlot, stage, std::ignore) =
            generateOptimizedMultiIntervalIndexScan(state,
                                                    collection,
                                                    indexName,
                                                    keyPattern,
                                                    ixn->direction == 1,
                                                    std::move(intervals),
                                                    indexKeyBitset,
                                                    indexKeySlots,
                                                    snapshotIdSlot,
                                                    indexIdSlot,
                                                    indexKeySlot,
                                                    indexKeyPatternSlot,
                                                    yieldPolicy,
                                                    ixn->nodeId());

        outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    } else {
        // Generate a generic index scan for multi-interval index bounds.
        sbe::value::SlotId recordIdSlot;
        std::tie(recordIdSlot, stage, std::ignore) = generateGenericMultiIntervalIndexScan(
            state,
            collection,
            ixn,
            accessMethod->getSortedDataInterface()->getKeyStringVersion(),
            accessMethod->getSortedDataInterface()->getOrdering(),
            indexKeyBitset,
            indexKeySlots,
            snapshotIdSlot,
            indexIdSlot,
            indexKeySlot,
            indexKeyPatternSlot,
            state.slotIdGenerator,
            state.spoolIdGenerator,
            yieldPolicy);

        outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    }

    if (ixn->shouldDedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(PlanStageSlots::kRecordId)), ixn->nodeId());
    }

    relevantSlots.push_back(outputs.get(PlanStageSlots::kRecordId));

    if (ixn->filter) {
        // We only need to pass those index key slots to the filter generator which correspond to
        // the fields of the index key pattern that are depended on to compute the predicate.
        auto indexFilterKeySlots = makeIndexKeyOutputSlotsMatchingParentReqs(
            ixn->index.keyPattern, indexFilterKeyBitset, indexKeyBitset, indexKeySlots);

        // Relevant slots must include slots for all index keys in case they are needed by parent
        // stages (for instance, covered shard filter).
        relevantSlots.insert(relevantSlots.end(), indexKeySlots.begin(), indexKeySlots.end());

        auto outputStage = generateIndexFilter(state,
                                               ixn->filter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               std::move(indexFilterKeySlots),
                                               std::move(indexFilterKeyFields),
                                               ixn->nodeId());
        stage = outputStage.extractStage(ixn->nodeId());
    }

    outputs.setIndexKeySlots(makeIndexKeyOutputSlotsMatchingParentReqs(
        ixn->index.keyPattern, originalIndexKeyBitset, indexKeyBitset, indexKeySlots));

    return {std::move(stage), std::move(outputs)};
}

IndexIntervals makeIntervalsFromIndexBounds(const IndexBounds& bounds,
                                            bool forward,
                                            KeyString::Version version,
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
                    "lowKey"_attr = lowKey,
                    "highKey"_attr = highKey);
        // Note that 'makeKeyFromBSONKeyForSeek()' is intended to compute the "start" key for an
        // index scan. The logic for computing a "discriminator" for an "end" key is reversed, which
        // is why we use 'makeKeyStringFromBSONKey()' to manually specify the discriminator for the
        // end key.
        result.push_back(
            {std::make_unique<KeyString::Value>(
                 IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                     lowKey, version, ordering, forward, lowKeyInclusive)),
             std::make_unique<KeyString::Value>(IndexEntryComparison::makeKeyStringFromBSONKey(
                 highKey,
                 version,
                 ordering,
                 forward != highKeyInclusive ? KeyString::Discriminator::kExclusiveBefore
                                             : KeyString::Discriminator::kExclusiveAfter))});
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
        obj->push_back("l"_sd,
                       sbe::value::TypeTags::ksValue,
                       sbe::value::bitcastFrom<KeyString::Value*>(lowKey.release()));
        obj->push_back("h"_sd,
                       sbe::value::TypeTags::ksValue,
                       sbe::value::bitcastFrom<KeyString::Value*>(highKey.release()));
        guard.reset();
        arr->push_back(tag, val);
    }
    boundsGuard.reset();
    return {boundsTag, boundsVal};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateIndexScanWithDynamicBounds(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const IndexScanNode* ixn,
    const sbe::IndexKeysInclusionSet& originalIndexKeyBitset,
    PlanYieldPolicy* yieldPolicy,
    StringMap<const IndexAccessMethod*>* iamMap,
    bool needsCorruptionCheck) {
    const bool forward = ixn->direction == 1;
    auto indexName = ixn->index.identifier.catalogName;
    auto descriptor = collection->getIndexCatalog()->findIndexByName(state.opCtx, indexName);
    tassert(6335101,
            str::stream() << "failed to find index in catalog named: "
                          << ixn->index.identifier.catalogName,
            descriptor);
    auto keyPattern = descriptor->keyPattern();

    // Find the IndexAccessMethod which corresponds to the 'indexName'.
    auto accessMethod = descriptor->getEntry()->accessMethod()->asSortedData();

    // Add the access method corresponding to 'indexName' to the 'iamMap' if a parent stage needs to
    // execute a consistency check.
    if (iamMap) {
        iamMap->insert({indexName, accessMethod});
    }
    PlanStageSlots outputs;
    sbe::value::SlotVector relevantSlots;
    std::unique_ptr<sbe::PlanStage> stage;
    sbe::value::SlotId recordIdSlot;
    ParameterizedIndexScanSlots parameterizedScanSlots;

    // Determine the set of fields from the index required to apply the filter and union those
    // with the set of fields from the index required by the parent stage.
    auto [indexFilterKeyBitset, indexFilterKeyFields] = [&]() {
        if (ixn->filter) {
            DepsTracker tracker;
            match_expression::addDependencies(ixn->filter.get(), &tracker);
            return makeIndexKeyInclusionSet(ixn->index.keyPattern, tracker.fields);
        }
        return std::make_pair(sbe::IndexKeysInclusionSet{}, std::vector<std::string>{});
    }();
    auto indexKeyBitset = originalIndexKeyBitset | indexFilterKeyBitset;
    auto outputIndexKeySlots = state.slotIdGenerator->generateMultiple(indexKeyBitset.count());

    // Whenever possible we should prefer building simplified single interval index scan plans in
    // order to get the best performance.
    if (canGenerateSingleIntervalIndexScan(ixn->iets)) {
        auto makeSlot = [&](const bool cond,
                            const StringData slotKey) -> boost::optional<sbe::value::SlotId> {
            if (!cond)
                return boost::none;

            const auto slot = state.slotId();
            outputs.set(slotKey, slot);
            relevantSlots.push_back(slot);
            return slot;
        };

        boost::optional<std::pair<sbe::value::SlotId, sbe::value::SlotId>> indexScanBoundsSlots;
        std::tie(recordIdSlot, stage, indexScanBoundsSlots) = generateSingleIntervalIndexScan(
            state,
            collection,
            indexName,
            keyPattern,
            forward,
            nullptr,
            nullptr,
            indexKeyBitset,
            outputIndexKeySlots,
            makeSlot(iamMap, PlanStageSlots::kSnapshotId),
            makeSlot(iamMap, PlanStageSlots::kIndexId),
            makeSlot(iamMap, PlanStageSlots::kIndexKey),
            makeSlot(needsCorruptionCheck, PlanStageSlots::kIndexKeyPattern),
            yieldPolicy,
            ixn->nodeId());
        relevantSlots.push_back(recordIdSlot);
        outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
        tassert(6484702,
                "lowKey and highKey runtime environment slots must be present",
                indexScanBoundsSlots);
        parameterizedScanSlots = {ParameterizedIndexScanSlots::SingleIntervalPlan{
            indexScanBoundsSlots->first, indexScanBoundsSlots->second}};
    } else {
        auto genericIndexKeySlots = state.slotIdGenerator->generateMultiple(indexKeyBitset.count());
        auto optimizedIndexKeySlots =
            state.slotIdGenerator->generateMultiple(indexKeyBitset.count());
        auto genericIndexScanSlots = genericIndexKeySlots;
        auto optimizedIndexScanSlots = optimizedIndexKeySlots;
        auto branchOutputSlots = outputIndexKeySlots;

        auto makeSlotsForThenElseBranches =
            [&](const bool cond,
                const StringData slotKey) -> std::tuple<boost::optional<sbe::value::SlotId>,
                                                        boost::optional<sbe::value::SlotId>> {
            if (!cond)
                return {boost::none, boost::none};

            const auto genericSlot = state.slotId();
            const auto optimizedSlot = state.slotId();
            const auto outputSlot = state.slotId();
            outputs.set(slotKey, outputSlot);
            genericIndexScanSlots.push_back(genericSlot);
            optimizedIndexScanSlots.push_back(optimizedSlot);
            branchOutputSlots.push_back(outputSlot);
            relevantSlots.push_back(outputSlot);
            return {genericSlot, optimizedSlot};
        };

        auto [genericIndexScanSnapshotIdSlot, optimizedIndexScanSnapshotIdSlot] =
            makeSlotsForThenElseBranches(iamMap, PlanStageSlots::kSnapshotId);
        auto [genericIndexScanIndexIdSlot, optimizedIndexScanIndexIdSlot] =
            makeSlotsForThenElseBranches(iamMap, PlanStageSlots::kIndexId);
        auto [genericIndexScanIndexKeySlot, optimizedIndexScanIndexKeySlot] =
            makeSlotsForThenElseBranches(iamMap, PlanStageSlots::kIndexKey);

        // Generate a slot for an index key pattern if a parent stage needs to execute a
        // corruption check.
        auto [genericIndexKeyPatternSlot, optimizedIndexKeyPatternSlot] =
            makeSlotsForThenElseBranches(needsCorruptionCheck, PlanStageSlots::kIndexKeyPattern);

        // Generate a generic index scan for multi-interval index bounds.
        auto [genericIndexScanRecordIdSlot,
              genericIndexScanPlanStage,
              genericIndexScanBoundsSlots] =
            generateGenericMultiIntervalIndexScan(
                state,
                collection,
                ixn,
                accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                accessMethod->getSortedDataInterface()->getOrdering(),
                indexKeyBitset,
                genericIndexKeySlots,
                genericIndexScanSnapshotIdSlot,
                genericIndexScanIndexIdSlot,
                genericIndexScanIndexKeySlot,
                genericIndexKeyPatternSlot,
                state.slotIdGenerator,
                state.spoolIdGenerator,
                yieldPolicy);
        tassert(6335203, "bounds slots for index scan are undefined", genericIndexScanBoundsSlots);
        genericIndexScanSlots.push_back(genericIndexScanRecordIdSlot);

        // If we were able to decompose multi-interval index bounds into a number of
        // single-interval bounds, we can also built an optimized sub-tree to perform an index
        // scan.
        auto [optimizedIndexScanRecordIdSlot,
              optimizedIndexScanPlanStage,
              optimizedIndexScanBoundsSlot] =
            generateOptimizedMultiIntervalIndexScan(state,
                                                    collection,
                                                    indexName,
                                                    keyPattern,
                                                    forward,
                                                    boost::none,
                                                    indexKeyBitset,
                                                    optimizedIndexKeySlots,
                                                    optimizedIndexScanSnapshotIdSlot,
                                                    optimizedIndexScanIndexIdSlot,
                                                    optimizedIndexScanIndexKeySlot,
                                                    optimizedIndexKeyPatternSlot,
                                                    yieldPolicy,
                                                    ixn->nodeId());
        tassert(6335204, "bounds slot for index scan is undefined", optimizedIndexScanBoundsSlot);
        optimizedIndexScanSlots.push_back(optimizedIndexScanRecordIdSlot);

        // Generate a branch stage that will either execute an optimized or a generic index scan
        // based on the condition in the slot 'isGenericScanSlot'.
        auto isGenericScanSlot = state.data->env->registerSlot(
            sbe::value::TypeTags::Nothing, 0, true /* owned */, state.slotIdGenerator);
        auto isGenericScanCondition = makeVariable(isGenericScanSlot);
        recordIdSlot = state.slotId();
        relevantSlots.push_back(recordIdSlot);
        branchOutputSlots.push_back(recordIdSlot);
        outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
        stage = sbe::makeS<sbe::BranchStage>(std::move(genericIndexScanPlanStage),
                                             std::move(optimizedIndexScanPlanStage),
                                             std::move(isGenericScanCondition),
                                             genericIndexScanSlots,
                                             optimizedIndexScanSlots,
                                             branchOutputSlots,
                                             ixn->nodeId());

        parameterizedScanSlots = {
            ParameterizedIndexScanSlots::GenericPlan{isGenericScanSlot,
                                                     genericIndexScanBoundsSlots->first,
                                                     genericIndexScanBoundsSlots->second,
                                                     *optimizedIndexScanBoundsSlot}};
    }

    if (ixn->shouldDedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(PlanStageSlots::kRecordId)), ixn->nodeId());
    }

    if (ixn->filter) {
        // We only need to pass those index key slots to the filter generator which correspond
        // to the fields of the index key pattern that are depended on to compute the predicate.
        auto indexFilterKeySlots = makeIndexKeyOutputSlotsMatchingParentReqs(
            ixn->index.keyPattern, indexFilterKeyBitset, indexKeyBitset, outputIndexKeySlots);

        // Relevant slots must include slots for all index keys in case they are needed by parent
        // stages (for instance, covered shard filter).
        relevantSlots.insert(
            relevantSlots.end(), outputIndexKeySlots.begin(), outputIndexKeySlots.end());

        auto outputStage = generateIndexFilter(state,
                                               ixn->filter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               std::move(indexFilterKeySlots),
                                               std::move(indexFilterKeyFields),
                                               ixn->nodeId());
        stage = outputStage.extractStage(ixn->nodeId());
    }

    outputs.setIndexKeySlots(makeIndexKeyOutputSlotsMatchingParentReqs(
        ixn->index.keyPattern, originalIndexKeyBitset, indexKeyBitset, outputIndexKeySlots));

    state.data->indexBoundsEvaluationInfos.emplace_back(
        IndexBoundsEvaluationInfo{ixn->index,
                                  accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                                  accessMethod->getSortedDataInterface()->getOrdering(),
                                  ixn->direction,
                                  std::move(ixn->iets),
                                  std::move(parameterizedScanSlots)});

    return {std::move(stage), std::move(outputs)};
}
}  // namespace mongo::stage_builder
