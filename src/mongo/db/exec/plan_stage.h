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

#include <memory>
#include <vector>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/invalidation_type.h"

namespace mongo {

class ClockSource;
class Collection;
class OperationContext;
class RecordId;

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
    PlanStage(const char* typeName, OperationContext* opCtx)
        : _commonStats(typeName), _opCtx(opCtx) {}

    virtual ~PlanStage() {}

    using Children = std::vector<std::unique_ptr<PlanStage>>;

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

        // The storage engine says we need to yield, possibly to fetch a record from disk, or
        // due to an aborted transaction in the storage layer.
        //
        // Full yield request semantics:
        //
        // Each stage that receives a NEED_YIELD from a child must propagate the NEED_YIELD up
        // and perform no work.
        //
        // If a yield is requested due to a WriteConflict, the out parameter of work(...) should
        // be populated with WorkingSet::INVALID_ID. If it is illegal to yield, a
        // WriteConflictException will be thrown.
        //
        // A yield-requesting stage populates the out parameter of work(...) with a WSID that
        // refers to a WSM with a Fetcher*. If it is illegal to yield, this is ignored. This
        // difference in behavior can be removed once SERVER-16051 is resolved.
        //
        // The plan executor is responsible for yielding and, if requested, paging in the data
        // upon receipt of a NEED_YIELD. The plan executor does NOT free the WSID of the
        // requested fetch. The stage that requested the fetch holds the WSID of the loc it
        // wants fetched. On the next call to work() that stage can assume a fetch was performed
        // on the WSM that the held WSID refers to.
        NEED_YIELD,

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
        } else if (IS_EOF == state) {
            return "IS_EOF";
        } else if (NEED_TIME == state) {
            return "NEED_TIME";
        } else if (NEED_YIELD == state) {
            return "NEED_YIELD";
        } else if (DEAD == state) {
            return "DEAD";
        } else {
            verify(FAILURE == state);
            return "FAILURE";
        }
    }


    /**
     * Perform a unit of work on the query.  Ask the stage to produce the next unit of output.
     * Stage returns StageState::ADVANCED if *out is set to the next unit of output.  Otherwise,
     * returns another value of StageState to indicate the stage's status.
     */
    StageState work(WorkingSetID* out);

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
     * Notifies the stage that the underlying data source may change.
     *
     * It is illegal to call work() or isEOF() when a stage is in the "saved" state.
     *
     * Propagates to all children, then calls doSaveState().
     */
    void saveState();

    /**
     * Notifies the stage that underlying data is stable again and prepares for calls to work().
     *
     * Can only be called while the stage in is the "saved" state.
     *
     * Propagates to all children, then calls doRestoreState().
     */
    void restoreState();

    /**
     * Detaches from the OperationContext and releases any storage-engine state.
     *
     * It is only legal to call this when in a "saved" state. While in the "detached" state, it is
     * only legal to call reattachToOperationContext or the destructor. It is not legal to call
     * detachFromOperationContext() while already in the detached state.
     *
     * Propagates to all children, then calls doDetachFromOperationContext().
     */
    void detachFromOperationContext();

    /**
     * Reattaches to the OperationContext and reacquires any storage-engine state.
     *
     * It is only legal to call this in the "detached" state. On return, the cursor is left in a
     * "saved" state, so callers must still call restoreState to use this object.
     *
     * Propagates to all children, then calls doReattachToOperationContext().
     */
    void reattachToOperationContext(OperationContext* opCtx);

    /**
     * Notifies a stage that a RecordId is going to be deleted (or in-place updated) so that the
     * stage can invalidate or modify any state required to continue processing without this
     * RecordId.
     *
     * Can only be called after a saveState but before a restoreState.
     *
     * The provided OperationContext should be used if any work needs to be performed during the
     * invalidate (as the state of the stage must be saved before any calls to invalidate, the
     * stage's own OperationContext is inactive during the invalidate and should not be used).
     *
     * Propagates to all children, then calls doInvalidate().
     */
    void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type);

    /**
     * Retrieve a list of this stage's children. This stage keeps ownership of
     * its children.
     */
    const Children& getChildren() const {
        return _children;
    }

    /**
     * Returns the only child.
     *
     * Convenience method for PlanStages that have exactly one child.
     */
    const std::unique_ptr<PlanStage>& child() const {
        dassert(_children.size() == 1);
        return _children.front();
    }

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
     */
    virtual std::unique_ptr<PlanStageStats> getStats() = 0;

    /**
     * Get the CommonStats for this stage. The pointer is *not* owned by the caller.
     *
     * The returned pointer is only valid when the corresponding stage is also valid.
     * It must not exist past the stage. If you need the stats to outlive the stage,
     * use the getStats(...) method above.
     */
    const CommonStats* getCommonStats() const {
        return &_commonStats;
    }

    /**
     * Get stats specific to this stage. Some stages may not have specific stats, in which
     * case they return NULL. The pointer is *not* owned by the caller.
     *
     * The returned pointer is only valid when the corresponding stage is also valid.
     * It must not exist past the stage. If you need the stats to outlive the stage,
     * use the getStats(...) method above.
     */
    virtual const SpecificStats* getSpecificStats() const = 0;

protected:
    /**
     * Performs one unit of work.  See comment at work() above.
     */
    virtual StageState doWork(WorkingSetID* out) = 0;

    /**
     * Saves any stage-specific state required to resume where it was if the underlying data
     * changes.
     *
     * Stages must be able to handle multiple calls to doSaveState() in a row without a call to
     * doRestoreState() in between.
     */
    virtual void doSaveState() {}

    /**
     * Restores any stage-specific saved state and prepares to handle calls to work().
     */
    virtual void doRestoreState() {}

    /**
     * Does stage-specific detaching.
     *
     * Implementations of this method cannot use the pointer returned from getOpCtx().
     */
    virtual void doDetachFromOperationContext() {}

    /**
     * Does stage-specific attaching.
     *
     * If an OperationContext* is needed, use getOpCtx(), which will return a valid
     * OperationContext* (the one to which the stage is reattaching).
     */
    virtual void doReattachToOperationContext() {}

    /**
     * Does the stage-specific invalidation work.
     */
    virtual void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {}

    ClockSource* getClock() const;

    OperationContext* getOpCtx() const {
        return _opCtx;
    }

    Children _children;
    CommonStats _commonStats;

private:
    OperationContext* _opCtx;
};

}  // namespace mongo
