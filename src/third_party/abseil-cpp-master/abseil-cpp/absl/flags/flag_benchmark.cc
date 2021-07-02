//
// Copyright 2020 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>

#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/marshalling.h"
#include "absl/flags/parse.h"
#include "absl/flags/reflection.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "benchmark/benchmark.h"

namespace {
using String = std::string;
using VectorOfStrings = std::vector<std::string>;
using AbslDuration = absl::Duration;

// We do not want to take over marshalling for the types absl::optional<int>,
// absl::optional<std::string> which we do not own. Instead we introduce unique
// "aliases" to these types, which we do.
using AbslOptionalInt = absl::optional<int>;
struct OptionalInt : AbslOptionalInt {
  using AbslOptionalInt::AbslOptionalInt;
};
// Next two functions represent Abseil Flags marshalling for OptionalInt.
bool AbslParseFlag(absl::string_view src, OptionalInt* flag,
                   std::string* error) {
  int val;
  if (src.empty())
    flag->reset();
  else if (!absl::ParseFlag(src, &val, error))
    return false;
  *flag = val;
  return true;
}
std::string AbslUnparseFlag(const OptionalInt& flag) {
  return !flag ? "" : absl::UnparseFlag(*flag);
}

using AbslOptionalString = absl::optional<std::string>;
struct OptionalString : AbslOptionalString {
  using AbslOptionalString::AbslOptionalString;
};
// Next two functions represent Abseil Flags marshalling for OptionalString.
bool AbslParseFlag(absl::string_view src, OptionalString* flag,
                   std::string* error) {
  std::string val;
  if (src.empty())
    flag->reset();
  else if (!absl::ParseFlag(src, &val, error))
    return false;
  *flag = val;
  return true;
}
std::string AbslUnparseFlag(const OptionalString& flag) {
  return !flag ? "" : absl::UnparseFlag(*flag);
}

struct UDT {
  UDT() = default;
  UDT(const UDT&) {}
  UDT& operator=(const UDT&) { return *this; }
};
// Next two functions represent Abseil Flags marshalling for UDT.
bool AbslParseFlag(absl::string_view, UDT*, std::string*) { return true; }
std::string AbslUnparseFlag(const UDT&) { return ""; }

}  // namespace

#define BENCHMARKED_TYPES(A) \
  A(bool)                    \
  A(int16_t)                 \
  A(uint16_t)                \
  A(int32_t)                 \
  A(uint32_t)                \
  A(int64_t)                 \
  A(uint64_t)                \
  A(double)                  \
  A(float)                   \
  A(String)                  \
  A(VectorOfStrings)         \
  A(OptionalInt)             \
  A(OptionalString)          \
  A(AbslDuration)            \
  A(UDT)

#define FLAG_DEF(T) ABSL_FLAG(T, T##_flag, {}, "");

#if defined(__clang__) && defined(__linux__)
// Force the flags used for benchmarks into a separate ELF section.
// This ensures that, even when other parts of the code might change size,
// the layout of the flags across cachelines is kept constant. This makes
// benchmark results more reproducible across unrelated code changes.
#pragma clang section data = ".benchmark_flags"
#endif
BENCHMARKED_TYPES(FLAG_DEF)
#if defined(__clang__) && defined(__linux__)
#pragma clang section data = ""
#endif
// Register thousands of flags to bloat up the size of the registry.
// This mimics real life production binaries.
#define DEFINE_FLAG_0(name) ABSL_FLAG(int, name, 0, "");
#define DEFINE_FLAG_1(name) DEFINE_FLAG_0(name##0) DEFINE_FLAG_0(name##1)
#define DEFINE_FLAG_2(name) DEFINE_FLAG_1(name##0) DEFINE_FLAG_1(name##1)
#define DEFINE_FLAG_3(name) DEFINE_FLAG_2(name##0) DEFINE_FLAG_2(name##1)
#define DEFINE_FLAG_4(name) DEFINE_FLAG_3(name##0) DEFINE_FLAG_3(name##1)
#define DEFINE_FLAG_5(name) DEFINE_FLAG_4(name##0) DEFINE_FLAG_4(name##1)
#define DEFINE_FLAG_6(name) DEFINE_FLAG_5(name##0) DEFINE_FLAG_5(name##1)
#define DEFINE_FLAG_7(name) DEFINE_FLAG_6(name##0) DEFINE_FLAG_6(name##1)
#define DEFINE_FLAG_8(name) DEFINE_FLAG_7(name##0) DEFINE_FLAG_7(name##1)
#define DEFINE_FLAG_9(name) DEFINE_FLAG_8(name##0) DEFINE_FLAG_8(name##1)
#define DEFINE_FLAG_10(name) DEFINE_FLAG_9(name##0) DEFINE_FLAG_9(name##1)
#define DEFINE_FLAG_11(name) DEFINE_FLAG_10(name##0) DEFINE_FLAG_10(name##1)
#define DEFINE_FLAG_12(name) DEFINE_FLAG_11(name##0) DEFINE_FLAG_11(name##1)
DEFINE_FLAG_12(bloat_flag_);

namespace {

#define BM_GetFlag(T)                                            \
  void BM_GetFlag_##T(benchmark::State& state) {                 \
    for (auto _ : state) {                                       \
      benchmark::DoNotOptimize(absl::GetFlag(FLAGS_##T##_flag)); \
    }                                                            \
  }                                                              \
  BENCHMARK(BM_GetFlag_##T)->ThreadRange(1, 16);

BENCHMARKED_TYPES(BM_GetFlag)

void BM_ThreadedFindCommandLineFlag(benchmark::State& state) {
  char dummy[] = "dummy";
  char* argv[] = {dummy};
  // We need to ensure that flags have been parsed. That is where the registry
  // is finalized.
  absl::ParseCommandLine(1, argv);

  for (auto s : state) {
    benchmark::DoNotOptimize(
        absl::FindCommandLineFlag("bloat_flag_010101010101"));
  }
}
BENCHMARK(BM_ThreadedFindCommandLineFlag)->ThreadRange(1, 16);

}  // namespace

#define InvokeGetFlag(T)                                               \
  T AbslInvokeGetFlag##T() { return absl::GetFlag(FLAGS_##T##_flag); } \
  int odr##T = (benchmark::DoNotOptimize(AbslInvokeGetFlag##T), 1);

BENCHMARKED_TYPES(InvokeGetFlag)

// To veiw disassembly use: gdb ${BINARY}  -batch -ex "disassemble /s $FUNC"
