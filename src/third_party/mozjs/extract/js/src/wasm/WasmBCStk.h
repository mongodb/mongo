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

// This is an INTERNAL header for Wasm baseline compiler: Wasm value stack.

#ifndef wasm_wasm_baseline_stk_h
#define wasm_wasm_baseline_stk_h

#include "wasm/WasmBCDefs.h"
#include "wasm/WasmBCRegDefs.h"

namespace js {
namespace wasm {

// Value stack: stack elements

struct Stk {
 private:
  Stk() : kind_(Unknown), i64val_(0) {}

 public:
  enum Kind {
    // The Mem opcodes are all clustered at the beginning to
    // allow for a quick test within sync().
    MemI32,  // 32-bit integer stack value ("offs")
    MemI64,  // 64-bit integer stack value ("offs")
    MemF32,  // 32-bit floating stack value ("offs")
    MemF64,  // 64-bit floating stack value ("offs")
#ifdef ENABLE_WASM_SIMD
    MemV128,  // 128-bit vector stack value ("offs")
#endif
    MemRef,  // reftype (pointer wide) stack value ("offs")

    // The Local opcodes follow the Mem opcodes for a similar
    // quick test within hasLocal().
    LocalI32,  // Local int32 var ("slot")
    LocalI64,  // Local int64 var ("slot")
    LocalF32,  // Local float32 var ("slot")
    LocalF64,  // Local double var ("slot")
#ifdef ENABLE_WASM_SIMD
    LocalV128,  // Local v128 var ("slot")
#endif
    LocalRef,  // Local reftype (pointer wide) var ("slot")

    RegisterI32,  // 32-bit integer register ("i32reg")
    RegisterI64,  // 64-bit integer register ("i64reg")
    RegisterF32,  // 32-bit floating register ("f32reg")
    RegisterF64,  // 64-bit floating register ("f64reg")
#ifdef ENABLE_WASM_SIMD
    RegisterV128,  // 128-bit vector register ("v128reg")
#endif
    RegisterRef,  // reftype (pointer wide) register ("refReg")

    ConstI32,  // 32-bit integer constant ("i32val")
    ConstI64,  // 64-bit integer constant ("i64val")
    ConstF32,  // 32-bit floating constant ("f32val")
    ConstF64,  // 64-bit floating constant ("f64val")
#ifdef ENABLE_WASM_SIMD
    ConstV128,  // 128-bit vector constant ("v128val")
#endif
    ConstRef,  // reftype (pointer wide) constant ("refval")

    Unknown,
  };

  Kind kind_;

  static const Kind MemLast = MemRef;
  static const Kind LocalLast = LocalRef;

  union {
    RegI32 i32reg_;
    RegI64 i64reg_;
    RegRef refReg_;
    RegF32 f32reg_;
    RegF64 f64reg_;
#ifdef ENABLE_WASM_SIMD
    RegV128 v128reg_;
#endif
    int32_t i32val_;
    int64_t i64val_;
    intptr_t refval_;
    float f32val_;
    double f64val_;
#ifdef ENABLE_WASM_SIMD
    V128 v128val_;
#endif
    uint32_t slot_;
    uint32_t offs_;
  };

  explicit Stk(RegI32 r) : kind_(RegisterI32), i32reg_(r) {}
  explicit Stk(RegI64 r) : kind_(RegisterI64), i64reg_(r) {}
  explicit Stk(RegRef r) : kind_(RegisterRef), refReg_(r) {}
  explicit Stk(RegF32 r) : kind_(RegisterF32), f32reg_(r) {}
  explicit Stk(RegF64 r) : kind_(RegisterF64), f64reg_(r) {}
#ifdef ENABLE_WASM_SIMD
  explicit Stk(RegV128 r) : kind_(RegisterV128), v128reg_(r) {}
#endif
  explicit Stk(int32_t v) : kind_(ConstI32), i32val_(v) {}
  explicit Stk(uint32_t v) : kind_(ConstI32), i32val_(int32_t(v)) {}
  explicit Stk(int64_t v) : kind_(ConstI64), i64val_(v) {}
  explicit Stk(float v) : kind_(ConstF32), f32val_(v) {}
  explicit Stk(double v) : kind_(ConstF64), f64val_(v) {}
#ifdef ENABLE_WASM_SIMD
  explicit Stk(V128 v) : kind_(ConstV128), v128val_(v) {}
#endif
  explicit Stk(Kind k, uint32_t v) : kind_(k), slot_(v) {
    MOZ_ASSERT(k > MemLast && k <= LocalLast);
  }
  static Stk StkRef(intptr_t v) {
    Stk s;
    s.kind_ = ConstRef;
    s.refval_ = v;
    return s;
  }
  static Stk StackResult(ValType type, uint32_t offs) {
    Kind k;
    switch (type.kind()) {
      case ValType::I32:
        k = Stk::MemI32;
        break;
      case ValType::I64:
        k = Stk::MemI64;
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        k = Stk::MemV128;
        break;
#else
        MOZ_CRASH("No SIMD");
#endif
      case ValType::F32:
        k = Stk::MemF32;
        break;
      case ValType::F64:
        k = Stk::MemF64;
        break;
      case ValType::Ref:
        k = Stk::MemRef;
        break;
    }
    Stk s;
    s.setOffs(k, offs);
    return s;
  }

