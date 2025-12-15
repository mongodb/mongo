/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implementation of nursery eviction (tenuring).
 */

#include "gc/Tenuring.h"

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
using namespace js;
using namespace js::gc;

constexpr size_t MAX_DEDUPLICATABLE_STRING_LENGTH = 500;

#ifdef JS_GC_ZEAL
class js::gc::PromotionStats {
  static constexpr size_t AttentionThreshold = 100;

  size_t objectCount = 0;
  size_t stringCount = 0;
  size_t bigIntCount = 0;

  using BaseShapeCountMap =
      HashMap<BaseShape*, size_t, PointerHasher<BaseShape*>, SystemAllocPolicy>;
  BaseShapeCountMap objectCountByBaseShape;

  using AllocKindCountArray =
      mozilla::EnumeratedArray<AllocKind, size_t, size_t(AllocKind::LIMIT)>;
  AllocKindCountArray stringCountByKind;

  bool hadOOM = false;

  struct LabelAndCount {
    char label[32] = {'\0'};
    size_t count = 0;
  };
  using CountsVector = Vector<LabelAndCount, 0, SystemAllocPolicy>;

 public:
  void notePromotedObject(JSObject* obj);
  void notePromotedString(JSString* str);
  void notePromotedBigInt(JS::BigInt* bi);

  bool shouldPrintReport() const;
  void printReport(JSContext* cx, const JS::AutoRequireNoGC& nogc);

 private:
  void printObjectCounts(JSContext* cx, const JS::AutoRequireNoGC& nogc);
  void printStringCounts();

  void printCounts(CountsVector& counts, size_t total);
  void printLine(const char* name, size_t count, size_t total);

  UniqueChars getConstructorName(JSContext* cx, BaseShape* baseShape,
                                 const JS::AutoRequireNoGC& nogc);
};
#endif  // JS_GC_ZEAL

TenuringTracer::TenuringTracer(JSRuntime* rt, Nursery* nursery,
                               bool tenureEverything)
    : JSTracer(rt, JS::TracerKind::Tenuring,
               JS::WeakMapTraceAction::TraceKeysAndValues),
      nursery_(*nursery),
      tenureEverything(tenureEverything) {
  stringDeDupSet.emplace();
}

TenuringTracer::~TenuringTracer() = default;

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

  return promoteObject(obj);
}

JSObject* TenuringTracer::promoteObject(JSObject* obj) {
  MOZ_ASSERT(IsInsideNursery(obj));
  MOZ_ASSERT(!obj->isForwarded());

#ifdef JS_GC_ZEAL
  if (promotionStats) {
    promotionStats->notePromotedObject(obj);
  }
#endif

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

  return promoteBigInt(bi);
}

// Ignore edges to cell kinds that are not allocated in the nursery.
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
void TenuringTracer::onSmallBufferEdge(SmallBuffer** sizedp, const char* name) {
}

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
    JSObject* obj = promoteObject(&value.toObject());
    MOZ_ASSERT(obj != &value.toObject());
    *thingp = JS::ObjectValue(*obj);
    return;
  }
  if (value.isString()) {
    JSString* str = promoteString(value.toString());
    MOZ_ASSERT(str != value.toString());
    *thingp = JS::StringValue(str);
    return;
  }
  MOZ_ASSERT(value.isBigInt());
  JS::BigInt* bi = promoteBigInt(value.toBigInt());
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

void JSDependentString::setBase(JSLinearString* newBase) {
  // This compiles down to a single assignment, with no type test.
  if (isAtomRef()) {
    MOZ_ASSERT(newBase->isAtom());
    d.s.u3.atom = &newBase->asAtom();
  } else {
    MOZ_ASSERT(newBase->canOwnDependentChars());
    d.s.u3.base = newBase;
  }

  if (isTenured() && !newBase->isTenured()) {
    MOZ_ASSERT(!InCollectedNurseryRegion(newBase));
    newBase->storeBuffer()->putWholeCell(this);
  }
}

