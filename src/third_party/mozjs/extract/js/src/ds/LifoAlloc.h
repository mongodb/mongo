/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_LifoAlloc_h
#define ds_LifoAlloc_h

#include "mozilla/Attributes.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TemplateLib.h"

#include <algorithm>
#include <new>
#include <stddef.h>  // size_t
#include <type_traits>
#include <utility>

// [SMDOC] LifoAlloc bump allocator
//
// This file defines an allocator named LifoAlloc which is a Bump allocator,
// which has the property of making fast allocation but is not able to reclaim
// individual allocations.
//
// * Allocation principle
//
// In practice a LifoAlloc is implemented using a list of BumpChunks, which are
// contiguous memory areas which are chained in a single linked list.
//
// When an allocation is performed, we check if there is space in the last
// chunk. If there is we bump the pointer of the last chunk and return the
// previous value of the pointer. Otherwise we allocate a new chunk which is
// large enough and perform the allocation the same way.
//
// Each allocation is made with 2 main functions, called
// BumpChunk::nextAllocBase and BumpChunk::nextAllocEnd. These functions are
// made to avoid duplicating logic, such as allocating, checking if we can
// allocate or reserving a given buffer space. They are made to align the
// pointer for the next allocation (8-byte aligned), and also to reserve some
// red-zones to improve reports of our security instrumentation. (see Security
// features below)
//
// The Chunks sizes are following the heuristics implemented in NextSize
// (LifoAlloc.cpp), which doubles the size until we reach 1 MB and then
// continues with a smaller geometric series. This heuristic is meant to reduce
// the number of allocations, such that we spend less time allocating/freeing
// chunks of a few KB at a time.
//
// ** Oversize allocations
//
// When allocating with a LifoAlloc, we distinguish 2 different kinds of
// allocations, the small allocations and the large allocations. The reason for
// splitting in 2 sets is to avoid wasting memory.
//
// If you had a single linked list of chunks, then making oversized allocations
// can cause chunks to contain a lot of wasted space as new chunks would have to
// be allocated to fit these allocations, and the space of the previous chunk
// would remain unused.
//
// Oversize allocation size can be disabled or customized with disableOversize
// and setOversizeThreshold, which must be smaller than the default chunk size
// with which the LifoAlloc was initialized.
//
// ** LifoAllocScope (mark & release)
//
// As the memory cannot be reclaimed except when the LifoAlloc structure is
// deleted, the LifoAllocScope structure is used to create scopes, related to a
// stacked task. When going out of a LifoAllocScope the memory associated to the
// scope is marked as unused but not reclaimed. This implies that the memory
// allocated for one task can be reused for a similar task later on. (see
// Safety)
//
// LifoAllocScope is based on mark and release functions. The mark function is
// used to recall the offsets at which a LifoAllocScope got created. The release
// function takes the Mark as input and will flag all memory allocated after the
// mark creation as unused.
//
// When releasing all the memory of BumpChunks, these are moved to a list of
// unused chunks which will later be reused by new allocations.
//
// A bump chunk allocator normally has a single bump pointers, whereas we have
// 2. (see Oversize allocations) By doing so, we lose the ordering of allocation
// coming from a single linked list of allocation.
//
// However, we rely on the ordering of allocation with LifoAllocScope, i-e when
// mark and release functions are used. Thus the LifoAlloc::Mark is composed of
// 2 marks, One for each singled linked list of allocations, to keep both lists
// of allocations ordered.
//
// ** Infallible Allocator
//
// LifoAlloc can also be used as an infallible allocator. This requires the user
// to periodically ensure that enough space has been reserved to satisfy the
// upcoming set of allocations by calling LifoAlloc::ensureUnusedApproximate or
// LifoAlloc::allocEnsureUnused functions. Between 2 calls of these functions,
// functions such as allocInfallible can be used without checking against
// nullptr, as long as there is a bounded number of such calls and that all
// allocations including their red-zone fit in the reserved space.
//
// The infallible allocator mode can be toggle as being the default by calling
// setAsInfallibleByDefault, in which case an AutoFallibleScope should be used
// to make any large allocations. Failing to do so will raise an issue when
// running the LifoAlloc with the OOM Simulator. (see Security features)
//
// * LifoAlloc::Enum Iterator
//
// A LifoAlloc is used for backing the store-buffer of the Garbage Collector
// (GC). The store-buffer is appending data as it is being reported during
// incremental GC. The LifoAlloc::Enum class is used for iterating over the set
// of allocations made within the LifoAlloc.
//
// However, one must take extra care into having the proper associated types for
// the data which are being written and read out of the LifoAlloc. The iterator
// is reusing the same logic as the allocator in order to skip red-zones.
//
// At the moment, the iterator will cause a hard failure if any oversize
// allocation are made.
//
// * Safety
//
// A LifoAlloc is neither thread-safe nor interrupt-safe. It should only be
// manipulated in one thread of execution at a time. It can be transferred from
// one thread to another but should not be used concurrently.
//
// When using LifoAllocScope, no pointer to the data allocated within a
// LifoAllocScope should be stored in data allocated before the latest
// LifoAllocScope. This kind of issue can hide in different forms, such as
// appending to a Vector backed by a LifoAlloc, which can resize and move the
// data below the LifoAllocScope. Thus causing a use-after-free once leaving a
// LifoAllocScope.
//
// * Security features
//
// ** Single Linked List
//
// For sanity reasons this LifoAlloc implementation makes use of its own single
// linked list implementation based on unique pointers (UniquePtr). The reason
// for this is to ensure that a BumpChunk is owned only once, thus preventing
// use-after-free issues.
//
// ** OOM Simulator
//
// The OOM simulator is controlled by the JS_OOM_BREAKPOINT macro, and used to
// check any fallible allocation for potential OOM. Fallible functions are
// instrumented with JS_OOM_POSSIBLY_FAIL(); function calls, and are expected to
// return null on failures.
//
// Except for simulating OOMs, LifoAlloc is instrumented in DEBUG and OOM
// Simulator builds to checks for the correctness of the Infallible Allocator
// state. When using a LifoAlloc as an infallible allocator, enough space should
// always be reserved for the next allocations. Therefore, to check this
// invariant LifoAlloc::newChunkWithCapacity checks that any new chunks are
// allocated within a fallible scope, under AutoFallibleScope.
//
// ** Address Sanitizers & Valgrind
//
// When manipulating memory in a LifoAlloc, the memory remains contiguous and
// therefore subject to potential buffer overflow/underflow. To check for these
// memory corruptions, the macro LIFO_HAVE_MEM_CHECK is used to add red-zones
// with LIFO_MAKE_MEM_NOACCESS and LIFO_MAKE_MEM_UNDEFINED.
//
// The red-zone is a minimum space left in between 2 allocations. Any access to
// these red-zones should warn in both valgrind / ASan builds.
//
// The red-zone size is defined in BumpChunk::RedZoneSize and default to 0 if
// not instrumentation is expected, and 16 otherwise.
//
// ** Magic Number
//
// A simple sanity check is present in all BumpChunk under the form of a
// constant field which is never mutated. the BumpChunk::magic_ is initalized to
// the "Lif" string. Any mutation of this value indicate a memory corruption.
//
// This magic number is enabled in all MOZ_DIAGNOSTIC_ASSERT_ENABLED builds,
// which implies that all Nightly and dev-edition versions of
// Firefox/SpiderMonkey contain this instrumentation.
//
// ** Memory protection
//
// LifoAlloc chunks are holding a lot of memory. When the memory is known to be
// unused, unchanged for some period of time, such as moving from one thread to
// another. Then the memory can be set as read-only with LifoAlloc::setReadOnly
// and reset as read-write with LifoAlloc::setReadWrite.
//
// This code is guarded by LIFO_CHUNK_PROTECT and at the moment only enabled in
// DEBUG builds in order to avoid the fragmentation of the TLB which might run
// out-of-memory when calling mprotect.
//

