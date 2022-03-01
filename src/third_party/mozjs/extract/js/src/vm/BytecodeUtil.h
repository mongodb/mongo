/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BytecodeUtil_h
#define vm_BytecodeUtil_h

/*
 * JS bytecode definitions.
 */

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/EndianUtils.h"

#include <algorithm>
#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"
#include "NamespaceImports.h"

#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "vm/BytecodeFormatFlags.h"  // JOF_*
#include "vm/GeneratorResumeKind.h"
#include "vm/Opcodes.h"
#include "vm/SharedStencil.h"  // js::GCThingIndex
#include "vm/ThrowMsgKind.h"   // ThrowMsgKind, ThrowCondition

namespace js {
class Sprinter;
}  // namespace js

/*
 * JS operation bytecodes.
 */
enum class JSOp : uint8_t {
#define ENUMERATE_OPCODE(op, ...) op,
  FOR_EACH_OPCODE(ENUMERATE_OPCODE)
#undef ENUMERATE_OPCODE
};

/* Shorthand for type from format. */

static inline uint32_t JOF_TYPE(uint32_t fmt) { return fmt & JOF_TYPEMASK; }

/* Shorthand for mode from format. */

static inline uint32_t JOF_MODE(uint32_t fmt) { return fmt & JOF_MODEMASK; }

/*
 * Immediate operand getters, setters, and bounds.
 */

static MOZ_ALWAYS_INLINE uint8_t GET_UINT8(jsbytecode* pc) {
  return uint8_t(pc[1]);
}

static MOZ_ALWAYS_INLINE void SET_UINT8(jsbytecode* pc, uint8_t u) {
  pc[1] = jsbytecode(u);
}

/* Common uint16_t immediate format helpers. */

static inline jsbytecode UINT16_HI(uint16_t i) { return jsbytecode(i >> 8); }

static inline jsbytecode UINT16_LO(uint16_t i) { return jsbytecode(i); }

static MOZ_ALWAYS_INLINE uint16_t GET_UINT16(const jsbytecode* pc) {
  uint16_t result;
  mozilla::NativeEndian::copyAndSwapFromLittleEndian(&result, pc + 1, 1);
  return result;
}

static MOZ_ALWAYS_INLINE void SET_UINT16(jsbytecode* pc, uint16_t i) {
  mozilla::NativeEndian::copyAndSwapToLittleEndian(pc + 1, &i, 1);
}

static const unsigned UINT16_LIMIT = 1 << 16;

/* Helpers for accessing the offsets of jump opcodes. */
static const unsigned JUMP_OFFSET_LEN = 4;
static const int32_t JUMP_OFFSET_MIN = INT32_MIN;
static const int32_t JUMP_OFFSET_MAX = INT32_MAX;

static MOZ_ALWAYS_INLINE uint32_t GET_UINT24(const jsbytecode* pc) {
#if MOZ_LITTLE_ENDIAN()
  // Do a single 32-bit load (for opcode and operand), then shift off the
  // opcode.
  uint32_t result;
  memcpy(&result, pc, 4);
  return result >> 8;
#else
  return uint32_t((pc[3] << 16) | (pc[2] << 8) | pc[1]);
#endif
}

static MOZ_ALWAYS_INLINE void SET_UINT24(jsbytecode* pc, uint32_t i) {
  MOZ_ASSERT(i < (1 << 24));

#if MOZ_LITTLE_ENDIAN()
  memcpy(pc + 1, &i, 3);
#else
  pc[1] = jsbytecode(i);
  pc[2] = jsbytecode(i >> 8);
  pc[3] = jsbytecode(i >> 16);
#endif
}

static MOZ_ALWAYS_INLINE int8_t GET_INT8(const jsbytecode* pc) {
  return int8_t(pc[1]);
}

static MOZ_ALWAYS_INLINE uint32_t GET_UINT32(const jsbytecode* pc) {
  uint32_t result;
  mozilla::NativeEndian::copyAndSwapFromLittleEndian(&result, pc + 1, 1);
  return result;
}

static MOZ_ALWAYS_INLINE void SET_UINT32(jsbytecode* pc, uint32_t u) {
  mozilla::NativeEndian::copyAndSwapToLittleEndian(pc + 1, &u, 1);
}

