/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>

#include "mongo/util/clock_source.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/system_clock_source.h"

namespace mongo {
namespace {

/**
 * Benchmark calls to the now() method of a clock source. With an argument of 0,
 * tests the system clock source, and with larger values, uses the FastClockSource
 * with a clock resolution of the specified number of milliseconds.
 *
 * All threads executing the benchmark use the same instance of the clock source,
 * to allow benchmarking to identify synchronization costs inside the now() method.
 */
void BM_ClockNow(benchmark::State& state) {
    static std::unique_ptr<ClockSource> clock;
    if (state.thread_index == 0) {
        if (state.range(0) > 0) {
            clock = FastClockSourceFactory::create(Milliseconds{state.range(0)});
        } else if (state.range(0) == 0) {
            clock = std::make_unique<SystemClockSource>();
        } else {
            state.SkipWithError("poll period must be non-negative");
            return;
        }
    }

    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(clock->now());
    }

    if (state.thread_index == 0) {
        clock.reset();
    }
}

BENCHMARK(BM_ClockNow)
    ->ThreadRange(1, ProcessInfo::getNumAvailableCores())
    ->ArgName("poll period")
    ->Arg(0)
    ->Arg(1)
    ->Arg(10);

}  // namespace
}  // namespace mongo
