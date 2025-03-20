/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/CodeGenerator-x86.h"

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"

#include <iterator>

#include "jsnum.h"

#include "jit/CodeGenerator.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "js/Conversions.h"
#include "vm/Shape.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmInstanceData.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using JS::GenericNaN;
using mozilla::BitwiseCast;
using mozilla::DebugOnly;
using mozilla::FloatingPoint;

CodeGeneratorX86::CodeGeneratorX86(MIRGenerator* gen, LIRGraph* graph,
                                   MacroAssembler* masm)
    : CodeGeneratorX86Shared(gen, graph, masm) {}

ValueOperand CodeGeneratorX86::ToValue(LInstruction* ins, size_t pos) {
  Register typeReg = ToRegister(ins->getOperand(pos + TYPE_INDEX));
  Register payloadReg = ToRegister(ins->getOperand(pos + PAYLOAD_INDEX));
  return ValueOperand(typeReg, payloadReg);
}

ValueOperand CodeGeneratorX86::ToTempValue(LInstruction* ins, size_t pos) {
  Register typeReg = ToRegister(ins->getTemp(pos + TYPE_INDEX));
  Register payloadReg = ToRegister(ins->getTemp(pos + PAYLOAD_INDEX));
  return ValueOperand(typeReg, payloadReg);
}

void CodeGenerator::visitValue(LValue* value) {
  const ValueOperand out = ToOutValue(value);
  masm.moveValue(value->value(), out);
}

void CodeGenerator::visitBox(LBox* box) {
  const LDefinition* type = box->getDef(TYPE_INDEX);

  DebugOnly<const LAllocation*> a = box->getOperand(0);
  MOZ_ASSERT(!a->isConstant());

  // On x86, the input operand and the output payload have the same
  // virtual register. All that needs to be written is the type tag for
  // the type definition.
  masm.mov(ImmWord(MIRTypeToTag(box->type())), ToRegister(type));
}

void CodeGenerator::visitBoxFloatingPoint(LBoxFloatingPoint* box) {
  const AnyRegister in = ToAnyRegister(box->getOperand(0));
  const ValueOperand out = ToOutValue(box);

  masm.moveValue(TypedOrValueRegister(box->type(), in), out);

  if (JitOptions.spectreValueMasking) {
    Register scratch = ToRegister(box->spectreTemp());
    masm.move32(Imm32(JSVAL_TAG_CLEAR), scratch);
    masm.cmp32Move32(Assembler::Below, scratch, out.typeReg(), scratch,
                     out.typeReg());
  }
}

void CodeGenerator::visitUnbox(LUnbox* unbox) {
  // Note that for unbox, the type and payload indexes are switched on the
  // inputs.
  Operand type = ToOperand(unbox->type());
  Operand payload = ToOperand(unbox->payload());
  Register output = ToRegister(unbox->output());
  MUnbox* mir = unbox->mir();

  JSValueTag tag = MIRTypeToTag(mir->type());
  if (mir->fallible()) {
    masm.cmp32(type, Imm32(tag));
    bailoutIf(Assembler::NotEqual, unbox->snapshot());
  } else {
#ifdef DEBUG
    Label ok;
    masm.branch32(Assembler::Equal, type, Imm32(tag), &ok);
    masm.assumeUnreachable("Infallible unbox type mismatch");
    masm.bind(&ok);
#endif
  }

  // Note: If spectreValueMasking is disabled, then this instruction will
  // default to a no-op as long as the lowering allocate the same register for
  // the output and the payload.
  masm.unboxNonDouble(type, payload, output, ValueTypeFromMIRType(mir->type()));
}

void CodeGenerator::visitAtomicLoad64(LAtomicLoad64* lir) {
  Register elements = ToRegister(lir->elements());
  Register temp = ToRegister(lir->temp());
  Register64 temp64 = ToRegister64(lir->temp64());
  Register out = ToRegister(lir->output());

  MOZ_ASSERT(out == ecx);
  MOZ_ASSERT(temp == ebx);
  MOZ_ASSERT(temp64 == Register64(edx, eax));

  const MLoadUnboxedScalar* mir = lir->mir();

  Scalar::Type storageType = mir->storageType();

  if (lir->index()->isConstant()) {
    Address source =
        ToAddress(elements, lir->index(), storageType, mir->offsetAdjustment());
    masm.atomicLoad64(Synchronization::Load(), source, Register64(ecx, ebx),
                      Register64(edx, eax));
  } else {
    BaseIndex source(elements, ToRegister(lir->index()),
                     ScaleFromScalarType(storageType), mir->offsetAdjustment());
    masm.atomicLoad64(Synchronization::Load(), source, Register64(ecx, ebx),
                      Register64(edx, eax));
  }

  emitCreateBigInt(lir, storageType, temp64, out, temp);
}

void CodeGenerator::visitAtomicStore64(LAtomicStore64* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register64 temp2 = Register64(value, ToRegister(lir->tempLow()));

  MOZ_ASSERT(temp1 == Register64(ecx, ebx));
  MOZ_ASSERT(temp2 == Register64(edx, eax));

  Scalar::Type writeType = lir->mir()->writeType();

  masm.loadBigInt64(value, temp1);

  masm.push(value);
  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), writeType);
    masm.atomicStore64(Synchronization::Store(), dest, temp1, temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(writeType));
    masm.atomicStore64(Synchronization::Store(), dest, temp1, temp2);
  }
  masm.pop(value);
}

