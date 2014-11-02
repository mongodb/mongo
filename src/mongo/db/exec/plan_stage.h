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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/invalidation_type.h"

namespace mongo {

    class Collection;
    class DiskLoc;
    class OperationContext;

    /**
     * A PlanStage ("stage") is the basic building block of a "Query Execution Plan."  A stage is
     * the smallest piece of machinery used in executing a compiled query.  Stages either access
     * data (from a collection or an index) to create a stream of results, or transform a stream of
     * results (e.g. AND, OR, SORT) to create a stream of results.
     *
     * Stages have zero or more input streams but only one output stream.  Data-accessing stages are
     * leaves and data-transforming stages have children.  Stages can be connected together to form
     * a tree which is then executed (see plan_executor.h) to solve a query.
     *
     * A stage's input and output are each typed.  Only stages with compatible types can be
     * connected.
     *
     * All of the stages of a QEP share a WorkingSet (see working_set.h).  Data source stages
     * allocate a slot in the WorkingSet, fill the slot with data, and return the ID of that slot.
     * Subsequent stages fetch a WorkingSetElement by its ID and operate on the enclosed data.
     *
     * Stages do nothing unless work() is called.  work() is a request to the stage to consume one
     * unit of input.  Some stages (e.g. AND, SORT) require many calls to work() before generating
     * output as they must consume many units of input.  These stages will inform the caller that
     * they need more time, and work() must be called again in order to produce an output.
     *
     * Every stage of a query implements the PlanStage interface.  Queries perform a unit of work
     * and report on their subsequent status; see StatusCode for possible states.  Query results are
     * passed through the WorkingSet interface; see working_set.h for details.
     *
     * All synchronization is the responsibility of the caller.  Queries must be told to yield with
     * saveState() if any underlying database state changes.  If saveState() is called,
     * restoreState() must be called again before any work() is done.
     *
     * Here is a very simple usage example:
     *
     * WorkingSet workingSet;
     * PlanStage* rootStage = makeQueryPlan(&workingSet, ...);
     * while (!rootStage->isEOF()) {
     *     WorkingSetID result;
     *     switch(rootStage->work(&result)) {
     *     case PlanStage::ADVANCED:
     *         // do something with result
     *         WorkingSetMember* member = workingSet.get(result);
     *         cout << "Result: " << member->obj << std::endl;
     *         break;
     *     case PlanStage::IS_EOF:
     *         // All done.  Will fall out of while loop.
     *         break;
     *     case PlanStage::NEED_TIME:
     *         // Need more time.
     *         break;
     *     case PlanStage::FAILURE:
     *         // Throw exception or return error
     *         break;
     *     }
     *
     *     if (shouldYield) {
     *         // Occasionally yield.
     *         stage->saveState();
     *         // Do work that requires a yield here (execute other plans, insert, delete, etc.).
     *         stage->restoreState();
     *     }
     * }
     */
    class PlanStage {
    public:
        virtual ~PlanStage() { }

        /**
         * All possible return values of work(...)
         */
        enum StageState {
            // work(...) has returned a new result in its out parameter.  The caller must free it
            // from the working set when done with it.
            ADVANCED,

            // work(...) won't do anything more.  isEOF() will also be true.  There is nothing
            // output in the out parameter.
            IS_EOF,

            // work(...) needs more time to product a result.  Call work(...) again.  There is
            // nothing output in the out parameter.
            NEED_TIME,

            // Something went wrong but it's not an internal error.  Perhaps our collection was
            // dropped or state deleted.
            DEAD,

            // Something has gone unrecoverably wrong.  Stop running this query.
            // If the out parameter does not refer to an invalid working set member,
            // call WorkingSetCommon::getStatusMemberObject() to get details on the failure.
            // Any class implementing this interface must set the WSID out parameter to
            // INVALID_ID or a valid WSM ID if FAILURE is returned.
            FAILURE,
        };

        static std::string stateStr(const StageState& state) {
            if (ADVANCED == state) {
                return "ADVANCED";
            }
            else if (IS_EOF == state) {
                return "IS_EOF";
            }
            else if (NEED_TIME == state) {
                return "NEED_TIME";
            }
            else if (DEAD == state) {
                return "DEAD";
            }
            else {
                verify(FAILURE == state);
                return "FAILURE";
            }
        }


        /**
         * Perform a unit of work on the query.  Ask the stage to produce the next unit of output.
         * Stage returns StageState::ADVANCED if *out is set to the next unit of output.  Otherwise,
         * returns another value of StageState to indicate the stage's status.
         */
        virtual StageState work(WorkingSetID* out) = 0;

        /**
         * Returns true if no more work can be done on the query / out of results.
         */
        virtual bool isEOF() = 0;

        //
        // Yielding and isolation semantics:
        //
        // Any data that is not inserted, deleted, or modified during a yield will be faithfully
        // returned by a query that should return that data.
        //
        // Any data inserted, deleted, or modified during a yield that should be returned by a query
        // may or may not be returned by that query.  The query could return: nothing; the data
        // before; the data after; or both the data before and the data after.
        //
        // In short, there is no isolation between a query and an insert/delete/update.  AKA,
        // READ_UNCOMMITTED.
        //

        /**
         * Notifies the stage that all locks are about to be released.  The stage must save any
         * state required to resume where it was before saveState was called.
         */
        virtual void saveState() = 0;

        /**
         * Notifies the stage that any required locks have been reacquired.  The stage must restore
         * any saved state and be ready to handle calls to work().
         *
         * Can only be called after saveState.
         *
         * XXX: We may not need to pass down 'opCtx' if getMore'd queries use the same
         * OperationContext they were created with.
         */
        virtual void restoreState(OperationContext* opCtx) = 0;

        /**
         * Notifies a stage that a DiskLoc is going to be deleted (or in-place updated) so that the
         * stage can invalidate or modify any state required to continue processing without this
         * DiskLoc.
         *
         * Can only be called after a saveState but before a restoreState.
         */
        virtual void invalidate(const DiskLoc& dl, InvalidationType type) = 0;

        /**
         * Retrieve a list of this stage's children. This stage keeps ownership of
         * its children.
         */
        virtual std::vector<PlanStage*> getChildren() const = 0;

        /**
         * What type of stage is this?
         */
        virtual StageType stageType() const = 0;

        //
        // Execution stats.
        //

        /**
         * Returns a tree of stats.  See plan_stats.h for the details of this structure.  If the
         * stage has any children it must propagate the request for stats to them.
         *
         * Creates plan stats tree which has the same topology as the original execution tree,
         * but has a separate lifetime.
         *
         * Caller owns returned pointer.
         */
        virtual PlanStageStats* getStats() = 0;

        /**
         * Get the CommonStats for this stage. The pointer is *not* owned by the caller.
         *
         * The returned pointer is only valid when the corresponding stage is also valid.
         * It must not exist past the stage. If you need the stats to outlive the stage,
         * use the getStats(...) method above.
         */
        virtual const CommonStats* getCommonStats() = 0;

        /**
         * Get stats specific to this stage. Some stages may not have specific stats, in which
         * case they return NULL. The pointer is *not* owned by the caller.
         *
         * The returned pointer is only valid when the corresponding stage is also valid.
         * It must not exist past the stage. If you need the stats to outlive the stage,
         * use the getStats(...) method above.
         */
        virtual const SpecificStats* getSpecificStats() = 0;

    };

}  // namespace mongo
