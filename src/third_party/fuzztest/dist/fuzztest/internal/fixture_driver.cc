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

#include "./fuzztest/internal/fixture_driver.h"

#include <utility>

#include "absl/functional/any_invocable.h"

namespace fuzztest::internal {

UntypedFixtureDriver::~UntypedFixtureDriver() = default;
void UntypedFixtureDriver::RunFuzzTest(absl::AnyInvocable<void() &&> run_test) {
  std::move(run_test)();
}
void UntypedFixtureDriver::RunFuzzTestIteration(
    absl::AnyInvocable<void() &&> run_iteration) {
  std::move(run_iteration)();
}

}  // namespace fuzztest::internal
