/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BytecodeLocation_h
#define vm_BytecodeLocation_h

#include "frontend/NameAnalysisTypes.h"
#include "js/TypeDecls.h"
#include "vm/AsyncFunctionResolveKind.h"
#include "vm/BuiltinObjectKind.h"
#include "vm/BytecodeUtil.h"
#include "vm/CheckIsObjectKind.h"   // CheckIsObjectKind
#include "vm/CompletionKind.h"      // CompletionKind
#include "vm/FunctionPrefixKind.h"  // FunctionPrefixKind
#include "vm/GeneratorResumeKind.h"

namespace js {

using RawBytecodeLocationOffset = uint32_t;

class PropertyName;
class RegExpObject;

class BytecodeLocationOffset {
  RawBytecodeLocationOffset rawOffset_;

 public:
  explicit BytecodeLocationOffset(RawBytecodeLocationOffset offset)
      : rawOffset_(offset) {}

  RawBytecodeLocationOffset rawOffset() const { return rawOffset_; }
};

using RawBytecode = jsbytecode*;

// A immutable representation of a program location
//
class BytecodeLocation {
  RawBytecode rawBytecode_;
#ifdef DEBUG
  const JSScript* debugOnlyScript_;
#endif

  // Construct a new BytecodeLocation, while borrowing scriptIdentity
  // from some other BytecodeLocation.
  BytecodeLocation(const BytecodeLocation& loc, RawBytecode pc)
      : rawBytecode_(pc)
#ifdef DEBUG
        ,
        debugOnlyScript_(loc.debugOnlyScript_)
#endif
  {
    MOZ_ASSERT(isValid());
  }

 public:
  // Disallow the creation of an uninitialized location.
  BytecodeLocation() = delete;

  BytecodeLocation(const JSScript* script, RawBytecode pc)
      : rawBytecode_(pc)
#ifdef DEBUG
        ,
        debugOnlyScript_(script)
#endif
  {
    MOZ_ASSERT(isValid());
  }

  RawBytecode toRawBytecode() const { return rawBytecode_; }

#ifdef DEBUG
  // Return true if this bytecode location is valid for the given script.
  // This includes the location 1-past the end of the bytecode.
  bool isValid(const JSScript* script) const;

  // Return true if this bytecode location is within the bounds of the
  // bytecode for a given script.
  bool isInBounds(const JSScript* script) const;

  const JSScript* getDebugOnlyScript() const;
#endif

  inline uint32_t bytecodeToOffset(const JSScript* script) const;

  inline uint32_t tableSwitchCaseOffset(const JSScript* script,
                                        uint32_t caseIndex) const;

  inline uint32_t getJumpTargetOffset(const JSScript* script) const;

  inline uint32_t getTableSwitchDefaultOffset(const JSScript* script) const;

  inline BytecodeLocation getTableSwitchDefaultTarget() const;
  inline BytecodeLocation getTableSwitchCaseTarget(const JSScript* script,
                                                   uint32_t caseIndex) const;

  inline uint32_t useCount() const;
  inline uint32_t defCount() const;

  int32_t jumpOffset() const { return GET_JUMP_OFFSET(rawBytecode_); }

  inline JSAtom* getAtom(const JSScript* script) const;
  inline JSString* getString(const JSScript* script) const;
  inline PropertyName* getPropertyName(const JSScript* script) const;
  inline JS::BigInt* getBigInt(const JSScript* script) const;
  inline JSObject* getObject(const JSScript* script) const;
  inline JSFunction* getFunction(const JSScript* script) const;
  inline js::RegExpObject* getRegExp(const JSScript* script) const;
  inline js::Scope* getScope(const JSScript* script) const;

  uint32_t getSymbolIndex() const {
    MOZ_ASSERT(is(JSOp::Symbol));
    return GET_UINT8(rawBytecode_);
  }

  inline Scope* innermostScope(const JSScript* script) const;

#ifdef DEBUG
  bool hasSameScript(const BytecodeLocation& other) const {
    return debugOnlyScript_ == other.debugOnlyScript_;
  }
#endif

  // Overloaded operators

