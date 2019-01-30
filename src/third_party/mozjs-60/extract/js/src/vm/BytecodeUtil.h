/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BytecodeUtil_h
#define vm_BytecodeUtil_h

/*
 * JS bytecode definitions.
 */

#include "mozilla/Attributes.h"
#include "mozilla/EndianUtils.h"

#include "jstypes.h"
#include "NamespaceImports.h"

#include "frontend/SourceNotes.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "vm/Opcodes.h"
#include "vm/Printer.h"

/*
 * JS operation bytecodes.
 */
typedef enum JSOp {
#define ENUMERATE_OPCODE(op, val, ...) op = val,
FOR_EACH_OPCODE(ENUMERATE_OPCODE)
#undef ENUMERATE_OPCODE

    JSOP_LIMIT
} JSOp;

/*
 * JS bytecode formats.
 */
enum {
    JOF_BYTE            = 0,        /* single bytecode, no immediates */
    JOF_JUMP            = 1,        /* signed 16-bit jump offset immediate */
    JOF_ATOM            = 2,        /* unsigned 16-bit constant index */
    JOF_UINT16          = 3,        /* unsigned 16-bit immediate operand */
    JOF_TABLESWITCH     = 4,        /* table switch */
    /* 5 is unused */
    JOF_QARG            = 6,        /* quickened get/set function argument ops */
    JOF_LOCAL           = 7,        /* var or block-local variable */
    JOF_DOUBLE          = 8,        /* uint32_t index for double value */
    JOF_UINT24          = 12,       /* extended unsigned 24-bit literal (index) */
    JOF_UINT8           = 13,       /* uint8_t immediate, e.g. top 8 bits of 24-bit
                                       atom index */
    JOF_INT32           = 14,       /* int32_t immediate operand */
    JOF_UINT32          = 15,       /* uint32_t immediate operand */
    JOF_OBJECT          = 16,       /* unsigned 32-bit object index */
    JOF_REGEXP          = 17,       /* unsigned 32-bit regexp index */
    JOF_INT8            = 18,       /* int8_t immediate operand */
    JOF_ATOMOBJECT      = 19,       /* uint16_t constant index + object index */
    JOF_SCOPE           = 20,       /* unsigned 32-bit scope index */
    JOF_ENVCOORD        = 21,       /* embedded ScopeCoordinate immediate */
    JOF_TYPEMASK        = 0x001f,   /* mask for above immediate types */

    JOF_NAME            = 1 << 5,   /* name operation */
    JOF_PROP            = 2 << 5,   /* obj.prop operation */
    JOF_ELEM            = 3 << 5,   /* obj[index] operation */
    JOF_MODEMASK        = 3 << 5,   /* mask for above addressing modes */
    JOF_PROPSET         = 1 << 7,   /* property/element/name set operation */
    JOF_PROPINIT        = 1 << 8,   /* property/element/name init operation */
    /* 1 << 9 is unused */
    /* 1 << 10 is unused */
    /* 1 << 11 is unused */
    /* 1 << 12 is unused */
    /* 1 << 13 is unused */
    JOF_DETECTING       = 1 << 14,  /* object detection for warning-quelling */
    /* 1 << 15 is unused */
    JOF_LEFTASSOC       = 1 << 16,  /* left-associative operator */
    /* 1 << 17 is unused */
    /* 1 << 18 is unused */
    JOF_CHECKSLOPPY     = 1 << 19,  /* Op can only be generated in sloppy mode */
    JOF_CHECKSTRICT     = 1 << 20,  /* Op can only be generated in strict mode */
    JOF_INVOKE          = 1 << 21,  /* JSOP_CALL, JSOP_FUNCALL, JSOP_FUNAPPLY,
                                       JSOP_NEW, JSOP_EVAL, JSOP_CALLITER */
    /* 1 << 22 is unused */
    /* 1 << 23 is unused */
    /* 1 << 24 is unused */
    JOF_GNAME           = 1 << 25,  /* predicted global name */
    JOF_TYPESET         = 1 << 26,  /* has an entry in a script's type sets */
    JOF_ARITH           = 1 << 27   /* unary or binary arithmetic opcode */
};

