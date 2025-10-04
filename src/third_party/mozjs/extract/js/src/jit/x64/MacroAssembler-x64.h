/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_MacroAssembler_x64_h
#define jit_x64_MacroAssembler_x64_h

#include "jit/x86-shared/MacroAssembler-x86-shared.h"
#include "js/HeapAPI.h"
#include "wasm/WasmBuiltins.h"

namespace js {
namespace jit {

struct ImmShiftedTag : public ImmWord {
  explicit ImmShiftedTag(JSValueShiftedTag shtag) : ImmWord((uintptr_t)shtag) {}

  explicit ImmShiftedTag(JSValueType type)
      : ImmWord(uintptr_t(JSVAL_TYPE_TO_SHIFTED_TAG(type))) {}
};

struct ImmTag : public Imm32 {
  explicit ImmTag(JSValueTag tag) : Imm32(tag) {}
};

// ScratchTagScope and ScratchTagScopeRelease are used to manage the tag
// register for splitTagForTest(), which has different register management on
// different platforms.  On 64-bit platforms it requires a scratch register that
// does not interfere with other operations; on 32-bit platforms it uses a
// register that is already part of the Value.
//
// The ScratchTagScope RAII type acquires the appropriate register; a reference
// to a variable of this type is then passed to splitTagForTest().
//
// On 64-bit platforms ScratchTagScopeRelease makes the owned scratch register
// available in a dynamic scope during compilation.  However it is important to
// remember that that does not preserve the register value in any way, so this
// RAII type should only be used along paths that eventually branch past further
// uses of the extracted tag value.
//
// On 32-bit platforms ScratchTagScopeRelease has no effect, since it does not
// manage a register, it only aliases a register in the ValueOperand.

class ScratchTagScope : public ScratchRegisterScope {
 public:
  ScratchTagScope(MacroAssembler& masm, const ValueOperand&)
      : ScratchRegisterScope(masm) {}
};

class ScratchTagScopeRelease {
  ScratchTagScope* ts_;

 public:
  explicit ScratchTagScopeRelease(ScratchTagScope* ts) : ts_(ts) {
    ts_->release();
  }
  ~ScratchTagScopeRelease() { ts_->reacquire(); }
};

class MacroAssemblerX64 : public MacroAssemblerX86Shared {
 private:
  // Perform a downcast. Should be removed by Bug 996602.
  MacroAssembler& asMasm();
  const MacroAssembler& asMasm() const;

  void bindOffsets(const MacroAssemblerX86Shared::UsesVector&);

  void vpRiprOpSimd128(const SimdConstant& v, FloatRegister reg,
                       JmpSrc (X86Encoding::BaseAssemblerX64::*op)(
                           X86Encoding::XMMRegisterID id));

  void vpRiprOpSimd128(const SimdConstant& v, FloatRegister lhs,
                       FloatRegister dest,
                       JmpSrc (X86Encoding::BaseAssemblerX64::*op)(
                           X86Encoding::XMMRegisterID srcId,
                           X86Encoding::XMMRegisterID destId));

 public:
  using MacroAssemblerX86Shared::load32;
  using MacroAssemblerX86Shared::store16;
  using MacroAssemblerX86Shared::store32;

  MacroAssemblerX64() = default;

  // The buffer is about to be linked, make sure any constant pools or excess
  // bookkeeping has been flushed to the instruction stream.
  void finish();

  /////////////////////////////////////////////////////////////////
  // X64 helpers.
  /////////////////////////////////////////////////////////////////
  void writeDataRelocation(const Value& val) {
    // Raw GC pointer relocations and Value relocations both end up in
    // Assembler::TraceDataRelocations.
    if (val.isGCThing()) {
      gc::Cell* cell = val.toGCThing();
      if (cell && gc::IsInsideNursery(cell)) {
        embedsNurseryPointers_ = true;
      }
      dataRelocations_.writeUnsigned(masm.currentOffset());
    }
  }

