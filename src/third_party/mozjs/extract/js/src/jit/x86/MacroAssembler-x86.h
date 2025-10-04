/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_MacroAssembler_x86_h
#define jit_x86_MacroAssembler_x86_h

#include "jit/JitOptions.h"
#include "jit/MoveResolver.h"
#include "jit/x86-shared/MacroAssembler-x86-shared.h"
#include "js/HeapAPI.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCodegenTypes.h"

using js::wasm::FaultingCodeOffsetPair;

namespace js {
namespace jit {

// See documentation for ScratchTagScope and ScratchTagScopeRelease in
// MacroAssembler-x64.h.

class ScratchTagScope {
  const ValueOperand& v_;

 public:
  ScratchTagScope(MacroAssembler&, const ValueOperand& v) : v_(v) {}
  operator Register() { return v_.typeReg(); }
  void release() {}
  void reacquire() {}
};

class ScratchTagScopeRelease {
 public:
  explicit ScratchTagScopeRelease(ScratchTagScope*) {}
};

class MacroAssemblerX86 : public MacroAssemblerX86Shared {
 private:
  // Perform a downcast. Should be removed by Bug 996602.
  MacroAssembler& asMasm();
  const MacroAssembler& asMasm() const;

 protected:
  MoveResolver moveResolver_;

 private:
  Operand payloadOfAfterStackPush(const Address& address) {
    // If we are basing off %esp, the address will be invalid after the
    // first push.
    if (address.base == StackPointer) {
      return Operand(address.base, address.offset + 4);
    }
    return payloadOf(address);
  }
  Operand payloadOfAfterStackPush(const BaseIndex& address) {
    // If we are basing off %esp, the address will be invalid after the
    // first push.
    if (address.base == StackPointer) {
      return Operand(address.base, address.index, address.scale,
                     address.offset + 4);
    }
    return payloadOf(address);
  }
  Operand payloadOf(const Address& address) {
    return Operand(address.base, address.offset);
  }
  Operand payloadOf(const BaseIndex& address) {
    return Operand(address.base, address.index, address.scale, address.offset);
  }
  Operand tagOf(const Address& address) {
    return Operand(address.base, address.offset + 4);
  }
  Operand tagOf(const BaseIndex& address) {
    return Operand(address.base, address.index, address.scale,
                   address.offset + 4);
  }

  void setupABICall(uint32_t args);

  void vpPatchOpSimd128(const SimdConstant& v, FloatRegister reg,
                        void (X86Encoding::BaseAssemblerX86::*op)(
                            const void* address,
                            X86Encoding::XMMRegisterID srcId,
                            X86Encoding::XMMRegisterID destId)) {
    vpPatchOpSimd128(v, reg, reg, op);
  }

  void vpPatchOpSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest,
                        void (X86Encoding::BaseAssemblerX86::*op)(
                            const void* address,
                            X86Encoding::XMMRegisterID srcId,
                            X86Encoding::XMMRegisterID destId));

  void vpPatchOpSimd128(const SimdConstant& v, FloatRegister reg,
                        size_t (X86Encoding::BaseAssemblerX86::*op)(
                            const void* address,
                            X86Encoding::XMMRegisterID srcId,
                            X86Encoding::XMMRegisterID destId)) {
    vpPatchOpSimd128(v, reg, reg, op);
  }

  void vpPatchOpSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest,
                        size_t (X86Encoding::BaseAssemblerX86::*op)(
                            const void* address,
                            X86Encoding::XMMRegisterID srcId,
                            X86Encoding::XMMRegisterID destId));

 public:
  using MacroAssemblerX86Shared::call;
  using MacroAssemblerX86Shared::load32;
  using MacroAssemblerX86Shared::store16;
  using MacroAssemblerX86Shared::store32;

  MacroAssemblerX86() {}

  // The buffer is about to be linked, make sure any constant pools or excess
  // bookkeeping has been flushed to the instruction stream.
  void finish();

  /////////////////////////////////////////////////////////////////
  // X86-specific interface.
  /////////////////////////////////////////////////////////////////

  Operand ToPayload(Operand base) { return base; }
  Address ToPayload(Address base) { return base; }
  BaseIndex ToPayload(BaseIndex base) { return base; }
  Operand ToType(Operand base) {
    switch (base.kind()) {
      case Operand::MEM_REG_DISP:
        return Operand(Register::FromCode(base.base()),
                       base.disp() + sizeof(void*));

      case Operand::MEM_SCALE:
        return Operand(Register::FromCode(base.base()),
                       Register::FromCode(base.index()), base.scale(),
                       base.disp() + sizeof(void*));

      default:
        MOZ_CRASH("unexpected operand kind");
    }
  }
  Address ToType(Address base) { return ToType(Operand(base)).toAddress(); }
  BaseIndex ToType(BaseIndex base) {
    return ToType(Operand(base)).toBaseIndex();
  }

