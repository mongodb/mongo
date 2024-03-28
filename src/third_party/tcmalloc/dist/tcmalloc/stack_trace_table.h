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
//
// Utility class for coalescing sampled stack traces.  Not thread-safe.

#ifndef TCMALLOC_STACK_TRACE_TABLE_H_
#define TCMALLOC_STACK_TRACE_TABLE_H_



#include "absl/base/thread_annotations.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class StackTraceTable final : public ProfileBase {
 public:
  StackTraceTable(ProfileType type) ABSL_LOCKS_EXCLUDED(pageheap_lock);

  ~StackTraceTable() override ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // base::Profile methods.
  void Iterate(
      absl::FunctionRef<void(const Profile::Sample&)> func) const override;

  ProfileType Type() const override { return type_; }

  void SetDuration(absl::Duration duration) { duration_ = duration; }
  absl::Duration Duration() const override { return duration_; }

  // Adds stack trace "t" of the sample to table with the given weight of the
  // sample. `sample_weight` is a floating point value used to calculate the
  // the expected number of objects allocated (might be fractional considering
  // fragmentation) corresponding to a given sample.
  void AddTrace(double sample_weight, const StackTrace& t)
      ABSL_LOCKS_EXCLUDED(pageheap_lock);

  // Exposed for PageHeapAllocator
  struct LinkedSample {
    Profile::Sample sample;
    LinkedSample* next;
  };

  // For testing
  int depth_total() const { return depth_total_; }

 private:
  ProfileType type_;
  absl::Duration duration_ = absl::ZeroDuration();
  int depth_total_;
  LinkedSample* all_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_STACK_TRACE_TABLE_H_
