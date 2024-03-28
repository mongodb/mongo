// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/experiment.h"

#include <string.h>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

const char kDelimiter = ',';
const char kExperiments[] = "BORG_EXPERIMENTS";
const char kDisableExperiments[] = "BORG_DISABLE_EXPERIMENTS";
constexpr absl::string_view kEnableAll = "enable-all-known-experiments";
constexpr absl::string_view kDisableAll = "all";

// Experiments that have known issues with brittle tests, are not enabled
// involuntarily in tests, and shouldn't be enabled widely.
bool HasBrittleTestFailures(Experiment exp) {

  if (exp == Experiment::TEST_ONLY_TCMALLOC_POW2_SIZECLASS) {
    return true;
  }

  if (exp == Experiment::TEST_ONLY_TCMALLOC_LOWFRAG_SIZECLASSES) {
    return true;
  }

  return false;
}

bool IsCompilerExperiment(Experiment exp) {
#ifdef NPX_COMPILER_ENABLED_EXPERIMENT
  return exp == Experiment::NPX_COMPILER_EXPERIMENT;
#else
  return false;
#endif
}

bool LookupExperimentID(absl::string_view label, Experiment* exp) {
  for (auto config : experiments) {
    if (config.name == label) {
      *exp = config.id;
      return true;
    }
  }

  return false;
}

const bool* GetSelectedExperiments() {
  ABSL_CONST_INIT static bool by_id[kNumExperiments];
  ABSL_CONST_INIT static absl::once_flag flag;

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    const char* test_target = thread_safe_getenv("TEST_TARGET");
    const char* active_experiments = thread_safe_getenv(kExperiments);
    const char* disabled_experiments = thread_safe_getenv(kDisableExperiments);
    SelectExperiments(
        by_id, test_target ? test_target : "",
        active_experiments ? active_experiments : "",
        disabled_experiments ? disabled_experiments : "",
        active_experiments == nullptr && disabled_experiments == nullptr);
  });
  return by_id;
}

template <typename F>
void ParseExperiments(absl::string_view labels, F f) {
  absl::string_view::size_type pos = 0;
  do {
    absl::string_view token;
    auto end = labels.find(kDelimiter, pos);
    if (end == absl::string_view::npos) {
      token = labels.substr(pos);
      pos = end;
    } else {
      token = labels.substr(pos, end - pos);
      pos = end + 1;
    }

    f(token);
  } while (pos != absl::string_view::npos);
}

}  // namespace

const bool* SelectExperiments(bool* buffer, absl::string_view test_target,
                              absl::string_view active,
                              absl::string_view disabled, bool unset) {
  memset(buffer, 0, sizeof(*buffer) * kNumExperiments);

  if (active == kEnableAll) {
    std::fill(buffer, buffer + kNumExperiments, true);
  }

  ParseExperiments(active, [buffer](absl::string_view token) {
    Experiment id;
    if (LookupExperimentID(token, &id)) {
      buffer[static_cast<int>(id)] = true;
    }
  });

  // The compiler experiments should be env variable independent.
#ifdef NPX_COMPILER_ENABLED_EXPERIMENT
  if (!absl::StrContains(active, NPX_COMPILER_ENABLED_EXPERIMENT)) {
    Experiment id;
    if (LookupExperimentID(NPX_COMPILER_ENABLED_EXPERIMENT, &id)) {
      buffer[static_cast<int>(id)] = true;
    }
  }
#endif

  if (disabled == kDisableAll) {
    for (auto config : experiments) {
      // Exclude compile-time experiments
      if (!IsCompilerExperiment(config.id)) {
        buffer[static_cast<int>(config.id)] = false;
      }
    }
  }

  // disable non-compiler experiments
  ParseExperiments(disabled, [buffer](absl::string_view token) {
    Experiment id;
    if (LookupExperimentID(token, &id) && !IsCompilerExperiment(id)) {
      buffer[static_cast<int>(id)] = false;
    }
  });

  // Enable some random combination of experiments for tests that don't
  // explicitly set any of the experiment env vars. This allows to get better
  // test coverage of experiments before production.
  // Tests can opt out by exporting BORG_EXPERIMENTS="".
  // Enabled experiments are selected based on the stable test target name hash,
  // this allows get a wide range of experiment permutations on a large test
  // base, but at the same time avoids flaky test failures (if a particular
  // test fails only with a particular experiment combination).
  // It would be nice to print what experiments we enable, but printing even
  // to stderr breaks some tests that capture subprocess output.
  if (unset && !test_target.empty()) {
    TC_CHECK(active.empty() && disabled.empty());
    const size_t target_hash = std::hash<std::string_view>{}(test_target);
    constexpr size_t kVanillaOneOf = 10;
    constexpr size_t kEnableOneOf = 3;
    if ((target_hash % kVanillaOneOf) == 0) {
      return buffer;
    }

    for (auto config : experiments) {
      if (IsCompilerExperiment(config.id) ||
          HasBrittleTestFailures(config.id)) {
        continue;
      }
      TC_CHECK(!buffer[static_cast<int>(config.id)]);
      // Enabling is specifically based on the experiment name so that it's
      // stable when experiments are added/removed.
      buffer[static_cast<int>(config.id)] =
          ((target_hash ^ std::hash<std::string_view>{}(config.name)) %
           kEnableOneOf) == 0;
    }
  }

  return buffer;
}

}  // namespace tcmalloc_internal

bool IsExperimentActive(Experiment exp) {
  TC_ASSERT_GE(static_cast<int>(exp), 0);
  TC_ASSERT_LT(exp, Experiment::kMaxExperimentID);

  return tcmalloc_internal::GetSelectedExperiments()[static_cast<int>(exp)];
}

std::optional<Experiment> FindExperimentByName(absl::string_view name) {
  for (const auto& config : experiments) {
    if (name == config.name) {
      return config.id;
    }
  }

  return std::nullopt;
}

void WalkExperiments(
    absl::FunctionRef<void(absl::string_view name, bool active)> callback) {
  for (const auto& config : experiments) {
    callback(config.name, IsExperimentActive(config.id));
  }
}

extern "C" void MallocExtension_Internal_GetExperiments(
    std::map<std::string, MallocExtension::Property>* result) {
  WalkExperiments([&](absl::string_view name, bool active) {
    (*result)[absl::StrCat("tcmalloc.experiment.", name)].value = active;
  });
}

}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