#include "js/UniquePtr.h"
#include "util/Memory.h"
#include "util/Poison.h"

namespace js {

namespace detail {

template <typename T, typename D>
class SingleLinkedList;

template <typename T, typename D = JS::DeletePolicy<T>>
class SingleLinkedListElement {
  friend class SingleLinkedList<T, D>;
  js::UniquePtr<T, D> next_;

 public:
  SingleLinkedListElement() : next_(nullptr) {}
  ~SingleLinkedListElement() { MOZ_ASSERT(!next_); }

  T* next() const { return next_.get(); }
};

// Single linked list which is using UniquePtr to hold the next pointers.
// UniquePtr are used to ensure that none of the elements are used
// silmutaneously in 2 different list.
template <typename T, typename D = JS::DeletePolicy<T>>
class SingleLinkedList {
 private:
  // First element of the list which owns the next element, and ensure that
  // that this list is the only owner of the element.
  UniquePtr<T, D> head_;

  // Weak pointer to the last element of the list.
  T* last_;

  void assertInvariants() {
    MOZ_ASSERT(bool(head_) == bool(last_));
    MOZ_ASSERT_IF(last_, !last_->next_);
  }

 public:
  SingleLinkedList() : head_(nullptr), last_(nullptr) { assertInvariants(); }

  SingleLinkedList(SingleLinkedList&& other)
      : head_(std::move(other.head_)), last_(other.last_) {
    other.last_ = nullptr;
    assertInvariants();
    other.assertInvariants();
  }

  ~SingleLinkedList() {
    MOZ_ASSERT(!head_);
    MOZ_ASSERT(!last_);
  }

  // Move the elements of the |other| list in the current one, and implicitly
  // remove all the elements of the current list.
  SingleLinkedList& operator=(SingleLinkedList&& other) {
    head_ = std::move(other.head_);
    last_ = other.last_;
    other.last_ = nullptr;
    assertInvariants();
    other.assertInvariants();
    return *this;
  }

  bool empty() const { return !last_; }

  // Iterates over the elements of the list, this is used to avoid raw
  // manipulation of pointers as much as possible.
  class Iterator {
    T* current_;

   public:
    explicit Iterator(T* current) : current_(current) {}

    T& operator*() const { return *current_; }
    T* operator->() const { return current_; }
    T* get() const { return current_; }

    const Iterator& operator++() {
      current_ = current_->next();
      return *this;
    }

    bool operator!=(const Iterator& other) const {
      return current_ != other.current_;
    }
    bool operator==(const Iterator& other) const {
      return current_ == other.current_;
    }
  };

