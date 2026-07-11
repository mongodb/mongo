// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/record_id_range_list.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <algorithm>
#include <compare>

namespace mongo {

namespace {

// ---------------------------------------------------------------------------
// Helpers for sorting and merging ranges
// ---------------------------------------------------------------------------

/**
 * Returns true if range `a` starts strictly before range `b`.
 * Absent min is treated as -∞ (starts first). When two ranges share the same min
 * bound value, the inclusive one starts "earlier" (it includes the boundary point).
 */
bool rangeStartsBefore(const RecordIdRange& a, const RecordIdRange& b) {
    const auto& aMin = a.getMin();
    const auto& bMin = b.getMin();
    if (!bMin)
        return false;  // b starts at -∞ → a does not start first
    if (!aMin)
        return true;  // a = -∞, b is finite → a starts first
    auto cmp = *aMin <=> *bMin;
    if (std::is_lt(cmp))
        return true;
    if (std::is_gt(cmp))
        return false;
    // Equal bound values: inclusive start is "earlier" ([x,… before (x,…)
    return a.isMinInclusive() && !b.isMinInclusive();
}

/**
 * Returns true if range `a` ends strictly before range `b`.
 * Absent max is treated as +∞ (ends last). When two ranges share the same
 * max bound value, the exclusive one ends "earlier" (e.g. [1,5) ends before [1,5]).
 */
bool rangeEndsBefore(const RecordIdRange& a, const RecordIdRange& b) {
    const auto& aMax = a.getMax();
    const auto& bMax = b.getMax();
    if (!aMax)
        return false;  // a ends at +∞ → can never end before anything
    if (!bMax)
        return true;  // a has finite end, b ends at +∞ → a ends first
    auto cmp = *aMax <=> *bMax;
    if (std::is_lt(cmp))
        return true;
    if (std::is_gt(cmp))
        return false;
    // Equal max values: exclusive end is "earlier"
    return !a.isMaxInclusive() && b.isMaxInclusive();
}

/**
 * Returns true if two sorted, consecutive ranges should be merged (overlap or are adjacent).
 * Assumes r1 starts no later than r2 (i.e. rangeStartsBefore(r2, r1) is false).
 *
 * Adjacency: [1,3) and [3,5] touch at 3 and must be merged → [1,5].
 */
bool shouldMerge(const RecordIdRange& r1, const RecordIdRange& r2) {
    if (!r1.getMax())
        return true;  // r1 extends to +∞, so r2 is always within/touching (see the assumption
                      // above)
    if (!r2.getMin())
        return true;  // r2 starts at -∞ (implies r1 does too after sorting), always overlap

    auto cmp = *r1.getMax() <=> *r2.getMin();
    if (std::is_gt(cmp))
        return true;  // r1.max > r2.min: definite overlap
    if (std::is_lt(cmp))
        return false;  // r1.max < r2.min: gap between ranges
    // r1.max == r2.min: merge if at least one side is inclusive (adjacent/touching)
    return r1.isMaxInclusive() || r2.isMinInclusive();
}

/**
 * Merges two overlapping or adjacent ranges into their union.
 * The result spans from the earlier start to the later end.
 */
RecordIdRange mergeRanges(const RecordIdRange& r1, const RecordIdRange& r2) {
    RecordIdRange result;  // starts as (-∞, +∞)

    // New min = the earlier of r1.min and r2.min (more permissive lower bound).
    if (r1.getMin() && r2.getMin()) {
        auto cmp = *r1.getMin() <=> *r2.getMin();
        if (std::is_lt(cmp)) {
            result.maybeNarrowMin(*r1.getMin(), r1.isMinInclusive());
        } else if (std::is_gt(cmp)) {
            result.maybeNarrowMin(*r2.getMin(), r2.isMinInclusive());
        } else {
            // Same bound value: inclusive if either side is inclusive.
            result.maybeNarrowMin(*r1.getMin(), r1.isMinInclusive() || r2.isMinInclusive());
        }
    }
    // If either min is absent (-∞), leave result's min as absent (-∞).

    // New max = the later of r1.max and r2.max (more permissive upper bound).
    if (r1.getMax() && r2.getMax()) {
        auto cmp = *r1.getMax() <=> *r2.getMax();
        if (std::is_gt(cmp)) {
            result.maybeNarrowMax(*r1.getMax(), r1.isMaxInclusive());
        } else if (std::is_lt(cmp)) {
            result.maybeNarrowMax(*r2.getMax(), r2.isMaxInclusive());
        } else {
            // Same bound value: inclusive if either side is inclusive.
            result.maybeNarrowMax(*r1.getMax(), r1.isMaxInclusive() || r2.isMaxInclusive());
        }
    }
    // If either max is absent (+∞), leave result's max as absent (+∞).

    return result;
}

bool endsBeforeStarts(const RecordIdRange& a, const RecordIdRange& b) {
    if (!a.getMax())
        return false;  // a extends to +∞, cannot end before anything
    if (!b.getMin())
        return false;  // b starts at -∞, a cannot end before that
    auto cmp = *a.getMax() <=> *b.getMin();
    if (std::is_lt(cmp))
        return true;
    if (std::is_gt(cmp))
        return false;
    return !a.isMaxInclusive() || !b.isMinInclusive();
}

}  // namespace

// ---------------------------------------------------------------------------
// RecordIdRangeList member functions
// ---------------------------------------------------------------------------

RecordIdRange RecordIdRangeList::outerBounds() const {
    RecordIdRange outer;
    if (isEmpty()) {
        // Empty list (∅): return an empty range represented as an exclusive point at the
        // default-constructed RecordId, so that any scan using these bounds immediately stops.
        RecordIdBound defaultBound;
        outer.intersectRange(defaultBound, defaultBound, false, false);
    } else {
        const auto& first = _ranges.front();
        const auto& last = _ranges.back();
        outer.intersectRange(
            first.getMin(), last.getMax(), first.isMinInclusive(), last.isMaxInclusive());
    }
    return outer;
}

BSONArray RecordIdRangeList::toBSONArray() const {
    BSONArrayBuilder arr;
    for (const auto& range : _ranges) {
        BSONObjBuilder obj;
        if (auto& min = range.getMin()) {
            min->appendToBSONAs(&obj, "min");
            obj.appendBool("minInclusive", range.isMinInclusive());
        }
        if (auto& max = range.getMax()) {
            max->appendToBSONAs(&obj, "max");
            obj.appendBool("maxInclusive", range.isMaxInclusive());
        }
        arr.append(obj.obj());
    }
    return arr.arr();
}

RecordIdRangeList::SeekResult RecordIdRangeList::seek(const RecordId& rid,
                                                      size_t startIdx,
                                                      bool forward) const {
    const int exhausted = forward ? 1 : -1;
    auto search = [&](auto begin, auto end) -> boost::optional<size_t> {
        auto recordRange =
            std::lower_bound(forward ? begin + startIdx : begin + (_ranges.size() - 1 - startIdx),
                             end,
                             rid,
                             [&](const RecordIdRange& range, const RecordId& rid) {
                                 return range.compare(rid) == exhausted;
                             });

        if (recordRange == end) {
            return boost::none;
        }

        if (forward) {
            return recordRange - begin;
        } else {
            return _ranges.size() - 1 - (recordRange - begin);
        }
    };

    auto idx =
        forward ? search(_ranges.begin(), _ranges.end()) : search(_ranges.rbegin(), _ranges.rend());

    if (!idx) {
        return SeekBeyondAllRanges{};
    }

    if (_ranges[*idx].compare(rid) == 0) {
        return SeekInRange{*idx};
    }
    return SeekBeforeRange{*idx};
}

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

RecordIdRangeList RecordIdRangeList::makeUnion(std::vector<RecordIdRange> ranges) {
    // start with an empty list (∅)
    RecordIdRangeList result{RecordIdRangeList::EMPTY_TAG{}};

    if (ranges.empty()) {
        return result;  // empty list: union of no ranges is the empty set
    }

    // Sort by lower bound (ascending; -∞ first, then by value, inclusive before exclusive).
    std::sort(ranges.begin(), ranges.end(), rangeStartsBefore);

    // Sweep and merge overlapping/adjacent ranges.
    result._ranges.push_back(ranges[0]);
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (shouldMerge(result._ranges.back(), ranges[i])) {
            result._ranges.back() = mergeRanges(result._ranges.back(), ranges[i]);
        } else {
            result._ranges.push_back(std::move(ranges[i]));
        }
    }

