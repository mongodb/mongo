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

#ifndef wasm_op_iter_h
#define wasm_op_iter_h

#include "mozilla/CompactPair.h"
#include "mozilla/Poison.h"

#include <type_traits>

#include "jit/AtomicOp.h"
#include "js/Printf.h"
#include "wasm/WasmUtility.h"
#include "wasm/WasmValidate.h"

namespace js {
namespace wasm {

// The kind of a control-flow stack item.
enum class LabelKind : uint8_t {
  Body,
  Block,
  Loop,
  Then,
  Else,
#ifdef ENABLE_WASM_EXCEPTIONS
  Try,
  Catch,
  CatchAll,
#endif
};

// The type of values on the operand stack during validation.  This is either a
// ValType or the special type "Bottom".

class StackType {
  PackedTypeCode tc_;

  explicit StackType(PackedTypeCode tc) : tc_(tc) {}

 public:
  StackType() : tc_(PackedTypeCode::invalid()) {}

  explicit StackType(const ValType& t) : tc_(t.packed()) {
    MOZ_ASSERT(tc_.isValid());
    MOZ_ASSERT(!isBottom());
  }

  static StackType bottom() {
    return StackType(PackedTypeCode::pack(TypeCode::Limit));
  }

  bool isBottom() const {
    MOZ_ASSERT(tc_.isValid());
    return tc_.typeCode() == TypeCode::Limit;
  }

  ValType valType() const {
    MOZ_ASSERT(tc_.isValid());
    MOZ_ASSERT(!isBottom());
    return ValType(tc_);
  }

  ValType asNonNullable() const {
    MOZ_ASSERT(tc_.isValid());
    MOZ_ASSERT(!isBottom());
    return ValType(tc_.asNonNullable());
  }

  bool isValidForUntypedSelect() const {
    MOZ_ASSERT(tc_.isValid());
    if (isBottom()) {
      return true;
    }
    switch (valType().kind()) {
      case ValType::I32:
      case ValType::F32:
      case ValType::I64:
      case ValType::F64:
#ifdef ENABLE_WASM_SIMD
      case ValType::V128:
#endif
        return true;
      default:
        return false;
    }
  }

  bool operator==(const StackType& that) const {
    MOZ_ASSERT(tc_.isValid() && that.tc_.isValid());
    return tc_ == that.tc_;
  }

  bool operator!=(const StackType& that) const {
    MOZ_ASSERT(tc_.isValid() && that.tc_.isValid());
    return tc_ != that.tc_;
  }
};

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
  V128,
  Br,
  BrIf,
  BrTable,
  Nop,
  Unary,
  Binary,
  Comparison,
  Conversion,
  Load,
  Store,
  TeeStore,
  MemorySize,
  MemoryGrow,
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
  Fence,
  AtomicLoad,
  AtomicStore,
  AtomicBinOp,
  AtomicCompareExchange,
  OldAtomicLoad,
  OldAtomicStore,
  OldAtomicBinOp,
  OldAtomicCompareExchange,
  OldAtomicExchange,
  MemOrTableCopy,
  DataOrElemDrop,
  MemFill,
  MemOrTableInit,
  TableFill,
  TableGet,
  TableGrow,
  TableSet,
  TableSize,
  RefNull,
  RefFunc,
  RefAsNonNull,
  BrOnNull,
  StructNewWithRtt,
  StructNewDefaultWithRtt,
  StructGet,
  StructSet,
  ArrayNewWithRtt,
  ArrayNewDefaultWithRtt,
  ArrayGet,
  ArraySet,
  ArrayLen,
  RttCanon,
  RttSub,
  RefTest,
  RefCast,
  BrOnCast,
#  ifdef ENABLE_WASM_SIMD
  ExtractLane,
  ReplaceLane,
  LoadLane,
  StoreLane,
  VectorShift,
  VectorSelect,
  VectorShuffle,
#  endif
#  ifdef ENABLE_WASM_EXCEPTIONS
  Catch,
  CatchAll,
  Delegate,
  Throw,
  Rethrow,
  Try,
#  endif
};

// Return the OpKind for a given Op. This is used for sanity-checking that
// API users use the correct read function for a given Op.
OpKind Classify(OpBytes op);
#endif

// Common fields for linear memory access.
template <typename Value>
struct LinearMemoryAddress {
  Value base;
  uint32_t offset;
  uint32_t align;

  LinearMemoryAddress() : offset(0), align(0) {}
  LinearMemoryAddress(Value base, uint32_t offset, uint32_t align)
      : base(base), offset(offset), align(align) {}
};

template <typename ControlItem>
class ControlStackEntry {
  // Use a pair to optimize away empty ControlItem.
  mozilla::CompactPair<BlockType, ControlItem> typeAndItem_;

  // The "base" of a control stack entry is valueStack_.length() minus
  // type().params().length(), i.e., the size of the value stack "below"
  // this block.
  uint32_t valueStackBase_;
  bool polymorphicBase_;

  LabelKind kind_;

 public:
  ControlStackEntry(LabelKind kind, BlockType type, uint32_t valueStackBase)
      : typeAndItem_(type, ControlItem()),
        valueStackBase_(valueStackBase),
        polymorphicBase_(false),
        kind_(kind) {
    MOZ_ASSERT(type != BlockType());
  }

  LabelKind kind() const { return kind_; }
  BlockType type() const { return typeAndItem_.first(); }
  ResultType resultType() const { return type().results(); }
  ResultType branchTargetType() const {
    return kind_ == LabelKind::Loop ? type().params() : type().results();
  }
  uint32_t valueStackBase() const { return valueStackBase_; }
  ControlItem& controlItem() { return typeAndItem_.second(); }
  void setPolymorphicBase() { polymorphicBase_ = true; }
  bool polymorphicBase() const { return polymorphicBase_; }

  void switchToElse() {
    MOZ_ASSERT(kind() == LabelKind::Then);
    kind_ = LabelKind::Else;
    polymorphicBase_ = false;
  }

#ifdef ENABLE_WASM_EXCEPTIONS
  void switchToCatch() {
    MOZ_ASSERT(kind() == LabelKind::Try);
    kind_ = LabelKind::Catch;
    polymorphicBase_ = false;
  }

  void switchToCatchAll() {
    MOZ_ASSERT(kind() == LabelKind::Try || kind() == LabelKind::Catch);
    kind_ = LabelKind::CatchAll;
    polymorphicBase_ = false;
  }
#endif
};

template <typename Value>
class TypeAndValueT {
  // Use a Pair to optimize away empty Value.
  mozilla::CompactPair<StackType, Value> tv_;

 public:
  TypeAndValueT() : tv_(StackType::bottom(), Value()) {}
  explicit TypeAndValueT(StackType type) : tv_(type, Value()) {}
  explicit TypeAndValueT(ValType type) : tv_(StackType(type), Value()) {}
  TypeAndValueT(StackType type, Value value) : tv_(type, value) {}
  TypeAndValueT(ValType type, Value value) : tv_(StackType(type), value) {}
  StackType type() const { return tv_.first(); }
  StackType& typeRef() { return tv_.first(); }
  Value value() const { return tv_.second(); }
  void setValue(Value value) { tv_.second() = value; }
};

// An iterator over the bytes of a function body. It performs validation
// and unpacks the data into a usable form.
//
// The MOZ_STACK_CLASS attribute here is because of the use of DebugOnly.
// There's otherwise nothing inherent in this class which would require
// it to be used on the stack.
template <typename Policy>
class MOZ_STACK_CLASS OpIter : private Policy {
 public:
  using Value = typename Policy::Value;
  using ValueVector = typename Policy::ValueVector;
  using TypeAndValue = TypeAndValueT<Value>;
  using TypeAndValueStack = Vector<TypeAndValue, 8, SystemAllocPolicy>;
  using ControlItem = typename Policy::ControlItem;
  using Control = ControlStackEntry<ControlItem>;
  using ControlStack = Vector<Control, 8, SystemAllocPolicy>;

  enum Kind {
    Func,
    InitExpr,
  };

 private:
  Kind kind_;
  Decoder& d_;
  const ModuleEnvironment& env_;
  TypeCache cache_;

  TypeAndValueStack valueStack_;
  TypeAndValueStack elseParamStack_;
  ControlStack controlStack_;

#ifdef DEBUG
  OpBytes op_;
#endif
  size_t offsetOfLastReadOp_;

  [[nodiscard]] bool readFixedU8(uint8_t* out) { return d_.readFixedU8(out); }
  [[nodiscard]] bool readFixedU32(uint32_t* out) {
    return d_.readFixedU32(out);
  }
  [[nodiscard]] bool readVarS32(int32_t* out) { return d_.readVarS32(out); }
  [[nodiscard]] bool readVarU32(uint32_t* out) { return d_.readVarU32(out); }
  [[nodiscard]] bool readVarS64(int64_t* out) { return d_.readVarS64(out); }
  [[nodiscard]] bool readVarU64(uint64_t* out) { return d_.readVarU64(out); }
  [[nodiscard]] bool readFixedF32(float* out) { return d_.readFixedF32(out); }
  [[nodiscard]] bool readFixedF64(double* out) { return d_.readFixedF64(out); }

  [[nodiscard]] bool readMemOrTableIndex(bool isMem, uint32_t* index);
  [[nodiscard]] bool readLinearMemoryAddress(uint32_t byteSize,
                                             LinearMemoryAddress<Value>* addr);
  [[nodiscard]] bool readLinearMemoryAddressAligned(
      uint32_t byteSize, LinearMemoryAddress<Value>* addr);
  [[nodiscard]] bool readBlockType(BlockType* type);
  [[nodiscard]] bool readGcTypeIndex(uint32_t* typeIndex);
  [[nodiscard]] bool readStructTypeIndex(uint32_t* typeIndex);
  [[nodiscard]] bool readArrayTypeIndex(uint32_t* typeIndex);
  [[nodiscard]] bool readFieldIndex(uint32_t* fieldIndex,
                                    const StructType& structType);

  [[nodiscard]] bool popCallArgs(const ValTypeVector& expectedTypes,
                                 ValueVector* values);

  [[nodiscard]] bool failEmptyStack();
  [[nodiscard]] bool popStackType(StackType* type, Value* value);
  [[nodiscard]] bool popWithType(ValType expected, Value* value);
  [[nodiscard]] bool popWithType(ResultType expected, ValueVector* values);
  [[nodiscard]] bool popWithRefType(Value* value, StackType* type);
  [[nodiscard]] bool popWithRttType(Value* rtt, uint32_t* rttTypeIndex,
                                    uint32_t* rttDepth);
  [[nodiscard]] bool popThenPushType(ResultType expected, ValueVector* values);
  [[nodiscard]] bool topWithType(ResultType expected, ValueVector* values);

  [[nodiscard]] bool pushControl(LabelKind kind, BlockType type);
  [[nodiscard]] bool checkStackAtEndOfBlock(ResultType* type,
                                            ValueVector* values);
  [[nodiscard]] bool getControl(uint32_t relativeDepth, Control** controlEntry);
  [[nodiscard]] bool checkBranchValue(uint32_t relativeDepth, ResultType* type,
                                      ValueVector* values);
  [[nodiscard]] bool checkCastedBranchValue(uint32_t relativeDepth,
                                            ValType castedFromType,
                                            ValType castedToType,
                                            ResultType* branchTargetType,
                                            ValueVector* values);
  [[nodiscard]] bool checkBrTableEntry(uint32_t* relativeDepth,
                                       ResultType prevBranchType,
                                       ResultType* branchType,
                                       ValueVector* branchValues);

  [[nodiscard]] bool push(ValType t) { return valueStack_.emplaceBack(t); }
  [[nodiscard]] bool push(TypeAndValue tv) { return valueStack_.append(tv); }
  [[nodiscard]] bool push(ResultType t) {
    for (size_t i = 0; i < t.length(); i++) {
      if (!push(t[i])) {
        return false;
      }
    }
    return true;
  }
  void infalliblePush(StackType t) { valueStack_.infallibleEmplaceBack(t); }
  void infalliblePush(ValType t) {
    valueStack_.infallibleEmplaceBack(StackType(t));
  }
  void infalliblePush(TypeAndValue tv) { valueStack_.infallibleAppend(tv); }

