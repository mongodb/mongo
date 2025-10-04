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

#ifndef TCMALLOC_INTERNAL_FAKE_PROFILE_H_
#define TCMALLOC_INTERNAL_FAKE_PROFILE_H_

#include <utility>
#include <vector>

#include "absl/functional/function_ref.h"
#include "absl/time/time.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class FakeProfile final : public ProfileBase {
 public:
  void SetSamples(std::vector<Profile::Sample> samples) {
    samples_ = std::move(samples);
  }

  // For each sample in the profile, Iterate invokes the callback f on the
  // sample.
  void Iterate(
      absl::FunctionRef<void(const Profile::Sample&)> f) const override {
    for (const auto& sample : samples_) {
      f(sample);
    }
  }

  // The type of profile (live objects, allocated, etc.).
  ProfileType Type() const override { return type_; }
  void SetType(ProfileType type) { type_ = type; }

  // The duration of the profile
  absl::Duration Duration() const override { return duration_; }
  void SetDuration(absl::Duration duration) { duration_ = duration; }

 private:
  std::vector<Profile::Sample> samples_;
  ProfileType type_;
  absl::Duration duration_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_FAKE_PROFILE_H_
