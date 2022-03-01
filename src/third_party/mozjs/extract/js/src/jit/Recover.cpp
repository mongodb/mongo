/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Recover.h"

#include "jsapi.h"
#include "jsmath.h"

#include "builtin/RegExp.h"
#include "builtin/String.h"
#include "jit/CompileInfo.h"
#include "jit/Ion.h"
#include "jit/JitSpewer.h"
#include "jit/JSJitFrameIter.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "jit/VMFunctions.h"
#include "vm/BigIntType.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"

#include "vm/Interpreter-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::jit;

bool MNode::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_CRASH("This instruction is not serializable");
}

void RInstruction::readRecoverData(CompactBufferReader& reader,
                                   RInstructionStorage* raw) {
  uint32_t op = reader.readUnsigned();
  switch (Opcode(op)) {
#define MATCH_OPCODES_(op)                                                  \
  case Recover_##op:                                                        \
    static_assert(sizeof(R##op) <= sizeof(RInstructionStorage),             \
                  "storage space must be big enough to store R" #op);       \
    static_assert(alignof(R##op) <= alignof(RInstructionStorage),           \
                  "storage space must be aligned adequate to store R" #op); \
    new (raw->addr()) R##op(reader);                                        \
    break;

    RECOVER_OPCODE_LIST(MATCH_OPCODES_)
#undef MATCH_OPCODES_

    case Recover_Invalid:
    default:
      MOZ_CRASH("Bad decoding of the previous instruction?");
  }
}

bool MResumePoint::writeRecoverData(CompactBufferWriter& writer) const {
  writer.writeUnsigned(uint32_t(RInstruction::Recover_ResumePoint));

  MBasicBlock* bb = block();
  JSFunction* fun = bb->info().funMaybeLazy();
  JSScript* script = bb->info().script();
  uint32_t exprStack = stackDepth() - bb->info().ninvoke();

#ifdef DEBUG
  // Ensure that all snapshot which are encoded can safely be used for
  // bailouts.
  if (GetJitContext()->cx) {
    uint32_t stackDepth;
    bool reachablePC;
    jsbytecode* bailPC = pc();

    if (mode() == MResumePoint::ResumeAfter) {
      bailPC = GetNextPc(pc());
    }

    if (!ReconstructStackDepth(GetJitContext()->cx, script, bailPC, &stackDepth,
                               &reachablePC)) {
      return false;
    }

    if (reachablePC) {
      JSOp bailOp = JSOp(*bailPC);
      if (bailOp == JSOp::FunCall) {
        // For fun.call(this, ...); the reconstructStackDepth will
        // include the this. When inlining that is not included.  So the
        // exprStackSlots will be one less.
        MOZ_ASSERT(stackDepth - exprStack <= 1);
      } else {
        // With accessors, we have different stack depths depending on
        // whether or not we inlined the accessor, as the inlined stack
        // contains a callee function that should never have been there
        // and we might just be capturing an uneventful property site,
        // in which case there won't have been any violence.
        MOZ_ASSERT_IF(!IsIonInlinableGetterOrSetterOp(bailOp),
                      exprStack == stackDepth);
      }
    }
  }
#endif

  // Test if we honor the maximum of arguments at all times.  This is a sanity
  // check and not an algorithm limit. So check might be a bit too loose.  +4
  // to account for scope chain, return value, this value and maybe
  // arguments_object.
  MOZ_ASSERT(CountArgSlots(script, fun) < SNAPSHOT_MAX_NARGS + 4);

#ifdef JS_JITSPEW
  uint32_t implicit = StartArgSlot(script);
#endif
  uint32_t formalArgs = CountArgSlots(script, fun);
  uint32_t nallocs = formalArgs + script->nfixed() + exprStack;

  JitSpew(JitSpew_IonSnapshots,
          "Starting frame; implicit %u, formals %u, fixed %zu, exprs %u",
          implicit, formalArgs - implicit, script->nfixed(), exprStack);

  uint32_t pcoff = script->pcToOffset(pc());
  JitSpew(JitSpew_IonSnapshots, "Writing pc offset %u, nslots %u", pcoff,
          nallocs);
  writer.writeUnsigned(pcoff);
  writer.writeUnsigned(nallocs);
  return true;
}

RResumePoint::RResumePoint(CompactBufferReader& reader) {
  pcOffset_ = reader.readUnsigned();
  numOperands_ = reader.readUnsigned();
  JitSpew(JitSpew_IonSnapshots, "Read RResumePoint (pc offset %u, nslots %u)",
          pcOffset_, numOperands_);
}

bool RResumePoint::recover(JSContext* cx, SnapshotIterator& iter) const {
  MOZ_CRASH("This instruction is not recoverable.");
}

bool MBitNot::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BitNot));
  return true;
}

RBitNot::RBitNot(CompactBufferReader& reader) {}

bool RBitNot::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());
  RootedValue result(cx);

  if (!js::BitNot(cx, &operand, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBitAnd::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BitAnd));
  return true;
}

RBitAnd::RBitAnd(CompactBufferReader& reader) {}

bool RBitAnd::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);
  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());

  if (!js::BitAnd(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBitOr::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BitOr));
  return true;
}

RBitOr::RBitOr(CompactBufferReader& reader) {}