/* Shorthand for type from format. */

static inline uint32_t
JOF_TYPE(uint32_t fmt)
{
    return fmt & JOF_TYPEMASK;
}

/* Shorthand for mode from format. */

static inline uint32_t
JOF_MODE(uint32_t fmt)
{
    return fmt & JOF_MODEMASK;
}

/*
 * Immediate operand getters, setters, and bounds.
 */

static MOZ_ALWAYS_INLINE uint8_t
GET_UINT8(jsbytecode* pc)
{
    return uint8_t(pc[1]);
}

static MOZ_ALWAYS_INLINE void
SET_UINT8(jsbytecode* pc, uint8_t u)
{
    pc[1] = jsbytecode(u);
}

/* Common uint16_t immediate format helpers. */

static inline jsbytecode
UINT16_HI(uint16_t i)
{
    return jsbytecode(i >> 8);
}

static inline jsbytecode
UINT16_LO(uint16_t i)
{
    return jsbytecode(i);
}

static MOZ_ALWAYS_INLINE uint16_t
GET_UINT16(const jsbytecode* pc)
{
#if MOZ_LITTLE_ENDIAN
    uint16_t result;
    memcpy(&result, pc + 1, sizeof(result));
    return result;
#else
    return uint16_t((pc[2] << 8) | pc[1]);
#endif
}

static MOZ_ALWAYS_INLINE void
SET_UINT16(jsbytecode* pc, uint16_t i)
{
#if MOZ_LITTLE_ENDIAN
    memcpy(pc + 1, &i, sizeof(i));
#else
    pc[1] = UINT16_LO(i);
    pc[2] = UINT16_HI(i);
#endif
}

static const unsigned UINT16_LIMIT      = 1 << 16;

/* Helpers for accessing the offsets of jump opcodes. */
static const unsigned JUMP_OFFSET_LEN   = 4;
static const int32_t JUMP_OFFSET_MIN    = INT32_MIN;
static const int32_t JUMP_OFFSET_MAX    = INT32_MAX;

static MOZ_ALWAYS_INLINE uint32_t
GET_UINT24(const jsbytecode* pc)
{
#if MOZ_LITTLE_ENDIAN
    // Do a single 32-bit load (for opcode and operand), then shift off the
    // opcode.
    uint32_t result;
    memcpy(&result, pc, 4);
    return result >> 8;
#else
    return unsigned((pc[3] << 16) | (pc[2] << 8) | pc[1]);
#endif
}

static MOZ_ALWAYS_INLINE void
SET_UINT24(jsbytecode* pc, uint32_t i)
{
    MOZ_ASSERT(i < (1 << 24));

#if MOZ_LITTLE_ENDIAN
    memcpy(pc + 1, &i, 3);
#else
    pc[1] = jsbytecode(i);
    pc[2] = jsbytecode(i >> 8);
    pc[3] = jsbytecode(i >> 16);
#endif
}

static MOZ_ALWAYS_INLINE int8_t
GET_INT8(const jsbytecode* pc)
{
    return int8_t(pc[1]);
}

static MOZ_ALWAYS_INLINE uint32_t
GET_UINT32(const jsbytecode* pc)
{
#if MOZ_LITTLE_ENDIAN
    uint32_t result;
    memcpy(&result, pc + 1, sizeof(result));
    return result;
#else
    return  (uint32_t(pc[4]) << 24) |
            (uint32_t(pc[3]) << 16) |
            (uint32_t(pc[2]) << 8)  |
            uint32_t(pc[1]);
#endif
}

