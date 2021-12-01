/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implementation details of the atoms table.
 */

#ifndef vm_AtomsTable_h
#define vm_AtomsTable_h

#include "js/GCHashTable.h"
#include "js/TypeDecls.h"

namespace js {

class AtomStateEntry
{
    uintptr_t bits;

    static const uintptr_t NO_TAG_MASK = uintptr_t(-1) - 1;

  public:
    AtomStateEntry() : bits(0) {}
    AtomStateEntry(const AtomStateEntry& other) : bits(other.bits) {}
    AtomStateEntry(JSAtom* ptr, bool tagged)
      : bits(uintptr_t(ptr) | uintptr_t(tagged))
    {
        MOZ_ASSERT((uintptr_t(ptr) & 0x1) == 0);
    }

    bool isPinned() const {
        return bits & 0x1;
    }

    /*
     * Non-branching code sequence. Note that the const_cast is safe because
     * the hash function doesn't consider the tag to be a portion of the key.
     */
    void setPinned(bool pinned) const {
        const_cast<AtomStateEntry*>(this)->bits |= uintptr_t(pinned);
    }

    JSAtom* asPtrUnbarriered() const {
        MOZ_ASSERT(bits);
        return reinterpret_cast<JSAtom*>(bits & NO_TAG_MASK);
    }

    JSAtom* asPtr(JSContext* cx) const;

    bool needsSweep() {
        JSAtom* atom = asPtrUnbarriered();
        return gc::IsAboutToBeFinalizedUnbarriered(&atom);
    }
};

struct AtomHasher
{
    struct Lookup;
    static inline HashNumber hash(const Lookup& l);
    static MOZ_ALWAYS_INLINE bool match(const AtomStateEntry& entry, const Lookup& lookup);
    static void rekey(AtomStateEntry& k, const AtomStateEntry& newKey) { k = newKey; }
};

using AtomSet = JS::GCHashSet<AtomStateEntry, AtomHasher, SystemAllocPolicy>;

// This class is a wrapper for AtomSet that is used to ensure the AtomSet is
// not modified. It should only expose read-only methods from AtomSet.
// Note however that the atoms within the table can be marked during GC.
class FrozenAtomSet
{
    AtomSet* mSet;

  public:
    // This constructor takes ownership of the passed-in AtomSet.
    explicit FrozenAtomSet(AtomSet* set) { mSet = set; }

    ~FrozenAtomSet() { js_delete(mSet); }

    MOZ_ALWAYS_INLINE AtomSet::Ptr readonlyThreadsafeLookup(const AtomSet::Lookup& l) const;

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return mSet->sizeOfIncludingThis(mallocSizeOf);
    }

    typedef AtomSet::Range Range;

    AtomSet::Range all() const { return mSet->all(); }
};

} // namespace js

#endif /* vm_AtomTables_h */
