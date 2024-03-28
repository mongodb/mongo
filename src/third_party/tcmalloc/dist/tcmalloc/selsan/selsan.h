// Copyright 2024 The TCMalloc Authors
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

#ifndef TCMALLOC_SELSAN_SELSAN_H_
#define TCMALLOC_SELSAN_SELSAN_H_

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal::selsan {

#ifdef TCMALLOC_INTERNAL_SELSAN

bool IsEnabled();
void PrintTextStats(Printer* out);
void PrintPbtxtStats(PbtxtRegion* out);

#else  // #ifdef TCMALLOC_INTERNAL_SELSAN

inline bool IsEnabled() { return false; }
inline void PrintTextStats(Printer* out) {}
inline void PrintPbtxtStats(PbtxtRegion* out) {}

#endif  // #ifdef TCMALLOC_INTERNAL_SELSAN

}  // namespace tcmalloc::tcmalloc_internal::selsan
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SELSAN_SELSAN_H_
