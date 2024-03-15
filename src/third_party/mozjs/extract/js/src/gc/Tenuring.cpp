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
#ifdef ENABLE_RECORD_TUPLE
#  include "vm/TupleType.h"
#endif

using namespace js;
using namespace js::gc;

using mozilla::PodCopy;

constexpr size_t MAX_DEDUPLICATABLE_STRING_LENGTH = 500;

TenuringTracer::TenuringTracer(JSRuntime* rt, Nursery* nursery)
    : JSTracer(rt, JS::TracerKind::Tenuring,
               JS::WeakMapTraceAction::TraceKeysAndValues),
      nursery_(*nursery) {
  stringDeDupSet.emplace();
}

size_t TenuringTracer::getTenuredSize() const {
  return tenuredSize + tenuredCells * sizeof(NurseryCellHeader);
}

size_t TenuringTracer::getTenuredCells() const { return tenuredCells; }

static inline void UpdateAllocSiteOnTenure(Cell* cell) {
  AllocSite* site = NurseryCellHeader::from(cell)->allocSite();
  site->incTenuredCount();
}

void TenuringTracer::onObjectEdge(JSObject** objp, const char* name) {
  JSObject* obj = *objp;
  if (!IsInsideNursery(obj)) {
    return;
  }

  if (obj->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(obj);
    *objp = static_cast<JSObject*>(overlay->forwardingAddress());
    return;
  }

  UpdateAllocSiteOnTenure(obj);

  // Take a fast path for tenuring a plain object which is by far the most
  // common case.
  if (obj->is<PlainObject>()) {
    *objp = movePlainObjectToTenured(&obj->as<PlainObject>());
    return;
  }

  *objp = moveToTenuredSlow(obj);
}

void TenuringTracer::onStringEdge(JSString** strp, const char* name) {
  JSString* str = *strp;
  if (!IsInsideNursery(str)) {
    return;
  }

  if (str->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(str);
    *strp = static_cast<JSString*>(overlay->forwardingAddress());
    return;
  }

  UpdateAllocSiteOnTenure(str);

  *strp = moveToTenured(str);
}

void TenuringTracer::onBigIntEdge(JS::BigInt** bip, const char* name) {
  JS::BigInt* bi = *bip;
  if (!IsInsideNursery(bi)) {
    return;
  }

  if (bi->isForwarded()) {
    const gc::RelocationOverlay* overlay = gc::RelocationOverlay::fromCell(bi);
    *bip = static_cast<JS::BigInt*>(overlay->forwardingAddress());
    return;
  }

  UpdateAllocSiteOnTenure(bi);

  *bip = moveToTenured(bi);
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
  MOZ_ASSERT(!nursery().isInside(thingp));

  Value value = *thingp;
  CheckTracedThing(this, value);

  // We only care about a few kinds of GC thing here and this generates much
  // tighter code than using MapGCThingTyped.
  Value post;
  if (value.isObject()) {
    JSObject* obj = &value.toObject();
    onObjectEdge(&obj, "value");
    post = JS::ObjectValue(*obj);
  }
#ifdef ENABLE_RECORD_TUPLE
  else if (value.isExtendedPrimitive()) {
    JSObject* obj = &value.toExtendedPrimitive();
    onObjectEdge(&obj, "value");
    post = JS::ExtendedPrimitiveValue(*obj);
  }
#endif
  else if (value.isString()) {
    JSString* str = value.toString();
    onStringEdge(&str, "value");
    post = JS::StringValue(str);
  } else if (value.isBigInt()) {
    JS::BigInt* bi = value.toBigInt();
    onBigIntEdge(&bi, "value");
    post = JS::BigIntValue(bi);
  } else {
    MOZ_ASSERT_IF(value.isGCThing(), !IsInsideNursery(value.toGCThing()));
    return;
  }

  if (post != value) {
    *thingp = post;
  }
}

template <typename T>
void js::gc::StoreBuffer::MonoTypeBuffer<T>::trace(TenuringTracer& mover) {
  mozilla::ReentrancyGuard g(*owner_);
  MOZ_ASSERT(owner_->isEnabled());
  if (last_) {
    last_.trace(mover);
  }
  for (typename StoreSet::Range r = stores_.all(); !r.empty(); r.popFront()) {
    r.front().trace(mover);
  }
}

