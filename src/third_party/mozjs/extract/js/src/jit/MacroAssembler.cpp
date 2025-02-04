/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MacroAssembler-inl.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <algorithm>
#include <utility>

#include "jit/AtomicOp.h"
#include "jit/AtomicOperations.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineJIT.h"
#include "jit/JitFrames.h"
#include "jit/JitOptions.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "jit/MoveEmitter.h"
#include "jit/ReciprocalMulConstants.h"
#include "jit/SharedICHelpers.h"
#include "jit/SharedICRegisters.h"
#include "jit/Simulator.h"
#include "jit/VMFunctions.h"
#include "js/Conversions.h"
#include "js/friend/DOMProxy.h"  // JS::ExpandoAndGeneration
#include "js/ScalarType.h"       // js::Scalar::Type
#include "vm/ArgumentsObject.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/BoundFunctionObject.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCodegenConstants.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmInstanceData.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValidate.h"

#include "jit/TemplateObject-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::jit;

using JS::GenericNaN;
using JS::ToInt32;

using mozilla::CheckedInt;

TrampolinePtr MacroAssembler::preBarrierTrampoline(MIRType type) {
  const JitRuntime* rt = runtime()->jitRuntime();
  return rt->preBarrier(type);
}

template <typename S, typename T>
static void StoreToTypedFloatArray(MacroAssembler& masm, int arrayType,
                                   const S& value, const T& dest) {
  switch (arrayType) {
    case Scalar::Float32:
      masm.storeFloat32(value, dest);
      break;
    case Scalar::Float64:
      masm.storeDouble(value, dest);
      break;
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

void MacroAssembler::storeToTypedFloatArray(Scalar::Type arrayType,
                                            FloatRegister value,
                                            const BaseIndex& dest) {
  StoreToTypedFloatArray(*this, arrayType, value, dest);
}
void MacroAssembler::storeToTypedFloatArray(Scalar::Type arrayType,
                                            FloatRegister value,
                                            const Address& dest) {
  StoreToTypedFloatArray(*this, arrayType, value, dest);
}

template <typename S, typename T>
static void StoreToTypedBigIntArray(MacroAssembler& masm,
                                    Scalar::Type arrayType, const S& value,
                                    const T& dest) {
  MOZ_ASSERT(Scalar::isBigIntType(arrayType));
  masm.store64(value, dest);
}

void MacroAssembler::storeToTypedBigIntArray(Scalar::Type arrayType,
                                             Register64 value,
                                             const BaseIndex& dest) {
  StoreToTypedBigIntArray(*this, arrayType, value, dest);
}
void MacroAssembler::storeToTypedBigIntArray(Scalar::Type arrayType,
                                             Register64 value,
                                             const Address& dest) {
  StoreToTypedBigIntArray(*this, arrayType, value, dest);
}

void MacroAssembler::boxUint32(Register source, ValueOperand dest,
                               Uint32Mode mode, Label* fail) {
  switch (mode) {
    // Fail if the value does not fit in an int32.
    case Uint32Mode::FailOnDouble: {
      branchTest32(Assembler::Signed, source, source, fail);
      tagValue(JSVAL_TYPE_INT32, source, dest);
      break;
    }
    case Uint32Mode::ForceDouble: {
      // Always convert the value to double.
      ScratchDoubleScope fpscratch(*this);
      convertUInt32ToDouble(source, fpscratch);
      boxDouble(fpscratch, dest, fpscratch);
      break;
    }
  }
}

template <typename T>
void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType, const T& src,
                                        AnyRegister dest, Register temp,
                                        Label* fail) {
  switch (arrayType) {
    case Scalar::Int8:
      load8SignExtend(src, dest.gpr());
      break;
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
      load8ZeroExtend(src, dest.gpr());
      break;
    case Scalar::Int16:
      load16SignExtend(src, dest.gpr());
      break;
    case Scalar::Uint16:
      load16ZeroExtend(src, dest.gpr());
      break;
    case Scalar::Int32:
      load32(src, dest.gpr());
      break;
    case Scalar::Uint32:
      if (dest.isFloat()) {
        load32(src, temp);
        convertUInt32ToDouble(temp, dest.fpu());
      } else {
        load32(src, dest.gpr());

        // Bail out if the value doesn't fit into a signed int32 value. This
        // is what allows MLoadUnboxedScalar to have a type() of
        // MIRType::Int32 for UInt32 array loads.
        branchTest32(Assembler::Signed, dest.gpr(), dest.gpr(), fail);
      }
      break;
    case Scalar::Float32:
      loadFloat32(src, dest.fpu());
      canonicalizeFloat(dest.fpu());
      break;
    case Scalar::Float64:
      loadDouble(src, dest.fpu());
      canonicalizeDouble(dest.fpu());
      break;
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

template void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType,
                                                 const Address& src,
                                                 AnyRegister dest,
                                                 Register temp, Label* fail);
template void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType,
                                                 const BaseIndex& src,
                                                 AnyRegister dest,
                                                 Register temp, Label* fail);

template <typename T>
void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType, const T& src,
                                        const ValueOperand& dest,
                                        Uint32Mode uint32Mode, Register temp,
                                        Label* fail) {
  switch (arrayType) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
      loadFromTypedArray(arrayType, src, AnyRegister(dest.scratchReg()),
                         InvalidReg, nullptr);
      tagValue(JSVAL_TYPE_INT32, dest.scratchReg(), dest);
      break;
    case Scalar::Uint32:
      // Don't clobber dest when we could fail, instead use temp.
      load32(src, temp);
      boxUint32(temp, dest, uint32Mode, fail);
      break;
    case Scalar::Float32: {
      ScratchDoubleScope dscratch(*this);
      FloatRegister fscratch = dscratch.asSingle();
      loadFromTypedArray(arrayType, src, AnyRegister(fscratch),
                         dest.scratchReg(), nullptr);
      convertFloat32ToDouble(fscratch, dscratch);
      boxDouble(dscratch, dest, dscratch);
      break;
    }
    case Scalar::Float64: {
      ScratchDoubleScope fpscratch(*this);
      loadFromTypedArray(arrayType, src, AnyRegister(fpscratch),
                         dest.scratchReg(), nullptr);
      boxDouble(fpscratch, dest, fpscratch);
      break;
    }
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

template void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType,
                                                 const Address& src,
                                                 const ValueOperand& dest,
                                                 Uint32Mode uint32Mode,
                                                 Register temp, Label* fail);
template void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType,
                                                 const BaseIndex& src,
                                                 const ValueOperand& dest,
                                                 Uint32Mode uint32Mode,
                                                 Register temp, Label* fail);

template <typename T>
void MacroAssembler::loadFromTypedBigIntArray(Scalar::Type arrayType,
                                              const T& src, Register bigInt,
                                              Register64 temp) {
  MOZ_ASSERT(Scalar::isBigIntType(arrayType));

  load64(src, temp);
  initializeBigInt64(arrayType, bigInt, temp);
}

template void MacroAssembler::loadFromTypedBigIntArray(Scalar::Type arrayType,
                                                       const Address& src,
                                                       Register bigInt,
                                                       Register64 temp);
template void MacroAssembler::loadFromTypedBigIntArray(Scalar::Type arrayType,
                                                       const BaseIndex& src,
                                                       Register bigInt,
                                                       Register64 temp);

// Inlined version of gc::CheckAllocatorState that checks the bare essentials
// and bails for anything that cannot be handled with our jit allocators.
void MacroAssembler::checkAllocatorState(Label* fail) {
  // Don't execute the inline path if GC probes are built in.
#ifdef JS_GC_PROBES
  jump(fail);
#endif

#ifdef JS_GC_ZEAL
  // Don't execute the inline path if gc zeal or tracing are active.
  const uint32_t* ptrZealModeBits = runtime()->addressOfGCZealModeBits();
  branch32(Assembler::NotEqual, AbsoluteAddress(ptrZealModeBits), Imm32(0),
           fail);
#endif

  // Don't execute the inline path if the realm has an object metadata callback,
  // as the metadata to use for the object may vary between executions of the
  // op.
  if (realm()->hasAllocationMetadataBuilder()) {
    jump(fail);
  }
}

bool MacroAssembler::shouldNurseryAllocate(gc::AllocKind allocKind,
                                           gc::Heap initialHeap) {
  // Note that Ion elides barriers on writes to objects known to be in the
  // nursery, so any allocation that can be made into the nursery must be made
  // into the nursery, even if the nursery is disabled. At runtime these will
  // take the out-of-line path, which is required to insert a barrier for the
  // initializing writes.
  return IsNurseryAllocable(allocKind) && initialHeap != gc::Heap::Tenured;
}

// Inline version of Nursery::allocateObject. If the object has dynamic slots,
// this fills in the slots_ pointer.
void MacroAssembler::nurseryAllocateObject(Register result, Register temp,
                                           gc::AllocKind allocKind,
                                           size_t nDynamicSlots, Label* fail,
                                           const AllocSiteInput& allocSite) {
  MOZ_ASSERT(IsNurseryAllocable(allocKind));

  // Currently the JIT does not nursery allocate foreground finalized
  // objects. This is allowed for objects that support this and have the
  // JSCLASS_SKIP_NURSERY_FINALIZE class flag set. It's hard to assert that here
  // though so disallow all foreground finalized objects for now.
  MOZ_ASSERT(!IsForegroundFinalized(allocKind));

  // We still need to allocate in the nursery, per the comment in
  // shouldNurseryAllocate; however, we need to insert into the
  // mallocedBuffers set, so bail to do the nursery allocation in the
  // interpreter.
  if (nDynamicSlots >= Nursery::MaxNurseryBufferSize / sizeof(Value)) {
    jump(fail);
    return;
  }

  // Check whether this allocation site needs pretenuring. This dynamic check
  // only happens for baseline code.
  if (allocSite.is<Register>()) {
    Register site = allocSite.as<Register>();
    branchTestPtr(Assembler::NonZero,
                  Address(site, gc::AllocSite::offsetOfScriptAndState()),
                  Imm32(gc::AllocSite::LONG_LIVED_BIT), fail);
  }

  // No explicit check for nursery.isEnabled() is needed, as the comparison
  // with the nursery's end will always fail in such cases.
  CompileZone* zone = realm()->zone();
  size_t thingSize = gc::Arena::thingSize(allocKind);
  size_t totalSize = thingSize;
  if (nDynamicSlots) {
    totalSize += ObjectSlots::allocSize(nDynamicSlots);
  }
  MOZ_ASSERT(totalSize < INT32_MAX);
  MOZ_ASSERT(totalSize % gc::CellAlignBytes == 0);

  bumpPointerAllocate(result, temp, fail, zone, JS::TraceKind::Object,
                      totalSize, allocSite);

  if (nDynamicSlots) {
    store32(Imm32(nDynamicSlots),
            Address(result, thingSize + ObjectSlots::offsetOfCapacity()));
    store32(
        Imm32(0),
        Address(result, thingSize + ObjectSlots::offsetOfDictionarySlotSpan()));
    store64(Imm64(ObjectSlots::NoUniqueIdInDynamicSlots),
            Address(result, thingSize + ObjectSlots::offsetOfMaybeUniqueId()));
    computeEffectiveAddress(
        Address(result, thingSize + ObjectSlots::offsetOfSlots()), temp);
    storePtr(temp, Address(result, NativeObject::offsetOfSlots()));
  }
}

// Inlined version of FreeSpan::allocate. This does not fill in slots_.
void MacroAssembler::freeListAllocate(Register result, Register temp,
                                      gc::AllocKind allocKind, Label* fail) {
  CompileZone* zone = realm()->zone();
  int thingSize = int(gc::Arena::thingSize(allocKind));

  Label fallback;
  Label success;

  // Load the first and last offsets of |zone|'s free list for |allocKind|.
  // If there is no room remaining in the span, fall back to get the next one.
  gc::FreeSpan** ptrFreeList = zone->addressOfFreeList(allocKind);
  loadPtr(AbsoluteAddress(ptrFreeList), temp);
  load16ZeroExtend(Address(temp, js::gc::FreeSpan::offsetOfFirst()), result);
  load16ZeroExtend(Address(temp, js::gc::FreeSpan::offsetOfLast()), temp);
  branch32(Assembler::AboveOrEqual, result, temp, &fallback);

  // Bump the offset for the next allocation.
  add32(Imm32(thingSize), result);
  loadPtr(AbsoluteAddress(ptrFreeList), temp);
  store16(result, Address(temp, js::gc::FreeSpan::offsetOfFirst()));
  sub32(Imm32(thingSize), result);
  addPtr(temp, result);  // Turn the offset into a pointer.
  jump(&success);

  bind(&fallback);
  // If there are no free spans left, we bail to finish the allocation. The
  // interpreter will call the GC allocator to set up a new arena to allocate
  // from, after which we can resume allocating in the jit.
  branchTest32(Assembler::Zero, result, result, fail);
  loadPtr(AbsoluteAddress(ptrFreeList), temp);
  addPtr(temp, result);  // Turn the offset into a pointer.
  Push(result);
  // Update the free list to point to the next span (which may be empty).
  load32(Address(result, 0), result);
  store32(result, Address(temp, js::gc::FreeSpan::offsetOfFirst()));
  Pop(result);

  bind(&success);

  if (runtime()->geckoProfiler().enabled()) {
    uint32_t* countAddress = zone->addressOfTenuredAllocCount();
    movePtr(ImmPtr(countAddress), temp);
    add32(Imm32(1), Address(temp, 0));
  }
}

void MacroAssembler::callFreeStub(Register slots) {
  // This register must match the one in JitRuntime::generateFreeStub.
  const Register regSlots = CallTempReg0;

  push(regSlots);
  movePtr(slots, regSlots);
  call(runtime()->jitRuntime()->freeStub());
  pop(regSlots);
}

// Inlined equivalent of gc::AllocateObject, without failure case handling.
void MacroAssembler::allocateObject(Register result, Register temp,
                                    gc::AllocKind allocKind,
                                    uint32_t nDynamicSlots,
                                    gc::Heap initialHeap, Label* fail,
                                    const AllocSiteInput& allocSite) {
  MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));

  checkAllocatorState(fail);

  if (shouldNurseryAllocate(allocKind, initialHeap)) {
    MOZ_ASSERT(initialHeap == gc::Heap::Default);
    return nurseryAllocateObject(result, temp, allocKind, nDynamicSlots, fail,
                                 allocSite);
  }

  // Fall back to calling into the VM to allocate objects in the tenured heap
  // that have dynamic slots.
  if (nDynamicSlots) {
    jump(fail);
    return;
  }

  return freeListAllocate(result, temp, allocKind, fail);
}

void MacroAssembler::createGCObject(Register obj, Register temp,
                                    const TemplateObject& templateObj,
                                    gc::Heap initialHeap, Label* fail,
                                    bool initContents /* = true */) {
  gc::AllocKind allocKind = templateObj.getAllocKind();
  MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));

  uint32_t nDynamicSlots = 0;
  if (templateObj.isNativeObject()) {
    const TemplateNativeObject& ntemplate =
        templateObj.asTemplateNativeObject();
    nDynamicSlots = ntemplate.numDynamicSlots();
  }

  allocateObject(obj, temp, allocKind, nDynamicSlots, initialHeap, fail);
  initGCThing(obj, temp, templateObj, initContents);
}

void MacroAssembler::createPlainGCObject(
    Register result, Register shape, Register temp, Register temp2,
    uint32_t numFixedSlots, uint32_t numDynamicSlots, gc::AllocKind allocKind,
    gc::Heap initialHeap, Label* fail, const AllocSiteInput& allocSite,
    bool initContents /* = true */) {
  MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));
  MOZ_ASSERT(shape != temp, "shape can overlap with temp2, but not temp");

  // Allocate object.
  allocateObject(result, temp, allocKind, numDynamicSlots, initialHeap, fail,
                 allocSite);

  // Initialize shape field.
  storePtr(shape, Address(result, JSObject::offsetOfShape()));

  // If the object has dynamic slots, allocateObject will initialize
  // the slots field. If not, we must initialize it now.
  if (numDynamicSlots == 0) {
    storePtr(ImmPtr(emptyObjectSlots),
             Address(result, NativeObject::offsetOfSlots()));
  }

  // Initialize elements field.
  storePtr(ImmPtr(emptyObjectElements),
           Address(result, NativeObject::offsetOfElements()));

  // Initialize fixed slots.
  if (initContents) {
    fillSlotsWithUndefined(Address(result, NativeObject::getFixedSlotOffset(0)),
                           temp, 0, numFixedSlots);
  }

  // Initialize dynamic slots.
  if (numDynamicSlots > 0) {
    loadPtr(Address(result, NativeObject::offsetOfSlots()), temp2);
    fillSlotsWithUndefined(Address(temp2, 0), temp, 0, numDynamicSlots);
  }
}

void MacroAssembler::createArrayWithFixedElements(
    Register result, Register shape, Register temp, uint32_t arrayLength,
    uint32_t arrayCapacity, gc::AllocKind allocKind, gc::Heap initialHeap,
    Label* fail, const AllocSiteInput& allocSite) {
  MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));
  MOZ_ASSERT(shape != temp, "shape can overlap with temp2, but not temp");
  MOZ_ASSERT(result != temp);

  // This only supports allocating arrays with fixed elements and does not
  // support any dynamic slots or elements.
  MOZ_ASSERT(arrayCapacity >= arrayLength);
  MOZ_ASSERT(gc::GetGCKindSlots(allocKind) >=
             arrayCapacity + ObjectElements::VALUES_PER_HEADER);

  // Allocate object.
  allocateObject(result, temp, allocKind, 0, initialHeap, fail, allocSite);

  // Initialize shape field.
  storePtr(shape, Address(result, JSObject::offsetOfShape()));

  // There are no dynamic slots.
  storePtr(ImmPtr(emptyObjectSlots),
           Address(result, NativeObject::offsetOfSlots()));

  // Initialize elements pointer for fixed (inline) elements.
  computeEffectiveAddress(
      Address(result, NativeObject::offsetOfFixedElements()), temp);
  storePtr(temp, Address(result, NativeObject::offsetOfElements()));

  // Initialize elements header.
  store32(Imm32(ObjectElements::FIXED),
          Address(temp, ObjectElements::offsetOfFlags()));
  store32(Imm32(0), Address(temp, ObjectElements::offsetOfInitializedLength()));
  store32(Imm32(arrayCapacity),
          Address(temp, ObjectElements::offsetOfCapacity()));
  store32(Imm32(arrayLength), Address(temp, ObjectElements::offsetOfLength()));
}

// Inline version of Nursery::allocateString.
void MacroAssembler::nurseryAllocateString(Register result, Register temp,
                                           gc::AllocKind allocKind,
                                           Label* fail) {
  MOZ_ASSERT(IsNurseryAllocable(allocKind));

  // No explicit check for nursery.isEnabled() is needed, as the comparison
  // with the nursery's end will always fail in such cases.

  CompileZone* zone = realm()->zone();
  size_t thingSize = gc::Arena::thingSize(allocKind);
  bumpPointerAllocate(result, temp, fail, zone, JS::TraceKind::String,
                      thingSize);
}

// Inline version of Nursery::allocateBigInt.
void MacroAssembler::nurseryAllocateBigInt(Register result, Register temp,
                                           Label* fail) {
  MOZ_ASSERT(IsNurseryAllocable(gc::AllocKind::BIGINT));

  // No explicit check for nursery.isEnabled() is needed, as the comparison
  // with the nursery's end will always fail in such cases.

  CompileZone* zone = realm()->zone();
  size_t thingSize = gc::Arena::thingSize(gc::AllocKind::BIGINT);

  bumpPointerAllocate(result, temp, fail, zone, JS::TraceKind::BigInt,
                      thingSize);
}

static bool IsNurseryAllocEnabled(CompileZone* zone, JS::TraceKind kind) {
  switch (kind) {
    case JS::TraceKind::Object:
      return zone->allocNurseryObjects();
    case JS::TraceKind::String:
      return zone->allocNurseryStrings();
    case JS::TraceKind::BigInt:
      return zone->allocNurseryBigInts();
    default:
      MOZ_CRASH("Bad nursery allocation kind");
  }
}

void MacroAssembler::bumpPointerAllocate(Register result, Register temp,
                                         Label* fail, CompileZone* zone,
                                         JS::TraceKind traceKind, uint32_t size,
                                         const AllocSiteInput& allocSite) {
  MOZ_ASSERT(size >= gc::MinCellSize);

  uint32_t totalSize = size + Nursery::nurseryCellHeaderSize();
  MOZ_ASSERT(totalSize < INT32_MAX, "Nursery allocation too large");
  MOZ_ASSERT(totalSize % gc::CellAlignBytes == 0);

  // We know statically whether nursery allocation is enable for a particular
  // kind because we discard JIT code when this changes.
  if (!IsNurseryAllocEnabled(zone, traceKind)) {
    jump(fail);
    return;
  }

  // Use a relative 32 bit offset to the Nursery position_ to currentEnd_ to
  // avoid 64-bit immediate loads.
  void* posAddr = zone->addressOfNurseryPosition();
  int32_t endOffset = Nursery::offsetOfCurrentEndFromPosition();

  movePtr(ImmPtr(posAddr), temp);
  loadPtr(Address(temp, 0), result);
  addPtr(Imm32(totalSize), result);
  branchPtr(Assembler::Below, Address(temp, endOffset), result, fail);
  storePtr(result, Address(temp, 0));
  subPtr(Imm32(size), result);

  if (allocSite.is<gc::CatchAllAllocSite>()) {
    // No allocation site supplied. This is the case when called from Warp, or
    // from places that don't support pretenuring.
    gc::CatchAllAllocSite siteKind = allocSite.as<gc::CatchAllAllocSite>();
    gc::AllocSite* site = zone->catchAllAllocSite(traceKind, siteKind);
    uintptr_t headerWord = gc::NurseryCellHeader::MakeValue(site, traceKind);
    storePtr(ImmWord(headerWord),
             Address(result, -js::Nursery::nurseryCellHeaderSize()));

    // Update the catch all allocation site for strings or if the profiler is
    // enabled. This is used to calculate the nursery allocation count. The
    // string data is used to determine whether to disable nursery string
    // allocation.
    if (traceKind == JS::TraceKind::String ||
        runtime()->geckoProfiler().enabled()) {
      uint32_t* countAddress = site->nurseryAllocCountAddress();
      CheckedInt<int32_t> counterOffset =
          (CheckedInt<uintptr_t>(uintptr_t(countAddress)) -
           CheckedInt<uintptr_t>(uintptr_t(posAddr)))
              .toChecked<int32_t>();
      if (counterOffset.isValid()) {
        add32(Imm32(1), Address(temp, counterOffset.value()));
      } else {
        movePtr(ImmPtr(countAddress), temp);
        add32(Imm32(1), Address(temp, 0));
      }
    }
  } else {
    // Update allocation site and store pointer in the nursery cell header. This
    // is only used from baseline.
    Register site = allocSite.as<Register>();
    updateAllocSite(temp, result, zone, site);
    // See NurseryCellHeader::MakeValue.
    orPtr(Imm32(int32_t(traceKind)), site);
    storePtr(site, Address(result, -js::Nursery::nurseryCellHeaderSize()));
  }
}

// Update the allocation site in the same way as Nursery::allocateCell.
void MacroAssembler::updateAllocSite(Register temp, Register result,
                                     CompileZone* zone, Register site) {
  Label done;

  add32(Imm32(1), Address(site, gc::AllocSite::offsetOfNurseryAllocCount()));

  branch32(Assembler::NotEqual,
           Address(site, gc::AllocSite::offsetOfNurseryAllocCount()), Imm32(1),
           &done);

  loadPtr(AbsoluteAddress(zone->addressOfNurseryAllocatedSites()), temp);
  storePtr(temp, Address(site, gc::AllocSite::offsetOfNextNurseryAllocated()));
  storePtr(site, AbsoluteAddress(zone->addressOfNurseryAllocatedSites()));

  bind(&done);
}

// Inlined equivalent of gc::AllocateString, jumping to fail if nursery
// allocation requested but unsuccessful.
void MacroAssembler::allocateString(Register result, Register temp,
                                    gc::AllocKind allocKind,
                                    gc::Heap initialHeap, Label* fail) {
  MOZ_ASSERT(allocKind == gc::AllocKind::STRING ||
             allocKind == gc::AllocKind::FAT_INLINE_STRING);

  checkAllocatorState(fail);

  if (shouldNurseryAllocate(allocKind, initialHeap)) {
    MOZ_ASSERT(initialHeap == gc::Heap::Default);
    return nurseryAllocateString(result, temp, allocKind, fail);
  }

  freeListAllocate(result, temp, allocKind, fail);
}

void MacroAssembler::newGCString(Register result, Register temp,
                                 gc::Heap initialHeap, Label* fail) {
  allocateString(result, temp, js::gc::AllocKind::STRING, initialHeap, fail);
}

void MacroAssembler::newGCFatInlineString(Register result, Register temp,
                                          gc::Heap initialHeap, Label* fail) {
  allocateString(result, temp, js::gc::AllocKind::FAT_INLINE_STRING,
                 initialHeap, fail);
}

void MacroAssembler::newGCBigInt(Register result, Register temp,
                                 gc::Heap initialHeap, Label* fail) {
  checkAllocatorState(fail);

  if (shouldNurseryAllocate(gc::AllocKind::BIGINT, initialHeap)) {
    MOZ_ASSERT(initialHeap == gc::Heap::Default);
    return nurseryAllocateBigInt(result, temp, fail);
  }

  freeListAllocate(result, temp, gc::AllocKind::BIGINT, fail);
}

void MacroAssembler::copySlotsFromTemplate(
    Register obj, const TemplateNativeObject& templateObj, uint32_t start,
    uint32_t end) {
  uint32_t nfixed = std::min(templateObj.numFixedSlots(), end);
  for (unsigned i = start; i < nfixed; i++) {
    // Template objects are not exposed to script and therefore immutable.
    // However, regexp template objects are sometimes used directly (when
    // the cloning is not observable), and therefore we can end up with a
    // non-zero lastIndex. Detect this case here and just substitute 0, to
    // avoid racing with the main thread updating this slot.
    Value v;
    if (templateObj.isRegExpObject() && i == RegExpObject::lastIndexSlot()) {
      v = Int32Value(0);
    } else {
      v = templateObj.getSlot(i);
    }
    storeValue(v, Address(obj, NativeObject::getFixedSlotOffset(i)));
  }
}

void MacroAssembler::fillSlotsWithConstantValue(Address base, Register temp,
                                                uint32_t start, uint32_t end,
                                                const Value& v) {
  MOZ_ASSERT(v.isUndefined() || IsUninitializedLexical(v));

  if (start >= end) {
    return;
  }

#ifdef JS_NUNBOX32
  // We only have a single spare register, so do the initialization as two
  // strided writes of the tag and body.
  Address addr = base;
  move32(Imm32(v.toNunboxPayload()), temp);
  for (unsigned i = start; i < end; ++i, addr.offset += sizeof(GCPtr<Value>)) {
    store32(temp, ToPayload(addr));
  }

  addr = base;
  move32(Imm32(v.toNunboxTag()), temp);
  for (unsigned i = start; i < end; ++i, addr.offset += sizeof(GCPtr<Value>)) {
    store32(temp, ToType(addr));
  }
#else
  moveValue(v, ValueOperand(temp));
  for (uint32_t i = start; i < end; ++i, base.offset += sizeof(GCPtr<Value>)) {
    storePtr(temp, base);
  }
#endif
}

void MacroAssembler::fillSlotsWithUndefined(Address base, Register temp,
                                            uint32_t start, uint32_t end) {
  fillSlotsWithConstantValue(base, temp, start, end, UndefinedValue());
}

void MacroAssembler::fillSlotsWithUninitialized(Address base, Register temp,
                                                uint32_t start, uint32_t end) {
  fillSlotsWithConstantValue(base, temp, start, end,
                             MagicValue(JS_UNINITIALIZED_LEXICAL));
}

static std::pair<uint32_t, uint32_t> FindStartOfUninitializedAndUndefinedSlots(
    const TemplateNativeObject& templateObj, uint32_t nslots) {
  MOZ_ASSERT(nslots == templateObj.slotSpan());
  MOZ_ASSERT(nslots > 0);

  uint32_t first = nslots;
  for (; first != 0; --first) {
    if (templateObj.getSlot(first - 1) != UndefinedValue()) {
      break;
    }
  }
  uint32_t startOfUndefined = first;

  if (first != 0 && IsUninitializedLexical(templateObj.getSlot(first - 1))) {
    for (; first != 0; --first) {
      if (!IsUninitializedLexical(templateObj.getSlot(first - 1))) {
        break;
      }
    }
  }
  uint32_t startOfUninitialized = first;

  return {startOfUninitialized, startOfUndefined};
}

void MacroAssembler::initTypedArraySlots(Register obj, Register temp,
                                         Register lengthReg,
                                         LiveRegisterSet liveRegs, Label* fail,
                                         TypedArrayObject* templateObj,
                                         TypedArrayLength lengthKind) {
  MOZ_ASSERT(!templateObj->hasBuffer());

  constexpr size_t dataSlotOffset = ArrayBufferViewObject::dataOffset();
  constexpr size_t dataOffset = dataSlotOffset + sizeof(HeapSlot);

  static_assert(
      TypedArrayObject::FIXED_DATA_START == TypedArrayObject::DATA_SLOT + 1,
      "fixed inline element data assumed to begin after the data slot");

  static_assert(
      TypedArrayObject::INLINE_BUFFER_LIMIT ==
          JSObject::MAX_BYTE_SIZE - dataOffset,
      "typed array inline buffer is limited by the maximum object byte size");

  // Initialise data elements to zero.
  size_t length = templateObj->length();
  MOZ_ASSERT(length <= INT32_MAX,
             "Template objects are only created for int32 lengths");
  size_t nbytes = length * templateObj->bytesPerElement();

  if (lengthKind == TypedArrayLength::Fixed &&
      nbytes <= TypedArrayObject::INLINE_BUFFER_LIMIT) {
    MOZ_ASSERT(dataOffset + nbytes <= templateObj->tenuredSizeOfThis());

    // Store data elements inside the remaining JSObject slots.
    computeEffectiveAddress(Address(obj, dataOffset), temp);
    storePrivateValue(temp, Address(obj, dataSlotOffset));

    // Write enough zero pointers into fixed data to zero every
    // element.  (This zeroes past the end of a byte count that's
    // not a multiple of pointer size.  That's okay, because fixed
    // data is a count of 8-byte HeapSlots (i.e. <= pointer size),
    // and we won't inline unless the desired memory fits in that
    // space.)
    static_assert(sizeof(HeapSlot) == 8, "Assumed 8 bytes alignment");

    size_t numZeroPointers = ((nbytes + 7) & ~0x7) / sizeof(char*);
    for (size_t i = 0; i < numZeroPointers; i++) {
      storePtr(ImmWord(0), Address(obj, dataOffset + i * sizeof(char*)));
    }
    MOZ_ASSERT(nbytes > 0, "Zero-length TypedArrays need ZeroLengthArrayData");
  } else {
    if (lengthKind == TypedArrayLength::Fixed) {
      move32(Imm32(length), lengthReg);
    }

    // Ensure volatile |obj| is saved across the call.
    if (obj.volatile_()) {
      liveRegs.addUnchecked(obj);
    }

    // Allocate a buffer on the heap to store the data elements.
    PushRegsInMask(liveRegs);
    using Fn = void (*)(JSContext* cx, TypedArrayObject* obj, int32_t count);
    setupUnalignedABICall(temp);
    loadJSContext(temp);
    passABIArg(temp);
    passABIArg(obj);
    passABIArg(lengthReg);
    callWithABI<Fn, AllocateAndInitTypedArrayBuffer>();
    PopRegsInMask(liveRegs);

    // Fail when data slot is UndefinedValue.
    branchTestUndefined(Assembler::Equal, Address(obj, dataSlotOffset), fail);
  }
}

