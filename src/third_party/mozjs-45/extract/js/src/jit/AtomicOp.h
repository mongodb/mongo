/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AtomicOp_h
#define jit_AtomicOp_h

namespace js {
namespace jit {

// Types of atomic operation, shared by MIR and LIR.

enum AtomicOp {
    AtomicFetchAddOp,
    AtomicFetchSubOp,
    AtomicFetchAndOp,
    AtomicFetchOrOp,
    AtomicFetchXorOp
};

// Memory barrier types, shared by MIR and LIR.
//
// MembarSynchronizing is here because some platforms can make the
// distinction (DSB vs DMB on ARM, SYNC vs parameterized SYNC on MIPS)
// but there's been no reason to use it yet.

enum MemoryBarrierBits {
    MembarLoadLoad = 1,
    MembarLoadStore = 2,
    MembarStoreStore = 4,
    MembarStoreLoad = 8,

    MembarSynchronizing = 16,

    // For validity testing
    MembarNobits = 0,
    MembarAllbits = 31,
};

static inline MOZ_CONSTEXPR MemoryBarrierBits
operator|(MemoryBarrierBits a, MemoryBarrierBits b)
{
    return MemoryBarrierBits(int(a) | int(b));
}

static inline MOZ_CONSTEXPR MemoryBarrierBits
operator&(MemoryBarrierBits a, MemoryBarrierBits b)
{
    return MemoryBarrierBits(int(a) & int(b));
}

static inline MOZ_CONSTEXPR MemoryBarrierBits
operator~(MemoryBarrierBits a)
{
    return MemoryBarrierBits(~int(a));
}

// Standard barrier bits for a full barrier.
static MOZ_CONSTEXPR_VAR MemoryBarrierBits MembarFull = MembarLoadLoad|MembarLoadStore|MembarStoreLoad|MembarStoreStore;

// Standard sets of barrier bits for atomic loads and stores.
// See http://gee.cs.oswego.edu/dl/jmm/cookbook.html for more.
static MOZ_CONSTEXPR_VAR MemoryBarrierBits MembarBeforeLoad = MembarNobits;
static MOZ_CONSTEXPR_VAR MemoryBarrierBits MembarAfterLoad = MembarLoadLoad|MembarLoadStore;
static MOZ_CONSTEXPR_VAR MemoryBarrierBits MembarBeforeStore = MembarStoreStore;
static MOZ_CONSTEXPR_VAR MemoryBarrierBits MembarAfterStore = MembarStoreLoad;

} // namespace jit
} // namespace js

#endif /* jit_AtomicOp_h */
