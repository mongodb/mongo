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

using mozilla::MallocSizeOf;

// This reusable base class factors out the logic for a resource that is shared
// by multiple instances/modules but should only be counted once when computing
// about:memory stats.

template <class T>
struct ShareableBase : AtomicRefCounted<T> {
  using SeenSet = HashSet<const T*, DefaultHasher<const T*>, SystemAllocPolicy>;

  size_t sizeOfIncludingThisIfNotSeen(MallocSizeOf mallocSizeOf,
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

struct ShareableBytes : ShareableBase<ShareableBytes> {
  // Vector is 'final', so instead make Vector a member and add boilerplate.
  Bytes bytes;

  ShareableBytes() = default;
  explicit ShareableBytes(Bytes&& bytes) : bytes(std::move(bytes)) {}
  size_t sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
    return bytes.sizeOfExcludingThis(mallocSizeOf);
  }
  const uint8_t* begin() const { return bytes.begin(); }
  const uint8_t* end() const { return bytes.end(); }
  size_t length() const { return bytes.length(); }
  bool append(const uint8_t* start, size_t len) {
    return bytes.append(start, len);
  }
};

using MutableBytes = RefPtr<ShareableBytes>;
using SharedBytes = RefPtr<const ShareableBytes>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_shareable_h
