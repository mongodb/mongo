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
 *    linked combinations including the prograxm with the OpenSSL library. You
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

namespace {

// This is a trivial test case to sanity check that "benchmark" runs.
// For more information on how benchmarks should be written, please refer to Google Benchmark's
// excellent README: https://github.com/google/benchmark/blob/v1.3.0/README.md
void BM_empty(benchmark::State& state) {
    for (auto keepRunning : state) {
        // The code inside this for-loop is what's being timed.
        benchmark::DoNotOptimize(state.iterations());
    }
}

// Register two benchmarks, one runs the "BM_empty" function in a single thread, the other runs a
// copy per CPU core.
BENCHMARK(BM_empty);
BENCHMARK(BM_empty)->ThreadPerCpu();

}  // namespace
