/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implementation of nursery eviction (tenuring).
 */

#include "gc/Tenuring.h"

#include "mozilla/PodOperations.h"

#include "gc/Cell.h"
#include "gc/GCInternals.h"
#include "gc/GCProbes.h"
#include "gc/Pretenuring.h"
#include "gc/Zone.h"
#include "jit/JitCode.h"
#include "js/TypeDecls.h"
#include "proxy/Proxy.h"
#include "vm/BigIntType.h"
#include "vm/JSScript.h"
#include "vm/NativeObject.h"
#include "vm/Runtime.h"
#include "vm/TypedArrayObject.h"

#include "gc/Heap-inl.h"
#include "gc/Marking-inl.h"
#include "gc/ObjectKind-inl.h"
#include "gc/StoreBuffer-inl.h"
#include "gc/TraceMethods-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/PlainObject-inl.h"
#include "vm/StringType-inl.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/TupleType.h"
#endif

using namespace js;
using namespace js::gc;

using mozilla::PodCopy;

constexpr size_t MAX_DEDUPLICATABLE_STRING_LENGTH = 500;

TenuringTracer::TenuringTracer(JSRuntime* rt, Nursery* nursery,
                               bool tenureEverything)
    : JSTracer(rt, JS::TracerKind::Tenuring,
               JS::WeakMapTraceAction::TraceKeysAndValues),
      nursery_(*nursery),
      tenureEverything(tenureEverything) {
  stringDeDupSet.emplace();
}

size_t TenuringTracer::getPromotedSize() const {
  return promotedSize + promotedCells * sizeof(NurseryCellHeader);
}

size_t TenuringTracer::getPromotedCells() const { return promotedCells; }

void TenuringTracer::onObjectEdge(JSObject** objp, const char* name) {
  JSObject* obj = *objp;
  if (!nursery_.inCollectedRegion(obj)) {
    MOZ_ASSERT(!obj->isForwarded());
    return;
  }

  *objp = promoteOrForward(obj);
  MOZ_ASSERT(!(*objp)->isForwarded());
}

JSObject* TenuringTracer::promoteOrForward(JSObject* obj) {
  MOZ_ASSERT(nursery_.inCollectedRegion(obj));

  if (obj->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(obj);
    obj = static_cast<JSObject*>(overlay->forwardingAddress());
    if (IsInsideNursery(obj)) {
      promotedToNursery = true;
    }
    return obj;
  }

  return onNonForwardedNurseryObject(obj);
}

JSObject* TenuringTracer::onNonForwardedNurseryObject(JSObject* obj) {
  MOZ_ASSERT(IsInsideNursery(obj));
  MOZ_ASSERT(!obj->isForwarded());

  // Take a fast path for promoting a plain object as this is by far the most
  // common case.
  if (obj->is<PlainObject>()) {
    return promotePlainObject(&obj->as<PlainObject>());
  }

  return promoteObjectSlow(obj);
}

void TenuringTracer::onStringEdge(JSString** strp, const char* name) {
  JSString* str = *strp;
  if (!nursery_.inCollectedRegion(str)) {
    return;
  }

  *strp = promoteOrForward(str);
}

JSString* TenuringTracer::promoteOrForward(JSString* str) {
  MOZ_ASSERT(nursery_.inCollectedRegion(str));

  if (str->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(str);
    str = static_cast<JSString*>(overlay->forwardingAddress());
    if (IsInsideNursery(str)) {
      promotedToNursery = true;
    }
    return str;
  }

  return onNonForwardedNurseryString(str);
}

JSString* TenuringTracer::onNonForwardedNurseryString(JSString* str) {
  MOZ_ASSERT(IsInsideNursery(str));
  MOZ_ASSERT(!str->isForwarded());

  return promoteString(str);
}

void TenuringTracer::onBigIntEdge(JS::BigInt** bip, const char* name) {
  JS::BigInt* bi = *bip;
  if (!nursery_.inCollectedRegion(bi)) {
    return;
  }

  *bip = promoteOrForward(bi);
}

JS::BigInt* TenuringTracer::promoteOrForward(JS::BigInt* bi) {
  MOZ_ASSERT(nursery_.inCollectedRegion(bi));

  if (bi->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(bi);
    bi = static_cast<JS::BigInt*>(overlay->forwardingAddress());
    if (IsInsideNursery(bi)) {
      promotedToNursery = true;
    }
    return bi;
  }

  return onNonForwardedNurseryBigInt(bi);
}

JS::BigInt* TenuringTracer::onNonForwardedNurseryBigInt(JS::BigInt* bi) {
  MOZ_ASSERT(IsInsideNursery(bi));
  MOZ_ASSERT(!bi->isForwarded());

  return promoteBigInt(bi);
}

void TenuringTracer::onSymbolEdge(JS::Symbol** symp, const char* name) {}
void TenuringTracer::onScriptEdge(BaseScript** scriptp, const char* name) {}
void TenuringTracer::onShapeEdge(Shape** shapep, const char* name) {}
void TenuringTracer::onRegExpSharedEdge(RegExpShared** sharedp,
                                        const char* name) {}
void TenuringTracer::onBaseShapeEdge(BaseShape** basep, const char* name) {}
void TenuringTracer::onGetterSetterEdge(GetterSetter** gsp, const char* name) {}
void TenuringTracer::onPropMapEdge(PropMap** mapp, const char* name) {}
void TenuringTracer::onJitCodeEdge(jit::JitCode** codep, const char* name) {}
void TenuringTracer::onScopeEdge(Scope** scopep, const char* name) {}

