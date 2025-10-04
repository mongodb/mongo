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
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
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

    /**
     * Ensures that accessor owns the underlying BSON value, which can be potentially owned by
     * storage.
     */
    void prepareForYielding(value::BSONObjValueAccessor& accessor, bool isAccessible) {
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
     * The 'disableSlotAccess' parameter indicates whether this stage is allowed to discard slot
     * state before saving.
     *
     * Propagates to all children, then calls doSaveState().
     *
     */
    void saveState(bool disableSlotAccess = false) {
        auto stage = static_cast<T*>(this);
        stage->_commonStats.yields++;

        if (disableSlotAccess) {
            stage->disableSlotAccess();
        }

        stage->doSaveState();
        // Save the children in a right to left order so dependent stages (i.e. one using correlated
        // slots) are saved first.
        auto& children = stage->_children;
        for (auto idx = children.size(); idx-- > 0;) {
            children[idx]->saveState(disableSlotAccess ? shouldOptimizeSaveState(idx) : false);
        }

#if defined(MONGO_CONFIG_DEBUG_BUILD)
        _saveState = SaveState::kSaved;
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
     */
    void restoreState() {
        auto stage = static_cast<T*>(this);
        stage->_commonStats.unyields++;
#if defined(MONGO_CONFIG_DEBUG_BUILD)
        invariant(_saveState == SaveState::kSaved);
#endif

        for (auto&& child : stage->_children) {
            child->restoreState();
        }

        stage->doRestoreState();
#if defined(MONGO_CONFIG_DEBUG_BUILD)
        stage->_saveState = SaveState::kActive;
#endif
    }

protected:
    // We do not want to incur the overhead of tracking information about saved-ness
    // per stage. This information is only used for sanity checking, so we only run these
    // checks in debug builds.
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    enum class SaveState {
        // The "active" state is when the plan can access storage and operations like
        // open(), getNext(), close() are permitted.
        kActive,

        // In the "saved" state, the plan is completely detached from the storage engine, but
        // cannot be executed. In order to bring the plan back into an active state, restoreState()
        // must be called.
        kSaved
    };
    SaveState _saveState{SaveState::kActive};
#endif

    virtual bool shouldOptimizeSaveState(size_t idx) const {
        return false;
    }

    static bool shouldCopyValue(value::TypeTags tag) {
        if (isShallowType(tag)) {
            return false;
        }
        switch (tag) {
            case value::TypeTags::NumberDecimal:
            case value::TypeTags::StringBig:
            case value::TypeTags::Array:
            case value::TypeTags::ArraySet:
            case value::TypeTags::ArrayMultiSet:
            case value::TypeTags::Object:
            case value::TypeTags::ObjectId:
            case value::TypeTags::RecordId:
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
    // Bit flags to indicate what stats are tracked when a TrialRunTracker is attached.
    enum TrialRunTrackingType : uint32_t {
        NoTracking = 0x0,
        TrackReads = 1 << 0,
        TrackResults = 1 << 1,
    };

    // Bit mask to accumulate what stats are tracked when a TrialRunTracker is attached.
    using TrialRunTrackingTypeMask = uint32_t;

    CanTrackStats(StringData stageType,
                  PlanNodeId nodeId,
                  bool participateInTrialRunTracking,
                  TrialRunTrackingType trackingType)
        : _commonStats(stageType, nodeId),
          _participateInTrialRunTracking(participateInTrialRunTracking),
          _trackingType(trackingType) {
        tassert(8804701,
                "Expect individual stages to track only reads, as results are tracked in the base "
                "CanTrackStats code",
                trackingType == TrialRunTrackingType::NoTracking ||
                    trackingType == TrialRunTrackingType::TrackReads);
    }

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
        _tracker = nullptr;
        auto stage = static_cast<T*>(this);
        for (auto&& child : stage->_children) {
            child->detachFromTrialRunTracker();
        }
    }

    /**
     * Recursively traverses the plan tree and attaches the trial run tracker to nodes that do the
     * tracking.
     */
    TrialRunTrackingTypeMask attachToTrialRunTracker(
        TrialRunTracker* tracker,
        PlanNodeId runtimePlanningRootNodeId = kEmptyPlanNodeId,
        bool foundPlanningRoot = false) {
        TrialRunTrackingTypeMask result = TrialRunTrackingType::NoTracking;
        if (!_participateInTrialRunTracking) {
            return result;
        }

        if (runtimePlanningRootNodeId != kEmptyPlanNodeId) {
            // RuntimePlanningRootNodeId determines a root of a QuerySolution sub-tree that was used
            // in runtime planning. Stage builders are allowed to do any optimizations when
            // converting QuerySolution to SBE plan, such as dropping or combining nodes,
            // so 'runtimePlanningRootNodeId' itself may not be present in the plan.
            // However, for replanning to work we require the
            // following property: a part of the query solution that was used in runtime planning
            // should correspond to a single sub-tree in the resulting SBE plan.

            bool isPlanningNode = _commonStats.nodeId <= runtimePlanningRootNodeId;
            if (foundPlanningRoot) {
                tassert(8523904,
                        "There should be no stages that implements QSNs after planning root in the "
                        "planning sub-tree",
                        isPlanningNode);
            } else if (isPlanningNode) {
                foundPlanningRoot = true;
                _trackingType |= TrialRunTrackingType::TrackResults;
            }
        }

        auto stage = static_cast<T*>(this);
        size_t childrenWithPlanningRoot = 0;
        for (auto&& child : stage->_children) {
            auto childAttachResult = child->attachToTrialRunTracker(
                tracker, runtimePlanningRootNodeId, foundPlanningRoot);
            if (childAttachResult & TrialRunTrackingType::TrackResults) {
                ++childrenWithPlanningRoot;
                tassert(8523905,
                        "A part of the query that participated in runtime planning should be "
                        "implemented as a single sub-tree in SBE plan",
                        childrenWithPlanningRoot <= 1);
            }
            result |= childAttachResult;
        }

        return result | doAttachToTrialRunTracker(tracker, result);
    }

    /**
     * Force this stage to collect timing info during its execution. Must not be called after
     * execution has started.
     */
    void markShouldCollectTimingInfo() {
        tassert(11093508,
                "Execution should not have started",
                durationCount<Microseconds>(_commonStats.executionTime.executionTimeEstimate) == 0);

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

    const boost::optional<SimpleMemoryUsageTracker>& getMemoryTracker() const {
        return _memoryTracker;
    }

protected:
    bool participateInTrialRunTracking() const {
        return _participateInTrialRunTracking;
    }

    /**
     * If trial run tracker is attached, increments the read metric and terminates the trial run
     * with a special exception if metric has reached the limit.
     */
    void trackRead() {
        tassert(8796901,
                str::stream() << "Stage " << _commonStats.stageType
                              << " tracks reads but tracking type is " << _trackingType,
                (_trackingType & TrialRunTrackingType::TrackReads));
        if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumReads>(1)) {
            uasserted(ErrorCodes::QueryTrialRunCompleted,
                      str::stream() << "Trial run early exit in " << _commonStats.stageType);
        }
    }

    PlanState trackPlanState(PlanState state) {
        if (state == PlanState::IS_EOF) {
            _commonStats.isEOF = true;
            _slotsAccessible = false;
        } else {
            _commonStats.advances++;
            _slotsAccessible = true;

            if ((_trackingType & TrialRunTrackingType::TrackResults) && _tracker) {
                bool trackerResult = _tracker->trackProgress<TrialRunTracker::kNumResults>(1);
                tassert(8523903,
                        "TrialRunTracker should not terminate plans on reaching kNumResults",
                        !trackerResult);
            }
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

            if (MONGO_likely(_commonStats.executionTime.precision ==
                             QueryExecTimerPrecision::kMillis)) {
                return boost::optional<ScopedTimer>(
                    boost::in_place_init,
                    &_commonStats.executionTime.executionTimeEstimate,
                    &opCtx->fastClockSource());
            } else {
                return boost::optional<ScopedTimer>(
                    boost::in_place_init,
                    &_commonStats.executionTime.executionTimeEstimate,
                    opCtx->getServiceContext()->getTickSource());
            }
        }

        return boost::none;
    }

    CommonStats _commonStats;

    boost::optional<SimpleMemoryUsageTracker> _memoryTracker{boost::none};

private:
    // Attaches the trial run tracker to this specific node if needed.
    TrialRunTrackingTypeMask doAttachToTrialRunTracker(
        TrialRunTracker* tracker, TrialRunTrackingTypeMask childrenAttachResult) {
        TrialRunTrackingTypeMask result = TrialRunTrackingType::NoTracking;
        if (_trackingType & TrialRunTrackingType::TrackReads) {
            _tracker = tracker;
            result |= TrialRunTrackingType::TrackReads;
        }
        if (_trackingType & TrialRunTrackingType::TrackResults) {
            _tracker = tracker;
            result |= TrialRunTrackingType::TrackResults;
        }
        return result;
    }

    /**
     * In general, accessors can be accessed only after getNext returns a row. It is most definitely
     * not OK to access accessors in ANY other state; e.g. closed, not yet opened, after EOF. We
     * need this tracker to support unfortunate consequences of the internal yielding feature. Once
     * that feature is retired we can then simply revisit all stages and simplify them.
     */
    bool _slotsAccessible{false};

    // Flag which determines whether this node and its children can participate in trial run
    // tracking. A stage and its children are not eligible for trial run tracking when they are
    // planned deterministically (that is, the amount of work they perform is independent of
    // other parts of the tree which are multiplanned).
    bool _participateInTrialRunTracking{true};

    // Determines what stats are tracked during trial by this specific node.
    TrialRunTrackingTypeMask _trackingType{TrialRunTrackingType::NoTracking};

    // If provided, used during a trial run to accumulate certain execution stats. Once the trial
    // run is complete, this pointer is reset to nullptr.
    TrialRunTracker* _tracker{nullptr};
};

/**
 * Provides a method which can be used to check if the current operation has been interrupted.
 * Maintains an internal state to maintain the interrupt check period. Also responsible for
 * triggering yields if this object has been configured with a yield policy.
 *
 * When yielding is enabled, we use the following rules to determine when SBE is required to yield:
 *
 * 1) Stages that are sources of iteration (scan, coscan, unwind, sort, group, spool) should
 *    perform a yield check at least once per unit of work / getNext().
 *
 * 2) Prolonged blocking computation (like sorting) should also periodically perform a yield check.
 *
 * 3) Stages that are not sources of iteration (project, nlj, filter, limit, union, merge) don't
 *    need to perform a yield check as long as they call subtree methods that do.
 *
 * "Source of iteration" refers to a stage's ability to iterate over more than 1 row for each input
 * row (if it exists). Note: filter, nlj stages do introduce a loop, but it is not a source of
 * iteration, as each input row maps to 0-1 output rows rather than 0-N output rows like unwind.
 *
 * Two yielding models were considered, and the second one was adopted for SBE:
 *
 * 1) Strong: "Every call to getNext() must result in at least one yield check." We don't satisfy
 *    that model today because of unwindy stages, which do not perform yielding when iterating over
 *    their inner loop. If they did, then we would be able to prove by structural induction that
 *    when a stage calls a child getNext(), then it itself is not required to perform a yield check
 *    to satisfy that invariant
 *
 * 2) Weak: "Every O(1) calls to getNext() must result in a least one yield check." We do satisfy
 *    that model today if we assume the size of unwind is O(1). As above, the inductive proof works
 *    and stages that call a child getNext() are not required to perform a yield check themselves.
 *
 * In the Weak model, the constant in O(1) is basically limited by the sizes of arrays (which are
 * limited by the document size limit), craziness of scalar expressions, and craziness of the
 * pipeline (like chaining many unwinds together). The pipeline itself also has a limited maximum
 * size, so in fact the amount of work by non-sources of iteration is indeed bounded by O(1).
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
    void checkForInterruptAndYield(OperationContext* opCtx) {
        invariant(opCtx);

        if (!_yieldPolicy) {
            // Yielding has been disabled, but interrupt checking can never be disabled (all
            // SBE operations must be interruptible). When yielding is enabled, it is responsible
            // for interrupt checking, but when disabled we do it ourselves.
            checkForInterruptNoYield(opCtx);
        } else if (_yieldPolicy->shouldYieldOrInterrupt(opCtx)) {
            uassertStatusOK(_yieldPolicy->yieldOrInterrupt(
                opCtx, nullptr, RestoreContext::RestoreType::kYield));
        }
    }

    /**
     * Checks for interrupt if necessary. This will never yield regardless of the yielding policy.
     * Should only be used in special cases, e.g. needed for performance or to avoid bad edge cases.
     */
    void checkForInterruptNoYield(OperationContext* opCtx) {
        invariant(opCtx);

        if (--_interruptCounter == 0) {
            _interruptCounter = kInterruptCheckPeriod;
            opCtx->checkForInterrupt();
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
              bool participateInTrialRunTracking,
              TrialRunTrackingType trackingType = TrialRunTrackingType::NoTracking)
        : CanTrackStats{stageType, nodeId, participateInTrialRunTracking, trackingType},
          CanInterrupt{yieldPolicy} {}

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
     * The stage spills its data and asks from all its children to spill their data as well.
     */
    void forceSpill(PlanYieldPolicy* yieldPolicy) {
        if (yieldPolicy && yieldPolicy->shouldYieldOrInterrupt(_opCtx)) {
            uassertStatusOK(yieldPolicy->yieldOrInterrupt(
                _opCtx, nullptr, RestoreContext::RestoreType::kYield));
        }
        doForceSpill();
        for (const auto& child : _children) {
            child->forceSpill(yieldPolicy);
        }
    }

    void attachCollectionAcquisition(const MultipleCollectionAccessor& mca) {
        doAttachCollectionAcquisition(mca);
        for (auto&& child : _children) {
            child->attachCollectionAcquisition(mca);
        }
    }

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

private:
    /**
     * Spills the stage's data to disk. Stages that can spill their own data need to override this
     * method.
     */
    virtual void doForceSpill() {}

protected:
    // Derived classes can optionally override these methods.
    virtual void doSaveState() {}
    virtual void doRestoreState() {}
    virtual void doDetachFromOperationContext() {}
    virtual void doAttachToOperationContext(OperationContext* opCtx) {}

    /**
     * Allows collection accessing stages to obtain a reference to the collection acquisition. This
     * should be a no-op for non-collection accessing stages, and needs to be implemented by
     * collection accessing stages like scan and ixscan.
     */
    virtual void doAttachCollectionAcquisition(const MultipleCollectionAccessor& mca) = 0;

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
