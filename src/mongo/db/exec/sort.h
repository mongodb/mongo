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

#include <vector>

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

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

        // We read the child into this.
        vector<WorkingSetID> _data;

        // Have we sorted our data?
        bool _sorted;

        // Iterates through _data post-sort returning it.
        vector<WorkingSetID>::iterator _resultIterator;

        // We buffer a lot of data and we want to look it up by DiskLoc quickly upon invalidation.
        typedef unordered_map<DiskLoc, WorkingSetID, DiskLoc::Hasher> DataMap;
        DataMap _wsidByDiskLoc;

        // Stats
        CommonStats _commonStats;
        SortStats _specificStats;
    };

    // Parameters that must be provided to a SortStage
    class SortStageParams {
    public:
        //SortStageParams() : limit(0) { }

        // How we're sorting.
        BSONObj pattern;

        // TODO: Implement this.
        // Must be >= 0.  Equal to 0 for no limit.
        // int limit;
    };

}  // namespace mongo
