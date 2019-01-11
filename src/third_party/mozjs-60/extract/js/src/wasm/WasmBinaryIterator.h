/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

#ifndef wasm_binary_iterator_h
#define wasm_binary_iterator_h

#include "mozilla/Poison.h"

#include "jit/AtomicOp.h"
#include "js/Printf.h"
#include "wasm/WasmValidate.h"

namespace js {
namespace wasm {

// The kind of a control-flow stack item.
enum class LabelKind : uint8_t
{
    Block,
    Loop,
    Then,
    Else
};

// The type of values on the operand stack during validation. The Any type
// represents the type of a value produced by an unconditional branch.
enum class StackType
{
    I32   = uint8_t(ValType::I32),
    I64   = uint8_t(ValType::I64),
    F32   = uint8_t(ValType::F32),
    F64   = uint8_t(ValType::F64),

    I8x16 = uint8_t(ValType::I8x16),
    I16x8 = uint8_t(ValType::I16x8),
    I32x4 = uint8_t(ValType::I32x4),
    F32x4 = uint8_t(ValType::F32x4),
    B8x16 = uint8_t(ValType::B8x16),
    B16x8 = uint8_t(ValType::B16x8),
    B32x4 = uint8_t(ValType::B32x4),

    Any   = uint8_t(TypeCode::Limit)
};

static inline StackType
ToStackType(ValType type)
{
    return StackType(type);
}

static inline ValType
NonAnyToValType(StackType type)
{
    MOZ_ASSERT(type != StackType::Any);
    return ValType(type);
}

static inline bool
Unify(StackType one, StackType two, StackType* result)
{
    if (MOZ_LIKELY(one == two)) {
        *result = one;
        return true;
    }

    if (one == StackType::Any) {
        *result = two;
        return true;
    }

    if (two == StackType::Any) {
        *result = one;
        return true;
    }

    return false;
}

#ifdef DEBUG
// Families of opcodes that share a signature and validation logic.
enum class OpKind {
    Block,
    Loop,
    Unreachable,
    Drop,
    I32,
    I64,
    F32,
    F64,
    I8x16,
    I16x8,
    I32x4,
    F32x4,
    B8x16,
    B16x8,
    B32x4,
    Br,
    BrIf,
    BrTable,
    Nop,
    Nullary,
    Unary,
    Binary,
    Comparison,
    Conversion,
    Load,
    Store,
    TeeStore,
    CurrentMemory,
    GrowMemory,
    Select,
    GetLocal,
    SetLocal,
    TeeLocal,
    GetGlobal,
    SetGlobal,
    TeeGlobal,
    Call,
    CallIndirect,
    OldCallDirect,
    OldCallIndirect,
    Return,
    If,
    Else,
    End,
    Wait,
    Wake,
    AtomicLoad,
    AtomicStore,
    AtomicBinOp,
    AtomicCompareExchange,
    OldAtomicLoad,
    OldAtomicStore,
    OldAtomicBinOp,
    OldAtomicCompareExchange,
    OldAtomicExchange,
    ExtractLane,
    ReplaceLane,
    Swizzle,
    Shuffle,
    Splat,
    SimdSelect,
    SimdCtor,
    SimdBooleanReduction,
    SimdShiftByScalar,
    SimdComparison,
};

// Return the OpKind for a given Op. This is used for sanity-checking that
// API users use the correct read function for a given Op.
OpKind
Classify(OpBytes op);
#endif

// Common fields for linear memory access.
template <typename Value>
struct LinearMemoryAddress
{
    Value base;
    uint32_t offset;
    uint32_t align;

    LinearMemoryAddress()
    {}
    LinearMemoryAddress(Value base, uint32_t offset, uint32_t align)
      : base(base), offset(offset), align(align)
    {}
};

template <typename ControlItem>
class ControlStackEntry
{
    LabelKind kind_;
    bool polymorphicBase_;
    ExprType type_;
    size_t valueStackStart_;
    ControlItem controlItem_;

  public:
    ControlStackEntry(LabelKind kind, ExprType type, size_t valueStackStart)
      : kind_(kind), polymorphicBase_(false), type_(type), valueStackStart_(valueStackStart),
        controlItem_()
    {
        MOZ_ASSERT(type != ExprType::Limit);
    }

    LabelKind kind() const { return kind_; }
    ExprType resultType() const { return type_; }
    ExprType branchTargetType() const { return kind_ == LabelKind::Loop ? ExprType::Void : type_; }
    size_t valueStackStart() const { return valueStackStart_; }
    ControlItem& controlItem() { return controlItem_; }
    void setPolymorphicBase() { polymorphicBase_ = true; }
    bool polymorphicBase() const { return polymorphicBase_; }

    void switchToElse() {
        MOZ_ASSERT(kind_ == LabelKind::Then);
        kind_ = LabelKind::Else;
        polymorphicBase_ = false;
    }
};

// Specialization for when there is no additional data needed.
template <>
class ControlStackEntry<Nothing>
{
    LabelKind kind_;
    bool polymorphicBase_;
    ExprType type_;
    size_t valueStackStart_;

  public:
    ControlStackEntry(LabelKind kind, ExprType type, size_t valueStackStart)
      : kind_(kind), polymorphicBase_(false), type_(type), valueStackStart_(valueStackStart)
    {
        MOZ_ASSERT(type != ExprType::Limit);
    }

    LabelKind kind() const { return kind_; }
    ExprType resultType() const { return type_; }
    ExprType branchTargetType() const { return kind_ == LabelKind::Loop ? ExprType::Void : type_; }
    size_t valueStackStart() const { return valueStackStart_; }
    Nothing controlItem() { return Nothing(); }
    void setPolymorphicBase() { polymorphicBase_ = true; }
    bool polymorphicBase() const { return polymorphicBase_; }

    void switchToElse() {
        MOZ_ASSERT(kind_ == LabelKind::Then);
        kind_ = LabelKind::Else;
        polymorphicBase_ = false;
    }
};

template <typename Value>
class TypeAndValue
{
    StackType type_;
    Value value_;

  public:
    TypeAndValue() : type_(StackType(TypeCode::Limit)), value_() {}
    explicit TypeAndValue(StackType type)
      : type_(type), value_()
    {}
    explicit TypeAndValue(ValType type)
      : type_(ToStackType(type)), value_()
    {}
    TypeAndValue(StackType type, Value value)
      : type_(type), value_(value)
    {}
    TypeAndValue(ValType type, Value value)
      : type_(ToStackType(type)), value_(value)
    {}
    StackType type() const {
        return type_;
    }
    StackType& typeRef() {
        return type_;
    }
    Value value() const {
        return value_;
    }
    void setValue(Value value) {
        value_ = value;
    }
};

// Specialization for when there is no additional data needed.
template <>
class TypeAndValue<Nothing>
{
    StackType type_;

  public:
    TypeAndValue() : type_(StackType(TypeCode::Limit)) {}
    explicit TypeAndValue(StackType type) : type_(type) {}
    explicit TypeAndValue(ValType type) : type_(ToStackType(type)) {}
    TypeAndValue(StackType type, Nothing value) : type_(type) {}
    TypeAndValue(ValType type, Nothing value) : type_(ToStackType(type)) {}

