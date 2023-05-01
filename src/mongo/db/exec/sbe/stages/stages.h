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

#include "mongo/config.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/scoped_timer_factory.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {

struct CompileCtx;
class PlanStage;
enum class PlanState { ADVANCED, IS_EOF };

/**
 * Provides methods to detach and re-attach to an operation context, which derived classes may
 * override to perform additional actions when these events occur.
 *
 * Parameter 'T' is the typename of the class derived from this class. It's used to implement the
 * curiously recurring template pattern and access the internal state of the derived class.
 */
template <typename T>
class CanSwitchOperationContext {
public:
    CanSwitchOperationContext() = default;

    /**
     * Detaches from the OperationContext and releases any storage-engine state.
     *
     * It is only legal to call this when in a "saved" state. While in the "detached" state, it is
     * only legal to call reattachToOperationContext or the destructor. It is not legal to call
     * detachFromOperationContext() while already in the detached state.
     *
     * Propagates to all children, then calls doDetachFromOperationContext().
     */
    void detachFromOperationContext() {
        invariant(_opCtx);

        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->detachFromOperationContext();
        }

        stage->doDetachFromOperationContext();
        _opCtx = nullptr;
    }

    /**
     * Reattaches to the OperationContext and reacquires any storage-engine state.
     *
     * It is only legal to call this in the "detached" state. On return, the cursor is left in a
     * "saved" state, so callers must still call restoreState to use this object.
     *
     * Propagates to all children, then calls doReattachToOperationContext().
     */
    void attachToOperationContext(OperationContext* opCtx) {
        invariant(opCtx);
        invariant(!_opCtx);

        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->attachToOperationContext(opCtx);
        }

        _opCtx = opCtx;
        stage->doAttachToOperationContext(opCtx);
    }

protected:
    OperationContext* _opCtx{nullptr};
};

/**
 * Provides methods to save and restore the state of the object which derives from this class
 * when corresponding events are generated as a response to a change in the underlying data source.
 * Derived classes may override these methods to perform additional actions when these events occur.
 *
 * Parameter 'T' is the typename of the class derived from this class. It's used to implement the
 * curiously recurring template pattern and access the internal state of the derived class.
 */
template <typename T>
class CanChangeState {
public:
    CanChangeState() = default;

    /**
     * Ensures that accessor owns the underlying BSON value, which can be potentially owned by
     * storage.
     */
    void prepareForYielding(value::OwnedValueAccessor& accessor, bool isAccessible) {
        if (isAccessible) {
            auto [tag, value] = accessor.getViewOfValue();
            if (shouldCopyValue(tag)) {
                accessor.makeOwned();
            }
        } else {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
            auto [tag, val] = value::getPoisonValue();
            accessor.reset(false, tag, val);
#endif
        }
    }

    void prepareForYielding(value::MaterializedRow& row, bool isAccessible) {
        if (isAccessible) {
            for (size_t idx = 0; idx < row.size(); idx++) {
                auto [tag, value] = row.getViewOfValue(idx);
                if (shouldCopyValue(tag)) {
                    row.makeOwned(idx);
                }
            }
        } else {
#if defined(MONGO_CONFIG_DEBUG_BUILD)
            auto [tag, val] = value::getPoisonValue();
            for (size_t idx = 0; idx < row.size(); idx++) {
                row.reset(idx, false, tag, val);
            }
#endif
        }
    }

