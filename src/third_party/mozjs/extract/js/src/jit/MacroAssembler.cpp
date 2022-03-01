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

#include "jsfriendapi.h"

#include "gc/GCProbes.h"
#include "jit/ABIFunctions.h"
#include "jit/AtomicOp.h"
#include "jit/AtomicOperations.h"
#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/JitFrames.h"
#include "jit/JitOptions.h"
#include "jit/JitRuntime.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"
#include "jit/MoveEmitter.h"
#include "jit/SharedICHelpers.h"
#include "jit/SharedICRegisters.h"
#include "jit/Simulator.h"
#include "js/Conversions.h"
#include "js/friend/DOMProxy.h"  // JS::ExpandoAndGeneration
#include "js/ScalarType.h"       // js::Scalar::Type
#include "vm/ArgumentsObject.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/JSContext.h"
#include "vm/TraceLogging.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmTypes.h"
#include "wasm/WasmValidate.h"

#include "gc/Nursery-inl.h"
#include "jit/ABIFunctionList-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
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
  const JitRuntime* rt = GetJitContext()->runtime->jitRuntime();
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
  const uint32_t* ptrZealModeBits =
      GetJitContext()->runtime->addressOfGCZealModeBits();
  branch32(Assembler::NotEqual, AbsoluteAddress(ptrZealModeBits), Imm32(0),
           fail);
#endif

  // Don't execute the inline path if the realm has an object metadata callback,
  // as the metadata to use for the object may vary between executions of the
  // op.
  if (GetJitContext()->realm()->hasAllocationMetadataBuilder()) {
    jump(fail);
  }
}

bool MacroAssembler::shouldNurseryAllocate(gc::AllocKind allocKind,
                                           gc::InitialHeap initialHeap) {
  // Note that Ion elides barriers on writes to objects known to be in the
  // nursery, so any allocation that can be made into the nursery must be made
  // into the nursery, even if the nursery is disabled. At runtime these will
  // take the out-of-line path, which is required to insert a barrier for the
  // initializing writes.
  return IsNurseryAllocable(allocKind) && initialHeap != gc::TenuredHeap;
}

// Inline version of Nursery::allocateObject. If the object has dynamic slots,
// this fills in the slots_ pointer.
void MacroAssembler::nurseryAllocateObject(Register result, Register temp,
                                           gc::AllocKind allocKind,
                                           size_t nDynamicSlots, Label* fail,
                                           const AllocSiteInput& allocSite) {
  MOZ_ASSERT(IsNurseryAllocable(allocKind));

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
    branch32(Assembler::Equal, Address(site, gc::AllocSite::offsetOfState()),
             Imm32(int32_t(gc::AllocSite::State::LongLived)), fail);
  }

  // No explicit check for nursery.isEnabled() is needed, as the comparison
  // with the nursery's end will always fail in such cases.
  CompileZone* zone = GetJitContext()->realm()->zone();
  size_t thingSize = gc::Arena::thingSize(allocKind);
  size_t totalSize = thingSize + ObjectSlots::allocSize(nDynamicSlots);
  MOZ_ASSERT(totalSize < INT32_MAX);
  MOZ_ASSERT(totalSize % gc::CellAlignBytes == 0);

  bumpPointerAllocate(result, temp, fail, zone,
                      zone->addressOfNurseryPosition(),
                      zone->addressOfNurseryCurrentEnd(), JS::TraceKind::Object,
                      totalSize, allocSite);

  if (nDynamicSlots) {
    store32(Imm32(nDynamicSlots),
            Address(result, thingSize + ObjectSlots::offsetOfCapacity()));
    store32(
        Imm32(0),
        Address(result, thingSize + ObjectSlots::offsetOfDictionarySlotSpan()));
    computeEffectiveAddress(
        Address(result, thingSize + ObjectSlots::offsetOfSlots()), temp);
    storePtr(temp, Address(result, NativeObject::offsetOfSlots()));
  }
}

// Inlined version of FreeSpan::allocate. This does not fill in slots_.
void MacroAssembler::freeListAllocate(Register result, Register temp,
                                      gc::AllocKind allocKind, Label* fail) {
  CompileZone* zone = GetJitContext()->realm()->zone();
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

  if (GetJitContext()->runtime->geckoProfiler().enabled()) {
    uint32_t* countAddress =
        GetJitContext()->runtime->addressOfTenuredAllocCount();
    movePtr(ImmPtr(countAddress), temp);
    add32(Imm32(1), Address(temp, 0));
  }
}

void MacroAssembler::callFreeStub(Register slots) {
  // This register must match the one in JitRuntime::generateFreeStub.
  const Register regSlots = CallTempReg0;

  push(regSlots);
  movePtr(slots, regSlots);
  call(GetJitContext()->runtime->jitRuntime()->freeStub());
  pop(regSlots);
}

// Inlined equivalent of gc::AllocateObject, without failure case handling.
void MacroAssembler::allocateObject(Register result, Register temp,
                                    gc::AllocKind allocKind,
                                    uint32_t nDynamicSlots,
                                    gc::InitialHeap initialHeap, Label* fail,
                                    const AllocSiteInput& allocSite) {
  MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));

  checkAllocatorState(fail);

  if (shouldNurseryAllocate(allocKind, initialHeap)) {
    MOZ_ASSERT(initialHeap == gc::DefaultHeap);
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
                                    gc::InitialHeap initialHeap, Label* fail,
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
    gc::InitialHeap initialHeap, Label* fail, const AllocSiteInput& allocSite) {
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
  fillSlotsWithUndefined(Address(result, NativeObject::getFixedSlotOffset(0)),
                         temp, 0, numFixedSlots);

  // Initialize dynamic slots.
  if (numDynamicSlots > 0) {
    loadPtr(Address(result, NativeObject::offsetOfSlots()), temp2);
    fillSlotsWithUndefined(Address(temp2, 0), temp, 0, numDynamicSlots);
  }
}

void MacroAssembler::createArrayWithFixedElements(
    Register result, Register shape, Register temp, uint32_t arrayLength,
    uint32_t arrayCapacity, gc::AllocKind allocKind,
    gc::InitialHeap initialHeap, Label* fail, const AllocSiteInput& allocSite) {
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
  store32(Imm32(0), Address(temp, ObjectElements::offsetOfFlags()));
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

  CompileZone* zone = GetJitContext()->realm()->zone();
  uint64_t* allocStrsPtr = &zone->zone()->nurseryAllocatedStrings.ref();
  inc64(AbsoluteAddress(allocStrsPtr));
  size_t thingSize = gc::Arena::thingSize(allocKind);

  bumpPointerAllocate(result, temp, fail, zone,
                      zone->addressOfStringNurseryPosition(),
                      zone->addressOfStringNurseryCurrentEnd(),
                      JS::TraceKind::String, thingSize);
}

