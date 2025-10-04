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

#ifndef TCMALLOC_INTERNAL_ENVIRONMENT_H_
#define TCMALLOC_INTERNAL_ENVIRONMENT_H_

#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// WARNING ********************************************************************
// getenv(2) can only be safely used in the absence of calls which perturb the
// environment (e.g. putenv/setenv/clearenv).  The use of such calls is
// strictly thread-hostile since these calls do *NOT* synchronize and there is
// *NO* thread-safe way in which the POSIX **environ array may be queried about
// modification.
// ****************************************************************************
// The default getenv(2) is not guaranteed to be thread-safe as there are no
// semantics specifying the implementation of the result buffer.  The result
// from thread_safe_getenv() may be safely queried in a multi-threaded context.
// If you have explicit synchronization with changes environment variables then
// any copies of the returned pointer must be invalidated across modification.
const char* thread_safe_getenv(const char* env_var);

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_ENVIRONMENT_H_
