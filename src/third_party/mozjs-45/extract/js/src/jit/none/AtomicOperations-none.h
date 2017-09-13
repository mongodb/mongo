/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* For documentation, see jit/AtomicOperations.h */

#ifndef jit_none_AtomicOperations_none_h
#define jit_none_AtomicOperations_none_h

#include "mozilla/Assertions.h"

// A "none" build is never run (ref IRC discussion with h4writer) and
// all functions here can therefore MOZ_CRASH, even if they are
// referenced from other than jitted code.

inline bool
js::jit::AtomicOperations::isLockfree8()
{
    MOZ_CRASH();
}

inline void
js::jit::AtomicOperations::fenceSeqCst()
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::loadSeqCst(T* addr)
{
    MOZ_CRASH();
}

template<typename T>
inline void
js::jit::AtomicOperations::storeSeqCst(T* addr, T val)
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::compareExchangeSeqCst(T* addr, T oldval, T newval)
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::fetchAddSeqCst(T* addr, T val)
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::fetchSubSeqCst(T* addr, T val)
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::fetchAndSeqCst(T* addr, T val)
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::fetchOrSeqCst(T* addr, T val)
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::fetchXorSeqCst(T* addr, T val)
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::exchangeSeqCst(T* addr, T val)
{
    MOZ_CRASH();
}

template<typename T>
inline T
js::jit::AtomicOperations::loadSafeWhenRacy(T* addr)
{
    MOZ_CRASH();
}

template<typename T>
inline void
js::jit::AtomicOperations::storeSafeWhenRacy(T* addr, T val)
{
    MOZ_CRASH();
}

inline void
js::jit::AtomicOperations::memcpySafeWhenRacy(void* dest, const void* src, size_t nbytes)
{
    MOZ_CRASH();
}

inline void
js::jit::AtomicOperations::memmoveSafeWhenRacy(void* dest, const void* src, size_t nbytes)
{
    MOZ_CRASH();
}

template<size_t nbytes>
inline void
js::jit::RegionLock::acquire(void* addr)
{
    (void)spinlock;             // Remove a lot of "unused" warnings.
    MOZ_CRASH();
}

template<size_t nbytes>
inline void
js::jit::RegionLock::release(void* addr)
{
    MOZ_CRASH();
}

#endif // jit_none_AtomicOperations_none_h
