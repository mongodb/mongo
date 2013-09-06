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

#include "mongo/db/diskloc.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    /**
     * This stage outputs the union of its children.  It optionally deduplicates on DiskLoc.
     *
     * Preconditions: Valid DiskLoc.
     *
     * If we're deduping, we may fail to dedup any invalidated DiskLoc properly.
     */
    class OrStage : public PlanStage {
    public:
        OrStage(WorkingSet* ws, bool dedup, const MatchExpression* filter);
        virtual ~OrStage();

        void addChild(PlanStage* child);

        virtual bool isEOF();

        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        virtual PlanStageStats* getStats();

    private:
        // Not owned by us.
        WorkingSet* _ws;

        // The filter is not owned by us.
        const MatchExpression* _filter;

        // Owned by us.
        vector<PlanStage*> _children;

        // Which of _children are we calling work(...) on now?
        size_t _currentChild;

        // True if we dedup on DiskLoc, false otherwise.
        bool _dedup;

        // Which DiskLocs have we returned?
        unordered_set<DiskLoc, DiskLoc::Hasher> _seen;

        // Stats
        CommonStats _commonStats;
        OrStats _specificStats;
    };

}  // namespace mongo
