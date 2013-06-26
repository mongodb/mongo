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

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    class IndexAccessMethod;
    class IndexCursor;
    class IndexDescriptor;
    class IndexScanParams;
    class WorkingSet;

    /**
     * Stage scans over an index from startKey to endKey, returning results that pass the provided
     * filter.  Internally dedups on DiskLoc.
     *
     * Sub-stage preconditions: None.  Is a leaf and consumes no stage data.
     */
    class IndexScan : public PlanStage {
    public:
        IndexScan(const IndexScanParams& params, WorkingSet* workingSet, Matcher* matcher);
        virtual ~IndexScan() { }

        virtual StageState work(WorkingSetID* out);
        virtual bool isEOF();
        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

    private:
        /** See if the cursor is pointing at or past _endKey, if _endKey is non-empty. */
        void checkEnd();

        // The WorkingSet we annotate with results.  Not owned by us.
        WorkingSet* _workingSet;

        // Index access.
        scoped_ptr<IndexAccessMethod> _iam;
        scoped_ptr<IndexCursor> _indexCursor;
        scoped_ptr<IndexDescriptor> _descriptor;

        // Bounds for the cursor.  TODO: take a set of bounds.
        BSONObj _startKey;
        BSONObj _endKey;
        bool _endKeyInclusive;
        int _direction;
        bool _hitEnd;

        // Contains expressions only over fields in the index key.  We assume this is built
        // correctly by whomever creates this class.
        scoped_ptr<Matcher> _matcher;

        // Could our index have duplicates?  If so, we use _returned to dedup.
        bool _shouldDedup;
        unordered_set<DiskLoc, DiskLoc::Hasher> _returned;

        // For yielding.
        BSONObj _savedKey;
        DiskLoc _savedLoc;

        // True if there was a yield and the yield changed the cursor position.
        bool _yieldMovedCursor;

        // This is IndexScanParams::limit.  See comment there.
        int _numWanted;
    };

    struct IndexScanParams {
        IndexScanParams() : descriptor(NULL), endKeyInclusive(true), direction(1), limit(0),
                            forceBtreeAccessMethod(false) { }
        IndexDescriptor* descriptor;
        BSONObj startKey;
        BSONObj endKey;
        bool endKeyInclusive;
        int direction;

        // This only matters for 2d indices and will be ignored by every other index.
        int limit;

        // Special indices internally open an IndexCursor over themselves but as a straight Btree.
        bool forceBtreeAccessMethod;
    };

}  // namespace mongo
