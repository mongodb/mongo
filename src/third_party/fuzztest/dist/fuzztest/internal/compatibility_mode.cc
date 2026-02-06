// Copyright 2022 Google LLC
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

#include "./fuzztest/internal/compatibility_mode.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "./fuzztest/internal/domains/domain.h"
#include "./fuzztest/internal/logging.h"

namespace fuzztest::internal {

#ifdef FUZZTEST_COMPATIBILITY_MODE

static ExternalEngineCallback* external_engine_callback = nullptr;

void SetExternalEngineCallback(ExternalEngineCallback* callback) {
  external_engine_callback = callback;
}

ExternalEngineCallback* GetExternalEngineCallback() {
  return external_engine_callback;
}

// libFuzzer-style custom mutator interface for external engine.
extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                          size_t max_size, unsigned int seed);

size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size, size_t max_size,
                               unsigned int seed) {
  ExternalEngineCallback* callback = GetExternalEngineCallback();
  FUZZTEST_INTERNAL_CHECK(
      callback,
      "External engine callback is unset while running the FuzzTest mutator.");
  const std::string mutated_data = callback->MutateData(
      absl::string_view(reinterpret_cast<const char*>(data), size), max_size,
      seed);
  if (mutated_data.size() > max_size) {
    absl::FPrintF(GetStderr(),
                  "Mutated data is larger than the limit. Returning the "
                  "original data.\n");
    return size;
  }
  memcpy(data, mutated_data.data(), mutated_data.size());
  return mutated_data.size();
}

FuzzTestExternalEngineAdaptor::FuzzTestExternalEngineAdaptor(
    const FuzzTest& test, std::unique_ptr<Driver> fixture_driver)
    : test_(test), fixture_driver_staging_(std::move(fixture_driver)) {}

bool FuzzTestExternalEngineAdaptor::RunInUnitTestMode(
    const Configuration& configuration) {
  return GetFuzzerImpl().RunInUnitTestMode(configuration);
}

bool FuzzTestExternalEngineAdaptor::RunInFuzzingMode(
    int* argc, char*** argv, const Configuration& configuration) {
  FUZZTEST_INTERNAL_CHECK(&LLVMFuzzerRunDriver,
                          "LibFuzzer Driver API not defined.");
  FUZZTEST_INTERNAL_CHECK(
      GetExternalEngineCallback() == nullptr,
      "External engine callback is already set while running a fuzz test.");
  SetExternalEngineCallback(this);
  runtime_.SetRunMode(RunMode::kFuzz);
  auto& impl = GetFuzzerImpl();
  runtime_.SetCurrentTest(&impl.test_, &configuration);
  runtime_.EnableReporter(&impl.stats_, [] { return absl::Now(); });

  FUZZTEST_INTERNAL_CHECK(impl.fixture_driver_ != nullptr,
                          "Invalid fixture driver!");
  impl.fixture_driver_->RunFuzzTest([&] {
    static bool driver_started = false;
    FUZZTEST_INTERNAL_CHECK(!driver_started, "Driver started more than once!");
    driver_started = true;
    LLVMFuzzerRunDriver(
        argc, argv, [](const uint8_t* data, size_t size) -> int {
          GetExternalEngineCallback()->RunOneInputData(
              absl::string_view(reinterpret_cast<const char*>(data), size));
          return 0;
        });
  });

  return true;
}

// External engine callbacks.

static bool IsEnginePlaceholderInput(absl::string_view data) {
  // https://github.com/llvm/llvm-project/blob/5840aa95e3c2d93f400e638e7cbf167a693c75f5/compiler-rt/lib/fuzzer/FuzzerLoop.cpp#L807
  if (data.size() == 0) return true;
  // https://github.com/llvm/llvm-project/blob/5840aa95e3c2d93f400e638e7cbf167a693c75f5/compiler-rt/lib/fuzzer/FuzzerLoop.cpp#L811
  if (data.size() == 1 && data[0] == '\n') return true;
  return false;
}

void FuzzTestExternalEngineAdaptor::RunOneInputData(absl::string_view data) {
  auto& impl = GetFuzzerImpl();
  if (impl.ShouldStop()) {
    runtime_.PrintFinalStatsOnDefaultSink();
    // Use _Exit instead of exit so libFuzzer does not treat it as a crash.
    std::_Exit(0);
  }
  if (IsEnginePlaceholderInput(data)) return;
  auto input = impl.TryParse(data);
  if (!input.ok()) return;
  impl.RunOneInput({*std::move(input)});
}

std::string FuzzTestExternalEngineAdaptor::MutateData(absl::string_view data,
                                                      size_t max_size,
                                                      unsigned int seed) {
  auto& impl = GetFuzzerImpl();
  typename FuzzerImpl::PRNG prng(seed);
  std::optional<GenericDomainCorpusType> input = std::nullopt;
  if (!IsEnginePlaceholderInput(data)) {
    auto parse_result = impl.TryParse(data);
    if (parse_result.ok()) input = *std::move(parse_result);
  }
  if (!input) input = impl.params_domain_.Init(prng);
  FUZZTEST_INTERNAL_CHECK(
      input.has_value(),
      "Both parsing and initiating the mutation input has failed.");
  constexpr int kNumAttempts = 10;
  std::string result;
  for (int i = 0; i < kNumAttempts; ++i) {
    auto copy = *input;
    for (int mutations_at_once = absl::Poisson<int>(prng) + 1;
         mutations_at_once > 0; --mutations_at_once) {
      impl.params_domain_.Mutate(copy, prng,
                                 /*only_shrink=*/max_size < data.size());
    }
    result = impl.params_domain_.SerializeCorpus(copy).ToString();
    if (result.size() <= max_size) break;
  }
  return result;
}

FuzzTestExternalEngineAdaptor::FuzzerImpl&
FuzzTestExternalEngineAdaptor::GetFuzzerImpl() {
  // Postpone the creation to override libFuzzer signal setup.
  if (!fuzzer_impl_) {
    fuzzer_impl_ =
        std::make_unique<FuzzerImpl>(test_, std::move(fixture_driver_staging_));
    fixture_driver_staging_ = nullptr;
  }
  return *fuzzer_impl_;
}

#endif  // FUZZTEST_COMPATIBILITY_MODE

}  // namespace fuzztest::internal
