/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS atom table.
 */

#include "vm/JSAtom-inl.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/HashFunctions.h"  // mozilla::HashStringKnownLength
#include "mozilla/RangedPtr.h"

#include <iterator>
#include <string.h>

#include "jstypes.h"

#include "gc/GC.h"
#include "gc/Marking.h"
#include "gc/MaybeRooted.h"
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Symbol.h"
#include "util/Text.h"
#include "vm/JSContext.h"
#include "vm/SymbolType.h"
#include "vm/WellKnownAtom.h"  // js_*_str
#include "vm/Xdr.h"

#include "gc/AtomMarking-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/Realm-inl.h"
#include "vm/StringType-inl.h"

using namespace js;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::RangedPtr;

template <typename CharT>
extern void InflateUTF8CharsToBufferAndTerminate(const JS::UTF8Chars src,
                                                 CharT* dst, size_t dstLen,
                                                 JS::SmallestEncoding encoding);

template <typename CharT>
extern bool UTF8EqualsChars(const JS::UTF8Chars utf8, const CharT* chars);

extern bool GetUTF8AtomizationData(JSContext* cx, const JS::UTF8Chars utf8,
                                   size_t* outlen,
                                   JS::SmallestEncoding* encoding,
                                   HashNumber* hashNum);

struct js::AtomHasher::Lookup {
  union {
    const JS::Latin1Char* latin1Chars;
    const char16_t* twoByteChars;
    LittleEndianChars littleEndianChars;
    const char* utf8Bytes;
  };
  enum { TwoByteChar, LittleEndianTwoByte, Latin1, UTF8 } type;
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

  MOZ_ALWAYS_INLINE Lookup(LittleEndianChars chars, size_t length)
      : littleEndianChars(chars),
        type(LittleEndianTwoByte),
        length(length),
        atom(nullptr),
        hash(mozilla::HashStringKnownLength(chars, length)) {}
};

inline HashNumber js::AtomHasher::hash(const Lookup& l) { return l.hash; }

MOZ_ALWAYS_INLINE bool js::AtomHasher::match(const AtomStateEntry& entry,
                                             const Lookup& lookup) {
  JSAtom* key = entry.asPtrUnbarriered();
  if (lookup.atom) {
    return lookup.atom == key;
  }
  if (key->length() != lookup.length || key->hash() != lookup.hash) {
    return false;
  }

  auto EqualsLittleEndianChars = [&lookup](auto keyChars) {
    for (size_t i = 0, len = lookup.length; i < len; i++) {
      if (keyChars[i] != lookup.littleEndianChars[i]) {
        return false;
      }
    }
    return true;
  };

  if (key->hasLatin1Chars()) {
    const Latin1Char* keyChars = key->latin1Chars(lookup.nogc);
    switch (lookup.type) {
      case Lookup::Latin1:
        return mozilla::ArrayEqual(keyChars, lookup.latin1Chars, lookup.length);
      case Lookup::TwoByteChar:
        return EqualChars(keyChars, lookup.twoByteChars, lookup.length);
      case Lookup::LittleEndianTwoByte:
        return EqualsLittleEndianChars(keyChars);
      case Lookup::UTF8: {
        JS::UTF8Chars utf8(lookup.utf8Bytes, lookup.byteLength);
        return UTF8EqualsChars(utf8, keyChars);
      }
    }
  }

  const char16_t* keyChars = key->twoByteChars(lookup.nogc);
  switch (lookup.type) {
    case Lookup::Latin1:
      return EqualChars(lookup.latin1Chars, keyChars, lookup.length);
    case Lookup::TwoByteChar:
      return mozilla::ArrayEqual(keyChars, lookup.twoByteChars, lookup.length);
    case Lookup::LittleEndianTwoByte:
      return EqualsLittleEndianChars(keyChars);
    case Lookup::UTF8: {
      JS::UTF8Chars utf8(lookup.utf8Bytes, lookup.byteLength);
      return UTF8EqualsChars(utf8, keyChars);
    }
  }

  MOZ_ASSERT_UNREACHABLE("AtomHasher::match unknown type");
  return false;
}

