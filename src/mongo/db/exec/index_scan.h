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
 */

#pragma once

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/index/btree_index_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    class IndexAccessMethod;
    class IndexCursor;
    class IndexDescriptor;
    class WorkingSet;

    struct IndexScanParams {
        IndexScanParams() : descriptor(NULL), direction(1), limit(0),
                            forceBtreeAccessMethod(false) { }

        IndexDescriptor* descriptor;

        IndexBounds bounds;

        int direction;

        // This only matters for 2d indices and will be ignored by every other index.
        int limit;

        // Special indices internally open an IndexCursor over themselves but as a straight Btree.
        bool forceBtreeAccessMethod;
    };

    /**
     * Stage scans over an index from startKey to endKey, returning results that pass the provided
     * filter.  Internally dedups on DiskLoc.
     *
     * Sub-stage preconditions: None.  Is a leaf and consumes no stage data.
     */
    class IndexScan : public PlanStage {
    public:
        IndexScan(const IndexScanParams& params, WorkingSet* workingSet,
                  const MatchExpression* filter);

        virtual ~IndexScan() { }

        virtual StageState work(WorkingSetID* out);
        virtual bool isEOF();
        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        virtual PlanStageStats* getStats();

    private:
        /** See if the cursor is pointing at or past _endKey, if _endKey is non-empty. */
        void checkEnd();

        // The WorkingSet we annotate with results.  Not owned by us.
        WorkingSet* _workingSet;

        // Index access.
        scoped_ptr<IndexAccessMethod> _iam;
        scoped_ptr<IndexCursor> _indexCursor;
        scoped_ptr<IndexDescriptor> _descriptor;

        // Have we hit the end of the index scan?
        bool _hitEnd;

        // Contains expressions only over fields in the index key.  We assume this is built
        // correctly by whomever creates this class.
        // The filter is not owned by us.
        const MatchExpression* _filter;

        // Could our index have duplicates?  If so, we use _returned to dedup.
        bool _shouldDedup;
        unordered_set<DiskLoc, DiskLoc::Hasher> _returned;

        // For yielding.
        BSONObj _savedKey;
        DiskLoc _savedLoc;

        // True if there was a yield and the yield changed the cursor position.
        bool _yieldMovedCursor;

        IndexScanParams _params;

        // For our "fast" Btree-only navigation AKA the index bounds optimization.
        scoped_ptr<IndexBoundsChecker> _checker;
        BtreeIndexCursor* _btreeCursor;
        int _keyEltsToUse;
        bool _movePastKeyElts;
        vector<const BSONElement*> _keyElts;
        vector<bool> _keyEltsInc;

        // Stats
        CommonStats _commonStats;
        IndexScanStats _specificStats;
    };

}  // namespace mongo
