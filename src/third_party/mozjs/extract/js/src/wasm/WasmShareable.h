/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_shareable_h
#define wasm_shareable_h

#include "mozilla/RefPtr.h"
#include "js/RefCounted.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace wasm {

// This reusable base class factors out the logic for a resource that is shared
// by multiple instances/modules but should only be counted once when computing
// about:memory stats.

template <class T>
using SeenSet = HashSet<const T*, DefaultHasher<const T*>, SystemAllocPolicy>;

template <class T>
struct ShareableBase : AtomicRefCounted<T> {
  using SeenSet = wasm::SeenSet<T>;

  size_t sizeOfIncludingThisIfNotSeen(mozilla::MallocSizeOf mallocSizeOf,
                                      SeenSet* seen) const {
    const T* self = static_cast<const T*>(this);
    typename SeenSet::AddPtr p = seen->lookupForAdd(self);
    if (p) {
      return 0;
    }
    bool ok = seen->add(p, self);
    (void)ok;  // oh well
    return mallocSizeOf(self) + self->sizeOfExcludingThis(mallocSizeOf);
  }
};

// ShareableBytes is a reference-counted Vector of bytes.

// Vector is 'final' and cannot be inherited to combine with ShareableBase, so
// we need to define a wrapper class with boilerplate methods.
template <typename T, size_t MinInlineCapacity, class AllocPolicy>
struct ShareableVector
    : public ShareableBase<ShareableVector<T, MinInlineCapacity, AllocPolicy>> {
  using VecT = mozilla::Vector<T, MinInlineCapacity, AllocPolicy>;

  VecT vector;

  size_t length() const { return vector.length(); }
  bool empty() const { return vector.empty(); }
  T* begin() { return vector.begin(); }
  T* end() { return vector.end(); }
  const T* begin() const { return vector.begin(); }
  const T* end() const { return vector.end(); }
  mozilla::Span<const T> span() const {
    return mozilla::Span<const T>(begin(), end());
  }
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return vector.sizeOfExcludingThis(mallocSizeOf);
  }
  bool append(const T* start, size_t len) { return vector.append(start, len); }
  bool appendAll(const VecT& other) { return vector.appendAll(other); }
  void shrinkTo(size_t len) { return vector.shrinkTo(len); }

  ShareableVector() = default;
  explicit ShareableVector(VecT&& vector) : vector(std::move(vector)) {}

  static const ShareableVector* fromSpan(mozilla::Span<const T> span) {
    ShareableVector* vector = js_new<ShareableVector>();
    if (!vector) {
      return nullptr;
    }

    // If we succeed in allocating the vector but fail to append something to
    // it, we need to delete this vector before returning.
    if (!vector->append(span.data(), span.size())) {
      js_free(vector);
      return nullptr;
    }

    return vector;
  }
};

using ShareableBytes = ShareableVector<uint8_t, 0, SystemAllocPolicy>;
using MutableBytes = RefPtr<ShareableBytes>;
using SharedBytes = RefPtr<const ShareableBytes>;

struct ShareableChars : public ShareableBase<ShareableChars> {
  UniqueChars chars;

  ShareableChars() = default;
  explicit ShareableChars(UniqueChars&& chars) : chars(std::move(chars)) {}
};

using SharedChars = RefPtr<const ShareableChars>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_shareable_h
