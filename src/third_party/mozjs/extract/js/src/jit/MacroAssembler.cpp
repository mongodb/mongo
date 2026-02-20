/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MacroAssembler-inl.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/Latin1.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/XorShift128PlusRNG.h"

#include <algorithm>
#include <limits>
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
#include "js/GCAPI.h"            // JS::AutoCheckCannotGC
#include "js/ScalarType.h"       // js::Scalar::Type
#include "util/Unicode.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/BoundFunctionObject.h"
#include "vm/DateObject.h"
#include "vm/DateTime.h"
#include "vm/Float16.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/StringType.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCodegenConstants.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmInstanceData.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValidate.h"

#include "jit/TemplateObject-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSObject-inl.h"
#include "wasm/WasmGcObject-inl.h"

using namespace js;
using namespace js::jit;

using JS::GenericNaN;

using mozilla::CheckedInt;

TrampolinePtr MacroAssembler::preBarrierTrampoline(MIRType type) {
  const JitRuntime* rt = runtime()->jitRuntime();
  return rt->preBarrier(type);
}

template <typename T>
void MacroAssembler::storeToTypedFloatArray(Scalar::Type arrayType,
                                            FloatRegister value, const T& dest,
                                            Register temp,
                                            LiveRegisterSet volatileLiveRegs) {
  switch (arrayType) {
    case Scalar::Float16:
      storeFloat16(value, dest, temp, volatileLiveRegs);
      break;
    case Scalar::Float32: {
      if (value.isDouble()) {
        ScratchFloat32Scope fpscratch(*this);
        convertDoubleToFloat32(value, fpscratch);
        storeFloat32(fpscratch, dest);
      } else {
        MOZ_ASSERT(value.isSingle());
        storeFloat32(value, dest);
      }
      break;
    }
    case Scalar::Float64:
      MOZ_ASSERT(value.isDouble());
      storeDouble(value, dest);
      break;
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

template void MacroAssembler::storeToTypedFloatArray(
    Scalar::Type arrayType, FloatRegister value, const BaseIndex& dest,
    Register temp, LiveRegisterSet volatileLiveRegs);
template void MacroAssembler::storeToTypedFloatArray(
    Scalar::Type arrayType, FloatRegister value, const Address& dest,
    Register temp, LiveRegisterSet volatileLiveRegs);

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
                                        AnyRegister dest, Register temp1,
                                        Register temp2, Label* fail,
                                        LiveRegisterSet volatileLiveRegs) {
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
        load32(src, temp1);
        convertUInt32ToDouble(temp1, dest.fpu());
      } else {
        load32(src, dest.gpr());

        // Bail out if the value doesn't fit into a signed int32 value. This
        // is what allows MLoadUnboxedScalar to have a type() of
        // MIRType::Int32 for UInt32 array loads.
        branchTest32(Assembler::Signed, dest.gpr(), dest.gpr(), fail);
      }
      break;
    case Scalar::Float16:
      loadFloat16(src, dest.fpu(), temp1, temp2, volatileLiveRegs);
      canonicalizeFloat(dest.fpu());
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

template void MacroAssembler::loadFromTypedArray(
    Scalar::Type arrayType, const Address& src, AnyRegister dest,
    Register temp1, Register temp2, Label* fail,
    LiveRegisterSet volatileLiveRegs);
template void MacroAssembler::loadFromTypedArray(
    Scalar::Type arrayType, const BaseIndex& src, AnyRegister dest,
    Register temp1, Register temp2, Label* fail,
    LiveRegisterSet volatileLiveRegs);

void MacroAssembler::loadFromTypedArray(Scalar::Type arrayType,
                                        const BaseIndex& src,
                                        const ValueOperand& dest,
                                        Uint32Mode uint32Mode, Register temp,
                                        Label* fail,
                                        LiveRegisterSet volatileLiveRegs) {
  switch (arrayType) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
      loadFromTypedArray(arrayType, src, AnyRegister(dest.scratchReg()),
                         InvalidReg, InvalidReg, nullptr, LiveRegisterSet{});
      tagValue(JSVAL_TYPE_INT32, dest.scratchReg(), dest);
      break;
    case Scalar::Uint32:
      load32(src, dest.scratchReg());
      boxUint32(dest.scratchReg(), dest, uint32Mode, fail);
      break;
    case Scalar::Float16: {
      ScratchDoubleScope dscratch(*this);
      FloatRegister fscratch = dscratch.asSingle();
      loadFromTypedArray(arrayType, src, AnyRegister(fscratch),
                         dest.scratchReg(), temp, nullptr, volatileLiveRegs);
      convertFloat32ToDouble(fscratch, dscratch);
      boxDouble(dscratch, dest, dscratch);
      break;
    }
    case Scalar::Float32: {
      ScratchDoubleScope dscratch(*this);
      FloatRegister fscratch = dscratch.asSingle();
      loadFromTypedArray(arrayType, src, AnyRegister(fscratch), InvalidReg,
                         InvalidReg, nullptr, LiveRegisterSet{});
      convertFloat32ToDouble(fscratch, dscratch);
      boxDouble(dscratch, dest, dscratch);
      break;
    }
    case Scalar::Float64: {
      ScratchDoubleScope fpscratch(*this);
      loadFromTypedArray(arrayType, src, AnyRegister(fpscratch), InvalidReg,
                         InvalidReg, nullptr, LiveRegisterSet{});
      boxDouble(fpscratch, dest, fpscratch);
      break;
    }
    case Scalar::BigInt64:
    case Scalar::BigUint64:
    default:
      MOZ_CRASH("Invalid typed array type");
  }
}

void MacroAssembler::loadFromTypedBigIntArray(Scalar::Type arrayType,
                                              const BaseIndex& src,
                                              const ValueOperand& dest,
                                              Register bigInt,
                                              Register64 temp) {
  MOZ_ASSERT(Scalar::isBigIntType(arrayType));

  load64(src, temp);
  initializeBigInt64(arrayType, bigInt, temp);
  tagValue(JSVAL_TYPE_BIGINT, bigInt, dest);
}

// Inlined version of gc::CheckAllocatorState that checks the bare essentials
// and bails for anything that cannot be handled with our jit allocators.
void MacroAssembler::checkAllocatorState(Register temp, gc::AllocKind allocKind,
                                         Label* fail) {
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

  // If the zone has a realm with an object allocation metadata hook, emit a
  // guard for this. Note that IC stubs and some other trampolines can be shared
  // across realms, so we don't bake in a realm pointer.
  if (gc::IsObjectAllocKind(allocKind) &&
      realm()->zone()->hasRealmWithAllocMetadataBuilder()) {
    loadJSContext(temp);
    loadPtr(Address(temp, JSContext::offsetOfRealm()), temp);
    branchPtr(Assembler::NotEqual,
              Address(temp, Realm::offsetOfAllocationMetadataBuilder()),
              ImmWord(0), fail);
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

// Inlined equivalent of gc::AllocateObject, without failure case handling.
void MacroAssembler::allocateObject(Register result, Register temp,
                                    gc::AllocKind allocKind,
                                    uint32_t nDynamicSlots,
                                    gc::Heap initialHeap, Label* fail,
                                    const AllocSiteInput& allocSite) {
  MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));

  checkAllocatorState(temp, allocKind, fail);

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
                                    bool initContents /* = true */,
                                    const AllocSiteInput& allocSite) {
  gc::AllocKind allocKind = templateObj.getAllocKind();
  MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));

  uint32_t nDynamicSlots = 0;
  if (templateObj.isNativeObject()) {
    const TemplateNativeObject& ntemplate =
        templateObj.asTemplateNativeObject();
    nDynamicSlots = ntemplate.numDynamicSlots();
  }

  allocateObject(obj, temp, allocKind, nDynamicSlots, initialHeap, fail,
                 allocSite);
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
    Register result, Register shape, Register temp, Register dynamicSlotsTemp,
    uint32_t arrayLength, uint32_t arrayCapacity, uint32_t numUsedDynamicSlots,
    uint32_t numDynamicSlots, gc::AllocKind allocKind, gc::Heap initialHeap,
    Label* fail, const AllocSiteInput& allocSite) {
  MOZ_ASSERT(gc::IsObjectAllocKind(allocKind));
  MOZ_ASSERT(shape != temp, "shape can overlap with temp2, but not temp");
  MOZ_ASSERT(result != temp);

  // This only supports allocating arrays with fixed elements and does not
  // support any dynamic elements.
  MOZ_ASSERT(arrayCapacity >= arrayLength);
  MOZ_ASSERT(gc::GetGCKindSlots(allocKind) >=
             arrayCapacity + ObjectElements::VALUES_PER_HEADER);

  MOZ_ASSERT(numUsedDynamicSlots <= numDynamicSlots);

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

  // Initialize dynamic slots.
  if (numUsedDynamicSlots > 0) {
    MOZ_ASSERT(dynamicSlotsTemp != temp);
    MOZ_ASSERT(dynamicSlotsTemp != InvalidReg);
    loadPtr(Address(result, NativeObject::offsetOfSlots()), dynamicSlotsTemp);
    fillSlotsWithUndefined(Address(dynamicSlotsTemp, 0), temp, 0,
                           numUsedDynamicSlots);
  }
}

