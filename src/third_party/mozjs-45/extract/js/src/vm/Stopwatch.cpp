/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Stopwatch.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/unused.h"

#if defined(XP_WIN)
#include <processthreadsapi.h>
#include <windows.h>
#endif // defined(XP_WIN)

#include "jscompartment.h"

#include "gc/Zone.h"
#include "vm/Runtime.h"

namespace js {

bool
PerformanceMonitoring::addRecentGroup(PerformanceGroup* group)
{
    if (group->isUsedInThisIteration())
        return true;

    group->setIsUsedInThisIteration(true);
    return recentGroups_.append(group);
}

void
PerformanceMonitoring::reset()
{
    // All ongoing measures are dependent on the current iteration#.
    // By incrementing it, we mark all data as stale. Stale data will
    // be overwritten progressively during the execution.
    ++iteration_;
    recentGroups_.clear();
}

void
PerformanceMonitoring::start()
{
    if (!isMonitoringJank_)
        return;

    if (iteration_ == startedAtIteration_) {
        // The stopwatch is already started for this iteration.
        return;
    }

    startedAtIteration_ = iteration_;
    if (stopwatchStartCallback)
        stopwatchStartCallback(iteration_, stopwatchStartClosure);
}

// Commit the data that has been collected during the iteration
// into the actual `PerformanceData`.
//
// We use the proportion of cycles-spent-in-group over
// cycles-spent-in-toplevel-group as an approximation to allocate
// system (kernel) time and user (CPU) time to each group. Note
// that cycles are not an exact measure:
//
// 1. if the computer has gone to sleep, the clock may be reset to 0;
// 2. if the process is moved between CPUs/cores, it may end up on a CPU
//    or core with an unsynchronized clock;
// 3. the mapping between clock cycles and walltime varies with the current
//    frequency of the CPU;
// 4. other threads/processes using the same CPU will also increment
//    the counter.
//
// ** Effect of 1. (computer going to sleep)
//
// We assume that this will happen very seldom. Since the final numbers
// are bounded by the CPU time and Kernel time reported by `getresources`,
// the effect will be contained to a single iteration of the event loop.
//
// ** Effect of 2. (moving between CPUs/cores)
//
// On platforms that support it, we only measure the number of cycles
// if we start and end execution of a group on the same
// CPU/core. While there is a small window (a few cycles) during which
// the thread can be migrated without us noticing, we expect that this
// will happen rarely enough that this won't affect the statistics
// meaningfully.
//
// On other platforms, assuming that the probability of jumping
// between CPUs/cores during a given (real) cycle is constant, and
// that the distribution of differences between clocks is even, the
// probability that the number of cycles reported by a measure is
// modified by X cycles should be a gaussian distribution, with groups
// with longer execution having a larger amplitude than groups with
// shorter execution. Since we discard measures that result in a
// negative number of cycles, this distribution is actually skewed
// towards over-estimating the number of cycles of groups that already
// have many cycles and under-estimating the number of cycles that
// already have fewer cycles.
//
// Since the final numbers are bounded by the CPU time and Kernel time
// reported by `getresources`, we accept this bias.
//
// ** Effect of 3. (mapping between clock cycles and walltime)
//
// Assuming that this is evenly distributed, we expect that this will
// eventually balance out.
//
// ** Effect of 4. (cycles increase with system activity)
//
// Assuming that, within an iteration of the event loop, this happens
// unformly over time, this will skew towards over-estimating the number
// of cycles of groups that already have many cycles and under-estimating
// the number of cycles that already have fewer cycles.
//
// Since the final numbers are bounded by the CPU time and Kernel time
// reported by `getresources`, we accept this bias.
//
// ** Big picture
//
// Computing the number of cycles is fast and should be accurate
// enough in practice. Alternatives (such as calling `getresources`
// all the time or sampling from another thread) are very expensive
// in system calls and/or battery and not necessarily more accurate.
bool
PerformanceMonitoring::commit()
{
#if !defined(MOZ_HAVE_RDTSC)
    // The AutoStopwatch is only executed if `MOZ_HAVE_RDTSC`.
    return false;
#endif // !defined(MOZ_HAVE_RDTSC)

    if (!isMonitoringJank_) {
        // Either we have not started monitoring or monitoring has
        // been cancelled during the iteration.
        return true;
    }

    if (startedAtIteration_ != iteration_) {
        // No JS code has been monitored during this iteration.
        return true;
    }

    GroupVector recentGroups;
    recentGroups_.swap(recentGroups);

    bool success = true;
    if (stopwatchCommitCallback)
        success = stopwatchCommitCallback(iteration_, recentGroups, stopwatchCommitClosure);

    // Reset immediately, to make sure that we're not hit by the end
    // of a nested event loop (which would cause `commit` to be called
    // twice in succession).
    reset();
    return success;
}

void
PerformanceMonitoring::dispose(JSRuntime* rt)
{
    reset();
    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        c->performanceMonitoring.unlink();
    }
}