void MacroAssembler::initGCSlots(Register obj, Register temp,
                                 const TemplateNativeObject& templateObj) {
  MOZ_ASSERT(!templateObj.isArrayObject());

  // Slots of non-array objects are required to be initialized.
  // Use the values currently in the template object.
  uint32_t nslots = templateObj.slotSpan();
  if (nslots == 0) {
    return;
  }

  uint32_t nfixed = templateObj.numUsedFixedSlots();
  uint32_t ndynamic = templateObj.numDynamicSlots();

  // Attempt to group slot writes such that we minimize the amount of
  // duplicated data we need to embed in code and load into registers. In
  // general, most template object slots will be undefined except for any
  // reserved slots. Since reserved slots come first, we split the object
  // logically into independent non-UndefinedValue writes to the head and
  // duplicated writes of UndefinedValue to the tail. For the majority of
  // objects, the "tail" will be the entire slot range.
  //
  // The template object may be a CallObject, in which case we need to
  // account for uninitialized lexical slots as well as undefined
  // slots. Uninitialized lexical slots appears in CallObjects if the function
  // has parameter expressions, in which case closed over parameters have
  // TDZ. Uninitialized slots come before undefined slots in CallObjects.
  auto [startOfUninitialized, startOfUndefined] =
      FindStartOfUninitializedAndUndefinedSlots(templateObj, nslots);
  MOZ_ASSERT(startOfUninitialized <= nfixed);  // Reserved slots must be fixed.
  MOZ_ASSERT(startOfUndefined >= startOfUninitialized);
  MOZ_ASSERT_IF(!templateObj.isCallObject() &&
                    !templateObj.isBlockLexicalEnvironmentObject(),
                startOfUninitialized == startOfUndefined);

  // Copy over any preserved reserved slots.
  copySlotsFromTemplate(obj, templateObj, 0, startOfUninitialized);

  // Fill the rest of the fixed slots with undefined and uninitialized.
  size_t offset = NativeObject::getFixedSlotOffset(startOfUninitialized);
  fillSlotsWithUninitialized(Address(obj, offset), temp, startOfUninitialized,
                             std::min(startOfUndefined, nfixed));

  if (startOfUndefined < nfixed) {
    offset = NativeObject::getFixedSlotOffset(startOfUndefined);
    fillSlotsWithUndefined(Address(obj, offset), temp, startOfUndefined,
                           nfixed);
  }

  if (ndynamic) {
    // We are short one register to do this elegantly. Borrow the obj
    // register briefly for our slots base address.
    push(obj);
    loadPtr(Address(obj, NativeObject::offsetOfSlots()), obj);

    // Fill uninitialized slots if necessary. Otherwise initialize all
    // slots to undefined.
    if (startOfUndefined > nfixed) {
      MOZ_ASSERT(startOfUninitialized != startOfUndefined);
      fillSlotsWithUninitialized(Address(obj, 0), temp, 0,
                                 startOfUndefined - nfixed);
      size_t offset = (startOfUndefined - nfixed) * sizeof(Value);
      fillSlotsWithUndefined(Address(obj, offset), temp,
                             startOfUndefined - nfixed, ndynamic);
    } else {
      fillSlotsWithUndefined(Address(obj, 0), temp, 0, ndynamic);
    }

    pop(obj);
  }
}

void MacroAssembler::initGCThing(Register obj, Register temp,
                                 const TemplateObject& templateObj,
                                 bool initContents) {
  // Fast initialization of an empty object returned by allocateObject().

  storePtr(ImmGCPtr(templateObj.shape()),
           Address(obj, JSObject::offsetOfShape()));

  if (templateObj.isNativeObject()) {
    const TemplateNativeObject& ntemplate =
        templateObj.asTemplateNativeObject();
    MOZ_ASSERT(!ntemplate.hasDynamicElements());

    // If the object has dynamic slots, the slots member has already been
    // filled in.
    if (ntemplate.numDynamicSlots() == 0) {
      storePtr(ImmPtr(emptyObjectSlots),
               Address(obj, NativeObject::offsetOfSlots()));
    }

    if (ntemplate.isArrayObject()) {
      // Can't skip initializing reserved slots.
      MOZ_ASSERT(initContents);

      int elementsOffset = NativeObject::offsetOfFixedElements();

      computeEffectiveAddress(Address(obj, elementsOffset), temp);
      storePtr(temp, Address(obj, NativeObject::offsetOfElements()));

      // Fill in the elements header.
      store32(
          Imm32(ntemplate.getDenseCapacity()),
          Address(obj, elementsOffset + ObjectElements::offsetOfCapacity()));
      store32(Imm32(ntemplate.getDenseInitializedLength()),
              Address(obj, elementsOffset +
                               ObjectElements::offsetOfInitializedLength()));
      store32(Imm32(ntemplate.getArrayLength()),
              Address(obj, elementsOffset + ObjectElements::offsetOfLength()));
      store32(Imm32(ObjectElements::FIXED),
              Address(obj, elementsOffset + ObjectElements::offsetOfFlags()));
    } else if (ntemplate.isArgumentsObject()) {
      // The caller will initialize the reserved slots.
      MOZ_ASSERT(!initContents);
      storePtr(ImmPtr(emptyObjectElements),
               Address(obj, NativeObject::offsetOfElements()));
    } else {
      // If the target type could be a TypedArray that maps shared memory
      // then this would need to store emptyObjectElementsShared in that case.
      MOZ_ASSERT(!ntemplate.isSharedMemory());

      // Can't skip initializing reserved slots.
      MOZ_ASSERT(initContents);

      storePtr(ImmPtr(emptyObjectElements),
               Address(obj, NativeObject::offsetOfElements()));

      initGCSlots(obj, temp, ntemplate);
    }
  } else {
    MOZ_CRASH("Unknown object");
  }

#ifdef JS_GC_PROBES
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);

  regs.takeUnchecked(obj);
  Register temp2 = regs.takeAnyGeneral();

  using Fn = void (*)(JSObject* obj);
  setupUnalignedABICall(temp2);
  passABIArg(obj);
  callWithABI<Fn, TraceCreateObject>();

  PopRegsInMask(save);
#endif
}

void MacroAssembler::compareStrings(JSOp op, Register left, Register right,
                                    Register result, Label* fail) {
  MOZ_ASSERT(left != result);
  MOZ_ASSERT(right != result);
  MOZ_ASSERT(IsEqualityOp(op) || IsRelationalOp(op));

  Label notPointerEqual;
  // If operands point to the same instance, the strings are trivially equal.
  branchPtr(Assembler::NotEqual, left, right,
            IsEqualityOp(op) ? &notPointerEqual : fail);
  move32(Imm32(op == JSOp::Eq || op == JSOp::StrictEq || op == JSOp::Le ||
               op == JSOp::Ge),
         result);

  if (IsEqualityOp(op)) {
    Label done;
    jump(&done);

    bind(&notPointerEqual);

    Label leftIsNotAtom;
    Label setNotEqualResult;
    // Atoms cannot be equal to each other if they point to different strings.
    Imm32 atomBit(JSString::ATOM_BIT);
    branchTest32(Assembler::Zero, Address(left, JSString::offsetOfFlags()),
                 atomBit, &leftIsNotAtom);
    branchTest32(Assembler::NonZero, Address(right, JSString::offsetOfFlags()),
                 atomBit, &setNotEqualResult);

    bind(&leftIsNotAtom);
    // Strings of different length can never be equal.
    loadStringLength(left, result);
    branch32(Assembler::Equal, Address(right, JSString::offsetOfLength()),
             result, fail);

    bind(&setNotEqualResult);
    move32(Imm32(op == JSOp::Ne || op == JSOp::StrictNe), result);

    bind(&done);
  }
}

void MacroAssembler::loadStringChars(Register str, Register dest,
                                     CharEncoding encoding) {
  MOZ_ASSERT(str != dest);

  if (JitOptions.spectreStringMitigations) {
    if (encoding == CharEncoding::Latin1) {
      // If the string is a rope, zero the |str| register. The code below
      // depends on str->flags so this should block speculative execution.
      movePtr(ImmWord(0), dest);
      test32MovePtr(Assembler::Zero, Address(str, JSString::offsetOfFlags()),
                    Imm32(JSString::LINEAR_BIT), dest, str);
    } else {
      // If we're loading TwoByte chars, there's an additional risk:
      // if the string has Latin1 chars, we could read out-of-bounds. To
      // prevent this, we check both the Linear and Latin1 bits. We don't
      // have a scratch register, so we use these flags also to block
      // speculative execution, similar to the use of 0 above.
      MOZ_ASSERT(encoding == CharEncoding::TwoByte);
      static constexpr uint32_t Mask =
          JSString::LINEAR_BIT | JSString::LATIN1_CHARS_BIT;
      static_assert(Mask < 1024,
                    "Mask should be a small, near-null value to ensure we "
                    "block speculative execution when it's used as string "
                    "pointer");
      move32(Imm32(Mask), dest);
      and32(Address(str, JSString::offsetOfFlags()), dest);
      cmp32MovePtr(Assembler::NotEqual, dest, Imm32(JSString::LINEAR_BIT), dest,
                   str);
    }
  }

  // Load the inline chars.
  computeEffectiveAddress(Address(str, JSInlineString::offsetOfInlineStorage()),
                          dest);

  // If it's not an inline string, load the non-inline chars. Use a
  // conditional move to prevent speculative execution.
  test32LoadPtr(Assembler::Zero, Address(str, JSString::offsetOfFlags()),
                Imm32(JSString::INLINE_CHARS_BIT),
                Address(str, JSString::offsetOfNonInlineChars()), dest);
}

void MacroAssembler::loadNonInlineStringChars(Register str, Register dest,
                                              CharEncoding encoding) {
  MOZ_ASSERT(str != dest);

  if (JitOptions.spectreStringMitigations) {
    // If the string is a rope, has inline chars, or has a different
    // character encoding, set str to a near-null value to prevent
    // speculative execution below (when reading str->nonInlineChars).

    static constexpr uint32_t Mask = JSString::LINEAR_BIT |
                                     JSString::INLINE_CHARS_BIT |
                                     JSString::LATIN1_CHARS_BIT;
    static_assert(Mask < 1024,
                  "Mask should be a small, near-null value to ensure we "
                  "block speculative execution when it's used as string "
                  "pointer");

    uint32_t expectedBits = JSString::LINEAR_BIT;
    if (encoding == CharEncoding::Latin1) {
      expectedBits |= JSString::LATIN1_CHARS_BIT;
    }

    move32(Imm32(Mask), dest);
    and32(Address(str, JSString::offsetOfFlags()), dest);

    cmp32MovePtr(Assembler::NotEqual, dest, Imm32(expectedBits), dest, str);
  }

  loadPtr(Address(str, JSString::offsetOfNonInlineChars()), dest);
}

void MacroAssembler::storeNonInlineStringChars(Register chars, Register str) {
  MOZ_ASSERT(chars != str);
  storePtr(chars, Address(str, JSString::offsetOfNonInlineChars()));
}

void MacroAssembler::loadInlineStringCharsForStore(Register str,
                                                   Register dest) {
  computeEffectiveAddress(Address(str, JSInlineString::offsetOfInlineStorage()),
                          dest);
}

void MacroAssembler::loadInlineStringChars(Register str, Register dest,
                                           CharEncoding encoding) {
  MOZ_ASSERT(str != dest);

  if (JitOptions.spectreStringMitigations) {
    // Making this Spectre-safe is a bit complicated: using
    // computeEffectiveAddress and then zeroing the output register if
    // non-inline is not sufficient: when the index is very large, it would
    // allow reading |nullptr + index|. Just fall back to loadStringChars
    // for now.
    loadStringChars(str, dest, encoding);
  } else {
    computeEffectiveAddress(
        Address(str, JSInlineString::offsetOfInlineStorage()), dest);
  }
}

void MacroAssembler::loadRopeLeftChild(Register str, Register dest) {
  MOZ_ASSERT(str != dest);

  if (JitOptions.spectreStringMitigations) {
    // Zero the output register if the input was not a rope.
    movePtr(ImmWord(0), dest);
    test32LoadPtr(Assembler::Zero, Address(str, JSString::offsetOfFlags()),
                  Imm32(JSString::LINEAR_BIT),
                  Address(str, JSRope::offsetOfLeft()), dest);
  } else {
    loadPtr(Address(str, JSRope::offsetOfLeft()), dest);
  }
}

void MacroAssembler::loadRopeRightChild(Register str, Register dest) {
  MOZ_ASSERT(str != dest);

  if (JitOptions.spectreStringMitigations) {
    // Zero the output register if the input was not a rope.
    movePtr(ImmWord(0), dest);
    test32LoadPtr(Assembler::Zero, Address(str, JSString::offsetOfFlags()),
                  Imm32(JSString::LINEAR_BIT),
                  Address(str, JSRope::offsetOfRight()), dest);
  } else {
    loadPtr(Address(str, JSRope::offsetOfRight()), dest);
  }
}

void MacroAssembler::storeRopeChildren(Register left, Register right,
                                       Register str) {
  storePtr(left, Address(str, JSRope::offsetOfLeft()));
  storePtr(right, Address(str, JSRope::offsetOfRight()));
}

void MacroAssembler::loadDependentStringBase(Register str, Register dest) {
  MOZ_ASSERT(str != dest);

  if (JitOptions.spectreStringMitigations) {
    // If the string is not a dependent string, zero the |str| register.
    // The code below loads str->base so this should block speculative
    // execution.
    movePtr(ImmWord(0), dest);
    test32MovePtr(Assembler::Zero, Address(str, JSString::offsetOfFlags()),
                  Imm32(JSString::DEPENDENT_BIT), dest, str);
  }

  loadPtr(Address(str, JSDependentString::offsetOfBase()), dest);
}

void MacroAssembler::storeDependentStringBase(Register base, Register str) {
  storePtr(base, Address(str, JSDependentString::offsetOfBase()));
}

void MacroAssembler::loadRopeChild(Register str, Register index,
                                   Register output, Label* isLinear) {
  // This follows JSString::getChar.
  branchIfNotRope(str, isLinear);

  loadRopeLeftChild(str, output);

  // Check if the index is contained in the leftChild.
  Label loadedChild;
  branch32(Assembler::Above, Address(output, JSString::offsetOfLength()), index,
           &loadedChild);

  // The index must be in the rightChild.
  loadRopeRightChild(str, output);

  bind(&loadedChild);
}

void MacroAssembler::branchIfCanLoadStringChar(Register str, Register index,
                                               Register scratch, Label* label) {
  loadRopeChild(str, index, scratch, label);

  // Branch if the left resp. right side is linear.
  branchIfNotRope(scratch, label);
}

void MacroAssembler::branchIfNotCanLoadStringChar(Register str, Register index,
                                                  Register scratch,
                                                  Label* label) {
  Label done;
  loadRopeChild(str, index, scratch, &done);

  // Branch if the left or right side is another rope.
  branchIfRope(scratch, label);

  bind(&done);
}

void MacroAssembler::loadStringChar(Register str, Register index,
                                    Register output, Register scratch1,
                                    Register scratch2, Label* fail) {
  MOZ_ASSERT(str != output);
  MOZ_ASSERT(str != index);
  MOZ_ASSERT(index != output);
  MOZ_ASSERT(output != scratch1);
  MOZ_ASSERT(output != scratch2);

  // Use scratch1 for the index (adjusted below).
  move32(index, scratch1);
  movePtr(str, output);

  // This follows JSString::getChar.
  Label notRope;
  branchIfNotRope(str, &notRope);

  loadRopeLeftChild(str, output);

  // Check if the index is contained in the leftChild.
  Label loadedChild, notInLeft;
  spectreBoundsCheck32(scratch1, Address(output, JSString::offsetOfLength()),
                       scratch2, &notInLeft);
  jump(&loadedChild);

  // The index must be in the rightChild.
  // index -= rope->leftChild()->length()
  bind(&notInLeft);
  sub32(Address(output, JSString::offsetOfLength()), scratch1);
  loadRopeRightChild(str, output);

  // If the left or right side is another rope, give up.
  bind(&loadedChild);
  branchIfRope(output, fail);

  bind(&notRope);

  Label isLatin1, done;
  // We have to check the left/right side for ropes,
  // because a TwoByte rope might have a Latin1 child.
  branchLatin1String(output, &isLatin1);
  loadStringChars(output, scratch2, CharEncoding::TwoByte);
  loadChar(scratch2, scratch1, output, CharEncoding::TwoByte);
  jump(&done);

  bind(&isLatin1);
  loadStringChars(output, scratch2, CharEncoding::Latin1);
  loadChar(scratch2, scratch1, output, CharEncoding::Latin1);

  bind(&done);
}

void MacroAssembler::loadStringIndexValue(Register str, Register dest,
                                          Label* fail) {
  MOZ_ASSERT(str != dest);

  load32(Address(str, JSString::offsetOfFlags()), dest);

  // Does not have a cached index value.
  branchTest32(Assembler::Zero, dest, Imm32(JSString::INDEX_VALUE_BIT), fail);

  // Extract the index.
  rshift32(Imm32(JSString::INDEX_VALUE_SHIFT), dest);
}

void MacroAssembler::loadChar(Register chars, Register index, Register dest,
                              CharEncoding encoding, int32_t offset /* = 0 */) {
  if (encoding == CharEncoding::Latin1) {
    loadChar(BaseIndex(chars, index, TimesOne, offset), dest, encoding);
  } else {
    loadChar(BaseIndex(chars, index, TimesTwo, offset), dest, encoding);
  }
}

void MacroAssembler::addToCharPtr(Register chars, Register index,
                                  CharEncoding encoding) {
  if (encoding == CharEncoding::Latin1) {
    static_assert(sizeof(char) == 1,
                  "Latin-1 string index shouldn't need scaling");
    addPtr(index, chars);
  } else {
    computeEffectiveAddress(BaseIndex(chars, index, TimesTwo), chars);
  }
}

void MacroAssembler::loadStringFromUnit(Register unit, Register dest,
                                        const StaticStrings& staticStrings) {
  movePtr(ImmPtr(&staticStrings.unitStaticTable), dest);
  loadPtr(BaseIndex(dest, unit, ScalePointer), dest);
}

void MacroAssembler::loadLengthTwoString(Register c1, Register c2,
                                         Register dest,
                                         const StaticStrings& staticStrings) {
  // Compute (toSmallCharTable[c1] << SMALL_CHAR_BITS) + toSmallCharTable[c2]
  // to obtain the index into `StaticStrings::length2StaticTable`.
  static_assert(sizeof(StaticStrings::SmallChar) == 1);

  movePtr(ImmPtr(&StaticStrings::toSmallCharTable.storage), dest);
  load8ZeroExtend(BaseIndex(dest, c1, Scale::TimesOne), c1);
  load8ZeroExtend(BaseIndex(dest, c2, Scale::TimesOne), c2);

  lshift32(Imm32(StaticStrings::SMALL_CHAR_BITS), c1);
  add32(c2, c1);

  // Look up the string from the computed index.
  movePtr(ImmPtr(&staticStrings.length2StaticTable), dest);
  loadPtr(BaseIndex(dest, c1, ScalePointer), dest);
}

void MacroAssembler::loadInt32ToStringWithBase(
    Register input, Register base, Register dest, Register scratch1,
    Register scratch2, const StaticStrings& staticStrings,
    const LiveRegisterSet& volatileRegs, Label* fail) {
#ifdef DEBUG
  Label baseBad, baseOk;
  branch32(Assembler::LessThan, base, Imm32(2), &baseBad);
  branch32(Assembler::LessThanOrEqual, base, Imm32(36), &baseOk);
  bind(&baseBad);
  assumeUnreachable("base must be in range [2, 36]");
  bind(&baseOk);
#endif

  // Compute |"0123456789abcdefghijklmnopqrstuvwxyz"[r]|.
  auto toChar = [this, base](Register r) {
#ifdef DEBUG
    Label ok;
    branch32(Assembler::Below, r, base, &ok);
    assumeUnreachable("bad digit");
    bind(&ok);
#else
    // Silence unused lambda capture warning.
    (void)base;
#endif

    Label done;
    add32(Imm32('0'), r);
    branch32(Assembler::BelowOrEqual, r, Imm32('9'), &done);
    add32(Imm32('a' - '0' - 10), r);
    bind(&done);
  };

  // Perform a "unit" lookup when |unsigned(input) < unsigned(base)|.
  Label lengthTwo, done;
  branch32(Assembler::AboveOrEqual, input, base, &lengthTwo);
  {
    move32(input, scratch1);
    toChar(scratch1);

    loadStringFromUnit(scratch1, dest, staticStrings);

    jump(&done);
  }
  bind(&lengthTwo);

  // Compute |base * base|.
  move32(base, scratch1);
  mul32(scratch1, scratch1);

  // Perform a "length2" lookup when |unsigned(input) < unsigned(base * base)|.
  branch32(Assembler::AboveOrEqual, input, scratch1, fail);
  {
    // Compute |scratch1 = input / base| and |scratch2 = input % base|.
    move32(input, scratch1);
    flexibleDivMod32(base, scratch1, scratch2, true, volatileRegs);

    // Compute the digits of the divisor and remainder.
    toChar(scratch1);
    toChar(scratch2);

    // Look up the 2-character digit string in the small-char table.
    loadLengthTwoString(scratch1, scratch2, dest, staticStrings);
  }
  bind(&done);
}

void MacroAssembler::loadInt32ToStringWithBase(
    Register input, int32_t base, Register dest, Register scratch1,
    Register scratch2, const StaticStrings& staticStrings, Label* fail) {
  MOZ_ASSERT(2 <= base && base <= 36, "base must be in range [2, 36]");

  // Compute |"0123456789abcdefghijklmnopqrstuvwxyz"[r]|.
  auto toChar = [this, base](Register r) {
#ifdef DEBUG
    Label ok;
    branch32(Assembler::Below, r, Imm32(base), &ok);
    assumeUnreachable("bad digit");
    bind(&ok);
#endif

    if (base <= 10) {
      add32(Imm32('0'), r);
    } else {
      Label done;
      add32(Imm32('0'), r);
      branch32(Assembler::BelowOrEqual, r, Imm32('9'), &done);
      add32(Imm32('a' - '0' - 10), r);
      bind(&done);
    }
  };

  // Perform a "unit" lookup when |unsigned(input) < unsigned(base)|.
  Label lengthTwo, done;
  branch32(Assembler::AboveOrEqual, input, Imm32(base), &lengthTwo);
  {
    move32(input, scratch1);
    toChar(scratch1);

    loadStringFromUnit(scratch1, dest, staticStrings);

    jump(&done);
  }
  bind(&lengthTwo);

  // Perform a "length2" lookup when |unsigned(input) < unsigned(base * base)|.
  branch32(Assembler::AboveOrEqual, input, Imm32(base * base), fail);
  {
    // Compute |scratch1 = input / base| and |scratch2 = input % base|.
    if (mozilla::IsPowerOfTwo(uint32_t(base))) {
      uint32_t shift = mozilla::FloorLog2(base);

      move32(input, scratch1);
      rshift32(Imm32(shift), scratch1);

      move32(input, scratch2);
      and32(Imm32((uint32_t(1) << shift) - 1), scratch2);
    } else {
      // The following code matches CodeGenerator::visitUDivOrModConstant()
      // for x86-shared. Also see Hacker's Delight 2nd edition, chapter 10-8
      // "Unsigned Division by 7" for the case when |rmc.multiplier| exceeds
      // UINT32_MAX and we need to adjust the shift amount.

      auto rmc = ReciprocalMulConstants::computeUnsignedDivisionConstants(base);

      // We first compute |q = (M * n) >> 32), where M = rmc.multiplier.
      mulHighUnsigned32(Imm32(rmc.multiplier), input, scratch1);

      if (rmc.multiplier > UINT32_MAX) {
        // M >= 2^32 and shift == 0 is impossible, as d >= 2 implies that
        // ((M * n) >> (32 + shift)) >= n > floor(n/d) whenever n >= d,
        // contradicting the proof of correctness in computeDivisionConstants.
        MOZ_ASSERT(rmc.shiftAmount > 0);
        MOZ_ASSERT(rmc.multiplier < (int64_t(1) << 33));

        // Compute |t = (n - q) / 2|.
        move32(input, scratch2);
        sub32(scratch1, scratch2);
        rshift32(Imm32(1), scratch2);

        // Compute |t = (n - q) / 2 + q = (n + q) / 2|.
        add32(scratch2, scratch1);

        // Finish the computation |q = floor(n / d)|.
        rshift32(Imm32(rmc.shiftAmount - 1), scratch1);
      } else {
        rshift32(Imm32(rmc.shiftAmount), scratch1);
      }

      // Compute the remainder from |r = n - q * d|.
      move32(scratch1, dest);
      mul32(Imm32(base), dest);
      move32(input, scratch2);
      sub32(dest, scratch2);
    }

    // Compute the digits of the divisor and remainder.
    toChar(scratch1);
    toChar(scratch2);

    // Look up the 2-character digit string in the small-char table.
    loadLengthTwoString(scratch1, scratch2, dest, staticStrings);
  }
  bind(&done);
}

void MacroAssembler::loadBigIntDigits(Register bigInt, Register digits) {
  MOZ_ASSERT(digits != bigInt);

  // Load the inline digits.
  computeEffectiveAddress(Address(bigInt, BigInt::offsetOfInlineDigits()),
                          digits);

  // If inline digits aren't used, load the heap digits. Use a conditional move
  // to prevent speculative execution.
  cmp32LoadPtr(Assembler::Above, Address(bigInt, BigInt::offsetOfLength()),
               Imm32(int32_t(BigInt::inlineDigitsLength())),
               Address(bigInt, BigInt::offsetOfHeapDigits()), digits);
}

void MacroAssembler::loadBigInt64(Register bigInt, Register64 dest) {
  // This code follows the implementation of |BigInt::toUint64()|. We're also
  // using it for inline callers of |BigInt::toInt64()|, which works, because
  // all supported Jit architectures use a two's complement representation for
  // int64 values, which means the WrapToSigned call in toInt64() is a no-op.

  Label done, nonZero;

  branchIfBigIntIsNonZero(bigInt, &nonZero);
  {
    move64(Imm64(0), dest);
    jump(&done);
  }
  bind(&nonZero);

#ifdef JS_PUNBOX64
  Register digits = dest.reg;
#else
  Register digits = dest.high;
#endif

  loadBigIntDigits(bigInt, digits);

#if JS_PUNBOX64
  // Load the first digit into the destination register.
  load64(Address(digits, 0), dest);
#else
  // Load the first digit into the destination register's low value.
  load32(Address(digits, 0), dest.low);

  // And conditionally load the second digit into the high value register.
  Label twoDigits, digitsDone;
  branch32(Assembler::Above, Address(bigInt, BigInt::offsetOfLength()),
           Imm32(1), &twoDigits);
  {
    move32(Imm32(0), dest.high);
    jump(&digitsDone);
  }
  {
    bind(&twoDigits);
    load32(Address(digits, sizeof(BigInt::Digit)), dest.high);
  }
  bind(&digitsDone);
#endif

  branchTest32(Assembler::Zero, Address(bigInt, BigInt::offsetOfFlags()),
               Imm32(BigInt::signBitMask()), &done);
  neg64(dest);

  bind(&done);
}

void MacroAssembler::loadFirstBigIntDigitOrZero(Register bigInt,
                                                Register dest) {
  Label done, nonZero;
  branchIfBigIntIsNonZero(bigInt, &nonZero);
  {
    movePtr(ImmWord(0), dest);
    jump(&done);
  }
  bind(&nonZero);

  loadBigIntDigits(bigInt, dest);

  // Load the first digit into the destination register.
  loadPtr(Address(dest, 0), dest);

  bind(&done);
}

void MacroAssembler::loadBigInt(Register bigInt, Register dest, Label* fail) {
  Label done, nonZero;
  branchIfBigIntIsNonZero(bigInt, &nonZero);
  {
    movePtr(ImmWord(0), dest);
    jump(&done);
  }
  bind(&nonZero);

  loadBigIntNonZero(bigInt, dest, fail);

  bind(&done);
}

void MacroAssembler::loadBigIntNonZero(Register bigInt, Register dest,
                                       Label* fail) {
  MOZ_ASSERT(bigInt != dest);

#ifdef DEBUG
  Label nonZero;
  branchIfBigIntIsNonZero(bigInt, &nonZero);
  assumeUnreachable("Unexpected zero BigInt");
  bind(&nonZero);
#endif

  branch32(Assembler::Above, Address(bigInt, BigInt::offsetOfLength()),
           Imm32(1), fail);

  static_assert(BigInt::inlineDigitsLength() > 0,
                "Single digit BigInts use inline storage");

  // Load the first inline digit into the destination register.
  loadPtr(Address(bigInt, BigInt::offsetOfInlineDigits()), dest);

  // Return as a signed pointer.
  bigIntDigitToSignedPtr(bigInt, dest, fail);
}

void MacroAssembler::bigIntDigitToSignedPtr(Register bigInt, Register digit,
                                            Label* fail) {
  // BigInt digits are stored as absolute numbers. Take the failure path when
  // the digit can't be stored in intptr_t.
  branchTestPtr(Assembler::Signed, digit, digit, fail);

  // Negate |dest| when the BigInt is negative.
  Label nonNegative;
  branchIfBigIntIsNonNegative(bigInt, &nonNegative);
  negPtr(digit);
  bind(&nonNegative);
}

void MacroAssembler::loadBigIntAbsolute(Register bigInt, Register dest,
                                        Label* fail) {
  MOZ_ASSERT(bigInt != dest);

  branch32(Assembler::Above, Address(bigInt, BigInt::offsetOfLength()),
           Imm32(1), fail);

  static_assert(BigInt::inlineDigitsLength() > 0,
                "Single digit BigInts use inline storage");

  // Load the first inline digit into the destination register.
  movePtr(ImmWord(0), dest);
  cmp32LoadPtr(Assembler::NotEqual, Address(bigInt, BigInt::offsetOfLength()),
               Imm32(0), Address(bigInt, BigInt::offsetOfInlineDigits()), dest);
}