  Iterator begin() const { return Iterator(head_.get()); }
  Iterator end() const { return Iterator(nullptr); }
  Iterator last() const { return Iterator(last_); }

  // Split the list in 2 single linked lists after the element given as
  // argument.  The front of the list remains in the current list, while the
  // back goes in the newly create linked list.
  //
  // This is used for example to extract one element from a list. (see
  // LifoAlloc::getOrCreateChunk)
  SingleLinkedList splitAfter(T* newLast) {
    MOZ_ASSERT(newLast);
    SingleLinkedList result;
    if (newLast->next_) {
      result.head_ = std::move(newLast->next_);
      result.last_ = last_;
      last_ = newLast;
    }
    assertInvariants();
    result.assertInvariants();
    return result;
  }

  void pushFront(UniquePtr<T, D>&& elem) {
    if (!last_) {
      last_ = elem.get();
    }
    elem->next_ = std::move(head_);
    head_ = std::move(elem);
    assertInvariants();
  }

  void append(UniquePtr<T, D>&& elem) {
    if (last_) {
      last_->next_ = std::move(elem);
      last_ = last_->next_.get();
    } else {
      head_ = std::move(elem);
      last_ = head_.get();
    }
    assertInvariants();
  }
  void appendAll(SingleLinkedList&& list) {
    if (list.empty()) {
      return;
    }
    if (last_) {
      last_->next_ = std::move(list.head_);
    } else {
      head_ = std::move(list.head_);
    }
    last_ = list.last_;
    list.last_ = nullptr;
    assertInvariants();
    list.assertInvariants();
  }
  void steal(SingleLinkedList&& list) {
    head_ = std::move(list.head_);
    last_ = list.last_;
    list.last_ = nullptr;
    assertInvariants();
    list.assertInvariants();
  }
  void prependAll(SingleLinkedList&& list) {
    list.appendAll(std::move(*this));
    steal(std::move(list));
  }
  UniquePtr<T, D> popFirst() {
    MOZ_ASSERT(head_);
    UniquePtr<T, D> result = std::move(head_);
    head_ = std::move(result->next_);
    if (!head_) {
      last_ = nullptr;
    }
    assertInvariants();
    return result;
  }
};

static const size_t LIFO_ALLOC_ALIGN = 8;

MOZ_ALWAYS_INLINE
uint8_t* AlignPtr(uint8_t* orig) {
  static_assert(mozilla::IsPowerOfTwo(LIFO_ALLOC_ALIGN),
                "LIFO_ALLOC_ALIGN must be a power of two");

  uint8_t* result = (uint8_t*)AlignBytes(uintptr_t(orig), LIFO_ALLOC_ALIGN);
  MOZ_ASSERT(uintptr_t(result) % LIFO_ALLOC_ALIGN == 0);
  return result;
}

// A Chunk represent a single memory allocation made with the system
// allocator. As the owner of the memory, it is responsible for the allocation
// and the deallocation.
//
// This structure is only move-able, but not copyable.
class BumpChunk : public SingleLinkedListElement<BumpChunk> {
 private:
  // Pointer to the last byte allocated in this chunk.
  uint8_t* bump_;
  // Pointer to the first byte after this chunk.
  uint8_t* const capacity_;

#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  // Magic number used to check against poisoned values.
  const uintptr_t magic_ : 24;
  static constexpr uintptr_t magicNumber = uintptr_t(0x4c6966);
#endif

#if defined(DEBUG)
#  define LIFO_CHUNK_PROTECT 1
#endif

  // Poison the memory with memset, in order to catch errors due to
  // use-after-free, with JS_LIFO_UNDEFINED_PATTERN pattern, or to catch
  // use-before-init with JS_LIFO_UNINITIALIZED_PATTERN.
#if defined(DEBUG)
#  define LIFO_HAVE_MEM_CHECKS 1
#  define LIFO_MAKE_MEM_NOACCESS(addr, size)       \
    do {                                           \
      uint8_t* base = (addr);                      \
      size_t sz = (size);                          \
      MOZ_MAKE_MEM_UNDEFINED(base, sz);            \
      memset(base, JS_LIFO_UNDEFINED_PATTERN, sz); \
      MOZ_MAKE_MEM_NOACCESS(base, sz);             \
    } while (0)

#  define LIFO_MAKE_MEM_UNDEFINED(addr, size)          \
    do {                                               \
      uint8_t* base = (addr);                          \
      size_t sz = (size);                              \
      MOZ_MAKE_MEM_UNDEFINED(base, sz);                \
      memset(base, JS_LIFO_UNINITIALIZED_PATTERN, sz); \
      MOZ_MAKE_MEM_UNDEFINED(base, sz);                \
    } while (0)

#elif defined(MOZ_HAVE_MEM_CHECKS)
#  define LIFO_HAVE_MEM_CHECKS 1
#  define LIFO_MAKE_MEM_NOACCESS(addr, size) \
    MOZ_MAKE_MEM_NOACCESS((addr), (size))
#  define LIFO_MAKE_MEM_UNDEFINED(addr, size) \
    MOZ_MAKE_MEM_UNDEFINED((addr), (size))
#endif

#ifdef LIFO_HAVE_MEM_CHECKS
  // Red zone reserved after each allocation.
  static constexpr size_t RedZoneSize = 16;
#else
  static constexpr size_t RedZoneSize = 0;
#endif