bool RBitOr::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);
  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());

  if (!js::BitOr(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBitXor::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BitXor));
  return true;
}

RBitXor::RBitXor(CompactBufferReader& reader) {}

bool RBitXor::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  if (!js::BitXor(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MLsh::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Lsh));
  return true;
}

RLsh::RLsh(CompactBufferReader& reader) {}

bool RLsh::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);
  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());

  if (!js::BitLsh(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MRsh::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Rsh));
  return true;
}

RRsh::RRsh(CompactBufferReader& reader) {}

bool RRsh::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);
  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());

  if (!js::BitRsh(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MUrsh::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Ursh));
  return true;
}

RUrsh::RUrsh(CompactBufferReader& reader) {}

bool RUrsh::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());

  RootedValue result(cx);
  if (!js::UrshValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MSignExtendInt32::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_SignExtendInt32));
  MOZ_ASSERT(Mode(uint8_t(mode_)) == mode_);
  writer.writeByte(uint8_t(mode_));
  return true;
}

RSignExtendInt32::RSignExtendInt32(CompactBufferReader& reader) {
  mode_ = reader.readByte();
}

bool RSignExtendInt32::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());

  int32_t i;
  if (!ToInt32(cx, operand, &i)) {
    return false;
  }

  int32_t result;
  switch (MSignExtendInt32::Mode(mode_)) {
    case MSignExtendInt32::Byte:
      result = static_cast<int8_t>(i);
      break;
    case MSignExtendInt32::Half:
      result = static_cast<int16_t>(i);
      break;
  }

  RootedValue rootedResult(cx, js::Int32Value(result));
  iter.storeInstructionResult(rootedResult);
  return true;
}

bool MAdd::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Add));
  writer.writeByte(type() == MIRType::Float32);
  return true;
}

RAdd::RAdd(CompactBufferReader& reader) {
  isFloatOperation_ = reader.readByte();
}

bool RAdd::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());
  if (!js::AddValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  // MIRType::Float32 is a specialization embedding the fact that the result is
  // rounded to a Float32.
  if (isFloatOperation_ && !RoundFloat32(cx, result, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MSub::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Sub));
  writer.writeByte(type() == MIRType::Float32);
  return true;
}

RSub::RSub(CompactBufferReader& reader) {
  isFloatOperation_ = reader.readByte();
}

bool RSub::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());
  if (!js::SubValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  // MIRType::Float32 is a specialization embedding the fact that the result is
  // rounded to a Float32.
  if (isFloatOperation_ && !RoundFloat32(cx, result, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MMul::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Mul));
  writer.writeByte(type() == MIRType::Float32);
  MOZ_ASSERT(Mode(uint8_t(mode_)) == mode_);
  writer.writeByte(uint8_t(mode_));
  return true;
}

RMul::RMul(CompactBufferReader& reader) {
  isFloatOperation_ = reader.readByte();
  mode_ = reader.readByte();
}

bool RMul::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  if (MMul::Mode(mode_) == MMul::Normal) {
    if (!js::MulValues(cx, &lhs, &rhs, &result)) {
      return false;
    }

    // MIRType::Float32 is a specialization embedding the fact that the
    // result is rounded to a Float32.
    if (isFloatOperation_ && !RoundFloat32(cx, result, &result)) {
      return false;
    }
  } else {
    MOZ_ASSERT(MMul::Mode(mode_) == MMul::Integer);
    if (!js::math_imul_handle(cx, lhs, rhs, &result)) {
      return false;
    }
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MDiv::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Div));
  writer.writeByte(type() == MIRType::Float32);
  return true;
}

RDiv::RDiv(CompactBufferReader& reader) {
  isFloatOperation_ = reader.readByte();
}

bool RDiv::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  if (!js::DivValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  // MIRType::Float32 is a specialization embedding the fact that the result is
  // rounded to a Float32.
  if (isFloatOperation_ && !RoundFloat32(cx, result, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MMod::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Mod));
  return true;
}

RMod::RMod(CompactBufferReader& reader) {}

bool RMod::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());
  if (!js::ModValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MNot::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Not));
  return true;
}

RNot::RNot(CompactBufferReader& reader) {}

bool RNot::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue v(cx, iter.read());
  RootedValue result(cx);

  result.setBoolean(!ToBoolean(v));

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntAdd::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntAdd));
  return true;
}

RBigIntAdd::RBigIntAdd(CompactBufferReader& reader) {}

bool RBigIntAdd::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  if (!js::AddValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntSub::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntSub));
  return true;
}

RBigIntSub::RBigIntSub(CompactBufferReader& reader) {}

bool RBigIntSub::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  if (!js::SubValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntMul::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntMul));
  return true;
}

RBigIntMul::RBigIntMul(CompactBufferReader& reader) {}

bool RBigIntMul::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  if (!js::MulValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntDiv::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntDiv));
  return true;
}

RBigIntDiv::RBigIntDiv(CompactBufferReader& reader) {}

bool RBigIntDiv::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  MOZ_ASSERT(!rhs.toBigInt()->isZero(),
             "division by zero throws and therefore can't be recovered");
  if (!js::DivValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntMod::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntMod));
  return true;
}

