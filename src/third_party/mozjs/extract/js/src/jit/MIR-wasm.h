/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Everything needed to build actual MIR instructions: the actual opcodes and
 * instructions, the instruction interface, and use chains.
 */

#ifndef jit_MIR_wasm_h
#define jit_MIR_wasm_h

#include "mozilla/Array.h"
#include "mozilla/HashFunctions.h"
#ifdef JS_JITSPEW
#  include "mozilla/Sprintf.h"
#  include "mozilla/Vector.h"
#endif

#include <algorithm>
#include <initializer_list>

#include "jit/MIR.h"
#include "util/DifferentialTesting.h"
#include "wasm/WasmGcObject.h"

namespace js {

class WasmInstanceObject;

namespace wasm {
class FuncExport;
extern uint32_t MIRTypeToABIResultSize(jit::MIRType);
}  // namespace wasm

namespace jit {

class MWasmNullConstant : public MNullaryInstruction {
  mozilla::Maybe<wasm::RefTypeHierarchy> hierarchy_;

  explicit MWasmNullConstant(wasm::MaybeRefType type)
      : MNullaryInstruction(classOpcode),
        hierarchy_(type.isSome() ? mozilla::Some(type.value().hierarchy())
                                 : mozilla::Nothing()) {
    setResultType(MIRType::WasmAnyRef);
    setMovable();
    if (type.isSome()) {
      initWasmRefType(wasm::MaybeRefType(type.value().bottomType()));
    }
  }

 public:
  INSTRUCTION_HEADER(WasmNullConstant)
  TRIVIAL_NEW_WRAPPERS

  mozilla::Maybe<wasm::RefTypeHierarchy> hierarchy() const {
    return hierarchy_;
  }

  HashNumber valueHash() const override;
  bool congruentTo(const MDefinition* ins) const override {
    return ins->isWasmNullConstant() &&
           hierarchy() == ins->toWasmNullConstant()->hierarchy();
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MWasmNullConstant)
};

// Floating-point value as created by wasm. Just a constant value, used to
// effectively inhibit all the MIR optimizations. This uses the same LIR nodes
// as a MConstant of the same type would.
class MWasmFloatConstant : public MNullaryInstruction {
  union {
    float f32_;
    double f64_;
#ifdef ENABLE_WASM_SIMD
    int8_t s128_[16];
    uint64_t bits_[2];
#else
    uint64_t bits_[1];
#endif
  } u;

  explicit MWasmFloatConstant(MIRType type) : MNullaryInstruction(classOpcode) {
    u.bits_[0] = 0;
#ifdef ENABLE_WASM_SIMD
    u.bits_[1] = 0;
#endif
    setResultType(type);
  }

 public:
  INSTRUCTION_HEADER(WasmFloatConstant)

  static MWasmFloatConstant* NewDouble(TempAllocator& alloc, double d) {
    auto* ret = new (alloc) MWasmFloatConstant(MIRType::Double);
    ret->u.f64_ = d;
    return ret;
  }

  static MWasmFloatConstant* NewFloat32(TempAllocator& alloc, float f) {
    auto* ret = new (alloc) MWasmFloatConstant(MIRType::Float32);
    ret->u.f32_ = f;
    return ret;
  }

#ifdef ENABLE_WASM_SIMD
  static MWasmFloatConstant* NewSimd128(TempAllocator& alloc,
                                        const SimdConstant& s) {
    auto* ret = new (alloc) MWasmFloatConstant(MIRType::Simd128);
    memcpy(ret->u.s128_, s.bytes(), 16);
    return ret;
  }
#endif

  HashNumber valueHash() const override;
  bool congruentTo(const MDefinition* ins) const override;
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  const double& toDouble() const {
    MOZ_ASSERT(type() == MIRType::Double);
    return u.f64_;
  }
  const float& toFloat32() const {
    MOZ_ASSERT(type() == MIRType::Float32);
    return u.f32_;
  }
#ifdef ENABLE_WASM_SIMD
  const SimdConstant toSimd128() const {
    MOZ_ASSERT(type() == MIRType::Simd128);
    return SimdConstant::CreateX16(u.s128_);
  }
#endif
#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    switch (type()) {
      case MIRType::Float32:
        SprintfLiteral(buf, "f32{%e}", (double)u.f32_);
        break;
      case MIRType::Double:
        SprintfLiteral(buf, "f64{%e}", u.f64_);
        break;
#  ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
        SprintfLiteral(buf, "v128{[1]=%016llx:[0]=%016llx}",
                       (unsigned long long int)u.bits_[1],
                       (unsigned long long int)u.bits_[0]);
        break;
#  endif
      default:
        SprintfLiteral(buf, "!!getExtras: missing case!!");
        break;
    }
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MWasmFloatConstant)
};