static MOZ_ALWAYS_INLINE JS::Value GET_INLINE_VALUE(const jsbytecode* pc) {
  uint64_t raw;
  mozilla::NativeEndian::copyAndSwapFromLittleEndian(&raw, pc + 1, 1);
  return JS::Value::fromRawBits(raw);
}

static MOZ_ALWAYS_INLINE void SET_INLINE_VALUE(jsbytecode* pc,
                                               const JS::Value& v) {
  uint64_t raw = v.asRawBits();
  mozilla::NativeEndian::copyAndSwapToLittleEndian(pc + 1, &raw, 1);
}

static MOZ_ALWAYS_INLINE int32_t GET_INT32(const jsbytecode* pc) {
  return static_cast<int32_t>(GET_UINT32(pc));
}

static MOZ_ALWAYS_INLINE void SET_INT32(jsbytecode* pc, int32_t i) {
  SET_UINT32(pc, static_cast<uint32_t>(i));
}

static MOZ_ALWAYS_INLINE int32_t GET_JUMP_OFFSET(jsbytecode* pc) {
  return GET_INT32(pc);
}

static MOZ_ALWAYS_INLINE void SET_JUMP_OFFSET(jsbytecode* pc, int32_t off) {
  SET_INT32(pc, off);
}

static const unsigned GCTHING_INDEX_LEN = 4;

static MOZ_ALWAYS_INLINE js::GCThingIndex GET_GCTHING_INDEX(
    const jsbytecode* pc) {
  return js::GCThingIndex(GET_UINT32(pc));
}

static MOZ_ALWAYS_INLINE void SET_GCTHING_INDEX(jsbytecode* pc,
                                                js::GCThingIndex index) {
  SET_UINT32(pc, index.index);
}

// Index limit is determined by SrcNote::FourByteOffsetFlag, see
// frontend/BytecodeEmitter.h.
static const unsigned INDEX_LIMIT_LOG2 = 31;
static const uint32_t INDEX_LIMIT = uint32_t(1) << INDEX_LIMIT_LOG2;

static inline jsbytecode ARGC_HI(uint16_t argc) { return UINT16_HI(argc); }

static inline jsbytecode ARGC_LO(uint16_t argc) { return UINT16_LO(argc); }

static inline uint16_t GET_ARGC(const jsbytecode* pc) { return GET_UINT16(pc); }

static const unsigned ARGC_LIMIT = UINT16_LIMIT;

static inline uint16_t GET_ARGNO(const jsbytecode* pc) {
  return GET_UINT16(pc);
}

static inline void SET_ARGNO(jsbytecode* pc, uint16_t argno) {
  SET_UINT16(pc, argno);
}

static const unsigned ARGNO_LEN = 2;
static const unsigned ARGNO_LIMIT = UINT16_LIMIT;

static inline uint32_t GET_LOCALNO(const jsbytecode* pc) {
  return GET_UINT24(pc);
}

static inline void SET_LOCALNO(jsbytecode* pc, uint32_t varno) {
  SET_UINT24(pc, varno);
}

static const unsigned LOCALNO_LEN = 3;
static const unsigned LOCALNO_BITS = 24;
static const uint32_t LOCALNO_LIMIT = 1 << LOCALNO_BITS;

static inline uint32_t GET_RESUMEINDEX(const jsbytecode* pc) {
  return GET_UINT24(pc);
}

static inline void SET_RESUMEINDEX(jsbytecode* pc, uint32_t resumeIndex) {
  SET_UINT24(pc, resumeIndex);
}

static const unsigned ICINDEX_LEN = 4;

static inline uint32_t GET_ICINDEX(const jsbytecode* pc) {
  return GET_UINT32(pc);
}

static inline void SET_ICINDEX(jsbytecode* pc, uint32_t icIndex) {
  SET_UINT32(pc, icIndex);
}

static inline unsigned LoopHeadDepthHint(jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::LoopHead);
  return GET_UINT8(pc + 4);
}

static inline void SetLoopHeadDepthHint(jsbytecode* pc, unsigned loopDepth) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::LoopHead);
  uint8_t data = std::min(loopDepth, unsigned(UINT8_MAX));
  SET_UINT8(pc + 4, data);
}