  bool operator==(const BytecodeLocation& other) const {
    MOZ_ASSERT(this->debugOnlyScript_ == other.debugOnlyScript_);
    return rawBytecode_ == other.rawBytecode_;
  }

  bool operator!=(const BytecodeLocation& other) const {
    return !(other == *this);
  }

  bool operator<(const BytecodeLocation& other) const {
    MOZ_ASSERT(this->debugOnlyScript_ == other.debugOnlyScript_);
    return rawBytecode_ < other.rawBytecode_;
  }

  // It is traditional to represent the rest of the relational operators
  // using operator<, so we don't need to assert for these.
  bool operator>(const BytecodeLocation& other) const { return other < *this; }

  bool operator<=(const BytecodeLocation& other) const {
    return !(other < *this);
  }

  bool operator>=(const BytecodeLocation& other) const {
    return !(*this < other);
  }

  // Return the next bytecode
  BytecodeLocation next() const {
    return BytecodeLocation(*this,
                            rawBytecode_ + GetBytecodeLength(rawBytecode_));
  }

  // Add an offset.
  BytecodeLocation operator+(const BytecodeLocationOffset& offset) const {
    return BytecodeLocation(*this, rawBytecode_ + offset.rawOffset());
  }

  // Identity Checks
  bool is(JSOp op) const {
    MOZ_ASSERT(isInBounds());
    return getOp() == op;
  }

  // Accessors:

  uint32_t length() const { return GetBytecodeLength(rawBytecode_); }

  bool isJumpTarget() const { return BytecodeIsJumpTarget(getOp()); }

  bool isJump() const { return IsJumpOpcode(getOp()); }

  bool isBackedge() const { return IsBackedgePC(rawBytecode_); }

  bool isBackedgeForLoophead(BytecodeLocation loopHead) const {
    return IsBackedgeForLoopHead(rawBytecode_, loopHead.rawBytecode_);
  }

  bool opHasIC() const { return BytecodeOpHasIC(getOp()); }

  bool fallsThrough() const { return BytecodeFallsThrough(getOp()); }

  uint32_t icIndex() const { return GET_ICINDEX(rawBytecode_); }

  uint32_t local() const { return GET_LOCALNO(rawBytecode_); }

  uint16_t arg() const { return GET_ARGNO(rawBytecode_); }

  bool isEqualityOp() const { return IsEqualityOp(getOp()); }

  bool isStrictEqualityOp() const { return IsStrictEqualityOp(getOp()); }

  bool isStrictSetOp() const { return IsStrictSetPC(rawBytecode_); }

  bool isNameOp() const { return IsNameOp(getOp()); }

  bool isSpreadOp() const { return IsSpreadOp(getOp()); }

  bool isInvokeOp() const { return IsInvokeOp(getOp()); }

  bool isGetPropOp() const { return IsGetPropOp(getOp()); }
  bool isGetElemOp() const { return IsGetElemOp(getOp()); }

  bool isSetPropOp() const { return IsSetPropOp(getOp()); }
  bool isSetElemOp() const { return IsSetElemOp(getOp()); }

  AsyncFunctionResolveKind getAsyncFunctionResolveKind() {
    return AsyncFunctionResolveKind(GET_UINT8(rawBytecode_));
  }

  bool resultIsPopped() const {
    MOZ_ASSERT(StackDefs(getOp()) == 1);
    return BytecodeIsPopped(rawBytecode_);
  }

  // Accessors:
  JSOp getOp() const { return JSOp(*rawBytecode_); }

  BytecodeLocation getJumpTarget() const {
    MOZ_ASSERT(isJump());
    return BytecodeLocation(*this,
                            rawBytecode_ + GET_JUMP_OFFSET(rawBytecode_));
  }

  // Return the 'low' parameter to the tableswitch opcode
  int32_t getTableSwitchLow() const {
    MOZ_ASSERT(is(JSOp::TableSwitch));
    return GET_JUMP_OFFSET(rawBytecode_ + JUMP_OFFSET_LEN);
  }

  // Return the 'high' parameter to the tableswitch opcode
  int32_t getTableSwitchHigh() const {
    MOZ_ASSERT(is(JSOp::TableSwitch));
    return GET_JUMP_OFFSET(rawBytecode_ + (2 * JUMP_OFFSET_LEN));
  }