static MOZ_ALWAYS_INLINE void
SET_UINT32(jsbytecode* pc, uint32_t u)
{
#if MOZ_LITTLE_ENDIAN
    memcpy(pc + 1, &u, sizeof(u));
#else
    pc[1] = jsbytecode(u);
    pc[2] = jsbytecode(u >> 8);
    pc[3] = jsbytecode(u >> 16);
    pc[4] = jsbytecode(u >> 24);
#endif
}

static MOZ_ALWAYS_INLINE int32_t
GET_INT32(const jsbytecode* pc)
{
    return static_cast<int32_t>(GET_UINT32(pc));
}

static MOZ_ALWAYS_INLINE void
SET_INT32(jsbytecode* pc, int32_t i)
{
    SET_UINT32(pc, static_cast<uint32_t>(i));
}

static MOZ_ALWAYS_INLINE int32_t
GET_JUMP_OFFSET(jsbytecode* pc)
{
    return GET_INT32(pc);
}

static MOZ_ALWAYS_INLINE void
SET_JUMP_OFFSET(jsbytecode* pc, int32_t off)
{
    SET_INT32(pc, off);
}

static const unsigned UINT32_INDEX_LEN  = 4;

static MOZ_ALWAYS_INLINE uint32_t
GET_UINT32_INDEX(const jsbytecode* pc)
{
    return GET_UINT32(pc);
}

static MOZ_ALWAYS_INLINE void
SET_UINT32_INDEX(jsbytecode* pc, uint32_t index)
{
    SET_UINT32(pc, index);
}

/* Index limit is determined by SN_4BYTE_OFFSET_FLAG, see frontend/BytecodeEmitter.h. */
static const unsigned INDEX_LIMIT_LOG2  = 31;
static const uint32_t INDEX_LIMIT       = uint32_t(1) << INDEX_LIMIT_LOG2;

static inline jsbytecode
ARGC_HI(uint16_t argc)
{
    return UINT16_HI(argc);
}

static inline jsbytecode
ARGC_LO(uint16_t argc)
{
    return UINT16_LO(argc);
}

static inline uint16_t
GET_ARGC(const jsbytecode* pc)
{
    return GET_UINT16(pc);
}

static const unsigned ARGC_LIMIT        = UINT16_LIMIT;

static inline uint16_t
GET_ARGNO(const jsbytecode* pc)
{
    return GET_UINT16(pc);
}

static inline void
SET_ARGNO(jsbytecode* pc, uint16_t argno)
{
    SET_UINT16(pc, argno);
}

static const unsigned ARGNO_LEN         = 2;
static const unsigned ARGNO_LIMIT       = UINT16_LIMIT;

static inline uint32_t
GET_LOCALNO(const jsbytecode* pc)
{
    return GET_UINT24(pc);
}

static inline void
SET_LOCALNO(jsbytecode* pc, uint32_t varno)
{
    SET_UINT24(pc, varno);
}

static const unsigned LOCALNO_LEN       = 3;
static const unsigned LOCALNO_BITS      = 24;
static const uint32_t LOCALNO_LIMIT     = 1 << LOCALNO_BITS;

static inline unsigned
LoopEntryDepthHint(jsbytecode* pc)
{
    MOZ_ASSERT(*pc == JSOP_LOOPENTRY);
    return GET_UINT8(pc) & 0x7f;
}

static inline bool
LoopEntryCanIonOsr(jsbytecode* pc)
{
    MOZ_ASSERT(*pc == JSOP_LOOPENTRY);
    return GET_UINT8(pc) & 0x80;
}