namespace js {
namespace gc {
template void StoreBuffer::MonoTypeBuffer<StoreBuffer::ValueEdge>::trace(
    TenuringTracer&);
template void StoreBuffer::MonoTypeBuffer<StoreBuffer::SlotsEdge>::trace(
    TenuringTracer&);
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::StringPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::BigIntPtrEdge>;
template struct StoreBuffer::MonoTypeBuffer<StoreBuffer::ObjectPtrEdge>;
}  // namespace gc
}  // namespace js

void js::gc::StoreBuffer::SlotsEdge::trace(TenuringTracer& mover) const {
  NativeObject* obj = object();
  MOZ_ASSERT(IsCellPointerValid(obj));

  // Beware JSObject::swap exchanging a native object for a non-native one.
  if (!obj->is<NativeObject>()) {
    return;
  }

  MOZ_ASSERT(!IsInsideNursery(obj), "obj shouldn't live in nursery.");

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
    mover.traceSlots(
        static_cast<HeapSlot*>(obj->getDenseElements() + clampedStart)
            ->unbarrieredAddress(),
        clampedEnd - clampedStart);
  } else {
    uint32_t start = std::min(start_, obj->slotSpan());
    uint32_t end = std::min(start_ + count_, obj->slotSpan());
    MOZ_ASSERT(start <= end);
    mover.traceObjectSlots(obj, start, end);
  }
}

static inline void TraceWholeCell(TenuringTracer& mover, JSObject* object) {
  MOZ_ASSERT_IF(object->storeBuffer(),
                !object->storeBuffer()->markingNondeduplicatable);
  mover.traceObject(object);
}

// Non-deduplicatable marking is necessary because of the following 2 reasons:
//
// 1. Tenured string chars cannot be updated:
//
//    If any of the tenured string's bases were deduplicated during tenuring,
//    the tenured string's chars pointer would need to be adjusted. This would
//    then require updating any other tenured strings that are dependent on the
//    first tenured string, and we have no way to find them without scanning
//    the entire tenured heap.
//
// 2. Tenured string cannot store its nursery base or base's chars:
//
//    Tenured strings have no place to stash a pointer to their nursery base or
//    its chars. You need to be able to traverse any dependent string's chain
//    of bases up to a nursery "root base" that points to the malloced chars
//    that the dependent strings started out pointing to, so that you can
//    calculate the offset of any dependent string and update the ptr+offset if
//    the root base gets deduplicated to a different allocation. Tenured
//    strings in this base chain will stop you from reaching the nursery
//    version of the root base; you can only get to the tenured version, and it
//    has no place to store the original chars pointer.
static inline void PreventDeduplicationOfReachableStrings(JSString* str) {
  MOZ_ASSERT(str->isTenured());
  MOZ_ASSERT(!str->isForwarded());

  JSLinearString* baseOrRelocOverlay = str->nurseryBaseOrRelocOverlay();

  // Walk along the chain of dependent strings' base string pointers
  // to mark them all non-deduplicatable.
  while (true) {
    // baseOrRelocOverlay can be one of the three cases:
    // 1. forwarded nursery string:
    //    The forwarded string still retains the flag that can tell whether
    //    this string is a dependent string with a base. Its
    //    StringRelocationOverlay holds a saved pointer to its base in the
    //    nursery.
    // 2. not yet forwarded nursery string:
    //    Retrieve the base field directly from the string.
    // 3. tenured string:
    //    The nursery base chain ends here, so stop traversing.
    if (baseOrRelocOverlay->isForwarded()) {
      JSLinearString* tenuredBase = Forwarded(baseOrRelocOverlay);
      if (!tenuredBase->hasBase()) {
        break;
      }
      baseOrRelocOverlay = StringRelocationOverlay::fromCell(baseOrRelocOverlay)
                               ->savedNurseryBaseOrRelocOverlay();
    } else {
      JSLinearString* base = baseOrRelocOverlay;
      if (base->isTenured()) {
        break;
      }
      if (base->isDeduplicatable()) {
        base->setNonDeduplicatable();
      }
      if (!base->hasBase()) {
        break;
      }
      baseOrRelocOverlay = base->nurseryBaseOrRelocOverlay();
    }
  }
}

