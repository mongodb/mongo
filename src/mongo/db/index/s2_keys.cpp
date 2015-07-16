/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "s2_keys.h"

#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_indexing_params.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/index_bounds_builder.h"

namespace mongo {

using mongo::Interval;

long long S2CellIdToIndexKey(const S2CellId& cellId) {
    // The range of an unsigned long long is
    // |-----------------|------------------|
    // 0                2^32               2^64 - 1
    // 000...           100...             111...
    // The range of a signed long long is
    // |-----------------|------------------|
    // -2^63             0                 2^63 - 1
    // 100...           000...             011...
    // S2 gives us an unsigned long long, and we need
    // to use signed long longs for the index.
    //
    // The relative ordering may be changed with unsigned
    // numbers around 2^32 being converted to signed
    //
    // However, because a single cell cannot span over
    // more than once face, individual intervals will
    // never cross that threshold. Thus, scans will still
    // produce the same results.
    return static_cast<long long>(cellId.id());
}

S2CellId S2CellIdFromIndexKey(long long indexKey) {
    S2CellId cellId(static_cast<unsigned long long>(indexKey));
    return cellId;
}

void S2CellIdToIndexRange(const S2CellId& cellId, long long* start, long long* end) {
    *start = S2CellIdToIndexKey(cellId.range_min());
    *end = S2CellIdToIndexKey(cellId.range_max());
    invariant(*start <= *end);
}

namespace {
bool compareIntervals(const Interval& a, const Interval& b) {
    return a.precedes(b);
}

void S2CellIdsToIntervalsUnsorted(const std::vector<S2CellId>& intervalSet,
                                  const S2IndexingParams& indexParams,
                                  OrderedIntervalList* oilOut) {
    for (const S2CellId& interval : intervalSet) {
        BSONObjBuilder b;
        if (indexParams.indexVersion < S2_INDEX_VERSION_3) {
            // for backwards compatibility, use strings
            std::string start = interval.toString();
            std::string end = start;
            end[start.size() - 1]++;
            b.append("start", start);
            b.append("end", end);
            oilOut->intervals.push_back(
                IndexBoundsBuilder::makeRangeInterval(b.obj(), true, false));
        } else {
            long long start, end;
            S2CellIdToIndexRange(interval, &start, &end);
            b.append("start", start);
            b.append("end", end);
            // note: numeric form of S2CellId is end inclusive
            oilOut->intervals.push_back(IndexBoundsBuilder::makeRangeInterval(b.obj(), true, true));
        }
    }
}
}

void S2CellIdsToIntervals(const std::vector<S2CellId>& intervalSet,
                          const S2IndexingParams& indexParams,
                          OrderedIntervalList* oilOut) {
    // Order is not preserved in changing from numeric to string
    // form of index key. Therefore, sorting is deferred to after
    // intervals are made
    S2CellIdsToIntervalsUnsorted(intervalSet, indexParams, oilOut);
    std::sort(oilOut->intervals.begin(), oilOut->intervals.end(), compareIntervals);
    // Make sure that our intervals don't overlap each other and are ordered correctly.
    // This perhaps should only be done in debug mode.
    if (!oilOut->isValidFor(1)) {
        cout << "check your assumptions! OIL = " << oilOut->toString() << std::endl;
        verify(0);
    }
}

void S2CellIdsToIntervalsWithParents(const std::vector<S2CellId>& intervalSet,
                                     const S2IndexingParams& indexParams,
                                     OrderedIntervalList* oilOut) {
    // There may be duplicates when going up parent cells if two cells share a parent
    std::unordered_set<S2CellId> exactSet;
    for (const S2CellId& interval : intervalSet) {
        S2CellId coveredCell = interval;
        // Look at the cells that cover us.  We want to look at every cell that contains the
        // covering we would index on if we were to insert the query geometry.  We generate
        // the would-index-with-this-covering and find all the cells strictly containing the
        // cells in that set, until we hit the coarsest indexed cell.  We use equality, not
        // a prefix match.  Why not prefix?  Because we've already looked at everything
        // finer or as fine as our initial covering.
        //
        // Say we have a fine point with cell id 212121, we go up one, get 21212, we don't
        // want to look at cells 21212[not-1] because we know they're not going to intersect
        // with 212121, but entries inserted with cell value 21212 (no trailing digits) may.
        // And we've already looked at points with the cell id 211111 from the regex search
        // created above, so we only want things where the value of the last digit is not
        // stored (and therefore could be 1).

        while (coveredCell.level() > indexParams.coarsestIndexedLevel) {
            // Add the parent cell of the currently covered cell since we aren't at the
            // coarsest level yet
            // NOTE: Be careful not to generate cells strictly less than the
            // coarsestIndexedLevel - this can result in S2 failures when level < 0.

            coveredCell = coveredCell.parent();
            exactSet.insert(coveredCell);
        }
    }
    for (const S2CellId& exact : exactSet) {
        BSONObjBuilder b;
        if (indexParams.indexVersion < S2_INDEX_VERSION_3) {
            // for backwards compatibility, use strings
            b.append("", exact.toString());
        } else {
            b.append("", S2CellIdToIndexKey(exact));
        }
        oilOut->intervals.push_back(IndexBoundsBuilder::makePointInterval(b.obj()));
    }

    S2CellIdsToIntervalsUnsorted(intervalSet, indexParams, oilOut);
    std::sort(oilOut->intervals.begin(), oilOut->intervals.end(), compareIntervals);
    // Make sure that our intervals don't overlap each other and are ordered correctly.
    // This perhaps should only be done in debug mode.
    if (!oilOut->isValidFor(1)) {
        cout << "check your assumptions! OIL = " << oilOut->toString() << std::endl;
        verify(0);
    }
}
}