void MacroAssembler::initializeBigInt64(Scalar::Type type, Register bigInt,
                                        Register64 val) {
  MOZ_ASSERT(Scalar::isBigIntType(type));

  store32(Imm32(0), Address(bigInt, BigInt::offsetOfFlags()));

  Label done, nonZero;
  branch64(Assembler::NotEqual, val, Imm64(0), &nonZero);
  {
    store32(Imm32(0), Address(bigInt, BigInt::offsetOfLength()));
    jump(&done);
  }
  bind(&nonZero);

  if (type == Scalar::BigInt64) {
    // Set the sign-bit for negative values and then continue with the two's
    // complement.
    Label isPositive;
    branch64(Assembler::GreaterThan, val, Imm64(0), &isPositive);
    {
      store32(Imm32(BigInt::signBitMask()),
              Address(bigInt, BigInt::offsetOfFlags()));
      neg64(val);
    }
    bind(&isPositive);
  }

  store32(Imm32(1), Address(bigInt, BigInt::offsetOfLength()));

  static_assert(sizeof(BigInt::Digit) == sizeof(uintptr_t),
                "BigInt Digit size matches uintptr_t, so there's a single "
                "store on 64-bit and up to two stores on 32-bit");

#ifndef JS_PUNBOX64
  Label singleDigit;
  branchTest32(Assembler::Zero, val.high, val.high, &singleDigit);
  store32(Imm32(2), Address(bigInt, BigInt::offsetOfLength()));
  bind(&singleDigit);

  // We can perform a single store64 on 32-bit platforms, because inline
  // storage can store at least two 32-bit integers.
  static_assert(BigInt::inlineDigitsLength() >= 2,
                "BigInt inline storage can store at least two digits");
#endif

  store64(val, Address(bigInt, js::BigInt::offsetOfInlineDigits()));

  bind(&done);
}

void MacroAssembler::initializeBigInt(Register bigInt, Register val) {
  store32(Imm32(0), Address(bigInt, BigInt::offsetOfFlags()));

  Label done, nonZero;
  branchTestPtr(Assembler::NonZero, val, val, &nonZero);
  {
    store32(Imm32(0), Address(bigInt, BigInt::offsetOfLength()));
    jump(&done);
  }
  bind(&nonZero);

  // Set the sign-bit for negative values and then continue with the two's
  // complement.
  Label isPositive;
  branchTestPtr(Assembler::NotSigned, val, val, &isPositive);
  {
    store32(Imm32(BigInt::signBitMask()),
            Address(bigInt, BigInt::offsetOfFlags()));
    negPtr(val);
  }
  bind(&isPositive);

  store32(Imm32(1), Address(bigInt, BigInt::offsetOfLength()));

  static_assert(sizeof(BigInt::Digit) == sizeof(uintptr_t),
                "BigInt Digit size matches uintptr_t");

  storePtr(val, Address(bigInt, js::BigInt::offsetOfInlineDigits()));

  bind(&done);
}

void MacroAssembler::initializeBigIntAbsolute(Register bigInt, Register val) {
  store32(Imm32(0), Address(bigInt, BigInt::offsetOfFlags()));

  Label done, nonZero;
  branchTestPtr(Assembler::NonZero, val, val, &nonZero);
  {
    store32(Imm32(0), Address(bigInt, BigInt::offsetOfLength()));
    jump(&done);
  }
  bind(&nonZero);

  store32(Imm32(1), Address(bigInt, BigInt::offsetOfLength()));

  static_assert(sizeof(BigInt::Digit) == sizeof(uintptr_t),
                "BigInt Digit size matches uintptr_t");

  storePtr(val, Address(bigInt, js::BigInt::offsetOfInlineDigits()));

  bind(&done);
}

void MacroAssembler::copyBigIntWithInlineDigits(Register src, Register dest,
                                                Register temp,
                                                gc::Heap initialHeap,
                                                Label* fail) {
  branch32(Assembler::Above, Address(src, BigInt::offsetOfLength()),
           Imm32(int32_t(BigInt::inlineDigitsLength())), fail);

  newGCBigInt(dest, temp, initialHeap, fail);

  // Copy the sign-bit, but not any of the other bits used by the GC.
  load32(Address(src, BigInt::offsetOfFlags()), temp);
  and32(Imm32(BigInt::signBitMask()), temp);
  store32(temp, Address(dest, BigInt::offsetOfFlags()));

  // Copy the length.
  load32(Address(src, BigInt::offsetOfLength()), temp);
  store32(temp, Address(dest, BigInt::offsetOfLength()));

  // Copy the digits.
  Address srcDigits(src, js::BigInt::offsetOfInlineDigits());
  Address destDigits(dest, js::BigInt::offsetOfInlineDigits());

  for (size_t i = 0; i < BigInt::inlineDigitsLength(); i++) {
    static_assert(sizeof(BigInt::Digit) == sizeof(uintptr_t),
                  "BigInt Digit size matches uintptr_t");

    loadPtr(srcDigits, temp);
    storePtr(temp, destDigits);

    srcDigits = Address(src, srcDigits.offset + sizeof(BigInt::Digit));
    destDigits = Address(dest, destDigits.offset + sizeof(BigInt::Digit));
  }
}

void MacroAssembler::compareBigIntAndInt32(JSOp op, Register bigInt,
                                           Register int32, Register scratch1,
                                           Register scratch2, Label* ifTrue,
                                           Label* ifFalse) {
  MOZ_ASSERT(IsLooseEqualityOp(op) || IsRelationalOp(op));

  static_assert(std::is_same_v<BigInt::Digit, uintptr_t>,
                "BigInt digit can be loaded in a pointer-sized register");
  static_assert(sizeof(BigInt::Digit) >= sizeof(uint32_t),
                "BigInt digit stores at least an uint32");

  // Test for too large numbers.
  //
  // If the absolute value of the BigInt can't be expressed in an uint32/uint64,
  // the result of the comparison is a constant.
  if (op == JSOp::Eq || op == JSOp::Ne) {
    Label* tooLarge = op == JSOp::Eq ? ifFalse : ifTrue;
    branch32(Assembler::GreaterThan,
             Address(bigInt, BigInt::offsetOfDigitLength()), Imm32(1),
             tooLarge);
  } else {
    Label doCompare;
    branch32(Assembler::LessThanOrEqual,
             Address(bigInt, BigInt::offsetOfDigitLength()), Imm32(1),
             &doCompare);

    // Still need to take the sign-bit into account for relational operations.
    if (op == JSOp::Lt || op == JSOp::Le) {
      branchIfBigIntIsNegative(bigInt, ifTrue);
      jump(ifFalse);
    } else {
      branchIfBigIntIsNegative(bigInt, ifFalse);
      jump(ifTrue);
    }

    bind(&doCompare);
  }

  // Test for mismatched signs and, if the signs are equal, load |abs(x)| in
  // |scratch1| and |abs(y)| in |scratch2| and then compare the absolute numbers
  // against each other.
  {
    // Jump to |ifTrue| resp. |ifFalse| if the BigInt is strictly less than
    // resp. strictly greater than the int32 value, depending on the comparison
    // operator.
    Label* greaterThan;
    Label* lessThan;
    if (op == JSOp::Eq) {
      greaterThan = ifFalse;
      lessThan = ifFalse;
    } else if (op == JSOp::Ne) {
      greaterThan = ifTrue;
      lessThan = ifTrue;
    } else if (op == JSOp::Lt || op == JSOp::Le) {
      greaterThan = ifFalse;
      lessThan = ifTrue;
    } else {
      MOZ_ASSERT(op == JSOp::Gt || op == JSOp::Ge);
      greaterThan = ifTrue;
      lessThan = ifFalse;
    }

    // BigInt digits are always stored as an absolute number.
    loadFirstBigIntDigitOrZero(bigInt, scratch1);

    // Load the int32 into |scratch2| and negate it for negative numbers.
    move32(int32, scratch2);

    Label isNegative, doCompare;
    branchIfBigIntIsNegative(bigInt, &isNegative);
    branch32(Assembler::LessThan, int32, Imm32(0), greaterThan);
    jump(&doCompare);

    // We rely on |neg32(INT32_MIN)| staying INT32_MIN, because we're using an
    // unsigned comparison below.
    bind(&isNegative);
    branch32(Assembler::GreaterThanOrEqual, int32, Imm32(0), lessThan);
    neg32(scratch2);

    // Not all supported platforms (e.g. MIPS64) zero-extend 32-bit operations,
    // so we need to explicitly clear any high 32-bits.
    move32ZeroExtendToPtr(scratch2, scratch2);

    // Reverse the relational comparator for negative numbers.
    // |-x < -y| <=> |+x > +y|.
    // |-x ≤ -y| <=> |+x ≥ +y|.
    // |-x > -y| <=> |+x < +y|.
    // |-x ≥ -y| <=> |+x ≤ +y|.
    JSOp reversed = ReverseCompareOp(op);
    if (reversed != op) {
      branchPtr(JSOpToCondition(reversed, /* isSigned = */ false), scratch1,
                scratch2, ifTrue);
      jump(ifFalse);
    }

    bind(&doCompare);
    branchPtr(JSOpToCondition(op, /* isSigned = */ false), scratch1, scratch2,
              ifTrue);
  }
}

void MacroAssembler::equalBigInts(Register left, Register right, Register temp1,
                                  Register temp2, Register temp3,
                                  Register temp4, Label* notSameSign,
                                  Label* notSameLength, Label* notSameDigit) {
  MOZ_ASSERT(left != temp1);
  MOZ_ASSERT(right != temp1);
  MOZ_ASSERT(right != temp2);

  // Jump to |notSameSign| when the sign aren't the same.
  load32(Address(left, BigInt::offsetOfFlags()), temp1);
  xor32(Address(right, BigInt::offsetOfFlags()), temp1);
  branchTest32(Assembler::NonZero, temp1, Imm32(BigInt::signBitMask()),
               notSameSign);

  // Jump to |notSameLength| when the digits length is different.
  load32(Address(right, BigInt::offsetOfLength()), temp1);
  branch32(Assembler::NotEqual, Address(left, BigInt::offsetOfLength()), temp1,
           notSameLength);

  // Both BigInts have the same sign and the same number of digits. Loop
  // over each digit, starting with the left-most one, and break from the
  // loop when the first non-matching digit was found.

  loadBigIntDigits(left, temp2);
  loadBigIntDigits(right, temp3);

  static_assert(sizeof(BigInt::Digit) == sizeof(void*),
                "BigInt::Digit is pointer sized");

  computeEffectiveAddress(BaseIndex(temp2, temp1, ScalePointer), temp2);
  computeEffectiveAddress(BaseIndex(temp3, temp1, ScalePointer), temp3);

  Label start, loop;
  jump(&start);
  bind(&loop);

  subPtr(Imm32(sizeof(BigInt::Digit)), temp2);
  subPtr(Imm32(sizeof(BigInt::Digit)), temp3);

  loadPtr(Address(temp3, 0), temp4);
  branchPtr(Assembler::NotEqual, Address(temp2, 0), temp4, notSameDigit);

  bind(&start);
  branchSub32(Assembler::NotSigned, Imm32(1), temp1, &loop);

  // No different digits were found, both BigInts are equal to each other.
}

void MacroAssembler::typeOfObject(Register obj, Register scratch, Label* slow,
                                  Label* isObject, Label* isCallable,
                                  Label* isUndefined) {
  loadObjClassUnsafe(obj, scratch);

  // Proxies can emulate undefined and have complex isCallable behavior.
  branchTestClassIsProxy(true, scratch, slow);

  // JSFunctions are always callable.
  branchTestClassIsFunction(Assembler::Equal, scratch, isCallable);

  // Objects that emulate undefined.
  Address flags(scratch, JSClass::offsetOfFlags());
  branchTest32(Assembler::NonZero, flags, Imm32(JSCLASS_EMULATES_UNDEFINED),
               isUndefined);

  // Handle classes with a call hook.
  branchPtr(Assembler::Equal, Address(scratch, offsetof(JSClass, cOps)),
            ImmPtr(nullptr), isObject);

  loadPtr(Address(scratch, offsetof(JSClass, cOps)), scratch);
  branchPtr(Assembler::Equal, Address(scratch, offsetof(JSClassOps, call)),
            ImmPtr(nullptr), isObject);

  jump(isCallable);
}

void MacroAssembler::isCallableOrConstructor(bool isCallable, Register obj,
                                             Register output, Label* isProxy) {
  MOZ_ASSERT(obj != output);

  Label notFunction, hasCOps, done;
  loadObjClassUnsafe(obj, output);

  // An object is callable iff:
  //   is<JSFunction>() || (getClass()->cOps && getClass()->cOps->call).
  // An object is constructor iff:
  //  ((is<JSFunction>() && as<JSFunction>().isConstructor) ||
  //   (getClass()->cOps && getClass()->cOps->construct)).
  branchTestClassIsFunction(Assembler::NotEqual, output, &notFunction);
  if (isCallable) {
    move32(Imm32(1), output);
  } else {
    static_assert(mozilla::IsPowerOfTwo(uint32_t(FunctionFlags::CONSTRUCTOR)),
                  "FunctionFlags::CONSTRUCTOR has only one bit set");

    load32(Address(obj, JSFunction::offsetOfFlagsAndArgCount()), output);
    rshift32(Imm32(mozilla::FloorLog2(uint32_t(FunctionFlags::CONSTRUCTOR))),
             output);
    and32(Imm32(1), output);
  }
  jump(&done);

  bind(&notFunction);

  if (!isCallable) {
    // For bound functions, we need to check the isConstructor flag.
    Label notBoundFunction;
    branchPtr(Assembler::NotEqual, output, ImmPtr(&BoundFunctionObject::class_),
              &notBoundFunction);

    static_assert(BoundFunctionObject::IsConstructorFlag == 0b1,
                  "AND operation results in boolean value");
    unboxInt32(Address(obj, BoundFunctionObject::offsetOfFlagsSlot()), output);
    and32(Imm32(BoundFunctionObject::IsConstructorFlag), output);
    jump(&done);

    bind(&notBoundFunction);
  }

  // Just skim proxies off. Their notion of isCallable()/isConstructor() is
  // more complicated.
  branchTestClassIsProxy(true, output, isProxy);

  branchPtr(Assembler::NonZero, Address(output, offsetof(JSClass, cOps)),
            ImmPtr(nullptr), &hasCOps);
  move32(Imm32(0), output);
  jump(&done);

  bind(&hasCOps);
  loadPtr(Address(output, offsetof(JSClass, cOps)), output);
  size_t opsOffset =
      isCallable ? offsetof(JSClassOps, call) : offsetof(JSClassOps, construct);
  cmpPtrSet(Assembler::NonZero, Address(output, opsOffset), ImmPtr(nullptr),
            output);

  bind(&done);
}

void MacroAssembler::loadJSContext(Register dest) {
  movePtr(ImmPtr(runtime()->mainContextPtr()), dest);
}

static const uint8_t* ContextRealmPtr(CompileRuntime* rt) {
  return (static_cast<const uint8_t*>(rt->mainContextPtr()) +
          JSContext::offsetOfRealm());
}

void MacroAssembler::switchToRealm(Register realm) {
  storePtr(realm, AbsoluteAddress(ContextRealmPtr(runtime())));
}

void MacroAssembler::switchToRealm(const void* realm, Register scratch) {
  MOZ_ASSERT(realm);

  movePtr(ImmPtr(realm), scratch);
  switchToRealm(scratch);
}

void MacroAssembler::switchToObjectRealm(Register obj, Register scratch) {
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfRealm()), scratch);
  switchToRealm(scratch);
}

void MacroAssembler::switchToBaselineFrameRealm(Register scratch) {
  Address envChain(FramePointer,
                   BaselineFrame::reverseOffsetOfEnvironmentChain());
  loadPtr(envChain, scratch);
  switchToObjectRealm(scratch, scratch);
}

void MacroAssembler::switchToWasmInstanceRealm(Register scratch1,
                                               Register scratch2) {
  loadPtr(Address(InstanceReg, wasm::Instance::offsetOfCx()), scratch1);
  loadPtr(Address(InstanceReg, wasm::Instance::offsetOfRealm()), scratch2);
  storePtr(scratch2, Address(scratch1, JSContext::offsetOfRealm()));
}

void MacroAssembler::debugAssertContextRealm(const void* realm,
                                             Register scratch) {
#ifdef DEBUG
  Label ok;
  movePtr(ImmPtr(realm), scratch);
  branchPtr(Assembler::Equal, AbsoluteAddress(ContextRealmPtr(runtime())),
            scratch, &ok);
  assumeUnreachable("Unexpected context realm");
  bind(&ok);
#endif
}

void MacroAssembler::setIsCrossRealmArrayConstructor(Register obj,
                                                     Register output) {
#ifdef DEBUG
  Label notProxy;
  branchTestObjectIsProxy(false, obj, output, &notProxy);
  assumeUnreachable("Unexpected proxy in setIsCrossRealmArrayConstructor");
  bind(&notProxy);
#endif

  // The object's realm must not be cx->realm.
  Label isFalse, done;
  loadPtr(Address(obj, JSObject::offsetOfShape()), output);
  loadPtr(Address(output, Shape::offsetOfBaseShape()), output);
  loadPtr(Address(output, BaseShape::offsetOfRealm()), output);
  branchPtr(Assembler::Equal, AbsoluteAddress(ContextRealmPtr(runtime())),
            output, &isFalse);

  // The object must be a function.
  branchTestObjIsFunction(Assembler::NotEqual, obj, output, obj, &isFalse);

  // The function must be the ArrayConstructor native.
  branchPtr(Assembler::NotEqual,
            Address(obj, JSFunction::offsetOfNativeOrEnv()),
            ImmPtr(js::ArrayConstructor), &isFalse);

  move32(Imm32(1), output);
  jump(&done);

  bind(&isFalse);
  move32(Imm32(0), output);

  bind(&done);
}

void MacroAssembler::setIsDefinitelyTypedArrayConstructor(Register obj,
                                                          Register output) {
  Label isFalse, isTrue, done;

  // The object must be a function. (Wrappers are not supported.)
  branchTestObjIsFunction(Assembler::NotEqual, obj, output, obj, &isFalse);

  // Load the native into |output|.
  loadPtr(Address(obj, JSFunction::offsetOfNativeOrEnv()), output);

  auto branchIsTypedArrayCtor = [&](Scalar::Type type) {
    // The function must be a TypedArrayConstructor native (from any realm).
    JSNative constructor = TypedArrayConstructorNative(type);
    branchPtr(Assembler::Equal, output, ImmPtr(constructor), &isTrue);
  };

#define TYPED_ARRAY_CONSTRUCTOR_NATIVE(_, T, N) \
  branchIsTypedArrayCtor(Scalar::N);
  JS_FOR_EACH_TYPED_ARRAY(TYPED_ARRAY_CONSTRUCTOR_NATIVE)
#undef TYPED_ARRAY_CONSTRUCTOR_NATIVE

  // Falls through to the false case.

  bind(&isFalse);
  move32(Imm32(0), output);
  jump(&done);

  bind(&isTrue);
  move32(Imm32(1), output);

  bind(&done);
}

void MacroAssembler::loadMegamorphicCache(Register dest) {
  movePtr(ImmPtr(runtime()->addressOfMegamorphicCache()), dest);
}
void MacroAssembler::loadMegamorphicSetPropCache(Register dest) {
  movePtr(ImmPtr(runtime()->addressOfMegamorphicSetPropCache()), dest);
}

void MacroAssembler::loadStringToAtomCacheLastLookups(Register dest) {
  uintptr_t cachePtr = uintptr_t(runtime()->addressOfStringToAtomCache());
  void* offset = (void*)(cachePtr + StringToAtomCache::offsetOfLastLookups());
  movePtr(ImmPtr(offset), dest);
}

void MacroAssembler::loadAtomHash(Register id, Register outHash, Label* done) {
  Label doneInner, fatInline;
  if (!done) {
    done = &doneInner;
  }
  move32(Imm32(JSString::FAT_INLINE_MASK), outHash);
  and32(Address(id, JSString::offsetOfFlags()), outHash);

  branch32(Assembler::Equal, outHash, Imm32(JSString::FAT_INLINE_MASK),
           &fatInline);
  load32(Address(id, NormalAtom::offsetOfHash()), outHash);
  jump(done);
  bind(&fatInline);
  load32(Address(id, FatInlineAtom::offsetOfHash()), outHash);
  jump(done);
  bind(&doneInner);
}

void MacroAssembler::loadAtomOrSymbolAndHash(ValueOperand value, Register outId,
                                             Register outHash,
                                             Label* cacheMiss) {
  Label isString, isSymbol, isNull, isUndefined, done, nonAtom, atom,
      lastLookupAtom;

  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);
    branchTestString(Assembler::Equal, tag, &isString);
    branchTestSymbol(Assembler::Equal, tag, &isSymbol);
    branchTestNull(Assembler::Equal, tag, &isNull);
    branchTestUndefined(Assembler::NotEqual, tag, cacheMiss);
  }

  const JSAtomState& names = runtime()->names();
  movePropertyKey(PropertyKey::NonIntAtom(names.undefined), outId);
  move32(Imm32(names.undefined->hash()), outHash);
  jump(&done);

  bind(&isNull);
  movePropertyKey(PropertyKey::NonIntAtom(names.null), outId);
  move32(Imm32(names.null->hash()), outHash);
  jump(&done);

  bind(&isSymbol);
  unboxSymbol(value, outId);
  load32(Address(outId, JS::Symbol::offsetOfHash()), outHash);
  orPtr(Imm32(PropertyKey::SymbolTypeTag), outId);
  jump(&done);

  bind(&isString);
  unboxString(value, outId);
  branchTest32(Assembler::Zero, Address(outId, JSString::offsetOfFlags()),
               Imm32(JSString::ATOM_BIT), &nonAtom);

  bind(&atom);
  loadAtomHash(outId, outHash, &done);

  bind(&nonAtom);
  loadStringToAtomCacheLastLookups(outHash);

  // Compare each entry in the StringToAtomCache's lastLookups_ array
  size_t stringOffset = StringToAtomCache::LastLookup::offsetOfString();
  branchPtr(Assembler::Equal, Address(outHash, stringOffset), outId,
            &lastLookupAtom);
  for (size_t i = 0; i < StringToAtomCache::NumLastLookups - 1; ++i) {
    addPtr(Imm32(sizeof(StringToAtomCache::LastLookup)), outHash);
    branchPtr(Assembler::Equal, Address(outHash, stringOffset), outId,
              &lastLookupAtom);
  }

  // Couldn't find us in the cache, so fall back to the C++ call
  jump(cacheMiss);

  // We found a hit in the lastLookups_ array! Load the associated atom
  // and jump back up to our usual atom handling code
  bind(&lastLookupAtom);
  size_t atomOffset = StringToAtomCache::LastLookup::offsetOfAtom();
  loadPtr(Address(outHash, atomOffset), outId);
  jump(&atom);

  bind(&done);
}

void MacroAssembler::emitExtractValueFromMegamorphicCacheEntry(
    Register obj, Register entry, Register scratch1, Register scratch2,
    ValueOperand output, Label* cacheHit, Label* cacheMiss) {
  Label isMissing, dynamicSlot, protoLoopHead, protoLoopTail;

  // scratch2 = entry->numHops_
  load8ZeroExtend(Address(entry, MegamorphicCache::Entry::offsetOfNumHops()),
                  scratch2);
  // if (scratch2 == NumHopsForMissingOwnProperty) goto cacheMiss
  branch32(Assembler::Equal, scratch2,
           Imm32(MegamorphicCache::Entry::NumHopsForMissingOwnProperty),
           cacheMiss);
  // if (scratch2 == NumHopsForMissingProperty) goto isMissing
  branch32(Assembler::Equal, scratch2,
           Imm32(MegamorphicCache::Entry::NumHopsForMissingProperty),
           &isMissing);

  // NOTE: Where this is called, `output` can actually alias `obj`, and before
  // the last cacheMiss branch above we can't write to `obj`, so we can't
  // use `output`'s scratch register there. However a cache miss is impossible
  // now, so we're free to use `output` as we like.
  Register outputScratch = output.scratchReg();
  if (!outputScratch.aliases(obj)) {
    // We're okay with paying this very slight extra cost to avoid a potential
    // footgun of writing to what callers understand as only an input register.
    movePtr(obj, outputScratch);
  }
  branchTest32(Assembler::Zero, scratch2, scratch2, &protoLoopTail);
  bind(&protoLoopHead);
  loadObjProto(outputScratch, outputScratch);
  branchSub32(Assembler::NonZero, Imm32(1), scratch2, &protoLoopHead);
  bind(&protoLoopTail);

  // scratch1 = entry->slotOffset()
  load32(Address(entry, MegamorphicCacheEntry::offsetOfSlotOffset()), scratch1);

  // scratch2 = slotOffset.offset()
  move32(scratch1, scratch2);
  rshift32(Imm32(TaggedSlotOffset::OffsetShift), scratch2);

  // if (!slotOffset.isFixedSlot()) goto dynamicSlot
  branchTest32(Assembler::Zero, scratch1,
               Imm32(TaggedSlotOffset::IsFixedSlotFlag), &dynamicSlot);
  // output = outputScratch[scratch2]
  loadValue(BaseIndex(outputScratch, scratch2, TimesOne), output);
  jump(cacheHit);

  bind(&dynamicSlot);
  // output = outputScratch->slots_[scratch2]
  loadPtr(Address(outputScratch, NativeObject::offsetOfSlots()), outputScratch);
  loadValue(BaseIndex(outputScratch, scratch2, TimesOne), output);
  jump(cacheHit);

  bind(&isMissing);
  // output = undefined
  moveValue(UndefinedValue(), output);
  jump(cacheHit);
}

template <typename IdOperandType>
void MacroAssembler::emitMegamorphicCacheLookupByValueCommon(
    IdOperandType id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, Label* cacheMiss, Label* cacheMissWithEntry) {
  // A lot of this code is shared with emitMegamorphicCacheLookup. It would
  // be nice to be able to avoid the duplication here, but due to a few
  // differences like taking the id in a ValueOperand instead of being able
  // to bake it in as an immediate, and only needing a Register for the output
  // value, it seemed more awkward to read once it was deduplicated.

  // outEntryPtr = obj->shape()
  loadPtr(Address(obj, JSObject::offsetOfShape()), outEntryPtr);

  movePtr(outEntryPtr, scratch2);

  // outEntryPtr = (outEntryPtr >> 3) ^ (outEntryPtr >> 13) + idHash
  rshiftPtr(Imm32(MegamorphicCache::ShapeHashShift1), outEntryPtr);
  rshiftPtr(Imm32(MegamorphicCache::ShapeHashShift2), scratch2);
  xorPtr(scratch2, outEntryPtr);

  if constexpr (std::is_same<IdOperandType, ValueOperand>::value) {
    loadAtomOrSymbolAndHash(id, scratch1, scratch2, cacheMiss);
  } else {
    static_assert(std::is_same<IdOperandType, Register>::value);
    movePtr(id, scratch1);
    loadAtomHash(scratch1, scratch2, nullptr);
  }
  addPtr(scratch2, outEntryPtr);

  // outEntryPtr %= MegamorphicCache::NumEntries
  constexpr size_t cacheSize = MegamorphicCache::NumEntries;
  static_assert(mozilla::IsPowerOfTwo(cacheSize));
  size_t cacheMask = cacheSize - 1;
  and32(Imm32(cacheMask), outEntryPtr);

  loadMegamorphicCache(scratch2);
  // outEntryPtr = &scratch2->entries_[outEntryPtr]
  constexpr size_t entrySize = sizeof(MegamorphicCache::Entry);
  static_assert(sizeof(void*) == 4 || entrySize == 24);
  if constexpr (sizeof(void*) == 4) {
    mul32(Imm32(entrySize), outEntryPtr);
    computeEffectiveAddress(BaseIndex(scratch2, outEntryPtr, TimesOne,
                                      MegamorphicCache::offsetOfEntries()),
                            outEntryPtr);
  } else {
    computeEffectiveAddress(BaseIndex(outEntryPtr, outEntryPtr, TimesTwo),
                            outEntryPtr);
    computeEffectiveAddress(BaseIndex(scratch2, outEntryPtr, TimesEight,
                                      MegamorphicCache::offsetOfEntries()),
                            outEntryPtr);
  }

  // if (outEntryPtr->key_ != scratch1) goto cacheMissWithEntry
  branchPtr(Assembler::NotEqual,
            Address(outEntryPtr, MegamorphicCache::Entry::offsetOfKey()),
            scratch1, cacheMissWithEntry);
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch1);

  // if (outEntryPtr->shape_ != scratch1) goto cacheMissWithEntry
  branchPtr(Assembler::NotEqual,
            Address(outEntryPtr, MegamorphicCache::Entry::offsetOfShape()),
            scratch1, cacheMissWithEntry);

  // scratch2 = scratch2->generation_
  load16ZeroExtend(Address(scratch2, MegamorphicCache::offsetOfGeneration()),
                   scratch2);
  load16ZeroExtend(
      Address(outEntryPtr, MegamorphicCache::Entry::offsetOfGeneration()),
      scratch1);
  // if (outEntryPtr->generation_ != scratch2) goto cacheMissWithEntry
  branch32(Assembler::NotEqual, scratch1, scratch2, cacheMissWithEntry);
}

void MacroAssembler::emitMegamorphicCacheLookup(
    PropertyKey id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, ValueOperand output, Label* cacheHit) {
  Label cacheMiss, isMissing, dynamicSlot, protoLoopHead, protoLoopTail;

  // scratch1 = obj->shape()
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch1);

  movePtr(scratch1, outEntryPtr);
  movePtr(scratch1, scratch2);

  // outEntryPtr = (scratch1 >> 3) ^ (scratch1 >> 13) + hash(id)
  rshiftPtr(Imm32(MegamorphicCache::ShapeHashShift1), outEntryPtr);
  rshiftPtr(Imm32(MegamorphicCache::ShapeHashShift2), scratch2);
  xorPtr(scratch2, outEntryPtr);
  addPtr(Imm32(HashAtomOrSymbolPropertyKey(id)), outEntryPtr);

  // outEntryPtr %= MegamorphicCache::NumEntries
  constexpr size_t cacheSize = MegamorphicCache::NumEntries;
  static_assert(mozilla::IsPowerOfTwo(cacheSize));
  size_t cacheMask = cacheSize - 1;
  and32(Imm32(cacheMask), outEntryPtr);

  loadMegamorphicCache(scratch2);
  // outEntryPtr = &scratch2->entries_[outEntryPtr]
  constexpr size_t entrySize = sizeof(MegamorphicCache::Entry);
  static_assert(sizeof(void*) == 4 || entrySize == 24);
  if constexpr (sizeof(void*) == 4) {
    mul32(Imm32(entrySize), outEntryPtr);
    computeEffectiveAddress(BaseIndex(scratch2, outEntryPtr, TimesOne,
                                      MegamorphicCache::offsetOfEntries()),
                            outEntryPtr);
  } else {
    computeEffectiveAddress(BaseIndex(outEntryPtr, outEntryPtr, TimesTwo),
                            outEntryPtr);
    computeEffectiveAddress(BaseIndex(scratch2, outEntryPtr, TimesEight,
                                      MegamorphicCache::offsetOfEntries()),
                            outEntryPtr);
  }

  // if (outEntryPtr->shape_ != scratch1) goto cacheMiss
  branchPtr(Assembler::NotEqual,
            Address(outEntryPtr, MegamorphicCache::Entry::offsetOfShape()),
            scratch1, &cacheMiss);

  // if (outEntryPtr->key_ != id) goto cacheMiss
  movePropertyKey(id, scratch1);
  branchPtr(Assembler::NotEqual,
            Address(outEntryPtr, MegamorphicCache::Entry::offsetOfKey()),
            scratch1, &cacheMiss);

  // scratch2 = scratch2->generation_
  load16ZeroExtend(Address(scratch2, MegamorphicCache::offsetOfGeneration()),
                   scratch2);
  load16ZeroExtend(
      Address(outEntryPtr, MegamorphicCache::Entry::offsetOfGeneration()),
      scratch1);
  // if (outEntryPtr->generation_ != scratch2) goto cacheMiss
  branch32(Assembler::NotEqual, scratch1, scratch2, &cacheMiss);

  emitExtractValueFromMegamorphicCacheEntry(
      obj, outEntryPtr, scratch1, scratch2, output, cacheHit, &cacheMiss);

  bind(&cacheMiss);
}