  void afterUnconditionalBranch() {
    valueStack_.shrinkTo(controlStack_.back().valueStackBase());
    controlStack_.back().setPolymorphicBase();
  }

  inline bool checkIsSubtypeOf(ValType actual, ValType expected);
  inline bool checkIsSubtypeOf(ResultType params, ResultType results);

 public:
#ifdef DEBUG
  explicit OpIter(const ModuleEnvironment& env, Decoder& decoder,
                  Kind kind = OpIter::Func)
      : kind_(kind),
        d_(decoder),
        env_(env),
        op_(OpBytes(Op::Limit)),
        offsetOfLastReadOp_(0) {}
#else
  explicit OpIter(const ModuleEnvironment& env, Decoder& decoder,
                  Kind kind = OpIter::Func)
      : kind_(kind), d_(decoder), env_(env), offsetOfLastReadOp_(0) {}
#endif

  // Return the decoding byte offset.
  uint32_t currentOffset() const { return d_.currentOffset(); }

  // Return the offset within the entire module of the last-read op.
  size_t lastOpcodeOffset() const {
    return offsetOfLastReadOp_ ? offsetOfLastReadOp_ : d_.currentOffset();
  }

  // Return a BytecodeOffset describing where the current op should be reported
  // to trap/call.
  BytecodeOffset bytecodeOffset() const {
    return BytecodeOffset(lastOpcodeOffset());
  }

  // Test whether the iterator has reached the end of the buffer.
  bool done() const { return d_.done(); }

  // Return a pointer to the end of the buffer being decoded by this iterator.
  const uint8_t* end() const { return d_.end(); }

  // Report a general failure.
  [[nodiscard]] bool fail(const char* msg) MOZ_COLD;

  // Report a general failure with a context
  [[nodiscard]] bool fail_ctx(const char* fmt, const char* context) MOZ_COLD;

  // Report an unrecognized opcode.
  [[nodiscard]] bool unrecognizedOpcode(const OpBytes* expr) MOZ_COLD;

  // Return whether the innermost block has a polymorphic base of its stack.
  // Ideally this accessor would be removed; consider using something else.
  bool currentBlockHasPolymorphicBase() const {
    return !controlStack_.empty() && controlStack_.back().polymorphicBase();
  }

  // ------------------------------------------------------------------------
  // Decoding and validation interface.

  // Initialization and termination

  [[nodiscard]] bool startFunction(uint32_t funcIndex);
  [[nodiscard]] bool endFunction(const uint8_t* bodyEnd);

  [[nodiscard]] bool startInitExpr(ValType expected);
  [[nodiscard]] bool endInitExpr();

  // Value and reference types

  [[nodiscard]] bool readValType(ValType* type);
  [[nodiscard]] bool readHeapType(bool nullable, RefType* type);

  // Instructions

  [[nodiscard]] bool readOp(OpBytes* op);
  [[nodiscard]] bool readReturn(ValueVector* values);
  [[nodiscard]] bool readBlock(ResultType* paramType);
  [[nodiscard]] bool readLoop(ResultType* paramType);
  [[nodiscard]] bool readIf(ResultType* paramType, Value* condition);
  [[nodiscard]] bool readElse(ResultType* paramType, ResultType* resultType,
                              ValueVector* thenResults);
  [[nodiscard]] bool readEnd(LabelKind* kind, ResultType* type,
                             ValueVector* results,
                             ValueVector* resultsForEmptyElse);
  void popEnd();
  [[nodiscard]] bool readBr(uint32_t* relativeDepth, ResultType* type,
                            ValueVector* values);
  [[nodiscard]] bool readBrIf(uint32_t* relativeDepth, ResultType* type,
                              ValueVector* values, Value* condition);
  [[nodiscard]] bool readBrTable(Uint32Vector* depths, uint32_t* defaultDepth,
                                 ResultType* defaultBranchType,
                                 ValueVector* branchValues, Value* index);
#ifdef ENABLE_WASM_EXCEPTIONS
  [[nodiscard]] bool readTry(ResultType* type);
  [[nodiscard]] bool readCatch(LabelKind* kind, uint32_t* eventIndex,
                               ResultType* paramType, ResultType* resultType,
                               ValueVector* tryResults);
  [[nodiscard]] bool readCatchAll(LabelKind* kind, ResultType* paramType,
                                  ResultType* resultType,
                                  ValueVector* tryResults);
  [[nodiscard]] bool readDelegate(uint32_t* relativeDepth,
                                  ResultType* resultType,
                                  ValueVector* tryResults);
  void popDelegate();
  [[nodiscard]] bool readThrow(uint32_t* eventIndex, ValueVector* argValues);
  [[nodiscard]] bool readRethrow(uint32_t* relativeDepth);
#endif
  [[nodiscard]] bool readUnreachable();
  [[nodiscard]] bool readDrop();
  [[nodiscard]] bool readUnary(ValType operandType, Value* input);
  [[nodiscard]] bool readConversion(ValType operandType, ValType resultType,
                                    Value* input);
  [[nodiscard]] bool readBinary(ValType operandType, Value* lhs, Value* rhs);
  [[nodiscard]] bool readComparison(ValType operandType, Value* lhs,
                                    Value* rhs);
  [[nodiscard]] bool readLoad(ValType resultType, uint32_t byteSize,
                              LinearMemoryAddress<Value>* addr);
  [[nodiscard]] bool readStore(ValType resultType, uint32_t byteSize,
                               LinearMemoryAddress<Value>* addr, Value* value);
  [[nodiscard]] bool readTeeStore(ValType resultType, uint32_t byteSize,
                                  LinearMemoryAddress<Value>* addr,
                                  Value* value);
  [[nodiscard]] bool readNop();
  [[nodiscard]] bool readMemorySize();
  [[nodiscard]] bool readMemoryGrow(Value* input);
  [[nodiscard]] bool readSelect(bool typed, StackType* type, Value* trueValue,
                                Value* falseValue, Value* condition);
  [[nodiscard]] bool readGetLocal(const ValTypeVector& locals, uint32_t* id);
  [[nodiscard]] bool readSetLocal(const ValTypeVector& locals, uint32_t* id,
                                  Value* value);
  [[nodiscard]] bool readTeeLocal(const ValTypeVector& locals, uint32_t* id,
                                  Value* value);
  [[nodiscard]] bool readGetGlobal(uint32_t* id);
  [[nodiscard]] bool readSetGlobal(uint32_t* id, Value* value);
  [[nodiscard]] bool readTeeGlobal(uint32_t* id, Value* value);
  [[nodiscard]] bool readI32Const(int32_t* i32);
  [[nodiscard]] bool readI64Const(int64_t* i64);
  [[nodiscard]] bool readF32Const(float* f32);
  [[nodiscard]] bool readF64Const(double* f64);
  [[nodiscard]] bool readRefFunc(uint32_t* funcIndex);
  [[nodiscard]] bool readRefNull(RefType* type);
  [[nodiscard]] bool readRefIsNull(Value* input);
  [[nodiscard]] bool readRefAsNonNull(Value* input);
  [[nodiscard]] bool readBrOnNull(uint32_t* relativeDepth, ResultType* type,
                                  ValueVector* values, Value* condition);
  [[nodiscard]] bool readCall(uint32_t* funcTypeIndex, ValueVector* argValues);
  [[nodiscard]] bool readCallIndirect(uint32_t* funcTypeIndex,
                                      uint32_t* tableIndex, Value* callee,
                                      ValueVector* argValues);
  [[nodiscard]] bool readOldCallDirect(uint32_t numFuncImports,
                                       uint32_t* funcTypeIndex,
                                       ValueVector* argValues);
  [[nodiscard]] bool readOldCallIndirect(uint32_t* funcTypeIndex, Value* callee,
                                         ValueVector* argValues);
  [[nodiscard]] bool readWake(LinearMemoryAddress<Value>* addr, Value* count);
  [[nodiscard]] bool readWait(LinearMemoryAddress<Value>* addr,
                              ValType valueType, uint32_t byteSize,
                              Value* value, Value* timeout);
  [[nodiscard]] bool readFence();
  [[nodiscard]] bool readAtomicLoad(LinearMemoryAddress<Value>* addr,
                                    ValType resultType, uint32_t byteSize);
  [[nodiscard]] bool readAtomicStore(LinearMemoryAddress<Value>* addr,
                                     ValType resultType, uint32_t byteSize,
                                     Value* value);
  [[nodiscard]] bool readAtomicRMW(LinearMemoryAddress<Value>* addr,
                                   ValType resultType, uint32_t byteSize,
                                   Value* value);
  [[nodiscard]] bool readAtomicCmpXchg(LinearMemoryAddress<Value>* addr,
                                       ValType resultType, uint32_t byteSize,
                                       Value* oldValue, Value* newValue);
  [[nodiscard]] bool readMemOrTableCopy(bool isMem,
                                        uint32_t* dstMemOrTableIndex,
                                        Value* dst,
                                        uint32_t* srcMemOrTableIndex,
                                        Value* src, Value* len);
  [[nodiscard]] bool readDataOrElemDrop(bool isData, uint32_t* segIndex);
  [[nodiscard]] bool readMemFill(Value* start, Value* val, Value* len);
  [[nodiscard]] bool readMemOrTableInit(bool isMem, uint32_t* segIndex,
                                        uint32_t* dstTableIndex, Value* dst,
                                        Value* src, Value* len);
  [[nodiscard]] bool readTableFill(uint32_t* tableIndex, Value* start,
                                   Value* val, Value* len);
  [[nodiscard]] bool readTableGet(uint32_t* tableIndex, Value* index);
  [[nodiscard]] bool readTableGrow(uint32_t* tableIndex, Value* initValue,
                                   Value* delta);
  [[nodiscard]] bool readTableSet(uint32_t* tableIndex, Value* index,
                                  Value* value);

  [[nodiscard]] bool readTableSize(uint32_t* tableIndex);
  [[nodiscard]] bool readStructNewWithRtt(uint32_t* typeIndex, Value* rtt,
                                          ValueVector* argValues);
  [[nodiscard]] bool readStructNewDefaultWithRtt(uint32_t* typeIndex,
                                                 Value* rtt);
  [[nodiscard]] bool readStructGet(uint32_t* typeIndex, uint32_t* fieldIndex,
                                   FieldExtension extension, Value* ptr);
  [[nodiscard]] bool readStructSet(uint32_t* typeIndex, uint32_t* fieldIndex,
                                   Value* ptr, Value* val);
  [[nodiscard]] bool readArrayNewWithRtt(uint32_t* typeIndex, Value* rtt,
                                         Value* length, Value* argValue);
  [[nodiscard]] bool readArrayNewDefaultWithRtt(uint32_t* typeIndex, Value* rtt,
                                                Value* length);
  [[nodiscard]] bool readArrayGet(uint32_t* typeIndex, FieldExtension extension,
                                  Value* index, Value* ptr);
  [[nodiscard]] bool readArraySet(uint32_t* typeIndex, Value* val, Value* index,
                                  Value* ptr);
  [[nodiscard]] bool readArrayLen(uint32_t* typeIndex, Value* ptr);
  [[nodiscard]] bool readRttCanon(ValType* rttType);
  [[nodiscard]] bool readRttSub(Value* parentRtt);
  [[nodiscard]] bool readRefTest(Value* rtt, uint32_t* rttTypeIndex,
                                 uint32_t* rttDepth, Value* ref);
  [[nodiscard]] bool readRefCast(Value* rtt, uint32_t* rttTypeIndex,
                                 uint32_t* rttDepth, Value* ref);
  [[nodiscard]] bool readBrOnCast(uint32_t* relativeDepth, Value* rtt,
                                  uint32_t* rttTypeIndex, uint32_t* rttDepth,
                                  ResultType* branchTargetType,
                                  ValueVector* values);

#ifdef ENABLE_WASM_SIMD
  [[nodiscard]] bool readLaneIndex(uint32_t inputLanes, uint32_t* laneIndex);
  [[nodiscard]] bool readExtractLane(ValType resultType, uint32_t inputLanes,
                                     uint32_t* laneIndex, Value* input);
  [[nodiscard]] bool readReplaceLane(ValType operandType, uint32_t inputLanes,
                                     uint32_t* laneIndex, Value* baseValue,
                                     Value* operand);
  [[nodiscard]] bool readVectorShift(Value* baseValue, Value* shift);
  [[nodiscard]] bool readVectorSelect(Value* v1, Value* v2, Value* controlMask);
  [[nodiscard]] bool readVectorShuffle(Value* v1, Value* v2, V128* selectMask);
  [[nodiscard]] bool readV128Const(V128* value);
  [[nodiscard]] bool readLoadSplat(uint32_t byteSize,
                                   LinearMemoryAddress<Value>* addr);
  [[nodiscard]] bool readLoadExtend(LinearMemoryAddress<Value>* addr);
  [[nodiscard]] bool readLoadLane(uint32_t byteSize,
                                  LinearMemoryAddress<Value>* addr,
                                  uint32_t* laneIndex, Value* input);
  [[nodiscard]] bool readStoreLane(uint32_t byteSize,
                                   LinearMemoryAddress<Value>* addr,
                                   uint32_t* laneIndex, Value* input);
#endif