void CodeGenerator::visitCompareExchangeTypedArrayElement64(
    LCompareExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register oldval = ToRegister(lir->oldval());
  DebugOnly<Register> newval = ToRegister(lir->newval());
  DebugOnly<Register> temp = ToRegister(lir->tempLow());
  Register out = ToRegister(lir->output());

  MOZ_ASSERT(elements == esi);
  MOZ_ASSERT(oldval == eax);
  MOZ_ASSERT(newval.inspect() == edx);
  MOZ_ASSERT(temp.inspect() == ebx);
  MOZ_ASSERT(out == ecx);

  Scalar::Type arrayType = lir->mir()->arrayType();

  DebugOnly<uint32_t> framePushed = masm.framePushed();

  // Save eax and edx before they're clobbered below.
  masm.push(eax);
  masm.push(edx);

  auto restoreSavedRegisters = [&]() {
    masm.pop(edx);
    masm.pop(eax);
  };

  Register64 expected = Register64(edx, eax);
  Register64 replacement = Register64(ecx, ebx);

  // Load |oldval| and |newval| into |expected| resp. |replacement|.
  {
    // Use `esi` as a temp register.
    Register bigInt = esi;
    masm.push(bigInt);

    masm.mov(oldval, bigInt);
    masm.loadBigInt64(bigInt, expected);

    // |newval| is stored in `edx`, which is already pushed onto the stack.
    masm.loadPtr(Address(masm.getStackPointer(), sizeof(uintptr_t)), bigInt);
    masm.loadBigInt64(bigInt, replacement);

    masm.pop(bigInt);
  }

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.compareExchange64(Synchronization::Full(), dest, expected, replacement,
                           expected);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.compareExchange64(Synchronization::Full(), dest, expected, replacement,
                           expected);
  }

  // Move the result from `edx:eax` to `ecx:ebx`.
  masm.move64(expected, replacement);

  // OutOfLineCallVM tracks the currently pushed stack entries as reported by
  // |masm.framePushed()|. We mustn't have any additional entries on the stack
  // which weren't previously recorded by the safepoint, otherwise the GC
  // complains when tracing the Ion frames, because the stack frames don't
  // have their expected layout.
  MOZ_ASSERT(framePushed == masm.framePushed());

  OutOfLineCode* ool = createBigIntOutOfLine(lir, arrayType, replacement, out);

  // Use `edx:eax`, which are both already on the stack, as temp registers.
  Register bigInt = eax;
  Register temp2 = edx;

  Label fail;
  masm.newGCBigInt(bigInt, temp2, initialBigIntHeap(), &fail);
  masm.initializeBigInt64(arrayType, bigInt, replacement);
  masm.mov(bigInt, out);
  restoreSavedRegisters();
  masm.jump(ool->rejoin());

  // Couldn't create the BigInt. Restore `edx:eax` and call into the VM.
  masm.bind(&fail);
  restoreSavedRegisters();
  masm.jump(ool->entry());

  // At this point `edx:eax` must have been restored to their original values.
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitAtomicExchangeTypedArrayElement64(
    LAtomicExchangeTypedArrayElement64* lir) {
  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register out = ToRegister(lir->output());
  Register64 temp2 = Register64(value, out);

  MOZ_ASSERT(value == edx);
  MOZ_ASSERT(temp1 == Register64(ecx, ebx));
  MOZ_ASSERT(temp2 == Register64(edx, eax));
  MOZ_ASSERT(out == eax);

  Scalar::Type arrayType = lir->mir()->arrayType();

  DebugOnly<uint32_t> framePushed = masm.framePushed();

  // Save edx before it's clobbered below.
  masm.push(edx);

  auto restoreSavedRegisters = [&]() { masm.pop(edx); };

  masm.loadBigInt64(value, temp1);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicExchange64(Synchronization::Full(), dest, temp1, temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicExchange64(Synchronization::Full(), dest, temp1, temp2);
  }

  // Move the result from `edx:eax` to `ecx:ebx`.
  masm.move64(temp2, temp1);

  // OutOfLineCallVM tracks the currently pushed stack entries as reported by
  // |masm.framePushed()|. We mustn't have any additional entries on the stack
  // which weren't previously recorded by the safepoint, otherwise the GC
  // complains when tracing the Ion frames, because the stack frames don't
  // have their expected layout.
  MOZ_ASSERT(framePushed == masm.framePushed());

  OutOfLineCode* ool = createBigIntOutOfLine(lir, arrayType, temp1, out);

  // Use `edx`, which is already on the stack, as a temp register.
  Register temp = edx;

  Label fail;
  masm.newGCBigInt(out, temp, initialBigIntHeap(), &fail);
  masm.initializeBigInt64(arrayType, out, temp1);
  restoreSavedRegisters();
  masm.jump(ool->rejoin());

  // Couldn't create the BigInt. Restore `edx` and call into the VM.
  masm.bind(&fail);
  restoreSavedRegisters();
  masm.jump(ool->entry());

  // At this point `edx` must have been restored to its original value.
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitAtomicTypedArrayElementBinop64(
    LAtomicTypedArrayElementBinop64* lir) {
  MOZ_ASSERT(!lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register out = ToRegister(lir->output());
  Register64 temp2 = Register64(value, out);

  MOZ_ASSERT(value == edx);
  MOZ_ASSERT(temp1 == Register64(ecx, ebx));
  MOZ_ASSERT(temp2 == Register64(edx, eax));
  MOZ_ASSERT(out == eax);

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  DebugOnly<uint32_t> framePushed = masm.framePushed();

  // Save edx before it's clobbered below.
  masm.push(edx);

  auto restoreSavedRegisters = [&]() { masm.pop(edx); };

  masm.loadBigInt64(value, temp1);

  masm.Push(temp1);

  Address addr(masm.getStackPointer(), 0);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, addr, dest, temp1,
                         temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, addr, dest, temp1,
                         temp2);
  }

  masm.freeStack(sizeof(uint64_t));

  // Move the result from `edx:eax` to `ecx:ebx`.
  masm.move64(temp2, temp1);

  // OutOfLineCallVM tracks the currently pushed stack entries as reported by
  // |masm.framePushed()|. We mustn't have any additional entries on the stack
  // which weren't previously recorded by the safepoint, otherwise the GC
  // complains when tracing the Ion frames, because the stack frames don't
  // have their expected layout.
  MOZ_ASSERT(framePushed == masm.framePushed());

  OutOfLineCode* ool = createBigIntOutOfLine(lir, arrayType, temp1, out);

  // Use `edx`, which is already on the stack, as a temp register.
  Register temp = edx;

  Label fail;
  masm.newGCBigInt(out, temp, initialBigIntHeap(), &fail);
  masm.initializeBigInt64(arrayType, out, temp1);
  restoreSavedRegisters();
  masm.jump(ool->rejoin());

  // Couldn't create the BigInt. Restore `edx` and call into the VM.
  masm.bind(&fail);
  restoreSavedRegisters();
  masm.jump(ool->entry());

  // At this point `edx` must have been restored to its original value.
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitAtomicTypedArrayElementBinopForEffect64(
    LAtomicTypedArrayElementBinopForEffect64* lir) {
  MOZ_ASSERT(lir->mir()->isForEffect());

  Register elements = ToRegister(lir->elements());
  Register value = ToRegister(lir->value());
  Register64 temp1 = ToRegister64(lir->temp1());
  Register tempLow = ToRegister(lir->tempLow());
  Register64 temp2 = Register64(value, tempLow);

  MOZ_ASSERT(value == edx);
  MOZ_ASSERT(temp1 == Register64(ecx, ebx));
  MOZ_ASSERT(temp2 == Register64(edx, eax));
  MOZ_ASSERT(tempLow == eax);

  Scalar::Type arrayType = lir->mir()->arrayType();
  AtomicOp atomicOp = lir->mir()->operation();

  // Save edx before it's clobbered below.
  masm.push(edx);

  masm.loadBigInt64(value, temp1);

  masm.Push(temp1);

  Address addr(masm.getStackPointer(), 0);

  if (lir->index()->isConstant()) {
    Address dest = ToAddress(elements, lir->index(), arrayType);
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, addr, dest, temp1,
                         temp2);
  } else {
    BaseIndex dest(elements, ToRegister(lir->index()),
                   ScaleFromScalarType(arrayType));
    masm.atomicFetchOp64(Synchronization::Full(), atomicOp, addr, dest, temp1,
                         temp2);
  }

  masm.freeStack(sizeof(uint64_t));

  masm.pop(edx);
}