// Converts a uint32 to a float32 (coming from wasm).
class MWasmUnsignedToFloat32 : public MUnaryInstruction,
                               public NoTypePolicy::Data {
  explicit MWasmUnsignedToFloat32(MDefinition* def)
      : MUnaryInstruction(classOpcode, def) {
    setResultType(MIRType::Float32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmUnsignedToFloat32)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool canProduceFloat32() const override { return true; }

  ALLOW_CLONE(MWasmUnsignedToFloat32)
};

class MWasmNewI31Ref : public MUnaryInstruction, public NoTypePolicy::Data {
  explicit MWasmNewI31Ref(MDefinition* input)
      : MUnaryInstruction(classOpcode, input) {
    MOZ_ASSERT(input->type() == MIRType::Int32);
    setResultType(MIRType::WasmAnyRef);
    setMovable();
    initWasmRefType(wasm::MaybeRefType(wasm::RefType::i31().asNonNullable()));
  }

 public:
  INSTRUCTION_HEADER(WasmNewI31Ref)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// The same as MWasmTruncateToInt64 but with the Instance dependency.
// It used only for arm now because on arm we need to call builtin to truncate
// to i64.
class MWasmBuiltinTruncateToInt64 : public MAryInstruction<2>,
                                    public NoTypePolicy::Data {
  TruncFlags flags_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmBuiltinTruncateToInt64(MDefinition* def, MDefinition* instance,
                              TruncFlags flags,
                              const wasm::TrapSiteDesc& trapSiteDesc)
      : MAryInstruction(classOpcode),
        flags_(flags),
        trapSiteDesc_(trapSiteDesc) {
    initOperand(0, def);
    initOperand(1, instance);

    setResultType(MIRType::Int64);
    setGuard();  // neither removable nor movable because of possible
                 // side-effects.
  }

 public:
  INSTRUCTION_HEADER(WasmBuiltinTruncateToInt64)
  NAMED_OPERANDS((0, input), (1, instance));
  TRIVIAL_NEW_WRAPPERS

  bool isUnsigned() const { return flags_ & TRUNC_UNSIGNED; }
  bool isSaturating() const { return flags_ & TRUNC_SATURATING; }
  TruncFlags flags() const { return flags_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmBuiltinTruncateToInt64()->flags() == flags_;
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MWasmTruncateToInt64 : public MUnaryInstruction,
                             public NoTypePolicy::Data {
  TruncFlags flags_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmTruncateToInt64(MDefinition* def, TruncFlags flags,
                       const wasm::TrapSiteDesc& trapSiteDesc)
      : MUnaryInstruction(classOpcode, def),
        flags_(flags),
        trapSiteDesc_(trapSiteDesc) {
    setResultType(MIRType::Int64);
    setGuard();  // neither removable nor movable because of possible
                 // side-effects.
  }

 public:
  INSTRUCTION_HEADER(WasmTruncateToInt64)
  TRIVIAL_NEW_WRAPPERS

  bool isUnsigned() const { return flags_ & TRUNC_UNSIGNED; }
  bool isSaturating() const { return flags_ & TRUNC_SATURATING; }
  TruncFlags flags() const { return flags_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmTruncateToInt64()->flags() == flags_;
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Truncate a value to an int32, with wasm semantics: this will trap when the
// value is out of range.
class MWasmTruncateToInt32 : public MUnaryInstruction,
                             public NoTypePolicy::Data {
  TruncFlags flags_;
  wasm::TrapSiteDesc trapSiteDesc_;

  explicit MWasmTruncateToInt32(MDefinition* def, TruncFlags flags,
                                const wasm::TrapSiteDesc& trapSiteDesc)
      : MUnaryInstruction(classOpcode, def),
        flags_(flags),
        trapSiteDesc_(trapSiteDesc) {
    setResultType(MIRType::Int32);
    setGuard();  // neither removable nor movable because of possible
                 // side-effects.
  }

 public:
  INSTRUCTION_HEADER(WasmTruncateToInt32)
  TRIVIAL_NEW_WRAPPERS

  bool isUnsigned() const { return flags_ & TRUNC_UNSIGNED; }
  bool isSaturating() const { return flags_ & TRUNC_SATURATING; }
  TruncFlags flags() const { return flags_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmTruncateToInt32()->flags() == flags_;
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// It is like MTruncateToInt32 but with instance dependency.
class MWasmBuiltinTruncateToInt32 : public MAryInstruction<2>,
                                    public ToInt32Policy::Data {
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmBuiltinTruncateToInt32(
      MDefinition* def, MDefinition* instance,
      wasm::TrapSiteDesc trapSiteDesc = wasm::TrapSiteDesc())
      : MAryInstruction(classOpcode), trapSiteDesc_(trapSiteDesc) {
    initOperand(0, def);
    initOperand(1, instance);
    setResultType(MIRType::Int32);
    setMovable();

    // Guard unless the conversion is known to be non-effectful & non-throwing.
    if (MTruncateToInt32::mightHaveSideEffects(def)) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(WasmBuiltinTruncateToInt32)
  NAMED_OPERANDS((0, input), (1, instance))
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  ALLOW_CLONE(MWasmBuiltinTruncateToInt32)
};

class MWasmBuiltinDivI64 : public MAryInstruction<3>, public ArithPolicy::Data {
  bool canBeNegativeZero_;
  bool canBeNegativeOverflow_;
  bool canBeDivideByZero_;
  bool canBeNegativeDividend_;
  bool unsigned_;  // If false, signedness will be derived from operands
  bool trapOnError_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmBuiltinDivI64(MDefinition* left, MDefinition* right,
                     MDefinition* instance)
      : MAryInstruction(classOpcode),
        canBeNegativeZero_(true),
        canBeNegativeOverflow_(true),
        canBeDivideByZero_(true),
        canBeNegativeDividend_(true),
        unsigned_(false),
        trapOnError_(false) {
    initOperand(0, left);
    initOperand(1, right);
    initOperand(2, instance);

    setResultType(MIRType::Int64);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmBuiltinDivI64)

  NAMED_OPERANDS((0, lhs), (1, rhs), (2, instance))

  static MWasmBuiltinDivI64* New(
      TempAllocator& alloc, MDefinition* left, MDefinition* right,
      MDefinition* instance, bool unsignd, bool trapOnError = false,
      wasm::TrapSiteDesc trapSiteDesc = wasm::TrapSiteDesc()) {
    auto* wasm64Div = new (alloc) MWasmBuiltinDivI64(left, right, instance);
    wasm64Div->unsigned_ = unsignd;
    wasm64Div->trapOnError_ = trapOnError;
    wasm64Div->trapSiteDesc_ = trapSiteDesc;
    if (trapOnError) {
      wasm64Div->setGuard();  // not removable because of possible side-effects.
      wasm64Div->setNotMovable();
    }
    return wasm64Div;
  }

  bool canBeNegativeZero() const { return canBeNegativeZero_; }
  void setCanBeNegativeZero(bool negativeZero) {
    canBeNegativeZero_ = negativeZero;
  }

  bool canBeNegativeOverflow() const { return canBeNegativeOverflow_; }

  bool canBeDivideByZero() const { return canBeDivideByZero_; }

  bool canBeNegativeDividend() const {
    // "Dividend" is an ambiguous concept for unsigned truncated
    // division, because of the truncation procedure:
    // ((x>>>0)/2)|0, for example, gets transformed in
    // MWasmDiv::truncate into a node with lhs representing x (not
    // x>>>0) and rhs representing the constant 2; in other words,
    // the MIR node corresponds to "cast operands to unsigned and
    // divide" operation. In this case, is the dividend x or is it
    // x>>>0? In order to resolve such ambiguities, we disallow
    // the usage of this method for unsigned division.
    MOZ_ASSERT(!unsigned_);
    return canBeNegativeDividend_;
  }

  bool isUnsigned() const { return unsigned_; }

  bool trapOnError() const { return trapOnError_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const {
    MOZ_ASSERT(trapSiteDesc_.isValid());
    return trapSiteDesc_;
  }

  ALLOW_CLONE(MWasmBuiltinDivI64)
};

class MWasmBuiltinModD : public MAryInstruction<3>, public ArithPolicy::Data {
  wasm::BytecodeOffset bytecodeOffset_;

  MWasmBuiltinModD(MDefinition* left, MDefinition* right, MDefinition* instance,
                   MIRType type)
      : MAryInstruction(classOpcode) {
    initOperand(0, left);
    initOperand(1, right);
    initOperand(2, instance);

    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmBuiltinModD)
  NAMED_OPERANDS((0, lhs), (1, rhs), (2, instance))

  static MWasmBuiltinModD* New(
      TempAllocator& alloc, MDefinition* left, MDefinition* right,
      MDefinition* instance, MIRType type,
      wasm::BytecodeOffset bytecodeOffset = wasm::BytecodeOffset()) {
    auto* wasmBuiltinModD =
        new (alloc) MWasmBuiltinModD(left, right, instance, type);
    wasmBuiltinModD->bytecodeOffset_ = bytecodeOffset;
    return wasmBuiltinModD;
  }

  wasm::BytecodeOffset bytecodeOffset() const {
    MOZ_ASSERT(bytecodeOffset_.isValid());
    return bytecodeOffset_;
  }

  ALLOW_CLONE(MWasmBuiltinModD)
};

class MWasmBuiltinModI64 : public MAryInstruction<3>, public ArithPolicy::Data {
  bool unsigned_;  // If false, signedness will be derived from operands
  bool canBeNegativeDividend_;
  bool canBeDivideByZero_;
  bool trapOnError_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmBuiltinModI64(MDefinition* left, MDefinition* right,
                     MDefinition* instance)
      : MAryInstruction(classOpcode),
        unsigned_(false),
        canBeNegativeDividend_(true),
        canBeDivideByZero_(true),
        trapOnError_(false) {
    initOperand(0, left);
    initOperand(1, right);
    initOperand(2, instance);

    setResultType(MIRType::Int64);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmBuiltinModI64)

  NAMED_OPERANDS((0, lhs), (1, rhs), (2, instance))

  static MWasmBuiltinModI64* New(
      TempAllocator& alloc, MDefinition* left, MDefinition* right,
      MDefinition* instance, bool unsignd, bool trapOnError = false,
      wasm::TrapSiteDesc trapSiteDesc = wasm::TrapSiteDesc()) {
    auto* mod = new (alloc) MWasmBuiltinModI64(left, right, instance);
    mod->unsigned_ = unsignd;
    mod->trapOnError_ = trapOnError;
    mod->trapSiteDesc_ = trapSiteDesc;
    if (trapOnError) {
      mod->setGuard();  // not removable because of possible side-effects.
      mod->setNotMovable();
    }
    return mod;
  }

  bool canBeNegativeDividend() const {
    MOZ_ASSERT(!unsigned_);
    return canBeNegativeDividend_;
  }

  bool canBeDivideByZero() const { return canBeDivideByZero_; }

  bool isUnsigned() const { return unsigned_; }

  bool trapOnError() const { return trapOnError_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const {
    MOZ_ASSERT(trapSiteDesc_.isValid());
    return trapSiteDesc_;
  }

  ALLOW_CLONE(MWasmBuiltinModI64)
};

// Check whether we need to fire the interrupt handler (in wasm code).
class MWasmInterruptCheck : public MUnaryInstruction,
                            public NoTypePolicy::Data {
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmInterruptCheck(MDefinition* instance,
                      const wasm::TrapSiteDesc& trapSiteDesc)
      : MUnaryInstruction(classOpcode, instance), trapSiteDesc_(trapSiteDesc) {
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(WasmInterruptCheck)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance))

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  ALLOW_CLONE(MWasmInterruptCheck)
};

// Directly jumps to the indicated trap, leaving Wasm code and reporting a
// runtime error.

class MWasmTrap : public MAryControlInstruction<0, 0>,
                  public NoTypePolicy::Data {
  wasm::Trap trap_;
  wasm::TrapSiteDesc trapSiteDesc_;

  explicit MWasmTrap(wasm::Trap trap, const wasm::TrapSiteDesc& trapSiteDesc)
      : MAryControlInstruction(classOpcode),
        trap_(trap),
        trapSiteDesc_(trapSiteDesc) {}

 public:
  INSTRUCTION_HEADER(WasmTrap)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  wasm::Trap trap() const { return trap_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }
};

// Flips the input's sign bit, independently of the rest of the number's
// payload. Note this is different from multiplying by minus-one, which has
// side-effects for e.g. NaNs.
class MWasmNeg : public MUnaryInstruction, public NoTypePolicy::Data {
  MWasmNeg(MDefinition* op, MIRType type) : MUnaryInstruction(classOpcode, op) {
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmNeg)
  TRIVIAL_NEW_WRAPPERS
  ALLOW_CLONE(MWasmNeg)
};

// Machine-level bitwise AND/OR/XOR, avoiding all JS-level complexity embodied
// in MBinaryBitwiseInstruction.
class MWasmBinaryBitwise : public MBinaryInstruction,
                           public NoTypePolicy::Data {
 public:
  enum class SubOpcode { And, Or, Xor };

 protected:
  MWasmBinaryBitwise(MDefinition* left, MDefinition* right, MIRType type,
                     SubOpcode subOpcode)
      : MBinaryInstruction(classOpcode, left, right), subOpcode_(subOpcode) {
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Int64);
    setResultType(type);
    setMovable();
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(WasmBinaryBitwise)
  TRIVIAL_NEW_WRAPPERS

  SubOpcode subOpcode() const { return subOpcode_; }
  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    return ins->isWasmBinaryBitwise() &&
           ins->toWasmBinaryBitwise()->subOpcode() == subOpcode() &&
           binaryCongruentTo(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    const char* what = "!!unknown!!";
    switch (subOpcode()) {
      case SubOpcode::And:
        what = "And";
        break;
      case SubOpcode::Or:
        what = "Or";
        break;
      case SubOpcode::Xor:
        what = "Xor";
        break;
    }
    extras->add(what);
  }
#endif

 private:
  SubOpcode subOpcode_;

  ALLOW_CLONE(MWasmBinaryBitwise)
};

class MWasmLoadInstance : public MUnaryInstruction, public NoTypePolicy::Data {
  uint32_t offset_;
  AliasSet aliases_;

  explicit MWasmLoadInstance(MDefinition* instance, uint32_t offset,
                             MIRType type, AliasSet aliases)
      : MUnaryInstruction(classOpcode, instance),
        offset_(offset),
        aliases_(aliases) {
    // Different instance data have different alias classes and only those
    // classes are allowed.
    MOZ_ASSERT(
        aliases_.flags() == AliasSet::Load(AliasSet::WasmHeapMeta).flags() ||
        aliases_.flags() == AliasSet::Load(AliasSet::WasmTableMeta).flags() ||
        aliases_.flags() ==
            AliasSet::Load(AliasSet::WasmPendingException).flags() ||
        aliases_.flags() == AliasSet::None().flags());

    // The only types supported at the moment.
    MOZ_ASSERT(type == MIRType::Pointer || type == MIRType::Int32 ||
               type == MIRType::Int64 || type == MIRType::WasmAnyRef);

    setMovable();
    setResultType(type);
  }

 public:
  INSTRUCTION_HEADER(WasmLoadInstance)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance))

  uint32_t offset() const { return offset_; }

  bool congruentTo(const MDefinition* ins) const override {
    return op() == ins->op() &&
           offset() == ins->toWasmLoadInstance()->offset() &&
           type() == ins->type();
  }

  HashNumber valueHash() const override {
    return addU32ToHash(HashNumber(op()), offset());
  }

  AliasSet getAliasSet() const override { return aliases_; }
};

class MWasmStoreInstance : public MBinaryInstruction,
                           public NoTypePolicy::Data {
  uint32_t offset_;
  AliasSet aliases_;

  explicit MWasmStoreInstance(MDefinition* instance, MDefinition* value,
                              uint32_t offset, MIRType type, AliasSet aliases)
      : MBinaryInstruction(classOpcode, instance, value),
        offset_(offset),
        aliases_(aliases) {
    // Different instance data have different alias classes and only those
    // classes are allowed.
    MOZ_ASSERT(aliases_.flags() ==
               AliasSet::Store(AliasSet::WasmPendingException).flags());

    // The only types supported at the moment.
    MOZ_ASSERT(type == MIRType::Pointer || type == MIRType::Int32 ||
               type == MIRType::Int64 || type == MIRType::WasmAnyRef);
  }

 public:
  INSTRUCTION_HEADER(WasmStoreInstance)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance), (1, value))

  uint32_t offset() const { return offset_; }

  AliasSet getAliasSet() const override { return aliases_; }
};

class MWasmHeapReg : public MNullaryInstruction {
  AliasSet aliases_;

  explicit MWasmHeapReg(AliasSet aliases)
      : MNullaryInstruction(classOpcode), aliases_(aliases) {
    setMovable();
    setResultType(MIRType::Pointer);
  }

 public:
  INSTRUCTION_HEADER(WasmHeapReg)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return ins->isWasmHeapReg();
  }

  AliasSet getAliasSet() const override { return aliases_; }
};

// For memory32, bounds check nodes are of type Int32 on 32-bit systems for both
// wasm and asm.js code, as well as on 64-bit systems for asm.js code and for
// wasm code that is known to have a bounds check limit that fits into 32 bits.
// They are of type Int64 only on 64-bit systems for wasm code with 4GB heaps.
// There is no way for nodes of both types to be present in the same function.
// Should this change, then BCE must be updated to take type into account.
//
// For memory64, bounds check nodes are always of type Int64.

class MWasmBoundsCheck : public MBinaryInstruction, public NoTypePolicy::Data {
 public:
  enum Target {
    // Linear memory at index zero, which is the only memory allowed so far.
    Memory0,
    // Everything else.  Currently comprises tables, and arrays in the GC
    // proposal.
    Unknown
  };

 private:
  wasm::TrapSiteDesc trapSiteDesc_;
  Target target_;

  explicit MWasmBoundsCheck(MDefinition* index, MDefinition* boundsCheckLimit,
                            const wasm::TrapSiteDesc& trapSiteDesc,
                            Target target)
      : MBinaryInstruction(classOpcode, index, boundsCheckLimit),
        trapSiteDesc_(trapSiteDesc),
        target_(target) {
    MOZ_ASSERT(index->type() == boundsCheckLimit->type());

    // Bounds check is effectful: it throws for OOB.
    setGuard();

    if (JitOptions.spectreIndexMasking) {
      setResultType(index->type());
    }
  }

 public:
  INSTRUCTION_HEADER(WasmBoundsCheck)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, index), (1, boundsCheckLimit))

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool isMemory0() const { return target_ == MWasmBoundsCheck::Memory0; }

  bool isRedundant() const { return !isGuard(); }

  void setRedundant() { setNotGuard(); }

  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  ALLOW_CLONE(MWasmBoundsCheck)
};

class MWasmAddOffset : public MUnaryInstruction, public NoTypePolicy::Data {
  uint64_t offset_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmAddOffset(MDefinition* base, uint64_t offset,
                 const wasm::TrapSiteDesc& trapSiteDesc)
      : MUnaryInstruction(classOpcode, base),
        offset_(offset),
        trapSiteDesc_(trapSiteDesc) {
    setGuard();
    MOZ_ASSERT(base->type() == MIRType::Int32 ||
               base->type() == MIRType::Int64);
    setResultType(base->type());
  }

 public:
  INSTRUCTION_HEADER(WasmAddOffset)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, base))

  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  uint64_t offset() const { return offset_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }
};

class MWasmAlignmentCheck : public MUnaryInstruction,
                            public NoTypePolicy::Data {
  uint32_t byteSize_;
  wasm::TrapSiteDesc trapSiteDesc_;

  explicit MWasmAlignmentCheck(MDefinition* index, uint32_t byteSize,
                               const wasm::TrapSiteDesc& trapSiteDesc)
      : MUnaryInstruction(classOpcode, index),
        byteSize_(byteSize),
        trapSiteDesc_(trapSiteDesc) {
    MOZ_ASSERT(mozilla::IsPowerOfTwo(byteSize));
    // Alignment check is effectful: it throws for unaligned.
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(WasmAlignmentCheck)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, index))

  bool congruentTo(const MDefinition* ins) const override;

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  uint32_t byteSize() const { return byteSize_; }

  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  ALLOW_CLONE(MWasmAlignmentCheck)
};

class MWasmLoad
    : public MVariadicInstruction,  // memoryBase is nullptr on some platforms
      public NoTypePolicy::Data {
  wasm::MemoryAccessDesc access_;

  explicit MWasmLoad(const wasm::MemoryAccessDesc& access, MIRType resultType)
      : MVariadicInstruction(classOpcode), access_(access) {
    setGuard();
    setResultType(resultType);
  }

 public:
  INSTRUCTION_HEADER(WasmLoad)
  NAMED_OPERANDS((0, base), (1, memoryBase));

  static MWasmLoad* New(TempAllocator& alloc, MDefinition* memoryBase,
                        MDefinition* base, const wasm::MemoryAccessDesc& access,
                        MIRType resultType) {
    MWasmLoad* load = new (alloc) MWasmLoad(access, resultType);
    if (!load->init(alloc, 1 + !!memoryBase)) {
      return nullptr;
    }

    load->initOperand(0, base);
    if (memoryBase) {
      load->initOperand(1, memoryBase);
    }

    return load;
  }

  const wasm::MemoryAccessDesc& access() const { return access_; }

  AliasSet getAliasSet() const override {
    // When a barrier is needed, make the instruction effectful by giving
    // it a "store" effect.
    if (access_.isAtomic()) {
      return AliasSet::Store(AliasSet::WasmHeap);
    }
    return AliasSet::Load(AliasSet::WasmHeap);
  }

  bool hasMemoryBase() const { return numOperands() > 1; }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "(offs=%lld)", (long long int)access().offset64());
    extras->add(buf);
  }