static inline void TraceWholeCell(TenuringTracer& mover, JSString* str) {
  MOZ_ASSERT_IF(str->storeBuffer(),
                str->storeBuffer()->markingNondeduplicatable);

  // Mark all strings reachable from the tenured string `str` as
  // non-deduplicatable. These strings are the bases of the tenured dependent
  // string.
  if (str->hasBase()) {
    PreventDeduplicationOfReachableStrings(str);
  }

  str->traceChildren(&mover);
}

static inline void TraceWholeCell(TenuringTracer& mover, BaseScript* script) {
  script->traceChildren(&mover);
}

static inline void TraceWholeCell(TenuringTracer& mover,
                                  jit::JitCode* jitcode) {
  jitcode->traceChildren(&mover);
}

template <typename T>
static void TraceBufferedCells(TenuringTracer& mover, Arena* arena,
                               ArenaCellSet* cells) {
  for (size_t i = 0; i < MaxArenaCellIndex; i += cells->BitsPerWord) {
    ArenaCellSet::WordT bitset = cells->getWord(i / cells->BitsPerWord);
    while (bitset) {
      size_t bit = i + js::detail::CountTrailingZeroes(bitset);
      auto cell =
          reinterpret_cast<T*>(uintptr_t(arena) + ArenaCellIndexBytes * bit);
      TraceWholeCell(mover, cell);
      bitset &= bitset - 1;  // Clear the low bit.
    }
  }
}

void ArenaCellSet::trace(TenuringTracer& mover) {
  for (ArenaCellSet* cells = this; cells; cells = cells->next) {
    cells->check();

    Arena* arena = cells->arena;
    arena->bufferedCells() = &ArenaCellSet::Empty;

    JS::TraceKind kind = MapAllocToTraceKind(arena->getAllocKind());
    switch (kind) {
      case JS::TraceKind::Object:
        TraceBufferedCells<JSObject>(mover, arena, cells);
        break;
      case JS::TraceKind::String:
        TraceBufferedCells<JSString>(mover, arena, cells);
        break;
      case JS::TraceKind::Script:
        TraceBufferedCells<BaseScript>(mover, arena, cells);
        break;
      case JS::TraceKind::JitCode:
        TraceBufferedCells<jit::JitCode>(mover, arena, cells);
        break;
      default:
        MOZ_CRASH("Unexpected trace kind");
    }
  }
}

void js::gc::StoreBuffer::WholeCellBuffer::trace(TenuringTracer& mover) {
  MOZ_ASSERT(owner_->isEnabled());

#ifdef DEBUG
  // Verify that all string whole cells are traced first before any other
  // strings are visited for any reason.
  MOZ_ASSERT(!owner_->markingNondeduplicatable);
  owner_->markingNondeduplicatable = true;
#endif
  // Trace all of the strings to mark the non-deduplicatable bits, then trace
  // all other whole cells.
  if (stringHead_) {
    stringHead_->trace(mover);
  }
#ifdef DEBUG
  owner_->markingNondeduplicatable = false;
#endif
  if (nonStringHead_) {
    nonStringHead_->trace(mover);
  }

  stringHead_ = nonStringHead_ = nullptr;
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

  DispatchToOnEdge(&mover, edge, "CellPtrEdge");
}

