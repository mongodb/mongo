// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/time/civil_time.h"

#include "benchmark/benchmark.h"

namespace {

// Benchmark                            Time(ns)       CPU(ns)    Iterations
// -------------------------------------------------------------------------
// BM_Difference_Days                         20            20      34542508
// BM_Step_Days                               15            15      48098146
// BM_Format                                 688           687       1019803
// BM_Parse                                  921           920        762788
// BM_RoundTripFormatParse                  1766          1764        396092

void BM_Difference_Days(benchmark::State& state) {
  const absl::CivilDay c(2014, 8, 22);
  const absl::CivilDay epoch(1970, 1, 1);
  while (state.KeepRunning()) {
    const absl::civil_diff_t n = c - epoch;
    benchmark::DoNotOptimize(n);
  }
}
BENCHMARK(BM_Difference_Days);

void BM_Step_Days(benchmark::State& state) {
  const absl::CivilDay kStart(2014, 8, 22);
  absl::CivilDay c = kStart;
  while (state.KeepRunning()) {
    benchmark::DoNotOptimize(++c);
  }
}
BENCHMARK(BM_Step_Days);

void BM_Format(benchmark::State& state) {
  const absl::CivilSecond c(2014, 1, 2, 3, 4, 5);
  while (state.KeepRunning()) {
    const std::string s = absl::FormatCivilTime(c);
    benchmark::DoNotOptimize(s);
  }
}
BENCHMARK(BM_Format);

}  // namespace