    StackType type() const { return type_; }
    StackType& typeRef() { return type_; }
    Nothing value() const { return Nothing(); }
    void setValue(Nothing value) {}
};

// An iterator over the bytes of a function body. It performs validation
// and unpacks the data into a usable form.
//
// The MOZ_STACK_CLASS attribute here is because of the use of DebugOnly.
// There's otherwise nothing inherent in this class which would require
// it to be used on the stack.
template <typename Policy>
class MOZ_STACK_CLASS OpIter : private Policy
{
    typedef typename Policy::Value Value;
    typedef typename Policy::ControlItem ControlItem;

    Decoder& d_;
    const ModuleEnvironment& env_;

    Vector<TypeAndValue<Value>, 8, SystemAllocPolicy> valueStack_;
    Vector<ControlStackEntry<ControlItem>, 8, SystemAllocPolicy> controlStack_;

#ifdef DEBUG
    OpBytes op_;
#endif
    size_t offsetOfLastReadOp_;

    MOZ_MUST_USE bool readFixedU8(uint8_t* out) {
        return d_.readFixedU8(out);
    }
    MOZ_MUST_USE bool readFixedU32(uint32_t* out) {
        return d_.readFixedU32(out);
    }
    MOZ_MUST_USE bool readVarS32(int32_t* out) {
        return d_.readVarS32(out);
    }
    MOZ_MUST_USE bool readVarU32(uint32_t* out) {
        return d_.readVarU32(out);
    }
    MOZ_MUST_USE bool readVarS64(int64_t* out) {
        return d_.readVarS64(out);
    }
    MOZ_MUST_USE bool readVarU64(uint64_t* out) {
        return d_.readVarU64(out);
    }
    MOZ_MUST_USE bool readFixedF32(float* out) {
        return d_.readFixedF32(out);
    }
    MOZ_MUST_USE bool readFixedF64(double* out) {
        return d_.readFixedF64(out);
    }
    MOZ_MUST_USE bool readFixedI8x16(I8x16* out) {
        return d_.readFixedI8x16(out);
    }
    MOZ_MUST_USE bool readFixedI16x8(I16x8* out) {
        return d_.readFixedI16x8(out);
    }
    MOZ_MUST_USE bool readFixedI32x4(I32x4* out) {
        return d_.readFixedI32x4(out);
    }
    MOZ_MUST_USE bool readFixedF32x4(F32x4* out) {
        return d_.readFixedF32x4(out);
    }

    MOZ_MUST_USE bool readAtomicViewType(Scalar::Type* viewType) {
        uint8_t x;
        if (!readFixedU8(&x))
            return fail("unable to read atomic view");
        if (x >= Scalar::MaxTypedArrayViewType)
            return fail("invalid atomic view type");
        *viewType = Scalar::Type(x);
        return true;
    }

    MOZ_MUST_USE bool readAtomicBinOpOp(jit::AtomicOp* op) {
        uint8_t x;
        if (!readFixedU8(&x))
            return fail("unable to read atomic opcode");
        switch (x) {
          case jit::AtomicFetchAddOp:
          case jit::AtomicFetchSubOp:
          case jit::AtomicFetchAndOp:
          case jit::AtomicFetchOrOp:
          case jit::AtomicFetchXorOp:
            break;
          default:
            return fail("unrecognized atomic binop");
        }
        *op = jit::AtomicOp(x);
        return true;
    }

    MOZ_MUST_USE bool readLinearMemoryAddress(uint32_t byteSize, LinearMemoryAddress<Value>* addr);
    MOZ_MUST_USE bool readLinearMemoryAddressAligned(uint32_t byteSize, LinearMemoryAddress<Value>* addr);
    MOZ_MUST_USE bool readBlockType(ExprType* expr);
    MOZ_MUST_USE bool popCallArgs(const ValTypeVector& expectedTypes, Vector<Value, 8, SystemAllocPolicy>* values);

    MOZ_MUST_USE bool popAnyType(StackType* type, Value* value);
    MOZ_MUST_USE bool typeMismatch(StackType actual, StackType expected);
    MOZ_MUST_USE bool popWithType(StackType expectedType, Value* value);
    MOZ_MUST_USE bool popWithType(ValType valType, Value* value) { return popWithType(ToStackType(valType), value); }
    MOZ_MUST_USE bool popWithType(ExprType expectedType, Value* value);
    MOZ_MUST_USE bool topWithType(ExprType expectedType, Value* value);
    MOZ_MUST_USE bool topWithType(ValType valType, Value* value);

    MOZ_MUST_USE bool pushControl(LabelKind kind, ExprType type);
    MOZ_MUST_USE bool checkStackAtEndOfBlock(ExprType* type, Value* value);
    MOZ_MUST_USE bool getControl(uint32_t relativeDepth, ControlStackEntry<ControlItem>** controlEntry);
    MOZ_MUST_USE bool checkBranchValue(uint32_t relativeDepth, ExprType* type, Value* value);
    MOZ_MUST_USE bool checkBrTableEntry(uint32_t* relativeDepth, ExprType* branchValueType, Value* branchValue);

    MOZ_MUST_USE bool push(StackType t) {
        return valueStack_.emplaceBack(t);
    }
    MOZ_MUST_USE bool push(ValType t) {
        return valueStack_.emplaceBack(t);
    }
    MOZ_MUST_USE bool push(ExprType t) {
        return IsVoid(t) || push(NonVoidToValType(t));
    }
    MOZ_MUST_USE bool push(TypeAndValue<Value> tv) {
        return valueStack_.append(tv);
    }
    void infalliblePush(StackType t) {
        valueStack_.infallibleEmplaceBack(t);
    }
    void infalliblePush(ValType t) {
        valueStack_.infallibleEmplaceBack(ToStackType(t));
    }
    void infalliblePush(TypeAndValue<Value> tv) {
        valueStack_.infallibleAppend(tv);
    }

    void afterUnconditionalBranch() {
        valueStack_.shrinkTo(controlStack_.back().valueStackStart());
        controlStack_.back().setPolymorphicBase();
    }

  public:
    typedef Vector<Value, 8, SystemAllocPolicy> ValueVector;

#ifdef DEBUG
    explicit OpIter(const ModuleEnvironment& env, Decoder& decoder)
      : d_(decoder), env_(env), op_(OpBytes(Op::Limit)), offsetOfLastReadOp_(0)
    {}
#else
    explicit OpIter(const ModuleEnvironment& env, Decoder& decoder)
      : d_(decoder), env_(env), offsetOfLastReadOp_(0)
    {}
#endif

    // Return the decoding byte offset.
    uint32_t currentOffset() const {
        return d_.currentOffset();
    }

    // Return the offset within the entire module of the last-read op.
    size_t lastOpcodeOffset() const {
        return offsetOfLastReadOp_ ? offsetOfLastReadOp_ : d_.currentOffset();
    }

    // Return a BytecodeOffset describing where the current op should be reported to trap/call.
    BytecodeOffset bytecodeOffset() const {
        return BytecodeOffset(lastOpcodeOffset());
    }

    // Test whether the iterator has reached the end of the buffer.
    bool done() const {
        return d_.done();
    }

    // Return a pointer to the end of the buffer being decoded by this iterator.
    const uint8_t* end() const {
        return d_.end();
    }

    // Report a general failure.
    MOZ_MUST_USE bool fail(const char* msg) MOZ_COLD;

    // Report an unrecognized opcode.
    MOZ_MUST_USE bool unrecognizedOpcode(const OpBytes* expr) MOZ_COLD;

    // Return whether the innermost block has a polymorphic base of its stack.
    // Ideally this accessor would be removed; consider using something else.
    bool currentBlockHasPolymorphicBase() const {
        return !controlStack_.empty() && controlStack_.back().polymorphicBase();
    }

