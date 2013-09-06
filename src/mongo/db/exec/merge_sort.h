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

#include <list>
#include <queue>
#include <vector>

#include "mongo/db/diskloc.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    // External params for the merge sort stage.  Declared below.
    class MergeSortStageParams;

    /**
     * Merges the outputs of N children, each of which is sorted in the order specified by
     * 'pattern'.  The output is sorted by 'pattern'.  Practically speaking, all of this stage's
     * children are indices.
     *
     * AKA the SERVER-1205 stage.  Allows very efficient handling of the following query:
     * find($or[{a:1}, {b:1}]).sort({c:1}) with indices {a:1, c:1} and {b:1, c:1}.
     *
     * Preconditions: For each field in 'pattern' all inputs in the child must handle a
     * getFieldDotted for that field.
     */
    class MergeSortStage : public PlanStage {
    public:
        MergeSortStage(const MergeSortStageParams& params, WorkingSet* ws);
        virtual ~MergeSortStage();

        void addChild(PlanStage* child);

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        PlanStageStats* getStats();

    private:
        // Not owned by us.
        WorkingSet* _ws;

        // The pattern that we're sorting by.
        BSONObj _pattern;

        // Are we deduplicating on DiskLoc?
        bool _dedup;

        // Which DiskLocs have we seen?
        unordered_set<DiskLoc, DiskLoc::Hasher> _seen;

        // Owned by us.  All the children we're reading from.
        vector<PlanStage*> _children;

        // In order to pick the next smallest value, we need each child work(...) until it produces
        // a result.  This is the queue of children that haven't given us a result yet.
        std::queue<PlanStage*> _noResultToMerge;

        // There is some confusing STL wrangling going on below.  Here's a guide:
        //
        // We want to keep a priority_queue of results so we can quickly return the min result.
        //
        // If we receive an invalidate, we need to iterate over any cached state to see if the
        // invalidate is relevant.
        //
        // We can't iterate over a priority_queue, so we keep the actual cached state in a list and
        // have a priority_queue of iterators into that list.
        //
        // Why an iterator instead of a pointer?  We need to be able to use the information in the
        // priority_queue to remove the item from the list and quickly.

        struct StageWithValue {
            WorkingSetID id;
            PlanStage* stage;
        };

        // We have a priority queue of these.
        typedef list<StageWithValue>::iterator MergingRef;

        // The comparison function used in our priority queue.
        class StageWithValueComparison {
        public:
            StageWithValueComparison(WorkingSet* ws, BSONObj pattern)
                : _ws(ws), _pattern(pattern) {}

            // Is lhs less than rhs?  Note that priority_queue is a max heap by default so we invert
            // the return from the expected value.
            bool operator()(const MergingRef& lhs, const MergingRef& rhs);

        private:
            WorkingSet* _ws;
            BSONObj _pattern;
        };

        // The min heap of the results we're returning.
        std::priority_queue<MergingRef, vector<MergingRef>, StageWithValueComparison> _merging;

        // The data referred to by the _merging queue above.
        list<StageWithValue> _mergingData;

        // Stats
        CommonStats _commonStats;
        MergeSortStats _specificStats;
    };

    // Parameters that must be provided to a MergeSortStage
    class MergeSortStageParams {
    public:
        MergeSortStageParams() : dedup(true) { }

        // How we're sorting.
        BSONObj pattern;

        // Do we deduplicate on DiskLoc?
        bool dedup;
    };

}  // namespace mongo