void TenuringTracer::traverse(JS::Value* thingp) {
  MOZ_ASSERT(!nursery().inCollectedRegion(thingp));

  Value value = *thingp;
  CheckTracedThing(this, value);

  if (!value.isGCThing()) {
    return;
  }

  Cell* cell = value.toGCThing();
  if (!nursery_.inCollectedRegion(cell)) {
    return;
  }

  if (cell->isForwarded()) {
    const gc::RelocationOverlay* overlay =
        gc::RelocationOverlay::fromCell(cell);
    Cell* target = overlay->forwardingAddress();
    thingp->changeGCThingPayload(target);
    if (IsInsideNursery(target)) {
      promotedToNursery = true;
    }
    return;
  }

  // We only care about a few kinds of GC thing here and this generates much
  // tighter code than using MapGCThingTyped.
  if (value.isObject()) {
    JSObject* obj = onNonForwardedNurseryObject(&value.toObject());
    MOZ_ASSERT(obj != &value.toObject());
    *thingp = JS::ObjectValue(*obj);
    return;
  }
#ifdef ENABLE_RECORD_TUPLE
  if (value.isExtendedPrimitive()) {
    JSObject* obj = onNonForwardedNurseryObject(&value.toExtendedPrimitive());
    MOZ_ASSERT(obj != &value.toExtendedPrimitive());
    *thingp = JS::ExtendedPrimitiveValue(*obj);
    return;
  }
#endif
  if (value.isString()) {
    JSString* str = onNonForwardedNurseryString(value.toString());
    MOZ_ASSERT(str != value.toString());
    *thingp = JS::StringValue(str);
    return;
  }
  MOZ_ASSERT(value.isBigInt());
  JS::BigInt* bi = onNonForwardedNurseryBigInt(value.toBigInt());
  MOZ_ASSERT(bi != value.toBigInt());
  *thingp = JS::BigIntValue(bi);
}

void TenuringTracer::traverse(wasm::AnyRef* thingp) {
  MOZ_ASSERT(!nursery().inCollectedRegion(thingp));

  wasm::AnyRef value = *thingp;
  CheckTracedThing(this, value);

  Cell* cell = value.toGCThing();
  if (!nursery_.inCollectedRegion(cell)) {
    return;
  }

  wasm::AnyRef post = wasm::AnyRef::invalid();
  switch (value.kind()) {
    case wasm::AnyRefKind::Object: {
      JSObject* obj = promoteOrForward(&value.toJSObject());
      MOZ_ASSERT(obj != &value.toJSObject());
      post = wasm::AnyRef::fromJSObject(*obj);
      break;
    }
    case wasm::AnyRefKind::String: {
      JSString* str = promoteOrForward(value.toJSString());
      MOZ_ASSERT(str != value.toJSString());
      post = wasm::AnyRef::fromJSString(str);
      break;
    }
    case wasm::AnyRefKind::I31:
    case wasm::AnyRefKind::Null: {
      // This function must only be called for GC things.
      MOZ_CRASH();
    }
  }

  *thingp = post;
}

class MOZ_RAII TenuringTracer::AutoPromotedAnyToNursery {
 public:
  explicit AutoPromotedAnyToNursery(TenuringTracer& trc) : trc_(trc) {
    trc.promotedToNursery = false;
  }
  explicit operator bool() const { return trc_.promotedToNursery; }

 private:
  TenuringTracer& trc_;
};

template <typename T>
void js::gc::StoreBuffer::MonoTypeBuffer<T>::trace(TenuringTracer& mover,
                                                   StoreBuffer* owner) {
  mozilla::ReentrancyGuard g(*owner);
  MOZ_ASSERT(owner->isEnabled());

  if (last_) {
    last_.trace(mover);
  }

  for (typename StoreSet::Range r = stores_.all(); !r.empty(); r.popFront()) {
    r.front().trace(mover);
  }
}

namespace js::gc {
template void StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>::trace(
    TenuringTracer&, StoreBuffer* owner);
template void StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>::trace(
    TenuringTracer&, StoreBuffer* owner);
template void StoreBuffer::MonoTypeBuffer<StoreBuffer::WasmAnyRefEdge>::trace(
    TenuringTracer&, StoreBuffer* owner);
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::StringPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::BigIntPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::ObjectPtrEdge>;
}  // namespace js::gc

void js::gc::StoreBuffer::SlotsEdge::trace(TenuringTracer& mover) const {
  NativeObject* obj = object();
  MOZ_ASSERT(IsCellPointerValid(obj));

  // Beware JSObject::swap exchanging a native object for a non-native one.
  if (!obj->is<NativeObject>()) {
    return;
  }

  MOZ_ASSERT(!IsInsideNursery(obj), "obj shouldn't live in nursery.");

  TenuringTracer::AutoPromotedAnyToNursery promotedToNursery(mover);

  if (kind() == ElementKind) {
    uint32_t initLen = obj->getDenseInitializedLength();
    uint32_t numShifted = obj->getElementsHeader()->numShiftedElements();
    uint32_t clampedStart = start_;
    clampedStart = numShifted < clampedStart ? clampedStart - numShifted : 0;
    clampedStart = std::min(clampedStart, initLen);
    uint32_t clampedEnd = start_ + count_;
    clampedEnd = numShifted < clampedEnd ? clampedEnd - numShifted : 0;
    clampedEnd = std::min(clampedEnd, initLen);
    MOZ_ASSERT(clampedStart <= clampedEnd);
    auto* slotStart =
        static_cast<HeapSlot*>(obj->getDenseElements() + clampedStart);
    uint32_t nslots = clampedEnd - clampedStart;
    mover.traceObjectElements(slotStart->unbarrieredAddress(), nslots);
  } else {
    uint32_t start = std::min(start_, obj->slotSpan());
    uint32_t end = std::min(start_ + count_, obj->slotSpan());
    MOZ_ASSERT(start <= end);
    mover.traceObjectSlots(obj, start, end);
  }

  if (promotedToNursery) {
    mover.runtime()->gc.storeBuffer().putSlot(obj, kind(), start_, count_);
  }
}

static inline void TraceWholeCell(TenuringTracer& mover, JSObject* object) {
  mover.traceObject(object);
}