PerformanceGroupHolder::~PerformanceGroupHolder()
{
    unlink();
}

void
PerformanceGroupHolder::unlink()
{
    initialized_ = false;
    groups_.clear();
}

const GroupVector*
PerformanceGroupHolder::getGroups(JSContext* cx)
{
    if (initialized_)
        return &groups_;

    if (!runtime_->performanceMonitoring.getGroupsCallback)
        return nullptr;

    if (!runtime_->performanceMonitoring.getGroupsCallback(cx, groups_, runtime_->performanceMonitoring.getGroupsClosure))
        return nullptr;

    initialized_ = true;
    return &groups_;
}

AutoStopwatch::AutoStopwatch(JSContext* cx MOZ_GUARD_OBJECT_NOTIFIER_PARAM_IN_IMPL)
  : cx_(cx)
  , iteration_(0)
  , isMonitoringJank_(false)
  , isMonitoringCPOW_(false)
  , cyclesStart_(0)
  , CPOWTimeStart_(0)
{
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;

    JSCompartment* compartment = cx_->compartment();
    if (compartment->scheduledForDestruction)
        return;

    JSRuntime* runtime = cx_->runtime();
    iteration_ = runtime->performanceMonitoring.iteration();

    const GroupVector* groups = compartment->performanceMonitoring.getGroups(cx);
    if (!groups) {
      // Either the embedding has not provided any performance
      // monitoring logistics or there was an error that prevents
      // performance monitoring.
      return;
    }
    for (auto group = groups->begin(); group < groups->end(); group++) {
      auto acquired = acquireGroup(*group);
      if (acquired)
        groups_.append(acquired);
    }
    if (groups_.length() == 0) {
      // We are not in charge of monitoring anything.
      return;
    }

    // Now that we are sure that JS code is being executed,
    // initialize the stopwatch for this iteration, lazily.
    runtime->performanceMonitoring.start();
    enter();
}

AutoStopwatch::~AutoStopwatch()
{
    if (groups_.length() == 0) {
        // We are not in charge of monitoring anything.
        return;
    }

    JSCompartment* compartment = cx_->compartment();
    if (compartment->scheduledForDestruction)
        return;

    JSRuntime* runtime = cx_->runtime();
    if (iteration_ != runtime->performanceMonitoring.iteration()) {
        // We have entered a nested event loop at some point.
        // Any information we may have is obsolete.
        return;
    }

    mozilla::Unused << exit(); // Sadly, there is nothing we can do about an error at this point.

    for (auto group = groups_.begin(); group < groups_.end(); group++)
        releaseGroup(*group);
}

void
AutoStopwatch::enter()
{
    JSRuntime* runtime = cx_->runtime();

    if (runtime->performanceMonitoring.isMonitoringCPOW()) {
        CPOWTimeStart_ = runtime->performanceMonitoring.totalCPOWTime;
        isMonitoringCPOW_ = true;
    }

    if (runtime->performanceMonitoring.isMonitoringJank()) {
        cyclesStart_ = this->getCycles();
        cpuStart_ = this->getCPU();
        isMonitoringJank_ = true;
    }
}

bool
AutoStopwatch::exit()
{
    JSRuntime* runtime = cx_->runtime();

    uint64_t cyclesDelta = 0;
    if (isMonitoringJank_ && runtime->performanceMonitoring.isMonitoringJank()) {
        // We were monitoring jank when we entered and we still are.

        // If possible, discard results when we don't end on the
        // same CPU as we started.  Note that we can be
        // rescheduled to another CPU beween `getCycles()` and
        // `getCPU()`.  We hope that this will happen rarely
        // enough that the impact on our statistics will remain
        // limited.
        const cpuid_t cpuEnd = this->getCPU();
        if (isSameCPU(cpuStart_, cpuEnd)) {
            const uint64_t cyclesEnd = getCycles();
            cyclesDelta = getDelta(cyclesEnd, cyclesStart_);
        }
#if WINVER >= 0x600
        updateTelemetry(cpuStart_, cpuEnd);
#elif defined(__linux__)
        updateTelemetry(cpuStart_, cpuEnd);
#endif // WINVER >= 0x600 || _linux__
    }

    uint64_t CPOWTimeDelta = 0;
    if (isMonitoringCPOW_ && runtime->performanceMonitoring.isMonitoringCPOW()) {
        // We were monitoring CPOW when we entered and we still are.
        const uint64_t CPOWTimeEnd = runtime->performanceMonitoring.totalCPOWTime;
        CPOWTimeDelta = getDelta(CPOWTimeEnd, CPOWTimeStart_);
    }
    return addToGroups(cyclesDelta, CPOWTimeDelta);
}

