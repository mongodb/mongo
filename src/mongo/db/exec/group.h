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

#include "mongo/db/exec/plan_stage.h"

namespace mongo {

    class Collection;

    /**
     * A description of a request for a group operation.  Copyable.
     */
    struct GroupRequest {
        // Namespace to operate on (e.g. "foo.bar").
        std::string ns;

        // A predicate describing the set of documents to group.
        BSONObj query;

        // The field(s) to group by.  Alternative to "keyFunctionCode".  Empty if "keyFunctionCode"
        // is being used instead.
        BSONObj keyPattern;

        // A Javascript function that maps a document to a key object.  Alternative to "keyPattern".
        // Empty is "keyPattern" is being used instead.
        std::string keyFunctionCode;

        // A Javascript function that takes a (input document, group result) pair and
        // updates the group result document.
        std::string reduceCode;

        // Scope for the reduce function.  Optional.
        BSONObj reduceScope;

        // The initial value for the group result.
        BSONObj initial;

        // A Javascript function that "finalizes" a group result.  Optional.
        std::string finalize;

        // Whether this is an explain of a group.
        bool explain;
    };

    /**
     * Stage used by the group command.  Consumes input documents from its child stage, returns
     * ADVANCED exactly once with the entire group result, then returns EOF.  Does not return
     * NEED_TIME.
     *
     * Only created through the getExecutorGroup path.
     */
    class GroupStage: public PlanStage {
        MONGO_DISALLOW_COPYING(GroupStage);
    public:
        GroupStage(OperationContext* txn,
                   const GroupRequest& request,
                   WorkingSet* workingSet,
                   PlanStage* child);
        virtual ~GroupStage() { }

        virtual StageState work(WorkingSetID* out);
        virtual bool isEOF();
        virtual void saveState();
        virtual void restoreState(OperationContext* opCtx);
        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual std::vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const { return STAGE_GROUP; }

        virtual PlanStageStats* getStats();

        virtual const CommonStats* getCommonStats();

        virtual const SpecificStats* getSpecificStats();

        static const char* kStageType;

    private:
        // Transactional context for read locks.  Not owned by us.
        OperationContext* _txn;

        GroupRequest _request;

        // The WorkingSet we annotate with results.  Not owned by us.
        WorkingSet* _ws;

        CommonStats _commonStats;
        GroupStats _specificStats;

        scoped_ptr<PlanStage> _child;

        // Flag indicating whether results have been returned yet.
        bool _groupCompleted;
    };

}  // namespace mongo