  template <typename T>
  void add64FromMemory(const T& address, Register64 dest) {
    addl(Operand(LowWord(address)), dest.low);
    adcl(Operand(HighWord(address)), dest.high);
  }
  template <typename T>
  void sub64FromMemory(const T& address, Register64 dest) {
    subl(Operand(LowWord(address)), dest.low);
    sbbl(Operand(HighWord(address)), dest.high);
  }
  template <typename T>
  void and64FromMemory(const T& address, Register64 dest) {
    andl(Operand(LowWord(address)), dest.low);
    andl(Operand(HighWord(address)), dest.high);
  }
  template <typename T>
  void or64FromMemory(const T& address, Register64 dest) {
    orl(Operand(LowWord(address)), dest.low);
    orl(Operand(HighWord(address)), dest.high);
  }
  template <typename T>
  void xor64FromMemory(const T& address, Register64 dest) {
    xorl(Operand(LowWord(address)), dest.low);
    xorl(Operand(HighWord(address)), dest.high);
  }

  /////////////////////////////////////////////////////////////////
  // X86/X64-common interface.
  /////////////////////////////////////////////////////////////////
  void storeValue(ValueOperand val, Operand dest) {
    movl(val.payloadReg(), ToPayload(dest));
    movl(val.typeReg(), ToType(dest));
  }
  void storeValue(ValueOperand val, const Address& dest) {
    storeValue(val, Operand(dest));
  }
  template <typename T>
  void storeValue(JSValueType type, Register reg, const T& dest) {
    storeTypeTag(ImmTag(JSVAL_TYPE_TO_TAG(type)), Operand(dest));
    storePayload(reg, Operand(dest));
  }
  template <typename T>
  void storeValue(const Value& val, const T& dest) {
    storeTypeTag(ImmTag(val.toNunboxTag()), Operand(dest));
    storePayload(val, Operand(dest));
  }
  void storeValue(ValueOperand val, BaseIndex dest) {
    storeValue(val, Operand(dest));
  }
  void storeValue(const Address& src, const Address& dest, Register temp) {
    MOZ_ASSERT(src.base != temp);
    MOZ_ASSERT(dest.base != temp);

    load32(ToType(src), temp);
    store32(temp, ToType(dest));

    load32(ToPayload(src), temp);
    store32(temp, ToPayload(dest));
  }
  void storePrivateValue(Register src, const Address& dest) {
    store32(Imm32(0), ToType(dest));
    store32(src, ToPayload(dest));
  }
  void storePrivateValue(ImmGCPtr imm, const Address& dest) {
    store32(Imm32(0), ToType(dest));
    movl(imm, Operand(ToPayload(dest)));
  }
  void loadValue(Operand src, ValueOperand val) {
    Operand payload = ToPayload(src);
    Operand type = ToType(src);

    // Ensure that loading the payload does not erase the pointer to the
    // Value in memory or the index.
    Register baseReg = Register::FromCode(src.base());
    Register indexReg = (src.kind() == Operand::MEM_SCALE)
                            ? Register::FromCode(src.index())
                            : InvalidReg;

    // If we have a BaseIndex that uses both result registers, first compute
    // the address and then load the Value from there.
    if ((baseReg == val.payloadReg() && indexReg == val.typeReg()) ||
        (baseReg == val.typeReg() && indexReg == val.payloadReg())) {
      computeEffectiveAddress(src, val.scratchReg());
      loadValue(Address(val.scratchReg(), 0), val);
      return;
    }

    if (baseReg == val.payloadReg() || indexReg == val.payloadReg()) {
      MOZ_ASSERT(baseReg != val.typeReg());
      MOZ_ASSERT(indexReg != val.typeReg());

      movl(type, val.typeReg());
      movl(payload, val.payloadReg());
    } else {
      MOZ_ASSERT(baseReg != val.payloadReg());
      MOZ_ASSERT(indexReg != val.payloadReg());

      movl(payload, val.payloadReg());
      movl(type, val.typeReg());
    }
  }
  void loadValue(Address src, ValueOperand val) {
    loadValue(Operand(src), val);
  }
  void loadValue(const BaseIndex& src, ValueOperand val) {
    loadValue(Operand(src), val);
  }
  void loadUnalignedValue(const Address& src, ValueOperand dest) {
    loadValue(src, dest);
  }
  void tagValue(JSValueType type, Register payload, ValueOperand dest) {
    MOZ_ASSERT(dest.typeReg() != dest.payloadReg());
    if (payload != dest.payloadReg()) {
      movl(payload, dest.payloadReg());
    }
    movl(ImmType(type), dest.typeReg());
  }
  void pushValue(ValueOperand val) {
    push(val.typeReg());
    push(val.payloadReg());
  }
  void popValue(ValueOperand val) {
    pop(val.payloadReg());
    pop(val.typeReg());
  }
  void pushValue(const Value& val) {
    push(Imm32(val.toNunboxTag()));
    if (val.isGCThing()) {
      push(ImmGCPtr(val.toGCThing()));
    } else {
      push(Imm32(val.toNunboxPayload()));
    }
  }
  void pushValue(JSValueType type, Register reg) {
    push(ImmTag(JSVAL_TYPE_TO_TAG(type)));
    push(reg);
  }
  void pushValue(const Address& addr) {
    push(tagOf(addr));
    push(payloadOfAfterStackPush(addr));
  }
  void pushValue(const BaseIndex& addr, Register scratch) {
    push(tagOf(addr));
    push(payloadOfAfterStackPush(addr));
  }
  void push64(Register64 src) {
    push(src.high);
    push(src.low);
  }
  void pop64(Register64 dest) {
    pop(dest.low);
    pop(dest.high);
  }
  void storePayload(const Value& val, Operand dest) {
    if (val.isGCThing()) {
      movl(ImmGCPtr(val.toGCThing()), ToPayload(dest));
    } else {
      movl(Imm32(val.toNunboxPayload()), ToPayload(dest));
    }
  }
  void storePayload(Register src, Operand dest) { movl(src, ToPayload(dest)); }
  void storeTypeTag(ImmTag tag, Operand dest) { movl(tag, ToType(dest)); }

