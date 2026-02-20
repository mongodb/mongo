/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS atom table.
 */

#include "vm/JSAtomUtils-inl.h"

#include "mozilla/HashFunctions.h"  // mozilla::HashStringKnownLength
#include "mozilla/RangedPtr.h"

#include <charconv>
#include <iterator>
#include <string.h>

#include "jstypes.h"

#include "frontend/CompilationStencil.h"
#include "gc/GC.h"
#include "gc/Marking.h"
#include "gc/MaybeRooted.h"
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Symbol.h"
#include "util/Text.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/StaticStrings.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"
#include "vm/WellKnownAtom.h"  // WellKnownAtomInfo, WellKnownAtomId, wellKnownAtomInfos
#include "gc/AtomMarking-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/Realm-inl.h"
#include "vm/StringType-inl.h"

using namespace js;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::RangedPtr;

template <typename CharT>
extern void InflateUTF8CharsToBuffer(const JS::UTF8Chars& src, CharT* dst,
                                     size_t dstLen,
                                     JS::SmallestEncoding encoding);

template <typename CharT>
extern bool UTF8EqualsChars(const JS::UTF8Chars& utf8, const CharT* chars);

extern bool GetUTF8AtomizationData(JSContext* cx, const JS::UTF8Chars& utf8,
                                   size_t* outlen,
                                   JS::SmallestEncoding* encoding,
                                   HashNumber* hashNum);

MOZ_ALWAYS_INLINE bool js::AtomHasher::Lookup::StringsMatch(
    const JSAtom& atom) const {
  if (atom.hasLatin1Chars()) {
    const Latin1Char* keyChars = atom.latin1Chars(nogc);
    switch (type) {
      case Lookup::Latin1:
        return EqualChars(keyChars, latin1Chars, length);
      case Lookup::TwoByteChar:
        return EqualChars(keyChars, twoByteChars, length);
      case Lookup::UTF8: {
        JS::UTF8Chars utf8(utf8Bytes, byteLength);
        return UTF8EqualsChars(utf8, keyChars);
      }
    }
  }

  const char16_t* keyChars = atom.twoByteChars(nogc);
  switch (type) {
    case Lookup::Latin1:
      return EqualChars(latin1Chars, keyChars, length);
    case Lookup::TwoByteChar:
      return EqualChars(keyChars, twoByteChars, length);
    case Lookup::UTF8: {
      JS::UTF8Chars utf8(utf8Bytes, byteLength);
      return UTF8EqualsChars(utf8, keyChars);
    }
  }

  MOZ_ASSERT_UNREACHABLE("AtomHasher::match unknown type");
  return false;
}

inline HashNumber js::AtomHasher::hash(const Lookup& l) { return l.hash; }

MOZ_ALWAYS_INLINE bool js::AtomHasher::match(const WeakHeapPtr<JSAtom*>& entry,
                                             const Lookup& lookup) {
  JSAtom* key = entry.unbarrieredGet();
  if (lookup.atom) {
    return lookup.atom == key;
  }
  if (key->length() != lookup.length || key->hash() != lookup.hash) {
    return false;
  }

  return lookup.StringsMatch(*key);
}

UniqueChars js::AtomToPrintableString(JSContext* cx, JSAtom* atom) {
  return QuoteString(cx, atom);
}

// Use a low initial capacity for the permanent atoms table to avoid penalizing
// runtimes that create a small number of atoms.
static const uint32_t JS_PERMANENT_ATOM_SIZE = 64;

MOZ_ALWAYS_INLINE AtomSet::Ptr js::FrozenAtomSet::readonlyThreadsafeLookup(
    const AtomSet::Lookup& l) const {
  return mSet->readonlyThreadsafeLookup(l);
}

static JSAtom* PermanentlyAtomizeCharsValidLength(JSContext* cx,
                                                  AtomSet& atomSet,
                                                  mozilla::HashNumber hash,
                                                  const Latin1Char* chars,
                                                  size_t length);