RBigIntMod::RBigIntMod(CompactBufferReader& reader) {}

bool RBigIntMod::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  MOZ_ASSERT(!rhs.toBigInt()->isZero(),
             "division by zero throws and therefore can't be recovered");
  if (!js::ModValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntPow::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntPow));
  return true;
}

RBigIntPow::RBigIntPow(CompactBufferReader& reader) {}

bool RBigIntPow::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  MOZ_ASSERT(!rhs.toBigInt()->isNegative(),
             "negative exponent throws and therefore can't be recovered");
  if (!js::PowValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntBitAnd::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntBitAnd));
  return true;
}

RBigIntBitAnd::RBigIntBitAnd(CompactBufferReader& reader) {}

bool RBigIntBitAnd::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  if (!js::BitAnd(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntBitOr::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntBitOr));
  return true;
}

RBigIntBitOr::RBigIntBitOr(CompactBufferReader& reader) {}

bool RBigIntBitOr::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  if (!js::BitOr(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntBitXor::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntBitXor));
  return true;
}

RBigIntBitXor::RBigIntBitXor(CompactBufferReader& reader) {}

bool RBigIntBitXor::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  if (!js::BitXor(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntLsh::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntLsh));
  return true;
}

RBigIntLsh::RBigIntLsh(CompactBufferReader& reader) {}

bool RBigIntLsh::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  if (!js::BitLsh(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntRsh::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntRsh));
  return true;
}

RBigIntRsh::RBigIntRsh(CompactBufferReader& reader) {}

bool RBigIntRsh::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(lhs.isBigInt() && rhs.isBigInt());
  if (!js::BitRsh(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntIncrement::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntIncrement));
  return true;
}

RBigIntIncrement::RBigIntIncrement(CompactBufferReader& reader) {}

bool RBigIntIncrement::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(operand.isBigInt());
  if (!js::IncOperation(cx, operand, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntDecrement::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntDecrement));
  return true;
}

RBigIntDecrement::RBigIntDecrement(CompactBufferReader& reader) {}

bool RBigIntDecrement::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(operand.isBigInt());
  if (!js::DecOperation(cx, operand, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntNegate::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntNegate));
  return true;
}

RBigIntNegate::RBigIntNegate(CompactBufferReader& reader) {}

bool RBigIntNegate::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(operand.isBigInt());
  if (!js::NegOperation(cx, &operand, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MBigIntBitNot::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntBitNot));
  return true;
}

RBigIntBitNot::RBigIntBitNot(CompactBufferReader& reader) {}

bool RBigIntBitNot::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(operand.isBigInt());
  if (!js::BitNot(cx, &operand, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MConcat::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Concat));
  return true;
}

RConcat::RConcat(CompactBufferReader& reader) {}

bool RConcat::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue lhs(cx, iter.read());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!lhs.isObject() && !rhs.isObject());
  if (!js::AddValues(cx, &lhs, &rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

RStringLength::RStringLength(CompactBufferReader& reader) {}

bool RStringLength::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!operand.isObject());
  if (!js::GetLengthProperty(operand, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MStringLength::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_StringLength));
  return true;
}

bool MArgumentsLength::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_ArgumentsLength));
  return true;
}

RArgumentsLength::RArgumentsLength(CompactBufferReader& reader) {}

bool RArgumentsLength::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue result(cx);

  result.setInt32(iter.frame()->numActualArgs());

  iter.storeInstructionResult(result);
  return true;
}

bool MFloor::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Floor));
  return true;
}

RFloor::RFloor(CompactBufferReader& reader) {}

bool RFloor::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue v(cx, iter.read());
  RootedValue result(cx);

  if (!js::math_floor_handle(cx, v, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MCeil::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Ceil));
  return true;
}

RCeil::RCeil(CompactBufferReader& reader) {}

bool RCeil::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue v(cx, iter.read());
  RootedValue result(cx);

  if (!js::math_ceil_handle(cx, v, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MRound::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Round));
  return true;
}

RRound::RRound(CompactBufferReader& reader) {}

bool RRound::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue arg(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!arg.isObject());
  if (!js::math_round_handle(cx, arg, &result)) return false;

  iter.storeInstructionResult(result);
  return true;
}

bool MTrunc::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Trunc));
  return true;
}

RTrunc::RTrunc(CompactBufferReader& reader) {}

bool RTrunc::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue arg(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!arg.isObject());
  if (!js::math_trunc_handle(cx, arg, &result)) return false;

  iter.storeInstructionResult(result);
  return true;
}

bool MCharCodeAt::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_CharCodeAt));
  return true;
}

RCharCodeAt::RCharCodeAt(CompactBufferReader& reader) {}

bool RCharCodeAt::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedString lhs(cx, iter.read().toString());
  RootedValue rhs(cx, iter.read());
  RootedValue result(cx);

  if (!js::str_charCodeAt_impl(cx, lhs, rhs, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MFromCharCode::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_FromCharCode));
  return true;
}

RFromCharCode::RFromCharCode(CompactBufferReader& reader) {}

bool RFromCharCode::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!operand.isObject());
  if (!js::str_fromCharCode_one_arg(cx, operand, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MPow::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Pow));
  return true;
}