// Return whether the string needs to be swept.
//
// We can break down the relevant dependency chains as follows:
//
//  T -> T2 : will not be swept, but safe because T2.chars is fixed.
//  T -> N1 -> ... -> T2 : safe because T2.chars is fixed
//  T -> N1 -> ... -> N2 : update T.chars += tenured(N2).chars - N2.chars
//
// Collapse the base chain down to simply T -> T2 or T -> N2. The pointer update
// will happen during sweeping.
//
// Note that in cases like T -> N1 -> T2 -> T3 -> N2, both T -> N1 and T3 -> N2
// will be processed by the whole cell buffer (or rather, only T and T3 will
// be in the store buffer). The order that these strings are
// visited does not matter because the nursery bases are left alone until
// sweeping.
static inline bool TraceWholeCell(TenuringTracer& mover, JSString* str) {
  if (str->hasBase()) {
    // For tenured dependent strings -> nursery string edges, sweep the
    // (tenured) strings at the end of nursery marking to update chars pointers
    // that were in the nursery. Rather than updating the base pointer to point
    // directly to the tenured version of itself, we will leave it pointing at
    // the nursery Cell (which will become a StringRelocationOverlay during the
    // minor GC.)
    JSLinearString* base = str->nurseryBaseOrRelocOverlay();
    if (IsInsideNursery(base)) {
      str->traceBaseFromStoreBuffer(&mover);
      return IsInsideNursery(str->nurseryBaseOrRelocOverlay());
    }
  }

  str->traceChildren(&mover);

  return false;
}

static inline void TraceWholeCell(TenuringTracer& mover, BaseScript* script) {
  script->traceChildren(&mover);
}

static inline void TraceWholeCell(TenuringTracer& mover,
                                  jit::JitCode* jitcode) {
  jitcode->traceChildren(&mover);
}

template <typename T>
bool TenuringTracer::traceBufferedCells(Arena* arena, ArenaCellSet* cells) {
  for (size_t i = 0; i < MaxArenaCellIndex; i += cells->BitsPerWord) {
    ArenaCellSet::WordT bitset = cells->getWord(i / cells->BitsPerWord);
    while (bitset) {
      size_t bit = i + js::detail::CountTrailingZeroes(bitset);
      bitset &= bitset - 1;  // Clear the low bit.

      auto cell =
          reinterpret_cast<T*>(uintptr_t(arena) + ArenaCellIndexBytes * bit);

      TenuringTracer::AutoPromotedAnyToNursery promotedToNursery(*this);

      TraceWholeCell(*this, cell);

      if (promotedToNursery) {
        runtime()->gc.storeBuffer().putWholeCell(cell);
      }
    }
  }

  return false;
}

template <>
bool TenuringTracer::traceBufferedCells<JSString>(Arena* arena,
                                                  ArenaCellSet* cells) {
  bool needsSweep = false;
  for (size_t i = 0; i < MaxArenaCellIndex; i += cells->BitsPerWord) {
    ArenaCellSet::WordT bitset = cells->getWord(i / cells->BitsPerWord);
    ArenaCellSet::WordT tosweep = bitset;
    while (bitset) {
      size_t bit = i + js::detail::CountTrailingZeroes(bitset);
      auto* cell = reinterpret_cast<JSString*>(uintptr_t(arena) +
                                               ArenaCellIndexBytes * bit);
      TenuringTracer::AutoPromotedAnyToNursery promotedToNursery(*this);
      bool needsSweep = TraceWholeCell(*this, cell);
      if (promotedToNursery) {
        runtime()->gc.storeBuffer().putWholeCell(cell);
      }
      ArenaCellSet::WordT mask = bitset - 1;
      bitset &= mask;
      if (!needsSweep) {
        tosweep &= mask;
      }
    }

    cells->setWord(i / cells->BitsPerWord, tosweep);
    if (tosweep) {
      needsSweep = true;
    }
  }

  return needsSweep;
}

ArenaCellSet* ArenaCellSet::trace(TenuringTracer& mover) {
  ArenaCellSet* head = nullptr;

  ArenaCellSet* cells = this;
  while (cells) {
    cells->check();

    Arena* arena = cells->arena;
    arena->bufferedCells() = &ArenaCellSet::Empty;

    JS::TraceKind kind = MapAllocToTraceKind(arena->getAllocKind());
    bool needsSweep;
    switch (kind) {
      case JS::TraceKind::Object:
        needsSweep = mover.traceBufferedCells<JSObject>(arena, cells);
        break;
      case JS::TraceKind::String:
        needsSweep = mover.traceBufferedCells<JSString>(arena, cells);
        break;
      case JS::TraceKind::Script:
        needsSweep = mover.traceBufferedCells<BaseScript>(arena, cells);
        break;
      case JS::TraceKind::JitCode:
        needsSweep = mover.traceBufferedCells<jit::JitCode>(arena, cells);
        break;
      default:
        MOZ_CRASH("Unexpected trace kind");
    }

    ArenaCellSet* next = cells->next;
    if (needsSweep) {
      cells->next = head;
      head = cells;
    }

    cells = next;
  }

  return head;
}

void js::gc::StoreBuffer::WholeCellBuffer::trace(TenuringTracer& mover,
                                                 StoreBuffer* owner) {
  MOZ_ASSERT(owner->isEnabled());

  if (head_) {
    head_ = head_->trace(mover);
  }
}

// Sweep a tenured dependent string with a nursery base. The base chain will
// have been collapsed to a single link before this string was added to the
// sweep set, so only the simple case of a tenured dependent string with a
// nursery base needs to be considered.
template <typename CharT>
void JSDependentString::sweepTypedAfterMinorGC() {
  MOZ_ASSERT(isTenured());
  MOZ_ASSERT(IsInsideNursery(nurseryBaseOrRelocOverlay()));

  JSLinearString* base = nurseryBaseOrRelocOverlay();
  MOZ_ASSERT(IsInsideNursery(base));
  MOZ_ASSERT(!Forwarded(base)->hasBase(), "base chain should be collapsed");
  MOZ_ASSERT(base->isForwarded(), "root base should be kept alive");
  auto* baseOverlay = js::gc::StringRelocationOverlay::fromCell(base);
  const CharT* oldBaseChars = baseOverlay->savedNurseryChars<CharT>();

  // We have the base's original chars pointer and its current chars pointer.
  // Update our chars pointer, which is an offset from the original base
  // chars, and make it point to the same offset within the root's chars.
  // (Most of the time, the base chars didn't move and so this has no
  // effect.)
  const CharT* oldChars = JSString::nonInlineCharsRaw<CharT>();
  size_t offset = oldChars - oldBaseChars;
  JSLinearString* tenuredBase = Forwarded(base);
  MOZ_ASSERT(offset < tenuredBase->length());

  const CharT* newBaseChars = tenuredBase->JSString::nonInlineCharsRaw<CharT>();
  relocateNonInlineChars(newBaseChars, offset);
  MOZ_ASSERT(tenuredBase->assertIsValidBase());
  d.s.u3.base = tenuredBase;
}