template <typename IdOperandType>
void MacroAssembler::emitMegamorphicCacheLookupByValue(
    IdOperandType id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, ValueOperand output, Label* cacheHit) {
  Label cacheMiss, cacheMissWithEntry;
  emitMegamorphicCacheLookupByValueCommon(id, obj, scratch1, scratch2,
                                          outEntryPtr, &cacheMiss,
                                          &cacheMissWithEntry);
  emitExtractValueFromMegamorphicCacheEntry(obj, outEntryPtr, scratch1,
                                            scratch2, output, cacheHit,
                                            &cacheMissWithEntry);
  bind(&cacheMiss);
  xorPtr(outEntryPtr, outEntryPtr);
  bind(&cacheMissWithEntry);
}

template void MacroAssembler::emitMegamorphicCacheLookupByValue<ValueOperand>(
    ValueOperand id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, ValueOperand output, Label* cacheHit);

template void MacroAssembler::emitMegamorphicCacheLookupByValue<Register>(
    Register id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, ValueOperand output, Label* cacheHit);

void MacroAssembler::emitMegamorphicCacheLookupExists(
    ValueOperand id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, Register output, Label* cacheHit, bool hasOwn) {
  Label cacheMiss, cacheMissWithEntry, cacheHitFalse;
  emitMegamorphicCacheLookupByValueCommon(id, obj, scratch1, scratch2,
                                          outEntryPtr, &cacheMiss,
                                          &cacheMissWithEntry);

  // scratch1 = outEntryPtr->numHops_
  load8ZeroExtend(
      Address(outEntryPtr, MegamorphicCache::Entry::offsetOfNumHops()),
      scratch1);

  branch32(Assembler::Equal, scratch1,
           Imm32(MegamorphicCache::Entry::NumHopsForMissingProperty),
           &cacheHitFalse);

  if (hasOwn) {
    branch32(Assembler::NotEqual, scratch1, Imm32(0), &cacheHitFalse);
  } else {
    branch32(Assembler::Equal, scratch1,
             Imm32(MegamorphicCache::Entry::NumHopsForMissingOwnProperty),
             &cacheMissWithEntry);
  }

  move32(Imm32(1), output);
  jump(cacheHit);

  bind(&cacheHitFalse);
  xor32(output, output);
  jump(cacheHit);

  bind(&cacheMiss);
  xorPtr(outEntryPtr, outEntryPtr);
  bind(&cacheMissWithEntry);
}

void MacroAssembler::extractCurrentIndexAndKindFromIterator(Register iterator,
                                                            Register outIndex,
                                                            Register outKind) {
  // Load iterator object
  Address nativeIterAddr(iterator,
                         PropertyIteratorObject::offsetOfIteratorSlot());
  loadPrivate(nativeIterAddr, outIndex);

  // Compute offset of propertyCursor_ from propertiesBegin()
  loadPtr(Address(outIndex, NativeIterator::offsetOfPropertyCursor()), outKind);
  subPtr(Address(outIndex, NativeIterator::offsetOfShapesEnd()), outKind);

  // Compute offset of current index from indicesBegin(). Note that because
  // propertyCursor has already been incremented, this is actually the offset
  // of the next index. We adjust accordingly below.
  size_t indexAdjustment =
      sizeof(GCPtr<JSLinearString*>) / sizeof(PropertyIndex);
  if (indexAdjustment != 1) {
    MOZ_ASSERT(indexAdjustment == 2);
    rshift32(Imm32(1), outKind);
  }

  // Load current index.
  loadPtr(Address(outIndex, NativeIterator::offsetOfPropertiesEnd()), outIndex);
  load32(BaseIndex(outIndex, outKind, Scale::TimesOne,
                   -int32_t(sizeof(PropertyIndex))),
         outIndex);

  // Extract kind.
  move32(outIndex, outKind);
  rshift32(Imm32(PropertyIndex::KindShift), outKind);

  // Extract index.
  and32(Imm32(PropertyIndex::IndexMask), outIndex);
}

template <typename IdType>
void MacroAssembler::emitMegamorphicCachedSetSlot(
    IdType id, Register obj, Register scratch1,
#ifndef JS_CODEGEN_X86  // See MegamorphicSetElement in LIROps.yaml
    Register scratch2, Register scratch3,
#endif
    ValueOperand value, Label* cacheHit,
    void (*emitPreBarrier)(MacroAssembler&, const Address&, MIRType)) {
  Label cacheMiss, dynamicSlot, doAdd, doSet, doAddDynamic, doSetDynamic;

#ifdef JS_CODEGEN_X86
  pushValue(value);
  Register scratch2 = value.typeReg();
  Register scratch3 = value.payloadReg();
#endif

  // outEntryPtr = obj->shape()
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch3);

  movePtr(scratch3, scratch2);

  // scratch3 = (scratch3 >> 3) ^ (scratch3 >> 13) + idHash
  rshiftPtr(Imm32(MegamorphicSetPropCache::ShapeHashShift1), scratch3);
  rshiftPtr(Imm32(MegamorphicSetPropCache::ShapeHashShift2), scratch2);
  xorPtr(scratch2, scratch3);

  if constexpr (std::is_same<IdType, ValueOperand>::value) {
    loadAtomOrSymbolAndHash(id, scratch1, scratch2, &cacheMiss);
    addPtr(scratch2, scratch3);
  } else {
    static_assert(std::is_same<IdType, PropertyKey>::value);
    addPtr(Imm32(HashAtomOrSymbolPropertyKey(id)), scratch3);
    movePropertyKey(id, scratch1);
  }

  // scratch3 %= MegamorphicSetPropCache::NumEntries
  constexpr size_t cacheSize = MegamorphicSetPropCache::NumEntries;
  static_assert(mozilla::IsPowerOfTwo(cacheSize));
  size_t cacheMask = cacheSize - 1;
  and32(Imm32(cacheMask), scratch3);

  loadMegamorphicSetPropCache(scratch2);
  // scratch3 = &scratch2->entries_[scratch3]
  constexpr size_t entrySize = sizeof(MegamorphicSetPropCache::Entry);
  mul32(Imm32(entrySize), scratch3);
  computeEffectiveAddress(BaseIndex(scratch2, scratch3, TimesOne,
                                    MegamorphicSetPropCache::offsetOfEntries()),
                          scratch3);

  // if (scratch3->key_ != scratch1) goto cacheMiss
  branchPtr(Assembler::NotEqual,
            Address(scratch3, MegamorphicSetPropCache::Entry::offsetOfKey()),
            scratch1, &cacheMiss);

  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch1);
  // if (scratch3->shape_ != scratch1) goto cacheMiss
  branchPtr(Assembler::NotEqual,
            Address(scratch3, MegamorphicSetPropCache::Entry::offsetOfShape()),
            scratch1, &cacheMiss);

  // scratch2 = scratch2->generation_
  load16ZeroExtend(
      Address(scratch2, MegamorphicSetPropCache::offsetOfGeneration()),
      scratch2);
  load16ZeroExtend(
      Address(scratch3, MegamorphicSetPropCache::Entry::offsetOfGeneration()),
      scratch1);
  // if (scratch3->generation_ != scratch2) goto cacheMiss
  branch32(Assembler::NotEqual, scratch1, scratch2, &cacheMiss);

  // scratch2 = entry->slotOffset()
  load32(
      Address(scratch3, MegamorphicSetPropCache::Entry::offsetOfSlotOffset()),
      scratch2);

  // scratch1 = slotOffset.offset()
  move32(scratch2, scratch1);
  rshift32(Imm32(TaggedSlotOffset::OffsetShift), scratch1);

  Address afterShapePtr(scratch3,
                        MegamorphicSetPropCache::Entry::offsetOfAfterShape());

  // if (!slotOffset.isFixedSlot()) goto dynamicSlot
  branchTest32(Assembler::Zero, scratch2,
               Imm32(TaggedSlotOffset::IsFixedSlotFlag), &dynamicSlot);

  // Calculate slot address in scratch1. Jump to doSet if scratch3 == nullptr,
  // else jump (or fall-through) to doAdd.
  addPtr(obj, scratch1);
  branchPtr(Assembler::Equal, afterShapePtr, ImmPtr(nullptr), &doSet);
  jump(&doAdd);

  bind(&dynamicSlot);
  branchPtr(Assembler::Equal, afterShapePtr, ImmPtr(nullptr), &doSetDynamic);

  Address slotAddr(scratch1, 0);

  // If entry->newCapacity_ is nonzero, we need to grow the slots on the
  // object. Otherwise just jump straight to a dynamic add.
  load16ZeroExtend(
      Address(scratch3, MegamorphicSetPropCache::Entry::offsetOfNewCapacity()),
      scratch2);
  branchTest32(Assembler::Zero, scratch2, scratch2, &doAddDynamic);

  AllocatableRegisterSet regs(RegisterSet::Volatile());
  regs.takeUnchecked(scratch2);

  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);

  Register tmp;
  if (regs.has(obj)) {
    regs.takeUnchecked(obj);
    tmp = regs.takeAnyGeneral();
    regs.addUnchecked(obj);
  } else {
    tmp = regs.takeAnyGeneral();
  }

  using Fn = bool (*)(JSContext* cx, NativeObject* obj, uint32_t newCount);
  setupUnalignedABICall(tmp);
  loadJSContext(tmp);
  passABIArg(tmp);
  passABIArg(obj);
  passABIArg(scratch2);
  callWithABI<Fn, NativeObject::growSlotsPure>();
  storeCallPointerResult(scratch2);
  PopRegsInMask(save);

  branchIfFalseBool(scratch2, &cacheMiss);

  bind(&doAddDynamic);
  addPtr(Address(obj, NativeObject::offsetOfSlots()), scratch1);

  bind(&doAdd);
  // scratch3 = entry->afterShape()
  loadPtr(
      Address(scratch3, MegamorphicSetPropCache::Entry::offsetOfAfterShape()),
      scratch3);

  storeObjShape(scratch3, obj,
                [emitPreBarrier](MacroAssembler& masm, const Address& addr) {
                  emitPreBarrier(masm, addr, MIRType::Shape);
                });
#ifdef JS_CODEGEN_X86
  popValue(value);
#endif
  storeValue(value, slotAddr);
  jump(cacheHit);

  bind(&doSetDynamic);
  addPtr(Address(obj, NativeObject::offsetOfSlots()), scratch1);
  bind(&doSet);
  guardedCallPreBarrier(slotAddr, MIRType::Value);

#ifdef JS_CODEGEN_X86
  popValue(value);
#endif
  storeValue(value, slotAddr);
  jump(cacheHit);

  bind(&cacheMiss);
#ifdef JS_CODEGEN_X86
  popValue(value);
#endif
}

template void MacroAssembler::emitMegamorphicCachedSetSlot<PropertyKey>(
    PropertyKey id, Register obj, Register scratch1,
#ifndef JS_CODEGEN_X86  // See MegamorphicSetElement in LIROps.yaml
    Register scratch2, Register scratch3,
#endif
    ValueOperand value, Label* cacheHit,
    void (*emitPreBarrier)(MacroAssembler&, const Address&, MIRType));

template void MacroAssembler::emitMegamorphicCachedSetSlot<ValueOperand>(
    ValueOperand id, Register obj, Register scratch1,
#ifndef JS_CODEGEN_X86  // See MegamorphicSetElement in LIROps.yaml
    Register scratch2, Register scratch3,
#endif
    ValueOperand value, Label* cacheHit,
    void (*emitPreBarrier)(MacroAssembler&, const Address&, MIRType));

void MacroAssembler::guardNonNegativeIntPtrToInt32(Register reg, Label* fail) {
#ifdef DEBUG
  Label ok;
  branchPtr(Assembler::NotSigned, reg, reg, &ok);
  assumeUnreachable("Unexpected negative value");
  bind(&ok);
#endif

#ifdef JS_64BIT
  branchPtr(Assembler::Above, reg, Imm32(INT32_MAX), fail);
#endif
}

void MacroAssembler::loadArrayBufferByteLengthIntPtr(Register obj,
                                                     Register output) {
  Address slotAddr(obj, ArrayBufferObject::offsetOfByteLengthSlot());
  loadPrivate(slotAddr, output);
}

void MacroAssembler::loadArrayBufferViewByteOffsetIntPtr(Register obj,
                                                         Register output) {
  Address slotAddr(obj, ArrayBufferViewObject::byteOffsetOffset());
  loadPrivate(slotAddr, output);
}

void MacroAssembler::loadArrayBufferViewLengthIntPtr(Register obj,
                                                     Register output) {
  Address slotAddr(obj, ArrayBufferViewObject::lengthOffset());
  loadPrivate(slotAddr, output);
}

void MacroAssembler::loadDOMExpandoValueGuardGeneration(
    Register obj, ValueOperand output,
    JS::ExpandoAndGeneration* expandoAndGeneration, uint64_t generation,
    Label* fail) {
  loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()),
          output.scratchReg());
  loadValue(Address(output.scratchReg(),
                    js::detail::ProxyReservedSlots::offsetOfPrivateSlot()),
            output);

  // Guard the ExpandoAndGeneration* matches the proxy's ExpandoAndGeneration
  // privateSlot.
  branchTestValue(Assembler::NotEqual, output,
                  PrivateValue(expandoAndGeneration), fail);

  // Guard expandoAndGeneration->generation matches the expected generation.
  Address generationAddr(output.payloadOrValueReg(),
                         JS::ExpandoAndGeneration::offsetOfGeneration());
  branch64(Assembler::NotEqual, generationAddr, Imm64(generation), fail);

  // Load expandoAndGeneration->expando into the output Value register.
  loadValue(Address(output.payloadOrValueReg(),
                    JS::ExpandoAndGeneration::offsetOfExpando()),
            output);
}

void MacroAssembler::loadJitActivation(Register dest) {
  loadJSContext(dest);
  loadPtr(Address(dest, offsetof(JSContext, activation_)), dest);
}

void MacroAssembler::guardSpecificAtom(Register str, JSAtom* atom,
                                       Register scratch,
                                       const LiveRegisterSet& volatileRegs,
                                       Label* fail) {
  Label done;
  branchPtr(Assembler::Equal, str, ImmGCPtr(atom), &done);

  // The pointers are not equal, so if the input string is also an atom it
  // must be a different string.
  branchTest32(Assembler::NonZero, Address(str, JSString::offsetOfFlags()),
               Imm32(JSString::ATOM_BIT), fail);

  // Check the length.
  branch32(Assembler::NotEqual, Address(str, JSString::offsetOfLength()),
           Imm32(atom->length()), fail);

  // We have a non-atomized string with the same length. Call a helper
  // function to do the comparison.
  PushRegsInMask(volatileRegs);

  using Fn = bool (*)(JSString* str1, JSString* str2);
  setupUnalignedABICall(scratch);
  movePtr(ImmGCPtr(atom), scratch);
  passABIArg(scratch);
  passABIArg(str);
  callWithABI<Fn, EqualStringsHelperPure>();
  storeCallPointerResult(scratch);

  MOZ_ASSERT(!volatileRegs.has(scratch));
  PopRegsInMask(volatileRegs);
  branchIfFalseBool(scratch, fail);

  bind(&done);
}

void MacroAssembler::guardStringToInt32(Register str, Register output,
                                        Register scratch,
                                        LiveRegisterSet volatileRegs,
                                        Label* fail) {
  Label vmCall, done;
  // Use indexed value as fast path if possible.
  loadStringIndexValue(str, output, &vmCall);
  jump(&done);
  {
    bind(&vmCall);

    // Reserve space for holding the result int32_t of the call. Use
    // pointer-size to avoid misaligning the stack on 64-bit platforms.
    reserveStack(sizeof(uintptr_t));
    moveStackPtrTo(output);

    volatileRegs.takeUnchecked(scratch);
    if (output.volatile_()) {
      volatileRegs.addUnchecked(output);
    }
    PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSContext* cx, JSString* str, int32_t* result);
    setupUnalignedABICall(scratch);
    loadJSContext(scratch);
    passABIArg(scratch);
    passABIArg(str);
    passABIArg(output);
    callWithABI<Fn, GetInt32FromStringPure>();
    storeCallPointerResult(scratch);

    PopRegsInMask(volatileRegs);

    Label ok;
    branchIfTrueBool(scratch, &ok);
    {
      // OOM path, recovered by GetInt32FromStringPure.
      //
      // Use addToStackPtr instead of freeStack as freeStack tracks stack height
      // flow-insensitively, and using it twice would confuse the stack height
      // tracking.
      addToStackPtr(Imm32(sizeof(uintptr_t)));
      jump(fail);
    }
    bind(&ok);
    load32(Address(output, 0), output);
    freeStack(sizeof(uintptr_t));
  }
  bind(&done);
}

void MacroAssembler::generateBailoutTail(Register scratch,
                                         Register bailoutInfo) {
  Label bailoutFailed;
  branchIfFalseBool(ReturnReg, &bailoutFailed);

  // Finish bailing out to Baseline.
  {
    // Prepare a register set for use in this case.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT_IF(!IsHiddenSP(getStackPointer()),
                  !regs.has(AsRegister(getStackPointer())));
    regs.take(bailoutInfo);

    Register temp = regs.takeAny();

#ifdef DEBUG
    // Assert the stack pointer points to the JitFrameLayout header. Copying
    // starts here.
    Label ok;
    loadPtr(Address(bailoutInfo, offsetof(BaselineBailoutInfo, incomingStack)),
            temp);
    branchStackPtr(Assembler::Equal, temp, &ok);
    assumeUnreachable("Unexpected stack pointer value");
    bind(&ok);
#endif

    Register copyCur = regs.takeAny();
    Register copyEnd = regs.takeAny();

    // Copy data onto stack.
    loadPtr(Address(bailoutInfo, offsetof(BaselineBailoutInfo, copyStackTop)),
            copyCur);
    loadPtr(
        Address(bailoutInfo, offsetof(BaselineBailoutInfo, copyStackBottom)),
        copyEnd);
    {
      Label copyLoop;
      Label endOfCopy;
      bind(&copyLoop);
      branchPtr(Assembler::BelowOrEqual, copyCur, copyEnd, &endOfCopy);
      subPtr(Imm32(sizeof(uintptr_t)), copyCur);
      subFromStackPtr(Imm32(sizeof(uintptr_t)));
      loadPtr(Address(copyCur, 0), temp);
      storePtr(temp, Address(getStackPointer(), 0));
      jump(&copyLoop);
      bind(&endOfCopy);
    }

    loadPtr(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeFramePtr)),
            FramePointer);

    // Enter exit frame for the FinishBailoutToBaseline call.
    pushFrameDescriptor(FrameType::BaselineJS);
    push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeAddr)));
    push(FramePointer);
    // No GC things to mark on the stack, push a bare token.
    loadJSContext(scratch);
    enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    // Save needed values onto stack temporarily.
    push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeAddr)));

    // Call a stub to free allocated memory and create arguments objects.
    using Fn = bool (*)(BaselineBailoutInfo* bailoutInfoArg);
    setupUnalignedABICall(temp);
    passABIArg(bailoutInfo);
    callWithABI<Fn, FinishBailoutToBaseline>(
        MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);
    branchIfFalseBool(ReturnReg, exceptionLabel());

    // Restore values where they need to be and resume execution.
    AllocatableGeneralRegisterSet enterRegs(GeneralRegisterSet::All());
    MOZ_ASSERT(!enterRegs.has(FramePointer));
    Register jitcodeReg = enterRegs.takeAny();

    pop(jitcodeReg);

    // Discard exit frame.
    addToStackPtr(Imm32(ExitFrameLayout::SizeWithFooter()));

    jump(jitcodeReg);
  }

  bind(&bailoutFailed);
  {
    // jit::Bailout or jit::InvalidationBailout failed and returned false. The
    // Ion frame has already been discarded and the stack pointer points to the
    // JitFrameLayout header. Turn it into an ExitFrameLayout, similar to
    // EnsureUnwoundJitExitFrame, and call the exception handler.
    loadJSContext(scratch);
    enterFakeExitFrame(scratch, scratch, ExitFrameType::UnwoundJit);
    jump(exceptionLabel());
  }
}

void MacroAssembler::loadJitCodeRaw(Register func, Register dest) {
  static_assert(BaseScript::offsetOfJitCodeRaw() ==
                    SelfHostedLazyScript::offsetOfJitCodeRaw(),
                "SelfHostedLazyScript and BaseScript must use same layout for "
                "jitCodeRaw_");
  static_assert(
      BaseScript::offsetOfJitCodeRaw() == wasm::JumpTableJitEntryOffset,
      "Wasm exported functions jit entries must use same layout for "
      "jitCodeRaw_");
  loadPrivate(Address(func, JSFunction::offsetOfJitInfoOrScript()), dest);
  loadPtr(Address(dest, BaseScript::offsetOfJitCodeRaw()), dest);
}

void MacroAssembler::loadBaselineJitCodeRaw(Register func, Register dest,
                                            Label* failure) {
  // Load JitScript
  loadPrivate(Address(func, JSFunction::offsetOfJitInfoOrScript()), dest);
  if (failure) {
    branchIfScriptHasNoJitScript(dest, failure);
  }
  loadJitScript(dest, dest);

  // Load BaselineScript
  loadPtr(Address(dest, JitScript::offsetOfBaselineScript()), dest);
  if (failure) {
    static_assert(BaselineDisabledScript == 0x1);
    branchPtr(Assembler::BelowOrEqual, dest, ImmWord(BaselineDisabledScript),
              failure);
  }

  // Load Baseline jitcode
  loadPtr(Address(dest, BaselineScript::offsetOfMethod()), dest);
  loadPtr(Address(dest, JitCode::offsetOfCode()), dest);
}

void MacroAssembler::loadBaselineFramePtr(Register framePtr, Register dest) {
  if (framePtr != dest) {
    movePtr(framePtr, dest);
  }
  subPtr(Imm32(BaselineFrame::Size()), dest);
}

static const uint8_t* ContextInlinedICScriptPtr(CompileRuntime* rt) {
  return (static_cast<const uint8_t*>(rt->mainContextPtr()) +
          JSContext::offsetOfInlinedICScript());
}

void MacroAssembler::storeICScriptInJSContext(Register icScript) {
  storePtr(icScript, AbsoluteAddress(ContextInlinedICScriptPtr(runtime())));
}

void MacroAssembler::handleFailure() {
  // Re-entry code is irrelevant because the exception will leave the
  // running function and never come back
  TrampolinePtr excTail = runtime()->jitRuntime()->getExceptionTail();
  jump(excTail);
}

void MacroAssembler::assumeUnreachable(const char* output) {
#ifdef JS_MASM_VERBOSE
  if (!IsCompilingWasm()) {
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    LiveRegisterSet save(regs.asLiveSet());
    PushRegsInMask(save);
    Register temp = regs.takeAnyGeneral();

    using Fn = void (*)(const char* output);
    setupUnalignedABICall(temp);
    movePtr(ImmPtr(output), temp);
    passABIArg(temp);
    callWithABI<Fn, AssumeUnreachable>(MoveOp::GENERAL,
                                       CheckUnsafeCallWithABI::DontCheckOther);

    PopRegsInMask(save);
  }
#endif

  breakpoint();
}

void MacroAssembler::printf(const char* output) {
#ifdef JS_MASM_VERBOSE
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);

  Register temp = regs.takeAnyGeneral();

  using Fn = void (*)(const char* output);
  setupUnalignedABICall(temp);
  movePtr(ImmPtr(output), temp);
  passABIArg(temp);
  callWithABI<Fn, Printf0>();

  PopRegsInMask(save);
#endif
}

void MacroAssembler::printf(const char* output, Register value) {
#ifdef JS_MASM_VERBOSE
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);

  regs.takeUnchecked(value);

  Register temp = regs.takeAnyGeneral();

  using Fn = void (*)(const char* output, uintptr_t value);
  setupUnalignedABICall(temp);
  movePtr(ImmPtr(output), temp);
  passABIArg(temp);
  passABIArg(value);
  callWithABI<Fn, Printf1>();

  PopRegsInMask(save);
#endif
}

void MacroAssembler::convertInt32ValueToDouble(ValueOperand val) {
  Label done;
  branchTestInt32(Assembler::NotEqual, val, &done);
  unboxInt32(val, val.scratchReg());
  ScratchDoubleScope fpscratch(*this);
  convertInt32ToDouble(val.scratchReg(), fpscratch);
  boxDouble(fpscratch, val, fpscratch);
  bind(&done);
}

void MacroAssembler::convertValueToFloatingPoint(ValueOperand value,
                                                 FloatRegister output,
                                                 Label* fail,
                                                 MIRType outputType) {
  Label isDouble, isInt32, isBool, isNull, done;

  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);

    branchTestDouble(Assembler::Equal, tag, &isDouble);
    branchTestInt32(Assembler::Equal, tag, &isInt32);
    branchTestBoolean(Assembler::Equal, tag, &isBool);
    branchTestNull(Assembler::Equal, tag, &isNull);
    branchTestUndefined(Assembler::NotEqual, tag, fail);
  }

  // fall-through: undefined
  loadConstantFloatingPoint(GenericNaN(), float(GenericNaN()), output,
                            outputType);
  jump(&done);

  bind(&isNull);
  loadConstantFloatingPoint(0.0, 0.0f, output, outputType);
  jump(&done);

  bind(&isBool);
  boolValueToFloatingPoint(value, output, outputType);
  jump(&done);

  bind(&isInt32);
  int32ValueToFloatingPoint(value, output, outputType);
  jump(&done);

  // On some non-multiAlias platforms, unboxDouble may use the scratch register,
  // so do not merge code paths here.
  bind(&isDouble);
  if (outputType == MIRType::Float32 && hasMultiAlias()) {
    ScratchDoubleScope tmp(*this);
    unboxDouble(value, tmp);
    convertDoubleToFloat32(tmp, output);
  } else {
    FloatRegister tmp = output.asDouble();
    unboxDouble(value, tmp);
    if (outputType == MIRType::Float32) {
      convertDoubleToFloat32(tmp, output);
    }
  }

  bind(&done);
}

void MacroAssembler::outOfLineTruncateSlow(FloatRegister src, Register dest,
                                           bool widenFloatToDouble,
                                           bool compilingWasm,
                                           wasm::BytecodeOffset callOffset) {
  if (compilingWasm) {
    Push(InstanceReg);
  }
  int32_t framePushedAfterInstance = framePushed();

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) ||     \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_RISCV64)
  ScratchDoubleScope fpscratch(*this);
  if (widenFloatToDouble) {
    convertFloat32ToDouble(src, fpscratch);
    src = fpscratch;
  }
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  FloatRegister srcSingle;
  if (widenFloatToDouble) {
    MOZ_ASSERT(src.isSingle());
    srcSingle = src;
    src = src.asDouble();
    Push(srcSingle);
    convertFloat32ToDouble(srcSingle, src);
  }
#else
  // Also see below
  MOZ_CRASH("MacroAssembler platform hook: outOfLineTruncateSlow");
#endif

  MOZ_ASSERT(src.isDouble());

  if (compilingWasm) {
    int32_t instanceOffset = framePushed() - framePushedAfterInstance;
    setupWasmABICall();
    passABIArg(src, MoveOp::DOUBLE);
    callWithABI(callOffset, wasm::SymbolicAddress::ToInt32,
                mozilla::Some(instanceOffset));
  } else {
    using Fn = int32_t (*)(double);
    setupUnalignedABICall(dest);
    passABIArg(src, MoveOp::DOUBLE);
    callWithABI<Fn, JS::ToInt32>(MoveOp::GENERAL,
                                 CheckUnsafeCallWithABI::DontCheckOther);
  }
  storeCallInt32Result(dest);

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) ||     \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_RISCV64)
  // Nothing
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  if (widenFloatToDouble) {
    Pop(srcSingle);
  }
#else
  MOZ_CRASH("MacroAssembler platform hook: outOfLineTruncateSlow");
#endif

  if (compilingWasm) {
    Pop(InstanceReg);
  }
}

void MacroAssembler::convertDoubleToInt(FloatRegister src, Register output,
                                        FloatRegister temp, Label* truncateFail,
                                        Label* fail,
                                        IntConversionBehavior behavior) {
  switch (behavior) {
    case IntConversionBehavior::Normal:
    case IntConversionBehavior::NegativeZeroCheck:
      convertDoubleToInt32(
          src, output, fail,
          behavior == IntConversionBehavior::NegativeZeroCheck);
      break;
    case IntConversionBehavior::Truncate:
      branchTruncateDoubleMaybeModUint32(src, output,
                                         truncateFail ? truncateFail : fail);
      break;
    case IntConversionBehavior::ClampToUint8:
      // Clamping clobbers the input register, so use a temp.
      if (src != temp) {
        moveDouble(src, temp);
      }
      clampDoubleToUint8(temp, output);
      break;
  }
}

