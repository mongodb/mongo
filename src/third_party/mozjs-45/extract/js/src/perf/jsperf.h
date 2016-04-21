/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef perf_jsperf_h
#define perf_jsperf_h

#include "jstypes.h"

#include "js/TypeDecls.h"
#include "js/Utility.h"

namespace JS {

/*
 * JS::PerfMeasurement is a generic way to access detailed performance
 * measurement APIs provided by your operating system.  The details of
 * exactly how this works and what can be measured are highly
 * system-specific, but this interface is (one hopes) implementable
 * on top of all of them.
 *
 * To use this API, create a PerfMeasurement object, passing its
 * constructor a bitmask indicating which events you are interested
 * in.  Thereafter, Start() zeroes all counters and starts timing;
 * Stop() stops timing again; and the counters for the events you
 * requested are available as data values after calling Stop().  The
 * object may be reused for many measurements.
 */
class JS_FRIEND_API(PerfMeasurement)
{
  protected:
    // Implementation-specific data, if any.
    void* impl;

  public:
    /*
     * Events that may be measured.  Taken directly from the list of
     * "generalized hardware performance event types" in the Linux
     * perf_event API, plus some of the "software events".
     */
    enum EventMask {
        CPU_CYCLES          = 0x00000001,
        INSTRUCTIONS        = 0x00000002,
        CACHE_REFERENCES    = 0x00000004,
        CACHE_MISSES        = 0x00000008,
        BRANCH_INSTRUCTIONS = 0x00000010,
        BRANCH_MISSES       = 0x00000020,
        BUS_CYCLES          = 0x00000040,
        PAGE_FAULTS         = 0x00000080,
        MAJOR_PAGE_FAULTS   = 0x00000100,
        CONTEXT_SWITCHES    = 0x00000200,
        CPU_MIGRATIONS      = 0x00000400,

        ALL                 = 0x000007ff,
        NUM_MEASURABLE_EVENTS  = 11
    };

    /*
     * Bitmask of events that will be measured when this object is
     * active (between Start() and Stop()).  This may differ from the
     * bitmask passed to the constructor if the platform does not
     * support measuring all of the requested events.
     */
    const EventMask eventsMeasured;

    /*
     * Counters for each measurable event.
     * Immediately after one of these objects is created, all of the
     * counters for enabled events will be zero, and all of the
     * counters for disabled events will be uint64_t(-1).
     */
    uint64_t cpu_cycles;
    uint64_t instructions;
    uint64_t cache_references;
    uint64_t cache_misses;
    uint64_t branch_instructions;
    uint64_t branch_misses;
    uint64_t bus_cycles;
    uint64_t page_faults;
    uint64_t major_page_faults;
    uint64_t context_switches;
    uint64_t cpu_migrations;

    /*
     * Prepare to measure the indicated set of events.  If not all of
     * the requested events can be measured on the current platform,
     * then the eventsMeasured bitmask will only include the subset of
     * |toMeasure| corresponding to the events that can be measured.
     */
    explicit PerfMeasurement(EventMask toMeasure);

    /* Done with this set of measurements, tear down OS-level state. */
    ~PerfMeasurement();

    /* Start a measurement cycle. */
    void start();

    /*
     * End a measurement cycle, and for each enabled counter, add the
     * number of measured events of that type to the appropriate
     * visible variable.
     */
    void stop();

    /* Reset all enabled counters to zero. */
    void reset();

    /*
     * True if this platform supports measuring _something_, i.e. it's
     * not using the stub implementation.
     */
    static bool canMeasureSomething();
};

/* Inject a Javascript wrapper around the above C++ class into the
 * Javascript object passed as an argument (this will normally be a
 * global object).  The JS-visible API is identical to the C++ API.
 */
extern JS_FRIEND_API(JSObject*)
    RegisterPerfMeasurement(JSContext* cx, JS::HandleObject global);

/*
 * Given a Value which contains an instance of the aforementioned
 * wrapper class, extract the C++ object.  Returns nullptr if the
 * Value is not an instance of the wrapper.
 */
extern JS_FRIEND_API(PerfMeasurement*)
    ExtractPerfMeasurement(Value wrapper);

} // namespace JS

#endif /* perf_jsperf_h */
