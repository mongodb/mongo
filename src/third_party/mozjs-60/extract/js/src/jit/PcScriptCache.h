/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_PcScriptCache_h
#define jit_PcScriptCache_h

#include "mozilla/Array.h"
#include "js/TypeDecls.h"

// Defines a fixed-size hash table solely for the purpose of caching jit::GetPcScript().
// One cache is attached to each JSRuntime; it functions as if cleared on GC.

namespace js {
namespace jit {

struct PcScriptCacheEntry
{
    uint8_t* returnAddress; // Key into the hash table.
    jsbytecode* pc;         // Cached PC.
    JSScript* script;       // Cached script.
};

struct PcScriptCache
{
    static const uint32_t Length = 73;

    // GC number at the time the cache was filled or created.
    // Storing and checking against this number allows us to not bother
    // clearing this cache on every GC -- only when actually necessary.
    uint64_t gcNumber;

    // List of cache entries.
    mozilla::Array<PcScriptCacheEntry, Length> entries;

    void clear(uint64_t gcNumber) {
        for (uint32_t i = 0; i < Length; i++)
            entries[i].returnAddress = nullptr;
        this->gcNumber = gcNumber;
    }

    // Get a value from the cache. May perform lazy allocation.
    MOZ_MUST_USE bool get(JSRuntime* rt, uint32_t hash, uint8_t* addr,
                          JSScript** scriptRes, jsbytecode** pcRes)
    {
        // If a GC occurred, lazily clear the cache now.
        if (gcNumber != rt->gc.gcNumber()) {
            clear(rt->gc.gcNumber());
            return false;
        }

        if (entries[hash].returnAddress != addr)
            return false;

        *scriptRes = entries[hash].script;
        if (pcRes)
            *pcRes = entries[hash].pc;

        return true;
    }

    void add(uint32_t hash, uint8_t* addr, jsbytecode* pc, JSScript* script) {
        MOZ_ASSERT(addr);
        MOZ_ASSERT(pc);
        MOZ_ASSERT(script);
        entries[hash].returnAddress = addr;
        entries[hash].pc = pc;
        entries[hash].script = script;
    }

    static uint32_t Hash(uint8_t* addr) {
        uint32_t key = (uint32_t)((uintptr_t)addr);
        return ((key >> 3) * 2654435761u) % Length;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_PcScriptCache_h */