RPow::RPow(CompactBufferReader& reader) {}

bool RPow::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue base(cx, iter.read());
  RootedValue power(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(base.isNumber() && power.isNumber());
  if (!js::PowValues(cx, &base, &power, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MPowHalf::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_PowHalf));
  return true;
}

RPowHalf::RPowHalf(CompactBufferReader& reader) {}

bool RPowHalf::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue base(cx, iter.read());
  RootedValue power(cx);
  RootedValue result(cx);
  power.setNumber(0.5);

  MOZ_ASSERT(base.isNumber());
  if (!js::PowValues(cx, &base, &power, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MMinMax::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_MinMax));
  writer.writeByte(isMax_);
  return true;
}

RMinMax::RMinMax(CompactBufferReader& reader) { isMax_ = reader.readByte(); }

bool RMinMax::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue a(cx, iter.read());
  RootedValue b(cx, iter.read());
  RootedValue result(cx);

  if (!js::minmax_impl(cx, isMax_, a, b, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MAbs::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Abs));
  return true;
}

RAbs::RAbs(CompactBufferReader& reader) {}

bool RAbs::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue v(cx, iter.read());
  RootedValue result(cx);

  if (!js::math_abs_handle(cx, v, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MSqrt::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Sqrt));
  writer.writeByte(type() == MIRType::Float32);
  return true;
}

RSqrt::RSqrt(CompactBufferReader& reader) {
  isFloatOperation_ = reader.readByte();
}

bool RSqrt::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue num(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(num.isNumber());
  if (!math_sqrt_handle(cx, num, &result)) {
    return false;
  }

  // MIRType::Float32 is a specialization embedding the fact that the result is
  // rounded to a Float32.
  if (isFloatOperation_ && !RoundFloat32(cx, result, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MAtan2::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Atan2));
  return true;
}

RAtan2::RAtan2(CompactBufferReader& reader) {}

bool RAtan2::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue y(cx, iter.read());
  RootedValue x(cx, iter.read());
  RootedValue result(cx);

  if (!math_atan2_handle(cx, y, x, &result)) return false;

  iter.storeInstructionResult(result);
  return true;
}

bool MHypot::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Hypot));
  writer.writeUnsigned(uint32_t(numOperands()));
  return true;
}

RHypot::RHypot(CompactBufferReader& reader)
    : numOperands_(reader.readUnsigned()) {}

bool RHypot::recover(JSContext* cx, SnapshotIterator& iter) const {
  JS::RootedValueVector vec(cx);

  if (!vec.reserve(numOperands_)) {
    return false;
  }

  for (uint32_t i = 0; i < numOperands_; ++i) {
    vec.infallibleAppend(iter.read());
  }

  RootedValue result(cx);

  if (!js::math_hypot_handle(cx, vec, &result)) return false;

  iter.storeInstructionResult(result);
  return true;
}

bool MNearbyInt::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  switch (roundingMode_) {
    case RoundingMode::Up:
      writer.writeUnsigned(uint32_t(RInstruction::Recover_Ceil));
      return true;
    case RoundingMode::Down:
      writer.writeUnsigned(uint32_t(RInstruction::Recover_Floor));
      return true;
    case RoundingMode::TowardsZero:
      writer.writeUnsigned(uint32_t(RInstruction::Recover_Trunc));
      return true;
    default:
      MOZ_CRASH("Unsupported rounding mode.");
  }
}

RNearbyInt::RNearbyInt(CompactBufferReader& reader) {
  roundingMode_ = reader.readByte();
}

bool RNearbyInt::recover(JSContext* cx, SnapshotIterator& iter) const {
  MOZ_CRASH("Unsupported rounding mode.");
}

bool MSign::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Sign));
  return true;
}

RSign::RSign(CompactBufferReader& reader) {}

bool RSign::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue arg(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!arg.isObject());
  if (!js::math_sign_handle(cx, arg, &result)) return false;

  iter.storeInstructionResult(result);
  return true;
}

bool MMathFunction::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  switch (function_) {
    case UnaryMathFunction::Ceil:
      writer.writeUnsigned(uint32_t(RInstruction::Recover_Ceil));
      return true;
    case UnaryMathFunction::Floor:
      writer.writeUnsigned(uint32_t(RInstruction::Recover_Floor));
      return true;
    case UnaryMathFunction::Round:
      writer.writeUnsigned(uint32_t(RInstruction::Recover_Round));
      return true;
    case UnaryMathFunction::Trunc:
      writer.writeUnsigned(uint32_t(RInstruction::Recover_Trunc));
      return true;
    case UnaryMathFunction::Sin:
    case UnaryMathFunction::Log:
      static_assert(sizeof(UnaryMathFunction) == sizeof(uint8_t));
      writer.writeUnsigned(uint32_t(RInstruction::Recover_MathFunction));
      writer.writeByte(uint8_t(function_));
      return true;
    default:
      MOZ_CRASH("Unknown math function.");
  }
}

RMathFunction::RMathFunction(CompactBufferReader& reader) {
  function_ = UnaryMathFunction(reader.readByte());
}

