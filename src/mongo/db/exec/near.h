/**
 *    Copyright (C) 2014 10gen Inc.
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

#include <queue>

#include "mongo/base/string_data.h"
#include "mongo/base/status_with.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    /**
     * An abstract stage which uses a progressive sort to return results sorted by distance.  This
     * is useful when we do not have a full ordering computed over the distance metric and don't
     * want to generate one.
     *
     * Child stages need to implement functionality which:
     *
     * - defines a distance metric
     * - iterates through ordered distance intervals, nearest to furthest
     * - provides a covering for each distance interval
     *
     * For example - given a distance search over documents with distances from [0 -> 10], the child
     * stage might break up the search into intervals [0->5),[5,7),[7->10].
     *
     * Each interval requires a PlanStage which *covers* the interval (returns all results in the
     * interval).  Results in each interval are buffered fully before being returned to ensure that
     * ordering is preserved.
     *
     * For efficient search, the child stage which covers the distance interval in question should
     * not return too many results outside the interval, but correctness only depends on the child
     * stage returning all results inside the interval. As an example, a PlanStage which covers the
     * interval [0->5) might just be a full collection scan - this will always cover every interval,
     * but is slow.  If there is an index available, an IndexScan stage might also return all
     * documents with distance [0->5) but would be much faster.
     *
     * Also for efficient search, the intervals should not be too large or too small - though again
     * correctness does not depend on interval size.
     *
     * TODO: Right now the interface allows the nextCovering() to be adaptive, but doesn't allow
     * aborting and shrinking a covered range being buffered if we guess wrong.
     */
    class NearStage : public PlanStage {
    public:

        struct CoveredInterval;

        virtual ~NearStage();

        /**
         * Sets a limit on the total number of results this stage will return.
         * Not required.
         */
        void setLimit(int limit);

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const;
        virtual PlanStageStats* getStats();
        virtual const CommonStats* getCommonStats();
        virtual const SpecificStats* getSpecificStats();

    protected:

        /**
         * Subclasses of NearStage must provide basics + a stats object which gets owned here.
         * The stats object must have specific stats which are a subclass of NearStats, otherwise
         * it's generated automatically.
         */
        NearStage(OperationContext* txn,
                  WorkingSet* workingSet,
                  Collection* collection,
                  PlanStageStats* stats);

        /**
         * Exposes NearStats for adaptive search, allows additional specific stats in subclasses.
         */
        NearStats* getNearStats();

        //
        // Methods implemented for specific search functionality
        //

        /**
         * Constructs the next covering over the next interval to buffer results from, or NULL
         * if the full range has been searched.  Use the provided working set as the working
         * set for the covering stage if required.
         *
         * Returns !OK on failure to create next stage.
         */
        virtual StatusWith<CoveredInterval*> nextInterval(OperationContext* txn,
                                                          WorkingSet* workingSet,
                                                          Collection* collection) = 0;

        /**
         * Computes the distance value for the given member data, or -1 if the member should not be
         * returned in the sorted results.
         *
         * Returns !OK on invalid member data.
         */
        virtual StatusWith<double> computeDistance(WorkingSetMember* member) = 0;

    private:

        //
        // Generic methods for progressive search functionality
        //

        StageState bufferNext(Status* error);
        StageState advanceNext(WorkingSetID* toReturn);

        //
        // Generic state for progressive near search
        //

        // Not owned here
        OperationContext* _txn;
        // Not owned here
        WorkingSet* const _workingSet;
        // Not owned here, used for fetching buffered results before invalidation
        Collection* const _collection;

        // A progressive search works in stages of buffering and then advancing
        enum SearchState {
            SearchState_Buffering,
            SearchState_Advancing,
            SearchState_Finished
        } _searchState;

        // The current stage from which this stage should buffer results
        scoped_ptr<CoveredInterval> _nextInterval;

        // May need to track disklocs from the child stage to do our own deduping, also to do
        // invalidation of buffered results.
        unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher> _nextIntervalSeen;

        // Stats for the stage covering this interval
        scoped_ptr<IntervalStats> _nextIntervalStats;

        // Sorted buffered results to be returned - the current interval
        struct SearchResult;
        std::priority_queue<SearchResult> _resultBuffer;

        // Tracking for the number of results we should return
        int _limit;
        int _totalReturned;

        // Stats
        scoped_ptr<PlanStageStats> _stats;
    };

    /**
     * A covered interval over which a portion of a near search can be run.
     */
    struct NearStage::CoveredInterval {

        CoveredInterval(PlanStage* covering,
                        bool dedupCovering,
                        double minDistance,
                        double maxDistance,
                        bool inclusiveMax);

        const scoped_ptr<PlanStage> covering;
        const bool dedupCovering;

        const double minDistance;
        const double maxDistance;
        const bool inclusiveMax;
    };

}  // namespace mongo