// Inline version of Nursery::allocateBigInt.
void MacroAssembler::nurseryAllocateBigInt(Register result, Register temp,
                                           Label* fail) {
  MOZ_ASSERT(IsNurseryAllocable(gc::AllocKind::BIGINT));

  // No explicit check for nursery.isEnabled() is needed, as the comparison
  // with the nursery's end will always fail in such cases.

  CompileZone* zone = GetJitContext()->realm()->zone();
  size_t thingSize = gc::Arena::thingSize(gc::AllocKind::BIGINT);

  bumpPointerAllocate(result, temp, fail, zone,
                      zone->addressOfBigIntNurseryPosition(),
                      zone->addressOfBigIntNurseryCurrentEnd(),
                      JS::TraceKind::BigInt, thingSize);
}

void MacroAssembler::bumpPointerAllocate(Register result, Register temp,
                                         Label* fail, CompileZone* zone,
                                         void* posAddr, const void* curEndAddr,
                                         JS::TraceKind traceKind, uint32_t size,
                                         const AllocSiteInput& allocSite) {
  uint32_t totalSize = size + Nursery::nurseryCellHeaderSize();
  MOZ_ASSERT(totalSize < INT32_MAX, "Nursery allocation too large");
  MOZ_ASSERT(totalSize % gc::CellAlignBytes == 0);

  // The position (allocation pointer) and the end pointer are stored
  // very close to each other -- specifically, easily within a 32 bit offset.
  // Use relative offsets between them, to avoid 64-bit immediate loads.
  //
  // I tried to optimise this further by using an extra register to avoid
  // the final subtraction and hopefully get some more instruction
  // parallelism, but it made no difference.
  movePtr(ImmPtr(posAddr), temp);
  loadPtr(Address(temp, 0), result);
  addPtr(Imm32(totalSize), result);
  CheckedInt<int32_t> endOffset =
      (CheckedInt<uintptr_t>(uintptr_t(curEndAddr)) -
       CheckedInt<uintptr_t>(uintptr_t(posAddr)))
          .toChecked<int32_t>();
  MOZ_ASSERT(endOffset.isValid(), "Position and end pointers must be nearby");
  branchPtr(Assembler::Below, Address(temp, endOffset.value()), result, fail);
  storePtr(result, Address(temp, 0));
  subPtr(Imm32(size), result);

  if (GetJitContext()->runtime->geckoProfiler().enabled()) {
    uint32_t* countAddress = zone->addressOfNurseryAllocCount();
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

  if (allocSite.is<gc::CatchAllAllocSite>()) {
    // No allocation site supplied. This is the case when called from Warp, or
    // from places that don't support pretenuring.
    gc::CatchAllAllocSite siteKind = allocSite.as<gc::CatchAllAllocSite>();
    storePtr(ImmWord(zone->nurseryCellHeader(traceKind, siteKind)),
             Address(result, -js::Nursery::nurseryCellHeaderSize()));
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

  branchPtr(Assembler::NotEqual,
            Address(site, gc::AllocSite::offsetOfNextNurseryAllocated()),
            ImmPtr(nullptr), &done);

  loadPtr(AbsoluteAddress(zone->addressOfNurseryAllocatedSites()), temp);
  storePtr(temp, Address(site, gc::AllocSite::offsetOfNextNurseryAllocated()));
  storePtr(site, AbsoluteAddress(zone->addressOfNurseryAllocatedSites()));

  bind(&done);
}

// Inlined equivalent of gc::AllocateString, jumping to fail if nursery
// allocation requested but unsuccessful.
void MacroAssembler::allocateString(Register result, Register temp,
                                    gc::AllocKind allocKind,
                                    gc::InitialHeap initialHeap, Label* fail) {
  MOZ_ASSERT(allocKind == gc::AllocKind::STRING ||
             allocKind == gc::AllocKind::FAT_INLINE_STRING);

  checkAllocatorState(fail);

  if (shouldNurseryAllocate(allocKind, initialHeap)) {
    MOZ_ASSERT(initialHeap == gc::DefaultHeap);
    return nurseryAllocateString(result, temp, allocKind, fail);
  }

  freeListAllocate(result, temp, allocKind, fail);
}

void MacroAssembler::newGCString(Register result, Register temp, Label* fail,
                                 bool attemptNursery) {
  allocateString(result, temp, js::gc::AllocKind::STRING,
                 attemptNursery ? gc::DefaultHeap : gc::TenuredHeap, fail);
}

void MacroAssembler::newGCFatInlineString(Register result, Register temp,
                                          Label* fail, bool attemptNursery) {
  allocateString(result, temp, js::gc::AllocKind::FAT_INLINE_STRING,
                 attemptNursery ? gc::DefaultHeap : gc::TenuredHeap, fail);
}

void MacroAssembler::newGCBigInt(Register result, Register temp, Label* fail,
                                 bool attemptNursery) {
  checkAllocatorState(fail);

  gc::InitialHeap initialHeap =
      attemptNursery ? gc::DefaultHeap : gc::TenuredHeap;
  if (shouldNurseryAllocate(gc::AllocKind::BIGINT, initialHeap)) {
    MOZ_ASSERT(initialHeap == gc::DefaultHeap);
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
  for (unsigned i = start; i < end; ++i, addr.offset += sizeof(GCPtrValue)) {
    store32(temp, ToPayload(addr));
  }

  addr = base;
  move32(Imm32(v.toNunboxTag()), temp);
  for (unsigned i = start; i < end; ++i, addr.offset += sizeof(GCPtrValue)) {
    store32(temp, ToType(addr));
  }
#else
  moveValue(v, ValueOperand(temp));
  for (uint32_t i = start; i < end; ++i, base.offset += sizeof(GCPtrValue)) {
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

static void FindStartOfUninitializedAndUndefinedSlots(
    const TemplateNativeObject& templateObj, uint32_t nslots,
    uint32_t* startOfUninitialized, uint32_t* startOfUndefined) {
  MOZ_ASSERT(nslots == templateObj.slotSpan());
  MOZ_ASSERT(nslots > 0);

  uint32_t first = nslots;
  for (; first != 0; --first) {
    if (templateObj.getSlot(first - 1) != UndefinedValue()) {
      break;
    }
  }
  *startOfUndefined = first;

  if (first != 0 && IsUninitializedLexical(templateObj.getSlot(first - 1))) {
    for (; first != 0; --first) {
      if (!IsUninitializedLexical(templateObj.getSlot(first - 1))) {
        break;
      }
    }
    *startOfUninitialized = first;
  } else {
    *startOfUninitialized = *startOfUndefined;
  }
}

template <typename Src>
inline void MacroAssembler::storeObjPrivate(Src ptr, const Address& address) {
  // The private pointer is stored as a PrivateValue in a JS::Value, so on 32
  // bit systems we also need to zero the top word.
#ifdef JS_PUNBOX64
  storePtr(ptr, address);
#else
  storePtr(ptr, LowWord(address));
  store32(Imm32(0), HighWord(address));
#endif
}

void MacroAssembler::initTypedArraySlots(Register obj, Register temp,
                                         Register lengthReg,
                                         LiveRegisterSet liveRegs, Label* fail,
                                         TypedArrayObject* templateObj,
                                         TypedArrayLength lengthKind) {
  MOZ_ASSERT(templateObj->hasPrivate());
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
    storeObjPrivate(temp, Address(obj, dataSlotOffset));

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

    // Allocate a buffer on the heap to store the data elements.
    liveRegs.addUnchecked(temp);
    liveRegs.addUnchecked(obj);
    liveRegs.addUnchecked(lengthReg);
    PushRegsInMask(liveRegs);
    using Fn = void (*)(JSContext * cx, TypedArrayObject * obj, int32_t count);
    setupUnalignedABICall(temp);
    loadJSContext(temp);
    passABIArg(temp);
    passABIArg(obj);
    passABIArg(lengthReg);
    callWithABI<Fn, AllocateAndInitTypedArrayBuffer>();
    PopRegsInMask(liveRegs);

    // Fail when data elements is set to NULL.
    branchPtr(Assembler::Equal, Address(obj, dataSlotOffset), ImmWord(0), fail);
  }
}

void MacroAssembler::initGCSlots(Register obj, Register temp,
                                 const TemplateNativeObject& templateObj,
                                 bool initContents) {
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
  // slots. Unitialized lexical slots appears in CallObjects if the function
  // has parameter expressions, in which case closed over parameters have
  // TDZ. Uninitialized slots come before undefined slots in CallObjects.
  uint32_t startOfUninitialized = nslots;
  uint32_t startOfUndefined = nslots;
  FindStartOfUninitializedAndUndefinedSlots(
      templateObj, nslots, &startOfUninitialized, &startOfUndefined);
  MOZ_ASSERT(startOfUninitialized <= nfixed);  // Reserved slots must be fixed.
  MOZ_ASSERT(startOfUndefined >= startOfUninitialized);
  MOZ_ASSERT_IF(!templateObj.isCallObject(),
                startOfUninitialized == startOfUndefined);

  // Copy over any preserved reserved slots.
  copySlotsFromTemplate(obj, templateObj, 0, startOfUninitialized);

  // Fill the rest of the fixed slots with undefined and uninitialized.
  if (initContents) {
    size_t offset = NativeObject::getFixedSlotOffset(startOfUninitialized);
    fillSlotsWithUninitialized(Address(obj, offset), temp, startOfUninitialized,
                               std::min(startOfUndefined, nfixed));

    if (startOfUndefined < nfixed) {
      offset = NativeObject::getFixedSlotOffset(startOfUndefined);
      fillSlotsWithUndefined(Address(obj, offset), temp, startOfUndefined,
                             nfixed);
    }
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
    if (!ntemplate.hasDynamicSlots()) {
      storePtr(ImmPtr(emptyObjectSlots),
               Address(obj, NativeObject::offsetOfSlots()));
    }

    if (ntemplate.isArrayObject()) {
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
      store32(Imm32(0),
              Address(obj, elementsOffset + ObjectElements::offsetOfFlags()));
      MOZ_ASSERT(!ntemplate.hasPrivate());
    } else if (ntemplate.isArgumentsObject()) {
      // The caller will initialize the reserved slots.
      MOZ_ASSERT(!initContents);
      MOZ_ASSERT(!ntemplate.hasPrivate());
      storePtr(ImmPtr(emptyObjectElements),
               Address(obj, NativeObject::offsetOfElements()));
    } else {
      // If the target type could be a TypedArray that maps shared memory
      // then this would need to store emptyObjectElementsShared in that case.
      MOZ_ASSERT(!ntemplate.isSharedMemory());

      storePtr(ImmPtr(emptyObjectElements),
               Address(obj, NativeObject::offsetOfElements()));

      initGCSlots(obj, temp, ntemplate, initContents);

      if (ntemplate.hasPrivate() && !ntemplate.isTypedArrayObject()) {
        uint32_t nfixed = ntemplate.numFixedSlots();
        Address privateSlot(obj, NativeObject::getPrivateDataOffset(nfixed));
        storeObjPrivate(ImmPtr(ntemplate.getPrivate()), privateSlot);
      }
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

  using Fn = void (*)(JSObject * obj);
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

void MacroAssembler::loadStringChar(Register str, Register index,
                                    Register output, Register scratch,
                                    Label* fail) {
  MOZ_ASSERT(str != output);
  MOZ_ASSERT(str != index);
  MOZ_ASSERT(index != output);
  MOZ_ASSERT(output != scratch);

  movePtr(str, output);

  // This follows JSString::getChar.
  Label notRope;
  branchIfNotRope(str, &notRope);

  loadRopeLeftChild(str, output);

  // Check if the index is contained in the leftChild.
  // Todo: Handle index in the rightChild.
  spectreBoundsCheck32(index, Address(output, JSString::offsetOfLength()),
                       scratch, fail);

  // If the left side is another rope, give up.
  branchIfRope(output, fail);

  bind(&notRope);

  Label isLatin1, done;
  // We have to check the left/right side for ropes,
  // because a TwoByte rope might have a Latin1 child.
  branchLatin1String(output, &isLatin1);
  loadStringChars(output, scratch, CharEncoding::TwoByte);
  loadChar(scratch, index, output, CharEncoding::TwoByte);
  jump(&done);

  bind(&isLatin1);
  loadStringChars(output, scratch, CharEncoding::Latin1);
  loadChar(scratch, index, output, CharEncoding::Latin1);

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
                                                Register temp, Label* fail,
                                                bool attemptNursery) {
  branch32(Assembler::Above, Address(src, BigInt::offsetOfLength()),
           Imm32(int32_t(BigInt::inlineDigitsLength())), fail);

  newGCBigInt(dest, temp, fail, attemptNursery);

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

void MacroAssembler::typeOfObject(Register obj, Register scratch, Label* slow,
                                  Label* isObject, Label* isCallable,
                                  Label* isUndefined) {
  loadObjClassUnsafe(obj, scratch);

  // Proxies can emulate undefined and have complex isCallable behavior.
  branchTestClassIsProxy(true, scratch, slow);

  // JSFunctions are always callable.
  branchPtr(Assembler::Equal, scratch, ImmPtr(&JSFunction::class_), isCallable);

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
  branchPtr(Assembler::NotEqual, output, ImmPtr(&JSFunction::class_),
            &notFunction);
  if (isCallable) {
    move32(Imm32(1), output);
  } else {
    static_assert(mozilla::IsPowerOfTwo(uint32_t(FunctionFlags::CONSTRUCTOR)),
                  "FunctionFlags::CONSTRUCTOR has only one bit set");

    load16ZeroExtend(Address(obj, JSFunction::offsetOfFlags()), output);
    rshift32(Imm32(mozilla::FloorLog2(uint32_t(FunctionFlags::CONSTRUCTOR))),
             output);
    and32(Imm32(1), output);
  }
  jump(&done);

  bind(&notFunction);

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
  JitContext* jcx = GetJitContext();
  movePtr(ImmPtr(jcx->runtime->mainContextPtr()), dest);
}

static const uint8_t* ContextRealmPtr() {
  return (
      static_cast<const uint8_t*>(GetJitContext()->runtime->mainContextPtr()) +
      JSContext::offsetOfRealm());
}

void MacroAssembler::switchToRealm(Register realm) {
  storePtr(realm, AbsoluteAddress(ContextRealmPtr()));
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
  Address envChain(BaselineFrameReg,
                   BaselineFrame::reverseOffsetOfEnvironmentChain());
  loadPtr(envChain, scratch);
  switchToObjectRealm(scratch, scratch);
}

void MacroAssembler::switchToWasmTlsRealm(Register scratch1,
                                          Register scratch2) {
  loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, cx)), scratch1);
  loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, realm)), scratch2);
  storePtr(scratch2, Address(scratch1, JSContext::offsetOfRealm()));
}

void MacroAssembler::debugAssertContextRealm(const void* realm,
                                             Register scratch) {
#ifdef DEBUG
  Label ok;
  movePtr(ImmPtr(realm), scratch);
  branchPtr(Assembler::Equal, AbsoluteAddress(ContextRealmPtr()), scratch, &ok);
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
  branchPtr(Assembler::Equal, AbsoluteAddress(ContextRealmPtr()), output,
            &isFalse);

  // The object must be a function.
  branchTestObjClass(Assembler::NotEqual, obj, &JSFunction::class_, output, obj,
                     &isFalse);

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
  branchTestObjClass(Assembler::NotEqual, obj, &JSFunction::class_, output, obj,
                     &isFalse);

  // Load the native into |output|.
  loadPtr(Address(obj, JSFunction::offsetOfNativeOrEnv()), output);

  auto branchIsTypedArrayCtor = [&](Scalar::Type type) {
    // The function must be a TypedArrayConstructor native (from any realm).
    JSNative constructor = TypedArrayConstructorNative(type);
    branchPtr(Assembler::Equal, output, ImmPtr(constructor), &isTrue);
  };

#define TYPED_ARRAY_CONSTRUCTOR_NATIVE(T, N) branchIsTypedArrayCtor(Scalar::N);
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

  using Fn = bool (*)(JSString * str1, JSString * str2);
  setupUnalignedABICall(scratch);
  movePtr(ImmGCPtr(atom), scratch);
  passABIArg(scratch);
  passABIArg(str);
  callWithABI<Fn, EqualStringsHelperPure>();
  mov(ReturnReg, scratch);

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

    using Fn = bool (*)(JSContext * cx, JSString * str, int32_t * result);
    setupUnalignedABICall(scratch);
    loadJSContext(scratch);
    passABIArg(scratch);
    passABIArg(str);
    passABIArg(output);
    callWithABI<Fn, GetInt32FromStringPure>();
    mov(ReturnReg, scratch);

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
  loadJSContext(scratch);
  enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

  branchIfFalseBool(ReturnReg, exceptionLabel());

  // Finish bailing out to Baseline.
  {
    // Prepare a register set for use in this case.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    MOZ_ASSERT_IF(!IsHiddenSP(getStackPointer()),
                  !regs.has(AsRegister(getStackPointer())));
    regs.take(bailoutInfo);

    // Reset SP to the point where clobbering starts.
    loadStackPtr(
        Address(bailoutInfo, offsetof(BaselineBailoutInfo, incomingStack)));

    Register copyCur = regs.takeAny();
    Register copyEnd = regs.takeAny();
    Register temp = regs.takeAny();

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
      subPtr(Imm32(4), copyCur);
      subFromStackPtr(Imm32(4));
      load32(Address(copyCur, 0), temp);
      store32(temp, Address(getStackPointer(), 0));
      jump(&copyLoop);
      bind(&endOfCopy);
    }

    // Enter exit frame for the FinishBailoutToBaseline call.
    load32(Address(bailoutInfo,
                   offsetof(BaselineBailoutInfo, frameSizeOfInnerMostFrame)),
           temp);
    makeFrameDescriptor(temp, FrameType::BaselineJS, ExitFrameLayout::Size());
    push(temp);
    push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeAddr)));
    // No GC things to mark on the stack, push a bare token.
    loadJSContext(scratch);
    enterFakeExitFrame(scratch, scratch, ExitFrameType::Bare);

    // Save needed values onto stack temporarily.
    push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeFramePtr)));
    push(Address(bailoutInfo, offsetof(BaselineBailoutInfo, resumeAddr)));

    // Call a stub to free allocated memory and create arguments objects.
    using Fn = bool (*)(BaselineBailoutInfo * bailoutInfoArg);
    setupUnalignedABICall(temp);
    passABIArg(bailoutInfo);
    callWithABI<Fn, FinishBailoutToBaseline>(
        MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckHasExitFrame);
    branchIfFalseBool(ReturnReg, exceptionLabel());

    // Restore values where they need to be and resume execution.
    AllocatableGeneralRegisterSet enterRegs(GeneralRegisterSet::All());
    enterRegs.take(BaselineFrameReg);
    Register jitcodeReg = enterRegs.takeAny();

    pop(jitcodeReg);
    pop(BaselineFrameReg);

    // Discard exit frame.
    addToStackPtr(Imm32(ExitFrameLayout::SizeWithFooter()));

    jump(jitcodeReg);
  }
}