void CodeGenerator::visitWasmUint32ToDouble(LWasmUint32ToDouble* lir) {
  Register input = ToRegister(lir->input());
  Register temp = ToRegister(lir->temp());

  if (input != temp) {
    masm.mov(input, temp);
  }

  // Beware: convertUInt32ToDouble clobbers input.
  masm.convertUInt32ToDouble(temp, ToFloatRegister(lir->output()));
}

void CodeGenerator::visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir) {
  Register input = ToRegister(lir->input());
  Register temp = ToRegister(lir->temp());
  FloatRegister output = ToFloatRegister(lir->output());

  if (input != temp) {
    masm.mov(input, temp);
  }

  // Beware: convertUInt32ToFloat32 clobbers input.
  masm.convertUInt32ToFloat32(temp, output);
}

template <typename T>
void CodeGeneratorX86::emitWasmLoad(T* ins) {
  const MWasmLoad* mir = ins->mir();

  mir->access().assertOffsetInGuardPages();
  uint32_t offset = mir->access().offset();

  const LAllocation* ptr = ins->ptr();
  const LAllocation* memoryBase = ins->memoryBase();

  // Lowering has set things up so that we can use a BaseIndex form if the
  // pointer is constant and the offset is zero, or if the pointer is zero.

  Operand srcAddr =
      ptr->isBogus()
          ? Operand(ToRegister(memoryBase),
                    offset ? offset : mir->base()->toConstant()->toInt32())
          : Operand(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

  if (mir->type() == MIRType::Int64) {
    MOZ_ASSERT_IF(mir->access().isAtomic(),
                  mir->access().type() != Scalar::Int64);
    masm.wasmLoadI64(mir->access(), srcAddr, ToOutRegister64(ins));
  } else {
    masm.wasmLoad(mir->access(), srcAddr, ToAnyRegister(ins->output()));
  }
}

void CodeGenerator::visitWasmLoad(LWasmLoad* ins) { emitWasmLoad(ins); }

void CodeGenerator::visitWasmLoadI64(LWasmLoadI64* ins) { emitWasmLoad(ins); }

template <typename T>
void CodeGeneratorX86::emitWasmStore(T* ins) {
  const MWasmStore* mir = ins->mir();

  mir->access().assertOffsetInGuardPages();
  uint32_t offset = mir->access().offset();

  const LAllocation* ptr = ins->ptr();
  const LAllocation* memoryBase = ins->memoryBase();

  // Lowering has set things up so that we can use a BaseIndex form if the
  // pointer is constant and the offset is zero, or if the pointer is zero.

  Operand dstAddr =
      ptr->isBogus()
          ? Operand(ToRegister(memoryBase),
                    offset ? offset : mir->base()->toConstant()->toInt32())
          : Operand(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

  if (mir->access().type() == Scalar::Int64) {
    Register64 value =
        ToRegister64(ins->getInt64Operand(LWasmStoreI64::ValueIndex));
    masm.wasmStoreI64(mir->access(), value, dstAddr);
  } else {
    AnyRegister value = ToAnyRegister(ins->getOperand(LWasmStore::ValueIndex));
    masm.wasmStore(mir->access(), value, dstAddr);
  }
}

void CodeGenerator::visitWasmStore(LWasmStore* ins) { emitWasmStore(ins); }

void CodeGenerator::visitWasmStoreI64(LWasmStoreI64* ins) {
  emitWasmStore(ins);
}

void CodeGenerator::visitWasmCompareExchangeHeap(
    LWasmCompareExchangeHeap* ins) {
  MWasmCompareExchangeHeap* mir = ins->mir();

  Register ptrReg = ToRegister(ins->ptr());
  Register oldval = ToRegister(ins->oldValue());
  Register newval = ToRegister(ins->newValue());
  Register addrTemp = ToRegister(ins->addrTemp());
  Register memoryBase = ToRegister(ins->memoryBase());
  Register output = ToRegister(ins->output());

  masm.leal(Operand(memoryBase, ptrReg, TimesOne, mir->access().offset()),
            addrTemp);

  Address memAddr(addrTemp, 0);
  masm.wasmCompareExchange(mir->access(), memAddr, oldval, newval, output);
}

void CodeGenerator::visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins) {
  MWasmAtomicExchangeHeap* mir = ins->mir();

  Register ptrReg = ToRegister(ins->ptr());
  Register value = ToRegister(ins->value());
  Register addrTemp = ToRegister(ins->addrTemp());
  Register memoryBase = ToRegister(ins->memoryBase());
  Register output = ToRegister(ins->output());

  masm.leal(Operand(memoryBase, ptrReg, TimesOne, mir->access().offset()),
            addrTemp);

  Address memAddr(addrTemp, 0);
  masm.wasmAtomicExchange(mir->access(), memAddr, value, output);
}

void CodeGenerator::visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins) {
  MWasmAtomicBinopHeap* mir = ins->mir();

  Register ptrReg = ToRegister(ins->ptr());
  Register temp =
      ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());
  Register addrTemp = ToRegister(ins->addrTemp());
  Register out = ToRegister(ins->output());
  const LAllocation* value = ins->value();
  AtomicOp op = mir->operation();
  Register memoryBase = ToRegister(ins->memoryBase());

  masm.leal(Operand(memoryBase, ptrReg, TimesOne, mir->access().offset()),
            addrTemp);

  Address memAddr(addrTemp, 0);
  if (value->isConstant()) {
    masm.wasmAtomicFetchOp(mir->access(), op, Imm32(ToInt32(value)), memAddr,
                           temp, out);
  } else {
    masm.wasmAtomicFetchOp(mir->access(), op, ToRegister(value), memAddr, temp,
                           out);
  }
}