#endif

  // Unfortunately we cannot use ALLOW_CLONE here, due to the variable number
  // of operands.
  bool canClone() const override { return true; }
  MInstruction* clone(TempAllocator& alloc,
                      const MDefinitionVector& inputs) const override {
    MInstruction* res =
        MWasmLoad::New(alloc, hasMemoryBase() ? memoryBase() : nullptr, base(),
                       access(), type());
    if (!res) {
      return nullptr;
    }
    for (size_t i = 0; i < numOperands(); i++) {
      res->replaceOperand(i, inputs[i]);
    }
    return res;
  }
};

class MWasmStore : public MVariadicInstruction, public NoTypePolicy::Data {
  wasm::MemoryAccessDesc access_;

  explicit MWasmStore(const wasm::MemoryAccessDesc& access)
      : MVariadicInstruction(classOpcode), access_(access) {
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(WasmStore)
  NAMED_OPERANDS((0, base), (1, value), (2, memoryBase))

  static MWasmStore* New(TempAllocator& alloc, MDefinition* memoryBase,
                         MDefinition* base,
                         const wasm::MemoryAccessDesc& access,
                         MDefinition* value) {
    MWasmStore* store = new (alloc) MWasmStore(access);
    if (!store->init(alloc, 2 + !!memoryBase)) {
      return nullptr;
    }

    store->initOperand(0, base);
    store->initOperand(1, value);
    if (memoryBase) {
      store->initOperand(2, memoryBase);
    }

    return store;
  }

  const wasm::MemoryAccessDesc& access() const { return access_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmHeap);
  }

  bool hasMemoryBase() const { return numOperands() > 2; }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "(offs=%lld)", (long long int)access().offset64());
    extras->add(buf);
  }
#endif

  bool canClone() const override { return true; }
  MInstruction* clone(TempAllocator& alloc,
                      const MDefinitionVector& inputs) const override {
    MInstruction* res =
        MWasmStore::New(alloc, hasMemoryBase() ? memoryBase() : nullptr, base(),
                        access(), value());
    if (!res) {
      return nullptr;
    }
    for (size_t i = 0; i < numOperands(); i++) {
      res->replaceOperand(i, inputs[i]);
    }
    return res;
  }
};

class MAsmJSMemoryAccess {
  Scalar::Type accessType_;
  bool needsBoundsCheck_;

 public:
  explicit MAsmJSMemoryAccess(Scalar::Type accessType)
      : accessType_(accessType), needsBoundsCheck_(true) {
    MOZ_ASSERT(accessType != Scalar::Uint8Clamped);
  }

  Scalar::Type accessType() const { return accessType_; }
  unsigned byteSize() const { return TypedArrayElemSize(accessType()); }
  bool needsBoundsCheck() const { return needsBoundsCheck_; }

  wasm::MemoryAccessDesc access() const {
    return wasm::MemoryAccessDesc(0, accessType_, Scalar::byteSize(accessType_),
                                  0, wasm::TrapSiteDesc(), false);
  }

  void removeBoundsCheck() { needsBoundsCheck_ = false; }
};

class MAsmJSLoadHeap
    : public MVariadicInstruction,  // 1 plus optional memoryBase and
                                    // boundsCheckLimit
      public MAsmJSMemoryAccess,
      public NoTypePolicy::Data {
  uint32_t memoryBaseIndex_;

  explicit MAsmJSLoadHeap(uint32_t memoryBaseIndex, Scalar::Type accessType)
      : MVariadicInstruction(classOpcode),
        MAsmJSMemoryAccess(accessType),
        memoryBaseIndex_(memoryBaseIndex) {
    setResultType(ScalarTypeToMIRType(accessType));
  }

 public:
  INSTRUCTION_HEADER(AsmJSLoadHeap)
  NAMED_OPERANDS((0, base), (1, boundsCheckLimit))

  static MAsmJSLoadHeap* New(TempAllocator& alloc, MDefinition* memoryBase,
                             MDefinition* base, MDefinition* boundsCheckLimit,
                             Scalar::Type accessType) {
    uint32_t nextIndex = 2;
    uint32_t memoryBaseIndex = memoryBase ? nextIndex++ : UINT32_MAX;

    MAsmJSLoadHeap* load =
        new (alloc) MAsmJSLoadHeap(memoryBaseIndex, accessType);
    if (!load->init(alloc, nextIndex)) {
      return nullptr;
    }

    load->initOperand(0, base);
    load->initOperand(1, boundsCheckLimit);
    if (memoryBase) {
      load->initOperand(memoryBaseIndex, memoryBase);
    }

    return load;
  }

  bool hasMemoryBase() const { return memoryBaseIndex_ != UINT32_MAX; }
  MDefinition* memoryBase() const {
    MOZ_ASSERT(hasMemoryBase());
    return getOperand(memoryBaseIndex_);
  }

  bool congruentTo(const MDefinition* ins) const override;
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::WasmHeap);
  }
  AliasType mightAlias(const MDefinition* def) const override;
};

class MAsmJSStoreHeap
    : public MVariadicInstruction,  // 2 plus optional memoryBase and
                                    // boundsCheckLimit
      public MAsmJSMemoryAccess,
      public NoTypePolicy::Data {
  uint32_t memoryBaseIndex_;

  explicit MAsmJSStoreHeap(uint32_t memoryBaseIndex, Scalar::Type accessType)
      : MVariadicInstruction(classOpcode),
        MAsmJSMemoryAccess(accessType),
        memoryBaseIndex_(memoryBaseIndex) {}

 public:
  INSTRUCTION_HEADER(AsmJSStoreHeap)
  NAMED_OPERANDS((0, base), (1, value), (2, boundsCheckLimit))

  static MAsmJSStoreHeap* New(TempAllocator& alloc, MDefinition* memoryBase,
                              MDefinition* base, MDefinition* boundsCheckLimit,
                              Scalar::Type accessType, MDefinition* v) {
    uint32_t nextIndex = 3;
    uint32_t memoryBaseIndex = memoryBase ? nextIndex++ : UINT32_MAX;

    MAsmJSStoreHeap* store =
        new (alloc) MAsmJSStoreHeap(memoryBaseIndex, accessType);
    if (!store->init(alloc, nextIndex)) {
      return nullptr;
    }

    store->initOperand(0, base);
    store->initOperand(1, v);
    store->initOperand(2, boundsCheckLimit);
    if (memoryBase) {
      store->initOperand(memoryBaseIndex, memoryBase);
    }

    return store;
  }

  bool hasMemoryBase() const { return memoryBaseIndex_ != UINT32_MAX; }
  MDefinition* memoryBase() const {
    MOZ_ASSERT(hasMemoryBase());
    return getOperand(memoryBaseIndex_);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmHeap);
  }
};

class MWasmCompareExchangeHeap : public MVariadicInstruction,
                                 public NoTypePolicy::Data {
  wasm::MemoryAccessDesc access_;
  wasm::BytecodeOffset bytecodeOffset_;

  explicit MWasmCompareExchangeHeap(const wasm::MemoryAccessDesc& access,
                                    wasm::BytecodeOffset bytecodeOffset)
      : MVariadicInstruction(classOpcode),
        access_(access),
        bytecodeOffset_(bytecodeOffset) {
    setGuard();  // Not removable
    setResultType(ScalarTypeToMIRType(access.type()));
  }

 public:
  INSTRUCTION_HEADER(WasmCompareExchangeHeap)
  NAMED_OPERANDS((0, base), (1, oldValue), (2, newValue), (3, instance),
                 (4, memoryBase))

  static MWasmCompareExchangeHeap* New(TempAllocator& alloc,
                                       wasm::BytecodeOffset bytecodeOffset,
                                       MDefinition* memoryBase,
                                       MDefinition* base,
                                       const wasm::MemoryAccessDesc& access,
                                       MDefinition* oldv, MDefinition* newv,
                                       MDefinition* instance) {
    MWasmCompareExchangeHeap* cas =
        new (alloc) MWasmCompareExchangeHeap(access, bytecodeOffset);
    if (!cas->init(alloc, 4 + !!memoryBase)) {
      return nullptr;
    }
    cas->initOperand(0, base);
    cas->initOperand(1, oldv);
    cas->initOperand(2, newv);
    cas->initOperand(3, instance);
    if (memoryBase) {
      cas->initOperand(4, memoryBase);
    }
    return cas;
  }

  const wasm::MemoryAccessDesc& access() const { return access_; }
  wasm::BytecodeOffset bytecodeOffset() const { return bytecodeOffset_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmHeap);
  }

  bool hasMemoryBase() const { return numOperands() > 4; }
};

class MWasmAtomicExchangeHeap : public MVariadicInstruction,
                                public NoTypePolicy::Data {
  wasm::MemoryAccessDesc access_;
  wasm::BytecodeOffset bytecodeOffset_;

  explicit MWasmAtomicExchangeHeap(const wasm::MemoryAccessDesc& access,
                                   wasm::BytecodeOffset bytecodeOffset)
      : MVariadicInstruction(classOpcode),
        access_(access),
        bytecodeOffset_(bytecodeOffset) {
    setGuard();  // Not removable
    setResultType(ScalarTypeToMIRType(access.type()));
  }

 public:
  INSTRUCTION_HEADER(WasmAtomicExchangeHeap)
  NAMED_OPERANDS((0, base), (1, value), (2, instance), (3, memoryBase))

  static MWasmAtomicExchangeHeap* New(TempAllocator& alloc,
                                      wasm::BytecodeOffset bytecodeOffset,
                                      MDefinition* memoryBase,
                                      MDefinition* base,
                                      const wasm::MemoryAccessDesc& access,
                                      MDefinition* value,
                                      MDefinition* instance) {
    MWasmAtomicExchangeHeap* xchg =
        new (alloc) MWasmAtomicExchangeHeap(access, bytecodeOffset);
    if (!xchg->init(alloc, 3 + !!memoryBase)) {
      return nullptr;
    }

    xchg->initOperand(0, base);
    xchg->initOperand(1, value);
    xchg->initOperand(2, instance);
    if (memoryBase) {
      xchg->initOperand(3, memoryBase);
    }

    return xchg;
  }

  const wasm::MemoryAccessDesc& access() const { return access_; }
  wasm::BytecodeOffset bytecodeOffset() const { return bytecodeOffset_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmHeap);
  }

  bool hasMemoryBase() const { return numOperands() > 3; }
};

class MWasmAtomicBinopHeap : public MVariadicInstruction,
                             public NoTypePolicy::Data {
  AtomicOp op_;
  wasm::MemoryAccessDesc access_;
  wasm::BytecodeOffset bytecodeOffset_;

  explicit MWasmAtomicBinopHeap(AtomicOp op,
                                const wasm::MemoryAccessDesc& access,
                                wasm::BytecodeOffset bytecodeOffset)
      : MVariadicInstruction(classOpcode),
        op_(op),
        access_(access),
        bytecodeOffset_(bytecodeOffset) {
    setGuard();  // Not removable
    setResultType(ScalarTypeToMIRType(access.type()));
  }

 public:
  INSTRUCTION_HEADER(WasmAtomicBinopHeap)
  NAMED_OPERANDS((0, base), (1, value), (2, instance), (3, memoryBase))

  static MWasmAtomicBinopHeap* New(TempAllocator& alloc,
                                   wasm::BytecodeOffset bytecodeOffset,
                                   AtomicOp op, MDefinition* memoryBase,
                                   MDefinition* base,
                                   const wasm::MemoryAccessDesc& access,
                                   MDefinition* v, MDefinition* instance) {
    MWasmAtomicBinopHeap* binop =
        new (alloc) MWasmAtomicBinopHeap(op, access, bytecodeOffset);
    if (!binop->init(alloc, 3 + !!memoryBase)) {
      return nullptr;
    }

    binop->initOperand(0, base);
    binop->initOperand(1, v);
    binop->initOperand(2, instance);
    if (memoryBase) {
      binop->initOperand(3, memoryBase);
    }

    return binop;
  }

  AtomicOp operation() const { return op_; }
  const wasm::MemoryAccessDesc& access() const { return access_; }
  wasm::BytecodeOffset bytecodeOffset() const { return bytecodeOffset_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmHeap);
  }

  bool hasMemoryBase() const { return numOperands() > 3; }
};