void MacroAssembler::convertValueToInt(
    ValueOperand value, Label* handleStringEntry, Label* handleStringRejoin,
    Label* truncateDoubleSlow, Register stringReg, FloatRegister temp,
    Register output, Label* fail, IntConversionBehavior behavior,
    IntConversionInputKind conversion) {
  Label done, isInt32, isBool, isDouble, isNull, isString;

  bool handleStrings = (behavior == IntConversionBehavior::Truncate ||
                        behavior == IntConversionBehavior::ClampToUint8) &&
                       handleStringEntry && handleStringRejoin;

  MOZ_ASSERT_IF(handleStrings, conversion == IntConversionInputKind::Any);

  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);

    branchTestInt32(Equal, tag, &isInt32);
    if (conversion == IntConversionInputKind::Any ||
        conversion == IntConversionInputKind::NumbersOrBoolsOnly) {
      branchTestBoolean(Equal, tag, &isBool);
    }
    branchTestDouble(Equal, tag, &isDouble);

    if (conversion == IntConversionInputKind::Any) {
      // If we are not truncating, we fail for anything that's not
      // null. Otherwise we might be able to handle strings and undefined.
      switch (behavior) {
        case IntConversionBehavior::Normal:
        case IntConversionBehavior::NegativeZeroCheck:
          branchTestNull(Assembler::NotEqual, tag, fail);
          break;

        case IntConversionBehavior::Truncate:
        case IntConversionBehavior::ClampToUint8:
          branchTestNull(Equal, tag, &isNull);
          if (handleStrings) {
            branchTestString(Equal, tag, &isString);
          }
          branchTestUndefined(Assembler::NotEqual, tag, fail);
          break;
      }
    } else {
      jump(fail);
    }
  }

  // The value is null or undefined in truncation contexts - just emit 0.
  if (conversion == IntConversionInputKind::Any) {
    if (isNull.used()) {
      bind(&isNull);
    }
    mov(ImmWord(0), output);
    jump(&done);
  }

  // |output| needs to be different from |stringReg| to load string indices.
  bool handleStringIndices = handleStrings && output != stringReg;

  // First try loading a string index. If that fails, try converting a string
  // into a double, then jump to the double case.
  Label handleStringIndex;
  if (handleStrings) {
    bind(&isString);
    unboxString(value, stringReg);
    if (handleStringIndices) {
      loadStringIndexValue(stringReg, output, handleStringEntry);
      jump(&handleStringIndex);
    } else {
      jump(handleStringEntry);
    }
  }

  // Try converting double into integer.
  if (isDouble.used() || handleStrings) {
    if (isDouble.used()) {
      bind(&isDouble);
      unboxDouble(value, temp);
    }

    if (handleStrings) {
      bind(handleStringRejoin);
    }

    convertDoubleToInt(temp, output, temp, truncateDoubleSlow, fail, behavior);
    jump(&done);
  }

  // Just unbox a bool, the result is 0 or 1.
  if (isBool.used()) {
    bind(&isBool);
    unboxBoolean(value, output);
    jump(&done);
  }

  // Integers can be unboxed.
  if (isInt32.used() || handleStringIndices) {
    if (isInt32.used()) {
      bind(&isInt32);
      unboxInt32(value, output);
    }

    if (handleStringIndices) {
      bind(&handleStringIndex);
    }

    if (behavior == IntConversionBehavior::ClampToUint8) {
      clampIntToUint8(output);
    }
  }

  bind(&done);
}

void MacroAssembler::finish() {
  if (failureLabel_.used()) {
    bind(&failureLabel_);
    handleFailure();
  }

  MacroAssemblerSpecific::finish();

  MOZ_RELEASE_ASSERT(
      size() <= MaxCodeBytesPerProcess,
      "AssemblerBuffer should ensure we don't exceed MaxCodeBytesPerProcess");

  if (bytesNeeded() > MaxCodeBytesPerProcess) {
    setOOM();
  }
}

void MacroAssembler::link(JitCode* code) {
  MOZ_ASSERT(!oom());
  linkProfilerCallSites(code);
}

MacroAssembler::AutoProfilerCallInstrumentation::
    AutoProfilerCallInstrumentation(MacroAssembler& masm) {
  if (!masm.emitProfilingInstrumentation_) {
    return;
  }

  Register reg = CallTempReg0;
  Register reg2 = CallTempReg1;
  masm.push(reg);
  masm.push(reg2);

  CodeOffset label = masm.movWithPatch(ImmWord(uintptr_t(-1)), reg);
  masm.loadJSContext(reg2);
  masm.loadPtr(Address(reg2, offsetof(JSContext, profilingActivation_)), reg2);
  masm.storePtr(reg,
                Address(reg2, JitActivation::offsetOfLastProfilingCallSite()));

  masm.appendProfilerCallSite(label);

  masm.pop(reg2);
  masm.pop(reg);
}

void MacroAssembler::linkProfilerCallSites(JitCode* code) {
  for (size_t i = 0; i < profilerCallSites_.length(); i++) {
    CodeOffset offset = profilerCallSites_[i];
    CodeLocationLabel location(code, offset);
    PatchDataWithValueCheck(location, ImmPtr(location.raw()),
                            ImmPtr((void*)-1));
  }
}

void MacroAssembler::alignJitStackBasedOnNArgs(Register nargs,
                                               bool countIncludesThis) {
  // The stack should already be aligned to the size of a value.
  assertStackAlignment(sizeof(Value), 0);

  static_assert(JitStackValueAlignment == 1 || JitStackValueAlignment == 2,
                "JitStackValueAlignment is either 1 or 2.");
  if (JitStackValueAlignment == 1) {
    return;
  }
  // A jit frame is composed of the following:
  //
  // [padding?] [argN] .. [arg1] [this] [[argc] [callee] [descr] [raddr]]
  //                                    \________JitFrameLayout_________/
  // (The stack grows this way --->)
  //
  // We want to ensure that |raddr|, the return address, is 16-byte aligned.
  // (Note: if 8-byte alignment was sufficient, we would have already
  // returned above.)

  // JitFrameLayout does not affect the alignment, so we can ignore it.
  static_assert(sizeof(JitFrameLayout) % JitStackAlignment == 0,
                "JitFrameLayout doesn't affect stack alignment");

  // Therefore, we need to ensure that |this| is aligned.
  // This implies that |argN| must be aligned if N is even,
  // and offset by |sizeof(Value)| if N is odd.

  // Depending on the context of the caller, it may be easier to pass in a
  // register that has already been modified to include |this|. If that is the
  // case, we want to flip the direction of the test.
  Assembler::Condition condition =
      countIncludesThis ? Assembler::NonZero : Assembler::Zero;

  Label alignmentIsOffset, end;
  branchTestPtr(condition, nargs, Imm32(1), &alignmentIsOffset);

  // |argN| should be aligned to 16 bytes.
  andToStackPtr(Imm32(~(JitStackAlignment - 1)));
  jump(&end);

  // |argN| should be offset by 8 bytes from 16-byte alignment.
  // We already know that it is 8-byte aligned, so the only possibilities are:
  // a) It is 16-byte aligned, and we must offset it by 8 bytes.
  // b) It is not 16-byte aligned, and therefore already has the right offset.
  // Therefore, we test to see if it is 16-byte aligned, and adjust it if it is.
  bind(&alignmentIsOffset);
  branchTestStackPtr(Assembler::NonZero, Imm32(JitStackAlignment - 1), &end);
  subFromStackPtr(Imm32(sizeof(Value)));

  bind(&end);
}

void MacroAssembler::alignJitStackBasedOnNArgs(uint32_t argc,
                                               bool countIncludesThis) {
  // The stack should already be aligned to the size of a value.
  assertStackAlignment(sizeof(Value), 0);

  static_assert(JitStackValueAlignment == 1 || JitStackValueAlignment == 2,
                "JitStackValueAlignment is either 1 or 2.");
  if (JitStackValueAlignment == 1) {
    return;
  }

  // See above for full explanation.
  uint32_t nArgs = argc + !countIncludesThis;
  if (nArgs % 2 == 0) {
    // |argN| should be 16-byte aligned
    andToStackPtr(Imm32(~(JitStackAlignment - 1)));
  } else {
    // |argN| must be 16-byte aligned if argc is even,
    // and offset by 8 if argc is odd.
    Label end;
    branchTestStackPtr(Assembler::NonZero, Imm32(JitStackAlignment - 1), &end);
    subFromStackPtr(Imm32(sizeof(Value)));
    bind(&end);
    assertStackAlignment(JitStackAlignment, sizeof(Value));
  }
}

// ===============================================================

MacroAssembler::MacroAssembler(TempAllocator& alloc,
                               CompileRuntime* maybeRuntime,
                               CompileRealm* maybeRealm)
    : maybeRuntime_(maybeRuntime),
      maybeRealm_(maybeRealm),
      wasmMaxOffsetGuardLimit_(0),
      framePushed_(0),
#ifdef DEBUG
      inCall_(false),
#endif
      dynamicAlignment_(false),
      emitProfilingInstrumentation_(false) {
  moveResolver_.setAllocator(alloc);
}

StackMacroAssembler::StackMacroAssembler(JSContext* cx, TempAllocator& alloc)
    : MacroAssembler(alloc, CompileRuntime::get(cx->runtime()),
                     CompileRealm::get(cx->realm())) {}

IonHeapMacroAssembler::IonHeapMacroAssembler(TempAllocator& alloc,
                                             CompileRealm* realm)
    : MacroAssembler(alloc, realm->runtime(), realm) {
  MOZ_ASSERT(CurrentThreadIsIonCompiling());
}

WasmMacroAssembler::WasmMacroAssembler(TempAllocator& alloc, bool limitedSize)
    : MacroAssembler(alloc) {
#if defined(JS_CODEGEN_ARM64)
  // Stubs + builtins + the baseline compiler all require the native SP,
  // not the PSP.
  SetStackPointer64(sp);
#endif
  if (!limitedSize) {
    setUnlimitedBuffer();
  }
}

WasmMacroAssembler::WasmMacroAssembler(TempAllocator& alloc,
                                       const wasm::ModuleEnvironment& env,
                                       bool limitedSize)
    : MacroAssembler(alloc) {
#if defined(JS_CODEGEN_ARM64)
  // Stubs + builtins + the baseline compiler all require the native SP,
  // not the PSP.
  SetStackPointer64(sp);
#endif
  setWasmMaxOffsetGuardLimit(
      wasm::GetMaxOffsetGuardLimit(env.hugeMemoryEnabled()));
  if (!limitedSize) {
    setUnlimitedBuffer();
  }
}

bool MacroAssembler::icBuildOOLFakeExitFrame(void* fakeReturnAddr,
                                             AutoSaveLiveRegisters& save) {
  return buildOOLFakeExitFrame(fakeReturnAddr);
}

#ifndef JS_CODEGEN_ARM64
void MacroAssembler::subFromStackPtr(Register reg) {
  subPtr(reg, getStackPointer());
}
#endif  // JS_CODEGEN_ARM64

//{{{ check_macroassembler_style
// ===============================================================
// Stack manipulation functions.

void MacroAssembler::PushRegsInMask(LiveGeneralRegisterSet set) {
  PushRegsInMask(LiveRegisterSet(set.set(), FloatRegisterSet()));
}

void MacroAssembler::PopRegsInMask(LiveRegisterSet set) {
  PopRegsInMaskIgnore(set, LiveRegisterSet());
}

void MacroAssembler::PopRegsInMask(LiveGeneralRegisterSet set) {
  PopRegsInMask(LiveRegisterSet(set.set(), FloatRegisterSet()));
}

void MacroAssembler::Push(PropertyKey key, Register scratchReg) {
  if (key.isGCThing()) {
    // If we're pushing a gcthing, then we can't just push the tagged key
    // value since the GC won't have any idea that the push instruction
    // carries a reference to a gcthing.  Need to unpack the pointer,
    // push it using ImmGCPtr, and then rematerialize the PropertyKey at
    // runtime.

    if (key.isString()) {
      JSString* str = key.toString();
      MOZ_ASSERT((uintptr_t(str) & PropertyKey::TypeMask) == 0);
      static_assert(PropertyKey::StringTypeTag == 0,
                    "need to orPtr StringTypeTag if it's not 0");
      Push(ImmGCPtr(str));
    } else {
      MOZ_ASSERT(key.isSymbol());
      movePropertyKey(key, scratchReg);
      Push(scratchReg);
    }
  } else {
    MOZ_ASSERT(key.isInt());
    Push(ImmWord(key.asRawBits()));
  }
}

void MacroAssembler::movePropertyKey(PropertyKey key, Register dest) {
  if (key.isGCThing()) {
    // See comment in |Push(PropertyKey, ...)| above for an explanation.
    if (key.isString()) {
      JSString* str = key.toString();
      MOZ_ASSERT((uintptr_t(str) & PropertyKey::TypeMask) == 0);
      static_assert(PropertyKey::StringTypeTag == 0,
                    "need to orPtr JSID_TYPE_STRING tag if it's not 0");
      movePtr(ImmGCPtr(str), dest);
    } else {
      MOZ_ASSERT(key.isSymbol());
      JS::Symbol* sym = key.toSymbol();
      movePtr(ImmGCPtr(sym), dest);
      orPtr(Imm32(PropertyKey::SymbolTypeTag), dest);
    }
  } else {
    MOZ_ASSERT(key.isInt());
    movePtr(ImmWord(key.asRawBits()), dest);
  }
}

void MacroAssembler::Push(TypedOrValueRegister v) {
  if (v.hasValue()) {
    Push(v.valueReg());
  } else if (IsFloatingPointType(v.type())) {
    FloatRegister reg = v.typedReg().fpu();
    if (v.type() == MIRType::Float32) {
      ScratchDoubleScope fpscratch(*this);
      convertFloat32ToDouble(reg, fpscratch);
      PushBoxed(fpscratch);
    } else {
      PushBoxed(reg);
    }
  } else {
    Push(ValueTypeFromMIRType(v.type()), v.typedReg().gpr());
  }
}

void MacroAssembler::Push(const ConstantOrRegister& v) {
  if (v.constant()) {
    Push(v.value());
  } else {
    Push(v.reg());
  }
}

void MacroAssembler::Push(const Address& addr) {
  push(addr);
  framePushed_ += sizeof(uintptr_t);
}

void MacroAssembler::Push(const ValueOperand& val) {
  pushValue(val);
  framePushed_ += sizeof(Value);
}

void MacroAssembler::Push(const Value& val) {
  pushValue(val);
  framePushed_ += sizeof(Value);
}

void MacroAssembler::Push(JSValueType type, Register reg) {
  pushValue(type, reg);
  framePushed_ += sizeof(Value);
}

void MacroAssembler::Push(const Register64 reg) {
#if JS_BITS_PER_WORD == 64
  Push(reg.reg);
#else
  MOZ_ASSERT(MOZ_LITTLE_ENDIAN(), "Big-endian not supported.");
  Push(reg.high);
  Push(reg.low);
#endif
}

void MacroAssembler::PushEmptyRooted(VMFunctionData::RootType rootType) {
  switch (rootType) {
    case VMFunctionData::RootNone:
      MOZ_CRASH("Handle must have root type");
    case VMFunctionData::RootObject:
    case VMFunctionData::RootString:
    case VMFunctionData::RootCell:
    case VMFunctionData::RootBigInt:
      Push(ImmPtr(nullptr));
      break;
    case VMFunctionData::RootValue:
      Push(UndefinedValue());
      break;
    case VMFunctionData::RootId:
      Push(ImmWord(JS::PropertyKey::Void().asRawBits()));
      break;
  }
}

void MacroAssembler::popRooted(VMFunctionData::RootType rootType,
                               Register cellReg, const ValueOperand& valueReg) {
  switch (rootType) {
    case VMFunctionData::RootNone:
      MOZ_CRASH("Handle must have root type");
    case VMFunctionData::RootObject:
    case VMFunctionData::RootString:
    case VMFunctionData::RootCell:
    case VMFunctionData::RootId:
    case VMFunctionData::RootBigInt:
      Pop(cellReg);
      break;
    case VMFunctionData::RootValue:
      Pop(valueReg);
      break;
  }
}

void MacroAssembler::adjustStack(int amount) {
  if (amount > 0) {
    freeStack(amount);
  } else if (amount < 0) {
    reserveStack(-amount);
  }
}

void MacroAssembler::freeStack(uint32_t amount) {
  MOZ_ASSERT(amount <= framePushed_);
  if (amount) {
    addToStackPtr(Imm32(amount));
  }
  framePushed_ -= amount;
}

void MacroAssembler::freeStack(Register amount) { addToStackPtr(amount); }

// ===============================================================
// ABI function calls.
template <class ABIArgGeneratorT>
void MacroAssembler::setupABICallHelper() {
#ifdef DEBUG
  MOZ_ASSERT(!inCall_);
  inCall_ = true;
#endif

#ifdef JS_SIMULATOR
  signature_ = 0;
#endif

  // Reinitialize the ABIArg generator.
  abiArgs_ = ABIArgGeneratorT();

#if defined(JS_CODEGEN_ARM)
  // On ARM, we need to know what ABI we are using, either in the
  // simulator, or based on the configure flags.
#  if defined(JS_SIMULATOR_ARM)
  abiArgs_.setUseHardFp(UseHardFpABI());
#  elif defined(JS_CODEGEN_ARM_HARDFP)
  abiArgs_.setUseHardFp(true);
#  else
  abiArgs_.setUseHardFp(false);
#  endif
#endif

#if defined(JS_CODEGEN_MIPS32)
  // On MIPS, the system ABI use general registers pairs to encode double
  // arguments, after one or 2 integer-like arguments. Unfortunately, the
  // Lowering phase is not capable to express it at the moment. So we enforce
  // the system ABI here.
  abiArgs_.enforceO32ABI();
#endif
}

void MacroAssembler::setupNativeABICall() {
  setupABICallHelper<ABIArgGenerator>();
}

void MacroAssembler::setupWasmABICall() {
  MOZ_ASSERT(IsCompilingWasm(), "non-wasm should use setupAlignedABICall");
  setupABICallHelper<WasmABIArgGenerator>();

#if defined(JS_CODEGEN_ARM)
  // The builtin thunk does the FP -> GPR moving on soft-FP, so
  // use hard fp unconditionally.
  abiArgs_.setUseHardFp(true);
#endif
  dynamicAlignment_ = false;
}

void MacroAssembler::setupAlignedABICall() {
  MOZ_ASSERT(!IsCompilingWasm(), "wasm should use setupWasmABICall");
  setupNativeABICall();
  dynamicAlignment_ = false;
}

void MacroAssembler::passABIArg(const MoveOperand& from, MoveOp::Type type) {
  MOZ_ASSERT(inCall_);
  appendSignatureType(type);

  ABIArg arg;
  switch (type) {
    case MoveOp::FLOAT32:
      arg = abiArgs_.next(MIRType::Float32);
      break;
    case MoveOp::DOUBLE:
      arg = abiArgs_.next(MIRType::Double);
      break;
    case MoveOp::GENERAL:
      arg = abiArgs_.next(MIRType::Pointer);
      break;
    default:
      MOZ_CRASH("Unexpected argument type");
  }

  MoveOperand to(*this, arg);
  if (from == to) {
    return;
  }

  if (oom()) {
    return;
  }
  propagateOOM(moveResolver_.addMove(from, to, type));
}

void MacroAssembler::callWithABINoProfiler(void* fun, MoveOp::Type result,
                                           CheckUnsafeCallWithABI check) {
  appendSignatureType(result);
#ifdef JS_SIMULATOR
  fun = Simulator::RedirectNativeFunction(fun, signature());
#endif

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);

#ifdef DEBUG
  if (check == CheckUnsafeCallWithABI::Check) {
    push(ReturnReg);
    loadJSContext(ReturnReg);
    Address flagAddr(ReturnReg, JSContext::offsetOfInUnsafeCallWithABI());
    store32(Imm32(1), flagAddr);
    pop(ReturnReg);
    // On arm64, SP may be < PSP now (that's OK).
    // eg testcase: tests/bug1375074.js
  }
#endif

  call(ImmPtr(fun));

  callWithABIPost(stackAdjust, result);

#ifdef DEBUG
  if (check == CheckUnsafeCallWithABI::Check) {
    Label ok;
    push(ReturnReg);
    loadJSContext(ReturnReg);
    Address flagAddr(ReturnReg, JSContext::offsetOfInUnsafeCallWithABI());
    branch32(Assembler::Equal, flagAddr, Imm32(0), &ok);
    assumeUnreachable("callWithABI: callee did not use AutoUnsafeCallWithABI");
    bind(&ok);
    pop(ReturnReg);
    // On arm64, SP may be < PSP now (that's OK).
    // eg testcase: tests/bug1375074.js
  }
#endif
}

CodeOffset MacroAssembler::callWithABI(wasm::BytecodeOffset bytecode,
                                       wasm::SymbolicAddress imm,
                                       mozilla::Maybe<int32_t> instanceOffset,
                                       MoveOp::Type result) {
  MOZ_ASSERT(wasm::NeedsBuiltinThunk(imm));

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust, /* callFromWasm = */ true);

  // The instance register is used in builtin thunks and must be set.
  if (instanceOffset) {
    loadPtr(Address(getStackPointer(), *instanceOffset + stackAdjust),
            InstanceReg);
  } else {
    MOZ_CRASH("instanceOffset is Nothing only for unsupported abi calls.");
  }
  CodeOffset raOffset = call(
      wasm::CallSiteDesc(bytecode.offset(), wasm::CallSite::Symbolic), imm);

  callWithABIPost(stackAdjust, result, /* callFromWasm = */ true);

  return raOffset;
}

void MacroAssembler::callDebugWithABI(wasm::SymbolicAddress imm,
                                      MoveOp::Type result) {
  MOZ_ASSERT(!wasm::NeedsBuiltinThunk(imm));
  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust, /* callFromWasm = */ false);
  call(imm);
  callWithABIPost(stackAdjust, result, /* callFromWasm = */ false);
}

// ===============================================================
// Exit frame footer.

void MacroAssembler::linkExitFrame(Register cxreg, Register scratch) {
  loadPtr(Address(cxreg, JSContext::offsetOfActivation()), scratch);
  storeStackPtr(Address(scratch, JitActivation::offsetOfPackedExitFP()));
}

// ===============================================================
// Simple value-shuffling helpers, to hide MoveResolver verbosity
// in common cases.

void MacroAssembler::moveRegPair(Register src0, Register src1, Register dst0,
                                 Register dst1, MoveOp::Type type) {
  MoveResolver& moves = moveResolver();
  if (src0 != dst0) {
    propagateOOM(moves.addMove(MoveOperand(src0), MoveOperand(dst0), type));
  }
  if (src1 != dst1) {
    propagateOOM(moves.addMove(MoveOperand(src1), MoveOperand(dst1), type));
  }
  propagateOOM(moves.resolve());
  if (oom()) {
    return;
  }

  MoveEmitter emitter(*this);
  emitter.emit(moves);
  emitter.finish();
}

// ===============================================================
// Arithmetic functions

void MacroAssembler::pow32(Register base, Register power, Register dest,
                           Register temp1, Register temp2, Label* onOver) {
  // Inline int32-specialized implementation of js::powi with overflow
  // detection.

  move32(Imm32(1), dest);  // result = 1

  // x^y where x == 1 returns 1 for any y.
  Label done;
  branch32(Assembler::Equal, base, Imm32(1), &done);

  move32(base, temp1);   // runningSquare = x
  move32(power, temp2);  // n = y

  // x^y where y < 0 returns a non-int32 value for any x != 1. Except when y is
  // large enough so that the result is no longer representable as a double with
  // fractional parts. We can't easily determine when y is too large, so we bail
  // here.
  // Note: it's important for this condition to match the code in CacheIR.cpp
  // (CanAttachInt32Pow) to prevent failure loops.
  Label start;
  branchTest32(Assembler::NotSigned, power, power, &start);
  jump(onOver);

  Label loop;
  bind(&loop);

  // runningSquare *= runningSquare
  branchMul32(Assembler::Overflow, temp1, temp1, onOver);

  bind(&start);

  // if ((n & 1) != 0) result *= runningSquare
  Label even;
  branchTest32(Assembler::Zero, temp2, Imm32(1), &even);
  branchMul32(Assembler::Overflow, temp1, dest, onOver);
  bind(&even);

  // n >>= 1
  // if (n == 0) return result
  branchRshift32(Assembler::NonZero, Imm32(1), temp2, &loop);

  bind(&done);
}

void MacroAssembler::signInt32(Register input, Register output) {
  MOZ_ASSERT(input != output);

  Label done;
  move32(input, output);
  rshift32Arithmetic(Imm32(31), output);
  branch32(Assembler::LessThanOrEqual, input, Imm32(0), &done);
  move32(Imm32(1), output);
  bind(&done);
}

void MacroAssembler::signDouble(FloatRegister input, FloatRegister output) {
  MOZ_ASSERT(input != output);

  Label done, zeroOrNaN, negative;
  loadConstantDouble(0.0, output);
  branchDouble(Assembler::DoubleEqualOrUnordered, input, output, &zeroOrNaN);
  branchDouble(Assembler::DoubleLessThan, input, output, &negative);

  loadConstantDouble(1.0, output);
  jump(&done);

  bind(&negative);
  loadConstantDouble(-1.0, output);
  jump(&done);

  bind(&zeroOrNaN);
  moveDouble(input, output);

  bind(&done);
}

void MacroAssembler::signDoubleToInt32(FloatRegister input, Register output,
                                       FloatRegister temp, Label* fail) {
  MOZ_ASSERT(input != temp);

  Label done, zeroOrNaN, negative;
  loadConstantDouble(0.0, temp);
  branchDouble(Assembler::DoubleEqualOrUnordered, input, temp, &zeroOrNaN);
  branchDouble(Assembler::DoubleLessThan, input, temp, &negative);

  move32(Imm32(1), output);
  jump(&done);

  bind(&negative);
  move32(Imm32(-1), output);
  jump(&done);

  // Fail for NaN and negative zero.
  bind(&zeroOrNaN);
  branchDouble(Assembler::DoubleUnordered, input, input, fail);

  // The easiest way to distinguish -0.0 from 0.0 is that 1.0/-0.0
  // is -Infinity instead of Infinity.
  loadConstantDouble(1.0, temp);
  divDouble(input, temp);
  branchDouble(Assembler::DoubleLessThan, temp, input, fail);
  move32(Imm32(0), output);

  bind(&done);
}

void MacroAssembler::randomDouble(Register rng, FloatRegister dest,
                                  Register64 temp0, Register64 temp1) {
  using mozilla::non_crypto::XorShift128PlusRNG;

  static_assert(
      sizeof(XorShift128PlusRNG) == 2 * sizeof(uint64_t),
      "Code below assumes XorShift128PlusRNG contains two uint64_t values");

  Address state0Addr(rng, XorShift128PlusRNG::offsetOfState0());
  Address state1Addr(rng, XorShift128PlusRNG::offsetOfState1());

  Register64 s0Reg = temp0;
  Register64 s1Reg = temp1;

  // uint64_t s1 = mState[0];
  load64(state0Addr, s1Reg);

  // s1 ^= s1 << 23;
  move64(s1Reg, s0Reg);
  lshift64(Imm32(23), s1Reg);
  xor64(s0Reg, s1Reg);

  // s1 ^= s1 >> 17
  move64(s1Reg, s0Reg);
  rshift64(Imm32(17), s1Reg);
  xor64(s0Reg, s1Reg);

  // const uint64_t s0 = mState[1];
  load64(state1Addr, s0Reg);

  // mState[0] = s0;
  store64(s0Reg, state0Addr);

  // s1 ^= s0
  xor64(s0Reg, s1Reg);

  // s1 ^= s0 >> 26
  rshift64(Imm32(26), s0Reg);
  xor64(s0Reg, s1Reg);

  // mState[1] = s1
  store64(s1Reg, state1Addr);

  // s1 += mState[0]
  load64(state0Addr, s0Reg);
  add64(s0Reg, s1Reg);

  // See comment in XorShift128PlusRNG::nextDouble().
  static constexpr int MantissaBits =
      mozilla::FloatingPoint<double>::kExponentShift + 1;
  static constexpr double ScaleInv = double(1) / (1ULL << MantissaBits);

  and64(Imm64((1ULL << MantissaBits) - 1), s1Reg);

  // Note: we know s1Reg isn't signed after the and64 so we can use the faster
  // convertInt64ToDouble instead of convertUInt64ToDouble.
  convertInt64ToDouble(s1Reg, dest);

  // dest *= ScaleInv
  mulDoublePtr(ImmPtr(&ScaleInv), s0Reg.scratchReg(), dest);
}

void MacroAssembler::sameValueDouble(FloatRegister left, FloatRegister right,
                                     FloatRegister temp, Register dest) {
  Label nonEqual, isSameValue, isNotSameValue;
  branchDouble(Assembler::DoubleNotEqualOrUnordered, left, right, &nonEqual);
  {
    // First, test for being equal to 0.0, which also includes -0.0.
    loadConstantDouble(0.0, temp);
    branchDouble(Assembler::DoubleNotEqual, left, temp, &isSameValue);

    // The easiest way to distinguish -0.0 from 0.0 is that 1.0/-0.0
    // is -Infinity instead of Infinity.
    Label isNegInf;
    loadConstantDouble(1.0, temp);
    divDouble(left, temp);
    branchDouble(Assembler::DoubleLessThan, temp, left, &isNegInf);
    {
      loadConstantDouble(1.0, temp);
      divDouble(right, temp);
      branchDouble(Assembler::DoubleGreaterThan, temp, right, &isSameValue);
      jump(&isNotSameValue);
    }
    bind(&isNegInf);
    {
      loadConstantDouble(1.0, temp);
      divDouble(right, temp);
      branchDouble(Assembler::DoubleLessThan, temp, right, &isSameValue);
      jump(&isNotSameValue);
    }
  }
  bind(&nonEqual);
  {
    // Test if both values are NaN.
    branchDouble(Assembler::DoubleOrdered, left, left, &isNotSameValue);
    branchDouble(Assembler::DoubleOrdered, right, right, &isNotSameValue);
  }

  Label done;
  bind(&isSameValue);
  move32(Imm32(1), dest);
  jump(&done);

  bind(&isNotSameValue);
  move32(Imm32(0), dest);

  bind(&done);
}

void MacroAssembler::minMaxArrayInt32(Register array, Register result,
                                      Register temp1, Register temp2,
                                      Register temp3, bool isMax, Label* fail) {
  // array must be a packed array. Load its elements.
  Register elements = temp1;
  loadPtr(Address(array, NativeObject::offsetOfElements()), elements);

  // Load the length and guard that it is non-zero.
  Address lengthAddr(elements, ObjectElements::offsetOfInitializedLength());
  load32(lengthAddr, temp3);
  branchTest32(Assembler::Zero, temp3, temp3, fail);

  // Compute the address of the last element.
  Register elementsEnd = temp2;
  BaseObjectElementIndex elementsEndAddr(elements, temp3,
                                         -int32_t(sizeof(Value)));
  computeEffectiveAddress(elementsEndAddr, elementsEnd);

  // Load the first element into result.
  fallibleUnboxInt32(Address(elements, 0), result, fail);

  Label loop, done;
  bind(&loop);

  // Check whether we're done.
  branchPtr(Assembler::Equal, elements, elementsEnd, &done);

  // If not, advance to the next element and load it.
  addPtr(Imm32(sizeof(Value)), elements);
  fallibleUnboxInt32(Address(elements, 0), temp3, fail);

  // Update result if necessary.
  Assembler::Condition cond =
      isMax ? Assembler::GreaterThan : Assembler::LessThan;
  cmp32Move32(cond, temp3, result, temp3, result);

  jump(&loop);
  bind(&done);
}

