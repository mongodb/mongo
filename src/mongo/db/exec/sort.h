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
#include <vector>

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    class BtreeKeyGenerator;

    // External params for the sort stage.  Declared below.
    class SortStageParams;

    /**
     * Sorts the input received from the child according to the sort pattern provided.
     *
     * Preconditions: For each field in 'pattern', all inputs in the child must handle a
     * getFieldDotted for that field.
     */
    class SortStage : public PlanStage {
    public:
        SortStage(const SortStageParams& params, WorkingSet* ws, PlanStage* child);

        virtual ~SortStage();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        PlanStageStats* getStats();

    private:
        // Not owned by us.
        WorkingSet* _ws;

        // Where we're reading data to sort from.
        scoped_ptr<PlanStage> _child;

        // Our sort pattern.
        BSONObj _pattern;

        // Have we sorted our data? If so, we can access _resultIterator. If not,
        // we're still populating _data.
        bool _sorted;

        // Collection of working set members to sort with their respective sort key.
        struct SortableDataItem {
            WorkingSetID wsid;
            BSONObj sortKey;
            // Since we must replicate the behavior of a covered sort as much as possible we use the
            // DiskLoc to break sortKey ties.
            // See sorta.js.
            DiskLoc loc;
        };
        vector<SortableDataItem> _data;

        // Iterates through _data post-sort returning it.
        vector<SortableDataItem>::iterator _resultIterator;

        // We buffer a lot of data and we want to look it up by DiskLoc quickly upon invalidation.
        typedef unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher> DataMap;
        DataMap _wsidByDiskLoc;

        //
        // Sort Apparatus
        //

        // A comparator for SortableDataItems.
        struct WorkingSetComparator;
        boost::scoped_ptr<WorkingSetComparator> _cmp;

        // Bounds we should consider before sorting.
        IndexBounds _bounds;

        bool _hasBounds;

        // Helper to extract sorting keys from documents containing dotted fields, arrays,
        // or both.
        boost::scoped_ptr<BtreeKeyGenerator> _keyGen;

        // Helper to filter keys, thus enforcing _bounds over whatever keys generated with
        // _keyGen.
        boost::scoped_ptr<IndexBoundsChecker> _boundsChecker;

        //
        // Stats
        //

        CommonStats _commonStats;
        SortStats _specificStats;

        // The usage in bytes of all bufered data that we're sorting.
        size_t _memUsage;
    };

    // Parameters that must be provided to a SortStage
    class SortStageParams {
    public:
        SortStageParams() : hasBounds(false) { }

        // How we're sorting.
        BSONObj pattern;

        IndexBounds bounds;

        bool hasBounds;

        // TODO: Implement this.
        // Must be >= 0.  Equal to 0 for no limit.
        // int limit;
    };

}  // namespace mongo