  void setOffs(Kind k, uint32_t v) {
    MOZ_ASSERT(k <= MemLast);
    kind_ = k;
    offs_ = v;
  }

  Kind kind() const { return kind_; }
  bool isMem() const { return kind_ <= MemLast; }

  RegI32 i32reg() const {
    MOZ_ASSERT(kind_ == RegisterI32);
    return i32reg_;
  }
  RegI64 i64reg() const {
    MOZ_ASSERT(kind_ == RegisterI64);
    return i64reg_;
  }
  RegRef refReg() const {
    MOZ_ASSERT(kind_ == RegisterRef);
    return refReg_;
  }
  RegF32 f32reg() const {
    MOZ_ASSERT(kind_ == RegisterF32);
    return f32reg_;
  }
  RegF64 f64reg() const {
    MOZ_ASSERT(kind_ == RegisterF64);
    return f64reg_;
  }
#ifdef ENABLE_WASM_SIMD
  RegV128 v128reg() const {
    MOZ_ASSERT(kind_ == RegisterV128);
    return v128reg_;
  }
#endif
  int32_t i32val() const {
    MOZ_ASSERT(kind_ == ConstI32);
    return i32val_;
  }
  int64_t i64val() const {
    MOZ_ASSERT(kind_ == ConstI64);
    return i64val_;
  }
  intptr_t refval() const {
    MOZ_ASSERT(kind_ == ConstRef);
    return refval_;
  }

  // For these two, use an out-param instead of simply returning, to
  // use the normal stack and not the x87 FP stack (which has effect on
  // NaNs with the signaling bit set).

  void f32val(float* out) const {
    MOZ_ASSERT(kind_ == ConstF32);
    *out = f32val_;
  }
  void f64val(double* out) const {
    MOZ_ASSERT(kind_ == ConstF64);
    *out = f64val_;
  }

#ifdef ENABLE_WASM_SIMD
  // For SIMD, do the same as for floats since we're using float registers to
  // hold vectors; this is just conservative.
  void v128val(V128* out) const {
    MOZ_ASSERT(kind_ == ConstV128);
    *out = v128val_;
  }
#endif

  uint32_t slot() const {
    MOZ_ASSERT(kind_ > MemLast && kind_ <= LocalLast);
    return slot_;
  }
  uint32_t offs() const {
    MOZ_ASSERT(isMem());
    return offs_;
  }

#ifdef DEBUG
  // Print a stack element (Stk) to stderr.  Skip the trailing \n.  Printing
  // of the actual contents of each stack element (see case ConstI32) can be
  // filled in on demand -- even printing just the element `kind_` fields can
  // be very useful.
  void showStackElem() const {
    switch (kind_) {
      case MemI32:
        fprintf(stderr, "MemI32()");
        break;
      case MemI64:
        fprintf(stderr, "MemI64()");
        break;
      case MemF32:
        fprintf(stderr, "MemF32()");
        break;
      case MemF64:
        fprintf(stderr, "MemF64()");
        break;
#  ifdef ENABLE_WASM_SIMD
      case MemV128:
        fprintf(stderr, "MemV128()");
        break;
#  endif
      case MemRef:
        fprintf(stderr, "MemRef()");
        break;
      case LocalI32:
        fprintf(stderr, "LocalI32()");
        break;
      case LocalI64:
        fprintf(stderr, "LocalI64()");
        break;
      case LocalF32:
        fprintf(stderr, "LocalF32()");
        break;
      case LocalF64:
        fprintf(stderr, "LocalF64()");
        break;
#  ifdef ENABLE_WASM_SIMD
      case LocalV128:
        fprintf(stderr, "LocalV128()");
        break;
#  endif
      case LocalRef:
        fprintf(stderr, "LocalRef()");
        break;
      case RegisterI32:
        fprintf(stderr, "RegisterI32()");
        break;
      case RegisterI64:
        fprintf(stderr, "RegisterI64()");
        break;
      case RegisterF32:
        fprintf(stderr, "RegisterF32()");
        break;
      case RegisterF64:
        fprintf(stderr, "RegisterF64()");
        break;
#  ifdef ENABLE_WASM_SIMD
      case RegisterV128:
        fprintf(stderr, "RegisterV128()");
        break;
#  endif
      case RegisterRef:
        fprintf(stderr, "RegisterRef()");
        break;
      case ConstI32:
        fprintf(stderr, "ConstI32(%d)", (int)i32val_);
        break;
      case ConstI64:
        fprintf(stderr, "ConstI64()");
        break;
      case ConstF32:
        fprintf(stderr, "ConstF32()");
        break;
      case ConstF64:
        fprintf(stderr, "ConstF64()");
        break;
#  ifdef ENABLE_WASM_SIMD
      case ConstV128:
        fprintf(stderr, "ConstV128()");
        break;
#  endif
      case ConstRef:
        fprintf(stderr, "ConstRef()");
        break;
      case Unknown:
        fprintf(stderr, "Unknown()");
        break;
      default:
        fprintf(stderr, "!! Stk::showStackElem !!");
        break;
    }
  }
#endif
};

using StkVector = Vector<Stk, 0, SystemAllocPolicy>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_stk_h