inline void JSDependentString::sweepAfterMinorGC() {
  if (hasTwoByteChars()) {
    sweepTypedAfterMinorGC<char16_t>();
  } else {
    sweepTypedAfterMinorGC<JS::Latin1Char>();
  }
}

static void SweepDependentStrings(Arena* arena, ArenaCellSet* cells) {
  for (size_t i = 0; i < MaxArenaCellIndex; i += cells->BitsPerWord) {
    ArenaCellSet::WordT bitset = cells->getWord(i / cells->BitsPerWord);
    while (bitset) {
      size_t bit = i + js::detail::CountTrailingZeroes(bitset);
      auto* str = reinterpret_cast<JSString*>(uintptr_t(arena) +
                                              ArenaCellIndexBytes * bit);
      MOZ_ASSERT(str->isTenured());
      str->asDependent().sweepAfterMinorGC();
      bitset &= bitset - 1;  // Clear the low bit.
    }
  }
}

void ArenaCellSet::sweepDependentStrings() {
  for (ArenaCellSet* cells = this; cells; cells = cells->next) {
    Arena* arena = cells->arena;
    arena->bufferedCells() = &ArenaCellSet::Empty;
    MOZ_ASSERT(MapAllocToTraceKind(arena->getAllocKind()) ==
               JS::TraceKind::String);
    SweepDependentStrings(arena, cells);
  }
}

template <typename T>
void js::gc::StoreBuffer::CellPtrEdge<T>::trace(TenuringTracer& mover) const {
  static_assert(std::is_base_of_v<Cell, T>, "T must be a Cell type");
  static_assert(!GCTypeIsTenured<T>(), "T must not be a tenured Cell type");

  T* thing = *edge;
  if (!thing) {
    return;
  }

  MOZ_ASSERT(IsCellPointerValid(thing));
  MOZ_ASSERT(thing->getTraceKind() == JS::MapTypeToTraceKind<T>::kind);

  if (std::is_same_v<JSString, T>) {
    // Nursery string deduplication requires all tenured string -> nursery
    // string edges to be registered with the whole cell buffer in order to
    // correctly set the non-deduplicatable bit.
    MOZ_ASSERT(!mover.runtime()->gc.isPointerWithinTenuredCell(
        edge, JS::TraceKind::String));
  }

  if (!mover.nursery().inCollectedRegion(thing)) {
    return;
  }

  *edge = mover.promoteOrForward(thing);

  if (IsInsideNursery(*edge)) {
    mover.runtime()->gc.storeBuffer().putCell(edge);
  }
}

void js::gc::StoreBuffer::ValueEdge::trace(TenuringTracer& mover) const {
  if (!isGCThing()) {
    return;
  }

  TenuringTracer::AutoPromotedAnyToNursery promotedToNursery(mover);

  mover.traverse(edge);

  if (promotedToNursery) {
    mover.runtime()->gc.storeBuffer().putValue(edge);
  }
}

void js::gc::StoreBuffer::WasmAnyRefEdge::trace(TenuringTracer& mover) const {
  if (!isGCThing()) {
    return;
  }

  TenuringTracer::AutoPromotedAnyToNursery promotedToNursery(mover);

  mover.traverse(edge);

  if (promotedToNursery) {
    mover.runtime()->gc.storeBuffer().putWasmAnyRef(edge);
  }
}

// Visit all object children of the object and trace them.
void js::gc::TenuringTracer::traceObject(JSObject* obj) {
  const JSClass* clasp = obj->getClass();
  MOZ_ASSERT(clasp);

  if (clasp->hasTrace()) {
    clasp->doTrace(this, obj);
  }

  if (!obj->is<NativeObject>()) {
    return;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->hasEmptyElements()) {
    HeapSlotArray elements = nobj->getDenseElements();
    Value* elems = elements.begin()->unbarrieredAddress();
    traceObjectElements(elems, nobj->getDenseInitializedLength());
  }

  traceObjectSlots(nobj, 0, nobj->slotSpan());
}

void js::gc::TenuringTracer::traceObjectSlots(NativeObject* nobj,
                                              uint32_t start, uint32_t end) {
  auto traceRange = [this](HeapSlot* slotStart, HeapSlot* slotEnd) {
    traceSlots(slotStart->unbarrieredAddress(), slotEnd->unbarrieredAddress());
  };
  nobj->forEachSlotRange(start, end, traceRange);
}

void js::gc::TenuringTracer::traceObjectElements(JS::Value* vp,
                                                 uint32_t count) {
  traceSlots(vp, vp + count);
}

void js::gc::TenuringTracer::traceSlots(Value* vp, Value* end) {
  for (; vp != end; ++vp) {
    traverse(vp);
  }
}

void js::gc::TenuringTracer::traceString(JSString* str) {
  str->traceChildren(this);
}

void js::gc::TenuringTracer::traceBigInt(JS::BigInt* bi) {
  bi->traceChildren(this);
}

#ifdef DEBUG
static inline uintptr_t OffsetFromChunkStart(void* p) {
  return uintptr_t(p) & gc::ChunkMask;
}
static inline size_t OffsetToChunkEnd(void* p) {
  uintptr_t offsetFromStart = OffsetFromChunkStart(p);
  MOZ_ASSERT(offsetFromStart < ChunkSize);
  return ChunkSize - offsetFromStart;
}
#endif

/* Insert the given relocation entry into the list of things to visit. */
inline void js::gc::TenuringTracer::insertIntoObjectFixupList(
    RelocationOverlay* entry) {
  entry->setNext(objHead);
  objHead = entry;
}

template <typename T>
inline T* js::gc::TenuringTracer::alloc(Zone* zone, AllocKind kind, Cell* src) {
  AllocSite* site = NurseryCellHeader::from(src)->allocSite();
  site->incPromotedCount();

  void* ptr = allocCell<T::TraceKind>(zone, kind, site, src);
  auto* cell = reinterpret_cast<T*>(ptr);
  if (IsInsideNursery(cell)) {
    MOZ_ASSERT(!nursery().inCollectedRegion(cell));
    promotedToNursery = true;
  }

  return cell;
}