    // ------------------------------------------------------------------------
    // Decoding and validation interface.

    MOZ_MUST_USE bool readOp(OpBytes* op);
    MOZ_MUST_USE bool readFunctionStart(ExprType ret);
    MOZ_MUST_USE bool readFunctionEnd(const uint8_t* bodyEnd);
    MOZ_MUST_USE bool readReturn(Value* value);
    MOZ_MUST_USE bool readBlock();
    MOZ_MUST_USE bool readLoop();
    MOZ_MUST_USE bool readIf(Value* condition);
    MOZ_MUST_USE bool readElse(ExprType* thenType, Value* thenValue);
    MOZ_MUST_USE bool readEnd(LabelKind* kind, ExprType* type, Value* value);
    void popEnd();
    MOZ_MUST_USE bool readBr(uint32_t* relativeDepth, ExprType* type, Value* value);
    MOZ_MUST_USE bool readBrIf(uint32_t* relativeDepth, ExprType* type,
                               Value* value, Value* condition);
    MOZ_MUST_USE bool readBrTable(Uint32Vector* depths, uint32_t* defaultDepth,
                                  ExprType* branchValueType, Value* branchValue, Value* index);
    MOZ_MUST_USE bool readUnreachable();
    MOZ_MUST_USE bool readDrop();
    MOZ_MUST_USE bool readUnary(ValType operandType, Value* input);
    MOZ_MUST_USE bool readConversion(ValType operandType, ValType resultType, Value* input);
    MOZ_MUST_USE bool readBinary(ValType operandType, Value* lhs, Value* rhs);
    MOZ_MUST_USE bool readComparison(ValType operandType, Value* lhs, Value* rhs);
    MOZ_MUST_USE bool readLoad(ValType resultType, uint32_t byteSize,
                               LinearMemoryAddress<Value>* addr);
    MOZ_MUST_USE bool readStore(ValType resultType, uint32_t byteSize,
                                LinearMemoryAddress<Value>* addr, Value* value);
    MOZ_MUST_USE bool readTeeStore(ValType resultType, uint32_t byteSize,
                                   LinearMemoryAddress<Value>* addr, Value* value);
    MOZ_MUST_USE bool readNop();
    MOZ_MUST_USE bool readCurrentMemory();
    MOZ_MUST_USE bool readGrowMemory(Value* input);
    MOZ_MUST_USE bool readSelect(StackType* type,
                                 Value* trueValue, Value* falseValue, Value* condition);
    MOZ_MUST_USE bool readGetLocal(const ValTypeVector& locals, uint32_t* id);
    MOZ_MUST_USE bool readSetLocal(const ValTypeVector& locals, uint32_t* id, Value* value);
    MOZ_MUST_USE bool readTeeLocal(const ValTypeVector& locals, uint32_t* id, Value* value);
    MOZ_MUST_USE bool readGetGlobal(uint32_t* id);
    MOZ_MUST_USE bool readSetGlobal(uint32_t* id, Value* value);
    MOZ_MUST_USE bool readTeeGlobal(uint32_t* id, Value* value);
    MOZ_MUST_USE bool readI32Const(int32_t* i32);
    MOZ_MUST_USE bool readI64Const(int64_t* i64);
    MOZ_MUST_USE bool readF32Const(float* f32);
    MOZ_MUST_USE bool readF64Const(double* f64);
    MOZ_MUST_USE bool readI8x16Const(I8x16* i8x16);
    MOZ_MUST_USE bool readI16x8Const(I16x8* i16x8);
    MOZ_MUST_USE bool readI32x4Const(I32x4* i32x4);
    MOZ_MUST_USE bool readF32x4Const(F32x4* f32x4);
    MOZ_MUST_USE bool readB8x16Const(I8x16* i8x16);
    MOZ_MUST_USE bool readB16x8Const(I16x8* i16x8);
    MOZ_MUST_USE bool readB32x4Const(I32x4* i32x4);
    MOZ_MUST_USE bool readCall(uint32_t* calleeIndex, ValueVector* argValues);
    MOZ_MUST_USE bool readCallIndirect(uint32_t* sigIndex, Value* callee, ValueVector* argValues);
    MOZ_MUST_USE bool readOldCallDirect(uint32_t numFuncImports, uint32_t* funcIndex,
                                        ValueVector* argValues);
    MOZ_MUST_USE bool readOldCallIndirect(uint32_t* sigIndex, Value* callee, ValueVector* argValues);
    MOZ_MUST_USE bool readWake(LinearMemoryAddress<Value>* addr, Value* count);
    MOZ_MUST_USE bool readWait(LinearMemoryAddress<Value>* addr,
                               ValType resultType,
                               uint32_t byteSize,
                               Value* value,
                               Value* timeout);
    MOZ_MUST_USE bool readAtomicLoad(LinearMemoryAddress<Value>* addr,
                                     ValType resultType,
                                     uint32_t byteSize);
    MOZ_MUST_USE bool readAtomicStore(LinearMemoryAddress<Value>* addr,
                                      ValType resultType,
                                      uint32_t byteSize,
                                      Value* value);
    MOZ_MUST_USE bool readAtomicRMW(LinearMemoryAddress<Value>* addr,
                                    ValType resultType,
                                    uint32_t byteSize,
                                    Value* value);
    MOZ_MUST_USE bool readAtomicCmpXchg(LinearMemoryAddress<Value>* addr,
                                        ValType resultType,
                                        uint32_t byteSize,
                                        Value* oldValue,
                                        Value* newValue);
    MOZ_MUST_USE bool readOldAtomicLoad(LinearMemoryAddress<Value>* addr,
                                        Scalar::Type* viewType);
    MOZ_MUST_USE bool readOldAtomicStore(LinearMemoryAddress<Value>* addr,
                                         Scalar::Type* viewType,
                                         Value* value);
    MOZ_MUST_USE bool readOldAtomicBinOp(LinearMemoryAddress<Value>* addr,
                                         Scalar::Type* viewType,
                                         jit::AtomicOp* op,
                                         Value* value);
    MOZ_MUST_USE bool readOldAtomicCompareExchange(LinearMemoryAddress<Value>* addr,
                                                   Scalar::Type* viewType,
                                                   Value* oldValue,
                                                   Value* newValue);
    MOZ_MUST_USE bool readOldAtomicExchange(LinearMemoryAddress<Value>* addr,
                                            Scalar::Type* viewType,
                                            Value* newValue);
    MOZ_MUST_USE bool readSimdComparison(ValType simdType, Value* lhs,
                                         Value* rhs);
    MOZ_MUST_USE bool readSimdShiftByScalar(ValType simdType, Value* lhs,
                                            Value* rhs);
    MOZ_MUST_USE bool readSimdBooleanReduction(ValType simdType, Value* input);
    MOZ_MUST_USE bool readExtractLane(ValType simdType, uint8_t* lane,
                                      Value* vector);
    MOZ_MUST_USE bool readReplaceLane(ValType simdType, uint8_t* lane,
                                      Value* vector, Value* scalar);
    MOZ_MUST_USE bool readSplat(ValType simdType, Value* scalar);
    MOZ_MUST_USE bool readSwizzle(ValType simdType, uint8_t (* lanes)[16], Value* vector);
    MOZ_MUST_USE bool readShuffle(ValType simdType, uint8_t (* lanes)[16],
                                  Value* lhs, Value* rhs);
    MOZ_MUST_USE bool readSimdSelect(ValType simdType, Value* trueValue,
                                     Value* falseValue,
                                     Value* condition);
    MOZ_MUST_USE bool readSimdCtor(ValType elementType, uint32_t numElements, ValType simdType,
                                   ValueVector* argValues);

