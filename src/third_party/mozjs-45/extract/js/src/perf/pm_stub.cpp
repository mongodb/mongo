/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "perf/jsperf.h"

namespace JS {

PerfMeasurement::PerfMeasurement(PerfMeasurement::EventMask)
  : impl(0),
    eventsMeasured(EventMask(0)),
    cpu_cycles(-1),
    instructions(-1),
    cache_references(-1),
    cache_misses(-1),
    branch_instructions(-1),
    branch_misses(-1),
    bus_cycles(-1),
    page_faults(-1),
    major_page_faults(-1),
    context_switches(-1),
    cpu_migrations(-1)
{
}

PerfMeasurement::~PerfMeasurement()
{
}

void
PerfMeasurement::start()
{
}

void
PerfMeasurement::stop()
{
}

void
PerfMeasurement::reset()
{
    cpu_cycles = -1;
    instructions = -1;
    cache_references = -1;
    cache_misses = -1;
    branch_instructions = -1;
    branch_misses = -1;
    bus_cycles = -1;
    page_faults = -1;
    major_page_faults = -1;
    context_switches = -1;
    cpu_migrations = -1;
}

bool
PerfMeasurement::canMeasureSomething()
{
    return false;
}

} // namespace JS
