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
#include "tcmalloc/internal/environment.h"

#include <string.h>

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// POSIX provides the **environ array which contains environment variables in a
// linear array, terminated by a NULL string.  This array is only perturbed when
// the environment is changed (which is inherently unsafe) so it's safe to
// return a const pointer into it.
// e.g. { "SHELL=/bin/bash", "MY_ENV_VAR=1", "" }
extern "C" char** environ;
const char* thread_safe_getenv(const char* env_var) {
  int var_len = strlen(env_var);

  char** envv = environ;
  if (!envv) {
    return nullptr;
  }

  for (; *envv != nullptr; envv++)
    if (strncmp(*envv, env_var, var_len) == 0 && (*envv)[var_len] == '=')
      return *envv + var_len + 1;  // skip over the '='

  return nullptr;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
