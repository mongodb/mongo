/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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
#include "mongo/db/geo/hash.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/time_support.h"

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
        CommonStats(const char* type)
                      : stageTypeStr(type),
                        works(0),
                        yields(0),
                        unyields(0),
                        invalidates(0),
                        advanced(0),
                        needTime(0),
                        executionTimeMillis(0),
                        isEOF(false) { }
        // String giving the type of the stage. Not owned.
        const char* stageTypeStr;

        // Count calls into the stage.
        size_t works;
        size_t yields;
        size_t unyields;
        size_t invalidates;

        // How many times was this state the return value of work(...)?
        size_t advanced;
        size_t needTime;

        // BSON representation of a MatchExpression affixed to this node. If there
        // is no filter affixed, then 'filter' should be an empty BSONObj.
        BSONObj filter;

        // Time elapsed while working inside this stage.
        long long executionTimeMillis;

        // TODO: have some way of tracking WSM sizes (or really any series of #s).  We can measure
        // the size of our inputs and the size of our outputs.  We can do a lot with the WS here.

        // TODO: once we've picked a plan, collect different (or additional) stats for display to
        // the user, eg. time_t totalTimeSpent;

        bool isEOF;
    private:
        // Default constructor is illegal.
        CommonStats();
    };

    /**
     * This class increments a counter by the time elapsed since its construction when
     * it goes out of scope.
     */
    class ScopedTimer {
    public:
        ScopedTimer(long long* counter) : _counter(counter) {
            _start = curTimeMillis64();
        }

        ~ScopedTimer() {
            long long elapsed = curTimeMillis64() - _start;
            *_counter += elapsed;
        }

    private:
        // Default constructor disallowed.
        ScopedTimer();

        MONGO_DISALLOW_COPYING(ScopedTimer);

        // Reference to the counter that we are incrementing with the elapsed time.
        long long* _counter;

        // Time at which the timer was constructed.
        long long _start;
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
                         flaggedInProgress(0),
                         memUsage(0),
                         memLimit(0) { }

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

        // What's our current memory usage?
        size_t memUsage;

        // What's our memory limit?
        size_t memLimit;
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

    struct CachedPlanStats : public SpecificStats {
        CachedPlanStats() { }

        virtual SpecificStats* clone() const {
            return new CachedPlanStats(*this);
        }
    };

    struct CollectionScanStats : public SpecificStats {
        CollectionScanStats() : docsTested(0), direction(1) { }

        virtual SpecificStats* clone() const {
            CollectionScanStats* specific = new CollectionScanStats(*this);
            return specific;
        }

        // How many documents did we check against our filter?
        size_t docsTested;

        // >0 if we're traversing the collection forwards. <0 if we're traversing it
        // backwards.
        int direction;
    };

    struct CountStats : public SpecificStats {
        CountStats() : nCounted(0), nSkipped(0), trivialCount(false) { }

        virtual SpecificStats* clone() const {
            CountStats* specific = new CountStats(*this);
            return specific;
        }

        // The result of the count.
        long long nCounted;

        // The number of results we skipped over.
        long long nSkipped;

        // A "trivial count" is one that we can answer by calling numRecords() on the
        // collection, without actually going through any query logic.
        bool trivialCount;
    };

    struct CountScanStats : public SpecificStats {
        CountScanStats() : isMultiKey(false),
                           keysExamined(0) { }

        virtual ~CountScanStats() { }

        virtual SpecificStats* clone() const {
            CountScanStats* specific = new CountScanStats(*this);
            // BSON objects have to be explicitly copied.
            specific->keyPattern = keyPattern.getOwned();
            return specific;
        }

        BSONObj keyPattern;

        bool isMultiKey;

        size_t keysExamined;

    };

    struct DeleteStats : public SpecificStats {
        DeleteStats() : docsDeleted(0) { }

        virtual SpecificStats* clone() const {
            return new DeleteStats(*this);
        }

        size_t docsDeleted;
    };

    struct DistinctScanStats : public SpecificStats {
        DistinctScanStats() : keysExamined(0) { }

        virtual SpecificStats* clone() const {
            DistinctScanStats* specific = new DistinctScanStats(*this);
            specific->keyPattern = keyPattern.getOwned();
            return specific;
        }

        // How many keys did we look at while distinct-ing?
        size_t keysExamined;

        BSONObj keyPattern;
    };

    struct FetchStats : public SpecificStats {
        FetchStats() : alreadyHasObj(0),
                       forcedFetches(0),
                       matchTested(0),
                       docsExamined(0) { }

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

        // The total number of full documents touched by the fetch stage.
        size_t docsExamined;
    };

    struct GroupStats : public SpecificStats {
        GroupStats() : nGroups(0) { }

        virtual ~GroupStats() { }

        virtual SpecificStats* clone() const {
            GroupStats* specific = new GroupStats(*this);
            return specific;
        }

        // The total number of groups.
        size_t nGroups;
    };

    struct IDHackStats : public SpecificStats {
        IDHackStats() : keysExamined(0),
                        docsExamined(0) { }

        virtual ~IDHackStats() { }

        virtual SpecificStats* clone() const {
            IDHackStats* specific = new IDHackStats(*this);
            return specific;
        }

        // Number of entries retrieved from the index while executing the idhack.
        size_t keysExamined;

        // Number of documents retrieved from the collection while executing the idhack.
        size_t docsExamined;

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

    struct LimitStats : public SpecificStats {
        LimitStats() : limit(0) { }

        virtual SpecificStats* clone() const {
            LimitStats* specific = new LimitStats(*this);
            return specific;
        }

        size_t limit;
    };

    struct MultiPlanStats : public SpecificStats {
        MultiPlanStats() { }

        virtual SpecificStats* clone() const {
            return new MultiPlanStats(*this);
        }
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

    struct ProjectionStats : public SpecificStats {
        ProjectionStats() { }

        virtual SpecificStats* clone() const {
            ProjectionStats* specific = new ProjectionStats(*this);
            return specific;
        }

        // Object specifying the projection transformation to apply.
        BSONObj projObj;
    };

    struct SortStats : public SpecificStats {
        SortStats() : forcedFetches(0), memUsage(0), memLimit(0) { }

        virtual ~SortStats() { }

        virtual SpecificStats* clone() const {
            SortStats* specific = new SortStats(*this);
            return specific;
        }

        // How many records were we forced to fetch as the result of an invalidation?
        size_t forcedFetches;

        // What's our current memory usage?
        size_t memUsage;

        // What's our memory limit?
        size_t memLimit;

        // The number of results to return from the sort.
        size_t limit;

        // The pattern according to which we are sorting.
        BSONObj sortPattern;
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

        // The pattern according to which we are sorting.
        BSONObj sortPattern;
    };

    struct ShardingFilterStats : public SpecificStats {
        ShardingFilterStats() : chunkSkips(0) { }

        virtual SpecificStats* clone() const {
            ShardingFilterStats* specific = new ShardingFilterStats(*this);
            return specific;
        }

        size_t chunkSkips;
    };

    struct SkipStats : public SpecificStats {
        SkipStats() : skip(0) { }

        virtual SpecificStats* clone() const {
            SkipStats* specific = new SkipStats(*this);
            return specific;
        }

        size_t skip;
    };

    struct IntervalStats {

        IntervalStats() :
            numResultsFound(0),
            numResultsBuffered(0),
            minDistanceFound(-1),
            maxDistanceFound(-1),
            minDistanceBuffered(-1),
            maxDistanceBuffered(-1) {
        }

        long long numResultsFound;
        long long numResultsBuffered;

        double minDistanceFound;
        double maxDistanceFound;
        double minDistanceBuffered;
        double maxDistanceBuffered;
    };

    class NearStats : public SpecificStats {
    public:

        NearStats() {}

        virtual SpecificStats* clone() const {
            return new NearStats(*this);
        }

        long long totalResultsFound() {
            long long totalResultsFound = 0;
            for (vector<IntervalStats>::iterator it = intervalStats.begin();
                it != intervalStats.end(); ++it) {
                totalResultsFound += it->numResultsFound;
            }
            return totalResultsFound;
        }

        vector<IntervalStats> intervalStats;
        BSONObj keyPattern;
    };

    struct UpdateStats : public SpecificStats {
        UpdateStats()
            : nMatched(0),
              nModified(0),
              fastmod(false),
              fastmodinsert(false),
              inserted(false) { }

        virtual SpecificStats* clone() const {
            return new UpdateStats(*this);
        }

        // The number of documents which match the query part of the update.
        size_t nMatched;

        // The number of documents modified by this update.
        size_t nModified;

        // A 'fastmod' update is an in-place update that does not have to modify
        // any indices. It's "fast" because the only work needed is changing the bits
        // inside the document.
        bool fastmod;

        // A 'fastmodinsert' is an insert resulting from an {upsert: true} update
        // which is a doc-replacement style update. It's "fast" because we don't need
        // to compute the document to insert based on the modifiers.
        bool fastmodinsert;

        // Is this an {upsert: true} update that did an insert?
        bool inserted;

        // The object that was inserted. This is an empty document if no insert was performed.
        BSONObj objInserted;
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

        // Index keys that precede the "text" index key.
        BSONObj indexPrefix;
    };

}  // namespace mongo
