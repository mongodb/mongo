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


#include <cstdio>
#include <string>

#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/malloc_extension.h"

int main(int argc, char** argv) {
  std::string input = tcmalloc::MallocExtension::GetStats();

  if (absl::StrContains(
          input,
          "PARAMETER tcmalloc_separate_allocs_for_few_and_many_objects_spans "
          "1")) {
    printf("Active");
  } else {
    printf("Inactive");
  }

  return 0;
}
