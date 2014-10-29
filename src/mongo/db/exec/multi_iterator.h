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

#include "mongo/db/diskloc.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"

namespace mongo {

    /**
     * Iterates over a collection using multiple underlying RecordIterators.
     *
     * This is a special stage which is not used automatically by queries. It is intended for
     * special commands that work with RecordIterators. For example, it is used by the
     * parallelCollectionScan and repairCursor commands
     */
    class MultiIteratorStage : public PlanStage {
    public:
        MultiIteratorStage(OperationContext* txn, WorkingSet* ws, Collection* collection);

        ~MultiIteratorStage() { }

        /**
         * Takes ownership of 'it'.
         */
        void addIterator(RecordIterator* it);

        virtual PlanStage::StageState work(WorkingSetID* out);

        virtual bool isEOF();

        void kill();

        virtual void saveState();
        virtual void restoreState(OperationContext* opCtx);

        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        //
        // These should not be used.
        //

        virtual PlanStageStats* getStats() { return NULL; }
        virtual CommonStats* getCommonStats() { return NULL; }
        virtual SpecificStats* getSpecificStats() { return NULL; }

        virtual std::vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const { return STAGE_MULTI_ITERATOR; }

    private:

        /**
         * @return if more data
         */
        DiskLoc _advance();

        OperationContext* _txn;
        Collection* _collection;
        OwnedPointerVector<RecordIterator> _iterators;

        // Not owned by us.
        WorkingSet* _ws;

        // We allocate a working set member with this id on construction of the stage. It gets
        // used for all fetch requests, changing the DiskLoc as appropriate.
        const WorkingSetID _wsidForFetch;
    };

} // namespace mongo
