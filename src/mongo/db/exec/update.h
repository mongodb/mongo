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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_result.h"

namespace mongo {

    class OperationContext;

    struct UpdateStageParams {

        UpdateStageParams(const UpdateRequest* r,
                          UpdateDriver* d,
                          OpDebug* o)
            : request(r),
              driver(d),
              opDebug(o),
              canonicalQuery(NULL) { }

        // Contains update parameters like whether it's a multi update or an upsert. Not owned.
        // Must outlive the UpdateStage.
        const UpdateRequest* request;

        // Contains the logic for applying mods to documents. Not owned. Must outlive
        // the UpdateStage.
        UpdateDriver* driver;

        // Needed to pass to Collection::updateDocument(...).
        OpDebug* opDebug;

        // Not owned here.
        CanonicalQuery* canonicalQuery;

    private:
        // Default constructor not allowed.
        UpdateStageParams();
    };

    /**
     * Execution stage responsible for updates to documents and upserts. NEED_TIME is returned
     * after performing an update or an insert.
     *
     * Callers of work() must be holding a write lock.
     */
    class UpdateStage : public PlanStage {
        MONGO_DISALLOW_COPYING(UpdateStage);
    public:
        UpdateStage(OperationContext* txn,
                    const UpdateStageParams& params,
                    WorkingSet* ws,
                    Collection* collection,
                    PlanStage* child);

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void saveState();
        virtual void restoreState(OperationContext* opCtx);
        virtual void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type);

        virtual std::vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const { return STAGE_UPDATE; }

        virtual PlanStageStats* getStats();

        virtual const CommonStats* getCommonStats();

        virtual const SpecificStats* getSpecificStats();

        static const char* kStageType;

        /**
         * Converts the execution stats (stored by the update stage as an UpdateStats) for the
         * update plan represented by 'exec' into the UpdateResult format used to report the results
         * of writes.
         *
         * Also responsible for filling out 'opDebug' with execution info.
         *
         * Should only be called once this stage is EOF.
         */
        static UpdateResult makeUpdateResult(PlanExecutor* exec, OpDebug* opDebug);

    private:
        /**
         * Computes the result of applying mods to the document 'oldObj' at RecordId 'loc' in
         * memory, then commits these changes to the database.
         */
        void transformAndUpdate(const Snapshotted<BSONObj>& oldObj, RecordId& loc);

        /**
         * Computes the document to insert and inserts it into the collection. Used if the
         * user requested an upsert and no matching documents were found.
         */
        void doInsert();

        /**
         * Have we performed all necessary updates? Even if this is true, we might not be EOF,
         * as we might still have to do an insert.
         */
        bool doneUpdating();

        /**
         * Examines the stats / update request and returns whether there is still an insert left
         * to do. If so then this stage is not EOF yet.
         */
        bool needInsert();

        /**
         * Helper for restoring the state of this update.
         */
        Status restoreUpdateState(OperationContext* opCtx);

        // Transactional context.  Not owned by us.
        OperationContext* _txn;

        UpdateStageParams _params;

        // Not owned by us.
        WorkingSet* _ws;

        // Not owned by us. May be NULL.
        Collection* _collection;

        // Owned by us.
        boost::scoped_ptr<PlanStage> _child;

        // Stats
        CommonStats _commonStats;
        UpdateStats _specificStats;

        // If the update was in-place, we may see it again.  This only matters if we're doing
        // a multi-update; if we're not doing a multi-update we stop after one update and we
        // won't see any more docs.
        //
        // For example: If we're scanning an index {x:1} and performing {$inc:{x:5}}, we'll keep
        // moving the document forward and it will continue to reappear in our index scan.
        // Unless the index is multikey, the underlying query machinery won't de-dup.
        //
        // If the update wasn't in-place we may see it again.  Our query may return the new
        // document and we wouldn't want to update that.
        //
        // So, no matter what, we keep track of where the doc wound up.
        typedef unordered_set<RecordId, RecordId::Hasher> DiskLocSet;
        const boost::scoped_ptr<DiskLocSet> _updatedLocs;

        // These get reused for each update.
        mutablebson::Document& _doc;
        mutablebson::DamageVector _damages;
    };

}  // namespace mongo