  void assertInvariants() {
    MOZ_DIAGNOSTIC_ASSERT(magic_ == magicNumber);
    MOZ_ASSERT(begin() <= end());
    MOZ_ASSERT(end() <= capacity_);
  }

  BumpChunk& operator=(const BumpChunk&) = delete;
  BumpChunk(const BumpChunk&) = delete;

  explicit BumpChunk(uintptr_t capacity)
      : bump_(begin()),
        capacity_(base() + capacity)
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
        ,
        magic_(magicNumber)
#endif
  {
    assertInvariants();
#if defined(LIFO_HAVE_MEM_CHECKS)
    // The memory is freshly allocated and marked as undefined by the
    // allocator of the BumpChunk. Instead, we mark this memory as
    // no-access, as it has not been allocated within the BumpChunk.
    LIFO_MAKE_MEM_NOACCESS(bump_, capacity_ - bump_);
#endif
  }

  // Cast |this| into a uint8_t* pointer.
  //
  // Warning: Are you sure you do not want to use begin() instead?
  const uint8_t* base() const { return reinterpret_cast<const uint8_t*>(this); }
  uint8_t* base() { return reinterpret_cast<uint8_t*>(this); }

  // Update the bump pointer to any value contained in this chunk, which is
  // above the private fields of this chunk.
  //
  // The memory is poisoned and flagged as no-access when the bump pointer is
  // moving backward, such as when memory is released, or when a Mark is used
  // to unwind previous allocations.
  //
  // The memory is flagged as undefined when the bump pointer is moving
  // forward.
  void setBump(uint8_t* newBump) {
    assertInvariants();
    MOZ_ASSERT(begin() <= newBump);
    MOZ_ASSERT(newBump <= capacity_);
#if defined(LIFO_HAVE_MEM_CHECKS)
    // Poison/Unpoison memory that we just free'd/allocated.
    if (bump_ > newBump) {
      LIFO_MAKE_MEM_NOACCESS(newBump, bump_ - newBump);
    } else if (newBump > bump_) {
      MOZ_ASSERT(newBump - RedZoneSize >= bump_);
      LIFO_MAKE_MEM_UNDEFINED(bump_, newBump - RedZoneSize - bump_);
      // The area [newBump - RedZoneSize .. newBump[ is already flagged as
      // no-access either with the previous if-branch or with the
      // BumpChunk constructor. No need to mark it twice.
    }
#endif
    bump_ = newBump;
  }

 public:
  ~BumpChunk() { release(); }

  // Returns true if this chunk contains no allocated content.
  bool empty() const { return end() == begin(); }

  // Returns the size in bytes of the number of allocated space. This includes
  // the size consumed by the alignment of the allocations.
  size_t used() const { return end() - begin(); }

  // These are used for manipulating a chunk as if it was a vector of bytes,
  // and used for iterating over the content of the buffer (see
  // LifoAlloc::Enum)
  inline const uint8_t* begin() const;
  inline uint8_t* begin();
  uint8_t* end() const { return bump_; }

  // This function is the only way to allocate and construct a chunk. It
  // returns a UniquePtr to the newly allocated chunk.  The size given as
  // argument includes the space needed for the header of the chunk.
  static UniquePtr<BumpChunk> newWithCapacity(size_t size);

  // Report allocation.
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this);
  }

  // Report allocation size.
  size_t computedSizeOfIncludingThis() const { return capacity_ - base(); }

  // Opaque type used to carry a pointer to the current location of the bump_
  // pointer, within a BumpChunk.
  class Mark {
    // Chunk which owns the pointer.
    BumpChunk* chunk_;
    // Recorded of the bump_ pointer of the BumpChunk.
    uint8_t* bump_;

    friend class BumpChunk;
    Mark(BumpChunk* chunk, uint8_t* bump) : chunk_(chunk), bump_(bump) {}

   public:
    Mark() : chunk_(nullptr), bump_(nullptr) {}

    BumpChunk* markedChunk() const { return chunk_; }
  };

  // Return a mark to be able to unwind future allocations with the release
  // function. (see LifoAllocScope)
  Mark mark() { return Mark(this, end()); }

  // Check if a pointer is part of the allocated data of this chunk.
  bool contains(const void* ptr) const {
    // Note: We cannot check "ptr < end()" because the mark have a 0-size
    // length.
    return begin() <= ptr && ptr <= end();
  }

  // Check if a mark is part of the allocated data of this chunk.
  bool contains(Mark m) const {
    MOZ_ASSERT(m.chunk_ == this);
    return contains(m.bump_);
  }

  // Release the memory allocated in this chunk. This function does not call
  // any of the destructors.
  void release() { setBump(begin()); }

  // Release the memory allocated in this chunk since the corresponding mark
  // got created. This function does not call any of the destructors.
  void release(Mark m) {
    MOZ_RELEASE_ASSERT(contains(m));
    setBump(m.bump_);
  }

  // Given an amount, compute the total size of a chunk for it: reserved
  // space before |begin()|, space for |amount| bytes, and red-zone space
  // after those bytes that will ultimately end at |capacity_|.
  [[nodiscard]] static inline bool allocSizeWithRedZone(size_t amount,
                                                        size_t* size);

