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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_index_scan.h"

#include "mongo/db/catalog/collection.h"
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
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

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
 * TODO SERVER-48473: add a query knob which sets the limit on the number of statically generated
 * intervals.
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
            }
        }
    }

    // The 'keysQueue' contains all generated pairs of low/high keys.
    return {keysQueue.begin(), keysQueue.end()};
}

/**
 * Constructs low/high key values from the given index 'bounds if they can be represented either as
 * a single interval between the low and high keys, or multiple single intervals. If index bounds
 * for some interval cannot be expressed as valid low/high keys, then an empty vector is returned.
 */
std::vector<std::pair<std::unique_ptr<KeyString::Value>, std::unique_ptr<KeyString::Value>>>
makeIntervalsFromIndexBounds(const IndexBounds& bounds,
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
        47429005, 5, "Number of generated interval(s) for ixscan", "num"_attr = intervals.size());
    std::vector<std::pair<std::unique_ptr<KeyString::Value>, std::unique_ptr<KeyString::Value>>>
        result;
    for (auto&& [lowKey, highKey] : intervals) {
        LOGV2_DEBUG(47429006,
                    5,
                    "Generated interval [lowKey, highKey]",
                    "lowKey"_attr = lowKey,
                    "highKey"_attr = highKey);
        // For high keys use the opposite rule as a normal seek because a forward scan should end
        // after the key if inclusive, and before if exclusive.
        const auto inclusive = forward != highKeyInclusive;
        result.push_back({std::make_unique<KeyString::Value>(
                              IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                                  lowKey, version, ordering, forward, lowKeyInclusive)),
                          std::make_unique<KeyString::Value>(
                              IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                                  highKey, version, ordering, forward, inclusive))});
    }
    return result;
}

/**
 * Constructs an optimized version of an index scan for multi-interval index bounds for the case
 * when the bounds can be decomposed in a number of single-interval bounds. In this case, instead
 * of building a generic index scan to navigate through the index using the 'IndexBoundsChecker',
 * we will construct a subtree with a constant table scan containing all intervals we'd want to
 * scan through. Specifically, we will build the following subtree:
 *
 *         nlj [] [lowKeySlot, highKeySlot]
 *              left
 *                  project [lowKeySlot = getField (unwindSlot, "l"),
 *                           highKeySlot = getField (unwindSlot, "h")]
 *                  unwind unwindSlot indexSlot boundsSlot false
 *                  project [boundsSlot = [{"l" : KS(...), "h" : KS(...)},
 *                                         {"l" : KS(...), "h" : KS(...)}, ...]]
 *                  limit 1
 *                  coscan
 *               right
 *                  ixseek lowKeySlot highKeySlot recordIdSlot [] @coll @index
 *
 * This subtree is similar to the single-interval subtree with the only difference that instead
 * of projecting a single pair of the low/high keys, we project an array of such pairs and then
 * use the unwind stage to flatten the array and generate multiple input intervals to the ixscan.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
generateOptimizedMultiIntervalIndexScan(
    const Collection* collection,
    const std::string& indexName,
    bool forward,
    std::vector<std::pair<std::unique_ptr<KeyString::Value>, std::unique_ptr<KeyString::Value>>>
        intervals,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    TrialRunProgressTracker* tracker) {
    using namespace std::literals;

    auto recordIdSlot = slotIdGenerator->generate();
    auto lowKeySlot = slotIdGenerator->generate();
    auto highKeySlot = slotIdGenerator->generate();

    // Construct an array containing objects with the low and high keys for each interval. E.g.,
    //    [ {l: KS(...), h: KS(...)},
    //      {l: KS(...), h: KS(...)}, ... ]
    auto [boundsTag, boundsVal] = sbe::value::makeNewArray();
    auto arr = sbe::value::getArrayView(boundsVal);
    for (auto&& [lowKey, highKey] : intervals) {
        auto [tag, val] = sbe::value::makeNewObject();
        auto obj = sbe::value::getObjectView(val);
        obj->push_back(
            "l"sv, sbe::value::TypeTags::ksValue, sbe::value::bitcastFrom(lowKey.release()));
        obj->push_back(
            "h"sv, sbe::value::TypeTags::ksValue, sbe::value::bitcastFrom(highKey.release()));
        arr->push_back(tag, val);
    }

    auto boundsSlot = slotIdGenerator->generate();
    auto unwindSlot = slotIdGenerator->generate();

    // Project out the constructed array as a constant value and add an unwind stage on top to
    // flatten the array.
    auto unwind = sbe::makeS<sbe::UnwindStage>(
        sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
            boundsSlot,
            sbe::makeE<sbe::EConstant>(boundsTag, boundsVal)),
        boundsSlot,
        unwindSlot,
        slotIdGenerator->generate(), /* We don't need an index slot but must to provide it. */
        false /* Preserve null and empty arrays, in our case it cannot be empty anyway. */);

    // Add another project stage to extract low and high keys from each value produced by unwind and
    // bind the keys to the 'lowKeySlot' and 'highKeySlot'.
    auto project = sbe::makeProjectStage(
        std::move(unwind),
        lowKeySlot,
        sbe::makeE<sbe::EFunction>(
            "getField"sv,
            sbe::makeEs(sbe::makeE<sbe::EVariable>(unwindSlot), sbe::makeE<sbe::EConstant>("l"sv))),
        highKeySlot,
        sbe::makeE<sbe::EFunction>("getField"sv,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(unwindSlot),
                                               sbe::makeE<sbe::EConstant>("h"sv))));

    auto ixscan = sbe::makeS<sbe::IndexScanStage>(
        NamespaceStringOrUUID{collection->ns().db().toString(), collection->uuid()},
        indexName,
        forward,
        boost::none,
        recordIdSlot,
        std::vector<std::string>{},
        sbe::makeSV(),
        lowKeySlot,
        highKeySlot,
        yieldPolicy,
        tracker);

    // Finally, get the keys from the outer side and feed them to the inner side (ixscan).
    return {recordIdSlot,
            sbe::makeS<sbe::LoopJoinStage>(std::move(project),
                                           std::move(ixscan),
                                           sbe::makeSV(),
                                           sbe::makeSV(lowKeySlot, highKeySlot),
                                           nullptr)};
}