class MWasmLoadInstanceDataField : public MUnaryInstruction,
                                   public NoTypePolicy::Data {
  unsigned instanceDataOffset_;
  bool isConstant_;

  MWasmLoadInstanceDataField(
      MIRType type, unsigned instanceDataOffset, bool isConstant,
      MDefinition* instance,
      wasm::MaybeRefType maybeRefType = wasm::MaybeRefType())
      : MUnaryInstruction(classOpcode, instance),
        instanceDataOffset_(instanceDataOffset),
        isConstant_(isConstant) {
    MOZ_ASSERT(IsNumberType(type) || type == MIRType::Simd128 ||
               type == MIRType::Pointer || type == MIRType::WasmAnyRef);
    setResultType(type);
    setMovable();
    initWasmRefType(maybeRefType);
  }

 public:
  INSTRUCTION_HEADER(WasmLoadInstanceDataField)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance))

  unsigned instanceDataOffset() const { return instanceDataOffset_; }

  HashNumber valueHash() const override;
  bool congruentTo(const MDefinition* ins) const override;
  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override {
    return isConstant_ ? AliasSet::None()
                       : AliasSet::Load(AliasSet::WasmInstanceData);
  }

  AliasType mightAlias(const MDefinition* def) const override;

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[96];
    SprintfLiteral(buf, "(offs=%lld, isConst=%s)",
                   (long long int)instanceDataOffset_,
                   isConstant_ ? "true" : "false");
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MWasmLoadInstanceDataField)
};

class MWasmLoadGlobalCell : public MUnaryInstruction,
                            public NoTypePolicy::Data {
  MWasmLoadGlobalCell(MIRType type, MDefinition* cellPtr,
                      wasm::ValType globalType)
      : MUnaryInstruction(classOpcode, cellPtr) {
    setResultType(type);
    setMovable();
    initWasmRefType(globalType.toMaybeRefType());
  }

 public:
  INSTRUCTION_HEADER(WasmLoadGlobalCell)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, cellPtr))

  // The default valueHash is good enough, because there are no non-operand
  // fields.
  bool congruentTo(const MDefinition* ins) const override;

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::WasmGlobalCell);
  }

  AliasType mightAlias(const MDefinition* def) const override;

  ALLOW_CLONE(MWasmLoadGlobalCell)
};

class MWasmLoadTableElement : public MBinaryInstruction,
                              public NoTypePolicy::Data {
  MWasmLoadTableElement(MDefinition* elements, MDefinition* index,
                        wasm::RefType refType)
      : MBinaryInstruction(classOpcode, elements, index) {
    setResultType(MIRType::WasmAnyRef);
    setMovable();
    initWasmRefType(wasm::MaybeRefType(refType));
  }

 public:
  INSTRUCTION_HEADER(WasmLoadTableElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements))
  NAMED_OPERANDS((1, index))

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::WasmTableElement);
  }
};

class MWasmStoreInstanceDataField : public MBinaryInstruction,
                                    public NoTypePolicy::Data {
  MWasmStoreInstanceDataField(unsigned instanceDataOffset, MDefinition* value,
                              MDefinition* instance)
      : MBinaryInstruction(classOpcode, value, instance),
        instanceDataOffset_(instanceDataOffset) {}

  unsigned instanceDataOffset_;

 public:
  INSTRUCTION_HEADER(WasmStoreInstanceDataField)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value), (1, instance))

  unsigned instanceDataOffset() const { return instanceDataOffset_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmInstanceData);
  }
};

class MWasmStoreGlobalCell : public MBinaryInstruction,
                             public NoTypePolicy::Data {
  MWasmStoreGlobalCell(MDefinition* value, MDefinition* cellPtr)
      : MBinaryInstruction(classOpcode, value, cellPtr) {}

 public:
  INSTRUCTION_HEADER(WasmStoreGlobalCell)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value), (1, cellPtr))

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmGlobalCell);
  }

  ALLOW_CLONE(MWasmStoreGlobalCell)
};

class MWasmStoreStackResult : public MBinaryInstruction,
                              public NoTypePolicy::Data {
  MWasmStoreStackResult(MDefinition* stackResultArea, uint32_t offset,
                        MDefinition* value)
      : MBinaryInstruction(classOpcode, stackResultArea, value),
        offset_(offset) {}

  uint32_t offset_;

 public:
  INSTRUCTION_HEADER(WasmStoreStackResult)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, stackResultArea), (1, value))

  uint32_t offset() const { return offset_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmStackResult);
  }
};

// Represents a known-good derived pointer into an object or memory region (in
// the most general sense) that will not move while the derived pointer is live.
// The `offset` *must* be a valid offset into the object represented by `base`;
// hence overflow in the address calculation will never be an issue.  `offset`
// must be representable as a 31-bit unsigned integer.
//
// DO NOT use this with a base value of any JS-heap-resident object type.
// Such a value would need to be adjusted during GC, yet we have no mechanism
// to do that.  See bug 1810090.

class MWasmDerivedPointer : public MUnaryInstruction,
                            public NoTypePolicy::Data {
  MWasmDerivedPointer(MDefinition* base, size_t offset)
      : MUnaryInstruction(classOpcode, base), offset_(uint32_t(offset)) {
    MOZ_ASSERT(offset <= INT32_MAX);
    // Do not change this to allow `base` to be a GC-heap allocated type.
    MOZ_ASSERT(base->type() == MIRType::Pointer ||
               base->type() == TargetWordMIRType());
    setResultType(MIRType::Pointer);
    setMovable();
  }

  uint32_t offset_;

 public:
  INSTRUCTION_HEADER(WasmDerivedPointer)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, base))

  uint32_t offset() const { return offset_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmDerivedPointer()->offset() == offset();
  }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "(offs=%lld)", (long long int)offset_);
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MWasmDerivedPointer)
};

// As with MWasmDerivedPointer, DO NOT use this with a base value of any
// JS-heap-resident object type.
class MWasmDerivedIndexPointer : public MBinaryInstruction,
                                 public NoTypePolicy::Data {
  MWasmDerivedIndexPointer(MDefinition* base, MDefinition* index, Scale scale)
      : MBinaryInstruction(classOpcode, base, index), scale_(scale) {
    // Do not change this to allow `base` to be a GC-heap allocated type.
    MOZ_ASSERT(base->type() == MIRType::Pointer);
    setResultType(MIRType::Pointer);
    setMovable();
  }

  Scale scale_;

 public:
  INSTRUCTION_HEADER(WasmDerivedIndexPointer)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, base))
  NAMED_OPERANDS((1, index))

  Scale scale() const { return scale_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmDerivedIndexPointer()->scale() == scale();
  }

  ALLOW_CLONE(MWasmDerivedIndexPointer)
};

// Whether to perform a pre-write barrier for a wasm store reference.
enum class WasmPreBarrierKind : uint8_t { None, Normal };

// Stores a reference to an address. This performs a pre-barrier on the address,
// but not a post-barrier. A post-barrier must be performed separately, if it's
// required.  The accessed location is `valueBase + valueOffset`.  The latter
// must be be representable as a 31-bit unsigned integer.

class MWasmStoreRef : public MAryInstruction<3>, public NoTypePolicy::Data {
  uint32_t offset_;
  AliasSet::Flag aliasSet_;
  WasmPreBarrierKind preBarrierKind_;

  MWasmStoreRef(MDefinition* instance, MDefinition* valueBase,
                size_t valueOffset, MDefinition* value, AliasSet::Flag aliasSet,
                WasmPreBarrierKind preBarrierKind)
      : MAryInstruction<3>(classOpcode),
        offset_(uint32_t(valueOffset)),
        aliasSet_(aliasSet),
        preBarrierKind_(preBarrierKind) {
    MOZ_ASSERT(valueOffset <= INT32_MAX);
    MOZ_ASSERT(valueBase->type() == MIRType::Pointer ||
               valueBase->type() == MIRType::StackResults);
    MOZ_ASSERT(value->type() == MIRType::WasmAnyRef);
    initOperand(0, instance);
    initOperand(1, valueBase);
    initOperand(2, value);
  }

 public:
  INSTRUCTION_HEADER(WasmStoreRef)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance), (1, valueBase), (2, value))

  uint32_t offset() const { return offset_; }
  AliasSet getAliasSet() const override { return AliasSet::Store(aliasSet_); }
  WasmPreBarrierKind preBarrierKind() const { return preBarrierKind_; }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "(offs=%lld)", (long long int)offset_);
    extras->add(buf);
  }
#endif
};

// Given a value being written to another object, update the generational store
// buffer if the value is in the nursery and object is in the tenured heap.
class MWasmPostWriteBarrierWholeCell : public MTernaryInstruction,
                                       public NoTypePolicy::Data {
  MWasmPostWriteBarrierWholeCell(MDefinition* instance, MDefinition* object,
                                 MDefinition* value)
      : MTernaryInstruction(classOpcode, instance, object, value) {
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(WasmPostWriteBarrierWholeCell)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance), (1, object), (2, value))

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MWasmPostWriteBarrierWholeCell)
};

// Given a value being written to another object, update the generational store
// buffer if the value is in the nursery and object is in the tenured heap.
class MWasmPostWriteBarrierEdgeAtIndex : public MAryInstruction<5>,
                                         public NoTypePolicy::Data {
  uint32_t elemSize_;

  MWasmPostWriteBarrierEdgeAtIndex(MDefinition* instance, MDefinition* object,
                                   MDefinition* valueBase, MDefinition* index,
                                   uint32_t scale, MDefinition* value)
      : MAryInstruction<5>(classOpcode), elemSize_(scale) {
    initOperand(0, instance);
    initOperand(1, object);
    initOperand(2, valueBase);
    initOperand(3, index);
    initOperand(4, value);
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(WasmPostWriteBarrierEdgeAtIndex)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance), (1, object), (2, valueBase), (3, index),
                 (4, value))

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  uint32_t elemSize() const { return elemSize_; }

  ALLOW_CLONE(MWasmPostWriteBarrierEdgeAtIndex)
};

class MWasmParameter : public MNullaryInstruction {
  ABIArg abi_;

  MWasmParameter(ABIArg abi, MIRType mirType,
                 wasm::MaybeRefType refType = wasm::MaybeRefType())
      : MNullaryInstruction(classOpcode), abi_(abi) {
    setResultType(mirType);
    initWasmRefType(refType);
  }

 public:
  INSTRUCTION_HEADER(WasmParameter)
  TRIVIAL_NEW_WRAPPERS

  ABIArg abi() const { return abi_; }
};

class MWasmReturn : public MAryControlInstruction<2, 0>,
                    public NoTypePolicy::Data {
  MWasmReturn(MDefinition* ins, MDefinition* instance)
      : MAryControlInstruction(classOpcode) {
    initOperand(0, ins);
    initOperand(1, instance);
  }

 public:
  INSTRUCTION_HEADER(WasmReturn)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MWasmReturnVoid : public MAryControlInstruction<1, 0>,
                        public NoTypePolicy::Data {
  explicit MWasmReturnVoid(MDefinition* instance)
      : MAryControlInstruction(classOpcode) {
    initOperand(0, instance);
  }

 public:
  INSTRUCTION_HEADER(WasmReturnVoid)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MWasmStackArg : public MUnaryInstruction, public NoTypePolicy::Data {
  MWasmStackArg(uint32_t spOffset, MDefinition* ins)
      : MUnaryInstruction(classOpcode, ins), spOffset_(spOffset) {}

  uint32_t spOffset_;

 public:
  INSTRUCTION_HEADER(WasmStackArg)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, arg))

  uint32_t spOffset() const { return spOffset_; }
  void incrementOffset(uint32_t inc) { spOffset_ += inc; }

  ALLOW_CLONE(MWasmStackArg)
};

template <typename Location>
class MWasmResultBase : public MNullaryInstruction {
  Location loc_;

 protected:
  MWasmResultBase(Opcode op, MIRType type, Location loc)
      : MNullaryInstruction(op), loc_(loc) {
    setResultType(type);
    setCallResultCapture();
  }

 public:
  Location loc() { return loc_; }
};

class MWasmRegisterResult : public MWasmResultBase<Register> {
  MWasmRegisterResult(MIRType type, Register reg,
                      wasm::MaybeRefType maybeRefType = wasm::MaybeRefType())
      : MWasmResultBase(classOpcode, type, reg) {
    initWasmRefType(maybeRefType);
  }

 public:
  INSTRUCTION_HEADER(WasmRegisterResult)
  TRIVIAL_NEW_WRAPPERS
};

class MWasmFloatRegisterResult : public MWasmResultBase<FloatRegister> {
  MWasmFloatRegisterResult(MIRType type, FloatRegister reg)
      : MWasmResultBase(classOpcode, type, reg) {}

 public:
  INSTRUCTION_HEADER(WasmFloatRegisterResult)
  TRIVIAL_NEW_WRAPPERS
};

class MWasmRegister64Result : public MWasmResultBase<Register64> {
  explicit MWasmRegister64Result(Register64 reg)
      : MWasmResultBase(classOpcode, MIRType::Int64, reg) {}

 public:
  INSTRUCTION_HEADER(WasmRegister64Result)
  TRIVIAL_NEW_WRAPPERS
};

class MWasmStackResultArea : public MNullaryInstruction {
 public:
  class StackResult {
    // Offset in bytes from lowest address of stack result area.
    uint32_t offset_;
    MIRType type_;

   public:
    StackResult() : type_(MIRType::Undefined) {}
    StackResult(uint32_t offset, MIRType type) : offset_(offset), type_(type) {}

    bool initialized() const { return type_ != MIRType::Undefined; }
    uint32_t offset() const {
      MOZ_ASSERT(initialized());
      return offset_;
    }
    MIRType type() const {
      MOZ_ASSERT(initialized());
      return type_;
    }
    uint32_t endOffset() const {
      return offset() + wasm::MIRTypeToABIResultSize(type());
    }
  };