bool RMathFunction::recover(JSContext* cx, SnapshotIterator& iter) const {
  switch (function_) {
    case UnaryMathFunction::Sin: {
      RootedValue arg(cx, iter.read());
      RootedValue result(cx);

      if (!js::math_sin_handle(cx, arg, &result)) {
        return false;
      }

      iter.storeInstructionResult(result);
      return true;
    }
    case UnaryMathFunction::Log: {
      RootedValue arg(cx, iter.read());
      RootedValue result(cx);

      if (!js::math_log_handle(cx, arg, &result)) {
        return false;
      }

      iter.storeInstructionResult(result);
      return true;
    }
    default:
      MOZ_CRASH("Unknown math function.");
  }
}

bool MRandom::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(this->canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Random));
  return true;
}

bool MRandom::canRecoverOnBailout() const {
  return !js::SupportDifferentialTesting();
}

RRandom::RRandom(CompactBufferReader& reader) {}

bool RRandom::recover(JSContext* cx, SnapshotIterator& iter) const {
  iter.storeInstructionResult(DoubleValue(math_random_impl(cx)));
  return true;
}

bool MStringSplit::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_StringSplit));
  return true;
}

RStringSplit::RStringSplit(CompactBufferReader& reader) {}

bool RStringSplit::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedString str(cx, iter.read().toString());
  RootedString sep(cx, iter.read().toString());

  JSObject* res = StringSplitString(cx, str, sep, INT32_MAX);
  if (!res) {
    return false;
  }

  iter.storeInstructionResult(ObjectValue(*res));
  return true;
}

bool MNaNToZero::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_NaNToZero));
  return true;
}

RNaNToZero::RNaNToZero(CompactBufferReader& reader) {}

bool RNaNToZero::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue v(cx, iter.read());
  RootedValue result(cx);
  MOZ_ASSERT(v.isDouble() || v.isInt32());

  // x ? x : 0.0
  if (ToBoolean(v)) {
    result = v;
  } else {
    result.setDouble(0.0);
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MRegExpMatcher::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_RegExpMatcher));
  return true;
}

RRegExpMatcher::RRegExpMatcher(CompactBufferReader& reader) {}

bool RRegExpMatcher::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject regexp(cx, &iter.read().toObject());
  RootedString input(cx, iter.read().toString());
  int32_t lastIndex = iter.read().toInt32();

  RootedValue result(cx);
  if (!RegExpMatcherRaw(cx, regexp, input, lastIndex, nullptr, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MRegExpSearcher::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_RegExpSearcher));
  return true;
}

RRegExpSearcher::RRegExpSearcher(CompactBufferReader& reader) {}

bool RRegExpSearcher::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject regexp(cx, &iter.read().toObject());
  RootedString input(cx, iter.read().toString());
  int32_t lastIndex = iter.read().toInt32();

  int32_t result;
  if (!RegExpSearcherRaw(cx, regexp, input, lastIndex, nullptr, &result)) {
    return false;
  }

  RootedValue resultVal(cx);
  resultVal.setInt32(result);
  iter.storeInstructionResult(resultVal);
  return true;
}

bool MRegExpTester::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_RegExpTester));
  return true;
}

RRegExpTester::RRegExpTester(CompactBufferReader& reader) {}

bool RRegExpTester::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedString string(cx, iter.read().toString());
  RootedObject regexp(cx, &iter.read().toObject());
  int32_t lastIndex = iter.read().toInt32();
  int32_t endIndex;

  if (!js::RegExpTesterRaw(cx, regexp, string, lastIndex, &endIndex)) {
    return false;
  }

  RootedValue result(cx);
  result.setInt32(endIndex);
  iter.storeInstructionResult(result);
  return true;
}

bool MTypeOf::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_TypeOf));
  return true;
}

RTypeOf::RTypeOf(CompactBufferReader& reader) {}

bool RTypeOf::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue v(cx, iter.read());

  RootedValue result(cx, StringValue(TypeOfOperation(v, cx->runtime())));
  iter.storeInstructionResult(result);
  return true;
}

bool MToDouble::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_ToDouble));
  return true;
}

RToDouble::RToDouble(CompactBufferReader& reader) {}

bool RToDouble::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue v(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!v.isObject());
  MOZ_ASSERT(!v.isSymbol());

  double dbl;
  if (!ToNumber(cx, v, &dbl)) {
    return false;
  }

  result.setDouble(dbl);
  iter.storeInstructionResult(result);
  return true;
}

bool MToFloat32::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_ToFloat32));
  return true;
}

RToFloat32::RToFloat32(CompactBufferReader& reader) {}

bool RToFloat32::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue v(cx, iter.read());
  RootedValue result(cx);

  MOZ_ASSERT(!v.isObject());
  if (!RoundFloat32(cx, v, &result)) {
    return false;
  }

  iter.storeInstructionResult(result);
  return true;
}

bool MTruncateToInt32::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_TruncateToInt32));
  return true;
}

RTruncateToInt32::RTruncateToInt32(CompactBufferReader& reader) {}

bool RTruncateToInt32::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue value(cx, iter.read());
  RootedValue result(cx);

  int32_t trunc;
  if (!JS::ToInt32(cx, value, &trunc)) {
    return false;
  }

  result.setInt32(trunc);
  iter.storeInstructionResult(result);
  return true;
}