static inline bool IsBackedgePC(jsbytecode* pc) {
  switch (JSOp(*pc)) {
    case JSOp::Goto:
    case JSOp::JumpIfTrue:
      return GET_JUMP_OFFSET(pc) < 0;
    default:
      return false;
  }
}

static inline bool IsBackedgeForLoopHead(jsbytecode* pc, jsbytecode* loopHead) {
  MOZ_ASSERT(JSOp(*loopHead) == JSOp::LoopHead);
  return IsBackedgePC(pc) && pc + GET_JUMP_OFFSET(pc) == loopHead;
}

/*
 * Describes the 'hops' component of a JOF_ENVCOORD opcode.
 *
 * Note: this component is only 8 bits wide, limiting the maximum number of
 * scopes between a use and def to roughly 255. This is a pretty small limit but
 * note that SpiderMonkey's recursive descent parser can only parse about this
 * many functions before hitting the C-stack recursion limit so this shouldn't
 * be a significant limitation in practice.
 */

static inline uint8_t GET_ENVCOORD_HOPS(jsbytecode* pc) {
  return GET_UINT8(pc);
}

static inline void SET_ENVCOORD_HOPS(jsbytecode* pc, uint8_t hops) {
  SET_UINT8(pc, hops);
}

static const unsigned ENVCOORD_HOPS_LEN = 1;
static const unsigned ENVCOORD_HOPS_BITS = 8;
static const unsigned ENVCOORD_HOPS_LIMIT = 1 << ENVCOORD_HOPS_BITS;

/* Describes the 'slot' component of a JOF_ENVCOORD opcode. */
static inline uint32_t GET_ENVCOORD_SLOT(const jsbytecode* pc) {
  return GET_UINT24(pc);
}

static inline void SET_ENVCOORD_SLOT(jsbytecode* pc, uint32_t slot) {
  SET_UINT24(pc, slot);
}

static const unsigned ENVCOORD_SLOT_LEN = 3;
static const unsigned ENVCOORD_SLOT_BITS = 24;
static const uint32_t ENVCOORD_SLOT_LIMIT = 1 << ENVCOORD_SLOT_BITS;

struct JSCodeSpec {
  uint8_t length;  /* length including opcode byte */
  int8_t nuses;    /* arity, -1 if variadic */
  int8_t ndefs;    /* number of stack results */
  uint32_t format; /* immediate operand format */
};

namespace js {

extern const JSCodeSpec CodeSpecTable[];

inline const JSCodeSpec& CodeSpec(JSOp op) {
  return CodeSpecTable[uint8_t(op)];
}

extern const char* const CodeNameTable[];

inline const char* CodeName(JSOp op) { return CodeNameTable[uint8_t(op)]; }

/* Shorthand for type from opcode. */

static inline uint32_t JOF_OPTYPE(JSOp op) {
  return JOF_TYPE(CodeSpec(op).format);
}

static inline bool IsJumpOpcode(JSOp op) { return JOF_OPTYPE(op) == JOF_JUMP; }

static inline bool BytecodeFallsThrough(JSOp op) {
  // Note:
  // * JSOp::Yield/JSOp::Await is considered to fall through, like JSOp::Call.
  // * JSOp::Gosub falls through indirectly, after executing a 'finally'.
  switch (op) {
    case JSOp::Goto:
    case JSOp::Default:
    case JSOp::Return:
    case JSOp::RetRval:
    case JSOp::Retsub:
    case JSOp::FinalYieldRval:
    case JSOp::Throw:
    case JSOp::ThrowMsg:
    case JSOp::ThrowSetConst:
    case JSOp::TableSwitch:
      return false;
    default:
      return true;
  }
}

static inline bool BytecodeIsJumpTarget(JSOp op) {
  switch (op) {
    case JSOp::JumpTarget:
    case JSOp::LoopHead:
    case JSOp::AfterYield:
      return true;
    default:
      return false;
  }
}

MOZ_ALWAYS_INLINE unsigned StackUses(jsbytecode* pc) {
  JSOp op = JSOp(*pc);
  int nuses = CodeSpec(op).nuses;
  if (nuses >= 0) {
    return nuses;
  }

  MOZ_ASSERT(nuses == -1);
  switch (op) {
    case JSOp::PopN:
      return GET_UINT16(pc);
    case JSOp::New:
    case JSOp::SuperCall:
      return 2 + GET_ARGC(pc) + 1;
    default:
      /* stack: fun, this, [argc arguments] */
      MOZ_ASSERT(op == JSOp::Call || op == JSOp::CallIgnoresRv ||
                 op == JSOp::Eval || op == JSOp::CallIter ||
                 op == JSOp::StrictEval || op == JSOp::FunCall ||
                 op == JSOp::FunApply);
      return 2 + GET_ARGC(pc);
  }
}

MOZ_ALWAYS_INLINE unsigned StackDefs(jsbytecode* pc) {
  int ndefs = CodeSpec(JSOp(*pc)).ndefs;
  MOZ_ASSERT(ndefs >= 0);
  return ndefs;
}

#if defined(DEBUG) || defined(JS_JITSPEW)
/*
 * Given bytecode address pc in script's main program code, compute the operand
 * stack depth just before (JSOp) *pc executes.  If *pc is not reachable, return
 * false.
 */
extern bool ReconstructStackDepth(JSContext* cx, JSScript* script,
                                  jsbytecode* pc, uint32_t* depth,
                                  bool* reachablePC);
#endif

} /* namespace js */