  // At a location where readOp is allowed, peek at the next opcode
  // without consuming it or updating any internal state.
  // Never fails: returns uint16_t(Op::Limit) in op->b0 if it can't read.
  void peekOp(OpBytes* op);

  // ------------------------------------------------------------------------
  // Stack management.

  // Set the top N result values.
  void setResults(size_t count, const ValueVector& values) {
    MOZ_ASSERT(valueStack_.length() >= count);
    size_t base = valueStack_.length() - count;
    for (size_t i = 0; i < count; i++) {
      valueStack_[base + i].setValue(values[i]);
    }
  }

  bool getResults(size_t count, ValueVector* values) {
    MOZ_ASSERT(valueStack_.length() >= count);
    if (!values->resize(count)) {
      return false;
    }
    size_t base = valueStack_.length() - count;
    for (size_t i = 0; i < count; i++) {
      (*values)[i] = valueStack_[base + i].value();
    }
    return true;
  }

  // Set the result value of the current top-of-value-stack expression.
  void setResult(Value value) { valueStack_.back().setValue(value); }

  // Return the result value of the current top-of-value-stack expression.
  Value getResult() { return valueStack_.back().value(); }

  // Return a reference to the top of the control stack.
  ControlItem& controlItem() { return controlStack_.back().controlItem(); }

  // Return a reference to an element in the control stack.
  ControlItem& controlItem(uint32_t relativeDepth) {
    return controlStack_[controlStack_.length() - 1 - relativeDepth]
        .controlItem();
  }

  // Return the LabelKind of an element in the control stack.
  LabelKind controlKind(uint32_t relativeDepth) {
    return controlStack_[controlStack_.length() - 1 - relativeDepth].kind();
  }

  // Return a reference to the outermost element on the control stack.
  ControlItem& controlOutermost() { return controlStack_[0].controlItem(); }