void js::gc::StoreBuffer::ValueEdge::trace(TenuringTracer& mover) const {
  if (deref()) {
    mover.traverse(edge);
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
    traceSlots(elems, elems + nobj->getDenseInitializedLength());
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

void js::gc::TenuringTracer::traceSlots(Value* vp, Value* end) {
  for (; vp != end; ++vp) {
    traverse(vp);
  }
}

inline void js::gc::TenuringTracer::traceSlots(JS::Value* vp, uint32_t nslots) {
  traceSlots(vp, vp + nslots);
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
static inline ptrdiff_t OffsetToChunkEnd(void* p) {
  return ChunkSize - (uintptr_t(p) & gc::ChunkMask);
}
#endif

/* Insert the given relocation entry into the list of things to visit. */
inline void js::gc::TenuringTracer::insertIntoObjectFixupList(
    RelocationOverlay* entry) {
  entry->setNext(objHead);
  objHead = entry;
}

template <typename T>
inline T* js::gc::TenuringTracer::allocTenured(Zone* zone, AllocKind kind) {
  return static_cast<T*>(static_cast<Cell*>(AllocateCellInGC(zone, kind)));
}

JSString* js::gc::TenuringTracer::allocTenuredString(JSString* src, Zone* zone,
                                                     AllocKind dstKind) {
  JSString* dst = allocTenured<JSString>(zone, dstKind);
  tenuredSize += moveStringToTenured(dst, src, dstKind);
  tenuredCells++;

  return dst;
}

JSObject* js::gc::TenuringTracer::moveToTenuredSlow(JSObject* src) {
  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->is<PlainObject>());

  AllocKind dstKind = src->allocKindForTenure(nursery());
  auto dst = allocTenured<JSObject>(src->nurseryZone(), dstKind);

  size_t srcSize = Arena::thingSize(dstKind);

  // Arrays and Tuples do not necessarily have the same AllocKind between src
  // and dst. We deal with this by copying elements manually, possibly
  // re-inlining them if there is adequate room inline in dst.
  //
  // For Arrays and Tuples we're reducing tenuredSize to the smaller srcSize
  // because moveElementsToTenured() accounts for all Array or Tuple elements,
  // even if they are inlined.
  if (src->is<TypedArrayObject>()) {
    TypedArrayObject* tarray = &src->as<TypedArrayObject>();
    // Typed arrays with inline data do not necessarily have the same
    // AllocKind between src and dst. The nursery does not allocate an
    // inline data buffer that has the same size as the slow path will do.
    // In the slow path, the Typed Array Object stores the inline data
    // in the allocated space that fits the AllocKind. In the fast path,
    // the nursery will allocate another buffer that is directly behind the
    // minimal JSObject. That buffer size plus the JSObject size is not
    // necessarily as large as the slow path's AllocKind size.
    if (tarray->hasInlineElements()) {
      AllocKind srcKind = GetGCObjectKind(TypedArrayObject::FIXED_DATA_START);
      size_t headerSize = Arena::thingSize(srcKind);
      srcSize = headerSize + tarray->byteLength();
    }
  } else if (src->canHaveFixedElements()) {
    srcSize = sizeof(NativeObject);
  }

  tenuredSize += srcSize;
  tenuredCells++;

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetFromChunkStart(src) >= sizeof(ChunkBase));
  MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(srcSize));
  js_memcpy(dst, src, srcSize);

  // Move the slots and elements, if we need to.
  if (src->is<NativeObject>()) {
    NativeObject* ndst = &dst->as<NativeObject>();
    NativeObject* nsrc = &src->as<NativeObject>();
    tenuredSize += moveSlotsToTenured(ndst, nsrc);
    tenuredSize += moveElementsToTenured(ndst, nsrc, dstKind);
  }

  JSObjectMovedOp op = dst->getClass()->extObjectMovedOp();
  MOZ_ASSERT_IF(src->is<ProxyObject>(), op == proxy_ObjectMoved);
  if (op) {
    // Tell the hazard analysis that the object moved hook can't GC.
    JS::AutoSuppressGCAnalysis nogc;
    tenuredSize += op(dst, src);
  } else {
    MOZ_ASSERT_IF(src->getClass()->hasFinalize(),
                  CanNurseryAllocateFinalizedClass(src->getClass()));
  }

  RelocationOverlay* overlay = RelocationOverlay::forwardCell(src, dst);
  insertIntoObjectFixupList(overlay);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

inline JSObject* js::gc::TenuringTracer::movePlainObjectToTenured(
    PlainObject* src) {
  // Fast path version of moveToTenuredSlow() for specialized for PlainObject.

  MOZ_ASSERT(IsInsideNursery(src));

  AllocKind dstKind = src->allocKindForTenure();
  auto dst = allocTenured<PlainObject>(src->nurseryZone(), dstKind);

  size_t srcSize = Arena::thingSize(dstKind);
  tenuredSize += srcSize;
  tenuredCells++;

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetFromChunkStart(src) >= sizeof(ChunkBase));
  MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(srcSize));
  js_memcpy(dst, src, srcSize);

  // Move the slots and elements.
  tenuredSize += moveSlotsToTenured(dst, src);
  tenuredSize += moveElementsToTenured(dst, src, dstKind);

  MOZ_ASSERT(!dst->getClass()->extObjectMovedOp());

  RelocationOverlay* overlay = RelocationOverlay::forwardCell(src, dst);
  insertIntoObjectFixupList(overlay);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

