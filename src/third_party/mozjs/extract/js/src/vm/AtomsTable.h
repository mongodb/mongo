/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implementation details of the atoms table.
 */

#ifndef vm_AtomsTable_h
#define vm_AtomsTable_h

#include "gc/Barrier.h"
#include "js/GCHashTable.h"
#include "js/TypeDecls.h"
#include "js/Vector.h"
#include "vm/StringType.h"

/*
 * The atoms table is a mapping from strings to JSAtoms that supports
 * incremental sweeping.
 */

namespace js {

struct AtomHasher {
  struct Lookup;
  static inline HashNumber hash(const Lookup& l);
  static MOZ_ALWAYS_INLINE bool match(const WeakHeapPtr<JSAtom*>& entry,
                                      const Lookup& lookup);
  static void rekey(WeakHeapPtr<JSAtom*>& k,
                    const WeakHeapPtr<JSAtom*>& newKey) {
    k = newKey;
  }
};

struct js::AtomHasher::Lookup {
  union {
    const JS::Latin1Char* latin1Chars;
    const char16_t* twoByteChars;
    const char* utf8Bytes;
  };
  enum { TwoByteChar, Latin1, UTF8 } type;
  size_t length;
  size_t byteLength;
  const JSAtom* atom; /* Optional. */
  JS::AutoCheckCannotGC nogc;

  HashNumber hash;

  MOZ_ALWAYS_INLINE Lookup(const char* utf8Bytes, size_t byteLen, size_t length,
                           HashNumber hash)
      : utf8Bytes(utf8Bytes),
        type(UTF8),
        length(length),
        byteLength(byteLen),
        atom(nullptr),
        hash(hash) {}

  MOZ_ALWAYS_INLINE Lookup(const char16_t* chars, size_t length)
      : twoByteChars(chars),
        type(TwoByteChar),
        length(length),
        atom(nullptr),
        hash(mozilla::HashString(chars, length)) {}

  MOZ_ALWAYS_INLINE Lookup(const JS::Latin1Char* chars, size_t length)
      : latin1Chars(chars),
        type(Latin1),
        length(length),
        atom(nullptr),
        hash(mozilla::HashString(chars, length)) {}

  MOZ_ALWAYS_INLINE Lookup(HashNumber hash, const char16_t* chars,
                           size_t length)
      : twoByteChars(chars),
        type(TwoByteChar),
        length(length),
        atom(nullptr),
        hash(hash) {
    MOZ_ASSERT(hash == mozilla::HashString(chars, length));
  }

  MOZ_ALWAYS_INLINE Lookup(HashNumber hash, const JS::Latin1Char* chars,
                           size_t length)
      : latin1Chars(chars),
        type(Latin1),
        length(length),
        atom(nullptr),
        hash(hash) {
    MOZ_ASSERT(hash == mozilla::HashString(chars, length));
  }

  inline explicit Lookup(const JSAtom* atom)
      : type(atom->hasLatin1Chars() ? Latin1 : TwoByteChar),
        length(atom->length()),
        atom(atom),
        hash(atom->hash()) {
    if (type == Latin1) {
      latin1Chars = atom->latin1Chars(nogc);
      MOZ_ASSERT(mozilla::HashString(latin1Chars, length) == hash);
    } else {
      MOZ_ASSERT(type == TwoByteChar);
      twoByteChars = atom->twoByteChars(nogc);
      MOZ_ASSERT(mozilla::HashString(twoByteChars, length) == hash);
    }
  }

  // Return: true iff the string in |atom| matches the string in this Lookup.
  bool StringsMatch(const JSAtom& atom) const;
};

// Note: Use a 'class' here to make forward declarations easier to use.
class AtomSet : public JS::GCHashSet<WeakHeapPtr<JSAtom*>, AtomHasher,
                                     SystemAllocPolicy> {
  using Base =
      JS::GCHashSet<WeakHeapPtr<JSAtom*>, AtomHasher, SystemAllocPolicy>;

 public:
  AtomSet() = default;
  explicit AtomSet(size_t length) : Base(length) {};
};

// This class is a wrapper for AtomSet that is used to ensure the AtomSet is
// not modified. It should only expose read-only methods from AtomSet.
// Note however that the atoms within the table can be marked during GC.
class FrozenAtomSet {
  AtomSet* mSet;

 public:
  // This constructor takes ownership of the passed-in AtomSet.
  explicit FrozenAtomSet(AtomSet* set) { mSet = set; }

  ~FrozenAtomSet() { js_delete(mSet); }

  MOZ_ALWAYS_INLINE AtomSet::Ptr readonlyThreadsafeLookup(
      const AtomSet::Lookup& l) const;

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mSet->shallowSizeOfIncludingThis(mallocSizeOf);
  }

  using Range = AtomSet::Range;

  AtomSet::Range all() const { return mSet->all(); }
};

class AtomsTable {
  // Use a low initial capacity for atom hash tables to avoid penalizing
  // runtimes which create a small number of atoms.
  static const size_t InitialTableSize = 16;

  // The main atoms set.
  AtomSet atoms;

  // Set of atoms added while the |atoms| set is being swept.
  AtomSet* atomsAddedWhileSweeping;

  // List of pinned atoms that are traced in every GC.
  Vector<JSAtom*, 0, SystemAllocPolicy> pinnedAtoms;

 public:
  // An iterator used for sweeping atoms incrementally.
  using SweepIterator = AtomSet::Enum;

  AtomsTable();
  ~AtomsTable();
  bool init();

  template <typename CharT>
  MOZ_ALWAYS_INLINE JSAtom* atomizeAndCopyCharsNonStaticValidLength(
      JSContext* cx, const CharT* chars, size_t length,
      const mozilla::Maybe<uint32_t>& indexValue,
      const AtomHasher::Lookup& lookup);

  bool maybePinExistingAtom(JSContext* cx, JSAtom* atom);

  void tracePinnedAtoms(JSTracer* trc);

  // Sweep all atoms non-incrementally.
  void traceWeak(JSTracer* trc);

  bool startIncrementalSweep(mozilla::Maybe<SweepIterator>& atomsToSweepOut);

  // Sweep some atoms incrementally and return whether we finished.
  bool sweepIncrementally(SweepIterator& atomsToSweep, JS::SliceBudget& budget);

  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

 private:
  void mergeAtomsAddedWhileSweeping();
};

bool AtomIsPinned(JSContext* cx, JSAtom* atom);

}  // namespace js

#endif /* vm_AtomsTable_h */