template <JS::TraceKind traceKind>
void* js::gc::TenuringTracer::allocCell(Zone* zone, AllocKind allocKind,
                                        AllocSite* site, Cell* src) {
  MOZ_ASSERT(zone == src->zone());

  if (!shouldTenure(zone, traceKind, src)) {
    // Allocations from the optimized alloc site continue to use that site,
    // otherwise a special promoted alloc site it used.
    if (site->kind() != AllocSite::Kind::Optimized) {
      site = &zone->pretenuring.promotedAllocSite(traceKind);
    }

    size_t thingSize = Arena::thingSize(allocKind);
    void* ptr = nursery_.tryAllocateCell(site, thingSize, traceKind);
    if (MOZ_LIKELY(ptr)) {
      return ptr;
    }

    JSContext* cx = runtime()->mainContextFromOwnThread();
    ptr = CellAllocator::RetryNurseryAlloc<NoGC>(cx, traceKind, allocKind,
                                                 thingSize, site);
    if (MOZ_LIKELY(ptr)) {
      return ptr;
    }

    // The nursery is full. This is unlikely but can happen. Fall through to
    // the tenured allocation path.
  }

  return AllocateTenuredCellInGC(zone, allocKind);
}

JSString* js::gc::TenuringTracer::allocString(JSString* src, Zone* zone,
                                              AllocKind dstKind) {
  JSString* dst = alloc<JSString>(zone, dstKind, src);
  promotedSize += moveString(dst, src, dstKind);
  promotedCells++;

  return dst;
}

bool js::gc::TenuringTracer::shouldTenure(Zone* zone, JS::TraceKind traceKind,
                                          Cell* cell) {
  return tenureEverything || !zone->allocKindInNursery(traceKind) ||
         nursery_.shouldTenure(cell);
}

JSObject* js::gc::TenuringTracer::promoteObjectSlow(JSObject* src) {
  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->is<PlainObject>());

  AllocKind dstKind = src->allocKindForTenure(nursery());
  auto* dst = alloc<JSObject>(src->nurseryZone(), dstKind, src);

  size_t srcSize = Arena::thingSize(dstKind);

  // Arrays and Tuples do not necessarily have the same AllocKind between src
  // and dst. We deal with this by copying elements manually, possibly
  // re-inlining them if there is adequate room inline in dst.
  //
  // For Arrays and Tuples we're reducing promotedSize to the smaller srcSize
  // because moveElements() accounts for all Array or Tuple elements,
  // even if they are inlined.
  if (src->is<FixedLengthTypedArrayObject>()) {
    auto* tarray = &src->as<FixedLengthTypedArrayObject>();
    // Typed arrays with inline data do not necessarily have the same
    // AllocKind between src and dst. The nursery does not allocate an
    // inline data buffer that has the same size as the slow path will do.
    // In the slow path, the Typed Array Object stores the inline data
    // in the allocated space that fits the AllocKind. In the fast path,
    // the nursery will allocate another buffer that is directly behind the
    // minimal JSObject. That buffer size plus the JSObject size is not
    // necessarily as large as the slow path's AllocKind size.
    if (tarray->hasInlineElements()) {
      AllocKind srcKind =
          GetGCObjectKind(FixedLengthTypedArrayObject::FIXED_DATA_START);
      size_t headerSize = Arena::thingSize(srcKind);
      srcSize = headerSize + tarray->byteLength();
    }
  } else if (src->canHaveFixedElements()) {
    srcSize = sizeof(NativeObject);
  }

  promotedSize += srcSize;
  promotedCells++;

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetFromChunkStart(src) >= sizeof(ChunkBase));
  MOZ_ASSERT(OffsetToChunkEnd(src) >= srcSize);
  js_memcpy(dst, src, srcSize);

  // Move the slots and elements, if we need to.
  if (src->is<NativeObject>()) {
    NativeObject* ndst = &dst->as<NativeObject>();
    NativeObject* nsrc = &src->as<NativeObject>();
    promotedSize += moveSlots(ndst, nsrc);
    promotedSize += moveElements(ndst, nsrc, dstKind);
  }

  JSObjectMovedOp op = dst->getClass()->extObjectMovedOp();
  MOZ_ASSERT_IF(src->is<ProxyObject>(), op == proxy_ObjectMoved);
  if (op) {
    // Tell the hazard analysis that the object moved hook can't GC.
    JS::AutoSuppressGCAnalysis nogc;
    promotedSize += op(dst, src);
  } else {
    MOZ_ASSERT_IF(src->getClass()->hasFinalize(),
                  CanNurseryAllocateFinalizedClass(src->getClass()));
  }

  RelocationOverlay* overlay = RelocationOverlay::forwardCell(src, dst);
  insertIntoObjectFixupList(overlay);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

inline JSObject* js::gc::TenuringTracer::promotePlainObject(PlainObject* src) {
  // Fast path version of promoteObjectSlow() for specialized for PlainObject.

  MOZ_ASSERT(IsInsideNursery(src));

  AllocKind dstKind = src->allocKindForTenure();
  auto* dst = alloc<PlainObject>(src->nurseryZone(), dstKind, src);

  size_t srcSize = Arena::thingSize(dstKind);
  promotedSize += srcSize;
  promotedCells++;

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetFromChunkStart(src) >= sizeof(ChunkBase));
  MOZ_ASSERT(OffsetToChunkEnd(src) >= srcSize);
  js_memcpy(dst, src, srcSize);

  // Move the slots and elements.
  promotedSize += moveSlots(dst, src);
  promotedSize += moveElements(dst, src, dstKind);

  MOZ_ASSERT(!dst->getClass()->extObjectMovedOp());

  RelocationOverlay* overlay = RelocationOverlay::forwardCell(src, dst);
  insertIntoObjectFixupList(overlay);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

