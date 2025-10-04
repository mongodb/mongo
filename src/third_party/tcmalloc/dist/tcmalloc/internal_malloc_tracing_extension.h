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

// Extra extensions exported by some malloc implementations.  These
// extensions are accessed through a virtual base class so an
// application can link against a malloc that does not implement these
// extensions, and it will get default versions that do nothing.

#ifndef TCMALLOC_INTERNAL_MALLOC_TRACING_EXTENSION_H_
#define TCMALLOC_INTERNAL_MALLOC_TRACING_EXTENSION_H_

#include "absl/base/attributes.h"
#include "absl/status/statusor.h"
#include "tcmalloc/malloc_tracing_extension.h"

#if ABSL_HAVE_ATTRIBUTE_WEAK && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)

ABSL_ATTRIBUTE_WEAK
absl::StatusOr<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges>
MallocTracingExtension_Internal_GetAllocatedAddressRanges();

#endif

#endif  // TCMALLOC_INTERNAL_MALLOC_TRACING_EXTENSION_H_