    // At a location where readOp is allowed, peek at the next opcode
    // without consuming it or updating any internal state.
    // Never fails: returns uint16_t(Op::Limit) in op->b0 if it can't read.
    void peekOp(OpBytes* op);

    // ------------------------------------------------------------------------
    // Stack management.

    // Set the result value of the current top-of-value-stack expression.
    void setResult(Value value) {
        valueStack_.back().setValue(value);
    }

    // Return the result value of the current top-of-value-stack expression.
    Value getResult() {
        return valueStack_.back().value();
    }

    // Return a reference to the top of the control stack.
    ControlItem& controlItem() {
        return controlStack_.back().controlItem();
    }

    // Return a reference to an element in the control stack.
    ControlItem& controlItem(uint32_t relativeDepth) {
        return controlStack_[controlStack_.length() - 1 - relativeDepth].controlItem();
    }

    // Return a reference to the outermost element on the control stack.
    ControlItem& controlOutermost() {
        return controlStack_[0].controlItem();
    }

    // Test whether the control-stack is empty, meaning we've consumed the final
    // end of the function body.
    bool controlStackEmpty() const {
        return controlStack_.empty();
    }
};

template <typename Policy>
inline bool
OpIter<Policy>::unrecognizedOpcode(const OpBytes* expr)
{
    UniqueChars error(JS_smprintf("unrecognized opcode: %x %x", expr->b0,
                                  IsPrefixByte(expr->b0) ? expr->b1 : 0));
    if (!error)
        return false;

    return fail(error.get());
}

template <typename Policy>
inline bool
OpIter<Policy>::fail(const char* msg)
{
    return d_.fail(lastOpcodeOffset(), msg);
}

// This function pops exactly one value from the stack, yielding Any types in
// various cases and therefore making it the caller's responsibility to do the
// right thing for StackType::Any. Prefer (pop|top)WithType.
template <typename Policy>
inline bool
OpIter<Policy>::popAnyType(StackType* type, Value* value)
{
    ControlStackEntry<ControlItem>& block = controlStack_.back();

    MOZ_ASSERT(valueStack_.length() >= block.valueStackStart());
    if (MOZ_UNLIKELY(valueStack_.length() == block.valueStackStart())) {
        // If the base of this block's stack is polymorphic, then we can pop a
        // dummy value of any type; it won't be used since we're in unreachable code.
        if (block.polymorphicBase()) {
            *type = StackType::Any;
            *value = Value();

            // Maintain the invariant that, after a pop, there is always memory
            // reserved to push a value infallibly.
            return valueStack_.reserve(valueStack_.length() + 1);
        }

        if (valueStack_.empty())
            return fail("popping value from empty stack");
        return fail("popping value from outside block");
    }

    TypeAndValue<Value>& tv = valueStack_.back();
    *type = tv.type();
    *value = tv.value();
    valueStack_.popBack();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::typeMismatch(StackType actual, StackType expected)
{
    UniqueChars error(JS_smprintf("type mismatch: expression has type %s but expected %s",
                                  ToCString(NonAnyToValType(actual)),
                                  ToCString(NonAnyToValType(expected))));
    if (!error)
        return false;

    return fail(error.get());
}

// This function pops exactly one value from the stack, checking that it has the
// expected type which can either be a specific value type or a type variable.
template <typename Policy>
inline bool
OpIter<Policy>::popWithType(StackType expectedType, Value* value)
{
    ControlStackEntry<ControlItem>& block = controlStack_.back();

    MOZ_ASSERT(valueStack_.length() >= block.valueStackStart());
    if (MOZ_UNLIKELY(valueStack_.length() == block.valueStackStart())) {
        // If the base of this block's stack is polymorphic, then we can pop a
        // dummy value of any expected type; it won't be used since we're in
        // unreachable code.
        if (block.polymorphicBase()) {
            *value = Value();

            // Maintain the invariant that, after a pop, there is always memory
            // reserved to push a value infallibly.
            return valueStack_.reserve(valueStack_.length() + 1);
        }

        if (valueStack_.empty())
            return fail("popping value from empty stack");
        return fail("popping value from outside block");
    }

    TypeAndValue<Value> tv = valueStack_.popCopy();

    StackType _;
    if (MOZ_UNLIKELY(!Unify(tv.type(), expectedType, &_)))
        return typeMismatch(tv.type(), expectedType);

    *value = tv.value();
    return true;
}

// This function pops as many types from the stack as determined by the given
// signature. Currently, all signatures are limited to 0 or 1 types, with
// ExprType::Void meaning 0 and all other ValTypes meaning 1, but this will be
// generalized in the future.
template <typename Policy>
inline bool
OpIter<Policy>::popWithType(ExprType expectedType, Value* value)
{
    if (IsVoid(expectedType)) {
        *value = Value();
        return true;
    }

    return popWithType(NonVoidToValType(expectedType), value);
}

// This function is just an optimization of popWithType + push.
template <typename Policy>
inline bool
OpIter<Policy>::topWithType(ValType expectedType, Value* value)
{
    ControlStackEntry<ControlItem>& block = controlStack_.back();

    MOZ_ASSERT(valueStack_.length() >= block.valueStackStart());
    if (MOZ_UNLIKELY(valueStack_.length() == block.valueStackStart())) {
        // If the base of this block's stack is polymorphic, then we can just
        // pull out a dummy value of the expected type; it won't be used since
        // we're in unreachable code. We must however push this value onto the
        // stack since it is now fixed to a specific type by this type
        // constraint.
        if (block.polymorphicBase()) {
            if (!valueStack_.emplaceBack(expectedType, Value()))
                return false;

            *value = Value();
            return true;
        }

        if (valueStack_.empty())
            return fail("reading value from empty stack");
        return fail("reading value from outside block");
    }

    TypeAndValue<Value>& tv = valueStack_.back();

    if (MOZ_UNLIKELY(!Unify(tv.type(), ToStackType(expectedType), &tv.typeRef())))
        return typeMismatch(tv.type(), ToStackType(expectedType));

    *value = tv.value();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::topWithType(ExprType expectedType, Value* value)
{
    if (IsVoid(expectedType)) {
        *value = Value();
        return true;
    }

    return topWithType(NonVoidToValType(expectedType), value);
}

template <typename Policy>
inline bool
OpIter<Policy>::pushControl(LabelKind kind, ExprType type)
{
    return controlStack_.emplaceBack(kind, type, valueStack_.length());
}

template <typename Policy>
inline bool
OpIter<Policy>::checkStackAtEndOfBlock(ExprType* type, Value* value)
{
    ControlStackEntry<ControlItem>& block = controlStack_.back();

    MOZ_ASSERT(valueStack_.length() >= block.valueStackStart());
    size_t pushed = valueStack_.length() - block.valueStackStart();
    if (pushed > (IsVoid(block.resultType()) ? 0u : 1u))
        return fail("unused values not explicitly dropped by end of block");

    if (!topWithType(block.resultType(), value))
        return false;

    *type = block.resultType();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::getControl(uint32_t relativeDepth, ControlStackEntry<ControlItem>** controlEntry)
{
    if (relativeDepth >= controlStack_.length())
        return fail("branch depth exceeds current nesting level");

    *controlEntry = &controlStack_[controlStack_.length() - 1 - relativeDepth];
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readBlockType(ExprType* type)
{
    uint8_t unchecked;
    if (!d_.readBlockType(&unchecked))
        return fail("unable to read block signature");

    switch (unchecked) {
      case uint8_t(ExprType::Void):
      case uint8_t(ExprType::I32):
      case uint8_t(ExprType::I64):
      case uint8_t(ExprType::F32):
      case uint8_t(ExprType::F64):
      case uint8_t(ExprType::I8x16):
      case uint8_t(ExprType::I16x8):
      case uint8_t(ExprType::I32x4):
      case uint8_t(ExprType::F32x4):
      case uint8_t(ExprType::B8x16):
      case uint8_t(ExprType::B16x8):
      case uint8_t(ExprType::B32x4):
        break;
      default:
        return fail("invalid inline block type");
    }

    *type = ExprType(unchecked);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readOp(OpBytes* op)
{
    MOZ_ASSERT(!controlStack_.empty());

    offsetOfLastReadOp_ = d_.currentOffset();

    if (MOZ_UNLIKELY(!d_.readOp(op)))
        return fail("unable to read opcode");

#ifdef DEBUG
    op_ = *op;
#endif

    return true;
}

template <typename Policy>
inline void
OpIter<Policy>::peekOp(OpBytes* op)
{
    const uint8_t* pos = d_.currentPosition();

    if (MOZ_UNLIKELY(!d_.readOp(op)))
        op->b0 = uint16_t(Op::Limit);

    d_.rollbackPosition(pos);
}

template <typename Policy>
inline bool
OpIter<Policy>::readFunctionStart(ExprType ret)
{
    MOZ_ASSERT(valueStack_.empty());
    MOZ_ASSERT(controlStack_.empty());
    MOZ_ASSERT(op_.b0 == uint16_t(Op::Limit));

    return pushControl(LabelKind::Block, ret);
}

template <typename Policy>
inline bool
OpIter<Policy>::readFunctionEnd(const uint8_t* bodyEnd)
{
    if (d_.currentPosition() != bodyEnd)
        return fail("function body length mismatch");

    if (!controlStack_.empty())
        return fail("unbalanced function body control flow");

#ifdef DEBUG
    op_ = OpBytes(Op::Limit);
#endif
    valueStack_.clear();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readReturn(Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Return);

    ControlStackEntry<ControlItem>& body = controlStack_[0];
    MOZ_ASSERT(body.kind() == LabelKind::Block);

    if (!popWithType(body.resultType(), value))
        return false;

    afterUnconditionalBranch();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readBlock()
{
    MOZ_ASSERT(Classify(op_) == OpKind::Block);

    ExprType type = ExprType::Limit;
    if (!readBlockType(&type))
        return false;

    return pushControl(LabelKind::Block, type);
}

template <typename Policy>
inline bool
OpIter<Policy>::readLoop()
{
    MOZ_ASSERT(Classify(op_) == OpKind::Loop);

    ExprType type = ExprType::Limit;
    if (!readBlockType(&type))
        return false;

    return pushControl(LabelKind::Loop, type);
}

template <typename Policy>
inline bool
OpIter<Policy>::readIf(Value* condition)
{
    MOZ_ASSERT(Classify(op_) == OpKind::If);

    ExprType type = ExprType::Limit;
    if (!readBlockType(&type))
        return false;

    if (!popWithType(ValType::I32, condition))
        return false;

    return pushControl(LabelKind::Then, type);
}

template <typename Policy>
inline bool
OpIter<Policy>::readElse(ExprType* type, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Else);

    // Finish checking the then-block.

    if (!checkStackAtEndOfBlock(type, value))
        return false;

    ControlStackEntry<ControlItem>& block = controlStack_.back();

    if (block.kind() != LabelKind::Then)
        return fail("else can only be used within an if");

    // Switch to the else-block.

    if (!IsVoid(block.resultType()))
        valueStack_.popBack();

    MOZ_ASSERT(valueStack_.length() == block.valueStackStart());

    block.switchToElse();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readEnd(LabelKind* kind, ExprType* type, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::End);

    if (!checkStackAtEndOfBlock(type, value))
        return false;

    ControlStackEntry<ControlItem>& block = controlStack_.back();

    // If an `if` block ends with `end` instead of `else`, then we must
    // additionally validate that the then-block doesn't push anything.
    if (block.kind() == LabelKind::Then && !IsVoid(block.resultType()))
        return fail("if without else with a result value");

    *kind = block.kind();
    return true;
}

template <typename Policy>
inline void
OpIter<Policy>::popEnd()
{
    MOZ_ASSERT(Classify(op_) == OpKind::End);

    controlStack_.popBack();
}

template <typename Policy>
inline bool
OpIter<Policy>::checkBranchValue(uint32_t relativeDepth, ExprType* type, Value* value)
{
    ControlStackEntry<ControlItem>* block = nullptr;
    if (!getControl(relativeDepth, &block))
        return false;

    *type = block->branchTargetType();
    return topWithType(*type, value);
}

template <typename Policy>
inline bool
OpIter<Policy>::readBr(uint32_t* relativeDepth, ExprType* type, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Br);

    if (!readVarU32(relativeDepth))
        return fail("unable to read br depth");

    if (!checkBranchValue(*relativeDepth, type, value))
        return false;

    afterUnconditionalBranch();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readBrIf(uint32_t* relativeDepth, ExprType* type, Value* value, Value* condition)
{
    MOZ_ASSERT(Classify(op_) == OpKind::BrIf);

    if (!readVarU32(relativeDepth))
        return fail("unable to read br_if depth");

    if (!popWithType(ValType::I32, condition))
        return false;

    return checkBranchValue(*relativeDepth, type, value);
}

template <typename Policy>
inline bool
OpIter<Policy>::checkBrTableEntry(uint32_t* relativeDepth, ExprType* branchValueType,
                                  Value* branchValue)
{
    if (!readVarU32(relativeDepth))
        return false;

    // For the first encountered branch target, do a normal branch value type
    // check which will change *branchValueType to a non-sentinel value. For all
    // subsequent branch targets, check that the branch target matches the
    // now-known branch value type.

    if (*branchValueType == ExprType::Limit) {
        if (!checkBranchValue(*relativeDepth, branchValueType, branchValue))
            return false;
    } else {
        ControlStackEntry<ControlItem>* block = nullptr;
        if (!getControl(*relativeDepth, &block))
            return false;

        if (*branchValueType != block->branchTargetType())
            return fail("br_table targets must all have the same value type");
    }

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readBrTable(Uint32Vector* depths, uint32_t* defaultDepth,
                            ExprType* branchValueType, Value* branchValue, Value* index)
{
    MOZ_ASSERT(Classify(op_) == OpKind::BrTable);

    uint32_t tableLength;
    if (!readVarU32(&tableLength))
        return fail("unable to read br_table table length");

    if (tableLength > MaxBrTableElems)
        return fail("br_table too big");

    if (!popWithType(ValType::I32, index))
        return false;

    if (!depths->resize(tableLength))
        return false;

    *branchValueType = ExprType::Limit;

    for (uint32_t i = 0; i < tableLength; i++) {
        if (!checkBrTableEntry(&(*depths)[i], branchValueType, branchValue))
            return false;
    }

    if (!checkBrTableEntry(defaultDepth, branchValueType, branchValue))
        return false;

    MOZ_ASSERT(*branchValueType != ExprType::Limit);

    afterUnconditionalBranch();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readUnreachable()
{
    MOZ_ASSERT(Classify(op_) == OpKind::Unreachable);

    afterUnconditionalBranch();
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readDrop()
{
    MOZ_ASSERT(Classify(op_) == OpKind::Drop);
    StackType type;
    Value value;
    return popAnyType(&type, &value);
}

template <typename Policy>
inline bool
OpIter<Policy>::readUnary(ValType operandType, Value* input)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Unary);

    if (!popWithType(operandType, input))
        return false;

    infalliblePush(operandType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readConversion(ValType operandType, ValType resultType, Value* input)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Conversion);

    if (!popWithType(operandType, input))
        return false;

    infalliblePush(resultType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readBinary(ValType operandType, Value* lhs, Value* rhs)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Binary);

    if (!popWithType(operandType, rhs))
        return false;

    if (!popWithType(operandType, lhs))
        return false;

    infalliblePush(operandType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readComparison(ValType operandType, Value* lhs, Value* rhs)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Comparison);

    if (!popWithType(operandType, rhs))
        return false;

    if (!popWithType(operandType, lhs))
        return false;

    infalliblePush(ValType::I32);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readLinearMemoryAddress(uint32_t byteSize, LinearMemoryAddress<Value>* addr)
{
    if (!env_.usesMemory())
        return fail("can't touch memory without memory");

    uint8_t alignLog2;
    if (!readFixedU8(&alignLog2))
        return fail("unable to read load alignment");

    if (!readVarU32(&addr->offset))
        return fail("unable to read load offset");

    if (alignLog2 >= 32 || (uint32_t(1) << alignLog2) > byteSize)
        return fail("greater than natural alignment");

    if (!popWithType(ValType::I32, &addr->base))
        return false;

    addr->align = uint32_t(1) << alignLog2;
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readLinearMemoryAddressAligned(uint32_t byteSize, LinearMemoryAddress<Value>* addr)
{
    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    if (addr->align != byteSize)
        return fail("not natural alignment");

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readLoad(ValType resultType, uint32_t byteSize, LinearMemoryAddress<Value>* addr)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Load);

    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    infalliblePush(resultType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readStore(ValType resultType, uint32_t byteSize, LinearMemoryAddress<Value>* addr,
                          Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Store);

    if (!popWithType(resultType, value))
        return false;

    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readTeeStore(ValType resultType, uint32_t byteSize, LinearMemoryAddress<Value>* addr,
                             Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::TeeStore);

    if (!popWithType(resultType, value))
        return false;

    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    infalliblePush(TypeAndValue<Value>(resultType, *value));
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readNop()
{
    MOZ_ASSERT(Classify(op_) == OpKind::Nop);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readCurrentMemory()
{
    MOZ_ASSERT(Classify(op_) == OpKind::CurrentMemory);

    if (!env_.usesMemory())
        return fail("can't touch memory without memory");

    uint8_t flags;
    if (!readFixedU8(&flags))
        return false;

    if (flags != uint8_t(MemoryTableFlags::Default))
        return fail("unexpected flags");

    return push(ValType::I32);
}

template <typename Policy>
inline bool
OpIter<Policy>::readGrowMemory(Value* input)
{
    MOZ_ASSERT(Classify(op_) == OpKind::GrowMemory);

    if (!env_.usesMemory())
        return fail("can't touch memory without memory");

    uint8_t flags;
    if (!readFixedU8(&flags))
        return false;

    if (flags != uint8_t(MemoryTableFlags::Default))
        return fail("unexpected flags");

    if (!popWithType(ValType::I32, input))
        return false;

    infalliblePush(ValType::I32);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readSelect(StackType* type, Value* trueValue, Value* falseValue, Value* condition)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Select);

    if (!popWithType(ValType::I32, condition))
        return false;

    StackType falseType;
    if (!popAnyType(&falseType, falseValue))
        return false;

    StackType trueType;
    if (!popAnyType(&trueType, trueValue))
        return false;

    if (!Unify(falseType, trueType, type))
        return fail("select operand types must match");

    infalliblePush(*type);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readGetLocal(const ValTypeVector& locals, uint32_t* id)
{
    MOZ_ASSERT(Classify(op_) == OpKind::GetLocal);

    if (!readVarU32(id))
        return false;

    if (*id >= locals.length())
        return fail("get_local index out of range");

    return push(locals[*id]);
}

template <typename Policy>
inline bool
OpIter<Policy>::readSetLocal(const ValTypeVector& locals, uint32_t* id, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::SetLocal);

    if (!readVarU32(id))
        return false;

    if (*id >= locals.length())
        return fail("set_local index out of range");

    return popWithType(locals[*id], value);
}

template <typename Policy>
inline bool
OpIter<Policy>::readTeeLocal(const ValTypeVector& locals, uint32_t* id, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::TeeLocal);

    if (!readVarU32(id))
        return false;

    if (*id >= locals.length())
        return fail("set_local index out of range");

    return topWithType(locals[*id], value);
}

template <typename Policy>
inline bool
OpIter<Policy>::readGetGlobal(uint32_t* id)
{
    MOZ_ASSERT(Classify(op_) == OpKind::GetGlobal);

    if (!readVarU32(id))
        return false;

    if (*id >= env_.globals.length())
        return fail("get_global index out of range");

    return push(env_.globals[*id].type());
}

template <typename Policy>
inline bool
OpIter<Policy>::readSetGlobal(uint32_t* id, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::SetGlobal);

    if (!readVarU32(id))
        return false;

    if (*id >= env_.globals.length())
        return fail("set_global index out of range");

    if (!env_.globals[*id].isMutable())
        return fail("can't write an immutable global");

    return popWithType(env_.globals[*id].type(), value);
}

template <typename Policy>
inline bool
OpIter<Policy>::readTeeGlobal(uint32_t* id, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::TeeGlobal);

    if (!readVarU32(id))
        return false;

    if (*id >= env_.globals.length())
        return fail("set_global index out of range");

    if (!env_.globals[*id].isMutable())
        return fail("can't write an immutable global");

    return topWithType(env_.globals[*id].type(), value);
}

template <typename Policy>
inline bool
OpIter<Policy>::readI32Const(int32_t* i32)
{
    MOZ_ASSERT(Classify(op_) == OpKind::I32);

    return readVarS32(i32) &&
           push(ValType::I32);
}

template <typename Policy>
inline bool
OpIter<Policy>::readI64Const(int64_t* i64)
{
    MOZ_ASSERT(Classify(op_) == OpKind::I64);

    return readVarS64(i64) &&
           push(ValType::I64);
}

template <typename Policy>
inline bool
OpIter<Policy>::readF32Const(float* f32)
{
    MOZ_ASSERT(Classify(op_) == OpKind::F32);

    return readFixedF32(f32) &&
           push(ValType::F32);
}

template <typename Policy>
inline bool
OpIter<Policy>::readF64Const(double* f64)
{
    MOZ_ASSERT(Classify(op_) == OpKind::F64);

    return readFixedF64(f64) &&
           push(ValType::F64);
}

template <typename Policy>
inline bool
OpIter<Policy>::readI8x16Const(I8x16* i8x16)
{
    MOZ_ASSERT(Classify(op_) == OpKind::I8x16);

    return readFixedI8x16(i8x16) &&
           push(ValType::I8x16);
}

template <typename Policy>
inline bool
OpIter<Policy>::readI16x8Const(I16x8* i16x8)
{
    MOZ_ASSERT(Classify(op_) == OpKind::I16x8);

    return readFixedI16x8(i16x8) &&
           push(ValType::I16x8);
}

template <typename Policy>
inline bool
OpIter<Policy>::readI32x4Const(I32x4* i32x4)
{
    MOZ_ASSERT(Classify(op_) == OpKind::I32x4);

    return readFixedI32x4(i32x4) &&
           push(ValType::I32x4);
}

template <typename Policy>
inline bool
OpIter<Policy>::readF32x4Const(F32x4* f32x4)
{
    MOZ_ASSERT(Classify(op_) == OpKind::F32x4);

    return readFixedF32x4(f32x4) &&
           push(ValType::F32x4);
}

template <typename Policy>
inline bool
OpIter<Policy>::readB8x16Const(I8x16* i8x16)
{
    MOZ_ASSERT(Classify(op_) == OpKind::B8x16);

    return readFixedI8x16(i8x16) &&
           push(ValType::B8x16);
}

template <typename Policy>
inline bool
OpIter<Policy>::readB16x8Const(I16x8* i16x8)
{
    MOZ_ASSERT(Classify(op_) == OpKind::B16x8);

    return readFixedI16x8(i16x8) &&
           push(ValType::B16x8);
}

template <typename Policy>
inline bool
OpIter<Policy>::readB32x4Const(I32x4* i32x4)
{
    MOZ_ASSERT(Classify(op_) == OpKind::B32x4);

    return readFixedI32x4(i32x4) &&
           push(ValType::B32x4);
}

template <typename Policy>
inline bool
OpIter<Policy>::popCallArgs(const ValTypeVector& expectedTypes, ValueVector* values)
{
    // Iterate through the argument types backward so that pops occur in the
    // right order.

    if (!values->resize(expectedTypes.length()))
        return false;

    for (int32_t i = expectedTypes.length() - 1; i >= 0; i--) {
        if (!popWithType(expectedTypes[i], &(*values)[i]))
            return false;
    }

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readCall(uint32_t* funcIndex, ValueVector* argValues)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Call);

    if (!readVarU32(funcIndex))
        return fail("unable to read call function index");

    if (*funcIndex >= env_.funcSigs.length())
        return fail("callee index out of range");

    const Sig& sig = *env_.funcSigs[*funcIndex];

    if (!popCallArgs(sig.args(), argValues))
        return false;

    return push(sig.ret());
}

template <typename Policy>
inline bool
OpIter<Policy>::readCallIndirect(uint32_t* sigIndex, Value* callee, ValueVector* argValues)
{
    MOZ_ASSERT(Classify(op_) == OpKind::CallIndirect);

    if (!env_.tables.length())
        return fail("can't call_indirect without a table");

    if (!readVarU32(sigIndex))
        return fail("unable to read call_indirect signature index");

    if (*sigIndex >= env_.numSigs())
        return fail("signature index out of range");

    uint8_t flags;
    if (!readFixedU8(&flags))
        return false;

    if (flags != uint8_t(MemoryTableFlags::Default))
        return fail("unexpected flags");

    if (!popWithType(ValType::I32, callee))
        return false;

    const Sig& sig = env_.sigs[*sigIndex];

    if (!popCallArgs(sig.args(), argValues))
        return false;

    return push(sig.ret());
}

template <typename Policy>
inline bool
OpIter<Policy>::readOldCallDirect(uint32_t numFuncImports, uint32_t* funcIndex,
                                  ValueVector* argValues)
{
    MOZ_ASSERT(Classify(op_) == OpKind::OldCallDirect);

    uint32_t funcDefIndex;
    if (!readVarU32(&funcDefIndex))
        return fail("unable to read call function index");

    if (UINT32_MAX - funcDefIndex < numFuncImports)
        return fail("callee index out of range");

    *funcIndex = numFuncImports + funcDefIndex;

    if (*funcIndex >= env_.funcSigs.length())
        return fail("callee index out of range");

    const Sig& sig = *env_.funcSigs[*funcIndex];

    if (!popCallArgs(sig.args(), argValues))
        return false;

    return push(sig.ret());
}

template <typename Policy>
inline bool
OpIter<Policy>::readOldCallIndirect(uint32_t* sigIndex, Value* callee, ValueVector* argValues)
{
    MOZ_ASSERT(Classify(op_) == OpKind::OldCallIndirect);

    if (!readVarU32(sigIndex))
        return fail("unable to read call_indirect signature index");

    if (*sigIndex >= env_.numSigs())
        return fail("signature index out of range");

    const Sig& sig = env_.sigs[*sigIndex];

    if (!popCallArgs(sig.args(), argValues))
        return false;

    if (!popWithType(ValType::I32, callee))
        return false;

    if (!push(sig.ret()))
        return false;

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readWake(LinearMemoryAddress<Value>* addr, Value* count)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Wake);

    if (!env_.usesSharedMemory())
        return fail("can't touch memory with atomic operations without shared memory");

    if (!popWithType(ValType::I32, count))
        return false;

    uint32_t byteSize = 4;      // Per spec; smallest WAIT is i32.

    if (!readLinearMemoryAddressAligned(byteSize, addr))
        return false;

    infalliblePush(ValType::I32);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readWait(LinearMemoryAddress<Value>* addr,
                         ValType valueType,
                         uint32_t byteSize,
                         Value* value,
                         Value* timeout)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Wait);

    if (!env_.usesSharedMemory())
        return fail("can't touch memory with atomic operations without shared memory");

    if (!popWithType(ValType::I64, timeout))
        return false;

    if (!popWithType(valueType, value))
        return false;

    if (!readLinearMemoryAddressAligned(byteSize, addr))
        return false;

    infalliblePush(ValType::I32);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readAtomicLoad(LinearMemoryAddress<Value>* addr, ValType resultType,
                               uint32_t byteSize)
{
    MOZ_ASSERT(Classify(op_) == OpKind::AtomicLoad);

    if (!env_.usesSharedMemory())
        return fail("can't touch memory with atomic operations without shared memory");

    if (!readLinearMemoryAddressAligned(byteSize, addr))
        return false;

    infalliblePush(resultType);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readAtomicStore(LinearMemoryAddress<Value>* addr, ValType resultType,
                                uint32_t byteSize, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::AtomicStore);

    if (!env_.usesSharedMemory())
        return fail("can't touch memory with atomic operations without shared memory");

    if (!popWithType(resultType, value))
        return false;

    if (!readLinearMemoryAddressAligned(byteSize, addr))
        return false;

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readAtomicRMW(LinearMemoryAddress<Value>* addr, ValType resultType,
                              uint32_t byteSize, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::AtomicBinOp);

    if (!env_.usesSharedMemory())
        return fail("can't touch memory with atomic operations without shared memory");

    if (!popWithType(resultType, value))
        return false;

    if (!readLinearMemoryAddressAligned(byteSize, addr))
        return false;

    infalliblePush(resultType);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readAtomicCmpXchg(LinearMemoryAddress<Value>* addr, ValType resultType,
                                  uint32_t byteSize, Value* oldValue, Value* newValue)
{
    MOZ_ASSERT(Classify(op_) == OpKind::AtomicCompareExchange);

    if (!env_.usesSharedMemory())
        return fail("can't touch memory with atomic operations without shared memory");

    if (!popWithType(resultType, newValue))
        return false;

    if (!popWithType(resultType, oldValue))
        return false;

    if (!readLinearMemoryAddressAligned(byteSize, addr))
        return false;

    infalliblePush(resultType);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readOldAtomicLoad(LinearMemoryAddress<Value>* addr, Scalar::Type* viewType)
{
    MOZ_ASSERT(Classify(op_) == OpKind::OldAtomicLoad);

    if (!readAtomicViewType(viewType))
        return false;

    uint32_t byteSize = Scalar::byteSize(*viewType);
    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    infalliblePush(ValType::I32);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readOldAtomicStore(LinearMemoryAddress<Value>* addr, Scalar::Type* viewType,
                                   Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::OldAtomicStore);

    if (!readAtomicViewType(viewType))
        return false;

    uint32_t byteSize = Scalar::byteSize(*viewType);
    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    if (!popWithType(ValType::I32, value))
        return false;

    infalliblePush(ValType::I32);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readOldAtomicBinOp(LinearMemoryAddress<Value>* addr, Scalar::Type* viewType,
                                   jit::AtomicOp* op, Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::OldAtomicBinOp);

    if (!readAtomicViewType(viewType))
        return false;

    if (!readAtomicBinOpOp(op))
        return false;

    uint32_t byteSize = Scalar::byteSize(*viewType);
    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    if (!popWithType(ValType::I32, value))
        return false;

    infalliblePush(ValType::I32);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readOldAtomicCompareExchange(LinearMemoryAddress<Value>* addr,
                                             Scalar::Type* viewType, Value* oldValue,
                                             Value* newValue)
{
    MOZ_ASSERT(Classify(op_) == OpKind::OldAtomicCompareExchange);

    if (!readAtomicViewType(viewType))
        return false;

    uint32_t byteSize = Scalar::byteSize(*viewType);
    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    if (!popWithType(ValType::I32, newValue))
        return false;

    if (!popWithType(ValType::I32, oldValue))
        return false;

    infalliblePush(ValType::I32);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readOldAtomicExchange(LinearMemoryAddress<Value>* addr, Scalar::Type* viewType,
                                      Value* value)
{
    MOZ_ASSERT(Classify(op_) == OpKind::OldAtomicExchange);

    if (!readAtomicViewType(viewType))
        return false;

    uint32_t byteSize = Scalar::byteSize(*viewType);
    if (!readLinearMemoryAddress(byteSize, addr))
        return false;

    if (!popWithType(ValType::I32, value))
        return false;

    infalliblePush(ValType::I32);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readSimdComparison(ValType simdType, Value* lhs, Value* rhs)
{
    MOZ_ASSERT(Classify(op_) == OpKind::SimdComparison);

    if (!popWithType(simdType, rhs))
        return false;

    if (!popWithType(simdType, lhs))
        return false;

    infalliblePush(SimdBoolType(simdType));

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readSimdShiftByScalar(ValType simdType, Value* lhs, Value* rhs)
{
    MOZ_ASSERT(Classify(op_) == OpKind::SimdShiftByScalar);

    if (!popWithType(ValType::I32, rhs))
        return false;

    if (!popWithType(simdType, lhs))
        return false;

    infalliblePush(simdType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readSimdBooleanReduction(ValType simdType, Value* input)
{
    MOZ_ASSERT(Classify(op_) == OpKind::SimdBooleanReduction);

    if (!popWithType(simdType, input))
        return false;

    infalliblePush(ValType::I32);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readExtractLane(ValType simdType, uint8_t* lane, Value* vector)
{
    MOZ_ASSERT(Classify(op_) == OpKind::ExtractLane);

    uint32_t laneBits;
    if (!readVarU32(&laneBits))
        return false;

    if (laneBits >= NumSimdElements(simdType))
        return fail("simd lane out of bounds for simd type");

    *lane = uint8_t(laneBits);

    if (!popWithType(simdType, vector))
        return false;

    infalliblePush(SimdElementType(simdType));
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readReplaceLane(ValType simdType, uint8_t* lane, Value* vector, Value* scalar)
{
    MOZ_ASSERT(Classify(op_) == OpKind::ReplaceLane);

    uint32_t laneBits;
    if (!readVarU32(&laneBits))
        return false;

    if (laneBits >= NumSimdElements(simdType))
        return fail("simd lane out of bounds for simd type");

    *lane = uint8_t(laneBits);

    if (!popWithType(SimdElementType(simdType), scalar))
        return false;

    if (!popWithType(simdType, vector))
        return false;

    infalliblePush(simdType);
    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readSplat(ValType simdType, Value* scalar)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Splat);

    if (!popWithType(SimdElementType(simdType), scalar))
        return false;

    infalliblePush(simdType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readSwizzle(ValType simdType, uint8_t (* lanes)[16], Value* vector)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Swizzle);

    uint32_t numSimdLanes = NumSimdElements(simdType);
    MOZ_ASSERT(numSimdLanes <= mozilla::ArrayLength(*lanes));
    for (uint32_t i = 0; i < numSimdLanes; ++i) {
        if (!readFixedU8(&(*lanes)[i]))
            return fail("unable to read swizzle lane");
        if ((*lanes)[i] >= numSimdLanes)
            return fail("swizzle index out of bounds");
    }

    if (!popWithType(simdType, vector))
        return false;

    infalliblePush(simdType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readShuffle(ValType simdType, uint8_t (* lanes)[16], Value* lhs, Value* rhs)
{
    MOZ_ASSERT(Classify(op_) == OpKind::Shuffle);

    uint32_t numSimdLanes = NumSimdElements(simdType);
    MOZ_ASSERT(numSimdLanes <= mozilla::ArrayLength(*lanes));
    for (uint32_t i = 0; i < numSimdLanes; ++i) {
        if (!readFixedU8(&(*lanes)[i]))
            return fail("unable to read shuffle lane");
        if ((*lanes)[i] >= numSimdLanes * 2)
            return fail("shuffle index out of bounds");
    }

    if (!popWithType(simdType, rhs))
        return false;

    if (!popWithType(simdType, lhs))
        return false;

    infalliblePush(simdType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readSimdSelect(ValType simdType, Value* trueValue, Value* falseValue,
                               Value* condition)
{
    MOZ_ASSERT(Classify(op_) == OpKind::SimdSelect);

    if (!popWithType(simdType, falseValue))
        return false;
    if (!popWithType(simdType, trueValue))
        return false;
    if (!popWithType(SimdBoolType(simdType), condition))
        return false;

    infalliblePush(simdType);

    return true;
}

template <typename Policy>
inline bool
OpIter<Policy>::readSimdCtor(ValType elementType, uint32_t numElements, ValType simdType,
                             ValueVector* argValues)
{
    MOZ_ASSERT(Classify(op_) == OpKind::SimdCtor);

    if (!argValues->resize(numElements))
        return false;

    for (int32_t i = numElements - 1; i >= 0; i--) {
        if (!popWithType(elementType, &(*argValues)[i]))
            return false;
    }

    infalliblePush(simdType);

    return true;
}

} // namespace wasm
} // namespace js

namespace mozilla {

// Specialize IsPod for the Nothing specializations.
template<> struct IsPod<js::wasm::TypeAndValue<Nothing>> : TrueType {};
template<> struct IsPod<js::wasm::ControlStackEntry<Nothing>> : TrueType {};

} // namespace mozilla

#endif // wasm_iterator_h
