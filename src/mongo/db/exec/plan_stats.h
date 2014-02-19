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

    /**
     * The interface all specific-to-stage stats provide.
     */
    struct SpecificStats {
        virtual ~SpecificStats() { }

        /**
         * Make a deep copy.
         */
        virtual SpecificStats* clone() const = 0;
    };

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
        size_t works;
        size_t yields;
        size_t unyields;
        size_t invalidates;

        // How many times was this state the return value of work(...)?
        size_t advanced;
        size_t needTime;
        size_t needFetch;

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

        /**
         * Make a deep copy.
         */
        PlanStageStats* clone() const {
            PlanStageStats* stats = new PlanStageStats(common, stageType);
            if (specific.get()) {
                stats->specific.reset(specific->clone());
            }
            for (size_t i = 0; i < children.size(); ++i) {
                invariant(children[i]);
                stats->children.push_back(children[i]->clone());
            }
            return stats;
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

    struct AndHashStats : public SpecificStats {
        AndHashStats() : flaggedButPassed(0),
                         flaggedInProgress(0) { }

        virtual ~AndHashStats() { }

        virtual SpecificStats* clone() const {
            AndHashStats* specific = new AndHashStats(*this);
            return specific;
        }

        // Invalidation counters.
        // How many results had the AND fully evaluated but were invalidated?
        size_t flaggedButPassed;

        // How many results were mid-AND but got flagged?
        size_t flaggedInProgress;

        // How many entries are in the map after each child?
        // child 'i' produced children[i].common.advanced DiskLocs, of which mapAfterChild[i] were
        // intersections.
        std::vector<size_t> mapAfterChild;

        // mapAfterChild[mapAfterChild.size() - 1] WSMswere match tested.
        // commonstats.advanced is how many passed.
    };

    struct AndSortedStats : public SpecificStats {
        AndSortedStats() : flagged(0),
                           matchTested(0) { }

        virtual ~AndSortedStats() { }

        virtual SpecificStats* clone() const {
            AndSortedStats* specific = new AndSortedStats(*this);
            return specific;
        }

        // How many results from each child did not pass the AND?
        std::vector<size_t> failedAnd;

        // How many results were flagged via invalidation?
        size_t flagged;

        // Fails == common.advanced - matchTested
        size_t matchTested;
    };

    struct CollectionScanStats : public SpecificStats {
        CollectionScanStats() : docsTested(0) { }

        virtual SpecificStats* clone() const {
            CollectionScanStats* specific = new CollectionScanStats(*this);
            return specific;
        }

        // How many documents did we check against our filter?
        size_t docsTested;
    };

    struct DistinctScanStats : public SpecificStats {
        DistinctScanStats() : keysExamined(0) { }

        virtual SpecificStats* clone() const {
            return new DistinctScanStats(*this);
        }

        // How many keys did we look at while distinct-ing?
        size_t keysExamined;
    };

    struct FetchStats : public SpecificStats {
        FetchStats() : alreadyHasObj(0),
                       forcedFetches(0),
                       matchTested(0) { }

        virtual ~FetchStats() { }

        virtual SpecificStats* clone() const {
            FetchStats* specific = new FetchStats(*this);
            return specific;
        }

        // Have we seen anything that already had an object?
        size_t alreadyHasObj;

        // How many fetches weren't in memory?  it's common.needFetch.
        // How many total fetches did we do?  it's common.advanced.
        // So the number of fetches that were in memory are common.advanced - common.needFetch.

        // How many records were we forced to fetch as the result of an invalidation?
        size_t forcedFetches;

        // We know how many passed (it's the # of advanced) and therefore how many failed.
        size_t matchTested;
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

        virtual SpecificStats* clone() const {
            IndexScanStats* specific = new IndexScanStats(*this);
            // BSON objects have to be explicitly copied.
            specific->keyPattern = keyPattern.getOwned();
            specific->indexBounds = indexBounds.getOwned();
            return specific;
        }

        // Index type being used.
        std::string indexType;

        // name of the index being used
        std::string indexName;

        BSONObj keyPattern;

        // A BSON (opaque, ie. hands off other than toString() it) representation of the bounds
        // used.
        BSONObj indexBounds;

        // Contains same information as indexBounds with the addition of inclusivity of bounds.
        std::string indexBoundsVerbose;

        // >1 if we're traversing the index along with its order. <1 if we're traversing it
        // against the order.
        int direction;

        // Whether this index is over a field that contain array values.
        bool isMultiKey;

        size_t yieldMovedCursor;
        size_t dupsTested;
        size_t dupsDropped;

        size_t seenInvalidated;
        // TODO: we could track key sizes here.

        // We know how many passed (it's the # of advanced) and therefore how many failed.
        size_t matchTested;

        // Number of entries retrieved from the index during the scan.
        size_t keysExamined;

    };

    struct OrStats : public SpecificStats {
        OrStats() : dupsTested(0),
                    dupsDropped(0),
                    locsForgotten(0) { }

        virtual ~OrStats() { }

        virtual SpecificStats* clone() const {
            OrStats* specific = new OrStats(*this);
            return specific;
        }

        size_t dupsTested;
        size_t dupsDropped;

        // How many calls to invalidate(...) actually removed a DiskLoc from our deduping map?
        size_t locsForgotten;

        // We know how many passed (it's the # of advanced) and therefore how many failed.
        std::vector<size_t> matchTested;
    };

    struct SortStats : public SpecificStats {
        SortStats() : forcedFetches(0) { }

        virtual ~SortStats() { }

        virtual SpecificStats* clone() const {
            SortStats* specific = new SortStats(*this);
            return specific;
        }

        // How many records were we forced to fetch as the result of an invalidation?
        size_t forcedFetches;
    };

    struct MergeSortStats : public SpecificStats {
        MergeSortStats() : dupsTested(0),
                           dupsDropped(0),
                           forcedFetches(0) { }

        virtual ~MergeSortStats() { }

        virtual SpecificStats* clone() const {
            MergeSortStats* specific = new MergeSortStats(*this);
            return specific;
        }

        size_t dupsTested;
        size_t dupsDropped;

        // How many records were we forced to fetch as the result of an invalidation?
        size_t forcedFetches;
    };

    struct ShardingFilterStats : public SpecificStats {
        ShardingFilterStats() : chunkSkips(0) { }

        virtual SpecificStats* clone() const {
            ShardingFilterStats* specific = new ShardingFilterStats(*this);
            return specific;
        }

        size_t chunkSkips;
    };

    struct TwoDStats : public SpecificStats {
        TwoDStats() { }

        virtual SpecificStats* clone() const {
            TwoDStats* specific = new TwoDStats(*this);
            return specific;
        }

        std::string type;
    };

    struct TwoDNearStats : public SpecificStats {
        TwoDNearStats() : objectsLoaded(0), nscanned(0) { }

        virtual SpecificStats* clone() const {
            TwoDNearStats* specific = new TwoDNearStats(*this);
            return specific;
        }

        size_t objectsLoaded;

        // Since 2d's near does all its work in one go we can't divine the real nscanned from
        // anything else.
        size_t nscanned;
    };

    struct TextStats : public SpecificStats {
        TextStats() : keysExamined(0), fetches(0), parsedTextQuery() { }

        virtual SpecificStats* clone() const {
            TextStats* specific = new TextStats(*this);
            return specific;
        }

        size_t keysExamined;

        size_t fetches;

        // Human-readable form of the FTSQuery associated with the text stage.
        BSONObj parsedTextQuery;
    };

}  // namespace mongo