static inline uint8_t
PackLoopEntryDepthHintAndFlags(unsigned loopDepth, bool canIonOsr)
{
    return (loopDepth < 0x80 ? uint8_t(loopDepth) : 0x7f) | (canIonOsr ? 0x80 : 0);
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

static inline uint8_t
GET_ENVCOORD_HOPS(jsbytecode* pc)
{
    return GET_UINT8(pc);
}

static inline void
SET_ENVCOORD_HOPS(jsbytecode* pc, uint8_t hops)
{
    SET_UINT8(pc, hops);
}

static const unsigned ENVCOORD_HOPS_LEN   = 1;
static const unsigned ENVCOORD_HOPS_BITS  = 8;
static const unsigned ENVCOORD_HOPS_LIMIT = 1 << ENVCOORD_HOPS_BITS;

/* Describes the 'slot' component of a JOF_ENVCOORD opcode. */
static inline uint32_t
GET_ENVCOORD_SLOT(const jsbytecode* pc)
{
    return GET_UINT24(pc);
}

static inline void
SET_ENVCOORD_SLOT(jsbytecode* pc, uint32_t slot)
{
    SET_UINT24(pc, slot);
}

static const unsigned ENVCOORD_SLOT_LEN   = 3;
static const unsigned ENVCOORD_SLOT_BITS  = 24;
static const uint32_t ENVCOORD_SLOT_LIMIT = 1 << ENVCOORD_SLOT_BITS;

struct JSCodeSpec {
    int8_t              length;         /* length including opcode byte */
    int8_t              nuses;          /* arity, -1 if variadic */
    int8_t              ndefs;          /* number of stack results */
    uint32_t            format;         /* immediate operand format */

    uint32_t type() const { return JOF_TYPE(format); }
};

/* Silence unreferenced formal parameter warnings */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100)
#endif

namespace js {

extern const JSCodeSpec CodeSpec[];
extern const unsigned   NumCodeSpecs;
extern const char       * const CodeName[];

/* Shorthand for type from opcode. */

static inline uint32_t
JOF_OPTYPE(JSOp op)
{
    return JOF_TYPE(CodeSpec[op].format);
}

static inline bool
IsJumpOpcode(JSOp op)
{
    uint32_t type = JOF_TYPE(CodeSpec[op].format);

    /*
     * LABEL opcodes have type JOF_JUMP but are no-ops, don't treat them as
     * jumps to avoid degrading precision.
     */
    return type == JOF_JUMP && op != JSOP_LABEL;
}

static inline bool
BytecodeFallsThrough(JSOp op)
{
    switch (op) {
      case JSOP_GOTO:
      case JSOP_DEFAULT:
      case JSOP_RETURN:
      case JSOP_RETRVAL:
      case JSOP_FINALYIELDRVAL:
      case JSOP_THROW:
      case JSOP_THROWMSG:
      case JSOP_TABLESWITCH:
        return false;
      case JSOP_GOSUB:
        /* These fall through indirectly, after executing a 'finally'. */
        return true;
      default:
        return true;
    }
}

static inline bool
BytecodeIsJumpTarget(JSOp op)
{
    switch (op) {
      case JSOP_JUMPTARGET:
      case JSOP_LOOPHEAD:
      case JSOP_LOOPENTRY:
      case JSOP_ENDITER:
      case JSOP_TRY:
        return true;
      default:
        return false;
    }
}

class SrcNoteLineScanner
{
    /* offset of the current JSOp in the bytecode */
    ptrdiff_t offset;

    /* next src note to process */
    jssrcnote* sn;

    /* line number of the current JSOp */
    uint32_t lineno;

    /*
     * Is the current op the first one after a line change directive? Note that
     * multiple ops may be "first" if a line directive is used to return to a
     * previous line (eg, with a for loop increment expression.)
     */
    bool lineHeader;

  public:
    SrcNoteLineScanner(jssrcnote* sn, uint32_t lineno)
        : offset(0), sn(sn), lineno(lineno)
    {
    }

