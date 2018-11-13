//
// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef ABSL_STRINGS_INTERNAL_RESIZE_UNINITIALIZED_H_
#define ABSL_STRINGS_INTERNAL_RESIZE_UNINITIALIZED_H_

#include <string>
#include <utility>

#include "absl/base/port.h"
#include "absl/meta/type_traits.h"  //  for void_t

namespace absl {
namespace strings_internal {

// Is a subclass of true_type or false_type, depending on whether or not
// T has a resize_uninitialized member.
template <typename T, typename = void>
struct HasResizeUninitialized : std::false_type {};
template <typename T>
struct HasResizeUninitialized<
    T, absl::void_t<decltype(std::declval<T>().resize_uninitialized(237))>>
    : std::true_type {};

template <typename string_type>
void ResizeUninit(string_type* s, size_t new_size, std::true_type) {
  s->resize_uninitialized(new_size);
}
template <typename string_type>
void ResizeUninit(string_type* s, size_t new_size, std::false_type) {
  s->resize(new_size);
}

// Returns true if the string implementation supports a resize where
// the new characters added to the string are left untouched.
//
// (A better name might be "STLStringSupportsUninitializedResize", alluding to
// the previous function.)
template <typename string_type>
inline constexpr bool STLStringSupportsNontrashingResize(string_type*) {
  return HasResizeUninitialized<string_type>();
}

// Like str->resize(new_size), except any new characters added to "*str" as a
// result of resizing may be left uninitialized, rather than being filled with
// '0' bytes. Typically used when code is then going to overwrite the backing
// store of the string with known data. Uses a Google extension to ::string.
template <typename string_type, typename = void>
inline void STLStringResizeUninitialized(string_type* s, size_t new_size) {
  ResizeUninit(s, new_size, HasResizeUninitialized<string_type>());
}

}  // namespace strings_internal
}  // namespace absl

#endif  // ABSL_STRINGS_INTERNAL_RESIZE_UNINITIALIZED_H_