 private:
  FixedList<StackResult> results_;
  uint32_t base_;

  explicit MWasmStackResultArea()
      : MNullaryInstruction(classOpcode), base_(UINT32_MAX) {
    setResultType(MIRType::StackResults);
  }

  void assertInitialized() const {
    MOZ_ASSERT(results_.length() != 0);
#ifdef DEBUG
    for (size_t i = 0; i < results_.length(); i++) {
      MOZ_ASSERT(results_[i].initialized());
    }
#endif
  }

  bool baseInitialized() const { return base_ != UINT32_MAX; }

 public:
  INSTRUCTION_HEADER(WasmStackResultArea)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool init(TempAllocator& alloc, size_t stackResultCount) {
    MOZ_ASSERT(results_.length() == 0);
    MOZ_ASSERT(stackResultCount > 0);
    if (!results_.init(alloc, stackResultCount)) {
      return false;
    }
    for (size_t n = 0; n < stackResultCount; n++) {
      results_[n] = StackResult();
    }
    return true;
  }

  size_t resultCount() const { return results_.length(); }
  const StackResult& result(size_t n) const {
    MOZ_ASSERT(results_[n].initialized());
    return results_[n];
  }
  void initResult(size_t n, const StackResult& loc) {
    MOZ_ASSERT(!results_[n].initialized());
    MOZ_ASSERT((n == 0) == (loc.offset() == 0));
    MOZ_ASSERT_IF(n > 0, loc.offset() >= result(n - 1).endOffset());
    results_[n] = loc;
  }

  uint32_t byteSize() const {
    assertInitialized();
    return result(resultCount() - 1).endOffset();
  }

  // Stack index indicating base of stack area.
  uint32_t base() const {
    MOZ_ASSERT(baseInitialized());
    return base_;
  }
  void setBase(uint32_t base) {
    MOZ_ASSERT(!baseInitialized());
    base_ = base;
    MOZ_ASSERT(baseInitialized());
  }
};

class MWasmStackResult : public MUnaryInstruction, public NoTypePolicy::Data {
  uint32_t resultIdx_;

  MWasmStackResult(MWasmStackResultArea* resultArea, size_t idx)
      : MUnaryInstruction(classOpcode, resultArea), resultIdx_(idx) {
    setResultType(result().type());
    setCallResultCapture();
  }

 public:
  INSTRUCTION_HEADER(WasmStackResult)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, resultArea))

  const MWasmStackResultArea::StackResult& result() const {
    return resultArea()->toWasmStackResultArea()->result(resultIdx_);
  }
};

// Mixin class for wasm calls that may or may not be catchable.
class MWasmCallBase {
 public:
  struct Arg {
    AnyRegister reg;
    MDefinition* def;
    Arg(AnyRegister reg, MDefinition* def) : reg(reg), def(def) {}
  };
  using Args = Vector<Arg, 8, SystemAllocPolicy>;

 protected:
  wasm::CallSiteDesc desc_;
  wasm::CalleeDesc callee_;
  wasm::FailureMode builtinMethodFailureMode_;
  FixedList<AnyRegister> argRegs_;
  uint32_t stackArgAreaSizeUnaligned_;
  ABIArg instanceArg_;
  bool inTry_;
  size_t tryNoteIndex_;

  MWasmCallBase(const wasm::CallSiteDesc& desc, const wasm::CalleeDesc& callee,
                uint32_t stackArgAreaSizeUnaligned, bool inTry,
                size_t tryNoteIndex)
      : desc_(desc),
        callee_(callee),
        builtinMethodFailureMode_(wasm::FailureMode::Infallible),
        stackArgAreaSizeUnaligned_(stackArgAreaSizeUnaligned),
        inTry_(inTry),
        tryNoteIndex_(tryNoteIndex) {}

  template <class MVariadicT>
  [[nodiscard]] bool initWithArgs(TempAllocator& alloc, MVariadicT* ins,
                                  const Args& args,
                                  MDefinition* tableAddressOrRef) {
    if (!argRegs_.init(alloc, args.length())) {
      return false;
    }
    for (size_t i = 0; i < argRegs_.length(); i++) {
      argRegs_[i] = args[i].reg;
    }

    if (!ins->init(alloc, argRegs_.length() + (tableAddressOrRef ? 1 : 0))) {
      return false;
    }
    // FixedList doesn't initialize its elements, so do an unchecked init.
    for (size_t i = 0; i < argRegs_.length(); i++) {
      ins->initOperand(i, args[i].def);
    }
    if (tableAddressOrRef) {
      ins->initOperand(argRegs_.length(), tableAddressOrRef);
    }
    return true;
  }

 public:
  static bool IsWasmCall(MDefinition* def) {
    return def->isWasmCallCatchable() || def->isWasmCallUncatchable() ||
           def->isWasmReturnCall();
  }

  size_t numArgs() const { return argRegs_.length(); }
  AnyRegister registerForArg(size_t index) const {
    MOZ_ASSERT(index < numArgs());
    return argRegs_[index];
  }
  const wasm::CallSiteDesc& desc() const { return desc_; }
  const wasm::CalleeDesc& callee() const { return callee_; }
  wasm::FailureMode builtinMethodFailureMode() const {
    MOZ_ASSERT(callee_.which() == wasm::CalleeDesc::BuiltinInstanceMethod);
    return builtinMethodFailureMode_;
  }
  uint32_t stackArgAreaSizeUnaligned() const {
    return stackArgAreaSizeUnaligned_;
  }

  const ABIArg& instanceArg() const { return instanceArg_; }

  bool inTry() const { return inTry_; }
  size_t tryNoteIndex() const { return tryNoteIndex_; }

  static AliasSet wasmCallAliasSet() {
    // This is ok because:
    // - numElements is immutable
    // - the GC will rewrite any array data pointers on move
    AliasSet exclude = AliasSet(AliasSet::WasmArrayNumElements) |
                       AliasSet(AliasSet::WasmArrayDataPointer);
    return AliasSet::Store(AliasSet::Any) & ~exclude;
  }
};

// A wasm call that is catchable. This instruction is a control instruction,
// and terminates the block it is on. A normal return will proceed in a the
// fallthrough block. An exceptional return will unwind into the landing pad
// block for this call. The landing pad block must begin with an
// MWasmCallLandingPrePad.
class MWasmCallCatchable final : public MVariadicControlInstruction<2>,
                                 public MWasmCallBase,
                                 public NoTypePolicy::Data {
  MWasmCallCatchable(const wasm::CallSiteDesc& desc,
                     const wasm::CalleeDesc& callee,
                     uint32_t stackArgAreaSizeUnaligned, size_t tryNoteIndex)
      : MVariadicControlInstruction(classOpcode),
        MWasmCallBase(desc, callee, stackArgAreaSizeUnaligned, true,
                      tryNoteIndex) {}

 public:
  INSTRUCTION_HEADER(WasmCallCatchable)

  static MWasmCallCatchable* New(
      TempAllocator& alloc, const wasm::CallSiteDesc& desc,
      const wasm::CalleeDesc& callee, const Args& args,
      uint32_t stackArgAreaSizeUnaligned, uint32_t tryNoteIndex,
      MBasicBlock* fallthroughBlock, MBasicBlock* prePadBlock,
      MDefinition* tableAddressOrRef = nullptr);

  static MWasmCallCatchable* NewBuiltinInstanceMethodCall(
      TempAllocator& alloc, const wasm::CallSiteDesc& desc,
      const wasm::SymbolicAddress builtin, wasm::FailureMode failureMode,
      const ABIArg& instanceArg, const Args& args,
      uint32_t stackArgAreaSizeUnaligned, uint32_t tryNoteIndex,
      MBasicBlock* fallthroughBlock, MBasicBlock* prePadBlock);

  bool possiblyCalls() const override { return true; }
  AliasSet getAliasSet() const override { return wasmCallAliasSet(); }

  static const size_t FallthroughBranchIndex = 0;
  static const size_t PrePadBranchIndex = 1;
};

// A wasm call that is not catchable. This instruction is not a control
// instruction, and therefore is not a block terminator.
class MWasmCallUncatchable final : public MVariadicInstruction,
                                   public MWasmCallBase,
                                   public NoTypePolicy::Data {
  MWasmCallUncatchable(const wasm::CallSiteDesc& desc,
                       const wasm::CalleeDesc& callee,
                       uint32_t stackArgAreaSizeUnaligned)
      : MVariadicInstruction(classOpcode),
        MWasmCallBase(desc, callee, stackArgAreaSizeUnaligned, false, 0) {}

 public:
  INSTRUCTION_HEADER(WasmCallUncatchable)

  static MWasmCallUncatchable* New(TempAllocator& alloc,
                                   const wasm::CallSiteDesc& desc,
                                   const wasm::CalleeDesc& callee,
                                   const Args& args,
                                   uint32_t stackArgAreaSizeUnaligned,
                                   MDefinition* tableAddressOrRef = nullptr);

  static MWasmCallUncatchable* NewBuiltinInstanceMethodCall(
      TempAllocator& alloc, const wasm::CallSiteDesc& desc,
      const wasm::SymbolicAddress builtin, wasm::FailureMode failureMode,
      const ABIArg& instanceArg, const Args& args,
      uint32_t stackArgAreaSizeUnaligned);

  bool possiblyCalls() const override { return true; }
  AliasSet getAliasSet() const override { return wasmCallAliasSet(); }
};

class MWasmReturnCall final : public MVariadicControlInstruction<0>,
                              public MWasmCallBase,
                              public NoTypePolicy::Data {
  MWasmReturnCall(const wasm::CallSiteDesc& desc,
                  const wasm::CalleeDesc& callee,
                  uint32_t stackArgAreaSizeUnaligned)
      : MVariadicControlInstruction(classOpcode),
        MWasmCallBase(desc, callee, stackArgAreaSizeUnaligned, false, 0) {}

 public:
  INSTRUCTION_HEADER(WasmReturnCall)

  static MWasmReturnCall* New(TempAllocator& alloc,
                              const wasm::CallSiteDesc& desc,
                              const wasm::CalleeDesc& callee, const Args& args,
                              uint32_t stackArgAreaSizeUnaligned,
                              MDefinition* tableAddressOrRef = nullptr);

  bool possiblyCalls() const override { return true; }
};

// A marker instruction for a block which is the landing pad for a catchable
// wasm call. This instruction does not emit any code, only filling in
// metadata. This instruction must be the first instruction added to the
// landing pad block.
class MWasmCallLandingPrePad : public MNullaryInstruction {
  // The block of the call that may unwind to this landing pad.
  MBasicBlock* callBlock_;
  // The index of the try note to initialize a landing pad for.
  size_t tryNoteIndex_;

  explicit MWasmCallLandingPrePad(MBasicBlock* callBlock, size_t tryNoteIndex)
      : MNullaryInstruction(classOpcode),
        callBlock_(callBlock),
        tryNoteIndex_(tryNoteIndex) {
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(WasmCallLandingPrePad)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  size_t tryNoteIndex() { return tryNoteIndex_; }
  MBasicBlock* callBlock() { return callBlock_; }
};

class MWasmSelect : public MTernaryInstruction, public NoTypePolicy::Data {
  MWasmSelect(MDefinition* trueExpr, MDefinition* falseExpr,
              MDefinition* condExpr)
      : MTernaryInstruction(classOpcode, trueExpr, falseExpr, condExpr) {
    MOZ_ASSERT(condExpr->type() == MIRType::Int32);
    MOZ_ASSERT(trueExpr->type() == falseExpr->type());
    setResultType(trueExpr->type());
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmSelect)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, trueExpr), (1, falseExpr), (2, condExpr))

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  wasm::MaybeRefType computeWasmRefType() const override {
    return wasm::MaybeRefType::leastUpperBound(trueExpr()->wasmRefType(),
                                               falseExpr()->wasmRefType());
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  ALLOW_CLONE(MWasmSelect)
};

// Wasm SIMD.
//
// See comment in WasmIonCompile.cpp for a justification for these nodes.

// (v128, v128, v128) -> v128 effect-free operation.
class MWasmTernarySimd128 : public MTernaryInstruction,
                            public NoTypePolicy::Data {
  wasm::SimdOp simdOp_;

  MWasmTernarySimd128(MDefinition* v0, MDefinition* v1, MDefinition* v2,
                      wasm::SimdOp simdOp)
      : MTernaryInstruction(classOpcode, v0, v1, v2), simdOp_(simdOp) {
    setMovable();
    setResultType(MIRType::Simd128);
  }

 public:
  INSTRUCTION_HEADER(WasmTernarySimd128)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, v0), (1, v1), (2, v2))

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           simdOp() == ins->toWasmTernarySimd128()->simdOp();
  }
#ifdef ENABLE_WASM_SIMD
  MDefinition* foldsTo(TempAllocator& alloc) override;

  // If the control mask of a bitselect allows the operation to be specialized
  // as a shuffle and it is profitable to specialize it on this platform, return
  // true and the appropriate shuffle mask.
  bool specializeBitselectConstantMaskAsShuffle(int8_t shuffle[16]);
  // Checks if more relaxed version of lane select can be used. It returns true
  // if a bit mask input expected to be all 0s or 1s for entire 8-bit lanes,
  // false otherwise.
  bool canRelaxBitselect();
