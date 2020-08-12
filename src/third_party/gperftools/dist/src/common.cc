// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2008, Google Inc.
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// 
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat <opensource@google.com>

#include <stdlib.h> // for getenv and strtol
#include "config.h"
#include "common.h"
#include "system-alloc.h"
#include "base/spinlock.h"
#include "getenv_safe.h" // TCMallocGetenvSafe

namespace tcmalloc {

// Define the maximum number of object per classe type to transfer between
// thread and central caches.
static int32 FLAGS_tcmalloc_transfer_num_objects;

static const int32 kDefaultTransferNumObjecs = 32;

#if TCMALLOC_AGGRESSIVE_MERGE
static const bool kUseAggressiveMerge = true;
#else
static const bool kUseAggressiveMerge = false;
#endif

static const size_t kTargetTransferBytes = TCMALLOC_TARGET_TRANSFER_KB * 1024;

#if TCMALLOC_USE_UNCLAMPED_TRANSFER_SIZES
static const bool kUseUnclampedTransferSizes = true;
#else
static const bool kUseUnclampedTransferSizes = false;
#endif

// The init function is provided to explicit initialize the variable value
// from the env. var to avoid C++ global construction that might defer its
// initialization after a malloc/new call.
static inline void InitTCMallocTransferNumObjects()
{
  if (FLAGS_tcmalloc_transfer_num_objects == 0) {
    const char *envval = TCMallocGetenvSafe("TCMALLOC_TRANSFER_NUM_OBJ");
    FLAGS_tcmalloc_transfer_num_objects = !envval ? kDefaultTransferNumObjecs :
      strtol(envval, NULL, 10);
  }
}

// Note: the following only works for "n"s that fit in 32-bits, but
// that is fine since we only use it for small sizes.
static inline int LgFloor(size_t n) {
  int log = 0;
  for (int i = 4; i >= 0; --i) {
    int shift = (1 << i);
    size_t x = n >> shift;
    if (x != 0) {
      n = x;
      log += shift;
    }
  }
  ASSERT(n == 1);
  return log;
}

// Return the largest power of 2 of which 'n' is a multiple.
// Result is clipped to the closed interval [kMinAlign,kPageSize].
static size_t NaturalAlignment(size_t n) {
  if ((n & (kMinAlign-1)) != 0)
    return kMinAlign;
  for (size_t a = kMinAlign; a < kPageSize; a <<= 1) {
    if ((n & a) != 0) {
      return a;
    }
  }
  return kPageSize;
}

int AlignmentForSize(size_t size) {
  int alignment = kAlignment;
  if (size > kMaxSize) {
    // Cap alignment at kPageSize for large sizes.
    alignment = kPageSize;
  } else if (size >= 128) {
    // Space wasted due to alignment is at most 1/8, i.e., 12.5%.
    alignment = (1 << LgFloor(size)) / 8;
  } else if (size >= kMinAlign) {
    // We need an alignment of at least 16 bytes to satisfy
    // requirements for some SSE types.
    alignment = kMinAlign;
  }
  // Maximum alignment allowed is page size alignment.
  if (alignment > kPageSize) {
    alignment = kPageSize;
  }
  CHECK_CONDITION(size < kMinAlign || alignment >= kMinAlign);
  CHECK_CONDITION((alignment & (alignment - 1)) == 0);
  return alignment;
}

// Evaluate merging positions in [first, last) into [first, first+1).
static bool MergeOkayByFragmentation(const size_t *class_to_pages, const int32 *class_to_size,
                                     size_t first, size_t last) {
  if (last < first + 2) {
    return true;  // Nops are okay.
  }
  const size_t merge_top = last - 1;
  const size_t top_pages = class_to_pages[merge_top];
  const size_t top_size = class_to_size[merge_top];
  const size_t top_span_size = top_pages << kPageShift;
  const size_t top_objects = top_span_size / top_size;
  for (int i = first; i < merge_top; ++i) {
    const size_t prev_pages = class_to_pages[i];
    const size_t prev_size = class_to_size[i];
    if (kUseAggressiveMerge) {
      // See if we can merge this into the previous class(es) without
      // the fragmentation of any of them going over 12.5%.
      size_t used = prev_size * top_objects;
      size_t waste = top_span_size - used;
      if (waste > (top_span_size >> 3)) {
        return false;
      }
    } else {
      if (prev_pages != top_pages) {
        return false;
      }
      // See if we can merge this into the previous class without
      // increasing the fragmentation of the previous class.
      const size_t prev_objects = top_span_size / prev_size;
      if (top_objects != prev_objects) {
        return false;
      }
    }
  }
  return true;
}

// Evaluate merging positions in [first, last) into [first, first+1).
static bool MergeOkayByNaturalAlignment(const int32 *class_to_size, size_t first, size_t last) {
  if (last < first + 2) {
    return true;  // Nops are okay.
  }
  const size_t merge_top = last - 1;
  const size_t top_size = class_to_size[merge_top];
  for (size_t i = first; i < merge_top; ++i) {
    size_t prev_size = class_to_size[i];
    if (prev_size < kPageSize) {
      if (NaturalAlignment(top_size) < NaturalAlignment(prev_size)) {
        return false;
      }
    }
  }
  return true;
}

int SizeMap::NumMoveSize(size_t size) {
  if (size == 0) return 0;
  // Size of transfers between thread and central caches.
  int num = kTargetTransferBytes / size;
  if (num < 2) num = 2;

  // Avoid bringing too many objects into small object free lists.
  // If this value is too large:
  // - We waste memory with extra objects sitting in the thread caches.
  // - The central freelist holds its lock for too long while
  //   building a linked list of objects, slowing down the allocations
  //   of other threads.
  // If this value is too small:
  // - We go to the central freelist too often and we have to acquire
  //   its lock each time.
  // This value strikes a balance between the constraints above.
  if (num > FLAGS_tcmalloc_transfer_num_objects)
    num = FLAGS_tcmalloc_transfer_num_objects;

  return num;
}

// Initialize the mapping arrays
void SizeMap::Init() {
  InitTCMallocTransferNumObjects();

  // Do some sanity checking on add_amount[]/shift_amount[]/class_array[]
  if (ClassIndex(0) != 0) {
    Log(kCrash, __FILE__, __LINE__,
        "Invalid class index for size 0", ClassIndex(0));
  }
  if (ClassIndex(kMaxSize) >= sizeof(class_array_)) {
    Log(kCrash, __FILE__, __LINE__,
        "Invalid class index for kMaxSize", ClassIndex(kMaxSize));
  }

  // Compute the size classes we want to use
  int sc = 1;   // Next size class to assign
  int merge_edge = sc;  // leftmost boundary of merge evaluation.
  CHECK_CONDITION(kAlignment <= kMinAlign);
  for (size_t size = kAlignment; size <= kMaxSize; size += AlignmentForSize(size)) {
    CHECK_CONDITION((size % AlignmentForSize(size)) == 0);
    size_t blocks_to_move = kUseUnclampedTransferSizes ?
        (kTargetTransferBytes / size) : (NumMoveSize(size) / 4);
    size_t psize = 0;
    do {
      psize += kPageSize;
      // Allocate enough pages so leftover is less than 1/8 of total.
      // This bounds wasted space to at most 12.5%.
      while ((psize % size) > (psize >> 3)) {
        psize += kPageSize;
      }
      // Continue to add pages until there are at least as many objects in
      // the span as are needed when moving objects from the central
      // freelists and spans to the thread caches.
    } while ((psize / size) < (blocks_to_move));
    const size_t my_pages = psize >> kPageShift;

    // Add new class
    CHECK_CONDITION(sc < kClassSizesMax);
    class_to_pages_[sc] = my_pages;
    class_to_size_[sc] = size;
    sc++;

    // Evaluate merging [merge_edge, sc) to become [merge_edge, merge_edge + 1).
    if (MergeOkayByFragmentation(class_to_pages_, class_to_size_, merge_edge, sc)) {
      continue;  // Keep probing to find the greediest merge w.r.t. fragmentation.
    }
    --sc;  // Back out of the failing element.
    // Now the greediest merge w.r.t. fragmentation is [merge_edge, sc).
    // Ensure there are no alignment reductions in the merge.
    // Shrink the merge from the right until natural alignments are not reduced by it.
    while (!MergeOkayByNaturalAlignment(class_to_size_, merge_edge, sc)) {
      --sc;
    }

    class_to_size_[merge_edge] = class_to_size_[sc - 1];
    class_to_pages_[merge_edge] = class_to_pages_[sc - 1];
    ++merge_edge;
    sc = merge_edge;
    size = class_to_size_[sc - 1];  // restore the `size` invariant
  }
  num_size_classes = sc;
  if (sc > kClassSizesMax) {
    Log(kCrash, __FILE__, __LINE__,
        "too many size classes: (found vs. max)", sc, kClassSizesMax);
  }

  // Initialize the mapping arrays
  int next_size = 0;
  for (int c = 1; c < num_size_classes; c++) {
    const int max_size_in_class = class_to_size_[c];
    for (int s = next_size; s <= max_size_in_class; s += kAlignment) {
      class_array_[ClassIndex(s)] = c;
    }
    next_size = max_size_in_class + kAlignment;
  }

  // Double-check sizes just to be safe
  for (size_t size = 0; size <= kMaxSize;) {
    const int sc = SizeClass(size);
    if (sc <= 0 || sc >= num_size_classes) {
      Log(kCrash, __FILE__, __LINE__,
          "Bad size class (class, size)", sc, size);
    }
    if (sc > 1 && size <= class_to_size_[sc-1]) {
      Log(kCrash, __FILE__, __LINE__,
          "Allocating unnecessarily large class (class, size)", sc, size);
    }
    const size_t s = class_to_size_[sc];
    if (size > s || s == 0) {
      Log(kCrash, __FILE__, __LINE__,
          "Bad (class, size, requested)", sc, s, size);
    }
    if (size <= kMaxSmallSize) {
      size += 8;
    } else {
      size += 128;
    }
  }

  // Our fast-path aligned allocation functions rely on 'naturally
  // aligned' sizes to produce aligned addresses. Lets check if that
  // holds for size classes that we produced.
  //
  // I.e. we're checking that
  //
  // align = (1 << shift), malloc(i * align) % align == 0,
  //
  // for all align values up to kPageSize.
  for (size_t align = kMinAlign; align <= kPageSize; align <<= 1) {
    for (size_t size = align; size < kPageSize; size += align) {
      CHECK_CONDITION(class_to_size_[SizeClass(size)] % align == 0);
    }
  }

  // Initialize the num_objects_to_move array.
  for (size_t cl = 1; cl  < num_size_classes; ++cl) {
    num_objects_to_move_[cl] = NumMoveSize(ByteSizeForClass(cl));
  }
}

// Metadata allocator -- keeps stats about how many bytes allocated.
static uint64_t metadata_system_bytes_ = 0;
static const size_t kMetadataAllocChunkSize = 8*1024*1024;
// As ThreadCache objects are allocated with MetaDataAlloc, and also
// CACHELINE_ALIGNED, we must use the same alignment as TCMalloc_SystemAlloc.
static const size_t kMetadataAllignment = sizeof(MemoryAligner);

static char *metadata_chunk_alloc_;
static size_t metadata_chunk_avail_;

static SpinLock metadata_alloc_lock(SpinLock::LINKER_INITIALIZED);

void* MetaDataAlloc(size_t bytes) {
  if (bytes >= kMetadataAllocChunkSize) {
    void *rv = TCMalloc_SystemAlloc(bytes,
                                    NULL, kMetadataAllignment);
    if (rv != NULL) {
      metadata_system_bytes_ += bytes;
    }
    return rv;
  }

  SpinLockHolder h(&metadata_alloc_lock);

  // the following works by essentially turning address to integer of
  // log_2 kMetadataAllignment size and negating it. I.e. negated
  // value + original value gets 0 and that's what we want modulo
  // kMetadataAllignment. Note, we negate before masking higher bits
  // off, otherwise we'd have to mask them off after negation anyways.
  intptr_t alignment = -reinterpret_cast<intptr_t>(metadata_chunk_alloc_) & (kMetadataAllignment-1);

  if (metadata_chunk_avail_ < bytes + alignment) {
    size_t real_size;
    void *ptr = TCMalloc_SystemAlloc(kMetadataAllocChunkSize,
                                     &real_size, kMetadataAllignment);
    if (ptr == NULL) {
      return NULL;
    }

    metadata_chunk_alloc_ = static_cast<char *>(ptr);
    metadata_chunk_avail_ = real_size;

    alignment = 0;
  }

  void *rv = static_cast<void *>(metadata_chunk_alloc_ + alignment);
  bytes += alignment;
  metadata_chunk_alloc_ += bytes;
  metadata_chunk_avail_ -= bytes;
  metadata_system_bytes_ += bytes;
  return rv;
}

uint64_t metadata_system_bytes() { return metadata_system_bytes_; }

}  // namespace tcmalloc