size_t js::gc::TenuringTracer::moveSlotsToTenured(NativeObject* dst,
                                                  NativeObject* src) {
  /* Fixed slots have already been copied over. */
  if (!src->hasDynamicSlots()) {
    return 0;
  }

  Zone* zone = src->nurseryZone();
  size_t count = src->numDynamicSlots();

  uint64_t uid = src->maybeUniqueId();

  size_t allocSize = ObjectSlots::allocSize(count);

  ObjectSlots* srcHeader = src->getSlotsHeader();
  if (!nursery().isInside(srcHeader)) {
    AddCellMemory(dst, allocSize, MemoryUse::ObjectSlots);
    nursery().removeMallocedBufferDuringMinorGC(srcHeader);
    return 0;
  }

  {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    HeapSlot* allocation =
        zone->pod_malloc<HeapSlot>(ObjectSlots::allocCount(count));
    if (!allocation) {
      oomUnsafe.crash(allocSize, "Failed to allocate slots while tenuring.");
    }

    ObjectSlots* slotsHeader = new (allocation)
        ObjectSlots(count, srcHeader->dictionarySlotSpan(), uid);
    dst->slots_ = slotsHeader->slots();
  }

  AddCellMemory(dst, allocSize, MemoryUse::ObjectSlots);

  PodCopy(dst->slots_, src->slots_, count);
  if (count) {
    nursery().setSlotsForwardingPointer(src->slots_, dst->slots_, count);
  }

  return allocSize;
}

