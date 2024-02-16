// Copyright 2022 The TCMalloc Authors
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

#include <algorithm>
#include <vector>

#include "absl/log/check.h"
#include "tcmalloc/common.h"
#include "tcmalloc/size_class_info.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/tcmalloc_policy.h"

using tcmalloc::tcmalloc_internal::CppPolicy;
using tcmalloc::tcmalloc_internal::kMaxSize;
using tcmalloc::tcmalloc_internal::kNumClasses;
using tcmalloc::tcmalloc_internal::SizeClassInfo;
using tcmalloc::tcmalloc_internal::SizeMap;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const SizeClassInfo* info;
  if (size % sizeof(*info) != 0) {
    return 0;
  }
  info = reinterpret_cast<const SizeClassInfo*>(data);

  SizeMap m;
  if (!m.Init(absl::MakeSpan(info, size / sizeof(*info)))) {
    return 0;
  }

  // Validate that every size on [0, kMaxSize] maps to a size class that is
  // neither too big nor too small.
  int last_size_class = -1;
  for (size_t size = 0; size <= kMaxSize; size++) {
    const int size_class = m.SizeClass(CppPolicy(), size);
    CHECK_GT(size_class, 0) << size;
    CHECK_LT(size_class, kNumClasses) << size;

    const size_t s = m.class_to_size(size_class);
    CHECK_LE(size, s);
    CHECK_NE(s, 0) << size;

    CHECK_LE(last_size_class, size_class);
    last_size_class = size_class;
  }

  return 0;
}