void MacroAssembler::createFunctionClone(Register result, Register canonical,
                                         Register envChain, Register temp,
                                         gc::AllocKind allocKind, Label* fail,
                                         const AllocSiteInput& allocSite) {
  MOZ_ASSERT(allocKind == gc::AllocKind::FUNCTION ||
             allocKind == gc::AllocKind::FUNCTION_EXTENDED);
  MOZ_ASSERT(result != temp);

  // Allocate object.
  size_t numDynamicSlots = 0;
  gc::Heap initialHeap = gc::Heap::Default;
  allocateObject(result, temp, allocKind, numDynamicSlots, initialHeap, fail,
                 allocSite);

  // Initialize shape field.
  loadPtr(Address(canonical, JSObject::offsetOfShape()), temp);
  storePtr(temp, Address(result, JSObject::offsetOfShape()));

  // Initialize dynamic slots and elements pointers.
  storePtr(ImmPtr(emptyObjectSlots),
           Address(result, NativeObject::offsetOfSlots()));
  storePtr(ImmPtr(emptyObjectElements),
           Address(result, NativeObject::offsetOfElements()));

  // Initialize FlagsAndArgCountSlot.
  storeValue(Address(canonical, JSFunction::offsetOfFlagsAndArgCount()),
             Address(result, JSFunction::offsetOfFlagsAndArgCount()), temp);

  // Initialize NativeFuncOrInterpretedEnvSlot.
  storeValue(JSVAL_TYPE_OBJECT, envChain,
             Address(result, JSFunction::offsetOfEnvironment()));

#ifdef DEBUG
  // The new function must be allocated in the nursery if the nursery is
  // enabled. Assert no post-barrier is needed.
  Label ok;
  branchPtrInNurseryChunk(Assembler::Equal, result, temp, &ok);
  branchPtrInNurseryChunk(Assembler::NotEqual, envChain, temp, &ok);
  assumeUnreachable("Missing post write barrier in createFunctionClone");
  bind(&ok);
#endif

  // Initialize NativeJitInfoOrInterpretedScriptSlot. This is a BaseScript*
  // pointer stored as PrivateValue.
  loadPrivate(Address(canonical, JSFunction::offsetOfJitInfoOrScript()), temp);
  storePrivateValue(temp,
                    Address(result, JSFunction::offsetOfJitInfoOrScript()));

  // Initialize AtomSlot.
  storeValue(Address(canonical, JSFunction::offsetOfAtom()),
             Address(result, JSFunction::offsetOfAtom()), temp);

  // Initialize extended slots.
  if (allocKind == gc::AllocKind::FUNCTION_EXTENDED) {
    for (size_t i = 0; i < FunctionExtended::NUM_EXTENDED_SLOTS; i++) {
      Address addr(result, FunctionExtended::offsetOfExtendedSlot(i));
      storeValue(UndefinedValue(), addr);
    }
  }
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

// This function handles nursery allocations for JS. For wasm, see
// MacroAssembler::wasmBumpPointerAllocate.
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

    if (traceKind != JS::TraceKind::Object ||
        runtime()->geckoProfiler().enabled()) {
      // Update the catch all allocation site, which his is used to calculate
      // nursery allocation counts so we can determine whether to disable
      // nursery allocation of strings and bigints.
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
           Address(site, gc::AllocSite::offsetOfNurseryAllocCount()),
           Imm32(js::gc::NormalSiteAttentionThreshold), &done);

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

  checkAllocatorState(temp, allocKind, fail);

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
  constexpr gc::AllocKind allocKind = gc::AllocKind::BIGINT;

  checkAllocatorState(temp, allocKind, fail);

  if (shouldNurseryAllocate(allocKind, initialHeap)) {
    MOZ_ASSERT(initialHeap == gc::Heap::Default);
    return nurseryAllocateBigInt(result, temp, fail);
  }

  freeListAllocate(result, temp, allocKind, fail);
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

void MacroAssembler::initTypedArraySlots(
    Register obj, Register temp, Register lengthReg, LiveRegisterSet liveRegs,
    Label* fail, FixedLengthTypedArrayObject* templateObj,
    TypedArrayLength lengthKind) {
  MOZ_ASSERT(!templateObj->hasBuffer());

  constexpr size_t dataSlotOffset = ArrayBufferViewObject::dataOffset();
  constexpr size_t dataOffset = dataSlotOffset + sizeof(HeapSlot);

  static_assert(
      FixedLengthTypedArrayObject::FIXED_DATA_START ==
          FixedLengthTypedArrayObject::DATA_SLOT + 1,
      "fixed inline element data assumed to begin after the data slot");

  static_assert(
      FixedLengthTypedArrayObject::INLINE_BUFFER_LIMIT ==
          JSObject::MAX_BYTE_SIZE - dataOffset,
      "typed array inline buffer is limited by the maximum object byte size");

  // Initialise data elements to zero.
  size_t length = templateObj->length();
  MOZ_ASSERT(length <= INT32_MAX,
             "Template objects are only created for int32 lengths");
  size_t nbytes = length * templateObj->bytesPerElement();

  if (lengthKind == TypedArrayLength::Fixed &&
      nbytes <= FixedLengthTypedArrayObject::INLINE_BUFFER_LIMIT) {
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

static size_t StringCharsByteLength(const JSLinearString* linear) {
  CharEncoding encoding =
      linear->hasLatin1Chars() ? CharEncoding::Latin1 : CharEncoding::TwoByte;
  size_t encodingSize = encoding == CharEncoding::Latin1
                            ? sizeof(JS::Latin1Char)
                            : sizeof(char16_t);
  return linear->length() * encodingSize;
}

bool MacroAssembler::canCompareStringCharsInline(const JSLinearString* linear) {
  // Limit the number of inline instructions used for character comparisons. Use
  // the same instruction limit for both encodings, i.e. two-byte uses half the
  // limit of Latin-1 strings.
  constexpr size_t ByteLengthCompareCutoff = 32;

  size_t byteLength = StringCharsByteLength(linear);
  return 0 < byteLength && byteLength <= ByteLengthCompareCutoff;
}

template <typename T, typename CharT>
static inline T CopyCharacters(const CharT* chars) {
  T value = 0;
  std::memcpy(&value, chars, sizeof(T));
  return value;
}

template <typename T>
static inline T CopyCharacters(const JSLinearString* linear, size_t index) {
  JS::AutoCheckCannotGC nogc;

  if (linear->hasLatin1Chars()) {
    MOZ_ASSERT(index + sizeof(T) / sizeof(JS::Latin1Char) <= linear->length());
    return CopyCharacters<T>(linear->latin1Chars(nogc) + index);
  }

  MOZ_ASSERT(sizeof(T) >= sizeof(char16_t));
  MOZ_ASSERT(index + sizeof(T) / sizeof(char16_t) <= linear->length());
  return CopyCharacters<T>(linear->twoByteChars(nogc) + index);
}

void MacroAssembler::branchIfNotStringCharsEquals(Register stringChars,
                                                  const JSLinearString* linear,
                                                  Label* label) {
  CharEncoding encoding =
      linear->hasLatin1Chars() ? CharEncoding::Latin1 : CharEncoding::TwoByte;
  size_t encodingSize = encoding == CharEncoding::Latin1
                            ? sizeof(JS::Latin1Char)
                            : sizeof(char16_t);
  size_t byteLength = StringCharsByteLength(linear);

  size_t pos = 0;
  for (size_t stride : {8, 4, 2, 1}) {
    while (byteLength >= stride) {
      Address addr(stringChars, pos * encodingSize);
      switch (stride) {
        case 8: {
          auto x = CopyCharacters<uint64_t>(linear, pos);
          branch64(Assembler::NotEqual, addr, Imm64(x), label);
          break;
        }
        case 4: {
          auto x = CopyCharacters<uint32_t>(linear, pos);
          branch32(Assembler::NotEqual, addr, Imm32(x), label);
          break;
        }
        case 2: {
          auto x = CopyCharacters<uint16_t>(linear, pos);
          branch16(Assembler::NotEqual, addr, Imm32(x), label);
          break;
        }
        case 1: {
          auto x = CopyCharacters<uint8_t>(linear, pos);
          branch8(Assembler::NotEqual, addr, Imm32(x), label);
          break;
        }
      }

      byteLength -= stride;
      pos += stride / encodingSize;
    }

    // Prefer a single comparison for trailing bytes instead of doing
    // multiple consecutive comparisons.
    //
    // For example when comparing against the string "example", emit two
    // four-byte comparisons against "exam" and "mple" instead of doing
    // three comparisons against "exam", "pl", and finally "e".
    if (pos > 0 && byteLength > stride / 2) {
      MOZ_ASSERT(stride == 8 || stride == 4);

      size_t prev = pos - (stride - byteLength) / encodingSize;
      Address addr(stringChars, prev * encodingSize);
      switch (stride) {
        case 8: {
          auto x = CopyCharacters<uint64_t>(linear, prev);
          branch64(Assembler::NotEqual, addr, Imm64(x), label);
          break;
        }
        case 4: {
          auto x = CopyCharacters<uint32_t>(linear, prev);
          branch32(Assembler::NotEqual, addr, Imm32(x), label);
          break;
        }
      }

      // Break from the loop, because we've finished the complete string.
      break;
    }
  }
}

void MacroAssembler::loadStringCharsForCompare(Register input,
                                               const JSLinearString* linear,
                                               Register stringChars,
                                               Label* fail) {
  CharEncoding encoding =
      linear->hasLatin1Chars() ? CharEncoding::Latin1 : CharEncoding::TwoByte;

  // Take the slow path when the string is a rope or has a different character
  // representation.
  branchIfRope(input, fail);
  if (encoding == CharEncoding::Latin1) {
    branchTwoByteString(input, fail);
  } else {
    JS::AutoCheckCannotGC nogc;
    if (mozilla::IsUtf16Latin1(linear->twoByteRange(nogc))) {
      branchLatin1String(input, fail);
    } else {
      // This case was already handled in the caller.
#ifdef DEBUG
      Label ok;
      branchTwoByteString(input, &ok);
      assumeUnreachable("Unexpected Latin-1 string");
      bind(&ok);
#endif
    }
  }

#ifdef DEBUG
  {
    size_t length = linear->length();
    MOZ_ASSERT(length > 0);

    Label ok;
    branch32(Assembler::AboveOrEqual,
             Address(input, JSString::offsetOfLength()), Imm32(length), &ok);
    assumeUnreachable("Input mustn't be smaller than search string");
    bind(&ok);
  }
#endif

  // Load the input string's characters.
  loadStringChars(input, stringChars, encoding);
}

void MacroAssembler::compareStringChars(JSOp op, Register stringChars,
                                        const JSLinearString* linear,
                                        Register output) {
  MOZ_ASSERT(IsEqualityOp(op));

  size_t byteLength = StringCharsByteLength(linear);

  // Prefer a single compare-and-set instruction if possible.
  if (byteLength == 1 || byteLength == 2 || byteLength == 4 ||
      byteLength == 8) {
    auto cond = JSOpToCondition(op, /* isSigned = */ false);

    Address addr(stringChars, 0);
    switch (byteLength) {
      case 8: {
        auto x = CopyCharacters<uint64_t>(linear, 0);
        cmp64Set(cond, addr, Imm64(x), output);
        break;
      }
      case 4: {
        auto x = CopyCharacters<uint32_t>(linear, 0);
        cmp32Set(cond, addr, Imm32(x), output);
        break;
      }
      case 2: {
        auto x = CopyCharacters<uint16_t>(linear, 0);
        cmp16Set(cond, addr, Imm32(x), output);
        break;
      }
      case 1: {
        auto x = CopyCharacters<uint8_t>(linear, 0);
        cmp8Set(cond, addr, Imm32(x), output);
        break;
      }
    }
  } else {
    Label setNotEqualResult;
    branchIfNotStringCharsEquals(stringChars, linear, &setNotEqualResult);

    // Falls through if both strings are equal.

    Label done;
    move32(Imm32(op == JSOp::Eq || op == JSOp::StrictEq), output);
    jump(&done);

    bind(&setNotEqualResult);
    move32(Imm32(op == JSOp::Ne || op == JSOp::StrictNe), output);

    bind(&done);
  }
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
      static_assert(Mask < 2048,
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
    static_assert(Mask < 2048,
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

void MacroAssembler::branchIfMaybeSplitSurrogatePair(Register leftChild,
                                                     Register index,
                                                     Register scratch,
                                                     Label* maybeSplit,
                                                     Label* notSplit) {
  // If |index| is the last character of the left child and the left child
  // is a two-byte string, it's possible that a surrogate pair is split
  // between the left and right child of a rope.

  // Can't be a split surrogate when the left child is a Latin-1 string.
  branchLatin1String(leftChild, notSplit);

  // Can't be a split surrogate when |index + 1| is in the left child.
  add32(Imm32(1), index, scratch);
  branch32(Assembler::Above, Address(leftChild, JSString::offsetOfLength()),
           scratch, notSplit);

  // Jump to |maybeSplit| if the left child is another rope.
  branchIfRope(leftChild, maybeSplit);

  // Load the character at |index|.
  loadStringChars(leftChild, scratch, CharEncoding::TwoByte);
  loadChar(scratch, index, scratch, CharEncoding::TwoByte);

  // Jump to |maybeSplit| if the last character is a lead surrogate.
  branchIfLeadSurrogate(scratch, scratch, maybeSplit);
}

void MacroAssembler::loadRopeChild(CharKind kind, Register str, Register index,
                                   Register output, Register maybeScratch,
                                   Label* isLinear, Label* splitSurrogate) {
  // This follows JSString::getChar.
  branchIfNotRope(str, isLinear);

  loadRopeLeftChild(str, output);

  Label loadedChild;
  if (kind == CharKind::CharCode) {
    // Check if |index| is contained in the left child.
    branch32(Assembler::Above, Address(output, JSString::offsetOfLength()),
             index, &loadedChild);
  } else {
    MOZ_ASSERT(maybeScratch != InvalidReg);

    // Check if |index| is contained in the left child.
    Label loadRight;
    branch32(Assembler::BelowOrEqual,
             Address(output, JSString::offsetOfLength()), index, &loadRight);
    {
      // Handle possible split surrogate pairs.
      branchIfMaybeSplitSurrogatePair(output, index, maybeScratch,
                                      splitSurrogate, &loadedChild);
      jump(&loadedChild);
    }
    bind(&loadRight);
  }

  // The index must be in the rightChild.
  loadRopeRightChild(str, output);

  bind(&loadedChild);
}

void MacroAssembler::branchIfCanLoadStringChar(CharKind kind, Register str,
                                               Register index, Register scratch,
                                               Register maybeScratch,
                                               Label* label) {
  Label splitSurrogate;
  loadRopeChild(kind, str, index, scratch, maybeScratch, label,
                &splitSurrogate);

  // Branch if the left resp. right side is linear.
  branchIfNotRope(scratch, label);

  if (kind == CharKind::CodePoint) {
    bind(&splitSurrogate);
  }
}

void MacroAssembler::branchIfNotCanLoadStringChar(CharKind kind, Register str,
                                                  Register index,
                                                  Register scratch,
                                                  Register maybeScratch,
                                                  Label* label) {
  Label done;
  loadRopeChild(kind, str, index, scratch, maybeScratch, &done, label);

  // Branch if the left or right side is another rope.
  branchIfRope(scratch, label);

  bind(&done);
}

void MacroAssembler::loadStringChar(CharKind kind, Register str, Register index,
                                    Register output, Register scratch1,
                                    Register scratch2, Label* fail) {
  MOZ_ASSERT(str != output);
  MOZ_ASSERT(str != index);
  MOZ_ASSERT(index != output);
  MOZ_ASSERT_IF(kind == CharKind::CodePoint, index != scratch1);
  MOZ_ASSERT(output != scratch1);
  MOZ_ASSERT(output != scratch2);

  // Use scratch1 for the index (adjusted below).
  if (index != scratch1) {
    move32(index, scratch1);
  }
  movePtr(str, output);

  // This follows JSString::getChar.
  Label notRope;
  branchIfNotRope(str, &notRope);

  loadRopeLeftChild(str, output);

  // Check if the index is contained in the leftChild.
  Label loadedChild, notInLeft;
  spectreBoundsCheck32(scratch1, Address(output, JSString::offsetOfLength()),
                       scratch2, &notInLeft);
  if (kind == CharKind::CodePoint) {
    branchIfMaybeSplitSurrogatePair(output, scratch1, scratch2, fail,
                                    &loadedChild);
  }
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
  branchLatin1String(output, &isLatin1);
  {
    loadStringChars(output, scratch2, CharEncoding::TwoByte);

    if (kind == CharKind::CharCode) {
      loadChar(scratch2, scratch1, output, CharEncoding::TwoByte);
    } else {
      // Load the first character.
      addToCharPtr(scratch2, scratch1, CharEncoding::TwoByte);
      loadChar(Address(scratch2, 0), output, CharEncoding::TwoByte);

      // If the first character isn't a lead surrogate, go to |done|.
      branchIfNotLeadSurrogate(output, &done);

      // branchIfMaybeSplitSurrogatePair ensures that the surrogate pair can't
      // split between two rope children. So if |index + 1 < str.length|, then
      // |index| and |index + 1| are in the same rope child.
      //
      // NB: We use the non-adjusted |index| and |str| inputs, because |output|
      // was overwritten and no longer contains the rope child.

      // If |index + 1| is a valid index into |str|.
      add32(Imm32(1), index, scratch1);
      spectreBoundsCheck32(scratch1, Address(str, JSString::offsetOfLength()),
                           InvalidReg, &done);

      // Then load the next character at |scratch2 + sizeof(char16_t)|.
      loadChar(Address(scratch2, sizeof(char16_t)), scratch1,
               CharEncoding::TwoByte);

      // If the next character isn't a trail surrogate, go to |done|.
      branchIfNotTrailSurrogate(scratch1, scratch2, &done);

      // Inlined unicode::UTF16Decode(char16_t, char16_t).
      lshift32(Imm32(10), output);
      add32(Imm32(unicode::NonBMPMin - (unicode::LeadSurrogateMin << 10) -
                  unicode::TrailSurrogateMin),
            scratch1);
      add32(scratch1, output);
    }

    jump(&done);
  }
  bind(&isLatin1);
  {
    loadStringChars(output, scratch2, CharEncoding::Latin1);
    loadChar(scratch2, scratch1, output, CharEncoding::Latin1);
  }

  bind(&done);
}

void MacroAssembler::loadStringChar(Register str, int32_t index,
                                    Register output, Register scratch1,
                                    Register scratch2, Label* fail) {
  MOZ_ASSERT(str != output);
  MOZ_ASSERT(output != scratch1);
  MOZ_ASSERT(output != scratch2);

  if (index == 0) {
    movePtr(str, scratch1);

    // This follows JSString::getChar.
    Label notRope;
    branchIfNotRope(str, &notRope);

    loadRopeLeftChild(str, scratch1);

    // Rope children can't be empty, so the index can't be in the right side.

    // If the left side is another rope, give up.
    branchIfRope(scratch1, fail);

    bind(&notRope);

    Label isLatin1, done;
    branchLatin1String(scratch1, &isLatin1);
    loadStringChars(scratch1, scratch2, CharEncoding::TwoByte);
    loadChar(Address(scratch2, 0), output, CharEncoding::TwoByte);
    jump(&done);

    bind(&isLatin1);
    loadStringChars(scratch1, scratch2, CharEncoding::Latin1);
    loadChar(Address(scratch2, 0), output, CharEncoding::Latin1);

    bind(&done);
  } else {
    move32(Imm32(index), scratch1);
    loadStringChar(str, scratch1, output, scratch1, scratch2, fail);
  }
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

void MacroAssembler::branchIfNotLeadSurrogate(Register src, Label* label) {
  branch32(Assembler::Below, src, Imm32(unicode::LeadSurrogateMin), label);
  branch32(Assembler::Above, src, Imm32(unicode::LeadSurrogateMax), label);
}

void MacroAssembler::branchSurrogate(Assembler::Condition cond, Register src,
                                     Register scratch, Label* label,
                                     SurrogateChar surrogateChar) {
  // For TrailSurrogateMin ≤ x ≤ TrailSurrogateMax and
  // LeadSurrogateMin ≤ x ≤ LeadSurrogateMax, the following equations hold.
  //
  //    SurrogateMin ≤ x ≤ SurrogateMax
  // <> SurrogateMin ≤ x ≤ SurrogateMin + 2^10 - 1
  // <> ((x - SurrogateMin) >>> 10) = 0    where >>> is an unsigned-shift
  // See Hacker's Delight, section 4-1 for details.
  //
  //    ((x - SurrogateMin) >>> 10) = 0
  // <> floor((x - SurrogateMin) / 1024) = 0
  // <> floor((x / 1024) - (SurrogateMin / 1024)) = 0
  // <> floor(x / 1024) = SurrogateMin / 1024
  // <> floor(x / 1024) * 1024 = SurrogateMin
  // <> (x >>> 10) << 10 = SurrogateMin
  // <> x & ~(2^10 - 1) = SurrogateMin

  constexpr char16_t SurrogateMask = 0xFC00;
  char16_t SurrogateMin = surrogateChar == SurrogateChar::Lead
                              ? unicode::LeadSurrogateMin
                              : unicode::TrailSurrogateMin;

  and32(Imm32(SurrogateMask), src, scratch);
  branch32(cond, scratch, Imm32(SurrogateMin), label);
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

void MacroAssembler::lookupStaticString(Register ch, Register dest,
                                        const StaticStrings& staticStrings) {
  MOZ_ASSERT(ch != dest);

  movePtr(ImmPtr(&staticStrings.unitStaticTable), dest);
  loadPtr(BaseIndex(dest, ch, ScalePointer), dest);
}

void MacroAssembler::lookupStaticString(Register ch, Register dest,
                                        const StaticStrings& staticStrings,
                                        Label* fail) {
  MOZ_ASSERT(ch != dest);

  boundsCheck32PowerOfTwo(ch, StaticStrings::UNIT_STATIC_LIMIT, fail);
  movePtr(ImmPtr(&staticStrings.unitStaticTable), dest);
  loadPtr(BaseIndex(dest, ch, ScalePointer), dest);
}

void MacroAssembler::lookupStaticString(Register ch1, Register ch2,
                                        Register dest,
                                        const StaticStrings& staticStrings,
                                        Label* fail) {
  MOZ_ASSERT(ch1 != dest);
  MOZ_ASSERT(ch2 != dest);

  branch32(Assembler::AboveOrEqual, ch1,
           Imm32(StaticStrings::SMALL_CHAR_TABLE_SIZE), fail);
  branch32(Assembler::AboveOrEqual, ch2,
           Imm32(StaticStrings::SMALL_CHAR_TABLE_SIZE), fail);

  movePtr(ImmPtr(&StaticStrings::toSmallCharTable.storage), dest);
  load8ZeroExtend(BaseIndex(dest, ch1, Scale::TimesOne), ch1);
  load8ZeroExtend(BaseIndex(dest, ch2, Scale::TimesOne), ch2);

  branch32(Assembler::Equal, ch1, Imm32(StaticStrings::INVALID_SMALL_CHAR),
           fail);
  branch32(Assembler::Equal, ch2, Imm32(StaticStrings::INVALID_SMALL_CHAR),
           fail);

  lshift32(Imm32(StaticStrings::SMALL_CHAR_BITS), ch1);
  add32(ch2, ch1);

  // Look up the string from the computed index.
  movePtr(ImmPtr(&staticStrings.length2StaticTable), dest);
  loadPtr(BaseIndex(dest, ch1, ScalePointer), dest);
}

void MacroAssembler::lookupStaticIntString(Register integer, Register dest,
                                           Register scratch,
                                           const StaticStrings& staticStrings,
                                           Label* fail) {
  MOZ_ASSERT(integer != scratch);

  boundsCheck32PowerOfTwo(integer, StaticStrings::INT_STATIC_LIMIT, fail);
  movePtr(ImmPtr(&staticStrings.intStaticTable), scratch);
  loadPtr(BaseIndex(scratch, integer, ScalePointer), dest);
}

void MacroAssembler::loadInt32ToStringWithBase(
    Register input, Register base, Register dest, Register scratch1,
    Register scratch2, const StaticStrings& staticStrings,
    const LiveRegisterSet& volatileRegs, bool lowerCase, Label* fail) {
#ifdef DEBUG
  Label baseBad, baseOk;
  branch32(Assembler::LessThan, base, Imm32(2), &baseBad);
  branch32(Assembler::LessThanOrEqual, base, Imm32(36), &baseOk);
  bind(&baseBad);
  assumeUnreachable("base must be in range [2, 36]");
  bind(&baseOk);
#endif

  // Compute |"0123456789abcdefghijklmnopqrstuvwxyz"[r]|.
  auto toChar = [this, base, lowerCase](Register r) {
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
    add32(Imm32((lowerCase ? 'a' : 'A') - '0' - 10), r);
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
    Register scratch2, const StaticStrings& staticStrings, bool lowerCase,
    Label* fail) {
  MOZ_ASSERT(2 <= base && base <= 36, "base must be in range [2, 36]");

  // Compute |"0123456789abcdefghijklmnopqrstuvwxyz"[r]|.
  auto toChar = [this, base, lowerCase](Register r) {
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
      add32(Imm32((lowerCase ? 'a' : 'A') - '0' - 10), r);
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

      rshift32(Imm32(shift), input, scratch1);
      and32(Imm32((uint32_t(1) << shift) - 1), input, scratch2);
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

void MacroAssembler::loadBigIntDigit(Register bigInt, Register dest) {
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

void MacroAssembler::loadBigIntDigit(Register bigInt, Register dest,
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

void MacroAssembler::loadBigIntPtr(Register bigInt, Register dest,
                                   Label* fail) {
  loadBigIntDigit(bigInt, dest, fail);

  // BigInt digits are stored as unsigned numbers. Take the failure path when
  // the digit can't be stored in intptr_t.

  Label nonNegative, done;
  branchIfBigIntIsNonNegative(bigInt, &nonNegative);
  {
    // Negate |dest| when the BigInt is negative.
    negPtr(dest);

    // Test after negating to handle INTPTR_MIN correctly.
    branchTestPtr(Assembler::NotSigned, dest, dest, fail);
    jump(&done);
  }
  bind(&nonNegative);
  branchTestPtr(Assembler::Signed, dest, dest, fail);
  bind(&done);
}

void MacroAssembler::initializeBigInt64(Scalar::Type type, Register bigInt,
                                        Register64 val, Register64 temp) {
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
    // Copy the input when we're not allowed to clobber it.
    if (temp != Register64::Invalid()) {
      move64(val, temp);
      val = temp;
    }

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

void MacroAssembler::initializeBigIntPtr(Register bigInt, Register val) {
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
  // If the unsigned value of the BigInt can't be expressed in an uint32/uint64,
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
  // |scratch1| and |abs(y)| in |scratch2| and then compare the unsigned numbers
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

    // BigInt digits are always stored as an unsigned number.
    loadBigIntDigit(bigInt, scratch1);

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

void MacroAssembler::compareBigIntAndInt32(JSOp op, Register bigInt,
                                           Imm32 int32, Register scratch,
                                           Label* ifTrue, Label* ifFalse) {
  MOZ_ASSERT(IsLooseEqualityOp(op) || IsRelationalOp(op));

  static_assert(std::is_same_v<BigInt::Digit, uintptr_t>,
                "BigInt digit can be loaded in a pointer-sized register");
  static_assert(sizeof(BigInt::Digit) >= sizeof(uint32_t),
                "BigInt digit stores at least an uint32");

  // Comparison against zero doesn't require loading any BigInt digits.
  if (int32.value == 0) {
    switch (op) {
      case JSOp::Eq:
        branchIfBigIntIsZero(bigInt, ifTrue);
        break;
      case JSOp::Ne:
        branchIfBigIntIsNonZero(bigInt, ifTrue);
        break;
      case JSOp::Lt:
        branchIfBigIntIsNegative(bigInt, ifTrue);
        break;
      case JSOp::Le:
        branchIfBigIntIsZero(bigInt, ifTrue);
        branchIfBigIntIsNegative(bigInt, ifTrue);
        break;
      case JSOp::Gt:
        branchIfBigIntIsZero(bigInt, ifFalse);
        branchIfBigIntIsNonNegative(bigInt, ifTrue);
        break;
      case JSOp::Ge:
        branchIfBigIntIsNonNegative(bigInt, ifTrue);
        break;
      default:
        MOZ_CRASH("bad comparison operator");
    }

    // Fall through to the false case.
    return;
  }

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

  // Test for mismatched signs.
  if (int32.value > 0) {
    branchIfBigIntIsNegative(bigInt, lessThan);
  } else {
    branchIfBigIntIsNonNegative(bigInt, greaterThan);
  }

  // Both signs are equal, load |abs(x)| in |scratch| and then compare the
  // unsigned numbers against each other.
  //
  // If the unsigned value of the BigInt can't be expressed in an uint32/uint64,
  // the result of the comparison is a constant.
  Label* tooLarge = int32.value > 0 ? greaterThan : lessThan;
  loadBigIntDigit(bigInt, scratch, tooLarge);

  // Use the unsigned value of the immediate.
  ImmWord uint32 = ImmWord(mozilla::Abs(int32.value));

  // Reverse the relational comparator for negative numbers.
  // |-x < -y| <=> |+x > +y|.
  // |-x ≤ -y| <=> |+x ≥ +y|.
  // |-x > -y| <=> |+x < +y|.
  // |-x ≥ -y| <=> |+x ≤ +y|.
  if (int32.value < 0) {
    op = ReverseCompareOp(op);
  }

  branchPtr(JSOpToCondition(op, /* isSigned = */ false), scratch, uint32,
            ifTrue);
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

void MacroAssembler::loadGlobalObjectData(Register dest) {
  loadPtr(AbsoluteAddress(ContextRealmPtr(runtime())), dest);
  loadPtr(Address(dest, Realm::offsetOfActiveGlobal()), dest);
  loadPrivate(Address(dest, GlobalObject::offsetOfGlobalDataSlot()), dest);
}

void MacroAssembler::switchToRealm(Register realm) {
  storePtr(realm, AbsoluteAddress(ContextRealmPtr(runtime())));
}

void MacroAssembler::loadRealmFuse(RealmFuses::FuseIndex index, Register dest) {
  // Load Realm pointer
  loadPtr(AbsoluteAddress(ContextRealmPtr(runtime())), dest);
  loadPtr(Address(dest, RealmFuses::offsetOfFuseWordRelativeToRealm(index)),
          dest);
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

template <typename ValueType>
void MacroAssembler::storeLocalAllocSite(ValueType value, Register scratch) {
  loadPtr(AbsoluteAddress(ContextRealmPtr(runtime())), scratch);
  storePtr(value, Address(scratch, JS::Realm::offsetOfLocalAllocSite()));
}

template void MacroAssembler::storeLocalAllocSite(Register, Register);
template void MacroAssembler::storeLocalAllocSite(ImmWord, Register);
template void MacroAssembler::storeLocalAllocSite(ImmPtr, Register);

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

void MacroAssembler::guardObjectHasSameRealm(Register obj, Register scratch,
                                             Label* fail) {
  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfRealm()), scratch);
  branchPtr(Assembler::NotEqual, AbsoluteAddress(ContextRealmPtr(runtime())),
            scratch, fail);
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

void MacroAssembler::tryFastAtomize(Register str, Register scratch,
                                    Register output, Label* fail) {
  Label found, done, notAtomRef;

  branchTest32(Assembler::Zero, Address(str, JSString::offsetOfFlags()),
               Imm32(JSString::ATOM_REF_BIT), &notAtomRef);
  loadPtr(Address(str, JSAtomRefString::offsetOfAtom()), output);
  jump(&done);
  bind(&notAtomRef);

  uintptr_t cachePtr = uintptr_t(runtime()->addressOfStringToAtomCache());
  void* offset = (void*)(cachePtr + StringToAtomCache::offsetOfLastLookups());
  movePtr(ImmPtr(offset), scratch);

  static_assert(StringToAtomCache::NumLastLookups == 2);
  size_t stringOffset = StringToAtomCache::LastLookup::offsetOfString();
  size_t lookupSize = sizeof(StringToAtomCache::LastLookup);
  branchPtr(Assembler::Equal, Address(scratch, stringOffset), str, &found);
  branchPtr(Assembler::NotEqual, Address(scratch, lookupSize + stringOffset),
            str, fail);
  addPtr(Imm32(lookupSize), scratch);

  // We found a hit in the lastLookups_ array! Load the associated atom
  // and jump back up to our usual atom handling code
  bind(&found);
  size_t atomOffset = StringToAtomCache::LastLookup::offsetOfAtom();
  loadPtr(Address(scratch, atomOffset), output);
  bind(&done);
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
  Label isString, isSymbol, isNull, isUndefined, done, nonAtom, atom;

  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);
    branchTestString(Assembler::Equal, tag, &isString);
    branchTestSymbol(Assembler::Equal, tag, &isSymbol);
    branchTestNull(Assembler::Equal, tag, &isNull);
    branchTestUndefined(Assembler::NotEqual, tag, cacheMiss);
  }

  const JSAtomState& names = runtime()->names();
  movePropertyKey(NameToId(names.undefined), outId);
  move32(Imm32(names.undefined->hash()), outHash);
  jump(&done);

  bind(&isNull);
  movePropertyKey(NameToId(names.null), outId);
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
  tryFastAtomize(outId, outHash, outId, cacheMiss);
  jump(&atom);

  bind(&done);
}

void MacroAssembler::emitExtractValueFromMegamorphicCacheEntry(
    Register obj, Register entry, Register scratch1, Register scratch2,
    ValueOperand output, Label* cacheHit, Label* cacheMiss,
    Label* cacheHitGetter) {
  Label isMissing, dynamicSlot, protoLoopHead, protoLoopTail;

  // scratch2 = entry->hopsAndKind_
  load8ZeroExtend(
      Address(entry, MegamorphicCache::Entry::offsetOfHopsAndKind()), scratch2);
  // if (scratch2 == NumHopsForMissingProperty) goto isMissing
  branch32(Assembler::Equal, scratch2,
           Imm32(MegamorphicCache::Entry::NumHopsForMissingProperty),
           &isMissing);

  if (cacheHitGetter) {
    // Here we're going to set scratch1 to 0 for a data property and 1 for a
    // getter and scratch2 to the number of hops
    Label dataProperty;

    // if (scratch2 & NonDataPropertyFlag == 0) goto dataProperty
    move32(Imm32(0), scratch1);
    branchTest32(Assembler::Zero, scratch2,
                 Imm32(MegamorphicCache::Entry::NonDataPropertyFlag),
                 &dataProperty);

    // if (scratch2 > NonDataPropertyFlag | MaxHopsForAccessorProperty) goto
    // cacheMiss
    branch32(Assembler::GreaterThan, scratch2,
             Imm32(MegamorphicCache::Entry::NonDataPropertyFlag |
                   MegamorphicCache::Entry::MaxHopsForAccessorProperty),
             cacheMiss);

    and32(Imm32(~MegamorphicCache::Entry::NonDataPropertyFlag), scratch2);
    move32(Imm32(1), scratch1);

    bind(&dataProperty);
  } else {
    // if (scratch2 & NonDataPropertyFlag) goto cacheMiss
    branchTest32(Assembler::NonZero, scratch2,
                 Imm32(MegamorphicCache::Entry::NonDataPropertyFlag),
                 cacheMiss);
  }

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

  // entry = entry->slotOffset()
  load32(Address(entry, MegamorphicCacheEntry::offsetOfSlotOffset()), entry);

  // scratch2 = slotOffset.offset()
  rshift32(Imm32(TaggedSlotOffset::OffsetShift), entry, scratch2);

  // if (!slotOffset.isFixedSlot()) goto dynamicSlot
  branchTest32(Assembler::Zero, entry, Imm32(TaggedSlotOffset::IsFixedSlotFlag),
               &dynamicSlot);
  // output = outputScratch[scratch2]
  loadValue(BaseIndex(outputScratch, scratch2, TimesOne), output);
  if (cacheHitGetter) {
    branchTest32(Assembler::NonZero, scratch1, scratch1, cacheHitGetter);
  }
  jump(cacheHit);

  bind(&dynamicSlot);
  // output = outputScratch->slots_[scratch2]
  loadPtr(Address(outputScratch, NativeObject::offsetOfSlots()), outputScratch);
  loadValue(BaseIndex(outputScratch, scratch2, TimesOne), output);
  if (cacheHitGetter) {
    branchTest32(Assembler::NonZero, scratch1, scratch1, cacheHitGetter);
  }
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
    Register outEntryPtr, ValueOperand output, Label* cacheHit,
    Label* cacheHitGetter) {
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

  emitExtractValueFromMegamorphicCacheEntry(obj, outEntryPtr, scratch1,
                                            scratch2, output, cacheHit,
                                            &cacheMiss, cacheHitGetter);

  bind(&cacheMiss);
}

template <typename IdOperandType>
void MacroAssembler::emitMegamorphicCacheLookupByValue(
    IdOperandType id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, ValueOperand output, Label* cacheHit,
    Label* cacheHitGetter) {
  Label cacheMiss, cacheMissWithEntry;
  emitMegamorphicCacheLookupByValueCommon(id, obj, scratch1, scratch2,
                                          outEntryPtr, &cacheMiss,
                                          &cacheMissWithEntry);
  emitExtractValueFromMegamorphicCacheEntry(
      obj, outEntryPtr, scratch1, scratch2, output, cacheHit,
      &cacheMissWithEntry, cacheHitGetter);
  bind(&cacheMiss);
  xorPtr(outEntryPtr, outEntryPtr);
  bind(&cacheMissWithEntry);
}

template void MacroAssembler::emitMegamorphicCacheLookupByValue<ValueOperand>(
    ValueOperand id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, ValueOperand output, Label* cacheHit,
    Label* cacheHitGetter);

template void MacroAssembler::emitMegamorphicCacheLookupByValue<Register>(
    Register id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, ValueOperand output, Label* cacheHit,
    Label* cacheHitGetter);

void MacroAssembler::emitMegamorphicCacheLookupExists(
    ValueOperand id, Register obj, Register scratch1, Register scratch2,
    Register outEntryPtr, Register output, Label* cacheHit, bool hasOwn) {
  Label cacheMiss, cacheMissWithEntry, cacheHitFalse;
  emitMegamorphicCacheLookupByValueCommon(id, obj, scratch1, scratch2,
                                          outEntryPtr, &cacheMiss,
                                          &cacheMissWithEntry);

  // scratch1 = outEntryPtr->hopsAndKind_
  load8ZeroExtend(
      Address(outEntryPtr, MegamorphicCache::Entry::offsetOfHopsAndKind()),
      scratch1);

  branch32(Assembler::Equal, scratch1,
           Imm32(MegamorphicCache::Entry::NumHopsForMissingProperty),
           &cacheHitFalse);
  branchTest32(Assembler::NonZero, scratch1,
               Imm32(MegamorphicCache::Entry::NonDataPropertyFlag),
               &cacheMissWithEntry);

  if (hasOwn) {
    branch32(Assembler::NotEqual, scratch1, Imm32(0), &cacheHitFalse);
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
  rshift32(Imm32(PropertyIndex::KindShift), outIndex, outKind);

  // Extract index.
  and32(Imm32(PropertyIndex::IndexMask), outIndex);
}

template <typename IdType>
void MacroAssembler::emitMegamorphicCachedSetSlot(
    IdType id, Register obj, Register scratch1,
#ifndef JS_CODEGEN_X86  // See MegamorphicSetElement in LIROps.yaml
    Register scratch2, Register scratch3,
#endif
    ValueOperand value, const LiveRegisterSet& liveRegs, Label* cacheHit,
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
  rshift32(Imm32(TaggedSlotOffset::OffsetShift), scratch2, scratch1);

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

  LiveRegisterSet save;
  save.set() = RegisterSet::Intersect(liveRegs.set(), RegisterSet::Volatile());
  save.addUnchecked(scratch1);   // Used as call temp below.
  save.takeUnchecked(scratch2);  // Used for the return value.
  PushRegsInMask(save);

  using Fn = bool (*)(JSContext* cx, NativeObject* obj, uint32_t newCount);
  setupUnalignedABICall(scratch1);
  loadJSContext(scratch1);
  passABIArg(scratch1);
  passABIArg(obj);
  passABIArg(scratch2);
  callWithABI<Fn, NativeObject::growSlotsPure>();
  storeCallPointerResult(scratch2);

  MOZ_ASSERT(!save.has(scratch2));
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
    ValueOperand value, const LiveRegisterSet& liveRegs, Label* cacheHit,
    void (*emitPreBarrier)(MacroAssembler&, const Address&, MIRType));

template void MacroAssembler::emitMegamorphicCachedSetSlot<ValueOperand>(
    ValueOperand id, Register obj, Register scratch1,
#ifndef JS_CODEGEN_X86  // See MegamorphicSetElement in LIROps.yaml
    Register scratch2, Register scratch3,
#endif
    ValueOperand value, const LiveRegisterSet& liveRegs, Label* cacheHit,
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

void MacroAssembler::loadGrowableSharedArrayBufferByteLengthIntPtr(
    Synchronization sync, Register obj, Register output) {
  // Load the SharedArrayRawBuffer.
  loadPrivate(Address(obj, SharedArrayBufferObject::rawBufferOffset()), output);

  memoryBarrierBefore(sync);

  // Load the byteLength of the SharedArrayRawBuffer into |output|.
  static_assert(sizeof(mozilla::Atomic<size_t>) == sizeof(size_t));
  loadPtr(Address(output, SharedArrayRawBuffer::offsetOfByteLength()), output);

  memoryBarrierAfter(sync);
}

void MacroAssembler::loadResizableArrayBufferViewLengthIntPtr(
    ResizableArrayBufferView view, Synchronization sync, Register obj,
    Register output, Register scratch) {
  // Inline implementation of ArrayBufferViewObject::length(), when the input is
  // guaranteed to be a resizable arraybuffer view object.

  loadArrayBufferViewLengthIntPtr(obj, output);

  Label done;
  branchPtr(Assembler::NotEqual, output, ImmWord(0), &done);

  // Load obj->elements in |scratch|.
  loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  // If backed by non-shared memory, detached and out-of-bounds both return
  // zero, so we're done here.
  branchTest32(Assembler::Zero,
               Address(scratch, ObjectElements::offsetOfFlags()),
               Imm32(ObjectElements::SHARED_MEMORY), &done);

  // Load the auto-length slot.
  unboxBoolean(Address(obj, ArrayBufferViewObject::autoLengthOffset()),
               scratch);

  // If non-auto length, there's nothing to do.
  branchTest32(Assembler::Zero, scratch, scratch, &done);

  // Load bufferByteLength into |output|.
  {
    // Resizable TypedArrays are guaranteed to have an ArrayBuffer.
    unboxObject(Address(obj, ArrayBufferViewObject::bufferOffset()), output);

    // Load the byte length from the raw-buffer of growable SharedArrayBuffers.
    loadGrowableSharedArrayBufferByteLengthIntPtr(sync, output, output);
  }

  // Load the byteOffset into |scratch|.
  loadArrayBufferViewByteOffsetIntPtr(obj, scratch);

  // Compute the accessible byte length |bufferByteLength - byteOffset|.
  subPtr(scratch, output);

  if (view == ResizableArrayBufferView::TypedArray) {
    // Compute the array length from the byte length.
    resizableTypedArrayElementShiftBy(obj, output, scratch);
  }

  bind(&done);
}

void MacroAssembler::loadResizableTypedArrayByteOffsetMaybeOutOfBoundsIntPtr(
    Register obj, Register output, Register scratch) {
  // Inline implementation of TypedArrayObject::byteOffsetMaybeOutOfBounds(),
  // when the input is guaranteed to be a resizable typed array object.

  loadArrayBufferViewByteOffsetIntPtr(obj, output);

  // TypedArray is neither detached nor out-of-bounds when byteOffset non-zero.
  Label done;
  branchPtr(Assembler::NotEqual, output, ImmWord(0), &done);

  // We're done when the initial byteOffset is zero.
  loadPrivate(Address(obj, ArrayBufferViewObject::initialByteOffsetOffset()),
              output);
  branchPtr(Assembler::Equal, output, ImmWord(0), &done);

  // If the buffer is attached, return initialByteOffset.
  branchIfHasAttachedArrayBuffer(obj, scratch, &done);

  // Otherwise return zero to match the result for fixed-length TypedArrays.
  movePtr(ImmWord(0), output);

  bind(&done);
}

void MacroAssembler::dateFillLocalTimeSlots(
    Register obj, Register scratch, const LiveRegisterSet& volatileRegs) {
  // Inline implementation of the cache check from
  // DateObject::fillLocalTimeSlots().

  Label callVM, done;

  // Check if the cache is already populated.
  branchTestUndefined(Assembler::Equal,
                      Address(obj, DateObject::offsetOfLocalTimeSlot()),
                      &callVM);

  unboxInt32(Address(obj, DateObject::offsetOfUTCTimeZoneOffsetSlot()),
             scratch);

  branch32(Assembler::Equal,
           AbsoluteAddress(DateTimeInfo::addressOfUTCToLocalOffsetSeconds()),
           scratch, &done);

  bind(&callVM);
  {
    PushRegsInMask(volatileRegs);

    using Fn = void (*)(DateObject*);
    setupUnalignedABICall(scratch);
    passABIArg(obj);
    callWithABI<Fn, jit::DateFillLocalTimeSlots>();

    PopRegsInMask(volatileRegs);
  }

  bind(&done);
}

void MacroAssembler::udiv32ByConstant(Register src, uint32_t divisor,
                                      Register dest) {
  auto rmc = ReciprocalMulConstants::computeUnsignedDivisionConstants(divisor);
  MOZ_ASSERT(rmc.multiplier <= UINT32_MAX, "division needs scratch register");

  // We first compute |q = (M * n) >> 32), where M = rmc.multiplier.
  mulHighUnsigned32(Imm32(rmc.multiplier), src, dest);

  // Finish the computation |q = floor(n / d)|.
  rshift32(Imm32(rmc.shiftAmount), dest);
}

void MacroAssembler::umod32ByConstant(Register src, uint32_t divisor,
                                      Register dest, Register scratch) {
  MOZ_ASSERT(dest != scratch);

  auto rmc = ReciprocalMulConstants::computeUnsignedDivisionConstants(divisor);
  MOZ_ASSERT(rmc.multiplier <= UINT32_MAX, "division needs scratch register");

  if (src != dest) {
    move32(src, dest);
  }

  // We first compute |q = (M * n) >> 32), where M = rmc.multiplier.
  mulHighUnsigned32(Imm32(rmc.multiplier), dest, scratch);

  // Finish the computation |q = floor(n / d)|.
  rshift32(Imm32(rmc.shiftAmount), scratch);

  // Compute the remainder from |r = n - q * d|.
  mul32(Imm32(divisor), scratch);
  sub32(scratch, dest);
}

template <typename GetTimeFn>
void MacroAssembler::dateTimeFromSecondsIntoYear(ValueOperand secondsIntoYear,
                                                 ValueOperand output,
                                                 Register scratch1,
                                                 Register scratch2,
                                                 GetTimeFn getTimeFn) {
#ifdef DEBUG
  Label okValue;
  branchTestInt32(Assembler::Equal, secondsIntoYear, &okValue);
  branchTestNaNValue(Assembler::Equal, secondsIntoYear, scratch1, &okValue);
  assumeUnreachable("secondsIntoYear is an int32 or NaN");
  bind(&okValue);
#endif

  moveValue(secondsIntoYear, output);

  Label done;
  fallibleUnboxInt32(secondsIntoYear, scratch1, &done);

#ifdef DEBUG
  Label okInt;
  branchTest32(Assembler::NotSigned, scratch1, scratch1, &okInt);
  assumeUnreachable("secondsIntoYear is an unsigned int32");
  bind(&okInt);
#endif

  getTimeFn(scratch1, scratch1, scratch2);

  tagValue(JSVAL_TYPE_INT32, scratch1, output);

  bind(&done);
}

void MacroAssembler::dateHoursFromSecondsIntoYear(ValueOperand secondsIntoYear,
                                                  ValueOperand output,
                                                  Register scratch1,
                                                  Register scratch2) {
  // Inline implementation of seconds-into-year to local hours computation from
  // date_getHours.

  // Compute `(yearSeconds / SecondsPerHour) % HoursPerDay`.
  auto hoursFromSecondsIntoYear = [this](Register src, Register dest,
                                         Register scratch) {
    udiv32ByConstant(src, SecondsPerHour, dest);
    umod32ByConstant(dest, HoursPerDay, dest, scratch);
  };

  dateTimeFromSecondsIntoYear(secondsIntoYear, output, scratch1, scratch2,
                              hoursFromSecondsIntoYear);
}

void MacroAssembler::dateMinutesFromSecondsIntoYear(
    ValueOperand secondsIntoYear, ValueOperand output, Register scratch1,
    Register scratch2) {
  // Inline implementation of seconds-into-year to local minutes computation
  // from date_getMinutes.

  // Compute `(yearSeconds / SecondsPerMinute) % MinutesPerHour`.
  auto minutesFromSecondsIntoYear = [this](Register src, Register dest,
                                           Register scratch) {
    udiv32ByConstant(src, SecondsPerMinute, dest);
    umod32ByConstant(dest, MinutesPerHour, dest, scratch);
  };

  dateTimeFromSecondsIntoYear(secondsIntoYear, output, scratch1, scratch2,
                              minutesFromSecondsIntoYear);
}

void MacroAssembler::dateSecondsFromSecondsIntoYear(
    ValueOperand secondsIntoYear, ValueOperand output, Register scratch1,
    Register scratch2) {
  // Inline implementation of seconds-into-year to local seconds computation
  // from date_getSeconds.

  // Compute `yearSeconds % SecondsPerMinute`.
  auto secondsFromSecondsIntoYear = [this](Register src, Register dest,
                                           Register scratch) {
    umod32ByConstant(src, SecondsPerMinute, dest, scratch);
  };

  dateTimeFromSecondsIntoYear(secondsIntoYear, output, scratch1, scratch2,
                              secondsFromSecondsIntoYear);
}

void MacroAssembler::computeImplicitThis(Register env, ValueOperand output,
                                         Label* slowPath) {
  // Inline implementation of ComputeImplicitThis.

  Register scratch = output.scratchReg();
  MOZ_ASSERT(scratch != env);

  loadObjClassUnsafe(env, scratch);

  // Go to the slow path for possible debug environment proxies.
  branchTestClassIsProxy(true, scratch, slowPath);

  // WithEnvironmentObjects have an actual implicit |this|.
  Label nonWithEnv, done;
  branchPtr(Assembler::NotEqual, scratch,
            ImmPtr(&WithEnvironmentObject::class_), &nonWithEnv);
  {
    if (JitOptions.spectreObjectMitigations) {
      spectreZeroRegister(Assembler::NotEqual, scratch, env);
    }

    loadValue(Address(env, WithEnvironmentObject::offsetOfThisSlot()), output);

    jump(&done);
  }
  bind(&nonWithEnv);

  // The implicit |this| is |undefined| for all environment types except
  // WithEnvironmentObject.
  moveValue(JS::UndefinedValue(), output);

  bind(&done);
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

void MacroAssembler::loadBaselineCompileQueue(Register dest) {
  loadPtr(AbsoluteAddress(ContextRealmPtr(runtime())), dest);
  computeEffectiveAddress(Address(dest, Realm::offsetOfBaselineCompileQueue()),
                          dest);
}

void MacroAssembler::guardSpecificAtom(Register str, JSAtom* atom,
                                       Register scratch,
                                       const LiveRegisterSet& volatileRegs,
                                       Label* fail) {
  Label done, notCachedAtom;
  branchPtr(Assembler::Equal, str, ImmGCPtr(atom), &done);

  // The pointers are not equal, so if the input string is also an atom it
  // must be a different string.
  branchTest32(Assembler::NonZero, Address(str, JSString::offsetOfFlags()),
               Imm32(JSString::ATOM_BIT), fail);

  // Try to do a cheap atomize on the string and repeat the above test
  tryFastAtomize(str, scratch, scratch, &notCachedAtom);
  branchPtr(Assembler::Equal, scratch, ImmGCPtr(atom), &done);
  jump(fail);
  bind(&notCachedAtom);

  // Check the length.
  branch32(Assembler::NotEqual, Address(str, JSString::offsetOfLength()),
           Imm32(atom->length()), fail);

  // Compare short atoms using inline assembly.
  if (canCompareStringCharsInline(atom)) {
    // Pure two-byte strings can't be equal to Latin-1 strings.
    if (atom->hasTwoByteChars()) {
      JS::AutoCheckCannotGC nogc;
      if (!mozilla::IsUtf16Latin1(atom->twoByteRange(nogc))) {
        branchLatin1String(str, fail);
      }
    }

    // Call into the VM when the input is a rope or has a different encoding.
    Label vmCall;

    // Load the input string's characters.
    Register stringChars = scratch;
    loadStringCharsForCompare(str, atom, stringChars, &vmCall);

    // Start comparing character by character.
    branchIfNotStringCharsEquals(stringChars, atom, fail);

    // Falls through if both strings are equal.
    jump(&done);

    bind(&vmCall);
  }

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
        ABIType::General, CheckUnsafeCallWithABI::DontCheckHasExitFrame);
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
    static_assert(DisabledScript < CompilingScript);
    branchPtr(Assembler::BelowOrEqual, dest, ImmWord(CompilingScript), failure);
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

void MacroAssembler::storeICScriptInJSContext(Register icScript) {
  storePtr(icScript, AbsoluteAddress(runtime()->addressOfInlinedICScript()));
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
    callWithABI<Fn, AssumeUnreachable>(ABIType::General,
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
  ScratchDoubleScope fpscratch(*this);
  convertInt32ToDouble(val.payloadOrValueReg(), fpscratch);
  boxDouble(fpscratch, val, fpscratch);
  bind(&done);
}

void MacroAssembler::convertValueToFloatingPoint(
    ValueOperand value, FloatRegister output, Register maybeTemp,
    LiveRegisterSet volatileLiveRegs, Label* fail,
    FloatingPointType outputType) {
  Label isDouble, isInt32OrBool, isNull, done;

  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);

    branchTestDouble(Assembler::Equal, tag, &isDouble);
    branchTestInt32(Assembler::Equal, tag, &isInt32OrBool);
    branchTestBoolean(Assembler::Equal, tag, &isInt32OrBool);
    branchTestNull(Assembler::Equal, tag, &isNull);
    branchTestUndefined(Assembler::NotEqual, tag, fail);
  }

  // fall-through: undefined
  if (outputType == FloatingPointType::Float16 ||
      outputType == FloatingPointType::Float32) {
    loadConstantFloat32(float(GenericNaN()), output);
  } else {
    loadConstantDouble(GenericNaN(), output);
  }
  jump(&done);

  bind(&isNull);
  if (outputType == FloatingPointType::Float16 ||
      outputType == FloatingPointType::Float32) {
    loadConstantFloat32(0.0f, output);
  } else {
    loadConstantDouble(0.0, output);
  }
  jump(&done);

  bind(&isInt32OrBool);
  if (outputType == FloatingPointType::Float16) {
    convertInt32ToFloat16(value.payloadOrValueReg(), output, maybeTemp,
                          volatileLiveRegs);
  } else if (outputType == FloatingPointType::Float32) {
    convertInt32ToFloat32(value.payloadOrValueReg(), output);
  } else {
    convertInt32ToDouble(value.payloadOrValueReg(), output);
  }
  jump(&done);

  // On some non-multiAlias platforms, unboxDouble may use the scratch register,
  // so do not merge code paths here.
  bind(&isDouble);
  if ((outputType == FloatingPointType::Float16 ||
       outputType == FloatingPointType::Float32) &&
      hasMultiAlias()) {
    ScratchDoubleScope tmp(*this);
    unboxDouble(value, tmp);

    if (outputType == FloatingPointType::Float16) {
      convertDoubleToFloat16(tmp, output, maybeTemp, volatileLiveRegs);
    } else {
      convertDoubleToFloat32(tmp, output);
    }
  } else {
    FloatRegister tmp = output.asDouble();
    unboxDouble(value, tmp);

    if (outputType == FloatingPointType::Float16) {
      convertDoubleToFloat16(tmp, output, maybeTemp, volatileLiveRegs);
    } else if (outputType == FloatingPointType::Float32) {
      convertDoubleToFloat32(tmp, output);
    }
  }

  bind(&done);
}

void MacroAssembler::outOfLineTruncateSlow(FloatRegister src, Register dest,
                                           bool widenFloatToDouble,
                                           bool compilingWasm,
                                           wasm::BytecodeOffset callOffset) {
  ScratchDoubleScope fpscratch(*this);
  if (widenFloatToDouble) {
    convertFloat32ToDouble(src, fpscratch);
    src = fpscratch;
  }
  MOZ_ASSERT(src.isDouble());

  if (compilingWasm) {
    Push(InstanceReg);
    int32_t framePushedAfterInstance = framePushed();

    setupWasmABICall();
    passABIArg(src, ABIType::Float64);

    int32_t instanceOffset = framePushed() - framePushedAfterInstance;
    callWithABI(callOffset, wasm::SymbolicAddress::ToInt32,
                mozilla::Some(instanceOffset));
    storeCallInt32Result(dest);

    Pop(InstanceReg);
  } else {
    using Fn = int32_t (*)(double);
    setupUnalignedABICall(dest);
    passABIArg(src, ABIType::Float64);
    callWithABI<Fn, JS::ToInt32>(ABIType::General,
                                 CheckUnsafeCallWithABI::DontCheckOther);
    storeCallInt32Result(dest);
  }
}

void MacroAssembler::convertValueToInt32(ValueOperand value, FloatRegister temp,
                                         Register output, Label* fail,
                                         bool negativeZeroCheck,
                                         IntConversionInputKind conversion) {
  Label done, isInt32, isBool, isDouble, isString;

  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);

    branchTestInt32(Equal, tag, &isInt32);
    branchTestDouble(Equal, tag, &isDouble);
    if (conversion == IntConversionInputKind::Any) {
      branchTestBoolean(Equal, tag, &isBool);
      branchTestNull(Assembler::NotEqual, tag, fail);
    } else {
      jump(fail);
    }
  }

  // The value is null - just emit 0.
  if (conversion == IntConversionInputKind::Any) {
    move32(Imm32(0), output);
    jump(&done);
  }

  // Try converting double into integer.
  {
    bind(&isDouble);
    unboxDouble(value, temp);
    convertDoubleToInt32(temp, output, fail, negativeZeroCheck);
    jump(&done);
  }

  // Just unbox a bool, the result is 0 or 1.
  if (conversion == IntConversionInputKind::Any) {
    bind(&isBool);
    unboxBoolean(value, output);
    jump(&done);
  }

  // Integers can be unboxed.
  {
    bind(&isInt32);
    unboxInt32(value, output);
  }

  bind(&done);
}

void MacroAssembler::truncateValueToInt32(
    ValueOperand value, Label* handleStringEntry, Label* handleStringRejoin,
    Label* truncateDoubleSlow, Register stringReg, FloatRegister temp,
    Register output, Label* fail) {
  Label done, isInt32, isBool, isDouble, isNull, isString;

  bool handleStrings = handleStringEntry && handleStringRejoin;

  // |output| needs to be different from |stringReg| to load string indices.
  MOZ_ASSERT_IF(handleStrings, stringReg != output);

  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);

    branchTestInt32(Equal, tag, &isInt32);
    branchTestDouble(Equal, tag, &isDouble);
    branchTestBoolean(Equal, tag, &isBool);
    branchTestNull(Equal, tag, &isNull);
    if (handleStrings) {
      branchTestString(Equal, tag, &isString);
    }
    branchTestUndefined(Assembler::NotEqual, tag, fail);
  }

  // The value is null or undefined in truncation contexts - just emit 0.
  {
    bind(&isNull);
    move32(Imm32(0), output);
    jump(&done);
  }

  // First try loading a string index. If that fails, try converting a string
  // into a double, then jump to the double case.
  Label handleStringIndex;
  if (handleStrings) {
    bind(&isString);
    unboxString(value, stringReg);
    loadStringIndexValue(stringReg, output, handleStringEntry);
    jump(&done);
  }

  // Try converting double into integer.
  {
    bind(&isDouble);
    unboxDouble(value, temp);

    if (handleStrings) {
      bind(handleStringRejoin);
    }
    branchTruncateDoubleMaybeModUint32(
        temp, output, truncateDoubleSlow ? truncateDoubleSlow : fail);
    jump(&done);
  }

  // Just unbox a bool, the result is 0 or 1.
  {
    bind(&isBool);
    unboxBoolean(value, output);
    jump(&done);
  }

  // Integers can be unboxed.
  {
    bind(&isInt32);
    unboxInt32(value, output);
  }

  bind(&done);
}

void MacroAssembler::clampValueToUint8(ValueOperand value,
                                       Label* handleStringEntry,
                                       Label* handleStringRejoin,
                                       Register stringReg, FloatRegister temp,
                                       Register output, Label* fail) {
  Label done, isInt32, isBool, isDouble, isNull, isString;
  {
    ScratchTagScope tag(*this, value);
    splitTagForTest(value, tag);

    branchTestInt32(Equal, tag, &isInt32);
    branchTestDouble(Equal, tag, &isDouble);
    branchTestBoolean(Equal, tag, &isBool);
    branchTestNull(Equal, tag, &isNull);
    branchTestString(Equal, tag, &isString);
    branchTestUndefined(Assembler::NotEqual, tag, fail);
  }

  // The value is null or undefined in truncation contexts - just emit 0.
  {
    bind(&isNull);
    move32(Imm32(0), output);
    jump(&done);
  }

  // Try converting a string into a double, then jump to the double case.
  {
    bind(&isString);
    unboxString(value, stringReg);
    jump(handleStringEntry);
  }

  // Try converting double into integer.
  {
    bind(&isDouble);
    unboxDouble(value, temp);
    bind(handleStringRejoin);
    clampDoubleToUint8(temp, output);
    jump(&done);
  }

  // Just unbox a bool, the result is 0 or 1.
  {
    bind(&isBool);
    unboxBoolean(value, output);
    jump(&done);
  }

  // Integers can be unboxed.
  {
    bind(&isInt32);
    unboxInt32(value, output);
    clampIntToUint8(output);
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

OffThreadMacroAssembler::OffThreadMacroAssembler(TempAllocator& alloc,
                                                 CompileRealm* realm)
    : MacroAssembler(alloc, realm->runtime(), realm) {
  MOZ_ASSERT(CurrentThreadIsOffThreadCompiling());
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

void MacroAssembler::moveValue(const TypedOrValueRegister& src,
                               const ValueOperand& dest) {
  if (src.hasValue()) {
    moveValue(src.valueReg(), dest);
    return;
  }

  MIRType type = src.type();
  AnyRegister reg = src.typedReg();

  if (!IsFloatingPointType(type)) {
    boxNonDouble(ValueTypeFromMIRType(type), reg.gpr(), dest);
    return;
  }

  ScratchDoubleScope scratch(*this);
  FloatRegister freg = reg.fpu();
  if (type == MIRType::Float32) {
    convertFloat32ToDouble(freg, scratch);
    freg = scratch;
  }
  boxDouble(freg, dest, scratch);
}

void MacroAssembler::movePropertyKey(PropertyKey key, Register dest) {
  if (key.isGCThing()) {
    // See comment in |Push(PropertyKey, ...)| above for an explanation.
    if (key.isString()) {
      JSString* str = key.toString();
      MOZ_ASSERT((uintptr_t(str) & PropertyKey::TypeMask) == 0);
      static_assert(PropertyKey::StringTypeTag == 0,
                    "need to orPtr StringTypeTag tag if it's not 0");
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

void MacroAssembler::Pop(const Register64 reg) {
#if JS_BITS_PER_WORD == 64
  Pop(reg.reg);
#else
  MOZ_ASSERT(MOZ_LITTLE_ENDIAN(), "Big-endian not supported.");
  Pop(reg.low);
  Pop(reg.high);
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

void MacroAssembler::reserveVMFunctionOutParamSpace(const VMFunctionData& f) {
  switch (f.outParam) {
    case Type_Handle:
      PushEmptyRooted(f.outParamRootType);
      break;

    case Type_Value:
    case Type_Double:
    case Type_Pointer:
    case Type_Int32:
    case Type_Bool:
      reserveStack(f.sizeOfOutParamStackSlot());
      break;

    case Type_Void:
      break;

    case Type_Cell:
      MOZ_CRASH("Unexpected outparam type");
  }
}

void MacroAssembler::loadVMFunctionOutParam(const VMFunctionData& f,
                                            const Address& addr) {
  switch (f.outParam) {
    case Type_Handle:
      switch (f.outParamRootType) {
        case VMFunctionData::RootNone:
          MOZ_CRASH("Handle must have root type");
        case VMFunctionData::RootObject:
        case VMFunctionData::RootString:
        case VMFunctionData::RootCell:
        case VMFunctionData::RootBigInt:
        case VMFunctionData::RootId:
          loadPtr(addr, ReturnReg);
          break;
        case VMFunctionData::RootValue:
          loadValue(addr, JSReturnOperand);
          break;
      }
      break;

    case Type_Value:
      loadValue(addr, JSReturnOperand);
      break;

    case Type_Int32:
      load32(addr, ReturnReg);
      break;

    case Type_Bool:
      load8ZeroExtend(addr, ReturnReg);
      break;

    case Type_Double:
      loadDouble(addr, ReturnDoubleReg);
      break;

    case Type_Pointer:
      loadPtr(addr, ReturnReg);
      break;

    case Type_Void:
      break;

    case Type_Cell:
      MOZ_CRASH("Unexpected outparam type");
  }
}

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
  // On ARM, we need to know what ABI we are using.
  abiArgs_.setUseHardFp(ARMFlags::UseHardFpABI());
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

void MacroAssembler::setupUnalignedABICallDontSaveRestoreSP() {
  andToStackPtr(Imm32(~(ABIStackAlignment - 1)));
  setFramePushed(0);  // Required for aligned callWithABI.
  setupAlignedABICall();
}

void MacroAssembler::setupAlignedABICall() {
  MOZ_ASSERT(!IsCompilingWasm(), "wasm should use setupWasmABICall");
  setupNativeABICall();
  dynamicAlignment_ = false;
}

void MacroAssembler::passABIArg(const MoveOperand& from, ABIType type) {
  MOZ_ASSERT(inCall_);
  appendSignatureType(type);

  ABIArg arg;
  MoveOp::Type moveType;
  switch (type) {
    case ABIType::Float32:
      arg = abiArgs_.next(MIRType::Float32);
      moveType = MoveOp::FLOAT32;
      break;
    case ABIType::Float64:
      arg = abiArgs_.next(MIRType::Double);
      moveType = MoveOp::DOUBLE;
      break;
    case ABIType::General:
      arg = abiArgs_.next(MIRType::Pointer);
      moveType = MoveOp::GENERAL;
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
  propagateOOM(moveResolver_.addMove(from, to, moveType));
}

void MacroAssembler::callWithABINoProfiler(void* fun, ABIType result,
                                           CheckUnsafeCallWithABI check) {
  appendSignatureType(result);
#ifdef JS_SIMULATOR
  fun = Simulator::RedirectNativeFunction(fun, signature());
#endif

  uint32_t stackAdjust;
  callWithABIPre(&stackAdjust);

#ifdef JS_CHECK_UNSAFE_CALL_WITH_ABI
  if (check == CheckUnsafeCallWithABI::Check) {
    // Set the JSContext::inUnsafeCallWithABI flag.
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

#ifdef JS_CHECK_UNSAFE_CALL_WITH_ABI
  if (check == CheckUnsafeCallWithABI::Check) {
    // Check JSContext::inUnsafeCallWithABI was cleared as expected.
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
                                       ABIType result) {
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
      wasm::CallSiteDesc(bytecode.offset(), wasm::CallSiteKind::Symbolic), imm);

  callWithABIPost(stackAdjust, result, /* callFromWasm = */ true);

  return raOffset;
}

void MacroAssembler::callDebugWithABI(wasm::SymbolicAddress imm,
                                      ABIType result) {
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

  // x^y where y < 0 returns a non-int32 value for any x != 1. Except when y is
  // large enough so that the result is no longer representable as a double with
  // fractional parts. We can't easily determine when y is too large, so we bail
  // here.
  // Note: it's important for this condition to match the code in CacheIR.cpp
  // (CanAttachInt32Pow) to prevent failure loops.
  branchTest32(Assembler::Signed, power, power, onOver);

  move32(base, temp1);   // runningSquare = x
  move32(power, temp2);  // n = y

  Label start;
  jump(&start);

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

void MacroAssembler::powPtr(Register base, Register power, Register dest,
                            Register temp1, Register temp2, Label* onOver) {
  // Inline intptr-specialized implementation of BigInt::pow with overflow
  // detection.

  // Negative exponents are disallowed for any BigInts.
  branchTestPtr(Assembler::Signed, power, power, onOver);

  movePtr(ImmWord(1), dest);  // result = 1

  // x^y where x == 1 returns 1 for any y.
  Label done;
  branchPtr(Assembler::Equal, base, ImmWord(1), &done);

  // x^y where x == -1 returns 1 for even y, and -1 for odd y.
  Label notNegativeOne;
  branchPtr(Assembler::NotEqual, base, ImmWord(-1), &notNegativeOne);
  test32MovePtr(Assembler::NonZero, power, Imm32(1), base, dest);
  jump(&done);
  bind(&notNegativeOne);

  // x ** y with |x| > 1 and y >= DigitBits can't be pointer-sized.
  branchPtr(Assembler::GreaterThanOrEqual, power, Imm32(BigInt::DigitBits),
            onOver);

  movePtr(base, temp1);   // runningSquare = x
  movePtr(power, temp2);  // n = y

  Label start;
  jump(&start);

  Label loop;
  bind(&loop);

  // runningSquare *= runningSquare
  branchMulPtr(Assembler::Overflow, temp1, temp1, onOver);

  bind(&start);

  // if ((n & 1) != 0) result *= runningSquare
  Label even;
  branchTest32(Assembler::Zero, temp2, Imm32(1), &even);
  branchMulPtr(Assembler::Overflow, temp1, dest, onOver);
  bind(&even);

  // n >>= 1
  // if (n == 0) return result
  branchRshift32(Assembler::NonZero, Imm32(1), temp2, &loop);

  bind(&done);
}

void MacroAssembler::signInt32(Register input, Register output) {
  MOZ_ASSERT(input != output);

  rshift32Arithmetic(Imm32(31), input, output);
  or32(Imm32(1), output);
  cmp32Move32(Assembler::Equal, input, Imm32(0), input, output);
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

void MacroAssembler::loadAndClearRegExpSearcherLastLimit(Register result,
                                                         Register scratch) {
  MOZ_ASSERT(result != scratch);

  loadJSContext(scratch);

  Address limitField(scratch, JSContext::offsetOfRegExpSearcherLastLimit());
  load32(limitField, result);

#ifdef DEBUG
  Label ok;
  branch32(Assembler::NotEqual, result, Imm32(RegExpSearcherLastLimitSentinel),
           &ok);
  assumeUnreachable("Unexpected sentinel for regExpSearcherLastLimit");
  bind(&ok);
  store32(Imm32(RegExpSearcherLastLimitSentinel), limitField);
#endif
}

void MacroAssembler::loadParsedRegExpShared(Register regexp, Register result,
                                            Label* unparsed) {
  Address sharedSlot(regexp, RegExpObject::offsetOfShared());
  branchTestUndefined(Assembler::Equal, sharedSlot, unparsed);
  unboxNonDouble(sharedSlot, result, JSVAL_TYPE_PRIVATE_GCTHING);

  static_assert(sizeof(RegExpShared::Kind) == sizeof(uint32_t));
  branch32(Assembler::Equal, Address(result, RegExpShared::offsetOfKind()),
           Imm32(int32_t(RegExpShared::Kind::Unparsed)), unparsed);
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
    rshift32(Imm32(JSFunction::ArgCountShift), funFlagsAndArgCount, output);
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
  // If the function is an accessor with lazy name, AtomSlot contains the
  // unprefixed name.
  branchTest32(
      Assembler::NonZero, output,
      Imm32(FunctionFlags::RESOLVED_NAME | FunctionFlags::LAZY_ACCESSOR_NAME),
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
  Label* onNoMatch = cond == Assembler::Equal ? &done : label;

  // Load the object's shape pointer into shapeScratch, and prepare to compare
  // it with the shapes in the list. The shapes are stored as private values so
  // we can compare directly.
  loadPtr(Address(obj, JSObject::offsetOfShape()), shapeScratch);

  // Compute end pointer.
  Address lengthAddr(shapeElements,
                     ObjectElements::offsetOfInitializedLength());
  load32(lengthAddr, endScratch);
  branch32(Assembler::Equal, endScratch, Imm32(0), onNoMatch);
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
  }
  bind(&done);
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

void MacroAssembler::branchTestObjectNeedsProxyResultValidation(
    Condition cond, Register obj, Register scratch, Label* label) {
  MOZ_ASSERT(cond == Assembler::Zero || cond == Assembler::NonZero);

  Label done;
  Label* doValidation = cond == NonZero ? label : &done;
  Label* skipValidation = cond == NonZero ? &done : label;

  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  branchTest32(Assembler::Zero,
               Address(scratch, Shape::offsetOfImmutableFlags()),
               Imm32(Shape::isNativeBit()), doValidation);
  static_assert(sizeof(ObjectFlags) == sizeof(uint16_t));
  load16ZeroExtend(Address(scratch, Shape::offsetOfObjectFlags()), scratch);
  branchTest32(Assembler::NonZero, scratch,
               Imm32(uint32_t(ObjectFlag::NeedsProxyGetSetResultValidation)),
               doValidation);

  loadPtr(Address(obj, JSObject::offsetOfShape()), scratch);
  loadPtr(Address(scratch, Shape::offsetOfBaseShape()), scratch);
  loadPtr(Address(scratch, BaseShape::offsetOfClasp()), scratch);
  loadPtr(Address(scratch, offsetof(JSClass, cOps)), scratch);
  branchTestPtr(Assembler::Zero, scratch, scratch, skipValidation);
  loadPtr(Address(scratch, offsetof(JSClassOps, resolve)), scratch);
  branchTestPtr(Assembler::NonZero, scratch, scratch, doValidation);
  bind(&done);
}

void MacroAssembler::wasmTrap(wasm::Trap trap,
                              const wasm::TrapSiteDesc& trapSiteDesc) {
  FaultingCodeOffset fco = wasmTrapInstruction();
  MOZ_ASSERT_IF(!oom(),
                currentOffset() - fco.get() == WasmTrapInstructionLength);

  append(trap, wasm::TrapMachineInsn::OfficialUD, fco.get(), trapSiteDesc);
}

std::pair<CodeOffset, uint32_t> MacroAssembler::wasmReserveStackChecked(
    uint32_t amount, const wasm::TrapSiteDesc& trapSiteDesc) {
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
    wasmTrap(wasm::Trap::StackOverflow, trapSiteDesc);
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
  wasmTrap(wasm::Trap::StackOverflow, trapSiteDesc);
  CodeOffset trapInsnOffset = CodeOffset(currentOffset());
  bind(&ok);
  return std::pair<CodeOffset, uint32_t>(trapInsnOffset, amount);
}

static void MoveDataBlock(MacroAssembler& masm, Register base, int32_t from,
                          int32_t to, uint32_t size) {
  MOZ_ASSERT(base != masm.getStackPointer());
  if (from == to || size == 0) {
    return;  // noop
  }

#ifdef JS_CODEGEN_ARM64
  vixl::UseScratchRegisterScope temps(&masm);
  const Register scratch = temps.AcquireX().asUnsized();
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_X86)
  static constexpr Register scratch = ABINonArgReg0;
  masm.push(scratch);
#elif defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_RISCV64)
  ScratchRegisterScope scratch(masm);
#elif !defined(JS_CODEGEN_NONE)
  const Register scratch = ScratchReg;
#else
  const Register scratch = InvalidReg;
#endif

  if (to < from) {
    for (uint32_t i = 0; i < size; i += sizeof(void*)) {
      masm.loadPtr(Address(base, from + i), scratch);
      masm.storePtr(scratch, Address(base, to + i));
    }
  } else {
    for (uint32_t i = size; i > 0;) {
      i -= sizeof(void*);
      masm.loadPtr(Address(base, from + i), scratch);
      masm.storePtr(scratch, Address(base, to + i));
    }
  }

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_X86)
  masm.pop(scratch);
#endif
}

struct ReturnCallTrampolineData {
#ifdef JS_CODEGEN_ARM
  uint32_t trampolineOffset;
#else
  CodeLabel trampoline;
#endif
};

static ReturnCallTrampolineData MakeReturnCallTrampoline(MacroAssembler& masm) {
  uint32_t savedPushed = masm.framePushed();

  ReturnCallTrampolineData data;

  {
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
    AutoForbidPoolsAndNops afp(&masm, 1);
#elif defined(JS_CODEGEN_RISCV64)
    BlockTrampolinePoolScope block_trampoline_pool(&masm, 1);
#endif

    // Build simple trampoline code: load the instance slot from the frame,
    // restore FP, and return to prevous caller.
#ifdef JS_CODEGEN_ARM
    data.trampolineOffset = masm.currentOffset();
#else
    masm.bind(&data.trampoline);
#endif

    masm.setFramePushed(AlignBytes(
        wasm::FrameWithInstances::sizeOfInstanceFieldsAndShadowStack(),
        WasmStackAlignment));

    masm.wasmMarkCallAsSlow();
  }

  masm.loadPtr(
      Address(masm.getStackPointer(), WasmCallerInstanceOffsetBeforeCall),
      InstanceReg);
  masm.switchToWasmInstanceRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
  masm.moveToStackPtr(FramePointer);
#ifdef JS_CODEGEN_ARM64
  masm.pop(FramePointer, lr);
  masm.append(wasm::CodeRangeUnwindInfo::UseFpLr, masm.currentOffset());
  masm.Mov(PseudoStackPointer64, vixl::sp);
  masm.abiret();
#else
  masm.pop(FramePointer);
  masm.append(wasm::CodeRangeUnwindInfo::UseFp, masm.currentOffset());
  masm.ret();
#endif

  masm.append(wasm::CodeRangeUnwindInfo::Normal, masm.currentOffset());
  masm.setFramePushed(savedPushed);
  return data;
}

// CollapseWasmFrame methods merge frames fields: callee parameters, instance
// slots, and caller RA. See the diagram below. The C0 is the previous caller,
// the C1 is the caller of the return call, and the C2 is the callee.
//
//    +-------------------+          +--------------------+
//    |C0 instance slots  |          |C0 instance slots   |
//    +-------------------+ -+       +--------------------+ -+
//    |   RA              |  |       |   RA               |  |
//    +-------------------+  | C0    +--------------------+  |C0
//    |   FP              |  v       |   FP               |  v
//    +-------------------+          +--------------------+
//    |C0 private frame   |          |C0 private frame    |
//    +-------------------+          +--------------------+
//    |C1 results area    |          |C1/C2 results area  |
//    +-------------------+          +--------------------+
//    |C1 parameters      |          |? trampoline frame  |
//    +-------------------+          +--------------------+
//    |C1 instance slots  |          |C2 parameters       |
//    +-------------------+ -+       +--------------------+
//    |C0 RA              |  |       |C2 instance slots’  |
//    +-------------------+  | C1    +--------------------+ -+
//    |C0 FP              |  v       |C0 RA’              |  |
//    +-------------------+          +--------------------+  | C2
//    |C1 private frame   |          |C0 FP’              |  v
//    +-------------------+          +--------------------+ <= start of C2
//    |C2 parameters      |
//    +-------------------+
//    |C2 instance slots  |
//    +-------------------+ <= call C2
//
// The C2 parameters are moved in place of the C1 parameters, and the
// C1 frame data is removed. The instance slots, return address, and
// frame pointer to the C0 callsite are saved or adjusted.
//
// For cross-instance calls, the trampoline frame will be introduced
// if the C0 callsite has no ability to restore instance registers and realm.

static void CollapseWasmFrameFast(MacroAssembler& masm,
                                  const ReturnCallAdjustmentInfo& retCallInfo) {
  uint32_t framePushedAtStart = masm.framePushed();
  static_assert(sizeof(wasm::Frame) == 2 * sizeof(void*));

  // The instance slots + stack arguments are expected to be padded and
  // aligned to the WasmStackAlignment boundary. There is no data expected
  // in the padded region, such as results stack area or locals, to avoid
  // unwanted stack growth.
  uint32_t newSlotsAndStackArgBytes =
      AlignBytes(retCallInfo.newSlotsAndStackArgBytes, WasmStackAlignment);
  uint32_t oldSlotsAndStackArgBytes =
      AlignBytes(retCallInfo.oldSlotsAndStackArgBytes, WasmStackAlignment);

  static constexpr Register tempForCaller = WasmTailCallInstanceScratchReg;
  static constexpr Register tempForFP = WasmTailCallFPScratchReg;
  static constexpr Register tempForRA = WasmTailCallRAScratchReg;
#ifndef JS_USE_LINK_REGISTER
  masm.push(tempForRA);
#endif

  // Load the FP, RA, and instance slots into registers to preserve them while
  // the new frame is collapsed over the current one.
  masm.loadPtr(Address(FramePointer, wasm::Frame::callerFPOffset()), tempForFP);
  masm.loadPtr(Address(FramePointer, wasm::Frame::returnAddressOffset()),
               tempForRA);
  masm.append(wasm::CodeRangeUnwindInfo::RestoreFpRa, masm.currentOffset());
  bool copyCallerSlot = oldSlotsAndStackArgBytes != newSlotsAndStackArgBytes;
  if (copyCallerSlot) {
    masm.loadPtr(
        Address(FramePointer, wasm::FrameWithInstances::callerInstanceOffset()),
        tempForCaller);
  }

  // Copy parameters data, ignoring shadow data and instance slots.
  // Make all offsets relative to the FramePointer.
  int32_t newArgSrc = -framePushedAtStart;
  int32_t newArgDest =
      sizeof(wasm::Frame) + oldSlotsAndStackArgBytes - newSlotsAndStackArgBytes;
  const uint32_t SlotsSize =
      wasm::FrameWithInstances::sizeOfInstanceFieldsAndShadowStack();
  MoveDataBlock(masm, FramePointer, newArgSrc + SlotsSize,
                newArgDest + SlotsSize,
                retCallInfo.newSlotsAndStackArgBytes - SlotsSize);

  // Copy caller instance slots from the current frame.
  if (copyCallerSlot) {
    masm.storePtr(
        tempForCaller,
        Address(FramePointer, newArgDest + WasmCallerInstanceOffsetBeforeCall));
  }

  // Store current instance as the new callee instance slot.
  masm.storePtr(
      InstanceReg,
      Address(FramePointer, newArgDest + WasmCalleeInstanceOffsetBeforeCall));

#ifdef JS_USE_LINK_REGISTER
  // RA is already in its place, just move stack.
  masm.addToStackPtr(Imm32(framePushedAtStart + newArgDest));
#else
  // Push RA to new frame: store RA, restore temp, and move stack.
  int32_t newFrameOffset = newArgDest - sizeof(wasm::Frame);
  masm.storePtr(tempForRA,
                Address(FramePointer,
                        newFrameOffset + wasm::Frame::returnAddressOffset()));
  // Restore tempForRA, but keep RA on top of the stack.
  // There is no non-locking exchange instruction between register and memory.
  // Using tempForCaller as scratch register.
  masm.loadPtr(Address(masm.getStackPointer(), 0), tempForCaller);
  masm.storePtr(tempForRA, Address(masm.getStackPointer(), 0));
  masm.mov(tempForCaller, tempForRA);
  masm.append(wasm::CodeRangeUnwindInfo::RestoreFp, masm.currentOffset());
  masm.addToStackPtr(Imm32(framePushedAtStart + newFrameOffset +
                           wasm::Frame::returnAddressOffset() + sizeof(void*)));
#endif

  masm.movePtr(tempForFP, FramePointer);
  // Setting framePushed to pre-collapse state, to properly set that in the
  // following code.
  masm.setFramePushed(framePushedAtStart);
}

static void CollapseWasmFrameSlow(MacroAssembler& masm,
                                  const ReturnCallAdjustmentInfo& retCallInfo,
                                  wasm::CallSiteDesc desc,
                                  ReturnCallTrampolineData data) {
  uint32_t framePushedAtStart = masm.framePushed();
  static constexpr Register tempForCaller = WasmTailCallInstanceScratchReg;
  static constexpr Register tempForFP = WasmTailCallFPScratchReg;
  static constexpr Register tempForRA = WasmTailCallRAScratchReg;

  static_assert(sizeof(wasm::Frame) == 2 * sizeof(void*));

  // The hidden frame will "break" after wasm::Frame data fields.
  // Calculate sum of wasm stack alignment before and after the break as
  // the size to reserve.
  const uint32_t HiddenFrameAfterSize =
      AlignBytes(wasm::FrameWithInstances::sizeOfInstanceFieldsAndShadowStack(),
                 WasmStackAlignment);
  const uint32_t HiddenFrameSize =
      AlignBytes(sizeof(wasm::Frame), WasmStackAlignment) +
      HiddenFrameAfterSize;

  // If it is not slow, prepare two frame: one is regular wasm frame, and
  // another one is hidden. The hidden frame contains one instance slots
  // for unwind and recovering pinned registers.
  // The instance slots + stack arguments are expected to be padded and
  // aligned to the WasmStackAlignment boundary. There is no data expected
  // in the padded region, such as results stack area or locals, to avoid
  // unwanted stack growth.
  // The Hidden frame will be inserted with this constraint too.
  uint32_t newSlotsAndStackArgBytes =
      AlignBytes(retCallInfo.newSlotsAndStackArgBytes, WasmStackAlignment);
  uint32_t oldSlotsAndStackArgBytes =
      AlignBytes(retCallInfo.oldSlotsAndStackArgBytes, WasmStackAlignment);

  // Make all offsets relative to the FramePointer.
  int32_t newArgSrc = -framePushedAtStart;
  int32_t newArgDest = sizeof(wasm::Frame) + oldSlotsAndStackArgBytes -
                       HiddenFrameSize - newSlotsAndStackArgBytes;
  int32_t hiddenFrameArgsDest =
      sizeof(wasm::Frame) + oldSlotsAndStackArgBytes - HiddenFrameAfterSize;

  // It will be possible to overwrite data (on the top of the stack) due to
  // the added hidden frame, reserve needed space.
  uint32_t reserved = newArgDest - int32_t(sizeof(void*)) < newArgSrc
                          ? newArgSrc - newArgDest + sizeof(void*)
                          : 0;
  masm.reserveStack(reserved);

#ifndef JS_USE_LINK_REGISTER
  masm.push(tempForRA);
#endif

  // Load FP, RA and instance slots to preserve them from being overwritten.
  masm.loadPtr(Address(FramePointer, wasm::Frame::callerFPOffset()), tempForFP);
  masm.loadPtr(Address(FramePointer, wasm::Frame::returnAddressOffset()),
               tempForRA);
  masm.append(wasm::CodeRangeUnwindInfo::RestoreFpRa, masm.currentOffset());
  masm.loadPtr(
      Address(FramePointer, newArgSrc + WasmCallerInstanceOffsetBeforeCall),
      tempForCaller);

  // Copy parameters data, ignoring shadow data and instance slots.
  const uint32_t SlotsSize =
      wasm::FrameWithInstances::sizeOfInstanceFieldsAndShadowStack();
  MoveDataBlock(masm, FramePointer, newArgSrc + SlotsSize,
                newArgDest + SlotsSize,
                retCallInfo.newSlotsAndStackArgBytes - SlotsSize);

  // Form hidden frame for trampoline.
  int32_t newFPOffset = hiddenFrameArgsDest - sizeof(wasm::Frame);
  masm.storePtr(
      tempForRA,
      Address(FramePointer, newFPOffset + wasm::Frame::returnAddressOffset()));

  // Copy original FP.
  masm.storePtr(
      tempForFP,
      Address(FramePointer, newFPOffset + wasm::Frame::callerFPOffset()));

  // Set up instance slots.
  masm.storePtr(
      tempForCaller,
      Address(FramePointer,
              newFPOffset + wasm::FrameWithInstances::calleeInstanceOffset()));
  masm.storePtr(
      tempForCaller,
      Address(FramePointer, newArgDest + WasmCallerInstanceOffsetBeforeCall));
  masm.storePtr(
      InstanceReg,
      Address(FramePointer, newArgDest + WasmCalleeInstanceOffsetBeforeCall));

#ifdef JS_CODEGEN_ARM
  // ARM has no CodeLabel -- calculate PC directly.
  masm.mov(pc, tempForRA);
  masm.computeEffectiveAddress(
      Address(tempForRA,
              int32_t(data.trampolineOffset - masm.currentOffset() - 4)),
      tempForRA);
  masm.append(desc, CodeOffset(data.trampolineOffset));
#else
  masm.mov(&data.trampoline, tempForRA);

  masm.addCodeLabel(data.trampoline);
  // Add slow trampoline callsite description, to be annotated in
  // stack/frame iterators.
  masm.append(desc, *data.trampoline.target());
#endif

#ifdef JS_USE_LINK_REGISTER
  masm.freeStack(reserved);
  // RA is already in its place, just move stack.
  masm.addToStackPtr(Imm32(framePushedAtStart + newArgDest));
#else
  // Push RA to new frame: store RA, restore temp, and move stack.
  int32_t newFrameOffset = newArgDest - sizeof(wasm::Frame);
  masm.storePtr(tempForRA,
                Address(FramePointer,
                        newFrameOffset + wasm::Frame::returnAddressOffset()));
  // Restore tempForRA, but keep RA on top of the stack.
  // There is no non-locking exchange instruction between register and memory.
  // Using tempForCaller as scratch register.
  masm.loadPtr(Address(masm.getStackPointer(), 0), tempForCaller);
  masm.storePtr(tempForRA, Address(masm.getStackPointer(), 0));
  masm.mov(tempForCaller, tempForRA);
  masm.append(wasm::CodeRangeUnwindInfo::RestoreFp, masm.currentOffset());
  masm.addToStackPtr(Imm32(framePushedAtStart + newFrameOffset +
                           wasm::Frame::returnAddressOffset() + reserved +
                           sizeof(void*)));
#endif

  // Point FramePointer to hidden frame.
  masm.computeEffectiveAddress(Address(FramePointer, newFPOffset),
                               FramePointer);
  // Setting framePushed to pre-collapse state, to properly set that in the
  // following code.
  masm.setFramePushed(framePushedAtStart);
}

void MacroAssembler::wasmCollapseFrameFast(
    const ReturnCallAdjustmentInfo& retCallInfo) {
  CollapseWasmFrameFast(*this, retCallInfo);
}

void MacroAssembler::wasmCollapseFrameSlow(
    const ReturnCallAdjustmentInfo& retCallInfo, wasm::CallSiteDesc desc) {
  static constexpr Register temp1 = ABINonArgReg1;
  static constexpr Register temp2 = ABINonArgReg3;

  // Check if RA has slow marker. If there is no marker, generate a trampoline
  // frame to restore register state when this tail call returns.

  Label slow, done;
  loadPtr(Address(FramePointer, wasm::Frame::returnAddressOffset()), temp1);
  wasmCheckSlowCallsite(temp1, &slow, temp1, temp2);
  CollapseWasmFrameFast(*this, retCallInfo);
  jump(&done);
  append(wasm::CodeRangeUnwindInfo::Normal, currentOffset());

  ReturnCallTrampolineData data = MakeReturnCallTrampoline(*this);

  bind(&slow);
  CollapseWasmFrameSlow(*this, retCallInfo, desc, data);

  bind(&done);
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
  loadWasmPinnedRegsFromInstance(mozilla::Nothing());

  return wasmMarkedSlowCall(desc, ABINonArgReg0);
}

CodeOffset MacroAssembler::wasmReturnCallImport(
    const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee,
    const ReturnCallAdjustmentInfo& retCallInfo) {
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
  loadWasmPinnedRegsFromInstance(mozilla::Nothing());

  wasm::CallSiteDesc stubDesc(desc.lineOrBytecode(),
                              wasm::CallSiteKind::ReturnStub);
  wasmCollapseFrameSlow(retCallInfo, stubDesc);
  jump(ABINonArgReg0);
  append(wasm::CodeRangeUnwindInfo::Normal, currentOffset());
  return CodeOffset(currentOffset());
}

CodeOffset MacroAssembler::wasmReturnCall(
    const wasm::CallSiteDesc& desc, uint32_t funcDefIndex,
    const ReturnCallAdjustmentInfo& retCallInfo) {
  wasmCollapseFrameFast(retCallInfo);
  CodeOffset offset = farJumpWithPatch();
  append(desc, offset, funcDefIndex);
  append(wasm::CodeRangeUnwindInfo::Normal, currentOffset());
  return offset;
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
  wasmTrapOnFailedInstanceCall(ReturnReg, failureMode, desc.toTrapSiteDesc());

  return ret;
}

void MacroAssembler::wasmTrapOnFailedInstanceCall(
    Register resultRegister, wasm::FailureMode failureMode,
    const wasm::TrapSiteDesc& trapSiteDesc) {
  Label noTrap;
  switch (failureMode) {
    case wasm::FailureMode::Infallible:
      return;
    case wasm::FailureMode::FailOnNegI32:
      branchTest32(Assembler::NotSigned, resultRegister, resultRegister,
                   &noTrap);
      break;
    case wasm::FailureMode::FailOnMaxI32:
      branchPtr(Assembler::NotEqual, resultRegister,
                ImmWord(uintptr_t(INT32_MAX)), &noTrap);
      break;
    case wasm::FailureMode::FailOnNullPtr:
      branchTestPtr(Assembler::NonZero, resultRegister, resultRegister,
                    &noTrap);
      break;
    case wasm::FailureMode::FailOnInvalidRef:
      branchPtr(Assembler::NotEqual, resultRegister,
                ImmWord(uintptr_t(wasm::AnyRef::invalid().forCompiledCode())),
                &noTrap);
      break;
  }
  wasmTrap(wasm::Trap::ThrowReported, trapSiteDesc);
  bind(&noTrap);
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
                                       callIndirectId.instanceDataOffset() +
                                       offsetof(wasm::TypeDefInstanceData,
                                                superTypeVector))),
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
  loadWasmPinnedRegsFromInstance(mozilla::Some(desc.toTrapSiteDesc()));
#else
  MOZ_ASSERT(nullCheckFailedLabel != nullptr);
  branchTestPtr(Assembler::Zero, InstanceReg, InstanceReg,
                nullCheckFailedLabel);

  loadWasmPinnedRegsFromInstance(mozilla::Nothing());
#endif
  switchToWasmInstanceRealm(index, WasmTableCallScratchReg1);

  loadPtr(Address(calleeScratch, offsetof(wasm::FunctionTableElem, code)),
          calleeScratch);

  *slowCallOffset = wasmMarkedSlowCall(desc, calleeScratch);

  // Restore registers and realm and join up with the fast path.

  loadPtr(Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall),
          InstanceReg);
  loadWasmPinnedRegsFromInstance(mozilla::Nothing());
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
                             wasm::CallSiteKind::IndirectFast);
  *fastCallOffset = call(newDesc, calleeScratch);

  bind(&done);
}

void MacroAssembler::wasmReturnCallIndirect(
    const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee,
    Label* boundsCheckFailedLabel, Label* nullCheckFailedLabel,
    mozilla::Maybe<uint32_t> tableSize,
    const ReturnCallAdjustmentInfo& retCallInfo) {
  static_assert(sizeof(wasm::FunctionTableElem) == 2 * sizeof(void*),
                "Exactly two pointers or index scaling won't work correctly");
  MOZ_ASSERT(callee.which() == wasm::CalleeDesc::WasmTable);

  const int shift = sizeof(wasm::FunctionTableElem) == 8 ? 3 : 4;
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
                                       callIndirectId.instanceDataOffset() +
                                       offsetof(wasm::TypeDefInstanceData,
                                                superTypeVector))),
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

  // Slow path: Save context, check for null, setup new context.

  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall));
  movePtr(newInstanceTemp, InstanceReg);

#ifdef WASM_HAS_HEAPREG
  // Use the null pointer exception resulting from loading HeapReg from a null
  // instance to handle a call to a null slot.
  MOZ_ASSERT(nullCheckFailedLabel == nullptr);
  loadWasmPinnedRegsFromInstance(mozilla::Some(desc.toTrapSiteDesc()));
#else
  MOZ_ASSERT(nullCheckFailedLabel != nullptr);
  branchTestPtr(Assembler::Zero, InstanceReg, InstanceReg,
                nullCheckFailedLabel);

  loadWasmPinnedRegsFromInstance(mozilla::Nothing());
#endif
  switchToWasmInstanceRealm(index, WasmTableCallScratchReg1);

  loadPtr(Address(calleeScratch, offsetof(wasm::FunctionTableElem, code)),
          calleeScratch);

  wasm::CallSiteDesc stubDesc(desc.lineOrBytecode(),
                              wasm::CallSiteKind::ReturnStub);
  wasmCollapseFrameSlow(retCallInfo, stubDesc);
  jump(calleeScratch);
  append(wasm::CodeRangeUnwindInfo::Normal, currentOffset());

  // Fast path: just load the code pointer and go.

  bind(&fastCall);

  loadPtr(Address(calleeScratch, offsetof(wasm::FunctionTableElem, code)),
          calleeScratch);

  wasmCollapseFrameFast(retCallInfo);
  jump(calleeScratch);
  append(wasm::CodeRangeUnwindInfo::Normal, currentOffset());
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
  FaultingCodeOffset fco =
      loadPtr(Address(calleeFnObj, instanceSlotOffset), newInstanceTemp);
  append(wasm::Trap::NullPointerDereference, wasm::TrapMachineInsnForLoadWord(),
         fco.get(), desc.toTrapSiteDesc());
  branchPtr(Assembler::Equal, InstanceReg, newInstanceTemp, &fastCall);

  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall));
  movePtr(newInstanceTemp, InstanceReg);
  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCalleeInstanceOffsetBeforeCall));

  loadWasmPinnedRegsFromInstance(mozilla::Nothing());
  switchToWasmInstanceRealm(WasmCallRefCallScratchReg0,
                            WasmCallRefCallScratchReg1);

  // Get funcUncheckedCallEntry() from the function's
  // WASM_FUNC_UNCHECKED_ENTRY_SLOT extended slot.
  size_t uncheckedEntrySlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_FUNC_UNCHECKED_ENTRY_SLOT);
  loadPtr(Address(calleeFnObj, uncheckedEntrySlotOffset), calleeScratch);

  *slowCallOffset = wasmMarkedSlowCall(desc, calleeScratch);

  // Restore registers and realm and back to this caller's.
  loadPtr(Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall),
          InstanceReg);
  loadWasmPinnedRegsFromInstance(mozilla::Nothing());
  switchToWasmInstanceRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
  jump(&done);

  // Fast path: just load WASM_FUNC_UNCHECKED_ENTRY_SLOT value and go.
  // The instance and pinned registers are the same as in the caller.

  bind(&fastCall);

  loadPtr(Address(calleeFnObj, uncheckedEntrySlotOffset), calleeScratch);

  // We use a different type of call site for the fast call since the instance
  // slots in the frame do not have valid values.

  wasm::CallSiteDesc newDesc(desc.lineOrBytecode(),
                             wasm::CallSiteKind::FuncRefFast);
  *fastCallOffset = call(newDesc, calleeScratch);

  bind(&done);
}

void MacroAssembler::wasmReturnCallRef(
    const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee,
    const ReturnCallAdjustmentInfo& retCallInfo) {
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
  FaultingCodeOffset fco =
      loadPtr(Address(calleeFnObj, instanceSlotOffset), newInstanceTemp);
  append(wasm::Trap::NullPointerDereference, wasm::TrapMachineInsnForLoadWord(),
         fco.get(), desc.toTrapSiteDesc());
  branchPtr(Assembler::Equal, InstanceReg, newInstanceTemp, &fastCall);

  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCallerInstanceOffsetBeforeCall));
  movePtr(newInstanceTemp, InstanceReg);
  storePtr(InstanceReg,
           Address(getStackPointer(), WasmCalleeInstanceOffsetBeforeCall));

  loadWasmPinnedRegsFromInstance(mozilla::Nothing());
  switchToWasmInstanceRealm(WasmCallRefCallScratchReg0,
                            WasmCallRefCallScratchReg1);

  // Get funcUncheckedCallEntry() from the function's
  // WASM_FUNC_UNCHECKED_ENTRY_SLOT extended slot.
  size_t uncheckedEntrySlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_FUNC_UNCHECKED_ENTRY_SLOT);
  loadPtr(Address(calleeFnObj, uncheckedEntrySlotOffset), calleeScratch);

  wasm::CallSiteDesc stubDesc(desc.lineOrBytecode(),
                              wasm::CallSiteKind::ReturnStub);
  wasmCollapseFrameSlow(retCallInfo, stubDesc);
  jump(calleeScratch);
  append(wasm::CodeRangeUnwindInfo::Normal, currentOffset());

  // Fast path: just load WASM_FUNC_UNCHECKED_ENTRY_SLOT value and go.
  // The instance and pinned registers are the same as in the caller.

  bind(&fastCall);

  loadPtr(Address(calleeFnObj, uncheckedEntrySlotOffset), calleeScratch);

  wasmCollapseFrameFast(retCallInfo);
  jump(calleeScratch);
  append(wasm::CodeRangeUnwindInfo::Normal, currentOffset());
}

void MacroAssembler::wasmBoundsCheckRange32(
    Register index, Register length, Register limit, Register tmp,
    const wasm::TrapSiteDesc& trapSiteDesc) {
  Label ok;
  Label fail;

  mov(index, tmp);
  branchAdd32(Assembler::CarrySet, length, tmp, &fail);
  branch32(Assembler::Above, tmp, limit, &fail);
  jump(&ok);

  bind(&fail);
  wasmTrap(wasm::Trap::OutOfBounds, trapSiteDesc);

  bind(&ok);
}

#ifdef ENABLE_WASM_MEMORY64
void MacroAssembler::wasmClampTable64Address(Register64 address, Register out) {
  Label oob;
  Label ret;
  branch64(Assembler::Above, address, Imm64(UINT32_MAX), &oob);
  move64To32(address, out);
  jump(&ret);
  bind(&oob);
  static_assert(wasm::MaxTableElemsRuntime < UINT32_MAX);
  move32(Imm32(UINT32_MAX), out);
  bind(&ret);
};
#endif

BranchWasmRefIsSubtypeRegisters MacroAssembler::regsForBranchWasmRefIsSubtype(
    wasm::RefType type) {
  MOZ_ASSERT(type.isValid());
  switch (type.hierarchy()) {
    case wasm::RefTypeHierarchy::Any:
      return BranchWasmRefIsSubtypeRegisters{
          .needSuperSTV = type.isTypeRef(),
          .needScratch1 = !type.isNone() && !type.isAny(),
          .needScratch2 = type.isTypeRef() && !type.typeDef()->isFinal() &&
                          type.typeDef()->subTypingDepth() >=
                              wasm::MinSuperTypeVectorLength,
      };
    case wasm::RefTypeHierarchy::Func:
      return BranchWasmRefIsSubtypeRegisters{
          .needSuperSTV = type.isTypeRef(),
          .needScratch1 = type.isTypeRef(),
          .needScratch2 = type.isTypeRef() && !type.typeDef()->isFinal() &&
                          type.typeDef()->subTypingDepth() >=
                              wasm::MinSuperTypeVectorLength,
      };
    case wasm::RefTypeHierarchy::Extern:
    case wasm::RefTypeHierarchy::Exn:
      return BranchWasmRefIsSubtypeRegisters{
          .needSuperSTV = false,
          .needScratch1 = false,
          .needScratch2 = false,
      };
    default:
      MOZ_CRASH("unknown type hierarchy for cast");
  }
}

void MacroAssembler::branchWasmRefIsSubtype(
    Register ref, wasm::MaybeRefType sourceType, wasm::RefType destType,
    Label* label, bool onSuccess, Register superSTV, Register scratch1,
    Register scratch2) {
  switch (destType.hierarchy()) {
    case wasm::RefTypeHierarchy::Any: {
      branchWasmRefIsSubtypeAny(ref, sourceType.valueOr(wasm::RefType::any()),
                                destType, label, onSuccess, superSTV, scratch1,
                                scratch2);
    } break;
    case wasm::RefTypeHierarchy::Func: {
      branchWasmRefIsSubtypeFunc(ref, sourceType.valueOr(wasm::RefType::func()),
                                 destType, label, onSuccess, superSTV, scratch1,
                                 scratch2);
    } break;
    case wasm::RefTypeHierarchy::Extern: {
      branchWasmRefIsSubtypeExtern(ref,
                                   sourceType.valueOr(wasm::RefType::extern_()),
                                   destType, label, onSuccess);
    } break;
    case wasm::RefTypeHierarchy::Exn: {
      branchWasmRefIsSubtypeExn(ref, sourceType.valueOr(wasm::RefType::exn()),
                                destType, label, onSuccess);
    } break;
    default:
      MOZ_CRASH("unknown type hierarchy for wasm cast");
  }
}

void MacroAssembler::branchWasmRefIsSubtypeAny(
    Register ref, wasm::RefType sourceType, wasm::RefType destType,
    Label* label, bool onSuccess, Register superSTV, Register scratch1,
    Register scratch2) {
  MOZ_ASSERT(sourceType.isValid());
  MOZ_ASSERT(destType.isValid());
  MOZ_ASSERT(sourceType.isAnyHierarchy());
  MOZ_ASSERT(destType.isAnyHierarchy());

  mozilla::DebugOnly<BranchWasmRefIsSubtypeRegisters> needs =
      regsForBranchWasmRefIsSubtype(destType);
  MOZ_ASSERT_IF(needs.value.needSuperSTV, superSTV != Register::Invalid());
  MOZ_ASSERT_IF(needs.value.needScratch1, scratch1 != Register::Invalid());
  MOZ_ASSERT_IF(needs.value.needScratch2, scratch2 != Register::Invalid());

  Label fallthrough;
  Label* successLabel = onSuccess ? label : &fallthrough;
  Label* failLabel = onSuccess ? &fallthrough : label;
  Label* nullLabel = destType.isNullable() ? successLabel : failLabel;

  auto finishSuccess = [&]() {
    if (successLabel != &fallthrough) {
      jump(successLabel);
    }
    bind(&fallthrough);
  };
  auto finishFail = [&]() {
    if (failLabel != &fallthrough) {
      jump(failLabel);
    }
    bind(&fallthrough);
  };

  // Check for null.
  if (sourceType.isNullable()) {
    branchWasmAnyRefIsNull(true, ref, nullLabel);
  }

  // The only value that can inhabit 'none' is null. So, early out if we got
  // not-null.
  if (destType.isNone()) {
    finishFail();
    return;
  }

  if (destType.isAny()) {
    // No further checks for 'any'
    finishSuccess();
    return;
  }

  // 'type' is now 'eq' or lower, which currently will either be a gc object or
  // an i31.

  // Check first for i31 values, and get them out of the way. i31 values are
  // valid when casting to i31 or eq, and invalid otherwise.
  if (destType.isI31() || destType.isEq()) {
    branchWasmAnyRefIsI31(true, ref, successLabel);

    if (destType.isI31()) {
      // No further checks for 'i31'
      finishFail();
      return;
    }
  }

  // Then check for any kind of gc object.
  MOZ_ASSERT(scratch1 != Register::Invalid());
  if (!wasm::RefType::isSubTypeOf(sourceType, wasm::RefType::struct_()) &&
      !wasm::RefType::isSubTypeOf(sourceType, wasm::RefType::array())) {
    branchWasmAnyRefIsObjectOrNull(false, ref, failLabel);
    branchObjectIsWasmGcObject(false, ref, scratch1, failLabel);
  }

  if (destType.isEq()) {
    // No further checks for 'eq'
    finishSuccess();
    return;
  }

  // 'type' is now 'struct', 'array', or a concrete type. (Bottom types and i31
  // were handled above.)
  //
  // Casting to a concrete type only requires a simple check on the
  // object's super type vector. Casting to an abstract type (struct, array)
  // requires loading the object's superTypeVector->typeDef->kind, and checking
  // that it is correct.

  loadPtr(Address(ref, int32_t(WasmGcObject::offsetOfSuperTypeVector())),
          scratch1);
  if (destType.isTypeRef()) {
    // Concrete type, do superTypeVector check.
    branchWasmSTVIsSubtype(scratch1, superSTV, scratch2, destType.typeDef(),
                           label, onSuccess);
    bind(&fallthrough);
    return;
  }

  // Abstract type, do kind check
  loadPtr(
      Address(scratch1, int32_t(wasm::SuperTypeVector::offsetOfSelfTypeDef())),
      scratch1);
  load8ZeroExtend(Address(scratch1, int32_t(wasm::TypeDef::offsetOfKind())),
                  scratch1);
  branch32(onSuccess ? Assembler::Equal : Assembler::NotEqual, scratch1,
           Imm32(int32_t(destType.typeDefKind())), label);
  bind(&fallthrough);
}

void MacroAssembler::branchWasmRefIsSubtypeFunc(
    Register ref, wasm::RefType sourceType, wasm::RefType destType,
    Label* label, bool onSuccess, Register superSTV, Register scratch1,
    Register scratch2) {
  MOZ_ASSERT(sourceType.isValid());
  MOZ_ASSERT(destType.isValid());
  MOZ_ASSERT(sourceType.isFuncHierarchy());
  MOZ_ASSERT(destType.isFuncHierarchy());

  mozilla::DebugOnly<BranchWasmRefIsSubtypeRegisters> needs =
      regsForBranchWasmRefIsSubtype(destType);
  MOZ_ASSERT_IF(needs.value.needSuperSTV, superSTV != Register::Invalid());
  MOZ_ASSERT_IF(needs.value.needScratch1, scratch1 != Register::Invalid());
  MOZ_ASSERT_IF(needs.value.needScratch2, scratch2 != Register::Invalid());

  Label fallthrough;
  Label* successLabel = onSuccess ? label : &fallthrough;
  Label* failLabel = onSuccess ? &fallthrough : label;
  Label* nullLabel = destType.isNullable() ? successLabel : failLabel;

  auto finishSuccess = [&]() {
    if (successLabel != &fallthrough) {
      jump(successLabel);
    }
    bind(&fallthrough);
  };
  auto finishFail = [&]() {
    if (failLabel != &fallthrough) {
      jump(failLabel);
    }
    bind(&fallthrough);
  };

  // Check for null.
  if (sourceType.isNullable()) {
    branchTestPtr(Assembler::Zero, ref, ref, nullLabel);
  }

  // The only value that can inhabit 'nofunc' is null. So, early out if we got
  // not-null.
  if (destType.isNoFunc()) {
    finishFail();
    return;
  }

  if (destType.isFunc()) {
    // No further checks for 'func' (any func)
    finishSuccess();
    return;
  }

  // In the func hierarchy, a supertype vector check is now sufficient for all
  // remaining cases.
  loadPrivate(Address(ref, int32_t(FunctionExtended::offsetOfWasmSTV())),
              scratch1);
  branchWasmSTVIsSubtype(scratch1, superSTV, scratch2, destType.typeDef(),
                         label, onSuccess);
  bind(&fallthrough);
}

void MacroAssembler::branchWasmRefIsSubtypeExtern(Register ref,
                                                  wasm::RefType sourceType,
                                                  wasm::RefType destType,
                                                  Label* label,
                                                  bool onSuccess) {
  MOZ_ASSERT(sourceType.isValid());
  MOZ_ASSERT(destType.isValid());
  MOZ_ASSERT(sourceType.isExternHierarchy());
  MOZ_ASSERT(destType.isExternHierarchy());

  Label fallthrough;
  Label* successLabel = onSuccess ? label : &fallthrough;
  Label* failLabel = onSuccess ? &fallthrough : label;
  Label* nullLabel = destType.isNullable() ? successLabel : failLabel;

  auto finishSuccess = [&]() {
    if (successLabel != &fallthrough) {
      jump(successLabel);
    }
    bind(&fallthrough);
  };
  auto finishFail = [&]() {
    if (failLabel != &fallthrough) {
      jump(failLabel);
    }
    bind(&fallthrough);
  };

  // Check for null.
  if (sourceType.isNullable()) {
    branchTestPtr(Assembler::Zero, ref, ref, nullLabel);
  }

  // The only value that can inhabit 'noextern' is null. So, early out if we got
  // not-null.
  if (destType.isNoExtern()) {
    finishFail();
    return;
  }

  // There are no other possible types except externref, so succeed!
  finishSuccess();
}

void MacroAssembler::branchWasmRefIsSubtypeExn(Register ref,
                                               wasm::RefType sourceType,
                                               wasm::RefType destType,
                                               Label* label, bool onSuccess) {
  MOZ_ASSERT(sourceType.isValid());
  MOZ_ASSERT(destType.isValid());
  MOZ_ASSERT(sourceType.isExnHierarchy());
  MOZ_ASSERT(destType.isExnHierarchy());

  Label fallthrough;
  Label* successLabel = onSuccess ? label : &fallthrough;
  Label* failLabel = onSuccess ? &fallthrough : label;
  Label* nullLabel = destType.isNullable() ? successLabel : failLabel;

  auto finishSuccess = [&]() {
    if (successLabel != &fallthrough) {
      jump(successLabel);
    }
    bind(&fallthrough);
  };
  auto finishFail = [&]() {
    if (failLabel != &fallthrough) {
      jump(failLabel);
    }
    bind(&fallthrough);
  };

  // Check for null.
  if (sourceType.isNullable()) {
    branchTestPtr(Assembler::Zero, ref, ref, nullLabel);
  }

  // The only value that can inhabit 'noexn' is null. So, early out if we got
  // not-null.
  if (destType.isNoExn()) {
    finishFail();
    return;
  }

  // There are no other possible types except exnref, so succeed!
  finishSuccess();
}

void MacroAssembler::branchWasmSTVIsSubtype(Register subSTV, Register superSTV,
                                            Register scratch,
                                            const wasm::TypeDef* destType,
                                            Label* label, bool onSuccess) {
  if (destType->isFinal()) {
    // A final type cannot have subtypes, and therefore a simple equality check
    // is sufficient.
    MOZ_ASSERT(scratch == Register::Invalid());
    branchPtr(onSuccess ? Assembler::Equal : Assembler::NotEqual, subSTV,
              superSTV, label);
    return;
  }

  MOZ_ASSERT((destType->subTypingDepth() >= wasm::MinSuperTypeVectorLength) ==
             (scratch != Register::Invalid()));

  Label fallthrough;
  Label* successLabel = onSuccess ? label : &fallthrough;
  Label* failLabel = onSuccess ? &fallthrough : label;

  // Emit a fast success path for if subSTV == superSTV.
  // If they are unequal, they still may be subtypes.
  branchPtr(Assembler::Equal, subSTV, superSTV, successLabel);

  // Emit a bounds check if the super type depth may be out-of-bounds.
  if (destType->subTypingDepth() >= wasm::MinSuperTypeVectorLength) {
    load32(Address(subSTV, wasm::SuperTypeVector::offsetOfLength()), scratch);
    branch32(Assembler::BelowOrEqual, scratch,
             Imm32(destType->subTypingDepth()), failLabel);
  }

  // Load the `superTypeDepth` entry from subSTV. This will be `superSTV` if
  // `subSTV` is indeed a subtype.
  loadPtr(Address(subSTV, wasm::SuperTypeVector::offsetOfSTVInVector(
                              destType->subTypingDepth())),
          subSTV);

  // We succeed iff the entries are equal.
  branchPtr(onSuccess ? Assembler::Equal : Assembler::NotEqual, subSTV,
            superSTV, label);

  bind(&fallthrough);
}

void MacroAssembler::branchWasmSTVIsSubtypeDynamicDepth(
    Register subSTV, Register superSTV, Register superDepth, Register scratch,
    Label* label, bool onSuccess) {
  Label fallthrough;
  Label* failed = onSuccess ? &fallthrough : label;

  // Bounds check of the super type vector
  load32(Address(subSTV, wasm::SuperTypeVector::offsetOfLength()), scratch);
  branch32(Assembler::BelowOrEqual, scratch, superDepth, failed);

  // Load `subSTV[superTypeDepth]`. This will be `superSTV` if `subSTV` is
  // indeed a subtype.
  loadPtr(BaseIndex(subSTV, superDepth, ScalePointer,
                    offsetof(wasm::SuperTypeVector, types_)),
          subSTV);

  // We succeed iff the entries are equal
  branchPtr(onSuccess ? Assembler::Equal : Assembler::NotEqual, subSTV,
            superSTV, label);

  bind(&fallthrough);
}

void MacroAssembler::extractWasmAnyRefTag(Register src, Register dest) {
  andPtr(Imm32(int32_t(wasm::AnyRef::TagMask)), src, dest);
}

void MacroAssembler::untagWasmAnyRef(Register src, Register dest,
                                     wasm::AnyRefTag tag) {
  MOZ_ASSERT(tag != wasm::AnyRefTag::ObjectOrNull, "No untagging needed");
  computeEffectiveAddress(Address(src, -int32_t(tag)), dest);
}

void MacroAssembler::branchWasmAnyRefIsNull(bool isNull, Register src,
                                            Label* label) {
  branchTestPtr(isNull ? Assembler::Zero : Assembler::NonZero, src, src, label);
}

void MacroAssembler::branchWasmAnyRefIsI31(bool isI31, Register src,
                                           Label* label) {
  branchTestPtr(isI31 ? Assembler::NonZero : Assembler::Zero, src,
                Imm32(int32_t(wasm::AnyRefTag::I31)), label);
}

void MacroAssembler::branchWasmAnyRefIsObjectOrNull(bool isObject, Register src,
                                                    Label* label) {
  branchTestPtr(isObject ? Assembler::Zero : Assembler::NonZero, src,
                Imm32(int32_t(wasm::AnyRef::TagMask)), label);
}

void MacroAssembler::branchWasmAnyRefIsJSString(bool isJSString, Register src,
                                                Register temp, Label* label) {
  extractWasmAnyRefTag(src, temp);
  branch32(isJSString ? Assembler::Equal : Assembler::NotEqual, temp,
           Imm32(int32_t(wasm::AnyRefTag::String)), label);
}

void MacroAssembler::branchWasmAnyRefIsGCThing(bool isGCThing, Register src,
                                               Label* label) {
  Label fallthrough;
  Label* isGCThingLabel = isGCThing ? label : &fallthrough;
  Label* isNotGCThingLabel = isGCThing ? &fallthrough : label;

  // A null value or i31 value are not GC things.
  branchWasmAnyRefIsNull(true, src, isNotGCThingLabel);
  branchWasmAnyRefIsI31(true, src, isNotGCThingLabel);
  jump(isGCThingLabel);
  bind(&fallthrough);
}

void MacroAssembler::branchWasmAnyRefIsNurseryCell(bool isNurseryCell,
                                                   Register src, Register temp,
                                                   Label* label) {
  Label done;
  branchWasmAnyRefIsGCThing(false, src, isNurseryCell ? &done : label);

  getWasmAnyRefGCThingChunk(src, temp);
  branchPtr(isNurseryCell ? Assembler::NotEqual : Assembler::Equal,
            Address(temp, gc::ChunkStoreBufferOffset), ImmWord(0), label);
  bind(&done);
}

void MacroAssembler::truncate32ToWasmI31Ref(Register src, Register dest) {
  // This will either zero-extend or sign-extend the high 32-bits on 64-bit
  // platforms (see comments on invariants in MacroAssembler.h). Either case
  // is fine, as we won't use this bits.
  //
  // Move the payload of the integer over by 1 to make room for the tag. This
  // will perform the truncation required by the spec.
  lshift32(Imm32(1), src, dest);
  // Add the i31 tag to the integer.
  orPtr(Imm32(int32_t(wasm::AnyRefTag::I31)), dest);
#ifdef JS_64BIT
  debugAssertCanonicalInt32(dest);
#endif
}

void MacroAssembler::convertWasmI31RefTo32Signed(Register src, Register dest) {
#ifdef JS_64BIT
  debugAssertCanonicalInt32(src);
#endif
  // This will either zero-extend or sign-extend the high 32-bits on 64-bit
  // platforms (see comments on invariants in MacroAssembler.h). Either case
  // is fine, as we won't use this bits.
  //
  // Shift the payload back (clobbering the tag). This will sign-extend, giving
  // us the unsigned behavior we want.
  rshift32Arithmetic(Imm32(1), src, dest);
}

void MacroAssembler::convertWasmI31RefTo32Unsigned(Register src,
                                                   Register dest) {
#ifdef JS_64BIT
  debugAssertCanonicalInt32(src);
#endif
  // This will either zero-extend or sign-extend the high 32-bits on 64-bit
  // platforms (see comments on invariants in MacroAssembler.h). Either case
  // is fine, as we won't use this bits.
  //
  // Shift the payload back (clobbering the tag). This will zero-extend, giving
  // us the unsigned behavior we want.
  rshift32(Imm32(1), src, dest);
}

void MacroAssembler::branchValueConvertsToWasmAnyRefInline(
    ValueOperand src, Register scratchInt, FloatRegister scratchFloat,
    Label* label) {
  // We can convert objects, strings, 31-bit integers and null without boxing.
  Label checkInt32;
  Label checkDouble;
  Label fallthrough;
  {
    ScratchTagScope tag(*this, src);
    splitTagForTest(src, tag);
    branchTestObject(Assembler::Equal, tag, label);
    branchTestString(Assembler::Equal, tag, label);
    branchTestNull(Assembler::Equal, tag, label);
    branchTestInt32(Assembler::Equal, tag, &checkInt32);
    branchTestDouble(Assembler::Equal, tag, &checkDouble);
  }
  jump(&fallthrough);

  bind(&checkInt32);
  {
    unboxInt32(src, scratchInt);
    branch32(Assembler::GreaterThan, scratchInt,
             Imm32(wasm::AnyRef::MaxI31Value), &fallthrough);
    branch32(Assembler::LessThan, scratchInt, Imm32(wasm::AnyRef::MinI31Value),
             &fallthrough);
    jump(label);
  }

  bind(&checkDouble);
  {
    unboxDouble(src, scratchFloat);
    convertDoubleToInt32(scratchFloat, scratchInt, &fallthrough);
    branch32(Assembler::GreaterThan, scratchInt,
             Imm32(wasm::AnyRef::MaxI31Value), &fallthrough);
    branch32(Assembler::LessThan, scratchInt, Imm32(wasm::AnyRef::MinI31Value),
             &fallthrough);
    jump(label);
  }

  bind(&fallthrough);
}

void MacroAssembler::convertValueToWasmAnyRef(ValueOperand src, Register dest,
                                              FloatRegister scratchFloat,
                                              Label* oolConvert) {
  Label doubleValue, int32Value, nullValue, stringValue, objectValue, done;
  {
    ScratchTagScope tag(*this, src);
    splitTagForTest(src, tag);
    branchTestObject(Assembler::Equal, tag, &objectValue);
    branchTestString(Assembler::Equal, tag, &stringValue);
    branchTestNull(Assembler::Equal, tag, &nullValue);
    branchTestInt32(Assembler::Equal, tag, &int32Value);
    branchTestDouble(Assembler::Equal, tag, &doubleValue);
    jump(oolConvert);
  }

  bind(&doubleValue);
  {
    unboxDouble(src, scratchFloat);
    convertDoubleToInt32(scratchFloat, dest, oolConvert);
    branch32(Assembler::GreaterThan, dest, Imm32(wasm::AnyRef::MaxI31Value),
             oolConvert);
    branch32(Assembler::LessThan, dest, Imm32(wasm::AnyRef::MinI31Value),
             oolConvert);
    truncate32ToWasmI31Ref(dest, dest);
    jump(&done);
  }

  bind(&int32Value);
  {
    unboxInt32(src, dest);
    branch32(Assembler::GreaterThan, dest, Imm32(wasm::AnyRef::MaxI31Value),
             oolConvert);
    branch32(Assembler::LessThan, dest, Imm32(wasm::AnyRef::MinI31Value),
             oolConvert);
    truncate32ToWasmI31Ref(dest, dest);
    jump(&done);
  }

  bind(&nullValue);
  {
    static_assert(wasm::AnyRef::NullRefValue == 0);
    xorPtr(dest, dest);
    jump(&done);
  }

  bind(&stringValue);
  {
    unboxString(src, dest);
    orPtr(Imm32((int32_t)wasm::AnyRefTag::String), dest);
    jump(&done);
  }

  bind(&objectValue);
  {
    unboxObject(src, dest);
  }

  bind(&done);
}

void MacroAssembler::convertObjectToWasmAnyRef(Register src, Register dest) {
  // JS objects are represented without any tagging.
  movePtr(src, dest);
}

void MacroAssembler::convertStringToWasmAnyRef(Register src, Register dest) {
  // JS strings require a tag.
  orPtr(Imm32(int32_t(wasm::AnyRefTag::String)), src, dest);
}

void MacroAssembler::branchObjectIsWasmGcObject(bool isGcObject, Register src,
                                                Register scratch,
                                                Label* label) {
  constexpr uint32_t ShiftedMask = (Shape::kindMask() << Shape::kindShift());
  constexpr uint32_t ShiftedKind =
      (uint32_t(Shape::Kind::WasmGC) << Shape::kindShift());
  MOZ_ASSERT(src != scratch);

  loadPtr(Address(src, JSObject::offsetOfShape()), scratch);
  load32(Address(scratch, Shape::offsetOfImmutableFlags()), scratch);
  and32(Imm32(ShiftedMask), scratch);
  branch32(isGcObject ? Assembler::Equal : Assembler::NotEqual, scratch,
           Imm32(ShiftedKind), label);
}

void MacroAssembler::wasmNewStructObject(Register instance, Register result,
                                         Register allocSite, Register temp,
                                         size_t offsetOfTypeDefData,
                                         Label* fail, gc::AllocKind allocKind,
                                         bool zeroFields) {
  MOZ_ASSERT(instance != result);

  // Don't execute the inline path if GC probes are built in.
#ifdef JS_GC_PROBES
  jump(fail);
#endif

#ifdef JS_GC_ZEAL
  // Don't execute the inline path if gc zeal or tracing are active.
  loadPtr(Address(instance, wasm::Instance::offsetOfAddressOfGCZealModeBits()),
          temp);
  loadPtr(Address(temp, 0), temp);
  branch32(Assembler::NotEqual, temp, Imm32(0), fail);
#endif

  // If the alloc site is long lived, immediately fall back to the OOL path,
  // which will handle that.
  branchTestPtr(Assembler::NonZero,
                Address(allocSite, gc::AllocSite::offsetOfScriptAndState()),
                Imm32(gc::AllocSite::LONG_LIVED_BIT), fail);

  size_t sizeBytes = gc::Arena::thingSize(allocKind);
  wasmBumpPointerAllocate(instance, result, allocSite, temp, fail, sizeBytes);

  loadPtr(Address(instance, offsetOfTypeDefData +
                                wasm::TypeDefInstanceData::offsetOfShape()),
          temp);
  storePtr(temp, Address(result, WasmArrayObject::offsetOfShape()));
  loadPtr(Address(instance,
                  offsetOfTypeDefData +
                      wasm::TypeDefInstanceData::offsetOfSuperTypeVector()),
          temp);
  storePtr(temp, Address(result, WasmArrayObject::offsetOfSuperTypeVector()));
  storePtr(ImmWord(0),
           Address(result, WasmStructObject::offsetOfOutlineData()));

  if (zeroFields) {
    MOZ_ASSERT(sizeBytes % sizeof(void*) == 0);
    for (size_t i = WasmStructObject::offsetOfInlineData(); i < sizeBytes;
         i += sizeof(void*)) {
      storePtr(ImmWord(0), Address(result, i));
    }
  }
}

void MacroAssembler::wasmNewArrayObject(Register instance, Register result,
                                        Register numElements,
                                        Register allocSite, Register temp,
                                        size_t offsetOfTypeDefData, Label* fail,
                                        uint32_t elemSize, bool zeroFields) {
  MOZ_ASSERT(instance != result);

  // Don't execute the inline path if GC probes are built in.
#ifdef JS_GC_PROBES
  jump(fail);
#endif

#ifdef JS_GC_ZEAL
  // Don't execute the inline path if gc zeal or tracing are active.
  loadPtr(Address(instance, wasm::Instance::offsetOfAddressOfGCZealModeBits()),
          temp);
  loadPtr(Address(temp, 0), temp);
  branch32(Assembler::NotEqual, temp, Imm32(0), fail);
#endif

  // If the alloc site is long lived, immediately fall back to the OOL path,
  // which will handle that.
  branchTestPtr(Assembler::NonZero,
                Address(allocSite, gc::AllocSite::offsetOfScriptAndState()),
                Imm32(gc::AllocSite::LONG_LIVED_BIT), fail);

  // Ensure that the numElements is small enough to fit in inline storage.
  branch32(Assembler::Above, numElements,
           Imm32(WasmArrayObject::maxInlineElementsForElemSize(elemSize)),
           fail);

  // Push numElements for later; numElements will be used as a temp in the
  // meantime. Make sure that all exit paths pop the value again!
  Label popAndFail;
#ifdef JS_CODEGEN_ARM64
  // On arm64, we must maintain 16-alignment of both the actual and pseudo stack
  // pointers.
  push(numElements, xzr);
  syncStackPtr();
#else
  push(numElements);
#endif

  // Compute the size of the allocation in bytes. The final size must correspond
  // to an AllocKind. See WasmArrayObject::calcStorageBytes and
  // WasmArrayObject::allocKindForIL.

  // Compute the size of all array element data.
  mul32(Imm32(elemSize), numElements);
  // Add the data header.
  add32(Imm32(sizeof(WasmArrayObject::DataHeader)), numElements);
  // Round up to gc::CellAlignBytes to play nice with the GC and to simplify the
  // zeroing logic below.
  add32(Imm32(gc::CellAlignBytes - 1), numElements);
  and32(Imm32(~int32_t(gc::CellAlignBytes - 1)), numElements);
  // Add the size of the WasmArrayObject to get the full allocation size.
  static_assert(WasmArrayObject_MaxInlineBytes + sizeof(WasmArrayObject) <
                INT32_MAX);
  add32(Imm32(sizeof(WasmArrayObject)), numElements);
  // Per gc::slotsToAllocKindBytes, subtract sizeof(NativeObject),
  // divide by sizeof(js::Value), then look up the final AllocKind-based
  // allocation size from a table.
  movePtr(wasm::SymbolicAddress::SlotsToAllocKindBytesTable, temp);
  move32ZeroExtendToPtr(numElements, numElements);
  subPtr(Imm32(sizeof(NativeObject)), numElements);
  static_assert(sizeof(js::Value) == 8);
  rshiftPtr(Imm32(3), numElements);
  static_assert(sizeof(gc::slotsToAllocKindBytes[0]) == 4);
  load32(BaseIndex(temp, numElements, Scale::TimesFour), numElements);

  wasmBumpPointerAllocateDynamic(instance, result, allocSite,
                                 /*size=*/numElements, temp, &popAndFail);

  // Initialize the shape and STV
  loadPtr(Address(instance, offsetOfTypeDefData +
                                wasm::TypeDefInstanceData::offsetOfShape()),
          temp);
  storePtr(temp, Address(result, WasmArrayObject::offsetOfShape()));
  loadPtr(Address(instance,
                  offsetOfTypeDefData +
                      wasm::TypeDefInstanceData::offsetOfSuperTypeVector()),
          temp);
  storePtr(temp, Address(result, WasmArrayObject::offsetOfSuperTypeVector()));

  // Store inline data header and data pointer
  storePtr(ImmWord(WasmArrayObject::DataIsIL),
           Address(result, WasmArrayObject::offsetOfInlineStorage()));
  computeEffectiveAddress(
      Address(result, WasmArrayObject::offsetOfInlineArrayData()), temp);
  // temp now points at the base of the array data; this will be used later
  storePtr(temp, Address(result, WasmArrayObject::offsetOfData()));
  // numElements will be saved to the array object later; for now we want to
  // continue using numElements as a temp.

  // Zero the array elements. This loop depends on the size of the array data
  // being a multiple of the machine word size. This is currently always the
  // case since WasmArrayObject::calcStorageBytes rounds up to
  // gc::CellAlignBytes.
  static_assert(gc::CellAlignBytes % sizeof(void*) == 0);
  Label zeroed;
  if (zeroFields) {
    // numElements currently stores the total size of the allocation. temp
    // points at the base of the inline array data. We will zero the memory by
    // advancing numElements to the end of the allocation, then counting down
    // toward temp, zeroing one word at a time. The following aliases make this
    // clearer.
    Register current = numElements;
    Register inlineArrayData = temp;

    // We first need to update current to actually point at the end of the
    // allocation. We can compute this from the data pointer, since the data
    // pointer points at a known offset within the array.
    //
    // It is easier to understand the code below as first subtracting the offset
    // (to get back to the start of the allocation), then adding the total size
    // of the allocation (using Scale::TimesOne).
    computeEffectiveAddress(
        BaseIndex(inlineArrayData, current, Scale::TimesOne,
                  -int32_t(WasmArrayObject::offsetOfInlineArrayData())),
        current);

    // Exit immediately if the array has zero elements.
    branchPtr(Assembler::Equal, current, inlineArrayData, &zeroed);

    // Loop, counting down until current == inlineArrayData.
    Label loop;
    bind(&loop);
    subPtr(Imm32(sizeof(void*)), current);
    storePtr(ImmWord(0), Address(current, 0));
    branchPtr(Assembler::NotEqual, current, inlineArrayData, &loop);
  }
  bind(&zeroed);

  // Finally, store the actual numElements in the array object.
#ifdef JS_CODEGEN_ARM64
  pop(xzr, numElements);
  syncStackPtr();
#else
  pop(numElements);
#endif
  store32(numElements, Address(result, WasmArrayObject::offsetOfNumElements()));

  Label done;
  jump(&done);

  bind(&popAndFail);
#ifdef JS_CODEGEN_ARM64
  pop(xzr, numElements);
  syncStackPtr();
#else
  pop(numElements);
#endif
  jump(fail);

  bind(&done);
}

void MacroAssembler::wasmNewArrayObjectFixed(
    Register instance, Register result, Register allocSite, Register temp1,
    Register temp2, size_t offsetOfTypeDefData, Label* fail,
    uint32_t numElements, uint32_t storageBytes, bool zeroFields) {
  MOZ_ASSERT(storageBytes <= WasmArrayObject_MaxInlineBytes);
  MOZ_ASSERT(instance != result);

  // Don't execute the inline path if GC probes are built in.
#ifdef JS_GC_PROBES
  jump(fail);
#endif

#ifdef JS_GC_ZEAL
  // Don't execute the inline path if gc zeal or tracing are active.
  loadPtr(Address(instance, wasm::Instance::offsetOfAddressOfGCZealModeBits()),
          temp1);
  loadPtr(Address(temp1, 0), temp1);
  branch32(Assembler::NotEqual, temp1, Imm32(0), fail);
#endif

  // If the alloc site is long lived, immediately fall back to the OOL path,
  // which will handle that.
  branchTestPtr(Assembler::NonZero,
                Address(allocSite, gc::AllocSite::offsetOfScriptAndState()),
                Imm32(gc::AllocSite::LONG_LIVED_BIT), fail);

  gc::AllocKind allocKind = WasmArrayObject::allocKindForIL(storageBytes);
  uint32_t totalSize = gc::Arena::thingSize(allocKind);
  wasmBumpPointerAllocate(instance, result, allocSite, temp1, fail, totalSize);

  loadPtr(Address(instance, offsetOfTypeDefData +
                                wasm::TypeDefInstanceData::offsetOfShape()),
          temp1);
  loadPtr(Address(instance,
                  offsetOfTypeDefData +
                      wasm::TypeDefInstanceData::offsetOfSuperTypeVector()),
          temp2);
  storePtr(temp1, Address(result, WasmArrayObject::offsetOfShape()));
  storePtr(temp2, Address(result, WasmArrayObject::offsetOfSuperTypeVector()));
  store32(Imm32(numElements),
          Address(result, WasmArrayObject::offsetOfNumElements()));

  // Store inline data header and data pointer
  storePtr(ImmWord(WasmArrayObject::DataIsIL),
           Address(result, WasmArrayObject::offsetOfInlineStorage()));
  computeEffectiveAddress(
      Address(result, WasmArrayObject::offsetOfInlineArrayData()), temp2);
  // temp2 now points at the base of the array data; this will be used later
  storePtr(temp2, Address(result, WasmArrayObject::offsetOfData()));

  if (zeroFields) {
    MOZ_ASSERT(storageBytes % sizeof(void*) == 0);

    // Advance temp1 to the end of the allocation
    // (note that temp2 is already past the data header)
    Label done;
    computeEffectiveAddress(
        Address(temp2, -sizeof(WasmArrayObject::DataHeader) + storageBytes),
        temp1);
    branchPtr(Assembler::Equal, temp1, temp2, &done);

    // Count temp2 down toward temp1, zeroing one word at a time
    Label loop;
    bind(&loop);
    subPtr(Imm32(sizeof(void*)), temp1);
    storePtr(ImmWord(0), Address(temp1, 0));
    branchPtr(Assembler::NotEqual, temp1, temp2, &loop);

    bind(&done);
  }
}

void MacroAssembler::wasmBumpPointerAllocate(Register instance, Register result,
                                             Register allocSite, Register temp1,
                                             Label* fail, uint32_t size) {
  MOZ_ASSERT(size >= gc::MinCellSize);

  uint32_t totalSize = size + Nursery::nurseryCellHeaderSize();
  MOZ_ASSERT(totalSize < INT32_MAX, "Nursery allocation too large");
  MOZ_ASSERT(totalSize % gc::CellAlignBytes == 0);

  int32_t endOffset = Nursery::offsetOfCurrentEndFromPosition();

  // Bail to OOL code if the alloc site needs to be pushed onto the active
  // list.
  load32(Address(allocSite, gc::AllocSite::offsetOfNurseryAllocCount()), temp1);
  branch32(Assembler::Equal, temp1,
           Imm32(js::gc::NormalSiteAttentionThreshold - 1), fail);

  // Bump allocate in the nursery, bailing if there is not enough room.
  loadPtr(Address(instance, wasm::Instance::offsetOfAddressOfNurseryPosition()),
          temp1);
  loadPtr(Address(temp1, 0), result);
  addPtr(Imm32(totalSize), result);
  branchPtr(Assembler::Below, Address(temp1, endOffset), result, fail);
  storePtr(result, Address(temp1, 0));
  subPtr(Imm32(size), result);

  // Increment the alloc count in the allocation site and store pointer in the
  // nursery cell header. See NurseryCellHeader::MakeValue.
  add32(Imm32(1),
        Address(allocSite, gc::AllocSite::offsetOfNurseryAllocCount()));
  // Because JS::TraceKind::Object is zero, there is no need to explicitly set
  // it in the nursery cell header.
  static_assert(int(JS::TraceKind::Object) == 0);
  storePtr(allocSite, Address(result, -js::Nursery::nurseryCellHeaderSize()));
}

void MacroAssembler::wasmBumpPointerAllocateDynamic(
    Register instance, Register result, Register allocSite, Register size,
    Register temp1, Label* fail) {
#ifdef DEBUG
  // Replaces MOZ_ASSERT(size >= gc::MinCellSize);
  Label ok1;
  branch32(Assembler::AboveOrEqual, size, Imm32(gc::MinCellSize), &ok1);
  breakpoint();
  bind(&ok1);

  Label ok2;
  branch32(Assembler::BelowOrEqual, size, Imm32(JSObject::MAX_BYTE_SIZE), &ok2);
  breakpoint();
  bind(&ok2);
#endif

  int32_t endOffset = Nursery::offsetOfCurrentEndFromPosition();

  // Bail to OOL code if the alloc site needs to be initialized.
  load32(Address(allocSite, gc::AllocSite::offsetOfNurseryAllocCount()), temp1);
  branch32(Assembler::Equal, temp1,
           Imm32(js::gc::NormalSiteAttentionThreshold - 1), fail);

  // Bump allocate in the nursery, bailing if there is not enough room.
  loadPtr(Address(instance, wasm::Instance::offsetOfAddressOfNurseryPosition()),
          temp1);
  loadPtr(Address(temp1, 0), result);
  computeEffectiveAddress(BaseIndex(result, size, Scale::TimesOne,
                                    Nursery::nurseryCellHeaderSize()),
                          result);
  branchPtr(Assembler::Below, Address(temp1, endOffset), result, fail);
  storePtr(result, Address(temp1, 0));
  subPtr(size, result);

  // Increment the alloc count in the allocation site and store pointer in the
  // nursery cell header. See NurseryCellHeader::MakeValue.

  add32(Imm32(1),
        Address(allocSite, gc::AllocSite::offsetOfNurseryAllocCount()));
  // Because JS::TraceKind::Object is zero, there is no need to explicitly set
  // it in the nursery cell header.
  static_assert(int(JS::TraceKind::Object) == 0);
  storePtr(allocSite, Address(result, -js::Nursery::nurseryCellHeaderSize()));
}

// Unboxing is branchy and contorted because of Spectre mitigations - we don't
// have enough scratch registers.  Were it not for the spectre mitigations in
// branchTestObjClass, the branch nest below would be restructured significantly
// by inverting branches and using fewer registers.

// Unbox an anyref in src (clobbering src in the process) and then re-box it as
// a Value in *dst.  See the definition of AnyRef for a discussion of pointer
// representation.
void MacroAssembler::convertWasmAnyRefToValue(Register instance, Register src,
                                              ValueOperand dst,
                                              Register scratch) {
  MOZ_ASSERT(src != scratch);
#if JS_BITS_PER_WORD == 32
  MOZ_ASSERT(dst.typeReg() != scratch);
  MOZ_ASSERT(dst.payloadReg() != scratch);
#else
  MOZ_ASSERT(dst.valueReg() != scratch);
#endif

  Label isI31, isObjectOrNull, isObject, isWasmValueBox, done;

  // Check for if this is an i31 value first
  branchTestPtr(Assembler::NonZero, src, Imm32(int32_t(wasm::AnyRefTag::I31)),
                &isI31);
  // Then check for the object or null tag
  branchTestPtr(Assembler::Zero, src, Imm32(wasm::AnyRef::TagMask),
                &isObjectOrNull);

  // If we're not i31, object, or null, we must be a string
  untagWasmAnyRef(src, src, wasm::AnyRefTag::String);
  moveValue(TypedOrValueRegister(MIRType::String, AnyRegister(src)), dst);
  jump(&done);

  // This is an i31 value, convert to an int32 JS value
  bind(&isI31);
  convertWasmI31RefTo32Signed(src, src);
  moveValue(TypedOrValueRegister(MIRType::Int32, AnyRegister(src)), dst);
  jump(&done);

  // Check for the null value
  bind(&isObjectOrNull);
  branchTestPtr(Assembler::NonZero, src, src, &isObject);
  moveValue(NullValue(), dst);
  jump(&done);

  // Otherwise we must be a non-null object. We next to check if it's storing a
  // boxed value
  bind(&isObject);
  // The type test will clear src if the test fails, so store early.
  moveValue(TypedOrValueRegister(MIRType::Object, AnyRegister(src)), dst);
  // Spectre mitigations: see comment above about efficiency.
  branchTestObjClass(Assembler::Equal, src,
                     Address(instance, wasm::Instance::offsetOfValueBoxClass()),
                     scratch, src, &isWasmValueBox);
  jump(&done);

  // This is a boxed JS value, unbox it.
  bind(&isWasmValueBox);
  loadValue(Address(src, wasm::AnyRef::valueBoxOffsetOfValue()), dst);

  bind(&done);
}

void MacroAssembler::convertWasmAnyRefToValue(Register instance, Register src,
                                              const Address& dst,
                                              Register scratch) {
  MOZ_ASSERT(src != scratch);

  Label isI31, isObjectOrNull, isObject, isWasmValueBox, done;

  // Check for if this is an i31 value first
  branchTestPtr(Assembler::NonZero, src, Imm32(int32_t(wasm::AnyRefTag::I31)),
                &isI31);
  // Then check for the object or null tag
  branchTestPtr(Assembler::Zero, src, Imm32(wasm::AnyRef::TagMask),
                &isObjectOrNull);

  // If we're not i31, object, or null, we must be a string
  rshiftPtr(Imm32(wasm::AnyRef::TagShift), src);
  lshiftPtr(Imm32(wasm::AnyRef::TagShift), src);
  storeValue(JSVAL_TYPE_STRING, src, dst);
  jump(&done);

  // This is an i31 value, convert to an int32 JS value
  bind(&isI31);
  convertWasmI31RefTo32Signed(src, src);
  storeValue(JSVAL_TYPE_INT32, src, dst);
  jump(&done);

  // Check for the null value
  bind(&isObjectOrNull);
  branchTestPtr(Assembler::NonZero, src, src, &isObject);
  storeValue(NullValue(), dst);
  jump(&done);

  // Otherwise we must be a non-null object. We next to check if it's storing a
  // boxed value
  bind(&isObject);
  // The type test will clear src if the test fails, so store early.
  storeValue(JSVAL_TYPE_OBJECT, src, dst);
  // Spectre mitigations: see comment above about efficiency.
  branchTestObjClass(Assembler::Equal, src,
                     Address(instance, wasm::Instance::offsetOfValueBoxClass()),
                     scratch, src, &isWasmValueBox);
  jump(&done);

  // This is a boxed JS value, unbox it.
  bind(&isWasmValueBox);
  copy64(Address(src, wasm::AnyRef::valueBoxOffsetOfValue()), dst, scratch);

  bind(&done);
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
  } else if (type == MIRType::WasmAnyRef) {
    unboxWasmAnyRefGCThingForGCBarrier(Address(PreBarrierReg, 0), temp1);
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
  andPtr(Imm32(int32_t(~gc::ChunkMask)), temp1, temp2);

  // If the GC thing is in the nursery, we don't need to barrier it.
  if (type == MIRType::Value || type == MIRType::Object ||
      type == MIRType::String || type == MIRType::WasmAnyRef) {
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
  const size_t firstArenaAdjustment =
      gc::ChunkMarkBitmap::FirstThingAdjustmentBits / CHAR_BIT;
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
    const wasm::MaybeTrapSiteDesc& trapSiteDesc) {
#ifdef WASM_HAS_HEAPREG
  static_assert(wasm::Instance::offsetOfMemory0Base() < 4096,
                "We count only on the low page being inaccessible");
  FaultingCodeOffset fco = loadPtr(
      Address(InstanceReg, wasm::Instance::offsetOfMemory0Base()), HeapReg);
  if (trapSiteDesc) {
    append(wasm::Trap::IndirectCallToNull, wasm::TrapMachineInsnForLoadWord(),
           fco.get(), *trapSiteDesc);
  }
#else
  MOZ_ASSERT(!trapSiteDesc);
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

void MacroAssembler::memoryBarrierBefore(Synchronization sync) {
  memoryBarrier(sync.barrierBefore);
}

void MacroAssembler::memoryBarrierAfter(Synchronization sync) {
  memoryBarrier(sync.barrierAfter);
}

void MacroAssembler::convertDoubleToFloat16(FloatRegister src,
                                            FloatRegister dest, Register temp,
                                            LiveRegisterSet volatileLiveRegs) {
  if (MacroAssembler::SupportsFloat64To16()) {
    convertDoubleToFloat16(src, dest);

    // Float16 is currently passed as Float32, so expand again to Float32.
    convertFloat16ToFloat32(dest, dest);
    return;
  }

  LiveRegisterSet save = volatileLiveRegs;
  save.takeUnchecked(dest);
  save.takeUnchecked(dest.asDouble());
  save.takeUnchecked(temp);

  PushRegsInMask(save);

  using Fn = float (*)(double);
  setupUnalignedABICall(temp);
  passABIArg(src, ABIType::Float64);
  callWithABI<Fn, jit::RoundFloat16ToFloat32>(ABIType::Float32);
  storeCallFloatResult(dest);

  PopRegsInMask(save);
}

void MacroAssembler::convertDoubleToFloat16(FloatRegister src,
                                            FloatRegister dest, Register temp1,
                                            Register temp2) {
  MOZ_ASSERT(MacroAssembler::SupportsFloat64To16() ||
             MacroAssembler::SupportsFloat32To16());
  MOZ_ASSERT(src != dest);

  if (MacroAssembler::SupportsFloat64To16()) {
    convertDoubleToFloat16(src, dest);

    // Float16 is currently passed as Float32, so expand again to Float32.
    convertFloat16ToFloat32(dest, dest);
    return;
  }

  using Float32 = mozilla::FloatingPoint<float>;

#ifdef DEBUG
  static auto float32Bits = [](float16 f16) {
    // Cast to float and reinterpret to bit representation.
    return mozilla::BitwiseCast<Float32::Bits>(static_cast<float>(f16));
  };

  static auto nextExponent = [](float16 f16, int32_t direction) {
    constexpr auto kSignificandWidth = Float32::kSignificandWidth;

    // Shift out mantissa and then adjust exponent.
    auto bits = float32Bits(f16);
    return ((bits >> kSignificandWidth) + direction) << kSignificandWidth;
  };
#endif

  // Float32 larger or equals to |overflow| are infinity (or NaN) in Float16.
  constexpr uint32_t overflow = 0x4780'0000;
  MOZ_ASSERT(overflow == nextExponent(std::numeric_limits<float16>::max(), 1));

  // Float32 smaller than |underflow| are zero in Float16.
  constexpr uint32_t underflow = 0x3300'0000;
  MOZ_ASSERT(underflow ==
             nextExponent(std::numeric_limits<float16>::denorm_min(), -1));

  // Float32 larger or equals to |normal| are normal numbers in Float16.
  constexpr uint32_t normal = 0x3880'0000;
  MOZ_ASSERT(normal == float32Bits(std::numeric_limits<float16>::min()));

  // There are five possible cases to consider:
  // 1. Non-finite (infinity and NaN)
  // 2. Overflow to infinity
  // 3. Normal numbers
  // 4. Denormal numbers
  // 5. Underflow to zero
  //
  // Cases 1-2 and 4-5 don't need separate code paths, so we only need to be
  // concerned about incorrect double rounding for cases 3-4.
  //
  // Double rounding:
  //
  // Conversion from float64 -> float32 -> float16 can introduce double rounding
  // errors when compared to a direct conversion float64 -> float16.
  //
  // Number of bits in the exponent and mantissa. These are relevant below.
  //
  //       exponent  mantissa
  // -----------------------
  // f16 |  5        10
  // f32 |  8        23
  // f64 | 11        52
  //
  // Examples:
  //
  // Input (f64): 0.0000610649585723877
  // Bits (f64):  3f10'0200'0000'0000
  // Bits (f32):  3880'1000
  // Bits (f16):  0400
  //
  // Ignore the three left-most nibbles of the f64 bits (those are the sign and
  // exponent). Shift the f64 mantissa right by (52 - 23) = 29 bits. The bits
  // of the f32 mantissa are therefore 00'1000. Converting from f32 to f16 will
  // right shift the mantissa by (23 - 10) = 13 bits. `001000 >> 13` is all
  // zero. Directly converting from f64 to f16 right shifts the f64 mantissa by
  // (52 - 10) = 42 bits. `0'0200'0000'0000 >> 42` is also all zero. So in this
  // case no double rounding did take place.
  //
  // Input (f64): 0.00006106495857238771
  // Bits (f64):  3f10'0200'0000'0001
  // Bits (f32):  3880'1000
  // Bits (f16):  0401
  //
  // The f64 to f32 conversion returns the same result 3880'1000 as in the first
  // example, but the direct f64 to f16 conversion must return 0401. Let's look
  // at the binary representation of the mantissa.
  //
  // Mantissa of 3f10'0200'0000'0001 in binary representation:
  //
  //                                          Low 32-bits
  //                           __________________|__________________
  //                          /                                     |
  // 0000 0000 0010 0000 0000 0000 0000 0000 0000 0000 0000 0000 0001
  //            |               |
  //            |               GRS
  //            |               001
  //            |
  //            GRS  (G)uard bit
  //            011  (R)ound bit
  //                 (S)ticky bit
  //
  // The guard, round, and sticky bits control when to round: If the round bit
  // is one and at least one of guard or sticky is one, then round up. The
  // sticky bit is the or-ed value of all bits right of the round bit.
  //
  // When rounding to float16, GRS is 011, so we have to round up, whereas when
  // rounding to float32, GRS is 001, so no rounding takes place.
  //
  // Mantissa of 3880'1000 in binary representation:
  //
  // e000 0000 0001 0000 0000 0000
  //             |
  //             GRS
  //             010
  //
  // The round bit is set, but neither the guard nor sticky bit is set, so no
  // rounding takes place for the f32 -> f16 conversion. We can attempt to
  // recover the missing sticky bit from the f64 -> f16 conversion by looking at
  // the low 32-bits of the f64 mantissa. If at least one bit is set in the
  // low 32-bits (and the MSB is zero), then add one to the f32 mantissa.
  // Modified mantissa now looks like:
  //
  // e000 0000 0001 0000 0000 0001
  //             |
  //             GRS
  //             011
  //
  // GRS is now 011, so we round up and get the correctly rounded result 0401.
  //
  // Input (f64): 0.00006112456321716307
  // Bits (f64):  3f10'05ff'ffff'ffff
  // Bits (f32):  3880'3000
  // Bits (f16):  0401
  //
  //                                          Low 32-bits
  //                           __________________|__________________
  //                          /                                     |
  // 0000 0000 0101 1111 1111 1111 1111 1111 1111 1111 1111 1111 1111
  //            |               |
  //            |               GRS
  //            |               111
  //            |
  //            GRS
  //            101
  //
  // When rounding to float16, GRS is 101, so we don't round, whereas when
  // rounding to float32, GRS is 111, so we have to round up.
  //
  // Mantissa of 3880'3000 in binary representation:
  //
  // e000 0000 0011 0000 0000 0000
  //             |
  //             GRS
  //             110
  //
  // The guard and sticky bits are set, so the float32 -> float16 conversion
  // incorrectly rounds up when compared to the direct float64 -> float16
  // conversion. To avoid rounding twice we subtract one if the MSB of the low
  // 32-bits of the f64 mantissa is set. Modified mantissa now looks like:
  //
  // e000 0000 0010 1111 1111 1111
  //             |
  //             GRS
  //             101
  //
  // GRS is now 101, so we don't round and get the correct result 0401.
  //
  // Approach used to avoid double rounding:
  //
  // 1. For normal numbers, inspect the f32 mantissa and if its round bit is set
  // and the sticky bits are all zero, then possibly adjust the f32 mantissa
  // depending on the low 32-bits of the f64 mantissa.
  //
  // 2. For denormal numbers, possibly adjust the f32 mantissa if the round and
  // sticky bits are all zero.

  // First round to float32 and reinterpret to bit representation.
  convertDoubleToFloat32(src, dest);
  moveFloat32ToGPR(dest, temp1);

  // Mask off sign bit to simplify range checks.
  and32(Imm32(~Float32::kSignBit), temp1);

  Label done;

  // No changes necessary for underflow or overflow, including zero and
  // non-finite numbers.
  branch32(Assembler::Below, temp1, Imm32(underflow), &done);
  branch32(Assembler::AboveOrEqual, temp1, Imm32(overflow), &done);

  // Compute 0x1000 for normal and 0x0000 for denormal numbers.
  cmp32Set(Assembler::AboveOrEqual, temp1, Imm32(normal), temp2);
  lshift32(Imm32(12), temp2);

  // Look at the last thirteen bits of the mantissa which will be shifted out
  // when converting from float32 to float16. (The round and sticky bits.)
  //
  // Normal numbers: If the round bit is set and sticky bits are zero, then
  // adjust the float32 mantissa.
  // Denormal numbers: If all bits are zero, then adjust the mantissa.
  and32(Imm32(0x1fff), temp1);
  branch32(Assembler::NotEqual, temp1, temp2, &done);
  {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    // x86 can use SIMD instructions to avoid GPR<>XMM register moves.
    ScratchSimd128Scope scratch(*this);

    int32_t one[] = {1, 0, 0, 0};
    loadConstantSimd128(SimdConstant::CreateX4(one), scratch);

    // 1. If the low 32-bits of |src| are all zero, then set |scratch| to 0.
    // 2. If the MSB of the low 32-bits is set, then set |scratch| to -1.
    // 3. Otherwise set |scratch| to 1.
    vpsignd(Operand(src), scratch, scratch);

    // Add |scratch| to the mantissa.
    vpaddd(Operand(scratch), dest, dest);
#else
    // Determine in which direction to round. When the low 32-bits are all zero,
    // then we don't have to round.
    moveLowDoubleToGPR(src, temp2);
    branch32(Assembler::Equal, temp2, Imm32(0), &done);

    // Load either -1 or +1 into |temp2|.
    rshift32Arithmetic(Imm32(31), temp2);
    or32(Imm32(1), temp2);

    // Add or subtract one to the mantissa.
    moveFloat32ToGPR(dest, temp1);
    add32(temp2, temp1);
    moveGPRToFloat32(temp1, dest);
#endif
  }

  bind(&done);

  // Perform the actual float16 conversion.
  convertFloat32ToFloat16(dest, dest);

  // Float16 is currently passed as Float32, so expand again to Float32.
  convertFloat16ToFloat32(dest, dest);
}

void MacroAssembler::convertFloat32ToFloat16(FloatRegister src,
                                             FloatRegister dest, Register temp,
                                             LiveRegisterSet volatileLiveRegs) {
  if (MacroAssembler::SupportsFloat32To16()) {
    convertFloat32ToFloat16(src, dest);

    // Float16 is currently passed as Float32, so expand again to Float32.
    convertFloat16ToFloat32(dest, dest);
    return;
  }

  LiveRegisterSet save = volatileLiveRegs;
  save.takeUnchecked(dest);
  save.takeUnchecked(dest.asDouble());
  save.takeUnchecked(temp);

  PushRegsInMask(save);

  using Fn = float (*)(float);
  setupUnalignedABICall(temp);
  passABIArg(src, ABIType::Float32);
  callWithABI<Fn, jit::RoundFloat16ToFloat32>(ABIType::Float32);
  storeCallFloatResult(dest);

  PopRegsInMask(save);
}

void MacroAssembler::convertInt32ToFloat16(Register src, FloatRegister dest,
                                           Register temp,
                                           LiveRegisterSet volatileLiveRegs) {
  if (MacroAssembler::SupportsFloat32To16()) {
    convertInt32ToFloat16(src, dest);

    // Float16 is currently passed as Float32, so expand again to Float32.
    convertFloat16ToFloat32(dest, dest);
    return;
  }

  LiveRegisterSet save = volatileLiveRegs;
  save.takeUnchecked(dest);
  save.takeUnchecked(dest.asDouble());
  save.takeUnchecked(temp);

  PushRegsInMask(save);

  using Fn = float (*)(int32_t);
  setupUnalignedABICall(temp);
  passABIArg(src);
  callWithABI<Fn, jit::RoundFloat16ToFloat32>(ABIType::Float32);
  storeCallFloatResult(dest);

  PopRegsInMask(save);
}

template <typename T>
void MacroAssembler::loadFloat16(const T& src, FloatRegister dest,
                                 Register temp1, Register temp2,
                                 LiveRegisterSet volatileLiveRegs) {
  if (MacroAssembler::SupportsFloat32To16()) {
    loadFloat16(src, dest, temp1);

    // Float16 is currently passed as Float32, so expand again to Float32.
    convertFloat16ToFloat32(dest, dest);
    return;
  }

  load16ZeroExtend(src, temp1);

  LiveRegisterSet save = volatileLiveRegs;
  save.takeUnchecked(dest);
  save.takeUnchecked(dest.asDouble());
  save.takeUnchecked(temp1);
  save.takeUnchecked(temp2);

  PushRegsInMask(save);

  using Fn = float (*)(int32_t);
  setupUnalignedABICall(temp2);
  passABIArg(temp1);
  callWithABI<Fn, jit::Float16ToFloat32>(ABIType::Float32);
  storeCallFloatResult(dest);

  PopRegsInMask(save);
}

template void MacroAssembler::loadFloat16(const Address& src,
                                          FloatRegister dest, Register temp1,
                                          Register temp2,
                                          LiveRegisterSet volatileLiveRegs);

template void MacroAssembler::loadFloat16(const BaseIndex& src,
                                          FloatRegister dest, Register temp1,
                                          Register temp2,
                                          LiveRegisterSet volatileLiveRegs);

template <typename T>
void MacroAssembler::storeFloat16(FloatRegister src, const T& dest,
                                  Register temp,
                                  LiveRegisterSet volatileLiveRegs) {
  ScratchFloat32Scope fpscratch(*this);

  if (src.isDouble()) {
    if (MacroAssembler::SupportsFloat64To16()) {
      convertDoubleToFloat16(src, fpscratch);
      storeFloat16(fpscratch, dest, temp);
      return;
    }

    convertDoubleToFloat16(src, fpscratch, temp, volatileLiveRegs);
    src = fpscratch;
  }
  MOZ_ASSERT(src.isSingle());

  if (MacroAssembler::SupportsFloat32To16()) {
    convertFloat32ToFloat16(src, fpscratch);
    storeFloat16(fpscratch, dest, temp);
    return;
  }

  moveFloat16ToGPR(src, temp, volatileLiveRegs);
  store16(temp, dest);
}

template void MacroAssembler::storeFloat16(FloatRegister src,
                                           const Address& dest, Register temp,
                                           LiveRegisterSet volatileLiveRegs);

template void MacroAssembler::storeFloat16(FloatRegister src,
                                           const BaseIndex& dest, Register temp,
                                           LiveRegisterSet volatileLiveRegs);

void MacroAssembler::moveFloat16ToGPR(FloatRegister src, Register dest,
                                      LiveRegisterSet volatileLiveRegs) {
  if (MacroAssembler::SupportsFloat32To16()) {
    ScratchFloat32Scope fpscratch(*this);

    // Float16 is currently passed as Float32, so first narrow to Float16.
    convertFloat32ToFloat16(src, fpscratch);

    moveFloat16ToGPR(fpscratch, dest);
    return;
  }

  LiveRegisterSet save = volatileLiveRegs;
  save.takeUnchecked(dest);

  PushRegsInMask(save);

  using Fn = int32_t (*)(float);
  setupUnalignedABICall(dest);
  passABIArg(src, ABIType::Float32);
  callWithABI<Fn, jit::Float32ToFloat16>();
  storeCallInt32Result(dest);

  PopRegsInMask(save);
}

void MacroAssembler::moveGPRToFloat16(Register src, FloatRegister dest,
                                      Register temp,
                                      LiveRegisterSet volatileLiveRegs) {
  if (MacroAssembler::SupportsFloat32To16()) {
    moveGPRToFloat16(src, dest);

    // Float16 is currently passed as Float32, so expand again to Float32.
    convertFloat16ToFloat32(dest, dest);
    return;
  }

  LiveRegisterSet save = volatileLiveRegs;
  save.takeUnchecked(dest);
  save.takeUnchecked(dest.asDouble());
  save.takeUnchecked(temp);

  PushRegsInMask(save);

  using Fn = float (*)(int32_t);
  setupUnalignedABICall(temp);
  passABIArg(src);
  callWithABI<Fn, jit::Float16ToFloat32>(ABIType::Float32);
  storeCallFloatResult(dest);

  PopRegsInMask(save);
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

void MacroAssembler::debugAssertGCThingIsTenured(Register ptr, Register temp) {
#ifdef DEBUG
  Label done;
  branchPtrInNurseryChunk(Assembler::NotEqual, ptr, temp, &done);
  assumeUnreachable("Expected a tenured pointer");
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

void MacroAssembler::loadArgumentsObjectLength(Register obj, Register output) {
  // Get initial length value.
  unboxInt32(Address(obj, ArgumentsObject::getInitialLengthSlotOffset()),
             output);

#ifdef DEBUG
  // Assert length hasn't been overridden.
  Label ok;
  branchTest32(Assembler::Zero, output,
               Imm32(ArgumentsObject::LENGTH_OVERRIDDEN_BIT), &ok);
  assumeUnreachable("arguments object length has been overridden");
  bind(&ok);
#endif

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
  loadObjClassUnsafe(obj, output);

  // Map resizable to fixed-length TypedArray classes.
  Label fixedLength;
  branchPtr(Assembler::Below, output,
            ImmPtr(std::end(TypedArrayObject::fixedLengthClasses)),
            &fixedLength);
  {
    MOZ_ASSERT(std::end(TypedArrayObject::fixedLengthClasses) ==
                   std::begin(TypedArrayObject::resizableClasses),
               "TypedArray classes are in contiguous memory");

    const auto* firstFixedLengthTypedArrayClass =
        std::begin(TypedArrayObject::fixedLengthClasses);
    const auto* firstResizableTypedArrayClass =
        std::begin(TypedArrayObject::resizableClasses);

    MOZ_ASSERT(firstFixedLengthTypedArrayClass < firstResizableTypedArrayClass);

    ptrdiff_t diff =
        firstResizableTypedArrayClass - firstFixedLengthTypedArrayClass;

    mozilla::CheckedInt<int32_t> checked = diff;
    checked *= sizeof(JSClass);
    MOZ_ASSERT(checked.isValid(), "pointer difference fits in int32");

    subPtr(Imm32(int32_t(checked.value())), output);
  }
  bind(&fixedLength);

#ifdef DEBUG
  Label invalidClass, validClass;
  branchPtr(Assembler::Below, output,
            ImmPtr(std::begin(TypedArrayObject::fixedLengthClasses)),
            &invalidClass);
  branchPtr(Assembler::Below, output,
            ImmPtr(std::end(TypedArrayObject::fixedLengthClasses)),
            &validClass);
  bind(&invalidClass);
  assumeUnreachable("value isn't a valid FixedLengthTypedArray class");
  bind(&validClass);
#endif

  auto classForType = [](Scalar::Type type) {
    MOZ_ASSERT(type < Scalar::MaxTypedArrayViewType);
    return &TypedArrayObject::fixedLengthClasses[type];
  };

  Label one, two, four, eight, done;

  static_assert(ValidateSizeRange(Scalar::Int8, Scalar::Int16),
                "element size is one in [Int8, Int16)");
  branchPtr(Assembler::Below, output, ImmPtr(classForType(Scalar::Int16)),
            &one);

  static_assert(ValidateSizeRange(Scalar::Int16, Scalar::Int32),
                "element size is two in [Int16, Int32)");
  branchPtr(Assembler::Below, output, ImmPtr(classForType(Scalar::Int32)),
            &two);

  static_assert(ValidateSizeRange(Scalar::Int32, Scalar::Float64),
                "element size is four in [Int32, Float64)");
  branchPtr(Assembler::Below, output, ImmPtr(classForType(Scalar::Float64)),
            &four);

  static_assert(ValidateSizeRange(Scalar::Float64, Scalar::Uint8Clamped),
                "element size is eight in [Float64, Uint8Clamped)");
  branchPtr(Assembler::Below, output,
            ImmPtr(classForType(Scalar::Uint8Clamped)), &eight);

  static_assert(ValidateSizeRange(Scalar::Uint8Clamped, Scalar::BigInt64),
                "element size is one in [Uint8Clamped, BigInt64)");
  branchPtr(Assembler::Below, output, ImmPtr(classForType(Scalar::BigInt64)),
            &one);

  static_assert(ValidateSizeRange(Scalar::BigInt64, Scalar::Float16),
                "element size is eight in [BigInt64, Float16)");
  branchPtr(Assembler::Below, output, ImmPtr(classForType(Scalar::Float16)),
            &eight);

  static_assert(
      ValidateSizeRange(Scalar::Float16, Scalar::MaxTypedArrayViewType),
      "element size is two in [Float16, MaxTypedArrayViewType)");
  jump(&two);

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

void MacroAssembler::resizableTypedArrayElementShiftBy(Register obj,
                                                       Register output,
                                                       Register scratch) {
  loadObjClassUnsafe(obj, scratch);

#ifdef DEBUG
  Label invalidClass, validClass;
  branchPtr(Assembler::Below, scratch,
            ImmPtr(std::begin(TypedArrayObject::resizableClasses)),
            &invalidClass);
  branchPtr(Assembler::Below, scratch,
            ImmPtr(std::end(TypedArrayObject::resizableClasses)), &validClass);
  bind(&invalidClass);
  assumeUnreachable("value isn't a valid ResizableLengthTypedArray class");
  bind(&validClass);
#endif

  auto classForType = [](Scalar::Type type) {
    MOZ_ASSERT(type < Scalar::MaxTypedArrayViewType);
    return &TypedArrayObject::resizableClasses[type];
  };

  Label zero, one, two, three;

  static_assert(ValidateSizeRange(Scalar::Int8, Scalar::Int16),
                "element shift is zero in [Int8, Int16)");
  branchPtr(Assembler::Below, scratch, ImmPtr(classForType(Scalar::Int16)),
            &zero);

  static_assert(ValidateSizeRange(Scalar::Int16, Scalar::Int32),
                "element shift is one in [Int16, Int32)");
  branchPtr(Assembler::Below, scratch, ImmPtr(classForType(Scalar::Int32)),
            &one);

  static_assert(ValidateSizeRange(Scalar::Int32, Scalar::Float64),
                "element shift is two in [Int32, Float64)");
  branchPtr(Assembler::Below, scratch, ImmPtr(classForType(Scalar::Float64)),
            &two);

  static_assert(ValidateSizeRange(Scalar::Float64, Scalar::Uint8Clamped),
                "element shift is three in [Float64, Uint8Clamped)");
  branchPtr(Assembler::Below, scratch,
            ImmPtr(classForType(Scalar::Uint8Clamped)), &three);

  static_assert(ValidateSizeRange(Scalar::Uint8Clamped, Scalar::BigInt64),
                "element shift is zero in [Uint8Clamped, BigInt64)");
  branchPtr(Assembler::Below, scratch, ImmPtr(classForType(Scalar::BigInt64)),
            &zero);

  static_assert(ValidateSizeRange(Scalar::BigInt64, Scalar::Float16),
                "element shift is three in [BigInt64, Float16)");
  branchPtr(Assembler::Below, scratch, ImmPtr(classForType(Scalar::Float16)),
            &three);

  static_assert(
      ValidateSizeRange(Scalar::Float16, Scalar::MaxTypedArrayViewType),
      "element shift is one in [Float16, MaxTypedArrayViewType)");
  jump(&one);

  bind(&three);
  rshiftPtr(Imm32(3), output);
  jump(&zero);

  bind(&two);
  rshiftPtr(Imm32(2), output);
  jump(&zero);

  bind(&one);
  rshiftPtr(Imm32(1), output);

  bind(&zero);
}

void MacroAssembler::branchIfClassIsNotTypedArray(Register clasp,
                                                  Label* notTypedArray) {
  // Inline implementation of IsTypedArrayClass().

  const auto* firstTypedArrayClass =
      std::begin(TypedArrayObject::fixedLengthClasses);
  const auto* lastTypedArrayClass =
      std::prev(std::end(TypedArrayObject::resizableClasses));
  MOZ_ASSERT(std::end(TypedArrayObject::fixedLengthClasses) ==
                 std::begin(TypedArrayObject::resizableClasses),
             "TypedArray classes are in contiguous memory");

  branchPtr(Assembler::Below, clasp, ImmPtr(firstTypedArrayClass),
            notTypedArray);
  branchPtr(Assembler::Above, clasp, ImmPtr(lastTypedArrayClass),
            notTypedArray);
}

void MacroAssembler::branchIfClassIsNotFixedLengthTypedArray(
    Register clasp, Label* notTypedArray) {
  // Inline implementation of IsFixedLengthTypedArrayClass().

  const auto* firstTypedArrayClass =
      std::begin(TypedArrayObject::fixedLengthClasses);
  const auto* lastTypedArrayClass =
      std::prev(std::end(TypedArrayObject::fixedLengthClasses));

  branchPtr(Assembler::Below, clasp, ImmPtr(firstTypedArrayClass),
            notTypedArray);
  branchPtr(Assembler::Above, clasp, ImmPtr(lastTypedArrayClass),
            notTypedArray);
}

void MacroAssembler::branchIfClassIsNotResizableTypedArray(
    Register clasp, Label* notTypedArray) {
  // Inline implementation of IsResizableTypedArrayClass().

  const auto* firstTypedArrayClass =
      std::begin(TypedArrayObject::resizableClasses);
  const auto* lastTypedArrayClass =
      std::prev(std::end(TypedArrayObject::resizableClasses));

  branchPtr(Assembler::Below, clasp, ImmPtr(firstTypedArrayClass),
            notTypedArray);
  branchPtr(Assembler::Above, clasp, ImmPtr(lastTypedArrayClass),
            notTypedArray);
}

void MacroAssembler::branchIfHasDetachedArrayBuffer(BranchIfDetached branchIf,
                                                    Register obj, Register temp,
                                                    Label* label) {
  // Inline implementation of ArrayBufferViewObject::hasDetachedBuffer().

  // TODO: The data-slot of detached views is set to undefined, which would be
  // a faster way to detect detached buffers.

  // auto cond = branchIf == BranchIfDetached::Yes ? Assembler::Equal
  //                                               : Assembler::NotEqual;
  // branchTestUndefined(cond, Address(obj,
  //                     ArrayBufferViewObject::dataOffset()), label);

  Label done;
  Label* ifNotDetached = branchIf == BranchIfDetached::Yes ? &done : label;
  Condition detachedCond =
      branchIf == BranchIfDetached::Yes ? Assembler::NonZero : Assembler::Zero;

  // Load obj->elements in temp.
  loadPtr(Address(obj, NativeObject::offsetOfElements()), temp);

  // Shared buffers can't be detached.
  branchTest32(Assembler::NonZero,
               Address(temp, ObjectElements::offsetOfFlags()),
               Imm32(ObjectElements::SHARED_MEMORY), ifNotDetached);

  // An ArrayBufferView with a null/true buffer has never had its buffer
  // exposed, so nothing can possibly detach it.
  fallibleUnboxObject(Address(obj, ArrayBufferViewObject::bufferOffset()), temp,
                      ifNotDetached);

  // Load the ArrayBuffer flags and branch if the detached flag is (not) set.
  unboxInt32(Address(temp, ArrayBufferObject::offsetOfFlagsSlot()), temp);
  branchTest32(detachedCond, temp, Imm32(ArrayBufferObject::DETACHED), label);

  if (branchIf == BranchIfDetached::Yes) {
    bind(&done);
  }
}

void MacroAssembler::branchIfResizableArrayBufferViewOutOfBounds(Register obj,
                                                                 Register temp,
                                                                 Label* label) {
  // Implementation of ArrayBufferViewObject::isOutOfBounds().

  Label done;

  loadArrayBufferViewLengthIntPtr(obj, temp);
  branchPtr(Assembler::NotEqual, temp, ImmWord(0), &done);

  loadArrayBufferViewByteOffsetIntPtr(obj, temp);
  branchPtr(Assembler::NotEqual, temp, ImmWord(0), &done);

  loadPrivate(Address(obj, ArrayBufferViewObject::initialLengthOffset()), temp);
  branchPtr(Assembler::NotEqual, temp, ImmWord(0), label);

  loadPrivate(Address(obj, ArrayBufferViewObject::initialByteOffsetOffset()),
              temp);
  branchPtr(Assembler::NotEqual, temp, ImmWord(0), label);

  bind(&done);
}

void MacroAssembler::branchIfResizableArrayBufferViewInBounds(Register obj,
                                                              Register temp,
                                                              Label* label) {
  // Implementation of ArrayBufferViewObject::isOutOfBounds().

  Label done;

  loadArrayBufferViewLengthIntPtr(obj, temp);
  branchPtr(Assembler::NotEqual, temp, ImmWord(0), label);

  loadArrayBufferViewByteOffsetIntPtr(obj, temp);
  branchPtr(Assembler::NotEqual, temp, ImmWord(0), label);

  loadPrivate(Address(obj, ArrayBufferViewObject::initialLengthOffset()), temp);
  branchPtr(Assembler::NotEqual, temp, ImmWord(0), &done);

  loadPrivate(Address(obj, ArrayBufferViewObject::initialByteOffsetOffset()),
              temp);
  branchPtr(Assembler::Equal, temp, ImmWord(0), label);

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
  andPtr(Imm32(ShapeCachePtr::MASK), dest, temp3);
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

void MacroAssembler::prepareOOBStoreElement(Register object, Register index,
                                            Register elements,
                                            Register maybeTemp, Label* failure,
                                            LiveRegisterSet volatileLiveRegs) {
  Address length(elements, ObjectElements::offsetOfLength());
  Address initLength(elements, ObjectElements::offsetOfInitializedLength());
  Address capacity(elements, ObjectElements::offsetOfCapacity());
  Address flags(elements, ObjectElements::offsetOfFlags());

  // If index < capacity, we can add a dense element inline. If not, we
  // need to allocate more elements.
  Label allocElement, enoughCapacity;
  spectreBoundsCheck32(index, capacity, maybeTemp, &allocElement);
  jump(&enoughCapacity);

  bind(&allocElement);

  // We currently only support storing one past the current capacity.
  // We could add support for stores beyond that point by calling a different
  // function, but then we'd have to think carefully about when to go sparse.
  branch32(Assembler::NotEqual, capacity, index, failure);

  volatileLiveRegs.takeUnchecked(elements);
  if (maybeTemp != InvalidReg) {
    volatileLiveRegs.takeUnchecked(maybeTemp);
  }
  PushRegsInMask(volatileLiveRegs);

  // Use `elements` as a scratch register because we're about to reallocate it.
  using Fn = bool (*)(JSContext* cx, NativeObject* obj);
  setupUnalignedABICall(elements);
  loadJSContext(elements);
  passABIArg(elements);
  passABIArg(object);
  callWithABI<Fn, NativeObject::addDenseElementPure>();
  storeCallPointerResult(elements);

  PopRegsInMask(volatileLiveRegs);
  branchIfFalseBool(elements, failure);

  // Load the reallocated elements pointer.
  loadPtr(Address(object, NativeObject::offsetOfElements()), elements);

  bind(&enoughCapacity);

  // If our caller couldn't give us a temp register, use `object`.
  Register temp;
  if (maybeTemp == InvalidReg) {
    push(object);
    temp = object;
  } else {
    temp = maybeTemp;
  }

  // Load the index of the first uninitialized element into `temp`.
  load32(initLength, temp);

  // If it is not `index`, mark this elements array as non-packed.
  Label noHoles, loop, done;
  branch32(Assembler::Equal, temp, index, &noHoles);
  or32(Imm32(ObjectElements::NON_PACKED), flags);

  // Loop over intermediate elements and fill them with the magic hole value.
  bind(&loop);
  storeValue(MagicValue(JS_ELEMENTS_HOLE), BaseValueIndex(elements, temp));
  add32(Imm32(1), temp);
  branch32(Assembler::NotEqual, temp, index, &loop);

  bind(&noHoles);

  // The new initLength is index + 1. Update it.
  add32(Imm32(1), temp);
  store32(temp, initLength);

  // If necessary, update length as well.
  branch32(Assembler::Above, length, temp, &done);
  store32(temp, length);
  bind(&done);

  if (maybeTemp == InvalidReg) {
    pop(object);
  }
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
  // Inline implementation of |OrderedHashTableImpl::prepareHash()| and
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
  move32(value.typeReg(), temp);
#endif

  // mozilla::WrappingMultiply(kGoldenRatioU32, RotateLeft5(aHash) ^ aValue);
  // with |aHash = 0| and |aValue = v1|.
  mul32(Imm32(mozilla::kGoldenRatioU32), result);

  // mozilla::WrappingMultiply(kGoldenRatioU32, RotateLeft5(aHash) ^ aValue);
  // with |aHash = <above hash>| and |aValue = v2|.
  rotateLeft(Imm32(5), result, result);
  xor32(temp, result);

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
  // Inline implementation of |OrderedHashTableImpl::prepareHash()| and
  // |JSAtom::hash()|.

#ifdef DEBUG
  Label ok;
  branchTest32(Assembler::NonZero, Address(str, JSString::offsetOfFlags()),
               Imm32(JSString::ATOM_BIT), &ok);
  assumeUnreachable("Unexpected non-atom string");
  bind(&ok);
#endif

#ifdef JS_64BIT
  static_assert(FatInlineAtom::offsetOfHash() == NormalAtom::offsetOfHash());
  load32(Address(str, NormalAtom::offsetOfHash()), result);
#else
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
#endif

  scrambleHashCode(result);
}

void MacroAssembler::prepareHashSymbol(Register sym, Register result) {
  // Inline implementation of |OrderedHashTableImpl::prepareHash()| and
  // |Symbol::hash()|.

  load32(Address(sym, JS::Symbol::offsetOfHash()), result);

  scrambleHashCode(result);
}

void MacroAssembler::prepareHashBigInt(Register bigInt, Register result,
                                       Register temp1, Register temp2,
                                       Register temp3) {
  // Inline implementation of |OrderedHashTableImpl::prepareHash()| and
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
  // Inline implementation of |OrderedHashTableImpl::prepareHash()| and
  // |HashCodeScrambler::scramble(v.asRawBits())|.

  // Load |HashCodeScrambler*|. If the object has no buffer yet this will be
  // nullptr. In this case we use 0 as hash number because the hash won't be
  // used in MacroAssembler::orderedHashTableLookup when there are no entries.
  Label done;
  static_assert(MapObject::offsetOfHashCodeScrambler() ==
                SetObject::offsetOfHashCodeScrambler());
  loadPrivate(Address(setObj, SetObject::offsetOfHashCodeScrambler()), temp1);
  move32(Imm32(0), result);
  branchTestPtr(Assembler::Zero, temp1, temp1, &done);

  // Load |HashCodeScrambler::mK0| and |HashCodeScrambler::mK1|.
  auto k0 = Register64(temp1);
  auto k1 = Register64(temp2);
  load64(Address(temp1, mozilla::HashCodeScrambler::offsetOfMK1()), k1);
  load64(Address(temp1, mozilla::HashCodeScrambler::offsetOfMK0()), k0);

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

  bind(&done);
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

template <typename TableObject>
void MacroAssembler::orderedHashTableLookup(Register setOrMapObj,
                                            ValueOperand value, Register hash,
                                            Register entryTemp, Register temp1,
                                            Register temp2, Register temp3,
                                            Register temp4, Label* found,
                                            IsBigInt isBigInt) {
  // Inline implementation of |OrderedHashTableImpl::lookup()|.

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

  // Jump to notFound if the hash table has no entries and may not have a
  // buffer. Check this before calling Assert{Map,Set}ObjectHash because |hash|
  // may be 0 when there's no hash code scrambler.
  Label notFound;
  unboxInt32(Address(setOrMapObj, TableObject::offsetOfLiveCount()), temp1);
  branchTest32(Assembler::Zero, temp1, temp1, &notFound);

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

  if constexpr (std::is_same_v<TableObject, SetObject>) {
    using Fn =
        void (*)(JSContext*, SetObject*, const Value*, mozilla::HashNumber);
    callWithABI<Fn, jit::AssertSetObjectHash>();
  } else {
    static_assert(std::is_same_v<TableObject, MapObject>);
    using Fn =
        void (*)(JSContext*, MapObject*, const Value*, mozilla::HashNumber);
    callWithABI<Fn, jit::AssertMapObjectHash>();
  }

  popValue(value);
  PopRegsInMask(LiveRegisterSet(RegisterSet::Volatile()));
#endif

  // Determine the bucket by computing |hash >> object->hashShift|. The hash
  // shift is stored as PrivateUint32Value.
  move32(hash, entryTemp);
  unboxInt32(Address(setOrMapObj, TableObject::offsetOfHashShift()), temp2);
  flexibleRshift32(temp2, entryTemp);

  loadPrivate(Address(setOrMapObj, TableObject::offsetOfHashTable()), temp2);
  loadPtr(BaseIndex(temp2, entryTemp, ScalePointer), entryTemp);

  // Search for a match in this bucket.
  Label start, loop;
  jump(&start);
  bind(&loop);
  {
    // Inline implementation of |HashableValue::operator==|.

    static_assert(TableObject::Table::offsetOfImplDataElement() == 0,
                  "offsetof(Data, element) is 0");
    auto keyAddr = Address(entryTemp, TableObject::Table::offsetOfEntryKey());

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
  loadPtr(Address(entryTemp, TableObject::Table::offsetOfImplDataChain()),
          entryTemp);
  bind(&start);
  branchTestPtr(Assembler::NonZero, entryTemp, entryTemp, &loop);

  bind(&notFound);
}

void MacroAssembler::setObjectHas(Register setObj, ValueOperand value,
                                  Register hash, Register result,
                                  Register temp1, Register temp2,
                                  Register temp3, Register temp4,
                                  IsBigInt isBigInt) {
  Label found;
  orderedHashTableLookup<SetObject>(setObj, value, hash, result, temp1, temp2,
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
  orderedHashTableLookup<MapObject>(mapObj, value, hash, result, temp1, temp2,
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
  orderedHashTableLookup<MapObject>(mapObj, value, hash, temp1, temp2, temp3,
                                    temp4, temp5, &found, isBigInt);

  Label done;
  moveValue(UndefinedValue(), result);
  jump(&done);

  // |temp1| holds the found entry.
  bind(&found);
  loadValue(Address(temp1, MapObject::Table::Entry::offsetOfValue()), result);

  bind(&done);
}

template <typename TableObject>
void MacroAssembler::loadOrderedHashTableCount(Register setOrMapObj,
                                               Register result) {
  // Inline implementation of |OrderedHashTableImpl::count()|.

  // Load the live count, stored as PrivateUint32Value.
  unboxInt32(Address(setOrMapObj, TableObject::offsetOfLiveCount()), result);
}

void MacroAssembler::loadSetObjectSize(Register setObj, Register result) {
  loadOrderedHashTableCount<SetObject>(setObj, result);
}

void MacroAssembler::loadMapObjectSize(Register mapObj, Register result) {
  loadOrderedHashTableCount<MapObject>(mapObj, result);
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

  lshiftPtr(Imm32(3), numStackValues, scratch1);
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

#ifdef FUZZING_JS_FUZZILLI
void MacroAssembler::fuzzilliHashDouble(FloatRegister src, Register result,
                                        Register temp) {
  canonicalizeDouble(src);

#  ifdef JS_PUNBOX64
  Register64 r64(temp);
#  else
  Register64 r64(temp, result);
#  endif

  moveDoubleToGPR64(src, r64);

#  ifdef JS_PUNBOX64
  // Move the high word into |result|.
  move64(r64, Register64(result));
  rshift64(Imm32(32), Register64(result));
#  endif

  // Add the high and low words of |r64|.
  add32(temp, result);
}

void MacroAssembler::fuzzilliStoreHash(Register value, Register temp1,
                                       Register temp2) {
  loadJSContext(temp1);

  // stats
  Address addrExecHashInputs(temp1, offsetof(JSContext, executionHashInputs));
  add32(Imm32(1), addrExecHashInputs);

  // hash
  Address addrExecHash(temp1, offsetof(JSContext, executionHash));
  load32(addrExecHash, temp2);
  add32(value, temp2);
  rotateLeft(Imm32(1), temp2, temp2);
  store32(temp2, addrExecHash);
}
#endif

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
