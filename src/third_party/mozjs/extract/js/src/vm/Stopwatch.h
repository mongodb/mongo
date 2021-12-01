/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Stopwatch_h
#define vm_Stopwatch_h

#include "mozilla/RefPtr.h"
#include "mozilla/Vector.h"

#include "jsapi.h"

/*
  An API for following in real-time the amount of CPU spent executing
  webpages, add-ons, etc.
*/

namespace js {

/**
 * A container for performance groups.
 *
 * Performance monitoring deals with the execution duration of code
 * that belongs to components, for a notion of components defined by
 * the embedding.  Typically, in a web browser, a component may be a
 * webpage and/or a frame and/or a module and/or an add-on and/or a
 * sandbox and/or a process etc.
 *
 * A PerformanceGroupHolder is owned y a JSCompartment and maps that
 * compartment to all the components to which it belongs.
 */
struct PerformanceGroupHolder {

    /**
     * Get the groups to which this compartment belongs.
     *
     * Pre-condition: Execution must have entered the compartment.
     *
     * May return `nullptr` if the embedding has not initialized
     * support for performance groups.
     */
    const PerformanceGroupVector* getGroups(JSContext*);

    explicit PerformanceGroupHolder(JSRuntime* runtime)
      : runtime_(runtime)
      , initialized_(false)
    {  }
    ~PerformanceGroupHolder();
    void unlink();
  private:
    JSRuntime* runtime_;

    // `true` once a call to `getGroups` has succeeded.
    bool initialized_;

    // The groups to which this compartment belongs. Filled if and only
    // if `initialized_` is `true`.
    PerformanceGroupVector groups_;
};

/**
 * Container class for everything related to performance monitoring.
 */
struct PerformanceMonitoring {
    /**
     * The number of the current iteration of the event loop.
     */
    uint64_t iteration() {
        return iteration_;
    }

    explicit PerformanceMonitoring()
      : totalCPOWTime(0)
      , stopwatchStartCallback(nullptr)
      , stopwatchStartClosure(nullptr)
      , stopwatchCommitCallback(nullptr)
      , stopwatchCommitClosure(nullptr)
      , getGroupsCallback(nullptr)
      , getGroupsClosure(nullptr)
      , isMonitoringJank_(false)
      , isMonitoringCPOW_(false)
      , iteration_(0)
      , startedAtIteration_(0)
      , highestTimestampCounter_(0)
    { }

    /**
     * Reset the stopwatch.
     *
     * This method is meant to be called whenever we start
     * processing an event, to ensure that we stop any ongoing
     * measurement that would otherwise provide irrelevant
     * results.
     */
    void reset();

    /**
     * Start the stopwatch.
     *
     * This method is meant to be called once we know that the
     * current event contains JavaScript code to execute. Calling
     * this several times during the same iteration is idempotent.
     */
    void start();

    /**
     * Commit the performance data collected since the last call
     * to `start()`, unless `reset()` has been called since then.
     */
    bool commit();

    /**
     * Liberate memory and references.
     */
    void dispose(JSRuntime* rtx);

    /**
     * Activate/deactivate stopwatch measurement of jank.
     *
     * Noop if `value` is `true` and the stopwatch is already
     * measuring jank, or if `value` is `false` and the stopwatch
     * is not measuring jank.
     *
     * Otherwise, any pending measurements are dropped, but previous
     * measurements remain stored.
     *
     * May return `false` if the underlying hashtable cannot be allocated.
     */
    bool setIsMonitoringJank(bool value) {
        if (isMonitoringJank_ != value)
            reset();

        isMonitoringJank_ = value;
        return true;
    }
    bool isMonitoringJank() const {
        return isMonitoringJank_;
    }

    /**
     * Mark that a group has been used in this iteration.
     */
    bool addRecentGroup(PerformanceGroup* group);

