/**
 *    Copyright (C) 2014 MongoDB Inc.
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

    struct DistinctParams {
        DistinctParams() : descriptor(NULL),
                           direction(1),
                           fieldNo(0) { }

        // What index are we traversing?
        const IndexDescriptor* descriptor;

        // And in what direction?
        int direction;

        // What are the bounds?
        IndexBounds bounds;

        // What field in the index's key pattern is the one we're distinct-ing over?
        // For example:
        // If we have an index {a:1, b:1} we could use it to distinct over either 'a' or 'b'.
        // If we distinct over 'a' the position is 0.
        // If we distinct over 'b' the position is 1.
        int fieldNo;
    };

    /**
     * Used by the distinct command.  Executes a mutated index scan over the provided bounds.
     * However, rather than looking at every key in the bounds, it skips to the next value of the
     * _params.fieldNo-th indexed field.  This is because distinct only cares about distinct values
     * for that field, so there is no point in examining all keys with the same value for that
     * field.
     *
     * Only created through the getDistinctRunner path.  See db/query/get_runner.cpp
     */
    class DistinctScan : public PlanStage {
    public:
        DistinctScan(const DistinctParams& params, WorkingSet* workingSet);
        virtual ~DistinctScan() { }

        virtual StageState work(WorkingSetID* out);
        virtual bool isEOF();
        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual PlanStageStats* getStats();

    private:
        /**
         * Initialize the underlying IndexCursor
         */
        void initIndexCursor();

        /** See if the cursor is pointing at or past _endKey, if _endKey is non-empty. */
        void checkEnd();

        // The WorkingSet we annotate with results.  Not owned by us.
        WorkingSet* _workingSet;

        // Index access.
        const IndexDescriptor* _descriptor; // owned by Collection -> IndexCatalog
        const IndexAccessMethod* _iam; // owned by Collection -> IndexCatalog

        // The cursor we use to navigate the tree.
        boost::scoped_ptr<BtreeIndexCursor> _btreeCursor;

        // Have we hit the end of the index scan?
        bool _hitEnd;

        // For yielding.
        BSONObj _savedKey;
        DiskLoc _savedLoc;

        DistinctParams _params;

        // _checker gives us our start key and ensures we stay in bounds.
        boost::scoped_ptr<IndexBoundsChecker> _checker;
        int _keyEltsToUse;
        bool _movePastKeyElts;
        vector<const BSONElement*> _keyElts;
        vector<bool> _keyEltsInc;

        // Stats
        CommonStats _commonStats;
        DistinctScanStats _specificStats;
    };

}  // namespace mongo