void CodeGenerator::visitWasmAtomicBinopHeapForEffect(
    LWasmAtomicBinopHeapForEffect* ins) {
  MWasmAtomicBinopHeap* mir = ins->mir();
  MOZ_ASSERT(!mir->hasUses());

  Register ptrReg = ToRegister(ins->ptr());
  Register addrTemp = ToRegister(ins->addrTemp());
  const LAllocation* value = ins->value();
  AtomicOp op = mir->operation();
  Register memoryBase = ToRegister(ins->memoryBase());

  masm.leal(Operand(memoryBase, ptrReg, TimesOne, mir->access().offset()),
            addrTemp);

  Address memAddr(addrTemp, 0);
  if (value->isConstant()) {
    masm.wasmAtomicEffectOp(mir->access(), op, Imm32(ToInt32(value)), memAddr,
                            InvalidReg);
  } else {
    masm.wasmAtomicEffectOp(mir->access(), op, ToRegister(value), memAddr,
                            InvalidReg);
  }
}

void CodeGenerator::visitWasmAtomicLoadI64(LWasmAtomicLoadI64* ins) {
  ins->mir()->access().assertOffsetInGuardPages();
  uint32_t offset = ins->mir()->access().offset();

  const LAllocation* memoryBase = ins->memoryBase();
  const LAllocation* ptr = ins->ptr();
  BaseIndex srcAddr(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

  MOZ_ASSERT(ToRegister(ins->t1()) == ecx);
  MOZ_ASSERT(ToRegister(ins->t2()) == ebx);
  MOZ_ASSERT(ToOutRegister64(ins).high == edx);
  MOZ_ASSERT(ToOutRegister64(ins).low == eax);

  masm.wasmAtomicLoad64(ins->mir()->access(), srcAddr, Register64(ecx, ebx),
                        Register64(edx, eax));
}

void CodeGenerator::visitWasmCompareExchangeI64(LWasmCompareExchangeI64* ins) {
  ins->mir()->access().assertOffsetInGuardPages();
  uint32_t offset = ins->mir()->access().offset();

  const LAllocation* memoryBase = ins->memoryBase();
  const LAllocation* ptr = ins->ptr();
  Operand srcAddr(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

  MOZ_ASSERT(ToRegister64(ins->expected()).low == eax);
  MOZ_ASSERT(ToRegister64(ins->expected()).high == edx);
  MOZ_ASSERT(ToRegister64(ins->replacement()).low == ebx);
  MOZ_ASSERT(ToRegister64(ins->replacement()).high == ecx);
  MOZ_ASSERT(ToOutRegister64(ins).low == eax);
  MOZ_ASSERT(ToOutRegister64(ins).high == edx);

  masm.append(ins->mir()->access(), wasm::TrapMachineInsn::Atomic,
              FaultingCodeOffset(masm.currentOffset()));
  masm.lock_cmpxchg8b(edx, eax, ecx, ebx, srcAddr);
}

template <typename T>
void CodeGeneratorX86::emitWasmStoreOrExchangeAtomicI64(
    T* ins, const wasm::MemoryAccessDesc& access) {
  access.assertOffsetInGuardPages();
  const LAllocation* memoryBase = ins->memoryBase();
  const LAllocation* ptr = ins->ptr();
  Operand srcAddr(ToRegister(memoryBase), ToRegister(ptr), TimesOne,
                  access.offset());

  DebugOnly<const LInt64Allocation> value = ins->value();
  MOZ_ASSERT(ToRegister64(value).low == ebx);
  MOZ_ASSERT(ToRegister64(value).high == ecx);

  // eax and edx will be overwritten every time through the loop but
  // memoryBase and ptr must remain live for a possible second iteration.

  MOZ_ASSERT(ToRegister(memoryBase) != edx && ToRegister(memoryBase) != eax);
  MOZ_ASSERT(ToRegister(ptr) != edx && ToRegister(ptr) != eax);

  Label again;
  masm.bind(&again);
  masm.append(access, wasm::TrapMachineInsn::Atomic,
              FaultingCodeOffset(masm.currentOffset()));
  masm.lock_cmpxchg8b(edx, eax, ecx, ebx, srcAddr);
  masm.j(Assembler::Condition::NonZero, &again);
}

void CodeGenerator::visitWasmAtomicStoreI64(LWasmAtomicStoreI64* ins) {
  MOZ_ASSERT(ToRegister(ins->t1()) == edx);
  MOZ_ASSERT(ToRegister(ins->t2()) == eax);

  emitWasmStoreOrExchangeAtomicI64(ins, ins->mir()->access());
}

void CodeGenerator::visitWasmAtomicExchangeI64(LWasmAtomicExchangeI64* ins) {
  MOZ_ASSERT(ToOutRegister64(ins).high == edx);
  MOZ_ASSERT(ToOutRegister64(ins).low == eax);

  emitWasmStoreOrExchangeAtomicI64(ins, ins->access());
}

void CodeGenerator::visitWasmAtomicBinopI64(LWasmAtomicBinopI64* ins) {
  ins->access().assertOffsetInGuardPages();
  uint32_t offset = ins->access().offset();

  const LAllocation* memoryBase = ins->memoryBase();
  const LAllocation* ptr = ins->ptr();

  BaseIndex srcAddr(ToRegister(memoryBase), ToRegister(ptr), TimesOne, offset);

  MOZ_ASSERT(ToRegister(memoryBase) == esi || ToRegister(memoryBase) == edi);
  MOZ_ASSERT(ToRegister(ptr) == esi || ToRegister(ptr) == edi);

  Register64 value = ToRegister64(ins->value());

  MOZ_ASSERT(value.low == ebx);
  MOZ_ASSERT(value.high == ecx);

  Register64 output = ToOutRegister64(ins);

  MOZ_ASSERT(output.low == eax);
  MOZ_ASSERT(output.high == edx);

  masm.Push(ecx);
  masm.Push(ebx);

  Address valueAddr(esp, 0);

  // Here the `value` register acts as a temp, we'll restore it below.
  masm.wasmAtomicFetchOp64(ins->access(), ins->operation(), valueAddr, srcAddr,
                           value, output);

  masm.Pop(ebx);
  masm.Pop(ecx);
}

namespace js {
namespace jit {

class OutOfLineTruncate : public OutOfLineCodeBase<CodeGeneratorX86> {
  LInstruction* ins_;

 public:
  explicit OutOfLineTruncate(LInstruction* ins) : ins_(ins) {
    MOZ_ASSERT(ins_->isTruncateDToInt32() ||
               ins_->isWasmBuiltinTruncateDToInt32());
  }

  void accept(CodeGeneratorX86* codegen) override {
    codegen->visitOutOfLineTruncate(this);
  }

  LAllocation* input() { return ins_->getOperand(0); }
  LDefinition* output() { return ins_->getDef(0); }
  LDefinition* tempFloat() { return ins_->getTemp(0); }

  wasm::BytecodeOffset bytecodeOffset() const {
    if (ins_->isTruncateDToInt32()) {
      return ins_->toTruncateDToInt32()->mir()->bytecodeOffset();
    }

    return ins_->toWasmBuiltinTruncateDToInt32()->mir()->bytecodeOffset();
  }
};

class OutOfLineTruncateFloat32 : public OutOfLineCodeBase<CodeGeneratorX86> {
  LInstruction* ins_;

 public:
  explicit OutOfLineTruncateFloat32(LInstruction* ins) : ins_(ins) {
    MOZ_ASSERT(ins_->isTruncateFToInt32() ||
               ins_->isWasmBuiltinTruncateFToInt32());
  }

  void accept(CodeGeneratorX86* codegen) override {
    codegen->visitOutOfLineTruncateFloat32(this);
  }

  LAllocation* input() { return ins_->getOperand(0); }
  LDefinition* output() { return ins_->getDef(0); }
  LDefinition* tempFloat() { return ins_->getTemp(0); }

  wasm::BytecodeOffset bytecodeOffset() const {
    if (ins_->isTruncateFToInt32()) {
      return ins_->toTruncateDToInt32()->mir()->bytecodeOffset();
    }

    return ins_->toWasmBuiltinTruncateFToInt32()->mir()->bytecodeOffset();
  }
};

}  // namespace jit
}  // namespace js

void CodeGenerator::visitTruncateDToInt32(LTruncateDToInt32* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  Register output = ToRegister(ins->output());

  OutOfLineTruncate* ool = new (alloc()) OutOfLineTruncate(ins);
  addOutOfLineCode(ool, ins->mir());

  masm.branchTruncateDoubleMaybeModUint32(input, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitWasmBuiltinTruncateDToInt32(
    LWasmBuiltinTruncateDToInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->getOperand(0));
  Register output = ToRegister(lir->getDef(0));

  OutOfLineTruncate* ool = new (alloc()) OutOfLineTruncate(lir);
  addOutOfLineCode(ool, lir->mir());

  masm.branchTruncateDoubleMaybeModUint32(input, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitTruncateFToInt32(LTruncateFToInt32* ins) {
  FloatRegister input = ToFloatRegister(ins->input());
  Register output = ToRegister(ins->output());

  OutOfLineTruncateFloat32* ool = new (alloc()) OutOfLineTruncateFloat32(ins);
  addOutOfLineCode(ool, ins->mir());

  masm.branchTruncateFloat32MaybeModUint32(input, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGenerator::visitWasmBuiltinTruncateFToInt32(
    LWasmBuiltinTruncateFToInt32* lir) {
  FloatRegister input = ToFloatRegister(lir->getOperand(0));
  Register output = ToRegister(lir->getDef(0));

  OutOfLineTruncateFloat32* ool = new (alloc()) OutOfLineTruncateFloat32(lir);
  addOutOfLineCode(ool, lir->mir());

  masm.branchTruncateFloat32MaybeModUint32(input, output, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGeneratorX86::visitOutOfLineTruncate(OutOfLineTruncate* ool) {
  FloatRegister input = ToFloatRegister(ool->input());
  Register output = ToRegister(ool->output());

  Label fail;

  if (Assembler::HasSSE3()) {
    Label failPopDouble;
    // Push double.
    masm.subl(Imm32(sizeof(double)), esp);
    masm.storeDouble(input, Operand(esp, 0));

    // Check exponent to avoid fp exceptions.
    masm.branchDoubleNotInInt64Range(Address(esp, 0), output, &failPopDouble);

    // Load double, perform 64-bit truncation.
    masm.truncateDoubleToInt64(Address(esp, 0), Address(esp, 0), output);

    // Load low word, pop double and jump back.
    masm.load32(Address(esp, 0), output);
    masm.addl(Imm32(sizeof(double)), esp);
    masm.jump(ool->rejoin());

    masm.bind(&failPopDouble);
    masm.addl(Imm32(sizeof(double)), esp);
    masm.jump(&fail);
  } else {
    FloatRegister temp = ToFloatRegister(ool->tempFloat());

    // Try to convert doubles representing integers within 2^32 of a signed
    // integer, by adding/subtracting 2^32 and then trying to convert to int32.
    // This has to be an exact conversion, as otherwise the truncation works
    // incorrectly on the modified value.
    {
      ScratchDoubleScope fpscratch(masm);
      masm.zeroDouble(fpscratch);
      masm.vucomisd(fpscratch, input);
      masm.j(Assembler::Parity, &fail);
    }

    {
      Label positive;
      masm.j(Assembler::Above, &positive);

      masm.loadConstantDouble(4294967296.0, temp);
      Label skip;
      masm.jmp(&skip);

      masm.bind(&positive);
      masm.loadConstantDouble(-4294967296.0, temp);
      masm.bind(&skip);
    }

    masm.addDouble(input, temp);
    masm.vcvttsd2si(temp, output);
    ScratchDoubleScope fpscratch(masm);
    masm.vcvtsi2sd(output, fpscratch, fpscratch);

    masm.vucomisd(fpscratch, temp);
    masm.j(Assembler::Parity, &fail);
    masm.j(Assembler::Equal, ool->rejoin());
  }

  masm.bind(&fail);
  {
    if (gen->compilingWasm()) {
      masm.Push(InstanceReg);
    }
    int32_t framePushedAfterInstance = masm.framePushed();

    saveVolatile(output);

    if (gen->compilingWasm()) {
      masm.setupWasmABICall();
      masm.passABIArg(input, ABIType::Float64);

      int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
      masm.callWithABI(ool->bytecodeOffset(), wasm::SymbolicAddress::ToInt32,
                       mozilla::Some(instanceOffset));
    } else {
      using Fn = int32_t (*)(double);
      masm.setupUnalignedABICall(output);
      masm.passABIArg(input, ABIType::Float64);
      masm.callWithABI<Fn, JS::ToInt32>(ABIType::General,
                                        CheckUnsafeCallWithABI::DontCheckOther);
    }
    masm.storeCallInt32Result(output);

    restoreVolatile(output);

    if (gen->compilingWasm()) {
      masm.Pop(InstanceReg);
    }
  }

  masm.jump(ool->rejoin());
}

void CodeGeneratorX86::visitOutOfLineTruncateFloat32(
    OutOfLineTruncateFloat32* ool) {
  FloatRegister input = ToFloatRegister(ool->input());
  Register output = ToRegister(ool->output());

  Label fail;

  if (Assembler::HasSSE3()) {
    Label failPopFloat;

    // Push float32, but subtracts 64 bits so that the value popped by fisttp
    // fits
    masm.subl(Imm32(sizeof(uint64_t)), esp);
    masm.storeFloat32(input, Operand(esp, 0));

    // Check exponent to avoid fp exceptions.
    masm.branchFloat32NotInInt64Range(Address(esp, 0), output, &failPopFloat);

    // Load float, perform 32-bit truncation.
    masm.truncateFloat32ToInt64(Address(esp, 0), Address(esp, 0), output);

    // Load low word, pop 64bits and jump back.
    masm.load32(Address(esp, 0), output);
    masm.addl(Imm32(sizeof(uint64_t)), esp);
    masm.jump(ool->rejoin());

    masm.bind(&failPopFloat);
    masm.addl(Imm32(sizeof(uint64_t)), esp);
    masm.jump(&fail);
  } else {
    FloatRegister temp = ToFloatRegister(ool->tempFloat());

    // Try to convert float32 representing integers within 2^32 of a signed
    // integer, by adding/subtracting 2^32 and then trying to convert to int32.
    // This has to be an exact conversion, as otherwise the truncation works
    // incorrectly on the modified value.
    {
      ScratchFloat32Scope fpscratch(masm);
      masm.zeroFloat32(fpscratch);
      masm.vucomiss(fpscratch, input);
      masm.j(Assembler::Parity, &fail);
    }

    {
      Label positive;
      masm.j(Assembler::Above, &positive);

      masm.loadConstantFloat32(4294967296.f, temp);
      Label skip;
      masm.jmp(&skip);

      masm.bind(&positive);
      masm.loadConstantFloat32(-4294967296.f, temp);
      masm.bind(&skip);
    }

    masm.addFloat32(input, temp);
    masm.vcvttss2si(temp, output);
    ScratchFloat32Scope fpscratch(masm);
    masm.vcvtsi2ss(output, fpscratch, fpscratch);

    masm.vucomiss(fpscratch, temp);
    masm.j(Assembler::Parity, &fail);
    masm.j(Assembler::Equal, ool->rejoin());
  }

  masm.bind(&fail);
  {
    if (gen->compilingWasm()) {
      masm.Push(InstanceReg);
    }
    int32_t framePushedAfterInstance = masm.framePushed();

    saveVolatile(output);

    masm.Push(input);

    if (gen->compilingWasm()) {
      masm.setupWasmABICall();
    } else {
      masm.setupUnalignedABICall(output);
    }

    masm.vcvtss2sd(input, input, input);
    masm.passABIArg(input.asDouble(), ABIType::Float64);

    if (gen->compilingWasm()) {
      int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
      masm.callWithABI(ool->bytecodeOffset(), wasm::SymbolicAddress::ToInt32,
                       mozilla::Some(instanceOffset));
    } else {
      using Fn = int32_t (*)(double);
      masm.callWithABI<Fn, JS::ToInt32>(ABIType::General,
                                        CheckUnsafeCallWithABI::DontCheckOther);
    }

    masm.storeCallInt32Result(output);
    masm.Pop(input);

    restoreVolatile(output);

    if (gen->compilingWasm()) {
      masm.Pop(InstanceReg);
    }
  }

  masm.jump(ool->rejoin());
}

void CodeGenerator::visitCompareI64(LCompareI64* lir) {
  MCompare* mir = lir->mir();
  MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
             mir->compareType() == MCompare::Compare_UInt64);

  const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
  Register64 lhsRegs = ToRegister64(lhs);
  Register output = ToRegister(lir->output());

  bool isSigned = mir->compareType() == MCompare::Compare_Int64;
  Assembler::Condition condition = JSOpToCondition(lir->jsop(), isSigned);
  Label done;

  masm.move32(Imm32(1), output);

  if (IsConstant(rhs)) {
    Imm64 imm = Imm64(ToInt64(rhs));
    masm.branch64(condition, lhsRegs, imm, &done);
  } else {
    Register64 rhsRegs = ToRegister64(rhs);
    masm.branch64(condition, lhsRegs, rhsRegs, &done);
  }

  masm.xorl(output, output);
  masm.bind(&done);
}

void CodeGenerator::visitCompareI64AndBranch(LCompareI64AndBranch* lir) {
  MCompare* mir = lir->cmpMir();
  MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
             mir->compareType() == MCompare::Compare_UInt64);

  const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
  const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
  Register64 lhsRegs = ToRegister64(lhs);

  bool isSigned = mir->compareType() == MCompare::Compare_Int64;
  Assembler::Condition condition = JSOpToCondition(lir->jsop(), isSigned);

  Label* trueLabel = getJumpLabelForBranch(lir->ifTrue());
  Label* falseLabel = getJumpLabelForBranch(lir->ifFalse());

  if (isNextBlock(lir->ifFalse()->lir())) {
    falseLabel = nullptr;
  } else if (isNextBlock(lir->ifTrue()->lir())) {
    condition = Assembler::InvertCondition(condition);
    trueLabel = falseLabel;
    falseLabel = nullptr;
  }

  if (IsConstant(rhs)) {
    Imm64 imm = Imm64(ToInt64(rhs));
    masm.branch64(condition, lhsRegs, imm, trueLabel, falseLabel);
  } else {
    Register64 rhsRegs = ToRegister64(rhs);
    masm.branch64(condition, lhsRegs, rhsRegs, trueLabel, falseLabel);
  }
}

void CodeGenerator::visitDivOrModI64(LDivOrModI64* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  MOZ_ASSERT(ToRegister(lir->getOperand(LDivOrModI64::Instance)) ==
             InstanceReg);

  masm.Push(InstanceReg);
  int32_t framePushedAfterInstance = masm.framePushed();

  Register64 lhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Lhs));
  Register64 rhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Rhs));
  Register64 output = ToOutRegister64(lir);

  MOZ_ASSERT(output == ReturnReg64);

  Label done;

  // Handle divide by zero.
  if (lir->canBeDivideByZero()) {
    Label nonZero;
    // We can use InstanceReg as temp register because we preserved it
    // before.
    masm.branchTest64(Assembler::NonZero, rhs, rhs, InstanceReg, &nonZero);
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
    masm.bind(&nonZero);
  }

  MDefinition* mir = lir->mir();

  // Handle an integer overflow exception from INT64_MIN / -1.
  if (lir->canBeNegativeOverflow()) {
    Label notOverflow;
    masm.branch64(Assembler::NotEqual, lhs, Imm64(INT64_MIN), &notOverflow);
    masm.branch64(Assembler::NotEqual, rhs, Imm64(-1), &notOverflow);
    if (mir->isWasmBuiltinModI64()) {
      masm.xor64(output, output);
    } else {
      masm.wasmTrap(wasm::Trap::IntegerOverflow, lir->bytecodeOffset());
    }
    masm.jump(&done);
    masm.bind(&notOverflow);
  }

  masm.setupWasmABICall();
  masm.passABIArg(lhs.high);
  masm.passABIArg(lhs.low);
  masm.passABIArg(rhs.high);
  masm.passABIArg(rhs.low);

  int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
  if (mir->isWasmBuiltinModI64()) {
    masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::ModI64,
                     mozilla::Some(instanceOffset));
  } else {
    masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::DivI64,
                     mozilla::Some(instanceOffset));
  }

  // output in edx:eax, move to output register.
  masm.movl(edx, output.high);
  MOZ_ASSERT(eax == output.low);

  masm.bind(&done);
  masm.Pop(InstanceReg);
}

void CodeGenerator::visitUDivOrModI64(LUDivOrModI64* lir) {
  MOZ_ASSERT(gen->compilingWasm());
  MOZ_ASSERT(ToRegister(lir->getOperand(LDivOrModI64::Instance)) ==
             InstanceReg);

  masm.Push(InstanceReg);
  int32_t framePushedAfterInstance = masm.framePushed();

  Register64 lhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Lhs));
  Register64 rhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Rhs));
  Register64 output = ToOutRegister64(lir);

  MOZ_ASSERT(output == ReturnReg64);

  // Prevent divide by zero.
  if (lir->canBeDivideByZero()) {
    Label nonZero;
    // We can use InstanceReg as temp register because we preserved it
    // before.
    masm.branchTest64(Assembler::NonZero, rhs, rhs, InstanceReg, &nonZero);
    masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
    masm.bind(&nonZero);
  }

  masm.setupWasmABICall();
  masm.passABIArg(lhs.high);
  masm.passABIArg(lhs.low);
  masm.passABIArg(rhs.high);
  masm.passABIArg(rhs.low);

  MDefinition* mir = lir->mir();
  int32_t instanceOffset = masm.framePushed() - framePushedAfterInstance;
  if (mir->isWasmBuiltinModI64()) {
    masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::UModI64,
                     mozilla::Some(instanceOffset));
  } else {
    masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::UDivI64,
                     mozilla::Some(instanceOffset));
  }

  // output in edx:eax, move to output register.
  masm.movl(edx, output.high);
  MOZ_ASSERT(eax == output.low);

  masm.Pop(InstanceReg);
}