    /**
     * Notifies the stage that the underlying data source may change.
     *
     * It is illegal to call work() or isEOF() when a stage is in the "saved" state. May be called
     * before the first call to open(), before execution of the plan has begun.
     *
     * Propagates to all children, then calls doSaveState().
     *
     * The 'relinquishCursor' parameter indicates whether cursors should be reset and all data
     * should be copied.
     *
     * When 'relinquishCursor' is true, the 'disableSlotAccess' parameter indicates whether this
     * stage is allowed to discard slot state before saving. When 'relinquishCursor' is false, the
     * 'disableSlotAccess' parameter has no effect.
     *
     * TODO SERVER-59620: Remove the 'relinquishCursor' parameter once all callers pass 'false'.
     */
    void saveState(bool relinquishCursor, bool disableSlotAccess = false) {
        auto stage = static_cast<T*>(this);
        stage->_commonStats.yields++;

        if (relinquishCursor && disableSlotAccess) {
            stage->disableSlotAccess();
        }

        stage->doSaveState(relinquishCursor);
        if (!stage->_children.empty()) {
            saveChildrenState(relinquishCursor, disableSlotAccess);
        }

#if defined(MONGO_CONFIG_DEBUG_BUILD)
        _saveState = relinquishCursor ? SaveState::kSavedFull : SaveState::kSavedNotFull;
#endif
    }

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
     *
     * The 'relinquishCursor' parameter indicates whether the stages are recovering from a "full
     * save" or not, as discussed in saveState(). It is the caller's responsibility to pass the same
     * value for 'relinquishCursor' as was passed in the previous call to saveState().
     */
    void restoreState(bool relinquishCursor) {
        auto stage = static_cast<T*>(this);
        stage->_commonStats.unyields++;
#if defined(MONGO_CONFIG_DEBUG_BUILD)
        if (relinquishCursor) {
            invariant(_saveState == SaveState::kSavedFull);
        } else {
            invariant(_saveState == SaveState::kSavedNotFull);
        }
#endif

        for (auto&& child : stage->_children) {
            child->restoreState(relinquishCursor);
        }

        stage->doRestoreState(relinquishCursor);
#if defined(MONGO_CONFIG_DEBUG_BUILD)
        stage->_saveState = SaveState::kNotSaved;
#endif
    }

protected:
    // We do not want to incur the overhead of tracking information about saved-ness
    // per stage. This information is only used for sanity checking, so we only run these
    // checks in debug builds.
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    // TODO SERVER-59620: Remove this.
    enum class SaveState { kNotSaved, kSavedFull, kSavedNotFull };
    SaveState _saveState{SaveState::kNotSaved};
#endif

    virtual void saveChildrenState(bool relinquishCursor, bool disableSlotAccess) {
        // clang-format off
        static const StringDataSet propagateSet = {
            "branch", "cfilter", "efilter", "exchangep", "filter",   "limit", "limitskip",
            "lspool", "mkbson",  "mkobj",   "project",   "traverse", "union", "unique"};
        // clang-format on

        auto stage = static_cast<T*>(this);
        if (!propagateSet.count(stage->getCommonStats()->stageType)) {
            disableSlotAccess = false;
        }

        // Save the children in a right to left order so dependent stages (i.e. one using correlated
        // slots) are saved first.
        for (auto it = stage->_children.rbegin(); it != stage->_children.rend(); ++it) {
            (*it)->saveState(relinquishCursor, disableSlotAccess);
        }
    }

    static bool shouldCopyValue(value::TypeTags tag) {
        switch (tag) {
            case value::TypeTags::Array:
            case value::TypeTags::ArraySet:
            case value::TypeTags::Object:
                return false;

            default:
                return true;
        }
    }
};

template <typename T>
class CanTrackStats;

/**
 * An abstract class to be used for traversing a plan-stage tree.
 */
class PlanStageVisitor {
public:
    virtual ~PlanStageVisitor() = default;

    friend class CanTrackStats<PlanStage>;

protected:
    /**
     * Visits one plan-stage during a traversal over the plan-stage tree.
     */
    virtual void visit(const PlanStage* stage) = 0;
};

/**
 * Provides methods to obtain execution statistics specific to a plan stage.
 *
 * Parameter 'T' is the typename of the class derived from this class. It's used to implement the
 * curiously recurring template pattern and access the internal state of the derived class.
 */
template <typename T>
class CanTrackStats {
public:
    CanTrackStats(StringData stageType, PlanNodeId nodeId, bool participateInTrialRunTracking)
        : _commonStats(stageType, nodeId),
          _participateInTrialRunTracking(participateInTrialRunTracking) {}

    /**
     * Returns a tree of stats. If the stage has any children it must propagate the request for
     * stats to them. If 'includeDebugInfo' is set to 'true' the stage may include some additional
     * debug info, opaque to the caller, which will be available via 'PlanStageStats::debugInfo'
     * member.
     */
    virtual std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const = 0;

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