#endif

  wasm::SimdOp simdOp() const { return simdOp_; }

  ALLOW_CLONE(MWasmTernarySimd128)
};

// (v128, v128) -> v128 effect-free operations.
class MWasmBinarySimd128 : public MBinaryInstruction,
                           public NoTypePolicy::Data {
  wasm::SimdOp simdOp_;

  MWasmBinarySimd128(MDefinition* lhs, MDefinition* rhs, bool commutative,
                     wasm::SimdOp simdOp)
      : MBinaryInstruction(classOpcode, lhs, rhs), simdOp_(simdOp) {
    setMovable();
    setResultType(MIRType::Simd128);
    if (commutative) {
      setCommutative();
    }
  }

 public:
  INSTRUCTION_HEADER(WasmBinarySimd128)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmBinarySimd128()->simdOp() == simdOp_;
  }
#ifdef ENABLE_WASM_SIMD
  MDefinition* foldsTo(TempAllocator& alloc) override;

  // Checks if pmaddubsw operation is supported.
  bool canPmaddubsw();
#endif

  wasm::SimdOp simdOp() const { return simdOp_; }

  // Platform-dependent specialization.
  bool specializeForConstantRhs();

  ALLOW_CLONE(MWasmBinarySimd128)
};

// (v128, const) -> v128 effect-free operations.
class MWasmBinarySimd128WithConstant : public MUnaryInstruction,
                                       public NoTypePolicy::Data {
  SimdConstant rhs_;
  wasm::SimdOp simdOp_;

  MWasmBinarySimd128WithConstant(MDefinition* lhs, const SimdConstant& rhs,
                                 wasm::SimdOp simdOp)
      : MUnaryInstruction(classOpcode, lhs), rhs_(rhs), simdOp_(simdOp) {
    setMovable();
    setResultType(MIRType::Simd128);
  }

 public:
  INSTRUCTION_HEADER(WasmBinarySimd128WithConstant)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmBinarySimd128WithConstant()->simdOp() == simdOp_ &&
           rhs_.bitwiseEqual(ins->toWasmBinarySimd128WithConstant()->rhs());
  }

  wasm::SimdOp simdOp() const { return simdOp_; }
  MDefinition* lhs() const { return input(); }
  const SimdConstant& rhs() const { return rhs_; }

  ALLOW_CLONE(MWasmBinarySimd128WithConstant)
};

// (v128, scalar, imm) -> v128 effect-free operations.
class MWasmReplaceLaneSimd128 : public MBinaryInstruction,
                                public NoTypePolicy::Data {
  uint32_t laneIndex_;
  wasm::SimdOp simdOp_;

  MWasmReplaceLaneSimd128(MDefinition* lhs, MDefinition* rhs,
                          uint32_t laneIndex, wasm::SimdOp simdOp)
      : MBinaryInstruction(classOpcode, lhs, rhs),
        laneIndex_(laneIndex),
        simdOp_(simdOp) {
    setMovable();
    setResultType(MIRType::Simd128);
  }

 public:
  INSTRUCTION_HEADER(WasmReplaceLaneSimd128)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmReplaceLaneSimd128()->simdOp() == simdOp_ &&
           ins->toWasmReplaceLaneSimd128()->laneIndex() == laneIndex_;
  }

  uint32_t laneIndex() const { return laneIndex_; }
  wasm::SimdOp simdOp() const { return simdOp_; }

  ALLOW_CLONE(MWasmReplaceLaneSimd128)
};

// (scalar) -> v128 effect-free operations.
class MWasmScalarToSimd128 : public MUnaryInstruction,
                             public NoTypePolicy::Data {
  wasm::SimdOp simdOp_;

  MWasmScalarToSimd128(MDefinition* src, wasm::SimdOp simdOp)
      : MUnaryInstruction(classOpcode, src), simdOp_(simdOp) {
    setMovable();
    setResultType(MIRType::Simd128);
  }

 public:
  INSTRUCTION_HEADER(WasmScalarToSimd128)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmScalarToSimd128()->simdOp() == simdOp_;
  }
#ifdef ENABLE_WASM_SIMD
  MDefinition* foldsTo(TempAllocator& alloc) override;
#endif

  wasm::SimdOp simdOp() const { return simdOp_; }

  ALLOW_CLONE(MWasmScalarToSimd128)
};

// (v128, imm) -> scalar effect-free operations.
class MWasmReduceSimd128 : public MUnaryInstruction, public NoTypePolicy::Data {
  wasm::SimdOp simdOp_;
  uint32_t imm_;

  MWasmReduceSimd128(MDefinition* src, wasm::SimdOp simdOp, MIRType outType,
                     uint32_t imm)
      : MUnaryInstruction(classOpcode, src), simdOp_(simdOp), imm_(imm) {
    setMovable();
    setResultType(outType);
  }

 public:
  INSTRUCTION_HEADER(WasmReduceSimd128)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toWasmReduceSimd128()->simdOp() == simdOp_ &&
           ins->toWasmReduceSimd128()->imm() == imm_;
  }
#ifdef ENABLE_WASM_SIMD
  MDefinition* foldsTo(TempAllocator& alloc) override;
#endif

  uint32_t imm() const { return imm_; }
  wasm::SimdOp simdOp() const { return simdOp_; }

  ALLOW_CLONE(MWasmReduceSimd128)
};

class MWasmLoadLaneSimd128
    : public MVariadicInstruction,  // memoryBase is nullptr on some platforms
      public NoTypePolicy::Data {
  wasm::MemoryAccessDesc access_;
  uint32_t laneSize_;
  uint32_t laneIndex_;
  uint32_t memoryBaseIndex_;

  MWasmLoadLaneSimd128(const wasm::MemoryAccessDesc& access, uint32_t laneSize,
                       uint32_t laneIndex, uint32_t memoryBaseIndex)
      : MVariadicInstruction(classOpcode),
        access_(access),
        laneSize_(laneSize),
        laneIndex_(laneIndex),
        memoryBaseIndex_(memoryBaseIndex) {
    MOZ_ASSERT(!access_.isAtomic());
    setGuard();
    setResultType(MIRType::Simd128);
  }

 public:
  INSTRUCTION_HEADER(WasmLoadLaneSimd128)
  NAMED_OPERANDS((0, base), (1, value));

  static MWasmLoadLaneSimd128* New(TempAllocator& alloc,
                                   MDefinition* memoryBase, MDefinition* base,
                                   const wasm::MemoryAccessDesc& access,
                                   uint32_t laneSize, uint32_t laneIndex,
                                   MDefinition* value) {
    uint32_t nextIndex = 2;
    uint32_t memoryBaseIndex = memoryBase ? nextIndex++ : UINT32_MAX;

    MWasmLoadLaneSimd128* load = new (alloc)
        MWasmLoadLaneSimd128(access, laneSize, laneIndex, memoryBaseIndex);
    if (!load->init(alloc, nextIndex)) {
      return nullptr;
    }

    load->initOperand(0, base);
    load->initOperand(1, value);
    if (memoryBase) {
      load->initOperand(memoryBaseIndex, memoryBase);
    }

    return load;
  }

  const wasm::MemoryAccessDesc& access() const { return access_; }
  uint32_t laneSize() const { return laneSize_; }
  uint32_t laneIndex() const { return laneIndex_; }
  bool hasMemoryBase() const { return memoryBaseIndex_ != UINT32_MAX; }
  MDefinition* memoryBase() const {
    MOZ_ASSERT(hasMemoryBase());
    return getOperand(memoryBaseIndex_);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::WasmHeap);
  }
};

class MWasmStoreLaneSimd128 : public MVariadicInstruction,
                              public NoTypePolicy::Data {
  wasm::MemoryAccessDesc access_;
  uint32_t laneSize_;
  uint32_t laneIndex_;
  uint32_t memoryBaseIndex_;

  explicit MWasmStoreLaneSimd128(const wasm::MemoryAccessDesc& access,
                                 uint32_t laneSize, uint32_t laneIndex,
                                 uint32_t memoryBaseIndex)
      : MVariadicInstruction(classOpcode),
        access_(access),
        laneSize_(laneSize),
        laneIndex_(laneIndex),
        memoryBaseIndex_(memoryBaseIndex) {
    MOZ_ASSERT(!access_.isAtomic());
    setGuard();
    setResultType(MIRType::Simd128);
  }

 public:
  INSTRUCTION_HEADER(WasmStoreLaneSimd128)
  NAMED_OPERANDS((0, base), (1, value))

  static MWasmStoreLaneSimd128* New(TempAllocator& alloc,
                                    MDefinition* memoryBase, MDefinition* base,
                                    const wasm::MemoryAccessDesc& access,
                                    uint32_t laneSize, uint32_t laneIndex,
                                    MDefinition* value) {
    uint32_t nextIndex = 2;
    uint32_t memoryBaseIndex = memoryBase ? nextIndex++ : UINT32_MAX;

    MWasmStoreLaneSimd128* store = new (alloc)
        MWasmStoreLaneSimd128(access, laneSize, laneIndex, memoryBaseIndex);
    if (!store->init(alloc, nextIndex)) {
      return nullptr;
    }

    store->initOperand(0, base);
    store->initOperand(1, value);
    if (memoryBase) {
      store->initOperand(memoryBaseIndex, memoryBase);
    }

    return store;
  }

  const wasm::MemoryAccessDesc& access() const { return access_; }
  uint32_t laneSize() const { return laneSize_; }
  uint32_t laneIndex() const { return laneIndex_; }
  bool hasMemoryBase() const { return memoryBaseIndex_ != UINT32_MAX; }
  MDefinition* memoryBase() const {
    MOZ_ASSERT(hasMemoryBase());
    return getOperand(memoryBaseIndex_);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::WasmHeap);
  }
};

// End Wasm SIMD

class MIonToWasmCall final : public MVariadicInstruction,
                             public NoTypePolicy::Data {
  CompilerGCPointer<WasmInstanceObject*> instanceObj_;
  const wasm::FuncExport& funcExport_;

  MIonToWasmCall(WasmInstanceObject* instanceObj, MIRType resultType,
                 const wasm::FuncExport& funcExport)
      : MVariadicInstruction(classOpcode),
        instanceObj_(instanceObj),
        funcExport_(funcExport) {
    setResultType(resultType);
  }

 public:
  INSTRUCTION_HEADER(IonToWasmCall);

  static MIonToWasmCall* New(TempAllocator& alloc,
                             WasmInstanceObject* instanceObj,
                             const wasm::FuncExport& funcExport);

  void initArg(size_t i, MDefinition* arg) { initOperand(i, arg); }

  WasmInstanceObject* instanceObject() const { return instanceObj_; }
  wasm::Instance* instance() const { return &instanceObj_->instance(); }
  const wasm::FuncExport& funcExport() const { return funcExport_; }
  bool possiblyCalls() const override { return true; }
#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override;
#endif
};

// For accesses to wasm object fields, we need to be able to describe 8- and
// 16-bit accesses.  But MIRType can't represent those.  Hence these two
// supplemental enums, used for reading and writing fields respectively.

// Indicates how to widen an 8- or 16-bit value (when it is read from memory).
enum class MWideningOp : uint8_t { None, FromU16, FromS16, FromU8, FromS8 };

#ifdef JS_JITSPEW
static inline const char* StringFromMWideningOp(MWideningOp op) {
  switch (op) {
    case MWideningOp::None:
      return "None";
    case MWideningOp::FromU16:
      return "FromU16";
    case MWideningOp::FromS16:
      return "FromS16";
    case MWideningOp::FromU8:
      return "FromU8";
    case MWideningOp::FromS8:
      return "FromS8";
    default:
      break;
  }
  MOZ_CRASH("Unknown MWideningOp");
}
#endif

// Indicates how to narrow a 32-bit value (when it is written to memory).  The
// operation is a simple truncate.
enum class MNarrowingOp : uint8_t { None, To16, To8 };

#ifdef JS_JITSPEW
static inline const char* StringFromMNarrowingOp(MNarrowingOp op) {
  switch (op) {
    case MNarrowingOp::None:
      return "None";
    case MNarrowingOp::To16:
      return "To16";
    case MNarrowingOp::To8:
      return "To8";
    default:
      break;
  }
  MOZ_CRASH("Unknown MNarrowingOp");
}
#endif

// Loads a value from a location, denoted as a fixed offset from a base
// pointer. This field may be any value type, including references. No
// barriers are performed.
//
// This instruction can extend the lifetime of an optional `keepAlive`
// parameter to match the lifetime of this instruction. This is necessary if
// the base pointer is owned by some GC'ed object, which means that the GC
// object must have the same lifetime as all uses of it's owned pointers.
// No code to access the keepAlive value is generated.
//
// `offset` must be representable as a 31-bit unsigned integer.
//
// An optional structFieldIndex can be given for struct accesses and used in
// scalar replacement.
class MWasmLoadField : public MBinaryInstruction, public NoTypePolicy::Data {
  uint32_t offset_;
  mozilla::Maybe<uint32_t> structFieldIndex_;
  MWideningOp wideningOp_;
  AliasSet aliases_;
  wasm::MaybeTrapSiteDesc maybeTrap_;
  mozilla::Maybe<wasm::RefTypeHierarchy> hierarchy_;

