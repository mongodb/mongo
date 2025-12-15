/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This is an INTERNAL header for Wasm baseline compiler: inline methods in the
// compiler for basic code generation.

#ifndef wasm_wasm_baseline_codegen_inl_h
#define wasm_wasm_baseline_codegen_inl_h

// The templates for register management must be defined by the time we use the
// templated emitters, below.
#include "wasm/WasmBCRegMgmt-inl.h"

namespace js {
namespace wasm {

//////////////////////////////////////////////////////////////////////////////
//
// Register-to-register moves.

void BaseCompiler::moveI32(RegI32 src, RegI32 dest) {
  if (src != dest) {
    masm.move32(src, dest);
  }
}

void BaseCompiler::moveI64(RegI64 src, RegI64 dest) {
  if (src != dest) {
    masm.move64(src, dest);
  }
}

void BaseCompiler::moveRef(RegRef src, RegRef dest) {
  if (src != dest) {
    masm.movePtr(src, dest);
  }
}

void BaseCompiler::movePtr(RegPtr src, RegPtr dest) {
  if (src != dest) {
    masm.movePtr(src, dest);
  }
}

void BaseCompiler::moveF64(RegF64 src, RegF64 dest) {
  if (src != dest) {
    masm.moveDouble(src, dest);
  }
}

void BaseCompiler::moveF32(RegF32 src, RegF32 dest) {
  if (src != dest) {
    masm.moveFloat32(src, dest);
  }
}

#ifdef ENABLE_WASM_SIMD
void BaseCompiler::moveV128(RegV128 src, RegV128 dest) {
  if (src != dest) {
    masm.moveSimd128(src, dest);
  }
}
#endif

template <>
inline void BaseCompiler::move<RegI32>(RegI32 src, RegI32 dest) {
  moveI32(src, dest);
}

template <>
inline void BaseCompiler::move<RegI64>(RegI64 src, RegI64 dest) {
  moveI64(src, dest);
}

template <>
inline void BaseCompiler::move<RegF32>(RegF32 src, RegF32 dest) {
  moveF32(src, dest);
}

template <>
inline void BaseCompiler::move<RegF64>(RegF64 src, RegF64 dest) {
  moveF64(src, dest);
}

template <>
inline void BaseCompiler::move<RegRef>(RegRef src, RegRef dest) {
  moveRef(src, dest);
}

template <>
inline void BaseCompiler::move<RegPtr>(RegPtr src, RegPtr dest) {
  movePtr(src, dest);
}

#ifdef ENABLE_WASM_SIMD
template <>
inline void BaseCompiler::move<RegV128>(RegV128 src, RegV128 dest) {
  moveV128(src, dest);
}
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Constant loads.

void BaseCompiler::moveImm32(int32_t v, RegI32 dest) {
  masm.move32(Imm32(v), dest);
}

void BaseCompiler::moveImm64(int64_t v, RegI64 dest) {
  masm.move64(Imm64(v), dest);
}

void BaseCompiler::moveImmRef(intptr_t v, RegRef dest) {
  masm.movePtr(ImmWord(v), dest);
}

//////////////////////////////////////////////////////////////////////////////
//
// Calls.

RegI32 BaseCompiler::captureReturnedI32() {
  RegI32 r = RegI32(ReturnReg);
  MOZ_ASSERT(isAvailableI32(r));
  needI32(r);
#if defined(JS_64BIT)
  masm.widenInt32(r);
#endif
  return r;
}

RegI64 BaseCompiler::captureReturnedI64() {
  RegI64 r = RegI64(ReturnReg64);
  MOZ_ASSERT(isAvailableI64(r));
  needI64(r);
  return r;
}

RegF32 BaseCompiler::captureReturnedF32(const FunctionCall& call) {
  RegF32 r = RegF32(ReturnFloat32Reg);
  MOZ_ASSERT(isAvailableF32(r));
  needF32(r);
#if defined(JS_CODEGEN_ARM)
  if (call.usesSystemAbi && !call.hardFP) {
    masm.ma_vxfer(ReturnReg, r);
  }
#endif
  return r;
}

RegF64 BaseCompiler::captureReturnedF64(const FunctionCall& call) {
  RegF64 r = RegF64(ReturnDoubleReg);
  MOZ_ASSERT(isAvailableF64(r));
  needF64(r);
#if defined(JS_CODEGEN_ARM)
  if (call.usesSystemAbi && !call.hardFP) {
    masm.ma_vxfer(ReturnReg64.low, ReturnReg64.high, r);
  }
#endif
  return r;
}

#ifdef ENABLE_WASM_SIMD
RegV128 BaseCompiler::captureReturnedV128(const FunctionCall& call) {
  RegV128 r = RegV128(ReturnSimd128Reg);
  MOZ_ASSERT(isAvailableV128(r));
  needV128(r);
  return r;
}
#endif

RegRef BaseCompiler::captureReturnedRef() {
  RegRef r = RegRef(ReturnReg);
  MOZ_ASSERT(isAvailableRef(r));
  needRef(r);
  return r;
}

//////////////////////////////////////////////////////////////////////////////
//
// Miscellaneous.

void BaseCompiler::trap(Trap t) const { masm.wasmTrap(t, trapSiteDesc()); }

void BaseCompiler::cmp64Set(Assembler::Condition cond, RegI64 lhs, RegI64 rhs,
                            RegI32 dest) {
#if defined(JS_PUNBOX64)
  masm.cmpPtrSet(cond, lhs.reg, rhs.reg, dest);
#else
  // TODO / OPTIMIZE (Bug 1316822): This is pretty branchy, we should be
  // able to do better.
  Label done, condTrue;
  masm.branch64(cond, lhs, rhs, &condTrue);
  moveImm32(0, dest);
  masm.jump(&done);
  masm.bind(&condTrue);
  moveImm32(1, dest);
  masm.bind(&done);
#endif
}

[[nodiscard]] bool BaseCompiler::supportsRoundInstruction(RoundingMode mode) {
  return Assembler::HasRoundInstruction(mode);
}

void BaseCompiler::roundF32(RoundingMode roundingMode, RegF32 f0) {
  masm.nearbyIntFloat32(roundingMode, f0, f0);
}

void BaseCompiler::roundF64(RoundingMode roundingMode, RegF64 f0) {
  masm.nearbyIntDouble(roundingMode, f0, f0);
}

void BaseCompiler::branchTo(Assembler::DoubleCondition c, RegF64 lhs,
                            RegF64 rhs, Label* l) {
  masm.branchDouble(c, lhs, rhs, l);
}

void BaseCompiler::branchTo(Assembler::DoubleCondition c, RegF32 lhs,
                            RegF32 rhs, Label* l) {
  masm.branchFloat(c, lhs, rhs, l);
}

void BaseCompiler::branchTo(Assembler::Condition c, RegI32 lhs, RegI32 rhs,
                            Label* l) {
  masm.branch32(c, lhs, rhs, l);
}

void BaseCompiler::branchTo(Assembler::Condition c, RegI32 lhs, Imm32 rhs,
                            Label* l) {
  masm.branch32(c, lhs, rhs, l);
}

void BaseCompiler::branchTo(Assembler::Condition c, RegI64 lhs, RegI64 rhs,
                            Label* l) {
  masm.branch64(c, lhs, rhs, l);
}

void BaseCompiler::branchTo(Assembler::Condition c, RegI64 lhs, Imm64 rhs,
                            Label* l) {
  masm.branch64(c, lhs, rhs, l);
}

void BaseCompiler::branchTo(Assembler::Condition c, RegRef lhs, ImmWord rhs,
                            Label* l) {
  masm.branchPtr(c, lhs, rhs, l);
}

//////////////////////////////////////////////////////////////////////////////
//
// Templated emitters

template <>
inline BaseCompiler& BaseCompiler::selectCompiler<BaseCompiler>() {
  return *this;
}

template <>
inline MacroAssembler& BaseCompiler::selectCompiler<MacroAssembler>() {
  return masm;
}

template <typename SourceType, typename DestType>
void BaseCompiler::emitUnop(void (*op)(MacroAssembler& masm, SourceType rs,
                                       DestType rd)) {
  SourceType rs = pop<SourceType>();
  DestType rd = need<DestType>();
  op(masm, rs, rd);
  free(rs);
  push(rd);
}

// Specialize narrowing reuse.  Consumers may assume that rs.reg==rd on 64-bit
// platforms, or rs.low==rd on 32-bit platforms.
template <>
inline void BaseCompiler::emitUnop(void (*op)(MacroAssembler& masm, RegI64 rs,
                                              RegI32 rd)) {
  RegI64 rs = pop<RegI64>();
  RegI32 rd = fromI64(rs);
  op(masm, rs, rd);
  freeI64Except(rs, rd);
  push(rd);
}

template <typename CompilerType, typename RegType>
void BaseCompiler::emitUnop(void (*op)(CompilerType& compiler, RegType rsd)) {
  RegType rsd = pop<RegType>();
  op(selectCompiler<CompilerType>(), rsd);
  push(rsd);
}

template <typename RegType, typename TempType>
void BaseCompiler::emitUnop(void (*op)(BaseCompiler& bc, RegType rsd,
                                       TempType rt),
                            TempType (*getSpecializedTemp)(BaseCompiler& bc)) {
  RegType rsd = pop<RegType>();
  TempType temp = getSpecializedTemp(*this);
  op(*this, rsd, temp);
  maybeFree(temp);
  push(rsd);
}

template <typename SourceType, typename DestType, typename TempType>
void BaseCompiler::emitUnop(void (*op)(MacroAssembler& masm, SourceType rs,
                                       DestType rd, TempType temp)) {
  SourceType rs = pop<SourceType>();
  DestType rd = need<DestType>();
  TempType temp = need<TempType>();
  op(masm, rs, rd, temp);
  free(rs);
  free(temp);
  push(rd);
}

template <typename SourceType, typename DestType, typename ImmType>
void BaseCompiler::emitUnop(ImmType immediate,
                            void (*op)(MacroAssembler&, ImmType, SourceType,
                                       DestType)) {
  SourceType rs = pop<SourceType>();
  DestType rd = need<DestType>();
  op(masm, immediate, rs, rd);
  free(rs);
  push(rd);
}

template <typename CompilerType, typename RhsType, typename LhsDestType>
void BaseCompiler::emitBinop(void (*op)(CompilerType& masm, RhsType src,
                                        LhsDestType srcDest)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  op(selectCompiler<CompilerType>(), rs, rsd);
  free(rs);
  push(rsd);
}

template <typename CompilerType, typename ValType>
void BaseCompiler::emitTernary(void (*op)(CompilerType&, ValType src0,
                                          ValType src1, ValType srcDest)) {
  ValType src2 = pop<ValType>();
  ValType src1 = pop<ValType>();
  ValType srcDest = pop<ValType>();
  op(selectCompiler<CompilerType>(), src1, src2, srcDest);
  free(src2);
  free(src1);
  push(srcDest);
}

template <typename CompilerType, typename ValType>
void BaseCompiler::emitTernary(void (*op)(CompilerType&, ValType src0,
                                          ValType src1, ValType srcDest,
                                          ValType temp)) {
  ValType src2 = pop<ValType>();
  ValType src1 = pop<ValType>();
  ValType srcDest = pop<ValType>();
  ValType temp = need<ValType>();
  op(selectCompiler<CompilerType>(), src1, src2, srcDest, temp);
  free(temp);
  free(src2);
  free(src1);
  push(srcDest);
}

template <typename CompilerType, typename ValType>
void BaseCompiler::emitTernaryResultLast(void (*op)(CompilerType&, ValType src0,
                                                    ValType src1,
                                                    ValType srcDest)) {
  ValType srcDest = pop<ValType>();
  ValType src2 = pop<ValType>();
  ValType src1 = pop<ValType>();
  op(selectCompiler<CompilerType>(), src1, src2, srcDest);
  free(src2);
  free(src1);
  push(srcDest);
}

template <typename RhsDestType, typename LhsType>
void BaseCompiler::emitBinop(void (*op)(MacroAssembler& masm, RhsDestType src,
                                        LhsType srcDest, RhsDestOp)) {
  RhsDestType rsd = pop<RhsDestType>();
  LhsType rs = pop<LhsType>();
  op(masm, rsd, rs, RhsDestOp::True);
  free(rs);
  push(rsd);
}

template <typename RhsType, typename LhsDestType, typename TempType>
void BaseCompiler::emitBinop(void (*op)(MacroAssembler& masm, RhsType rs,
                                        LhsDestType rsd, TempType temp)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  TempType temp = need<TempType>();
  op(masm, rs, rsd, temp);
  free(rs);
  free(temp);
  push(rsd);
}

template <typename RhsType, typename LhsDestType, typename TempType1,
          typename TempType2>
void BaseCompiler::emitBinop(void (*op)(MacroAssembler& masm, RhsType rs,
                                        LhsDestType rsd, TempType1 temp1,
                                        TempType2 temp2)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  TempType1 temp1 = need<TempType1>();
  TempType2 temp2 = need<TempType2>();
  op(masm, rs, rsd, temp1, temp2);
  free(rs);
  free(temp1);
  free(temp2);
  push(rsd);
}

template <typename RhsType, typename LhsDestType, typename ImmType>
void BaseCompiler::emitBinop(ImmType immediate,
                             void (*op)(MacroAssembler&, ImmType, RhsType,
                                        LhsDestType)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  op(masm, immediate, rs, rsd);
  free(rs);
  push(rsd);
}

template <typename RhsType, typename LhsDestType, typename ImmType,
          typename TempType1, typename TempType2>
void BaseCompiler::emitBinop(ImmType immediate,
                             void (*op)(MacroAssembler&, ImmType, RhsType,
                                        LhsDestType, TempType1 temp1,
                                        TempType2 temp2)) {
  RhsType rs = pop<RhsType>();
  LhsDestType rsd = pop<LhsDestType>();
  TempType1 temp1 = need<TempType1>();
  TempType2 temp2 = need<TempType2>();
  op(masm, immediate, rs, rsd, temp1, temp2);
  free(rs);
  free(temp1);
  free(temp2);
  push(rsd);
}

template <typename CompilerType1, typename CompilerType2, typename RegType,
          typename ImmType>
void BaseCompiler::emitBinop(void (*op)(CompilerType1& compiler, RegType rs,
                                        RegType rsd),
                             void (*opConst)(CompilerType2& compiler, ImmType c,
                                             RegType rsd),
                             RegType (BaseCompiler::*rhsPopper)()) {
  ImmType c;
  if (popConst(&c)) {
    RegType rsd = pop<RegType>();
    opConst(selectCompiler<CompilerType2>(), c, rsd);
    push(rsd);
  } else {
    RegType rs = rhsPopper ? (this->*rhsPopper)() : pop<RegType>();
    RegType rsd = pop<RegType>();
    op(selectCompiler<CompilerType1>(), rs, rsd);
    free(rs);
    push(rsd);
  }
}

template <typename R>
bool BaseCompiler::emitInstanceCallOp(const SymbolicAddressSignature& fn,
                                      R reader) {
  if (!reader()) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  return emitInstanceCall(fn);
}

template <typename A1, typename R>
bool BaseCompiler::emitInstanceCallOp(const SymbolicAddressSignature& fn,
                                      R reader) {
  A1 arg = 0;
  if (!reader(&arg)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  push(arg);
  return emitInstanceCall(fn);
}

template <typename A1, typename A2, typename R>
bool BaseCompiler::emitInstanceCallOp(const SymbolicAddressSignature& fn,
                                      R reader) {
  A1 arg1 = 0;
  A2 arg2 = 0;
  if (!reader(&arg1, &arg2)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  // Note order of arguments must be the same as for the reader.
  push(arg1);
  push(arg2);
  return emitInstanceCall(fn);
}

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_codegen_inl_h