static void TraceWholeCell(TenuringTracer& mover, JSString* str) {
  if (str->isDependent() && !str->isAtomRef()) {
    // For tenured dependent strings -> nursery base string edges, promote the
    // base immediately and then use its old chars pointer to find the offset
    // needed to update the dependent string's pointer if the base string moves
    // its chars.
    JSDependentString* dep = &str->asDependent();
    JSLinearString* base = dep->rootBaseDuringMinorGC();
    if (InCollectedNurseryRegion(base)) {
      mover.promoteOrForward(base);
      dep->updateToPromotedBase(base);
    } else {
      // Set base to (promoted) root base.
      dep->setBase(base);
    }
    return;
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

bool ArenaCellSet::trace(TenuringTracer& mover) {
  check();

  arena->bufferedCells() = &ArenaCellSet::Empty;

  JS::TraceKind kind = MapAllocToTraceKind(arena->getAllocKind());
  switch (kind) {
    case JS::TraceKind::Object:
      return mover.traceBufferedCells<JSObject>(arena, this);
      break;
    case JS::TraceKind::String:
      return mover.traceBufferedCells<JSString>(arena, this);
      break;
    case JS::TraceKind::Script:
      return mover.traceBufferedCells<BaseScript>(arena, this);
      break;
    case JS::TraceKind::JitCode:
      return mover.traceBufferedCells<jit::JitCode>(arena, this);
      break;
    default:
      MOZ_CRASH("Unexpected trace kind");
  }
}

void js::gc::StoreBuffer::WholeCellBuffer::trace(TenuringTracer& mover,
                                                 StoreBuffer* owner) {
  MOZ_ASSERT(owner->isEnabled());

  ArenaCellSet** sweepListTail = &sweepHead_;

  for (LifoAlloc::Enum e(*storage_); !e.empty();) {
    ArenaCellSet* cellSet = e.read<ArenaCellSet>();
    bool needsSweep = cellSet->trace(mover);
    if (needsSweep) {
      MOZ_ASSERT(!*sweepListTail);
      *sweepListTail = cellSet;
      sweepListTail = &cellSet->next;
    }
  }
}

namespace js::gc {
// StringRelocationOverlay assists with updating the string chars
// pointers of dependent strings when their base strings are
// deduplicated. It stores:
//  - nursery chars of potential root base strings
//  - the original pointer to the original root base (still in the nursery if it
//    was originally in the nursery, even if it has been forwarded to a promoted
//    string now).
//
// StringRelocationOverlay exploits the fact that the 3rd word of a JSString's
// RelocationOverlay is not utilized and can be used to store extra information.
class StringRelocationOverlay : public RelocationOverlay {
  union {
    // nursery chars of a root base
    const JS::Latin1Char* nurseryCharsLatin1;
    const char16_t* nurseryCharsTwoByte;

    // The nursery base can be forwarded, which becomes a string relocation
    // overlay, or it is not yet forwarded and is simply the (nursery) base
    // string.
    JSLinearString* nurseryBaseOrRelocOverlay;

    // For ropes. Present only to simplify the generated code.
    JSString* unusedLeftChild;
  };

 public:
  StringRelocationOverlay(Cell* dst, const JS::Latin1Char* chars)
      : RelocationOverlay(dst), nurseryCharsLatin1(chars) {}

  StringRelocationOverlay(Cell* dst, const char16_t* chars)
      : RelocationOverlay(dst), nurseryCharsTwoByte(chars) {}

  StringRelocationOverlay(Cell* dst, JSLinearString* origBase)
      : RelocationOverlay(dst), nurseryBaseOrRelocOverlay(origBase) {}

  StringRelocationOverlay(Cell* dst, JSString* origLeftChild)
      : RelocationOverlay(dst), unusedLeftChild(origLeftChild) {}

  static const StringRelocationOverlay* fromCell(const Cell* cell) {
    return static_cast<const StringRelocationOverlay*>(cell);
  }

  static StringRelocationOverlay* fromCell(Cell* cell) {
    return static_cast<StringRelocationOverlay*>(cell);
  }

  void setNext(StringRelocationOverlay* next) {
    RelocationOverlay::setNext(next);
  }

  StringRelocationOverlay* next() const {
    MOZ_ASSERT(isForwarded());
    return (StringRelocationOverlay*)next_;
  }

  template <typename CharT>
  MOZ_ALWAYS_INLINE const CharT* savedNurseryChars() const {
    if constexpr (std::is_same_v<CharT, JS::Latin1Char>) {
      return savedNurseryCharsLatin1();
    } else {
      return savedNurseryCharsTwoByte();
    }
  }

  const MOZ_ALWAYS_INLINE JS::Latin1Char* savedNurseryCharsLatin1() const {
    MOZ_ASSERT(!forwardingAddress()->as<JSString>()->hasBase());
    return nurseryCharsLatin1;
  }

  const MOZ_ALWAYS_INLINE char16_t* savedNurseryCharsTwoByte() const {
    MOZ_ASSERT(!forwardingAddress()->as<JSString>()->hasBase());
    return nurseryCharsTwoByte;
  }

  JSLinearString* savedNurseryBaseOrRelocOverlay() const {
    MOZ_ASSERT(forwardingAddress()->as<JSString>()->hasBase());
    return nurseryBaseOrRelocOverlay;
  }

  // Transform a nursery string to a StringRelocationOverlay that is forwarded
  // to a promoted string.
  inline static StringRelocationOverlay* forwardDependentString(JSString* src,
                                                                Cell* dst);

  // Usually only called on non-dependent strings, except for the case where a
  // dependent string is converted to a linear string.
  static StringRelocationOverlay* forwardString(JSString* src, Cell* dst) {
    MOZ_ASSERT(!src->isForwarded());
    MOZ_ASSERT(!dst->isForwarded());

    JS::AutoCheckCannotGC nogc;

    // Initialize the overlay for a non-dependent string (that could be the root
    // base of other strings), remember nursery non-inlined chars.
    //
    // Note that this will store the chars pointer even when it is known that it
    // will never be used (!canOwnDependentChar()), or a left child pointer of
    // a rope that will never get used, in order to simplify the generated code
    // to do an unconditional store.
    //
    // All of these compile down to
    //    header_.value_ = dst | 1; /* offset 0 */
    //    StringRelocationOverlay.union = d.s.u2; /* offset 16 <- offset 8 */
    if (src->isLinear()) {
      if (src->hasTwoByteChars()) {
        auto* nurseryCharsTwoByte = src->asLinear().twoByteChars(nogc);
        return new (src) StringRelocationOverlay(dst, nurseryCharsTwoByte);
      }
      auto* nurseryCharsLatin1 = src->asLinear().latin1Chars(nogc);
      return new (src) StringRelocationOverlay(dst, nurseryCharsLatin1);
    } else {
      return new (src) StringRelocationOverlay(
          dst, dst->as<JSString>()->asRope().leftChild());
    }
  }
};

/* static */
StringRelocationOverlay* StringRelocationOverlay::forwardDependentString(
    JSString* src, Cell* dst) {
  MOZ_ASSERT(src->isDependent());
  MOZ_ASSERT(!src->isForwarded());
  MOZ_ASSERT(!dst->isForwarded());
  JSLinearString* origBase = src->asDependent().rootBaseDuringMinorGC();
  return new (src) StringRelocationOverlay(dst, origBase);
}

}  // namespace js::gc

JSLinearString* JSDependentString::rootBaseDuringMinorGC() {
  JSLinearString* root = this;
  while (MaybeForwarded(root)->hasBase()) {
    if (root->isForwarded()) {
      root = js::gc::StringRelocationOverlay::fromCell(root)
                 ->savedNurseryBaseOrRelocOverlay();
    } else {
      // Possibly nursery or tenured string (not an overlay).
      root = root->nurseryBaseOrRelocOverlay();
    }
  }
  return root;
}

template <typename CharT>
static bool PtrIsWithinRange(const CharT* ptr,
                             const mozilla::Range<const CharT>& valid) {
  return size_t(ptr - valid.begin().get()) <= valid.length();
}

/* static */
template <typename CharT>
void JSLinearString::maybeCloneCharsOnPromotionTyped(JSLinearString* str) {
  MOZ_ASSERT(!InCollectedNurseryRegion(str), "str should have been promoted");
  MOZ_ASSERT(str->isDependent());
  JSLinearString* root = str->asDependent().rootBaseDuringMinorGC();
  if (InCollectedNurseryRegion(root)) {
    // Can still fixup the original chars pointer.
    return;
  }

  // If the base has not moved its chars, continue using them.
  JS::AutoCheckCannotGC nogc;
  const CharT* chars = str->chars<CharT>(nogc);
  if (PtrIsWithinRange(chars, root->range<CharT>(nogc))) {
    return;
  }

  // Clone the chars.
  js::AutoEnterOOMUnsafeRegion oomUnsafe;
  size_t len = str->length();
  size_t nbytes = len * sizeof(CharT);
  CharT* data =
      str->zone()->pod_arena_malloc<CharT>(js::StringBufferArena, len);
  if (!data) {
    oomUnsafe.crash("cloning at-risk dependent string");
  }
  js_memcpy(data, chars, nbytes);

  // Overwrite the dest string with a new linear string.
  new (str) JSLinearString(data, len, false /* hasBuffer */);
  if (str->isTenured()) {
    str->zone()->addCellMemory(str, nbytes, js::MemoryUse::StringContents);
  } else {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    JSRuntime* rt = str->runtimeFromAnyThread();
    if (!rt->gc.nursery().registerMallocedBuffer(data, nbytes)) {
      oomUnsafe.crash("maybeCloneCharsOnPromotionTyped");
    }
  }
}

template void JSLinearString::maybeCloneCharsOnPromotionTyped<JS::Latin1Char>(
    JSLinearString* str);
template void JSLinearString::maybeCloneCharsOnPromotionTyped<char16_t>(
    JSLinearString* str);

// Update a promoted dependent string with a nursery base. The base chain will
// have been collapsed to a single link, so only the simple case of a promoted
// dependent string with a nursery base needs to be considered.
template <typename CharT>
void JSDependentString::updateToPromotedBaseImpl(JSLinearString* base) {
  MOZ_ASSERT(!InCollectedNurseryRegion(this));
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
  JSLinearString* promotedBase = Forwarded(base);
  MOZ_ASSERT(offset < promotedBase->length());

  const CharT* newBaseChars =
      promotedBase->JSString::nonInlineCharsRaw<CharT>();
  relocateBaseAndChars(promotedBase, newBaseChars, offset);
}

inline void JSDependentString::updateToPromotedBase(JSLinearString* base) {
  // The base should have been traced.
  MOZ_ASSERT(base->isForwarded() || !InCollectedNurseryRegion(base));

  if (hasTwoByteChars()) {
    updateToPromotedBaseImpl<char16_t>(base);
  } else {
    updateToPromotedBaseImpl<JS::Latin1Char>(base);
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
  AutoPromotedAnyToNursery promotedToNursery(*this);
  str->traceChildren(this);
  if (str->isTenured() && promotedToNursery) {
    runtime()->gc.storeBuffer().putWholeCell(str);
  }
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
  Nursery::WasBufferMoved result =
      nursery().maybeMoveBufferOnPromotion(&header, dst, allocSize);
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

  Nursery::WasBufferMoved result =
      nursery().maybeMoveBufferOnPromotion(&unshiftedHeader, dst, allocSize);
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
  MOZ_ASSERT(!src->isForwarded());
  MOZ_ASSERT(!src->isExternal());

#ifdef JS_GC_ZEAL
  if (promotionStats) {
    promotionStats->notePromotedString(src);
  }
#endif

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

        StringRelocationOverlay::forwardString(src, atom);
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
      MOZ_ASSERT(dst->isTenured());  // Never deduplicate to a from-space cell.
      zone->stringStats.ref().noteDeduplicated(src->length(), src->allocSize());
      if (src->isDependent()) {
        StringRelocationOverlay::forwardDependentString(&src->asDependent(),
                                                        dst);
      } else {
        StringRelocationOverlay::forwardString(src, dst);
      }
      gcprobes::PromoteToTenured(src, dst);
      return dst;
    }

    dst = allocString(src, zone, dstKind);

    // In some situations, a string may be converted to a different type when
    // tenured. Currently, this only happens when a dependent string's chain of
    // base strings makes it impossible to recover its data, in which case it
    // will get converted to a regular linear string. In order to avoid
    // rehashing and some complexity, do not deduplicate to such strings.
    if (dst->flags() == src->flags()) {
      using DedupHasher [[maybe_unused]] = DeduplicationStringHasher<JSString*>;
      MOZ_ASSERT(DedupHasher::hash(src) == DedupHasher::hash(dst),
                 "src and dst must have the same hash for lookupForAdd");

      if (!stringDeDupSet->add(p, dst)) {
        // When there is oom caused by the stringDeDupSet, stop deduplicating
        // strings.
        stringDeDupSet.reset();
      }
    }
  } else {
    dst = allocString(src, zone, dstKind);
    if (dst->isTenured()) {
      src->clearBitsOnTenure();
      dst->clearBitsOnTenure();
    }
  }

  gcprobes::PromoteToTenured(src, dst);
  zone->stringStats.ref().noteTenured(src->allocSize());

  if (dst->isDependent()) {
    // Dependent string:
    //   - root base was tenured => done
    //   - otherwise => promote the root base if it has not already been
    //     promoted, then update the dependent string's chars pointer based on
    //     the root base's original chars pointer (stored in its
    //     StringRelocationOverlay during promotion)

    JSLinearString* base = src->asDependent().rootBaseDuringMinorGC();

    // Limited recursion: the root base cannot be dependent, so this will not
    // recurse again.
    JSString* promotedBase =
        InCollectedNurseryRegion(base) ? promoteOrForward(base) : base;
    MOZ_ASSERT(!promotedBase->isDependent());

    dst->asDependent().setBase(&promotedBase->asLinear());
    if (InCollectedNurseryRegion(base)) {
      dst->asDependent().updateToPromotedBase(base);
    }

    StringRelocationOverlay::forwardDependentString(src, dst);
  } else {
    // Non-dependent string: store the original chars pointer in the nursery
    // cell (for future promotions of any dependent strings that use this string
    // as a base), then forward to the promoted cell.

    StringRelocationOverlay::forwardString(src, dst);
    if (dst->isRope()) {
      // Enqueue for recursion through the children.
      insertIntoStringFixupList(StringRelocationOverlay::fromCell(src));
    }
  }

  MOZ_ASSERT_IF(dst->isTenured() && dst->isLinear(), dst->isDeduplicatable());

  return dst;
}

JS::BigInt* js::gc::TenuringTracer::promoteBigInt(JS::BigInt* src) {
  MOZ_ASSERT(IsInsideNursery(src));
  MOZ_ASSERT(!src->isForwarded());

#ifdef JS_GC_ZEAL
  if (promotionStats) {
    promotionStats->notePromotedBigInt(src);
  }
#endif

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
  // Recurse through ropes.

  while (StringRelocationOverlay* p = stringHead) {
    MOZ_ASSERT(nursery().inCollectedRegion(p));
    stringHead = stringHead->next();

    auto* promotedStr = static_cast<JSString*>(p->forwardingAddress());
    MOZ_ASSERT_IF(IsInsideNursery(promotedStr),
                  !nursery().inCollectedRegion(promotedStr));

    // To ensure the NON_DEDUP_BIT was reset properly.
    MOZ_ASSERT(!promotedStr->isAtom());
    MOZ_ASSERT_IF(promotedStr->isTenured() && promotedStr->isLinear(),
                  promotedStr->isDeduplicatable());

    MOZ_ASSERT(promotedStr->isRope());
    traceString(promotedStr);
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

  if (src->isDependent()) {
    // Special case: if src is a dependent string whose base chain goes through
    // tenured space, then it may point to dead chars -- either because its root
    // base was deduplicated, or its root base's chars were allocated in the
    // nursery. If src's chars pointer will no longer be valid once minor GC is
    // complete, give it its own copy of the chars.
    //
    // Note that the size of any cloned data is *not* included in the "number
    // of bytes tenured" return value here, since the donor owns them and may
    // still be alive and we don't want to double-count.
    JSLinearString::maybeCloneCharsOnPromotion(&dst->asDependent());
    return size;
  }

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

  if (src->asLinear().hasStringBuffer()) {
    auto* buffer = src->asLinear().stringBuffer();
    if (dst->isTenured()) {
      // Increment the buffer's refcount because the tenured string now has a
      // reference to it. The nursery's reference will be released at the end of
      // the minor GC in Nursery::sweep.
      buffer->AddRef();
      AddCellMemory(dst, dst->asLinear().allocSize(),
                    MemoryUse::StringContents);
    }
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

  Nursery::WasBufferMoved result =
      nursery().maybeMoveNurseryOrMallocBufferOnPromotion(
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

#ifdef JS_GC_ZEAL

void TenuringTracer::initPromotionReport() {
  MOZ_ASSERT(!promotionStats);
  promotionStats = MakeUnique<PromotionStats>();
  // Ignore OOM.
}

void TenuringTracer::printPromotionReport(
    JSContext* cx, JS::GCReason reason, const JS::AutoRequireNoGC& nogc) const {
  if (!promotionStats || !promotionStats->shouldPrintReport()) {
    return;
  }

  size_t minorGCCount = runtime()->gc.minorGCCount();
  double usedBytes = double(nursery_.previousGC.nurseryUsedBytes);
  double capacityBytes = double(nursery_.previousGC.nurseryCapacity);
  double fractionPromoted = double(getPromotedSize()) / usedBytes;
  double usedMB = usedBytes / (1024 * 1024);
  double capacityMB = capacityBytes / (1024 * 1024);
  fprintf(stderr, "Promotion stats for minor GC %zu:\n", minorGCCount);
  fprintf(stderr, "  Reason: %s\n", ExplainGCReason(reason));
  fprintf(stderr, "  Nursery size: %4.1f MB used of %4.1f MB\n", usedMB,
          capacityMB);
  fprintf(stderr, "  Promotion rate: %5.1f%%\n", fractionPromoted);

  promotionStats->printReport(cx, nogc);
}

void PromotionStats::notePromotedObject(JSObject* obj) {
  objectCount++;

  BaseShape* baseShape = obj->shape()->base();
  auto ptr = objectCountByBaseShape.lookupForAdd(baseShape);
  if (!ptr) {
    if (!objectCountByBaseShape.add(ptr, baseShape, 0)) {
      hadOOM = true;
      return;
    }
  }
  ptr->value()++;
}

void PromotionStats::notePromotedString(JSString* str) {
  stringCount++;

  AllocKind kind = str->getAllocKind();
  stringCountByKind[kind]++;
}

void PromotionStats::notePromotedBigInt(JS::BigInt* bi) { bigIntCount++; }

bool PromotionStats::shouldPrintReport() const {
  if (hadOOM) {
    return false;
  }

  return objectCount || stringCount || bigIntCount;
}

void PromotionStats::printReport(JSContext* cx,
                                 const JS::AutoRequireNoGC& nogc) {
  if (objectCount) {
    fprintf(stderr, "  Objects promoted: %zu\n", objectCount);
    printObjectCounts(cx, nogc);
  }

  if (stringCount) {
    fprintf(stderr, "  Strings promoted: %zu\n", stringCount);
    printStringCounts();
  }

  if (bigIntCount) {
    fprintf(stderr, "  BigInts promoted: %zu\n", bigIntCount);
  }
}

void PromotionStats::printObjectCounts(JSContext* cx,
                                       const JS::AutoRequireNoGC& nogc) {
  CountsVector counts;

  for (auto r = objectCountByBaseShape.all(); !r.empty(); r.popFront()) {
    size_t count = r.front().value();
    if (count < AttentionThreshold) {
      continue;
    }

    BaseShape* baseShape = r.front().key();

    const char* className = baseShape->clasp()->name;

    UniqueChars constructorName = getConstructorName(cx, baseShape, nogc);
    const char* constructorChars = constructorName ? constructorName.get() : "";

    LabelAndCount entry;
    snprintf(entry.label, sizeof(entry.label), "%s %s", className,
             constructorChars);
    entry.count = count;
    if (!counts.append(entry)) {
      return;
    }
  }

  printCounts(counts, objectCount);
}

UniqueChars PromotionStats::getConstructorName(
    JSContext* cx, BaseShape* baseShape, const JS::AutoRequireNoGC& nogc) {
  TaggedProto taggedProto = baseShape->proto();
  if (taggedProto.isDynamic()) {
    return nullptr;
  }

  JSObject* proto = taggedProto.toObjectOrNull();
  if (!proto) {
    return nullptr;
  }

  MOZ_ASSERT(!proto->isForwarded());

  AutoRealm ar(cx, proto);
  bool found;
  Value value;
  if (!GetOwnPropertyPure(cx, proto, NameToId(cx->names().constructor), &value,
                          &found)) {
    return nullptr;
  }

  if (!found || !value.isObject()) {
    return nullptr;
  }

  JSFunction* constructor = &value.toObject().as<JSFunction>();
  JSAtom* atom = constructor->maybePartialDisplayAtom();
  if (!atom) {
    return nullptr;
  }

  return EncodeAscii(cx, atom);
}

void PromotionStats::printStringCounts() {
  CountsVector counts;
  for (AllocKind kind : AllAllocKinds()) {
    size_t count = stringCountByKind[kind];
    if (count < AttentionThreshold) {
      continue;
    }

    const char* name = AllocKindName(kind);
    LabelAndCount entry;
    strncpy(entry.label, name, sizeof(entry.label));
    entry.label[sizeof(entry.label) - 1] = 0;
    entry.count = count;
    if (!counts.append(entry)) {
      return;
    }
  }

  printCounts(counts, stringCount);
}

void PromotionStats::printCounts(CountsVector& counts, size_t total) {
  std::sort(counts.begin(), counts.end(),
            [](const auto& a, const auto& b) { return a.count > b.count; });

  size_t max = std::min(counts.length(), size_t(10));
  for (size_t i = 0; i < max; i++) {
    const auto& entry = counts[i];
    printLine(entry.label, entry.count, total);
  }
}

void PromotionStats::printLine(const char* name, size_t count, size_t total) {
  double percent = 100.0 * double(count) / double(total);
  fprintf(stderr, "    %5.1f%%: %s\n", percent, name);
}

#endif

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
    MOZ_ASSERT(!IsForwarded(thing));
    return;
  }

  MOZ_ASSERT(runtime()->gc.nursery().inCollectedRegion(thing));
  if (IsForwarded(thing)) {
    *thingp = Forwarded(thing);
    return;
  }

  *thingp = nullptr;
}