void CodeGeneratorX86::emitBigIntDiv(LBigIntDiv* ins, Register dividend,
                                     Register divisor, Register output,
                                     Label* fail) {
  // Callers handle division by zero and integer overflow.

  MOZ_ASSERT(dividend == eax);
  MOZ_ASSERT(output == edx);

  // Sign extend the lhs into rdx to make rdx:rax.
  masm.cdq();

  masm.idiv(divisor);

  // Create and return the result.
  masm.newGCBigInt(output, divisor, initialBigIntHeap(), fail);
  masm.initializeBigInt(output, dividend);
}

void CodeGeneratorX86::emitBigIntMod(LBigIntMod* ins, Register dividend,
                                     Register divisor, Register output,
                                     Label* fail) {
  // Callers handle division by zero and integer overflow.

  MOZ_ASSERT(dividend == eax);
  MOZ_ASSERT(output == edx);

  // Sign extend the lhs into rdx to make edx:eax.
  masm.cdq();

  masm.idiv(divisor);

  // Move the remainder from edx.
  masm.movl(output, dividend);

  // Create and return the result.
  masm.newGCBigInt(output, divisor, initialBigIntHeap(), fail);
  masm.initializeBigInt(output, dividend);
}

void CodeGenerator::visitWasmSelectI64(LWasmSelectI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);

  Register cond = ToRegister(lir->condExpr());
  Register64 falseExpr = ToRegister64(lir->falseExpr());
  Register64 out = ToOutRegister64(lir);

  MOZ_ASSERT(ToRegister64(lir->trueExpr()) == out,
             "true expr is reused for input");

  Label done;
  masm.branchTest32(Assembler::NonZero, cond, cond, &done);
  masm.movl(falseExpr.low, out.low);
  masm.movl(falseExpr.high, out.high);
  masm.bind(&done);
}

