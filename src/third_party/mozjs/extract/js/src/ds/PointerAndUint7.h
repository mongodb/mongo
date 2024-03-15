/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_PointerAndUint7_h
#define gc_PointerAndUint7_h

#include "mozilla/Assertions.h"

#include <stdint.h>

namespace js {

// A class that can store an address and a 7-bit unsigned integer in 64 bits,
// even on a 64-bit target.
//
// On 64-bit targets, it assumes that all supported target architectures
// contain at most 57 significant bits in their addresses, and that the valid
// address space is split evenly between addresses increasing from 0--(64)--0
// and addresses decreasing from 1--(64)--1.
//
// The 57-significant-bit constraint comes from Intel's 5-level paging scheme
// as introduced in the Ice Lake processor line, circa late 2019; see
// https://en.wikipedia.org/wiki/Intel_5-level_paging.  Prior to that, Intel
// required only 48 significant bits.  AArch64 requires 52 significant bits,
// as of the ARMv8.2 LVA (Large Virtual Addressing) extension, and so is less
// constraining than Intel.
//
// In any case, NaN-boxing of pointers in JS::Value gives us a pretty hard
// requirement that we can store pointers in 47 bits.  So that constraint will
// break before the 57-bit constraint here breaks.  See SMDOC in
// js/public/Value.h.
//
// On 32-bit targets, both components are stored unmodified in the upper and
// lower 32-bit chunks of the value, and there are no constraints on the
// component values.

#ifdef JS_64BIT

// The implementation for 64-bit targets.
class PointerAndUint7 final {
  // The representation is: the lowest 57 bits of the pointer are stored in
  // the top 57 bits of val_, and the Uint7 is stored in the bottom 7 bits.
  // Hence recovering the pointer is 7-bit signed shift right of val_, and
  // recovering the UInt7 is an AND with 127.  In both cases, that's a single
  // machine instruction.
  uint64_t val_;

  static const uint8_t SHIFT_PTR = 7;
  static const uint64_t MASK_UINT7 = (uint64_t(1) << SHIFT_PTR) - 1;

  static inline bool isRepresentablePtr(void* ptr) {
    // We require that the top 7 bits (bits 63:57) are the same as bit 56.
    // That will be the case iff, when we signedly shift `ptr` right by 56
    // bits, the value is all zeroes or all ones.
    int64_t s = int64_t(ptr);
    // s should be bbbb'bbbb'X--(56)--X, for b = 0 or 1, and X can be anything
    s >>= (64 - SHIFT_PTR - 1);  // 56
    // s should be 0--(64)--0 or 1--(64)--1
    uint64_t u = uint64_t(s);
    // Note, this addition can overflow, intentionally.
    u += 1;
    // u should be 0--(64)--0 or 0--(63)--01
    return u <= uint64_t(1);
  }
  static inline bool isRepresentableUint7(uint32_t uint7) {
    return uint7 <= MASK_UINT7;
  }

 public:
  inline PointerAndUint7() : val_(0) {}
  inline PointerAndUint7(void* ptr, uint32_t uint7)
      : val_((uint64_t(ptr) << SHIFT_PTR) | (uint64_t(uint7 & MASK_UINT7))) {
    MOZ_ASSERT(isRepresentablePtr(ptr));
    MOZ_ASSERT(isRepresentableUint7(uint7));
  }
  inline void* pointer() const { return (void*)(int64_t(val_) >> SHIFT_PTR); }
  inline uint32_t uint7() const { return uint32_t(val_ & MASK_UINT7); }
};

static_assert(sizeof(void*) == 8);
// "int64_t really is signed"
static_assert(((int64_t(1) << 63) >> 63) == int64_t(0xFFFFFFFFFFFFFFFFULL));

#else

// The implementation for 32-bit targets.
class PointerAndUint7 final {
  // The representation places the pointer in the upper 32 bits of val_ and
  // the Uint7 in the lower 32 bits.  This is represented using a single
  // 64-bit field in the hope of increasing the chance that the class will be
  // passed around in a register-pair rather than through memory.
  uint64_t val_;

  static const uint8_t SHIFT_PTR = 32;
  static const uint64_t MASK_UINT7 = (uint64_t(1) << 7) - 1;

  static inline bool isRepresentableUint7(uint32_t uint7) {
    return uint7 <= MASK_UINT7;
  }

 public:
  inline PointerAndUint7() : val_(0) {}
  inline PointerAndUint7(void* ptr, uint32_t uint7)
      : val_((uint64_t(uint32_t(ptr)) << SHIFT_PTR) |
             (uint64_t(uint7) & MASK_UINT7)) {
    MOZ_ASSERT(isRepresentableUint7(uint7));
  }
  inline void* pointer() const { return (void*)(int32_t(val_ >> SHIFT_PTR)); }
  inline uint32_t uint7() const { return uint32_t(val_ & MASK_UINT7); }
};

static_assert(sizeof(void*) == 4);

#endif  // JS_64BIT

// We require this for both 32- and 64-bit targets.
static_assert(sizeof(PointerAndUint7) == 8);

}  // namespace js

#endif  // gc_PointerAndUint7_h
