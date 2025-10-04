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

#ifndef TCMALLOC_INTERNAL_CLOCK_H_
#define TCMALLOC_INTERNAL_CLOCK_H_

#include <stdint.h>

#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Represents an abstract clock. The now and freq functions are analogous to
// CycleClock::Now and CycleClock::Frequency, which will be the most commonly
// used implementations. Tests can use this interface to mock out the clock.
struct Clock {
  // Returns the current time in ticks (relative to an arbitrary time base).
  int64_t (*now)();

  // Returns the number of ticks per second.
  double (*freq)();
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_CLOCK_H_