    /**
     * Populates plan 'summary' object by walking through the entire PlanStage tree and for each
     * node whose plan node ID equals to the given 'nodeId', or if 'nodeId' is 'kEmptyPlanNodeId',
     * invoking 'acceptVisitor(visitor)' on the SpecificStats instance obtained by calling
     * 'getSpecificStats()'.
     */
    template <bool IsConst>
    void accumulate(PlanNodeId nodeId, PlanStatsVisitor<IsConst>* visitor) const {
        if (auto stats = getSpecificStats();
            stats && (nodeId == kEmptyPlanNodeId || _commonStats.nodeId == nodeId)) {
            stats->acceptVisitor(visitor);
        }

        auto stage = static_cast<const T*>(this);
        for (auto&& child : stage->_children) {
            child->accumulate(nodeId, visitor);
        }
    }

    /**
     * Implements a pre-order traversal over the plan-stage tree starting from this node. The
     * visitor parameter plays the role of an accumulator during this traversal.
     */
    void accumulate(PlanStageVisitor& visitor) const {
        auto stage = static_cast<const T*>(this);
        visitor.visit(stage);
        for (auto&& child : stage->_children) {
            child->accumulate(visitor);
        }
    }

    void detachFromTrialRunTracker() {
        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->detachFromTrialRunTracker();
        }

        stage->doDetachFromTrialRunTracker();
    }

    // Bit flags to indicate what kinds of stages a TrialRunTracker was attached to by a call to the
    // 'attachToTrialRunTracker()' method.
    enum TrialRunTrackerAttachResultFlags : uint32_t {
        NoAttachment = 0x0,
        AttachedToStreamingStage = 1 << 0,
        AttachedToBlockingStage = 1 << 1,
    };

    using TrialRunTrackerAttachResultMask = uint32_t;

    TrialRunTrackerAttachResultMask attachToTrialRunTracker(TrialRunTracker* tracker) {
        TrialRunTrackerAttachResultMask result = TrialRunTrackerAttachResultFlags::NoAttachment;
        if (!_participateInTrialRunTracking) {
            return result;
        }

        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            result |= child->attachToTrialRunTracker(tracker);
        }

        return result | stage->doAttachToTrialRunTracker(tracker, result);
    }

    /**
     * Force this stage to collect timing info during its execution. Must not be called after
     * execution has started.
     */
    void markShouldCollectTimingInfo() {
        invariant(durationCount<Microseconds>(_commonStats.executionTime.executionTimeEstimate) ==
                  0);

        if (internalMeasureQueryExecutionTimeInNanoseconds.load()) {
            _commonStats.executionTime.precision = QueryExecTimerPrecision::kNanos;
        } else {
            _commonStats.executionTime.precision = QueryExecTimerPrecision::kMillis;
        }

        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->markShouldCollectTimingInfo();
        }
    }

    void disableSlotAccess(bool recursive = false) {
        auto stage = static_cast<T*>(this);
        stage->_slotsAccessible = false;
        if (recursive) {
            for (auto& child : stage->_children) {
                child->disableSlotAccess(true);
            }
        }
    }

    void disableTrialRunTracking() {
        _participateInTrialRunTracking = false;
    }

    bool slotsAccessible() const {
        return _slotsAccessible;
    }

protected:
    PlanState trackPlanState(PlanState state) {
        if (state == PlanState::IS_EOF) {
            _commonStats.isEOF = true;
            _slotsAccessible = false;
        } else {
            _commonStats.advances++;
            _slotsAccessible = true;
        }
        return state;
    }

    void trackClose() {
        _commonStats.closes++;
        _slotsAccessible = false;
    }

    /**
     * Returns an optional timer which is used to collect time spent executing the current stage.
     * May return boost::none if it is not necessary to collect timing info.
     */
    boost::optional<ScopedTimer> getOptTimer(OperationContext* opCtx) {
        if (opCtx && _commonStats.executionTime.precision != QueryExecTimerPrecision::kNoTiming) {
            return scoped_timer_factory::make(opCtx->getServiceContext(),
                                              _commonStats.executionTime.precision,
                                              &_commonStats.executionTime.executionTimeEstimate);
        }

        return boost::none;
    }

    CommonStats _commonStats;

    // Flag which determines whether this node and its children can participate in trial run
    // tracking. A stage and its children are not eligible for trial run tracking when they are
    // planned deterministically (that is, the amount of work they perform is independent of
    // other parts of the tree which are multiplanned).
    bool _participateInTrialRunTracking{true};