  // Given a bump chunk pointer, find the next base/end pointers. This is
  // useful for having consistent allocations, and iterating over known size
  // allocations.
  static uint8_t* nextAllocBase(uint8_t* e) { return detail::AlignPtr(e); }
  static uint8_t* nextAllocEnd(uint8_t* b, size_t n) {
    return b + n + RedZoneSize;
  }

  // Returns true, if the unused space is large enough for an allocation of
  // |n| bytes.
  bool canAlloc(size_t n) const {
    uint8_t* newBump = nextAllocEnd(nextAllocBase(end()), n);
    // bump_ <= newBump, is necessary to catch overflow.
    return bump_ <= newBump && newBump <= capacity_;
  }

  // Space remaining in the current chunk.
  size_t unused() const {
    uint8_t* aligned = nextAllocBase(end());
    if (aligned < capacity_) {
      return capacity_ - aligned;
    }
    return 0;
  }

  // Try to perform an allocation of size |n|, returns nullptr if not possible.
  MOZ_ALWAYS_INLINE
  void* tryAlloc(size_t n) {
    uint8_t* aligned = nextAllocBase(end());
    uint8_t* newBump = nextAllocEnd(aligned, n);

    if (newBump > capacity_) {
      return nullptr;
    }

    // Check for overflow.
    if (MOZ_UNLIKELY(newBump < bump_)) {
      return nullptr;
    }

    MOZ_ASSERT(canAlloc(n));  // Ensure consistency between "can" and "try".
    setBump(newBump);
    return aligned;
  }

#ifdef LIFO_CHUNK_PROTECT
  void setReadOnly();
  void setReadWrite();
#else
  void setReadOnly() const {}
  void setReadWrite() const {}
#endif
};

// Space reserved for the BumpChunk internal data, and the alignment of the
// first allocation content. This can be used to ensure there is enough space
// for the next allocation (see LifoAlloc::newChunkWithCapacity).
static constexpr size_t BumpChunkReservedSpace =
    AlignBytes(sizeof(BumpChunk), LIFO_ALLOC_ALIGN);

[[nodiscard]] /* static */ inline bool BumpChunk::allocSizeWithRedZone(
    size_t amount, size_t* size) {
  constexpr size_t SpaceBefore = BumpChunkReservedSpace;
  static_assert((SpaceBefore % LIFO_ALLOC_ALIGN) == 0,
                "reserved space presumed already aligned");

  constexpr size_t SpaceAfter = RedZoneSize;  // may be zero

  constexpr size_t SpaceBeforeAndAfter = SpaceBefore + SpaceAfter;
  static_assert(SpaceBeforeAndAfter >= SpaceBefore,
                "intermediate addition must not overflow");

  *size = SpaceBeforeAndAfter + amount;
  return MOZ_LIKELY(*size >= SpaceBeforeAndAfter);
}

inline const uint8_t* BumpChunk::begin() const {
  return base() + BumpChunkReservedSpace;
}

inline uint8_t* BumpChunk::begin() { return base() + BumpChunkReservedSpace; }

}  // namespace detail

// LIFO bump allocator: used for phase-oriented and fast LIFO allocations.
//
// Note: We leave BumpChunks latent in the set of unused chunks after they've
// been released to avoid thrashing before a GC.
class LifoAlloc {
  using UniqueBumpChunk = js::UniquePtr<detail::BumpChunk>;
  using BumpChunkList = detail::SingleLinkedList<detail::BumpChunk>;

  // List of chunks containing allocated data of size smaller than the default
  // chunk size. In the common case, the last chunk of this list is always
  // used to perform the allocations. When the allocation cannot be performed,
  // we move a Chunk from the unused set to the list of used chunks.
  BumpChunkList chunks_;

  // List of chunks containing allocated data where each allocation is larger
  // than the oversize threshold. Each chunk contains exactly one allocation.
  // This reduces wasted space in the chunk list.
  //
  // Oversize chunks are allocated on demand and freed as soon as they are
  // released, instead of being pushed to the unused list.
  BumpChunkList oversize_;

  // Set of unused chunks, which can be reused for future allocations.
  BumpChunkList unused_;

  size_t markCount;
  size_t defaultChunkSize_;
  size_t oversizeThreshold_;

  // Size of all chunks in chunks_, oversize_, unused_ lists.
  size_t curSize_;
  size_t peakSize_;

  // Size of all chunks containing small bump allocations. This heuristic is
  // used to compute growth rate while ignoring chunks such as oversized,
  // now-unused, or transferred (which followed their own growth patterns).
  size_t smallAllocsSize_;

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
  bool fallibleScope_;
#endif

  void operator=(const LifoAlloc&) = delete;
  LifoAlloc(const LifoAlloc&) = delete;

  // Return a BumpChunk that can perform an allocation of at least size |n|.
  UniqueBumpChunk newChunkWithCapacity(size_t n, bool oversize);

  // Reuse or allocate a BumpChunk that can perform an allocation of at least
  // size |n|, if successful it is placed at the end the list of |chunks_|.
  UniqueBumpChunk getOrCreateChunk(size_t n);

  void reset(size_t defaultChunkSize);

  // Append unused chunks to the end of this LifoAlloc.
  void appendUnused(BumpChunkList&& otherUnused) {
#ifdef DEBUG
    for (detail::BumpChunk& bc : otherUnused) {
      MOZ_ASSERT(bc.empty());
    }
#endif
    unused_.appendAll(std::move(otherUnused));
  }

