// Copyright 2020 The Abseil Authors
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

#ifndef ABSL_STRINGS_INTERNAL_CORD_REP_FLAT_H_
#define ABSL_STRINGS_INTERNAL_CORD_REP_FLAT_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "absl/strings/internal/cord_internal.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

// Note: all constants below are never ODR used and internal to cord, we define
// these as static constexpr to avoid 'in struct' definition and usage clutter.

// Largest and smallest flat node lengths we are willing to allocate
// Flat allocation size is stored in tag, which currently can encode sizes up
// to 4K, encoded as multiple of either 8 or 32 bytes.
// If we allow for larger sizes, we need to change this to 8/64, 16/128, etc.
// kMinFlatSize is bounded by tag needing to be at least FLAT * 8 bytes, and
// ideally a 'nice' size aligning with allocation and cacheline sizes like 32.
// kMaxFlatSize is bounded by the size resulting in a computed tag no greater
// than MAX_FLAT_TAG. MAX_FLAT_TAG provides for additional 'high' tag values.
static constexpr size_t kFlatOverhead = offsetof(CordRep, storage);
static constexpr size_t kMinFlatSize = 32;
static constexpr size_t kMaxFlatSize = 4096;
static constexpr size_t kMaxFlatLength = kMaxFlatSize - kFlatOverhead;
static constexpr size_t kMinFlatLength = kMinFlatSize - kFlatOverhead;

constexpr uint8_t AllocatedSizeToTagUnchecked(size_t size) {
  return static_cast<uint8_t>((size <= 1024) ? size / 8
                                             : 128 + size / 32 - 1024 / 32);
}

static_assert(kMinFlatSize / 8 >= FLAT, "");
static_assert(AllocatedSizeToTagUnchecked(kMaxFlatSize) <= MAX_FLAT_TAG, "");

// Helper functions for rounded div, and rounding to exact sizes.
constexpr size_t DivUp(size_t n, size_t m) { return (n + m - 1) / m; }
constexpr size_t RoundUp(size_t n, size_t m) { return DivUp(n, m) * m; }

// Returns the size to the nearest equal or larger value that can be
// expressed exactly as a tag value.
inline size_t RoundUpForTag(size_t size) {
  return RoundUp(size, (size <= 1024) ? 8 : 32);
}

// Converts the allocated size to a tag, rounding down if the size
// does not exactly match a 'tag expressible' size value. The result is
// undefined if the size exceeds the maximum size that can be encoded in
// a tag, i.e., if size is larger than TagToAllocatedSize(<max tag>).
inline uint8_t AllocatedSizeToTag(size_t size) {
  const uint8_t tag = AllocatedSizeToTagUnchecked(size);
  assert(tag <= MAX_FLAT_TAG);
  return tag;
}

// Converts the provided tag to the corresponding allocated size
constexpr size_t TagToAllocatedSize(uint8_t tag) {
  return (tag <= 128) ? (tag * 8) : (1024 + (tag - 128) * 32);
}

// Converts the provided tag to the corresponding available data length
constexpr size_t TagToLength(uint8_t tag) {
  return TagToAllocatedSize(tag) - kFlatOverhead;
}

// Enforce that kMaxFlatSize maps to a well-known exact tag value.
static_assert(TagToAllocatedSize(224) == kMaxFlatSize, "Bad tag logic");

struct CordRepFlat : public CordRep {
  // Creates a new flat node.
  static CordRepFlat* New(size_t len) {
    if (len <= kMinFlatLength) {
      len = kMinFlatLength;
    } else if (len > kMaxFlatLength) {
      len = kMaxFlatLength;
    }

    // Round size up so it matches a size we can exactly express in a tag.
    const size_t size = RoundUpForTag(len + kFlatOverhead);
    void* const raw_rep = ::operator new(size);
    CordRepFlat* rep = new (raw_rep) CordRepFlat();
    rep->tag = AllocatedSizeToTag(size);
    return rep;
  }

  // Deletes a CordRepFlat instance created previously through a call to New().
  // Flat CordReps are allocated and constructed with raw ::operator new and
  // placement new, and must be destructed and deallocated accordingly.
  static void Delete(CordRep*rep) {
    assert(rep->tag >= FLAT && rep->tag <= MAX_FLAT_TAG);

#if defined(__cpp_sized_deallocation)
    size_t size = TagToAllocatedSize(rep->tag);
    rep->~CordRep();
    ::operator delete(rep, size);
#else
    rep->~CordRep();
    ::operator delete(rep);
#endif
  }

  // Returns a pointer to the data inside this flat rep.
  char* Data() { return storage; }
  const char* Data() const { return storage; }

  // Returns the maximum capacity (payload size) of this instance.
  size_t Capacity() const { return TagToLength(tag); }

  // Returns the allocated size (payload + overhead) of this instance.
  size_t AllocatedSize() const { return TagToAllocatedSize(tag); }
};

// Now that CordRepFlat is defined, we can define CordRep's helper casts:
inline CordRepFlat* CordRep::flat() {
  assert(tag >= FLAT && tag <= MAX_FLAT_TAG);
  return reinterpret_cast<CordRepFlat*>(this);
}

inline const CordRepFlat* CordRep::flat() const {
  assert(tag >= FLAT && tag <= MAX_FLAT_TAG);
  return reinterpret_cast<const CordRepFlat*>(this);
}

}  // namespace cord_internal
ABSL_NAMESPACE_END
}  // namespace absl

#endif  // ABSL_STRINGS_INTERNAL_CORD_REP_FLAT_H_