bool MNewObject::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_NewObject));
  MOZ_ASSERT(Mode(uint8_t(mode_)) == mode_);
  writer.writeByte(uint8_t(mode_));
  return true;
}

RNewObject::RNewObject(CompactBufferReader& reader) {
  mode_ = MNewObject::Mode(reader.readByte());
}

bool RNewObject::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject templateObject(cx, &iter.read().toObject());
  RootedValue result(cx);
  JSObject* resultObject = nullptr;

  // See CodeGenerator::visitNewObjectVMCall
  switch (mode_) {
    case MNewObject::ObjectLiteral:
      resultObject = NewObjectOperationWithTemplate(cx, templateObject);
      break;
    case MNewObject::ObjectCreate:
      resultObject =
          ObjectCreateWithTemplate(cx, templateObject.as<PlainObject>());
      break;
  }

  if (!resultObject) {
    return false;
  }

  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MNewPlainObject::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_NewPlainObject));

  MOZ_ASSERT(gc::AllocKind(uint8_t(allocKind_)) == allocKind_);
  writer.writeByte(uint8_t(allocKind_));
  MOZ_ASSERT(gc::InitialHeap(uint8_t(initialHeap_)) == initialHeap_);
  writer.writeByte(uint8_t(initialHeap_));
  return true;
}

RNewPlainObject::RNewPlainObject(CompactBufferReader& reader) {
  allocKind_ = gc::AllocKind(reader.readByte());
  MOZ_ASSERT(gc::IsValidAllocKind(allocKind_));
  initialHeap_ = gc::InitialHeap(reader.readByte());
  MOZ_ASSERT(initialHeap_ == gc::DefaultHeap ||
             initialHeap_ == gc::TenuredHeap);
}

bool RNewPlainObject::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedShape shape(cx, &iter.read().toGCCellPtr().as<Shape>());

  // See CodeGenerator::visitNewPlainObject.
  JSObject* resultObject =
      NewPlainObjectOptimizedFallback(cx, shape, allocKind_, initialHeap_);
  if (!resultObject) {
    return false;
  }

  RootedValue result(cx);
  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MNewArrayObject::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_NewArrayObject));

  writer.writeUnsigned(length_);
  MOZ_ASSERT(gc::InitialHeap(uint8_t(initialHeap_)) == initialHeap_);
  writer.writeByte(uint8_t(initialHeap_));
  return true;
}

RNewArrayObject::RNewArrayObject(CompactBufferReader& reader) {
  length_ = reader.readUnsigned();
  initialHeap_ = gc::InitialHeap(reader.readByte());
  MOZ_ASSERT(initialHeap_ == gc::DefaultHeap ||
             initialHeap_ == gc::TenuredHeap);
}

bool RNewArrayObject::recover(JSContext* cx, SnapshotIterator& iter) const {
  iter.read();  // Skip unused shape field.

  NewObjectKind kind =
      initialHeap_ == gc::TenuredHeap ? TenuredObject : GenericObject;
  JSObject* array = NewArrayOperation(cx, length_, kind);
  if (!array) {
    return false;
  }

  RootedValue result(cx);
  result.setObject(*array);
  iter.storeInstructionResult(result);
  return true;
}

bool MNewTypedArray::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_NewTypedArray));
  return true;
}

RNewTypedArray::RNewTypedArray(CompactBufferReader& reader) {}

bool RNewTypedArray::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject templateObject(cx, &iter.read().toObject());
  RootedValue result(cx);

  size_t length = templateObject.as<TypedArrayObject>()->length();
  MOZ_ASSERT(length <= INT32_MAX,
             "Template objects are only created for int32 lengths");

  JSObject* resultObject =
      NewTypedArrayWithTemplateAndLength(cx, templateObject, length);
  if (!resultObject) {
    return false;
  }

  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MNewArray::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_NewArray));
  writer.writeUnsigned(length());
  return true;
}

RNewArray::RNewArray(CompactBufferReader& reader) {
  count_ = reader.readUnsigned();
}

bool RNewArray::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject templateObject(cx, &iter.read().toObject());
  RootedValue result(cx);
  RootedShape shape(cx, templateObject->shape());

  ArrayObject* resultObject = NewArrayWithShape(cx, count_, shape);
  if (!resultObject) {
    return false;
  }

  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MNewIterator::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_NewIterator));
  writer.writeByte(type_);
  return true;
}

RNewIterator::RNewIterator(CompactBufferReader& reader) {
  type_ = reader.readByte();
}

bool RNewIterator::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject templateObject(cx, &iter.read().toObject());
  RootedValue result(cx);

  JSObject* resultObject = nullptr;
  switch (MNewIterator::Type(type_)) {
    case MNewIterator::ArrayIterator:
      resultObject = NewArrayIterator(cx);
      break;
    case MNewIterator::StringIterator:
      resultObject = NewStringIterator(cx);
      break;
    case MNewIterator::RegExpStringIterator:
      resultObject = NewRegExpStringIterator(cx);
      break;
  }

  if (!resultObject) {
    return false;
  }

  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MCreateThisWithTemplate::writeRecoverData(
    CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_CreateThisWithTemplate));
  return true;
}

RCreateThisWithTemplate::RCreateThisWithTemplate(CompactBufferReader& reader) {}