size_t js::gc::TenuringTracer::moveSlots(NativeObject* dst, NativeObject* src) {
  /* Fixed slots have already been copied over. */
  if (!src->hasDynamicSlots()) {
    return 0;
  }

  size_t count = src->numDynamicSlots();
  size_t allocSize = ObjectSlots::allocSize(count);

  ObjectSlots* header = src->getSlotsHeader();
  Nursery::WasBufferMoved result = nursery().maybeMoveBufferOnPromotion(
      &header, dst, allocSize, MemoryUse::ObjectSlots);
  if (result == Nursery::BufferNotMoved) {
    return 0;
  }

  dst->slots_ = header->slots();
  if (count) {
    nursery().setSlotsForwardingPointer(src->slots_, dst->slots_, count);
  }
  return allocSize;
}

size_t js::gc::TenuringTracer::moveElements(NativeObject* dst,
                                            NativeObject* src,
                                            AllocKind dstKind) {
  if (src->hasEmptyElements()) {
    return 0;
  }

  ObjectElements* srcHeader = src->getElementsHeader();
  size_t nslots = srcHeader->numAllocatedElements();
  size_t allocSize = nslots * sizeof(HeapSlot);

  // Shifted elements are copied too.
  uint32_t numShifted = srcHeader->numShiftedElements();

  void* unshiftedHeader = src->getUnshiftedElementsHeader();

  /* Unlike other objects, Arrays and Tuples can have fixed elements. */
  if (src->canHaveFixedElements() && nslots <= GetGCKindSlots(dstKind)) {
    dst->as<NativeObject>().setFixedElements();
    js_memcpy(dst->getElementsHeader(), unshiftedHeader, allocSize);
    dst->elements_ += numShifted;
    dst->getElementsHeader()->flags |= ObjectElements::FIXED;
    nursery().setElementsForwardingPointer(srcHeader, dst->getElementsHeader(),
                                           srcHeader->capacity);
    return allocSize;
  }

  /* TODO Bug 874151: Prefer to put element data inline if we have space. */

  Nursery::WasBufferMoved result = nursery().maybeMoveBufferOnPromotion(
      &unshiftedHeader, dst, allocSize, MemoryUse::ObjectElements);
  if (result == Nursery::BufferNotMoved) {
    return 0;
  }

  dst->elements_ =
      static_cast<ObjectElements*>(unshiftedHeader)->elements() + numShifted;
  dst->getElementsHeader()->flags &= ~ObjectElements::FIXED;
  nursery().setElementsForwardingPointer(srcHeader, dst->getElementsHeader(),
                                         srcHeader->capacity);
  return allocSize;
}

inline void js::gc::TenuringTracer::insertIntoStringFixupList(
    StringRelocationOverlay* entry) {
  entry->setNext(stringHead);
  stringHead = entry;
}

JSString* js::gc::TenuringTracer::promoteString(JSString* src) {
  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->isExternal());

  AllocKind dstKind = src->getAllocKind();
  Zone* zone = src->nurseryZone();

  // If this string is in the StringToAtomCache, try to deduplicate it by using
  // the atom. Don't do this for dependent strings because they're more
  // complicated. See StringRelocationOverlay and DeduplicationStringHasher
  // comments.
  MOZ_ASSERT(!src->isAtom());
  if (src->isLinear() && src->inStringToAtomCache() &&
      src->isDeduplicatable() && !src->hasBase()) {
    JSLinearString* linear = &src->asLinear();
    JSAtom* atom = runtime()->caches().stringToAtomCache.lookupInMap(linear);
    // The string will not be present in the cache if it was previously promoted
    // to the second nursery generation.
    if (atom) {
      // Only deduplicate if both strings have the same encoding, to not confuse
      // dependent strings.
      if (src->hasTwoByteChars() == atom->hasTwoByteChars()) {
        // The StringToAtomCache isn't used for inline strings (due to the
        // minimum length) so canOwnDependentChars must be true for both src and
        // atom. This means if there are dependent strings floating around using
        // str's chars, they will be able to use the chars from the atom.
        static_assert(StringToAtomCache::MinStringLength >
                      JSFatInlineString::MAX_LENGTH_LATIN1);
        static_assert(StringToAtomCache::MinStringLength >
                      JSFatInlineString::MAX_LENGTH_TWO_BYTE);
        MOZ_ASSERT(src->canOwnDependentChars());
        MOZ_ASSERT(atom->canOwnDependentChars());

        StringRelocationOverlay::forwardCell(src, atom);
        gcprobes::PromoteToTenured(src, atom);
        return atom;
      }
    }
  }

  JSString* dst;

  // A live nursery string can only get deduplicated when:
  // 1. Its length is smaller than MAX_DEDUPLICATABLE_STRING_LENGTH:
  //    Hashing a long string can affect performance.
  // 2. It is linear:
  //    Deduplicating every node in it would end up doing O(n^2) hashing work.
  // 3. It is deduplicatable:
  //    The JSString NON_DEDUP_BIT flag is unset.
  // 4. It matches an entry in stringDeDupSet.
  // 5. It is moved to the tenured heap.

  if (shouldTenure(zone, JS::TraceKind::String, src) &&
      src->length() < MAX_DEDUPLICATABLE_STRING_LENGTH && src->isLinear() &&
      src->isDeduplicatable() && stringDeDupSet.isSome()) {
    src->clearBitsOnTenure();
    auto p = stringDeDupSet->lookupForAdd(src);
    if (p) {
      // Deduplicate to the looked-up string!
      dst = *p;
      zone->stringStats.ref().noteDeduplicated(src->length(), src->allocSize());
      StringRelocationOverlay::forwardCell(src, dst);
      gcprobes::PromoteToTenured(src, dst);
      return dst;
    }

    dst = allocString(src, zone, dstKind);

    using DedupHasher [[maybe_unused]] = DeduplicationStringHasher<JSString*>;
    MOZ_ASSERT(DedupHasher::hash(src) == DedupHasher::hash(dst),
               "src and dst must have the same hash for lookupForAdd");

    if (!stringDeDupSet->add(p, dst)) {
      // When there is oom caused by the stringDeDupSet, stop deduplicating
      // strings.
      stringDeDupSet.reset();
    }
  } else {
    dst = allocString(src, zone, dstKind);
    if (dst->isTenured()) {
      src->clearBitsOnTenure();
      dst->clearBitsOnTenure();
    }
  }

  zone->stringStats.ref().noteTenured(src->allocSize());

  auto* overlay = StringRelocationOverlay::forwardCell(src, dst);
  MOZ_ASSERT_IF(dst->isTenured() && dst->isLinear(), dst->isDeduplicatable());

  if (dst->hasBase() || dst->isRope()) {
    // dst or one of its leaves might have a base that will be deduplicated.
    // Insert the overlay into the fixup list to relocate it later.
    insertIntoStringFixupList(overlay);
  }

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