    return result;
}

RecordIdRangeList RecordIdRangeList::unite(std::vector<RecordIdRangeList> lists) {
    std::vector<RecordIdRange> all;
    for (auto& list : lists) {
        for (auto& r : list._ranges) {
            all.push_back(std::move(r));
        }
    }

    auto res = makeUnion(std::move(all));
    return res;
}

RecordIdRangeList RecordIdRangeList::intersect(std::vector<RecordIdRangeList> lists) {
    if (lists.empty()) {
        return RecordIdRangeList{};  // identity: intersection of no constraints = all records (∀)
    }
    // The intersection of a single range list is itself.
    if (lists.size() == 1) {
        return std::move(lists[0]);
    }

    // Start from an empty range list (no ranges, includes no RecordId).
    RecordIdRangeList result{RecordIdRangeList::EMPTY_TAG{}};

    // Any empty list (∅) makes the whole intersection empty.
    for (size_t k = 0; k < lists.size(); ++k) {
        if (lists[k].isEmpty()) {
            return result;
        }
    }

    // A pair where the first element is the index of the RecordIdRangeList
    // and the second element is the index of the RecordIdRange inside.
    using ListRangeIdx = std::pair<size_t, size_t>;

    // One range per list, same pair format.
    // Sorted in rangeEndsBefore order (see heapComp below).
    // This lets us quickly pick the range with the lowest end bound.
    std::vector<ListRangeIdx> heap;
    heap.reserve(lists.size());

    // Helper to retrieve a range given a range list index and a range index into it.
    const auto getRange = [&](ListRangeIdx idxs) -> const RecordIdRange& {
        return lists[std::get<0>(idxs)]._ranges[std::get<1>(idxs)];
    };
    const auto heapComp = [&](const auto& t1, const auto& t2) {
        return rangeEndsBefore(getRange(t2), getRange(t1));
    };

    // Keep track of the range among the ones in the heap with the highest starting bound
    // (determines the lower bounds of the yielded intersections).
    ListRangeIdx lo = std::make_pair(0, 0);

    // Initialize the heap by pushing the first range of each range list.
    // Also initialize `lo` to the range with the highest start among those ranges.
    for (size_t i = 0; i < lists.size(); i++) {
        auto idxs = std::make_pair(i, 0);
        heap.push_back(idxs);
        if (rangeStartsBefore(getRange(lo), getRange(idxs))) {
            lo = idxs;
        }
    }
    std::make_heap(heap.begin(), heap.end(), heapComp);

    // Loop to discover and yield the intersections.
    while (true) {
        // Highest start
        const auto loRange = getRange(lo);

        // Lowest end
        const auto hi = heap.front();
        const auto [hiListIdx, hiRangeIdx] = hi;
        const auto hiRange = getRange(hi);
        if (!endsBeforeStarts(hiRange, loRange)) {
            // All current ranges share the interval [lo.start, hi.end]; emit the intersection.
            RecordIdRange intersection;
            intersection.intersectRange(loRange.getMin(),
                                        hiRange.getMax(),
                                        loRange.isMinInclusive(),
                                        hiRange.isMaxInclusive());
            result._ranges.push_back(intersection);
        }

        // Find the first range from this range list that does not end before `lo` starts.
        auto hiNextRangeIt = std::lower_bound(
            lists[hiListIdx]._ranges.begin() + hiRangeIdx + 1,
            lists[hiListIdx]._ranges.end(),
            loRange,
            [&](const RecordIdRange& a, const RecordIdRange& b) { return endsBeforeStarts(a, b); });
        auto hiNextRangeIdx =
            lists[hiListIdx]._ranges.size() - (lists[hiListIdx]._ranges.end() - hiNextRangeIt);

        // If `hi` has no such range - return. No more intersections are possible.
        if (hiNextRangeIdx == lists[hiListIdx]._ranges.size()) {
            return result;
        }

        // Pop `hi` from the heap & replace it with the next range in its range list.
        std::pop_heap(heap.begin(), heap.end(), heapComp);
        heap.pop_back();

        // Push the next range
        auto newIdxs = std::make_pair(hiListIdx, hiNextRangeIdx);
        heap.push_back(newIdxs);
        std::push_heap(heap.begin(), heap.end(), heapComp);

        // Update `lo` to satisfy its invariant if needed.
        if (rangeStartsBefore(getRange(lo), getRange(newIdxs))) {
            lo = newIdxs;
        }
    }
}

}  // namespace mongo
