/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_yield_policy.h"

namespace mongo {
namespace sbe {

struct CompileCtx;
class PlanStage;
enum class PlanState { ADVANCED, IS_EOF };

/**
 * Provides methods to detach and re-attach to an operation context, which derived classes may
 * override to perform additional actions when these events occur.
 */
class CanSwitchOperationContext {
public:
    CanSwitchOperationContext(PlanStage* stage) : _stage(stage) {
        invariant(_stage);
    }

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
    void attachFromOperationContext(OperationContext* opCtx);

protected:
    // Derived classes can optionally override these methods.
    virtual void doDetachFromOperationContext() {}
    virtual void doAttachFromOperationContext(OperationContext* opCtx) {}

    OperationContext* _opCtx{nullptr};

private:
    PlanStage* const _stage;
};

/**
 * Provides methods to save and restore the state of the object which derives from this class
 * when corresponding events are generated as a response to a change in the underlying data source.
 * Derived classes may override these methods to perform additional actions when these events occur.
 */
class CanChangeState {
public:
    CanChangeState(PlanStage* stage) : _stage(stage) {
        invariant(_stage);
    }

    /**
     * Notifies the stage that the underlying data source may change.
     *
     * It is illegal to call work() or isEOF() when a stage is in the "saved" state. May be called
     * before the first call to open(), before execution of the plan has begun.
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
     * Throws a UserException on failure to restore due to a conflicting event such as a
     * collection drop. May throw a WriteConflictException, in which case the caller may choose to
     * retry.
     */
    void restoreState();

protected:
    // Derived classes can optionally override these methods.
    virtual void doSaveState() {}
    virtual void doRestoreState() {}

private:
    PlanStage* const _stage;
};

/**
 * Provides methods to obtain execution statistics specific to a plan stage.
 */
class CanTrackStats {
public:
    CanTrackStats(StringData stageType) : _commonStats(stageType) {}

    /**
     * Returns a tree of stats. If the stage has any children it must propagate the request for
     * stats to them.
     */
    virtual std::unique_ptr<PlanStageStats> getStats() const = 0;

    /**
     * Get stats specific to this stage. Some stages may not have specific stats, in which
     * case they return nullptr. The pointer is *not* owned by the caller.
     *
     * The returned pointer is only valid when the corresponding stage is also valid.
     * It must not exist past the stage. If you need the stats to outlive the stage,
     * use the getStats(...) method above.
     */
    virtual const SpecificStats* getSpecificStats() const = 0;

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

protected:
    PlanState trackPlanState(PlanState state) {
        if (state == PlanState::IS_EOF) {
            _commonStats.isEOF = true;
        } else {
            invariant(state == PlanState::ADVANCED);
            _commonStats.advances++;
        }
        return state;
    }

    CommonStats _commonStats;
};

/**
 * Provides a methods which can be used to check if the current operation has been interrupted.
 * Maintains an internal state to maintain the interrupt check period.
 */
class CanInterrupt {
public:
    /**
     * This object will always be responsible for interrupt checking, but it can also optionally be
     * responsible for yielding. In order to enable yielding, the caller should pass a non-null
     * 'PlanYieldPolicy' pointer. Yielding may be disabled by providing a nullptr.
     */
    explicit CanInterrupt(PlanYieldPolicy* yieldPolicy) : _yieldPolicy(yieldPolicy) {}

    /**
     * Checks for interrupt if necessary. If yielding has been enabled for this object, then also
     * performs a yield if necessary.
     */
    void checkForInterrupt(OperationContext* opCtx) {
        invariant(opCtx);

        if (--_interruptCounter == 0) {
            _interruptCounter = kInterruptCheckPeriod;
            opCtx->checkForInterrupt();
        }

        if (_yieldPolicy && _yieldPolicy->shouldYieldOrInterrupt(opCtx)) {
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(opCtx));
        }
    }

protected:
    PlanYieldPolicy* const _yieldPolicy{nullptr};

private:
    static const int kInterruptCheckPeriod = 128;
    int _interruptCounter = kInterruptCheckPeriod;
};

/**
 * This is an abstract base class of all plan stages in SBE.
 */
class PlanStage : public CanSwitchOperationContext,
                  public CanChangeState,
                  public CanTrackStats,
                  public CanInterrupt {
public:
    PlanStage(StringData stageType, PlanYieldPolicy* yieldPolicy)
        : CanSwitchOperationContext{this},
          CanChangeState{this},
          CanTrackStats{stageType},
          CanInterrupt{yieldPolicy} {}

    explicit PlanStage(StringData stageType) : PlanStage(stageType, nullptr) {}

    virtual ~PlanStage() = default;

    /**
     * The idiomatic C++ pattern of object cloning. Plan stages must be fully copyable as every
     * thread in parallel execution needs its own private copy.
     */
    virtual std::unique_ptr<PlanStage> clone() const = 0;

    /**
     * Prepare this SBE PlanStage tree for execution. Must be called once, and must be called
     * prior to open(), getNext(), close(), saveState(), or restoreState(),
     */
    virtual void prepare(CompileCtx& ctx) = 0;

    /**
     * Returns a slot accessor for a given slot id. This method is only called during the prepare
     * phase.
     */
    virtual value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) = 0;

    /**
     * Opens the plan tree and makes it ready for subsequent open(), getNext(), and close() calls.
     * The expectation is that a plan stage acquires resources (e.g. memory buffers) during the open
     * call and avoids resource acquisition in getNext().
     *
     * When reOpen flag is true then the plan stage should reinitizalize already acquired resources
     * (e.g. re-hash, re-sort, re-seek, etc).
     */
    virtual void open(bool reOpen) = 0;

    /**
     * Moves to the next position. If the end is reached then return EOF otherwise ADVANCED. Callers
     * are not required to call getNext until EOF. They can stop consuming results at any time. Once
     * EOF is reached it will stay at EOF unless reopened.
     */
    virtual PlanState getNext() = 0;

    /**
     * The mirror method to open(). It releases any acquired resources.
     */
    virtual void close() = 0;

    virtual std::vector<DebugPrinter::Block> debugPrint() const = 0;

    friend class CanSwitchOperationContext;
    friend class CanChangeState;

protected:
    std::vector<std::unique_ptr<PlanStage>> _children;
};

template <typename T, typename... Args>
inline std::unique_ptr<PlanStage> makeS(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}
}  // namespace sbe
}  // namespace mongo