void MacroAssembler::assertRectifierFrameParentType(Register frameType) {
#ifdef DEBUG
  {
    // Check the possible previous frame types here.
    Label checkOk;
    branch32(Assembler::Equal, frameType, Imm32(FrameType::IonJS), &checkOk);
    branch32(Assembler::Equal, frameType, Imm32(FrameType::BaselineStub),
             &checkOk);
    branch32(Assembler::Equal, frameType, Imm32(FrameType::WasmToJSJit),
             &checkOk);
    branch32(Assembler::Equal, frameType, Imm32(FrameType::CppToJSJit),
             &checkOk);
    assumeUnreachable("Unrecognized frame type preceding RectifierFrame.");
    bind(&checkOk);
  }
#endif
}

void MacroAssembler::loadJitCodeRaw(Register func, Register dest) {
  static_assert(BaseScript::offsetOfJitCodeRaw() ==
                    SelfHostedLazyScript::offsetOfJitCodeRaw(),
                "SelfHostedLazyScript and BaseScript must use same layout for "
                "jitCodeRaw_");
  loadPtr(Address(func, JSFunction::offsetOfScript()), dest);
  loadPtr(Address(dest, BaseScript::offsetOfJitCodeRaw()), dest);
}

void MacroAssembler::loadBaselineJitCodeRaw(Register func, Register dest,
                                            Label* failure) {
  // Load JitScript
  loadPtr(Address(func, JSFunction::offsetOfScript()), dest);
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

static const uint8_t* ContextInlinedICScriptPtr() {
  return (
      static_cast<const uint8_t*>(GetJitContext()->runtime->mainContextPtr()) +
      JSContext::offsetOfInlinedICScript());
}

void MacroAssembler::storeICScriptInJSContext(Register icScript) {
  storePtr(icScript, AbsoluteAddress(ContextInlinedICScriptPtr()));
}

void MacroAssembler::handleFailure() {
  // Re-entry code is irrelevant because the exception will leave the
  // running function and never come back
  TrampolinePtr excTail =
      GetJitContext()->runtime->jitRuntime()->getExceptionTail();
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

#ifdef JS_TRACE_LOGGING
void MacroAssembler::loadTraceLogger(Register logger) {
  loadJSContext(logger);
  loadPtr(Address(logger, offsetof(JSContext, traceLogger)), logger);
}

void MacroAssembler::tracelogStartId(Register logger, uint32_t textId,
                                     bool force) {
  if (!force && !TraceLogTextIdEnabled(textId)) {
    return;
  }

  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);
  regs.takeUnchecked(logger);

  Register temp = regs.takeAnyGeneral();

  using Fn = void (*)(TraceLoggerThread * logger, uint32_t id);
  setupUnalignedABICall(temp);
  passABIArg(logger);
  move32(Imm32(textId), temp);
  passABIArg(temp);
  callWithABI<Fn, TraceLogStartEventPrivate>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);

  PopRegsInMask(save);
}

