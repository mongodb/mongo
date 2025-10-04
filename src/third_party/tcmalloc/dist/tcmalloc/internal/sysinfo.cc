// Copyright 2023 The TCMalloc Authors
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

#include "tcmalloc/internal/sysinfo.h"

#include <fcntl.h>
#include <sched.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <optional>

#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

#if __linux__
namespace {
bool IsInBounds(int cpu) { return 0 <= cpu && cpu < CPU_SETSIZE; }
}  // namespace

std::optional<cpu_set_t> ParseCpulist(
    absl::FunctionRef<ssize_t(char*, size_t)> read) {
  cpu_set_t set;
  CPU_ZERO(&set);

  std::array<char, 16> buf;
  size_t carry_over = 0;
  int cpu_from = -1;

  while (true) {
    const ssize_t rc = read(buf.data() + carry_over, buf.size() - carry_over);
    if (ABSL_PREDICT_FALSE(rc < 0)) {
      return std::nullopt;
    }

    const absl::string_view current(buf.data(), carry_over + rc);

    // If we have no more data to parse & couldn't read any then we've reached
    // the end of the input & are done.
    if (current.empty() && rc == 0) {
      break;
    }
    if (current == "\n" && rc == 0) {
      break;
    }

    size_t consumed;
    const size_t dash = current.find('-');
    const size_t comma = current.find(',');
    if (dash != absl::string_view::npos && dash < comma) {
      if (!absl::SimpleAtoi(current.substr(0, dash), &cpu_from) ||
          !IsInBounds(cpu_from)) {
        return std::nullopt;
      }
      consumed = dash + 1;
    } else if (comma != absl::string_view::npos || rc == 0) {
      int cpu;
      if (!absl::SimpleAtoi(current.substr(0, comma), &cpu) ||
          !IsInBounds(cpu)) {
        return std::nullopt;
      }
      if (comma == absl::string_view::npos) {
        consumed = current.size();
      } else {
        consumed = comma + 1;
      }
      if (cpu_from != -1) {
        for (int c = cpu_from; c <= cpu; c++) {
          CPU_SET(c, &set);
        }
        cpu_from = -1;
      } else {
        CPU_SET(cpu, &set);
      }
    } else {
      consumed = 0;
    }

    carry_over = current.size() - consumed;
    memmove(buf.data(), buf.data() + consumed, carry_over);
  }

  return set;
}

namespace sysinfo_internal {

int NumPossibleCPUsNoCache() {
  int fd = signal_safe_open("/sys/devices/system/cpu/possible",
                            O_RDONLY | O_CLOEXEC);

  // This is slightly more state than we actually need, but it lets us reuse
  // an already fuzz tested implementation detail.
  std::optional<cpu_set_t> cpus =
      ParseCpulist([&](char* const buf, const size_t count) {
        return signal_safe_read(fd, buf, count, /*bytes_read=*/nullptr);
      });

  signal_safe_close(fd);

  TC_CHECK(cpus.has_value());
  std::optional<int> max_so_far;
  for (int i = 0; i < CPU_SETSIZE; ++i) {
    if (CPU_ISSET(i, &*cpus)) {
      max_so_far = std::max(i, max_so_far.value_or(-1));
    }
  }
  TC_CHECK(max_so_far.has_value());
  return *max_so_far + 1;
}

}  // namespace sysinfo_internal

#endif  // __linux__

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
