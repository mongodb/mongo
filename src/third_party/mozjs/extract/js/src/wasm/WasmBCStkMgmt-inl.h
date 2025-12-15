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
// compiler for Stk values and value stack management.

#ifndef wasm_wasm_baseline_stk_mgmt_inl_h
#define wasm_wasm_baseline_stk_mgmt_inl_h

namespace js {
namespace wasm {

#ifdef DEBUG
size_t BaseCompiler::countMemRefsOnStk() {
  size_t nRefs = 0;
  for (Stk& v : stk_) {
    if (v.kind() == Stk::MemRef) {
      nRefs++;
    }
  }
  return nRefs;
}
#endif

template <typename T>
void BaseCompiler::push(T item) {
  // None of the single-arg Stk constructors create a Stk::MemRef, so
  // there's no need to increment stackMapGenerator_.memRefsOnStk here.
  stk_.infallibleEmplaceBack(Stk(item));
}

void BaseCompiler::pushConstRef(intptr_t v) {
  stk_.infallibleEmplaceBack(Stk::StkRef(v));
}

void BaseCompiler::loadConstI32(const Stk& src, RegI32 dest) {
  moveImm32(src.i32val(), dest);
}

void BaseCompiler::loadMemI32(const Stk& src, RegI32 dest) {
  fr.loadStackI32(src.offs(), dest);
}

void BaseCompiler::loadLocalI32(const Stk& src, RegI32 dest) {
  fr.loadLocalI32(localFromSlot(src.slot(), MIRType::Int32), dest);
}

void BaseCompiler::loadRegisterI32(const Stk& src, RegI32 dest) {
  moveI32(src.i32reg(), dest);
}

void BaseCompiler::loadConstI64(const Stk& src, RegI64 dest) {
  moveImm64(src.i64val(), dest);
}

void BaseCompiler::loadMemI64(const Stk& src, RegI64 dest) {
  fr.loadStackI64(src.offs(), dest);
}

void BaseCompiler::loadLocalI64(const Stk& src, RegI64 dest) {
  fr.loadLocalI64(localFromSlot(src.slot(), MIRType::Int64), dest);
}

void BaseCompiler::loadRegisterI64(const Stk& src, RegI64 dest) {
  moveI64(src.i64reg(), dest);
}

void BaseCompiler::loadConstRef(const Stk& src, RegRef dest) {
  moveImmRef(src.refval(), dest);
}

void BaseCompiler::loadMemRef(const Stk& src, RegRef dest) {
  fr.loadStackRef(src.offs(), dest);
}

void BaseCompiler::loadLocalRef(const Stk& src, RegRef dest) {
  fr.loadLocalRef(localFromSlot(src.slot(), MIRType::WasmAnyRef), dest);
}

void BaseCompiler::loadRegisterRef(const Stk& src, RegRef dest) {
  moveRef(src.refReg(), dest);
}

void BaseCompiler::loadConstF64(const Stk& src, RegF64 dest) {
  double d;
  src.f64val(&d);
  masm.loadConstantDouble(d, dest);
}

void BaseCompiler::loadMemF64(const Stk& src, RegF64 dest) {
  fr.loadStackF64(src.offs(), dest);
}

void BaseCompiler::loadLocalF64(const Stk& src, RegF64 dest) {
  fr.loadLocalF64(localFromSlot(src.slot(), MIRType::Double), dest);
}

void BaseCompiler::loadRegisterF64(const Stk& src, RegF64 dest) {
  moveF64(src.f64reg(), dest);
}

void BaseCompiler::loadConstF32(const Stk& src, RegF32 dest) {
  float f;
  src.f32val(&f);
  masm.loadConstantFloat32(f, dest);
}

void BaseCompiler::loadMemF32(const Stk& src, RegF32 dest) {
  fr.loadStackF32(src.offs(), dest);
}

void BaseCompiler::loadLocalF32(const Stk& src, RegF32 dest) {
  fr.loadLocalF32(localFromSlot(src.slot(), MIRType::Float32), dest);
}

void BaseCompiler::loadRegisterF32(const Stk& src, RegF32 dest) {
  moveF32(src.f32reg(), dest);
}

#ifdef ENABLE_WASM_SIMD
void BaseCompiler::loadConstV128(const Stk& src, RegV128 dest) {
  V128 f;
  src.v128val(&f);
  masm.loadConstantSimd128(SimdConstant::CreateX16((int8_t*)f.bytes), dest);
}

void BaseCompiler::loadMemV128(const Stk& src, RegV128 dest) {
  fr.loadStackV128(src.offs(), dest);
}

void BaseCompiler::loadLocalV128(const Stk& src, RegV128 dest) {
  fr.loadLocalV128(localFromSlot(src.slot(), MIRType::Simd128), dest);
}

void BaseCompiler::loadRegisterV128(const Stk& src, RegV128 dest) {
  moveV128(src.v128reg(), dest);
}
#endif

void BaseCompiler::loadI32(const Stk& src, RegI32 dest) {
  switch (src.kind()) {
    case Stk::ConstI32:
      loadConstI32(src, dest);
      break;
    case Stk::MemI32:
      loadMemI32(src, dest);
      break;
    case Stk::LocalI32:
      loadLocalI32(src, dest);
      break;
    case Stk::RegisterI32:
      loadRegisterI32(src, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: Expected I32 on stack");
  }
}

void BaseCompiler::loadI64(const Stk& src, RegI64 dest) {
  switch (src.kind()) {
    case Stk::ConstI64:
      loadConstI64(src, dest);
      break;
    case Stk::MemI64:
      loadMemI64(src, dest);
      break;
    case Stk::LocalI64:
      loadLocalI64(src, dest);
      break;
    case Stk::RegisterI64:
      loadRegisterI64(src, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: Expected I64 on stack");
  }
}

#if !defined(JS_PUNBOX64)
void BaseCompiler::loadI64Low(const Stk& src, RegI32 dest) {
  switch (src.kind()) {
    case Stk::ConstI64:
      moveImm32(int32_t(src.i64val()), dest);
      break;
    case Stk::MemI64:
      fr.loadStackI64Low(src.offs(), dest);
      break;
    case Stk::LocalI64:
      fr.loadLocalI64Low(localFromSlot(src.slot(), MIRType::Int64), dest);
      break;
    case Stk::RegisterI64:
      moveI32(RegI32(src.i64reg().low), dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: Expected I64 on stack");
  }
}

void BaseCompiler::loadI64High(const Stk& src, RegI32 dest) {
  switch (src.kind()) {
    case Stk::ConstI64:
      moveImm32(int32_t(src.i64val() >> 32), dest);
      break;
    case Stk::MemI64:
      fr.loadStackI64High(src.offs(), dest);
      break;
    case Stk::LocalI64:
      fr.loadLocalI64High(localFromSlot(src.slot(), MIRType::Int64), dest);
      break;
    case Stk::RegisterI64:
      moveI32(RegI32(src.i64reg().high), dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: Expected I64 on stack");
  }
}
#endif

void BaseCompiler::loadF64(const Stk& src, RegF64 dest) {
  switch (src.kind()) {
    case Stk::ConstF64:
      loadConstF64(src, dest);
      break;
    case Stk::MemF64:
      loadMemF64(src, dest);
      break;
    case Stk::LocalF64:
      loadLocalF64(src, dest);
      break;
    case Stk::RegisterF64:
      loadRegisterF64(src, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected F64 on stack");
  }
}

void BaseCompiler::loadF32(const Stk& src, RegF32 dest) {
  switch (src.kind()) {
    case Stk::ConstF32:
      loadConstF32(src, dest);
      break;
    case Stk::MemF32:
      loadMemF32(src, dest);
      break;
    case Stk::LocalF32:
      loadLocalF32(src, dest);
      break;
    case Stk::RegisterF32:
      loadRegisterF32(src, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected F32 on stack");
  }
}

#ifdef ENABLE_WASM_SIMD
void BaseCompiler::loadV128(const Stk& src, RegV128 dest) {
  switch (src.kind()) {
    case Stk::ConstV128:
      loadConstV128(src, dest);
      break;
    case Stk::MemV128:
      loadMemV128(src, dest);
      break;
    case Stk::LocalV128:
      loadLocalV128(src, dest);
      break;
    case Stk::RegisterV128:
      loadRegisterV128(src, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected V128 on stack");
  }
}
#endif

void BaseCompiler::loadRef(const Stk& src, RegRef dest) {
  switch (src.kind()) {
    case Stk::ConstRef:
      loadConstRef(src, dest);
      break;
    case Stk::MemRef:
      loadMemRef(src, dest);
      break;
    case Stk::LocalRef:
      loadLocalRef(src, dest);
      break;
    case Stk::RegisterRef:
      loadRegisterRef(src, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected ref on stack");
  }
}

void BaseCompiler::peekRefAt(uint32_t depth, RegRef dest) {
  MOZ_ASSERT(depth < stk_.length());
  Stk& src = peek(stk_.length() - depth - 1);
  loadRef(src, dest);
}

// Flush all local and register value stack elements to memory.
//
// TODO / OPTIMIZE: As this is fairly expensive and causes worse
// code to be emitted subsequently, it is useful to avoid calling
// it.  (Bug 1316802)
//
// Some optimization has been done already.  Remaining
// opportunities:
//
//  - It would be interesting to see if we can specialize it
//    before calls with particularly simple signatures, or where
//    we can do parallel assignment of register arguments, or
//    similar.  See notes in emitCall().
//
//  - Operations that need specific registers: multiply, quotient,
//    remainder, will tend to sync because the registers we need
//    will tend to be allocated.  We may be able to avoid that by
//    prioritizing registers differently (takeLast instead of
//    takeFirst) but we may also be able to allocate an unused
//    register on demand to free up one we need, thus avoiding the
//    sync.  That type of fix would go into needI32().

void BaseCompiler::sync() {
  size_t start = 0;
  size_t lim = stk_.length();

  for (size_t i = lim; i > 0; i--) {
    // Memory opcodes are first in the enum, single check against MemLast is
    // fine.
    if (stk_[i - 1].kind() <= Stk::MemLast) {
      start = i;
      break;
    }
  }

  for (size_t i = start; i < lim; i++) {
    Stk& v = stk_[i];
    switch (v.kind()) {
      case Stk::LocalI32: {
        ScratchI32 scratch(*this);
        loadLocalI32(v, scratch);
        uint32_t offs = fr.pushGPR(scratch);
        v.setOffs(Stk::MemI32, offs);
        break;
      }
      case Stk::RegisterI32: {
        uint32_t offs = fr.pushGPR(v.i32reg());
        freeI32(v.i32reg());
        v.setOffs(Stk::MemI32, offs);
        break;
      }
      case Stk::LocalI64: {
        ScratchI32 scratch(*this);
#ifdef JS_PUNBOX64
        loadI64(v, fromI32(scratch));
        uint32_t offs = fr.pushGPR(scratch);
#else
        fr.loadLocalI64High(localFromSlot(v.slot(), MIRType::Int64), scratch);
        fr.pushGPR(scratch);
        fr.loadLocalI64Low(localFromSlot(v.slot(), MIRType::Int64), scratch);
        uint32_t offs = fr.pushGPR(scratch);
#endif
        v.setOffs(Stk::MemI64, offs);
        break;
      }
      case Stk::RegisterI64: {
#ifdef JS_PUNBOX64
        uint32_t offs = fr.pushGPR(v.i64reg().reg);
        freeI64(v.i64reg());
#else
        fr.pushGPR(v.i64reg().high);
        uint32_t offs = fr.pushGPR(v.i64reg().low);
        freeI64(v.i64reg());
#endif
        v.setOffs(Stk::MemI64, offs);
        break;
      }
      case Stk::LocalF64: {
        ScratchF64 scratch(*this);
        loadF64(v, scratch);
        uint32_t offs = fr.pushDouble(scratch);
        v.setOffs(Stk::MemF64, offs);
        break;
      }
      case Stk::RegisterF64: {
        uint32_t offs = fr.pushDouble(v.f64reg());
        freeF64(v.f64reg());
        v.setOffs(Stk::MemF64, offs);
        break;
      }
      case Stk::LocalF32: {
        ScratchF32 scratch(*this);
        loadF32(v, scratch);
        uint32_t offs = fr.pushFloat32(scratch);
        v.setOffs(Stk::MemF32, offs);
        break;
      }
      case Stk::RegisterF32: {
        uint32_t offs = fr.pushFloat32(v.f32reg());
        freeF32(v.f32reg());
        v.setOffs(Stk::MemF32, offs);
        break;
      }
#ifdef ENABLE_WASM_SIMD
      case Stk::LocalV128: {
        ScratchV128 scratch(*this);
        loadV128(v, scratch);
        uint32_t offs = fr.pushV128(scratch);
        v.setOffs(Stk::MemV128, offs);
        break;
      }
      case Stk::RegisterV128: {
        uint32_t offs = fr.pushV128(v.v128reg());
        freeV128(v.v128reg());
        v.setOffs(Stk::MemV128, offs);
        break;
      }
#endif
      case Stk::LocalRef: {
        ScratchRef scratch(*this);
        loadLocalRef(v, scratch);
        uint32_t offs = fr.pushGPR(scratch);
        v.setOffs(Stk::MemRef, offs);
        stackMapGenerator_.memRefsOnStk++;
        break;
      }
      case Stk::RegisterRef: {
        uint32_t offs = fr.pushGPR(v.refReg());
        freeRef(v.refReg());
        v.setOffs(Stk::MemRef, offs);
        stackMapGenerator_.memRefsOnStk++;
        break;
      }
      default: {
        break;
      }
    }
  }
}

// This is an optimization used to avoid calling sync() for
// setLocal(): if the local does not exist unresolved on the stack
// then we can skip the sync.

bool BaseCompiler::hasLocal(uint32_t slot) {
  for (size_t i = stk_.length(); i > 0; i--) {
    // Memory opcodes are first in the enum, single check against MemLast is
    // fine.
    Stk::Kind kind = stk_[i - 1].kind();
    if (kind <= Stk::MemLast) {
      return false;
    }

    // Local opcodes follow memory opcodes in the enum, single check against
    // LocalLast is sufficient.
    if (kind <= Stk::LocalLast && stk_[i - 1].slot() == slot) {
      return true;
    }
  }
  return false;
}

void BaseCompiler::syncLocal(uint32_t slot) {
  if (hasLocal(slot)) {
    sync();  // TODO / OPTIMIZE: Improve this?  (Bug 1316817)
  }
}

// Push the register r onto the stack.

void BaseCompiler::pushAny(AnyReg r) {
  switch (r.tag) {
    case AnyReg::I32: {
      pushI32(r.i32());
      break;
    }
    case AnyReg::I64: {
      pushI64(r.i64());
      break;
    }
    case AnyReg::F32: {
      pushF32(r.f32());
      break;
    }
    case AnyReg::F64: {
      pushF64(r.f64());
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case AnyReg::V128: {
      pushV128(r.v128());
      break;
    }
#endif
    case AnyReg::REF: {
      pushRef(r.ref());
      break;
    }
  }
}

void BaseCompiler::pushI32(RegI32 r) {
  MOZ_ASSERT(!isAvailableI32(r));
  push(Stk(r));
}

void BaseCompiler::pushI64(RegI64 r) {
  MOZ_ASSERT(!isAvailableI64(r));
  push(Stk(r));
}

void BaseCompiler::pushRef(RegRef r) {
  MOZ_ASSERT(!isAvailableRef(r));
  push(Stk(r));
}

void BaseCompiler::pushPtr(RegPtr r) {
  MOZ_ASSERT(!isAvailablePtr(r));
#ifdef JS_64BIT
  pushI64(RegI64(Register64(r)));
#else
  pushI32(RegI32(r));
#endif
}

void BaseCompiler::pushF64(RegF64 r) {
  MOZ_ASSERT(!isAvailableF64(r));
  push(Stk(r));
}

void BaseCompiler::pushF32(RegF32 r) {
  MOZ_ASSERT(!isAvailableF32(r));
  push(Stk(r));
}

#ifdef ENABLE_WASM_SIMD
void BaseCompiler::pushV128(RegV128 r) {
  MOZ_ASSERT(!isAvailableV128(r));
  push(Stk(r));
}
#endif

// Push the value onto the stack.  PushI32 can also take uint32_t, and PushI64
// can take uint64_t; the semantics are the same.  Appropriate sign extension
// for a 32-bit value on a 64-bit architecture happens when the value is
// popped, see the definition of moveImm32 below.

void BaseCompiler::pushI32(int32_t v) { push(Stk(v)); }

void BaseCompiler::pushI64(int64_t v) { push(Stk(v)); }

void BaseCompiler::pushRef(intptr_t v) { pushConstRef(v); }

void BaseCompiler::pushPtr(intptr_t v) {
#ifdef JS_64BIT
  pushI64(v);
#else
  pushI32(v);
#endif
}

void BaseCompiler::pushF64(double v) { push(Stk(v)); }

void BaseCompiler::pushF32(float v) { push(Stk(v)); }

#ifdef ENABLE_WASM_SIMD
void BaseCompiler::pushV128(V128 v) { push(Stk(v)); }
#endif

// Push the local slot onto the stack.  The slot will not be read
// here; it will be read when it is consumed, or when a side
// effect to the slot forces its value to be saved.

void BaseCompiler::pushLocalI32(uint32_t slot) {
  stk_.infallibleEmplaceBack(Stk(Stk::LocalI32, slot));
}

void BaseCompiler::pushLocalI64(uint32_t slot) {
  stk_.infallibleEmplaceBack(Stk(Stk::LocalI64, slot));
}

void BaseCompiler::pushLocalRef(uint32_t slot) {
  stk_.infallibleEmplaceBack(Stk(Stk::LocalRef, slot));
}

void BaseCompiler::pushLocalF64(uint32_t slot) {
  stk_.infallibleEmplaceBack(Stk(Stk::LocalF64, slot));
}

void BaseCompiler::pushLocalF32(uint32_t slot) {
  stk_.infallibleEmplaceBack(Stk(Stk::LocalF32, slot));
}

#ifdef ENABLE_WASM_SIMD
void BaseCompiler::pushLocalV128(uint32_t slot) {
  stk_.infallibleEmplaceBack(Stk(Stk::LocalV128, slot));
}
#endif

void BaseCompiler::pushU32AsI64(RegI32 rs) {
  RegI64 rd = widenI32(rs);
  masm.move32To64ZeroExtend(rs, rd);
  pushI64(rd);
}

AnyReg BaseCompiler::popAny(AnyReg specific) {
  switch (stk_.back().kind()) {
    case Stk::MemI32:
    case Stk::LocalI32:
    case Stk::RegisterI32:
    case Stk::ConstI32:
      return AnyReg(popI32(specific.i32()));

    case Stk::MemI64:
    case Stk::LocalI64:
    case Stk::RegisterI64:
    case Stk::ConstI64:
      return AnyReg(popI64(specific.i64()));

    case Stk::MemF32:
    case Stk::LocalF32:
    case Stk::RegisterF32:
    case Stk::ConstF32:
      return AnyReg(popF32(specific.f32()));

    case Stk::MemF64:
    case Stk::LocalF64:
    case Stk::RegisterF64:
    case Stk::ConstF64:
      return AnyReg(popF64(specific.f64()));

#ifdef ENABLE_WASM_SIMD
    case Stk::MemV128:
    case Stk::LocalV128:
    case Stk::RegisterV128:
    case Stk::ConstV128:
      return AnyReg(popV128(specific.v128()));
#endif

    case Stk::MemRef:
    case Stk::LocalRef:
    case Stk::RegisterRef:
    case Stk::ConstRef:
      return AnyReg(popRef(specific.ref()));

    case Stk::Unknown:
      MOZ_CRASH();

    default:
      MOZ_CRASH();
  }
}

AnyReg BaseCompiler::popAny() {
  switch (stk_.back().kind()) {
    case Stk::MemI32:
    case Stk::LocalI32:
    case Stk::RegisterI32:
    case Stk::ConstI32:
      return AnyReg(popI32());

    case Stk::MemI64:
    case Stk::LocalI64:
    case Stk::RegisterI64:
    case Stk::ConstI64:
      return AnyReg(popI64());

    case Stk::MemF32:
    case Stk::LocalF32:
    case Stk::RegisterF32:
    case Stk::ConstF32:
      return AnyReg(popF32());

    case Stk::MemF64:
    case Stk::LocalF64:
    case Stk::RegisterF64:
    case Stk::ConstF64:
      return AnyReg(popF64());

#ifdef ENABLE_WASM_SIMD
    case Stk::MemV128:
    case Stk::LocalV128:
    case Stk::RegisterV128:
    case Stk::ConstV128:
      return AnyReg(popV128());
#endif

    case Stk::MemRef:
    case Stk::LocalRef:
    case Stk::RegisterRef:
    case Stk::ConstRef:
      return AnyReg(popRef());

    case Stk::Unknown:
      MOZ_CRASH();

    default:
      MOZ_CRASH();
  }
}

// Call only from other popI32() variants.
// v must be the stack top.  May pop the CPU stack.

void BaseCompiler::popI32(const Stk& v, RegI32 dest) {
  MOZ_ASSERT(&v == &stk_.back());
  switch (v.kind()) {
    case Stk::ConstI32:
      loadConstI32(v, dest);
      break;
    case Stk::LocalI32:
      loadLocalI32(v, dest);
      break;
    case Stk::MemI32:
      fr.popGPR(dest);
      break;
    case Stk::RegisterI32:
      loadRegisterI32(v, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected int on stack");
  }
}

RegI32 BaseCompiler::popI32() {
  Stk& v = stk_.back();
  RegI32 r;
  if (v.kind() == Stk::RegisterI32) {
    r = v.i32reg();
  } else {
    popI32(v, (r = needI32()));
  }
  stk_.popBack();
  return r;
}

RegI32 BaseCompiler::popI32(RegI32 specific) {
  Stk& v = stk_.back();

  if (!(v.kind() == Stk::RegisterI32 && v.i32reg() == specific)) {
    needI32(specific);
    popI32(v, specific);
    if (v.kind() == Stk::RegisterI32) {
      freeI32(v.i32reg());
    }
  }

  stk_.popBack();
  return specific;
}

#ifdef ENABLE_WASM_SIMD
// Call only from other popV128() variants.
// v must be the stack top.  May pop the CPU stack.

void BaseCompiler::popV128(const Stk& v, RegV128 dest) {
  MOZ_ASSERT(&v == &stk_.back());
  switch (v.kind()) {
    case Stk::ConstV128:
      loadConstV128(v, dest);
      break;
    case Stk::LocalV128:
      loadLocalV128(v, dest);
      break;
    case Stk::MemV128:
      fr.popV128(dest);
      break;
    case Stk::RegisterV128:
      loadRegisterV128(v, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected int on stack");
  }
}

RegV128 BaseCompiler::popV128() {
  Stk& v = stk_.back();
  RegV128 r;
  if (v.kind() == Stk::RegisterV128) {
    r = v.v128reg();
  } else {
    popV128(v, (r = needV128()));
  }
  stk_.popBack();
  return r;
}

RegV128 BaseCompiler::popV128(RegV128 specific) {
  Stk& v = stk_.back();

  if (!(v.kind() == Stk::RegisterV128 && v.v128reg() == specific)) {
    needV128(specific);
    popV128(v, specific);
    if (v.kind() == Stk::RegisterV128) {
      freeV128(v.v128reg());
    }
  }

  stk_.popBack();
  return specific;
}
#endif

// Call only from other popI64() variants.
// v must be the stack top.  May pop the CPU stack.

void BaseCompiler::popI64(const Stk& v, RegI64 dest) {
  MOZ_ASSERT(&v == &stk_.back());
  switch (v.kind()) {
    case Stk::ConstI64:
      loadConstI64(v, dest);
      break;
    case Stk::LocalI64:
      loadLocalI64(v, dest);
      break;
    case Stk::MemI64:
#ifdef JS_PUNBOX64
      fr.popGPR(dest.reg);
#else
      fr.popGPR(dest.low);
      fr.popGPR(dest.high);
#endif
      break;
    case Stk::RegisterI64:
      loadRegisterI64(v, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected long on stack");
  }
}

RegI64 BaseCompiler::popI64() {
  Stk& v = stk_.back();
  RegI64 r;
  if (v.kind() == Stk::RegisterI64) {
    r = v.i64reg();
  } else {
    popI64(v, (r = needI64()));
  }
  stk_.popBack();
  return r;
}

// Note, the stack top can be in one half of "specific" on 32-bit
// systems.  We can optimize, but for simplicity, if the register
// does not match exactly, then just force the stack top to memory
// and then read it back in.

RegI64 BaseCompiler::popI64(RegI64 specific) {
  Stk& v = stk_.back();

  if (!(v.kind() == Stk::RegisterI64 && v.i64reg() == specific)) {
    needI64(specific);
    popI64(v, specific);
    if (v.kind() == Stk::RegisterI64) {
      freeI64(v.i64reg());
    }
  }

  stk_.popBack();
  return specific;
}

// Call only from other popRef() variants.
// v must be the stack top.  May pop the CPU stack.

void BaseCompiler::popRef(const Stk& v, RegRef dest) {
  MOZ_ASSERT(&v == &stk_.back());
  switch (v.kind()) {
    case Stk::ConstRef:
      loadConstRef(v, dest);
      break;
    case Stk::LocalRef:
      loadLocalRef(v, dest);
      break;
    case Stk::MemRef:
      fr.popGPR(dest);
      break;
    case Stk::RegisterRef:
      loadRegisterRef(v, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected ref on stack");
  }
}

RegRef BaseCompiler::popRef(RegRef specific) {
  Stk& v = stk_.back();

  if (!(v.kind() == Stk::RegisterRef && v.refReg() == specific)) {
    needRef(specific);
    popRef(v, specific);
    if (v.kind() == Stk::RegisterRef) {
      freeRef(v.refReg());
    }
  }

  stk_.popBack();
  if (v.kind() == Stk::MemRef) {
    stackMapGenerator_.memRefsOnStk--;
  }
  return specific;
}

RegRef BaseCompiler::popRef() {
  Stk& v = stk_.back();
  RegRef r;
  if (v.kind() == Stk::RegisterRef) {
    r = v.refReg();
  } else {
    popRef(v, (r = needRef()));
  }
  stk_.popBack();
  if (v.kind() == Stk::MemRef) {
    stackMapGenerator_.memRefsOnStk--;
  }
  return r;
}

// Call only from other popPtr() variants.
// v must be the stack top.  May pop the CPU stack.

void BaseCompiler::popPtr(const Stk& v, RegPtr dest) {
#ifdef JS_64BIT
  popI64(v, RegI64(Register64(dest)));
#else
  popI32(v, RegI32(dest));
#endif
}

RegPtr BaseCompiler::popPtr(RegPtr specific) {
#ifdef JS_64BIT
  return RegPtr(popI64(RegI64(Register64(specific))).reg);
#else
  return RegPtr(popI32(RegI32(specific)));
#endif
}

RegPtr BaseCompiler::popPtr() {
#ifdef JS_64BIT
  return RegPtr(popI64().reg);
#else
  return RegPtr(popI32());
#endif
}

// Call only from other popF64() variants.
// v must be the stack top.  May pop the CPU stack.

void BaseCompiler::popF64(const Stk& v, RegF64 dest) {
  MOZ_ASSERT(&v == &stk_.back());
  switch (v.kind()) {
    case Stk::ConstF64:
      loadConstF64(v, dest);
      break;
    case Stk::LocalF64:
      loadLocalF64(v, dest);
      break;
    case Stk::MemF64:
      fr.popDouble(dest);
      break;
    case Stk::RegisterF64:
      loadRegisterF64(v, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected double on stack");
  }
}

RegF64 BaseCompiler::popF64() {
  Stk& v = stk_.back();
  RegF64 r;
  if (v.kind() == Stk::RegisterF64) {
    r = v.f64reg();
  } else {
    popF64(v, (r = needF64()));
  }
  stk_.popBack();
  return r;
}

RegF64 BaseCompiler::popF64(RegF64 specific) {
  Stk& v = stk_.back();

  if (!(v.kind() == Stk::RegisterF64 && v.f64reg() == specific)) {
    needF64(specific);
    popF64(v, specific);
    if (v.kind() == Stk::RegisterF64) {
      freeF64(v.f64reg());
    }
  }

  stk_.popBack();
  return specific;
}

// Call only from other popF32() variants.
// v must be the stack top.  May pop the CPU stack.

void BaseCompiler::popF32(const Stk& v, RegF32 dest) {
  MOZ_ASSERT(&v == &stk_.back());
  switch (v.kind()) {
    case Stk::ConstF32:
      loadConstF32(v, dest);
      break;
    case Stk::LocalF32:
      loadLocalF32(v, dest);
      break;
    case Stk::MemF32:
      fr.popFloat32(dest);
      break;
    case Stk::RegisterF32:
      loadRegisterF32(v, dest);
      break;
    default:
      MOZ_CRASH("Compiler bug: expected float on stack");
  }
}

RegF32 BaseCompiler::popF32() {
  Stk& v = stk_.back();
  RegF32 r;
  if (v.kind() == Stk::RegisterF32) {
    r = v.f32reg();
  } else {
    popF32(v, (r = needF32()));
  }
  stk_.popBack();
  return r;
}

RegF32 BaseCompiler::popF32(RegF32 specific) {
  Stk& v = stk_.back();

  if (!(v.kind() == Stk::RegisterF32 && v.f32reg() == specific)) {
    needF32(specific);
    popF32(v, specific);
    if (v.kind() == Stk::RegisterF32) {
      freeF32(v.f32reg());
    }
  }

  stk_.popBack();
  return specific;
}

bool BaseCompiler::hasConst() const {
  const Stk& v = stk_.back();
  switch (v.kind()) {
    case Stk::ConstI32:
    case Stk::ConstI64:
    case Stk::ConstF32:
    case Stk::ConstF64:
#ifdef ENABLE_WASM_SIMD
    case Stk::ConstV128:
#endif
    case Stk::ConstRef:
      return true;
    default:
      return false;
  }
}

bool BaseCompiler::popConst(int32_t* c) {
  Stk& v = stk_.back();
  if (v.kind() != Stk::ConstI32) {
    return false;
  }
  *c = v.i32val();
  stk_.popBack();
  return true;
}

bool BaseCompiler::popConst(int64_t* c) {
  Stk& v = stk_.back();
  if (v.kind() != Stk::ConstI64) {
    return false;
  }
  *c = v.i64val();
  stk_.popBack();
  return true;
}

bool BaseCompiler::peekConst(int32_t* c) {
  Stk& v = stk_.back();
  if (v.kind() != Stk::ConstI32) {
    return false;
  }
  *c = v.i32val();
  return true;
}

bool BaseCompiler::peekConst(int64_t* c) {
  Stk& v = stk_.back();
  if (v.kind() != Stk::ConstI64) {
    return false;
  }
  *c = v.i64val();
  return true;
}

bool BaseCompiler::peek2xConst(int32_t* c0, int32_t* c1) {
  MOZ_ASSERT(stk_.length() >= 2);
  const Stk& v0 = *(stk_.end() - 1);
  const Stk& v1 = *(stk_.end() - 2);
  if (v0.kind() != Stk::ConstI32 || v1.kind() != Stk::ConstI32) {
    return false;
  }
  *c0 = v0.i32val();
  *c1 = v1.i32val();
  return true;
}

bool BaseCompiler::popConstPositivePowerOfTwo(int32_t* c, uint_fast8_t* power,
                                              int32_t cutoff) {
  Stk& v = stk_.back();
  if (v.kind() != Stk::ConstI32) {
    return false;
  }
  *c = v.i32val();
  if (*c <= cutoff || !mozilla::IsPowerOfTwo(static_cast<uint32_t>(*c))) {
    return false;
  }
  *power = mozilla::FloorLog2(*c);
  stk_.popBack();
  return true;
}

bool BaseCompiler::popConstPositivePowerOfTwo(int64_t* c, uint_fast8_t* power,
                                              int64_t cutoff) {
  Stk& v = stk_.back();
  if (v.kind() != Stk::ConstI64) {
    return false;
  }
  *c = v.i64val();
  if (*c <= cutoff || !mozilla::IsPowerOfTwo(static_cast<uint64_t>(*c))) {
    return false;
  }
  *power = mozilla::FloorLog2(*c);
  stk_.popBack();
  return true;
}

void BaseCompiler::pop2xI32(RegI32* r0, RegI32* r1) {
  *r1 = popI32();
  *r0 = popI32();
}

void BaseCompiler::pop2xI64(RegI64* r0, RegI64* r1) {
  *r1 = popI64();
  *r0 = popI64();
}

void BaseCompiler::pop2xF32(RegF32* r0, RegF32* r1) {
  *r1 = popF32();
  *r0 = popF32();
}

void BaseCompiler::pop2xF64(RegF64* r0, RegF64* r1) {
  *r1 = popF64();
  *r0 = popF64();
}

#ifdef ENABLE_WASM_SIMD
void BaseCompiler::pop2xV128(RegV128* r0, RegV128* r1) {
  *r1 = popV128();
  *r0 = popV128();
}
#endif

void BaseCompiler::pop2xRef(RegRef* r0, RegRef* r1) {
  *r1 = popRef();
  *r0 = popRef();
}

// Pop to a specific register
RegI32 BaseCompiler::popI32ToSpecific(RegI32 specific) {
  freeI32(specific);
  return popI32(specific);
}

RegI64 BaseCompiler::popI64ToSpecific(RegI64 specific) {
  freeI64(specific);
  return popI64(specific);
}

RegI64 BaseCompiler::popAddressToInt64(AddressType addressType) {
  if (addressType == AddressType::I64) {
    return popI64();
  }

  MOZ_ASSERT(addressType == AddressType::I32);
#ifdef JS_64BIT
  return RegI64(Register64(popI32()));
#else
  RegI32 lowPart = popI32();
  RegI32 highPart = needI32();
  masm.xor32(highPart, highPart);
  return RegI64(Register64(highPart, lowPart));
#endif
}

RegI32 BaseCompiler::popTableAddressToClampedInt32(AddressType addressType) {
  if (addressType == AddressType::I32) {
    return popI32();
  }

#ifdef ENABLE_WASM_MEMORY64
  MOZ_ASSERT(addressType == AddressType::I64);
  RegI64 val = popI64();
  RegI32 clamped = narrowI64(val);
  masm.wasmClampTable64Address(val, clamped);
  return clamped;
#else
  MOZ_CRASH("got i64 table address without memory64 enabled");
#endif
}

void BaseCompiler::replaceTableAddressWithClampedInt32(
    AddressType addressType) {
  if (addressType == AddressType::I32) {
    return;
  }

  pushI32(popTableAddressToClampedInt32(addressType));
}

#ifdef JS_CODEGEN_ARM
// Pop an I64 as a valid register pair.
RegI64 BaseCompiler::popI64Pair() {
  RegI64 r = needI64Pair();
  popI64ToSpecific(r);
  return r;
}
#endif

// Pop an I64 but narrow it and return the narrowed part.
RegI32 BaseCompiler::popI64ToI32() {
  RegI64 r = popI64();
  return narrowI64(r);
}

RegI32 BaseCompiler::popI64ToSpecificI32(RegI32 specific) {
  RegI64 rd = widenI32(specific);
  popI64ToSpecific(rd);
  return narrowI64(rd);
}

bool BaseCompiler::peekLocal(uint32_t* local) {
  Stk& v = stk_.back();
  // See hasLocal() for documentation of this logic.
  if (v.kind() <= Stk::MemLast || v.kind() > Stk::LocalLast) {
    return false;
  }
  *local = v.slot();
  return true;
}

size_t BaseCompiler::stackConsumed(size_t numval) {
  size_t size = 0;
  MOZ_ASSERT(numval <= stk_.length());
  for (uint32_t i = stk_.length() - 1; numval > 0; numval--, i--) {
    Stk& v = stk_[i];
    switch (v.kind()) {
      case Stk::MemRef:
        size += BaseStackFrame::StackSizeOfPtr;
        break;
      case Stk::MemI32:
        size += BaseStackFrame::StackSizeOfPtr;
        break;
      case Stk::MemI64:
        size += BaseStackFrame::StackSizeOfInt64;
        break;
      case Stk::MemF64:
        size += BaseStackFrame::StackSizeOfDouble;
        break;
      case Stk::MemF32:
        size += BaseStackFrame::StackSizeOfFloat;
        break;
#ifdef ENABLE_WASM_SIMD
      case Stk::MemV128:
        size += BaseStackFrame::StackSizeOfV128;
        break;
#endif
      default:
        break;
    }
  }
  return size;
}

void BaseCompiler::popValueStackTo(uint32_t stackSize) {
  for (uint32_t i = stk_.length(); i > stackSize; i--) {
    Stk& v = stk_[i - 1];
    switch (v.kind()) {
      case Stk::RegisterI32:
        freeI32(v.i32reg());
        break;
      case Stk::RegisterI64:
        freeI64(v.i64reg());
        break;
      case Stk::RegisterF64:
        freeF64(v.f64reg());
        break;
      case Stk::RegisterF32:
        freeF32(v.f32reg());
        break;
#ifdef ENABLE_WASM_SIMD
      case Stk::RegisterV128:
        freeV128(v.v128reg());
        break;
#endif
      case Stk::RegisterRef:
        freeRef(v.refReg());
        break;
      case Stk::MemRef:
        stackMapGenerator_.memRefsOnStk--;
        break;
      default:
        break;
    }
  }
  stk_.shrinkTo(stackSize);
}

void BaseCompiler::popValueStackBy(uint32_t items) {
  popValueStackTo(stk_.length() - items);
}

void BaseCompiler::dropValue() {
  if (peek(0).isMem()) {
    fr.popBytes(stackConsumed(1));
  }
  popValueStackBy(1);
}

// Peek at the stack, for calls.

Stk& BaseCompiler::peek(uint32_t relativeDepth) {
  return stk_[stk_.length() - 1 - relativeDepth];
}
}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_stk_mgmt_inl_h
