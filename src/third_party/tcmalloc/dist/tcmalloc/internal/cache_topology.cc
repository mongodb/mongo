// Copyright 2021 The TCMalloc Authors
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

#include "tcmalloc/internal/cache_topology.h"

#include <fcntl.h>
#include <string.h>

#include "absl/strings/numbers.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

namespace {
int OpenSysfsCacheList(size_t cpu) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path),
           "/sys/devices/system/cpu/cpu%zu/cache/index3/shared_cpu_list", cpu);
  return signal_safe_open(path, O_RDONLY | O_CLOEXEC);
}
}  // namespace

int BuildCpuToL3CacheMap_FindFirstNumberInBuf(absl::string_view current) {
  // Remove all parts coming after a dash or comma.
  const size_t dash = current.find('-');
  if (dash != absl::string_view::npos) current = current.substr(0, dash);
  const size_t comma = current.find(',');
  if (comma != absl::string_view::npos) current = current.substr(0, comma);

  int first_cpu;
  CHECK_CONDITION(absl::SimpleAtoi(current, &first_cpu));
  CHECK_CONDITION(first_cpu < CPU_SETSIZE);
  return first_cpu;
}

int BuildCpuToL3CacheMap(uint8_t l3_cache_index[CPU_SETSIZE]) {
  int index = 0;
  // Set to a sane value.
  memset(l3_cache_index, 0, CPU_SETSIZE);
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    const int fd = OpenSysfsCacheList(cpu);
    if (fd == -1) {
      // At some point we reach the number of CPU on the system, and
      // we should exit. We verify that there was no other problem.
      CHECK_CONDITION(errno == ENOENT);
      // For aarch64 if
      // /sys/devices/system/cpu/cpu*/cache/index3/shared_cpu_list is missing
      // then L3 is assumed to be shared by all CPUs.
      // TODO(b/210049384): find a better replacement for shared_cpu_list in
      // this case, e.g. based on numa nodes.
#ifdef __aarch64__
      if (index == 0) return 1;
#endif
      return index;
    }
    // The file contains something like:
    //   0-11,22-33
    // we are looking for the first number in that file.
    char buf[10];
    const size_t bytes_read =
        signal_safe_read(fd, buf, 10, /*bytes_read=*/nullptr);
    signal_safe_close(fd);
    CHECK_CONDITION(bytes_read >= 0);

    const int first_cpu =
        BuildCpuToL3CacheMap_FindFirstNumberInBuf({buf, bytes_read});
    CHECK_CONDITION(first_cpu < CPU_SETSIZE);
    CHECK_CONDITION(first_cpu <= cpu);
    if (cpu == first_cpu) {
      l3_cache_index[cpu] = index++;
    } else {
      l3_cache_index[cpu] = l3_cache_index[first_cpu];
    }
  }
  return index;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