  // Refers to the upper 32 bits of a 64-bit Value operand.
  // On x86_64, the upper 32 bits do not necessarily only contain the type.
  Operand ToUpper32(Operand base) {
    switch (base.kind()) {
      case Operand::MEM_REG_DISP:
        return Operand(Register::FromCode(base.base()), base.disp() + 4);

      case Operand::MEM_SCALE:
        return Operand(Register::FromCode(base.base()),
                       Register::FromCode(base.index()), base.scale(),
                       base.disp() + 4);

      default:
        MOZ_CRASH("unexpected operand kind");
    }
  }
  static inline Operand ToUpper32(const Address& address) {
    return Operand(address.base, address.offset + 4);
  }
  static inline Operand ToUpper32(const BaseIndex& address) {
    return Operand(address.base, address.index, address.scale,
                   address.offset + 4);
  }

  uint32_t Upper32Of(JSValueShiftedTag tag) { return uint32_t(tag >> 32); }

  JSValueShiftedTag GetShiftedTag(JSValueType type) {
    return (JSValueShiftedTag)JSVAL_TYPE_TO_SHIFTED_TAG(type);
  }

  /////////////////////////////////////////////////////////////////
  // X86/X64-common interface.
  /////////////////////////////////////////////////////////////////

  void storeValue(ValueOperand val, Operand dest) {
    movq(val.valueReg(), dest);
  }
  void storeValue(ValueOperand val, const Address& dest) {
    storeValue(val, Operand(dest));
  }
  template <typename T>
  void storeValue(JSValueType type, Register reg, const T& dest) {
    // Value types with 32-bit payloads can be emitted as two 32-bit moves.
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      movl(reg, Operand(dest));
      movl(Imm32(Upper32Of(GetShiftedTag(type))), ToUpper32(Operand(dest)));
    } else {
      ScratchRegisterScope scratch(asMasm());
      boxValue(type, reg, scratch);
      movq(scratch, Operand(dest));
    }
  }
  template <typename T>
  void storeValue(const Value& val, const T& dest) {
    ScratchRegisterScope scratch(asMasm());
    if (val.isGCThing()) {
      movWithPatch(ImmWord(val.asRawBits()), scratch);
      writeDataRelocation(val);
    } else {
      mov(ImmWord(val.asRawBits()), scratch);
    }
    movq(scratch, Operand(dest));
  }
  void storeValue(ValueOperand val, BaseIndex dest) {
    storeValue(val, Operand(dest));
  }
  void storeValue(const Address& src, const Address& dest, Register temp) {
    loadPtr(src, temp);
    storePtr(temp, dest);
  }
  void storePrivateValue(Register src, const Address& dest) {
    storePtr(src, dest);
  }
  void storePrivateValue(ImmGCPtr imm, const Address& dest) {
    storePtr(imm, dest);
  }
  void loadValue(Operand src, ValueOperand val) { movq(src, val.valueReg()); }
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
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(dest.valueReg() != scratch);
    if (payload != dest.valueReg()) {
      movq(payload, dest.valueReg());
    }
    mov(ImmShiftedTag(type), scratch);
    orq(scratch, dest.valueReg());
  }
  void pushValue(ValueOperand val) { push(val.valueReg()); }
  void popValue(ValueOperand val) { pop(val.valueReg()); }
  void pushValue(const Value& val) {
    if (val.isGCThing()) {
      ScratchRegisterScope scratch(asMasm());
      movWithPatch(ImmWord(val.asRawBits()), scratch);
      writeDataRelocation(val);
      push(scratch);
    } else {
      push(ImmWord(val.asRawBits()));
    }
  }
  void pushValue(JSValueType type, Register reg) {
    ScratchRegisterScope scratch(asMasm());
    boxValue(type, reg, scratch);
    push(scratch);
  }
  void pushValue(const Address& addr) { push(Operand(addr)); }

  void pushValue(const BaseIndex& addr, Register scratch) {
    push(Operand(addr));
  }

  void boxValue(JSValueType type, Register src, Register dest);

