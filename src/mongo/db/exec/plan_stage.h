/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/restore_context.h"

namespace mongo {

class ClockSource;
class Collection;
class CollectionPtr;
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
 * If an error occurs at runtime (e.g. we reach resource limits for the request), then work() throws
 * an exception. At this point, statistics may be extracted from the execution plan, but the
 * execution tree is otherwise unusable and the plan must be discarded.
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
    PlanStage(const char* typeName, ExpressionContext* expCtx)
        : _commonStats(typeName), _opCtx(expCtx->opCtx), _expCtx(expCtx) {
        invariant(expCtx);
        if (expCtx->explain || expCtx->mayDbProfile) {
            // Populating the field for execution time indicates that this stage should time each
            // call to work().
            _commonStats.executionTimeMillis.emplace(0);
        }
    }

protected:
    /**
     * Obtain a PlanStage given a child stage. Called during the construction of derived
     * PlanStage types with a single direct descendant.
     */
    PlanStage(ExpressionContext* expCtx, std::unique_ptr<PlanStage> child, const char* typeName)
        : PlanStage(typeName, expCtx) {
        _children.push_back(std::move(child));
    }

public:
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
        // If the storage engine aborts the storage-level transaction with WriteConflictException or
        // TemporarilyUnavailableException, then an execution stage that interfaces with storage is
        // responsible for catching this exception. After catching the exception, it suspends its
        // state in such a way that will allow it to retry the storage-level operation on the next
        // work() call. Then it populates the out parameter of work(...) with WorkingSet::INVALID_ID
        // and returns NEED_YIELD to its parent.
        //
        // Each stage that receives a NEED_YIELD from a child must propagate the NEED_YIELD up
        // and perform no work.
        //
        // The NEED_YIELD is handled at the level of the PlanExecutor, either by re-throwing the
        // WCE/TUE or by resetting transaction state.
        //
        NEED_YIELD,
    };

    static std::string stateStr(const StageState& state) {
        switch (state) {
            case PlanStage::ADVANCED:
                return "ADVANCED";
            case PlanStage::IS_EOF:
                return "IS_EOF";
            case PlanStage::NEED_TIME:
                return "NEED_TIME";
            case PlanStage::NEED_YIELD:
                return "NEED_YIELD";
        }
        MONGO_UNREACHABLE;
    }


    /**
     * Perform a unit of work on the query.  Ask the stage to produce the next unit of output.
     * Stage returns StageState::ADVANCED if *out is set to the next unit of output.  Otherwise,
     * returns another value of StageState to indicate the stage's status.
     *
     * Throws an exception if an error is encountered while executing the query.
     */
    StageState work(WorkingSetID* out) {
        auto optTimer(getOptTimer());

        ++_commonStats.works;

        StageState workResult;
        try {
            workResult = doWork(out);
        } catch (...) {
            _commonStats.failed = true;
            throw;
        }

        if (StageState::ADVANCED == workResult) {
            ++_commonStats.advanced;
        } else if (StageState::NEED_TIME == workResult) {
            ++_commonStats.needTime;
        } else if (StageState::NEED_YIELD == workResult) {
            ++_commonStats.needYield;
        }

        return workResult;
    }

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
     *
     * RestoreContext is a context containing external state needed by plan stages to be able to
     * restore into a valid state. The RequiresCollectionStage requires a valid CollectionPtr for
     * example.
     *
     * Throws a UserException on failure to restore due to a conflicting event such as a
     * collection drop. May throw a WriteConflictException, in which case the caller may choose to
     * retry.
     */
    void restoreState(const RestoreContext& context);

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

    /**
     * Force this stage to collect timing info during its execution. Must not be called after
     * execution has started.
     */
    void markShouldCollectTimingInfo() {
        invariant(!_commonStats.executionTimeMillis || *_commonStats.executionTimeMillis == 0);
        _commonStats.executionTimeMillis.emplace(0);
    }

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
    virtual void doRestoreState(const RestoreContext& context) {}

    /**
     * Does stage-specific detaching.
     *
     * Implementations of this method cannot use the pointer returned from opCtx().
     */
    virtual void doDetachFromOperationContext() {}

    /**
     * Does stage-specific attaching.
     *
     * If an OperationContext* is needed, use opCtx(), which will return a valid
     * OperationContext* (the one to which the stage is reattaching).
     */
    virtual void doReattachToOperationContext() {}

    ClockSource* getClock() const;

    OperationContext* opCtx() const {
        return _opCtx;
    }

    ExpressionContext* expCtx() const {
        return _expCtx;
    }

    /**
     * Returns an optional timer which is used to collect time spent executing the current
     * stage. May return boost::none if it is not necessary to collect timing info.
     */
    boost::optional<ScopedTimer> getOptTimer() {
        if (_commonStats.executionTimeMillis) {
            return {{getClock(), _commonStats.executionTimeMillis.get_ptr()}};
        }

        return boost::none;
    }

    Children _children;
    CommonStats _commonStats;

private:
    OperationContext* _opCtx;

    // The PlanExecutor holds a strong reference to this which ensures that this pointer remains
    // valid for the entire lifetime of the PlanStage.
    ExpressionContext* _expCtx;
};

}  // namespace mongo