// We expect to handle only the case where compare is {U,}Int32 and select is
// {U,}Int32.  Some values may be stack allocated, and the "true" input is
// reused for the output.
void CodeGenerator::visitWasmCompareAndSelect(LWasmCompareAndSelect* ins) {
  bool cmpIs32bit = ins->compareType() == MCompare::Compare_Int32 ||
                    ins->compareType() == MCompare::Compare_UInt32;
  bool selIs32bit = ins->mir()->type() == MIRType::Int32;

  MOZ_RELEASE_ASSERT(
      cmpIs32bit && selIs32bit,
      "CodeGenerator::visitWasmCompareAndSelect: unexpected types");

  Register trueExprAndDest = ToRegister(ins->output());
  MOZ_ASSERT(ToRegister(ins->ifTrueExpr()) == trueExprAndDest,
             "true expr input is reused for output");

  Assembler::Condition cond = Assembler::InvertCondition(
      JSOpToCondition(ins->compareType(), ins->jsop()));
  const LAllocation* rhs = ins->rightExpr();
  const LAllocation* falseExpr = ins->ifFalseExpr();
  Register lhs = ToRegister(ins->leftExpr());

  if (rhs->isRegister()) {
    if (falseExpr->isRegister()) {
      masm.cmp32Move32(cond, lhs, ToRegister(rhs), ToRegister(falseExpr),
                       trueExprAndDest);
    } else {
      masm.cmp32Load32(cond, lhs, ToRegister(rhs), ToAddress(falseExpr),
                       trueExprAndDest);
    }
  } else {
    if (falseExpr->isRegister()) {
      masm.cmp32Move32(cond, lhs, ToAddress(rhs), ToRegister(falseExpr),
                       trueExprAndDest);
    } else {
      masm.cmp32Load32(cond, lhs, ToAddress(rhs), ToAddress(falseExpr),
                       trueExprAndDest);
    }
  }
}