  MWasmLoadField(MDefinition* base, MDefinition* keepAlive, size_t offset,
                 mozilla::Maybe<uint32_t> structFieldIndex, MIRType type,
                 MWideningOp wideningOp, AliasSet aliases,
                 wasm::MaybeTrapSiteDesc maybeTrap = mozilla::Nothing(),
                 wasm::MaybeRefType maybeRefType = wasm::MaybeRefType())
      : MBinaryInstruction(classOpcode, base, keepAlive ? keepAlive : base),
        offset_(uint32_t(offset)),
        structFieldIndex_(structFieldIndex),
        wideningOp_(wideningOp),
        aliases_(aliases),
        maybeTrap_(std::move(maybeTrap)),
        hierarchy_(maybeRefType.hierarchy()) {
    MOZ_ASSERT(offset <= INT32_MAX);
    // "if you want to widen the value when it is loaded, the destination type
    // must be Int32".
    MOZ_ASSERT_IF(wideningOp != MWideningOp::None, type == MIRType::Int32);
    MOZ_ASSERT(
        aliases.flags() ==
            AliasSet::Load(AliasSet::WasmStructOutlineDataPointer).flags() ||
        aliases.flags() ==
            AliasSet::Load(AliasSet::WasmStructInlineDataArea).flags() ||
        aliases.flags() ==
            AliasSet::Load(AliasSet::WasmStructOutlineDataArea).flags() ||
        aliases.flags() ==
            AliasSet::Load(AliasSet::WasmArrayNumElements).flags() ||
        aliases.flags() ==
            AliasSet::Load(AliasSet::WasmArrayDataPointer).flags() ||
        aliases.flags() ==
            AliasSet::Load(AliasSet::WasmArrayDataArea).flags() ||
        aliases.flags() == AliasSet::Load(AliasSet::Any).flags());
    setResultType(type);
    if (maybeTrap_) {
      setGuard();
    }
    initWasmRefType(maybeRefType);
  }

 public:
  INSTRUCTION_HEADER(WasmLoadField)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, base), (1, keepAlive))

  uint32_t offset() const { return offset_; }
  mozilla::Maybe<uint32_t> structFieldIndex() const {
    return structFieldIndex_;
  }
  MWideningOp wideningOp() const { return wideningOp_; }
  AliasSet getAliasSet() const override { return aliases_; }
  wasm::MaybeTrapSiteDesc maybeTrap() const { return maybeTrap_; }
  mozilla::Maybe<wasm::RefTypeHierarchy> hierarchy() const {
    return hierarchy_;
  }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isWasmLoadField()) {
      return false;
    }
    const MWasmLoadField* other = ins->toWasmLoadField();
    return ins->isWasmLoadField() && congruentIfOperandsEqual(ins) &&
           offset() == other->offset() &&
           structFieldIndex() == other->structFieldIndex() &&
           wideningOp() == other->wideningOp() &&
           getAliasSet().flags() == other->getAliasSet().flags() &&
           hierarchy() == other->hierarchy();
  }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[96];
    SprintfLiteral(buf, "(offs=%lld, wideningOp=%s)", (long long int)offset_,
                   StringFromMWideningOp(wideningOp_));
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MWasmLoadField)
};

// Loads a value from base pointer, given an index and element size. This field
// may be any value type, including references. No barriers are performed.
//
// The element size is implicitly defined by MIRType and MWideningOp. For
// example, MIRType::Float32 indicates an element size of 32 bits, and
// MIRType::Int32 and MWideningOp::FromU16 together indicate an element size of
// 16 bits.
//
// This instruction takes an optional second object `keepAlive` that must be
// kept alive, as described for MWasmLoadField above.
class MWasmLoadElement : public MTernaryInstruction, public NoTypePolicy::Data {
  MWideningOp wideningOp_;
  Scale scale_;
  AliasSet aliases_;
  wasm::MaybeTrapSiteDesc maybeTrap_;

  MWasmLoadElement(MDefinition* base, MDefinition* keepAlive,
                   MDefinition* index, MIRType type, MWideningOp wideningOp,
                   Scale scale, AliasSet aliases,
                   wasm::MaybeTrapSiteDesc maybeTrap = mozilla::Nothing(),
                   wasm::MaybeRefType maybeRefType = wasm::MaybeRefType())
      : MTernaryInstruction(classOpcode, base, index,
                            keepAlive ? keepAlive : base),
        wideningOp_(wideningOp),
        scale_(scale),
        aliases_(aliases),
        maybeTrap_(std::move(maybeTrap)) {
    MOZ_ASSERT(base->type() == MIRType::WasmArrayData);
    MOZ_ASSERT(aliases.flags() ==
                   AliasSet::Load(AliasSet::WasmArrayDataArea).flags() ||
               aliases.flags() == AliasSet::Load(AliasSet::Any).flags());
    setResultType(type);
    if (maybeTrap_) {
      setGuard();
    }
    initWasmRefType(maybeRefType);
  }

 public:
  INSTRUCTION_HEADER(WasmLoadElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, base), (1, index), (2, keepAlive))

  MWideningOp wideningOp() const { return wideningOp_; }
  Scale scale() const { return scale_; }
  AliasSet getAliasSet() const override { return aliases_; }
  wasm::MaybeTrapSiteDesc maybeTrap() const { return maybeTrap_; }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[96];
    SprintfLiteral(buf, "(wideningOp=%s, scale=%s)",
                   StringFromMWideningOp(wideningOp_), StringFromScale(scale_));
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MWasmLoadElement)
};

// Stores a non-reference value to anlocation, denoted as a fixed offset from
// a base pointer, which (it is assumed) is within a wasm object.  This field
// may be any value type, _excluding_ references.  References _must_ use the
// 'Ref' variant of this instruction.  The offset must be representable as a
// 31-bit unsigned integer.
//
// This instruction takes a second object `keepAlive` that must be kept alive,
// as described for MWasmLoadField above.
class MWasmStoreField : public MTernaryInstruction, public NoTypePolicy::Data {
  uint32_t offset_;
  mozilla::Maybe<uint32_t> structFieldIndex_;
  MNarrowingOp narrowingOp_;
  AliasSet aliases_;
  wasm::MaybeTrapSiteDesc maybeTrap_;

  MWasmStoreField(MDefinition* base, MDefinition* keepAlive, size_t offset,
                  mozilla::Maybe<uint32_t> structFieldIndex, MDefinition* value,
                  MNarrowingOp narrowingOp, AliasSet aliases,
                  wasm::MaybeTrapSiteDesc maybeTrap = mozilla::Nothing())
      : MTernaryInstruction(classOpcode, base, value,
                            keepAlive ? keepAlive : base),
        offset_(uint32_t(offset)),
        structFieldIndex_(structFieldIndex),
        narrowingOp_(narrowingOp),
        aliases_(aliases),
        maybeTrap_(std::move(maybeTrap)) {
    MOZ_ASSERT(offset <= INT32_MAX);
    MOZ_ASSERT(value->type() != MIRType::WasmAnyRef);
    // "if you want to narrow the value when it is stored, the source type
    // must be Int32".
    MOZ_ASSERT_IF(narrowingOp != MNarrowingOp::None,
                  value->type() == MIRType::Int32);
    MOZ_ASSERT(
        aliases.flags() ==
            AliasSet::Store(AliasSet::WasmStructInlineDataArea).flags() ||
        aliases.flags() ==
            AliasSet::Store(AliasSet::WasmStructOutlineDataArea).flags() ||
        aliases.flags() ==
            AliasSet::Store(AliasSet::WasmArrayDataArea).flags() ||
        aliases.flags() == AliasSet::Store(AliasSet::Any).flags());
    if (maybeTrap_) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(WasmStoreField)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, base), (1, value), (2, keepAlive))

  uint32_t offset() const { return offset_; }
  mozilla::Maybe<uint32_t> structFieldIndex() const {
    return structFieldIndex_;
  }
  MNarrowingOp narrowingOp() const { return narrowingOp_; }
  AliasSet getAliasSet() const override { return aliases_; }
  wasm::MaybeTrapSiteDesc maybeTrap() const { return maybeTrap_; }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[96];
    SprintfLiteral(buf, "(offs=%lld, narrowingOp=%s)", (long long int)offset_,
                   StringFromMNarrowingOp(narrowingOp_));
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MWasmStoreField)
};

// Stores a reference value to a location, denoted as a fixed offset from a
// base pointer, which (it is assumed) is within a wasm object.  This
// instruction emits a pre-barrier.  A post barrier _must_ be performed
// separately.  The offset must be representable as a 31-bit unsigned integer.
//
// This instruction takes a second object `keepAlive` that must be kept alive,
// as described for MWasmLoadField above.
class MWasmStoreFieldRef : public MAryInstruction<4>,
                           public NoTypePolicy::Data {
  uint32_t offset_;
  mozilla::Maybe<uint32_t> structFieldIndex_;
  AliasSet aliases_;
  wasm::MaybeTrapSiteDesc maybeTrap_;
  WasmPreBarrierKind preBarrierKind_;

  MWasmStoreFieldRef(MDefinition* instance, MDefinition* base,
                     MDefinition* keepAlive, size_t offset,
                     mozilla::Maybe<uint32_t> structFieldIndex,
                     MDefinition* value, AliasSet aliases,
                     wasm::MaybeTrapSiteDesc maybeTrap,
                     WasmPreBarrierKind preBarrierKind)
      : MAryInstruction<4>(classOpcode),
        offset_(uint32_t(offset)),
        structFieldIndex_(structFieldIndex),
        aliases_(aliases),
        maybeTrap_(std::move(maybeTrap)),
        preBarrierKind_(preBarrierKind) {
    MOZ_ASSERT(base->type() == TargetWordMIRType() ||
               base->type() == MIRType::Pointer ||
               base->type() == MIRType::WasmAnyRef ||
               base->type() == MIRType::WasmArrayData);
    MOZ_ASSERT(offset <= INT32_MAX);
    MOZ_ASSERT(value->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(
        aliases.flags() ==
            AliasSet::Store(AliasSet::WasmStructInlineDataArea).flags() ||
        aliases.flags() ==
            AliasSet::Store(AliasSet::WasmStructOutlineDataArea).flags() ||
        aliases.flags() ==
            AliasSet::Store(AliasSet::WasmArrayDataArea).flags() ||
        aliases.flags() == AliasSet::Store(AliasSet::Any).flags());
    initOperand(0, instance);
    initOperand(1, base);
    initOperand(2, value);
    initOperand(3, keepAlive ? keepAlive : base);
    if (maybeTrap_) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(WasmStoreFieldRef)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance), (1, base), (2, value), (3, keepAlive))

  uint32_t offset() const { return offset_; }
  mozilla::Maybe<uint32_t> structFieldIndex() const {
    return structFieldIndex_;
  }
  AliasSet getAliasSet() const override { return aliases_; }
  wasm::MaybeTrapSiteDesc maybeTrap() const { return maybeTrap_; }
  WasmPreBarrierKind preBarrierKind() const { return preBarrierKind_; }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "(offs=%lld)", (long long int)offset_);
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MWasmStoreFieldRef)
};

// Stores a non-reference value to a base pointer, given an index and element
// size. This field may be any value type, excluding references. References MUST
// use the 'Ref' variant of this instruction.
//
// The element size is implicitly defined by MIRType and MNarrowingOp. For
// example, MIRType::Float32 indicates an element size of 32 bits, and
// MIRType::Int32 and MNarrowingOp::To16 together indicate an element size of 16
// bits.
//
// This instruction takes a second object `keepAlive` that must be kept alive,
// as described for MWasmLoadField above.
class MWasmStoreElement : public MQuaternaryInstruction,
                          public NoTypePolicy::Data {
  MNarrowingOp narrowingOp_;
  Scale scale_;
  AliasSet aliases_;
  wasm::MaybeTrapSiteDesc maybeTrap_;

  MWasmStoreElement(MDefinition* base, MDefinition* index, MDefinition* value,
                    MDefinition* keepAlive, MNarrowingOp narrowingOp,
                    Scale scale, AliasSet aliases,
                    wasm::MaybeTrapSiteDesc maybeTrap = mozilla::Nothing())
      : MQuaternaryInstruction(classOpcode, base, index, value,
                               keepAlive ? keepAlive : base),
        narrowingOp_(narrowingOp),
        scale_(scale),
        aliases_(aliases),
        maybeTrap_(std::move(maybeTrap)) {
    MOZ_ASSERT(base->type() == MIRType::WasmArrayData);
    MOZ_ASSERT(value->type() != MIRType::WasmAnyRef);
    // "if you want to narrow the value when it is stored, the source type
    // must be Int32".
    MOZ_ASSERT_IF(narrowingOp != MNarrowingOp::None,
                  value->type() == MIRType::Int32);
    MOZ_ASSERT(aliases.flags() ==
                   AliasSet::Store(AliasSet::WasmArrayDataArea).flags() ||
               aliases.flags() == AliasSet::Store(AliasSet::Any).flags());
    if (maybeTrap_) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(WasmStoreElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, base), (1, index), (2, value), (3, keepAlive))

  MNarrowingOp narrowingOp() const { return narrowingOp_; }
  Scale scale() const { return scale_; }
  AliasSet getAliasSet() const override { return aliases_; }
  wasm::MaybeTrapSiteDesc maybeTrap() const { return maybeTrap_; }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[96];
    SprintfLiteral(buf, "(narrowingOp=%s, scale=%s)",
                   StringFromMNarrowingOp(narrowingOp_),
                   StringFromScale(scale_));
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MWasmStoreElement)
};