inline JSAtom* js::AtomStateEntry::asPtr(JSContext* cx) const {
  JSAtom* atom = asPtrUnbarriered();
  if (!cx->isHelperThreadContext()) {
    gc::ReadBarrier(atom);
  }
  return atom;
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

bool JSRuntime::initializeAtoms(JSContext* cx) {
  MOZ_ASSERT(!atoms_);
  MOZ_ASSERT(!permanentAtomsDuringInit_);
  MOZ_ASSERT(!permanentAtoms_);

  if (parentRuntime) {
    permanentAtoms_ = parentRuntime->permanentAtoms_;

    staticStrings = parentRuntime->staticStrings;
    commonNames = parentRuntime->commonNames;
    emptyString = parentRuntime->emptyString;
    wellKnownSymbols = parentRuntime->wellKnownSymbols;

    atoms_ = js_new<AtomsTable>();
    if (!atoms_) {
      return false;
    }

    return atoms_->init();
  }

  permanentAtomsDuringInit_ = js_new<AtomSet>(JS_PERMANENT_ATOM_SIZE);
  if (!permanentAtomsDuringInit_) {
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

  ImmutablePropertyNamePtr* names =
      reinterpret_cast<ImmutablePropertyNamePtr*>(commonNames.ref());
  for (size_t i = 0; i < uint32_t(WellKnownAtomId::Limit); i++) {
    const auto& info = wellKnownAtomInfos[i];
    JSAtom* atom = Atomize(cx, info.hash, info.content, info.length, PinAtom);
    if (!atom) {
      return false;
    }
    names->init(atom->asPropertyName());
    names++;
  }

  for (const auto& info : symbolDescInfo) {
    JSAtom* atom = Atomize(cx, info.hash, info.content, info.length, PinAtom);
    if (!atom) {
      return false;
    }
    names->init(atom->asPropertyName());
    names++;
  }
  MOZ_ASSERT(uintptr_t(names) == uintptr_t(commonNames + 1));

  emptyString = commonNames->empty;

  // Create the well-known symbols.
  auto wks = js_new<WellKnownSymbols>();
  if (!wks) {
    return false;
  }

  // Prevent GC until we have fully initialized the well known symbols table.
  // Faster than zeroing the array and null checking during every GC.
  gc::AutoSuppressGC nogc(cx);

  ImmutablePropertyNamePtr* descriptions =
      commonNames->wellKnownSymbolDescriptions();
  ImmutableSymbolPtr* symbols = reinterpret_cast<ImmutableSymbolPtr*>(wks);
  for (size_t i = 0; i < JS::WellKnownSymbolLimit; i++) {
    HandlePropertyName description = descriptions[i];
    JS::Symbol* symbol = JS::Symbol::new_(cx, JS::SymbolCode(i), description);
    if (!symbol) {
      ReportOutOfMemory(cx);
      return false;
    }
    symbols[i].init(symbol);
  }

  wellKnownSymbols = wks;
  return true;
}

void JSRuntime::finishAtoms() {
  js_delete(atoms_.ref());

  if (!parentRuntime) {
    js_delete(permanentAtomsDuringInit_.ref());
    js_delete(permanentAtoms_.ref());
    js_delete(staticStrings.ref());
    js_delete(commonNames.ref());
    js_delete(wellKnownSymbols.ref());
  }

  atoms_ = nullptr;
  permanentAtomsDuringInit_ = nullptr;
  permanentAtoms_ = nullptr;
  staticStrings = nullptr;
  commonNames = nullptr;
  wellKnownSymbols = nullptr;
  emptyString = nullptr;
}

class AtomsTable::AutoLock {
  Mutex* lock = nullptr;

 public:
  MOZ_ALWAYS_INLINE explicit AutoLock(JSRuntime* rt, Mutex& aLock) {
    if (rt->hasHelperThreadZones()) {
      lock = &aLock;
      lock->lock();
    }
  }

  MOZ_ALWAYS_INLINE ~AutoLock() {
    if (lock) {
      lock->unlock();
    }
  }
};

AtomsTable::Partition::Partition(uint32_t index)
    : lock(
          MutexId{mutexid::AtomsTable.name, mutexid::AtomsTable.order + index}),
      atoms(InitialTableSize),
      atomsAddedWhileSweeping(nullptr) {}

AtomsTable::Partition::~Partition() { MOZ_ASSERT(!atomsAddedWhileSweeping); }

AtomsTable::~AtomsTable() {
  for (size_t i = 0; i < PartitionCount; i++) {
    js_delete(partitions[i]);
  }
}

bool AtomsTable::init() {
  for (size_t i = 0; i < PartitionCount; i++) {
    partitions[i] = js_new<Partition>(i);
    if (!partitions[i]) {
      return false;
    }
  }
  return true;
}

void AtomsTable::lockAll() {
  MOZ_ASSERT(!allPartitionsLocked);

  for (size_t i = 0; i < PartitionCount; i++) {
    partitions[i]->lock.lock();
  }

#ifdef DEBUG
  allPartitionsLocked = true;
#endif
}

void AtomsTable::unlockAll() {
  MOZ_ASSERT(allPartitionsLocked);

  for (size_t i = 0; i < PartitionCount; i++) {
    partitions[PartitionCount - i - 1]->lock.unlock();
  }

#ifdef DEBUG
  allPartitionsLocked = false;
#endif
}

MOZ_ALWAYS_INLINE size_t
AtomsTable::getPartitionIndex(const AtomHasher::Lookup& lookup) {
  size_t index = lookup.hash >> (32 - PartitionShift);
  MOZ_ASSERT(index < PartitionCount);
  return index;
}

inline void AtomsTable::tracePinnedAtomsInSet(JSTracer* trc, AtomSet& atoms) {
  for (auto r = atoms.all(); !r.empty(); r.popFront()) {
    const AtomStateEntry& entry = r.front();
    MOZ_DIAGNOSTIC_ASSERT(entry.asPtrUnbarriered());
    if (entry.isPinned()) {
      JSAtom* atom = entry.asPtrUnbarriered();
      TraceRoot(trc, &atom, "interned_atom");
      MOZ_ASSERT(entry.asPtrUnbarriered() == atom);
    }
  }
}

void AtomsTable::tracePinnedAtoms(JSTracer* trc,
                                  const AutoAccessAtomsZone& access) {
  for (size_t i = 0; i < PartitionCount; i++) {
    Partition& part = *partitions[i];
    tracePinnedAtomsInSet(trc, part.atoms);
    if (part.atomsAddedWhileSweeping) {
      tracePinnedAtomsInSet(trc, *part.atomsAddedWhileSweeping);
    }
  }
}

void js::TraceAtoms(JSTracer* trc, const AutoAccessAtomsZone& access) {
  JSRuntime* rt = trc->runtime();
  if (rt->permanentAtomsPopulated()) {
    rt->atoms().tracePinnedAtoms(trc, access);
  }
}

static void TracePermanentAtoms(JSTracer* trc, AtomSet::Range atoms) {
  for (; !atoms.empty(); atoms.popFront()) {
    const AtomStateEntry& entry = atoms.front();
    JSAtom* atom = entry.asPtrUnbarriered();
    MOZ_ASSERT(atom->isPermanentAtom());
    TraceProcessGlobalRoot(trc, atom, "permanent atom");
  }
}

void JSRuntime::tracePermanentAtoms(JSTracer* trc) {
  // Permanent atoms only need to be traced in the runtime which owns them.
  if (parentRuntime) {
    return;
  }

  // Static strings are not included in the permanent atoms table.
  if (staticStrings) {
    staticStrings->trace(trc);
  }

  if (permanentAtomsDuringInit_) {
    TracePermanentAtoms(trc, permanentAtomsDuringInit_->all());
  }

  if (permanentAtoms_) {
    TracePermanentAtoms(trc, permanentAtoms_->all());
  }
}

void js::TraceWellKnownSymbols(JSTracer* trc) {
  JSRuntime* rt = trc->runtime();

  if (rt->parentRuntime) {
    return;
  }

  if (WellKnownSymbols* wks = rt->wellKnownSymbols) {
    for (size_t i = 0; i < JS::WellKnownSymbolLimit; i++) {
      TraceProcessGlobalRoot(trc, wks->get(i).get(), "well_known_symbol");
    }
  }
}

void AtomsTable::traceWeak(JSTracer* trc) {
  JSRuntime* rt = trc->runtime();
  for (size_t i = 0; i < PartitionCount; i++) {
    AutoLock lock(rt, partitions[i]->lock);
    AtomSet& atoms = partitions[i]->atoms;
    for (AtomSet::Enum e(atoms); !e.empty(); e.popFront()) {
      JSAtom* atom = e.front().asPtrUnbarriered();
      MOZ_DIAGNOSTIC_ASSERT(atom);
      if (!TraceManuallyBarrieredWeakEdge(trc, &atom,
                                          "AtomsTable::partitions::atoms")) {
        e.removeFront();
      } else {
        MOZ_ASSERT(atom == e.front().asPtrUnbarriered());
      }
    }
  }
}

AtomsTable::SweepIterator::SweepIterator(AtomsTable& atoms)
    : atoms(atoms), partitionIndex(0) {
  startSweepingPartition();
  settle();
}

inline void AtomsTable::SweepIterator::startSweepingPartition() {
  MOZ_ASSERT(atoms.partitions[partitionIndex]->atomsAddedWhileSweeping);
  atomsIter.emplace(atoms.partitions[partitionIndex]->atoms);
}

inline void AtomsTable::SweepIterator::finishSweepingPartition() {
  atomsIter.reset();
  atoms.mergeAtomsAddedWhileSweeping(*atoms.partitions[partitionIndex]);
}

inline void AtomsTable::SweepIterator::settle() {
  MOZ_ASSERT(!empty());

  while (atomsIter->empty()) {
    finishSweepingPartition();
    partitionIndex++;
    if (empty()) {
      return;
    }
    startSweepingPartition();
  }
}

inline bool AtomsTable::SweepIterator::empty() const {
  return partitionIndex == PartitionCount;
}

inline AtomStateEntry AtomsTable::SweepIterator::front() const {
  MOZ_ASSERT(!empty());
  return atomsIter->front();
}

inline void AtomsTable::SweepIterator::removeFront() {
  MOZ_ASSERT(!empty());
  return atomsIter->removeFront();
}

inline void AtomsTable::SweepIterator::popFront() {
  MOZ_ASSERT(!empty());
  atomsIter->popFront();
  settle();
}

bool AtomsTable::startIncrementalSweep() {
  MOZ_ASSERT(JS::RuntimeHeapIsCollecting());

  bool ok = true;
  for (size_t i = 0; i < PartitionCount; i++) {
    auto& part = *partitions[i];

    auto newAtoms = js_new<AtomSet>();
    if (!newAtoms) {
      ok = false;
      break;
    }

    MOZ_ASSERT(!part.atomsAddedWhileSweeping);
    part.atomsAddedWhileSweeping = newAtoms;
  }

  if (!ok) {
    for (size_t i = 0; i < PartitionCount; i++) {
      auto& part = *partitions[i];
      js_delete(part.atomsAddedWhileSweeping);
      part.atomsAddedWhileSweeping = nullptr;
    }
  }

  return ok;
}

void AtomsTable::mergeAtomsAddedWhileSweeping(Partition& part) {
  // Add atoms that were added to the secondary table while we were sweeping
  // the main table.

  AutoEnterOOMUnsafeRegion oomUnsafe;

  auto newAtoms = part.atomsAddedWhileSweeping;
  part.atomsAddedWhileSweeping = nullptr;

  for (auto r = newAtoms->all(); !r.empty(); r.popFront()) {
    if (!part.atoms.putNew(AtomHasher::Lookup(r.front().asPtrUnbarriered()),
                           r.front())) {
      oomUnsafe.crash("Adding atom from secondary table after sweep");
    }
  }

  js_delete(newAtoms);
}

bool AtomsTable::sweepIncrementally(SweepIterator& atomsToSweep,
                                    SliceBudget& budget) {
  // Sweep the table incrementally until we run out of work or budget.
  while (!atomsToSweep.empty()) {
    budget.step();
    if (budget.isOverBudget()) {
      return false;
    }

    AtomStateEntry entry = atomsToSweep.front();
    JSAtom* atom = entry.asPtrUnbarriered();
    MOZ_DIAGNOSTIC_ASSERT(atom);
    if (IsAboutToBeFinalizedUnbarriered(&atom)) {
      MOZ_ASSERT(!entry.isPinned());
      atomsToSweep.removeFront();
    } else {
      MOZ_ASSERT(atom == entry.asPtrUnbarriered());
    }
    atomsToSweep.popFront();
  }

  for (size_t i = 0; i < PartitionCount; i++) {
    MOZ_ASSERT(!partitions[i]->atomsAddedWhileSweeping);
  }

  return true;
}

size_t AtomsTable::sizeOfIncludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t size = sizeof(AtomsTable);
  for (size_t i = 0; i < PartitionCount; i++) {
    size += sizeof(Partition);
    size += partitions[i]->atoms.shallowSizeOfExcludingThis(mallocSizeOf);
  }
  return size;
}

bool JSRuntime::initMainAtomsTables(JSContext* cx) {
  MOZ_ASSERT(!parentRuntime);
  MOZ_ASSERT(!permanentAtomsPopulated());

  // The permanent atoms table has now been populated.
  permanentAtoms_ =
      js_new<FrozenAtomSet>(permanentAtomsDuringInit_);  // Takes ownership.
  permanentAtomsDuringInit_ = nullptr;

  // Initialize the main atoms table.
  MOZ_ASSERT(!atoms_);
  atoms_ = js_new<AtomsTable>();
  return atoms_ && atoms_->init();
}

template <typename Chars>
static MOZ_ALWAYS_INLINE JSAtom* AtomizeAndCopyCharsFromLookup(
    JSContext* cx, Chars chars, size_t length, const AtomHasher::Lookup& lookup,
    PinningBehavior pin, const Maybe<uint32_t>& indexValue);

template <typename CharT, typename = std::enable_if_t<!std::is_const_v<CharT>>>
static MOZ_ALWAYS_INLINE JSAtom* AtomizeAndCopyCharsFromLookup(
    JSContext* cx, CharT* chars, size_t length,
    const AtomHasher::Lookup& lookup, PinningBehavior pin,
    const Maybe<uint32_t>& indexValue) {
  return AtomizeAndCopyCharsFromLookup(cx, const_cast<const CharT*>(chars),
                                       length, lookup, pin, indexValue);
}

template <typename Chars>
static MOZ_NEVER_INLINE JSAtom* PermanentlyAtomizeAndCopyChars(
    JSContext* cx, Maybe<AtomSet::AddPtr>& zonePtr, Chars chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup);

template <typename CharT, typename = std::enable_if_t<!std::is_const_v<CharT>>>
static JSAtom* PermanentlyAtomizeAndCopyChars(
    JSContext* cx, Maybe<AtomSet::AddPtr>& zonePtr, CharT* chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup) {
  return PermanentlyAtomizeAndCopyChars(
      cx, zonePtr, const_cast<const CharT*>(chars), length, indexValue, lookup);
}

template <typename Chars>
static MOZ_ALWAYS_INLINE JSAtom* AtomizeAndCopyCharsFromLookup(
    JSContext* cx, Chars chars, size_t length, const AtomHasher::Lookup& lookup,
    PinningBehavior pin, const Maybe<uint32_t>& indexValue) {
  // Try the per-Zone cache first. If we find the atom there we can avoid the
  // atoms lock, the markAtom call, and the multiple HashSet lookups below.
  // We don't use the per-Zone cache if we want a pinned atom: handling that
  // is more complicated and pinning atoms is relatively uncommon.
  Zone* zone = cx->zone();
  Maybe<AtomSet::AddPtr> zonePtr;
  if (MOZ_LIKELY(zone && pin == DoNotPinAtom)) {
    zonePtr.emplace(zone->atomCache().lookupForAdd(lookup));
    if (zonePtr.ref()) {
      // The cache is purged on GC so if we're in the middle of an
      // incremental GC we should have barriered the atom when we put
      // it in the cache.
      JSAtom* atom = zonePtr.ref()->asPtrUnbarriered();
      MOZ_ASSERT(AtomIsMarked(zone, atom));
      return atom;
    }
  }

  // This function can be called during initialization, while the permanent
  // atoms table is being created. In this case all atoms created are added to
  // the permanent atoms table.
  if (!cx->permanentAtomsPopulated()) {
    return PermanentlyAtomizeAndCopyChars(cx, zonePtr, chars, length,
                                          indexValue, lookup);
  }

  AtomSet::Ptr pp = cx->permanentAtoms().readonlyThreadsafeLookup(lookup);
  if (pp) {
    JSAtom* atom = pp->asPtr(cx);
    if (zonePtr && MOZ_UNLIKELY(!zone->atomCache().add(
                       *zonePtr, AtomStateEntry(atom, false)))) {
      ReportOutOfMemory(cx);
      return nullptr;
    }

    return atom;
  }

  // Validate the length before taking an atoms partition lock, as throwing an
  // exception here may reenter this code.
  if (MOZ_UNLIKELY(!JSString::validateLength(cx, length))) {
    return nullptr;
  }

  JSAtom* atom = cx->atoms().atomizeAndCopyChars(cx, chars, length, pin,
                                                 indexValue, lookup);
  if (!atom) {
    return nullptr;
  }

  if (MOZ_UNLIKELY(!cx->atomMarking().inlinedMarkAtomFallible(cx, atom))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  if (zonePtr && MOZ_UNLIKELY(!zone->atomCache().add(
                     *zonePtr, AtomStateEntry(atom, false)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return atom;
}

template <typename Chars>
static MOZ_ALWAYS_INLINE JSAtom* AllocateNewAtom(
    JSContext* cx, Chars chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup);

template <typename CharT, typename = std::enable_if_t<!std::is_const_v<CharT>>>
static MOZ_ALWAYS_INLINE JSAtom* AllocateNewAtom(
    JSContext* cx, CharT* chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup) {
  return AllocateNewAtom(cx, const_cast<const CharT*>(chars), length,
                         indexValue, lookup);
}

template <typename Chars>
MOZ_ALWAYS_INLINE JSAtom* AtomsTable::atomizeAndCopyChars(
    JSContext* cx, Chars chars, size_t length, PinningBehavior pin,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup) {
  Partition& part = *partitions[getPartitionIndex(lookup)];
  AutoLock lock(cx->runtime(), part.lock);

  AtomSet& atoms = part.atoms;
  AtomSet* atomsAddedWhileSweeping = part.atomsAddedWhileSweeping;
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
        JSAtom* atom = p2->asPtrUnbarriered();
        if (!IsAboutToBeFinalizedUnbarriered(&atom)) {
          p = p2;
        }
      }
    }
  }

  if (p) {
    JSAtom* atom = p->asPtr(cx);
    if (pin && !p->isPinned()) {
      p->setPinned(true);
    }
    return atom;
  }

  JSAtom* atom = AllocateNewAtom(cx, chars, length, indexValue, lookup);
  if (!atom) {
    return nullptr;
  }

  // We have held the lock since looking up p, and the operations we've done
  // since then can't GC; therefore the atoms table has not been modified and
  // p is still valid.
  AtomSet* addSet =
      part.atomsAddedWhileSweeping ? part.atomsAddedWhileSweeping : &atoms;
  if (MOZ_UNLIKELY(!addSet->add(p, AtomStateEntry(atom, bool(pin))))) {
    ReportOutOfMemory(cx); /* SystemAllocPolicy does not report OOM. */
    return nullptr;
  }

  return atom;
}

/* |chars| must not point into an inline or short string. */
template <typename CharT>
static MOZ_ALWAYS_INLINE JSAtom* AtomizeAndCopyChars(
    JSContext* cx, const CharT* chars, size_t length, PinningBehavior pin,
    const Maybe<uint32_t>& indexValue) {
  if (JSAtom* s = cx->staticStrings().lookup(chars, length)) {
    return s;
  }

  AtomHasher::Lookup lookup(chars, length);
  return AtomizeAndCopyCharsFromLookup(cx, chars, length, lookup, pin,
                                       indexValue);
}

template <typename Chars>
static MOZ_NEVER_INLINE JSAtom* PermanentlyAtomizeAndCopyChars(
    JSContext* cx, Maybe<AtomSet::AddPtr>& zonePtr, Chars chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup) {
  MOZ_ASSERT(!cx->permanentAtomsPopulated());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));

  JSRuntime* rt = cx->runtime();
  AtomSet& atoms = *rt->permanentAtomsDuringInit();
  AtomSet::AddPtr p = atoms.lookupForAdd(lookup);
  if (p) {
    return p->asPtr(cx);
  }

  JSAtom* atom = AllocateNewAtom(cx, chars, length, indexValue, lookup);
  if (!atom) {
    return nullptr;
  }

  atom->morphIntoPermanentAtom();

  // We are single threaded at this point, and the operations we've done since
  // then can't GC; therefore the atoms table has not been modified and p is
  // still valid.
  if (!atoms.add(p, AtomStateEntry(atom, true))) {
    ReportOutOfMemory(cx); /* SystemAllocPolicy does not report OOM. */
    return nullptr;
  }

  if (zonePtr && MOZ_UNLIKELY(!cx->zone()->atomCache().add(
                     *zonePtr, AtomStateEntry(atom, false)))) {
    ReportOutOfMemory(cx);
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

// MakeLinearStringForAtomization has 4 variants.
// This is used by Latin1Char and char16_t.
template <typename CharT>
static MOZ_ALWAYS_INLINE JSLinearString* MakeLinearStringForAtomization(
    JSContext* cx, const CharT* chars, size_t length) {
  return NewStringCopyN<NoGC>(cx, chars, length, gc::TenuredHeap);
}

// MakeLinearStringForAtomization has one further variant -- a non-template
// overload accepting LittleEndianChars.
static MOZ_ALWAYS_INLINE JSLinearString* MakeLinearStringForAtomization(
    JSContext* cx, LittleEndianChars chars, size_t length) {
  return NewStringFromLittleEndianNoGC(cx, chars, length, gc::TenuredHeap);
}

template <typename CharT>
static MOZ_ALWAYS_INLINE JSLinearString* MakeUTF8AtomHelper(
    JSContext* cx, const AtomizeUTF8CharsWrapper* chars, size_t length) {
  if (JSInlineString::lengthFits<CharT>(length)) {
    CharT* storage;
    JSInlineString* str =
        AllocateInlineString<NoGC>(cx, length, &storage, gc::TenuredHeap);
    if (!str) {
      return nullptr;
    }

    InflateUTF8CharsToBufferAndTerminate(chars->utf8, storage, length,
                                         chars->encoding);
    return str;
  }

  // MakeAtomUTF8Helper is called from deep in the Atomization path, which
  // expects functions to fail gracefully with nullptr on OOM, without throwing.
  //
  // Flat strings are null-terminated. Leave room with length + 1
  UniquePtr<CharT[], JS::FreePolicy> newStr(
      js_pod_arena_malloc<CharT>(js::StringBufferArena, length + 1));
  if (!newStr) {
    return nullptr;
  }

  InflateUTF8CharsToBufferAndTerminate(chars->utf8, newStr.get(), length,
                                       chars->encoding);

  return JSLinearString::new_<NoGC>(cx, std::move(newStr), length,
                                    gc::TenuredHeap);
}

// Another 2 variants of MakeLinearStringForAtomization.
static MOZ_ALWAYS_INLINE JSLinearString* MakeLinearStringForAtomization(
    JSContext* cx, const AtomizeUTF8CharsWrapper* chars, size_t length) {
  if (length == 0) {
    return cx->emptyString();
  }

  if (chars->encoding == JS::SmallestEncoding::UTF16) {
    return MakeUTF8AtomHelper<char16_t>(cx, chars, length);
  }
  return MakeUTF8AtomHelper<JS::Latin1Char>(cx, chars, length);
}

template <typename Chars>
static MOZ_ALWAYS_INLINE JSAtom* AllocateNewAtom(
    JSContext* cx, Chars chars, size_t length,
    const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup) {
  AutoAllocInAtomsZone ac(cx);

  JSLinearString* linear = MakeLinearStringForAtomization(cx, chars, length);
  if (!linear) {
    // Grudgingly forgo last-ditch GC. The alternative would be to release
    // the lock, manually GC here, and retry from the top.
    ReportOutOfMemory(cx);
    return nullptr;
  }

  JSAtom* atom = linear->morphAtomizedStringIntoAtom(lookup.hash);
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

JSAtom* js::AtomizeString(JSContext* cx, JSString* str,
                          js::PinningBehavior pin /* = js::DoNotPinAtom */) {
  if (str->isAtom()) {
    JSAtom& atom = str->asAtom();
    /* N.B. static atoms are effectively always interned. */
    if (pin == PinAtom && !atom.isPermanentAtom()) {
      cx->runtime()->atoms().maybePinExistingAtom(cx, &atom);
    }

    return &atom;
  }

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }

  if (cx->isMainThreadContext() && pin == DoNotPinAtom) {
    if (JSAtom* atom = cx->caches().stringToAtomCache.lookup(linear)) {
      return atom;
    }
  }

  Maybe<uint32_t> indexValue;
  if (str->hasIndexValue()) {
    indexValue.emplace(str->getIndexValue());
  }

  JS::AutoCheckCannotGC nogc;
  JSAtom* atom = linear->hasLatin1Chars()
                     ? AtomizeAndCopyChars(cx, linear->latin1Chars(nogc),
                                           linear->length(), pin, indexValue)
                     : AtomizeAndCopyChars(cx, linear->twoByteChars(nogc),
                                           linear->length(), pin, indexValue);
  if (!atom) {
    return nullptr;
  }

  if (cx->isMainThreadContext() && pin == DoNotPinAtom) {
    cx->caches().stringToAtomCache.maybePut(linear, atom);
  }

  return atom;
}

bool js::AtomIsPinned(JSContext* cx, JSAtom* atom) {
  JSRuntime* rt = cx->runtime();
  return rt->atoms().atomIsPinned(rt, atom);
}

bool AtomsTable::atomIsPinned(JSRuntime* rt, JSAtom* atom) {
  MOZ_ASSERT(atom);

  if (atom->isPermanentAtom()) {
    return true;
  }

  AtomHasher::Lookup lookup(atom);

  AtomsTable::Partition& part = *partitions[getPartitionIndex(lookup)];
  AtomsTable::AutoLock lock(rt, part.lock);
  AtomSet::Ptr p = part.atoms.lookup(lookup);
  if (!p && part.atomsAddedWhileSweeping) {
    p = part.atomsAddedWhileSweeping->lookup(lookup);
  }

  MOZ_ASSERT(p);  // Non-permanent atoms must exist in atoms table.
  MOZ_ASSERT(p->asPtrUnbarriered() == atom);

  return p->isPinned();
}

void AtomsTable::maybePinExistingAtom(JSContext* cx, JSAtom* atom) {
  MOZ_ASSERT(atom);
  MOZ_ASSERT(!atom->isPermanentAtom());

  AtomHasher::Lookup lookup(atom);

  AtomsTable::Partition& part = *partitions[getPartitionIndex(lookup)];
  AtomsTable::AutoLock lock(cx->runtime(), part.lock);
  AtomSet::Ptr p = part.atoms.lookup(lookup);
  if (!p && part.atomsAddedWhileSweeping) {
    p = part.atomsAddedWhileSweeping->lookup(lookup);
  }

  MOZ_ASSERT(p);  // Non-permanent atoms must exist in atoms table.
  MOZ_ASSERT(p->asPtrUnbarriered() == atom);

  p->setPinned(true);
}

JSAtom* js::Atomize(JSContext* cx, const char* bytes, size_t length,
                    PinningBehavior pin, const Maybe<uint32_t>& indexValue) {
  const Latin1Char* chars = reinterpret_cast<const Latin1Char*>(bytes);
  return AtomizeAndCopyChars(cx, chars, length, pin, indexValue);
}

JSAtom* js::Atomize(JSContext* cx, HashNumber hash, const char* bytes,
                    size_t length, PinningBehavior pin) {
  const Latin1Char* chars = reinterpret_cast<const Latin1Char*>(bytes);
  if (JSAtom* s = cx->staticStrings().lookup(chars, length)) {
    return s;
  }

  AtomHasher::Lookup lookup(hash, chars, length);
  return AtomizeAndCopyCharsFromLookup(cx, chars, length, lookup, pin,
                                       Nothing());
}

template <typename CharT>
JSAtom* js::AtomizeChars(JSContext* cx, const CharT* chars, size_t length,
                         PinningBehavior pin) {
  return AtomizeAndCopyChars(cx, chars, length, pin, Nothing());
}

template JSAtom* js::AtomizeChars(JSContext* cx, const Latin1Char* chars,
                                  size_t length, PinningBehavior pin);

template JSAtom* js::AtomizeChars(JSContext* cx, const char16_t* chars,
                                  size_t length, PinningBehavior pin);

/* |chars| must not point into an inline or short string. */
template <typename CharT>
JSAtom* js::AtomizeChars(JSContext* cx, HashNumber hash, const CharT* chars,
                         size_t length) {
  if (JSAtom* s = cx->staticStrings().lookup(chars, length)) {
    return s;
  }

  AtomHasher::Lookup lookup(hash, chars, length);
  return AtomizeAndCopyCharsFromLookup(
      cx, chars, length, lookup, PinningBehavior::DoNotPinAtom, Nothing());
}

template JSAtom* js::AtomizeChars(JSContext* cx, HashNumber hash,
                                  const Latin1Char* chars, size_t length);

template JSAtom* js::AtomizeChars(JSContext* cx, HashNumber hash,
                                  const char16_t* chars, size_t length);

JSAtom* js::AtomizeUTF8Chars(JSContext* cx, const char* utf8Chars,
                             size_t utf8ByteLength) {
  {
    // Permanent atoms,|JSRuntime::atoms_|, and  static strings are disjoint
    // sets.  |AtomizeAndCopyCharsFromLookup| only consults the first two sets,
    // so we must map any static strings ourselves.  See bug 1575947.
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

  AtomizeUTF8CharsWrapper chars(utf8, forCopy);
  AtomHasher::Lookup lookup(utf8Chars, utf8ByteLength, length, hash);
  return AtomizeAndCopyCharsFromLookup(cx, &chars, length, lookup, DoNotPinAtom,
                                       Nothing());
}

bool js::IndexToIdSlow(JSContext* cx, uint32_t index, MutableHandleId idp) {
  MOZ_ASSERT(index > JSID_INT_MAX);

  char16_t buf[UINT32_CHAR_BUFFER_LENGTH];
  RangedPtr<char16_t> end(std::end(buf), buf, std::end(buf));
  RangedPtr<char16_t> start = BackfillIndexInCharBuffer(index, end);

  JSAtom* atom = AtomizeChars(cx, start.get(), end - start);
  if (!atom) {
    return false;
  }

  idp.set(JS::PropertyKey::fromNonIntAtom(atom));
  return true;
}

template <AllowGC allowGC>
static JSAtom* ToAtomSlow(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType arg) {
  MOZ_ASSERT(!arg.isString());

  Value v = arg;
  if (!v.isPrimitive()) {
    MOZ_ASSERT(!cx->isHelperThreadContext());
    if (!allowGC) {
      return nullptr;
    }
    RootedValue v2(cx, v);
    if (!ToPrimitive(cx, JSTYPE_STRING, &v2)) {
      return nullptr;
    }
    v = v2;
  }

  if (v.isString()) {
    JSAtom* atom = AtomizeString(cx, v.toString());
    if (!allowGC && !atom) {
      cx->recoverFromOutOfMemory();
    }
    return atom;
  }
  if (v.isInt32()) {
    JSAtom* atom = Int32ToAtom(cx, v.toInt32());
    if (!allowGC && !atom) {
      cx->recoverFromOutOfMemory();
    }
    return atom;
  }
  if (v.isDouble()) {
    JSAtom* atom = NumberToAtom(cx, v.toDouble());
    if (!allowGC && !atom) {
      cx->recoverFromOutOfMemory();
    }
    return atom;
  }
  if (v.isBoolean()) {
    return v.toBoolean() ? cx->names().true_ : cx->names().false_;
  }
  if (v.isNull()) {
    return cx->names().null;
  }
  if (v.isSymbol()) {
    MOZ_ASSERT(!cx->isHelperThreadContext());
    if (allowGC) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_SYMBOL_TO_STRING);
    }
    return nullptr;
  }
  if (v.isBigInt()) {
    RootedBigInt i(cx, v.toBigInt());
    JSAtom* atom = BigIntToAtom<allowGC>(cx, i);
    if (!allowGC && !atom) {
      cx->recoverFromOutOfMemory();
    }
    return atom;
  }
  MOZ_ASSERT(v.isUndefined());
  return cx->names().undefined;
}

template <AllowGC allowGC>
JSAtom* js::ToAtom(JSContext* cx,
                   typename MaybeRooted<Value, allowGC>::HandleType v) {
  if (!v.isString()) {
    return ToAtomSlow<allowGC>(cx, v);
  }

  JSString* str = v.toString();
  if (str->isAtom()) {
    return &str->asAtom();
  }

  JSAtom* atom = AtomizeString(cx, str);
  if (!atom && !allowGC) {
    MOZ_ASSERT_IF(!cx->isHelperThreadContext(), cx->isThrowingOutOfMemory());
    cx->recoverFromOutOfMemory();
  }
  return atom;
}

template JSAtom* js::ToAtom<CanGC>(JSContext* cx, HandleValue v);

template JSAtom* js::ToAtom<NoGC>(JSContext* cx, const Value& v);

static JSAtom* AtomizeLittleEndianTwoByteChars(JSContext* cx,
                                               const uint8_t* leTwoByte,
                                               size_t length) {
  LittleEndianChars chars(leTwoByte);

  if (JSAtom* s = cx->staticStrings().lookup(chars, length)) {
    return s;
  }

  AtomHasher::Lookup lookup(chars, length);
  return AtomizeAndCopyCharsFromLookup(cx, chars, length, lookup, DoNotPinAtom,
                                       Nothing());
}

template <XDRMode mode>
XDRResult js::XDRAtomOrNull(XDRState<mode>* xdr, MutableHandleAtom atomp) {
  uint8_t isNull = false;
  if (mode == XDR_ENCODE) {
    if (!atomp) {
      isNull = true;
    }
  }

  MOZ_TRY(xdr->codeUint8(&isNull));

  if (!isNull) {
    MOZ_TRY(XDRAtom(xdr, atomp));
  } else if (mode == XDR_DECODE) {
    atomp.set(nullptr);
  }

  return Ok();
}

template XDRResult js::XDRAtomOrNull(XDRState<XDR_DECODE>* xdr,
                                     MutableHandleAtom atomp);

template XDRResult js::XDRAtomOrNull(XDRState<XDR_ENCODE>* xdr,
                                     MutableHandleAtom atomp);

template <XDRMode mode>
XDRResult js::XDRAtom(XDRState<mode>* xdr, MutableHandleAtom atomp) {
  bool latin1 = false;
  uint32_t length = 0;
  uint32_t lengthAndEncoding = 0;

  if (mode == XDR_ENCODE) {
    JS::AutoCheckCannotGC nogc;
    static_assert(JSString::MAX_LENGTH <= INT32_MAX,
                  "String length must fit in 31 bits");
    latin1 = atomp->hasLatin1Chars();
    length = atomp->length();
    lengthAndEncoding = (length << 1) | uint32_t(latin1);
    MOZ_TRY(xdr->codeUint32(&lengthAndEncoding));
    if (latin1) {
      return xdr->codeChars(
          const_cast<JS::Latin1Char*>(atomp->latin1Chars(nogc)), length);
    }
    return xdr->codeChars(const_cast<char16_t*>(atomp->twoByteChars(nogc)),
                          length);
  }

  MOZ_ASSERT(mode == XDR_DECODE);
  /* Avoid JSString allocation for already existing atoms. See bug 321985. */
  JSContext* cx = xdr->cx();
  JSAtom* atom = nullptr;
  MOZ_TRY(xdr->codeUint32(&lengthAndEncoding));
  length = lengthAndEncoding >> 1;
  latin1 = lengthAndEncoding & 0x1;

  if (latin1) {
    const Latin1Char* chars = nullptr;
    if (length) {
      const uint8_t* ptr;
      size_t nbyte = length * sizeof(Latin1Char);
      MOZ_TRY(xdr->readData(&ptr, nbyte));
      chars = reinterpret_cast<const Latin1Char*>(ptr);
    }
    atom = AtomizeChars(cx, chars, length);
  } else {
    const uint8_t* twoByteCharsLE = nullptr;
    if (length) {
      size_t nbyte = length * sizeof(char16_t);
      MOZ_TRY(xdr->readData(&twoByteCharsLE, nbyte));
    }
    atom = AtomizeLittleEndianTwoByteChars(cx, twoByteCharsLE, length);
  }

  if (!atom) {
    return xdr->fail(JS::TranscodeResult::Throw);
  }
  atomp.set(atom);
  return Ok();
}

template XDRResult js::XDRAtom(XDRState<XDR_DECODE>* xdr,
                               MutableHandleAtom atomp);

template XDRResult js::XDRAtom(XDRState<XDR_ENCODE>* xdr,
                               MutableHandleAtom atomp);

Handle<PropertyName*> js::ClassName(JSProtoKey key, JSContext* cx) {
  return ClassName(key, cx->names());
}

js::AutoLockAllAtoms::AutoLockAllAtoms(JSRuntime* rt) : runtime(rt) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime));
  if (runtime->hasHelperThreadZones()) {
    runtime->atoms().lockAll();
  }
}

js::AutoLockAllAtoms::~AutoLockAllAtoms() {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime));
  if (runtime->hasHelperThreadZones()) {
    runtime->atoms().unlockAll();
  }
}