void MacroAssembler::minMaxArrayNumber(Register array, FloatRegister result,
                                       FloatRegister floatTemp, Register temp1,
                                       Register temp2, bool isMax,
                                       Label* fail) {
  // array must be a packed array. Load its elements.
  Register elements = temp1;
  loadPtr(Address(array, NativeObject::offsetOfElements()), elements);

  // Load the length and check if the array is empty.
  Label isEmpty;
  Address lengthAddr(elements, ObjectElements::offsetOfInitializedLength());
  load32(lengthAddr, temp2);
  branchTest32(Assembler::Zero, temp2, temp2, &isEmpty);

  // Compute the address of the last element.
  Register elementsEnd = temp2;
  BaseObjectElementIndex elementsEndAddr(elements, temp2,
                                         -int32_t(sizeof(Value)));
  computeEffectiveAddress(elementsEndAddr, elementsEnd);

  // Load the first element into result.
  ensureDouble(Address(elements, 0), result, fail);

  Label loop, done;
  bind(&loop);

  // Check whether we're done.
  branchPtr(Assembler::Equal, elements, elementsEnd, &done);

  // If not, advance to the next element and load it into floatTemp.
  addPtr(Imm32(sizeof(Value)), elements);
  ensureDouble(Address(elements, 0), floatTemp, fail);

  // Update result if necessary.
  if (isMax) {
    maxDouble(floatTemp, result, /* handleNaN = */ true);
  } else {
    minDouble(floatTemp, result, /* handleNaN = */ true);
  }
  jump(&loop);

  // With no arguments, min/max return +Infinity/-Infinity respectively.
  bind(&isEmpty);
  if (isMax) {
    loadConstantDouble(mozilla::NegativeInfinity<double>(), result);
  } else {
    loadConstantDouble(mozilla::PositiveInfinity<double>(), result);
  }

  bind(&done);
}

void MacroAssembler::branchIfNotRegExpPrototypeOptimizable(Register proto,
                                                           Register temp,
                                                           Label* fail) {
  loadJSContext(temp);
  loadPtr(Address(temp, JSContext::offsetOfRealm()), temp);
  size_t offset = Realm::offsetOfRegExps() +
                  RegExpRealm::offsetOfOptimizableRegExpPrototypeShape();
  loadPtr(Address(temp, offset), temp);
  branchTestObjShapeUnsafe(Assembler::NotEqual, proto, temp, fail);
}

void MacroAssembler::branchIfNotRegExpInstanceOptimizable(Register regexp,
                                                          Register temp,
                                                          Label* label) {
  loadJSContext(temp);
  loadPtr(Address(temp, JSContext::offsetOfRealm()), temp);
  size_t offset = Realm::offsetOfRegExps() +
                  RegExpRealm::offsetOfOptimizableRegExpInstanceShape();
  loadPtr(Address(temp, offset), temp);
  branchTestObjShapeUnsafe(Assembler::NotEqual, regexp, temp, label);
}

void MacroAssembler::loadRegExpLastIndex(Register regexp, Register string,
                                         Register lastIndex,
                                         Label* notFoundZeroLastIndex) {
  Address flagsSlot(regexp, RegExpObject::offsetOfFlags());
  Address lastIndexSlot(regexp, RegExpObject::offsetOfLastIndex());
  Address stringLength(string, JSString::offsetOfLength());

  Label notGlobalOrSticky, loadedLastIndex;

  branchTest32(Assembler::Zero, flagsSlot,
               Imm32(JS::RegExpFlag::Global | JS::RegExpFlag::Sticky),
               &notGlobalOrSticky);
  {
    // It's a global or sticky regular expression. Emit the following code:
    //
    //   lastIndex = regexp.lastIndex
    //   if lastIndex > string.length:
    //     jump to notFoundZeroLastIndex (skip the regexp match/test operation)
    //
    // The `notFoundZeroLastIndex` code should set regexp.lastIndex to 0 and
    // treat this as a not-found result.
    //
    // See steps 5-8 in js::RegExpBuiltinExec.
    //
    // Earlier guards must have ensured regexp.lastIndex is a non-negative
    // integer.
#ifdef DEBUG
    {
      Label ok;
      branchTestInt32(Assembler::Equal, lastIndexSlot, &ok);
      assumeUnreachable("Expected int32 value for lastIndex");
      bind(&ok);
    }
#endif
    unboxInt32(lastIndexSlot, lastIndex);
#ifdef DEBUG
    {
      Label ok;
      branchTest32(Assembler::NotSigned, lastIndex, lastIndex, &ok);
      assumeUnreachable("Expected non-negative lastIndex");
      bind(&ok);
    }
#endif
    branch32(Assembler::Below, stringLength, lastIndex, notFoundZeroLastIndex);
    jump(&loadedLastIndex);
  }

  bind(&notGlobalOrSticky);
  move32(Imm32(0), lastIndex);

  bind(&loadedLastIndex);
}

// ===============================================================
// Branch functions

void MacroAssembler::loadFunctionLength(Register func,
                                        Register funFlagsAndArgCount,
                                        Register output, Label* slowPath) {
#ifdef DEBUG
  {
    // These flags should already have been checked by caller.
    Label ok;
    uint32_t FlagsToCheck =
        FunctionFlags::SELFHOSTLAZY | FunctionFlags::RESOLVED_LENGTH;
    branchTest32(Assembler::Zero, funFlagsAndArgCount, Imm32(FlagsToCheck),
                 &ok);
    assumeUnreachable("The function flags should already have been checked.");
    bind(&ok);
  }
#endif  // DEBUG

  // NOTE: `funFlagsAndArgCount` and `output` must be allowed to alias.

  // Load the target function's length.
  Label isInterpreted, lengthLoaded;
  branchTest32(Assembler::NonZero, funFlagsAndArgCount,
               Imm32(FunctionFlags::BASESCRIPT), &isInterpreted);
  {
    // The length property of a native function stored with the flags.
    move32(funFlagsAndArgCount, output);
    rshift32(Imm32(JSFunction::ArgCountShift), output);
    jump(&lengthLoaded);
  }
  bind(&isInterpreted);
  {
    // Load the length property of an interpreted function.
    loadPrivate(Address(func, JSFunction::offsetOfJitInfoOrScript()), output);
    loadPtr(Address(output, JSScript::offsetOfSharedData()), output);
    branchTestPtr(Assembler::Zero, output, output, slowPath);
    loadPtr(Address(output, SharedImmutableScriptData::offsetOfISD()), output);
    load16ZeroExtend(Address(output, ImmutableScriptData::offsetOfFunLength()),
                     output);
  }
  bind(&lengthLoaded);
}

void MacroAssembler::loadFunctionName(Register func, Register output,
                                      ImmGCPtr emptyString, Label* slowPath) {
  MOZ_ASSERT(func != output);

  // Get the JSFunction flags.
  load32(Address(func, JSFunction::offsetOfFlagsAndArgCount()), output);

  // If the name was previously resolved, the name property may be shadowed.
  branchTest32(Assembler::NonZero, output, Imm32(FunctionFlags::RESOLVED_NAME),
               slowPath);

  Label noName, done;
  branchTest32(Assembler::NonZero, output,
               Imm32(FunctionFlags::HAS_GUESSED_ATOM), &noName);

  Address atomAddr(func, JSFunction::offsetOfAtom());
  branchTestUndefined(Assembler::Equal, atomAddr, &noName);
  unboxString(atomAddr, output);
  jump(&done);

  {
    bind(&noName);

    // An absent name property defaults to the empty string.
    movePtr(emptyString, output);
  }

  bind(&done);
}

void MacroAssembler::assertFunctionIsExtended(Register func) {
#ifdef DEBUG
  Label extended;
  branchTestFunctionFlags(func, FunctionFlags::EXTENDED, Assembler::NonZero,
                          &extended);
  assumeUnreachable("Function is not extended");
  bind(&extended);
#endif
}

void MacroAssembler::branchTestType(Condition cond, Register tag,
                                    JSValueType type, Label* label) {
  switch (type) {
    case JSVAL_TYPE_DOUBLE:
      branchTestDouble(cond, tag, label);
      break;
    case JSVAL_TYPE_INT32:
      branchTestInt32(cond, tag, label);
      break;
    case JSVAL_TYPE_BOOLEAN:
      branchTestBoolean(cond, tag, label);
      break;
    case JSVAL_TYPE_UNDEFINED:
      branchTestUndefined(cond, tag, label);
      break;
    case JSVAL_TYPE_NULL:
      branchTestNull(cond, tag, label);
      break;
    case JSVAL_TYPE_MAGIC:
      branchTestMagic(cond, tag, label);
      break;
    case JSVAL_TYPE_STRING:
      branchTestString(cond, tag, label);
      break;
    case JSVAL_TYPE_SYMBOL:
      branchTestSymbol(cond, tag, label);
      break;
    case JSVAL_TYPE_BIGINT:
      branchTestBigInt(cond, tag, label);
      break;
    case JSVAL_TYPE_OBJECT:
      branchTestObject(cond, tag, label);
      break;
    default:
      MOZ_CRASH("Unexpected value type");
  }
}

void MacroAssembler::branchTestObjShapeList(
    Condition cond, Register obj, Register shapeElements, Register shapeScratch,
    Register endScratch, Register spectreScratch, Label* label) {
  MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);

  bool needSpectreMitigations = spectreScratch != InvalidReg;

  Label done;
  Label* onMatch = cond == Assembler::Equal ? label : &done;

  // Load the object's shape pointer into shapeScratch, and prepare to compare
  // it with the shapes in the list. On 64-bit, we box the shape. On 32-bit,
  // we only have to compare the 32-bit payload.
#ifdef JS_PUNBOX64
  loadPtr(Address(obj, JSObject::offsetOfShape()), endScratch);
  tagValue(JSVAL_TYPE_PRIVATE_GCTHING, endScratch, ValueOperand(shapeScratch));
#else
  loadPtr(Address(obj, JSObject::offsetOfShape()), shapeScratch);
#endif

  // Compute end pointer.
  Address lengthAddr(shapeElements,
                     ObjectElements::offsetOfInitializedLength());
  load32(lengthAddr, endScratch);
  BaseObjectElementIndex endPtrAddr(shapeElements, endScratch);
  computeEffectiveAddress(endPtrAddr, endScratch);

  Label loop;
  bind(&loop);

  // Compare the object's shape with a shape from the list. Note that on 64-bit
  // this includes the tag bits, but on 32-bit we only compare the low word of
  // the value. This is fine because the list of shapes is never exposed and the
  // tag is guaranteed to be PrivateGCThing.
  if (needSpectreMitigations) {
    move32(Imm32(0), spectreScratch);
  }
  branchPtr(Assembler::Equal, Address(shapeElements, 0), shapeScratch, onMatch);
  if (needSpectreMitigations) {
    spectreMovePtr(Assembler::Equal, spectreScratch, obj);
  }

  // Advance to next shape and loop if not finished.
  addPtr(Imm32(sizeof(Value)), shapeElements);
  branchPtr(Assembler::Below, shapeElements, endScratch, &loop);

  if (cond == Assembler::NotEqual) {
    jump(label);
    bind(&done);
  }
}

void MacroAssembler::branchTestObjCompartment(Condition cond, Register obj,
                                              const Address& compartment,
                                              Register scratch, Label* label) {
  MOZ_ASSERT(obj != scratch);
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfRealm()), scratch);
  loadPtr(Address(scratch, Realm::offsetOfCompartment()), scratch);
  branchPtr(cond, compartment, scratch, label);
}

void MacroAssembler::branchTestObjCompartment(
    Condition cond, Register obj, const JS::Compartment* compartment,
    Register scratch, Label* label) {
  MOZ_ASSERT(obj != scratch);
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfRealm()), scratch);
  loadPtr(Address(scratch, Realm::offsetOfCompartment()), scratch);
  branchPtr(cond, scratch, ImmPtr(compartment), label);
}

void MacroAssembler::branchIfNonNativeObj(Register obj, Register scratch,
                                          Label* label) {
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  branchTest32(Assembler::Zero,
               Address(scratch, Shape::offsetOfImmutableFlags()),
               Imm32(Shape::isNativeBit()), label);
}

void MacroAssembler::branchIfObjectNotExtensible(Register obj, Register scratch,
                                                 Label* label) {
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);

  // Spectre-style checks are not needed here because we do not interpret data
  // based on this check.
  static_assert(sizeof(ObjectFlags) == sizeof(uint16_t));
  load16ZeroExtend(Address(scratch, Shape::offsetOfObjectFlags()), scratch);
  branchTest32(Assembler::NonZero, scratch,
               Imm32(uint32_t(ObjectFlag::NotExtensible)), label);
}

void MacroAssembler::wasmTrap(wasm::Trap trap,
                              wasm::BytecodeOffset bytecodeOffset) {
  uint32_t trapOffset = wasmTrapInstruction().offset();
  MOZ_ASSERT_IF(!oom(),
                currentOffset() - trapOffset == WasmTrapInstructionLength);

  append(trap, wasm::TrapSite(trapOffset, bytecodeOffset));
}

std::pair<CodeOffset, uint32_t> MacroAssembler::wasmReserveStackChecked(
    uint32_t amount, wasm::BytecodeOffset trapOffset) {
  if (amount > MAX_UNCHECKED_LEAF_FRAME_SIZE) {
    // The frame is large.  Don't bump sp until after the stack limit check so
    // that the trap handler isn't called with a wild sp.
    Label ok;
    Register scratch = ABINonArgReg0;
    moveStackPtrTo(scratch);

    Label trap;
    branchPtr(Assembler::Below, scratch, Imm32(amount), &trap);
    subPtr(Imm32(amount), scratch);
    branchPtr(Assembler::Below,
              Address(InstanceReg, wasm::Instance::offsetOfStackLimit()),
              scratch, &ok);

    bind(&trap);
    wasmTrap(wasm::Trap::StackOverflow, trapOffset);
    CodeOffset trapInsnOffset = CodeOffset(currentOffset());

    bind(&ok);
    reserveStack(amount);
    return std::pair<CodeOffset, uint32_t>(trapInsnOffset, 0);
  }

  reserveStack(amount);
  Label ok;
  branchStackPtrRhs(Assembler::Below,
                    Address(InstanceReg, wasm::Instance::offsetOfStackLimit()),
                    &ok);
  wasmTrap(wasm::Trap::StackOverflow, trapOffset);
  CodeOffset trapInsnOffset = CodeOffset(currentOffset());
  bind(&ok);
  return std::pair<CodeOffset, uint32_t>(trapInsnOffset, amount);
}

CodeOffset MacroAssembler::wasmCallImport(const wasm::CallSiteDesc& desc,
                                          const wasm::CalleeDesc& callee) {
  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall));

  // Load the callee, before the caller's registers are clobbered.
  uint32_t instanceDataOffset = callee.importInstanceDataOffset();
  loadPtr(
      Address(InstanceReg, wasm::Instance::offsetInData(
                               instanceDataOffset +
                               offsetof(wasm::FuncImportInstanceData, code))),
      ABINonArgReg0);

#if !defined(JS_CODEGEN_NONE) && !defined(JS_CODEGEN_WASM32)
  static_assert(ABINonArgReg0 != InstanceReg, "by constraint");
#endif

  // Switch to the callee's realm.
  loadPtr(
      Address(InstanceReg, wasm::Instance::offsetInData(
                               instanceDataOffset +
                               offsetof(wasm::FuncImportInstanceData, realm))),
      ABINonArgReg1);
  loadPtr(Address(InstanceReg, wasm::Instance::offsetOfCx()), ABINonArgReg2);
  storePtr(ABINonArgReg1, Address(ABINonArgReg2, JSContext::offsetOfRealm()));

  // Switch to the callee's instance and pinned registers and make the call.
  loadPtr(Address(InstanceReg,
                  wasm::Instance::offsetInData(
                      instanceDataOffset +
                      offsetof(wasm::FuncImportInstanceData, instance))),
          InstanceReg);

  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCalleeInstanceOffsetBeforeCall));
  loadWasmPinnedRegsFromInstance();

  return call(desc, ABINonArgReg0);
}

CodeOffset MacroAssembler::wasmCallBuiltinInstanceMethod(
    const wasm::CallSiteDesc& desc, const ABIArg& instanceArg,
    wasm::SymbolicAddress builtin, wasm::FailureMode failureMode) {
  MOZ_ASSERT(instanceArg != ABIArg());

  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall));
  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCalleeInstanceOffsetBeforeCall));

  if (instanceArg.kind() == ABIArg::GPR) {
    movePtr(InstanceReg, instanceArg.gpr());
  } else if (instanceArg.kind() == ABIArg::Stack) {
    storePtr(InstanceReg,
             Address(getStackPointer(), instanceArg.offsetFromArgBase()));
  } else {
    MOZ_CRASH("Unknown abi passing style for pointer");
  }

  CodeOffset ret = call(desc, builtin);

  if (failureMode != wasm::FailureMode::Infallible) {
    Label noTrap;
    switch (failureMode) {
      case wasm::FailureMode::Infallible:
        MOZ_CRASH();
      case wasm::FailureMode::FailOnNegI32:
        branchTest32(Assembler::NotSigned, ReturnReg, ReturnReg, &noTrap);
        break;
      case wasm::FailureMode::FailOnNullPtr:
        branchTestPtr(Assembler::NonZero, ReturnReg, ReturnReg, &noTrap);
        break;
      case wasm::FailureMode::FailOnInvalidRef:
        branchPtr(Assembler::NotEqual, ReturnReg,
                  ImmWord(uintptr_t(wasm::AnyRef::invalid().forCompiledCode())),
                  &noTrap);
        break;
    }
    wasmTrap(wasm::Trap::ThrowReported,
             wasm::BytecodeOffset(desc.lineOrBytecode()));
    bind(&noTrap);
  }

  return ret;
}

CodeOffset MacroAssembler::asmCallIndirect(const wasm::CallSiteDesc& desc,
                                           const wasm::CalleeDesc& callee) {
  MOZ_ASSERT(callee.which() == wasm::CalleeDesc::AsmJSTable);

  const Register scratch = WasmTableCallScratchReg0;
  const Register index = WasmTableCallIndexReg;

  // Optimization opportunity: when offsetof(FunctionTableElem, code) == 0, as
  // it is at present, we can probably generate better code here by folding
  // the address computation into the load.

  static_assert(sizeof(wasm::FunctionTableElem) == 8 ||
                    sizeof(wasm::FunctionTableElem) == 16,
                "elements of function tables are two words");

  // asm.js tables require no signature check, and have had their index
  // masked into range and thus need no bounds check.
  loadPtr(
      Address(InstanceReg, wasm::Instance::offsetInData(
                               callee.tableFunctionBaseInstanceDataOffset())),
      scratch);
  if (sizeof(wasm::FunctionTableElem) == 8) {
    computeEffectiveAddress(BaseIndex(scratch, index, TimesEight), scratch);
  } else {
    lshift32(Imm32(4), index);
    addPtr(index, scratch);
  }
  loadPtr(Address(scratch, offsetof(wasm::FunctionTableElem, code)), scratch);
  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall));
  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCalleeInstanceOffsetBeforeCall));
  return call(desc, scratch);
}

// In principle, call_indirect requires an expensive context switch to the
// callee's instance and realm before the call and an almost equally expensive
// switch back to the caller's ditto after.  However, if the caller's instance
// is the same as the callee's instance then no context switch is required, and
// it only takes a compare-and-branch at run-time to test this - all values are
// in registers already.  We therefore generate two call paths, one for the fast
// call without the context switch (which additionally avoids a null check) and
// one for the slow call with the context switch.

void MacroAssembler::wasmCallIndirect(const wasm::CallSiteDesc& desc,
                                      const wasm::CalleeDesc& callee,
                                      Label* boundsCheckFailedLabel,
                                      Label* nullCheckFailedLabel,
                                      mozilla::Maybe<uint32_t> tableSize,
                                      CodeOffset* fastCallOffset,
                                      CodeOffset* slowCallOffset) {
  static_assert(sizeof(wasm::FunctionTableElem) == 2 * sizeof(void*),
                "Exactly two pointers or index scaling won't work correctly");
  MOZ_ASSERT(callee.which() == wasm::CalleeDesc::WasmTable);

  const int shift = sizeof(wasm::FunctionTableElem) == 8 ? 3 : 4;
  wasm::BytecodeOffset trapOffset(desc.lineOrBytecode());
  const Register calleeScratch = WasmTableCallScratchReg0;
  const Register index = WasmTableCallIndexReg;

  // Check the table index and throw if out-of-bounds.
  //
  // Frequently the table size is known, so optimize for that.  Otherwise
  // compare with a memory operand when that's possible.  (There's little sense
  // in hoisting the load of the bound into a register at a higher level and
  // reusing that register, because a hoisted value would either have to be
  // spilled and re-loaded before the next call_indirect, or would be abandoned
  // because we could not trust that a hoisted value would not have changed.)

  if (boundsCheckFailedLabel) {
    if (tableSize.isSome()) {
      branch32(Assembler::Condition::AboveOrEqual, index, Imm32(*tableSize),
               boundsCheckFailedLabel);
    } else {
      branch32(
          Assembler::Condition::BelowOrEqual,
          Address(InstanceReg, wasm::Instance::offsetInData(
                                   callee.tableLengthInstanceDataOffset())),
          index, boundsCheckFailedLabel);
    }
  }

  // Write the functype-id into the ABI functype-id register.

  const wasm::CallIndirectId callIndirectId = callee.wasmTableSigId();
  switch (callIndirectId.kind()) {
    case wasm::CallIndirectIdKind::Global:
      loadPtr(Address(InstanceReg, wasm::Instance::offsetInData(
                                       callIndirectId.instanceDataOffset())),
              WasmTableCallSigReg);
      break;
    case wasm::CallIndirectIdKind::Immediate:
      move32(Imm32(callIndirectId.immediate()), WasmTableCallSigReg);
      break;
    case wasm::CallIndirectIdKind::AsmJS:
    case wasm::CallIndirectIdKind::None:
      break;
  }

  // Load the base pointer of the table and compute the address of the callee in
  // the table.

  loadPtr(
      Address(InstanceReg, wasm::Instance::offsetInData(
                               callee.tableFunctionBaseInstanceDataOffset())),
      calleeScratch);
  shiftIndex32AndAdd(index, shift, calleeScratch);

  // Load the callee instance and decide whether to take the fast path or the
  // slow path.

  Label fastCall;
  Label done;
  const Register newInstanceTemp = WasmTableCallScratchReg1;
  loadPtr(Address(calleeScratch, offsetof(wasm::FunctionTableElem, instance)),
          newInstanceTemp);
  branchPtr(Assembler::Equal, InstanceReg, newInstanceTemp, &fastCall);

  // Slow path: Save context, check for null, setup new context, call, restore
  // context.
  //
  // TODO: The slow path could usefully be out-of-line and the test above would
  // just fall through to the fast path.  This keeps the fast-path code dense,
  // and has correct static prediction for the branch (forward conditional
  // branches predicted not taken, normally).

  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall));
  movePtr(newInstanceTemp, InstanceReg);
  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCalleeInstanceOffsetBeforeCall));

#ifdef WASM_HAS_HEAPREG
  // Use the null pointer exception resulting from loading HeapReg from a null
  // instance to handle a call to a null slot.
  MOZ_ASSERT(nullCheckFailedLabel == nullptr);
  loadWasmPinnedRegsFromInstance(mozilla::Some(trapOffset));
#else
  MOZ_ASSERT(nullCheckFailedLabel != nullptr);
  branchTestPtr(Assembler::Zero, InstanceReg, InstanceReg,
                nullCheckFailedLabel);

  loadWasmPinnedRegsFromInstance();
#endif
  switchToWasmInstanceRealm(index, WasmTableCallScratchReg1);

  loadPtr(Address(calleeScratch, offsetof(wasm::FunctionTableElem, code)),
          calleeScratch);

  *slowCallOffset = call(desc, calleeScratch);

  // Restore registers and realm and join up with the fast path.

  loadPtr(Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall),
          InstanceReg);
  loadWasmPinnedRegsFromInstance();
  switchToWasmInstanceRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
  jump(&done);

  // Fast path: just load the code pointer and go.  The instance and heap
  // register are the same as in the caller, and nothing will be null.
  //
  // (In particular, the code pointer will not be null: if it were, the instance
  // would have been null, and then it would not have been equivalent to our
  // current instance.  So no null check is needed on the fast path.)

  bind(&fastCall);

  loadPtr(Address(calleeScratch, offsetof(wasm::FunctionTableElem, code)),
          calleeScratch);

  // We use a different type of call site for the fast call since the instance
  // slots in the frame do not have valid values.

  wasm::CallSiteDesc newDesc(desc.lineOrBytecode(),
                             wasm::CallSiteDesc::IndirectFast);
  *fastCallOffset = call(newDesc, calleeScratch);

  bind(&done);
}

void MacroAssembler::wasmCallRef(const wasm::CallSiteDesc& desc,
                                 const wasm::CalleeDesc& callee,
                                 CodeOffset* fastCallOffset,
                                 CodeOffset* slowCallOffset) {
  MOZ_ASSERT(callee.which() == wasm::CalleeDesc::FuncRef);
  const Register calleeScratch = WasmCallRefCallScratchReg0;
  const Register calleeFnObj = WasmCallRefReg;

  // Load from the function's WASM_INSTANCE_SLOT extended slot, and decide
  // whether to take the fast path or the slow path. Register this load
  // instruction to be source of a trap -- null pointer check.

  Label fastCall;
  Label done;
  const Register newInstanceTemp = WasmCallRefCallScratchReg1;
  size_t instanceSlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_INSTANCE_SLOT);
  static_assert(FunctionExtended::WASM_INSTANCE_SLOT < wasm::NullPtrGuardSize);
  wasm::BytecodeOffset trapOffset(desc.lineOrBytecode());
  append(wasm::Trap::NullPointerDereference,
         wasm::TrapSite(currentOffset(), trapOffset));
  loadPtr(Address(calleeFnObj, instanceSlotOffset), newInstanceTemp);
  branchPtr(Assembler::Equal, InstanceReg, newInstanceTemp, &fastCall);

  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall));
  movePtr(newInstanceTemp, InstanceReg);
  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCalleeInstanceOffsetBeforeCall));

  loadWasmPinnedRegsFromInstance();
  switchToWasmInstanceRealm(WasmCallRefCallScratchReg0,
                            WasmCallRefCallScratchReg1);

  // Get funcUncheckedCallEntry() from the function's
  // WASM_FUNC_UNCHECKED_ENTRY_SLOT extended slot.
  size_t uncheckedEntrySlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_FUNC_UNCHECKED_ENTRY_SLOT);
  loadPtr(Address(calleeFnObj, uncheckedEntrySlotOffset), calleeScratch);

  *slowCallOffset = call(desc, calleeScratch);

  // Restore registers and realm and back to this caller's.
  loadPtr(Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall),
          InstanceReg);
  loadWasmPinnedRegsFromInstance();
  switchToWasmInstanceRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
  jump(&done);

  // Fast path: just load WASM_FUNC_UNCHECKED_ENTRY_SLOT value and go.
  // The instance and pinned registers are the same as in the caller.

  bind(&fastCall);

  loadPtr(Address(calleeFnObj, uncheckedEntrySlotOffset), calleeScratch);

  // We use a different type of call site for the fast call since the instance
  // slots in the frame do not have valid values.

  wasm::CallSiteDesc newDesc(desc.lineOrBytecode(),
                             wasm::CallSiteDesc::FuncRefFast);
  *fastCallOffset = call(newDesc, calleeScratch);

  bind(&done);
}

bool MacroAssembler::needScratch1ForBranchWasmGcRefType(wasm::RefType type) {
  MOZ_ASSERT(type.isValid());
  MOZ_ASSERT(type.isAnyHierarchy());
  return !type.isNone() && !type.isAny();
}

bool MacroAssembler::needScratch2ForBranchWasmGcRefType(wasm::RefType type) {
  MOZ_ASSERT(type.isValid());
  MOZ_ASSERT(type.isAnyHierarchy());
  return type.isTypeRef() &&
         type.typeDef()->subTypingDepth() >= wasm::MinSuperTypeVectorLength;
}

bool MacroAssembler::needSuperSuperTypeVectorForBranchWasmGcRefType(
    wasm::RefType type) {
  return type.isTypeRef();
}

void MacroAssembler::branchWasmGcObjectIsRefType(
    Register object, wasm::RefType sourceType, wasm::RefType destType,
    Label* label, bool onSuccess, Register superSuperTypeVector,
    Register scratch1, Register scratch2) {
  MOZ_ASSERT(sourceType.isValid());
  MOZ_ASSERT(destType.isValid());
  MOZ_ASSERT(sourceType.isAnyHierarchy());
  MOZ_ASSERT(destType.isAnyHierarchy());
  MOZ_ASSERT_IF(needScratch1ForBranchWasmGcRefType(destType),
                scratch1 != Register::Invalid());
  MOZ_ASSERT_IF(needScratch2ForBranchWasmGcRefType(destType),
                scratch2 != Register::Invalid());
  MOZ_ASSERT_IF(needSuperSuperTypeVectorForBranchWasmGcRefType(destType),
                superSuperTypeVector != Register::Invalid());

  Label fallthrough;
  Label* successLabel = onSuccess ? label : &fallthrough;
  Label* failLabel = onSuccess ? &fallthrough : label;
  Label* nullLabel = destType.isNullable() ? successLabel : failLabel;

  // Check for null.
  if (sourceType.isNullable()) {
    branchTestPtr(Assembler::Zero, object, object, nullLabel);
  }

  // The only value that can inhabit 'none' is null. So, early out if we got
  // not-null.
  if (destType.isNone()) {
    jump(failLabel);
    bind(&fallthrough);
    return;
  }

  if (destType.isAny()) {
    // No further checks for 'any'
    jump(successLabel);
    bind(&fallthrough);
    return;
  }

  // 'type' is now 'eq' or lower, which currently will always be a gc object.
  // Test for non-gc objects.
  MOZ_ASSERT(scratch1 != Register::Invalid());
  if (!wasm::RefType::isSubTypeOf(sourceType, wasm::RefType::eq())) {
    branchTestObjectIsWasmGcObject(false, object, scratch1, failLabel);
  }

  if (destType.isEq()) {
    // No further checks for 'eq'
    jump(successLabel);
    bind(&fallthrough);
    return;
  }

  // 'type' is now 'struct', 'array', or a concrete type. (Bottom types were
  // handled above.)
  //
  // Casting to a concrete type only requires a simple check on the
  // object's superTypeVector. Casting to an abstract type (struct, array)
  // requires loading the object's superTypeVector->typeDef->kind, and checking
  // that it is correct.

  loadPtr(Address(object, int32_t(WasmGcObject::offsetOfSuperTypeVector())),
          scratch1);
  if (destType.isTypeRef()) {
    // concrete type, do superTypeVector check
    branchWasmSuperTypeVectorIsSubtype(scratch1, superSuperTypeVector, scratch2,
                                       destType.typeDef()->subTypingDepth(),
                                       successLabel, true);
  } else {
    // abstract type, do kind check
    loadPtr(Address(scratch1,
                    int32_t(wasm::SuperTypeVector::offsetOfSelfTypeDef())),
            scratch1);
    load8ZeroExtend(Address(scratch1, int32_t(wasm::TypeDef::offsetOfKind())),
                    scratch1);
    branch32(Assembler::Equal, scratch1, Imm32(int32_t(destType.typeDefKind())),
             successLabel);
  }

  // The cast failed.
  jump(failLabel);
  bind(&fallthrough);
}