void MacroAssembler::tracelogStartId(Register logger, Register textId) {
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);
  regs.takeUnchecked(logger);
  regs.takeUnchecked(textId);

  Register temp = regs.takeAnyGeneral();

  using Fn = void (*)(TraceLoggerThread * logger, uint32_t id);
  setupUnalignedABICall(temp);
  passABIArg(logger);
  passABIArg(textId);
  callWithABI<Fn, TraceLogStartEventPrivate>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);

  PopRegsInMask(save);
}

void MacroAssembler::tracelogStartEvent(Register logger, Register event) {
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);
  regs.takeUnchecked(logger);
  regs.takeUnchecked(event);

  Register temp = regs.takeAnyGeneral();

  using Fn = void (*)(TraceLoggerThread*, const TraceLoggerEvent&);
  setupUnalignedABICall(temp);
  passABIArg(logger);
  passABIArg(event);
  callWithABI<Fn, TraceLogStartEvent>(MoveOp::GENERAL,
                                      CheckUnsafeCallWithABI::DontCheckOther);

  PopRegsInMask(save);
}

void MacroAssembler::tracelogStopId(Register logger, uint32_t textId,
                                    bool force) {
  if (!force && !TraceLogTextIdEnabled(textId)) {
    return;
  }

  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);
  regs.takeUnchecked(logger);

  Register temp = regs.takeAnyGeneral();

  using Fn = void (*)(TraceLoggerThread * logger, uint32_t id);
  setupUnalignedABICall(temp);
  passABIArg(logger);
  move32(Imm32(textId), temp);
  passABIArg(temp);

  callWithABI<Fn, TraceLogStopEventPrivate>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);

  PopRegsInMask(save);
}

