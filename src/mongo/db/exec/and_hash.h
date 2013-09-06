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
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    /**
     * Reads from N children, each of which must have a valid DiskLoc.  Uses a hash table to
     * intersect the outputs of the N children, and outputs the intersection.
     *
     * Preconditions: Valid DiskLoc.  More than one child.
     *
     * Any DiskLoc that we keep a reference to that is invalidated before we are able to return it
     * is fetched and added to the WorkingSet as "flagged for further review."  Because this stage
     * operates with DiskLocs, we are unable to evaluate the AND for the invalidated DiskLoc, and it
     * must be fully matched later.
     */
    class AndHashStage : public PlanStage {
    public:
        AndHashStage(WorkingSet* ws, const MatchExpression* filter);
        virtual ~AndHashStage();

        void addChild(PlanStage* child);

        virtual StageState work(WorkingSetID* out);
        virtual bool isEOF();

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        virtual PlanStageStats* getStats();

    private:
        StageState readFirstChild();
        StageState hashOtherChildren();

        // Not owned by us.
        WorkingSet* _ws;

        // Not owned by us.
        const MatchExpression* _filter;

        // The stages we read from.  Owned by us.
        vector<PlanStage*> _children;

        // _dataMap is filled out by the first child and probed by subsequent children.
        typedef unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher> DataMap;
        DataMap _dataMap;

        // Keeps track of what elements from _dataMap subsequent children have seen.
        typedef unordered_set<DiskLoc, DiskLoc::Hasher> SeenMap;
        SeenMap _seenMap;

        // Iterator over the members of _dataMap that survive.
        DataMap::iterator _resultIterator;

        // True if we're still scanning _children for results.
        bool _shouldScanChildren;

        // Which child are we currently working on?
        size_t _currentChild;

        // Stats
        CommonStats _commonStats;
        AndHashStats _specificStats;
    };

}  // namespace mongo