void MacroAssembler::branchWasmSuperTypeVectorIsSubtype(
    Register subSuperTypeVector, Register superSuperTypeVector,
    Register scratch, uint32_t superTypeDepth, Label* label, bool onSuccess) {
  MOZ_ASSERT_IF(superTypeDepth >= wasm::MinSuperTypeVectorLength,
                scratch != Register::Invalid());

  // We generate just different enough code for 'is' subtype vs 'is not'
  // subtype that we handle them separately.
  if (onSuccess) {
    Label failed;

    // At this point, we could generate a fast success check which jumps to
    // `label` if `subSuperTypeVector == superSuperTypeVector`.  However,
    // profiling of Barista-3 seems to show this is hardly worth anything,
    // whereas it is worth us generating smaller code and in particular one
    // fewer conditional branch.  So it is omitted:
    //
    //   branchPtr(Assembler::Equal, subSuperTypeVector, superSuperTypeVector,
    //   label);

    // Emit a bounds check if the super type depth may be out-of-bounds.
    if (superTypeDepth >= wasm::MinSuperTypeVectorLength) {
      // Slowest path for having a bounds check of the super type vector
      load32(
          Address(subSuperTypeVector, wasm::SuperTypeVector::offsetOfLength()),
          scratch);
      branch32(Assembler::LessThanOrEqual, scratch, Imm32(superTypeDepth),
               &failed);
    }

    // Load the `superTypeDepth` entry from subSuperTypeVector. This
    // will be `superSuperTypeVector` if `subSuperTypeVector` is indeed a
    // subtype.
    loadPtr(
        Address(subSuperTypeVector,
                wasm::SuperTypeVector::offsetOfTypeDefInVector(superTypeDepth)),
        subSuperTypeVector);
    branchPtr(Assembler::Equal, subSuperTypeVector, superSuperTypeVector,
              label);

    // Fallthrough to the failed case
    bind(&failed);
    return;
  }

  // Emit a bounds check if the super type depth may be out-of-bounds.
  if (superTypeDepth >= wasm::MinSuperTypeVectorLength) {
    load32(Address(subSuperTypeVector, wasm::SuperTypeVector::offsetOfLength()),
           scratch);
    branch32(Assembler::LessThanOrEqual, scratch, Imm32(superTypeDepth), label);
  }

  // Load the `superTypeDepth` entry from subSuperTypeVector. This will be
  // `superSuperTypeVector` if `subSuperTypeVector` is indeed a subtype.
  loadPtr(
      Address(subSuperTypeVector,
              wasm::SuperTypeVector::offsetOfTypeDefInVector(superTypeDepth)),
      subSuperTypeVector);
  branchPtr(Assembler::NotEqual, subSuperTypeVector, superSuperTypeVector,
            label);
  // Fallthrough to the success case
}

void MacroAssembler::nopPatchableToCall(const wasm::CallSiteDesc& desc) {
  CodeOffset offset = nopPatchableToCall();
  append(desc, offset);
}

void MacroAssembler::emitPreBarrierFastPath(JSRuntime* rt, MIRType type,
                                            Register temp1, Register temp2,
                                            Register temp3, Label* noBarrier) {
  MOZ_ASSERT(temp1 != PreBarrierReg);
  MOZ_ASSERT(temp2 != PreBarrierReg);
  MOZ_ASSERT(temp3 != PreBarrierReg);

  // Load the GC thing in temp1.
  if (type == MIRType::Value) {
    unboxGCThingForGCBarrier(Address(PreBarrierReg, 0), temp1);
  } else {
    MOZ_ASSERT(type == MIRType::Object || type == MIRType::String ||
               type == MIRType::Shape);
    loadPtr(Address(PreBarrierReg, 0), temp1);
  }

#ifdef DEBUG
  // The caller should have checked for null pointers.
  Label nonZero;
  branchTestPtr(Assembler::NonZero, temp1, temp1, &nonZero);
  assumeUnreachable("JIT pre-barrier: unexpected nullptr");
  bind(&nonZero);
#endif

  // Load the chunk address in temp2.
  movePtr(temp1, temp2);
  andPtr(Imm32(int32_t(~gc::ChunkMask)), temp2);

  // If the GC thing is in the nursery, we don't need to barrier it.
  if (type == MIRType::Value || type == MIRType::Object ||
      type == MIRType::String) {
    branchPtr(Assembler::NotEqual, Address(temp2, gc::ChunkStoreBufferOffset),
              ImmWord(0), noBarrier);
  } else {
#ifdef DEBUG
    Label isTenured;
    branchPtr(Assembler::Equal, Address(temp2, gc::ChunkStoreBufferOffset),
              ImmWord(0), &isTenured);
    assumeUnreachable("JIT pre-barrier: unexpected nursery pointer");
    bind(&isTenured);
#endif
  }

  // Determine the bit index and store in temp1.
  //
  // bit = (addr & js::gc::ChunkMask) / js::gc::CellBytesPerMarkBit +
  //        static_cast<uint32_t>(colorBit);
  static_assert(gc::CellBytesPerMarkBit == 8,
                "Calculation below relies on this");
  static_assert(size_t(gc::ColorBit::BlackBit) == 0,
                "Calculation below relies on this");
  andPtr(Imm32(gc::ChunkMask), temp1);
  rshiftPtr(Imm32(3), temp1);

  static_assert(gc::MarkBitmapWordBits == JS_BITS_PER_WORD,
                "Calculation below relies on this");

  // Load the bitmap word in temp2.
  //
  // word = chunk.bitmap[bit / MarkBitmapWordBits];

  // Fold the adjustment for the fact that arenas don't start at the beginning
  // of the chunk into the offset to the chunk bitmap.
  const size_t firstArenaAdjustment = gc::FirstArenaAdjustmentBits / CHAR_BIT;
  const intptr_t offset =
      intptr_t(gc::ChunkMarkBitmapOffset) - intptr_t(firstArenaAdjustment);

  movePtr(temp1, temp3);
#if JS_BITS_PER_WORD == 64
  rshiftPtr(Imm32(6), temp1);
  loadPtr(BaseIndex(temp2, temp1, TimesEight, offset), temp2);
#else
  rshiftPtr(Imm32(5), temp1);
  loadPtr(BaseIndex(temp2, temp1, TimesFour, offset), temp2);
#endif

  // Load the mask in temp1.
  //
  // mask = uintptr_t(1) << (bit % MarkBitmapWordBits);
  andPtr(Imm32(gc::MarkBitmapWordBits - 1), temp3);
  move32(Imm32(1), temp1);
#ifdef JS_CODEGEN_X64
  MOZ_ASSERT(temp3 == rcx);
  shlq_cl(temp1);
#elif JS_CODEGEN_X86
  MOZ_ASSERT(temp3 == ecx);
  shll_cl(temp1);
#elif JS_CODEGEN_ARM
  ma_lsl(temp3, temp1, temp1);
#elif JS_CODEGEN_ARM64
  Lsl(ARMRegister(temp1, 64), ARMRegister(temp1, 64), ARMRegister(temp3, 64));
#elif JS_CODEGEN_MIPS32
  ma_sll(temp1, temp1, temp3);
#elif JS_CODEGEN_MIPS64
  ma_dsll(temp1, temp1, temp3);
#elif JS_CODEGEN_LOONG64
  as_sll_d(temp1, temp1, temp3);
#elif JS_CODEGEN_RISCV64
  sll(temp1, temp1, temp3);
#elif JS_CODEGEN_WASM32
  MOZ_CRASH();
#elif JS_CODEGEN_NONE
  MOZ_CRASH();
#else
#  error "Unknown architecture"
#endif

  // No barrier is needed if the bit is set, |word & mask != 0|.
  branchTestPtr(Assembler::NonZero, temp2, temp1, noBarrier);
}

// ========================================================================
// JS atomic operations.

void MacroAssembler::atomicIsLockFreeJS(Register value, Register output) {
  // Keep this in sync with isLockfreeJS() in jit/AtomicOperations.h.
  static_assert(AtomicOperations::isLockfreeJS(1));  // Implementation artifact
  static_assert(AtomicOperations::isLockfreeJS(2));  // Implementation artifact
  static_assert(AtomicOperations::isLockfreeJS(4));  // Spec requirement
  static_assert(AtomicOperations::isLockfreeJS(8));  // Implementation artifact

  Label done;
  move32(Imm32(1), output);
  branch32(Assembler::Equal, value, Imm32(8), &done);
  branch32(Assembler::Equal, value, Imm32(4), &done);
  branch32(Assembler::Equal, value, Imm32(2), &done);
  branch32(Assembler::Equal, value, Imm32(1), &done);
  move32(Imm32(0), output);
  bind(&done);
}

// ========================================================================
// Spectre Mitigations.

void MacroAssembler::spectreMaskIndex32(Register index, Register length,
                                        Register output) {
  MOZ_ASSERT(JitOptions.spectreIndexMasking);
  MOZ_ASSERT(length != output);
  MOZ_ASSERT(index != output);

  move32(Imm32(0), output);
  cmp32Move32(Assembler::Below, index, length, index, output);
}

void MacroAssembler::spectreMaskIndex32(Register index, const Address& length,
                                        Register output) {
  MOZ_ASSERT(JitOptions.spectreIndexMasking);
  MOZ_ASSERT(index != length.base);
  MOZ_ASSERT(length.base != output);
  MOZ_ASSERT(index != output);

  move32(Imm32(0), output);
  cmp32Move32(Assembler::Below, index, length, index, output);
}

void MacroAssembler::spectreMaskIndexPtr(Register index, Register length,
                                         Register output) {
  MOZ_ASSERT(JitOptions.spectreIndexMasking);
  MOZ_ASSERT(length != output);
  MOZ_ASSERT(index != output);

  movePtr(ImmWord(0), output);
  cmpPtrMovePtr(Assembler::Below, index, length, index, output);
}

void MacroAssembler::spectreMaskIndexPtr(Register index, const Address& length,
                                         Register output) {
  MOZ_ASSERT(JitOptions.spectreIndexMasking);
  MOZ_ASSERT(index != length.base);
  MOZ_ASSERT(length.base != output);
  MOZ_ASSERT(index != output);

  movePtr(ImmWord(0), output);
  cmpPtrMovePtr(Assembler::Below, index, length, index, output);
}

void MacroAssembler::boundsCheck32PowerOfTwo(Register index, uint32_t length,
                                             Label* failure) {
  MOZ_ASSERT(mozilla::IsPowerOfTwo(length));
  branch32(Assembler::AboveOrEqual, index, Imm32(length), failure);

  // Note: it's fine to clobber the input register, as this is a no-op: it
  // only affects speculative execution.
  if (JitOptions.spectreIndexMasking) {
    and32(Imm32(length - 1), index);
  }
}

void MacroAssembler::loadWasmPinnedRegsFromInstance(
    mozilla::Maybe<wasm::BytecodeOffset> trapOffset) {
#ifdef WASM_HAS_HEAPREG
  static_assert(wasm::Instance::offsetOfMemoryBase() < 4096,
                "We count only on the low page being inaccessible");
  if (trapOffset) {
    append(wasm::Trap::IndirectCallToNull,
           wasm::TrapSite(currentOffset(), *trapOffset));
  }
  loadPtr(Address(InstanceReg, wasm::Instance::offsetOfMemoryBase()), HeapReg);
#else
  MOZ_ASSERT(!trapOffset);
#endif
}

//}}} check_macroassembler_style

#ifdef JS_64BIT
void MacroAssembler::debugAssertCanonicalInt32(Register r) {
#  ifdef DEBUG
  if (!js::jit::JitOptions.lessDebugCode) {
#    if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64)
    Label ok;
    branchPtr(Assembler::BelowOrEqual, r, ImmWord(UINT32_MAX), &ok);
    breakpoint();
    bind(&ok);
#    elif defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64)
    Label ok;
    ScratchRegisterScope scratch(asMasm());
    move32SignExtendToPtr(r, scratch);
    branchPtr(Assembler::Equal, r, scratch, &ok);
    breakpoint();
    bind(&ok);
#    else
    MOZ_CRASH("IMPLEMENT ME");
#    endif
  }
#  endif
}
#endif

void MacroAssembler::memoryBarrierBefore(const Synchronization& sync) {
  memoryBarrier(sync.barrierBefore);
}

void MacroAssembler::memoryBarrierAfter(const Synchronization& sync) {
  memoryBarrier(sync.barrierAfter);
}

void MacroAssembler::debugAssertIsObject(const ValueOperand& val) {
#ifdef DEBUG
  Label ok;
  branchTestObject(Assembler::Equal, val, &ok);
  assumeUnreachable("Expected an object!");
  bind(&ok);
#endif
}

void MacroAssembler::debugAssertObjHasFixedSlots(Register obj,
                                                 Register scratch) {
#ifdef DEBUG
  Label hasFixedSlots;
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  branchTest32(Assembler::NonZero,
               Address(scratch, Shape::offsetOfImmutableFlags()),
               Imm32(NativeShape::fixedSlotsMask()), &hasFixedSlots);
  assumeUnreachable("Expected a fixed slot");
  bind(&hasFixedSlots);
#endif
}

void MacroAssembler::debugAssertObjectHasClass(Register obj, Register scratch,
                                               const JSClass* clasp) {
#ifdef DEBUG
  Label done;
  branchTestObjClassNoSpectreMitigations(Assembler::Equal, obj, clasp, scratch,
                                         &done);
  assumeUnreachable("Class check failed");
  bind(&done);
#endif
}

void MacroAssembler::branchArrayIsNotPacked(Register array, Register temp1,
                                            Register temp2, Label* label) {
  loadPtr(Address(array, NativeObject::offsetOfElements()), temp1);

  // Test length == initializedLength.
  Address initLength(temp1, ObjectElements::offsetOfInitializedLength());
  load32(Address(temp1, ObjectElements::offsetOfLength()), temp2);
  branch32(Assembler::NotEqual, initLength, temp2, label);

  // Test the NON_PACKED flag.
  Address flags(temp1, ObjectElements::offsetOfFlags());
  branchTest32(Assembler::NonZero, flags, Imm32(ObjectElements::NON_PACKED),
               label);
}

void MacroAssembler::setIsPackedArray(Register obj, Register output,
                                      Register temp) {
  // Ensure it's an ArrayObject.
  Label notPackedArray;
  branchTestObjClass(Assembler::NotEqual, obj, &ArrayObject::class_, temp, obj,
                     &notPackedArray);

  branchArrayIsNotPacked(obj, temp, output, &notPackedArray);

  Label done;
  move32(Imm32(1), output);
  jump(&done);

  bind(&notPackedArray);
  move32(Imm32(0), output);

  bind(&done);
}

void MacroAssembler::packedArrayPop(Register array, ValueOperand output,
                                    Register temp1, Register temp2,
                                    Label* fail) {
  // Load obj->elements in temp1.
  loadPtr(Address(array, NativeObject::offsetOfElements()), temp1);

  // Check flags.
  static constexpr uint32_t UnhandledFlags =
      ObjectElements::Flags::NON_PACKED |
      ObjectElements::Flags::NONWRITABLE_ARRAY_LENGTH |
      ObjectElements::Flags::NOT_EXTENSIBLE |
      ObjectElements::Flags::MAYBE_IN_ITERATION;
  Address flags(temp1, ObjectElements::offsetOfFlags());
  branchTest32(Assembler::NonZero, flags, Imm32(UnhandledFlags), fail);

  // Load length in temp2. Ensure length == initializedLength.
  Address lengthAddr(temp1, ObjectElements::offsetOfLength());
  Address initLengthAddr(temp1, ObjectElements::offsetOfInitializedLength());
  load32(lengthAddr, temp2);
  branch32(Assembler::NotEqual, initLengthAddr, temp2, fail);

  // Result is |undefined| if length == 0.
  Label notEmpty, done;
  branchTest32(Assembler::NonZero, temp2, temp2, &notEmpty);
  {
    moveValue(UndefinedValue(), output);
    jump(&done);
  }

  bind(&notEmpty);

  // Load the last element.
  sub32(Imm32(1), temp2);
  BaseObjectElementIndex elementAddr(temp1, temp2);
  loadValue(elementAddr, output);

  // Pre-barrier the element because we're removing it from the array.
  EmitPreBarrier(*this, elementAddr, MIRType::Value);

  // Update length and initializedLength.
  store32(temp2, lengthAddr);
  store32(temp2, initLengthAddr);

  bind(&done);
}

void MacroAssembler::packedArrayShift(Register array, ValueOperand output,
                                      Register temp1, Register temp2,
                                      LiveRegisterSet volatileRegs,
                                      Label* fail) {
  // Load obj->elements in temp1.
  loadPtr(Address(array, NativeObject::offsetOfElements()), temp1);

  // Check flags.
  static constexpr uint32_t UnhandledFlags =
      ObjectElements::Flags::NON_PACKED |
      ObjectElements::Flags::NONWRITABLE_ARRAY_LENGTH |
      ObjectElements::Flags::NOT_EXTENSIBLE |
      ObjectElements::Flags::MAYBE_IN_ITERATION;
  Address flags(temp1, ObjectElements::offsetOfFlags());
  branchTest32(Assembler::NonZero, flags, Imm32(UnhandledFlags), fail);

  // Load length in temp2. Ensure length == initializedLength.
  Address lengthAddr(temp1, ObjectElements::offsetOfLength());
  Address initLengthAddr(temp1, ObjectElements::offsetOfInitializedLength());
  load32(lengthAddr, temp2);
  branch32(Assembler::NotEqual, initLengthAddr, temp2, fail);

  // Result is |undefined| if length == 0.
  Label notEmpty, done;
  branchTest32(Assembler::NonZero, temp2, temp2, &notEmpty);
  {
    moveValue(UndefinedValue(), output);
    jump(&done);
  }

  bind(&notEmpty);

  // Load the first element.
  Address elementAddr(temp1, 0);
  loadValue(elementAddr, output);

  // Move the other elements and update the initializedLength/length. This will
  // also trigger pre-barriers.
  {
    // Ensure output is in volatileRegs. Don't preserve temp1 and temp2.
    volatileRegs.takeUnchecked(temp1);
    volatileRegs.takeUnchecked(temp2);
    if (output.hasVolatileReg()) {
      volatileRegs.addUnchecked(output);
    }

    PushRegsInMask(volatileRegs);

    using Fn = void (*)(ArrayObject* arr);
    setupUnalignedABICall(temp1);
    passABIArg(array);
    callWithABI<Fn, ArrayShiftMoveElements>();

    PopRegsInMask(volatileRegs);
  }

  bind(&done);
}

void MacroAssembler::loadArgumentsObjectElement(Register obj, Register index,
                                                ValueOperand output,
                                                Register temp, Label* fail) {
  Register temp2 = output.scratchReg();

  // Get initial length value.
  unboxInt32(Address(obj, ArgumentsObject::getInitialLengthSlotOffset()), temp);

  // Ensure no overridden elements.
  branchTest32(Assembler::NonZero, temp,
               Imm32(ArgumentsObject::ELEMENT_OVERRIDDEN_BIT), fail);

  // Bounds check.
  rshift32(Imm32(ArgumentsObject::PACKED_BITS_COUNT), temp);
  spectreBoundsCheck32(index, temp, temp2, fail);

  // Load ArgumentsData.
  loadPrivate(Address(obj, ArgumentsObject::getDataSlotOffset()), temp);

  // Guard the argument is not a FORWARD_TO_CALL_SLOT MagicValue.
  BaseValueIndex argValue(temp, index, ArgumentsData::offsetOfArgs());
  branchTestMagic(Assembler::Equal, argValue, fail);
  loadValue(argValue, output);
}

void MacroAssembler::loadArgumentsObjectElementHole(Register obj,
                                                    Register index,
                                                    ValueOperand output,
                                                    Register temp,
                                                    Label* fail) {
  Register temp2 = output.scratchReg();

  // Get initial length value.
  unboxInt32(Address(obj, ArgumentsObject::getInitialLengthSlotOffset()), temp);

  // Ensure no overridden elements.
  branchTest32(Assembler::NonZero, temp,
               Imm32(ArgumentsObject::ELEMENT_OVERRIDDEN_BIT), fail);

  // Bounds check.
  Label outOfBounds, done;
  rshift32(Imm32(ArgumentsObject::PACKED_BITS_COUNT), temp);
  spectreBoundsCheck32(index, temp, temp2, &outOfBounds);

  // Load ArgumentsData.
  loadPrivate(Address(obj, ArgumentsObject::getDataSlotOffset()), temp);

  // Guard the argument is not a FORWARD_TO_CALL_SLOT MagicValue.
  BaseValueIndex argValue(temp, index, ArgumentsData::offsetOfArgs());
  branchTestMagic(Assembler::Equal, argValue, fail);
  loadValue(argValue, output);
  jump(&done);

  bind(&outOfBounds);
  branch32(Assembler::LessThan, index, Imm32(0), fail);
  moveValue(UndefinedValue(), output);

  bind(&done);
}

void MacroAssembler::loadArgumentsObjectElementExists(
    Register obj, Register index, Register output, Register temp, Label* fail) {
  // Ensure the index is non-negative.
  branch32(Assembler::LessThan, index, Imm32(0), fail);

  // Get initial length value.
  unboxInt32(Address(obj, ArgumentsObject::getInitialLengthSlotOffset()), temp);

  // Ensure no overridden or deleted elements.
  branchTest32(Assembler::NonZero, temp,
               Imm32(ArgumentsObject::ELEMENT_OVERRIDDEN_BIT), fail);

  // Compare index against the length.
  rshift32(Imm32(ArgumentsObject::PACKED_BITS_COUNT), temp);
  cmp32Set(Assembler::LessThan, index, temp, output);
}

void MacroAssembler::loadArgumentsObjectLength(Register obj, Register output,
                                               Label* fail) {
  // Get initial length value.
  unboxInt32(Address(obj, ArgumentsObject::getInitialLengthSlotOffset()),
             output);

  // Test if length has been overridden.
  branchTest32(Assembler::NonZero, output,
               Imm32(ArgumentsObject::LENGTH_OVERRIDDEN_BIT), fail);

  // Shift out arguments length and return it.
  rshift32(Imm32(ArgumentsObject::PACKED_BITS_COUNT), output);
}

void MacroAssembler::branchTestArgumentsObjectFlags(Register obj, Register temp,
                                                    uint32_t flags,
                                                    Condition cond,
                                                    Label* label) {
  MOZ_ASSERT((flags & ~ArgumentsObject::PACKED_BITS_MASK) == 0);

  // Get initial length value.
  unboxInt32(Address(obj, ArgumentsObject::getInitialLengthSlotOffset()), temp);

  // Test flags.
  branchTest32(cond, temp, Imm32(flags), label);
}

static constexpr bool ValidateSizeRange(Scalar::Type from, Scalar::Type to) {
  for (Scalar::Type type = from; type < to; type = Scalar::Type(type + 1)) {
    if (TypedArrayElemSize(type) != TypedArrayElemSize(from)) {
      return false;
    }
  }
  return true;
}

void MacroAssembler::typedArrayElementSize(Register obj, Register output) {
  static_assert(Scalar::Int8 == 0, "Int8 is the first typed array class");
  static_assert(
      (Scalar::BigUint64 - Scalar::Int8) == Scalar::MaxTypedArrayViewType - 1,
      "BigUint64 is the last typed array class");

  Label one, two, four, eight, done;

  loadObjClassUnsafe(obj, output);

  static_assert(ValidateSizeRange(Scalar::Int8, Scalar::Int16),
                "element size is one in [Int8, Int16)");
  branchPtr(Assembler::Below, output,
            ImmPtr(TypedArrayObject::classForType(Scalar::Int16)), &one);

  static_assert(ValidateSizeRange(Scalar::Int16, Scalar::Int32),
                "element size is two in [Int16, Int32)");
  branchPtr(Assembler::Below, output,
            ImmPtr(TypedArrayObject::classForType(Scalar::Int32)), &two);

  static_assert(ValidateSizeRange(Scalar::Int32, Scalar::Float64),
                "element size is four in [Int32, Float64)");
  branchPtr(Assembler::Below, output,
            ImmPtr(TypedArrayObject::classForType(Scalar::Float64)), &four);

  static_assert(ValidateSizeRange(Scalar::Float64, Scalar::Uint8Clamped),
                "element size is eight in [Float64, Uint8Clamped)");
  branchPtr(Assembler::Below, output,
            ImmPtr(TypedArrayObject::classForType(Scalar::Uint8Clamped)),
            &eight);

  static_assert(ValidateSizeRange(Scalar::Uint8Clamped, Scalar::BigInt64),
                "element size is one in [Uint8Clamped, BigInt64)");
  branchPtr(Assembler::Below, output,
            ImmPtr(TypedArrayObject::classForType(Scalar::BigInt64)), &one);

  static_assert(
      ValidateSizeRange(Scalar::BigInt64, Scalar::MaxTypedArrayViewType),
      "element size is eight in [BigInt64, MaxTypedArrayViewType)");
  // Fall through for BigInt64 and BigUint64

  bind(&eight);
  move32(Imm32(8), output);
  jump(&done);

  bind(&four);
  move32(Imm32(4), output);
  jump(&done);

  bind(&two);
  move32(Imm32(2), output);
  jump(&done);

  bind(&one);
  move32(Imm32(1), output);

  bind(&done);
}

void MacroAssembler::branchIfClassIsNotTypedArray(Register clasp,
                                                  Label* notTypedArray) {
  static_assert(Scalar::Int8 == 0, "Int8 is the first typed array class");
  const JSClass* firstTypedArrayClass =
      TypedArrayObject::classForType(Scalar::Int8);

  static_assert(
      (Scalar::BigUint64 - Scalar::Int8) == Scalar::MaxTypedArrayViewType - 1,
      "BigUint64 is the last typed array class");
  const JSClass* lastTypedArrayClass =
      TypedArrayObject::classForType(Scalar::BigUint64);

  branchPtr(Assembler::Below, clasp, ImmPtr(firstTypedArrayClass),
            notTypedArray);
  branchPtr(Assembler::Above, clasp, ImmPtr(lastTypedArrayClass),
            notTypedArray);
}

void MacroAssembler::branchIfHasDetachedArrayBuffer(Register obj, Register temp,
                                                    Label* label) {
  // Inline implementation of ArrayBufferViewObject::hasDetachedBuffer().

  // Load obj->elements in temp.
  loadPtr(Address(obj, NativeObject::offsetOfElements()), temp);

  // Shared buffers can't be detached.
  Label done;
  branchTest32(Assembler::NonZero,
               Address(temp, ObjectElements::offsetOfFlags()),
               Imm32(ObjectElements::SHARED_MEMORY), &done);

  // An ArrayBufferView with a null buffer has never had its buffer exposed to
  // become detached.
  fallibleUnboxObject(Address(obj, ArrayBufferViewObject::bufferOffset()), temp,
                      &done);

  // Load the ArrayBuffer flags and branch if the detached flag is set.
  unboxInt32(Address(temp, ArrayBufferObject::offsetOfFlagsSlot()), temp);
  branchTest32(Assembler::NonZero, temp, Imm32(ArrayBufferObject::DETACHED),
               label);

  bind(&done);
}

void MacroAssembler::branchIfNativeIteratorNotReusable(Register ni,
                                                       Label* notReusable) {
  // See NativeIterator::isReusable.
  Address flagsAddr(ni, NativeIterator::offsetOfFlagsAndCount());

#ifdef DEBUG
  Label niIsInitialized;
  branchTest32(Assembler::NonZero, flagsAddr,
               Imm32(NativeIterator::Flags::Initialized), &niIsInitialized);
  assumeUnreachable(
      "Expected a NativeIterator that's been completely "
      "initialized");
  bind(&niIsInitialized);
#endif

  branchTest32(Assembler::NonZero, flagsAddr,
               Imm32(NativeIterator::Flags::NotReusable), notReusable);
}

void MacroAssembler::branchNativeIteratorIndices(Condition cond, Register ni,
                                                 Register temp,
                                                 NativeIteratorIndices kind,
                                                 Label* label) {
  Address iterFlagsAddr(ni, NativeIterator::offsetOfFlagsAndCount());
  load32(iterFlagsAddr, temp);
  and32(Imm32(NativeIterator::IndicesMask), temp);
  uint32_t shiftedKind = uint32_t(kind) << NativeIterator::IndicesShift;
  branch32(cond, temp, Imm32(shiftedKind), label);
}

static void LoadNativeIterator(MacroAssembler& masm, Register obj,
                               Register dest) {
  MOZ_ASSERT(obj != dest);

#ifdef DEBUG
  // Assert we have a PropertyIteratorObject.
  Label ok;
  masm.branchTestObjClass(Assembler::Equal, obj,
                          &PropertyIteratorObject::class_, dest, obj, &ok);
  masm.assumeUnreachable("Expected PropertyIteratorObject!");
  masm.bind(&ok);
#endif

  // Load NativeIterator object.
  Address slotAddr(obj, PropertyIteratorObject::offsetOfIteratorSlot());
  masm.loadPrivate(slotAddr, dest);
}