void MacroAssembler::tracelogStopId(Register logger, Register textId) {
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  LiveRegisterSet save(regs.asLiveSet());
  PushRegsInMask(save);
  regs.takeUnchecked(logger);
  regs.takeUnchecked(textId);

  Register temp = regs.takeAnyGeneral();

  using Fn = void (*)(TraceLoggerThread * logger, uint32_t id);
  setupUnalignedABICall(temp);
  passABIArg(logger);
  passABIArg(textId);
  callWithABI<Fn, TraceLogStopEventPrivate>(
      MoveOp::GENERAL, CheckUnsafeCallWithABI::DontCheckOther);

  PopRegsInMask(save);
}
#endif

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
    Push(WasmTlsReg);
  }
  int32_t framePushedAfterTls = framePushed();

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
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
    int32_t tlsOffset = framePushed() - framePushedAfterTls;
    setupWasmABICall();
    passABIArg(src, MoveOp::DOUBLE);
    callWithABI(callOffset, wasm::SymbolicAddress::ToInt32,
                mozilla::Some(tlsOffset));
  } else {
    using Fn = int32_t (*)(double);
    setupUnalignedABICall(dest);
    passABIArg(src, MoveOp::DOUBLE);
    callWithABI<Fn, JS::ToInt32>(MoveOp::GENERAL,
                                 CheckUnsafeCallWithABI::DontCheckOther);
  }
  storeCallInt32Result(dest);

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
  // Nothing
#elif defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  if (widenFloatToDouble) {
    Pop(srcSingle);
  }
#else
  MOZ_CRASH("MacroAssembler platform hook: outOfLineTruncateSlow");