private:
    /**
     * In general, accessors can be accessed only after getNext returns a row. It is most definitely
     * not OK to access accessors in ANY other state; e.g. closed, not yet opened, after EOF. We
     * need this tracker to support unfortunate consequences of the internal yielding feature. Once
     * that feature is retired we can then simply revisit all stages and simplify them.
     */
    bool _slotsAccessible{false};
};

/**
 * Provides a method which can be used to check if the current operation has been interrupted.
 * Maintains an internal state to maintain the interrupt check period. Also responsible for
 * triggering yields if this object has been configured with a yield policy.
 */
template <typename T>
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

        if (!_yieldPolicy) {
            // Yielding has been disabled, but interrupt checking can never be disabled (all
            // SBE operations must be interruptible). When yielding is enabled, it is responsible
            // for interrupt checking, but when disabled we do it ourselves.
            if (--_interruptCounter == 0) {
                _interruptCounter = kInterruptCheckPeriod;
                opCtx->checkForInterrupt();
            }
        } else if (_yieldPolicy->shouldYieldOrInterrupt(opCtx)) {
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(opCtx));
        }
    }

    void attachNewYieldPolicy(PlanYieldPolicy* yieldPolicy) {
        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->attachNewYieldPolicy(yieldPolicy);
        }

        if (_yieldPolicy) {
            _yieldPolicy = yieldPolicy;
        }
    }

protected:
    PlanYieldPolicy* _yieldPolicy{nullptr};

private:
    static const int kInterruptCheckPeriod = 128;
    int _interruptCounter = kInterruptCheckPeriod;
};

/**
 * This is an abstract base class of all plan stages in SBE.
 */
class PlanStage : public CanSwitchOperationContext<PlanStage>,
                  public CanChangeState<PlanStage>,
                  public CanTrackStats<PlanStage>,
                  public CanInterrupt<PlanStage> {
public:
    using Vector = absl::InlinedVector<std::unique_ptr<PlanStage>, 2>;

    PlanStage(StringData stageType,
              PlanYieldPolicy* yieldPolicy,
              PlanNodeId nodeId,
              bool participateInTrialRunTracking)
        : CanTrackStats{stageType, nodeId, participateInTrialRunTracking},
          CanInterrupt{yieldPolicy} {}

    PlanStage(StringData stageType, PlanNodeId nodeId, bool participateInTrialRunTracking)
        : PlanStage(stageType, nullptr, nodeId, participateInTrialRunTracking) {}

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
     * (e.g. re-hash, re-sort, re-seek, etc), but it can avoid reinitializing things that do not
     * contain state and are not destroyed by close(), since close() is not called before a reopen.
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

    virtual std::vector<DebugPrinter::Block> debugPrint() const {
        auto stats = getCommonStats();
        std::string str = str::stream() << '[' << stats->nodeId << "] " << stats->stageType;
        return {DebugPrinter::Block(str)};
    }

    /**
     * Estimates the compile-time size of the current plan stage and its children (SBE Plan
     * subtree). The compile-time size is the size of the SBE subtree before it has been prepared or
     * executed.
     */
    virtual size_t estimateCompileTimeSize() const = 0;

    friend class CanSwitchOperationContext<PlanStage>;
    friend class CanChangeState<PlanStage>;
    friend class CanTrackStats<PlanStage>;
    friend class CanInterrupt<PlanStage>;

protected:
    // Derived classes can optionally override these methods.
    virtual void doSaveState(bool relinquishCursor) {}
    virtual void doRestoreState(bool relinquishCursor) {}
    virtual void doDetachFromOperationContext() {}
    virtual void doAttachToOperationContext(OperationContext* opCtx) {}
    virtual void doDetachFromTrialRunTracker() {}
    virtual TrialRunTrackerAttachResultMask doAttachToTrialRunTracker(
        TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
        return TrialRunTrackerAttachResultFlags::NoAttachment;
    }

    Vector _children;
};

template <typename T, typename... Args>
inline std::unique_ptr<PlanStage> makeS(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename... Ts>
inline auto makeSs(Ts&&... pack) {
    PlanStage::Vector stages;

    (stages.emplace_back(std::forward<Ts>(pack)), ...);

    return stages;
}

}  // namespace sbe
}  // namespace mongo