  // Append used chunks to the end of this LifoAlloc. We act as if all the
  // chunks in |this| are used, even if they're not, so memory may be wasted.
  void appendUsed(BumpChunkList&& otherChunks) {
    chunks_.appendAll(std::move(otherChunks));
  }

  // Track the amount of space allocated in used and unused chunks.
  void incrementCurSize(size_t size) {
    curSize_ += size;
    if (curSize_ > peakSize_) {
      peakSize_ = curSize_;
    }
  }
  void decrementCurSize(size_t size) {
    MOZ_ASSERT(curSize_ >= size);
    curSize_ -= size;
    MOZ_ASSERT(curSize_ >= smallAllocsSize_);
  }

  void* allocImplColdPath(size_t n);
  void* allocImplOversize(size_t n);

  MOZ_ALWAYS_INLINE
  void* allocImpl(size_t n) {
    void* result;
    // Give oversized allocations their own chunk instead of wasting space
    // due to fragmentation at the end of normal chunk.
    if (MOZ_UNLIKELY(n > oversizeThreshold_)) {
      return allocImplOversize(n);
    }
    if (MOZ_LIKELY(!chunks_.empty() &&
                   (result = chunks_.last()->tryAlloc(n)))) {
      return result;
    }
    return allocImplColdPath(n);
  }

  // Check for space in unused chunks or allocate a new unused chunk.
  [[nodiscard]] bool ensureUnusedApproximateColdPath(size_t n, size_t total);

 public:
  explicit LifoAlloc(size_t defaultChunkSize)
      : peakSize_(0)
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
        ,
        fallibleScope_(true)
#endif
  {
    reset(defaultChunkSize);
  }

  // Set the threshold to allocate data in its own chunk outside the space for
  // small allocations.
  void disableOversize() { oversizeThreshold_ = SIZE_MAX; }
  void setOversizeThreshold(size_t oversizeThreshold) {
    MOZ_ASSERT(oversizeThreshold <= defaultChunkSize_);
    oversizeThreshold_ = oversizeThreshold;
  }

  // Steal allocated chunks from |other|.
  void steal(LifoAlloc* other);

  // Append all chunks from |other|. They are removed from |other|.
  void transferFrom(LifoAlloc* other);

  // Append unused chunks from |other|. They are removed from |other|.
  void transferUnusedFrom(LifoAlloc* other);

  ~LifoAlloc() { freeAll(); }

  size_t defaultChunkSize() const { return defaultChunkSize_; }

  // Frees all held memory.
  void freeAll();

  static const unsigned HUGE_ALLOCATION = 50 * 1024 * 1024;
  void freeAllIfHugeAndUnused() {
    if (markCount == 0 && curSize_ > HUGE_ALLOCATION) {
      freeAll();
    }
  }

  MOZ_ALWAYS_INLINE
  void* alloc(size_t n) {
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    // Only simulate OOMs when we are not using the LifoAlloc as an
    // infallible allocator.
    if (fallibleScope_) {
      JS_OOM_POSSIBLY_FAIL();
    }
#endif
    return allocImpl(n);
  }

  // Allocates |n| bytes if we can guarantee that we will have
  // |needed| unused bytes remaining. Returns nullptr if we can't.
  // This is useful for maintaining our ballast invariants while
  // attempting fallible allocations.
  MOZ_ALWAYS_INLINE
  void* allocEnsureUnused(size_t n, size_t needed) {
    JS_OOM_POSSIBLY_FAIL();
    MOZ_ASSERT(fallibleScope_);

    Mark m = mark();
    void* result = allocImpl(n);
    if (!ensureUnusedApproximate(needed)) {
      release(m);
      return nullptr;
    }
    cancelMark(m);
    return result;
  }

  template <typename T, typename... Args>
  MOZ_ALWAYS_INLINE T* newWithSize(size_t n, Args&&... args) {
    MOZ_ASSERT(n >= sizeof(T), "must request enough space to store a T");
    static_assert(alignof(T) <= detail::LIFO_ALLOC_ALIGN,
                  "LifoAlloc must provide enough alignment to store T");
    void* ptr = alloc(n);
    if (!ptr) {
      return nullptr;
    }

    return new (ptr) T(std::forward<Args>(args)...);
  }