template <typename CharT>
void js::gc::TenuringTracer::relocateDependentStringChars(
    JSDependentString* tenuredDependentStr, JSLinearString* baseOrRelocOverlay,
    size_t* offset, bool* rootBaseNotYetForwarded, JSLinearString** rootBase) {
  MOZ_ASSERT(*offset == 0);
  MOZ_ASSERT(*rootBaseNotYetForwarded == false);
  MOZ_ASSERT(*rootBase == nullptr);

  JS::AutoCheckCannotGC nogc;

  const CharT* dependentStrChars =
      tenuredDependentStr->nonInlineChars<CharT>(nogc);

  // Traverse the dependent string nursery base chain to find the base that
  // it's using chars from.
  while (true) {
    if (baseOrRelocOverlay->isForwarded()) {
      JSLinearString* tenuredBase = Forwarded(baseOrRelocOverlay);
      StringRelocationOverlay* relocOverlay =
          StringRelocationOverlay::fromCell(baseOrRelocOverlay);

      if (!tenuredBase->hasBase()) {
        // The nursery root base is relocOverlay, it is tenured to tenuredBase.
        // Relocate tenuredDependentStr chars and reassign the tenured root base
        // as its base.
        JSLinearString* tenuredRootBase = tenuredBase;
        const CharT* rootBaseChars = relocOverlay->savedNurseryChars<CharT>();
        *offset = dependentStrChars - rootBaseChars;
        MOZ_ASSERT(*offset < tenuredRootBase->length());
        tenuredDependentStr->relocateNonInlineChars<const CharT*>(
            tenuredRootBase->nonInlineChars<CharT>(nogc), *offset);
        tenuredDependentStr->setBase(tenuredRootBase);
        MOZ_ASSERT(tenuredRootBase->assertIsValidBase());

        if (tenuredDependentStr->isTenured() && !tenuredRootBase->isTenured()) {
          runtime()->gc.storeBuffer().putWholeCell(tenuredDependentStr);
        }
        return;
      }

      baseOrRelocOverlay = relocOverlay->savedNurseryBaseOrRelocOverlay();

    } else {
      JSLinearString* base = baseOrRelocOverlay;

      if (!base->hasBase()) {
        // The root base is not forwarded yet, it is simply base.
        *rootBase = base;

        // The root base can be in either the nursery or the tenured heap.
        // dependentStr chars needs to be relocated after traceString if the
        // root base is in the nursery.
        if (nursery().inCollectedRegion(*rootBase)) {
          *rootBaseNotYetForwarded = true;
          const CharT* rootBaseChars = (*rootBase)->nonInlineChars<CharT>(nogc);
          *offset = dependentStrChars - rootBaseChars;
          MOZ_ASSERT(*offset < base->length(), "Tenured root base");
        }

        tenuredDependentStr->setBase(*rootBase);
        MOZ_ASSERT((*rootBase)->assertIsValidBase());

        return;
      }

      baseOrRelocOverlay = base->nurseryBaseOrRelocOverlay();
    }
  }
}

JS::BigInt* js::gc::TenuringTracer::promoteBigInt(JS::BigInt* src) {
  MOZ_ASSERT(IsInsideNursery(src));

  AllocKind dstKind = src->getAllocKind();
  Zone* zone = src->nurseryZone();

  JS::BigInt* dst = alloc<JS::BigInt>(zone, dstKind, src);
  promotedSize += moveBigInt(dst, src, dstKind);
  promotedCells++;

  RelocationOverlay::forwardCell(src, dst);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

void js::gc::TenuringTracer::collectToObjectFixedPoint() {
  while (RelocationOverlay* p = objHead) {
    MOZ_ASSERT(nursery().inCollectedRegion(p));
    objHead = objHead->next();
    auto* obj = static_cast<JSObject*>(p->forwardingAddress());

    MOZ_ASSERT_IF(IsInsideNursery(obj), !nursery().inCollectedRegion(obj));

    AutoPromotedAnyToNursery promotedAnyToNursery(*this);
    traceObject(obj);
    if (obj->isTenured() && promotedAnyToNursery) {
      runtime()->gc.storeBuffer().putWholeCell(obj);
    }
  }
}

void js::gc::TenuringTracer::collectToStringFixedPoint() {
  while (StringRelocationOverlay* p = stringHead) {
    MOZ_ASSERT(nursery().inCollectedRegion(p));
    stringHead = stringHead->next();

    auto* str = static_cast<JSString*>(p->forwardingAddress());
    MOZ_ASSERT_IF(IsInsideNursery(str), !nursery().inCollectedRegion(str));

    // To ensure the NON_DEDUP_BIT was reset properly.
    MOZ_ASSERT(!str->isAtom());
    MOZ_ASSERT_IF(str->isTenured() && str->isLinear(), str->isDeduplicatable());

    // The nursery root base might not be forwarded before
    // traceString(str). traceString(str) will forward the root
    // base if that's the case. Dependent string chars needs to be relocated
    // after traceString if root base was not forwarded.
    size_t offset = 0;
    bool rootBaseNotYetForwarded = false;
    JSLinearString* rootBase = nullptr;

    if (str->isDependent() && !str->isAtomRef()) {
      if (str->hasTwoByteChars()) {
        relocateDependentStringChars<char16_t>(
            &str->asDependent(), p->savedNurseryBaseOrRelocOverlay(), &offset,
            &rootBaseNotYetForwarded, &rootBase);
      } else {
        relocateDependentStringChars<JS::Latin1Char>(
            &str->asDependent(), p->savedNurseryBaseOrRelocOverlay(), &offset,
            &rootBaseNotYetForwarded, &rootBase);
      }
    }

    AutoPromotedAnyToNursery promotedAnyToNursery(*this);
    traceString(str);
    if (str->isTenured() && promotedAnyToNursery) {
      runtime()->gc.storeBuffer().putWholeCell(str);
    }

    if (rootBaseNotYetForwarded) {
      MOZ_ASSERT(rootBase->isForwarded(),
                 "traceString() should make it forwarded");
      JS::AutoCheckCannotGC nogc;

      JSLinearString* tenuredRootBase = Forwarded(rootBase);
      MOZ_ASSERT(offset < tenuredRootBase->length());

      if (str->hasTwoByteChars()) {
        str->asDependent().relocateNonInlineChars<const char16_t*>(
            tenuredRootBase->twoByteChars(nogc), offset);
      } else {
        str->asDependent().relocateNonInlineChars<const JS::Latin1Char*>(
            tenuredRootBase->latin1Chars(nogc), offset);
      }

      str->setBase(tenuredRootBase);
      MOZ_ASSERT(tenuredRootBase->assertIsValidBase());
      if (str->isTenured() && !tenuredRootBase->isTenured()) {
        runtime()->gc.storeBuffer().putWholeCell(str);
      }
    }

    if (str->hasBase()) {
      MOZ_ASSERT(!str->base()->isForwarded());
      MOZ_ASSERT_IF(!str->base()->isTenured(),
                    !nursery().inCollectedRegion(str->base()));
    }
  }
}

size_t js::gc::TenuringTracer::moveString(JSString* dst, JSString* src,
                                          AllocKind dstKind) {
  size_t size = Arena::thingSize(dstKind);

  MOZ_ASSERT_IF(dst->isTenured(),
                dst->asTenured().getAllocKind() == src->getAllocKind());

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetToChunkEnd(src) >= size);
  js_memcpy(dst, src, size);

  if (!src->hasOutOfLineChars()) {
    return size;
  }

  if (src->ownsMallocedChars()) {
    void* chars = src->asLinear().nonInlineCharsRaw();
    nursery().removeMallocedBufferDuringMinorGC(chars);
    nursery().trackMallocedBufferOnPromotion(
        chars, dst, dst->asLinear().allocSize(), MemoryUse::StringContents);
    return size;
  }

  // String data is in the nursery and needs to be moved to the malloc heap.

  MOZ_ASSERT(nursery().isInside(src->asLinear().nonInlineCharsRaw()));

  if (src->hasLatin1Chars()) {
    size += dst->asLinear().maybeMallocCharsOnPromotion<Latin1Char>(&nursery());
  } else {
    size += dst->asLinear().maybeMallocCharsOnPromotion<char16_t>(&nursery());
  }

  return size;
}