void CodeGenerator::visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Double);
  MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Int64);
  Register64 input = ToRegister64(lir->getInt64Operand(0));

  masm.Push(input.high);
  masm.Push(input.low);
  masm.vmovq(Operand(esp, 0), ToFloatRegister(lir->output()));
  masm.freeStack(sizeof(uint64_t));
}

void CodeGenerator::visitWasmReinterpretToI64(LWasmReinterpretToI64* lir) {
  MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);
  MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Double);
  Register64 output = ToOutRegister64(lir);

  masm.reserveStack(sizeof(uint64_t));
  masm.vmovq(ToFloatRegister(lir->input()), Operand(esp, 0));
  masm.Pop(output.low);
  masm.Pop(output.high);
}

void CodeGenerator::visitExtendInt32ToInt64(LExtendInt32ToInt64* lir) {
  Register64 output = ToOutRegister64(lir);
  Register input = ToRegister(lir->input());

  if (lir->mir()->isUnsigned()) {
    if (output.low != input) {
      masm.movl(input, output.low);
    }
    masm.xorl(output.high, output.high);
  } else {
    MOZ_ASSERT(output.low == input);
    MOZ_ASSERT(output.low == eax);
    MOZ_ASSERT(output.high == edx);
    masm.cdq();
  }
}

