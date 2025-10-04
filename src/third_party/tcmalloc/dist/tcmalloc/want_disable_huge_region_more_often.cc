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

#include "absl/base/attributes.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// This - if linked into a binary - allows huge-region-more-often feature to be
// disabled.
extern "C" ABSL_ATTRIBUTE_UNUSED bool
default_want_disable_huge_region_more_often() {
  return true;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
