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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/index/btree_index_cursor.h"
#include "mongo/db/index/index_access_method.h"
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
        IndexScanParams() : descriptor(NULL),
                            direction(1),
                            doNotDedup(false),
                            maxScan(0),
                            addKeyMetadata(false) { }

        const IndexDescriptor* descriptor;

        IndexBounds bounds;

        int direction;

        bool doNotDedup;

        // How many keys will we look at?
        size_t maxScan;

        // Do we want to add the key as metadata?
        bool addKeyMetadata;
    };

    /**
     * Stage scans over an index from startKey to endKey, returning results that pass the provided
     * filter.  Internally dedups on DiskLoc.
     *
     * TODO: we probably should split this into 2 stages: one btree-only "fast" ixscan and one that
     * strictly talks through the index API.  Need to figure out what we really want to ship down
     * through that API predicate-wise though, currently the language is a BSONObj but that's
     * clearly not enough (or we need different index scan exec nodes per index type?). See
     * SERVER-12397 for tracking.
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
        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual PlanStageStats* getStats();

        static const char* kStageType;

    private:
        /**
         * Initialize the underlying IndexCursor, grab information from the catalog for stats.
         */
        void initIndexScan();

        /** See if the cursor is pointing at or past _endKey, if _endKey is non-empty. */
        void checkEnd();

        // The WorkingSet we annotate with results.  Not owned by us.
        WorkingSet* _workingSet;

        // Index access.
        const IndexAccessMethod* _iam; // owned by Collection -> IndexCatalog
        scoped_ptr<IndexCursor> _indexCursor;
        BSONObj _keyPattern;

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
        std::vector<const BSONElement*> _keyElts;
        std::vector<bool> _keyEltsInc;

        // Stats
        CommonStats _commonStats;
        IndexScanStats _specificStats;
    };

}  // namespace mongo
