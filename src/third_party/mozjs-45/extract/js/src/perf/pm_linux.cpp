/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* This variant of nsIPerfMeasurement uses the perf_event interface
 * added in Linux 2.6.31.  We key compilation of this file off the
 * existence of <linux/perf_event.h>.
 */

#include <errno.h>
#include <linux/perf_event.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "perf/jsperf.h"

using namespace js;

// As of July 2010, this system call has not been added to the
// C library, so we have to provide our own wrapper function.
// If this code runs on a kernel that does not implement the
// system call (2.6.30 or older) nothing unpredictable will
// happen - it will just always fail and return -1.
static int
sys_perf_event_open(struct perf_event_attr* attr, pid_t pid, int cpu,
                    int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

namespace {

using JS::PerfMeasurement;
typedef PerfMeasurement::EventMask EventMask;

// Additional state required by this implementation.
struct Impl
{
    // Each active counter corresponds to an open file descriptor.
    int f_cpu_cycles;
    int f_instructions;
    int f_cache_references;
    int f_cache_misses;
    int f_branch_instructions;
    int f_branch_misses;
    int f_bus_cycles;
    int f_page_faults;
    int f_major_page_faults;
    int f_context_switches;
    int f_cpu_migrations;

    // Counter group leader, for Start and Stop.
    int group_leader;

    // Whether counters are running.
    bool running;

    Impl();
    ~Impl();

    EventMask init(EventMask toMeasure);
    void start();
    void stop(PerfMeasurement* counters);
};

// Mapping from our event bitmask to codes passed into the kernel, and
// to fields in the PerfMeasurement and PerfMeasurement::impl structures.
static const struct
{
    EventMask bit;
    uint32_t type;
    uint32_t config;
    uint64_t PerfMeasurement::* counter;
    int Impl::* fd;
} kSlots[PerfMeasurement::NUM_MEASURABLE_EVENTS] = {
#define HW(mask, constant, fieldname)                                   \
    { PerfMeasurement::mask, PERF_TYPE_HARDWARE, PERF_COUNT_HW_##constant, \
      &PerfMeasurement::fieldname, &Impl::f_##fieldname }
#define SW(mask, constant, fieldname)                                   \
    { PerfMeasurement::mask, PERF_TYPE_SOFTWARE, PERF_COUNT_SW_##constant, \
      &PerfMeasurement::fieldname, &Impl::f_##fieldname }

    HW(CPU_CYCLES,          CPU_CYCLES,          cpu_cycles),
    HW(INSTRUCTIONS,        INSTRUCTIONS,        instructions),
    HW(CACHE_REFERENCES,    CACHE_REFERENCES,    cache_references),
    HW(CACHE_MISSES,        CACHE_MISSES,        cache_misses),
    HW(BRANCH_INSTRUCTIONS, BRANCH_INSTRUCTIONS, branch_instructions),
    HW(BRANCH_MISSES,       BRANCH_MISSES,       branch_misses),
    HW(BUS_CYCLES,          BUS_CYCLES,          bus_cycles),
    SW(PAGE_FAULTS,         PAGE_FAULTS,         page_faults),
    SW(MAJOR_PAGE_FAULTS,   PAGE_FAULTS_MAJ,     major_page_faults),
    SW(CONTEXT_SWITCHES,    CONTEXT_SWITCHES,    context_switches),
    SW(CPU_MIGRATIONS,      CPU_MIGRATIONS,      cpu_migrations),

#undef HW
#undef SW
};

Impl::Impl()
  : f_cpu_cycles(-1),
    f_instructions(-1),
    f_cache_references(-1),
    f_cache_misses(-1),
    f_branch_instructions(-1),
    f_branch_misses(-1),
    f_bus_cycles(-1),
    f_page_faults(-1),
    f_major_page_faults(-1),
    f_context_switches(-1),
    f_cpu_migrations(-1),
    group_leader(-1),
    running(false)
{
}

Impl::~Impl()
{
    // Close all active counter descriptors.  Take care to do the group
    // leader last (this may not be necessary, but it's unclear what
    // happens if you close the group leader out from under a group).
    for (int i = 0; i < PerfMeasurement::NUM_MEASURABLE_EVENTS; i++) {
        int fd = this->*(kSlots[i].fd);
        if (fd != -1 && fd != group_leader)
            close(fd);
    }

    if (group_leader != -1)
        close(group_leader);
}

EventMask
Impl::init(EventMask toMeasure)
{
    MOZ_ASSERT(group_leader == -1);
    if (!toMeasure)
        return EventMask(0);

    EventMask measured = EventMask(0);
    struct perf_event_attr attr;
    for (int i = 0; i < PerfMeasurement::NUM_MEASURABLE_EVENTS; i++) {
        if (!(toMeasure & kSlots[i].bit))
            continue;

        memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);

        // Set the type and config fields to indicate the counter we
        // want to enable.  We want read format 0, and we're not using
        // sampling, so leave those fields unset.
        attr.type = kSlots[i].type;
        attr.config = kSlots[i].config;

        // If this will be the group leader it should start off
        // disabled.  Otherwise it should start off enabled (but blocked
        // on the group leader).
        if (group_leader == -1)
            attr.disabled = 1;

        // The rest of the bit fields are really poorly documented.
        // For instance, I have *no idea* whether we should be setting
        // the inherit, inherit_stat, or task flags.  I'm pretty sure
        // we do want to set mmap and comm, and not any of the ones I
        // haven't mentioned.
        attr.mmap = 1;
        attr.comm = 1;

        int fd = sys_perf_event_open(&attr,
                                     0 /* trace self */,
                                     -1 /* on any cpu */,
                                     group_leader,
                                     0 /* no flags presently defined */);
        if (fd == -1)
            continue;

        measured = EventMask(measured | kSlots[i].bit);
        this->*(kSlots[i].fd) = fd;
        if (group_leader == -1)
            group_leader = fd;
    }
    return measured;
}

void
Impl::start()
{
    if (running || group_leader == -1)
        return;

    running = true;
    ioctl(group_leader, PERF_EVENT_IOC_ENABLE, 0);
}

void
Impl::stop(PerfMeasurement* counters)
{
    // This scratch buffer is to ensure that we have read all the
    // available data, even if that's more than we expect.
    unsigned char buf[1024];

    if (!running || group_leader == -1)
        return;

    ioctl(group_leader, PERF_EVENT_IOC_DISABLE, 0);
    running = false;

    // read out and reset all the counter values
    for (int i = 0; i < PerfMeasurement::NUM_MEASURABLE_EVENTS; i++) {
        int fd = this->*(kSlots[i].fd);
        if (fd == -1)
            continue;

        if (read(fd, buf, sizeof(buf)) == sizeof(uint64_t)) {
            uint64_t cur;
            memcpy(&cur, buf, sizeof(uint64_t));
            counters->*(kSlots[i].counter) += cur;
        }

        // Reset the counter regardless of whether the read did what
        // we expected.
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    }
}

} // namespace


namespace JS {

#define initCtr(flag) ((eventsMeasured & flag) ? 0 : -1)

PerfMeasurement::PerfMeasurement(PerfMeasurement::EventMask toMeasure)
  : impl(js_new<Impl>()),
    eventsMeasured(impl ? static_cast<Impl*>(impl)->init(toMeasure)
                   : EventMask(0)),
    cpu_cycles(initCtr(CPU_CYCLES)),
    instructions(initCtr(INSTRUCTIONS)),
    cache_references(initCtr(CACHE_REFERENCES)),
    cache_misses(initCtr(CACHE_MISSES)),
    branch_instructions(initCtr(BRANCH_INSTRUCTIONS)),
    branch_misses(initCtr(BRANCH_MISSES)),
    bus_cycles(initCtr(BUS_CYCLES)),
    page_faults(initCtr(PAGE_FAULTS)),
    major_page_faults(initCtr(MAJOR_PAGE_FAULTS)),
    context_switches(initCtr(CONTEXT_SWITCHES)),
    cpu_migrations(initCtr(CPU_MIGRATIONS))
{
}

#undef initCtr

PerfMeasurement::~PerfMeasurement()
{
    js_delete(static_cast<Impl*>(impl));
}

void
PerfMeasurement::start()
{
    if (impl)
        static_cast<Impl*>(impl)->start();
}

void
PerfMeasurement::stop()
{
    if (impl)
        static_cast<Impl*>(impl)->stop(this);
}

void
PerfMeasurement::reset()
{
    for (int i = 0; i < NUM_MEASURABLE_EVENTS; i++) {
        if (eventsMeasured & kSlots[i].bit)
            this->*(kSlots[i].counter) = 0;
        else
            this->*(kSlots[i].counter) = -1;
    }
}

bool
PerfMeasurement::canMeasureSomething()
{
    // Find out if the kernel implements the performance measurement
    // API.  If it doesn't, syscall(__NR_perf_event_open, ...) is
    // guaranteed to return -1 and set errno to ENOSYS.
    //
    // We set up input parameters that should provoke an EINVAL error
    // from a kernel that does implement perf_event_open, but we can't
    // be sure it will (newer kernels might add more event types), so
    // we have to take care to close any valid fd it might return.

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_MAX;

    int fd = sys_perf_event_open(&attr, 0, -1, -1, 0);
    if (fd >= 0) {
        close(fd);
        return true;
    } else {
        return errno != ENOSYS;
    }
}

} // namespace JS