    /**
     * Activate/deactivate stopwatch measurement of CPOW.
     *
     * Noop if `value` is `true` and the stopwatch is already
     * measuring CPOW, or if `value` is `false` and the stopwatch
     * is not measuring CPOW.
     *
     * Otherwise, any pending measurements are dropped, but previous
     * measurements remain stored.
     *
     * May return `false` if the underlying hashtable cannot be allocated.
     */
    bool setIsMonitoringCPOW(bool value) {
        if (isMonitoringCPOW_ != value)
            reset();

        isMonitoringCPOW_ = value;
        return true;
    }

    bool isMonitoringCPOW() const {
        return isMonitoringCPOW_;
    }

    /**
     * Callbacks called when we start executing an event/when we have
     * run to completion (including enqueued microtasks).
     *
     * If there are no nested event loops, each call to
     * `stopwatchStartCallback` is followed by a call to
     * `stopwatchCommitCallback`. However, embedders should not assume
     * that this will always be the case, unless they take measures to
     * prevent nested event loops.
     *
     * In presence of nested event loops, several calls to
     * `stopwatchStartCallback` may occur before a call to
     * `stopwatchCommitCallback`. Embedders should assume that a
     * second call to `stopwatchStartCallback` cancels any measure
     * started by the previous calls to `stopwatchStartCallback` and
     * which have not been committed by `stopwatchCommitCallback`.
     */
    void setStopwatchStartCallback(js::StopwatchStartCallback cb, void* closure) {
        stopwatchStartCallback = cb;
        stopwatchStartClosure = closure;
    }
    void setStopwatchCommitCallback(js::StopwatchCommitCallback cb, void* closure) {
        stopwatchCommitCallback = cb;
        stopwatchCommitClosure = closure;
    }

    /**
     * Callback called to associate a JSCompartment to the set of
     * `PerformanceGroup`s that represent the components to which
     * it belongs.
     */
    void setGetGroupsCallback(js::GetGroupsCallback cb, void* closure) {
        getGroupsCallback = cb;
        getGroupsClosure = closure;
    }

    /**
     * The total amount of time spent waiting on CPOWs since the
     * start of the process, in microseconds.
     */
    uint64_t totalCPOWTime;

    /**
     * A variant of RDTSC artificially made monotonic.
     *
     * Always return 0 on platforms that do not support RDTSC.
     */
    uint64_t monotonicReadTimestampCounter();

    /**
     * Data extracted by the AutoStopwatch to determine how often
     * we reschedule the process to a different CPU during the
     * execution of JS.
     *
     * Warning: These values are incremented *only* on platforms
     * that offer a syscall/libcall to check on which CPU a
     * process is currently executed.
     */
    struct TestCpuRescheduling
    {
        // Incremented once we have finished executing code
        // in a group, if the CPU on which we started
        // execution is the same as the CPU on which
        // we finished.
        uint64_t stayed;
        // Incremented once we have finished executing code
        // in a group, if the CPU on which we started
        // execution is different from the CPU on which
        // we finished.
        uint64_t moved;
        TestCpuRescheduling()
            : stayed(0),
              moved(0)
        { }
    };
    TestCpuRescheduling testCpuRescheduling;
  private:
    PerformanceMonitoring(const PerformanceMonitoring&) = delete;
    PerformanceMonitoring& operator=(const PerformanceMonitoring&) = delete;

  private:
    friend struct PerformanceGroupHolder;
    js::StopwatchStartCallback stopwatchStartCallback;
    void* stopwatchStartClosure;
    js::StopwatchCommitCallback stopwatchCommitCallback;
    void* stopwatchCommitClosure;

    js::GetGroupsCallback getGroupsCallback;
    void* getGroupsClosure;

    /**
     * `true` if stopwatch monitoring is active for Jank, `false` otherwise.
     */
    bool isMonitoringJank_;
    /**
     * `true` if stopwatch monitoring is active for CPOW, `false` otherwise.
     */
    bool isMonitoringCPOW_;

    /**
     * The number of times we have entered the event loop.
     * Used to reset counters whenever we enter the loop,
     * which may be caused either by having completed the
     * previous run of the event loop, or by entering a
     * nested loop.
     *
     * Always incremented by 1, may safely overflow.
     */
    uint64_t iteration_;