  // Test whether the control-stack is empty, meaning we've consumed the final
  // end of the function body.
  bool controlStackEmpty() const { return controlStack_.empty(); }
};

template <typename Policy>
inline bool OpIter<Policy>::checkIsSubtypeOf(ValType actual, ValType expected) {
  return CheckIsSubtypeOf(d_, env_, lastOpcodeOffset(), actual, expected,
                          &cache_);
}

template <typename Policy>
inline bool OpIter<Policy>::checkIsSubtypeOf(ResultType params,
                                             ResultType results) {
  if (params.length() != results.length()) {
    UniqueChars error(
        JS_smprintf("type mismatch: expected %zu values, got %zu values",
                    results.length(), params.length()));
    if (!error) {
      return false;
    }
    return fail(error.get());
  }
  for (uint32_t i = 0; i < params.length(); i++) {
    ValType param = params[i];
    ValType result = results[i];
    if (!checkIsSubtypeOf(param, result)) {
      return false;
    }
  }
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::unrecognizedOpcode(const OpBytes* expr) {
  UniqueChars error(JS_smprintf("unrecognized opcode: %x %x", expr->b0,
                                IsPrefixByte(expr->b0) ? expr->b1 : 0));
  if (!error) {
    return false;
  }

  return fail(error.get());
}

template <typename Policy>
inline bool OpIter<Policy>::fail(const char* msg) {
  return d_.fail(lastOpcodeOffset(), msg);
}

template <typename Policy>
inline bool OpIter<Policy>::fail_ctx(const char* fmt, const char* context) {
  UniqueChars error(JS_smprintf(fmt, context));
  if (!error) {
    return false;
  }
  return fail(error.get());
}

template <typename Policy>
inline bool OpIter<Policy>::failEmptyStack() {
  return valueStack_.empty() ? fail("popping value from empty stack")
                             : fail("popping value from outside block");
}

// This function pops exactly one value from the stack, yielding Bottom types in
// various cases and therefore making it the caller's responsibility to do the
// right thing for StackType::Bottom. Prefer (pop|top)WithType.  This is an
// optimization for the super-common case where the caller is statically
// expecting the resulttype `[valtype]`.
template <typename Policy>
inline bool OpIter<Policy>::popStackType(StackType* type, Value* value) {
  Control& block = controlStack_.back();

  MOZ_ASSERT(valueStack_.length() >= block.valueStackBase());
  if (MOZ_UNLIKELY(valueStack_.length() == block.valueStackBase())) {
    // If the base of this block's stack is polymorphic, then we can pop a
    // dummy value of the bottom type; it won't be used since we're in
    // unreachable code.
    if (block.polymorphicBase()) {
      *type = StackType::bottom();
      *value = Value();

      // Maintain the invariant that, after a pop, there is always memory
      // reserved to push a value infallibly.
      return valueStack_.reserve(valueStack_.length() + 1);
    }

    return failEmptyStack();
  }

  TypeAndValue& tv = valueStack_.back();
  *type = tv.type();
  *value = tv.value();
  valueStack_.popBack();
  return true;
}

// This function pops exactly one value from the stack, checking that it has the
// expected type which can either be a specific value type or a type variable.
template <typename Policy>
inline bool OpIter<Policy>::popWithType(ValType expectedType, Value* value) {
  StackType stackType;
  if (!popStackType(&stackType, value)) {
    return false;
  }

  return stackType.isBottom() ||
         checkIsSubtypeOf(stackType.valType(), expectedType);
}

// Pops each of the given expected types (in reverse, because it's a stack).
template <typename Policy>
inline bool OpIter<Policy>::popWithType(ResultType expected,
                                        ValueVector* values) {
  size_t expectedLength = expected.length();
  if (!values->resize(expectedLength)) {
    return false;
  }
  for (size_t i = 0; i < expectedLength; i++) {
    size_t reverseIndex = expectedLength - i - 1;
    ValType expectedType = expected[reverseIndex];
    Value* value = &(*values)[reverseIndex];
    if (!popWithType(expectedType, value)) {
      return false;
    }
  }
  return true;
}

// This function pops exactly one value from the stack, checking that it is a
// reference type.
template <typename Policy>
inline bool OpIter<Policy>::popWithRefType(Value* value, StackType* type) {
  if (!popStackType(type, value)) {
    return false;
  }

  if (type->isBottom() || type->valType().isReference()) {
    return true;
  }

  UniqueChars actualText = ToString(type->valType());
  if (!actualText) {
    return false;
  }

  UniqueChars error(JS_smprintf(
      "type mismatch: expression has type %s but expected a reference type",
      actualText.get()));
  if (!error) {
    return false;
  }

  return fail(error.get());
}

// This function pops exactly one value from the stack, checking that it is an
// rtt type with any type index or depth value.
template <typename Policy>
inline bool OpIter<Policy>::popWithRttType(Value* rtt, uint32_t* rttTypeIndex,
                                           uint32_t* rttDepth) {
  StackType type;
  if (!popStackType(&type, rtt)) {
    return false;
  }

  if (type.isBottom()) {
    return fail("gc instruction temporarily not allowed in dead code");
  }

  if (type.valType().isRtt()) {
    *rttTypeIndex = type.valType().typeIndex();
    *rttDepth = type.valType().rttDepth();
    return true;
  }

  UniqueChars actualText = ToString(type.valType());
  if (!actualText) {
    return false;
  }

  UniqueChars error(
      JS_smprintf("type mismatch: expression has type %s but expected (rtt _)",
                  actualText.get()));
  if (!error) {
    return false;
  }

  return fail(error.get());
}

// This function is an optimization of the sequence:
//   popWithType(ResultType, tmp)
//   push(ResultType, tmp)
template <typename Policy>
inline bool OpIter<Policy>::popThenPushType(ResultType expected,
                                            ValueVector* values) {
  if (expected.empty()) {
    return true;
  }

  Control& block = controlStack_.back();

  size_t expectedLength = expected.length();
  if (values && !values->resize(expectedLength)) {
    return false;
  }

  for (size_t i = 0; i != expectedLength; i++) {
    // We're iterating as-if we were popping each expected/actual type one by
    // one, which means iterating the array of expected results backwards.
    // The "current" value stack length refers to what the value stack length
    // would have been if we were popping it.
    size_t reverseIndex = expectedLength - i - 1;
    ValType expectedType = expected[reverseIndex];
    auto collectValue = [&](const Value& v) {
      if (values) {
        (*values)[reverseIndex] = v;
      }
    };

    size_t currentValueStackLength = valueStack_.length() - i;

    MOZ_ASSERT(currentValueStackLength >= block.valueStackBase());
    if (currentValueStackLength == block.valueStackBase()) {
      if (!block.polymorphicBase()) {
        return failEmptyStack();
      }

      // If the base of this block's stack is polymorphic, then we can just
      // pull out as many fake values as we need to validate; they won't be used
      // since we're in unreachable code. We must however push these types on
      // the operand stack since they are now fixed by this constraint.
      if (!valueStack_.insert(valueStack_.begin() + currentValueStackLength,
                              TypeAndValue(expectedType))) {
        return false;
      }

      collectValue(Value());
    } else {
      TypeAndValue& observed = valueStack_[currentValueStackLength - 1];

      if (observed.type().isBottom()) {
        observed.typeRef() = StackType(expectedType);
        collectValue(Value());
      } else {
        if (!checkIsSubtypeOf(observed.type().valType(), expectedType)) {
          return false;
        }

        collectValue(observed.value());
      }
    }
  }
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::topWithType(ResultType expected,
                                        ValueVector* values) {
  if (expected.empty()) {
    return true;
  }

  Control& block = controlStack_.back();

  size_t expectedLength = expected.length();
  if (values && !values->resize(expectedLength)) {
    return false;
  }

  for (size_t i = 0; i != expectedLength; i++) {
    // We're iterating as-if we were popping each expected/actual type one by
    // one, which means iterating the array of expected results backwards.
    // The "current" value stack length refers to what the value stack length
    // would have been if we were popping it.
    size_t reverseIndex = expectedLength - i - 1;
    ValType expectedType = expected[reverseIndex];
    auto collectValue = [&](const Value& v) {
      if (values) {
        (*values)[reverseIndex] = v;
      }
    };

    size_t currentValueStackLength = valueStack_.length() - i;

    MOZ_ASSERT(currentValueStackLength >= block.valueStackBase());
    if (currentValueStackLength == block.valueStackBase()) {
      if (!block.polymorphicBase()) {
        return failEmptyStack();
      }

      // If the base of this block's stack is polymorphic, then we can just
      // pull out as many fake values as we need to validate; they won't be used
      // since we're in unreachable code.
      if (!valueStack_.insert(valueStack_.begin() + currentValueStackLength,
                              TypeAndValue())) {
        return false;
      }

      collectValue(Value());
    } else {
      TypeAndValue& observed = valueStack_[currentValueStackLength - 1];

      if (observed.type().isBottom()) {
        collectValue(Value());
      } else {
        if (!checkIsSubtypeOf(observed.type().valType(), expectedType)) {
          return false;
        }

        collectValue(observed.value());
      }
    }
  }
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::pushControl(LabelKind kind, BlockType type) {
  ResultType paramType = type.params();

  ValueVector values;
  if (!popThenPushType(paramType, &values)) {
    return false;
  }
  MOZ_ASSERT(valueStack_.length() >= paramType.length());
  uint32_t valueStackBase = valueStack_.length() - paramType.length();
  return controlStack_.emplaceBack(kind, type, valueStackBase);
}

template <typename Policy>
inline bool OpIter<Policy>::checkStackAtEndOfBlock(ResultType* expectedType,
                                                   ValueVector* values) {
  Control& block = controlStack_.back();
  *expectedType = block.type().results();

  MOZ_ASSERT(valueStack_.length() >= block.valueStackBase());
  if (expectedType->length() < valueStack_.length() - block.valueStackBase()) {
    return fail("unused values not explicitly dropped by end of block");
  }

  return popThenPushType(*expectedType, values);
}

template <typename Policy>
inline bool OpIter<Policy>::getControl(uint32_t relativeDepth,
                                       Control** controlEntry) {
  if (relativeDepth >= controlStack_.length()) {
    return fail("branch depth exceeds current nesting level");
  }

  *controlEntry = &controlStack_[controlStack_.length() - 1 - relativeDepth];
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readBlockType(BlockType* type) {
  uint8_t nextByte;
  if (!d_.peekByte(&nextByte)) {
    return fail("unable to read block type");
  }

  if (nextByte == uint8_t(TypeCode::BlockVoid)) {
    d_.uncheckedReadFixedU8();
    *type = BlockType::VoidToVoid();
    return true;
  }

  if ((nextByte & SLEB128SignMask) == SLEB128SignBit) {
    ValType v;
    if (!readValType(&v)) {
      return false;
    }
    *type = BlockType::VoidToSingle(v);
    return true;
  }

  int32_t x;
  if (!d_.readVarS32(&x) || x < 0 || uint32_t(x) >= env_.types.length()) {
    return fail("invalid block type type index");
  }

  if (!env_.types.isFuncType(x)) {
    return fail("block type type index must be func type");
  }

  *type = BlockType::Func(env_.types.funcType(x));

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readOp(OpBytes* op) {
  MOZ_ASSERT(!controlStack_.empty());

  offsetOfLastReadOp_ = d_.currentOffset();

  if (MOZ_UNLIKELY(!d_.readOp(op))) {
    return fail("unable to read opcode");
  }

#ifdef DEBUG
  op_ = *op;
#endif

  return true;
}

template <typename Policy>
inline void OpIter<Policy>::peekOp(OpBytes* op) {
  const uint8_t* pos = d_.currentPosition();

  if (MOZ_UNLIKELY(!d_.readOp(op))) {
    op->b0 = uint16_t(Op::Limit);
  }

  d_.rollbackPosition(pos);
}

template <typename Policy>
inline bool OpIter<Policy>::startFunction(uint32_t funcIndex) {
  MOZ_ASSERT(kind_ == OpIter::Func);
  MOZ_ASSERT(elseParamStack_.empty());
  MOZ_ASSERT(valueStack_.empty());
  MOZ_ASSERT(controlStack_.empty());
  MOZ_ASSERT(op_.b0 == uint16_t(Op::Limit));
  BlockType type = BlockType::FuncResults(*env_.funcs[funcIndex].type);
  return pushControl(LabelKind::Body, type);
}

template <typename Policy>
inline bool OpIter<Policy>::endFunction(const uint8_t* bodyEnd) {
  if (d_.currentPosition() != bodyEnd) {
    return fail("function body length mismatch");
  }

  if (!controlStack_.empty()) {
    return fail("unbalanced function body control flow");
  }
  MOZ_ASSERT(elseParamStack_.empty());

#ifdef DEBUG
  op_ = OpBytes(Op::Limit);
#endif
  valueStack_.clear();
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::startInitExpr(ValType expected) {
  MOZ_ASSERT(kind_ == OpIter::InitExpr);
  MOZ_ASSERT(elseParamStack_.empty());
  MOZ_ASSERT(valueStack_.empty());
  MOZ_ASSERT(controlStack_.empty());
  MOZ_ASSERT(op_.b0 == uint16_t(Op::Limit));
  BlockType type = BlockType::VoidToSingle(expected);
  return pushControl(LabelKind::Body, type);
}

template <typename Policy>
inline bool OpIter<Policy>::endInitExpr() {
  MOZ_ASSERT(controlStack_.empty());
  MOZ_ASSERT(elseParamStack_.empty());

#ifdef DEBUG
  op_ = OpBytes(Op::Limit);
#endif
  valueStack_.clear();
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readValType(ValType* type) {
  return d_.readValType(env_.types, env_.features, type);
}

template <typename Policy>
inline bool OpIter<Policy>::readHeapType(bool nullable, RefType* type) {
  return d_.readHeapType(env_.types, env_.features, nullable, type);
}

template <typename Policy>
inline bool OpIter<Policy>::readReturn(ValueVector* values) {
  MOZ_ASSERT(Classify(op_) == OpKind::Return);

  Control& body = controlStack_[0];
  MOZ_ASSERT(body.kind() == LabelKind::Body);

  if (!popWithType(body.resultType(), values)) {
    return false;
  }

  afterUnconditionalBranch();
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readBlock(ResultType* paramType) {
  MOZ_ASSERT(Classify(op_) == OpKind::Block);

  BlockType type;
  if (!readBlockType(&type)) {
    return false;
  }

  *paramType = type.params();
  return pushControl(LabelKind::Block, type);
}

template <typename Policy>
inline bool OpIter<Policy>::readLoop(ResultType* paramType) {
  MOZ_ASSERT(Classify(op_) == OpKind::Loop);

  BlockType type;
  if (!readBlockType(&type)) {
    return false;
  }

  *paramType = type.params();
  return pushControl(LabelKind::Loop, type);
}

template <typename Policy>
inline bool OpIter<Policy>::readIf(ResultType* paramType, Value* condition) {
  MOZ_ASSERT(Classify(op_) == OpKind::If);

  BlockType type;
  if (!readBlockType(&type)) {
    return false;
  }

  if (!popWithType(ValType::I32, condition)) {
    return false;
  }

  if (!pushControl(LabelKind::Then, type)) {
    return false;
  }

  *paramType = type.params();
  size_t paramsLength = type.params().length();
  return elseParamStack_.append(valueStack_.end() - paramsLength, paramsLength);
}

template <typename Policy>
inline bool OpIter<Policy>::readElse(ResultType* paramType,
                                     ResultType* resultType,
                                     ValueVector* thenResults) {
  MOZ_ASSERT(Classify(op_) == OpKind::Else);

  Control& block = controlStack_.back();
  if (block.kind() != LabelKind::Then) {
    return fail("else can only be used within an if");
  }

  *paramType = block.type().params();
  if (!checkStackAtEndOfBlock(resultType, thenResults)) {
    return false;
  }

  valueStack_.shrinkTo(block.valueStackBase());

  size_t nparams = block.type().params().length();
  MOZ_ASSERT(elseParamStack_.length() >= nparams);
  valueStack_.infallibleAppend(elseParamStack_.end() - nparams, nparams);
  elseParamStack_.shrinkBy(nparams);

  block.switchToElse();
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readEnd(LabelKind* kind, ResultType* type,
                                    ValueVector* results,
                                    ValueVector* resultsForEmptyElse) {
  MOZ_ASSERT(Classify(op_) == OpKind::End);

  Control& block = controlStack_.back();

  if (!checkStackAtEndOfBlock(type, results)) {
    return false;
  }

  if (block.kind() == LabelKind::Then) {
    ResultType params = block.type().params();
    // If an `if` block ends with `end` instead of `else`, then the `else` block
    // implicitly passes the `if` parameters as the `else` results.  In that
    // case, assert that the `if`'s param type matches the result type.
    if (params != block.type().results()) {
      return fail("if without else with a result value");
    }

    size_t nparams = params.length();
    MOZ_ASSERT(elseParamStack_.length() >= nparams);
    if (!resultsForEmptyElse->resize(nparams)) {
      return false;
    }
    const TypeAndValue* elseParams = elseParamStack_.end() - nparams;
    for (size_t i = 0; i < nparams; i++) {
      (*resultsForEmptyElse)[i] = elseParams[i].value();
    }
    elseParamStack_.shrinkBy(nparams);
  }

  *kind = block.kind();
  return true;
}

template <typename Policy>
inline void OpIter<Policy>::popEnd() {
  MOZ_ASSERT(Classify(op_) == OpKind::End);

  controlStack_.popBack();
}

template <typename Policy>
inline bool OpIter<Policy>::checkBranchValue(uint32_t relativeDepth,
                                             ResultType* type,
                                             ValueVector* values) {
  Control* block = nullptr;
  if (!getControl(relativeDepth, &block)) {
    return false;
  }

  *type = block->branchTargetType();
  return topWithType(*type, values);
}

// Check the typing of a branch instruction which casts an input type to
// an output type, branching on success to a target which takes the output
// type along with extra values from the stack. On casting failure, the
// original input type and extra values are left on the stack.
template <typename Policy>
inline bool OpIter<Policy>::checkCastedBranchValue(uint32_t relativeDepth,
                                                   ValType castedFromType,
                                                   ValType castedToType,
                                                   ResultType* branchTargetType,
                                                   ValueVector* values) {
  // Get the branch target type, which will determine the type of extra values
  // that are passed along with the casted type.
  Control* block = nullptr;
  if (!getControl(relativeDepth, &block)) {
    return false;
  }
  *branchTargetType = block->branchTargetType();

  // Check we at least have one type in the branch target type, which will take
  // the casted type.
  if (branchTargetType->length() < 1) {
    UniqueChars expectedText = ToString(castedToType);
    if (!expectedText) {
      return false;
    }

    UniqueChars error(JS_smprintf("type mismatch: expected [_, %s], got []",
                                  expectedText.get()));
    if (!error) {
      return false;
    }
    return fail(error.get());
  }

  // The top of the stack is the type that is being cast. This is the last type
  // in the branch target type. This is guaranteed to exist by the above check.
  const size_t castTypeIndex = branchTargetType->length() - 1;

  // Check that the branch target type can accept the castedToType. The branch
  // target may specify a super type of the castedToType, and this is okay.
  if (!checkIsSubtypeOf(castedToType, (*branchTargetType)[castTypeIndex])) {
    return false;
  }

  // Create a copy of the branch target type, with the castTypeIndex replaced
  // with the castedFromType. Use this to check that the stack has the proper
  // types to branch to the target type.
  //
  // TODO: We could avoid a potential allocation here by handwriting a custom
  //       topWithType that handles this case.
  ValTypeVector stackTargetType;
  if (!branchTargetType->cloneToVector(&stackTargetType)) {
    return false;
  }
  stackTargetType[castTypeIndex] = castedFromType;

  return topWithType(ResultType::Vector(stackTargetType), values);
}

template <typename Policy>
inline bool OpIter<Policy>::readBr(uint32_t* relativeDepth, ResultType* type,
                                   ValueVector* values) {
  MOZ_ASSERT(Classify(op_) == OpKind::Br);

  if (!readVarU32(relativeDepth)) {
    return fail("unable to read br depth");
  }

  if (!checkBranchValue(*relativeDepth, type, values)) {
    return false;
  }

  afterUnconditionalBranch();
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readBrIf(uint32_t* relativeDepth, ResultType* type,
                                     ValueVector* values, Value* condition) {
  MOZ_ASSERT(Classify(op_) == OpKind::BrIf);

  if (!readVarU32(relativeDepth)) {
    return fail("unable to read br_if depth");
  }

  if (!popWithType(ValType::I32, condition)) {
    return false;
  }

  return checkBranchValue(*relativeDepth, type, values);
}

#define UNKNOWN_ARITY UINT32_MAX

template <typename Policy>
inline bool OpIter<Policy>::checkBrTableEntry(uint32_t* relativeDepth,
                                              ResultType prevBranchType,
                                              ResultType* type,
                                              ValueVector* branchValues) {
  if (!readVarU32(relativeDepth)) {
    return fail("unable to read br_table depth");
  }

  Control* block = nullptr;
  if (!getControl(*relativeDepth, &block)) {
    return false;
  }

  *type = block->branchTargetType();

  if (prevBranchType != ResultType()) {
    if (prevBranchType.length() != type->length()) {
      return fail("br_table targets must all have the same arity");
    }

    // Avoid re-collecting the same values for subsequent branch targets.
    branchValues = nullptr;
  }

  return topWithType(*type, branchValues);
}

template <typename Policy>
inline bool OpIter<Policy>::readBrTable(Uint32Vector* depths,
                                        uint32_t* defaultDepth,
                                        ResultType* defaultBranchType,
                                        ValueVector* branchValues,
                                        Value* index) {
  MOZ_ASSERT(Classify(op_) == OpKind::BrTable);

  uint32_t tableLength;
  if (!readVarU32(&tableLength)) {
    return fail("unable to read br_table table length");
  }

  if (tableLength > MaxBrTableElems) {
    return fail("br_table too big");
  }

  if (!popWithType(ValType::I32, index)) {
    return false;
  }

  if (!depths->resize(tableLength)) {
    return false;
  }

  ResultType prevBranchType;
  for (uint32_t i = 0; i < tableLength; i++) {
    ResultType branchType;
    if (!checkBrTableEntry(&(*depths)[i], prevBranchType, &branchType,
                           branchValues)) {
      return false;
    }
    prevBranchType = branchType;
  }

  if (!checkBrTableEntry(defaultDepth, prevBranchType, defaultBranchType,
                         branchValues)) {
    return false;
  }

  MOZ_ASSERT(*defaultBranchType != ResultType());

  afterUnconditionalBranch();
  return true;
}

#undef UNKNOWN_ARITY

#ifdef ENABLE_WASM_EXCEPTIONS
template <typename Policy>
inline bool OpIter<Policy>::readTry(ResultType* paramType) {
  MOZ_ASSERT(Classify(op_) == OpKind::Try);

  BlockType type;
  if (!readBlockType(&type)) {
    return false;
  }

  *paramType = type.params();
  return pushControl(LabelKind::Try, type);
}

template <typename Policy>
inline bool OpIter<Policy>::readCatch(LabelKind* kind, uint32_t* eventIndex,
                                      ResultType* paramType,
                                      ResultType* resultType,
                                      ValueVector* tryResults) {
  MOZ_ASSERT(Classify(op_) == OpKind::Catch);

  if (!readVarU32(eventIndex)) {
    return fail("expected event index");
  }
  if (*eventIndex >= env_.events.length()) {
    return fail("event index out of range");
  }

  Control& block = controlStack_.back();
  if (block.kind() == LabelKind::CatchAll) {
    return fail("catch cannot follow a catch_all");
  }
  if (block.kind() != LabelKind::Try && block.kind() != LabelKind::Catch) {
    return fail("catch can only be used within a try-catch");
  }
  *kind = block.kind();
  *paramType = block.type().params();

  if (!checkStackAtEndOfBlock(resultType, tryResults)) {
    return false;
  }

  valueStack_.shrinkTo(block.valueStackBase());
  if (block.kind() == LabelKind::Try) {
    block.switchToCatch();
  }

  return push(env_.events[*eventIndex].resultType());
}

template <typename Policy>
inline bool OpIter<Policy>::readCatchAll(LabelKind* kind, ResultType* paramType,
                                         ResultType* resultType,
                                         ValueVector* tryResults) {
  MOZ_ASSERT(Classify(op_) == OpKind::CatchAll);

  Control& block = controlStack_.back();
  if (block.kind() != LabelKind::Try && block.kind() != LabelKind::Catch) {
    return fail("catch_all can only be used within a try-catch");
  }
  *kind = block.kind();
  *paramType = block.type().params();

  if (!checkStackAtEndOfBlock(resultType, tryResults)) {
    return false;
  }

  valueStack_.shrinkTo(block.valueStackBase());
  block.switchToCatchAll();

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readDelegate(uint32_t* relativeDepth,
                                         ResultType* resultType,
                                         ValueVector* tryResults) {
  MOZ_ASSERT(Classify(op_) == OpKind::Delegate);

  uint32_t originalDepth;
  if (!readVarU32(&originalDepth)) {
    return fail("unable to read delegate depth");
  }

  Control& block = controlStack_.back();
  if (block.kind() != LabelKind::Try) {
    return fail("delegate can only be used within a try");
  }

  // Depths for delegate start counting in the surrounding block.
  *relativeDepth = originalDepth + 1;
  if (*relativeDepth >= controlStack_.length()) {
    return fail("delegate depth exceeds current nesting level");
  }

  LabelKind kind = controlKind(*relativeDepth);
  if (kind != LabelKind::Try && kind != LabelKind::Body) {
    return fail("delegate target was not a try or function body");
  }

  // Because `delegate` acts like `end` and ends the block, we will check
  // the stack here.
  return checkStackAtEndOfBlock(resultType, tryResults);
}

// We need popDelegate because readDelegate cannot pop the control stack
// itself, as its caller may need to use the control item for delegate.
template <typename Policy>
inline void OpIter<Policy>::popDelegate() {
  MOZ_ASSERT(Classify(op_) == OpKind::Delegate);

  controlStack_.popBack();
}

template <typename Policy>
inline bool OpIter<Policy>::readThrow(uint32_t* eventIndex,
                                      ValueVector* argValues) {
  MOZ_ASSERT(Classify(op_) == OpKind::Throw);

  if (!readVarU32(eventIndex)) {
    return fail("expected event index");
  }
  if (*eventIndex >= env_.events.length()) {
    return fail("event index out of range");
  }

  if (!popWithType(env_.events[*eventIndex].resultType(), argValues)) {
    return false;
  }

  afterUnconditionalBranch();
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readRethrow(uint32_t* relativeDepth) {
  MOZ_ASSERT(Classify(op_) == OpKind::Rethrow);

  if (!readVarU32(relativeDepth)) {
    return fail("unable to read rethrow depth");
  }

  if (*relativeDepth >= controlStack_.length()) {
    return fail("rethrow depth exceeds current nesting level");
  }
  LabelKind kind = controlKind(*relativeDepth);
  if (kind != LabelKind::Catch && kind != LabelKind::CatchAll) {
    return fail("rethrow target was not a catch block");
  }

  afterUnconditionalBranch();
  return true;
}
#endif

template <typename Policy>
inline bool OpIter<Policy>::readUnreachable() {
  MOZ_ASSERT(Classify(op_) == OpKind::Unreachable);

  afterUnconditionalBranch();
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readDrop() {
  MOZ_ASSERT(Classify(op_) == OpKind::Drop);
  StackType type;
  Value value;
  return popStackType(&type, &value);
}

template <typename Policy>
inline bool OpIter<Policy>::readUnary(ValType operandType, Value* input) {
  MOZ_ASSERT(Classify(op_) == OpKind::Unary);

  if (!popWithType(operandType, input)) {
    return false;
  }

  infalliblePush(operandType);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readConversion(ValType operandType,
                                           ValType resultType, Value* input) {
  MOZ_ASSERT(Classify(op_) == OpKind::Conversion);

  if (!popWithType(operandType, input)) {
    return false;
  }

  infalliblePush(resultType);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readBinary(ValType operandType, Value* lhs,
                                       Value* rhs) {
  MOZ_ASSERT(Classify(op_) == OpKind::Binary);

  if (!popWithType(operandType, rhs)) {
    return false;
  }

  if (!popWithType(operandType, lhs)) {
    return false;
  }

  infalliblePush(operandType);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readComparison(ValType operandType, Value* lhs,
                                           Value* rhs) {
  MOZ_ASSERT(Classify(op_) == OpKind::Comparison);

  if (!popWithType(operandType, rhs)) {
    return false;
  }

  if (!popWithType(operandType, lhs)) {
    return false;
  }

  infalliblePush(ValType::I32);

  return true;
}

// For memories, the index is currently always a placeholder zero byte.
//
// For tables, the index is a placeholder zero byte until we get multi-table
// with the reftypes proposal.
//
// The zero-ness of the value must be checked by the caller.
template <typename Policy>
inline bool OpIter<Policy>::readMemOrTableIndex(bool isMem, uint32_t* index) {
  bool readByte = isMem;
  if (readByte) {
    uint8_t indexTmp;
    if (!readFixedU8(&indexTmp)) {
      return fail("unable to read memory or table index");
    }
    *index = indexTmp;
  } else {
    if (!readVarU32(index)) {
      return fail("unable to read memory or table index");
    }
  }
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readLinearMemoryAddress(
    uint32_t byteSize, LinearMemoryAddress<Value>* addr) {
  if (!env_.usesMemory()) {
    return fail("can't touch memory without memory");
  }

  uint8_t alignLog2;
  if (!readFixedU8(&alignLog2)) {
    return fail("unable to read load alignment");
  }

  if (!readVarU32(&addr->offset)) {
    return fail("unable to read load offset");
  }

  if (alignLog2 >= 32 || (uint32_t(1) << alignLog2) > byteSize) {
    return fail("greater than natural alignment");
  }

  if (!popWithType(ValType::I32, &addr->base)) {
    return false;
  }

  addr->align = uint32_t(1) << alignLog2;
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readLinearMemoryAddressAligned(
    uint32_t byteSize, LinearMemoryAddress<Value>* addr) {
  if (!readLinearMemoryAddress(byteSize, addr)) {
    return false;
  }

  if (addr->align != byteSize) {
    return fail("not natural alignment");
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readLoad(ValType resultType, uint32_t byteSize,
                                     LinearMemoryAddress<Value>* addr) {
  MOZ_ASSERT(Classify(op_) == OpKind::Load);

  if (!readLinearMemoryAddress(byteSize, addr)) {
    return false;
  }

  infalliblePush(resultType);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readStore(ValType resultType, uint32_t byteSize,
                                      LinearMemoryAddress<Value>* addr,
                                      Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::Store);

  if (!popWithType(resultType, value)) {
    return false;
  }

  if (!readLinearMemoryAddress(byteSize, addr)) {
    return false;
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readTeeStore(ValType resultType, uint32_t byteSize,
                                         LinearMemoryAddress<Value>* addr,
                                         Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::TeeStore);

  if (!popWithType(resultType, value)) {
    return false;
  }

  if (!readLinearMemoryAddress(byteSize, addr)) {
    return false;
  }

  infalliblePush(TypeAndValue(resultType, *value));
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readNop() {
  MOZ_ASSERT(Classify(op_) == OpKind::Nop);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readMemorySize() {
  MOZ_ASSERT(Classify(op_) == OpKind::MemorySize);

  if (!env_.usesMemory()) {
    return fail("can't touch memory without memory");
  }

  uint8_t flags;
  if (!readFixedU8(&flags)) {
    return fail("failed to read memory flags");
  }

  if (flags != uint8_t(MemoryTableFlags::Default)) {
    return fail("unexpected flags");
  }

  return push(ValType::I32);
}

template <typename Policy>
inline bool OpIter<Policy>::readMemoryGrow(Value* input) {
  MOZ_ASSERT(Classify(op_) == OpKind::MemoryGrow);

  if (!env_.usesMemory()) {
    return fail("can't touch memory without memory");
  }

  uint8_t flags;
  if (!readFixedU8(&flags)) {
    return fail("failed to read memory flags");
  }

  if (flags != uint8_t(MemoryTableFlags::Default)) {
    return fail("unexpected flags");
  }

  if (!popWithType(ValType::I32, input)) {
    return false;
  }

  infalliblePush(ValType::I32);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readSelect(bool typed, StackType* type,
                                       Value* trueValue, Value* falseValue,
                                       Value* condition) {
  MOZ_ASSERT(Classify(op_) == OpKind::Select);

  if (typed) {
    uint32_t length;
    if (!readVarU32(&length)) {
      return fail("unable to read select result length");
    }
    if (length != 1) {
      return fail("bad number of results");
    }
    ValType result;
    if (!readValType(&result)) {
      return fail("invalid result type for select");
    }

    if (!popWithType(ValType::I32, condition)) {
      return false;
    }
    if (!popWithType(result, falseValue)) {
      return false;
    }
    if (!popWithType(result, trueValue)) {
      return false;
    }

    *type = StackType(result);
    infalliblePush(*type);
    return true;
  }

  if (!popWithType(ValType::I32, condition)) {
    return false;
  }

  StackType falseType;
  if (!popStackType(&falseType, falseValue)) {
    return false;
  }

  StackType trueType;
  if (!popStackType(&trueType, trueValue)) {
    return false;
  }

  if (!falseType.isValidForUntypedSelect() ||
      !trueType.isValidForUntypedSelect()) {
    return fail("invalid types for untyped select");
  }

  if (falseType.isBottom()) {
    *type = trueType;
  } else if (trueType.isBottom() || falseType == trueType) {
    *type = falseType;
  } else {
    return fail("select operand types must match");
  }

  infalliblePush(*type);
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readGetLocal(const ValTypeVector& locals,
                                         uint32_t* id) {
  MOZ_ASSERT(Classify(op_) == OpKind::GetLocal);

  if (!readVarU32(id)) {
    return fail("unable to read local index");
  }

  if (*id >= locals.length()) {
    return fail("local.get index out of range");
  }

  return push(locals[*id]);
}

template <typename Policy>
inline bool OpIter<Policy>::readSetLocal(const ValTypeVector& locals,
                                         uint32_t* id, Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::SetLocal);

  if (!readVarU32(id)) {
    return fail("unable to read local index");
  }

  if (*id >= locals.length()) {
    return fail("local.set index out of range");
  }

  return popWithType(locals[*id], value);
}

template <typename Policy>
inline bool OpIter<Policy>::readTeeLocal(const ValTypeVector& locals,
                                         uint32_t* id, Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::TeeLocal);

  if (!readVarU32(id)) {
    return fail("unable to read local index");
  }

  if (*id >= locals.length()) {
    return fail("local.set index out of range");
  }

  ValueVector single;
  if (!popThenPushType(ResultType::Single(locals[*id]), &single)) {
    return false;
  }

  *value = single[0];
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readGetGlobal(uint32_t* id) {
  MOZ_ASSERT(Classify(op_) == OpKind::GetGlobal);

  if (!d_.readGetGlobal(id)) {
    return false;
  }

  if (*id >= env_.globals.length()) {
    return fail("global.get index out of range");
  }

  if (kind_ == OpIter::InitExpr &&
      (!env_.globals[*id].isImport() || env_.globals[*id].isMutable())) {
    return fail(
        "global.get in initializer expression must reference a global "
        "immutable import");
  }

  return push(env_.globals[*id].type());
}

template <typename Policy>
inline bool OpIter<Policy>::readSetGlobal(uint32_t* id, Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::SetGlobal);

  if (!readVarU32(id)) {
    return fail("unable to read global index");
  }

  if (*id >= env_.globals.length()) {
    return fail("global.set index out of range");
  }

  if (!env_.globals[*id].isMutable()) {
    return fail("can't write an immutable global");
  }

  return popWithType(env_.globals[*id].type(), value);
}

template <typename Policy>
inline bool OpIter<Policy>::readTeeGlobal(uint32_t* id, Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::TeeGlobal);

  if (!readVarU32(id)) {
    return fail("unable to read global index");
  }

  if (*id >= env_.globals.length()) {
    return fail("global.set index out of range");
  }

  if (!env_.globals[*id].isMutable()) {
    return fail("can't write an immutable global");
  }

  ValueVector single;
  if (!popThenPushType(ResultType::Single(env_.globals[*id].type()), &single)) {
    return false;
  }

  MOZ_ASSERT(single.length() == 1);
  *value = single[0];
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readI32Const(int32_t* i32) {
  MOZ_ASSERT(Classify(op_) == OpKind::I32);

  if (!d_.readI32Const(i32)) {
    return false;
  }

  return push(ValType::I32);
}

template <typename Policy>
inline bool OpIter<Policy>::readI64Const(int64_t* i64) {
  MOZ_ASSERT(Classify(op_) == OpKind::I64);

  if (!d_.readI64Const(i64)) {
    return false;
  }

  return push(ValType::I64);
}

template <typename Policy>
inline bool OpIter<Policy>::readF32Const(float* f32) {
  MOZ_ASSERT(Classify(op_) == OpKind::F32);

  if (!d_.readF32Const(f32)) {
    return false;
  }

  return push(ValType::F32);
}

template <typename Policy>
inline bool OpIter<Policy>::readF64Const(double* f64) {
  MOZ_ASSERT(Classify(op_) == OpKind::F64);

  if (!d_.readF64Const(f64)) {
    return false;
  }

  return push(ValType::F64);
}

template <typename Policy>
inline bool OpIter<Policy>::readRefFunc(uint32_t* funcIndex) {
  MOZ_ASSERT(Classify(op_) == OpKind::RefFunc);

  if (!d_.readRefFunc(funcIndex)) {
    return false;
  }
  if (*funcIndex >= env_.funcs.length()) {
    return fail("function index out of range");
  }
  if (kind_ == OpIter::Func && !env_.funcs[*funcIndex].canRefFunc()) {
    return fail(
        "function index is not declared in a section before the code section");
  }
  return push(RefType::func());
}

template <typename Policy>
inline bool OpIter<Policy>::readRefNull(RefType* type) {
  MOZ_ASSERT(Classify(op_) == OpKind::RefNull);

  if (!d_.readRefNull(env_.types, env_.features, type)) {
    return false;
  }
  return push(*type);
}

template <typename Policy>
inline bool OpIter<Policy>::readRefIsNull(Value* input) {
  MOZ_ASSERT(Classify(op_) == OpKind::Conversion);

  StackType type;
  if (!popWithRefType(input, &type)) {
    return false;
  }
  return push(ValType::I32);
}

template <typename Policy>
inline bool OpIter<Policy>::readRefAsNonNull(Value* input) {
  MOZ_ASSERT(Classify(op_) == OpKind::RefAsNonNull);

  StackType type;
  if (!popWithRefType(input, &type)) {
    return false;
  }

  if (type.isBottom()) {
    infalliblePush(type);
  } else {
    infalliblePush(type.asNonNullable());
  }
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readBrOnNull(uint32_t* relativeDepth,
                                         ResultType* type, ValueVector* values,
                                         Value* condition) {
  MOZ_ASSERT(Classify(op_) == OpKind::BrOnNull);

  if (!readVarU32(relativeDepth)) {
    return fail("unable to read br_on_null depth");
  }

  StackType refType;
  if (!popWithRefType(condition, &refType)) {
    return false;
  }

  if (!checkBranchValue(*relativeDepth, type, values)) {
    return false;
  }

  if (refType.isBottom()) {
    infalliblePush(refType);
  } else {
    infalliblePush(refType.asNonNullable());
  }
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::popCallArgs(const ValTypeVector& expectedTypes,
                                        ValueVector* values) {
  // Iterate through the argument types backward so that pops occur in the
  // right order.

  if (!values->resize(expectedTypes.length())) {
    return false;
  }

  for (int32_t i = expectedTypes.length() - 1; i >= 0; i--) {
    if (!popWithType(expectedTypes[i], &(*values)[i])) {
      return false;
    }
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readCall(uint32_t* funcTypeIndex,
                                     ValueVector* argValues) {
  MOZ_ASSERT(Classify(op_) == OpKind::Call);

  if (!readVarU32(funcTypeIndex)) {
    return fail("unable to read call function index");
  }

  if (*funcTypeIndex >= env_.funcs.length()) {
    return fail("callee index out of range");
  }

  const FuncType& funcType = *env_.funcs[*funcTypeIndex].type;

  if (!popCallArgs(funcType.args(), argValues)) {
    return false;
  }

  return push(ResultType::Vector(funcType.results()));
}

template <typename Policy>
inline bool OpIter<Policy>::readCallIndirect(uint32_t* funcTypeIndex,
                                             uint32_t* tableIndex,
                                             Value* callee,
                                             ValueVector* argValues) {
  MOZ_ASSERT(Classify(op_) == OpKind::CallIndirect);
  MOZ_ASSERT(funcTypeIndex != tableIndex);

  if (!readVarU32(funcTypeIndex)) {
    return fail("unable to read call_indirect signature index");
  }

  if (*funcTypeIndex >= env_.numTypes()) {
    return fail("signature index out of range");
  }

  if (!readVarU32(tableIndex)) {
    return fail("unable to read call_indirect table index");
  }
  if (*tableIndex >= env_.tables.length()) {
    // Special case this for improved user experience.
    if (!env_.tables.length()) {
      return fail("can't call_indirect without a table");
    }
    return fail("table index out of range for call_indirect");
  }
  if (!env_.tables[*tableIndex].elemType.isFunc()) {
    return fail("indirect calls must go through a table of 'funcref'");
  }

  if (!popWithType(ValType::I32, callee)) {
    return false;
  }

  if (!env_.types.isFuncType(*funcTypeIndex)) {
    return fail("expected signature type");
  }

  const FuncType& funcType = env_.types.funcType(*funcTypeIndex);

#ifdef WASM_PRIVATE_REFTYPES
  if (env_.tables[*tableIndex].importedOrExported &&
      funcType.exposesTypeIndex()) {
    return fail("cannot expose indexed reference type");
  }
#endif

  if (!popCallArgs(funcType.args(), argValues)) {
    return false;
  }

  return push(ResultType::Vector(funcType.results()));
}

template <typename Policy>
inline bool OpIter<Policy>::readOldCallDirect(uint32_t numFuncImports,
                                              uint32_t* funcTypeIndex,
                                              ValueVector* argValues) {
  MOZ_ASSERT(Classify(op_) == OpKind::OldCallDirect);

  uint32_t funcDefIndex;
  if (!readVarU32(&funcDefIndex)) {
    return fail("unable to read call function index");
  }

  if (UINT32_MAX - funcDefIndex < numFuncImports) {
    return fail("callee index out of range");
  }

  *funcTypeIndex = numFuncImports + funcDefIndex;

  if (*funcTypeIndex >= env_.funcs.length()) {
    return fail("callee index out of range");
  }

  const FuncType& funcType = *env_.funcs[*funcTypeIndex].type;

  if (!popCallArgs(funcType.args(), argValues)) {
    return false;
  }

  return push(ResultType::Vector(funcType.results()));
}

template <typename Policy>
inline bool OpIter<Policy>::readOldCallIndirect(uint32_t* funcTypeIndex,
                                                Value* callee,
                                                ValueVector* argValues) {
  MOZ_ASSERT(Classify(op_) == OpKind::OldCallIndirect);

  if (!readVarU32(funcTypeIndex)) {
    return fail("unable to read call_indirect signature index");
  }

  if (*funcTypeIndex >= env_.numTypes()) {
    return fail("signature index out of range");
  }

  if (!env_.types.isFuncType(*funcTypeIndex)) {
    return fail("expected signature type");
  }

  const FuncType& funcType = env_.types.funcType(*funcTypeIndex);

  if (!popCallArgs(funcType.args(), argValues)) {
    return false;
  }

  if (!popWithType(ValType::I32, callee)) {
    return false;
  }

  return push(ResultType::Vector(funcType.results()));
}

template <typename Policy>
inline bool OpIter<Policy>::readWake(LinearMemoryAddress<Value>* addr,
                                     Value* count) {
  MOZ_ASSERT(Classify(op_) == OpKind::Wake);

  if (!popWithType(ValType::I32, count)) {
    return false;
  }

  uint32_t byteSize = 4;  // Per spec; smallest WAIT is i32.

  if (!readLinearMemoryAddressAligned(byteSize, addr)) {
    return false;
  }

  infalliblePush(ValType::I32);
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readWait(LinearMemoryAddress<Value>* addr,
                                     ValType valueType, uint32_t byteSize,
                                     Value* value, Value* timeout) {
  MOZ_ASSERT(Classify(op_) == OpKind::Wait);

  if (!popWithType(ValType::I64, timeout)) {
    return false;
  }

  if (!popWithType(valueType, value)) {
    return false;
  }

  if (!readLinearMemoryAddressAligned(byteSize, addr)) {
    return false;
  }

  infalliblePush(ValType::I32);
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readFence() {
  MOZ_ASSERT(Classify(op_) == OpKind::Fence);
  uint8_t flags;
  if (!readFixedU8(&flags)) {
    return fail("expected memory order after fence");
  }
  if (flags != 0) {
    return fail("non-zero memory order not supported yet");
  }
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readAtomicLoad(LinearMemoryAddress<Value>* addr,
                                           ValType resultType,
                                           uint32_t byteSize) {
  MOZ_ASSERT(Classify(op_) == OpKind::AtomicLoad);

  if (!readLinearMemoryAddressAligned(byteSize, addr)) {
    return false;
  }

  infalliblePush(resultType);
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readAtomicStore(LinearMemoryAddress<Value>* addr,
                                            ValType resultType,
                                            uint32_t byteSize, Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::AtomicStore);

  if (!popWithType(resultType, value)) {
    return false;
  }

  if (!readLinearMemoryAddressAligned(byteSize, addr)) {
    return false;
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readAtomicRMW(LinearMemoryAddress<Value>* addr,
                                          ValType resultType, uint32_t byteSize,
                                          Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::AtomicBinOp);

  if (!popWithType(resultType, value)) {
    return false;
  }

  if (!readLinearMemoryAddressAligned(byteSize, addr)) {
    return false;
  }

  infalliblePush(resultType);
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readAtomicCmpXchg(LinearMemoryAddress<Value>* addr,
                                              ValType resultType,
                                              uint32_t byteSize,
                                              Value* oldValue,
                                              Value* newValue) {
  MOZ_ASSERT(Classify(op_) == OpKind::AtomicCompareExchange);

  if (!popWithType(resultType, newValue)) {
    return false;
  }

  if (!popWithType(resultType, oldValue)) {
    return false;
  }

  if (!readLinearMemoryAddressAligned(byteSize, addr)) {
    return false;
  }

  infalliblePush(resultType);
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readMemOrTableCopy(bool isMem,
                                               uint32_t* dstMemOrTableIndex,
                                               Value* dst,
                                               uint32_t* srcMemOrTableIndex,
                                               Value* src, Value* len) {
  MOZ_ASSERT(Classify(op_) == OpKind::MemOrTableCopy);
  MOZ_ASSERT(dstMemOrTableIndex != srcMemOrTableIndex);

  // Spec requires (dest, src) as of 2019-10-04.
  if (!readMemOrTableIndex(isMem, dstMemOrTableIndex)) {
    return false;
  }
  if (!readMemOrTableIndex(isMem, srcMemOrTableIndex)) {
    return false;
  }

  if (isMem) {
    if (!env_.usesMemory()) {
      return fail("can't touch memory without memory");
    }
    if (*srcMemOrTableIndex != 0 || *dstMemOrTableIndex != 0) {
      return fail("memory index out of range for memory.copy");
    }
  } else {
    if (*dstMemOrTableIndex >= env_.tables.length() ||
        *srcMemOrTableIndex >= env_.tables.length()) {
      return fail("table index out of range for table.copy");
    }
    ValType dstElemType = env_.tables[*dstMemOrTableIndex].elemType;
    ValType srcElemType = env_.tables[*srcMemOrTableIndex].elemType;
    if (!checkIsSubtypeOf(srcElemType, dstElemType)) {
      return false;
    }
  }

  if (!popWithType(ValType::I32, len)) {
    return false;
  }

  if (!popWithType(ValType::I32, src)) {
    return false;
  }

  if (!popWithType(ValType::I32, dst)) {
    return false;
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readDataOrElemDrop(bool isData,
                                               uint32_t* segIndex) {
  MOZ_ASSERT(Classify(op_) == OpKind::DataOrElemDrop);

  if (!readVarU32(segIndex)) {
    return fail("unable to read segment index");
  }

  if (isData) {
    if (env_.dataCount.isNothing()) {
      return fail("data.drop requires a DataCount section");
    }
    if (*segIndex >= *env_.dataCount) {
      return fail("data.drop segment index out of range");
    }
  } else {
    if (*segIndex >= env_.elemSegments.length()) {
      return fail("element segment index out of range for elem.drop");
    }
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readMemFill(Value* start, Value* val, Value* len) {
  MOZ_ASSERT(Classify(op_) == OpKind::MemFill);

  if (!env_.usesMemory()) {
    return fail("can't touch memory without memory");
  }

  uint8_t memoryIndex;
  if (!readFixedU8(&memoryIndex)) {
    return fail("failed to read memory index");
  }
  if (!env_.usesMemory()) {
    return fail("can't touch memory without memory");
  }
  if (memoryIndex != 0) {
    return fail("memory index must be zero");
  }

  if (!popWithType(ValType::I32, len)) {
    return false;
  }

  if (!popWithType(ValType::I32, val)) {
    return false;
  }

  if (!popWithType(ValType::I32, start)) {
    return false;
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readMemOrTableInit(bool isMem, uint32_t* segIndex,
                                               uint32_t* dstTableIndex,
                                               Value* dst, Value* src,
                                               Value* len) {
  MOZ_ASSERT(Classify(op_) == OpKind::MemOrTableInit);
  MOZ_ASSERT(segIndex != dstTableIndex);

  if (!popWithType(ValType::I32, len)) {
    return false;
  }

  if (!popWithType(ValType::I32, src)) {
    return false;
  }

  if (!popWithType(ValType::I32, dst)) {
    return false;
  }

  if (!readVarU32(segIndex)) {
    return fail("unable to read segment index");
  }

  uint32_t memOrTableIndex = 0;
  if (!readMemOrTableIndex(isMem, &memOrTableIndex)) {
    return false;
  }
  if (isMem) {
    if (!env_.usesMemory()) {
      return fail("can't touch memory without memory");
    }
    if (memOrTableIndex != 0) {
      return fail("memory index must be zero");
    }
    if (env_.dataCount.isNothing()) {
      return fail("memory.init requires a DataCount section");
    }
    if (*segIndex >= *env_.dataCount) {
      return fail("memory.init segment index out of range");
    }
  } else {
    if (memOrTableIndex >= env_.tables.length()) {
      return fail("table index out of range for table.init");
    }
    *dstTableIndex = memOrTableIndex;

    if (*segIndex >= env_.elemSegments.length()) {
      return fail("table.init segment index out of range");
    }
    if (!checkIsSubtypeOf(env_.elemSegments[*segIndex]->elemType,
                          env_.tables[*dstTableIndex].elemType)) {
      return false;
    }
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readTableFill(uint32_t* tableIndex, Value* start,
                                          Value* val, Value* len) {
  MOZ_ASSERT(Classify(op_) == OpKind::TableFill);

  if (!readVarU32(tableIndex)) {
    return fail("unable to read table index");
  }
  if (*tableIndex >= env_.tables.length()) {
    return fail("table index out of range for table.fill");
  }

  if (!popWithType(ValType::I32, len)) {
    return false;
  }
  if (!popWithType(env_.tables[*tableIndex].elemType, val)) {
    return false;
  }
  if (!popWithType(ValType::I32, start)) {
    return false;
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readTableGet(uint32_t* tableIndex, Value* index) {
  MOZ_ASSERT(Classify(op_) == OpKind::TableGet);

  if (!readVarU32(tableIndex)) {
    return fail("unable to read table index");
  }
  if (*tableIndex >= env_.tables.length()) {
    return fail("table index out of range for table.get");
  }

  if (!popWithType(ValType::I32, index)) {
    return false;
  }

  infalliblePush(env_.tables[*tableIndex].elemType);
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readTableGrow(uint32_t* tableIndex,
                                          Value* initValue, Value* delta) {
  MOZ_ASSERT(Classify(op_) == OpKind::TableGrow);

  if (!readVarU32(tableIndex)) {
    return fail("unable to read table index");
  }
  if (*tableIndex >= env_.tables.length()) {
    return fail("table index out of range for table.grow");
  }

  if (!popWithType(ValType::I32, delta)) {
    return false;
  }
  if (!popWithType(env_.tables[*tableIndex].elemType, initValue)) {
    return false;
  }

  infalliblePush(ValType::I32);
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readTableSet(uint32_t* tableIndex, Value* index,
                                         Value* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::TableSet);

  if (!readVarU32(tableIndex)) {
    return fail("unable to read table index");
  }
  if (*tableIndex >= env_.tables.length()) {
    return fail("table index out of range for table.set");
  }

  if (!popWithType(env_.tables[*tableIndex].elemType, value)) {
    return false;
  }
  if (!popWithType(ValType::I32, index)) {
    return false;
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readTableSize(uint32_t* tableIndex) {
  MOZ_ASSERT(Classify(op_) == OpKind::TableSize);

  *tableIndex = 0;

  if (!readVarU32(tableIndex)) {
    return fail("unable to read table index");
  }
  if (*tableIndex >= env_.tables.length()) {
    return fail("table index out of range for table.size");
  }

  return push(ValType::I32);
}

template <typename Policy>
inline bool OpIter<Policy>::readGcTypeIndex(uint32_t* typeIndex) {
  if (!readVarU32(typeIndex)) {
    return fail("unable to read type index");
  }

  if (*typeIndex >= env_.types.length()) {
    return fail("type index out of range");
  }

  if (!env_.types.isStructType(*typeIndex) &&
      !env_.types.isArrayType(*typeIndex)) {
    return fail("not a gc type");
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readStructTypeIndex(uint32_t* typeIndex) {
  if (!readVarU32(typeIndex)) {
    return fail("unable to read type index");
  }

  if (*typeIndex >= env_.types.length()) {
    return fail("type index out of range");
  }

  if (!env_.types.isStructType(*typeIndex)) {
    return fail("not a struct type");
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readArrayTypeIndex(uint32_t* typeIndex) {
  if (!readVarU32(typeIndex)) {
    return fail("unable to read type index");
  }

  if (*typeIndex >= env_.types.length()) {
    return fail("type index out of range");
  }

  if (!env_.types.isArrayType(*typeIndex)) {
    return fail("not an array type");
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readFieldIndex(uint32_t* fieldIndex,
                                           const StructType& structType) {
  if (!readVarU32(fieldIndex)) {
    return fail("unable to read field index");
  }

  if (structType.fields_.length() <= *fieldIndex) {
    return fail("field index out of range");
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readStructNewWithRtt(uint32_t* typeIndex,
                                                 Value* rtt,
                                                 ValueVector* argValues) {
  MOZ_ASSERT(Classify(op_) == OpKind::StructNewWithRtt);

  if (!readStructTypeIndex(typeIndex)) {
    return false;
  }

  const StructType& str = env_.types.structType(*typeIndex);
  const ValType rttType = ValType::fromRtt(*typeIndex, 0);

  if (!popWithType(rttType, rtt)) {
    return false;
  }

  if (!argValues->resize(str.fields_.length())) {
    return false;
  }

  static_assert(MaxStructFields <= INT32_MAX, "Or we iloop below");

  for (int32_t i = str.fields_.length() - 1; i >= 0; i--) {
    if (!popWithType(str.fields_[i].type.widenToValType(), &(*argValues)[i])) {
      return false;
    }
  }

  return push(RefType::fromTypeIndex(*typeIndex, false));
}

template <typename Policy>
inline bool OpIter<Policy>::readStructNewDefaultWithRtt(uint32_t* typeIndex,
                                                        Value* rtt) {
  MOZ_ASSERT(Classify(op_) == OpKind::StructNewDefaultWithRtt);

  if (!readStructTypeIndex(typeIndex)) {
    return false;
  }

  const StructType& str = env_.types.structType(*typeIndex);
  const ValType rttType = ValType::fromRtt(*typeIndex, 0);

  if (!popWithType(rttType, rtt)) {
    return false;
  }

  if (!str.isDefaultable()) {
    return fail("struct must be defaultable");
  }

  return push(RefType::fromTypeIndex(*typeIndex, false));
}

template <typename Policy>
inline bool OpIter<Policy>::readStructGet(uint32_t* typeIndex,
                                          uint32_t* fieldIndex,
                                          FieldExtension fieldExtension,
                                          Value* ptr) {
  MOZ_ASSERT(typeIndex != fieldIndex);
  MOZ_ASSERT(Classify(op_) == OpKind::StructGet);

  if (!readStructTypeIndex(typeIndex)) {
    return false;
  }

  const StructType& structType = env_.types.structType(*typeIndex);

  if (!readFieldIndex(fieldIndex, structType)) {
    return false;
  }

  if (!popWithType(RefType::fromTypeIndex(*typeIndex, true), ptr)) {
    return false;
  }

  FieldType fieldType = structType.fields_[*fieldIndex].type;

  if (fieldType.isValType() && fieldExtension != FieldExtension::None) {
    return fail("must not specify signedness for unpacked field type");
  }

  if (!fieldType.isValType() && fieldExtension == FieldExtension::None) {
    return fail("must specify signedness for packed field type");
  }

  return push(fieldType.widenToValType());
}

template <typename Policy>
inline bool OpIter<Policy>::readStructSet(uint32_t* typeIndex,
                                          uint32_t* fieldIndex, Value* ptr,
                                          Value* val) {
  MOZ_ASSERT(typeIndex != fieldIndex);
  MOZ_ASSERT(Classify(op_) == OpKind::StructSet);

  if (!readStructTypeIndex(typeIndex)) {
    return false;
  }

  const StructType& structType = env_.types.structType(*typeIndex);

  if (!readFieldIndex(fieldIndex, structType)) {
    return false;
  }

  if (!popWithType(structType.fields_[*fieldIndex].type.widenToValType(),
                   val)) {
    return false;
  }

  if (!structType.fields_[*fieldIndex].isMutable) {
    return fail("field is not mutable");
  }

  if (!popWithType(RefType::fromTypeIndex(*typeIndex, true), ptr)) {
    return false;
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readArrayNewWithRtt(uint32_t* typeIndex, Value* rtt,
                                                Value* length,
                                                Value* argValue) {
  MOZ_ASSERT(Classify(op_) == OpKind::ArrayNewWithRtt);

  if (!readArrayTypeIndex(typeIndex)) {
    return false;
  }

  const ArrayType& arr = env_.types.arrayType(*typeIndex);
  const ValType rttType = ValType::fromRtt(*typeIndex, 0);

  if (!popWithType(rttType, rtt)) {
    return false;
  }

  if (!popWithType(ValType::I32, length)) {
    return false;
  }

  if (!popWithType(arr.elementType_.widenToValType(), argValue)) {
    return false;
  }

  return push(RefType::fromTypeIndex(*typeIndex, false));
}

template <typename Policy>
inline bool OpIter<Policy>::readArrayNewDefaultWithRtt(uint32_t* typeIndex,
                                                       Value* rtt,
                                                       Value* length) {
  MOZ_ASSERT(Classify(op_) == OpKind::ArrayNewDefaultWithRtt);

  if (!readArrayTypeIndex(typeIndex)) {
    return false;
  }

  const ArrayType& arr = env_.types.arrayType(*typeIndex);
  const ValType rttType = ValType::fromRtt(*typeIndex, 0);

  if (!popWithType(rttType, rtt)) {
    return false;
  }

  if (!popWithType(ValType::I32, length)) {
    return false;
  }

  if (!arr.elementType_.isDefaultable()) {
    return fail("array must be defaultable");
  }

  return push(RefType::fromTypeIndex(*typeIndex, false));
}

template <typename Policy>
inline bool OpIter<Policy>::readArrayGet(uint32_t* typeIndex,
                                         FieldExtension extension, Value* index,
                                         Value* ptr) {
  MOZ_ASSERT(Classify(op_) == OpKind::ArrayGet);

  if (!readArrayTypeIndex(typeIndex)) {
    return false;
  }

  const ArrayType& arrayType = env_.types.arrayType(*typeIndex);

  if (!popWithType(ValType::I32, index)) {
    return false;
  }

  if (!popWithType(RefType::fromTypeIndex(*typeIndex, true), ptr)) {
    return false;
  }

  FieldType fieldType = arrayType.elementType_;

  if (fieldType.isValType() && extension != FieldExtension::None) {
    return fail("must not specify signedness for unpacked element type");
  }

  if (!fieldType.isValType() && extension == FieldExtension::None) {
    return fail("must specify signedness for packed element type");
  }

  return push(fieldType.widenToValType());
}

template <typename Policy>
inline bool OpIter<Policy>::readArraySet(uint32_t* typeIndex, Value* val,
                                         Value* index, Value* ptr) {
  MOZ_ASSERT(Classify(op_) == OpKind::ArraySet);

  if (!readArrayTypeIndex(typeIndex)) {
    return false;
  }

  const ArrayType& arrayType = env_.types.arrayType(*typeIndex);

  if (!arrayType.isMutable_) {
    return fail("array is not mutable");
  }

  if (!popWithType(arrayType.elementType_.widenToValType(), val)) {
    return false;
  }

  if (!popWithType(ValType::I32, index)) {
    return false;
  }

  if (!popWithType(RefType::fromTypeIndex(*typeIndex, true), ptr)) {
    return false;
  }

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readArrayLen(uint32_t* typeIndex, Value* ptr) {
  MOZ_ASSERT(Classify(op_) == OpKind::ArrayLen);

  if (!readArrayTypeIndex(typeIndex)) {
    return false;
  }

  if (!popWithType(RefType::fromTypeIndex(*typeIndex, true), ptr)) {
    return false;
  }

  return push(ValType::I32);
}

template <typename Policy>
inline bool OpIter<Policy>::readRttCanon(ValType* rttType) {
  MOZ_ASSERT(Classify(op_) == OpKind::RttCanon);

  uint32_t typeIndex;
  if (!readGcTypeIndex(&typeIndex)) {
    return false;
  }

  *rttType = ValType::fromRtt(typeIndex, 0);
  return push(*rttType);
}

template <typename Policy>
inline bool OpIter<Policy>::readRttSub(Value* parentRtt) {
  MOZ_ASSERT(Classify(op_) == OpKind::RttSub);

  uint32_t rttTypeIndex;
  uint32_t rttDepth;
  if (!popWithRttType(parentRtt, &rttTypeIndex, &rttDepth)) {
    return false;
  }

  if (rttDepth >= MaxRttDepth) {
    return fail("rtt depth is too deep");
  }
  return push(ValType::fromRtt(rttTypeIndex, rttDepth + 1));
}

template <typename Policy>
inline bool OpIter<Policy>::readRefTest(Value* rtt, uint32_t* rttTypeIndex,
                                        uint32_t* rttDepth, Value* ref) {
  MOZ_ASSERT(Classify(op_) == OpKind::RefTest);

  if (!popWithRttType(rtt, rttTypeIndex, rttDepth)) {
    return false;
  }
  if (!popWithType(RefType::eq(), ref)) {
    return false;
  }
  return push(ValType(ValType::I32));
}

template <typename Policy>
inline bool OpIter<Policy>::readRefCast(Value* rtt, uint32_t* rttTypeIndex,
                                        uint32_t* rttDepth, Value* ref) {
  MOZ_ASSERT(Classify(op_) == OpKind::RefCast);

  if (!popWithRttType(rtt, rttTypeIndex, rttDepth)) {
    return false;
  }
  if (!popWithType(RefType::eq(), ref)) {
    return false;
  }
  return push(RefType::fromTypeIndex(*rttTypeIndex, false));
}

template <typename Policy>
inline bool OpIter<Policy>::readBrOnCast(uint32_t* relativeDepth, Value* rtt,
                                         uint32_t* rttTypeIndex,
                                         uint32_t* rttDepth,
                                         ResultType* branchTargetType,
                                         ValueVector* values) {
  MOZ_ASSERT(Classify(op_) == OpKind::BrOnCast);

  if (!readVarU32(relativeDepth)) {
    return fail("unable to read br_on_cast depth");
  }

  if (!popWithRttType(rtt, rttTypeIndex, rttDepth)) {
    return false;
  }

  // The casted from type is any subtype of eqref
  ValType castedFromType(RefType::eq());

  // The casted to type is a non-nullable reference to the type index specified
  // by the input rtt on the stack
  ValType castedToType(RefType::fromTypeIndex(*rttTypeIndex, false));

  return checkCastedBranchValue(*relativeDepth, castedFromType, castedToType,
                                branchTargetType, values);
}

#ifdef ENABLE_WASM_SIMD

template <typename Policy>
inline bool OpIter<Policy>::readLaneIndex(uint32_t inputLanes,
                                          uint32_t* laneIndex) {
  uint8_t tmp;
  if (!readFixedU8(&tmp)) {
    return false;  // Caller signals error
  }
  if (tmp >= inputLanes) {
    return false;
  }
  *laneIndex = tmp;
  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readExtractLane(ValType resultType,
                                            uint32_t inputLanes,
                                            uint32_t* laneIndex, Value* input) {
  MOZ_ASSERT(Classify(op_) == OpKind::ExtractLane);

  if (!readLaneIndex(inputLanes, laneIndex)) {
    return fail("missing or invalid extract_lane lane index");
  }

  if (!popWithType(ValType::V128, input)) {
    return false;
  }

  infalliblePush(resultType);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readReplaceLane(ValType operandType,
                                            uint32_t inputLanes,
                                            uint32_t* laneIndex,
                                            Value* baseValue, Value* operand) {
  MOZ_ASSERT(Classify(op_) == OpKind::ReplaceLane);

  if (!readLaneIndex(inputLanes, laneIndex)) {
    return fail("missing or invalid replace_lane lane index");
  }

  if (!popWithType(operandType, operand)) {
    return false;
  }

  if (!popWithType(ValType::V128, baseValue)) {
    return false;
  }

  infalliblePush(ValType::V128);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readVectorShift(Value* baseValue, Value* shift) {
  MOZ_ASSERT(Classify(op_) == OpKind::VectorShift);

  if (!popWithType(ValType::I32, shift)) {
    return false;
  }

  if (!popWithType(ValType::V128, baseValue)) {
    return false;
  }

  infalliblePush(ValType::V128);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readVectorSelect(Value* v1, Value* v2,
                                             Value* controlMask) {
  MOZ_ASSERT(Classify(op_) == OpKind::VectorSelect);

  if (!popWithType(ValType::V128, controlMask)) {
    return false;
  }

  if (!popWithType(ValType::V128, v2)) {
    return false;
  }

  if (!popWithType(ValType::V128, v1)) {
    return false;
  }

  infalliblePush(ValType::V128);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readVectorShuffle(Value* v1, Value* v2,
                                              V128* selectMask) {
  MOZ_ASSERT(Classify(op_) == OpKind::VectorShuffle);

  for (unsigned char& byte : selectMask->bytes) {
    uint8_t tmp;
    if (!readFixedU8(&tmp)) {
      return fail("unable to read shuffle index");
    }
    if (tmp > 31) {
      return fail("shuffle index out of range");
    }
    byte = tmp;
  }

  if (!popWithType(ValType::V128, v2)) {
    return false;
  }

  if (!popWithType(ValType::V128, v1)) {
    return false;
  }

  infalliblePush(ValType::V128);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readV128Const(V128* value) {
  MOZ_ASSERT(Classify(op_) == OpKind::V128);

  if (!d_.readV128Const(value)) {
    return false;
  }

  return push(ValType::V128);
}

template <typename Policy>
inline bool OpIter<Policy>::readLoadSplat(uint32_t byteSize,
                                          LinearMemoryAddress<Value>* addr) {
  MOZ_ASSERT(Classify(op_) == OpKind::Load);

  if (!readLinearMemoryAddress(byteSize, addr)) {
    return false;
  }

  infalliblePush(ValType::V128);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readLoadExtend(LinearMemoryAddress<Value>* addr) {
  MOZ_ASSERT(Classify(op_) == OpKind::Load);

  if (!readLinearMemoryAddress(/*byteSize=*/8, addr)) {
    return false;
  }

  infalliblePush(ValType::V128);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readLoadLane(uint32_t byteSize,
                                         LinearMemoryAddress<Value>* addr,
                                         uint32_t* laneIndex, Value* input) {
  MOZ_ASSERT(Classify(op_) == OpKind::LoadLane);

  if (!popWithType(ValType::V128, input)) {
    return false;
  }

  if (!readLinearMemoryAddress(byteSize, addr)) {
    return false;
  }

  uint32_t inputLanes = 16 / byteSize;
  if (!readLaneIndex(inputLanes, laneIndex)) {
    return fail("missing or invalid load_lane lane index");
  }

  infalliblePush(ValType::V128);

  return true;
}

template <typename Policy>
inline bool OpIter<Policy>::readStoreLane(uint32_t byteSize,
                                          LinearMemoryAddress<Value>* addr,
                                          uint32_t* laneIndex, Value* input) {
  MOZ_ASSERT(Classify(op_) == OpKind::StoreLane);

  if (!popWithType(ValType::V128, input)) {
    return false;
  }

  if (!readLinearMemoryAddress(byteSize, addr)) {
    return false;
  }

  uint32_t inputLanes = 16 / byteSize;
  if (!readLaneIndex(inputLanes, laneIndex)) {
    return fail("missing or invalid store_lane lane index");
  }

  return true;
}

#endif  // ENABLE_WASM_SIMD

}  // namespace wasm
}  // namespace js

namespace mozilla {

// Specialize IsPod for the Nothing specializations.
template <>
struct IsPod<js::wasm::TypeAndValueT<Nothing>> : std::true_type {};
template <>
struct IsPod<js::wasm::ControlStackEntry<Nothing>> : std::true_type {};

}  // namespace mozilla

#endif  // wasm_op_iter_h