bool RCreateThisWithTemplate::recover(JSContext* cx,
                                      SnapshotIterator& iter) const {
  RootedObject templateObject(cx, &iter.read().toObject());

  // See CodeGenerator::visitCreateThisWithTemplate
  JSObject* resultObject = CreateThisWithTemplate(cx, templateObject);
  if (!resultObject) {
    return false;
  }

  RootedValue result(cx);
  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MLambda::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_Lambda));
  return true;
}

RLambda::RLambda(CompactBufferReader& reader) {}

bool RLambda::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject scopeChain(cx, &iter.read().toObject());
  RootedFunction fun(cx, &iter.read().toObject().as<JSFunction>());

  JSObject* resultObject = js::Lambda(cx, fun, scopeChain);
  if (!resultObject) {
    return false;
  }

  RootedValue result(cx);
  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MLambdaArrow::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_LambdaArrow));
  return true;
}

RLambdaArrow::RLambdaArrow(CompactBufferReader& reader) {}

bool RLambdaArrow::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject scopeChain(cx, &iter.read().toObject());
  RootedValue newTarget(cx, iter.read());
  RootedFunction fun(cx, &iter.read().toObject().as<JSFunction>());

  JSObject* resultObject = js::LambdaArrow(cx, fun, scopeChain, newTarget);
  if (!resultObject) {
    return false;
  }

  RootedValue result(cx);
  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MFunctionWithProto::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_FunctionWithProto));
  return true;
}

RFunctionWithProto::RFunctionWithProto(CompactBufferReader& reader) {}

bool RFunctionWithProto::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject scopeChain(cx, &iter.read().toObject());
  RootedObject prototype(cx, &iter.read().toObject());
  RootedFunction fun(cx, &iter.read().toObject().as<JSFunction>());

  JSObject* resultObject =
      js::FunWithProtoOperation(cx, fun, scopeChain, prototype);
  if (!resultObject) {
    return false;
  }

  RootedValue result(cx);
  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MNewCallObject::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_NewCallObject));
  return true;
}

RNewCallObject::RNewCallObject(CompactBufferReader& reader) {}

bool RNewCallObject::recover(JSContext* cx, SnapshotIterator& iter) const {
  Rooted<CallObject*> templateObj(cx, &iter.read().toObject().as<CallObject>());

  RootedShape shape(cx, templateObj->shape());

  JSObject* resultObject = NewCallObject(cx, shape);
  if (!resultObject) {
    return false;
  }

  RootedValue result(cx);
  result.setObject(*resultObject);
  iter.storeInstructionResult(result);
  return true;
}

bool MObjectState::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_ObjectState));
  writer.writeUnsigned(numSlots());
  return true;
}

RObjectState::RObjectState(CompactBufferReader& reader) {
  numSlots_ = reader.readUnsigned();
}

bool RObjectState::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedObject object(cx, &iter.read().toObject());
  RootedValue val(cx);
  RootedNativeObject nativeObject(cx, &object->as<NativeObject>());
  MOZ_ASSERT(nativeObject->slotSpan() == numSlots());

  for (size_t i = 0; i < numSlots(); i++) {
    val = iter.read();
    nativeObject->setSlot(i, val);
  }

  val.setObject(*object);
  iter.storeInstructionResult(val);
  return true;
}

bool MArrayState::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_ArrayState));
  writer.writeUnsigned(numElements());
  return true;
}

RArrayState::RArrayState(CompactBufferReader& reader) {
  numElements_ = reader.readUnsigned();
}

bool RArrayState::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue result(cx);
  ArrayObject* object = &iter.read().toObject().as<ArrayObject>();
  uint32_t initLength = iter.read().toInt32();

  MOZ_ASSERT(object->getDenseInitializedLength() == 0,
             "initDenseElement call below relies on this");
  object->setDenseInitializedLength(initLength);

  for (size_t index = 0; index < numElements(); index++) {
    Value val = iter.read();

    if (index >= initLength) {
      MOZ_ASSERT(val.isUndefined());
      continue;
    }

    object->initDenseElement(index, val);
  }

  result.setObject(*object);
  iter.storeInstructionResult(result);
  return true;
}

bool MSetArrayLength::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  // For simplicity, we capture directly the object instead of the elements
  // pointer.
  MOZ_ASSERT(elements()->type() != MIRType::Elements);
  writer.writeUnsigned(uint32_t(RInstruction::Recover_SetArrayLength));
  return true;
}

bool MSetArrayLength::canRecoverOnBailout() const {
  return isRecoveredOnBailout();
}

RSetArrayLength::RSetArrayLength(CompactBufferReader& reader) {}

bool RSetArrayLength::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue result(cx);
  RootedArrayObject obj(cx, &iter.read().toObject().as<ArrayObject>());
  RootedValue len(cx, iter.read());

  RootedId id(cx, NameToId(cx->names().length));
  Rooted<PropertyDescriptor> desc(
      cx, PropertyDescriptor::Data(len, JS::PropertyAttribute::Writable));
  ObjectOpResult error;
  if (!ArraySetLength(cx, obj, id, desc, error)) {
    return false;
  }

  result.setObject(*obj);
  iter.storeInstructionResult(result);
  return true;
}

