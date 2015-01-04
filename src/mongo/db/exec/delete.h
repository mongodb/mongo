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
#include "mongo/db/jsobj.h"

namespace mongo {

    class OperationContext;
    class PlanExecutor;

    struct DeleteStageParams {
        DeleteStageParams() :
            isMulti(false),
            shouldCallLogOp(false),
            fromMigrate(false),
            isExplain(false) { }

        // Should we delete all documents returned from the child (a "multi delete"), or at most one
        // (a "single delete")?
        bool isMulti;

        // Should we write each delete to the oplog?
        bool shouldCallLogOp;

        // Is this delete part of a migrate operation that is essentially like a no-op
        // when the cluster is observed by an external client.
        bool fromMigrate;

        // Are we explaining a delete command rather than actually executing it?
        bool isExplain;
    };

    /**
     * This stage delete documents by RecordId that are returned from its child.  NEED_TIME
     * is returned after deleting a document.
     *
     * Callers of work() must be holding a write lock (and, for shouldCallLogOp=true deletes,
     * callers must have had the replication coordinator approve the write).
     */
    class DeleteStage : public PlanStage {
        MONGO_DISALLOW_COPYING(DeleteStage);
    public:
        DeleteStage(OperationContext* txn,
                    const DeleteStageParams& params,
                    WorkingSet* ws,
                    Collection* collection,
                    PlanStage* child);
        virtual ~DeleteStage();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void saveState();
        virtual void restoreState(OperationContext* opCtx);
        virtual void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type);

        virtual std::vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const { return STAGE_DELETE; }

        virtual PlanStageStats* getStats();

        virtual const CommonStats* getCommonStats();

        virtual const SpecificStats* getSpecificStats();

        static const char* kStageType;

        /**
         * Extracts the number of documents deleted by the update plan 'exec'.
         *
         * Should only be called if the root plan stage of 'exec' is UPDATE and if 'exec' is EOF.
         */
        static long long getNumDeleted(PlanExecutor* exec);

    private:
        // Transactional context.  Not owned by us.
        OperationContext* _txn;

        DeleteStageParams _params;

        // Not owned by us.
        WorkingSet* _ws;

        // Collection to operate on.  Not owned by us.  Can be NULL (if NULL, isEOF() will always
        // return true).  If non-NULL, the lifetime of the collection must supersede that of the
        // stage.
        Collection* _collection;

        boost::scoped_ptr<PlanStage> _child;

        // Stats
        CommonStats _commonStats;
        DeleteStats _specificStats;
    };

}  // namespace mongo