  MOZ_ALWAYS_INLINE
  void* allocInfallible(size_t n) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (void* result = allocImpl(n)) {
      return result;
    }
    oomUnsafe.crash("LifoAlloc::allocInfallible");
    return nullptr;
  }

  // Ensures that enough space exists to satisfy N bytes worth of
  // allocation requests, not necessarily contiguous. Note that this does
  // not guarantee a successful single allocation of N bytes.
  [[nodiscard]] MOZ_ALWAYS_INLINE bool ensureUnusedApproximate(size_t n) {
    AutoFallibleScope fallibleAllocator(this);
    size_t total = 0;
    if (!chunks_.empty()) {
      total += chunks_.last()->unused();
      if (total >= n) {
        return true;
      }
    }

    return ensureUnusedApproximateColdPath(n, total);
  }

  MOZ_ALWAYS_INLINE
  void setAsInfallibleByDefault() {
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    fallibleScope_ = false;
#endif
  }

  class MOZ_NON_TEMPORARY_CLASS AutoFallibleScope {
#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    LifoAlloc* lifoAlloc_;
    bool prevFallibleScope_;

   public:
    explicit AutoFallibleScope(LifoAlloc* lifoAlloc) {
      lifoAlloc_ = lifoAlloc;
      prevFallibleScope_ = lifoAlloc->fallibleScope_;
      lifoAlloc->fallibleScope_ = true;
    }

    ~AutoFallibleScope() { lifoAlloc_->fallibleScope_ = prevFallibleScope_; }
#else
   public:
    explicit AutoFallibleScope(LifoAlloc*) {}
#endif
  };

  template <typename T>
  T* newArray(size_t count) {
    static_assert(std::is_trivial_v<T>,
                  "T must be trivially constructible so that constructors need "
                  "not be called");
    static_assert(std::is_trivially_destructible_v<T>,
                  "T must be trivially destructible so destructors don't need "
                  "to be called when the LifoAlloc is freed");
    return newArrayUninitialized<T>(count);
  }

  // Create an array with uninitialized elements of type |T|.
  // The caller is responsible for initialization.
  template <typename T>
  T* newArrayUninitialized(size_t count) {
    size_t bytes;
    if (MOZ_UNLIKELY(!CalculateAllocSize<T>(count, &bytes))) {
      return nullptr;
    }
    return static_cast<T*>(alloc(bytes));
  }

  class Mark {
    friend class LifoAlloc;
    detail::BumpChunk::Mark chunk;
    detail::BumpChunk::Mark oversize;
  };

  // Note: MOZ_NEVER_INLINE is a work around for a Clang 9 (PGO) miscompilation.
  // See bug 1583907.
  MOZ_NEVER_INLINE Mark mark();

  void release(Mark mark);

 private:
  void cancelMark(Mark mark) { markCount--; }

 public:
  void releaseAll() {
    MOZ_ASSERT(!markCount);

    // When releasing all chunks, we can no longer determine which chunks were
    // transferred and which were not, so simply clear the heuristic to zero
    // right away.
    smallAllocsSize_ = 0;

    for (detail::BumpChunk& bc : chunks_) {
      bc.release();
    }
    unused_.appendAll(std::move(chunks_));

    // On release, we free any oversize allocations instead of keeping them
    // in unused chunks.
    while (!oversize_.empty()) {
      UniqueBumpChunk bc = oversize_.popFirst();
      decrementCurSize(bc->computedSizeOfIncludingThis());
    }
  }

  // Protect the content of the LifoAlloc chunks.
#ifdef LIFO_CHUNK_PROTECT
  void setReadOnly();
  void setReadWrite();
#else
  void setReadOnly() const {}
  void setReadWrite() const {}
#endif

  // Get the total "used" (occupied bytes) count for the arena chunks.
  size_t used() const {
    size_t accum = 0;
    for (const detail::BumpChunk& chunk : chunks_) {
      accum += chunk.used();
    }
    return accum;
  }

  // Return true if the LifoAlloc does not currently contain any allocations.
  bool isEmpty() const {
    bool empty = chunks_.empty() ||
                 (chunks_.begin() == chunks_.last() && chunks_.last()->empty());
    MOZ_ASSERT_IF(!oversize_.empty(), !oversize_.last()->empty());
    return empty && oversize_.empty();
  }

  // Return the number of bytes remaining to allocate in the current chunk.
  // e.g. How many bytes we can allocate before needing a new block.
  size_t availableInCurrentChunk() const {
    if (chunks_.empty()) {
      return 0;
    }
    return chunks_.last()->unused();
  }

  // Get the total size of the arena chunks (including unused space).
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    size_t n = 0;
    for (const detail::BumpChunk& chunk : chunks_) {
      n += chunk.sizeOfIncludingThis(mallocSizeOf);
    }
    for (const detail::BumpChunk& chunk : oversize_) {
      n += chunk.sizeOfIncludingThis(mallocSizeOf);
    }
    for (const detail::BumpChunk& chunk : unused_) {
      n += chunk.sizeOfIncludingThis(mallocSizeOf);
    }
    return n;
  }

  // Like sizeOfExcludingThis(), but includes the size of the LifoAlloc itself.
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
  }

  // Get the current size of the arena chunks (including unused space and
  // bookkeeping space).
  size_t computedSizeOfExcludingThis() const { return curSize_; }

  // Get the peak size of the arena chunks (including unused space and
  // bookkeeping space).
  size_t peakSizeOfExcludingThis() const { return peakSize_; }

  // Doesn't perform construction; useful for lazily-initialized POD types.
  template <typename T>
  MOZ_ALWAYS_INLINE T* pod_malloc() {
    return static_cast<T*>(alloc(sizeof(T)));
  }

  JS_DECLARE_NEW_METHODS(new_, alloc, MOZ_ALWAYS_INLINE)
  JS_DECLARE_NEW_METHODS(newInfallible, allocInfallible, MOZ_ALWAYS_INLINE)

#ifdef DEBUG
  bool contains(const void* ptr) const {
    for (const detail::BumpChunk& chunk : chunks_) {
      if (chunk.contains(ptr)) {
        return true;
      }
    }
    for (const detail::BumpChunk& chunk : oversize_) {
      if (chunk.contains(ptr)) {
        return true;
      }
    }
    return false;
  }