  Condition testUndefined(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_UNDEFINED));
    return cond;
  }
  Condition testInt32(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_INT32));
    return cond;
  }
  Condition testBoolean(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_BOOLEAN));
    return cond;
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
  Condition testDouble(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, Imm32(JSVAL_TAG_MAX_DOUBLE));
    return cond == Equal ? BelowOrEqual : Above;
  }
  Condition testNumber(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, Imm32(JS::detail::ValueUpperInclNumberTag));
    return cond == Equal ? BelowOrEqual : Above;
  }
  Condition testGCThing(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, Imm32(JS::detail::ValueLowerInclGCThingTag));
    return cond == Equal ? AboveOrEqual : Below;
  }

  Condition testMagic(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JSVAL_TAG_MAGIC));
    return cond;
  }
  Condition testError(Condition cond, Register tag) {
    return testMagic(cond, tag);
  }
  Condition testPrimitive(Condition cond, Register tag) {
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    cmp32(tag, ImmTag(JS::detail::ValueUpperExclPrimitiveTag));
    return cond == Equal ? Below : AboveOrEqual;
  }

  Condition testUndefined(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testUndefined(cond, scratch);
  }
  Condition testInt32(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testInt32(cond, scratch);
  }
  Condition testBoolean(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testBoolean(cond, scratch);
  }
  Condition testDouble(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testDouble(cond, scratch);
  }
  Condition testNumber(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testNumber(cond, scratch);
  }
  Condition testNull(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testNull(cond, scratch);
  }
  Condition testString(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testString(cond, scratch);
  }
  Condition testSymbol(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testSymbol(cond, scratch);
  }
  Condition testBigInt(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testBigInt(cond, scratch);
  }
  Condition testObject(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testObject(cond, scratch);
  }
  Condition testGCThing(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testGCThing(cond, scratch);
  }
  Condition testPrimitive(Condition cond, const ValueOperand& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testPrimitive(cond, scratch);
  }

  Condition testUndefined(Condition cond, const Address& src) {
    cmp32(ToUpper32(src),
          Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_UNDEFINED))));
    return cond;
  }
  Condition testInt32(Condition cond, const Address& src) {
    cmp32(ToUpper32(src), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_INT32))));
    return cond;
  }
  Condition testBoolean(Condition cond, const Address& src) {
    cmp32(ToUpper32(src), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_BOOLEAN))));
    return cond;
  }
  Condition testDouble(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testDouble(cond, scratch);
  }
  Condition testNumber(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testNumber(cond, scratch);
  }
  Condition testNull(Condition cond, const Address& src) {
    cmp32(ToUpper32(src), Imm32(Upper32Of(GetShiftedTag(JSVAL_TYPE_NULL))));
    return cond;
  }
  Condition testString(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testString(cond, scratch);
  }
  Condition testSymbol(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testSymbol(cond, scratch);
  }
  Condition testBigInt(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testBigInt(cond, scratch);
  }
  Condition testObject(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testObject(cond, scratch);
  }
  Condition testPrimitive(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testPrimitive(cond, scratch);
  }
  Condition testGCThing(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testGCThing(cond, scratch);
  }
  Condition testMagic(Condition cond, const Address& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testMagic(cond, scratch);
  }

  Condition testUndefined(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testUndefined(cond, scratch);
  }
  Condition testNull(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testNull(cond, scratch);
  }
  Condition testBoolean(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testBoolean(cond, scratch);
  }
  Condition testString(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testString(cond, scratch);
  }
  Condition testSymbol(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testSymbol(cond, scratch);
  }
  Condition testBigInt(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testBigInt(cond, scratch);
  }
  Condition testInt32(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testInt32(cond, scratch);
  }
  Condition testObject(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testObject(cond, scratch);
  }
  Condition testDouble(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testDouble(cond, scratch);
  }
  Condition testMagic(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testMagic(cond, scratch);
  }
  Condition testGCThing(Condition cond, const BaseIndex& src) {
    ScratchRegisterScope scratch(asMasm());
    splitTag(src, scratch);
    return testGCThing(cond, scratch);
  }

  Condition isMagic(Condition cond, const ValueOperand& src, JSWhyMagic why) {
    uint64_t magic = MagicValue(why).asRawBits();
    cmpPtr(src.valueReg(), ImmWord(magic));
    return cond;
  }

  void cmpPtr(Register lhs, const ImmWord rhs) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(lhs != scratch);
    if (intptr_t(rhs.value) <= INT32_MAX && intptr_t(rhs.value) >= INT32_MIN) {
      cmpPtr(lhs, Imm32(int32_t(rhs.value)));
    } else {
      movePtr(rhs, scratch);
      cmpPtr(lhs, scratch);
    }
  }
  void cmpPtr(Register lhs, const ImmPtr rhs) {
    cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
  }
  void cmpPtr(Register lhs, const ImmGCPtr rhs) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(lhs != scratch);
    movePtr(rhs, scratch);
    cmpPtr(lhs, scratch);
  }
  void cmpPtr(Register lhs, const Imm32 rhs) { cmpq(rhs, lhs); }
  void cmpPtr(const Operand& lhs, const ImmGCPtr rhs) {
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(!lhs.containsReg(scratch));
    movePtr(rhs, scratch);
    cmpPtr(lhs, scratch);
  }
  void cmpPtr(const Operand& lhs, const ImmWord rhs) {
    if ((intptr_t)rhs.value <= INT32_MAX && (intptr_t)rhs.value >= INT32_MIN) {
      cmpPtr(lhs, Imm32((int32_t)rhs.value));
    } else {
      ScratchRegisterScope scratch(asMasm());
      movePtr(rhs, scratch);
      cmpPtr(lhs, scratch);
    }
  }
  void cmpPtr(const Operand& lhs, const ImmPtr rhs) {
    cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
  }
  void cmpPtr(const Address& lhs, const ImmGCPtr rhs) {
    cmpPtr(Operand(lhs), rhs);
  }
  void cmpPtr(const Address& lhs, const ImmWord rhs) {
    cmpPtr(Operand(lhs), rhs);
  }
  void cmpPtr(const Address& lhs, const ImmPtr rhs) {
    cmpPtr(lhs, ImmWord(uintptr_t(rhs.value)));
  }
  void cmpPtr(const Operand& lhs, Register rhs) { cmpq(rhs, lhs); }
  void cmpPtr(Register lhs, const Operand& rhs) { cmpq(rhs, lhs); }
  void cmpPtr(const Operand& lhs, const Imm32 rhs) { cmpq(rhs, lhs); }
  void cmpPtr(const Address& lhs, Register rhs) { cmpPtr(Operand(lhs), rhs); }
  void cmpPtr(Register lhs, Register rhs) { cmpq(rhs, lhs); }
  void testPtr(Register lhs, Register rhs) { testq(rhs, lhs); }
  void testPtr(Register lhs, Imm32 rhs) { testq(rhs, lhs); }
  void testPtr(const Operand& lhs, Imm32 rhs) { testq(rhs, lhs); }
  void test64(Register lhs, Register rhs) { testq(rhs, lhs); }
  void test64(Register lhs, const Imm64 rhs) {
    if ((intptr_t)rhs.value <= INT32_MAX && (intptr_t)rhs.value >= INT32_MIN) {
      testq(Imm32((int32_t)rhs.value), lhs);
    } else {
      ScratchRegisterScope scratch(asMasm());
      movq(ImmWord(rhs.value), scratch);
      testq(scratch, lhs);
    }
  }

  // Compare-then-conditionally-move/load, for integer types
  template <size_t CmpSize, size_t MoveSize>
  void cmpMove(Condition cond, Register lhs, Register rhs, Register falseVal,
               Register trueValAndDest);

  template <size_t CmpSize, size_t MoveSize>
  void cmpMove(Condition cond, Register lhs, const Address& rhs,
               Register falseVal, Register trueValAndDest);

  template <size_t CmpSize, size_t LoadSize>
  void cmpLoad(Condition cond, Register lhs, Register rhs,
               const Address& falseVal, Register trueValAndDest);

  template <size_t CmpSize, size_t LoadSize>
  void cmpLoad(Condition cond, Register lhs, const Address& rhs,
               const Address& falseVal, Register trueValAndDest);

  /////////////////////////////////////////////////////////////////
  // Common interface.
  /////////////////////////////////////////////////////////////////

  void movePtr(Register src, Register dest) { movq(src, dest); }
  void movePtr(Register src, const Operand& dest) { movq(src, dest); }
  void movePtr(ImmWord imm, Register dest) { mov(imm, dest); }
  void movePtr(ImmPtr imm, Register dest) { mov(imm, dest); }
  void movePtr(wasm::SymbolicAddress imm, Register dest) { mov(imm, dest); }
  void movePtr(ImmGCPtr imm, Register dest) { movq(imm, dest); }
  void loadPtr(AbsoluteAddress address, Register dest) {
    if (X86Encoding::IsAddressImmediate(address.addr)) {
      movq(Operand(address), dest);
    } else {
      ScratchRegisterScope scratch(asMasm());
      mov(ImmPtr(address.addr), scratch);
      loadPtr(Address(scratch, 0x0), dest);
    }
  }
  FaultingCodeOffset loadPtr(const Address& address, Register dest) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movq(Operand(address), dest);
    return fco;
  }
  void load64(const Address& address, Register dest) {
    movq(Operand(address), dest);
  }
  void loadPtr(const Operand& src, Register dest) { movq(src, dest); }
  FaultingCodeOffset loadPtr(const BaseIndex& src, Register dest) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movq(Operand(src), dest);
    return fco;
  }
  void loadPrivate(const Address& src, Register dest) { loadPtr(src, dest); }
  void load32(AbsoluteAddress address, Register dest) {
    if (X86Encoding::IsAddressImmediate(address.addr)) {
      movl(Operand(address), dest);
    } else {
      ScratchRegisterScope scratch(asMasm());
      mov(ImmPtr(address.addr), scratch);
      load32(Address(scratch, 0x0), dest);
    }
  }
  void load64(const Operand& address, Register64 dest) {
    movq(address, dest.reg);
  }
  FaultingCodeOffset load64(const Address& address, Register64 dest) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movq(Operand(address), dest.reg);
    return fco;
  }
  FaultingCodeOffset load64(const BaseIndex& address, Register64 dest) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movq(Operand(address), dest.reg);
    return fco;
  }
  template <typename S>
  void load64Unaligned(const S& src, Register64 dest) {
    load64(src, dest);
  }
  template <typename T>
  void storePtr(ImmWord imm, T address) {
    if ((intptr_t)imm.value <= INT32_MAX && (intptr_t)imm.value >= INT32_MIN) {
      movq(Imm32((int32_t)imm.value), Operand(address));
    } else {
      ScratchRegisterScope scratch(asMasm());
      mov(imm, scratch);
      movq(scratch, Operand(address));
    }
  }
  template <typename T>
  void storePtr(ImmPtr imm, T address) {
    storePtr(ImmWord(uintptr_t(imm.value)), address);
  }
  template <typename T>
  void storePtr(ImmGCPtr imm, T address) {
    ScratchRegisterScope scratch(asMasm());
    movq(imm, scratch);
    movq(scratch, Operand(address));
  }
  FaultingCodeOffset storePtr(Register src, const Address& address) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movq(src, Operand(address));
    return fco;
  }
  void store64(Register src, const Address& address) {
    movq(src, Operand(address));
  }
  void store64(Register64 src, const Operand& address) {
    movq(src.reg, address);
  }
  FaultingCodeOffset storePtr(Register src, const BaseIndex& address) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    movq(src, Operand(address));
    return fco;
  }
  void storePtr(Register src, const Operand& dest) { movq(src, dest); }
  void storePtr(Register src, AbsoluteAddress address) {
    if (X86Encoding::IsAddressImmediate(address.addr)) {
      movq(src, Operand(address));
    } else {
      ScratchRegisterScope scratch(asMasm());
      mov(ImmPtr(address.addr), scratch);
      storePtr(src, Address(scratch, 0x0));
    }
  }
  void store32(Register src, AbsoluteAddress address) {
    if (X86Encoding::IsAddressImmediate(address.addr)) {
      movl(src, Operand(address));
    } else {
      ScratchRegisterScope scratch(asMasm());
      mov(ImmPtr(address.addr), scratch);
      store32(src, Address(scratch, 0x0));
    }
  }
  void store16(Register src, AbsoluteAddress address) {
    if (X86Encoding::IsAddressImmediate(address.addr)) {
      movw(src, Operand(address));
    } else {
      ScratchRegisterScope scratch(asMasm());
      mov(ImmPtr(address.addr), scratch);
      store16(src, Address(scratch, 0x0));
    }
  }
  FaultingCodeOffset store64(Register64 src, Address address) {
    FaultingCodeOffset fco = FaultingCodeOffset(currentOffset());
    storePtr(src.reg, address);
    return fco;
  }
  FaultingCodeOffset store64(Register64 src, const BaseIndex& address) {
    return storePtr(src.reg, address);
  }
  void store64(Imm64 imm, Address address) {
    storePtr(ImmWord(imm.value), address);
  }
  void store64(Imm64 imm, const BaseIndex& address) {
    storePtr(ImmWord(imm.value), address);
  }
  template <typename S, typename T>
  void store64Unaligned(const S& src, const T& dest) {
    store64(src, dest);
  }

  void splitTag(Register src, Register dest) {
    if (src != dest) {
      movq(src, dest);
    }
    shrq(Imm32(JSVAL_TAG_SHIFT), dest);
  }
  void splitTag(const ValueOperand& operand, Register dest) {
    splitTag(operand.valueReg(), dest);
  }
  void splitTag(const Operand& operand, Register dest) {
    movq(operand, dest);
    shrq(Imm32(JSVAL_TAG_SHIFT), dest);
  }
  void splitTag(const Address& operand, Register dest) {
    splitTag(Operand(operand), dest);
  }
  void splitTag(const BaseIndex& operand, Register dest) {
    splitTag(Operand(operand), dest);
  }

  // Extracts the tag of a value and places it in tag.
  void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag) {
    splitTag(value, tag);
  }
  void cmpTag(const ValueOperand& operand, ImmTag tag) {
    ScratchTagScope reg(asMasm(), operand);
    splitTagForTest(operand, reg);
    cmp32(reg, tag);
  }

  Condition testMagic(Condition cond, const ValueOperand& src) {
    ScratchTagScope scratch(asMasm(), src);
    splitTagForTest(src, scratch);
    return testMagic(cond, scratch);
  }
  Condition testError(Condition cond, const ValueOperand& src) {
    return testMagic(cond, src);
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

  void boxDouble(FloatRegister src, const ValueOperand& dest, FloatRegister) {
    vmovq(src, dest.valueReg());
  }
  void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest) {
    MOZ_ASSERT(src != dest.valueReg());
    boxValue(type, src, dest.valueReg());
  }

  // Note that the |dest| register here may be ScratchReg, so we shouldn't
  // use it.
  void unboxInt32(const ValueOperand& src, Register dest) {
    movl(src.valueReg(), dest);
  }
  void unboxInt32(const Operand& src, Register dest) { movl(src, dest); }
  void unboxInt32(const Address& src, Register dest) {
    unboxInt32(Operand(src), dest);
  }
  void unboxInt32(const BaseIndex& src, Register dest) {
    unboxInt32(Operand(src), dest);
  }
  template <typename T>
  void unboxDouble(const T& src, FloatRegister dest) {
    loadDouble(Operand(src), dest);
  }

  void unboxArgObjMagic(const ValueOperand& src, Register dest) {
    unboxArgObjMagic(Operand(src.valueReg()), dest);
  }
  void unboxArgObjMagic(const Operand& src, Register dest) {
    mov(ImmWord(0), dest);
  }
  void unboxArgObjMagic(const Address& src, Register dest) {
    unboxArgObjMagic(Operand(src), dest);
  }

  void unboxBoolean(const ValueOperand& src, Register dest) {
    movl(src.valueReg(), dest);
  }
  void unboxBoolean(const Operand& src, Register dest) { movl(src, dest); }
  void unboxBoolean(const Address& src, Register dest) {
    unboxBoolean(Operand(src), dest);
  }
  void unboxBoolean(const BaseIndex& src, Register dest) {
    unboxBoolean(Operand(src), dest);
  }

  void unboxMagic(const ValueOperand& src, Register dest) {
    movl(src.valueReg(), dest);
  }

  void unboxDouble(const ValueOperand& src, FloatRegister dest) {
    vmovq(src.valueReg(), dest);
  }

  void notBoolean(const ValueOperand& val) { xorq(Imm32(1), val.valueReg()); }

  void unboxNonDouble(const ValueOperand& src, Register dest,
                      JSValueType type) {
    MOZ_ASSERT(type != JSVAL_TYPE_DOUBLE);
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      movl(src.valueReg(), dest);
      return;
    }
    if (src.valueReg() == dest) {
      ScratchRegisterScope scratch(asMasm());
      mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), scratch);
      xorq(scratch, dest);
    } else {
      mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), dest);
      xorq(src.valueReg(), dest);
    }
  }
  void unboxNonDouble(const Operand& src, Register dest, JSValueType type) {
    MOZ_ASSERT(type != JSVAL_TYPE_DOUBLE);
    if (type == JSVAL_TYPE_INT32 || type == JSVAL_TYPE_BOOLEAN) {
      movl(src, dest);
      return;
    }
    // Explicitly permits |dest| to be used in |src|.
    ScratchRegisterScope scratch(asMasm());
    MOZ_ASSERT(dest != scratch);
    if (src.containsReg(dest)) {
      mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), scratch);
      // If src is already a register, then src and dest are the same
      // thing and we don't need to move anything into dest.
      if (src.kind() != Operand::REG) {
        movq(src, dest);
      }
      xorq(scratch, dest);
    } else {
      mov(ImmWord(JSVAL_TYPE_TO_SHIFTED_TAG(type)), dest);
      xorq(src, dest);
    }
  }
  void unboxNonDouble(const Address& src, Register dest, JSValueType type) {
    unboxNonDouble(Operand(src), dest, type);
  }
  void unboxNonDouble(const BaseIndex& src, Register dest, JSValueType type) {
    unboxNonDouble(Operand(src), dest, type);
  }

  void unboxString(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
  }
  void unboxString(const Operand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
  }
  void unboxString(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_STRING);
  }

  void unboxSymbol(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
  }
  void unboxSymbol(const Operand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_SYMBOL);
  }

  void unboxBigInt(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
  }
  void unboxBigInt(const Operand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
  }
  void unboxBigInt(const Address& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_BIGINT);
  }

  void unboxObject(const ValueOperand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
  }
  void unboxObject(const Operand& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
  }
  void unboxObject(const Address& src, Register dest) {
    unboxNonDouble(Operand(src), dest, JSVAL_TYPE_OBJECT);
  }
  void unboxObject(const BaseIndex& src, Register dest) {
    unboxNonDouble(Operand(src), dest, JSVAL_TYPE_OBJECT);
  }

  template <typename T>
  void unboxObjectOrNull(const T& src, Register dest) {
    unboxNonDouble(src, dest, JSVAL_TYPE_OBJECT);
    ScratchRegisterScope scratch(asMasm());
    mov(ImmWord(~JS::detail::ValueObjectOrNullBit), scratch);
    andq(scratch, dest);
  }

  // This should only be used for GC barrier code, to unbox a GC thing Value.
  // It's fine there because we don't depend on the actual Value type (all Cells
  // are treated the same way). In almost all other cases this would be
  // Spectre-unsafe - use unboxNonDouble and friends instead.
  void unboxGCThingForGCBarrier(const Address& src, Register dest) {
    movq(ImmWord(JS::detail::ValueGCThingPayloadMask), dest);
    andq(Operand(src), dest);
  }
  void unboxGCThingForGCBarrier(const ValueOperand& src, Register dest) {
    MOZ_ASSERT(src.valueReg() != dest);
    movq(ImmWord(JS::detail::ValueGCThingPayloadMask), dest);
    andq(src.valueReg(), dest);
  }

  void unboxWasmAnyRefGCThingForGCBarrier(const Address& src, Register dest) {
    movq(ImmWord(wasm::AnyRef::GCThingMask), dest);
    andq(Operand(src), dest);
  }

  // Like unboxGCThingForGCBarrier, but loads the GC thing's chunk base.
  void getGCThingValueChunk(const Address& src, Register dest) {
    movq(ImmWord(JS::detail::ValueGCThingPayloadChunkMask), dest);
    andq(Operand(src), dest);
  }
  void getGCThingValueChunk(const ValueOperand& src, Register dest) {
    MOZ_ASSERT(src.valueReg() != dest);
    movq(ImmWord(JS::detail::ValueGCThingPayloadChunkMask), dest);
    andq(src.valueReg(), dest);
  }

  void getWasmAnyRefGCThingChunk(Register src, Register dest) {
    MOZ_ASSERT(src != dest);
    movq(ImmWord(wasm::AnyRef::GCThingChunkMask), dest);
    andq(src, dest);
  }

  inline void fallibleUnboxPtrImpl(const Operand& src, Register dest,
                                   JSValueType type, Label* fail);

  // Extended unboxing API. If the payload is already in a register, returns
  // that register. Otherwise, provides a move to the given scratch register,
  // and returns that.
  [[nodiscard]] Register extractObject(const Address& address,
                                       Register scratch) {
    MOZ_ASSERT(scratch != ScratchReg);
    unboxObject(address, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractObject(const ValueOperand& value,
                                       Register scratch) {
    MOZ_ASSERT(scratch != ScratchReg);
    unboxObject(value, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractSymbol(const ValueOperand& value,
                                       Register scratch) {
    MOZ_ASSERT(scratch != ScratchReg);
    unboxSymbol(value, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractInt32(const ValueOperand& value,
                                      Register scratch) {
    MOZ_ASSERT(scratch != ScratchReg);
    unboxInt32(value, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractBoolean(const ValueOperand& value,
                                        Register scratch) {
    MOZ_ASSERT(scratch != ScratchReg);
    unboxBoolean(value, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractTag(const Address& address, Register scratch) {
    MOZ_ASSERT(scratch != ScratchReg);
    loadPtr(address, scratch);
    splitTag(scratch, scratch);
    return scratch;
  }
  [[nodiscard]] Register extractTag(const ValueOperand& value,
                                    Register scratch) {
    MOZ_ASSERT(scratch != ScratchReg);
    splitTag(value, scratch);
    return scratch;
  }

  inline void unboxValue(const ValueOperand& src, AnyRegister dest,
                         JSValueType type);

  // These two functions use the low 32-bits of the full value register.
  void boolValueToDouble(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToDouble(operand.valueReg(), dest);
  }
  void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToDouble(operand.valueReg(), dest);
  }

  void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToFloat32(operand.valueReg(), dest);
  }
  void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest) {
    convertInt32ToFloat32(operand.valueReg(), dest);
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

 public:
  Condition testInt32Truthy(bool truthy, const ValueOperand& operand) {
    test32(operand.valueReg(), operand.valueReg());
    return truthy ? NonZero : Zero;
  }
  Condition testStringTruthy(bool truthy, const ValueOperand& value);
  Condition testBigIntTruthy(bool truthy, const ValueOperand& value);

  template <typename T>
  inline void loadInt32OrDouble(const T& src, FloatRegister dest);

  template <typename T>
  void loadUnboxedValue(const T& src, MIRType type, AnyRegister dest) {
    if (dest.isFloat()) {
      loadInt32OrDouble(src, dest.fpu());
    } else {
      unboxNonDouble(Operand(src), dest.gpr(), ValueTypeFromMIRType(type));
    }
  }

  template <typename T>
  void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes,
                           JSValueType type) {
    switch (nbytes) {
      case 8: {
        ScratchRegisterScope scratch(asMasm());
        unboxNonDouble(value, scratch, type);
        storePtr(scratch, address);
        if (type == JSVAL_TYPE_OBJECT) {
          // Ideally we would call unboxObjectOrNull, but we need an extra
          // scratch register for that. So unbox as object, then clear the
          // object-or-null bit.
          mov(ImmWord(~JS::detail::ValueObjectOrNullBit), scratch);
          andq(scratch, Operand(address));
        }
        return;
      }
      case 4:
        store32(value.valueReg(), address);
        return;
      case 1:
        store8(value.valueReg(), address);
        return;
      default:
        MOZ_CRASH("Bad payload width");
    }
  }

  // Checks whether a double is representable as a 64-bit integer. If so, the
  // integer is written to the output register. Otherwise, a bailout is taken to
  // the given snapshot. This function overwrites the scratch float register.
  void convertDoubleToPtr(FloatRegister src, Register dest, Label* fail,
                          bool negativeZeroCheck = true);

  void convertUInt32ToDouble(Register src, FloatRegister dest) {
    // Zero the output register to break dependencies, see convertInt32ToDouble.
    zeroDouble(dest);

    vcvtsq2sd(src, dest, dest);
  }

  void convertUInt32ToFloat32(Register src, FloatRegister dest) {
    // Zero the output register to break dependencies, see convertInt32ToDouble.
    zeroDouble(dest);

    vcvtsq2ss(src, dest, dest);
  }

  inline void incrementInt32Value(const Address& addr);

  inline void ensureDouble(const ValueOperand& source, FloatRegister dest,
                           Label* failure);

 public:
  void handleFailureWithHandlerTail(Label* profilerExitTail,
                                    Label* bailoutTail);

  // Instrumentation for entering and leaving the profiler.
  void profilerEnterFrame(Register framePtr, Register scratch);
  void profilerExitFrame();
};

using MacroAssemblerSpecific = MacroAssemblerX64;

}  // namespace jit
}  // namespace js

#endif /* jit_x64_MacroAssembler_x64_h */