  uint32_t getPopCount() const {
    MOZ_ASSERT(is(JSOp::PopN));
    return GET_UINT16(rawBytecode_);
  }

  uint32_t getDupAtIndex() const {
    MOZ_ASSERT(is(JSOp::DupAt));
    return GET_UINT24(rawBytecode_);
  }

  uint8_t getPickDepth() const {
    MOZ_ASSERT(is(JSOp::Pick));
    return GET_UINT8(rawBytecode_);
  }
  uint8_t getUnpickDepth() const {
    MOZ_ASSERT(is(JSOp::Unpick));
    return GET_UINT8(rawBytecode_);
  }

  uint32_t getEnvCalleeNumHops() const {
    MOZ_ASSERT(is(JSOp::EnvCallee));
    return GET_UINT8(rawBytecode_);
  }

  EnvironmentCoordinate getEnvironmentCoordinate() const {
    MOZ_ASSERT(JOF_OPTYPE(getOp()) == JOF_ENVCOORD);
    return EnvironmentCoordinate(rawBytecode_);
  }

  uint32_t getCallArgc() const {
    MOZ_ASSERT(JOF_OPTYPE(getOp()) == JOF_ARGC);
    return GET_ARGC(rawBytecode_);
  }

  uint32_t getInitElemArrayIndex() const {
    MOZ_ASSERT(is(JSOp::InitElemArray));
    uint32_t index = GET_UINT32(rawBytecode_);
    MOZ_ASSERT(index <= INT32_MAX,
               "the bytecode emitter must never generate JSOp::InitElemArray "
               "with an index exceeding int32_t range");
    return index;
  }

  FunctionPrefixKind getFunctionPrefixKind() const {
    MOZ_ASSERT(is(JSOp::SetFunName));
    return FunctionPrefixKind(GET_UINT8(rawBytecode_));
  }

  CheckIsObjectKind getCheckIsObjectKind() const {
    MOZ_ASSERT(is(JSOp::CheckIsObj));
    return CheckIsObjectKind(GET_UINT8(rawBytecode_));
  }

  BuiltinObjectKind getBuiltinObjectKind() const {
    MOZ_ASSERT(is(JSOp::BuiltinObject));
    return BuiltinObjectKind(GET_UINT8(rawBytecode_));
  }

  CompletionKind getCompletionKind() const {
    MOZ_ASSERT(is(JSOp::CloseIter));
    return CompletionKind(GET_UINT8(rawBytecode_));
  }

  uint32_t getNewArrayLength() const {
    MOZ_ASSERT(is(JSOp::NewArray));
    return GET_UINT32(rawBytecode_);
  }

  int8_t getInt8() const {
    MOZ_ASSERT(is(JSOp::Int8));
    return GET_INT8(rawBytecode_);
  }
  uint16_t getUint16() const {
    MOZ_ASSERT(is(JSOp::Uint16));
    return GET_UINT16(rawBytecode_);
  }
  uint32_t getUint24() const {
    MOZ_ASSERT(is(JSOp::Uint24));
    return GET_UINT24(rawBytecode_);
  }
  int32_t getInt32() const {
    MOZ_ASSERT(is(JSOp::Int32));
    return GET_INT32(rawBytecode_);
  }
  uint32_t getResumeIndex() const {
    MOZ_ASSERT(is(JSOp::InitialYield) || is(JSOp::Yield) || is(JSOp::Await));
    return GET_RESUMEINDEX(rawBytecode_);
  }
  Value getInlineValue() const {
    MOZ_ASSERT(is(JSOp::Double));
    return GET_INLINE_VALUE(rawBytecode_);
  }

  GeneratorResumeKind resumeKind() { return ResumeKindFromPC(rawBytecode_); }

  ThrowMsgKind throwMsgKind() {
    MOZ_ASSERT(is(JSOp::ThrowMsg));
    return static_cast<ThrowMsgKind>(GET_UINT8(rawBytecode_));
  }

#ifdef DEBUG
  // To ease writing assertions
  bool isValid() const { return isValid(debugOnlyScript_); }

  bool isInBounds() const { return isInBounds(debugOnlyScript_); }
#endif
};

}  // namespace js

#endif
