/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
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

#pragma once

#include <boost/scoped_ptr.hpp>
#include <cstdlib>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    struct SpecificStats;

    // Every stage has CommonStats.
    struct CommonStats {
        CommonStats() : works(0),
                        yields(0),
                        unyields(0),
                        invalidates(0),
                        advanced(0),
                        needTime(0),
                        needFetch(0),
                        isEOF(false) { }

        // Count calls into the stage.
        uint64_t works;
        uint64_t yields;
        uint64_t unyields;
        uint64_t invalidates;

        // How many times was this state the return value of work(...)?
        uint64_t advanced;
        uint64_t needTime;
        uint64_t needFetch;

        // TODO: have some way of tracking WSM sizes (or really any series of #s).  We can measure
        // the size of our inputs and the size of our outputs.  We can do a lot with the WS here.

        // TODO: once we've picked a plan, collect different (or additional) stats for display to
        // the user, eg. time_t totalTimeSpent;

        // TODO: keep track of total yield time / fetch time for a plan (done by runner)

        bool isEOF;
    };

    // The universal container for a stage's stats.
    struct PlanStageStats {
        PlanStageStats(const CommonStats& c, StageType t) : stageType(t), common(c) { }

        ~PlanStageStats() {
            for (size_t i = 0; i < children.size(); ++i) {
                delete children[i];
            }
        }

        // See query/stage_type.h
        StageType stageType;

        // Stats exported by implementing the PlanStage interface.
        CommonStats common;

        // Per-stage place to stash additional information
        boost::scoped_ptr<SpecificStats> specific;

        // The stats of the node's children.
        std::vector<PlanStageStats*> children;

    private:
        MONGO_DISALLOW_COPYING(PlanStageStats);
    };

    /**
     * The interface all specific-to-stage stats provide.
     */
    struct SpecificStats {
        virtual ~SpecificStats() { }
    };

    struct AndHashStats : public SpecificStats {
        AndHashStats() : flaggedButPassed(0),
                         flaggedInProgress(0) { }

        virtual ~AndHashStats() { }

        // Invalidation counters.
        // How many results had the AND fully evaluated but were invalidated?
        uint64_t flaggedButPassed;

        // How many results were mid-AND but got flagged?
        uint64_t flaggedInProgress;

        // How many entries are in the map after each child?
        // child 'i' produced children[i].common.advanced DiskLocs, of which mapAfterChild[i] were
        // intersections.
        std::vector<uint64_t> mapAfterChild;

        // mapAfterChild[mapAfterChild.size() - 1] WSMswere match tested.
        // commonstats.advanced is how many passed.
    };

    struct AndSortedStats : public SpecificStats {
        AndSortedStats() : flagged(0),
                           matchTested(0) { }

        virtual ~AndSortedStats() { }

        // How many results from each child did not pass the AND?
        std::vector<uint64_t> failedAnd;

        // How many results were flagged via invalidation?
        uint64_t flagged;

        // Fails == common.advanced - matchTested
        uint64_t matchTested;
    };

    struct CollectionScanStats : public SpecificStats {
        CollectionScanStats() : docsTested(0) { }

        // How many documents did we check against our filter?
        uint64_t docsTested;
    };

    struct FetchStats : public SpecificStats {
        FetchStats() : alreadyHasObj(0),
                       forcedFetches(0),
                       matchTested(0) { }

        virtual ~FetchStats() { }

        // Have we seen anything that already had an object?
        uint64_t alreadyHasObj;

        // How many fetches weren't in memory?  it's common.needFetch.
        // How many total fetches did we do?  it's common.advanced.
        // So the number of fetches that were in memory are common.advanced - common.needFetch.

        // How many records were we forced to fetch as the result of an invalidation?
        uint64_t forcedFetches;

        // We know how many passed (it's the # of advanced) and therefore how many failed.
        uint64_t matchTested;
    };

    struct IndexScanStats : public SpecificStats {
        IndexScanStats() : isMultiKey(false),
                           yieldMovedCursor(0),
                           dupsTested(0),
                           dupsDropped(0),
                           seenInvalidated(0),
                           matchTested(0),
                           keysExamined(0) { }

        virtual ~IndexScanStats() { }

        // Index type being used.
        std::string indexType;

        // name of the index being used
        std::string indexName;

        BSONObj keyPattern;

        // A BSON (opaque, ie. hands off other than toString() it) representation of the bounds
        // used.
        BSONObj indexBounds;

        // >1 if we're traversing the index along with its order. <1 if we're traversing it
        // against the order.
        int direction;

        // Whether this index is over a field that contain array values.
        bool isMultiKey;

        uint64_t yieldMovedCursor;
        uint64_t dupsTested;
        uint64_t dupsDropped;

        uint64_t seenInvalidated;
        // TODO: we could track key sizes here.

        // We know how many passed (it's the # of advanced) and therefore how many failed.
        uint64_t matchTested;

        // Number of entries retrieved from the index during the scan.
        uint64_t keysExamined;

    };

    struct OrStats : public SpecificStats {
        OrStats() : dupsTested(0),
                    dupsDropped(0),
                    locsForgotten(0) { }

        virtual ~OrStats() { }

        uint64_t dupsTested;
        uint64_t dupsDropped;

        // How many calls to invalidate(...) actually removed a DiskLoc from our deduping map?
        uint64_t locsForgotten;

        // We know how many passed (it's the # of advanced) and therefore how many failed.
        std::vector<uint64_t> matchTested;
    };

    struct SortStats : public SpecificStats {
        SortStats() : forcedFetches(0) { }

        virtual ~SortStats() { }

        // How many records were we forced to fetch as the result of an invalidation?
        uint64_t forcedFetches;
    };

    struct MergeSortStats : public SpecificStats {
        MergeSortStats() : dupsTested(0),
                           dupsDropped(0),
                           forcedFetches(0) { }

        virtual ~MergeSortStats() { }

        uint64_t dupsTested;
        uint64_t dupsDropped;

        // How many records were we forced to fetch as the result of an invalidation?
        uint64_t forcedFetches;
    };

    struct ShardingFilterStats : public SpecificStats {
        ShardingFilterStats() : chunkSkips(0) { }

        uint64_t chunkSkips;
    };

    struct TwoDNearStats : public SpecificStats {
        TwoDNearStats() : objectsLoaded(0), nscanned(0) { }

        uint64_t objectsLoaded;

        // Since 2d's near does all its work in one go we can't divine the real nscanned from
        // anything else.
        uint64_t nscanned;
    };

    struct TextStats : public SpecificStats {
        TextStats() : keysExamined(0), fetches(0) { }

        uint64_t keysExamined;

        uint64_t fetches;
    };

}  // namespace mongo