bool MAssertRecoveredOnBailout::writeRecoverData(
    CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  MOZ_RELEASE_ASSERT(input()->isRecoveredOnBailout() == mustBeRecovered_,
                     "assertRecoveredOnBailout failed during compilation");
  writer.writeUnsigned(
      uint32_t(RInstruction::Recover_AssertRecoveredOnBailout));
  return true;
}

RAssertRecoveredOnBailout::RAssertRecoveredOnBailout(
    CompactBufferReader& reader) {}

bool RAssertRecoveredOnBailout::recover(JSContext* cx,
                                        SnapshotIterator& iter) const {
  RootedValue result(cx);
  iter.read();  // skip the unused operand.
  result.setUndefined();
  iter.storeInstructionResult(result);
  return true;
}

bool MStringReplace::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_StringReplace));
  writer.writeByte(isFlatReplacement_);
  return true;
}

RStringReplace::RStringReplace(CompactBufferReader& reader) {
  isFlatReplacement_ = reader.readByte();
}

bool RStringReplace::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedString string(cx, iter.read().toString());
  RootedString pattern(cx, iter.read().toString());
  RootedString replace(cx, iter.read().toString());

  JSString* result =
      isFlatReplacement_
          ? js::StringFlatReplaceString(cx, string, pattern, replace)
          : js::str_replace_string_raw(cx, string, pattern, replace);

  if (!result) {
    return false;
  }

  iter.storeInstructionResult(StringValue(result));
  return true;
}

bool MAtomicIsLockFree::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_AtomicIsLockFree));
  return true;
}

RAtomicIsLockFree::RAtomicIsLockFree(CompactBufferReader& reader) {}

bool RAtomicIsLockFree::recover(JSContext* cx, SnapshotIterator& iter) const {
  RootedValue operand(cx, iter.read());
  MOZ_ASSERT(operand.isInt32());

  int32_t result;
  if (!js::AtomicIsLockFree(cx, operand, &result)) {
    return false;
  }

  RootedValue rootedResult(cx, js::Int32Value(result));
  iter.storeInstructionResult(rootedResult);
  return true;
}

bool MBigIntAsIntN::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntAsIntN));
  return true;
}

RBigIntAsIntN::RBigIntAsIntN(CompactBufferReader& reader) {}

bool RBigIntAsIntN::recover(JSContext* cx, SnapshotIterator& iter) const {
  int32_t bits = iter.read().toInt32();
  RootedBigInt input(cx, iter.read().toBigInt());

  MOZ_ASSERT(bits >= 0);
  BigInt* result = BigInt::asIntN(cx, input, bits);
  if (!result) {
    return false;
  }

  iter.storeInstructionResult(JS::BigIntValue(result));
  return true;
}

bool MBigIntAsUintN::writeRecoverData(CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_BigIntAsUintN));
  return true;
}

RBigIntAsUintN::RBigIntAsUintN(CompactBufferReader& reader) {}

bool RBigIntAsUintN::recover(JSContext* cx, SnapshotIterator& iter) const {
  int32_t bits = iter.read().toInt32();
  RootedBigInt input(cx, iter.read().toBigInt());

  MOZ_ASSERT(bits >= 0);
  BigInt* result = BigInt::asUintN(cx, input, bits);
  if (!result) {
    return false;
  }

  iter.storeInstructionResult(JS::BigIntValue(result));
  return true;
}

bool MCreateArgumentsObject::writeRecoverData(
    CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(uint32_t(RInstruction::Recover_CreateArgumentsObject));
  return true;
}

RCreateArgumentsObject::RCreateArgumentsObject(CompactBufferReader& reader) {}

bool RCreateArgumentsObject::recover(JSContext* cx,
                                     SnapshotIterator& iter) const {
  RootedObject callObject(cx, &iter.read().toObject());
  RootedObject result(
      cx, ArgumentsObject::createForIon(cx, iter.frame(), callObject));
  if (!result) {
    return false;
  }

  iter.storeInstructionResult(JS::ObjectValue(*result));
  return true;
}

bool MCreateInlinedArgumentsObject::writeRecoverData(
    CompactBufferWriter& writer) const {
  MOZ_ASSERT(canRecoverOnBailout());
  writer.writeUnsigned(
      uint32_t(RInstruction::Recover_CreateInlinedArgumentsObject));
  writer.writeUnsigned(numActuals());
  return true;
}

RCreateInlinedArgumentsObject::RCreateInlinedArgumentsObject(
    CompactBufferReader& reader) {
  numActuals_ = reader.readUnsigned();
}

bool RCreateInlinedArgumentsObject::recover(JSContext* cx,
                                            SnapshotIterator& iter) const {
  RootedObject callObject(cx, &iter.read().toObject());
  RootedFunction callee(cx, &iter.read().toObject().as<JSFunction>());

  JS::RootedValueArray<ArgumentsObject::MaxInlinedArgs> argsArray(cx);
  for (uint32_t i = 0; i < numActuals_; i++) {
    argsArray[i].set(iter.read());
  }

  RootedObject result(cx, ArgumentsObject::createFromValueArray(
                              cx, argsArray, callee, callObject, numActuals_));
  if (!result) {
    return false;
  }

  iter.storeInstructionResult(JS::ObjectValue(*result));
  return true;
}
