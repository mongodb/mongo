// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GOOGLE_PROTOBUF_ARENA_CLEANUP_H__
#define GOOGLE_PROTOBUF_ARENA_CLEANUP_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/strings/cord.h"


// Must be included last.
#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace internal {
namespace cleanup {

// Helper function invoking the destructor of `object`
template <typename T>
void arena_destruct_object(void* object) {
  reinterpret_cast<T*>(object)->~T();
}

// Tag defines the type of cleanup / cleanup object. This tag is stored in the
// lowest 2 bits of the `elem` value identifying the type of node. All node
// types must start with a `uintptr_t` that stores `Tag` in its low two bits.
enum class Tag : uintptr_t {
  kDynamic = 0,  // DynamicNode
  kString = 1,   // TaggedNode (std::string)
  kCord = 2,     // TaggedNode (absl::Cord)
};

// DynamicNode contains the object (`elem`) that needs to be
// destroyed, and the function to destroy it (`destructor`)
// elem must be aligned at minimum on a 4 byte boundary.
struct DynamicNode {
  uintptr_t elem;
  void (*destructor)(void*);
};

// TaggedNode contains a `std::string` or `absl::Cord` object (`elem`) that
// needs to be destroyed. The lowest 2 bits of `elem` contain the non-zero
// `kString` or `kCord` tag.
struct TaggedNode {
  uintptr_t elem;
};

// EnableSpecializedTags() return true if the alignment of tagged objects
// such as std::string allow us to poke tags in the 2 LSB bits.
inline constexpr bool EnableSpecializedTags() {
  // For now we require 2 bits
  return alignof(std::string) >= 8 && alignof(absl::Cord) >= 8;
}

// Adds a cleanup entry identified by `tag` at memory location `pos`.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void CreateNode(Tag tag, void* pos,
                                                    const void* elem_raw,
                                                    void (*destructor)(void*)) {
  auto elem = reinterpret_cast<uintptr_t>(elem_raw);
  if (EnableSpecializedTags()) {
    ABSL_DCHECK_EQ(elem & 3, 0ULL);  // Must be aligned
    switch (tag) {
      case Tag::kString: {
        TaggedNode n = {elem | static_cast<uintptr_t>(Tag::kString)};
        memcpy(pos, &n, sizeof(n));
        return;
      }
      case Tag::kCord: {
        TaggedNode n = {elem | static_cast<uintptr_t>(Tag::kCord)};
        memcpy(pos, &n, sizeof(n));
        return;
      }

      case Tag::kDynamic:
      default:
        break;
    }
  }
  DynamicNode n = {elem, destructor};
  memcpy(pos, &n, sizeof(n));
}

// Optimization: performs a prefetch on `elem_address`.
// Returns the size of the cleanup (meta) data at this address, allowing the
// caller to advance cleanup iterators without needing to examine or know
// anything about the underlying cleanup node or cleanup meta data / tags.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE size_t
PrefetchNode(const void* elem_address) {
  if (EnableSpecializedTags()) {
    uintptr_t elem;
    memcpy(&elem, elem_address, sizeof(elem));
    if (static_cast<Tag>(elem & 3) != Tag::kDynamic) {
      return sizeof(TaggedNode);
    }
  }
  return sizeof(DynamicNode);
}

// Destroys the object referenced by the cleanup node at memory location `pos`.
// Returns the size of the cleanup (meta) data at this address, allowing the
// caller to advance cleanup iterators without needing to examine or know
// anything about the underlying cleanup node or cleanup meta data / tags.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE size_t DestroyNode(const void* pos) {
  uintptr_t elem;
  memcpy(&elem, pos, sizeof(elem));
  if (EnableSpecializedTags()) {
    switch (static_cast<Tag>(elem & 3)) {
      case Tag::kString: {
        // Some compilers don't like fully qualified explicit dtor calls,
        // so use an alias to avoid having to type `::`.
        using T = std::string;
        reinterpret_cast<T*>(elem - static_cast<uintptr_t>(Tag::kString))->~T();
        return sizeof(TaggedNode);
      }
      case Tag::kCord: {
        using T = absl::Cord;
        reinterpret_cast<T*>(elem - static_cast<uintptr_t>(Tag::kCord))->~T();
        return sizeof(TaggedNode);
      }

      case Tag::kDynamic:

      default:
        break;
    }
  }
  static_cast<const DynamicNode*>(pos)->destructor(
      reinterpret_cast<void*>(elem - static_cast<uintptr_t>(Tag::kDynamic)));
  return sizeof(DynamicNode);
}

// Append in `out` the pointer to the to-be-cleaned object in `pos`.
// Return the length of the cleanup node to allow the caller to advance the
// position, like `DestroyNode` does.
inline size_t PeekNode(const void* pos, std::vector<void*>& out) {
  uintptr_t elem;
  memcpy(&elem, pos, sizeof(elem));
  out.push_back(reinterpret_cast<void*>(elem & ~3));
  if (EnableSpecializedTags()) {
    switch (static_cast<Tag>(elem & 3)) {
      case Tag::kString:
      case Tag::kCord:
        return sizeof(TaggedNode);

      case Tag::kDynamic:
      default:
        break;
    }
  }
  return sizeof(DynamicNode);
}

// Returns the `tag` identifying the type of object for `destructor` or
// kDynamic if `destructor` does not identify a well know object type.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE Tag Type(void (*destructor)(void*)) {
  if (EnableSpecializedTags()) {
    if (destructor == &arena_destruct_object<std::string>) {
      return Tag::kString;
    }
    if (destructor == &arena_destruct_object<absl::Cord>) {
      return Tag::kCord;
    }
  }
  return Tag::kDynamic;
}

// Returns the required size in bytes off the node type identified by `tag`.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE size_t Size(Tag tag) {
  if (!EnableSpecializedTags()) return sizeof(DynamicNode);

  switch (tag) {
    case Tag::kDynamic:
      return sizeof(DynamicNode);
    case Tag::kString:
      return sizeof(TaggedNode);
    case Tag::kCord:
      return sizeof(TaggedNode);
    default:
      ABSL_DCHECK(false) << "Corrupted cleanup tag: " << static_cast<int>(tag);
      return sizeof(DynamicNode);
  }
}

// Returns the required size in bytes off the node type for `destructor`.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE size_t Size(void (*destructor)(void*)) {
  return destructor == nullptr ? 0 : Size(Type(destructor));
}

}  // namespace cleanup
}  // namespace internal
}  // namespace protobuf
}  // namespace google

#include "google/protobuf/port_undef.inc"

#endif  // GOOGLE_PROTOBUF_ARENA_CLEANUP_H__
