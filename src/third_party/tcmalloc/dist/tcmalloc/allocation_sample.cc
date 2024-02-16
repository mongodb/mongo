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

#include "tcmalloc/allocation_sample.h"

#include <memory>

#include "absl/time/clock.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

AllocationSample::AllocationSample(AllocationSampleList* list, absl::Time start)
    : list_(list), start_(start) {
  mallocs_ = std::make_unique<StackTraceTable>(ProfileType::kAllocations);
  list->Add(this);
}

AllocationSample::~AllocationSample() {
  if (mallocs_ == nullptr) {
    return;
  }

  // deleted before ending profile, do it for them
  list_->Remove(this);
}

Profile AllocationSample::Stop() && {
  // We need to remove ourselves from list_ before we mutate mallocs_;
  //
  // A concurrent call to AllocationSampleList::ReportMalloc can access mallocs_
  // until we remove it from list_.
  if (mallocs_) {
    list_->Remove(this);
    mallocs_->SetDuration(absl::Now() - start_);
  }
  return ProfileAccessor::MakeProfile(std::move(mallocs_));
}

}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