#endif

  // Iterate over the data allocated in a LifoAlloc, and interpret it.
  class Enum {
    friend class LifoAlloc;
    friend class detail::BumpChunk;

    // Iterator over the list of bump chunks.
    BumpChunkList::Iterator chunkIt_;
    BumpChunkList::Iterator chunkEnd_;
    // Read head (must be within chunk_).
    uint8_t* head_;

    // If there is not enough room in the remaining block for |size|,
    // advance to the next block and update the position.
    uint8_t* seekBaseAndAdvanceBy(size_t size) {
      MOZ_ASSERT(!empty());

      uint8_t* aligned = detail::BumpChunk::nextAllocBase(head_);
      if (detail::BumpChunk::nextAllocEnd(aligned, size) > chunkIt_->end()) {
        ++chunkIt_;
        aligned = chunkIt_->begin();
        // The current code assumes that if we have a chunk, then we
        // have allocated something it in.
        MOZ_ASSERT(!chunkIt_->empty());
      }
      head_ = detail::BumpChunk::nextAllocEnd(aligned, size);
      MOZ_ASSERT(head_ <= chunkIt_->end());
      return aligned;
    }

   public:
    explicit Enum(LifoAlloc& alloc)
        : chunkIt_(alloc.chunks_.begin()),
          chunkEnd_(alloc.chunks_.end()),
          head_(nullptr) {
      MOZ_RELEASE_ASSERT(alloc.oversize_.empty());
      if (chunkIt_ != chunkEnd_) {
        head_ = chunkIt_->begin();
      }
    }

    // Return true if there are no more bytes to enumerate.
    bool empty() {
      return chunkIt_ == chunkEnd_ ||
             (chunkIt_->next() == chunkEnd_.get() && head_ >= chunkIt_->end());
    }

    // Move the read position forward by the size of one T.
    template <typename T>
    T* read(size_t size = sizeof(T)) {
      return reinterpret_cast<T*>(read(size));
    }

    // Return a pointer to the item at the current position. This returns a
    // pointer to the inline storage, not a copy, and moves the read-head by
    // the requested |size|.
    void* read(size_t size) { return seekBaseAndAdvanceBy(size); }
  };
};

class MOZ_NON_TEMPORARY_CLASS LifoAllocScope {
  LifoAlloc* lifoAlloc;
  LifoAlloc::Mark mark;
  LifoAlloc::AutoFallibleScope fallibleScope;

 public:
  explicit LifoAllocScope(LifoAlloc* lifoAlloc)
      : lifoAlloc(lifoAlloc),
        mark(lifoAlloc->mark()),
        fallibleScope(lifoAlloc) {}

  ~LifoAllocScope() {
    lifoAlloc->release(mark);

    /*
     * The parser can allocate enormous amounts of memory for large functions.
     * Eagerly free the memory now (which otherwise won't be freed until the
     * next GC) to avoid unnecessary OOMs.
     */
    lifoAlloc->freeAllIfHugeAndUnused();
  }

  LifoAlloc& alloc() { return *lifoAlloc; }
};

enum Fallibility { Fallible, Infallible };

template <Fallibility fb>
class LifoAllocPolicy {
  LifoAlloc& alloc_;

 public:
  MOZ_IMPLICIT LifoAllocPolicy(LifoAlloc& alloc) : alloc_(alloc) {}
  template <typename T>
  T* maybe_pod_malloc(size_t numElems) {
    size_t bytes;
    if (MOZ_UNLIKELY(!CalculateAllocSize<T>(numElems, &bytes))) {
      return nullptr;
    }
    void* p =
        fb == Fallible ? alloc_.alloc(bytes) : alloc_.allocInfallible(bytes);
    return static_cast<T*>(p);
  }
  template <typename T>
  T* maybe_pod_calloc(size_t numElems) {
    T* p = maybe_pod_malloc<T>(numElems);
    if (MOZ_UNLIKELY(!p)) {
      return nullptr;
    }
    memset(p, 0, numElems * sizeof(T));
    return p;
  }
  template <typename T>
  T* maybe_pod_realloc(T* p, size_t oldSize, size_t newSize) {
    T* n = maybe_pod_malloc<T>(newSize);
    if (MOZ_UNLIKELY(!n)) {
      return nullptr;
    }
    MOZ_ASSERT(!(oldSize & mozilla::tl::MulOverflowMask<sizeof(T)>::value));
    memcpy(n, p, std::min(oldSize * sizeof(T), newSize * sizeof(T)));
    return n;
  }
  template <typename T>
  T* pod_malloc(size_t numElems) {
    return maybe_pod_malloc<T>(numElems);
  }
  template <typename T>
  T* pod_calloc(size_t numElems) {
    return maybe_pod_calloc<T>(numElems);
  }
  template <typename T>
  T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
    return maybe_pod_realloc<T>(p, oldSize, newSize);
  }
  template <typename T>
  void free_(T* p, size_t numElems) {}
  void reportAllocOverflow() const {}
  [[nodiscard]] bool checkSimulatedOOM() const {
    return fb == Infallible || !js::oom::ShouldFailWithOOM();
  }
};

}  // namespace js

#endif /* ds_LifoAlloc_h */