#define JSDVG_IGNORE_STACK 0
#define JSDVG_SEARCH_STACK 1

namespace js {

/*
 * Find the source expression that resulted in v, and return a newly allocated
 * C-string containing it.  Fall back on v's string conversion (fallback) if we
 * can't find the bytecode that generated and pushed v on the operand stack.
 *
 * Search the current stack frame if spindex is JSDVG_SEARCH_STACK.  Don't
 * look for v on the stack if spindex is JSDVG_IGNORE_STACK.  Otherwise,
 * spindex is the negative index of v, measured from cx->fp->sp, or from a
 * lower frame's sp if cx->fp is native.
 *
 * The optional argument skipStackHits can be used to skip a hit in the stack
 * frame. This can be useful in self-hosted code that wants to report value
 * errors containing decompiled values that are useful for the user, instead of
 * values used internally by the self-hosted code.
 *
 * The caller must call JS_free on the result after a successful call.
 */
UniqueChars DecompileValueGenerator(JSContext* cx, int spindex, HandleValue v,
                                    HandleString fallback,
                                    int skipStackHits = 0);

/*
 * Decompile the formal argument at formalIndex in the nearest non-builtin
 * stack frame, falling back with converting v to source.
 */
JSString* DecompileArgument(JSContext* cx, int formalIndex, HandleValue v);

static inline unsigned GetOpLength(JSOp op) {
  MOZ_ASSERT(uint8_t(op) < JSOP_LIMIT);
  MOZ_ASSERT(CodeSpec(op).length > 0);
  return CodeSpec(op).length;
}

static inline unsigned GetBytecodeLength(const jsbytecode* pc) {
  JSOp op = (JSOp)*pc;
  return GetOpLength(op);
}

static inline bool BytecodeIsPopped(jsbytecode* pc) {
  jsbytecode* next = pc + GetBytecodeLength(pc);
  return JSOp(*next) == JSOp::Pop;
}

extern bool IsValidBytecodeOffset(JSContext* cx, JSScript* script,
                                  size_t offset);

inline bool IsArgOp(JSOp op) { return JOF_OPTYPE(op) == JOF_QARG; }

inline bool IsLocalOp(JSOp op) { return JOF_OPTYPE(op) == JOF_LOCAL; }

inline bool IsAliasedVarOp(JSOp op) { return JOF_OPTYPE(op) == JOF_ENVCOORD; }

inline bool IsGlobalOp(JSOp op) { return CodeSpec(op).format & JOF_GNAME; }

inline bool IsPropertySetOp(JSOp op) {
  return CodeSpec(op).format & JOF_PROPSET;
}

inline bool IsPropertyInitOp(JSOp op) {
  return CodeSpec(op).format & JOF_PROPINIT;
}

inline bool IsLooseEqualityOp(JSOp op) {
  return op == JSOp::Eq || op == JSOp::Ne;
}

inline bool IsStrictEqualityOp(JSOp op) {
  return op == JSOp::StrictEq || op == JSOp::StrictNe;
}

inline bool IsEqualityOp(JSOp op) {
  return IsLooseEqualityOp(op) || IsStrictEqualityOp(op);
}

inline bool IsRelationalOp(JSOp op) {
  return op == JSOp::Lt || op == JSOp::Le || op == JSOp::Gt || op == JSOp::Ge;
}

inline bool IsCheckStrictOp(JSOp op) {
  return CodeSpec(op).format & JOF_CHECKSTRICT;
}

inline bool IsNameOp(JSOp op) { return CodeSpec(op).format & JOF_NAME; }

#ifdef DEBUG
inline bool IsCheckSloppyOp(JSOp op) {
  return CodeSpec(op).format & JOF_CHECKSLOPPY;
}
#endif

inline bool IsAtomOp(JSOp op) { return JOF_OPTYPE(op) == JOF_ATOM; }

inline bool IsGetPropOp(JSOp op) { return op == JSOp::GetProp; }

inline bool IsGetPropPC(const jsbytecode* pc) { return IsGetPropOp(JSOp(*pc)); }

inline bool IsHiddenInitOp(JSOp op) {
  return op == JSOp::InitHiddenProp || op == JSOp::InitHiddenElem ||
         op == JSOp::InitHiddenPropGetter || op == JSOp::InitHiddenElemGetter ||
         op == JSOp::InitHiddenPropSetter || op == JSOp::InitHiddenElemSetter ||
         op == JSOp::InitLockedElem;
}

inline bool IsStrictSetPC(jsbytecode* pc) {
  JSOp op = JSOp(*pc);
  return op == JSOp::StrictSetProp || op == JSOp::StrictSetName ||
         op == JSOp::StrictSetGName || op == JSOp::StrictSetElem;
}

inline bool IsSetPropOp(JSOp op) {
  return op == JSOp::SetProp || op == JSOp::StrictSetProp ||
         op == JSOp::SetName || op == JSOp::StrictSetName ||
         op == JSOp::SetGName || op == JSOp::StrictSetGName;
}

inline bool IsSetPropPC(const jsbytecode* pc) { return IsSetPropOp(JSOp(*pc)); }

inline bool IsGetElemOp(JSOp op) { return op == JSOp::GetElem; }

inline bool IsGetElemPC(const jsbytecode* pc) { return IsGetElemOp(JSOp(*pc)); }

inline bool IsSetElemOp(JSOp op) {
  return op == JSOp::SetElem || op == JSOp::StrictSetElem;
}

inline bool IsSetElemPC(const jsbytecode* pc) { return IsSetElemOp(JSOp(*pc)); }

inline bool IsElemPC(const jsbytecode* pc) {
  return CodeSpec(JSOp(*pc)).format & JOF_ELEM;
}

inline bool IsInvokeOp(JSOp op) { return CodeSpec(op).format & JOF_INVOKE; }

inline bool IsInvokePC(jsbytecode* pc) { return IsInvokeOp(JSOp(*pc)); }

inline bool IsStrictEvalPC(jsbytecode* pc) {
  JSOp op = JSOp(*pc);
  return op == JSOp::StrictEval || op == JSOp::StrictSpreadEval;
}

inline bool IsConstructOp(JSOp op) {
  return CodeSpec(op).format & JOF_CONSTRUCT;
}
inline bool IsConstructPC(const jsbytecode* pc) {
  return IsConstructOp(JSOp(*pc));
}

inline bool IsSpreadOp(JSOp op) { return CodeSpec(op).format & JOF_SPREAD; }

inline bool IsSpreadPC(const jsbytecode* pc) { return IsSpreadOp(JSOp(*pc)); }

static inline int32_t GetBytecodeInteger(jsbytecode* pc) {
  switch (JSOp(*pc)) {
    case JSOp::Zero:
      return 0;
    case JSOp::One:
      return 1;
    case JSOp::Uint16:
      return GET_UINT16(pc);
    case JSOp::Uint24:
      return GET_UINT24(pc);
    case JSOp::Int8:
      return GET_INT8(pc);
    case JSOp::Int32:
      return GET_INT32(pc);
    default:
      MOZ_CRASH("Bad op");
  }
}

inline bool BytecodeOpHasIC(JSOp op) { return CodeSpec(op).format & JOF_IC; }

inline void GetCheckPrivateFieldOperands(jsbytecode* pc,
                                         ThrowCondition* throwCondition,
                                         ThrowMsgKind* throwKind) {
  static_assert(sizeof(ThrowCondition) == sizeof(uint8_t));
  static_assert(sizeof(ThrowMsgKind) == sizeof(uint8_t));

  MOZ_ASSERT(JSOp(*pc) == JSOp::CheckPrivateField);
  uint8_t throwConditionByte = GET_UINT8(pc);
  uint8_t throwKindByte = GET_UINT8(pc + 1);

  *throwCondition = static_cast<ThrowCondition>(throwConditionByte);
  *throwKind = static_cast<ThrowMsgKind>(throwKindByte);

  MOZ_ASSERT(*throwCondition == ThrowCondition::ThrowHas ||
             *throwCondition == ThrowCondition::ThrowHasNot ||
             *throwCondition == ThrowCondition::OnlyCheckRhs);

  MOZ_ASSERT(*throwKind == ThrowMsgKind::PrivateDoubleInit ||
             *throwKind == ThrowMsgKind::MissingPrivateOnGet ||
             *throwKind == ThrowMsgKind::MissingPrivateOnSet);
}

// Return true iff the combination of the ThrowCondition and hasOwn result
// will throw an exception.
static inline bool CheckPrivateFieldWillThrow(ThrowCondition condition,
                                              bool hasOwn) {
  if ((condition == ThrowCondition::ThrowHasNot && !hasOwn) ||
      (condition == ThrowCondition::ThrowHas && hasOwn)) {
    // Met a throw condition.
    return true;
  }

  return false;
}

/*
 * Counts accumulated for a single opcode in a script. The counts tracked vary
 * between opcodes, and this structure ensures that counts are accessed in a
 * coherent fashion.
 */
class PCCounts {
  /*
   * Offset of the pc inside the script. This fields is used to lookup opcode
   * which have annotations.
   */
  size_t pcOffset_;