void CodeGenerator::visitSignExtendInt64(LSignExtendInt64* lir) {
#ifdef DEBUG
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);
  MOZ_ASSERT(input.low == eax);
  MOZ_ASSERT(output.low == eax);
  MOZ_ASSERT(input.high == edx);
  MOZ_ASSERT(output.high == edx);
#endif
  switch (lir->mode()) {
    case MSignExtendInt64::Byte:
      masm.move8SignExtend(eax, eax);
      break;
    case MSignExtendInt64::Half:
      masm.move16SignExtend(eax, eax);
      break;
    case MSignExtendInt64::Word:
      break;
  }
  masm.cdq();
}

void CodeGenerator::visitWrapInt64ToInt32(LWrapInt64ToInt32* lir) {
  const LInt64Allocation& input = lir->getInt64Operand(0);
  Register output = ToRegister(lir->output());

  if (lir->mir()->bottomHalf()) {
    masm.movl(ToRegister(input.low()), output);
  } else {
    masm.movl(ToRegister(input.high()), output);
  }
}

void CodeGenerator::visitWasmExtendU32Index(LWasmExtendU32Index*) {
  MOZ_CRASH("64-bit only");
}

void CodeGenerator::visitWasmWrapU32Index(LWasmWrapU32Index* lir) {
  // Generates no code on this platform because we just return the low part of
  // the input register pair.
  MOZ_ASSERT(ToRegister(lir->input()) == ToRegister(lir->output()));
}

void CodeGenerator::visitClzI64(LClzI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);

  masm.clz64(input, output.low);
  masm.xorl(output.high, output.high);
}

void CodeGenerator::visitCtzI64(LCtzI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register64 output = ToOutRegister64(lir);

  masm.ctz64(input, output.low);
  masm.xorl(output.high, output.high);
}

void CodeGenerator::visitNotI64(LNotI64* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  Register output = ToRegister(lir->output());

  if (input.high == output) {
    masm.orl(input.low, output);
  } else if (input.low == output) {
    masm.orl(input.high, output);
  } else {
    masm.movl(input.high, output);
    masm.orl(input.low, output);
  }

  masm.cmpl(Imm32(0), output);
  masm.emitSet(Assembler::Equal, output);
}

void CodeGenerator::visitWasmTruncateToInt64(LWasmTruncateToInt64* lir) {
  FloatRegister input = ToFloatRegister(lir->input());
  Register64 output = ToOutRegister64(lir);

  MWasmTruncateToInt64* mir = lir->mir();
  FloatRegister floatTemp = ToFloatRegister(lir->temp());

  Label fail, convert;

  MOZ_ASSERT(mir->input()->type() == MIRType::Double ||
             mir->input()->type() == MIRType::Float32);

  auto* ool = new (alloc()) OutOfLineWasmTruncateCheck(mir, input, output);
  addOutOfLineCode(ool, mir);

  bool isSaturating = mir->isSaturating();
  if (mir->input()->type() == MIRType::Float32) {
    if (mir->isUnsigned()) {
      masm.wasmTruncateFloat32ToUInt64(input, output, isSaturating,
                                       ool->entry(), ool->rejoin(), floatTemp);
    } else {
      masm.wasmTruncateFloat32ToInt64(input, output, isSaturating, ool->entry(),
                                      ool->rejoin(), floatTemp);
    }
  } else {
    if (mir->isUnsigned()) {
      masm.wasmTruncateDoubleToUInt64(input, output, isSaturating, ool->entry(),
                                      ool->rejoin(), floatTemp);
    } else {
      masm.wasmTruncateDoubleToInt64(input, output, isSaturating, ool->entry(),
                                     ool->rejoin(), floatTemp);
    }
  }
}

void CodeGenerator::visitInt64ToFloatingPoint(LInt64ToFloatingPoint* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));
  FloatRegister output = ToFloatRegister(lir->output());
  Register temp =
      lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

  MIRType outputType = lir->mir()->type();
  MOZ_ASSERT(outputType == MIRType::Double || outputType == MIRType::Float32);

  if (outputType == MIRType::Double) {
    if (lir->mir()->isUnsigned()) {
      masm.convertUInt64ToDouble(input, output, temp);
    } else {
      masm.convertInt64ToDouble(input, output);
    }
  } else {
    if (lir->mir()->isUnsigned()) {
      masm.convertUInt64ToFloat32(input, output, temp);
    } else {
      masm.convertInt64ToFloat32(input, output);
    }
  }
}

void CodeGenerator::visitBitNotI64(LBitNotI64* ins) {
  const LInt64Allocation input = ins->getInt64Operand(0);
  Register64 inputR = ToRegister64(input);
  MOZ_ASSERT(inputR == ToOutRegister64(ins));
  masm.notl(inputR.high);
  masm.notl(inputR.low);
}

void CodeGenerator::visitTestI64AndBranch(LTestI64AndBranch* lir) {
  Register64 input = ToRegister64(lir->getInt64Operand(0));

  masm.testl(input.high, input.high);
  jumpToBlock(lir->ifTrue(), Assembler::NonZero);
  masm.testl(input.low, input.low);
  emitBranch(Assembler::NonZero, lir->ifTrue(), lir->ifFalse());
}

void CodeGenerator::visitBitAndAndBranch(LBitAndAndBranch* baab) {
  // LBitAndAndBranch only represents single-word ANDs, hence it can't be
  // 64-bit here.
  MOZ_ASSERT(!baab->is64());
  Register regL = ToRegister(baab->left());
  if (baab->right()->isConstant()) {
    masm.test32(regL, Imm32(ToInt32(baab->right())));
  } else {
    masm.test32(regL, ToRegister(baab->right()));
  }
  emitBranch(baab->cond(), baab->ifTrue(), baab->ifFalse());
}
