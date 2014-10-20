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

namespace mongo {

    /**
     * A description of a request for a count operation. Copyable.
     */
    struct CountRequest {
        // Namespace to operate on (e.g. "foo.bar").
        std::string ns;

        // A predicate describing the set of documents to count.
        //
        // NOTE:
        //   Parsing the raw BSON to our AST is left for later so that the parse method does not
        //   have to look at the catalog. Specifically, creating a CanonicalQuery requires a
        //   Collection* due to the WhereCallback, and we'd rather not have the parse method require
        //   a Collection*.
        BSONObj query;

        // Indicates to the query planner that it should generate a count plan using a
        // particular index.
        BSONObj hint;

        // An integer limiting the number of documents to count.
        long long limit;

        // An integer indicating to not include the first n documents in the count.
        long long skip;

        // Whether this is an explain of a count.
        bool explain;
    };

    /**
     * Stage used by the count command. This stage sits at the root of a plan tree
     * and counts the number of results returned by its child stage.
     *
     * This should not be confused with the CountScan stage. CountScan is a special
     * index access stage which can optimize index access for count operations in
     * some cases. On the other hand, *every* count op has a CountStage at its root.
     *
     * Only returns NEED_TIME until hitting EOF. The count result can be obtained by examining
     * the specific stats.
     */
    class CountStage : public PlanStage {
    public:
        CountStage(OperationContext* txn,
                   Collection* collection,
                   const CountRequest& request,
                   WorkingSet* ws,
                   PlanStage* child);

        virtual ~CountStage();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void saveState();
        virtual void restoreState(OperationContext* opCtx);
        virtual void invalidate(OperationContext* txn, const DiskLoc& dl, InvalidationType type);

        virtual std::vector<PlanStage*> getChildren() const;

        virtual StageType stageType() const { return STAGE_COUNT; }

        PlanStageStats* getStats();

        virtual const CommonStats* getCommonStats();

        virtual const SpecificStats* getSpecificStats();

        static const char* kStageType;

    private:
        /**
         * Computes the count in the case of an empty query, applying the skip and
         * limit if necessary. The result is stored in '_specificStats'.
         */
        void trivialCount();

        // Transactional context for read locks. Not owned by us.
        OperationContext* _txn;

        // The collection over which we are counting.
        Collection* _collection;

        CountRequest _request;

        // The number of documents that we still need to skip.
        long long _leftToSkip;

        // The working set used to pass intermediate results between stages. Not owned
        // by us.
        WorkingSet* _ws;

        scoped_ptr<PlanStage> _child;

        CommonStats _commonStats;
        CountStats _specificStats;
    };

}  // namespace mongo