  /*
   * Record the number of execution of one instruction, or the number of
   * throws executed.
   */
  uint64_t numExec_;

 public:
  explicit PCCounts(size_t off) : pcOffset_(off), numExec_(0) {}

  size_t pcOffset() const { return pcOffset_; }

  // Used for sorting and searching.
  bool operator<(const PCCounts& rhs) const {
    return pcOffset_ < rhs.pcOffset_;
  }

  uint64_t& numExec() { return numExec_; }
  uint64_t numExec() const { return numExec_; }

  static const char numExecName[];
};

static inline jsbytecode* GetNextPc(jsbytecode* pc) {
  return pc + GetBytecodeLength(pc);
}

inline GeneratorResumeKind IntToResumeKind(int32_t value) {
  MOZ_ASSERT(uint32_t(value) <= uint32_t(GeneratorResumeKind::Return));
  return static_cast<GeneratorResumeKind>(value);
}

inline GeneratorResumeKind ResumeKindFromPC(jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::ResumeKind);
  return IntToResumeKind(GET_UINT8(pc));
}

#if defined(DEBUG) || defined(JS_JITSPEW)

enum class DisassembleSkeptically { No, Yes };

/*
 * Disassemblers, for debugging only.
 */
[[nodiscard]] extern bool Disassemble(
    JSContext* cx, JS::Handle<JSScript*> script, bool lines, Sprinter* sp,
    DisassembleSkeptically skeptically = DisassembleSkeptically::No);

unsigned Disassemble1(JSContext* cx, JS::Handle<JSScript*> script,
                      jsbytecode* pc, unsigned loc, bool lines, Sprinter* sp);

#endif

[[nodiscard]] extern bool DumpRealmPCCounts(JSContext* cx);

}  // namespace js

#endif /* vm_BytecodeUtil_h */