    /*
     * This is called repeatedly with always-advancing relpc values. The src
     * notes are tuples of <PC offset from prev src note, type, args>. Scan
     * through, updating the lineno, until the next src note is for a later
     * bytecode.
     *
     * When looking at the desired PC offset ('relpc'), the op is first in that
     * line iff there is a SRC_SETLINE or SRC_NEWLINE src note for that exact
     * bytecode.
     *
     * Note that a single bytecode may have multiple line-modifying notes (even
     * though only one should ever be needed.)
     */
    void advanceTo(ptrdiff_t relpc) {
        // Must always advance! If the same or an earlier PC is erroneously
        // passed in, we will already be past the relevant src notes
        MOZ_ASSERT_IF(offset > 0, relpc > offset);

        // Next src note should be for after the current offset
        MOZ_ASSERT_IF(offset > 0, SN_IS_TERMINATOR(sn) || SN_DELTA(sn) > 0);

        // The first PC requested is always considered to be a line header
        lineHeader = (offset == 0);

        if (SN_IS_TERMINATOR(sn))
            return;

        ptrdiff_t nextOffset;
        while ((nextOffset = offset + SN_DELTA(sn)) <= relpc && !SN_IS_TERMINATOR(sn)) {
            offset = nextOffset;
            SrcNoteType type = SN_TYPE(sn);
            if (type == SRC_SETLINE || type == SRC_NEWLINE) {
                if (type == SRC_SETLINE)
                    lineno = GetSrcNoteOffset(sn, 0);
                else
                    lineno++;

                if (offset == relpc)
                    lineHeader = true;
            }

            sn = SN_NEXT(sn);
        }
    }

    bool isLineHeader() const {
        return lineHeader;
    }

    uint32_t getLine() const { return lineno; }
};

MOZ_ALWAYS_INLINE unsigned
StackUses(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    int nuses = CodeSpec[op].nuses;
    if (nuses >= 0)
        return nuses;

    MOZ_ASSERT(nuses == -1);
    switch (op) {
      case JSOP_POPN:
        return GET_UINT16(pc);
      case JSOP_NEW:
      case JSOP_SUPERCALL:
        return 2 + GET_ARGC(pc) + 1;
      default:
        /* stack: fun, this, [argc arguments] */
        MOZ_ASSERT(op == JSOP_CALL || op == JSOP_CALL_IGNORES_RV || op == JSOP_EVAL ||
                   op == JSOP_CALLITER ||
                   op == JSOP_STRICTEVAL || op == JSOP_FUNCALL || op == JSOP_FUNAPPLY);
        return 2 + GET_ARGC(pc);
    }
}

MOZ_ALWAYS_INLINE unsigned
StackDefs(jsbytecode* pc)
{
    int ndefs = CodeSpec[*pc].ndefs;
    MOZ_ASSERT(ndefs >= 0);
    return ndefs;
}

#ifdef DEBUG
/*
 * Given bytecode address pc in script's main program code, compute the operand
 * stack depth just before (JSOp) *pc executes.  If *pc is not reachable, return
 * false.
 */
extern bool
ReconstructStackDepth(JSContext* cx, JSScript* script, jsbytecode* pc, uint32_t* depth, bool* reachablePC);
#endif

}  /* namespace js */

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define JSDVG_IGNORE_STACK      0
#define JSDVG_SEARCH_STACK      1

