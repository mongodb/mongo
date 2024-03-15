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
// compiler for register management.

#ifndef wasm_wasm_baseline_reg_mgmt_inl_h
#define wasm_wasm_baseline_reg_mgmt_inl_h

namespace js {
namespace wasm {

bool BaseCompiler::isAvailableI32(RegI32 r) { return ra.isAvailableI32(r); }
bool BaseCompiler::isAvailableI64(RegI64 r) { return ra.isAvailableI64(r); }
bool BaseCompiler::isAvailableRef(RegRef r) { return ra.isAvailableRef(r); }
bool BaseCompiler::isAvailablePtr(RegPtr r) { return ra.isAvailablePtr(r); }
bool BaseCompiler::isAvailableF32(RegF32 r) { return ra.isAvailableF32(r); }
bool BaseCompiler::isAvailableF64(RegF64 r) { return ra.isAvailableF64(r); }
#ifdef ENABLE_WASM_SIMD
bool BaseCompiler::isAvailableV128(RegV128 r) { return ra.isAvailableV128(r); }
#endif

[[nodiscard]] RegI32 BaseCompiler::needI32() { return ra.needI32(); }
[[nodiscard]] RegI64 BaseCompiler::needI64() { return ra.needI64(); }
[[nodiscard]] RegRef BaseCompiler::needRef() { return ra.needRef(); }
[[nodiscard]] RegPtr BaseCompiler::needPtr() { return ra.needPtr(); }
[[nodiscard]] RegF32 BaseCompiler::needF32() { return ra.needF32(); }
[[nodiscard]] RegF64 BaseCompiler::needF64() { return ra.needF64(); }
#ifdef ENABLE_WASM_SIMD
[[nodiscard]] RegV128 BaseCompiler::needV128() { return ra.needV128(); }
#endif

void BaseCompiler::needI32(RegI32 specific) { ra.needI32(specific); }
void BaseCompiler::needI64(RegI64 specific) { ra.needI64(specific); }
void BaseCompiler::needRef(RegRef specific) { ra.needRef(specific); }
void BaseCompiler::needPtr(RegPtr specific) { ra.needPtr(specific); }
void BaseCompiler::needF32(RegF32 specific) { ra.needF32(specific); }
void BaseCompiler::needF64(RegF64 specific) { ra.needF64(specific); }
#ifdef ENABLE_WASM_SIMD
void BaseCompiler::needV128(RegV128 specific) { ra.needV128(specific); }
#endif

#if defined(JS_CODEGEN_ARM)
[[nodiscard]] RegI64 BaseCompiler::needI64Pair() { return ra.needI64Pair(); }
#endif

void BaseCompiler::freeI32(RegI32 r) { ra.freeI32(r); }
void BaseCompiler::freeI64(RegI64 r) { ra.freeI64(r); }
void BaseCompiler::freeRef(RegRef r) { ra.freeRef(r); }
void BaseCompiler::freePtr(RegPtr r) { ra.freePtr(r); }
void BaseCompiler::freeF32(RegF32 r) { ra.freeF32(r); }
void BaseCompiler::freeF64(RegF64 r) { ra.freeF64(r); }
#ifdef ENABLE_WASM_SIMD
void BaseCompiler::freeV128(RegV128 r) { ra.freeV128(r); }
#endif

void BaseCompiler::freeAny(AnyReg r) {
  switch (r.tag) {
    case AnyReg::I32:
      freeI32(r.i32());
      break;
    case AnyReg::I64:
      freeI64(r.i64());
      break;
    case AnyReg::REF:
      freeRef(r.ref());
      break;
    case AnyReg::F32:
      freeF32(r.f32());
      break;
    case AnyReg::F64:
      freeF64(r.f64());
      break;
#ifdef ENABLE_WASM_SIMD
    case AnyReg::V128:
      freeV128(r.v128());
      break;
#endif
    default:
      MOZ_CRASH();
  }
}

template <>
inline void BaseCompiler::free<RegI32>(RegI32 r) {
  freeI32(r);
}

template <>
inline void BaseCompiler::free<RegI64>(RegI64 r) {
  freeI64(r);
}

template <>
inline void BaseCompiler::free<RegRef>(RegRef r) {
  freeRef(r);
}

template <>
inline void BaseCompiler::free<RegPtr>(RegPtr r) {
  freePtr(r);
}

template <>
inline void BaseCompiler::free<RegF32>(RegF32 r) {
  freeF32(r);
}

template <>
inline void BaseCompiler::free<RegF64>(RegF64 r) {
  freeF64(r);
}

#ifdef ENABLE_WASM_SIMD
template <>
inline void BaseCompiler::free<RegV128>(RegV128 r) {
  freeV128(r);
}
#endif

template <>
inline void BaseCompiler::free<AnyReg>(AnyReg r) {
  freeAny(r);
}

void BaseCompiler::freeI64Except(RegI64 r, RegI32 except) {
#ifdef JS_PUNBOX64
  MOZ_ASSERT(r.reg == except);
#else
  MOZ_ASSERT(r.high == except || r.low == except);
  freeI64(r);
  needI32(except);
#endif
}

void BaseCompiler::maybeFree(RegI32 r) {
  if (r.isValid()) {
    freeI32(r);
  }
}

void BaseCompiler::maybeFree(RegI64 r) {
  if (r.isValid()) {
    freeI64(r);
  }
}

void BaseCompiler::maybeFree(RegF32 r) {
  if (r.isValid()) {
    freeF32(r);
  }
}

void BaseCompiler::maybeFree(RegF64 r) {
  if (r.isValid()) {
    freeF64(r);
  }
}

void BaseCompiler::maybeFree(RegRef r) {
  if (r.isValid()) {
    freeRef(r);
  }
}

void BaseCompiler::maybeFree(RegPtr r) {
  if (r.isValid()) {
    freePtr(r);
  }
}

#ifdef ENABLE_WASM_SIMD128
void BaseCompiler::maybeFree(RegV128 r) {
  if (r.isValid()) {
    freeV128(r);
  }
}
#endif

void BaseCompiler::needI32NoSync(RegI32 r) {
  MOZ_ASSERT(isAvailableI32(r));
  needI32(r);
}

// TODO / OPTIMIZE: need2xI32() can be optimized along with needI32()
// to avoid sync(). (Bug 1316802)

void BaseCompiler::need2xI32(RegI32 r0, RegI32 r1) {
  needI32(r0);
  needI32(r1);
}

void BaseCompiler::need2xI64(RegI64 r0, RegI64 r1) {
  needI64(r0);
  needI64(r1);
}

RegI32 BaseCompiler::fromI64(RegI64 r) { return RegI32(lowPart(r)); }

RegI32 BaseCompiler::maybeFromI64(RegI64 r) {
  if (!r.isValid()) {
    return RegI32::Invalid();
  }
  return fromI64(r);
}

#ifdef JS_PUNBOX64
RegI64 BaseCompiler::fromI32(RegI32 r) { return RegI64(Register64(r)); }
#endif

RegI64 BaseCompiler::widenI32(RegI32 r) {
  MOZ_ASSERT(!isAvailableI32(r));
#ifdef JS_PUNBOX64
  return fromI32(r);
#else
  RegI32 high = needI32();
  return RegI64(Register64(high, r));
#endif
}

RegI32 BaseCompiler::narrowI64(RegI64 r) {
#ifdef JS_PUNBOX64
  return RegI32(r.reg);
#else
  freeI32(RegI32(r.high));
  return RegI32(r.low);
#endif
}

RegI32 BaseCompiler::narrowRef(RegRef r) { return RegI32(r); }

RegI32 BaseCompiler::lowPart(RegI64 r) {
#ifdef JS_PUNBOX64
  return RegI32(r.reg);
#else
  return RegI32(r.low);
#endif
}

RegI32 BaseCompiler::maybeHighPart(RegI64 r) {
#ifdef JS_PUNBOX64
  return RegI32::Invalid();
#else
  return RegI32(r.high);
#endif
}

void BaseCompiler::maybeClearHighPart(RegI64 r) {
#if !defined(JS_PUNBOX64)
  moveImm32(0, RegI32(r.high));
#endif
}

// TODO: We want these to be inlined for sure; do we need an `inline` somewhere?

template <>
inline RegI32 BaseCompiler::need<RegI32>() {
  return needI32();
}
template <>
inline RegI64 BaseCompiler::need<RegI64>() {
  return needI64();
}
template <>
inline RegF32 BaseCompiler::need<RegF32>() {
  return needF32();
}
template <>
inline RegF64 BaseCompiler::need<RegF64>() {
  return needF64();
}

template <>
inline RegI32 BaseCompiler::pop<RegI32>() {
  return popI32();
}
template <>
inline RegI64 BaseCompiler::pop<RegI64>() {
  return popI64();
}
template <>
inline RegF32 BaseCompiler::pop<RegF32>() {
  return popF32();
}
template <>
inline RegF64 BaseCompiler::pop<RegF64>() {
  return popF64();
}

#ifdef ENABLE_WASM_SIMD
template <>
inline RegV128 BaseCompiler::need<RegV128>() {
  return needV128();
}
template <>
inline RegV128 BaseCompiler::pop<RegV128>() {
  return popV128();
}
#endif

// RegPtr values can't be pushed, hence can't be popped.
template <>
inline RegPtr BaseCompiler::need<RegPtr>() {
  return needPtr();
}

void BaseCompiler::needResultRegisters(ResultType type, ResultRegKind which) {
  if (type.empty()) {
    return;
  }

  for (ABIResultIter iter(type); !iter.done(); iter.next()) {
    ABIResult result = iter.cur();
    // Register results are visited first; when we see a stack result we're
    // done.
    if (!result.inRegister()) {
      return;
    }
    switch (result.type().kind()) {
      case ValType::I32:
        needI32(RegI32(result.gpr()));
        break;
      case ValType::I64:
        needI64(RegI64(result.gpr64()));
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        if (which == ResultRegKind::All) {
          needV128(RegV128(result.fpr()));
        }
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
      case ValType::F32:
        if (which == ResultRegKind::All) {
          needF32(RegF32(result.fpr()));
        }
        break;
      case ValType::F64:
        if (which == ResultRegKind::All) {
          needF64(RegF64(result.fpr()));
        }
        break;
      case ValType::Ref:
        needRef(RegRef(result.gpr()));
        break;
    }
  }
}

#ifdef JS_64BIT
void BaseCompiler::widenInt32ResultRegisters(ResultType type) {
  if (type.empty()) {
    return;
  }

  for (ABIResultIter iter(type); !iter.done(); iter.next()) {
    ABIResult result = iter.cur();
    if (result.inRegister() && result.type().kind() == ValType::I32) {
      masm.widenInt32(result.gpr());
    }
  }
}
#endif

void BaseCompiler::freeResultRegisters(ResultType type, ResultRegKind which) {
  if (type.empty()) {
    return;
  }

  for (ABIResultIter iter(type); !iter.done(); iter.next()) {
    ABIResult result = iter.cur();
    // Register results are visited first; when we see a stack result we're
    // done.
    if (!result.inRegister()) {
      return;
    }
    switch (result.type().kind()) {
      case ValType::I32:
        freeI32(RegI32(result.gpr()));
        break;
      case ValType::I64:
        freeI64(RegI64(result.gpr64()));
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        if (which == ResultRegKind::All) {
          freeV128(RegV128(result.fpr()));
        }
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
      case ValType::F32:
        if (which == ResultRegKind::All) {
          freeF32(RegF32(result.fpr()));
        }
        break;
      case ValType::F64:
        if (which == ResultRegKind::All) {
          freeF64(RegF64(result.fpr()));
        }
        break;
      case ValType::Ref:
        freeRef(RegRef(result.gpr()));
        break;
    }
  }
}

void BaseCompiler::needIntegerResultRegisters(ResultType type) {
  needResultRegisters(type, ResultRegKind::OnlyGPRs);
}

void BaseCompiler::freeIntegerResultRegisters(ResultType type) {
  freeResultRegisters(type, ResultRegKind::OnlyGPRs);
}

void BaseCompiler::needResultRegisters(ResultType type) {
  needResultRegisters(type, ResultRegKind::All);
}

void BaseCompiler::freeResultRegisters(ResultType type) {
  freeResultRegisters(type, ResultRegKind::All);
}

void BaseCompiler::captureResultRegisters(ResultType type) {
  assertResultRegistersAvailable(type);
  needResultRegisters(type);
}

void BaseCompiler::captureCallResultRegisters(ResultType type) {
  captureResultRegisters(type);
#ifdef JS_64BIT
  widenInt32ResultRegisters(type);
#endif
}

//////////////////////////////////////////////////////////////////////////////
//
// Control stack.  Some of these are very hot.

void BaseCompiler::initControl(Control& item, ResultType params) {
  // Make sure the constructor was run properly
  MOZ_ASSERT(!item.stackHeight.isValid() && item.stackSize == UINT32_MAX);

  uint32_t paramCount = deadCode_ ? 0 : params.length();
  uint32_t stackParamSize = stackConsumed(paramCount);
  item.stackHeight = fr.stackResultsBase(stackParamSize);
  item.stackSize = stk_.length() - paramCount;
  item.deadOnArrival = deadCode_;
  item.bceSafeOnEntry = bceSafe_;
}

Control& BaseCompiler::controlItem() { return iter_.controlItem(); }

Control& BaseCompiler::controlItem(uint32_t relativeDepth) {
  return iter_.controlItem(relativeDepth);
}

Control& BaseCompiler::controlOutermost() { return iter_.controlOutermost(); }

LabelKind BaseCompiler::controlKind(uint32_t relativeDepth) {
  return iter_.controlKind(relativeDepth);
}

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_reg_mgmt_inl_h
