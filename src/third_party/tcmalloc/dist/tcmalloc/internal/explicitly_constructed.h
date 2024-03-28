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

#ifndef TCMALLOC_INTERNAL_EXPLICITLY_CONSTRUCTED_H_
#define TCMALLOC_INTERNAL_EXPLICITLY_CONSTRUCTED_H_

#include <stdint.h>

#include <utility>

#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Wraps a variable whose constructor is explicitly called. It is particularly
// useful for a global variable, without its constructor and destructor run on
// start and end of the program lifetime.  This circumvents the initial
// construction order fiasco, while keeping the address of the empty string a
// compile time constant.
//
// Pay special attention to the initialization state of the object.
// 1. The object is "uninitialized" to begin with.
// 2. Call Construct() only if the object is uninitialized. After the call, the
//    object becomes "initialized".
// 3. Call get_mutable() only if the object is initialized.
template <typename T>
class ExplicitlyConstructed {
 public:
  template <typename... Args>
  void Construct(Args&&... args) {
    new (&union_) T(std::forward<Args>(args)...);
  }

  T& get_mutable() { return reinterpret_cast<T&>(union_); }

 private:
  union AlignedUnion {
    constexpr AlignedUnion() = default;
    alignas(T) char space[sizeof(T)];
    int64_t align_to_int64;
    void* align_to_ptr;
  } union_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_EXPLICITLY_CONSTRUCTED_H_