namespace js {

/*
 * Get the length of variable-length bytecode like JSOP_TABLESWITCH.
 */
extern size_t
GetVariableBytecodeLength(jsbytecode* pc);

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
UniqueChars
DecompileValueGenerator(JSContext* cx, int spindex, HandleValue v,
                        HandleString fallback, int skipStackHits = 0);

/*
 * Decompile the formal argument at formalIndex in the nearest non-builtin
 * stack frame, falling back with converting v to source.
 */
char*
DecompileArgument(JSContext* cx, int formalIndex, HandleValue v);

extern bool
CallResultEscapes(jsbytecode* pc);

static inline unsigned
GetDecomposeLength(jsbytecode* pc, size_t len)
{
    /*
     * The last byte of a DECOMPOSE op stores the decomposed length.  This is a
     * constant: perhaps we should just hardcode values instead?
     */
    MOZ_ASSERT(size_t(CodeSpec[*pc].length) == len);
    return (unsigned) pc[len - 1];
}

static inline unsigned
GetBytecodeLength(jsbytecode* pc)
{
    JSOp op = (JSOp)*pc;
    MOZ_ASSERT(op < JSOP_LIMIT);

    if (CodeSpec[op].length != -1)
        return CodeSpec[op].length;
    return GetVariableBytecodeLength(pc);
}

static inline bool
BytecodeIsPopped(jsbytecode* pc)
{
    jsbytecode* next = pc + GetBytecodeLength(pc);
    return JSOp(*next) == JSOP_POP;
}

static inline bool
BytecodeFlowsToBitop(jsbytecode* pc)
{
    // Look for simple bytecode for integer conversions like (x | 0) or (x & -1).
    jsbytecode* next = pc + GetBytecodeLength(pc);
    if (*next == JSOP_BITOR || *next == JSOP_BITAND)
        return true;
    if (*next == JSOP_INT8 && GET_INT8(next) == -1) {
        next += GetBytecodeLength(next);
        if (*next == JSOP_BITAND)
            return true;
        return false;
    }
    if (*next == JSOP_ONE) {
        next += GetBytecodeLength(next);
        if (*next == JSOP_NEG) {
            next += GetBytecodeLength(next);
            if (*next == JSOP_BITAND)
                return true;
        }
        return false;
    }
    if (*next == JSOP_ZERO) {
        next += GetBytecodeLength(next);
        if (*next == JSOP_BITOR)
            return true;
        return false;
    }
    return false;
}

extern bool
IsValidBytecodeOffset(JSContext* cx, JSScript* script, size_t offset);

inline bool
FlowsIntoNext(JSOp op)
{
    // JSOP_YIELD/JSOP_AWAIT is considered to flow into the next instruction,
    // like JSOP_CALL.
    switch (op) {
      case JSOP_RETRVAL:
      case JSOP_RETURN:
      case JSOP_THROW:
      case JSOP_GOTO:
      case JSOP_RETSUB:
      case JSOP_FINALYIELDRVAL:
        return false;
      default:
        return true;
    }
}

inline bool
IsArgOp(JSOp op)
{
    return JOF_OPTYPE(op) == JOF_QARG;
}

inline bool
IsLocalOp(JSOp op)
{
    return JOF_OPTYPE(op) == JOF_LOCAL;
}

inline bool
IsAliasedVarOp(JSOp op)
{
    return JOF_OPTYPE(op) == JOF_ENVCOORD;
}

inline bool
IsGlobalOp(JSOp op)
{
    return CodeSpec[op].format & JOF_GNAME;
}

inline bool
IsPropertySetOp(JSOp op)
{
    return CodeSpec[op].format & JOF_PROPSET;
}

inline bool
IsPropertyInitOp(JSOp op)
{
    return CodeSpec[op].format & JOF_PROPINIT;
}

inline bool
IsEqualityOp(JSOp op)
{
    return op == JSOP_EQ || op == JSOP_NE || op == JSOP_STRICTEQ || op == JSOP_STRICTNE;
}

inline bool
IsCheckStrictOp(JSOp op)
{
    return CodeSpec[op].format & JOF_CHECKSTRICT;
}

#ifdef DEBUG
inline bool
IsCheckSloppyOp(JSOp op)
{
    return CodeSpec[op].format & JOF_CHECKSLOPPY;
}
#endif

inline bool
IsAtomOp(JSOp op)
{
    return JOF_OPTYPE(op) == JOF_ATOM;
}

inline bool
IsGetPropPC(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    return op == JSOP_LENGTH  || op == JSOP_GETPROP || op == JSOP_CALLPROP;
}

inline bool
IsHiddenInitOp(JSOp op)
{
    return op == JSOP_INITHIDDENPROP || op == JSOP_INITHIDDENELEM ||
           op == JSOP_INITHIDDENPROP_GETTER || op == JSOP_INITHIDDENELEM_GETTER ||
           op == JSOP_INITHIDDENPROP_SETTER || op == JSOP_INITHIDDENELEM_SETTER;
}

inline bool
IsStrictSetPC(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    return op == JSOP_STRICTSETPROP ||
           op == JSOP_STRICTSETNAME ||
           op == JSOP_STRICTSETGNAME ||
           op == JSOP_STRICTSETELEM;
}

inline bool
IsSetPropPC(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    return op == JSOP_SETPROP || op == JSOP_STRICTSETPROP ||
           op == JSOP_SETNAME || op == JSOP_STRICTSETNAME ||
           op == JSOP_SETGNAME || op == JSOP_STRICTSETGNAME;
}

inline bool
IsGetElemPC(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    return op == JSOP_GETELEM || op == JSOP_CALLELEM;
}

inline bool
IsSetElemPC(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    return op == JSOP_SETELEM ||
           op == JSOP_STRICTSETELEM;
}

inline bool
IsElemPC(jsbytecode* pc)
{
    return CodeSpec[*pc].format & JOF_ELEM;
}

inline bool
IsCallPC(jsbytecode* pc)
{
    return CodeSpec[*pc].format & JOF_INVOKE;
}

inline bool
IsStrictEvalPC(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    return op == JSOP_STRICTEVAL || op == JSOP_STRICTSPREADEVAL;
}

inline bool
IsConstructorCallPC(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    return op == JSOP_NEW ||
           op == JSOP_SUPERCALL ||
           op == JSOP_SPREADNEW ||
           op == JSOP_SPREADSUPERCALL;
}

inline bool
IsSpreadCallPC(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    return op == JSOP_SPREADCALL ||
           op == JSOP_SPREADNEW ||
           op == JSOP_SPREADSUPERCALL ||
           op == JSOP_SPREADEVAL ||
           op == JSOP_STRICTSPREADEVAL;
}

static inline int32_t
GetBytecodeInteger(jsbytecode* pc)
{
    switch (JSOp(*pc)) {
      case JSOP_ZERO:   return 0;
      case JSOP_ONE:    return 1;
      case JSOP_UINT16: return GET_UINT16(pc);
      case JSOP_UINT24: return GET_UINT24(pc);
      case JSOP_INT8:   return GET_INT8(pc);
      case JSOP_INT32:  return GET_INT32(pc);
      default:
        MOZ_CRASH("Bad op");
    }
}

/*
 * Counts accumulated for a single opcode in a script. The counts tracked vary
 * between opcodes, and this structure ensures that counts are accessed in a
 * coherent fashion.
 */
class PCCounts
{
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
    explicit PCCounts(size_t off)
      : pcOffset_(off),
        numExec_(0)
    {}

    size_t pcOffset() const {
        return pcOffset_;
    }

    // Used for sorting and searching.
    bool operator<(const PCCounts& rhs) const {
        return pcOffset_ < rhs.pcOffset_;
    }

    uint64_t& numExec() {
        return numExec_;
    }
    uint64_t numExec() const {
        return numExec_;
    }

    static const char* numExecName;
};

static inline jsbytecode*
GetNextPc(jsbytecode* pc)
{
    return pc + GetBytecodeLength(pc);
}

#if defined(DEBUG)
/*
 * Disassemblers, for debugging only.
 */
extern MOZ_MUST_USE bool
Disassemble(JSContext* cx, JS::Handle<JSScript*> script, bool lines, Sprinter* sp);

unsigned
Disassemble1(JSContext* cx, JS::Handle<JSScript*> script, jsbytecode* pc, unsigned loc,
             bool lines, Sprinter* sp);

#endif

extern MOZ_MUST_USE bool
DumpCompartmentPCCounts(JSContext* cx);

} // namespace js

#endif /* vm_BytecodeUtil_h */