#endif

  if (compilingWasm) {
    Pop(WasmTlsReg);
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
    case IntConversionBehavior::TruncateNoWrap:
      branchTruncateDoubleToInt32(src, output,
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
        case IntConversionBehavior::TruncateNoWrap:
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

void MacroAssembler::alignJitStackBasedOnNArgs(uint32_t argc) {
  // The stack should already be aligned to the size of a value.
  assertStackAlignment(sizeof(Value), 0);

  static_assert(JitStackValueAlignment == 1 || JitStackValueAlignment == 2,
                "JitStackValueAlignment is either 1 or 2.");
  if (JitStackValueAlignment == 1) {
    return;
  }

  // See above for full explanation.
  uint32_t nArgs = argc + 1;
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

MacroAssembler::MacroAssembler(JSContext* cx)
    : wasmMaxOffsetGuardLimit_(0),
      framePushed_(0),
#ifdef DEBUG
      inCall_(false),
#endif
      dynamicAlignment_(false),
      emitProfilingInstrumentation_(false) {
  jitContext_.emplace(cx, (js::jit::TempAllocator*)nullptr);
  alloc_.emplace(&cx->tempLifoAlloc());
  moveResolver_.setAllocator(*jitContext_->temp);
#if defined(JS_CODEGEN_ARM)
  initWithAllocator();
  m_buffer.id = GetJitContext()->getNextAssemblerId();
#elif defined(JS_CODEGEN_ARM64)
  initWithAllocator();
  armbuffer_.id = GetJitContext()->getNextAssemblerId();
#endif
}

MacroAssembler::MacroAssembler()
    : wasmMaxOffsetGuardLimit_(0),
      framePushed_(0),
#ifdef DEBUG
      inCall_(false),
#endif
      dynamicAlignment_(false),
      emitProfilingInstrumentation_(false) {
  JitContext* jcx = GetJitContext();

  if (!jcx->temp) {
    JSContext* cx = jcx->cx;
    MOZ_ASSERT(cx);
    alloc_.emplace(&cx->tempLifoAlloc());
  }

  moveResolver_.setAllocator(*jcx->temp);

#if defined(JS_CODEGEN_ARM)
  initWithAllocator();
  m_buffer.id = jcx->getNextAssemblerId();
#elif defined(JS_CODEGEN_ARM64)
  initWithAllocator();
  armbuffer_.id = jcx->getNextAssemblerId();
#endif
}

MacroAssembler::MacroAssembler(WasmToken, TempAllocator& alloc)
    : wasmMaxOffsetGuardLimit_(0),
      framePushed_(0),
#ifdef DEBUG
      inCall_(false),
#endif
      dynamicAlignment_(false),
      emitProfilingInstrumentation_(false) {
  moveResolver_.setAllocator(alloc);

#if defined(JS_CODEGEN_ARM)
  initWithAllocator();
  m_buffer.id = 0;
#elif defined(JS_CODEGEN_ARM64)
  initWithAllocator();
  // Stubs + builtins + the baseline compiler all require the native SP,
  // not the PSP.
  SetStackPointer64(sp);
  armbuffer_.id = 0;
#endif
}

WasmMacroAssembler::WasmMacroAssembler(TempAllocator& alloc, bool limitedSize)
    : MacroAssembler(WasmToken(), alloc) {
  if (!limitedSize) {
    setUnlimitedBuffer();
  }
}

WasmMacroAssembler::WasmMacroAssembler(TempAllocator& alloc,
                                       const wasm::ModuleEnvironment& env,
                                       bool limitedSize)
    : MacroAssembler(WasmToken(), alloc) {
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
      MOZ_ASSERT((uintptr_t(str) & JSID_TYPE_MASK) == 0);
      static_assert(JSID_TYPE_STRING == 0,
                    "need to orPtr JSID_TYPE_STRING tag if it's not 0");
      Push(ImmGCPtr(str));
    } else {
      MOZ_ASSERT(key.isSymbol());
      movePropertyKey(key, scratchReg);
      Push(scratchReg);
    }
  } else {
    MOZ_ASSERT(key.isInt());
    Push(ImmWord(key.asBits));
  }
}

void MacroAssembler::movePropertyKey(PropertyKey key, Register dest) {
  if (key.isGCThing()) {
    // See comment in |Push(PropertyKey, ...)| above for an explanation.
    if (key.isString()) {
      JSString* str = key.toString();
      MOZ_ASSERT((uintptr_t(str) & JSID_TYPE_MASK) == 0);
      static_assert(JSID_TYPE_STRING == 0,
                    "need to orPtr JSID_TYPE_STRING tag if it's not 0");
      movePtr(ImmGCPtr(str), dest);
    } else {
      MOZ_ASSERT(key.isSymbol());
      JS::Symbol* sym = key.toSymbol();
      movePtr(ImmGCPtr(sym), dest);
      orPtr(Imm32(JSID_TYPE_SYMBOL), dest);
    }
  } else {
    MOZ_ASSERT(key.isInt());
    movePtr(ImmWord(key.asBits), dest);
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
    case VMFunctionData::RootFunction:
    case VMFunctionData::RootCell:
    case VMFunctionData::RootBigInt:
      Push(ImmPtr(nullptr));
      break;
    case VMFunctionData::RootValue:
      Push(UndefinedValue());
      break;
    case VMFunctionData::RootId:
      Push(ImmWord(JSID_BITS(JSID_VOID)));
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
    case VMFunctionData::RootFunction:
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

#if defined(JS_CODEGEN_ARM64)
  MOZ_CRASH("Not supported on arm64");
#endif
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
                                       mozilla::Maybe<int32_t> tlsOffset,
                                       MoveOp::Type result) {
  MOZ_ASSERT(wasm::NeedsBuiltinThunk(imm));

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust, /* callFromWasm = */ true);

  // The TLS register is used in builtin thunks and must be set.
  if (tlsOffset) {
    loadPtr(Address(getStackPointer(), *tlsOffset + stackAdjust), WasmTlsReg);
  } else {
    MOZ_CRASH("tlsOffset is Nothing only for unsupported abi calls.");
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

  move32(Imm32(1), dest);  // p = 1

  // x^y where x == 1 returns 1 for any y.
  Label done;
  branch32(Assembler::Equal, base, Imm32(1), &done);

  move32(base, temp1);   // m = x
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

  // m *= m
  branchMul32(Assembler::Overflow, temp1, temp1, onOver);

  bind(&start);

  // if ((n & 1) != 0) p *= m
  Label even;
  branchTest32(Assembler::Zero, temp2, Imm32(1), &even);
  branchMul32(Assembler::Overflow, temp1, dest, onOver);
  bind(&even);

  // n >>= 1
  // if (n == 0) return p
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

// ===============================================================
// Branch functions

void MacroAssembler::loadFunctionLength(Register func, Register funFlags,
                                        Register output, Label* slowPath) {
#ifdef DEBUG
  {
    // These flags should already have been checked by caller.
    Label ok;
    uint32_t FlagsToCheck =
        FunctionFlags::SELFHOSTLAZY | FunctionFlags::RESOLVED_LENGTH;
    branchTest32(Assembler::Zero, funFlags, Imm32(FlagsToCheck), &ok);
    assumeUnreachable("The function flags should already have been checked.");
    bind(&ok);
  }
#endif  // DEBUG

  // NOTE: `funFlags` and `output` must be allowed to alias.

  // Load the target function's length.
  Label isInterpreted, isBound, lengthLoaded;
  branchTest32(Assembler::NonZero, funFlags, Imm32(FunctionFlags::BOUND_FUN),
               &isBound);
  branchTest32(Assembler::NonZero, funFlags, Imm32(FunctionFlags::BASESCRIPT),
               &isInterpreted);
  {
    // Load the length property of a native function.
    load16ZeroExtend(Address(func, JSFunction::offsetOfNargs()), output);
    jump(&lengthLoaded);
  }
  bind(&isBound);
  {
    // Load the length property of a bound function.
    Address boundLength(func,
                        FunctionExtended::offsetOfBoundFunctionLengthSlot());
    fallibleUnboxInt32(boundLength, output, slowPath);
    jump(&lengthLoaded);
  }
  bind(&isInterpreted);
  {
    // Load the length property of an interpreted function.
    loadPtr(Address(func, JSFunction::offsetOfScript()), output);
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
  load16ZeroExtend(Address(func, JSFunction::offsetOfFlags()), output);

  // If the name was previously resolved, the name property may be shadowed.
  branchTest32(Assembler::NonZero, output, Imm32(FunctionFlags::RESOLVED_NAME),
               slowPath);

  Label notBoundTarget, loadName;
  branchTest32(Assembler::Zero, output, Imm32(FunctionFlags::BOUND_FUN),
               &notBoundTarget);
  {
    // Call into the VM if the target's name atom doesn't contain the bound
    // function prefix.
    branchTest32(Assembler::Zero, output,
                 Imm32(FunctionFlags::HAS_BOUND_FUNCTION_NAME_PREFIX),
                 slowPath);

    // Bound functions reuse HAS_GUESSED_ATOM for
    // HAS_BOUND_FUNCTION_NAME_PREFIX, so skip the guessed atom check below.
    static_assert(
        FunctionFlags::HAS_BOUND_FUNCTION_NAME_PREFIX ==
            FunctionFlags::HAS_GUESSED_ATOM,
        "HAS_BOUND_FUNCTION_NAME_PREFIX is shared with HAS_GUESSED_ATOM");
    jump(&loadName);
  }
  bind(&notBoundTarget);

  Label guessed, hasName;
  branchTest32(Assembler::NonZero, output,
               Imm32(FunctionFlags::HAS_GUESSED_ATOM), &guessed);
  bind(&loadName);
  loadPtr(Address(func, JSFunction::offsetOfAtom()), output);
  branchTestPtr(Assembler::NonZero, output, output, &hasName);
  {
    bind(&guessed);

    // An absent name property defaults to the empty string.
    movePtr(emptyString, output);
  }
  bind(&hasName);
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
  loadObjClassUnsafe(obj, scratch);
  branchTest32(Assembler::NonZero, Address(scratch, JSClass::offsetOfFlags()),
               Imm32(JSClass::NON_NATIVE), label);
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

void MacroAssembler::wasmInterruptCheck(Register tls,
                                        wasm::BytecodeOffset bytecodeOffset) {
  Label ok;
  branch32(Assembler::Equal, Address(tls, offsetof(wasm::TlsData, interrupt)),
           Imm32(0), &ok);
  wasmTrap(wasm::Trap::CheckInterrupt, bytecodeOffset);
  bind(&ok);
}

#ifdef ENABLE_WASM_EXCEPTIONS
size_t MacroAssembler::wasmStartTry() {
  wasm::WasmTryNote tryNote = wasm::WasmTryNote(currentOffset(), 0, 0);
  return append(tryNote);
}
#endif

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
              Address(WasmTlsReg, offsetof(wasm::TlsData, stackLimit)), scratch,
              &ok);

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
                    Address(WasmTlsReg, offsetof(wasm::TlsData, stackLimit)),
                    &ok);
  wasmTrap(wasm::Trap::StackOverflow, trapOffset);
  CodeOffset trapInsnOffset = CodeOffset(currentOffset());
  bind(&ok);
  return std::pair<CodeOffset, uint32_t>(trapInsnOffset, amount);
}

CodeOffset MacroAssembler::wasmCallImport(const wasm::CallSiteDesc& desc,
                                          const wasm::CalleeDesc& callee) {
  storePtr(WasmTlsReg,
           Address(getStackPointer(), WasmCallerTLSOffsetBeforeCall));

  // Load the callee, before the caller's registers are clobbered.
  uint32_t globalDataOffset = callee.importGlobalDataOffset();
  loadWasmGlobalPtr(globalDataOffset + offsetof(wasm::FuncImportTls, code),
                    ABINonArgReg0);

#ifndef JS_CODEGEN_NONE
  static_assert(ABINonArgReg0 != WasmTlsReg, "by constraint");
#endif

  // Switch to the callee's realm.
  loadWasmGlobalPtr(globalDataOffset + offsetof(wasm::FuncImportTls, realm),
                    ABINonArgReg1);
  loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, cx)), ABINonArgReg2);
  storePtr(ABINonArgReg1, Address(ABINonArgReg2, JSContext::offsetOfRealm()));

  // Switch to the callee's TLS and pinned registers and make the call.
  loadWasmGlobalPtr(globalDataOffset + offsetof(wasm::FuncImportTls, tls),
                    WasmTlsReg);

  storePtr(WasmTlsReg,
           Address(getStackPointer(), WasmCalleeTLSOffsetBeforeCall));
  loadWasmPinnedRegsFromTls();

  return call(desc, ABINonArgReg0);
}