size_t js::gc::TenuringTracer::moveElementsToTenured(NativeObject* dst,
                                                     NativeObject* src,
                                                     AllocKind dstKind) {
  if (src->hasEmptyElements()) {
    return 0;
  }

  Zone* zone = src->nurseryZone();

  ObjectElements* srcHeader = src->getElementsHeader();
  size_t nslots = srcHeader->numAllocatedElements();
  size_t allocSize = nslots * sizeof(HeapSlot);

  void* srcAllocatedHeader = src->getUnshiftedElementsHeader();

  /* TODO Bug 874151: Prefer to put element data inline if we have space. */
  if (!nursery().isInside(srcAllocatedHeader)) {
    MOZ_ASSERT(src->elements_ == dst->elements_);
    nursery().removeMallocedBufferDuringMinorGC(srcAllocatedHeader);

    AddCellMemory(dst, allocSize, MemoryUse::ObjectElements);

    return 0;
  }

  // Shifted elements are copied too.
  uint32_t numShifted = srcHeader->numShiftedElements();

  /* Unlike other objects, Arrays and Tuples can have fixed elements. */
  if (src->canHaveFixedElements() && nslots <= GetGCKindSlots(dstKind)) {
    dst->as<NativeObject>().setFixedElements();
    js_memcpy(dst->getElementsHeader(), srcAllocatedHeader, allocSize);
    dst->elements_ += numShifted;
    dst->getElementsHeader()->flags |= ObjectElements::FIXED;
    nursery().setElementsForwardingPointer(srcHeader, dst->getElementsHeader(),
                                           srcHeader->capacity);
    return allocSize;
  }

  MOZ_ASSERT(nslots >= 2);

  ObjectElements* dstHeader;
  {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    dstHeader =
        reinterpret_cast<ObjectElements*>(zone->pod_malloc<HeapSlot>(nslots));
    if (!dstHeader) {
      oomUnsafe.crash(allocSize, "Failed to allocate elements while tenuring.");
    }
  }

  AddCellMemory(dst, allocSize, MemoryUse::ObjectElements);

  js_memcpy(dstHeader, srcAllocatedHeader, allocSize);
  dst->elements_ = dstHeader->elements() + numShifted;
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

JSString* js::gc::TenuringTracer::moveToTenured(JSString* src) {
  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->isExternal());

  AllocKind dstKind = src->getAllocKind();
  Zone* zone = src->nurseryZone();

  // If this string is in the StringToAtomCache, try to deduplicate it by using
  // the atom. Don't do this for dependent strings because they're more
  // complicated. See StringRelocationOverlay and DeduplicationStringHasher
  // comments.
  if (src->isLinear() && src->inStringToAtomCache() &&
      src->isDeduplicatable() && !src->hasBase()) {
    JSLinearString* linear = &src->asLinear();
    JSAtom* atom = runtime()->caches().stringToAtomCache.lookupInMap(linear);
    MOZ_ASSERT(atom, "Why was the cache purged before minor GC?");

    // Only deduplicate if both strings have the same encoding, to not confuse
    // dependent strings.
    if (src->hasTwoByteChars() == atom->hasTwoByteChars()) {
      // The StringToAtomCache isn't used for inline strings (due to the minimum
      // length) so canOwnDependentChars must be true for both src and atom.
      // This means if there are dependent strings floating around using str's
      // chars, they will be able to use the chars from the atom.
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

  JSString* dst;

  // A live nursery string can only get deduplicated when:
  // 1. Its length is smaller than MAX_DEDUPLICATABLE_STRING_LENGTH:
  //    Hashing a long string can affect performance.
  // 2. It is linear:
  //    Deduplicating every node in it would end up doing O(n^2) hashing work.
  // 3. It is deduplicatable:
  //    The JSString NON_DEDUP_BIT flag is unset.
  // 4. It matches an entry in stringDeDupSet.

  if (src->length() < MAX_DEDUPLICATABLE_STRING_LENGTH && src->isLinear() &&
      src->isDeduplicatable() && stringDeDupSet.isSome()) {
    if (auto p = stringDeDupSet->lookup(src)) {
      // Deduplicate to the looked-up string!
      dst = *p;
      zone->stringStats.ref().noteDeduplicated(src->length(), src->allocSize());
      StringRelocationOverlay::forwardCell(src, dst);
      gcprobes::PromoteToTenured(src, dst);
      return dst;
    }

    dst = allocTenuredString(src, zone, dstKind);

    if (!stringDeDupSet->putNew(dst)) {
      // When there is oom caused by the stringDeDupSet, stop deduplicating
      // strings.
      stringDeDupSet.reset();
    }
  } else {
    dst = allocTenuredString(src, zone, dstKind);
    dst->clearNonDeduplicatable();
  }

  zone->stringStats.ref().noteTenured(src->allocSize());

  auto* overlay = StringRelocationOverlay::forwardCell(src, dst);
  MOZ_ASSERT(dst->isDeduplicatable());

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
        if (!(*rootBase)->isTenured()) {
          *rootBaseNotYetForwarded = true;
          const CharT* rootBaseChars = (*rootBase)->nonInlineChars<CharT>(nogc);
          *offset = dependentStrChars - rootBaseChars;
          MOZ_ASSERT(*offset < base->length(), "Tenured root base");
        }

        tenuredDependentStr->setBase(*rootBase);

        return;
      }

      baseOrRelocOverlay = base->nurseryBaseOrRelocOverlay();
    }
  }
}

JS::BigInt* js::gc::TenuringTracer::moveToTenured(JS::BigInt* src) {
  MOZ_ASSERT(IsInsideNursery(src));

  AllocKind dstKind = src->getAllocKind();
  Zone* zone = src->nurseryZone();
  zone->tenuredBigInts++;

  JS::BigInt* dst = allocTenured<JS::BigInt>(zone, dstKind);
  tenuredSize += moveBigIntToTenured(dst, src, dstKind);
  tenuredCells++;

  RelocationOverlay::forwardCell(src, dst);

  gcprobes::PromoteToTenured(src, dst);
  return dst;
}

void js::gc::TenuringTracer::collectToObjectFixedPoint() {
  while (RelocationOverlay* p = objHead) {
    objHead = objHead->next();
    auto* obj = static_cast<JSObject*>(p->forwardingAddress());
    traceObject(obj);
  }
}