// Stores a reference value to a base pointer, given an index and element size.
// This instruction emits a pre-barrier. A post barrier MUST be performed
// separately.
//
// The element size is implicitly defined by MIRType and MNarrowingOp, as
// described for MWasmStoreElement above.
//
// This instruction takes a second object `ka` that must be kept alive, as
// described for MWasmLoadField above.
class MWasmStoreElementRef : public MAryInstruction<5>,
                             public NoTypePolicy::Data {
  AliasSet aliases_;
  wasm::MaybeTrapSiteDesc maybeTrap_;
  WasmPreBarrierKind preBarrierKind_;

  MWasmStoreElementRef(MDefinition* instance, MDefinition* base,
                       MDefinition* index, MDefinition* value,
                       MDefinition* keepAlive, AliasSet aliases,
                       wasm::MaybeTrapSiteDesc maybeTrap,
                       WasmPreBarrierKind preBarrierKind)
      : MAryInstruction<5>(classOpcode),
        aliases_(aliases),
        maybeTrap_(std::move(maybeTrap)),
        preBarrierKind_(preBarrierKind) {
    MOZ_ASSERT(base->type() == MIRType::WasmArrayData);
    MOZ_ASSERT(value->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(aliases.flags() ==
                   AliasSet::Store(AliasSet::WasmArrayDataArea).flags() ||
               aliases.flags() == AliasSet::Store(AliasSet::Any).flags());
    initOperand(0, instance);
    initOperand(1, base);
    initOperand(2, index);
    initOperand(3, value);
    initOperand(4, keepAlive ? keepAlive : base);
    if (maybeTrap_) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(WasmStoreElementRef)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance), (1, base), (2, index), (3, value),
                 (4, keepAlive))

  AliasSet getAliasSet() const override { return aliases_; }
  wasm::MaybeTrapSiteDesc maybeTrap() const { return maybeTrap_; }
  WasmPreBarrierKind preBarrierKind() const { return preBarrierKind_; }

  ALLOW_CLONE(MWasmStoreElementRef)
};

class MWasmRefAsNonNull : public MUnaryInstruction, public NoTypePolicy::Data {
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmRefAsNonNull(MDefinition* ref, const wasm::TrapSiteDesc& trapSiteDesc)
      : MUnaryInstruction(classOpcode, ref), trapSiteDesc_(trapSiteDesc) {
    setResultType(MIRType::WasmAnyRef);
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(WasmRefAsNonNull)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, ref))

  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  wasm::MaybeRefType computeWasmRefType() const override {
    if (ref()->wasmRefType().isNothing()) {
      return wasm::MaybeRefType();
    }
    return wasm::MaybeRefType(ref()->wasmRefType().value().asNonNullable());
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  ALLOW_CLONE(MWasmRefAsNonNull)
};

// Tests if the wasm ref `ref` is a subtype of `destType` and returns the
// boolean representing the result.
class MWasmRefTestAbstract : public MUnaryInstruction,
                             public NoTypePolicy::Data {
  wasm::RefType destType_;

  MWasmRefTestAbstract(MDefinition* ref, wasm::RefType destType)
      : MUnaryInstruction(classOpcode, ref), destType_(destType) {
    MOZ_ASSERT(!destType.isTypeRef());
    setResultType(MIRType::Int32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmRefTestAbstract)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, ref))

  wasm::RefType destType() const { return destType_; };

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           destType() == ins->toWasmRefTestAbstract()->destType();
  }

  HashNumber valueHash() const override {
    HashNumber hn = MUnaryInstruction::valueHash();
    hn = addU64ToHash(hn, destType().packed().bits());
    return hn;
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  ALLOW_CLONE(MWasmRefTestAbstract)
};

// Tests if the wasm ref `ref` is a subtype of `superSTV` and returns the
// boolean representing the result.
//
// The actual super type definition must be known at compile time, so that the
// subtyping depth of super type depth can be used.
class MWasmRefTestConcrete : public MBinaryInstruction,
                             public NoTypePolicy::Data {
  wasm::RefType destType_;

  MWasmRefTestConcrete(MDefinition* ref, MDefinition* superSTV,
                       wasm::RefType destType)
      : MBinaryInstruction(classOpcode, ref, superSTV), destType_(destType) {
    MOZ_ASSERT(destType.isTypeRef());
    setResultType(MIRType::Int32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmRefTestConcrete)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, ref), (1, superSTV))

  wasm::RefType destType() const { return destType_; };

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           destType() == ins->toWasmRefTestConcrete()->destType();
  }

  HashNumber valueHash() const override {
    HashNumber hn = MBinaryInstruction::valueHash();
    hn = addU64ToHash(hn, destType().packed().bits());
    return hn;
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  ALLOW_CLONE(MWasmRefTestConcrete)
};

// Tests if the wasm ref `ref` is a subtype of `destType` and if so returns the
// ref, otherwise it does a wasm trap.
class MWasmRefCastAbstract : public MUnaryInstruction,
                             public NoTypePolicy::Data {
  wasm::RefType destType_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmRefCastAbstract(MDefinition* ref, wasm::RefType destType,
                       wasm::TrapSiteDesc&& trapSiteDesc)
      : MUnaryInstruction(classOpcode, ref),
        destType_(destType),
        trapSiteDesc_(std::move(trapSiteDesc)) {
    MOZ_ASSERT(!destType.isTypeRef());
    setResultType(MIRType::WasmAnyRef);
    // This may trap, which requires this to be a guard.
    setGuard();
    initWasmRefType(wasm::MaybeRefType(destType));
  }

 public:
  INSTRUCTION_HEADER(WasmRefCastAbstract)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, ref))

  wasm::RefType destType() const { return destType_; };
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
};

// Tests if the wasm ref `ref` is a subtype of `superSTV`, if so return the
// ref, otherwise do a wasm trap.
//
// The actual super type definition must be known at compile time, so that the
// subtyping depth of super type depth can be used.
class MWasmRefCastConcrete : public MBinaryInstruction,
                             public NoTypePolicy::Data {
  wasm::RefType destType_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmRefCastConcrete(MDefinition* ref, MDefinition* superSTV,
                       wasm::RefType destType,
                       wasm::TrapSiteDesc&& trapSiteDesc)
      : MBinaryInstruction(classOpcode, ref, superSTV),
        destType_(destType),
        trapSiteDesc_(std::move(trapSiteDesc)) {
    MOZ_ASSERT(destType.isTypeRef());
    setResultType(MIRType::WasmAnyRef);
    // This may trap, which requires this to be a guard.
    setGuard();
    initWasmRefType(wasm::MaybeRefType(destType));
  }

 public:
  INSTRUCTION_HEADER(WasmRefCastConcrete)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, ref), (1, superSTV))

  wasm::RefType destType() const { return destType_; };
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
};

class MWasmRefConvertAnyExtern : public MUnaryInstruction,
                                 public NoTypePolicy::Data {
  wasm::RefType::Kind destTypeKind_;

  MWasmRefConvertAnyExtern(MDefinition* ref, wasm::RefType::Kind destTypeKind)
      : MUnaryInstruction(classOpcode, ref), destTypeKind_(destTypeKind) {
    MOZ_ASSERT(destTypeKind_ == wasm::RefType::Kind::Any ||
               destTypeKind_ == wasm::RefType::Kind::Extern);
    setResultType(MIRType::WasmAnyRef);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WasmRefConvertAnyExtern)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, ref))

  wasm::RefType::Kind destTypeKind() const { return destTypeKind_; }

  wasm::MaybeRefType computeWasmRefType() const override {
    bool nullable = true;
    if (ref()->wasmRefType().isSome()) {
      nullable = ref()->wasmRefType().value().isNullable();
    };
    return wasm::MaybeRefType(wasm::RefType::fromKind(destTypeKind_, nullable));
  }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           destTypeKind() == ins->toWasmRefConvertAnyExtern()->destTypeKind();
  }

  HashNumber valueHash() const override {
    HashNumber hn = MUnaryInstruction::valueHash();
    hn = addU32ToHash(hn, destTypeKind());
    return hn;
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Represents the contents of all fields of a wasm struct.
// This class will be used for scalar replacement of wasm structs.
class MWasmStructState : public TempObject {
 private:
  MDefinition* wasmStruct_;
  // Represents the fields of this struct.
  Vector<MDefinition*, 0, JitAllocPolicy> fields_;

  explicit MWasmStructState(TempAllocator& alloc, MDefinition* structObject)
      : wasmStruct_(structObject), fields_(alloc) {}

 public:
  static MWasmStructState* New(TempAllocator& alloc, MDefinition* structObject);
  static MWasmStructState* Copy(TempAllocator& alloc, MWasmStructState* state);

  // Init the fields_ vector.
  [[nodiscard]] bool init();

  size_t numFields() const { return fields_.length(); }
  MDefinition* wasmStruct() const { return wasmStruct_; }

  // Get the field value based on the position of the field in the struct.
  MDefinition* getField(uint32_t index) const { return fields_[index]; }
  // Set the field offset based on the position of the field in the struct.
  void setField(uint32_t index, MDefinition* def) { fields_[index] = def; }
};

class MWasmNewStructObject : public MBinaryInstruction,
                             public NoTypePolicy::Data {
 private:
  const wasm::TypeDef* typeDef_;
  bool zeroFields_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmNewStructObject(MDefinition* instance, MDefinition* allocSite,
                       const wasm::TypeDef* typeDef, bool zeroFields,
                       const wasm::TrapSiteDesc& trapSiteDesc)
      : MBinaryInstruction(classOpcode, instance, allocSite),
        typeDef_(typeDef),
        zeroFields_(zeroFields),
        trapSiteDesc_(trapSiteDesc) {
    MOZ_ASSERT(typeDef->isStructType());
    setResultType(MIRType::WasmAnyRef);
    initWasmRefType(
        wasm::MaybeRefType(wasm::RefType::fromTypeDef(typeDef_, false)));
  }

 public:
  INSTRUCTION_HEADER(WasmNewStructObject)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance), (1, allocSite))

  AliasSet getAliasSet() const override {
    if (js::SupportDifferentialTesting()) {
      // Consider allocations effectful for differential testing.
      return MDefinition::getAliasSet();
    }
    return AliasSet::None();
  }
  const wasm::TypeDef& typeDef() { return *typeDef_; }
  const wasm::StructType& structType() const { return typeDef_->structType(); }
  bool isOutline() const {
    return WasmStructObject::requiresOutlineBytes(typeDef_->structType().size_);
  }
  bool zeroFields() const { return zeroFields_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }
  gc::AllocKind allocKind() const {
    return WasmStructObject::allocKindForTypeDef(typeDef_);
  }
};

class MWasmNewArrayObject : public MTernaryInstruction,
                            public NoTypePolicy::Data {
 private:
  const wasm::TypeDef* typeDef_;
  bool zeroFields_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MWasmNewArrayObject(MDefinition* instance, MDefinition* numElements,
                      MDefinition* allocSite, const wasm::TypeDef* typeDef,
                      bool zeroFields, const wasm::TrapSiteDesc& trapSiteDesc)
      : MTernaryInstruction(classOpcode, instance, numElements, allocSite),
        typeDef_(typeDef),
        zeroFields_(zeroFields),
        trapSiteDesc_(trapSiteDesc) {
    MOZ_ASSERT(typeDef->isArrayType());
    setResultType(MIRType::WasmAnyRef);
    initWasmRefType(
        wasm::MaybeRefType(wasm::RefType::fromTypeDef(typeDef_, false)));
  }

 public:
  INSTRUCTION_HEADER(WasmNewArrayObject)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, instance), (1, numElements), (2, allocSite))

  AliasSet getAliasSet() const override {
    if (js::SupportDifferentialTesting()) {
      // Consider allocations effectful for differential testing.
      return MDefinition::getAliasSet();
    }
    return AliasSet::None();
  }
  const wasm::TypeDef& typeDef() { return *typeDef_; }
  const wasm::ArrayType& arrayType() const { return typeDef_->arrayType(); }
  uint32_t elemSize() const {
    return typeDef_->arrayType().elementType().size();
  }
  bool zeroFields() const { return zeroFields_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }
};

#undef INSTRUCTION_HEADER

#ifdef ENABLE_WASM_SIMD
MWasmShuffleSimd128* BuildWasmShuffleSimd128(TempAllocator& alloc,
                                             const int8_t* control,
                                             MDefinition* lhs,
                                             MDefinition* rhs);
#endif  // ENABLE_WASM_SIMD

}  // namespace jit
}  // namespace js

#endif /* jit_MIR_wasm_h */