// The ShapeCachePtr may be used to cache an iterator for for-in. Return that
// iterator in |dest| if:
// - the shape cache pointer exists and stores a native iterator
// - the iterator is reusable
// - the iterated object has no dense elements
// - the shapes of each object on the proto chain of |obj| match the cached
//   shapes
// - the proto chain has no dense elements
// Otherwise, jump to |failure|.
void MacroAssembler::maybeLoadIteratorFromShape(Register obj, Register dest,
                                                Register temp, Register temp2,
                                                Register temp3,
                                                Label* failure) {
  // Register usage:
  // obj: always contains the input object
  // temp: walks the obj->shape->baseshape->proto->shape->... chain
  // temp2: points to the native iterator. Incremented to walk the shapes array.
  // temp3: scratch space
  // dest: stores the resulting PropertyIteratorObject on success

  Label success;
  Register shapeAndProto = temp;
  Register nativeIterator = temp2;

  // Load ShapeCache from shape.
  loadPtr(Address(obj, JSObject::offsetOfShape()), shapeAndProto);
  loadPtr(Address(shapeAndProto, Shape::offsetOfCachePtr()), dest);

  // Check if it's an iterator.
  movePtr(dest, temp3);
  andPtr(Imm32(ShapeCachePtr::MASK), temp3);
  branch32(Assembler::NotEqual, temp3, Imm32(ShapeCachePtr::ITERATOR), failure);

  // If we've cached an iterator, |obj| must be a native object.
#ifdef DEBUG
  Label nonNative;
  branchIfNonNativeObj(obj, temp3, &nonNative);
#endif

  // Verify that |obj| has no dense elements.
  loadPtr(Address(obj, NativeObject::offsetOfElements()), temp3);
  branch32(Assembler::NotEqual,
           Address(temp3, ObjectElements::offsetOfInitializedLength()),
           Imm32(0), failure);

  // Clear tag bits from iterator object. |dest| is now valid.
  // Load the native iterator and verify that it's reusable.
  andPtr(Imm32(~ShapeCachePtr::MASK), dest);
  LoadNativeIterator(*this, dest, nativeIterator);
  branchIfNativeIteratorNotReusable(nativeIterator, failure);

  // We have to compare the shapes in the native iterator with the shapes on the
  // proto chain to ensure the cached iterator is still valid. The shape array
  // always starts at a fixed offset from the base of the NativeIterator, so
  // instead of using an instruction outside the loop to initialize a pointer to
  // the shapes array, we can bake it into the offset and reuse the pointer to
  // the NativeIterator. We add |sizeof(Shape*)| to start at the second shape.
  // (The first shape corresponds to the object itself. We don't have to check
  // it, because we got the iterator via the shape.)
  size_t nativeIteratorProtoShapeOffset =
      NativeIterator::offsetOfFirstShape() + sizeof(Shape*);

  // Loop over the proto chain. At the head of the loop, |shape| is the shape of
  // the current object, and |iteratorShapes| points to the expected shape of
  // its proto.
  Label protoLoop;
  bind(&protoLoop);

  // Load the proto. If the proto is null, then we're done.
  loadPtr(Address(shapeAndProto, Shape::offsetOfBaseShape()), shapeAndProto);
  loadPtr(Address(shapeAndProto, BaseShape::offsetOfProto()), shapeAndProto);
  branchPtr(Assembler::Equal, shapeAndProto, ImmPtr(nullptr), &success);

#ifdef DEBUG
  // We have guarded every shape up until this point, so we know that the proto
  // is a native object.
  branchIfNonNativeObj(shapeAndProto, temp3, &nonNative);
#endif

  // Verify that the proto has no dense elements.
  loadPtr(Address(shapeAndProto, NativeObject::offsetOfElements()), temp3);
  branch32(Assembler::NotEqual,
           Address(temp3, ObjectElements::offsetOfInitializedLength()),
           Imm32(0), failure);

  // Compare the shape of the proto to the expected shape.
  loadPtr(Address(shapeAndProto, JSObject::offsetOfShape()), shapeAndProto);
  loadPtr(Address(nativeIterator, nativeIteratorProtoShapeOffset), temp3);
  branchPtr(Assembler::NotEqual, shapeAndProto, temp3, failure);

  // Increment |iteratorShapes| and jump back to the top of the loop.
  addPtr(Imm32(sizeof(Shape*)), nativeIterator);
  jump(&protoLoop);

#ifdef DEBUG
  bind(&nonNative);
  assumeUnreachable("Expected NativeObject in maybeLoadIteratorFromShape");
#endif

  bind(&success);
}

void MacroAssembler::iteratorMore(Register obj, ValueOperand output,
                                  Register temp) {
  Label done;
  Register outputScratch = output.scratchReg();
  LoadNativeIterator(*this, obj, outputScratch);

  // If propertyCursor_ < propertiesEnd_, load the next string and advance
  // the cursor.  Otherwise return MagicValue(JS_NO_ITER_VALUE).
  Label iterDone;
  Address cursorAddr(outputScratch, NativeIterator::offsetOfPropertyCursor());
  Address cursorEndAddr(outputScratch, NativeIterator::offsetOfPropertiesEnd());
  loadPtr(cursorAddr, temp);
  branchPtr(Assembler::BelowOrEqual, cursorEndAddr, temp, &iterDone);

  // Get next string.
  loadPtr(Address(temp, 0), temp);

  // Increase the cursor.
  addPtr(Imm32(sizeof(GCPtr<JSLinearString*>)), cursorAddr);

  tagValue(JSVAL_TYPE_STRING, temp, output);
  jump(&done);

  bind(&iterDone);
  moveValue(MagicValue(JS_NO_ITER_VALUE), output);

  bind(&done);
}

void MacroAssembler::iteratorClose(Register obj, Register temp1, Register temp2,
                                   Register temp3) {
  LoadNativeIterator(*this, obj, temp1);

  // The shared iterator used for for-in with null/undefined is immutable and
  // unlinked. See NativeIterator::isEmptyIteratorSingleton.
  Label done;
  branchTest32(Assembler::NonZero,
               Address(temp1, NativeIterator::offsetOfFlagsAndCount()),
               Imm32(NativeIterator::Flags::IsEmptyIteratorSingleton), &done);

  // Clear active bit.
  and32(Imm32(~NativeIterator::Flags::Active),
        Address(temp1, NativeIterator::offsetOfFlagsAndCount()));

  // Clear objectBeingIterated.
  Address iterObjAddr(temp1, NativeIterator::offsetOfObjectBeingIterated());
  guardedCallPreBarrierAnyZone(iterObjAddr, MIRType::Object, temp2);
  storePtr(ImmPtr(nullptr), iterObjAddr);

  // Reset property cursor.
  loadPtr(Address(temp1, NativeIterator::offsetOfShapesEnd()), temp2);
  storePtr(temp2, Address(temp1, NativeIterator::offsetOfPropertyCursor()));

  // Unlink from the iterator list.
  const Register next = temp2;
  const Register prev = temp3;
  loadPtr(Address(temp1, NativeIterator::offsetOfNext()), next);
  loadPtr(Address(temp1, NativeIterator::offsetOfPrev()), prev);
  storePtr(prev, Address(next, NativeIterator::offsetOfPrev()));
  storePtr(next, Address(prev, NativeIterator::offsetOfNext()));
#ifdef DEBUG
  storePtr(ImmPtr(nullptr), Address(temp1, NativeIterator::offsetOfNext()));
  storePtr(ImmPtr(nullptr), Address(temp1, NativeIterator::offsetOfPrev()));
#endif

  bind(&done);
}

void MacroAssembler::registerIterator(Register enumeratorsList, Register iter,
                                      Register temp) {
  // iter->next = list
  storePtr(enumeratorsList, Address(iter, NativeIterator::offsetOfNext()));

  // iter->prev = list->prev
  loadPtr(Address(enumeratorsList, NativeIterator::offsetOfPrev()), temp);
  storePtr(temp, Address(iter, NativeIterator::offsetOfPrev()));

  // list->prev->next = iter
  storePtr(iter, Address(temp, NativeIterator::offsetOfNext()));

  // list->prev = iter
  storePtr(iter, Address(enumeratorsList, NativeIterator::offsetOfPrev()));
}

void MacroAssembler::toHashableNonGCThing(ValueOperand value,
                                          ValueOperand result,
                                          FloatRegister tempFloat) {
  // Inline implementation of |HashableValue::setValue()|.

#ifdef DEBUG
  Label ok;
  branchTestGCThing(Assembler::NotEqual, value, &ok);
  assumeUnreachable("Unexpected GC thing");
  bind(&ok);
#endif

  Label useInput, done;
  branchTestDouble(Assembler::NotEqual, value, &useInput);
  {
    Register int32 = result.scratchReg();
    unboxDouble(value, tempFloat);

    // Normalize int32-valued doubles to int32 and negative zero to +0.
    Label canonicalize;
    convertDoubleToInt32(tempFloat, int32, &canonicalize, false);
    {
      tagValue(JSVAL_TYPE_INT32, int32, result);
      jump(&done);
    }
    bind(&canonicalize);
    {
      // Normalize the sign bit of a NaN.
      branchDouble(Assembler::DoubleOrdered, tempFloat, tempFloat, &useInput);
      moveValue(JS::NaNValue(), result);
      jump(&done);
    }
  }

  bind(&useInput);
  moveValue(value, result);

  bind(&done);
}

void MacroAssembler::toHashableValue(ValueOperand value, ValueOperand result,
                                     FloatRegister tempFloat,
                                     Label* atomizeString, Label* tagString) {
  // Inline implementation of |HashableValue::setValue()|.

  ScratchTagScope tag(*this, value);
  splitTagForTest(value, tag);

  Label notString, useInput, done;
  branchTestString(Assembler::NotEqual, tag, &notString);
  {
    ScratchTagScopeRelease _(&tag);

    Register str = result.scratchReg();
    unboxString(value, str);

    branchTest32(Assembler::NonZero, Address(str, JSString::offsetOfFlags()),
                 Imm32(JSString::ATOM_BIT), &useInput);

    jump(atomizeString);
    bind(tagString);

    tagValue(JSVAL_TYPE_STRING, str, result);
    jump(&done);
  }
  bind(&notString);
  branchTestDouble(Assembler::NotEqual, tag, &useInput);
  {
    ScratchTagScopeRelease _(&tag);

    Register int32 = result.scratchReg();
    unboxDouble(value, tempFloat);

    Label canonicalize;
    convertDoubleToInt32(tempFloat, int32, &canonicalize, false);
    {
      tagValue(JSVAL_TYPE_INT32, int32, result);
      jump(&done);
    }
    bind(&canonicalize);
    {
      branchDouble(Assembler::DoubleOrdered, tempFloat, tempFloat, &useInput);
      moveValue(JS::NaNValue(), result);
      jump(&done);
    }
  }

  bind(&useInput);
  moveValue(value, result);

  bind(&done);
}

void MacroAssembler::scrambleHashCode(Register result) {
  // Inline implementation of |mozilla::ScrambleHashCode()|.

  mul32(Imm32(mozilla::kGoldenRatioU32), result);
}

void MacroAssembler::prepareHashNonGCThing(ValueOperand value, Register result,
                                           Register temp) {
  // Inline implementation of |OrderedHashTable::prepareHash()| and
  // |mozilla::HashGeneric(v.asRawBits())|.

#ifdef DEBUG
  Label ok;
  branchTestGCThing(Assembler::NotEqual, value, &ok);
  assumeUnreachable("Unexpected GC thing");
  bind(&ok);
#endif

  // uint32_t v1 = static_cast<uint32_t>(aValue);
#ifdef JS_PUNBOX64
  move64To32(value.toRegister64(), result);
#else
  move32(value.payloadReg(), result);
#endif

  // uint32_t v2 = static_cast<uint32_t>(static_cast<uint64_t>(aValue) >> 32);
#ifdef JS_PUNBOX64
  auto r64 = Register64(temp);
  move64(value.toRegister64(), r64);
  rshift64Arithmetic(Imm32(32), r64);
#else
  // TODO: This seems like a bug in mozilla::detail::AddUintptrToHash().
  // The uint64_t input is first converted to uintptr_t and then back to
  // uint64_t. But |uint64_t(uintptr_t(bits))| actually only clears the high
  // bits, so this computation:
  //
  // aValue = uintptr_t(bits)
  // v2 = static_cast<uint32_t>(static_cast<uint64_t>(aValue) >> 32)
  //
  // really just sets |v2 = 0|. And that means the xor-operation in AddU32ToHash
  // can be optimized away, because |x ^ 0 = x|.
  //
  // Filed as bug 1718516.
#endif

  // mozilla::WrappingMultiply(kGoldenRatioU32, RotateLeft5(aHash) ^ aValue);
  // with |aHash = 0| and |aValue = v1|.
  mul32(Imm32(mozilla::kGoldenRatioU32), result);

  // mozilla::WrappingMultiply(kGoldenRatioU32, RotateLeft5(aHash) ^ aValue);
  // with |aHash = <above hash>| and |aValue = v2|.
  rotateLeft(Imm32(5), result, result);
#ifdef JS_PUNBOX64
  xor32(temp, result);
#endif

  // Combine |mul32| and |scrambleHashCode| by directly multiplying with
  // |kGoldenRatioU32 * kGoldenRatioU32|.
  //
  // mul32(Imm32(mozilla::kGoldenRatioU32), result);
  //
  // scrambleHashCode(result);
  mul32(Imm32(mozilla::kGoldenRatioU32 * mozilla::kGoldenRatioU32), result);
}

void MacroAssembler::prepareHashString(Register str, Register result,
                                       Register temp) {
  // Inline implementation of |OrderedHashTable::prepareHash()| and
  // |JSAtom::hash()|.

#ifdef DEBUG
  Label ok;
  branchTest32(Assembler::NonZero, Address(str, JSString::offsetOfFlags()),
               Imm32(JSString::ATOM_BIT), &ok);
  assumeUnreachable("Unexpected non-atom string");
  bind(&ok);
#endif

  move32(Imm32(JSString::FAT_INLINE_MASK), temp);
  and32(Address(str, JSString::offsetOfFlags()), temp);

  // Set |result| to 1 for FatInlineAtoms.
  move32(Imm32(0), result);
  cmp32Set(Assembler::Equal, temp, Imm32(JSString::FAT_INLINE_MASK), result);

  // Use a computed load for branch-free code.

  static_assert(FatInlineAtom::offsetOfHash() > NormalAtom::offsetOfHash());

  constexpr size_t offsetDiff =
      FatInlineAtom::offsetOfHash() - NormalAtom::offsetOfHash();
  static_assert(mozilla::IsPowerOfTwo(offsetDiff));

  uint8_t shift = mozilla::FloorLog2Size(offsetDiff);
  if (IsShiftInScaleRange(shift)) {
    load32(
        BaseIndex(str, result, ShiftToScale(shift), NormalAtom::offsetOfHash()),
        result);
  } else {
    lshift32(Imm32(shift), result);
    load32(BaseIndex(str, result, TimesOne, NormalAtom::offsetOfHash()),
           result);
  }

  scrambleHashCode(result);
}

void MacroAssembler::prepareHashSymbol(Register sym, Register result) {
  // Inline implementation of |OrderedHashTable::prepareHash()| and
  // |Symbol::hash()|.

  load32(Address(sym, JS::Symbol::offsetOfHash()), result);

  scrambleHashCode(result);
}

void MacroAssembler::prepareHashBigInt(Register bigInt, Register result,
                                       Register temp1, Register temp2,
                                       Register temp3) {
  // Inline implementation of |OrderedHashTable::prepareHash()| and
  // |BigInt::hash()|.

  // Inline implementation of |mozilla::AddU32ToHash()|.
  auto addU32ToHash = [&](auto toAdd) {
    rotateLeft(Imm32(5), result, result);
    xor32(toAdd, result);
    mul32(Imm32(mozilla::kGoldenRatioU32), result);
  };

  move32(Imm32(0), result);

  // Inline |mozilla::HashBytes()|.

  load32(Address(bigInt, BigInt::offsetOfLength()), temp1);
  loadBigIntDigits(bigInt, temp2);

  Label start, loop;
  jump(&start);
  bind(&loop);

  {
    // Compute |AddToHash(AddToHash(hash, data), sizeof(Digit))|.
#if defined(JS_CODEGEN_MIPS64)
    // Hash the lower 32-bits.
    addU32ToHash(Address(temp2, 0));

    // Hash the upper 32-bits.
    addU32ToHash(Address(temp2, sizeof(int32_t)));
#elif JS_PUNBOX64
    // Use a single 64-bit load on non-MIPS64 platforms.
    loadPtr(Address(temp2, 0), temp3);

    // Hash the lower 32-bits.
    addU32ToHash(temp3);

    // Hash the upper 32-bits.
    rshiftPtr(Imm32(32), temp3);
    addU32ToHash(temp3);
#else
    addU32ToHash(Address(temp2, 0));
#endif
  }
  addPtr(Imm32(sizeof(BigInt::Digit)), temp2);

  bind(&start);
  branchSub32(Assembler::NotSigned, Imm32(1), temp1, &loop);

  // Compute |mozilla::AddToHash(h, isNegative())|.
  {
    static_assert(mozilla::IsPowerOfTwo(BigInt::signBitMask()));

    load32(Address(bigInt, BigInt::offsetOfFlags()), temp1);
    and32(Imm32(BigInt::signBitMask()), temp1);
    rshift32(Imm32(mozilla::FloorLog2(BigInt::signBitMask())), temp1);

    addU32ToHash(temp1);
  }

  scrambleHashCode(result);
}

void MacroAssembler::prepareHashObject(Register setObj, ValueOperand value,
                                       Register result, Register temp1,
                                       Register temp2, Register temp3,
                                       Register temp4) {
#ifdef JS_PUNBOX64
  // Inline implementation of |OrderedHashTable::prepareHash()| and
  // |HashCodeScrambler::scramble(v.asRawBits())|.

  // Load the |ValueSet| or |ValueMap|.
  static_assert(SetObject::getDataSlotOffset() ==
                MapObject::getDataSlotOffset());
  loadPrivate(Address(setObj, SetObject::getDataSlotOffset()), temp1);

  // Load |HashCodeScrambler::mK0| and |HashCodeScrambler::mK0|.
  static_assert(ValueSet::offsetOfImplHcsK0() == ValueMap::offsetOfImplHcsK0());
  static_assert(ValueSet::offsetOfImplHcsK1() == ValueMap::offsetOfImplHcsK1());
  auto k0 = Register64(temp1);
  auto k1 = Register64(temp2);
  load64(Address(temp1, ValueSet::offsetOfImplHcsK1()), k1);
  load64(Address(temp1, ValueSet::offsetOfImplHcsK0()), k0);

  // Hash numbers are 32-bit values, so only hash the lower double-word.
  static_assert(sizeof(mozilla::HashNumber) == 4);
  move32To64ZeroExtend(value.valueReg(), Register64(result));

  // Inline implementation of |SipHasher::sipHash()|.
  auto m = Register64(result);
  auto v0 = Register64(temp3);
  auto v1 = Register64(temp4);
  auto v2 = k0;
  auto v3 = k1;

  auto sipRound = [&]() {
    // mV0 = WrappingAdd(mV0, mV1);
    add64(v1, v0);

    // mV1 = RotateLeft(mV1, 13);
    rotateLeft64(Imm32(13), v1, v1, InvalidReg);

    // mV1 ^= mV0;
    xor64(v0, v1);

    // mV0 = RotateLeft(mV0, 32);
    rotateLeft64(Imm32(32), v0, v0, InvalidReg);

    // mV2 = WrappingAdd(mV2, mV3);
    add64(v3, v2);

    // mV3 = RotateLeft(mV3, 16);
    rotateLeft64(Imm32(16), v3, v3, InvalidReg);

    // mV3 ^= mV2;
    xor64(v2, v3);

    // mV0 = WrappingAdd(mV0, mV3);
    add64(v3, v0);

    // mV3 = RotateLeft(mV3, 21);
    rotateLeft64(Imm32(21), v3, v3, InvalidReg);

    // mV3 ^= mV0;
    xor64(v0, v3);

    // mV2 = WrappingAdd(mV2, mV1);
    add64(v1, v2);

    // mV1 = RotateLeft(mV1, 17);
    rotateLeft64(Imm32(17), v1, v1, InvalidReg);

    // mV1 ^= mV2;
    xor64(v2, v1);

    // mV2 = RotateLeft(mV2, 32);
    rotateLeft64(Imm32(32), v2, v2, InvalidReg);
  };

  // 1. Initialization.
  // mV0 = aK0 ^ UINT64_C(0x736f6d6570736575);
  move64(Imm64(0x736f6d6570736575), v0);
  xor64(k0, v0);

  // mV1 = aK1 ^ UINT64_C(0x646f72616e646f6d);
  move64(Imm64(0x646f72616e646f6d), v1);
  xor64(k1, v1);

  // mV2 = aK0 ^ UINT64_C(0x6c7967656e657261);
  MOZ_ASSERT(v2 == k0);
  xor64(Imm64(0x6c7967656e657261), v2);

  // mV3 = aK1 ^ UINT64_C(0x7465646279746573);
  MOZ_ASSERT(v3 == k1);
  xor64(Imm64(0x7465646279746573), v3);

  // 2. Compression.
  // mV3 ^= aM;
  xor64(m, v3);

  // sipRound();
  sipRound();

  // mV0 ^= aM;
  xor64(m, v0);

  // 3. Finalization.
  // mV2 ^= 0xff;
  xor64(Imm64(0xff), v2);

  // for (int i = 0; i < 3; i++) sipRound();
  for (int i = 0; i < 3; i++) {
    sipRound();
  }

  // return mV0 ^ mV1 ^ mV2 ^ mV3;
  xor64(v1, v0);
  xor64(v2, v3);
  xor64(v3, v0);

  move64To32(v0, result);

  scrambleHashCode(result);
#else
  MOZ_CRASH("Not implemented");
#endif
}

void MacroAssembler::prepareHashValue(Register setObj, ValueOperand value,
                                      Register result, Register temp1,
                                      Register temp2, Register temp3,
                                      Register temp4) {
  Label isString, isObject, isSymbol, isBigInt;
  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);

    branchTestString(Assembler::Equal, tag, &isString);
    branchTestObject(Assembler::Equal, tag, &isObject);
    branchTestSymbol(Assembler::Equal, tag, &isSymbol);
    branchTestBigInt(Assembler::Equal, tag, &isBigInt);
  }

  Label done;
  {
    prepareHashNonGCThing(value, result, temp1);
    jump(&done);
  }
  bind(&isString);
  {
    unboxString(value, temp1);
    prepareHashString(temp1, result, temp2);
    jump(&done);
  }
  bind(&isObject);
  {
    prepareHashObject(setObj, value, result, temp1, temp2, temp3, temp4);
    jump(&done);
  }
  bind(&isSymbol);
  {
    unboxSymbol(value, temp1);
    prepareHashSymbol(temp1, result);
    jump(&done);
  }
  bind(&isBigInt);
  {
    unboxBigInt(value, temp1);
    prepareHashBigInt(temp1, result, temp2, temp3, temp4);

    // Fallthrough to |done|.
  }

  bind(&done);
}

template <typename OrderedHashTable>
void MacroAssembler::orderedHashTableLookup(Register setOrMapObj,
                                            ValueOperand value, Register hash,
                                            Register entryTemp, Register temp1,
                                            Register temp2, Register temp3,
                                            Register temp4, Label* found,
                                            IsBigInt isBigInt) {
  // Inline implementation of |OrderedHashTable::lookup()|.

  MOZ_ASSERT_IF(isBigInt == IsBigInt::No, temp3 == InvalidReg);
  MOZ_ASSERT_IF(isBigInt == IsBigInt::No, temp4 == InvalidReg);

#ifdef DEBUG
  Label ok;
  if (isBigInt == IsBigInt::No) {
    branchTestBigInt(Assembler::NotEqual, value, &ok);
    assumeUnreachable("Unexpected BigInt");
  } else if (isBigInt == IsBigInt::Yes) {
    branchTestBigInt(Assembler::Equal, value, &ok);
    assumeUnreachable("Unexpected non-BigInt");
  }
  bind(&ok);
#endif

#ifdef DEBUG
  PushRegsInMask(LiveRegisterSet(RegisterSet::Volatile()));

  pushValue(value);
  moveStackPtrTo(temp2);

  setupUnalignedABICall(temp1);
  loadJSContext(temp1);
  passABIArg(temp1);
  passABIArg(setOrMapObj);
  passABIArg(temp2);
  passABIArg(hash);

  if constexpr (std::is_same_v<OrderedHashTable, ValueSet>) {
    using Fn =
        void (*)(JSContext*, SetObject*, const Value*, mozilla::HashNumber);
    callWithABI<Fn, jit::AssertSetObjectHash>();
  } else {
    using Fn =
        void (*)(JSContext*, MapObject*, const Value*, mozilla::HashNumber);
    callWithABI<Fn, jit::AssertMapObjectHash>();
  }

  popValue(value);
  PopRegsInMask(LiveRegisterSet(RegisterSet::Volatile()));
#endif

  // Load the |ValueSet| or |ValueMap|.
  static_assert(SetObject::getDataSlotOffset() ==
                MapObject::getDataSlotOffset());
  loadPrivate(Address(setOrMapObj, SetObject::getDataSlotOffset()), temp1);

  // Load the bucket.
  move32(hash, entryTemp);
  load32(Address(temp1, OrderedHashTable::offsetOfImplHashShift()), temp2);
  flexibleRshift32(temp2, entryTemp);

  loadPtr(Address(temp1, OrderedHashTable::offsetOfImplHashTable()), temp2);
  loadPtr(BaseIndex(temp2, entryTemp, ScalePointer), entryTemp);

  // Search for a match in this bucket.
  Label start, loop;
  jump(&start);
  bind(&loop);
  {
    // Inline implementation of |HashableValue::operator==|.

    static_assert(OrderedHashTable::offsetOfImplDataElement() == 0,
                  "offsetof(Data, element) is 0");
    auto keyAddr = Address(entryTemp, OrderedHashTable::offsetOfEntryKey());

    if (isBigInt == IsBigInt::No) {
      // Two HashableValues are equal if they have equal bits.
      branch64(Assembler::Equal, keyAddr, value.toRegister64(), found);
    } else {
#ifdef JS_PUNBOX64
      auto key = ValueOperand(temp1);
#else
      auto key = ValueOperand(temp1, temp2);
#endif

      loadValue(keyAddr, key);

      // Two HashableValues are equal if they have equal bits.
      branch64(Assembler::Equal, key.toRegister64(), value.toRegister64(),
               found);

      // BigInt values are considered equal if they represent the same
      // mathematical value.
      Label next;
      fallibleUnboxBigInt(key, temp2, &next);
      if (isBigInt == IsBigInt::Yes) {
        unboxBigInt(value, temp1);
      } else {
        fallibleUnboxBigInt(value, temp1, &next);
      }
      equalBigInts(temp1, temp2, temp3, temp4, temp1, temp2, &next, &next,
                   &next);
      jump(found);
      bind(&next);
    }
  }
  loadPtr(Address(entryTemp, OrderedHashTable::offsetOfImplDataChain()),
          entryTemp);
  bind(&start);
  branchTestPtr(Assembler::NonZero, entryTemp, entryTemp, &loop);
}

void MacroAssembler::setObjectHas(Register setObj, ValueOperand value,
                                  Register hash, Register result,
                                  Register temp1, Register temp2,
                                  Register temp3, Register temp4,
                                  IsBigInt isBigInt) {
  Label found;
  orderedHashTableLookup<ValueSet>(setObj, value, hash, result, temp1, temp2,
                                   temp3, temp4, &found, isBigInt);

  Label done;
  move32(Imm32(0), result);
  jump(&done);

  bind(&found);
  move32(Imm32(1), result);
  bind(&done);
}

void MacroAssembler::mapObjectHas(Register mapObj, ValueOperand value,
                                  Register hash, Register result,
                                  Register temp1, Register temp2,
                                  Register temp3, Register temp4,
                                  IsBigInt isBigInt) {
  Label found;
  orderedHashTableLookup<ValueMap>(mapObj, value, hash, result, temp1, temp2,
                                   temp3, temp4, &found, isBigInt);

  Label done;
  move32(Imm32(0), result);
  jump(&done);

  bind(&found);
  move32(Imm32(1), result);
  bind(&done);
}

void MacroAssembler::mapObjectGet(Register mapObj, ValueOperand value,
                                  Register hash, ValueOperand result,
                                  Register temp1, Register temp2,
                                  Register temp3, Register temp4,
                                  Register temp5, IsBigInt isBigInt) {
  Label found;
  orderedHashTableLookup<ValueMap>(mapObj, value, hash, temp1, temp2, temp3,
                                   temp4, temp5, &found, isBigInt);

  Label done;
  moveValue(UndefinedValue(), result);
  jump(&done);

  // |temp1| holds the found entry.
  bind(&found);
  loadValue(Address(temp1, ValueMap::Entry::offsetOfValue()), result);

  bind(&done);
}

template <typename OrderedHashTable>
void MacroAssembler::loadOrderedHashTableCount(Register setOrMapObj,
                                               Register result) {
  // Inline implementation of |OrderedHashTable::count()|.

  // Load the |ValueSet| or |ValueMap|.
  static_assert(SetObject::getDataSlotOffset() ==
                MapObject::getDataSlotOffset());
  loadPrivate(Address(setOrMapObj, SetObject::getDataSlotOffset()), result);

  // Load the live count.
  load32(Address(result, OrderedHashTable::offsetOfImplLiveCount()), result);
}

void MacroAssembler::loadSetObjectSize(Register setObj, Register result) {
  loadOrderedHashTableCount<ValueSet>(setObj, result);
}

void MacroAssembler::loadMapObjectSize(Register mapObj, Register result) {
  loadOrderedHashTableCount<ValueMap>(mapObj, result);
}

// Can't push large frames blindly on windows, so we must touch frame memory
// incrementally, with no more than 4096 - 1 bytes between touches.
//
// This is used across all platforms for simplicity.
void MacroAssembler::touchFrameValues(Register numStackValues,
                                      Register scratch1, Register scratch2) {
  const size_t FRAME_TOUCH_INCREMENT = 2048;
  static_assert(FRAME_TOUCH_INCREMENT < 4096 - 1,
                "Frame increment is too large");

  moveStackPtrTo(scratch2);

  mov(numStackValues, scratch1);
  lshiftPtr(Imm32(3), scratch1);
  {
    // Note: this loop needs to update the stack pointer register because older
    // Linux kernels check the distance between the touched address and RSP.
    // See bug 1839669 comment 47.
    Label touchFrameLoop;
    Label touchFrameLoopEnd;
    bind(&touchFrameLoop);
    branchSub32(Assembler::Signed, Imm32(FRAME_TOUCH_INCREMENT), scratch1,
                &touchFrameLoopEnd);
    subFromStackPtr(Imm32(FRAME_TOUCH_INCREMENT));
    store32(Imm32(0), Address(getStackPointer(), 0));
    jump(&touchFrameLoop);
    bind(&touchFrameLoopEnd);
  }

  moveToStackPtr(scratch2);
}

namespace js {
namespace jit {

#ifdef DEBUG
template <class RegisterType>
AutoGenericRegisterScope<RegisterType>::AutoGenericRegisterScope(
    MacroAssembler& masm, RegisterType reg)
    : RegisterType(reg), masm_(masm), released_(false) {
  masm.debugTrackedRegisters_.add(reg);
}

template AutoGenericRegisterScope<Register>::AutoGenericRegisterScope(
    MacroAssembler& masm, Register reg);
template AutoGenericRegisterScope<FloatRegister>::AutoGenericRegisterScope(
    MacroAssembler& masm, FloatRegister reg);
#endif  // DEBUG

#ifdef DEBUG
template <class RegisterType>
AutoGenericRegisterScope<RegisterType>::~AutoGenericRegisterScope() {
  if (!released_) {
    release();
  }
}

template AutoGenericRegisterScope<Register>::~AutoGenericRegisterScope();
template AutoGenericRegisterScope<FloatRegister>::~AutoGenericRegisterScope();

template <class RegisterType>
void AutoGenericRegisterScope<RegisterType>::release() {
  MOZ_ASSERT(!released_);
  released_ = true;
  const RegisterType& reg = *dynamic_cast<RegisterType*>(this);
  masm_.debugTrackedRegisters_.take(reg);
}

template void AutoGenericRegisterScope<Register>::release();
template void AutoGenericRegisterScope<FloatRegister>::release();

template <class RegisterType>
void AutoGenericRegisterScope<RegisterType>::reacquire() {
  MOZ_ASSERT(released_);
  released_ = false;
  const RegisterType& reg = *dynamic_cast<RegisterType*>(this);
  masm_.debugTrackedRegisters_.add(reg);
}

template void AutoGenericRegisterScope<Register>::reacquire();
template void AutoGenericRegisterScope<FloatRegister>::reacquire();

#endif  // DEBUG

}  // namespace jit

}  // namespace js