void js::gc::TenuringTracer::collectToStringFixedPoint() {
  while (StringRelocationOverlay* p = stringHead) {
    stringHead = stringHead->next();

    auto* tenuredStr = static_cast<JSString*>(p->forwardingAddress());
    // To ensure the NON_DEDUP_BIT was reset properly.
    MOZ_ASSERT(tenuredStr->isDeduplicatable());

    // The nursery root base might not be forwarded before
    // traceString(tenuredStr). traceString(tenuredStr) will forward the root
    // base if that's the case. Dependent string chars needs to be relocated
    // after traceString if root base was not forwarded.
    size_t offset = 0;
    bool rootBaseNotYetForwarded = false;
    JSLinearString* rootBase = nullptr;

    if (tenuredStr->isDependent()) {
      if (tenuredStr->hasTwoByteChars()) {
        relocateDependentStringChars<char16_t>(
            &tenuredStr->asDependent(), p->savedNurseryBaseOrRelocOverlay(),
            &offset, &rootBaseNotYetForwarded, &rootBase);
      } else {
        relocateDependentStringChars<JS::Latin1Char>(
            &tenuredStr->asDependent(), p->savedNurseryBaseOrRelocOverlay(),
            &offset, &rootBaseNotYetForwarded, &rootBase);
      }
    }

    traceString(tenuredStr);

    if (rootBaseNotYetForwarded) {
      MOZ_ASSERT(rootBase->isForwarded(),
                 "traceString() should make it forwarded");
      JS::AutoCheckCannotGC nogc;

      JSLinearString* tenuredRootBase = Forwarded(rootBase);
      MOZ_ASSERT(offset < tenuredRootBase->length());

      if (tenuredStr->hasTwoByteChars()) {
        tenuredStr->asDependent().relocateNonInlineChars<const char16_t*>(
            tenuredRootBase->twoByteChars(nogc), offset);
      } else {
        tenuredStr->asDependent().relocateNonInlineChars<const JS::Latin1Char*>(
            tenuredRootBase->latin1Chars(nogc), offset);
      }
      tenuredStr->setBase(tenuredRootBase);
    }
  }
}

size_t js::gc::TenuringTracer::moveStringToTenured(JSString* dst, JSString* src,
                                                   AllocKind dstKind) {
  size_t size = Arena::thingSize(dstKind);

  // At the moment, strings always have the same AllocKind between src and
  // dst. This may change in the future.
  MOZ_ASSERT(dst->asTenured().getAllocKind() == src->getAllocKind());

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(size));
  js_memcpy(dst, src, size);

  if (src->ownsMallocedChars()) {
    void* chars = src->asLinear().nonInlineCharsRaw();
    nursery().removeMallocedBufferDuringMinorGC(chars);
    AddCellMemory(dst, dst->asLinear().allocSize(), MemoryUse::StringContents);
  }

  return size;
}

size_t js::gc::TenuringTracer::moveBigIntToTenured(JS::BigInt* dst,
                                                   JS::BigInt* src,
                                                   AllocKind dstKind) {
  size_t size = Arena::thingSize(dstKind);

  // At the moment, BigInts always have the same AllocKind between src and
  // dst. This may change in the future.
  MOZ_ASSERT(dst->asTenured().getAllocKind() == src->getAllocKind());

  // Copy the Cell contents.
  MOZ_ASSERT(OffsetToChunkEnd(src) >= ptrdiff_t(size));
  js_memcpy(dst, src, size);

  MOZ_ASSERT(dst->zone() == src->nurseryZone());

  if (src->hasHeapDigits()) {
    size_t length = dst->digitLength();
    if (!nursery().isInside(src->heapDigits_)) {
      nursery().removeMallocedBufferDuringMinorGC(src->heapDigits_);
    } else {
      Zone* zone = src->nurseryZone();
      {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        dst->heapDigits_ = zone->pod_malloc<JS::BigInt::Digit>(length);
        if (!dst->heapDigits_) {
          oomUnsafe.crash(sizeof(JS::BigInt::Digit) * length,
                          "Failed to allocate digits while tenuring.");
        }
      }

      PodCopy(dst->heapDigits_, src->heapDigits_, length);
      nursery().setDirectForwardingPointer(src->heapDigits_, dst->heapDigits_);

      size += length * sizeof(JS::BigInt::Digit);
    }

    AddCellMemory(dst, length * sizeof(JS::BigInt::Digit),
                  MemoryUse::BigIntDigits);
  }

  return size;
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