  void movePtr(Register src, Register dest) { movl(src, dest); }
  void movePtr(Register src, const Operand& dest) { movl(src, dest); }

  void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag) {
    MOZ_ASSERT(value.typeReg() == tag);
  }

  Condition testUndefined(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_UNDEFINED));
    return cond;
  }
  Condition testBoolean(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_BOOLEAN));
    return cond;
  }
  Condition testInt32(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_INT32));
    return cond;
  }
  Condition testDouble(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Condition actual = (cond == Equal) ? Below : AboveOrEqual;
    cmp32(tag, ImmTag(JSVAL_TAG_CLEAR));
    return actual;
  }
  Condition testNull(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_NULL));
    return cond;
  }
  Condition testString(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_STRING));
    return cond;
  }
  Condition testSymbol(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_SYMBOL));
    return cond;
  }
  Condition testBigInt(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_BIGINT));
    return cond;
  }
  Condition testObject(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_OBJECT));
    return cond;
  }
  Condition testNumber(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JS::detail::ValueUpperInclNumberTag));
    return cond == Equal ? BelowOrEqual : Above;
  }
  Condition testGCThing(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JS::detail::ValueLowerInclGCThingTag));
    return cond == Equal ? AboveOrEqual : Below;
  }
  Condition testGCThing(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JS::detail::ValueLowerInclGCThingTag));
    return cond == Equal ? AboveOrEqual : Below;
  }
  Condition testMagic(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_MAGIC));
    return cond;
  }
  Condition testMagic(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
  }
  Condition testMagic(Condition cond, const Operand& operand) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(ToType(operand), ImmTag(JSVAL_TAG_MAGIC));
    return cond;
  }
  Condition testPrimitive(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JS::detail::ValueUpperExclPrimitiveTag));
    return cond == Equal ? Below : AboveOrEqual;
  }
  Condition testError(Condition cond, Register tag) {
    return testMagic(cond, tag);
  }
  Condition testBoolean(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(Operand(ToType(address)), ImmTag(JSVAL_TAG_BOOLEAN));
    return cond;
  }
  Condition testInt32(Condition cond, const Operand& operand) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(ToType(operand), ImmTag(JSVAL_TAG_INT32));
    return cond;
  }
  Condition testInt32(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    return testInt32(cond, Operand(address));
  }
  Condition testObject(Condition cond, const Operand& operand) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(ToType(operand), ImmTag(JSVAL_TAG_OBJECT));
    return cond;
  }
  Condition testObject(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    return testObject(cond, Operand(address));
  }
  Condition testDouble(Condition cond, const Operand& operand) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    Condition actual = (cond == Equal) ? Below : AboveOrEqual;
    cmp32(ToType(operand), ImmTag(JSVAL_TAG_CLEAR));
    return actual;
  }
  Condition testDouble(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    return testDouble(cond, Operand(address));
  }

  Condition testUndefined(Condition cond, const Operand& operand) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(ToType(operand), ImmTag(JSVAL_TAG_UNDEFINED));
    return cond;
  }
  Condition testUndefined(Condition cond, const Address& addr) {
    return testUndefined(cond, Operand(addr));
  }
  Condition testNull(Condition cond, const Operand& operand) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(ToType(operand), ImmTag(JSVAL_TAG_NULL));
    return cond;
  }
  Condition testNull(Condition cond, const Address& addr) {
    return testNull(cond, Operand(addr));
  }

  Condition testUndefined(Condition cond, const ValueOperand& value) {
    return testUndefined(cond, value.typeReg());
  }
  Condition testBoolean(Condition cond, const ValueOperand& value) {
    return testBoolean(cond, value.typeReg());
  }
  Condition testInt32(Condition cond, const ValueOperand& value) {
    return testInt32(cond, value.typeReg());
  }
  Condition testDouble(Condition cond, const ValueOperand& value) {
    return testDouble(cond, value.typeReg());
  }
  Condition testNull(Condition cond, const ValueOperand& value) {
    return testNull(cond, value.typeReg());
  }
  Condition testString(Condition cond, const ValueOperand& value) {
    return testString(cond, value.typeReg());
  }
  Condition testSymbol(Condition cond, const ValueOperand& value) {
    return testSymbol(cond, value.typeReg());
  }
  Condition testBigInt(Condition cond, const ValueOperand& value) {
    return testBigInt(cond, value.typeReg());
  }
  Condition testObject(Condition cond, const ValueOperand& value) {
    return testObject(cond, value.typeReg());
  }
  Condition testMagic(Condition cond, const ValueOperand& value) {
    return testMagic(cond, value.typeReg());
  }
  Condition testError(Condition cond, const ValueOperand& value) {
    return testMagic(cond, value);
  }
  Condition testNumber(Condition cond, const ValueOperand& value) {
    return testNumber(cond, value.typeReg());
  }
  Condition testGCThing(Condition cond, const ValueOperand& value) {
    return testGCThing(cond, value.typeReg());
  }
  Condition testPrimitive(Condition cond, const ValueOperand& value) {
    return testPrimitive(cond, value.typeReg());
  }

  Condition testUndefined(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_UNDEFINED));
    return cond;
  }
  Condition testNull(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_NULL));
    return cond;
  }
  Condition testBoolean(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_BOOLEAN));
    return cond;
  }
  Condition testString(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_STRING));
    return cond;
  }
  Condition testString(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_STRING));
    return cond;
  }
  Condition testSymbol(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_SYMBOL));
    return cond;
  }
  Condition testSymbol(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_SYMBOL));
    return cond;
  }
  Condition testBigInt(Condition cond, const Address& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_BIGINT));
    return cond;
  }
  Condition testBigInt(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_BIGINT));
    return cond;
  }
  Condition testInt32(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_INT32));
    return cond;
  }
  Condition testObject(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_OBJECT));
    return cond;
  }
  Condition testDouble(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    Condition actual = (cond == Equal) ? Below : AboveOrEqual;
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_CLEAR));
    return actual;
  }
  Condition testMagic(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JSVAL_TAG_MAGIC));
    return cond;
  }
  Condition testGCThing(Condition cond, const BaseIndex& address) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tagOf(address), ImmTag(JS::detail::ValueLowerInclGCThingTag));
    return cond == Equal ? AboveOrEqual : Below;
  }

  void testNullSet(Condition cond, const ValueOperand& value, Register dest) {
    cond = testNull(cond, value);
    emitSet(cond, dest);
  }

  void testObjectSet(Condition cond, const ValueOperand& value, Register dest) {
    cond = testObject(cond, value);
    emitSet(cond, dest);
  }

  void testUndefinedSet(Condition cond, const ValueOperand& value,
                        Register dest) {
    cond = testUndefined(cond, value);
    emitSet(cond, dest);
  }

  void cmpPtr(Register lhs, const ImmWord rhs) { cmpl(Imm32(rhs.value), lhs); }
  void cmpPtr(Register lhs, const ImmPtr imm) {
    cmpPtr(lhs, ImmWord(uintptr_t(imm.value)));
  }
  void cmpPtr(Register lhs, const ImmGCPtr rhs) { cmpl(rhs, lhs); }
  void cmpPtr(const Operand& lhs, Imm32 rhs) { cmp32(lhs, rhs); }
  void cmpPtr(const Operand& lhs, const ImmWord rhs) {
    cmp32(lhs, Imm32(rhs.value));
  }
  void cmpPtr(const Operand& lhs, const ImmPtr imm) {
    cmpPtr(lhs, ImmWord(uintptr_t(imm.value)));
  }
  void cmpPtr(const Operand& lhs, const ImmGCPtr rhs) { cmpl(rhs, lhs); }
  void cmpPtr(const Address& lhs, Register rhs) { cmpPtr(Operand(lhs), rhs); }
  void cmpPtr(const Operand& lhs, Register rhs) { cmp32(lhs, rhs); }
  void cmpPtr(const Address& lhs, const ImmWord rhs) {
    cmpPtr(Operand(lhs), rhs);
  }
  void cmpPtr(const Address& lhs, const ImmPtr rhs) {
    cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
  }
  void cmpPtr(const Address& lhs, const ImmGCPtr rhs) {
    cmpPtr(Operand(lhs), rhs);
  }
  void cmpPtr(Register lhs, Register rhs) { cmp32(lhs, rhs); }
  void testPtr(Register lhs, Register rhs) { test32(lhs, rhs); }
  void testPtr(Register lhs, Imm32 rhs) { test32(lhs, rhs); }
  void testPtr(Register lhs, ImmWord rhs) { test32(lhs, Imm32(rhs.value)); }
  void testPtr(const Operand& lhs, Imm32 rhs) { test32(lhs, rhs); }
  void testPtr(const Operand& lhs, ImmWord rhs) {
    test32(lhs, Imm32(rhs.value));
  }

  /////////////////////////////////////////////////////////////////
  // Common interface.
  /////////////////////////////////////////////////////////////////

  void movePtr(ImmWord imm, Register dest) { movl(Imm32(imm.value), dest); }
  void movePtr(ImmPtr imm, Register dest) { movl(imm, dest); }
  void movePtr(wasm::SymbolicAddress imm, Register dest) { mov(imm, dest); }
  void movePtr(ImmGCPtr imm, Register dest) { movl(imm, dest); }
  FaultingCodeOffset loadPtr(const Address& address, Register dest) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movl(Operand(address), dest);
    return fco;
  }
  void loadPtr(const Operand& src, Register dest) { movl(src, dest); }
  FaultingCodeOffset loadPtr(const BaseIndex& src, Register dest) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movl(Operand(src), dest);
    return fco;
  }
  void loadPtr(AbsoluteAddress address, Register dest) {
    movl(Operand(address), dest);
  }
  void loadPrivate(const Address& src, Register dest) {
    movl(payloadOf(src), dest);
  }
  void load32(AbsoluteAddress address, Register dest) {
    movl(Operand(address), dest);
  }
  FaultingCodeOffsetPair load64(const Address& address, Register64 dest) {
    FaultingCodeOffset fco1, fco2;
    bool highBeforeLow = address.base == dest.low;
    if (highBeforeLow) {
      fco1 = FaultingCodeOffset(currentOffset());
      movl(Operand(HighWord(address)), dest.high);
      fco2 = FaultingCodeOffset(currentOffset());
      movl(Operand(LowWord(address)), dest.low);
    } else {
      fco1 = FaultingCodeOffset(currentOffset());
      movl(Operand(LowWord(address)), dest.low);
      fco2 = FaultingCodeOffset(currentOffset());
      movl(Operand(HighWord(address)), dest.high);
    }
    return FaultingCodeOffsetPair(fco1, fco2);
  }
  FaultingCodeOffsetPair load64(const BaseIndex& address, Register64 dest) {
    // If you run into this, relax your register allocation constraints.
    MOZ_RELEASE_ASSERT(
        !((address.base == dest.low || address.base == dest.high) &&
          (address.index == dest.low || address.index == dest.high)));
    FaultingCodeOffset fco1, fco2;
    bool highBeforeLow = address.base == dest.low || address.index == dest.low;
    if (highBeforeLow) {
      fco1 = FaultingCodeOffset(currentOffset());
      movl(Operand(HighWord(address)), dest.high);
      fco2 = FaultingCodeOffset(currentOffset());
      movl(Operand(LowWord(address)), dest.low);
    } else {
      fco1 = FaultingCodeOffset(currentOffset());
      movl(Operand(LowWord(address)), dest.low);
      fco2 = FaultingCodeOffset(currentOffset());
      movl(Operand(HighWord(address)), dest.high);
    }
    return FaultingCodeOffsetPair(fco1, fco2);
  }
  template <typename T>
  void load64Unaligned(const T& address, Register64 dest) {
    load64(address, dest);
  }
  template <typename T>
  void storePtr(ImmWord imm, T address) {
    movl(Imm32(imm.value), Operand(address));
  }
  template <typename T>
  void storePtr(ImmPtr imm, T address) {
    storePtr(ImmWord(uintptr_t(imm.value)), address);
  }
  template <typename T>
  void storePtr(ImmGCPtr imm, T address) {
    movl(imm, Operand(address));
  }
  FaultingCodeOffset storePtr(Register src, const Address& address) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movl(src, Operand(address));
    return fco;
  }
  FaultingCodeOffset storePtr(Register src, const BaseIndex& address) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movl(src, Operand(address));
    return fco;
  }
  void storePtr(Register src, const Operand& dest) { movl(src, dest); }
  void storePtr(Register src, AbsoluteAddress address) {
    movl(src, Operand(address));
  }
  void store32(Register src, AbsoluteAddress address) {
    movl(src, Operand(address));
  }
  void store16(Register src, AbsoluteAddress address) {
    movw(src, Operand(address));
  }
  template <typename T>
  FaultingCodeOffsetPair store64(Register64 src, const T& address) {
    FaultingCodeOffset fco1 = FaultingCodeOffset(currentOffset());
    movl(src.low, Operand(LowWord(address)));
    FaultingCodeOffset fco2 = FaultingCodeOffset(currentOffset());
    movl(src.high, Operand(HighWord(address)));
    return FaultingCodeOffsetPair(fco1, fco2);
  }
  void store64(Imm64 imm, Address address) {
    movl(imm.low(), Operand(LowWord(address)));
    movl(imm.hi(), Operand(HighWord(address)));
  }
  template <typename S, typename T>
  void store64Unaligned(const S& src, const T& dest) {
    store64(src, dest);
  }

  void setStackArg(Register reg, uint32_t arg) {
    movl(reg, Operand(esp, arg * sizeof(intptr_t)));
  }

  void boxDouble(FloatRegister src, const ValueOperand& dest,
                 FloatRegister temp) {
    if (Assembler::HasSSE41()) {
      vmovd(src, dest.payloadReg());
      vpextrd(1, src, dest.typeReg());
    } else {
      vmovd(src, dest.payloadReg());
      if (src != temp) {
        moveDouble(src, temp);
      }
      vpsrldq(Imm32(4), temp, temp);
      vmovd(temp, dest.typeReg());
    }
  }
  void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest) {
    if (src != dest.payloadReg()) {
      movl(src, dest.payloadReg());
    }
    movl(ImmType(type), dest.typeReg());
  }

  void unboxNonDouble(const ValueOperand& src, Register dest, JSValueType type,
                      Register scratch = InvalidReg) {
    unboxNonDouble(Operand(src.typeReg()), Operand(src.payloadReg()), dest,
                   type, scratch);
  }
  void unboxNonDouble(const Operand& tag, const Operand& payload, Register dest,
                      JSValueType type, Register scratch = InvalidReg) {
    auto movPayloadToDest = [&]() {
      if (payload.kind() != Operand::REG || !payload.containsReg(dest)) {
        movl(payload, dest);
      }
    };
    if (!JitOptions.spectreValueMasking) {
      movPayloadToDest();
      return;
    }

    // Spectre mitigation: We zero the payload if the tag does not match the
    // expected type and if this is a pointer type.
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      movPayloadToDest();
      return;
    }

    if (!tag.containsReg(dest) && !payload.containsReg(dest)) {
      // We zero the destination register and move the payload into it if
      // the tag corresponds to the given type.
      xorl(dest, dest);
      cmpl(Imm32(JSVAL_TYPE_TO_TAG(type)), tag);
      cmovCCl(Condition::Equal, payload, dest);
      return;
    }

    if (scratch == InvalidReg || scratch == dest || tag.containsReg(scratch) ||
        payload.containsReg(scratch)) {
      // UnboxedLayout::makeConstructorCode calls extractObject with a
      // scratch register which aliases the tag register, thus we cannot
      // assert the above condition.
      scratch = InvalidReg;
    }

    // The destination register aliases one of the operands. We create a
    // zero value either in a scratch register or on the stack and use it
    // to reset the destination register after reading both the tag and the
    // payload.
    Operand zero(Address(esp, 0));
    if (scratch == InvalidReg) {
      push(Imm32(0));
    } else {
      xorl(scratch, scratch);
      zero = Operand(scratch);
    }
    cmpl(Imm32(JSVAL_TYPE_TO_TAG(type)), tag);
    movPayloadToDest();
    cmovCCl(Condition::NotEqual, zero, dest);
    if (scratch == InvalidReg) {
      addl(Imm32(sizeof(void*)), esp);
    }
  }
  void unboxNonDouble(const Address& src, Register dest, JSValueType type) {
    unboxNonDouble(tagOf(src), payloadOf(src), dest, type);
  }
  void unboxNonDouble(const BaseIndex& src, Register dest, JSValueType type) {
    unboxNonDouble(tagOf(src), payloadOf(src), dest, type);
  }
  void unboxInt32(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_INT32);
  }
  void unboxInt32(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_INT32);
  }
  void unboxInt32(const BaseIndex& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_INT32);
  }
  void unboxBoolean(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BOOLEAN);
  }
  void unboxBoolean(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BOOLEAN);
  }
  void unboxBoolean(const BaseIndex& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BOOLEAN);
  }
  void unboxString(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
  }
  void unboxString(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
  }
  void unboxSymbol(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
  }
  void unboxSymbol(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
  }
  void unboxBigInt(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
  }
  void unboxBigInt(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
  }
  void unboxObject(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
  }
  void unboxObject(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
  }
  void unboxObject(const BaseIndex& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
  }
  template <typename T>
  void unboxObjectOrNull(const T& src, Register dest) {
    // Due to Spectre mitigation logic (see Value.h), if the value is an Object
    // then this yields the object; otherwise it yields zero (null), as desired.
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
  }
  template <typename T>
  void unboxDouble(const T& src, FloatRegister dest) {
    loadDouble(Operand(src), dest);
  }
  void unboxDouble(const ValueOperand& src, FloatRegister dest) {
    if (Assembler::HasSSE41()) {
      vmovd(src.payloadReg(), dest);
      vpinsrd(1, src.typeReg(), dest, dest);
    } else {
      ScratchDoubleScope fpscratch(asMasm());
      vmovd(src.payloadReg(), dest);
      vmovd(src.typeReg(), fpscratch);
      vunpcklps(fpscratch, dest, dest);
    }
  }
  void unboxDouble(const Operand& payload, const Operand& type,
                   Register scratch, FloatRegister dest) {
    if (Assembler::HasSSE41()) {
      movl(payload, scratch);
      vmovd(scratch, dest);
      movl(type, scratch);
      vpinsrd(1, scratch, dest, dest);
    } else {
      ScratchDoubleScope fpscratch(asMasm());
      movl(payload, scratch);
      vmovd(scratch, dest);
      movl(type, scratch);
      vmovd(scratch, fpscratch);
      vunpcklps(fpscratch, dest, dest);
    }
  }
  inline void unboxValue(const ValueOperand& src, AnyRegister dest,
                         JSValueType type);

  // See comment in MacroAssembler-x64.h.
  void unboxGCThingForGCBarrier(const Address& src, Register dest) {
    movl(payloadOf(src), dest);
  }

  void unboxWasmAnyRefGCThingForGCBarrier(const Address& src, Register dest) {
    movl(ImmWord(wasm::AnyRef::GCThingMask), dest);
    andl(Operand(src), dest);
  }

  void getWasmAnyRefGCThingChunk(Register src, Register dest) {
    MOZ_ASSERT(src != dest);
    movl(ImmWord(wasm::AnyRef::GCThingChunkMask), dest);
    andl(src, dest);
  }

  void notBoolean(const ValueOperand& val) { xorl(Imm32(1), val.payloadReg()); }

  template <typename T>
  void fallibleUnboxPtrImpl(const T& src, Register dest, JSValueType type,
                            Label* fail);

  // Extended unboxing API. If the payload is already in a register, returns
  // that register. Otherwise, provides a move to the given scratch register,
  // and returns that.
  [[nodiscard]] Register extractObject(const Address& address, Register dest) {
    unboxObject(address, dest);
    return dest;
  }
  [[nodiscard]] Register extractObject(const ValueOperand& value,
                                       Register scratch) {
    unboxNonDouble(value, value.payloadReg(), JSVAL_TYPE_OBJECT, scratch);
    return value.payloadReg();
  }
  [[nodiscard]] Register extractSymbol(const ValueOperand& value,
                                       Register scratch) {
    unboxNonDouble(value, value.payloadReg(), JSVAL_TYPE_SYMBOL, scratch);
    return value.payloadReg();
  }
  [[nodiscard]] Register extractInt32(const ValueOperand& value,
                                      Register scratch) {
    return value.payloadReg();
  }
  [[nodiscard]] Register extractBoolean(const ValueOperand& value,
                                        Register scratch) {
    return value.payloadReg();
  }
  [[nodiscard]] Register extractTag(const Address& address, Register scratch) {
    movl(tagOf(address), scratch);
    return scratch;
  }
  [[nodiscard]] Register extractTag(const ValueOperand& value,
                                    Register scratch) {
    return value.typeReg();
  }

  void convertDoubleToPtr(FloatRegister src, Register dest, Label* fail,
                          bool negativeZeroCheck = true) {
    convertDoubleToInt32(src, dest, fail, negativeZeroCheck);
  }

  void boolValueToDouble(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToDouble(operand.payloadReg(), dest);
  }
  void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToFloat32(operand.payloadReg(), dest);
  }
  void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToDouble(operand.payloadReg(), dest);
  }
  void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToFloat32(operand.payloadReg(), dest);
  }

  void loadConstantDouble(double d, FloatRegister dest);
  void loadConstantFloat32(float f, FloatRegister dest);

  void loadConstantSimd128Int(const SimdConstant& v, FloatRegister dest);
  void loadConstantSimd128Float(const SimdConstant& v, FloatRegister dest);
  void vpaddbSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpaddwSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpadddSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpaddqSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpsubbSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpsubwSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpsubdSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpsubqSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpmullwSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpmulldSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpaddsbSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpaddusbSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpaddswSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpadduswSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpsubsbSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpsubusbSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpsubswSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpsubuswSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpminsbSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpminubSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpminswSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpminuwSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpminsdSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpminudSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpmaxsbSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpmaxubSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpmaxswSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpmaxuwSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpmaxsdSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpmaxudSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vpandSimd128(const SimdConstant& v, FloatRegister lhs,
                    FloatRegister dest);
  void vpxorSimd128(const SimdConstant& v, FloatRegister lhs,
                    FloatRegister dest);
  void vporSimd128(const SimdConstant& v, FloatRegister lhs,
                   FloatRegister dest);
  void vaddpsSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vaddpdSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vsubpsSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vsubpdSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vdivpsSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vdivpdSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vmulpsSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vmulpdSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vandpdSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vminpdSimd128(const SimdConstant& v, FloatRegister lhs,
                     FloatRegister dest);
  void vpacksswbSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest);
  void vpackuswbSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest);
  void vpackssdwSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest);
  void vpackusdwSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest);
  void vpunpckldqSimd128(const SimdConstant& v, FloatRegister lhs,
                         FloatRegister dest);
  void vunpcklpsSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest);
  void vpshufbSimd128(const SimdConstant& v, FloatRegister lhs,
                      FloatRegister dest);
  void vptestSimd128(const SimdConstant& v, FloatRegister lhs);
  void vpmaddwdSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpcmpeqbSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpcmpgtbSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpcmpeqwSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpcmpgtwSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpcmpeqdSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpcmpgtdSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vcmpeqpsSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vcmpneqpsSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest);
  void vcmpltpsSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vcmplepsSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vcmpgepsSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vcmpeqpdSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vcmpneqpdSimd128(const SimdConstant& v, FloatRegister lhs,
                        FloatRegister dest);
  void vcmpltpdSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vcmplepdSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);
  void vpmaddubswSimd128(const SimdConstant& v, FloatRegister lhs,
                         FloatRegister dest);
  void vpmuludqSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest);

  Condition testInt32Truthy(bool truthy, const ValueOperand& operand) {
    test32(operand.payloadReg(), operand.payloadReg());
    return truthy ? NonZero : Zero;
  }
  Condition testStringTruthy(bool truthy, const ValueOperand& value);
  Condition testBigIntTruthy(bool truthy, const ValueOperand& value);

  template <typename T>
  inline void loadInt32OrDouble(const T& src, FloatRegister dest);

  template <typename T>
  inline void loadUnboxedValue(const T& src, MIRType type, AnyRegister dest);

  template <typename T>
  void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes,
                           JSValueType) {
    switch (nbytes) {
      case 4:
        storePtr(value.payloadReg(), address);
        return;
      case 1:
        store8(value.payloadReg(), address);
        return;
      default:
        MOZ_CRASH("Bad payload width");
    }
  }

  // Note: this function clobbers the source register.
  inline void convertUInt32ToDouble(Register src, FloatRegister dest);

  // Note: this function clobbers the source register.
  inline void convertUInt32ToFloat32(Register src, FloatRegister dest);

  void incrementInt32Value(const Address& addr) {
    addl(Imm32(1), payloadOf(addr));
  }

  inline void ensureDouble(const ValueOperand& source, FloatRegister dest,
                           Label* failure);

 public:
  // Used from within an Exit frame to handle a pending exception.
  void handleFailureWithHandlerTail(Label* profilerExitTail,
                                    Label* bailoutTail);

  // Instrumentation for entering and leaving the profiler.
  void profilerEnterFrame(Register framePtr, Register scratch);
  void profilerExitFrame();
};

typedef MacroAssemblerX86 MacroAssemblerSpecific;

}  // namespace jit
}  // namespace js

#endif /* jit_x86_MacroAssembler_x86_h */