CodeOffset MacroAssembler::wasmCallBuiltinInstanceMethod(
    const wasm::CallSiteDesc& desc, const ABIArg& instanceArg,
    wasm::SymbolicAddress builtin, wasm::FailureMode failureMode) {
  MOZ_ASSERT(instanceArg != ABIArg());

  storePtr(WasmTlsReg,
           Address(getStackPointer(), WasmCallerTLSOffsetBeforeCall));
  storePtr(WasmTlsReg,
           Address(getStackPointer(), WasmCalleeTLSOffsetBeforeCall));

  if (instanceArg.kind() == ABIArg::GPR) {
    loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, instance)),
            instanceArg.gpr());
  } else if (instanceArg.kind() == ABIArg::Stack) {
    // Safe to use ABINonArgReg0 since it's the last thing before the call.
    Register scratch = ABINonArgReg0;
    loadPtr(Address(WasmTlsReg, offsetof(wasm::TlsData, instance)), scratch);
    storePtr(scratch,
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

CodeOffset MacroAssembler::wasmCallIndirect(const wasm::CallSiteDesc& desc,
                                            const wasm::CalleeDesc& callee,
                                            bool needsBoundsCheck) {
  Register scratch = WasmTableCallScratchReg0;
  Register index = WasmTableCallIndexReg;

  // Optimization opportunity: when offsetof(FunctionTableElem, code) == 0, as
  // it is at present, we can probably generate better code here by folding
  // the address computation into the load.

  static_assert(sizeof(wasm::FunctionTableElem) == 8 ||
                    sizeof(wasm::FunctionTableElem) == 16,
                "elements of function tables are two words");

  if (callee.which() == wasm::CalleeDesc::AsmJSTable) {
    // asm.js tables require no signature check, and have had their index
    // masked into range and thus need no bounds check.
    loadWasmGlobalPtr(callee.tableFunctionBaseGlobalDataOffset(), scratch);
    if (sizeof(wasm::FunctionTableElem) == 8) {
      computeEffectiveAddress(BaseIndex(scratch, index, TimesEight), scratch);
    } else {
      lshift32(Imm32(4), index);
      addPtr(index, scratch);
    }
    loadPtr(Address(scratch, offsetof(wasm::FunctionTableElem, code)), scratch);
    storePtr(WasmTlsReg,
             Address(getStackPointer(), WasmCallerTLSOffsetBeforeCall));
    storePtr(WasmTlsReg,
             Address(getStackPointer(), WasmCalleeTLSOffsetBeforeCall));
    return call(desc, scratch);
  }

  MOZ_ASSERT(callee.which() == wasm::CalleeDesc::WasmTable);

  // Write the functype-id into the ABI functype-id register.
  wasm::TypeIdDesc funcTypeId = callee.wasmTableSigId();
  switch (funcTypeId.kind()) {
    case wasm::TypeIdDescKind::Global:
      loadWasmGlobalPtr(funcTypeId.globalDataOffset(), WasmTableCallSigReg);
      break;
    case wasm::TypeIdDescKind::Immediate:
      move32(Imm32(funcTypeId.immediate()), WasmTableCallSigReg);
      break;
    case wasm::TypeIdDescKind::None:
      break;
  }

  wasm::BytecodeOffset trapOffset(desc.lineOrBytecode());

  // WebAssembly throws if the index is out-of-bounds.
  if (needsBoundsCheck) {
    loadWasmGlobalPtr(callee.tableLengthGlobalDataOffset(), scratch);

    Label ok;
    branch32(Assembler::Condition::Below, index, scratch, &ok);
    wasmTrap(wasm::Trap::OutOfBounds, trapOffset);
    bind(&ok);
  }

  // Load the base pointer of the table.
  loadWasmGlobalPtr(callee.tableFunctionBaseGlobalDataOffset(), scratch);

  // Load the callee from the table.
  if (sizeof(wasm::FunctionTableElem) == 8) {
    computeEffectiveAddress(BaseIndex(scratch, index, TimesEight), scratch);
  } else {
    lshift32(Imm32(4), index);
    addPtr(index, scratch);
  }

  storePtr(WasmTlsReg,
           Address(getStackPointer(), WasmCallerTLSOffsetBeforeCall));
  loadPtr(Address(scratch, offsetof(wasm::FunctionTableElem, tls)), WasmTlsReg);
  storePtr(WasmTlsReg,
           Address(getStackPointer(), WasmCalleeTLSOffsetBeforeCall));

  Label nonNull;
  branchTest32(Assembler::NonZero, WasmTlsReg, WasmTlsReg, &nonNull);
  wasmTrap(wasm::Trap::IndirectCallToNull, trapOffset);
  bind(&nonNull);

  loadWasmPinnedRegsFromTls();
  switchToWasmTlsRealm(index, WasmTableCallScratchReg1);

  loadPtr(Address(scratch, offsetof(wasm::FunctionTableElem, code)), scratch);

  return call(desc, scratch);
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
  movePtr(ImmWord(~gc::ChunkMask), temp2);
  andPtr(temp1, temp2);

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

  // If it's a permanent atom or symbol from a parent runtime we don't
  // need to barrier it.
  if (type == MIRType::Value || type == MIRType::String) {
    branchPtr(Assembler::NotEqual, Address(temp2, gc::ChunkRuntimeOffset),
              ImmPtr(rt), noBarrier);
  } else {
#ifdef DEBUG
    Label thisRuntime;
    branchPtr(Assembler::Equal, Address(temp2, gc::ChunkRuntimeOffset),
              ImmPtr(rt), &thisRuntime);
    assumeUnreachable("JIT pre-barrier: unexpected runtime");
    bind(&thisRuntime);
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

//}}} check_macroassembler_style

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
               Imm32(Shape::fixedSlotsMask()), &hasFixedSlots);
  assumeUnreachable("Expected a fixed slot");
  bind(&hasFixedSlots);
#endif
}

void MacroAssembler::branchArrayIsNotPacked(Register array, Register temp1,
                                            Register temp2, Label* label) {
  loadPtr(Address(array, NativeObject::offsetOfElements()), temp1);

  // Test length == initializedLength.
  Label done;
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

  // Pre-barrier the element because we're removing it from the array.
  EmitPreBarrier(*this, elementAddr, MIRType::Value);

  // Move the other elements.
  {
    // Ensure output and temp2 are in volatileRegs. Don't preserve temp1.
    volatileRegs.takeUnchecked(temp1);
    if (output.hasVolatileReg()) {
      volatileRegs.addUnchecked(output);
    }
    if (temp2.volatile_()) {
      volatileRegs.addUnchecked(temp2);
    }

    PushRegsInMask(volatileRegs);

    using Fn = void (*)(ArrayObject * arr);
    setupUnalignedABICall(temp1);
    passABIArg(array);
    callWithABI<Fn, ArrayShiftMoveElements>();

    PopRegsInMask(volatileRegs);

    // Reload the elements. The call may have updated it.
    loadPtr(Address(array, NativeObject::offsetOfElements()), temp1);
  }

  // Update length and initializedLength.
  sub32(Imm32(1), temp2);
  store32(temp2, lengthAddr);
  store32(temp2, initLengthAddr);

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
  masm.loadObjPrivate(obj, PropertyIteratorObject::NUM_FIXED_SLOTS, dest);
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
  addPtr(Imm32(sizeof(GCPtrLinearString)), cursorAddr);

  tagValue(JSVAL_TYPE_STRING, temp, output);
  jump(&done);

  bind(&iterDone);
  moveValue(MagicValue(JS_NO_ITER_VALUE), output);

  bind(&done);
}

void MacroAssembler::iteratorClose(Register obj, Register temp1, Register temp2,
                                   Register temp3) {
  LoadNativeIterator(*this, obj, temp1);

  // Clear active bit.
  and32(Imm32(~NativeIterator::Flags::Active),
        Address(temp1, NativeIterator::offsetOfFlagsAndCount()));

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
  subPtr(scratch1, scratch2);
  {
    moveStackPtrTo(scratch1);
    subPtr(Imm32(FRAME_TOUCH_INCREMENT), scratch1);

    Label touchFrameLoop;
    Label touchFrameLoopEnd;
    bind(&touchFrameLoop);
    branchPtr(Assembler::Below, scratch1, scratch2, &touchFrameLoopEnd);
    store32(Imm32(0), Address(scratch1, 0));
    subPtr(Imm32(FRAME_TOUCH_INCREMENT), scratch1);
    jump(&touchFrameLoop);
    bind(&touchFrameLoopEnd);
  }
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

namespace wasm {
const TlsData* ExtractCallerTlsFromFrameWithTls(const Frame* fp) {
  return *reinterpret_cast<TlsData* const*>(
      reinterpret_cast<const uint8_t*>(fp) + sizeof(Frame) + ShadowStackSpace +
      FrameWithTls::callerTLSOffset());
}

const TlsData* ExtractCalleeTlsFromFrameWithTls(const Frame* fp) {
  return *reinterpret_cast<TlsData* const*>(
      reinterpret_cast<const uint8_t*>(fp) + sizeof(Frame) + ShadowStackSpace +
      FrameWithTls::calleeTLSOffset());
}
}  // namespace wasm

}  // namespace js