bool JSRuntime::initializeAtoms(JSContext* cx) {
  JS::AutoAssertNoGC nogc;

  MOZ_ASSERT(!atoms_);
  MOZ_ASSERT(!permanentAtoms_);

  if (parentRuntime) {
    permanentAtoms_ = parentRuntime->permanentAtoms_;

    staticStrings = parentRuntime->staticStrings;
    commonNames = parentRuntime->commonNames;
    emptyString = parentRuntime->emptyString;
    wellKnownSymbols = parentRuntime->wellKnownSymbols;

    atoms_ = js_new<AtomsTable>();
    return bool(atoms_);
  }

  // NOTE: There's no GC, but `gc.freezePermanentSharedThings` below contains
  //       a function call that's marked as "Can GC".
  Rooted<UniquePtr<AtomSet>> atomSet(cx,
                                     cx->new_<AtomSet>(JS_PERMANENT_ATOM_SIZE));
  if (!atomSet) {
    return false;
  }

  staticStrings = js_new<StaticStrings>();
  if (!staticStrings || !staticStrings->init(cx)) {
    return false;
  }

  // The bare symbol names are already part of the well-known set, but their
  // descriptions are not, so enumerate them here and add them to the initial
  // permanent atoms set below.
  static const WellKnownAtomInfo symbolDescInfo[] = {
#define COMMON_NAME_INFO(NAME)                                  \
  {uint32_t(sizeof("Symbol." #NAME) - 1),                       \
   mozilla::HashStringKnownLength("Symbol." #NAME,              \
                                  sizeof("Symbol." #NAME) - 1), \
   "Symbol." #NAME},
      JS_FOR_EACH_WELL_KNOWN_SYMBOL(COMMON_NAME_INFO)
#undef COMMON_NAME_INFO
  };

  commonNames = js_new<JSAtomState>();
  if (!commonNames) {
    return false;
  }

  ImmutableTenuredPtr<PropertyName*>* names =
      reinterpret_cast<ImmutableTenuredPtr<PropertyName*>*>(commonNames.ref());
  for (size_t i = 0; i < uint32_t(WellKnownAtomId::Limit); i++) {
    const auto& info = wellKnownAtomInfos[i];
    JSAtom* atom = PermanentlyAtomizeCharsValidLength(
        cx, *atomSet, info.hash,
        reinterpret_cast<const Latin1Char*>(info.content), info.length);
    if (!atom) {
      return false;
    }
    names->init(atom->asPropertyName());
    names++;
  }

  for (const auto& info : symbolDescInfo) {
    JSAtom* atom = PermanentlyAtomizeCharsNonStaticValidLength(
        cx, *atomSet, info.hash,
        reinterpret_cast<const Latin1Char*>(info.content), info.length);
    if (!atom) {
      return false;
    }
    names->init(atom->asPropertyName());
    names++;
  }
  MOZ_ASSERT(uintptr_t(names) == uintptr_t(commonNames + 1));

  emptyString = commonNames->empty_;

  // The self-hosted atoms are those that exist in a self-hosted JS source file,
  // but are not defined in any of the well-known atom collections.
  if (!cx->runtime()->selfHostStencil_->instantiateSelfHostedAtoms(
          cx, *atomSet, cx->runtime()->selfHostStencilInput_->atomCache)) {
    return false;
  }

  // Create the well-known symbols.
  auto wks = js_new<WellKnownSymbols>();
  if (!wks) {
    return false;
  }

  {
    // Prevent GC until we have fully initialized the well known symbols table.
    // Faster than zeroing the array and null checking during every GC.
    gc::AutoSuppressGC nogc(cx);

    ImmutableTenuredPtr<PropertyName*>* descriptions =
        commonNames->wellKnownSymbolDescriptions();
    ImmutableTenuredPtr<JS::Symbol*>* symbols =
        reinterpret_cast<ImmutableTenuredPtr<JS::Symbol*>*>(wks);
    for (size_t i = 0; i < JS::WellKnownSymbolLimit; i++) {
      JS::Symbol* symbol =
          JS::Symbol::newWellKnown(cx, JS::SymbolCode(i), descriptions[i]);
      if (!symbol) {
        ReportOutOfMemory(cx);
        return false;
      }
      symbols[i].init(symbol);
    }

    wellKnownSymbols = wks;
  }

  if (!gc.freezeSharedAtomsZone()) {
    return false;
  }

  // The permanent atoms table has now been populated.
  permanentAtoms_ =
      js_new<FrozenAtomSet>(atomSet.release());  // Takes ownership.
  if (!permanentAtoms_) {
    return false;
  }

  // Initialize the main atoms table.
  atoms_ = js_new<AtomsTable>();
  if (!atoms_) {
    return false;
  }

  return true;
}

void JSRuntime::finishAtoms() {
  js_delete(atoms_.ref());

  if (!parentRuntime) {
    js_delete(permanentAtoms_.ref());
    js_delete(staticStrings.ref());
    js_delete(commonNames.ref());
    js_delete(wellKnownSymbols.ref());
  }

  atoms_ = nullptr;
  permanentAtoms_ = nullptr;
  staticStrings = nullptr;
  commonNames = nullptr;
  wellKnownSymbols = nullptr;
  emptyString = nullptr;
}

AtomsTable::AtomsTable()
    : atoms(InitialTableSize), atomsAddedWhileSweeping(nullptr) {}

AtomsTable::~AtomsTable() { MOZ_ASSERT(!atomsAddedWhileSweeping); }

void AtomsTable::tracePinnedAtoms(JSTracer* trc) {
  for (JSAtom* atom : pinnedAtoms) {
    TraceRoot(trc, &atom, "pinned atom");
  }
}

void js::TraceAtoms(JSTracer* trc) {
  JSRuntime* rt = trc->runtime();
  if (rt->permanentAtomsPopulated()) {
    rt->atoms().tracePinnedAtoms(trc);
  }
}

void AtomsTable::traceWeak(JSTracer* trc) {
  for (AtomSet::Enum e(atoms); !e.empty(); e.popFront()) {
    JSAtom* atom = e.front().unbarrieredGet();
    MOZ_DIAGNOSTIC_ASSERT(atom);
    if (!TraceManuallyBarrieredWeakEdge(trc, &atom, "AtomsTable::atoms")) {
      e.removeFront();
    } else {
      MOZ_ASSERT(atom == e.front().unbarrieredGet());
    }
  }
}

bool AtomsTable::startIncrementalSweep(Maybe<SweepIterator>& atomsToSweepOut) {
  MOZ_ASSERT(JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT(atomsToSweepOut.isNothing());
  MOZ_ASSERT(!atomsAddedWhileSweeping);

  atomsAddedWhileSweeping = js_new<AtomSet>();
  if (!atomsAddedWhileSweeping) {
    return false;
  }

  atomsToSweepOut.emplace(atoms);

  return true;
}

void AtomsTable::mergeAtomsAddedWhileSweeping() {
  // Add atoms that were added to the secondary table while we were sweeping
  // the main table.

  AutoEnterOOMUnsafeRegion oomUnsafe;

  auto newAtoms = atomsAddedWhileSweeping;
  atomsAddedWhileSweeping = nullptr;

  for (auto r = newAtoms->all(); !r.empty(); r.popFront()) {
    if (!atoms.putNew(AtomHasher::Lookup(r.front().unbarrieredGet()),
                      r.front())) {
      oomUnsafe.crash("Adding atom from secondary table after sweep");
    }
  }

  js_delete(newAtoms);
}

bool AtomsTable::sweepIncrementally(SweepIterator& atomsToSweep,
                                    JS::SliceBudget& budget) {
  // Sweep the table incrementally until we run out of work or budget.
  while (!atomsToSweep.empty()) {
    budget.step();
    if (budget.isOverBudget()) {
      return false;
    }

    JSAtom* atom = atomsToSweep.front().unbarrieredGet();
    MOZ_DIAGNOSTIC_ASSERT(atom);
    if (IsAboutToBeFinalizedUnbarriered(atom)) {
      MOZ_ASSERT(!atom->isPinned());
      atomsToSweep.removeFront();
    } else {
      MOZ_ASSERT(atom == atomsToSweep.front().unbarrieredGet());
    }
    atomsToSweep.popFront();
  }

  mergeAtomsAddedWhileSweeping();
  return true;
}

size_t AtomsTable::sizeOfIncludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t size = sizeof(AtomsTable);
  size += atoms.shallowSizeOfExcludingThis(mallocSizeOf);
  if (atomsAddedWhileSweeping) {
    size += atomsAddedWhileSweeping->shallowSizeOfExcludingThis(mallocSizeOf);
  }
  size += pinnedAtoms.sizeOfExcludingThis(mallocSizeOf);
  return size;
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom*
AtomizeAndCopyCharsNonStaticValidLengthFromLookup(
    JSContext* cx, const CharT* chars, size_t length,
    const AtomHasher::Lookup& lookup, const Maybe<uint32_t>& indexValue) {
  Zone* zone = cx->zone();
  MOZ_ASSERT(zone);

  AtomCacheHashTable* atomCache = zone->atomCache();

  // Try the per-Zone cache first. If we find the atom there we can avoid the
  // markAtom call, and the multiple HashSet lookups below.
  if (MOZ_LIKELY(atomCache)) {
    JSAtom* const cachedAtom = atomCache->lookupForAdd(lookup);
    if (cachedAtom) {
      // The cache is purged on GC so if we're in the middle of an incremental
      // GC we should have barriered the atom when we put it in the cache.
      MOZ_ASSERT(AtomIsMarked(zone, cachedAtom));
      return cachedAtom;
    }
  }

  MOZ_ASSERT(cx->permanentAtomsPopulated());

  AtomSet::Ptr pp = cx->permanentAtoms().readonlyThreadsafeLookup(lookup);
  if (pp) {
    JSAtom* atom = pp->get();
    if (MOZ_LIKELY(atomCache)) {
      atomCache->add(lookup.hash, atom);
    }
    return atom;
  }

  JSAtom* atom = cx->atoms().atomizeAndCopyCharsNonStaticValidLength(
      cx, chars, length, indexValue, lookup);
  if (!atom) {
    return nullptr;
  }

  if (MOZ_UNLIKELY(!cx->atomMarking().inlinedMarkAtomFallible(cx, atom))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  if (MOZ_LIKELY(atomCache)) {
    atomCache->add(lookup.hash, atom);
  }
  return atom;
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* AllocateNewAtomNonStaticValidLength(
    JSContext* cx, const CharT* chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup);

template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* AllocateNewPermanentAtomNonStaticValidLength(
    JSContext* cx, const CharT* chars, size_t length,
    const AtomHasher::Lookup& lookup);

template <typename CharT>
MOZ_ALWAYS_INLINE JSAtom* AtomsTable::atomizeAndCopyCharsNonStaticValidLength(
    JSContext* cx, const CharT* chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup) {
  AtomSet::AddPtr p;

  if (!atomsAddedWhileSweeping) {
    p = atoms.lookupForAdd(lookup);
  } else {
    // We're currently sweeping the main atoms table and all new atoms will
    // be added to a secondary table. Check this first.
    p = atomsAddedWhileSweeping->lookupForAdd(lookup);

    // If that fails check the main table but check if any atom found there
    // is dead.
    if (!p) {
      if (AtomSet::AddPtr p2 = atoms.lookupForAdd(lookup)) {
        JSAtom* atom = p2->unbarrieredGet();
        if (!IsAboutToBeFinalizedUnbarriered(atom)) {
          p = p2;
        }
      }
    }
  }

  if (p) {
    return p->get();
  }

  JSAtom* atom = AllocateNewAtomNonStaticValidLength(cx, chars, length,
                                                     indexValue, lookup);
  if (!atom) {
    return nullptr;
  }

  // The operations above can't GC; therefore the atoms table has not been
  // modified and p is still valid.
  AtomSet* addSet = atomsAddedWhileSweeping ? atomsAddedWhileSweeping : &atoms;
  if (MOZ_UNLIKELY(!addSet->add(p, atom))) {
    ReportOutOfMemory(cx); /* SystemAllocPolicy does not report OOM. */
    return nullptr;
  }

  return atom;
}

/* |chars| must not point into an inline or short string. */
template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* AtomizeAndCopyChars(
    JSContext* cx, const CharT* chars, size_t length,
    const Maybe<uint32_t>& indexValue, const Maybe<js::HashNumber>& hash) {
  if (JSAtom* s = cx->staticStrings().lookup(chars, length)) {
    return s;
  }

  if (MOZ_UNLIKELY(!JSString::validateLength(cx, length))) {
    return nullptr;
  }

  if (hash.isSome()) {
    AtomHasher::Lookup lookup(hash.value(), chars, length);
    return AtomizeAndCopyCharsNonStaticValidLengthFromLookup(
        cx, chars, length, lookup, indexValue);
  }

  AtomHasher::Lookup lookup(chars, length);
  return AtomizeAndCopyCharsNonStaticValidLengthFromLookup(cx, chars, length,
                                                           lookup, indexValue);
}

template <typename CharT>
static MOZ_NEVER_INLINE JSAtom*
PermanentlyAtomizeAndCopyCharsNonStaticValidLength(
    JSContext* cx, AtomSet& atomSet, const CharT* chars, size_t length,
    const AtomHasher::Lookup& lookup) {
  MOZ_ASSERT(!cx->permanentAtomsPopulated());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));

  AtomSet::AddPtr p = atomSet.lookupForAdd(lookup);
  if (p) {
    return p->get();
  }

  JSAtom* atom =
      AllocateNewPermanentAtomNonStaticValidLength(cx, chars, length, lookup);
  if (!atom) {
    return nullptr;
  }

  // We are single threaded at this point, and the operations we've done since
  // then can't GC; therefore the atoms table has not been modified and p is
  // still valid.
  if (!atomSet.add(p, atom)) {
    ReportOutOfMemory(cx); /* SystemAllocPolicy does not report OOM. */
    return nullptr;
  }

  return atom;
}

struct AtomizeUTF8CharsWrapper {
  JS::UTF8Chars utf8;
  JS::SmallestEncoding encoding;

  AtomizeUTF8CharsWrapper(const JS::UTF8Chars& chars,
                          JS::SmallestEncoding minEncode)
      : utf8(chars), encoding(minEncode) {}
};

// NewAtomNonStaticValidLength has 3 variants.
// This is used by Latin1Char and char16_t.
template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* NewAtomNonStaticValidLength(
    JSContext* cx, const CharT* chars, size_t length, js::HashNumber hash) {
  return NewAtomCopyNMaybeDeflateValidLength(cx, chars, length, hash);
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* MakeUTF8AtomHelperNonStaticValidLength(
    JSContext* cx, const AtomizeUTF8CharsWrapper* chars, size_t length,
    js::HashNumber hash) {
  if (JSAtom::lengthFitsInline<CharT>(length)) {
    CharT* storage;
    JSAtom* str = AllocateInlineAtom(cx, length, &storage, hash);
    if (!str) {
      return nullptr;
    }

    InflateUTF8CharsToBuffer(chars->utf8, storage, length, chars->encoding);
    return str;
  }

  // MakeAtomUTF8Helper is called from deep in the Atomization path, which
  // expects functions to fail gracefully with nullptr on OOM, without throwing.
  JSString::OwnedChars<CharT> newChars(
      AllocAtomCharsValidLength<CharT>(cx, length));
  if (!newChars) {
    return nullptr;
  }

  InflateUTF8CharsToBuffer(chars->utf8, newChars.data(), length,
                           chars->encoding);

  return JSAtom::newValidLength<CharT>(cx, newChars, hash);
}

// Another variant of NewAtomNonStaticValidLength.
static MOZ_ALWAYS_INLINE JSAtom* NewAtomNonStaticValidLength(
    JSContext* cx, const AtomizeUTF8CharsWrapper* chars, size_t length,
    js::HashNumber hash) {
  if (length == 0) {
    return cx->emptyString();
  }

  if (chars->encoding == JS::SmallestEncoding::UTF16) {
    return MakeUTF8AtomHelperNonStaticValidLength<char16_t>(cx, chars, length,
                                                            hash);
  }
  return MakeUTF8AtomHelperNonStaticValidLength<JS::Latin1Char>(cx, chars,
                                                                length, hash);
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* AllocateNewAtomNonStaticValidLength(
    JSContext* cx, const CharT* chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup) {
  AutoAllocInAtomsZone ac(cx);

  JSAtom* atom = NewAtomNonStaticValidLength(cx, chars, length, lookup.hash);
  if (!atom) {
    // Grudgingly forgo last-ditch GC. The alternative would be to manually GC
    // here, and retry from the top.
    ReportOutOfMemory(cx);
    return nullptr;
  }

  MOZ_ASSERT(atom->hash() == lookup.hash);

  if (indexValue) {
    atom->setIsIndex(*indexValue);
  } else {
    // We need to call isIndexSlow directly to avoid the flag check in isIndex,
    // because we still have to initialize that flag.
    uint32_t index;
    if (atom->isIndexSlow(&index)) {
      atom->setIsIndex(index);
    }
  }

  return atom;
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* AllocateNewPermanentAtomNonStaticValidLength(
    JSContext* cx, const CharT* chars, size_t length,
    const AtomHasher::Lookup& lookup) {
  AutoAllocInAtomsZone ac(cx);

#ifdef DEBUG
  if constexpr (std::is_same_v<CharT, char16_t>) {
    // Can call DontDeflate variant.
    MOZ_ASSERT(!CanStoreCharsAsLatin1(chars, length));
  }
#endif

  JSAtom* atom =
      NewAtomCopyNDontDeflateValidLength(cx, chars, length, lookup.hash);
  if (!atom) {
    // Do not bother with a last-ditch GC here since we are very early in
    // startup and there is no potential garbage to collect.
    ReportOutOfMemory(cx);
    return nullptr;
  }
  atom->makePermanent();

  MOZ_ASSERT(atom->hash() == lookup.hash);

  uint32_t index;
  if (atom->isIndexSlow(&index)) {
    atom->setIsIndex(index);
  }

  return atom;
}

JSAtom* js::AtomizeStringSlow(JSContext* cx, JSString* str) {
  MOZ_ASSERT(!str->isAtom());

  if (str->isAtomRef()) {
    return str->atom();
  }

  if (JSAtom* atom = cx->caches().stringToAtomCache.lookup(str)) {
    return atom;
  }

  JS::Latin1Char flattenRope[StringToAtomCache::MinStringLength];
  mozilla::Maybe<StringToAtomCache::AtomTableKey> key;
  size_t length = str->length();
  if (str->isRope() && length < StringToAtomCache::MinStringLength &&
      str->hasLatin1Chars()) {
    StringSegmentRange<StringToAtomCache::MinStringLength> iter(cx);
    if (iter.init(str)) {
      size_t index = 0;
      do {
        const JSLinearString* s = iter.front();
        CopyChars(flattenRope + index, *s);
        index += s->length();
      } while (iter.popFront() && !iter.empty());

      if (JSAtom* atom = cx->caches().stringToAtomCache.lookupWithRopeChars(
              flattenRope, length, key)) {
        // Since this cache lookup is based on a string comparison, not object
        // identity, need to mark atom explicitly in this case. And this is
        // not done in lookup() itself, because #including JSContext.h there
        // causes some non-trivial #include ordering issues.
        cx->markAtom(atom);
        str->tryReplaceWithAtomRef(atom);
        return atom;
      }
    }
  }

  Maybe<uint32_t> indexValue;
  if (str->hasIndexValue()) {
    indexValue.emplace(str->getIndexValue());
  }

  JSAtom* atom = nullptr;
  if (key.isSome()) {
    atom = AtomizeAndCopyChars(cx, key.value().string_, key.value().length_,
                               indexValue, mozilla::Some(key.value().hash_));
  } else {
    JSLinearString* linear = str->ensureLinear(cx);
    if (!linear) {
      return nullptr;
    }

    JS::AutoCheckCannotGC nogc;
    atom = linear->hasLatin1Chars()
               ? AtomizeAndCopyChars(cx, linear->latin1Chars(nogc),
                                     linear->length(), indexValue, Nothing())
               : AtomizeAndCopyChars(cx, linear->twoByteChars(nogc),
                                     linear->length(), indexValue, Nothing());
  }

  if (!atom) {
    return nullptr;
  }

  if (!str->tryReplaceWithAtomRef(atom)) {
    cx->caches().stringToAtomCache.maybePut(str, atom, key);
  }

  return atom;
}

bool js::AtomIsPinned(JSContext* cx, JSAtom* atom) { return atom->isPinned(); }

bool js::PinAtom(JSContext* cx, JSAtom* atom) {
  JS::AutoCheckCannotGC nogc;
  return cx->runtime()->atoms().maybePinExistingAtom(cx, atom);
}

bool AtomsTable::maybePinExistingAtom(JSContext* cx, JSAtom* atom) {
  MOZ_ASSERT(atom);

  if (atom->isPinned()) {
    return true;
  }

  if (!pinnedAtoms.append(atom)) {
    return false;
  }

  atom->setPinned();
  return true;
}

JSAtom* js::Atomize(JSContext* cx, const char* bytes, size_t length,
                    const Maybe<uint32_t>& indexValue) {
  const Latin1Char* chars = reinterpret_cast<const Latin1Char*>(bytes);
  return AtomizeAndCopyChars(cx, chars, length, indexValue, Nothing());
}

template <typename CharT>
JSAtom* js::AtomizeChars(JSContext* cx, const CharT* chars, size_t length) {
  return AtomizeAndCopyChars(cx, chars, length, Nothing(), Nothing());
}

template JSAtom* js::AtomizeChars(JSContext* cx, const Latin1Char* chars,
                                  size_t length);

template JSAtom* js::AtomizeChars(JSContext* cx, const char16_t* chars,
                                  size_t length);

JSAtom* js::AtomizeWithoutActiveZone(JSContext* cx, const char* bytes,
                                     size_t length) {
  // This is used to implement JS_AtomizeAndPinString{N} when called without an
  // active zone. This simplifies the normal atomization code because it can
  // assume a non-null cx->zone().

  MOZ_ASSERT(!cx->zone());
  MOZ_ASSERT(cx->permanentAtomsPopulated());

  const Latin1Char* chars = reinterpret_cast<const Latin1Char*>(bytes);

  if (JSAtom* s = cx->staticStrings().lookup(chars, length)) {
    return s;
  }

  if (MOZ_UNLIKELY(!JSString::validateLength(cx, length))) {
    return nullptr;
  }

  AtomHasher::Lookup lookup(chars, length);
  if (AtomSet::Ptr pp = cx->permanentAtoms().readonlyThreadsafeLookup(lookup)) {
    return pp->get();
  }

  return cx->atoms().atomizeAndCopyCharsNonStaticValidLength(cx, chars, length,
                                                             Nothing(), lookup);
}

/* |chars| must not point into an inline or short string. */
template <typename CharT>
JSAtom* js::AtomizeCharsNonStaticValidLength(JSContext* cx, HashNumber hash,
                                             const CharT* chars,
                                             size_t length) {
  MOZ_ASSERT(!cx->staticStrings().lookup(chars, length));

  AtomHasher::Lookup lookup(hash, chars, length);
  return AtomizeAndCopyCharsNonStaticValidLengthFromLookup(cx, chars, length,
                                                           lookup, Nothing());
}

template JSAtom* js::AtomizeCharsNonStaticValidLength(JSContext* cx,
                                                      HashNumber hash,
                                                      const Latin1Char* chars,
                                                      size_t length);

template JSAtom* js::AtomizeCharsNonStaticValidLength(JSContext* cx,
                                                      HashNumber hash,
                                                      const char16_t* chars,
                                                      size_t length);

static JSAtom* PermanentlyAtomizeCharsValidLength(JSContext* cx,
                                                  AtomSet& atomSet,
                                                  HashNumber hash,
                                                  const Latin1Char* chars,
                                                  size_t length) {
  if (JSAtom* s = cx->staticStrings().lookup(chars, length)) {
    return s;
  }

  return PermanentlyAtomizeCharsNonStaticValidLength(cx, atomSet, hash, chars,
                                                     length);
}

JSAtom* js::PermanentlyAtomizeCharsNonStaticValidLength(JSContext* cx,
                                                        AtomSet& atomSet,
                                                        HashNumber hash,
                                                        const Latin1Char* chars,
                                                        size_t length) {
  MOZ_ASSERT(!cx->staticStrings().lookup(chars, length));
  MOZ_ASSERT(length <= JSString::MAX_LENGTH);

  AtomHasher::Lookup lookup(hash, chars, length);
  return PermanentlyAtomizeAndCopyCharsNonStaticValidLength(cx, atomSet, chars,
                                                            length, lookup);
}

JSAtom* js::AtomizeUTF8Chars(JSContext* cx, const char* utf8Chars,
                             size_t utf8ByteLength) {
  {
    StaticStrings& statics = cx->staticStrings();

    // Handle all pure-ASCII UTF-8 static strings.
    if (JSAtom* s = statics.lookup(utf8Chars, utf8ByteLength)) {
      return s;
    }

    // The only non-ASCII static strings are the single-code point strings
    // U+0080 through U+00FF, encoded as
    //
    //   0b1100'00xx 0b10xx'xxxx
    //
    // where the encoded code point is the concatenation of the 'x' bits -- and
    // where the highest 'x' bit is necessarily 1 (because U+0080 through U+00FF
    // all contain an 0x80 bit).
    if (utf8ByteLength == 2) {
      auto first = static_cast<uint8_t>(utf8Chars[0]);
      if ((first & 0b1111'1110) == 0b1100'0010) {
        auto second = static_cast<uint8_t>(utf8Chars[1]);
        if (mozilla::IsTrailingUnit(mozilla::Utf8Unit(second))) {
          uint8_t unit =
              static_cast<uint8_t>(first << 6) | (second & 0b0011'1111);

          MOZ_ASSERT(StaticStrings::hasUnit(unit));
          return statics.getUnit(unit);
        }
      }

      // Fallthrough code handles the cases where the two units aren't a Latin-1
      // code point or are invalid.
    }
  }

  size_t length;
  HashNumber hash;
  JS::SmallestEncoding forCopy;
  JS::UTF8Chars utf8(utf8Chars, utf8ByteLength);
  if (!GetUTF8AtomizationData(cx, utf8, &length, &forCopy, &hash)) {
    return nullptr;
  }

  if (MOZ_UNLIKELY(!JSString::validateLength(cx, length))) {
    return nullptr;
  }

  AtomizeUTF8CharsWrapper chars(utf8, forCopy);
  AtomHasher::Lookup lookup(utf8Chars, utf8ByteLength, length, hash);
  return AtomizeAndCopyCharsNonStaticValidLengthFromLookup(cx, &chars, length,
                                                           lookup, Nothing());
}

bool js::IndexToIdSlow(JSContext* cx, uint32_t index, MutableHandleId idp) {
  MOZ_ASSERT(index > JS::PropertyKey::IntMax);

  char buf[UINT32_CHAR_BUFFER_LENGTH];

  auto result = std::to_chars(buf, buf + std::size(buf), index, 10);
  MOZ_ASSERT(result.ec == std::errc());

  size_t length = result.ptr - buf;
  JSAtom* atom = Atomize(cx, buf, length);
  if (!atom) {
    return false;
  }

  idp.set(JS::PropertyKey::NonIntAtom(atom));
  return true;
}

template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE JSAtom* PrimitiveToAtom(JSContext* cx,
                                                 const Value& v) {
  MOZ_ASSERT(v.isPrimitive());
  switch (v.type()) {
    case ValueType::String: {
      JSAtom* atom = AtomizeString(cx, v.toString());
      if (!allowGC && !atom) {
        cx->recoverFromOutOfMemory();
      }
      return atom;
    }
    case ValueType::Int32: {
      JSAtom* atom = Int32ToAtom(cx, v.toInt32());
      if (!allowGC && !atom) {
        cx->recoverFromOutOfMemory();
      }
      return atom;
    }
    case ValueType::Double: {
      JSAtom* atom = NumberToAtom(cx, v.toDouble());
      if (!allowGC && !atom) {
        cx->recoverFromOutOfMemory();
      }
      return atom;
    }
    case ValueType::Boolean:
      return v.toBoolean() ? cx->names().true_ : cx->names().false_;
    case ValueType::Null:
      return cx->names().null;
    case ValueType::Undefined:
      return cx->names().undefined;
    case ValueType::Symbol:
      if constexpr (allowGC) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_SYMBOL_TO_STRING);
      }
      return nullptr;
    case ValueType::BigInt: {
      RootedBigInt i(cx, v.toBigInt());
      return BigIntToAtom<allowGC>(cx, i);
    }
    case ValueType::Object:
    case ValueType::Magic:
    case ValueType::PrivateGCThing:
      break;
  }
  MOZ_CRASH("Unexpected type");
}

template <AllowGC allowGC>
static JSAtom* ToAtomSlow(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType arg) {
  MOZ_ASSERT(!arg.isString());

  Value v = arg;
  if (!v.isPrimitive()) {
    if (!allowGC) {
      return nullptr;
    }
    RootedValue v2(cx, v);
    if (!ToPrimitive(cx, JSTYPE_STRING, &v2)) {
      return nullptr;
    }
    v = v2;
  }

  return PrimitiveToAtom<allowGC>(cx, v);
}

template <AllowGC allowGC>
JSAtom* js::ToAtom(JSContext* cx,
                   typename MaybeRooted<Value, allowGC>::HandleType v) {
  if (!v.isString()) {
    return ToAtomSlow<allowGC>(cx, v);
  }

  JSAtom* atom = AtomizeString(cx, v.toString());
  if (!atom && !allowGC) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    cx->recoverFromOutOfMemory();
  }
  return atom;
}

template JSAtom* js::ToAtom<CanGC>(JSContext* cx, HandleValue v);
template JSAtom* js::ToAtom<NoGC>(JSContext* cx, const Value& v);

template <AllowGC allowGC>
bool js::PrimitiveValueToIdSlow(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType v,
    typename MaybeRooted<jsid, allowGC>::MutableHandleType idp) {
  MOZ_ASSERT(v.isPrimitive());
  MOZ_ASSERT(!v.isString());
  MOZ_ASSERT(!v.isSymbol());
  MOZ_ASSERT_IF(v.isInt32(), !PropertyKey::fitsInInt(v.toInt32()));

  int32_t i;
  if (v.isDouble() && mozilla::NumberEqualsInt32(v.toDouble(), &i) &&
      PropertyKey::fitsInInt(i)) {
    idp.set(PropertyKey::Int(i));
    return true;
  }

  JSAtom* atom = PrimitiveToAtom<allowGC>(cx, v);
  if (!atom) {
    return false;
  }

  idp.set(AtomToId(atom));
  return true;
}

template bool js::PrimitiveValueToIdSlow<CanGC>(JSContext* cx, HandleValue v,
                                                MutableHandleId idp);
template bool js::PrimitiveValueToIdSlow<NoGC>(JSContext* cx, const Value& v,
                                               FakeMutableHandle<jsid> idp);

Handle<PropertyName*> js::ClassName(JSProtoKey key, JSContext* cx) {
  return ClassName(key, cx->names());
}