/**
 * Builds an anchor sub-tree of the recusrive index scan CTE to seed the result set with the initial
 * 'startKey' for the index scan.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> makeAnchorBranchForGenericIndexScan(
    std::unique_ptr<KeyString::Value> startKey, sbe::value::SlotIdGenerator* slotIdGenerator) {
    // Just project out the 'startKey'.
    auto startKeySlot = slotIdGenerator->generate();
    return {startKeySlot,
            sbe::makeProjectStage(
                sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
                startKeySlot,
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::ksValue,
                                           sbe::value::bitcastFrom(startKey.release())))};
}

/**
 * Builds a recursive sub-tree of the recursive CTE to generate the reminder of the result set
 * consisting of valid recordId's and index seek keys to restart the index scan from.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
makeRecursiveBranchForGenericIndexScan(const Collection* collection,
                                       const std::string& indexName,
                                       const sbe::CheckBoundsParams& params,
                                       sbe::SpoolId spoolId,
                                       sbe::value::SlotIdGenerator* slotIdGenerator,
                                       PlanYieldPolicy* yieldPolicy,
                                       TrialRunProgressTracker* tracker) {

    auto resultSlot = slotIdGenerator->generate();
    auto recordIdSlot = slotIdGenerator->generate();
    auto seekKeySlot = slotIdGenerator->generate();
    auto lowKeySlot = slotIdGenerator->generate();

    // Build a standard index scan nested loop join with the outer branch producing a low key
    // to be fed into the index scan. The low key is taken from the 'seekKeySlot' which would
    // contain a value from the stack spool. See below for details.
    auto project = sbe::makeProjectStage(
        sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
        lowKeySlot,
        sbe::makeE<sbe::EVariable>(seekKeySlot));

    auto ixscan = sbe::makeS<sbe::IndexScanStage>(
        NamespaceStringOrUUID{collection->ns().db().toString(), collection->uuid()},
        indexName,
        params.direction == 1,
        resultSlot,
        recordIdSlot,
        std::vector<std::string>{},
        sbe::makeSV(),
        lowKeySlot,
        boost::none,
        yieldPolicy,
        tracker);

    // Get the low key from the outer side and feed it to the inner side (ixscan).
    auto nlj = sbe::makeS<sbe::LoopJoinStage>(
        std::move(project), std::move(ixscan), sbe::makeSV(), sbe::makeSV(lowKeySlot), nullptr);

    // Inject another nested loop join with the outer branch being a stack spool, and the inner an
    // index scan nljoin which just constructed above. The stack spool is populated from the values
    // generated by the index scan above, and passed through the check bounds stage, which would
    // produce either a valid recordId to be consumed by the stage sitting above the index scan
    // sub-tree, or a seek key to restart the index scan from. The spool will only store the seek
    // keys, passing through valid recordId's.
    auto checkBoundsSlot = slotIdGenerator->generate();
    return {checkBoundsSlot,
            sbe::makeS<sbe::LoopJoinStage>(
                sbe::makeS<sbe::SpoolConsumerStage<true>>(spoolId, sbe::makeSV(seekKeySlot)),
                sbe::makeS<sbe::CheckBoundsStage>(
                    std::move(nlj), params, resultSlot, recordIdSlot, checkBoundsSlot),
                sbe::makeSV(),
                sbe::makeSV(seekKeySlot),
                nullptr)};
}

/**
 * Builds a generic multi-interval index scan for the cases when index bounds cannot be represented
 * as valid low/high keys. In this case we will build a recursive sub-tree and will use the
 * 'CheckBoundsStage' to navigate through the index. The recursive sub-tree is built using a union
 * stage in conjunction with the stack spool:
 *
 *         filter {isNumber(resultSlot)}
 *         lspool [resultSlot] {!isNumber(resultSlot)}
 *         union [resultSlot]
 *            [anchorSlot]
 *                project [startKeySlot = KS(...)]
 *                limit 1
 *                coscan
 *            [checkBoundsSlot]
 *                 nlj [] [seekKeySlot]
 *                     left
 *                         sspool [seekKeySlot]
 *                     right
 *                         chkbounds resultSlot recordIdSlot checkBoundsSlot
 *                         nlj [] [lowKeySlot]
 *                             left
 *                                 project [lowKeySlot = seekKeySlot]
 *                                 limit 1
 *                                 coscan
 *                             right
 *                                 ixseek lowKeySlot resultSlot recordIdSlot [] @coll @index
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
 *        3. The inner branch execution starts with the projection of the seek key, which is
 *           fed into the ixscan as a 'lowKeySlot'.
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
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
generateGenericMultiIntervalIndexScan(const Collection* collection,
                                      const IndexScanNode* ixn,
                                      KeyString::Version version,
                                      Ordering ordering,
                                      sbe::value::SlotIdGenerator* slotIdGenerator,
                                      sbe::value::SpoolIdGenerator* spoolIdGenerator,
                                      PlanYieldPolicy* yieldPolicy,
                                      TrialRunProgressTracker* tracker) {

    using namespace std::literals;

    auto resultSlot = slotIdGenerator->generate();

    IndexBoundsChecker checker{&ixn->bounds, ixn->index.keyPattern, ixn->direction};
    IndexSeekPoint seekPoint;

    // Get the start seek key for our recursive scan. If there are no possible index entries that
    // match the bounds and we cannot generate a start seek key, inject an EOF sub-tree an exit
    // straight away - this index scan won't emit any results.
    if (!checker.getStartSeekPoint(&seekPoint)) {
        return {resultSlot,
                sbe::makeS<sbe::MakeObjStage>(
                    sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 0, boost::none),
                    resultSlot,
                    boost::none,
                    std::vector<std::string>{},
                    std::vector<std::string>{},
                    sbe::makeSV(),
                    true,
                    false)};
    }

    // Build the anchor branch of the union.
    auto [anchorSlot, anchorBranch] = makeAnchorBranchForGenericIndexScan(
        std::make_unique<KeyString::Value>(IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
            seekPoint, version, ordering, ixn->direction == 1)),
        slotIdGenerator);

    auto spoolId = spoolIdGenerator->generate();

    // Build the recursive branch of the union.
    auto [recursiveSlot, recursiveBranch] = makeRecursiveBranchForGenericIndexScan(
        collection,
        ixn->index.identifier.catalogName,
        {ixn->bounds, ixn->index.keyPattern, ixn->direction, version, ordering},
        spoolId,
        slotIdGenerator,
        yieldPolicy,
        tracker);

    // Construct a union stage from the two branches.
    auto unionStage = sbe::makeS<sbe::UnionStage>(
        make_vector<std::unique_ptr<sbe::PlanStage>>(std::move(anchorBranch),
                                                     std::move(recursiveBranch)),
        std::vector<sbe::value::SlotVector>{sbe::makeSV(anchorSlot), sbe::makeSV(recursiveSlot)},
        sbe::makeSV(resultSlot));

    // Stick in a lazy producer spool on top. The specified predicate will ensure that we will only
    // store the seek key values in the spool (that is, if the value type is not a number, or not
    // a recordId).
    auto spool = sbe::makeS<sbe::SpoolLazyProducerStage>(
        std::move(unionStage),
        spoolId,
        sbe::makeSV(resultSlot),
        sbe::makeE<sbe::EPrimUnary>(
            sbe::EPrimUnary::logicNot,
            sbe::makeE<sbe::EFunction>("isNumber"sv,
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(resultSlot)))));

    // Finally, add a filter stage on top to filter out seek keys and return only recordIds.
    return {resultSlot,
            sbe::makeS<sbe::FilterStage<false>>(
                std::move(spool),
                sbe::makeE<sbe::EFunction>("isNumber"sv,
                                           sbe::makeEs(sbe::makeE<sbe::EVariable>(resultSlot))))};
}
}  // namespace

std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateSingleIntervalIndexScan(
    const Collection* collection,
    const std::string& indexName,
    bool forward,
    std::unique_ptr<KeyString::Value> lowKey,
    std::unique_ptr<KeyString::Value> highKey,
    boost::optional<sbe::value::SlotId> recordSlot,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    TrialRunProgressTracker* tracker) {
    auto recordIdSlot = slotIdGenerator->generate();
    auto lowKeySlot = slotIdGenerator->generate();
    auto highKeySlot = slotIdGenerator->generate();

    // Construct a constant table scan to deliver a single row with two fields 'lowKeySlot' and
    // 'highKeySlot', representing seek boundaries, into the index scan.
    auto project = sbe::makeProjectStage(
        sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
        lowKeySlot,
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::ksValue,
                                   sbe::value::bitcastFrom(lowKey.release())),
        highKeySlot,
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::ksValue,
                                   sbe::value::bitcastFrom(highKey.release())));

    // Scan the index in the range {'lowKeySlot', 'highKeySlot'} (subject to inclusive or
    // exclusive boundaries), and produce a single field recordIdSlot that can be used to
    // position into the collection.
    auto ixscan = sbe::makeS<sbe::IndexScanStage>(
        NamespaceStringOrUUID{collection->ns().db().toString(), collection->uuid()},
        indexName,
        forward,
        recordSlot,
        recordIdSlot,
        std::vector<std::string>{},
        sbe::makeSV(),
        lowKeySlot,
        highKeySlot,
        yieldPolicy,
        tracker);

    // Finally, get the keys from the outer side and feed them to the inner side.
    return {recordIdSlot,
            sbe::makeS<sbe::LoopJoinStage>(std::move(project),
                                           std::move(ixscan),
                                           sbe::makeSV(),
                                           sbe::makeSV(lowKeySlot, highKeySlot),
                                           nullptr)};
}


std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateIndexScan(
    OperationContext* opCtx,
    const Collection* collection,
    const IndexScanNode* ixn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::SpoolIdGenerator* spoolIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    TrialRunProgressTracker* tracker) {
    uassert(
        4822863, "Index scans with key metadata are not supported in SBE", !ixn->addKeyMetadata);
    uassert(4822864, "Index scans with a filter are not supported in SBE", !ixn->filter);

    auto descriptor =
        collection->getIndexCatalog()->findIndexByName(opCtx, ixn->index.identifier.catalogName);
    auto accessMethod = collection->getIndexCatalog()->getEntry(descriptor)->accessMethod();
    auto intervals =
        makeIntervalsFromIndexBounds(ixn->bounds,
                                     ixn->direction == 1,
                                     accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                                     accessMethod->getSortedDataInterface()->getOrdering());

    auto [slot, stage] = [&]() {
        if (intervals.size() == 1) {
            // If we have just a single interval, we can construct a simplified sub-tree.
            auto&& [lowKey, highKey] = intervals[0];
            return generateSingleIntervalIndexScan(collection,
                                                   ixn->index.identifier.catalogName,
                                                   ixn->direction == 1,
                                                   std::move(lowKey),
                                                   std::move(highKey),
                                                   boost::none,
                                                   slotIdGenerator,
                                                   yieldPolicy,
                                                   tracker);
        } else if (intervals.size() > 1) {
            // Or, if we were able to decompose multi-interval index bounds into a number of
            // single-interval bounds, we can also built an optimized sub-tree to perform an index
            // scan.
            return generateOptimizedMultiIntervalIndexScan(collection,
                                                           ixn->index.identifier.catalogName,
                                                           ixn->direction == 1,
                                                           std::move(intervals),
                                                           slotIdGenerator,
                                                           yieldPolicy,
                                                           tracker);
        } else {
            // Otherwise, build a generic index scan for multi-interval index bounds.
            return generateGenericMultiIntervalIndexScan(
                collection,
                ixn,
                accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                accessMethod->getSortedDataInterface()->getOrdering(),
                slotIdGenerator,
                spoolIdGenerator,
                yieldPolicy,
                tracker);
        }
    }();

    if (ixn->shouldDedup) {
        stage = sbe::makeS<sbe::HashAggStage>(std::move(stage), sbe::makeSV(slot), sbe::makeEM());
    }

    return {slot, std::move(stage)};
}
}  // namespace mongo::stage_builder
