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

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "benchmark/benchmark.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace {

// Benchmark performance of mincore. We use an array on the stack to gather
// mincore data. The larger the array the more we amortise the cost of calling
// mincore, but the more stack space the array takes up.
void BM_mincore(benchmark::State& state) {
  const int size = state.range(0);

  // If we want to place the array on the stack then the maximum frame size is
  // 16KiB. So there is no point in benchmarking sizes larger than this.
  const int kMaxArraySize = 16 * 1024;
  TC_CHECK_LE(size, kMaxArraySize);
  auto resident = std::make_unique<unsigned char[]>(kMaxArraySize);

  const size_t kPageSize = tcmalloc_internal::GetPageSize();
  // We want to scan the same amount of memory in all cases
  const size_t regionSize = 1 * 1024 * 1024 * 1024;
  for (auto s : state) {
    uintptr_t memory = 0;
    while (memory < regionSize) {
      // Call mincore for the next section
      int length = std::min(size * kPageSize, (regionSize - memory));
      ::mincore(reinterpret_cast<void*>(memory), length, resident.get());
      memory += length * kPageSize;
    }
  }
}
BENCHMARK(BM_mincore)->Range(1, 16 * 1024);

}  // namespace
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