void
AutoStopwatch::updateTelemetry(const cpuid_t& cpuStart_, const cpuid_t& cpuEnd)
{
  JSRuntime* runtime = cx_->runtime();

    if (isSameCPU(cpuStart_, cpuEnd))
        runtime->performanceMonitoring.testCpuRescheduling.stayed += 1;
    else
        runtime->performanceMonitoring.testCpuRescheduling.moved += 1;
}

PerformanceGroup*
AutoStopwatch::acquireGroup(PerformanceGroup* group)
{
    MOZ_ASSERT(group);

    if (group->isAcquired(iteration_))
        return nullptr;

    if (!group->isActive())
        return nullptr;

    group->acquire(iteration_, this);
    return group;
}

void
AutoStopwatch::releaseGroup(PerformanceGroup* group)
{
    MOZ_ASSERT(group);
        group->release(iteration_, this);
}

bool
AutoStopwatch::addToGroups(uint64_t cyclesDelta, uint64_t CPOWTimeDelta)
{
  JSRuntime* runtime = cx_->runtime();

    for (auto group = groups_.begin(); group < groups_.end(); ++group) {
      if (!addToGroup(runtime, cyclesDelta, CPOWTimeDelta, *group))
        return false;
    }
    return true;
}

bool
AutoStopwatch::addToGroup(JSRuntime* runtime, uint64_t cyclesDelta, uint64_t CPOWTimeDelta, PerformanceGroup* group)
{
    MOZ_ASSERT(group);
    MOZ_ASSERT(group->isAcquired(iteration_, this));

    if (!runtime->performanceMonitoring.addRecentGroup(group))
      return false;
    group->addRecentTicks(iteration_, 1);
    group->addRecentCycles(iteration_, cyclesDelta);
    group->addRecentCPOW(iteration_, CPOWTimeDelta);
    return true;
}

uint64_t
AutoStopwatch::getDelta(const uint64_t end, const uint64_t start) const
{
    if (start >= end)
      return 0;
    return end - start;
}

uint64_t
AutoStopwatch::getCycles() const
{
#if defined(MOZ_HAVE_RDTSC)
    return ReadTimestampCounter();
#else
    return 0;
#endif // defined(MOZ_HAVE_RDTSC)
}

cpuid_t inline
AutoStopwatch::getCPU() const
{
#if defined(XP_WIN) && WINVER >= _WIN32_WINNT_VISTA
    PROCESSOR_NUMBER proc;
    GetCurrentProcessorNumberEx(&proc);

    cpuid_t result(proc.Group, proc.Number);
    return result;
#elif defined(XP_LINUX)
    return sched_getcpu();
#else
    return {};
#endif // defined(XP_WIN) || defined(XP_LINUX)
}

bool inline
AutoStopwatch::isSameCPU(const cpuid_t& a, const cpuid_t& b) const
{
#if defined(XP_WIN)  && WINVER >= _WIN32_WINNT_VISTA
    return a.group_ == b.group_ && a.number_ == b.number_;
#elif defined(XP_LINUX)
    return a == b;
#else
    return true;
#endif
}

PerformanceGroup::PerformanceGroup()
    : recentCycles_(0)
    , recentTicks_(0)
    , recentCPOW_(0)
    , iteration_(0)
    , isActive_(false)
    , isUsedInThisIteration_(false)
    , owner_(nullptr)
    , refCount_(0)
{ }

uint64_t
PerformanceGroup::iteration() const
{
    return iteration_;
}


bool
PerformanceGroup::isAcquired(uint64_t it) const
{
    return owner_ != nullptr && iteration_ == it;
}

bool
PerformanceGroup::isAcquired(uint64_t it, const AutoStopwatch* owner) const
{
    return owner_ == owner && iteration_ == it;
}

void
PerformanceGroup::acquire(uint64_t it, const AutoStopwatch* owner)
{
    if (iteration_ != it) {
        // Any data that pretends to be recent is actually bound
        // to an older iteration and therefore stale.
        resetRecentData();
    }
    iteration_ = it;
    owner_ = owner;
}

void
PerformanceGroup::release(uint64_t it, const AutoStopwatch* owner)
{
    if (iteration_ != it)
        return;

    MOZ_ASSERT(owner == owner_ || owner_ == nullptr);
    owner_ = nullptr;
}