size_t js::gc::TenuringTracer::moveBigInt(JS::BigInt* dst, JS::BigInt* src,
                                          AllocKind dstKind) {
  size_t size = Arena::thingSize(dstKind);

  MOZ_ASSERT_IF(dst->isTenured(),
                dst->asTenured().getAllocKind() == src->getAllocKind());

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetToChunkEnd(src) >= size);
  js_memcpy(dst, src, size);

  MOZ_ASSERT(dst->zone() == src->nurseryZone());

  if (!src->hasHeapDigits()) {
    return size;
  }

  size_t length = dst->digitLength();
  size_t nbytes = length * sizeof(JS::BigInt::Digit);

  Nursery::WasBufferMoved result = nursery().maybeMoveBufferOnPromotion(
      &dst->heapDigits_, dst, nbytes, MemoryUse::BigIntDigits);
  if (result == Nursery::BufferMoved) {
    nursery().setDirectForwardingPointer(src->heapDigits_, dst->heapDigits_);
    size += nbytes;
  }

  return size;
}

template <typename Key>
/* static */
inline HashNumber DeduplicationStringHasher<Key>::hash(const Lookup& lookup) {
  JS::AutoCheckCannotGC nogc;
  HashNumber strHash;

  // Include flags in the hash. A string relocation overlay stores either the
  // nursery root base chars or the dependent string nursery base, but does not
  // indicate which one. If strings with different string types were
  // deduplicated, for example, a dependent string gets deduplicated into an
  // extensible string, the base chain would be broken and the root base would
  // be unreachable.

  if (lookup->asLinear().hasLatin1Chars()) {
    strHash = mozilla::HashString(lookup->asLinear().latin1Chars(nogc),
                                  lookup->length());
  } else {
    MOZ_ASSERT(lookup->asLinear().hasTwoByteChars());
    strHash = mozilla::HashString(lookup->asLinear().twoByteChars(nogc),
                                  lookup->length());
  }

  return mozilla::HashGeneric(strHash, lookup->zone(), lookup->flags());
}

template <typename Key>
/* static */
MOZ_ALWAYS_INLINE bool DeduplicationStringHasher<Key>::match(
    const Key& key, const Lookup& lookup) {
  if (!key->sameLengthAndFlags(*lookup) ||
      key->asTenured().zone() != lookup->zone() ||
      key->asTenured().getAllocKind() != lookup->getAllocKind()) {
    return false;
  }

  JS::AutoCheckCannotGC nogc;

  if (key->asLinear().hasLatin1Chars()) {
    MOZ_ASSERT(lookup->asLinear().hasLatin1Chars());
    return EqualChars(key->asLinear().latin1Chars(nogc),
                      lookup->asLinear().latin1Chars(nogc), lookup->length());
  }

  MOZ_ASSERT(key->asLinear().hasTwoByteChars());
  MOZ_ASSERT(lookup->asLinear().hasTwoByteChars());
  return EqualChars(key->asLinear().twoByteChars(nogc),
                    lookup->asLinear().twoByteChars(nogc), lookup->length());
}

MinorSweepingTracer::MinorSweepingTracer(JSRuntime* rt)
    : GenericTracerImpl(rt, JS::TracerKind::MinorSweeping,
                        JS::WeakMapTraceAction::TraceKeysAndValues) {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtime()));
  MOZ_ASSERT(JS::RuntimeHeapIsMinorCollecting());
}

template <typename T>
inline void MinorSweepingTracer::onEdge(T** thingp, const char* name) {
  T* thing = *thingp;
  if (thing->isTenured()) {
    return;
  }

  if (IsForwarded(thing)) {
    *thingp = Forwarded(thing);
    return;
  }

  *thingp = nullptr;
}
