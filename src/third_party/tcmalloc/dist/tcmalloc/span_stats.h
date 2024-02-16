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

#ifndef TCMALLOC_SPAN_STATS_H_
#define TCMALLOC_SPAN_STATS_H_

#include <stddef.h>

#include "absl/base/macros.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct SpanStats {
  size_t num_spans_requested = 0;
  size_t num_spans_returned = 0;
  size_t obj_capacity = 0;  // cap of number of objs that could be live anywhere

  size_t num_live_spans() {
    if (num_spans_requested < num_spans_returned) {
      return 0;
    }
    return num_spans_requested - num_spans_returned;
  }

  // Probability that a span will be returned
  double prob_returned() {
    if (ABSL_PREDICT_FALSE(num_spans_requested == 0)) return 0.0;
    return static_cast<double>(num_spans_returned) / num_spans_requested;
  }
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_SPAN_STATS_H_
