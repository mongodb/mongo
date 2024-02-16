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
#include <string>

#include "absl/base/macros.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

const char kDelimiter = ',';
const char kExperiments[] = "BORG_EXPERIMENTS";
const char kDisableExperiments[] = "BORG_DISABLE_EXPERIMENTS";
constexpr absl::string_view kEnableAll = "enable-all-known-experiments";
constexpr absl::string_view kDisableAll = "all";

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
  static bool by_id[kNumExperiments];

  static const bool* status = [&]() {
    const char* active_experiments = thread_safe_getenv(kExperiments);
    const char* disabled_experiments = thread_safe_getenv(kDisableExperiments);
    return SelectExperiments(by_id,
                             active_experiments ? active_experiments : "",
                             disabled_experiments ? disabled_experiments : "");
  }();
  return status;
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

const bool* SelectExperiments(bool* buffer, absl::string_view active,
                              absl::string_view disabled) {
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

  if (disabled == kDisableAll) {
    memset(buffer, 0, sizeof(*buffer) * kNumExperiments);
  }

  ParseExperiments(disabled, [buffer](absl::string_view token) {
    Experiment id;
    if (LookupExperimentID(token, &id)) {
      buffer[static_cast<int>(id)] = false;
    }
  });

  return buffer;
}

}  // namespace tcmalloc_internal

bool IsExperimentActive(Experiment exp) {
  ASSERT(static_cast<int>(exp) >= 0);
  ASSERT(exp < Experiment::kMaxExperimentID);

  return tcmalloc_internal::GetSelectedExperiments()[static_cast<int>(exp)];
}

absl::optional<Experiment> FindExperimentByName(absl::string_view name) {
  for (const auto& config : experiments) {
    if (name == config.name) {
      return config.id;
    }
  }

  return absl::nullopt;
}

void WalkExperiments(
    absl::FunctionRef<void(absl::string_view name, bool active)> callback) {
  for (const auto& config : experiments) {
    callback(config.name, IsExperimentActive(config.id));
  }
}

}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