    /**
     * The iteration at which the stopwatch was last started.
     *
     * Used both to avoid starting the stopwatch several times
     * during the same event loop and to avoid committing stale
     * stopwatch results.
     */
    uint64_t startedAtIteration_;

    /**
     * Groups used in the current iteration.
     */
    PerformanceGroupVector recentGroups_;

    /**
     * The highest value of the timestamp counter encountered
     * during this iteration.
     */
    uint64_t highestTimestampCounter_;
};

// Temporary disable untested code path.
#if 0 // WINVER >= 0x0600
struct cpuid_t {
    uint16_t group_;
    uint8_t number_;
    cpuid_t(uint16_t group, uint8_t number)
        : group_(group),
          number_(number)
    { }
    cpuid_t()
        : group_(0),
          number_(0)
    { }
};
#else
    typedef struct {} cpuid_t;
#endif // defined(WINVER >= 0x0600)

/**
 * RAII class to start/stop measuring performance when
 * entering/leaving a compartment.
 */
class AutoStopwatch final {
    // The context with which this object was initialized.
    // Non-null.
    JSContext* const cx_;

    // An indication of the number of times we have entered the event
    // loop.  Used only for comparison.
    uint64_t iteration_;

    // `true` if we are monitoring jank, `false` otherwise.
    bool isMonitoringJank_;
    // `true` if we are monitoring CPOW, `false` otherwise.
    bool isMonitoringCPOW_;

    // Timestamps captured while starting the stopwatch.
    uint64_t cyclesStart_;
    uint64_t CPOWTimeStart_;

    // The CPU on which we started the measure. Defined only
    // if `isMonitoringJank_` is `true`.
    cpuid_t cpuStart_;

    PerformanceGroupVector groups_;

  public:
    // If the stopwatch is active, constructing an instance of
    // AutoStopwatch causes it to become the current owner of the
    // stopwatch.
    //
    // Previous owner is restored upon destruction.
    explicit AutoStopwatch(JSContext* cx MOZ_GUARD_OBJECT_NOTIFIER_PARAM);
    ~AutoStopwatch();
  private:
    void inline enter();

    bool inline exit();

    // Attempt to acquire a group
    // If the group is inactive or if the group already has a stopwatch,
    // do nothing and return `null`.
    // Otherwise, bind the group to `this` for the current iteration
    // and return `group`.
    PerformanceGroup* acquireGroup(PerformanceGroup* group);

    // Release a group. Noop if `this` is not the stopwatch of
    // `group` for the current iteration.
    void releaseGroup(PerformanceGroup* group);

    // Add recent changes to all the groups owned by this stopwatch.
    // Mark the groups as changed recently.
    bool addToGroups(uint64_t cyclesDelta, uint64_t CPOWTimeDelta);

    // Add recent changes to a single group. Mark the group as changed recently.
    bool addToGroup(JSRuntime* runtime, uint64_t cyclesDelta, uint64_t CPOWTimeDelta, PerformanceGroup* group);

    // Update telemetry statistics.
    void updateTelemetry(const cpuid_t& a, const cpuid_t& b);

    // Perform a subtraction for a quantity that should be monotonic
    // but is not guaranteed to be so.
    //
    // If `start <= end`, return `end - start`.
    // Otherwise, return `0`.
    uint64_t inline getDelta(const uint64_t end, const uint64_t start) const;

    // Return the value of the Timestamp Counter, as provided by the CPU.
    // 0 on platforms for which we do not have access to a Timestamp Counter.
    uint64_t inline getCycles(JSRuntime*) const;


    // Return the identifier of the current CPU, on platforms for which we have
    // access to the current CPU.
    cpuid_t inline getCPU() const;

    // Compare two CPU identifiers.
    bool inline isSameCPU(const cpuid_t& a, const cpuid_t& b) const;
  private:
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER;
};


} // namespace js

#endif // vm_Stopwatch_h
