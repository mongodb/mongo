/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AtomicOp_h
#define jit_AtomicOp_h

#include <stdint.h>

namespace js {
namespace jit {

// Types of atomic operation, shared by MIR and LIR.

enum class AtomicOp {
  Add,
  Sub,
  And,
  Or,
  Xor,
};

// Memory barrier type, shared by MIR and LIR.
class MemoryBarrier {
  enum MemoryBarrierBits : uint8_t {
    MembarLoadLoad = 1,
    MembarLoadStore = 2,
    MembarStoreStore = 4,
    MembarStoreLoad = 8,

    // MembarSynchronizing is here because some platforms can make the
    // distinction (DSB vs DMB on ARM, SYNC vs parameterized SYNC on MIPS)
    // but there's been no reason to use it yet.
    MembarSynchronizing = 16,

    // For validity testing
    MembarNobits = 0,
    MembarAllbits = 31,
  };

  MemoryBarrierBits bits_;

  template <typename... MembarBits>
  constexpr explicit MemoryBarrier(MembarBits... bits)
      : bits_(static_cast<MemoryBarrierBits>((bits | ...))) {}

 public:
  // Accessors for currently used memory barrier types.

  constexpr bool isNone() const { return bits_ == MembarNobits; }

  constexpr bool isStoreStore() const { return bits_ == MembarStoreStore; }

  constexpr bool isSyncStoreStore() const {
    return bits_ == static_cast<MemoryBarrierBits>(MembarStoreStore |
                                                   MembarSynchronizing);
  }

  constexpr bool hasSync() const { return bits_ & MembarSynchronizing; }

  constexpr bool hasStoreLoad() const { return bits_ & MembarStoreLoad; }

  // No memory barrier.
  static constexpr MemoryBarrier None() { return MemoryBarrier{MembarNobits}; }

  // Full memory barrier.
  static constexpr MemoryBarrier Full() {
    return MemoryBarrier{MembarLoadLoad, MembarLoadStore, MembarStoreLoad,
                         MembarStoreStore};
  }

  // Standard sets of barriers for atomic loads and stores.
  // See http://gee.cs.oswego.edu/dl/jmm/cookbook.html for more.
  static constexpr MemoryBarrier BeforeLoad() {
    return MemoryBarrier{MembarNobits};
  }
  static constexpr MemoryBarrier AfterLoad() {
    return MemoryBarrier{MembarLoadLoad, MembarLoadStore};
  }
  static constexpr MemoryBarrier BeforeStore() {
    return MemoryBarrier{MembarStoreStore};
  }
  static constexpr MemoryBarrier AfterStore() {
    return MemoryBarrier{MembarStoreLoad};
  }
};

struct Synchronization {
  const MemoryBarrier barrierBefore;
  const MemoryBarrier barrierAfter;

  constexpr Synchronization(MemoryBarrier before, MemoryBarrier after)
      : barrierBefore(before), barrierAfter(after) {}

  static constexpr Synchronization None() {
    return {MemoryBarrier::None(), MemoryBarrier::None()};
  }

  static constexpr Synchronization Full() {
    return {MemoryBarrier::Full(), MemoryBarrier::Full()};
  }

  static constexpr Synchronization Load() {
    return {MemoryBarrier::BeforeLoad(), MemoryBarrier::AfterLoad()};
  }

  static constexpr Synchronization Store() {
    return {MemoryBarrier::BeforeStore(), MemoryBarrier::AfterStore()};
  }

  constexpr bool isNone() const {
    return barrierBefore.isNone() && barrierAfter.isNone();
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_AtomicOp_h */