void
PerformanceGroup::resetRecentData()
{
    recentCycles_ = 0;
    recentTicks_ = 0;
    recentCPOW_ = 0;
    isUsedInThisIteration_ = false;
}


uint64_t
PerformanceGroup::recentCycles(uint64_t iteration) const
{
    MOZ_ASSERT(iteration == iteration_);
    return recentCycles_;
}

void
PerformanceGroup::addRecentCycles(uint64_t iteration, uint64_t cycles)
{
    MOZ_ASSERT(iteration == iteration_);
    recentCycles_ += cycles;
}

uint64_t
PerformanceGroup::recentTicks(uint64_t iteration) const
{
    MOZ_ASSERT(iteration == iteration_);
    return recentTicks_;
}

void
PerformanceGroup::addRecentTicks(uint64_t iteration, uint64_t ticks)
{
    MOZ_ASSERT(iteration == iteration_);
    recentTicks_ += ticks;
}


uint64_t
PerformanceGroup::recentCPOW(uint64_t iteration) const
{
    MOZ_ASSERT(iteration == iteration_);
    return recentCPOW_;
}

void
PerformanceGroup::addRecentCPOW(uint64_t iteration, uint64_t CPOW)
{
    MOZ_ASSERT(iteration == iteration_);
    recentCPOW_ += CPOW;
}


bool
PerformanceGroup::isActive() const
{
    return isActive_;
}

void
PerformanceGroup::setIsActive(bool value)
{
  isActive_ = value;
}

void
PerformanceGroup::setIsUsedInThisIteration(bool value)
{
  isUsedInThisIteration_ = value;
}
bool
PerformanceGroup::isUsedInThisIteration() const
{
  return isUsedInThisIteration_;
}

void
PerformanceGroup::AddRef()
{
    ++refCount_;
}

void
PerformanceGroup::Release()
{
    MOZ_ASSERT(refCount_ > 0);
    --refCount_;
    if (refCount_ > 0)
        return;

    this->Delete();
}

JS_PUBLIC_API(bool) SetStopwatchStartCallback(JSRuntime* rt, StopwatchStartCallback cb, void* closure)
{
    rt->performanceMonitoring.setStopwatchStartCallback(cb, closure);
    return true;
}

JS_PUBLIC_API(bool) SetStopwatchCommitCallback(JSRuntime* rt, StopwatchCommitCallback cb, void* closure)
{
    rt->performanceMonitoring.setStopwatchCommitCallback(cb, closure);
    return true;
}

JS_PUBLIC_API(bool) SetGetPerformanceGroupsCallback(JSRuntime* rt, GetGroupsCallback cb, void* closure)
{
    rt->performanceMonitoring.setGetGroupsCallback(cb, closure);
    return true;
}

JS_PUBLIC_API(bool)
FlushPerformanceMonitoring(JSRuntime* rt)
{
    return rt->performanceMonitoring.commit();
}
JS_PUBLIC_API(void)
ResetPerformanceMonitoring(JSRuntime* rt)
{
    return rt->performanceMonitoring.reset();
}
JS_PUBLIC_API(void)
DisposePerformanceMonitoring(JSRuntime* rt)
{
    return rt->performanceMonitoring.dispose(rt);
}

JS_PUBLIC_API(bool)
SetStopwatchIsMonitoringJank(JSRuntime* rt, bool value)
{
    return rt->performanceMonitoring.setIsMonitoringJank(value);
}
JS_PUBLIC_API(bool)
GetStopwatchIsMonitoringJank(JSRuntime* rt)
{
    return rt->performanceMonitoring.isMonitoringJank();
}

JS_PUBLIC_API(bool)
SetStopwatchIsMonitoringCPOW(JSRuntime* rt, bool value)
{
    return rt->performanceMonitoring.setIsMonitoringCPOW(value);
}
JS_PUBLIC_API(bool)
GetStopwatchIsMonitoringCPOW(JSRuntime* rt)
{
    return rt->performanceMonitoring.isMonitoringCPOW();
}

JS_PUBLIC_API(void)
GetPerfMonitoringTestCpuRescheduling(JSRuntime* rt, uint64_t* stayed, uint64_t* moved)
{
    *stayed = rt->performanceMonitoring.testCpuRescheduling.stayed;
    *moved = rt->performanceMonitoring.testCpuRescheduling.moved;
}

JS_PUBLIC_API(void)
AddCPOWPerformanceDelta(JSRuntime* rt, uint64_t delta)
{
    rt->performanceMonitoring.totalCPOWTime += delta;
}


} // namespace js

