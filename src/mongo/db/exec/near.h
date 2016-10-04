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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

/**
 * An abstract stage which uses a progressive sort to return results sorted by distance.  This
 * is useful when we do not have a full ordering computed over the distance metric and don't
 * want to generate one.
 *
 * Some parts of the geoNear implementation depend on the type of index being used, so
 * subclasses need to implement these three functions:
 *
 * - initialize() - Prepares the stage to begin the geoNear search. Must return IS_EOF iff the
 *                  stage is prepared to begin buffering documents.
 * - nextInterval() - Must return the bounds of the next interval with a PlanStage that will find
 *                    all of the results in this interval that have not already been buffered in
 *                    previous intervals.
 * - computeDistance() - Must return the distance from a document to the centroid of search using
 *                       the correct metric (spherical/flat, radians/meters).
 *
 * For example - given a distance search over documents with distances from [0 -> 10], the child
 * stage might break up the search into intervals [0->5),[5,7),[7->10].
 *
 * Each interval requires a PlanStage which returns all of the results in the interval that have
 * not been buffered in a previous interval.  Results in each interval are buffered fully before
 * being returned to ensure that ordering is preserved. Results that are in the cover, but outside
 * the outer bounds of the current interval will remain buffered to be returned in subsequent
 * search intervals.
 *
 * For efficient search, the child stage should not return too many results outside the interval,
 * but correctness only depends on all the results in the interval being buffered before any are
 * returned. As an example, a PlanStage for the interval [0->5) might just be a full collection
 * scan - this will always buffer every result in the interval, but is slow.  If there is an index
 * available, an IndexScan stage might also return all documents with distance [0->5) but
 * would be much faster.
 *
 * Also for efficient search, the intervals should not be too large or too small - though again
 * correctness does not depend on interval size.
 *
 * The child stage may return duplicate documents, so it is the responsibility of NearStage to
 * deduplicate. Every document in _resultBuffer is kept track of in _seenDocuments. When a
 * document is returned or invalidated, it is removed from _seenDocuments.
 *
 * TODO: If a document is indexed in multiple cells (Polygons, PolyLines, etc.), there is a
 * possibility that it will be returned more than once. Since doInvalidate() force fetches a
 * document and removes it from _seenDocuments, NearStage will not deduplicate if it encounters
 * another instance of this document. This will only occur if two cells for a document are in the
 * same interval and the invalidation occurs between the scan of the first cell and the second, so
 * NearStage no longer knows that it's seen this document before.
 *
 * TODO: Right now the interface allows the nextCovering() to be adaptive, but doesn't allow
 * aborting and shrinking a covered range being buffered if we guess wrong.
 */
class NearStage : public PlanStage {
public:
    struct CoveredInterval;

    ~NearStage();

    bool isEOF() final;
    StageState doWork(WorkingSetID* out) final;

    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) final;

    StageType stageType() const final;
    std::unique_ptr<PlanStageStats> getStats() final;
    const SpecificStats* getSpecificStats() const final;

protected:
    /**
     * Subclasses of NearStage must provide basics + a stats object which gets owned here.
     */
    NearStage(OperationContext* txn,
              const char* typeName,
              StageType type,
              WorkingSet* workingSet,
              Collection* collection);

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

    /*
     * Initialize near stage before buffering the data.
     * Return IS_EOF if subclass finishes the initialization.
     * Return NEED_TIME if we need more time.
     * Return errors if an error occurs.
     * Can't return ADVANCED.
     */
    virtual StageState initialize(OperationContext* txn,
                                  WorkingSet* workingSet,
                                  Collection* collection,
                                  WorkingSetID* out) = 0;

    // Filled in by subclasses.
    NearStats _specificStats;

private:
    //
    // Generic methods for progressive search functionality
    //

    StageState initNext(WorkingSetID* out);
    StageState bufferNext(WorkingSetID* toReturn, Status* error);
    StageState advanceNext(WorkingSetID* toReturn);

    //
    // Generic state for progressive near search
    //

    // Not owned here
    WorkingSet* const _workingSet;
    // Not owned here, used for fetching buffered results before invalidation
    Collection* const _collection;

    // A progressive search works in stages of buffering and then advancing
    enum SearchState {
        SearchState_Initializing,
        SearchState_Buffering,
        SearchState_Advancing,
        SearchState_Finished
    } _searchState;

    // May need to track disklocs from the child stage to do our own deduping, also to do
    // invalidation of buffered results.
    unordered_map<RecordId, WorkingSetID, RecordId::Hasher> _seenDocuments;

    // Stats for the stage covering this interval
    // This is owned by _specificStats
    IntervalStats* _nextIntervalStats;

    // Sorted buffered results to be returned - the current interval
    struct SearchResult;
    std::priority_queue<SearchResult> _resultBuffer;

    // Stats
    const StageType _stageType;

    // The current stage from which this stage should buffer results
    // Pointer to the last interval in _childrenIntervals. Owned by _childrenIntervals.
    CoveredInterval* _nextInterval;

    // All children CoveredIntervals and the sub-stages owned by them.
    //
    // All children intervals except the last active one are only used by getStats(),
    // because they are all EOF.
    OwnedPointerVector<CoveredInterval> _childrenIntervals;
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

    PlanStage* const covering;  // Owned in PlanStage::_children.
    const bool dedupCovering;

    const double minDistance;
    const double maxDistance;
    const bool inclusiveMax;
};

}  // namespace mongo
