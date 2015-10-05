/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS bytecode generation.
 */

#include "frontend/BytecodeEmitter.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/PodOperations.h"
#include "mozilla/UniquePtr.h"

#include <string.h>

#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsfun.h"
#include "jsnum.h"
#include "jsopcode.h"
#include "jsscript.h"
#include "jstypes.h"
#include "jsutil.h"

#include "asmjs/AsmJSLink.h"
#include "frontend/Parser.h"
#include "frontend/TokenStream.h"
#include "vm/Debugger.h"
#include "vm/GeneratorObject.h"
#include "vm/Stack.h"

#include "jsatominlines.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "frontend/ParseMaps-inl.h"
#include "frontend/ParseNode-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ScopeObject-inl.h"

using namespace js;
using namespace js::gc;
using namespace js::frontend;

using mozilla::DebugOnly;
using mozilla::NumberIsInt32;
using mozilla::PodCopy;
using mozilla::UniquePtr;

static bool
SetSrcNoteOffset(ExclusiveContext* cx, BytecodeEmitter* bce, unsigned index, unsigned which, ptrdiff_t offset);

static bool
UpdateSourceCoordNotes(ExclusiveContext* cx, BytecodeEmitter* bce, uint32_t offset);

struct frontend::StmtInfoBCE : public StmtInfoBase
{
    StmtInfoBCE*    down;          /* info for enclosing statement */
    StmtInfoBCE*    downScope;     /* next enclosing lexical scope */

    ptrdiff_t       update;         /* loop update offset (top if none) */
    ptrdiff_t       breaks;         /* offset of last break in loop */
    ptrdiff_t       continues;      /* offset of last continue in loop */
    uint32_t        blockScopeIndex; /* index of scope in BlockScopeArray */

    explicit StmtInfoBCE(ExclusiveContext* cx) : StmtInfoBase(cx) {}

    /*
     * To reuse space, alias two of the ptrdiff_t fields for use during
     * try/catch/finally code generation and backpatching.
     *
     * Only a loop, switch, or label statement info record can have breaks and
     * continues, and only a for loop has an update backpatch chain, so it's
     * safe to overlay these for the "trying" StmtTypes.
     */

    ptrdiff_t& gosubs() {
        MOZ_ASSERT(type == STMT_FINALLY);
        return breaks;
    }

    ptrdiff_t& guardJump() {
        MOZ_ASSERT(type == STMT_TRY || type == STMT_FINALLY);
        return continues;
    }
};


namespace {

struct LoopStmtInfo : public StmtInfoBCE
{
    int32_t         stackDepth;     // Stack depth when this loop was pushed.
    uint32_t        loopDepth;      // Loop depth.

    // Can we OSR into Ion from here?  True unless there is non-loop state on the stack.
    bool            canIonOsr;

    explicit LoopStmtInfo(ExclusiveContext* cx) : StmtInfoBCE(cx) {}

    static LoopStmtInfo* fromStmtInfo(StmtInfoBCE* stmt) {
        MOZ_ASSERT(stmt->isLoop());
        return static_cast<LoopStmtInfo*>(stmt);
    }
};

} // anonymous namespace

BytecodeEmitter::BytecodeEmitter(BytecodeEmitter* parent,
                                 Parser<FullParseHandler>* parser, SharedContext* sc,
                                 HandleScript script, Handle<LazyScript*> lazyScript,
                                 bool insideEval, HandleScript evalCaller,
                                 Handle<StaticEvalObject*> staticEvalScope,
                                 bool hasGlobalScope, uint32_t lineNum, EmitterMode emitterMode)
  : sc(sc),
    parent(parent),
    script(sc->context, script),
    lazyScript(sc->context, lazyScript),
    prolog(sc->context, lineNum),
    main(sc->context, lineNum),
    current(&main),
    parser(parser),
    evalCaller(evalCaller),
    evalStaticScope(staticEvalScope),
    topStmt(nullptr),
    topScopeStmt(nullptr),
    staticScope(sc->context),
    atomIndices(sc->context),
    firstLine(lineNum),
    localsToFrameSlots_(sc->context),
    stackDepth(0), maxStackDepth(0),
    arrayCompDepth(0),
    emitLevel(0),
    constList(sc->context),
    tryNoteList(sc->context),
    blockScopeList(sc->context),
    yieldOffsetList(sc->context),
    typesetCount(0),
    hasSingletons(false),
    hasTryFinally(false),
    emittingForInit(false),
    emittingRunOnceLambda(false),
    insideEval(insideEval),
    hasGlobalScope(hasGlobalScope),
    emitterMode(emitterMode)
{
    MOZ_ASSERT_IF(evalCaller, insideEval);
    MOZ_ASSERT_IF(emitterMode == LazyFunction, lazyScript);
    // Function scripts are never eval scripts.
    MOZ_ASSERT_IF(evalStaticScope, !sc->isFunctionBox());
}

bool
BytecodeEmitter::init()
{
    return atomIndices.ensureMap(sc->context);
}

bool
BytecodeEmitter::updateLocalsToFrameSlots()
{
    // Assign stack slots to unaliased locals (aliased locals are stored in the
    // call object and don't need their own stack slots). We do this by filling
    // a Vector that can be used to map a local to its stack slot.

    if (localsToFrameSlots_.length() == script->bindings.numLocals()) {
        // CompileScript calls updateNumBlockScoped to update the block scope
        // depth. Do nothing if the depth didn't change.
        return true;
    }

    localsToFrameSlots_.clear();

    if (!localsToFrameSlots_.reserve(script->bindings.numLocals()))
        return false;

    uint32_t slot = 0;
    for (BindingIter bi(script); !bi.done(); bi++) {
        if (bi->kind() == Binding::ARGUMENT)
            continue;

        if (bi->aliased())
            localsToFrameSlots_.infallibleAppend(UINT32_MAX);
        else
            localsToFrameSlots_.infallibleAppend(slot++);
    }

    for (size_t i = 0; i < script->bindings.numBlockScoped(); i++)
        localsToFrameSlots_.infallibleAppend(slot++);

    return true;
}

static ptrdiff_t
EmitCheck(ExclusiveContext* cx, BytecodeEmitter* bce, ptrdiff_t delta)
{
    ptrdiff_t offset = bce->code().length();

    // Start it off moderately large to avoid repeated resizings early on.
    // ~98% of cases fit within 1024 bytes.
    if (bce->code().capacity() == 0 && !bce->code().reserve(1024))
        return -1;

    jsbytecode dummy = 0;
    if (!bce->code().appendN(dummy, delta)) {
        js_ReportOutOfMemory(cx);
        return -1;
    }
    return offset;
}

static void
UpdateDepth(ExclusiveContext* cx, BytecodeEmitter* bce, ptrdiff_t target)
{
    jsbytecode* pc = bce->code(target);
    JSOp op = (JSOp) *pc;
    const JSCodeSpec* cs = &js_CodeSpec[op];

    if (cs->format & JOF_TMPSLOT_MASK) {
        /*
         * An opcode may temporarily consume stack space during execution.
         * Account for this in maxStackDepth separately from uses/defs here.
         */
        uint32_t depth = (uint32_t) bce->stackDepth +
                         ((cs->format & JOF_TMPSLOT_MASK) >> JOF_TMPSLOT_SHIFT);
        if (depth > bce->maxStackDepth)
            bce->maxStackDepth = depth;
    }

    int nuses = StackUses(nullptr, pc);
    int ndefs = StackDefs(nullptr, pc);

    bce->stackDepth -= nuses;
    MOZ_ASSERT(bce->stackDepth >= 0);
    bce->stackDepth += ndefs;
    if ((uint32_t)bce->stackDepth > bce->maxStackDepth)
        bce->maxStackDepth = bce->stackDepth;
}

#ifdef DEBUG
static bool
CheckStrictOrSloppy(BytecodeEmitter* bce, JSOp op)
{
    if (IsCheckStrictOp(op) && !bce->sc->strict)
        return false;
    if (IsCheckSloppyOp(op) && bce->sc->strict)
        return false;
    return true;
}
#endif

ptrdiff_t
frontend::Emit1(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op)
{
    MOZ_ASSERT(CheckStrictOrSloppy(bce, op));
    ptrdiff_t offset = EmitCheck(cx, bce, 1);
    if (offset < 0)
        return -1;

    jsbytecode* code = bce->code(offset);
    code[0] = jsbytecode(op);
    UpdateDepth(cx, bce, offset);
    return offset;
}

ptrdiff_t
frontend::Emit2(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, jsbytecode op1)
{
    MOZ_ASSERT(CheckStrictOrSloppy(bce, op));
    ptrdiff_t offset = EmitCheck(cx, bce, 2);
    if (offset < 0)
        return -1;

    jsbytecode* code = bce->code(offset);
    code[0] = jsbytecode(op);
    code[1] = op1;
    UpdateDepth(cx, bce, offset);
    return offset;
}

ptrdiff_t
frontend::Emit3(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, jsbytecode op1,
                jsbytecode op2)
{
    MOZ_ASSERT(CheckStrictOrSloppy(bce, op));

    /* These should filter through EmitVarOp. */
    MOZ_ASSERT(!IsArgOp(op));
    MOZ_ASSERT(!IsLocalOp(op));

    ptrdiff_t offset = EmitCheck(cx, bce, 3);
    if (offset < 0)
        return -1;

    jsbytecode* code = bce->code(offset);
    code[0] = jsbytecode(op);
    code[1] = op1;
    code[2] = op2;
    UpdateDepth(cx, bce, offset);
    return offset;
}

ptrdiff_t
frontend::EmitN(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, size_t extra)
{
    MOZ_ASSERT(CheckStrictOrSloppy(bce, op));
    ptrdiff_t length = 1 + (ptrdiff_t)extra;
    ptrdiff_t offset = EmitCheck(cx, bce, length);
    if (offset < 0)
        return -1;

    jsbytecode* code = bce->code(offset);
    code[0] = jsbytecode(op);
    /* The remaining |extra| bytes are set by the caller */

    /*
     * Don't UpdateDepth if op's use-count comes from the immediate
     * operand yet to be stored in the extra bytes after op.
     */
    if (js_CodeSpec[op].nuses >= 0)
        UpdateDepth(cx, bce, offset);

    return offset;
}

static ptrdiff_t
EmitJump(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, ptrdiff_t off)
{
    ptrdiff_t offset = EmitCheck(cx, bce, 5);
    if (offset < 0)
        return -1;

    jsbytecode* code = bce->code(offset);
    code[0] = jsbytecode(op);
    SET_JUMP_OFFSET(code, off);
    UpdateDepth(cx, bce, offset);
    return offset;
}

static ptrdiff_t
EmitCall(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, uint16_t argc, ParseNode* pn=nullptr)
{
    if (pn && !UpdateSourceCoordNotes(cx, bce, pn->pn_pos.begin))
        return -1;
    return Emit3(cx, bce, op, ARGC_HI(argc), ARGC_LO(argc));
}

// Dup the var in operand stack slot "slot".  The first item on the operand
// stack is one slot past the last fixed slot.  The last (most recent) item is
// slot bce->stackDepth - 1.
//
// The instruction that is written (JSOP_DUPAT) switches the depth around so
// that it is addressed from the sp instead of from the fp.  This is useful when
// you don't know the size of the fixed stack segment (nfixed), as is the case
// when compiling scripts (because each statement is parsed and compiled
// separately, but they all together form one script with one fixed stack
// frame).
static bool
EmitDupAt(ExclusiveContext* cx, BytecodeEmitter* bce, unsigned slot)
{
    MOZ_ASSERT(slot < unsigned(bce->stackDepth));
    // The slot's position on the operand stack, measured from the top.
    unsigned slotFromTop = bce->stackDepth - 1 - slot;
    if (slotFromTop >= JS_BIT(24)) {
        bce->reportError(nullptr, JSMSG_TOO_MANY_LOCALS);
        return false;
    }
    ptrdiff_t off = EmitN(cx, bce, JSOP_DUPAT, 3);
    if (off < 0)
        return false;
    jsbytecode* pc = bce->code(off);
    SET_UINT24(pc, slotFromTop);
    return true;
}

/* XXX too many "... statement" L10N gaffes below -- fix via js.msg! */
const char js_with_statement_str[] = "with statement";
const char js_finally_block_str[]  = "finally block";
const char js_script_str[]         = "script";

static const char * const statementName[] = {
    "label statement",       /* LABEL */
    "if statement",          /* IF */
    "else statement",        /* ELSE */
    "destructuring body",    /* BODY */
    "switch statement",      /* SWITCH */
    "block",                 /* BLOCK */
    js_with_statement_str,   /* WITH */
    "catch block",           /* CATCH */
    "try block",             /* TRY */
    js_finally_block_str,    /* FINALLY */
    js_finally_block_str,    /* SUBROUTINE */
    "do loop",               /* DO_LOOP */
    "for loop",              /* FOR_LOOP */
    "for/in loop",           /* FOR_IN_LOOP */
    "for/of loop",           /* FOR_OF_LOOP */
    "while loop",            /* WHILE_LOOP */
    "spread",                /* SPREAD */
};

static_assert(MOZ_ARRAY_LENGTH(statementName) == STMT_LIMIT,
              "statementName array and StmtType enum must be consistent");

static const char*
StatementName(StmtInfoBCE* topStmt)
{
    if (!topStmt)
        return js_script_str;
    return statementName[topStmt->type];
}

static void
ReportStatementTooLarge(TokenStream& ts, StmtInfoBCE* topStmt)
{
    ts.reportError(JSMSG_NEED_DIET, StatementName(topStmt));
}

/*
 * Emit a backpatch op with offset pointing to the previous jump of this type,
 * so that we can walk back up the chain fixing up the op and jump offset.
 */
static ptrdiff_t
EmitBackPatchOp(ExclusiveContext* cx, BytecodeEmitter* bce, ptrdiff_t* lastp)
{
    ptrdiff_t offset, delta;

    offset = bce->offset();
    delta = offset - *lastp;
    *lastp = offset;
    MOZ_ASSERT(delta > 0);
    return EmitJump(cx, bce, JSOP_BACKPATCH, delta);
}

static inline unsigned
LengthOfSetLine(unsigned line)
{
    return 1 /* SN_SETLINE */ + (line > SN_4BYTE_OFFSET_MASK ? 4 : 1);
}

/* Updates line number notes, not column notes. */
static inline bool
UpdateLineNumberNotes(ExclusiveContext* cx, BytecodeEmitter* bce, uint32_t offset)
{
    TokenStream* ts = &bce->parser->tokenStream;
    bool onThisLine;
    if (!ts->srcCoords.isOnThisLine(offset, bce->currentLine(), &onThisLine))
        return ts->reportError(JSMSG_OUT_OF_MEMORY);
    if (!onThisLine) {
        unsigned line = ts->srcCoords.lineNum(offset);
        unsigned delta = line - bce->currentLine();

        /*
         * Encode any change in the current source line number by using
         * either several SRC_NEWLINE notes or just one SRC_SETLINE note,
         * whichever consumes less space.
         *
         * NB: We handle backward line number deltas (possible with for
         * loops where the update part is emitted after the body, but its
         * line number is <= any line number in the body) here by letting
         * unsigned delta_ wrap to a very large number, which triggers a
         * SRC_SETLINE.
         */
        bce->current->currentLine = line;
        bce->current->lastColumn  = 0;
        if (delta >= LengthOfSetLine(line)) {
            if (NewSrcNote2(cx, bce, SRC_SETLINE, (ptrdiff_t)line) < 0)
                return false;
        } else {
            do {
                if (NewSrcNote(cx, bce, SRC_NEWLINE) < 0)
                    return false;
            } while (--delta != 0);
        }
    }
    return true;
}

/* Updates the line number and column number information in the source notes. */
static bool
UpdateSourceCoordNotes(ExclusiveContext* cx, BytecodeEmitter* bce, uint32_t offset)
{
    if (!UpdateLineNumberNotes(cx, bce, offset))
        return false;

    uint32_t columnIndex = bce->parser->tokenStream.srcCoords.columnIndex(offset);
    ptrdiff_t colspan = ptrdiff_t(columnIndex) - ptrdiff_t(bce->current->lastColumn);
    if (colspan != 0) {
        // If the column span is so large that we can't store it, then just
        // discard this information. This can happen with minimized or otherwise
        // machine-generated code. Even gigantic column numbers are still
        // valuable if you have a source map to relate them to something real;
        // but it's better to fail soft here.
        if (!SN_REPRESENTABLE_COLSPAN(colspan))
            return true;
        if (NewSrcNote2(cx, bce, SRC_COLSPAN, SN_COLSPAN_TO_OFFSET(colspan)) < 0)
            return false;
        bce->current->lastColumn = columnIndex;
    }
    return true;
}

static ptrdiff_t
EmitLoopHead(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* nextpn)
{
    if (nextpn) {
        /*
         * Try to give the JSOP_LOOPHEAD the same line number as the next
         * instruction. nextpn is often a block, in which case the next
         * instruction typically comes from the first statement inside.
         */
        MOZ_ASSERT_IF(nextpn->isKind(PNK_STATEMENTLIST), nextpn->isArity(PN_LIST));
        if (nextpn->isKind(PNK_STATEMENTLIST) && nextpn->pn_head)
            nextpn = nextpn->pn_head;
        if (!UpdateSourceCoordNotes(cx, bce, nextpn->pn_pos.begin))
            return -1;
    }

    return Emit1(cx, bce, JSOP_LOOPHEAD);
}

static bool
EmitLoopEntry(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* nextpn)
{
    if (nextpn) {
        /* Update the line number, as for LOOPHEAD. */
        MOZ_ASSERT_IF(nextpn->isKind(PNK_STATEMENTLIST), nextpn->isArity(PN_LIST));
        if (nextpn->isKind(PNK_STATEMENTLIST) && nextpn->pn_head)
            nextpn = nextpn->pn_head;
        if (!UpdateSourceCoordNotes(cx, bce, nextpn->pn_pos.begin))
            return false;
    }

    LoopStmtInfo* loop = LoopStmtInfo::fromStmtInfo(bce->topStmt);
    MOZ_ASSERT(loop->loopDepth > 0);

    uint8_t loopDepthAndFlags = PackLoopEntryDepthHintAndFlags(loop->loopDepth, loop->canIonOsr);
    return Emit2(cx, bce, JSOP_LOOPENTRY, loopDepthAndFlags) >= 0;
}

/*
 * If op is JOF_TYPESET (see the type barriers comment in TypeInference.h),
 * reserve a type set to store its result.
 */
static inline void
CheckTypeSet(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op)
{
    if (js_CodeSpec[op].format & JOF_TYPESET) {
        if (bce->typesetCount < UINT16_MAX)
            bce->typesetCount++;
    }
}

/*
 * Macro to emit a bytecode followed by a uint16_t immediate operand stored in
 * big-endian order.
 *
 * NB: We use cx and bce from our caller's lexical environment, and return
 * false on error.
 */
#define EMIT_UINT16_IMM_OP(op, i)                                             \
    JS_BEGIN_MACRO                                                            \
        if (Emit3(cx, bce, op, UINT16_HI(i), UINT16_LO(i)) < 0)               \
            return false;                                                     \
        CheckTypeSet(cx, bce, op);                                            \
    JS_END_MACRO

static bool
FlushPops(ExclusiveContext* cx, BytecodeEmitter* bce, int* npops)
{
    MOZ_ASSERT(*npops != 0);
    EMIT_UINT16_IMM_OP(JSOP_POPN, *npops);
    *npops = 0;
    return true;
}

static bool
PopIterator(ExclusiveContext* cx, BytecodeEmitter* bce)
{
    if (Emit1(cx, bce, JSOP_ENDITER) < 0)
        return false;
    return true;
}

namespace {

class NonLocalExitScope {
    ExclusiveContext* cx;
    BytecodeEmitter* bce;
    const uint32_t savedScopeIndex;
    const int savedDepth;
    uint32_t openScopeIndex;

    NonLocalExitScope(const NonLocalExitScope&) = delete;

  public:
    explicit NonLocalExitScope(ExclusiveContext* cx_, BytecodeEmitter* bce_)
      : cx(cx_),
        bce(bce_),
        savedScopeIndex(bce->blockScopeList.length()),
        savedDepth(bce->stackDepth),
        openScopeIndex(UINT32_MAX) {
        if (bce->staticScope) {
            StmtInfoBCE* stmt = bce->topStmt;
            while (1) {
                MOZ_ASSERT(stmt);
                if (stmt->isNestedScope) {
                    openScopeIndex = stmt->blockScopeIndex;
                    break;
                }
                stmt = stmt->down;
            }
        }
    }

    ~NonLocalExitScope() {
        for (uint32_t n = savedScopeIndex; n < bce->blockScopeList.length(); n++)
            bce->blockScopeList.recordEnd(n, bce->offset());
        bce->stackDepth = savedDepth;
    }

    bool popScopeForNonLocalExit(uint32_t blockScopeIndex) {
        uint32_t scopeObjectIndex = bce->blockScopeList.findEnclosingScope(blockScopeIndex);
        uint32_t parent = openScopeIndex;

        if (!bce->blockScopeList.append(scopeObjectIndex, bce->offset(), parent))
            return false;
        openScopeIndex = bce->blockScopeList.length() - 1;
        return true;
    }

    bool prepareForNonLocalJump(StmtInfoBCE* toStmt);
};

/*
 * Emit additional bytecode(s) for non-local jumps.
 */
bool
NonLocalExitScope::prepareForNonLocalJump(StmtInfoBCE* toStmt)
{
    int npops = 0;

#define FLUSH_POPS() if (npops && !FlushPops(cx, bce, &npops)) return false

    for (StmtInfoBCE* stmt = bce->topStmt; stmt != toStmt; stmt = stmt->down) {
        switch (stmt->type) {
          case STMT_FINALLY:
            FLUSH_POPS();
            if (EmitBackPatchOp(cx, bce, &stmt->gosubs()) < 0)
                return false;
            break;

          case STMT_WITH:
            if (Emit1(cx, bce, JSOP_LEAVEWITH) < 0)
                return false;
            MOZ_ASSERT(stmt->isNestedScope);
            if (!popScopeForNonLocalExit(stmt->blockScopeIndex))
                return false;
            break;

          case STMT_FOR_OF_LOOP:
            npops += 2;
            break;

          case STMT_FOR_IN_LOOP:
            /* The iterator and the current value are on the stack. */
            npops += 1;
            FLUSH_POPS();
            if (!PopIterator(cx, bce))
                return false;
            break;

          case STMT_SPREAD:
            MOZ_ASSERT_UNREACHABLE("can't break/continue/return from inside a spread");
            break;

          case STMT_SUBROUTINE:
            /*
             * There's a [exception or hole, retsub pc-index] pair on the
             * stack that we need to pop.
             */
            npops += 2;
            break;

          default:;
        }

        if (stmt->isBlockScope) {
            MOZ_ASSERT(stmt->isNestedScope);
            StaticBlockObject& blockObj = stmt->staticBlock();
            if (Emit1(cx, bce, JSOP_DEBUGLEAVEBLOCK) < 0)
                return false;
            if (!popScopeForNonLocalExit(stmt->blockScopeIndex))
                return false;
            if (blockObj.needsClone()) {
                if (Emit1(cx, bce, JSOP_POPBLOCKSCOPE) < 0)
                    return false;
            }
        }
    }

    FLUSH_POPS();
    return true;

#undef FLUSH_POPS
}

}  // anonymous namespace

static ptrdiff_t
EmitGoto(ExclusiveContext* cx, BytecodeEmitter* bce, StmtInfoBCE* toStmt, ptrdiff_t* lastp,
         SrcNoteType noteType = SRC_NULL)
{
    NonLocalExitScope nle(cx, bce);

    if (!nle.prepareForNonLocalJump(toStmt))
        return -1;

    if (noteType != SRC_NULL) {
        if (NewSrcNote(cx, bce, noteType) < 0)
            return -1;
    }

    return EmitBackPatchOp(cx, bce, lastp);
}

static bool
BackPatch(ExclusiveContext* cx, BytecodeEmitter* bce, ptrdiff_t last, jsbytecode* target, jsbytecode op)
{
    jsbytecode* pc, *stop;
    ptrdiff_t delta, span;

    pc = bce->code(last);
    stop = bce->code(-1);
    while (pc != stop) {
        delta = GET_JUMP_OFFSET(pc);
        span = target - pc;
        SET_JUMP_OFFSET(pc, span);
        *pc = op;
        pc -= delta;
    }
    return true;
}

#define SET_STATEMENT_TOP(stmt, top)                                          \
    ((stmt)->update = (top), (stmt)->breaks = (stmt)->continues = (-1))

static void
PushStatementInner(BytecodeEmitter* bce, StmtInfoBCE* stmt, StmtType type, ptrdiff_t top)
{
    SET_STATEMENT_TOP(stmt, top);
    PushStatement(bce, stmt, type);
}

static void
PushStatementBCE(BytecodeEmitter* bce, StmtInfoBCE* stmt, StmtType type, ptrdiff_t top)
{
    PushStatementInner(bce, stmt, type, top);
    MOZ_ASSERT(!stmt->isLoop());
}

static void
PushLoopStatement(BytecodeEmitter* bce, LoopStmtInfo* stmt, StmtType type, ptrdiff_t top)
{
    PushStatementInner(bce, stmt, type, top);
    MOZ_ASSERT(stmt->isLoop());

    LoopStmtInfo* downLoop = nullptr;
    for (StmtInfoBCE* outer = stmt->down; outer; outer = outer->down) {
        if (outer->isLoop()) {
            downLoop = LoopStmtInfo::fromStmtInfo(outer);
            break;
        }
    }

    stmt->stackDepth = bce->stackDepth;
    stmt->loopDepth = downLoop ? downLoop->loopDepth + 1 : 1;

    int loopSlots;
    if (type == STMT_SPREAD)
        loopSlots = 3;
    else if (type == STMT_FOR_IN_LOOP || type == STMT_FOR_OF_LOOP)
        loopSlots = 2;
    else
        loopSlots = 0;

    MOZ_ASSERT(loopSlots <= stmt->stackDepth);

    if (downLoop)
        stmt->canIonOsr = (downLoop->canIonOsr &&
                           stmt->stackDepth == downLoop->stackDepth + loopSlots);
    else
        stmt->canIonOsr = stmt->stackDepth == loopSlots;
}

/*
 * Return the enclosing lexical scope, which is the innermost enclosing static
 * block object or compiler created function.
 */
static JSObject*
EnclosingStaticScope(BytecodeEmitter* bce)
{
    if (bce->staticScope)
        return bce->staticScope;

    if (!bce->sc->isFunctionBox()) {
        MOZ_ASSERT(!bce->parent);

        // Top-level eval scripts have a placeholder static scope so that
        // StaticScopeIter may iterate through evals.
        return bce->evalStaticScope;
    }

    return bce->sc->asFunctionBox()->function();
}

#ifdef DEBUG
static bool
AllLocalsAliased(StaticBlockObject& obj)
{
    for (unsigned i = 0; i < obj.numVariables(); i++)
        if (!obj.isAliased(i))
            return false;
    return true;
}
#endif

static bool
ComputeAliasedSlots(ExclusiveContext* cx, BytecodeEmitter* bce, Handle<StaticBlockObject*> blockObj)
{
    uint32_t numAliased = bce->script->bindings.numAliasedBodyLevelLocals();

    for (unsigned i = 0; i < blockObj->numVariables(); i++) {
        Definition* dn = blockObj->definitionParseNode(i);

        MOZ_ASSERT(dn->isDefn());

        // blockIndexToLocalIndex returns the frame slot following the unaliased
        // locals. We add numAliased so that the cookie's slot value comes after
        // all (aliased and unaliased) body level locals.
        if (!dn->pn_cookie.set(bce->parser->tokenStream, dn->pn_cookie.level(),
                               numAliased + blockObj->blockIndexToLocalIndex(dn->frameSlot())))
        {
            return false;
        }

#ifdef DEBUG
        for (ParseNode* pnu = dn->dn_uses; pnu; pnu = pnu->pn_link) {
            MOZ_ASSERT(pnu->pn_lexdef == dn);
            MOZ_ASSERT(!(pnu->pn_dflags & PND_BOUND));
            MOZ_ASSERT(pnu->pn_cookie.isFree());
        }
#endif

        blockObj->setAliased(i, bce->isAliasedName(dn));
    }

    MOZ_ASSERT_IF(bce->sc->allLocalsAliased(), AllLocalsAliased(*blockObj));

    return true;
}

static bool
EmitInternedObjectOp(ExclusiveContext* cx, uint32_t index, JSOp op, BytecodeEmitter* bce);

// In a function, block-scoped locals go after the vars, and form part of the
// fixed part of a stack frame.  Outside a function, there are no fixed vars,
// but block-scoped locals still form part of the fixed part of a stack frame
// and are thus addressable via GETLOCAL and friends.
static void
ComputeLocalOffset(ExclusiveContext* cx, BytecodeEmitter* bce, Handle<StaticBlockObject*> blockObj)
{
    unsigned nbodyfixed = bce->sc->isFunctionBox()
                          ? bce->script->bindings.numUnaliasedBodyLevelLocals()
                          : 0;
    unsigned localOffset = nbodyfixed;

    if (bce->staticScope) {
        Rooted<NestedScopeObject*> outer(cx, bce->staticScope);
        for (; outer; outer = outer->enclosingNestedScope()) {
            if (outer->is<StaticBlockObject>()) {
                StaticBlockObject& outerBlock = outer->as<StaticBlockObject>();
                localOffset = outerBlock.localOffset() + outerBlock.numVariables();
                break;
            }
        }
    }

    MOZ_ASSERT(localOffset + blockObj->numVariables()
               <= nbodyfixed + bce->script->bindings.numBlockScoped());

    blockObj->setLocalOffset(localOffset);
}

// ~ Nested Scopes ~
//
// A nested scope is a region of a compilation unit (function, script, or eval
// code) with an additional node on the scope chain.  This node may either be a
// "with" object or a "block" object.  "With" objects represent "with" scopes.
// Block objects represent lexical scopes, and contain named block-scoped
// bindings, for example "let" bindings or the exception in a catch block.
// Those variables may be local and thus accessible directly from the stack, or
// "aliased" (accessed by name from nested functions, or dynamically via nested
// "eval" or "with") and only accessible through the scope chain.
//
// All nested scopes are present on the "static scope chain".  A nested scope
// that is a "with" scope will be present on the scope chain at run-time as
// well.  A block scope may or may not have a corresponding link on the run-time
// scope chain; if no variable declared in the block scope is "aliased", then no
// scope chain node is allocated.
//
// To help debuggers, the bytecode emitter arranges to record the PC ranges
// comprehended by a nested scope, and ultimately attach them to the JSScript.
// An element in the "block scope array" specifies the PC range, and links to a
// NestedScopeObject in the object list of the script.  That scope object is
// linked to the previous link in the static scope chain, if any.  The static
// scope chain at any pre-retire PC can be retrieved using
// JSScript::getStaticScope(jsbytecode* pc).
//
// Block scopes store their locals in the fixed part of a stack frame, after the
// "fixed var" bindings.  A fixed var binding is a "var" or legacy "const"
// binding that occurs in a function (as opposed to a script or in eval code).
// Only functions have fixed var bindings.
//
// To assist the debugger, we emit a DEBUGLEAVEBLOCK opcode before leaving a
// block scope, even if the block has no aliased locals.  This allows
// DebugScopes to invalidate any association between a debugger scope object,
// which can proxy access to unaliased stack locals, and the actual live frame.
// In normal, non-debug mode, this opcode does not cause any baseline code to be
// emitted.
//
// Enter a nested scope with EnterNestedScope.  It will emit
// PUSHBLOCKSCOPE/ENTERWITH if needed, and arrange to record the PC bounds of
// the scope.  Leave a nested scope with LeaveNestedScope, which, for blocks,
// will emit DEBUGLEAVEBLOCK and may emit POPBLOCKSCOPE.  (For "with" scopes it
// emits LEAVEWITH, of course.)  Pass EnterNestedScope a fresh StmtInfoBCE
// object, and pass that same object to the corresponding LeaveNestedScope.  If
// the statement is a block scope, pass STMT_BLOCK as stmtType; otherwise for
// with scopes pass STMT_WITH.
//
static bool
EnterNestedScope(ExclusiveContext* cx, BytecodeEmitter* bce, StmtInfoBCE* stmt, ObjectBox* objbox,
                 StmtType stmtType)
{
    Rooted<NestedScopeObject*> scopeObj(cx, &objbox->object->as<NestedScopeObject>());
    uint32_t scopeObjectIndex = bce->objectList.add(objbox);

    switch (stmtType) {
      case STMT_BLOCK: {
        Rooted<StaticBlockObject*> blockObj(cx, &scopeObj->as<StaticBlockObject>());

        ComputeLocalOffset(cx, bce, blockObj);

        if (!ComputeAliasedSlots(cx, bce, blockObj))
            return false;

        if (blockObj->needsClone()) {
            if (!EmitInternedObjectOp(cx, scopeObjectIndex, JSOP_PUSHBLOCKSCOPE, bce))
                return false;
        }
        break;
      }
      case STMT_WITH:
        MOZ_ASSERT(scopeObj->is<StaticWithObject>());
        if (!EmitInternedObjectOp(cx, scopeObjectIndex, JSOP_ENTERWITH, bce))
            return false;
        break;
      default:
        MOZ_CRASH("Unexpected scope statement");
    }

    uint32_t parent = BlockScopeNote::NoBlockScopeIndex;
    if (StmtInfoBCE* stmt = bce->topScopeStmt) {
        for (; stmt->staticScope != bce->staticScope; stmt = stmt->down) {}
        parent = stmt->blockScopeIndex;
    }

    stmt->blockScopeIndex = bce->blockScopeList.length();
    if (!bce->blockScopeList.append(scopeObjectIndex, bce->offset(), parent))
        return false;

    PushStatementBCE(bce, stmt, stmtType, bce->offset());
    scopeObj->initEnclosingNestedScope(EnclosingStaticScope(bce));
    FinishPushNestedScope(bce, stmt, *scopeObj);
    MOZ_ASSERT(stmt->isNestedScope);
    stmt->isBlockScope = (stmtType == STMT_BLOCK);

    return true;
}

// Patches |breaks| and |continues| unless the top statement info record
// represents a try-catch-finally suite. May fail if a jump offset overflows.
static bool
PopStatementBCE(ExclusiveContext* cx, BytecodeEmitter* bce)
{
    StmtInfoBCE* stmt = bce->topStmt;
    if (!stmt->isTrying() &&
        (!BackPatch(cx, bce, stmt->breaks, bce->code().end(), JSOP_GOTO) ||
         !BackPatch(cx, bce, stmt->continues, bce->code(stmt->update), JSOP_GOTO)))
    {
        return false;
    }

    FinishPopStatement(bce);
    return true;
}

static bool
LeaveNestedScope(ExclusiveContext* cx, BytecodeEmitter* bce, StmtInfoBCE* stmt)
{
    MOZ_ASSERT(stmt == bce->topStmt);
    MOZ_ASSERT(stmt->isNestedScope);
    MOZ_ASSERT(stmt->isBlockScope == !(stmt->type == STMT_WITH));
    uint32_t blockScopeIndex = stmt->blockScopeIndex;

#ifdef DEBUG
    MOZ_ASSERT(bce->blockScopeList.list[blockScopeIndex].length == 0);
    uint32_t blockObjIndex = bce->blockScopeList.list[blockScopeIndex].index;
    ObjectBox* blockObjBox = bce->objectList.find(blockObjIndex);
    NestedScopeObject* staticScope = &blockObjBox->object->as<NestedScopeObject>();
    MOZ_ASSERT(stmt->staticScope == staticScope);
    MOZ_ASSERT(staticScope == bce->staticScope);
    MOZ_ASSERT_IF(!stmt->isBlockScope, staticScope->is<StaticWithObject>());
#endif

    if (!PopStatementBCE(cx, bce))
        return false;

    if (Emit1(cx, bce, stmt->isBlockScope ? JSOP_DEBUGLEAVEBLOCK : JSOP_LEAVEWITH) < 0)
        return false;

    bce->blockScopeList.recordEnd(blockScopeIndex, bce->offset());

    if (stmt->isBlockScope && stmt->staticScope->as<StaticBlockObject>().needsClone()) {
        if (Emit1(cx, bce, JSOP_POPBLOCKSCOPE) < 0)
            return false;
    }

    return true;
}

static bool
EmitIndex32(ExclusiveContext* cx, JSOp op, uint32_t index, BytecodeEmitter* bce)
{
    MOZ_ASSERT(CheckStrictOrSloppy(bce, op));
    const size_t len = 1 + UINT32_INDEX_LEN;
    MOZ_ASSERT(len == size_t(js_CodeSpec[op].length));
    ptrdiff_t offset = EmitCheck(cx, bce, len);
    if (offset < 0)
        return false;

    jsbytecode* code = bce->code(offset);
    code[0] = jsbytecode(op);
    SET_UINT32_INDEX(code, index);
    UpdateDepth(cx, bce, offset);
    CheckTypeSet(cx, bce, op);
    return true;
}

static bool
EmitIndexOp(ExclusiveContext* cx, JSOp op, uint32_t index, BytecodeEmitter* bce)
{
    MOZ_ASSERT(CheckStrictOrSloppy(bce, op));
    const size_t len = js_CodeSpec[op].length;
    MOZ_ASSERT(len >= 1 + UINT32_INDEX_LEN);
    ptrdiff_t offset = EmitCheck(cx, bce, len);
    if (offset < 0)
        return false;

    jsbytecode* code = bce->code(offset);
    code[0] = jsbytecode(op);
    SET_UINT32_INDEX(code, index);
    UpdateDepth(cx, bce, offset);
    CheckTypeSet(cx, bce, op);
    return true;
}

static bool
EmitAtomOp(ExclusiveContext* cx, JSAtom* atom, JSOp op, BytecodeEmitter* bce)
{
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);

    // .generator and .genrval lookups should be emitted as JSOP_GETALIASEDVAR
    // instead of JSOP_GETNAME etc, to bypass |with| objects on the scope chain.
    MOZ_ASSERT_IF(op == JSOP_GETNAME || op == JSOP_GETGNAME, !bce->sc->isDotVariable(atom));

    if (op == JSOP_GETPROP && atom == cx->names().length) {
        /* Specialize length accesses for the interpreter. */
        op = JSOP_LENGTH;
    }

    jsatomid index;
    if (!bce->makeAtomIndex(atom, &index))
        return false;

    return EmitIndexOp(cx, op, index, bce);
}

static bool
EmitAtomOp(ExclusiveContext* cx, ParseNode* pn, JSOp op, BytecodeEmitter* bce)
{
    MOZ_ASSERT(pn->pn_atom != nullptr);
    return EmitAtomOp(cx, pn->pn_atom, op, bce);
}

static bool
EmitInternedObjectOp(ExclusiveContext* cx, uint32_t index, JSOp op, BytecodeEmitter* bce)
{
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_OBJECT);
    MOZ_ASSERT(index < bce->objectList.length);
    return EmitIndex32(cx, op, index, bce);
}

static bool
EmitObjectOp(ExclusiveContext* cx, ObjectBox* objbox, JSOp op, BytecodeEmitter* bce)
{
    return EmitInternedObjectOp(cx, bce->objectList.add(objbox), op, bce);
}

static bool
EmitObjectPairOp(ExclusiveContext* cx, ObjectBox* objbox1, ObjectBox* objbox2, JSOp op,
                 BytecodeEmitter* bce)
{
    uint32_t index = bce->objectList.add(objbox1);
    bce->objectList.add(objbox2);
    return EmitInternedObjectOp(cx, index, op, bce);
}

static bool
EmitRegExp(ExclusiveContext* cx, uint32_t index, BytecodeEmitter* bce)
{
    return EmitIndex32(cx, JSOP_REGEXP, index, bce);
}

/*
 * To catch accidental misuse, EMIT_UINT16_IMM_OP/Emit3 assert that they are
 * not used to unconditionally emit JSOP_GETLOCAL. Variable access should
 * instead be emitted using EmitVarOp. In special cases, when the caller
 * definitely knows that a given local slot is unaliased, this function may be
 * used as a non-asserting version of EMIT_UINT16_IMM_OP.
 */
static bool
EmitLocalOp(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, uint32_t slot)
{
    MOZ_ASSERT(JOF_OPTYPE(op) != JOF_SCOPECOORD);
    MOZ_ASSERT(IsLocalOp(op));

    ptrdiff_t off = EmitN(cx, bce, op, LOCALNO_LEN);
    if (off < 0)
        return false;

    SET_LOCALNO(bce->code(off), slot);
    return true;
}

static bool
EmitUnaliasedVarOp(ExclusiveContext* cx, JSOp op, uint32_t slot, MaybeCheckLexical checkLexical,
                   BytecodeEmitter* bce)
{
    MOZ_ASSERT(JOF_OPTYPE(op) != JOF_SCOPECOORD);

    if (IsLocalOp(op)) {
        // Only unaliased locals have stack slots assigned to them. Convert the
        // var index (which includes unaliased and aliased locals) to the stack
        // slot index.
        MOZ_ASSERT(bce->localsToFrameSlots_[slot] <= slot);
        slot = bce->localsToFrameSlots_[slot];

        if (checkLexical) {
            MOZ_ASSERT(op != JSOP_INITLEXICAL);
            if (!EmitLocalOp(cx, bce, JSOP_CHECKLEXICAL, slot))
                return false;
        }

        return EmitLocalOp(cx, bce, op, slot);
    }

    MOZ_ASSERT(IsArgOp(op));
    ptrdiff_t off = EmitN(cx, bce, op, ARGNO_LEN);
    if (off < 0)
        return false;

    SET_ARGNO(bce->code(off), slot);
    return true;
}

static bool
EmitScopeCoordOp(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, ScopeCoordinate sc)
{
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_SCOPECOORD);

    unsigned n = SCOPECOORD_HOPS_LEN + SCOPECOORD_SLOT_LEN;
    MOZ_ASSERT(int(n) + 1 /* op */ == js_CodeSpec[op].length);

    ptrdiff_t off = EmitN(cx, bce, op, n);
    if (off < 0)
        return false;

    jsbytecode* pc = bce->code(off);
    SET_SCOPECOORD_HOPS(pc, sc.hops());
    pc += SCOPECOORD_HOPS_LEN;
    SET_SCOPECOORD_SLOT(pc, sc.slot());
    pc += SCOPECOORD_SLOT_LEN;
    CheckTypeSet(cx, bce, op);
    return true;
}

static bool
EmitAliasedVarOp(ExclusiveContext* cx, JSOp op, ScopeCoordinate sc, MaybeCheckLexical checkLexical,
                 BytecodeEmitter* bce)
{
    if (checkLexical) {
        MOZ_ASSERT(op != JSOP_INITALIASEDLEXICAL);
        if (!EmitScopeCoordOp(cx, bce, JSOP_CHECKALIASEDLEXICAL, sc))
            return false;
    }

    return EmitScopeCoordOp(cx, bce, op, sc);
}

// Compute the number of nested scope objects that will actually be on the scope
// chain at runtime, given the BCE's current staticScope.
static unsigned
DynamicNestedScopeDepth(BytecodeEmitter* bce)
{
    unsigned depth = 0;
    for (NestedScopeObject* b = bce->staticScope; b; b = b->enclosingNestedScope()) {
        if (!b->is<StaticBlockObject>() || b->as<StaticBlockObject>().needsClone())
            ++depth;
    }

    return depth;
}

static bool
LookupAliasedName(BytecodeEmitter* bce, HandleScript script, PropertyName* name, uint32_t* pslot,
                  ParseNode* pn = nullptr)
{
    LazyScript::FreeVariable* freeVariables = nullptr;
    uint32_t lexicalBegin = 0;
    uint32_t numFreeVariables = 0;
    if (bce->emitterMode == BytecodeEmitter::LazyFunction) {
        freeVariables = bce->lazyScript->freeVariables();
        lexicalBegin = script->bindings.lexicalBegin();
        numFreeVariables = bce->lazyScript->numFreeVariables();
    }

    /*
     * Beware: BindingIter may contain more than one Binding for a given name
     * (in the case of |function f(x,x) {}|) but only one will be aliased.
     */
    uint32_t bindingIndex = 0;
    uint32_t slot = CallObject::RESERVED_SLOTS;
    for (BindingIter bi(script); !bi.done(); bi++) {
        if (bi->aliased()) {
            if (bi->name() == name) {
                // Check if the free variable from a lazy script was marked as
                // a possible hoisted use and is a lexical binding. If so,
                // mark it as such so we emit a dead zone check.
                if (freeVariables) {
                    for (uint32_t i = 0; i < numFreeVariables; i++) {
                        if (freeVariables[i].atom() == name) {
                            if (freeVariables[i].isHoistedUse() && bindingIndex >= lexicalBegin) {
                                MOZ_ASSERT(pn);
                                MOZ_ASSERT(pn->isUsed());
                                pn->pn_dflags |= PND_LEXICAL;
                            }

                            break;
                        }
                    }
                }

                *pslot = slot;
                return true;
            }
            slot++;
        }
        bindingIndex++;
    }
    return false;
}

static bool
LookupAliasedNameSlot(BytecodeEmitter* bce, HandleScript script, PropertyName* name,
                      ScopeCoordinate* sc)
{
    uint32_t slot;
    if (!LookupAliasedName(bce, script, name, &slot))
        return false;

    sc->setSlot(slot);
    return true;
}

/*
 * Use this function instead of assigning directly to 'hops' to guard for
 * uint8_t overflows.
 */
static bool
AssignHops(BytecodeEmitter* bce, ParseNode* pn, unsigned src, ScopeCoordinate* dst)
{
    if (src > UINT8_MAX) {
        bce->reportError(pn, JSMSG_TOO_DEEP, js_function_str);
        return false;
    }

    dst->setHops(src);
    return true;
}

static inline MaybeCheckLexical
NodeNeedsCheckLexical(ParseNode* pn)
{
    return pn->isHoistedLexicalUse() ? CheckLexical : DontCheckLexical;
}

static bool
EmitAliasedVarOp(ExclusiveContext* cx, JSOp op, ParseNode* pn, BytecodeEmitter* bce)
{
    /*
     * While pn->pn_cookie tells us how many function scopes are between the use and the def this
     * is not the same as how many hops up the dynamic scope chain are needed. In particular:
     *  - a lexical function scope only contributes a hop if it is "heavyweight" (has a dynamic
     *    scope object).
     *  - a heavyweight named function scope contributes an extra scope to the scope chain (a
     *    DeclEnvObject that holds just the name).
     *  - all the intervening let/catch blocks must be counted.
     */
    unsigned skippedScopes = 0;
    BytecodeEmitter* bceOfDef = bce;
    if (pn->isUsed()) {
        /*
         * As explained in BindNameToSlot, the 'level' of a use indicates how
         * many function scopes (i.e., BytecodeEmitters) to skip to find the
         * enclosing function scope of the definition being accessed.
         */
        for (unsigned i = pn->pn_cookie.level(); i; i--) {
            skippedScopes += DynamicNestedScopeDepth(bceOfDef);
            FunctionBox* funbox = bceOfDef->sc->asFunctionBox();
            if (funbox->isHeavyweight()) {
                skippedScopes++;
                if (funbox->function()->isNamedLambda())
                    skippedScopes++;
            }
            bceOfDef = bceOfDef->parent;
        }
    } else {
        MOZ_ASSERT(pn->isDefn());
        MOZ_ASSERT(pn->pn_cookie.level() == bce->script->staticLevel());
    }

    /*
     * The final part of the skippedScopes computation depends on the type of
     * variable. An arg or local variable is at the outer scope of a function
     * and so includes the full DynamicNestedScopeDepth. A let/catch-binding
     * requires a search of the block chain to see how many (dynamic) block
     * objects to skip.
     */
    ScopeCoordinate sc;
    if (IsArgOp(pn->getOp())) {
        if (!AssignHops(bce, pn, skippedScopes + DynamicNestedScopeDepth(bceOfDef), &sc))
            return false;
        JS_ALWAYS_TRUE(LookupAliasedNameSlot(bceOfDef, bceOfDef->script, pn->name(), &sc));
    } else {
        MOZ_ASSERT(IsLocalOp(pn->getOp()) || pn->isKind(PNK_FUNCTION));
        uint32_t local = pn->pn_cookie.slot();
        if (local < bceOfDef->script->bindings.numBodyLevelLocals()) {
            if (!AssignHops(bce, pn, skippedScopes + DynamicNestedScopeDepth(bceOfDef), &sc))
                return false;
            JS_ALWAYS_TRUE(LookupAliasedNameSlot(bceOfDef, bceOfDef->script, pn->name(), &sc));
        } else {
            MOZ_ASSERT_IF(bce->sc->isFunctionBox(), local <= bceOfDef->script->bindings.numLocals());
            MOZ_ASSERT(bceOfDef->staticScope->is<StaticBlockObject>());
            Rooted<StaticBlockObject*> b(cx, &bceOfDef->staticScope->as<StaticBlockObject>());
            local = bceOfDef->localsToFrameSlots_[local];
            while (local < b->localOffset()) {
                if (b->needsClone())
                    skippedScopes++;
                b = &b->enclosingNestedScope()->as<StaticBlockObject>();
            }
            if (!AssignHops(bce, pn, skippedScopes, &sc))
                return false;
            sc.setSlot(b->localIndexToSlot(local));
        }
    }

    return EmitAliasedVarOp(cx, op, sc, NodeNeedsCheckLexical(pn), bce);
}

static bool
EmitVarOp(ExclusiveContext* cx, ParseNode* pn, JSOp op, BytecodeEmitter* bce)
{
    MOZ_ASSERT(pn->isKind(PNK_FUNCTION) || pn->isKind(PNK_NAME));
    MOZ_ASSERT(!pn->pn_cookie.isFree());

    if (IsAliasedVarOp(op)) {
        ScopeCoordinate sc;
        sc.setHops(pn->pn_cookie.level());
        sc.setSlot(pn->pn_cookie.slot());
        return EmitAliasedVarOp(cx, op, sc, NodeNeedsCheckLexical(pn), bce);
    }

    MOZ_ASSERT_IF(pn->isKind(PNK_NAME), IsArgOp(op) || IsLocalOp(op));

    if (!bce->isAliasedName(pn)) {
        MOZ_ASSERT(pn->isUsed() || pn->isDefn());
        MOZ_ASSERT_IF(pn->isUsed(), pn->pn_cookie.level() == 0);
        MOZ_ASSERT_IF(pn->isDefn(), pn->pn_cookie.level() == bce->script->staticLevel());
        return EmitUnaliasedVarOp(cx, op, pn->pn_cookie.slot(), NodeNeedsCheckLexical(pn), bce);
    }

    switch (op) {
      case JSOP_GETARG: case JSOP_GETLOCAL: op = JSOP_GETALIASEDVAR; break;
      case JSOP_SETARG: case JSOP_SETLOCAL: op = JSOP_SETALIASEDVAR; break;
      case JSOP_INITLEXICAL: op = JSOP_INITALIASEDLEXICAL; break;
      default: MOZ_CRASH("unexpected var op");
    }

    return EmitAliasedVarOp(cx, op, pn, bce);
}

static JSOp
GetIncDecInfo(ParseNodeKind kind, bool* post)
{
    MOZ_ASSERT(kind == PNK_POSTINCREMENT || kind == PNK_PREINCREMENT ||
               kind == PNK_POSTDECREMENT || kind == PNK_PREDECREMENT);
    *post = kind == PNK_POSTINCREMENT || kind == PNK_POSTDECREMENT;
    return (kind == PNK_POSTINCREMENT || kind == PNK_PREINCREMENT) ? JSOP_ADD : JSOP_SUB;
}

static bool
EmitVarIncDec(ExclusiveContext* cx, ParseNode* pn, BytecodeEmitter* bce)
{
    JSOp op = pn->pn_kid->getOp();
    MOZ_ASSERT(IsArgOp(op) || IsLocalOp(op) || IsAliasedVarOp(op));
    MOZ_ASSERT(pn->pn_kid->isKind(PNK_NAME));
    MOZ_ASSERT(!pn->pn_kid->pn_cookie.isFree());

    bool post;
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    JSOp getOp, setOp;
    if (IsLocalOp(op)) {
        getOp = JSOP_GETLOCAL;
        setOp = JSOP_SETLOCAL;
    } else if (IsArgOp(op)) {
        getOp = JSOP_GETARG;
        setOp = JSOP_SETARG;
    } else {
        getOp = JSOP_GETALIASEDVAR;
        setOp = JSOP_SETALIASEDVAR;
    }

    if (!EmitVarOp(cx, pn->pn_kid, getOp, bce))              // V
        return false;
    if (Emit1(cx, bce, JSOP_POS) < 0)                        // N
        return false;
    if (post && Emit1(cx, bce, JSOP_DUP) < 0)                // N? N
        return false;
    if (Emit1(cx, bce, JSOP_ONE) < 0)                        // N? N 1
        return false;
    if (Emit1(cx, bce, binop) < 0)                           // N? N+1
        return false;
    if (!EmitVarOp(cx, pn->pn_kid, setOp, bce))              // N? N+1
        return false;
    if (post && Emit1(cx, bce, JSOP_POP) < 0)                // RESULT
        return false;

    return true;
}

bool
BytecodeEmitter::isAliasedName(ParseNode* pn)
{
    Definition* dn = pn->resolve();
    MOZ_ASSERT(dn->isDefn());
    MOZ_ASSERT(!dn->isPlaceholder());
    MOZ_ASSERT(dn->isBound());

    /* If dn is in an enclosing function, it is definitely aliased. */
    if (dn->pn_cookie.level() != script->staticLevel())
        return true;

    switch (dn->kind()) {
      case Definition::LET:
      case Definition::CONST:
        /*
         * There are two ways to alias a let variable: nested functions and
         * dynamic scope operations. (This is overly conservative since the
         * bindingsAccessedDynamically flag, checked by allLocalsAliased, is
         * function-wide.)
         *
         * In addition all locals in generators are marked as aliased, to ensure
         * that they are allocated on scope chains instead of on the stack.  See
         * the definition of SharedContext::allLocalsAliased.
         */
        return dn->isClosed() || sc->allLocalsAliased();
      case Definition::ARG:
        /*
         * Consult the bindings, since they already record aliasing. We might
         * be tempted to use the same definition as VAR/CONST/LET, but there is
         * a problem caused by duplicate arguments: only the last argument with
         * a given name is aliased. This is necessary to avoid generating a
         * shape for the call object with with more than one name for a given
         * slot (which violates internal engine invariants). All this means that
         * the '|| sc->allLocalsAliased()' disjunct is incorrect since it will
         * mark both parameters in function(x,x) as aliased.
         */
        return script->formalIsAliased(pn->pn_cookie.slot());
      case Definition::VAR:
      case Definition::GLOBALCONST:
        MOZ_ASSERT_IF(sc->allLocalsAliased(), script->cookieIsAliased(pn->pn_cookie));
        return script->cookieIsAliased(pn->pn_cookie);
      case Definition::PLACEHOLDER:
      case Definition::NAMED_LAMBDA:
      case Definition::MISSING:
        MOZ_CRASH("unexpected dn->kind");
    }
    return false;
}

static JSOp
StrictifySetNameOp(JSOp op, BytecodeEmitter* bce)
{
    switch (op) {
      case JSOP_SETNAME:
        if (bce->sc->strict)
            op = JSOP_STRICTSETNAME;
        break;
      case JSOP_SETGNAME:
        if (bce->sc->strict)
            op = JSOP_STRICTSETGNAME;
        break;
        default:;
    }
    return op;
}

static void
StrictifySetNameNode(ParseNode* pn, BytecodeEmitter* bce)
{
    pn->setOp(StrictifySetNameOp(pn->getOp(), bce));
}

/*
 * Try to convert a *NAME op with a free name to a more specialized GNAME,
 * INTRINSIC or ALIASEDVAR op, which optimize accesses on that name.
 * Return true if a conversion was made.
 */
static bool
TryConvertFreeName(BytecodeEmitter* bce, ParseNode* pn)
{
    /*
     * In self-hosting mode, JSOP_*NAME is unconditionally converted to
     * JSOP_*INTRINSIC. This causes lookups to be redirected to the special
     * intrinsics holder in the global object, into which any missing values are
     * cloned lazily upon first access.
     */
    if (bce->emitterMode == BytecodeEmitter::SelfHosting) {
        JSOp op;
        switch (pn->getOp()) {
          case JSOP_GETNAME:  op = JSOP_GETINTRINSIC; break;
          case JSOP_SETNAME:  op = JSOP_SETINTRINSIC; break;
          /* Other *NAME ops aren't (yet) supported in self-hosted code. */
          default: MOZ_CRASH("intrinsic");
        }
        pn->setOp(op);
        return true;
    }

    /*
     * When parsing inner functions lazily, parse nodes for outer functions no
     * longer exist and only the function's scope chain is available for
     * resolving upvar accesses within the inner function.
     */
    if (bce->emitterMode == BytecodeEmitter::LazyFunction) {
        // The only statements within a lazy function which can push lexical
        // scopes are try/catch blocks. Use generic ops in this case.
        for (StmtInfoBCE* stmt = bce->topStmt; stmt; stmt = stmt->down) {
            if (stmt->type == STMT_CATCH)
                return true;
        }

        size_t hops = 0;
        FunctionBox* funbox = bce->sc->asFunctionBox();
        if (funbox->hasExtensibleScope())
            return false;
        if (funbox->function()->isNamedLambda() && funbox->function()->atom() == pn->pn_atom)
            return false;
        if (funbox->isHeavyweight()) {
            hops++;
            if (funbox->function()->isNamedLambda())
                hops++;
        }
        if (bce->script->directlyInsideEval())
            return false;
        RootedObject outerScope(bce->sc->context, bce->script->enclosingStaticScope());
        for (StaticScopeIter<CanGC> ssi(bce->sc->context, outerScope); !ssi.done(); ssi++) {
            if (ssi.type() != StaticScopeIter<CanGC>::Function) {
                if (ssi.type() == StaticScopeIter<CanGC>::Block) {
                    // Use generic ops if a catch block is encountered.
                    return false;
                }
                if (ssi.hasDynamicScopeObject())
                    hops++;
                continue;
            }
            RootedScript script(bce->sc->context, ssi.funScript());
            if (script->functionNonDelazifying()->atom() == pn->pn_atom)
                return false;
            if (ssi.hasDynamicScopeObject()) {
                uint32_t slot;
                if (LookupAliasedName(bce, script, pn->pn_atom->asPropertyName(), &slot, pn)) {
                    JSOp op;
                    switch (pn->getOp()) {
                      case JSOP_GETNAME: op = JSOP_GETALIASEDVAR; break;
                      case JSOP_SETNAME: op = JSOP_SETALIASEDVAR; break;
                      default: return false;
                    }

                    pn->setOp(op);
                    JS_ALWAYS_TRUE(pn->pn_cookie.set(bce->parser->tokenStream, hops, slot));
                    return true;
                }
                hops++;
            }

            if (script->funHasExtensibleScope() || script->directlyInsideEval())
                return false;
        }
    }

    // Unbound names aren't recognizable global-property references if the
    // script isn't running against its global object.
    if (!bce->script->compileAndGo() || !bce->hasGlobalScope)
        return false;

    // Deoptimized names also aren't necessarily globals.
    if (pn->isDeoptimized())
        return false;

    if (bce->sc->isFunctionBox()) {
        // Unbound names in function code may not be globals if new locals can
        // be added to this function (or an enclosing one) to alias a global
        // reference.
        FunctionBox* funbox = bce->sc->asFunctionBox();
        if (funbox->mightAliasLocals())
            return false;
    }

    // If this is eval code, being evaluated inside strict mode eval code,
    // an "unbound" name might be a binding local to that outer eval:
    //
    //   var x = "GLOBAL";
    //   eval('"use strict"; ' +
    //        'var x; ' +
    //        'eval("print(x)");'); // "undefined", not "GLOBAL"
    //
    // Given the enclosing eval code's strictness and its bindings (neither is
    // readily available now), we could exactly check global-ness, but it's not
    // worth the trouble for doubly-nested eval code.  So we conservatively
    // approximate.  If the outer eval code is strict, then this eval code will
    // be: thus, don't optimize if we're compiling strict code inside an eval.
    if (bce->insideEval && bce->sc->strict)
        return false;

    JSOp op;
    switch (pn->getOp()) {
      case JSOP_GETNAME:  op = JSOP_GETGNAME; break;
      case JSOP_SETNAME:  op = StrictifySetNameOp(JSOP_SETGNAME, bce); break;
      case JSOP_SETCONST:
        // Not supported.
        return false;
      default: MOZ_CRASH("gname");
    }
    pn->setOp(op);
    return true;
}

/*
 * BindNameToSlotHelper attempts to optimize name gets and sets to stack slot
 * loads and stores, given the compile-time information in bce and a PNK_NAME
 * node pn.  It returns false on error, true on success.
 *
 * The caller can test pn->pn_cookie.isFree() to tell whether optimization
 * occurred, in which case BindNameToSlotHelper also updated pn->pn_op.  If
 * pn->pn_cookie.isFree() is still true on return, pn->pn_op still may have
 * been optimized, e.g., from JSOP_GETNAME to JSOP_CALLEE.  Whether or not
 * pn->pn_op was modified, if this function finds an argument or local variable
 * name, PND_CONST will be set in pn_dflags for read-only properties after a
 * successful return.
 *
 * NB: if you add more opcodes specialized from JSOP_GETNAME, etc., don't forget
 * to update the special cases in EmitFor (for-in) and EmitAssignment (= and
 * op=, e.g. +=).
 */
static bool
BindNameToSlotHelper(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_NAME));

    /* Don't attempt if 'pn' is already bound or deoptimized or a function. */
    if (pn->isBound() || pn->isDeoptimized())
        return true;

    /* JSOP_CALLEE is pre-bound by definition. */
    JSOp op = pn->getOp();
    MOZ_ASSERT(op != JSOP_CALLEE);
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);

    /*
     * The parser already linked name uses to definitions when (where not
     * prevented by non-lexical constructs like 'with' and 'eval').
     */
    Definition* dn;
    if (pn->isUsed()) {
        MOZ_ASSERT(pn->pn_cookie.isFree());
        dn = pn->pn_lexdef;
        MOZ_ASSERT(dn->isDefn());
        pn->pn_dflags |= (dn->pn_dflags & PND_CONST);
    } else if (pn->isDefn()) {
        dn = (Definition*) pn;
    } else {
        return true;
    }

    // Throw an error on attempts to mutate const-declared bindings.
    switch (op) {
      case JSOP_GETNAME:
      case JSOP_SETCONST:
        break;
      default:
        if (pn->isConst()) {
            JSAutoByteString name;
            if (!AtomToPrintableString(cx, pn->pn_atom, &name))
                return false;
            bce->reportError(pn, JSMSG_BAD_CONST_ASSIGN, name.ptr());
            return false;
        }
    }

    if (dn->pn_cookie.isFree()) {
        if (HandleScript caller = bce->evalCaller) {
            MOZ_ASSERT(bce->script->compileAndGo());

            /*
             * Don't generate upvars on the left side of a for loop. See
             * bug 470758.
             */
            if (bce->emittingForInit)
                return true;

            /*
             * If this is an eval in the global scope, then unbound variables
             * must be globals, so try to use GNAME ops.
             */
            if (!caller->functionOrCallerFunction() && TryConvertFreeName(bce, pn)) {
                pn->pn_dflags |= PND_BOUND;
                return true;
            }

            /*
             * Out of tricks, so we must rely on PICs to optimize named
             * accesses from direct eval called from function code.
             */
            return true;
        }

        /* Optimize accesses to undeclared globals. */
        if (!TryConvertFreeName(bce, pn))
            return true;

        pn->pn_dflags |= PND_BOUND;
        return true;
    }

    /*
     * At this point, we are only dealing with uses that have already been
     * bound to definitions via pn_lexdef. The rest of this routine converts
     * the parse node of the use from its initial JSOP_*NAME* op to a LOCAL/ARG
     * op. This requires setting the node's pn_cookie with a pair (level, slot)
     * where 'level' is the number of function scopes between the use and the
     * def and 'slot' is the index to emit as the immediate of the ARG/LOCAL
     * op. For example, in this code:
     *
     *   function(a,b,x) { return x }
     *   function(y) { function() { return y } }
     *
     * x will get (level = 0, slot = 2) and y will get (level = 1, slot = 0).
     */
    MOZ_ASSERT(!pn->isDefn());
    MOZ_ASSERT(pn->isUsed());
    MOZ_ASSERT(pn->pn_lexdef);
    MOZ_ASSERT(pn->pn_cookie.isFree());

    /*
     * We are compiling a function body and may be able to optimize name
     * to stack slot. Look for an argument or variable in the function and
     * rewrite pn_op and update pn accordingly.
     */
    switch (dn->kind()) {
      case Definition::ARG:
        switch (op) {
          case JSOP_GETNAME:
            op = JSOP_GETARG; break;
          case JSOP_SETNAME:
          case JSOP_STRICTSETNAME:
            op = JSOP_SETARG; break;
          default: MOZ_CRASH("arg");
        }
        MOZ_ASSERT(!pn->isConst());
        break;

      case Definition::VAR:
      case Definition::GLOBALCONST:
      case Definition::CONST:
      case Definition::LET:
        switch (op) {
          case JSOP_GETNAME:
            op = JSOP_GETLOCAL; break;
          case JSOP_SETNAME:
          case JSOP_STRICTSETNAME:
            op = JSOP_SETLOCAL; break;
          case JSOP_SETCONST:
            op = JSOP_SETLOCAL; break;
          default: MOZ_CRASH("local");
        }
        break;

      case Definition::NAMED_LAMBDA: {
        MOZ_ASSERT(dn->isOp(JSOP_CALLEE));
        MOZ_ASSERT(op != JSOP_CALLEE);

        /*
         * Currently, the ALIASEDVAR ops do not support accessing the
         * callee of a DeclEnvObject, so use NAME.
         */
        if (dn->pn_cookie.level() != bce->script->staticLevel())
            return true;

        DebugOnly<JSFunction*> fun = bce->sc->asFunctionBox()->function();
        MOZ_ASSERT(fun->isLambda());
        MOZ_ASSERT(pn->pn_atom == fun->atom());

        /*
         * Leave pn->isOp(JSOP_GETNAME) if bce->fun is heavyweight to
         * address two cases: a new binding introduced by eval, and
         * assignment to the name in strict mode.
         *
         *   var fun = (function f(s) { eval(s); return f; });
         *   assertEq(fun("var f = 42"), 42);
         *
         * ECMAScript specifies that a function expression's name is bound
         * in a lexical environment distinct from that used to bind its
         * named parameters, the arguments object, and its variables.  The
         * new binding for "var f = 42" shadows the binding for the
         * function itself, so the name of the function will not refer to
         * the function.
         *
         *    (function f() { "use strict"; f = 12; })();
         *
         * Outside strict mode, assignment to a function expression's name
         * has no effect.  But in strict mode, this attempt to mutate an
         * immutable binding must throw a TypeError.  We implement this by
         * not optimizing such assignments and by marking such functions as
         * heavyweight, ensuring that the function name is represented in
         * the scope chain so that assignment will throw a TypeError.
         */
        if (!bce->sc->asFunctionBox()->isHeavyweight()) {
            op = JSOP_CALLEE;
            pn->pn_dflags |= PND_CONST;
        }

        pn->setOp(op);
        pn->pn_dflags |= PND_BOUND;
        return true;
      }

      case Definition::PLACEHOLDER:
        return true;

      case Definition::MISSING:
        MOZ_CRASH("missing");
    }

    /*
     * The difference between the current static level and the static level of
     * the definition is the number of function scopes between the current
     * scope and dn's scope.
     */
    unsigned skip = bce->script->staticLevel() - dn->pn_cookie.level();
    MOZ_ASSERT_IF(skip, dn->isClosed());

    /*
     * Explicitly disallow accessing var/let bindings in global scope from
     * nested functions. The reason for this limitation is that, since the
     * global script is not included in the static scope chain (1. because it
     * has no object to stand in the static scope chain, 2. to minimize memory
     * bloat where a single live function keeps its whole global script
     * alive.), ScopeCoordinateToTypeSet is not able to find the var/let's
     * associated TypeSet.
     */
    if (skip) {
        BytecodeEmitter* bceSkipped = bce;
        for (unsigned i = 0; i < skip; i++)
            bceSkipped = bceSkipped->parent;
        if (!bceSkipped->sc->isFunctionBox())
            return true;
    }

    MOZ_ASSERT(!pn->isOp(op));
    pn->setOp(op);
    if (!pn->pn_cookie.set(bce->parser->tokenStream, skip, dn->pn_cookie.slot()))
        return false;

    pn->pn_dflags |= PND_BOUND;
    return true;
}

/*
 * Attempts to bind the name, then checks that no dynamic scope lookup ops are
 * emitted in self-hosting mode. NAME ops do lookups off current scope chain,
 * and we do not want to allow self-hosted code to use the dynamic scope.
 */
static bool
BindNameToSlot(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    if (!BindNameToSlotHelper(cx, bce, pn))
        return false;

    StrictifySetNameNode(pn, bce);

    if (bce->emitterMode == BytecodeEmitter::SelfHosting && !pn->isBound()) {
        bce->reportError(pn, JSMSG_SELFHOSTED_UNBOUND_NAME);
        return false;
    }

    return true;
}

/*
 * If pn contains a useful expression, return true with *answer set to true.
 * If pn contains a useless expression, return true with *answer set to false.
 * Return false on error.
 *
 * The caller should initialize *answer to false and invoke this function on
 * an expression statement or similar subtree to decide whether the tree could
 * produce code that has any side effects.  For an expression statement, we
 * define useless code as code with no side effects, because the main effect,
 * the value left on the stack after the code executes, will be discarded by a
 * pop bytecode.
 */
static bool
CheckSideEffects(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, bool* answer)
{
    if (!pn || *answer)
        return true;

    switch (pn->getArity()) {
      case PN_CODE:
        /*
         * A named function, contrary to ES3, is no longer useful, because we
         * bind its name lexically (using JSOP_CALLEE) instead of creating an
         * Object instance and binding a readonly, permanent property in it
         * (the object and binding can be detected and hijacked or captured).
         * This is a bug fix to ES3; it is fixed in ES3.1 drafts.
         */
        MOZ_ASSERT(*answer == false);
        return true;

      case PN_LIST:
        if (pn->isOp(JSOP_NOP) || pn->isOp(JSOP_OR) || pn->isOp(JSOP_AND) ||
            pn->isOp(JSOP_STRICTEQ) || pn->isOp(JSOP_STRICTNE)) {
            /*
             * Non-operators along with ||, &&, ===, and !== never invoke
             * toString or valueOf.
             */
            bool ok = true;
            for (ParseNode* pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next)
                ok &= CheckSideEffects(cx, bce, pn2, answer);
            return ok;
        }

        if (pn->isKind(PNK_GENEXP)) {
            /* Generator-expressions are harmless if the result is ignored. */
            MOZ_ASSERT(*answer == false);
            return true;
        }

        /*
         * All invocation operations (construct: PNK_NEW, call: PNK_CALL)
         * are presumed to be useful, because they may have side effects
         * even if their main effect (their return value) is discarded.
         *
         * PNK_ELEM binary trees of 3+ nodes are flattened into lists to
         * avoid too much recursion. All such lists must be presumed to be
         * useful because each index operation could invoke a getter.
         *
         * Likewise, array and object initialisers may call prototype
         * setters (the __defineSetter__ built-in, and writable __proto__
         * on Array.prototype create this hazard). Initialiser list nodes
         * have JSOP_NEWINIT in their pn_op.
         */
        *answer = true;
        return true;

      case PN_TERNARY:
        return CheckSideEffects(cx, bce, pn->pn_kid1, answer) &&
               CheckSideEffects(cx, bce, pn->pn_kid2, answer) &&
               CheckSideEffects(cx, bce, pn->pn_kid3, answer);

      case PN_BINARY:
      case PN_BINARY_OBJ:
        if (pn->isAssignment()) {
            /*
             * Assignment is presumed to be useful, even if the next operation
             * is another assignment overwriting this one's ostensible effect,
             * because the left operand may be a property with a setter that
             * has side effects.
             *
             * The only exception is assignment of a useless value to a const
             * declared in the function currently being compiled.
             */
            ParseNode* pn2 = pn->pn_left;
            if (!pn2->isKind(PNK_NAME)) {
                *answer = true;
            } else {
                if (!BindNameToSlot(cx, bce, pn2))
                    return false;
                if (!CheckSideEffects(cx, bce, pn->pn_right, answer))
                    return false;
                if (!*answer && (!pn->isOp(JSOP_NOP) || !pn2->isConst()))
                    *answer = true;
            }
            return true;
        }

        MOZ_ASSERT(!pn->isOp(JSOP_OR), "|| produces a list now");
        MOZ_ASSERT(!pn->isOp(JSOP_AND), "&& produces a list now");
        MOZ_ASSERT(!pn->isOp(JSOP_STRICTEQ), "=== produces a list now");
        MOZ_ASSERT(!pn->isOp(JSOP_STRICTNE), "!== produces a list now");

        /*
         * We can't easily prove that neither operand ever denotes an
         * object with a toString or valueOf method.
         */
        *answer = true;
        return true;

      case PN_UNARY:
        switch (pn->getKind()) {
          case PNK_DELETE:
          {
            ParseNode* pn2 = pn->pn_kid;
            switch (pn2->getKind()) {
              case PNK_NAME:
                if (!BindNameToSlot(cx, bce, pn2))
                    return false;
                if (pn2->isConst()) {
                    MOZ_ASSERT(*answer == false);
                    return true;
                }
                /* FALL THROUGH */
              case PNK_DOT:
              case PNK_CALL:
              case PNK_ELEM:
                /* All these delete addressing modes have effects too. */
                *answer = true;
                return true;
              default:
                return CheckSideEffects(cx, bce, pn2, answer);
            }
            MOZ_CRASH("We have a returning default case");
          }

          case PNK_TYPEOF:
          case PNK_VOID:
          case PNK_NOT:
          case PNK_BITNOT:
            if (pn->isOp(JSOP_NOT)) {
                /* ! does not convert its operand via toString or valueOf. */
                return CheckSideEffects(cx, bce, pn->pn_kid, answer);
            }
            /* FALL THROUGH */

          default:
            /*
             * All of PNK_INC, PNK_DEC and PNK_THROW have direct effects. Of
             * the remaining unary-arity node types, we can't easily prove that
             * the operand never denotes an object with a toString or valueOf
             * method.
             */
            *answer = true;
            return true;
        }
        MOZ_CRASH("We have a returning default case");

      case PN_NAME:
        /*
         * Take care to avoid trying to bind a label name (labels, both for
         * statements and property values in object initialisers, have pn_op
         * defaulted to JSOP_NOP).
         */
        if (pn->isKind(PNK_NAME) && !pn->isOp(JSOP_NOP)) {
            if (!BindNameToSlot(cx, bce, pn))
                return false;
            if (!pn->isOp(JSOP_CALLEE) && pn->pn_cookie.isFree()) {
                /*
                 * Not a use of an unshadowed named function expression's given
                 * name, so this expression could invoke a getter that has side
                 * effects.
                 */
                *answer = true;
            }
        }

        if (pn->isHoistedLexicalUse()) {
            // Hoisted uses of lexical bindings throw on access.
            *answer = true;
        }

        if (pn->isKind(PNK_DOT)) {
            /* Dotted property references in general can call getters. */
            *answer = true;
        }
        return CheckSideEffects(cx, bce, pn->maybeExpr(), answer);

      case PN_NULLARY:
        if (pn->isKind(PNK_DEBUGGER))
            *answer = true;
        return true;
    }
    return true;
}

bool
BytecodeEmitter::isInLoop()
{
    for (StmtInfoBCE* stmt = topStmt; stmt; stmt = stmt->down) {
        if (stmt->isLoop())
            return true;
    }
    return false;
}

bool
BytecodeEmitter::checkSingletonContext()
{
    if (!script->compileAndGo() || sc->isFunctionBox() || isInLoop())
        return false;
    hasSingletons = true;
    return true;
}

bool
BytecodeEmitter::needsImplicitThis()
{
    if (!script->compileAndGo())
        return true;

    if (sc->isFunctionBox()) {
        if (sc->asFunctionBox()->inWith)
            return true;
    } else {
        JSObject* scope = sc->asGlobalSharedContext()->scopeChain();
        while (scope) {
            if (scope->is<DynamicWithObject>())
                return true;
            scope = scope->enclosingScope();
        }
    }

    for (StmtInfoBCE* stmt = topStmt; stmt; stmt = stmt->down) {
        if (stmt->type == STMT_WITH)
            return true;
    }
    return false;
}

void
BytecodeEmitter::tellDebuggerAboutCompiledScript(ExclusiveContext* cx)
{
    // Note: when parsing off thread the resulting scripts need to be handed to
    // the debugger after rejoining to the main thread.
    if (!cx->isJSContext())
        return;

    // Lazy scripts are never top level (despite always being invoked with a
    // nullptr parent), and so the hook should never be fired.
    if (emitterMode != LazyFunction && !parent) {
        Debugger::onNewScript(cx->asJSContext(), script);
    }
}

inline TokenStream*
BytecodeEmitter::tokenStream()
{
    return &parser->tokenStream;
}

bool
BytecodeEmitter::reportError(ParseNode* pn, unsigned errorNumber, ...)
{
    TokenPos pos = pn ? pn->pn_pos : tokenStream()->currentToken().pos;

    va_list args;
    va_start(args, errorNumber);
    bool result = tokenStream()->reportCompileErrorNumberVA(pos.begin, JSREPORT_ERROR,
                                                            errorNumber, args);
    va_end(args);
    return result;
}

bool
BytecodeEmitter::reportStrictWarning(ParseNode* pn, unsigned errorNumber, ...)
{
    TokenPos pos = pn ? pn->pn_pos : tokenStream()->currentToken().pos;

    va_list args;
    va_start(args, errorNumber);
    bool result = tokenStream()->reportStrictWarningErrorNumberVA(pos.begin, errorNumber, args);
    va_end(args);
    return result;
}

bool
BytecodeEmitter::reportStrictModeError(ParseNode* pn, unsigned errorNumber, ...)
{
    TokenPos pos = pn ? pn->pn_pos : tokenStream()->currentToken().pos;

    va_list args;
    va_start(args, errorNumber);
    bool result = tokenStream()->reportStrictModeErrorNumberVA(pos.begin, sc->strict,
                                                               errorNumber, args);
    va_end(args);
    return result;
}

static bool
EmitNewInit(ExclusiveContext* cx, BytecodeEmitter* bce, JSProtoKey key)
{
    const size_t len = 1 + UINT32_INDEX_LEN;
    ptrdiff_t offset = EmitCheck(cx, bce, len);
    if (offset < 0)
        return false;

    jsbytecode* code = bce->code(offset);
    code[0] = JSOP_NEWINIT;
    code[1] = jsbytecode(key);
    code[2] = 0;
    code[3] = 0;
    code[4] = 0;
    UpdateDepth(cx, bce, offset);
    CheckTypeSet(cx, bce, JSOP_NEWINIT);
    return true;
}

static bool
IteratorResultShape(ExclusiveContext* cx, BytecodeEmitter* bce, unsigned* shape)
{
    MOZ_ASSERT(bce->script->compileAndGo());

    RootedPlainObject obj(cx);
    gc::AllocKind kind = GuessObjectGCKind(2);
    obj = NewBuiltinClassInstance<PlainObject>(cx, kind);
    if (!obj)
        return false;

    Rooted<jsid> value_id(cx, AtomToId(cx->names().value));
    Rooted<jsid> done_id(cx, AtomToId(cx->names().done));
    if (!NativeDefineProperty(cx, obj, value_id, UndefinedHandleValue, nullptr, nullptr,
                              JSPROP_ENUMERATE))
    {
        return false;
    }
    if (!NativeDefineProperty(cx, obj, done_id, UndefinedHandleValue, nullptr, nullptr,
                              JSPROP_ENUMERATE))
    {
        return false;
    }

    ObjectBox* objbox = bce->parser->newObjectBox(obj);
    if (!objbox)
        return false;

    *shape = bce->objectList.add(objbox);

    return true;
}

static bool
EmitPrepareIteratorResult(ExclusiveContext* cx, BytecodeEmitter* bce)
{
    if (bce->script->compileAndGo()) {
        unsigned shape;
        if (!IteratorResultShape(cx, bce, &shape))
            return false;
        return EmitIndex32(cx, JSOP_NEWOBJECT, shape, bce);
    }

    return EmitNewInit(cx, bce, JSProto_Object);
}

static bool
EmitFinishIteratorResult(ExclusiveContext* cx, BytecodeEmitter* bce, bool done)
{
    jsatomid value_id;
    if (!bce->makeAtomIndex(cx->names().value, &value_id))
        return false;
    jsatomid done_id;
    if (!bce->makeAtomIndex(cx->names().done, &done_id))
        return false;

    if (!EmitIndex32(cx, JSOP_INITPROP, value_id, bce))
        return false;
    if (Emit1(cx, bce, done ? JSOP_TRUE : JSOP_FALSE) < 0)
        return false;
    if (!EmitIndex32(cx, JSOP_INITPROP, done_id, bce))
        return false;
    return true;
}

static bool
EmitNameOp(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, bool callContext)
{
    if (!BindNameToSlot(cx, bce, pn))
        return false;

    JSOp op = pn->getOp();

    if (op == JSOP_CALLEE) {
        if (Emit1(cx, bce, op) < 0)
            return false;
    } else {
        if (!pn->pn_cookie.isFree()) {
            MOZ_ASSERT(JOF_OPTYPE(op) != JOF_ATOM);
            if (!EmitVarOp(cx, pn, op, bce))
                return false;
        } else {
            if (!EmitAtomOp(cx, pn, op, bce))
                return false;
        }
    }

    /* Need to provide |this| value for call */
    if (callContext) {
        if (op == JSOP_GETNAME && bce->needsImplicitThis()) {
            if (!EmitAtomOp(cx, pn, JSOP_IMPLICITTHIS, bce))
                return false;
        } else {
            if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
                return false;
        }
    }

    return true;
}

static bool
EmitPropLHS(ExclusiveContext* cx, ParseNode* pn, JSOp op, BytecodeEmitter* bce)
{
    MOZ_ASSERT(pn->isKind(PNK_DOT));
    ParseNode* pn2 = pn->maybeExpr();

    /*
     * If the object operand is also a dotted property reference, reverse the
     * list linked via pn_expr temporarily so we can iterate over it from the
     * bottom up (reversing again as we go), to avoid excessive recursion.
     */
    if (pn2->isKind(PNK_DOT)) {
        ParseNode* pndot = pn2;
        ParseNode* pnup = nullptr, *pndown;
        ptrdiff_t top = bce->offset();
        for (;;) {
            /* Reverse pndot->pn_expr to point up, not down. */
            pndot->pn_offset = top;
            MOZ_ASSERT(!pndot->isUsed());
            pndown = pndot->pn_expr;
            pndot->pn_expr = pnup;
            if (!pndown->isKind(PNK_DOT))
                break;
            pnup = pndot;
            pndot = pndown;
        }

        /* pndown is a primary expression, not a dotted property reference. */
        if (!EmitTree(cx, bce, pndown))
            return false;

        do {
            /* Walk back up the list, emitting annotated name ops. */
            if (!EmitAtomOp(cx, pndot, JSOP_GETPROP, bce))
                return false;

            /* Reverse the pn_expr link again. */
            pnup = pndot->pn_expr;
            pndot->pn_expr = pndown;
            pndown = pndot;
        } while ((pndot = pnup) != nullptr);
        return true;
    }

    // The non-optimized case.
    return EmitTree(cx, bce, pn2);
}

static bool
EmitPropOp(ExclusiveContext* cx, ParseNode* pn, JSOp op, BytecodeEmitter* bce)
{
    MOZ_ASSERT(pn->isArity(PN_NAME));

    if (!EmitPropLHS(cx, pn, op, bce))
        return false;

    if (op == JSOP_CALLPROP && Emit1(cx, bce, JSOP_DUP) < 0)
        return false;

    if (!EmitAtomOp(cx, pn, op, bce))
        return false;

    if (op == JSOP_CALLPROP && Emit1(cx, bce, JSOP_SWAP) < 0)
        return false;

    return true;
}

static bool
EmitPropIncDec(ExclusiveContext* cx, ParseNode* pn, BytecodeEmitter* bce)
{
    MOZ_ASSERT(pn->pn_kid->getKind() == PNK_DOT);

    bool post;
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    JSOp get = JSOP_GETPROP;
    if (!EmitPropLHS(cx, pn->pn_kid, get, bce))     // OBJ
        return false;
    if (Emit1(cx, bce, JSOP_DUP) < 0)               // OBJ OBJ
        return false;
    if (!EmitAtomOp(cx, pn->pn_kid, JSOP_GETPROP, bce)) // OBJ V
        return false;
    if (Emit1(cx, bce, JSOP_POS) < 0)               // OBJ N
        return false;
    if (post && Emit1(cx, bce, JSOP_DUP) < 0)       // OBJ N? N
        return false;
    if (Emit1(cx, bce, JSOP_ONE) < 0)               // OBJ N? N 1
        return false;
    if (Emit1(cx, bce, binop) < 0)                  // OBJ N? N+1
        return false;

    if (post) {
        if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)2) < 0)   // N? N+1 OBJ
            return false;
        if (Emit1(cx, bce, JSOP_SWAP) < 0)                  // N? OBJ N+1
            return false;
    }

    JSOp setOp = bce->sc->strict ? JSOP_STRICTSETPROP : JSOP_SETPROP;
    if (!EmitAtomOp(cx, pn->pn_kid, setOp, bce))     // N? N+1
        return false;
    if (post && Emit1(cx, bce, JSOP_POP) < 0)       // RESULT
        return false;

    return true;
}

static bool
EmitNameIncDec(ExclusiveContext* cx, ParseNode* pn, BytecodeEmitter* bce)
{
    const JSCodeSpec* cs = &js_CodeSpec[pn->pn_kid->getOp()];

    bool global = (cs->format & JOF_GNAME);
    bool post;
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    if (!EmitAtomOp(cx, pn->pn_kid, global ? JSOP_BINDGNAME : JSOP_BINDNAME, bce))  // OBJ
        return false;
    if (!EmitAtomOp(cx, pn->pn_kid, global ? JSOP_GETGNAME : JSOP_GETNAME, bce))    // OBJ V
        return false;
    if (Emit1(cx, bce, JSOP_POS) < 0)               // OBJ N
        return false;
    if (post && Emit1(cx, bce, JSOP_DUP) < 0)       // OBJ N? N
        return false;
    if (Emit1(cx, bce, JSOP_ONE) < 0)               // OBJ N? N 1
        return false;
    if (Emit1(cx, bce, binop) < 0)                  // OBJ N? N+1
        return false;

    if (post) {
        if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)2) < 0)   // N? N+1 OBJ
            return false;
        if (Emit1(cx, bce, JSOP_SWAP) < 0)                  // N? OBJ N+1
            return false;
    }

    JSOp setOp = StrictifySetNameOp(global ? JSOP_SETGNAME : JSOP_SETNAME, bce);
    if (!EmitAtomOp(cx, pn->pn_kid, setOp, bce)) // N? N+1
        return false;
    if (post && Emit1(cx, bce, JSOP_POP) < 0)       // RESULT
        return false;

    return true;
}

/*
 * Emit bytecode to put operands for a JSOP_GETELEM/CALLELEM/SETELEM/DELELEM
 * opcode onto the stack in the right order. In the case of SETELEM, the
 * value to be assigned must already be pushed.
 */
static bool
EmitElemOperands(ExclusiveContext* cx, ParseNode* pn, JSOp op, BytecodeEmitter* bce)
{
    MOZ_ASSERT(pn->isArity(PN_BINARY));
    if (!EmitTree(cx, bce, pn->pn_left))
        return false;
    if (op == JSOP_CALLELEM && Emit1(cx, bce, JSOP_DUP) < 0)
        return false;
    if (!EmitTree(cx, bce, pn->pn_right))
        return false;
    bool isSetElem = op == JSOP_SETELEM || op == JSOP_STRICTSETELEM;
    if (isSetElem && Emit2(cx, bce, JSOP_PICK, (jsbytecode)2) < 0)
        return false;
    return true;
}

static inline bool
EmitElemOpBase(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op)
{
    if (Emit1(cx, bce, op) < 0)
        return false;
    CheckTypeSet(cx, bce, op);
    return true;
}

static bool
EmitElemOp(ExclusiveContext* cx, ParseNode* pn, JSOp op, BytecodeEmitter* bce)
{
    return EmitElemOperands(cx, pn, op, bce) && EmitElemOpBase(cx, bce, op);
}

static bool
EmitElemIncDec(ExclusiveContext* cx, ParseNode* pn, BytecodeEmitter* bce)
{
    MOZ_ASSERT(pn->pn_kid->getKind() == PNK_ELEM);

    if (!EmitElemOperands(cx, pn->pn_kid, JSOP_GETELEM, bce))
        return false;

    bool post;
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    /*
     * We need to convert the key to an object id first, so that we do not do
     * it inside both the GETELEM and the SETELEM.
     */
                                                    // OBJ KEY*
    if (Emit1(cx, bce, JSOP_TOID) < 0)              // OBJ KEY
        return false;
    if (Emit1(cx, bce, JSOP_DUP2) < 0)              // OBJ KEY OBJ KEY
        return false;
    if (!EmitElemOpBase(cx, bce, JSOP_GETELEM))     // OBJ KEY V
        return false;
    if (Emit1(cx, bce, JSOP_POS) < 0)               // OBJ KEY N
        return false;
    if (post && Emit1(cx, bce, JSOP_DUP) < 0)       // OBJ KEY N? N
        return false;
    if (Emit1(cx, bce, JSOP_ONE) < 0)               // OBJ KEY N? N 1
        return false;
    if (Emit1(cx, bce, binop) < 0)                  // OBJ KEY N? N+1
        return false;

    if (post) {
        if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)3) < 0)   // KEY N N+1 OBJ
            return false;
        if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)3) < 0)   // N N+1 OBJ KEY
            return false;
        if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)2) < 0)   // N OBJ KEY N+1
            return false;
    }

    JSOp setOp = bce->sc->strict ? JSOP_STRICTSETELEM : JSOP_SETELEM;
    if (!EmitElemOpBase(cx, bce, setOp))     // N? N+1
        return false;
    if (post && Emit1(cx, bce, JSOP_POP) < 0)       // RESULT
        return false;

    return true;
}

static bool
EmitNumberOp(ExclusiveContext* cx, double dval, BytecodeEmitter* bce)
{
    int32_t ival;
    uint32_t u;
    ptrdiff_t off;
    jsbytecode* pc;

    if (NumberIsInt32(dval, &ival)) {
        if (ival == 0)
            return Emit1(cx, bce, JSOP_ZERO) >= 0;
        if (ival == 1)
            return Emit1(cx, bce, JSOP_ONE) >= 0;
        if ((int)(int8_t)ival == ival)
            return Emit2(cx, bce, JSOP_INT8, (jsbytecode)(int8_t)ival) >= 0;

        u = (uint32_t)ival;
        if (u < JS_BIT(16)) {
            EMIT_UINT16_IMM_OP(JSOP_UINT16, u);
        } else if (u < JS_BIT(24)) {
            off = EmitN(cx, bce, JSOP_UINT24, 3);
            if (off < 0)
                return false;
            pc = bce->code(off);
            SET_UINT24(pc, u);
        } else {
            off = EmitN(cx, bce, JSOP_INT32, 4);
            if (off < 0)
                return false;
            pc = bce->code(off);
            SET_INT32(pc, ival);
        }
        return true;
    }

    if (!bce->constList.append(DoubleValue(dval)))
        return false;

    return EmitIndex32(cx, JSOP_DOUBLE, bce->constList.length() - 1, bce);
}

static inline void
SetJumpOffsetAt(BytecodeEmitter* bce, ptrdiff_t off)
{
    SET_JUMP_OFFSET(bce->code(off), bce->offset() - off);
}

static bool
PushInitialConstants(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, unsigned n)
{
    MOZ_ASSERT(op == JSOP_UNDEFINED || op == JSOP_UNINITIALIZED);
    for (unsigned i = 0; i < n; ++i) {
        if (Emit1(cx, bce, op) < 0)
            return false;
    }
    return true;
}

static bool
InitializeBlockScopedLocalsFromStack(ExclusiveContext* cx, BytecodeEmitter* bce,
                                     Handle<StaticBlockObject*> blockObj)
{
    for (unsigned i = blockObj->numVariables(); i > 0; --i) {
        if (blockObj->isAliased(i - 1)) {
            ScopeCoordinate sc;
            sc.setHops(0);
            sc.setSlot(BlockObject::RESERVED_SLOTS + i - 1);
            if (!EmitAliasedVarOp(cx, JSOP_INITALIASEDLEXICAL, sc, DontCheckLexical, bce))
                return false;
        } else {
            // blockIndexToLocalIndex returns the slot index after the unaliased
            // locals stored in the frame. EmitUnaliasedVarOp expects the slot index
            // to include both unaliased and aliased locals, so we have to add the
            // number of aliased locals.
            uint32_t numAliased = bce->script->bindings.numAliasedBodyLevelLocals();
            unsigned local = blockObj->blockIndexToLocalIndex(i - 1) + numAliased;
            if (!EmitUnaliasedVarOp(cx, JSOP_INITLEXICAL, local, DontCheckLexical, bce))
                return false;
        }
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
    }
    return true;
}

static bool
EnterBlockScope(ExclusiveContext* cx, BytecodeEmitter* bce, StmtInfoBCE* stmtInfo,
                ObjectBox* objbox, JSOp initialValueOp, unsigned alreadyPushed = 0)
{
    // Initial values for block-scoped locals. Whether it is undefined or the
    // JS_UNINITIALIZED_LEXICAL magic value depends on the context. The
    // current way we emit for-in and for-of heads means its let bindings will
    // always be initialized, so we can initialize them to undefined.
    Rooted<StaticBlockObject*> blockObj(cx, &objbox->object->as<StaticBlockObject>());
    if (!PushInitialConstants(cx, bce, initialValueOp, blockObj->numVariables() - alreadyPushed))
        return false;

    if (!EnterNestedScope(cx, bce, stmtInfo, objbox, STMT_BLOCK))
        return false;

    if (!InitializeBlockScopedLocalsFromStack(cx, bce, blockObj))
        return false;

    return true;
}

/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047.
 * LLVM is deciding to inline this function which uses a lot of stack space
 * into EmitTree which is recursive and uses relatively little stack space.
 */
MOZ_NEVER_INLINE static bool
EmitSwitch(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    JSOp switchOp;
    bool hasDefault;
    ptrdiff_t top, off, defaultOffset;
    ParseNode* pn2, *pn3, *pn4;
    int32_t low, high;
    int noteIndex;
    size_t switchSize;
    jsbytecode* pc;

    /* Try for most optimal, fall back if not dense ints. */
    switchOp = JSOP_TABLESWITCH;
    hasDefault = false;
    defaultOffset = -1;

    pn2 = pn->pn_right;
    MOZ_ASSERT(pn2->isKind(PNK_LEXICALSCOPE) || pn2->isKind(PNK_STATEMENTLIST));

    /* Push the discriminant. */
    if (!EmitTree(cx, bce, pn->pn_left))
        return false;

    StmtInfoBCE stmtInfo(cx);
    if (pn2->isKind(PNK_LEXICALSCOPE)) {
        if (!EnterBlockScope(cx, bce, &stmtInfo, pn2->pn_objbox, JSOP_UNINITIALIZED, 0))
            return false;

        stmtInfo.type = STMT_SWITCH;
        stmtInfo.update = top = bce->offset();
        /* Advance pn2 to refer to the switch case list. */
        pn2 = pn2->expr();
    } else {
        MOZ_ASSERT(pn2->isKind(PNK_STATEMENTLIST));
        top = bce->offset();
        PushStatementBCE(bce, &stmtInfo, STMT_SWITCH, top);
    }

    /* Switch bytecodes run from here till end of final case. */
    uint32_t caseCount = pn2->pn_count;
    uint32_t tableLength = 0;
    UniquePtr<ParseNode*[], JS::FreePolicy> table(nullptr);

    if (caseCount > JS_BIT(16)) {
        bce->parser->tokenStream.reportError(JSMSG_TOO_MANY_CASES);
        return false;
    }

    if (caseCount == 0 ||
        (caseCount == 1 &&
         (hasDefault = (pn2->pn_head->isKind(PNK_DEFAULT))))) {
        caseCount = 0;
        low = 0;
        high = -1;
    } else {
        bool ok = true;
#define INTMAP_LENGTH   256
        jsbitmap intmap_space[INTMAP_LENGTH];
        jsbitmap* intmap = nullptr;
        int32_t intmap_bitlen = 0;

        low  = JSVAL_INT_MAX;
        high = JSVAL_INT_MIN;

        for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
            if (pn3->isKind(PNK_DEFAULT)) {
                hasDefault = true;
                caseCount--;    /* one of the "cases" was the default */
                continue;
            }

            MOZ_ASSERT(pn3->isKind(PNK_CASE));
            if (switchOp == JSOP_CONDSWITCH)
                continue;

            MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);

            pn4 = pn3->pn_left;

            if (pn4->getKind() != PNK_NUMBER) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }

            int32_t i;
            if (!NumberIsInt32(pn4->pn_dval, &i)) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }

            if ((unsigned)(i + (int)JS_BIT(15)) >= (unsigned)JS_BIT(16)) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }
            if (i < low)
                low = i;
            if (high < i)
                high = i;

            /*
             * Check for duplicates, which require a JSOP_CONDSWITCH.
             * We bias i by 65536 if it's negative, and hope that's a rare
             * case (because it requires a malloc'd bitmap).
             */
            if (i < 0)
                i += JS_BIT(16);
            if (i >= intmap_bitlen) {
                if (!intmap &&
                    size_t(i) < (INTMAP_LENGTH * JS_BITMAP_NBITS)) {
                    intmap = intmap_space;
                    intmap_bitlen = INTMAP_LENGTH * JS_BITMAP_NBITS;
                } else {
                    /* Just grab 8K for the worst-case bitmap. */
                    intmap_bitlen = JS_BIT(16);
                    intmap = cx->pod_malloc<jsbitmap>(JS_BIT(16) / JS_BITMAP_NBITS);
                    if (!intmap) {
                        js_ReportOutOfMemory(cx);
                        return false;
                    }
                }
                memset(intmap, 0, size_t(intmap_bitlen) / CHAR_BIT);
            }
            if (JS_TEST_BIT(intmap, i)) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }
            JS_SET_BIT(intmap, i);
        }

        if (intmap && intmap != intmap_space)
            js_free(intmap);
        if (!ok)
            return false;

        /*
         * Compute table length and select condswitch instead if overlarge or
         * more than half-sparse.
         */
        if (switchOp == JSOP_TABLESWITCH) {
            tableLength = (uint32_t)(high - low + 1);
            if (tableLength >= JS_BIT(16) || tableLength > 2 * caseCount)
                switchOp = JSOP_CONDSWITCH;
        }
    }

    /*
     * The note has one or two offsets: first tells total switch code length;
     * second (if condswitch) tells offset to first JSOP_CASE.
     */
    if (switchOp == JSOP_CONDSWITCH) {
        /* 0 bytes of immediate for unoptimized switch. */
        switchSize = 0;
        noteIndex = NewSrcNote3(cx, bce, SRC_CONDSWITCH, 0, 0);
    } else {
        MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);

        /* 3 offsets (len, low, high) before the table, 1 per entry. */
        switchSize = (size_t)(JUMP_OFFSET_LEN * (3 + tableLength));
        noteIndex = NewSrcNote2(cx, bce, SRC_TABLESWITCH, 0);
    }
    if (noteIndex < 0)
        return false;

    /* Emit switchOp followed by switchSize bytes of jump or lookup table. */
    if (EmitN(cx, bce, switchOp, switchSize) < 0)
        return false;

    off = -1;
    if (switchOp == JSOP_CONDSWITCH) {
        int caseNoteIndex = -1;
        bool beforeCases = true;

        /* Emit code for evaluating cases and jumping to case statements. */
        for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
            pn4 = pn3->pn_left;
            if (pn4 && !EmitTree(cx, bce, pn4))
                return false;
            if (caseNoteIndex >= 0) {
                /* off is the previous JSOP_CASE's bytecode offset. */
                if (!SetSrcNoteOffset(cx, bce, (unsigned)caseNoteIndex, 0, bce->offset() - off))
                    return false;
            }
            if (!pn4) {
                MOZ_ASSERT(pn3->isKind(PNK_DEFAULT));
                continue;
            }
            caseNoteIndex = NewSrcNote2(cx, bce, SRC_NEXTCASE, 0);
            if (caseNoteIndex < 0)
                return false;
            off = EmitJump(cx, bce, JSOP_CASE, 0);
            if (off < 0)
                return false;
            pn3->pn_offset = off;
            if (beforeCases) {
                unsigned noteCount, noteCountDelta;

                /* Switch note's second offset is to first JSOP_CASE. */
                noteCount = bce->notes().length();
                if (!SetSrcNoteOffset(cx, bce, (unsigned)noteIndex, 1, off - top))
                    return false;
                noteCountDelta = bce->notes().length() - noteCount;
                if (noteCountDelta != 0)
                    caseNoteIndex += noteCountDelta;
                beforeCases = false;
            }
        }

        /*
         * If we didn't have an explicit default (which could fall in between
         * cases, preventing us from fusing this SetSrcNoteOffset with the call
         * in the loop above), link the last case to the implicit default for
         * the benefit of IonBuilder.
         */
        if (!hasDefault &&
            caseNoteIndex >= 0 &&
            !SetSrcNoteOffset(cx, bce, (unsigned)caseNoteIndex, 0, bce->offset() - off))
        {
            return false;
        }

        /* Emit default even if no explicit default statement. */
        defaultOffset = EmitJump(cx, bce, JSOP_DEFAULT, 0);
        if (defaultOffset < 0)
            return false;
    } else {
        MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);
        pc = bce->code(top + JUMP_OFFSET_LEN);

        /* Fill in switch bounds, which we know fit in 16-bit offsets. */
        SET_JUMP_OFFSET(pc, low);
        pc += JUMP_OFFSET_LEN;
        SET_JUMP_OFFSET(pc, high);
        pc += JUMP_OFFSET_LEN;

        /*
         * Use malloc to avoid arena bloat for programs with many switches.
         * UniquePtr takes care of freeing it on exit.
         */
        if (tableLength != 0) {
            table = cx->make_zeroed_pod_array<ParseNode*>(tableLength);
            if (!table)
                return false;
            for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
                if (pn3->isKind(PNK_DEFAULT))
                    continue;

                MOZ_ASSERT(pn3->isKind(PNK_CASE));

                pn4 = pn3->pn_left;
                MOZ_ASSERT(pn4->getKind() == PNK_NUMBER);

                int32_t i = int32_t(pn4->pn_dval);
                MOZ_ASSERT(double(i) == pn4->pn_dval);

                i -= low;
                MOZ_ASSERT(uint32_t(i) < tableLength);
                table[i] = pn3;
            }
        }
    }

    /* Emit code for each case's statements, copying pn_offset up to pn3. */
    for (pn3 = pn2->pn_head; pn3; pn3 = pn3->pn_next) {
        if (switchOp == JSOP_CONDSWITCH && !pn3->isKind(PNK_DEFAULT))
            SetJumpOffsetAt(bce, pn3->pn_offset);
        pn4 = pn3->pn_right;
        if (!EmitTree(cx, bce, pn4))
            return false;
        pn3->pn_offset = pn4->pn_offset;
        if (pn3->isKind(PNK_DEFAULT))
            off = pn3->pn_offset - top;
    }

    if (!hasDefault) {
        /* If no default case, offset for default is to end of switch. */
        off = bce->offset() - top;
    }

    /* We better have set "off" by now. */
    MOZ_ASSERT(off != -1);

    /* Set the default offset (to end of switch if no default). */
    if (switchOp == JSOP_CONDSWITCH) {
        pc = nullptr;
        MOZ_ASSERT(defaultOffset != -1);
        SET_JUMP_OFFSET(bce->code(defaultOffset), off - (defaultOffset - top));
    } else {
        pc = bce->code(top);
        SET_JUMP_OFFSET(pc, off);
        pc += JUMP_OFFSET_LEN;
    }

    /* Set the SRC_SWITCH note's offset operand to tell end of switch. */
    off = bce->offset() - top;
    if (!SetSrcNoteOffset(cx, bce, (unsigned)noteIndex, 0, off))
        return false;

    if (switchOp == JSOP_TABLESWITCH) {
        /* Skip over the already-initialized switch bounds. */
        pc += 2 * JUMP_OFFSET_LEN;

        /* Fill in the jump table, if there is one. */
        for (uint32_t i = 0; i < tableLength; i++) {
            pn3 = table[i];
            off = pn3 ? pn3->pn_offset - top : 0;
            SET_JUMP_OFFSET(pc, off);
            pc += JUMP_OFFSET_LEN;
        }
    }

    if (pn->pn_right->isKind(PNK_LEXICALSCOPE)) {
        if (!LeaveNestedScope(cx, bce, &stmtInfo))
            return false;
    } else {
        if (!PopStatementBCE(cx, bce))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::isRunOnceLambda()
{
    // The run once lambda flags set by the parser are approximate, and we look
    // at properties of the function itself before deciding to emit a function
    // as a run once lambda.

    if (!(parent && parent->emittingRunOnceLambda) &&
        (emitterMode != LazyFunction || !lazyScript->treatAsRunOnce()))
    {
        return false;
    }

    FunctionBox* funbox = sc->asFunctionBox();
    return !funbox->argumentsHasLocalBinding() &&
           !funbox->isGenerator() &&
           !funbox->function()->name();
}

static bool
EmitYieldOp(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op)
{
    if (op == JSOP_FINALYIELDRVAL)
        return Emit1(cx, bce, JSOP_FINALYIELDRVAL) >= 0;

    MOZ_ASSERT(op == JSOP_INITIALYIELD || op == JSOP_YIELD);

    ptrdiff_t off = EmitN(cx, bce, op, 3);
    if (off < 0)
        return false;

    uint32_t yieldIndex = bce->yieldOffsetList.length();
    if (yieldIndex >= JS_BIT(24)) {
        bce->reportError(nullptr, JSMSG_TOO_MANY_YIELDS);
        return false;
    }

    SET_UINT24(bce->code(off), yieldIndex);

    if (!bce->yieldOffsetList.append(bce->offset()))
        return false;

    return Emit1(cx, bce, JSOP_DEBUGAFTERYIELD) >= 0;
}

bool
frontend::EmitFunctionScript(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* body)
{
    if (!bce->updateLocalsToFrameSlots())
        return false;

    /*
     * IonBuilder has assumptions about what may occur immediately after
     * script->main (e.g., in the case of destructuring params). Thus, put the
     * following ops into the range [script->code, script->main). Note:
     * execution starts from script->code, so this has no semantic effect.
     */

    FunctionBox* funbox = bce->sc->asFunctionBox();
    if (funbox->argumentsHasLocalBinding()) {
        MOZ_ASSERT(bce->offset() == 0);  /* See JSScript::argumentsBytecode. */
        bce->switchToProlog();
        if (Emit1(cx, bce, JSOP_ARGUMENTS) < 0)
            return false;
        InternalBindingsHandle bindings(bce->script, &bce->script->bindings);
        BindingIter bi = Bindings::argumentsBinding(cx, bindings);
        if (bce->script->bindingIsAliased(bi)) {
            ScopeCoordinate sc;
            sc.setHops(0);
            sc.setSlot(0);  // initialize to silence GCC warning
            JS_ALWAYS_TRUE(LookupAliasedNameSlot(bce, bce->script, cx->names().arguments, &sc));
            if (!EmitAliasedVarOp(cx, JSOP_SETALIASEDVAR, sc, DontCheckLexical, bce))
                return false;
        } else {
            if (!EmitUnaliasedVarOp(cx, JSOP_SETLOCAL, bi.localIndex(), DontCheckLexical, bce))
                return false;
        }
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
        bce->switchToMain();
    }

    /*
     * Emit a prologue for run-once scripts which will deoptimize JIT code if
     * the script ends up running multiple times via foo.caller related
     * shenanigans.
     */
    bool runOnce = bce->isRunOnceLambda();
    if (runOnce) {
        bce->switchToProlog();
        if (Emit1(cx, bce, JSOP_RUNONCE) < 0)
            return false;
        bce->switchToMain();
    }

    if (!EmitTree(cx, bce, body))
        return false;

    if (bce->sc->isFunctionBox()) {
        if (bce->sc->asFunctionBox()->isGenerator()) {
            // If we fall off the end of a generator, do a final yield.
            if (bce->sc->asFunctionBox()->isStarGenerator() && !EmitPrepareIteratorResult(cx, bce))
                return false;

            if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
                return false;

            if (bce->sc->asFunctionBox()->isStarGenerator() &&
                !EmitFinishIteratorResult(cx, bce, true))
            {
                return false;
            }

            if (Emit1(cx, bce, JSOP_SETRVAL) < 0)
                return false;

            ScopeCoordinate sc;
            // We know that .generator is on the top scope chain node, as we are
            // at the function end.
            sc.setHops(0);
            MOZ_ALWAYS_TRUE(LookupAliasedNameSlot(bce, bce->script, cx->names().dotGenerator, &sc));
            if (!EmitAliasedVarOp(cx, JSOP_GETALIASEDVAR, sc, DontCheckLexical, bce))
                return false;

            // No need to check for finally blocks, etc as in EmitReturn.
            if (!EmitYieldOp(cx, bce, JSOP_FINALYIELDRVAL))
                return false;
        } else {
            // Non-generator functions just return |undefined|. The JSOP_RETRVAL
            // emitted below will do that, except if the script has a finally
            // block: there can be a non-undefined value in the return value
            // slot. We just emit an explicit return in this case.
            if (bce->hasTryFinally) {
                if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
                    return false;
                if (Emit1(cx, bce, JSOP_RETURN) < 0)
                    return false;
            }
        }
    }

    // Always end the script with a JSOP_RETRVAL. Some other parts of the codebase
    // depend on this opcode, e.g. InterpreterRegs::setToEndOfScript.
    if (Emit1(cx, bce, JSOP_RETRVAL) < 0)
        return false;

    // If all locals are aliased, the frame's block slots won't be used, so we
    // can set numBlockScoped = 0. This is nice for generators as it ensures
    // nfixed == 0, so we don't have to initialize any local slots when resuming
    // a generator.
    if (bce->sc->allLocalsAliased())
        bce->script->bindings.setAllLocalsAliased();

    if (!JSScript::fullyInitFromEmitter(cx, bce->script, bce))
        return false;

    /*
     * If this function is only expected to run once, mark the script so that
     * initializers created within it may be given more precise types.
     */
    if (runOnce) {
        bce->script->setTreatAsRunOnce();
        MOZ_ASSERT(!bce->script->hasRunOnce());
    }

    /* Initialize fun->script() so that the debugger has a valid fun->script(). */
    RootedFunction fun(cx, bce->script->functionNonDelazifying());
    MOZ_ASSERT(fun->isInterpreted());

    if (fun->isInterpretedLazy())
        fun->setUnlazifiedScript(bce->script);
    else
        fun->setScript(bce->script);

    bce->tellDebuggerAboutCompiledScript(cx);

    return true;
}

static bool
MaybeEmitVarDecl(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp prologOp, ParseNode* pn,
                 jsatomid* result)
{
    jsatomid atomIndex;

    if (!pn->pn_cookie.isFree()) {
        atomIndex = pn->pn_cookie.slot();
    } else {
        if (!bce->makeAtomIndex(pn->pn_atom, &atomIndex))
            return false;
    }

    if (JOF_OPTYPE(pn->getOp()) == JOF_ATOM &&
        (!bce->sc->isFunctionBox() || bce->sc->asFunctionBox()->isHeavyweight()))
    {
        bce->switchToProlog();
        if (!UpdateSourceCoordNotes(cx, bce, pn->pn_pos.begin))
            return false;
        if (!EmitIndexOp(cx, prologOp, atomIndex, bce))
            return false;
        bce->switchToMain();
    }

    if (result)
        *result = atomIndex;
    return true;
}

/*
 * This enum tells EmitVariables and the destructuring functions how emit the
 * given Parser::variables parse tree. In the base case, DefineVars, the caller
 * only wants variables to be defined in the prologue (if necessary). For
 * PushInitialValues, variable initializer expressions are evaluated and left
 * on the stack. For InitializeVars, the initializer expressions values are
 * assigned (to local variables) and popped.
 */
enum VarEmitOption
{
    DefineVars        = 0,
    PushInitialValues = 1,
    InitializeVars    = 2
};

typedef bool
(*DestructuringDeclEmitter)(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp prologOp, ParseNode* pn);

template <DestructuringDeclEmitter EmitName>
static bool
EmitDestructuringDeclsWithEmitter(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp prologOp,
                                  ParseNode* pattern)
{
    if (pattern->isKind(PNK_ARRAY)) {
        for (ParseNode* element = pattern->pn_head; element; element = element->pn_next) {
            if (element->isKind(PNK_ELISION))
                continue;
            ParseNode* target = element;
            if (element->isKind(PNK_SPREAD)) {
                MOZ_ASSERT(element->pn_kid->isKind(PNK_NAME));
                target = element->pn_kid;
            }
            if (target->isKind(PNK_ASSIGN))
                target = target->pn_left;
            if (target->isKind(PNK_NAME)) {
                if (!EmitName(cx, bce, prologOp, target))
                    return false;
            } else {
                if (!EmitDestructuringDeclsWithEmitter<EmitName>(cx, bce, prologOp, target))
                    return false;
            }
        }
        return true;
    }

    MOZ_ASSERT(pattern->isKind(PNK_OBJECT));
    for (ParseNode* member = pattern->pn_head; member; member = member->pn_next) {
        MOZ_ASSERT(member->isKind(PNK_MUTATEPROTO) ||
                   member->isKind(PNK_COLON) ||
                   member->isKind(PNK_SHORTHAND));

        ParseNode* target = member->isKind(PNK_MUTATEPROTO) ? member->pn_kid : member->pn_right;

        if (target->isKind(PNK_ASSIGN))
            target = target->pn_left;
        if (target->isKind(PNK_NAME)) {
            if (!EmitName(cx, bce, prologOp, target))
                return false;
        } else {
            if (!EmitDestructuringDeclsWithEmitter<EmitName>(cx, bce, prologOp, target))
                return false;
        }
    }
    return true;
}

bool
EmitDestructuringDecl(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp prologOp, ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_NAME));
    if (!BindNameToSlot(cx, bce, pn))
        return false;

    MOZ_ASSERT(!pn->isOp(JSOP_CALLEE));
    return MaybeEmitVarDecl(cx, bce, prologOp, pn, nullptr);
}

static inline bool
EmitDestructuringDecls(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp prologOp,
                       ParseNode* pattern)
{
    return EmitDestructuringDeclsWithEmitter<EmitDestructuringDecl>(cx, bce, prologOp, pattern);
}

bool
EmitInitializeDestructuringDecl(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp prologOp,
                                ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_NAME));
    MOZ_ASSERT(pn->isBound());
    return EmitVarOp(cx, pn, pn->getOp(), bce);
}

// Emit code to initialize all destructured names to the value on the top of
// the stack.
static inline bool
EmitInitializeDestructuringDecls(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp prologOp,
                                 ParseNode* pattern)
{
    return EmitDestructuringDeclsWithEmitter<EmitInitializeDestructuringDecl>(cx, bce,
                                                                              prologOp, pattern);
}

static bool
EmitDestructuringOpsHelper(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pattern,
                           VarEmitOption emitOption);

/*
 * EmitDestructuringLHS assumes the to-be-destructured value has been pushed on
 * the stack and emits code to destructure a single lhs expression (either a
 * name or a compound []/{} expression).
 *
 * If emitOption is InitializeVars, the to-be-destructured value is assigned to
 * locals and ultimately the initial slot is popped (-1 total depth change).
 *
 * If emitOption is PushInitialValues, the to-be-destructured value is replaced
 * with the initial values of the N (where 0 <= N) variables assigned in the
 * lhs expression. (Same post-condition as EmitDestructuringOpsHelper)
 */
static bool
EmitDestructuringLHS(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* target, VarEmitOption emitOption)
{
    MOZ_ASSERT(emitOption != DefineVars);

    // Now emit the lvalue opcode sequence. If the lvalue is a nested
    // destructuring initialiser-form, call ourselves to handle it, then pop
    // the matched value. Otherwise emit an lvalue bytecode sequence followed
    // by an assignment op.
    if (target->isKind(PNK_SPREAD))
        target = target->pn_kid;
    else if (target->isKind(PNK_ASSIGN))
        target = target->pn_left;
    if (target->isKind(PNK_ARRAY) || target->isKind(PNK_OBJECT)) {
        if (!EmitDestructuringOpsHelper(cx, bce, target, emitOption))
            return false;
        if (emitOption == InitializeVars) {
            // Per its post-condition, EmitDestructuringOpsHelper has left the
            // to-be-destructured value on top of the stack.
            if (Emit1(cx, bce, JSOP_POP) < 0)
                return false;
        }
    } else if (emitOption == PushInitialValues) {
        // The lhs is a simple name so the to-be-destructured value is
        // its initial value and there is nothing to do.
        MOZ_ASSERT(target->getOp() == JSOP_SETLOCAL || target->getOp() == JSOP_INITLEXICAL);
        MOZ_ASSERT(target->pn_dflags & PND_BOUND);
    } else {
        switch (target->getKind()) {
          case PNK_NAME:
            if (!BindNameToSlot(cx, bce, target))
                return false;

            switch (target->getOp()) {
              case JSOP_SETNAME:
              case JSOP_STRICTSETNAME:
              case JSOP_SETGNAME:
              case JSOP_STRICTSETGNAME:
              case JSOP_SETCONST: {
                // This is like ordinary assignment, but with one difference.
                //
                // In `a = b`, we first determine a binding for `a` (using
                // JSOP_BINDNAME or JSOP_BINDGNAME), then we evaluate `b`, then
                // a JSOP_SETNAME instruction.
                //
                // In `[a] = [b]`, per spec, `b` is evaluated first, then we
                // determine a binding for `a`. Then we need to do assignment--
                // but the operands are on the stack in the wrong order for
                // JSOP_SETPROP, so we have to add a JSOP_SWAP.
                jsatomid atomIndex;
                if (!bce->makeAtomIndex(target->pn_atom, &atomIndex))
                    return false;

                if (!target->isOp(JSOP_SETCONST)) {
                    bool global = target->isOp(JSOP_SETGNAME) || target->isOp(JSOP_STRICTSETGNAME);
                    JSOp bindOp = global ? JSOP_BINDGNAME : JSOP_BINDNAME;
                    if (!EmitIndex32(cx, bindOp, atomIndex, bce))
                        return false;
                    if (Emit1(cx, bce, JSOP_SWAP) < 0)
                        return false;
                }

                if (!EmitIndexOp(cx, target->getOp(), atomIndex, bce))
                    return false;
                break;
              }

              case JSOP_SETLOCAL:
              case JSOP_SETARG:
              case JSOP_INITLEXICAL:
                if (!EmitVarOp(cx, target, target->getOp(), bce))
                    return false;
                break;

              default:
                MOZ_CRASH("EmitDestructuringLHS: bad name op");
            }
            break;

          case PNK_DOT:
          {
            // See the (PNK_NAME, JSOP_SETNAME) case above.
            //
            // In `a.x = b`, `a` is evaluated first, then `b`, then a
            // JSOP_SETPROP instruction.
            //
            // In `[a.x] = [b]`, per spec, `b` is evaluated before `a`. Then we
            // need a property set -- but the operands are on the stack in the
            // wrong order for JSOP_SETPROP, so we have to add a JSOP_SWAP.
            if (!EmitTree(cx, bce, target->pn_expr))
                return false;
            if (Emit1(cx, bce, JSOP_SWAP) < 0)
                return false;
            JSOp setOp = bce->sc->strict ? JSOP_STRICTSETPROP : JSOP_SETPROP;
            if (!EmitAtomOp(cx, target, setOp, bce))
                return false;
            break;
          }

          case PNK_ELEM:
          {
            // See the comment at `case PNK_DOT:` above. This case,
            // `[a[x]] = [b]`, is handled much the same way. The JSOP_SWAP
            // is emitted by EmitElemOperands.
            JSOp setOp = bce->sc->strict ? JSOP_STRICTSETELEM : JSOP_SETELEM;
            if (!EmitElemOp(cx, target, setOp, bce))
                return false;
            break;
          }

          case PNK_CALL:
            MOZ_ASSERT(target->pn_xflags & PNX_SETCALL);
            if (!EmitTree(cx, bce, target))
                return false;

            // Pop the call return value. Below, we pop the RHS too, balancing
            // the stack --- presumably for the benefit of bytecode
            // analysis. (The interpreter will never reach these instructions
            // since we just emitted JSOP_SETCALL, which always throws. It's
            // possible no analyses actually depend on this either.)
            if (Emit1(cx, bce, JSOP_POP) < 0)
                return false;
            break;

          default:
            MOZ_CRASH("EmitDestructuringLHS: bad lhs kind");
        }

        // Pop the assigned value.
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
    }

    return true;
}

static bool EmitSpread(ExclusiveContext* cx, BytecodeEmitter* bce);
static bool EmitIterator(ExclusiveContext* cx, BytecodeEmitter* bce);

/**
 * EmitIteratorNext will pop iterator from the top of the stack.
 * It will push the result of |.next()| onto the stack.
 */
static bool
EmitIteratorNext(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn=nullptr)
{
    MOZ_ASSERT(bce->emitterMode != BytecodeEmitter::SelfHosting,
               ".next() iteration is prohibited in self-hosted code because it "
               "can run user-modifiable iteration code");

    if (Emit1(cx, bce, JSOP_DUP) < 0)                          // ... ITER ITER
        return false;
    if (!EmitAtomOp(cx, cx->names().next, JSOP_CALLPROP, bce)) // ... ITER NEXT
        return false;
    if (Emit1(cx, bce, JSOP_SWAP) < 0)                         // ... NEXT ITER
        return false;
    if (EmitCall(cx, bce, JSOP_CALL, 0, pn) < 0)               // ... RESULT
        return false;
    CheckTypeSet(cx, bce, JSOP_CALL);
    return true;
}

/**
 * EmitDefault will check if the value on top of the stack is "undefined".
 * If so, it will replace that value on the stack with the value defined by |defaultExpr|.
 */
static bool
EmitDefault(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* defaultExpr)
{
    if (Emit1(cx, bce, JSOP_DUP) < 0)                          // VALUE VALUE
        return false;
    if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)                    // VALUE VALUE UNDEFINED
        return false;
    if (Emit1(cx, bce, JSOP_STRICTEQ) < 0)                     // VALUE EQL?
        return false;
    // Emit source note to enable ion compilation.
    if (NewSrcNote(cx, bce, SRC_IF) < 0)
        return false;
    ptrdiff_t jump = EmitJump(cx, bce, JSOP_IFEQ, 0);          // VALUE
    if (jump < 0)
        return false;
    if (Emit1(cx, bce, JSOP_POP) < 0)                          // .
        return false;
    if (!EmitTree(cx, bce, defaultExpr))                       // DEFAULTVALUE
        return false;
    SetJumpOffsetAt(bce, jump);
    return true;
}

static bool
EmitDestructuringOpsArrayHelper(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pattern,
                                VarEmitOption emitOption)
{
    MOZ_ASSERT(pattern->isKind(PNK_ARRAY));
    MOZ_ASSERT(pattern->isArity(PN_LIST));
    MOZ_ASSERT(bce->stackDepth != 0);

    /*
     * Use an iterator to destructure the RHS, instead of index lookup.
     * InitializeVars expects us to leave the *original* value on the stack.
     */
    if (emitOption == InitializeVars) {
        if (Emit1(cx, bce, JSOP_DUP) < 0)                              // ... OBJ OBJ
            return false;
    }
    if (!EmitIterator(cx, bce))                                        // ... OBJ? ITER
        return false;
    bool needToPopIterator = true;

    for (ParseNode* member = pattern->pn_head; member; member = member->pn_next) {
        /*
         * Now push the property name currently being matched, which is the
         * current property name "label" on the left of a colon in the object
         * initializer.
         */
        ParseNode* pndefault = nullptr;
        ParseNode* elem = member;
        if (elem->isKind(PNK_ASSIGN)) {
            pndefault = elem->pn_right;
            elem = elem->pn_left;
        }

        if (elem->isKind(PNK_SPREAD)) {
            /* Create a new array with the rest of the iterator */
            ptrdiff_t off = EmitN(cx, bce, JSOP_NEWARRAY, 3);          // ... OBJ? ITER ARRAY
            if (off < 0)
                return false;
            CheckTypeSet(cx, bce, JSOP_NEWARRAY);
            jsbytecode* pc = bce->code(off);
            SET_UINT24(pc, 0);

            if (!EmitNumberOp(cx, 0, bce))                             // ... OBJ? ITER ARRAY INDEX
                return false;
            if (!EmitSpread(cx, bce))                                  // ... OBJ? ARRAY INDEX
                return false;
            if (Emit1(cx, bce, JSOP_POP) < 0)                          // ... OBJ? ARRAY
                return false;
            needToPopIterator = false;
        } else {
            if (Emit1(cx, bce, JSOP_DUP) < 0)                          // ... OBJ? ITER ITER
                return false;
            if (!EmitIteratorNext(cx, bce, pattern))                   // ... OBJ? ITER RESULT
                return false;
            if (Emit1(cx, bce, JSOP_DUP) < 0)                          // ... OBJ? ITER RESULT RESULT
                return false;
            if (!EmitAtomOp(cx, cx->names().done, JSOP_GETPROP, bce))  // ... OBJ? ITER RESULT DONE?
                return false;

            // Emit (result.done ? undefined : result.value)
            // This is mostly copied from EmitConditionalExpression, except that this code
            // does not push new values onto the stack.
            ptrdiff_t noteIndex = NewSrcNote(cx, bce, SRC_COND);
            if (noteIndex < 0)
                return false;
            ptrdiff_t beq = EmitJump(cx, bce, JSOP_IFEQ, 0);
            if (beq < 0)
                return false;

            if (Emit1(cx, bce, JSOP_POP) < 0)                          // ... OBJ? ITER
                return false;
            if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)                    // ... OBJ? ITER UNDEFINED
                return false;

            /* Jump around else, fixup the branch, emit else, fixup jump. */
            ptrdiff_t jmp = EmitJump(cx, bce, JSOP_GOTO, 0);
            if (jmp < 0)
                return false;
            SetJumpOffsetAt(bce, beq);

            if (!EmitAtomOp(cx, cx->names().value, JSOP_GETPROP, bce)) // ... OBJ? ITER VALUE
                return false;

            SetJumpOffsetAt(bce, jmp);
            if (!SetSrcNoteOffset(cx, bce, noteIndex, 0, jmp - beq))
                return false;
        }

        if (pndefault && !EmitDefault(cx, bce, pndefault))
            return false;

        // Destructure into the pattern the element contains.
        ParseNode* subpattern = elem;
        if (subpattern->isKind(PNK_ELISION)) {
            // The value destructuring into an elision just gets ignored.
            if (Emit1(cx, bce, JSOP_POP) < 0)                          // ... OBJ? ITER
                return false;
            continue;
        }

        int32_t depthBefore = bce->stackDepth;
        if (!EmitDestructuringLHS(cx, bce, subpattern, emitOption))
            return false;

        if (emitOption == PushInitialValues && needToPopIterator) {
            /*
             * After '[x,y]' in 'let ([[x,y], z] = o)', the stack is
             *   | to-be-destructured-value | x | y |
             * The goal is:
             *   | x | y | z |
             * so emit a pick to produce the intermediate state
             *   | x | y | to-be-destructured-value |
             * before destructuring z. This gives the loop invariant that
             * the to-be-destructured-value is always on top of the stack.
             */
            MOZ_ASSERT((bce->stackDepth - bce->stackDepth) >= -1);
            uint32_t pickDistance = (uint32_t)((bce->stackDepth + 1) - depthBefore);
            if (pickDistance > 0) {
                if (pickDistance > UINT8_MAX) {
                    bce->reportError(subpattern, JSMSG_TOO_MANY_LOCALS);
                    return false;
                }
                if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)pickDistance) < 0)
                    return false;
            }
        }
    }

    if (needToPopIterator && Emit1(cx, bce, JSOP_POP) < 0)
        return false;

    return true;
}

static bool
EmitDestructuringOpsObjectHelper(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pattern,
                                 VarEmitOption emitOption)
{
    MOZ_ASSERT(pattern->isKind(PNK_OBJECT));
    MOZ_ASSERT(pattern->isArity(PN_LIST));

    MOZ_ASSERT(bce->stackDepth != 0);                                  // ... OBJ

    for (ParseNode* member = pattern->pn_head; member; member = member->pn_next) {
        // Duplicate the value being destructured to use as a reference base.
        if (Emit1(cx, bce, JSOP_DUP) < 0)                              // ... OBJ OBJ
            return false;

        // Now push the property name currently being matched, which is the
        // current property name "label" on the left of a colon in the object
        // initialiser.
        bool needsGetElem = true;

        ParseNode* subpattern;
        if (member->isKind(PNK_MUTATEPROTO)) {
            if (!EmitAtomOp(cx, cx->names().proto, JSOP_GETPROP, bce)) // ... OBJ PROP
                return false;
            needsGetElem = false;
            subpattern = member->pn_kid;
        } else {
            MOZ_ASSERT(member->isKind(PNK_COLON) || member->isKind(PNK_SHORTHAND));

            ParseNode* key = member->pn_left;
            if (key->isKind(PNK_NUMBER)) {
                if (!EmitNumberOp(cx, key->pn_dval, bce))              // ... OBJ OBJ KEY
                    return false;
            } else if (key->isKind(PNK_OBJECT_PROPERTY_NAME) || key->isKind(PNK_STRING)) {
                PropertyName* name = key->pn_atom->asPropertyName();

                // The parser already checked for atoms representing indexes and
                // used PNK_NUMBER instead, but also watch for ids which TI treats
                // as indexes for simplification of downstream analysis.
                jsid id = NameToId(name);
                if (id != IdToTypeId(id)) {
                    if (!EmitTree(cx, bce, key))                       // ... OBJ OBJ KEY
                        return false;
                } else {
                    if (!EmitAtomOp(cx, name, JSOP_GETPROP, bce))      // ...OBJ PROP
                        return false;
                    needsGetElem = false;
                }
            } else {
                MOZ_ASSERT(key->isKind(PNK_COMPUTED_NAME));
                if (!EmitTree(cx, bce, key->pn_kid))                   // ... OBJ OBJ KEY
                    return false;
            }

            subpattern = member->pn_right;
        }

        // Get the property value if not done already.
        if (needsGetElem && !EmitElemOpBase(cx, bce, JSOP_GETELEM))    // ... OBJ PROP
            return false;

        if (subpattern->isKind(PNK_ASSIGN)) {
            if (!EmitDefault(cx, bce, subpattern->pn_right))
                return false;
            subpattern = subpattern->pn_left;
        }

        // Destructure PROP per this member's subpattern.
        int32_t depthBefore = bce->stackDepth;
        if (!EmitDestructuringLHS(cx, bce, subpattern, emitOption))
            return false;

        // If emitOption is InitializeVars, destructuring initialized each
        // target in the subpattern's LHS as it went, then popped PROP.  We've
        // correctly returned to the loop-entry stack, and we continue to the
        // next member.
        if (emitOption == InitializeVars)                              // ... OBJ
            continue;

        MOZ_ASSERT(emitOption == PushInitialValues);

        // EmitDestructuringLHS removed PROP, and it pushed a value per target
        // name in LHS (for |emitOption == PushInitialValues| only makes sense
        // when multiple values need to be pushed onto the stack to initialize
        // a single lexical scope). It also preserved OBJ deep in the stack as
        // the original object to be destructed into remaining target names in
        // the LHS object pattern. (We use PushInitialValues *only* as part of
        // SpiderMonkey's proprietary let block statements, which assign their
        // targets all in a single go [akin to Scheme's let, and distinct from
        // let*/letrec].) Thus for:
        //
        //   let ({arr: [x, y], z} = obj) { ... }
        //
        // we have this stack after the above acts upon the [x, y] subpattern:
        //
        //     ... OBJ x y
        //
        // (where of course x = obj.arr[0] and y = obj.arr[1], and []-indexing
        // is really iteration-indexing). We want to have:
        //
        //     ... x y OBJ
        //
        // so that we can continue, ready to destruct z from OBJ. Pick OBJ out
        // of the stack, moving it to the top, to accomplish this.
        MOZ_ASSERT((bce->stackDepth - bce->stackDepth) >= -1);
        uint32_t pickDistance = (uint32_t)((bce->stackDepth + 1) - depthBefore);
        if (pickDistance > 0) {
            if (pickDistance > UINT8_MAX) {
                bce->reportError(subpattern, JSMSG_TOO_MANY_LOCALS);
                return false;
            }
            if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)pickDistance) < 0)
                return false;
        }
    }

    if (emitOption == PushInitialValues) {
        // Per the above loop invariant, the value being destructured into this
        // object pattern is atop the stack.  Pop it to achieve the
        // post-condition.
        if (Emit1(cx, bce, JSOP_POP) < 0)                              // ... <pattern's target name values, seriatim>
            return false;
    }

    return true;
}

/*
 * Recursive helper for EmitDestructuringOps.
 * EmitDestructuringOpsHelper assumes the to-be-destructured value has been
 * pushed on the stack and emits code to destructure each part of a [] or {}
 * lhs expression.
 *
 * If emitOption is InitializeVars, the initial to-be-destructured value is
 * left untouched on the stack and the overall depth is not changed.
 *
 * If emitOption is PushInitialValues, the to-be-destructured value is replaced
 * with the initial values of the N (where 0 <= N) variables assigned in the
 * lhs expression. (Same post-condition as EmitDestructuringLHS)
 */
static bool
EmitDestructuringOpsHelper(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pattern,
                           VarEmitOption emitOption)
{
    MOZ_ASSERT(emitOption != DefineVars);

    if (pattern->isKind(PNK_ARRAY))
        return EmitDestructuringOpsArrayHelper(cx, bce, pattern, emitOption);
    return EmitDestructuringOpsObjectHelper(cx, bce, pattern, emitOption);
}

static bool
EmitDestructuringOps(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pattern,
                     bool isLet = false)
{
    /*
     * Call our recursive helper to emit the destructuring assignments and
     * related stack manipulations.
     */
    VarEmitOption emitOption = isLet ? PushInitialValues : InitializeVars;
    return EmitDestructuringOpsHelper(cx, bce, pattern, emitOption);
}

static bool
EmitTemplateString(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));

    for (ParseNode* pn2 = pn->pn_head; pn2 != NULL; pn2 = pn2->pn_next) {
        if (pn2->getKind() != PNK_STRING && pn2->getKind() != PNK_TEMPLATE_STRING) {
            // We update source notes before emitting the expression
            if (!UpdateSourceCoordNotes(cx, bce, pn2->pn_pos.begin))
                return false;
        }
        if (!EmitTree(cx, bce, pn2))
            return false;

        if (pn2->getKind() != PNK_STRING && pn2->getKind() != PNK_TEMPLATE_STRING) {
            // We need to convert the expression to a string
            if (Emit1(cx, bce, JSOP_TOSTRING) < 0)
                return false;
        }

        if (pn2 != pn->pn_head) {
            // We've pushed two strings onto the stack. Add them together, leaving just one.
            if (Emit1(cx, bce, JSOP_ADD) < 0)
                return false;
        }

    }
    return true;
}

static bool
EmitVariables(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, VarEmitOption emitOption,
              bool isLetExpr = false)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    MOZ_ASSERT(isLetExpr == (emitOption == PushInitialValues));

    ParseNode* next;
    for (ParseNode* pn2 = pn->pn_head; ; pn2 = next) {
        if (!UpdateSourceCoordNotes(cx, bce, pn2->pn_pos.begin))
            return false;
        next = pn2->pn_next;

        ParseNode* pn3;
        if (!pn2->isKind(PNK_NAME)) {
            if (pn2->isKind(PNK_ARRAY) || pn2->isKind(PNK_OBJECT)) {
                // If the emit option is DefineVars, emit variable binding
                // ops, but not destructuring ops.  The parser (see
                // Parser::variables) has ensured that our caller will be the
                // PNK_FOR/PNK_FORIN/PNK_FOROF case in EmitTree (we don't have
                // to worry about this being a variable declaration, as
                // destructuring declarations without initializers, e.g., |var
                // [x]|, are not legal syntax), and that case will emit the
                // destructuring code only after emitting an enumerating
                // opcode and a branch that tests whether the enumeration
                // ended. Thus, each iteration's assignment is responsible for
                // initializing, and nothing needs to be done here.
                //
                // Otherwise this is emitting destructuring let binding
                // initialization for a legacy comprehension expression. See
                // EmitForInOrOfVariables.
                MOZ_ASSERT(pn->pn_count == 1);
                if (emitOption == DefineVars) {
                    if (!EmitDestructuringDecls(cx, bce, pn->getOp(), pn2))
                        return false;
                } else {
                    // Lexical bindings cannot be used before they are
                    // initialized. Similar to the JSOP_INITLEXICAL case below.
                    MOZ_ASSERT(emitOption != DefineVars);
                    MOZ_ASSERT_IF(emitOption == InitializeVars, pn->pn_xflags & PNX_POPVAR);
                    if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
                        return false;
                    if (!EmitInitializeDestructuringDecls(cx, bce, pn->getOp(), pn2))
                        return false;
                }
                break;
            }

            /*
             * A destructuring initialiser assignment preceded by var will
             * never occur to the left of 'in' in a for-in loop.  As with 'for
             * (var x = i in o)...', this will cause the entire 'var [a, b] =
             * i' to be hoisted out of the loop.
             */
            MOZ_ASSERT(pn2->isKind(PNK_ASSIGN));
            MOZ_ASSERT(pn2->isOp(JSOP_NOP));
            MOZ_ASSERT(emitOption != DefineVars);

            /*
             * To allow the front end to rewrite var f = x; as f = x; when a
             * function f(){} precedes the var, detect simple name assignment
             * here and initialize the name.
             */
            if (pn2->pn_left->isKind(PNK_NAME)) {
                pn3 = pn2->pn_right;
                pn2 = pn2->pn_left;
                goto do_name;
            }

            pn3 = pn2->pn_left;
            if (!EmitDestructuringDecls(cx, bce, pn->getOp(), pn3))
                return false;

            if (!EmitTree(cx, bce, pn2->pn_right))
                return false;

            if (!EmitDestructuringOps(cx, bce, pn3, isLetExpr))
                return false;

            /* If we are not initializing, nothing to pop. */
            if (emitOption != InitializeVars) {
                if (next)
                    continue;
                break;
            }
            goto emit_note_pop;
        }

        /*
         * Load initializer early to share code above that jumps to do_name.
         * NB: if this var redeclares an existing binding, then pn2 is linked
         * on its definition's use-chain and pn_expr has been overlayed with
         * pn_lexdef.
         */
        pn3 = pn2->maybeExpr();

     do_name:
        if (!BindNameToSlot(cx, bce, pn2))
            return false;


        JSOp op;
        op = pn2->getOp();
        MOZ_ASSERT(op != JSOP_CALLEE);
        MOZ_ASSERT(!pn2->pn_cookie.isFree() || !pn->isOp(JSOP_NOP));

        jsatomid atomIndex;
        if (!MaybeEmitVarDecl(cx, bce, pn->getOp(), pn2, &atomIndex))
            return false;

        if (pn3) {
            MOZ_ASSERT(emitOption != DefineVars);
            if (op == JSOP_SETNAME ||
                op == JSOP_STRICTSETNAME ||
                op == JSOP_SETGNAME ||
                op == JSOP_STRICTSETGNAME ||
                op == JSOP_SETINTRINSIC)
            {
                MOZ_ASSERT(emitOption != PushInitialValues);
                JSOp bindOp;
                if (op == JSOP_SETNAME || op == JSOP_STRICTSETNAME)
                    bindOp = JSOP_BINDNAME;
                else if (op == JSOP_SETGNAME || op == JSOP_STRICTSETGNAME)
                    bindOp = JSOP_BINDGNAME;
                else
                    bindOp = JSOP_BINDINTRINSIC;
                if (!EmitIndex32(cx, bindOp, atomIndex, bce))
                    return false;
            }

            bool oldEmittingForInit = bce->emittingForInit;
            bce->emittingForInit = false;
            if (!EmitTree(cx, bce, pn3))
                return false;
            bce->emittingForInit = oldEmittingForInit;
        } else if (op == JSOP_INITLEXICAL || isLetExpr) {
            // 'let' bindings cannot be used before they are
            // initialized. JSOP_INITLEXICAL distinguishes the binding site.
            MOZ_ASSERT(emitOption != DefineVars);
            MOZ_ASSERT_IF(emitOption == InitializeVars, pn->pn_xflags & PNX_POPVAR);
            if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
                return false;
        }

        // If we are not initializing, nothing to pop. If we are initializing
        // lets, we must emit the pops.
        if (emitOption != InitializeVars) {
            if (next)
                continue;
            break;
        }

        MOZ_ASSERT_IF(pn2->isDefn(), pn3 == pn2->pn_expr);
        if (!pn2->pn_cookie.isFree()) {
            if (!EmitVarOp(cx, pn2, op, bce))
                return false;
        } else {
            if (!EmitIndexOp(cx, op, atomIndex, bce))
                return false;
        }

    emit_note_pop:
        if (!next)
            break;
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
    }

    if (pn->pn_xflags & PNX_POPVAR) {
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
    }

    return true;
}

static bool
EmitAssignment(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* lhs, JSOp op, ParseNode* rhs)
{
    /*
     * Check left operand type and generate specialized code for it.
     * Specialize to avoid ECMA "reference type" values on the operand
     * stack, which impose pervasive runtime "GetValue" costs.
     */
    jsatomid atomIndex = (jsatomid) -1;
    jsbytecode offset = 1;

    switch (lhs->getKind()) {
      case PNK_NAME:
        if (!BindNameToSlot(cx, bce, lhs))
            return false;
        if (lhs->pn_cookie.isFree()) {
            if (!bce->makeAtomIndex(lhs->pn_atom, &atomIndex))
                return false;
            if (!lhs->isConst()) {
                JSOp bindOp;
                if (lhs->isOp(JSOP_SETNAME) || lhs->isOp(JSOP_STRICTSETNAME))
                    bindOp = JSOP_BINDNAME;
                else if (lhs->isOp(JSOP_SETGNAME) || lhs->isOp(JSOP_STRICTSETGNAME))
                    bindOp = JSOP_BINDGNAME;
                else
                    bindOp = JSOP_BINDINTRINSIC;
                if (!EmitIndex32(cx, bindOp, atomIndex, bce))
                    return false;
                offset++;
            }
        }
        break;
      case PNK_DOT:
        if (!EmitTree(cx, bce, lhs->expr()))
            return false;
        offset++;
        if (!bce->makeAtomIndex(lhs->pn_atom, &atomIndex))
            return false;
        break;
      case PNK_ELEM:
        MOZ_ASSERT(lhs->isArity(PN_BINARY));
        if (!EmitTree(cx, bce, lhs->pn_left))
            return false;
        if (!EmitTree(cx, bce, lhs->pn_right))
            return false;
        offset += 2;
        break;
      case PNK_ARRAY:
      case PNK_OBJECT:
        break;
      case PNK_CALL:
        MOZ_ASSERT(lhs->pn_xflags & PNX_SETCALL);
        if (!EmitTree(cx, bce, lhs))
            return false;
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
        break;
      default:
        MOZ_ASSERT(0);
    }

    if (op != JSOP_NOP) {
        MOZ_ASSERT(rhs);
        switch (lhs->getKind()) {
          case PNK_NAME:
            if (lhs->isConst()) {
                if (lhs->isOp(JSOP_CALLEE)) {
                    if (Emit1(cx, bce, JSOP_CALLEE) < 0)
                        return false;
                } else if (lhs->isOp(JSOP_GETNAME) || lhs->isOp(JSOP_GETGNAME)) {
                    if (!EmitIndex32(cx, lhs->getOp(), atomIndex, bce))
                        return false;
                } else {
                    MOZ_ASSERT(JOF_OPTYPE(lhs->getOp()) != JOF_ATOM);
                    if (!EmitVarOp(cx, lhs, lhs->getOp(), bce))
                        return false;
                }
            } else if (lhs->isOp(JSOP_SETNAME) || lhs->isOp(JSOP_STRICTSETNAME)) {
                if (Emit1(cx, bce, JSOP_DUP) < 0)
                    return false;
                if (!EmitIndex32(cx, JSOP_GETXPROP, atomIndex, bce))
                    return false;
            } else if (lhs->isOp(JSOP_SETGNAME) || lhs->isOp(JSOP_STRICTSETGNAME)) {
                MOZ_ASSERT(lhs->pn_cookie.isFree());
                if (!EmitAtomOp(cx, lhs, JSOP_GETGNAME, bce))
                    return false;
            } else if (lhs->isOp(JSOP_SETINTRINSIC)) {
                MOZ_ASSERT(lhs->pn_cookie.isFree());
                if (!EmitAtomOp(cx, lhs, JSOP_GETINTRINSIC, bce))
                    return false;
            } else {
                JSOp op;
                switch (lhs->getOp()) {
                  case JSOP_SETARG: op = JSOP_GETARG; break;
                  case JSOP_SETLOCAL: op = JSOP_GETLOCAL; break;
                  case JSOP_SETALIASEDVAR: op = JSOP_GETALIASEDVAR; break;
                  default: MOZ_CRASH("Bad op");
                }
                if (!EmitVarOp(cx, lhs, op, bce))
                    return false;
            }
            break;
          case PNK_DOT: {
            if (Emit1(cx, bce, JSOP_DUP) < 0)
                return false;
            bool isLength = (lhs->pn_atom == cx->names().length);
            if (!EmitIndex32(cx, isLength ? JSOP_LENGTH : JSOP_GETPROP, atomIndex, bce))
                return false;
            break;
          }
          case PNK_ELEM:
            if (Emit1(cx, bce, JSOP_DUP2) < 0)
                return false;
            if (!EmitElemOpBase(cx, bce, JSOP_GETELEM))
                return false;
            break;
          case PNK_CALL:
            /*
             * We just emitted a JSOP_SETCALL (which will always throw) and
             * popped the call's return value. Push a random value to make sure
             * the stack depth is correct.
             */
            MOZ_ASSERT(lhs->pn_xflags & PNX_SETCALL);
            if (Emit1(cx, bce, JSOP_NULL) < 0)
                return false;
            break;
          default:;
        }
    }

    /* Now emit the right operand (it may affect the namespace). */
    if (rhs) {
        if (!EmitTree(cx, bce, rhs))
            return false;
    } else {
        /*
         * The value to assign is the next enumeration value in a for-in or
         * for-of loop.  That value has already been emitted: by JSOP_ITERNEXT
         * in the for-in case, or via a GETPROP "value" on the result object in
         * the for-of case.  If offset == 1, that slot is already at the top of
         * the stack. Otherwise, rearrange the stack to put that value on top.
         */
        if (offset != 1 && Emit2(cx, bce, JSOP_PICK, offset - 1) < 0)
            return false;
    }

    /* If += etc., emit the binary operator with a source note. */
    if (op != JSOP_NOP) {
        /*
         * Take care to avoid SRC_ASSIGNOP if the left-hand side is a const
         * declared in the current compilation unit, as in this case (just
         * a bit further below) we will avoid emitting the assignment op.
         */
        if (!lhs->isKind(PNK_NAME) || !lhs->isConst()) {
            if (NewSrcNote(cx, bce, SRC_ASSIGNOP) < 0)
                return false;
        }
        if (Emit1(cx, bce, op) < 0)
            return false;
    }

    /* Finally, emit the specialized assignment bytecode. */
    switch (lhs->getKind()) {
      case PNK_NAME:
        if (lhs->isOp(JSOP_SETARG) || lhs->isOp(JSOP_SETLOCAL) || lhs->isOp(JSOP_SETALIASEDVAR)) {
            if (!EmitVarOp(cx, lhs, lhs->getOp(), bce))
                return false;
        } else {
            if (!EmitIndexOp(cx, lhs->getOp(), atomIndex, bce))
                return false;
        }
        break;
      case PNK_DOT:
      {
        JSOp setOp = bce->sc->strict ? JSOP_STRICTSETPROP : JSOP_SETPROP;
        if (!EmitIndexOp(cx, setOp, atomIndex, bce))
            return false;
        break;
      }
      case PNK_CALL:
        /* Do nothing. The JSOP_SETCALL we emitted will always throw. */
        MOZ_ASSERT(lhs->pn_xflags & PNX_SETCALL);
        break;
      case PNK_ELEM:
      {
        JSOp setOp = bce->sc->strict ? JSOP_STRICTSETELEM : JSOP_SETELEM;
        if (Emit1(cx, bce, setOp) < 0)
            return false;
        break;
      }
      case PNK_ARRAY:
      case PNK_OBJECT:
        if (!EmitDestructuringOps(cx, bce, lhs))
            return false;
        break;
      default:
        MOZ_ASSERT(0);
    }
    return true;
}

bool
ParseNode::getConstantValue(ExclusiveContext* cx, AllowConstantObjects allowObjects, MutableHandleValue vp)
{
    switch (getKind()) {
      case PNK_NUMBER:
        vp.setNumber(pn_dval);
        return true;
      case PNK_TEMPLATE_STRING:
      case PNK_STRING:
        vp.setString(pn_atom);
        return true;
      case PNK_TRUE:
        vp.setBoolean(true);
        return true;
      case PNK_FALSE:
        vp.setBoolean(false);
        return true;
      case PNK_NULL:
        vp.setNull();
        return true;
      case PNK_CALLSITEOBJ:
      case PNK_ARRAY: {
        RootedValue value(cx);
        unsigned count;
        ParseNode* pn;

        if (allowObjects == DontAllowObjects) {
            vp.setMagic(JS_GENERIC_MAGIC);
            return true;
        }
        if (allowObjects == DontAllowNestedObjects)
            allowObjects = DontAllowObjects;

        if (getKind() == PNK_CALLSITEOBJ) {
            count = pn_count - 1;
            pn = pn_head->pn_next;
        } else {
            MOZ_ASSERT(isOp(JSOP_NEWINIT) && !(pn_xflags & PNX_NONCONST));
            count = pn_count;
            pn = pn_head;
        }

        RootedArrayObject obj(cx, NewDenseFullyAllocatedArray(cx, count, NullPtr(),
                                                              MaybeSingletonObject));
        if (!obj)
            return false;

        unsigned idx = 0;
        RootedId id(cx);
        for (; pn; idx++, pn = pn->pn_next) {
            if (!pn->getConstantValue(cx, allowObjects, &value))
                return false;
            if (value.isMagic(JS_GENERIC_MAGIC)) {
                vp.setMagic(JS_GENERIC_MAGIC);
                return true;
            }
            id = INT_TO_JSID(idx);
            if (!DefineProperty(cx, obj, id, value, nullptr, nullptr, JSPROP_ENUMERATE))
                return false;
        }
        MOZ_ASSERT(idx == count);

        ObjectGroup::fixArrayGroup(cx, obj);
        vp.setObject(*obj);
        return true;
      }
      case PNK_OBJECT: {
        MOZ_ASSERT(isOp(JSOP_NEWINIT));
        MOZ_ASSERT(!(pn_xflags & PNX_NONCONST));

        if (allowObjects == DontAllowObjects) {
            vp.setMagic(JS_GENERIC_MAGIC);
            return true;
        }
        if (allowObjects == DontAllowNestedObjects)
            allowObjects = DontAllowObjects;

        gc::AllocKind kind = GuessObjectGCKind(pn_count);
        RootedPlainObject obj(cx,
            NewBuiltinClassInstance<PlainObject>(cx, kind, MaybeSingletonObject));
        if (!obj)
            return false;

        RootedValue value(cx), idvalue(cx);
        for (ParseNode* pn = pn_head; pn; pn = pn->pn_next) {
            if (!pn->pn_right->getConstantValue(cx, allowObjects, &value))
                return false;
            if (value.isMagic(JS_GENERIC_MAGIC)) {
                vp.setMagic(JS_GENERIC_MAGIC);
                return true;
            }

            ParseNode* pnid = pn->pn_left;
            if (pnid->isKind(PNK_NUMBER)) {
                idvalue = NumberValue(pnid->pn_dval);
            } else {
                MOZ_ASSERT(pnid->isKind(PNK_OBJECT_PROPERTY_NAME) || pnid->isKind(PNK_STRING));
                MOZ_ASSERT(pnid->pn_atom != cx->names().proto);
                idvalue = StringValue(pnid->pn_atom);
            }

            uint32_t index;
            if (IsDefinitelyIndex(idvalue, &index)) {
                if (!DefineElement(cx, obj, index, value, nullptr, nullptr, JSPROP_ENUMERATE))
                    return false;

                continue;
            }

            JSAtom* name = ToAtom<CanGC>(cx, idvalue);
            if (!name)
                return false;

            if (name->isIndex(&index)) {
                if (!DefineElement(cx, obj, index, value, nullptr, nullptr, JSPROP_ENUMERATE))
                    return false;
            } else {
                if (!DefineProperty(cx, obj, name->asPropertyName(), value,
                                    nullptr, nullptr, JSPROP_ENUMERATE))
                {
                    return false;
                }
            }
        }

        ObjectGroup::fixPlainObjectGroup(cx, obj);
        vp.setObject(*obj);
        return true;
      }
      default:
        MOZ_CRASH("Unexpected node");
    }
    return false;
}

static bool
EmitSingletonInitialiser(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    RootedValue value(cx);
    if (!pn->getConstantValue(cx, ParseNode::AllowObjects, &value))
        return false;

    RootedNativeObject obj(cx, &value.toObject().as<NativeObject>());
    if (!obj->is<ArrayObject>() && !JSObject::setSingleton(cx, obj))
        return false;

    ObjectBox* objbox = bce->parser->newObjectBox(obj);
    if (!objbox)
        return false;

    return EmitObjectOp(cx, objbox, JSOP_OBJECT, bce);
}

static bool
EmitCallSiteObject(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    RootedValue value(cx);
    if (!pn->getConstantValue(cx, ParseNode::AllowObjects, &value))
        return false;

    MOZ_ASSERT(value.isObject());

    ObjectBox* objbox1 = bce->parser->newObjectBox(&value.toObject().as<NativeObject>());
    if (!objbox1)
        return false;

    if (!pn->as<CallSiteNode>().getRawArrayValue(cx, &value))
        return false;

    MOZ_ASSERT(value.isObject());

    ObjectBox* objbox2 = bce->parser->newObjectBox(&value.toObject().as<NativeObject>());
    if (!objbox2)
        return false;

    return EmitObjectPairOp(cx, objbox1, objbox2, JSOP_CALLSITEOBJ, bce);
}

/* See the SRC_FOR source note offsetBias comments later in this file. */
JS_STATIC_ASSERT(JSOP_NOP_LENGTH == 1);
JS_STATIC_ASSERT(JSOP_POP_LENGTH == 1);

namespace {

class EmitLevelManager
{
    BytecodeEmitter* bce;
  public:
    explicit EmitLevelManager(BytecodeEmitter* bce) : bce(bce) { bce->emitLevel++; }
    ~EmitLevelManager() { bce->emitLevel--; }
};

} /* anonymous namespace */

static bool
EmitCatch(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    /*
     * Morph STMT_BLOCK to STMT_CATCH, note the block entry code offset,
     * and save the block object atom.
     */
    StmtInfoBCE* stmt = bce->topStmt;
    MOZ_ASSERT(stmt->type == STMT_BLOCK && stmt->isBlockScope);
    stmt->type = STMT_CATCH;

    /* Go up one statement info record to the TRY or FINALLY record. */
    stmt = stmt->down;
    MOZ_ASSERT(stmt->type == STMT_TRY || stmt->type == STMT_FINALLY);

    /* Pick up the pending exception and bind it to the catch variable. */
    if (Emit1(cx, bce, JSOP_EXCEPTION) < 0)
        return false;

    /*
     * Dup the exception object if there is a guard for rethrowing to use
     * it later when rethrowing or in other catches.
     */
    if (pn->pn_kid2 && Emit1(cx, bce, JSOP_DUP) < 0)
        return false;

    ParseNode* pn2 = pn->pn_kid1;
    switch (pn2->getKind()) {
      case PNK_ARRAY:
      case PNK_OBJECT:
        if (!EmitDestructuringOps(cx, bce, pn2))
            return false;
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
        break;

      case PNK_NAME:
        /* Inline and specialize BindNameToSlot for pn2. */
        MOZ_ASSERT(!pn2->pn_cookie.isFree());
        if (!EmitVarOp(cx, pn2, JSOP_INITLEXICAL, bce))
            return false;
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
        break;

      default:
        MOZ_ASSERT(0);
    }

    // If there is a guard expression, emit it and arrange to jump to the next
    // catch block if the guard expression is false.
    if (pn->pn_kid2) {
        if (!EmitTree(cx, bce, pn->pn_kid2))
            return false;

        // If the guard expression is false, fall through, pop the block scope,
        // and jump to the next catch block.  Otherwise jump over that code and
        // pop the dupped exception.
        ptrdiff_t guardCheck = EmitJump(cx, bce, JSOP_IFNE, 0);
        if (guardCheck < 0)
            return false;

        {
            NonLocalExitScope nle(cx, bce);

            // Move exception back to cx->exception to prepare for
            // the next catch.
            if (Emit1(cx, bce, JSOP_THROWING) < 0)
                return false;

            // Leave the scope for this catch block.
            if (!nle.prepareForNonLocalJump(stmt))
                return false;

            // Jump to the next handler.  The jump target is backpatched by EmitTry.
            ptrdiff_t guardJump = EmitJump(cx, bce, JSOP_GOTO, 0);
            if (guardJump < 0)
                return false;
            stmt->guardJump() = guardJump;
        }

        // Back to normal control flow.
        SetJumpOffsetAt(bce, guardCheck);

        // Pop duplicated exception object as we no longer need it.
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
    }

    /* Emit the catch body. */
    return EmitTree(cx, bce, pn->pn_kid3);
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See the
// comment on EmitSwitch.
//
MOZ_NEVER_INLINE static bool
EmitTry(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    StmtInfoBCE stmtInfo(cx);

    // Push stmtInfo to track jumps-over-catches and gosubs-to-finally
    // for later fixup.
    //
    // When a finally block is active (STMT_FINALLY in our parse context),
    // non-local jumps (including jumps-over-catches) result in a GOSUB
    // being written into the bytecode stream and fixed-up later (c.f.
    // EmitBackPatchOp and BackPatch).
    //
    PushStatementBCE(bce, &stmtInfo, pn->pn_kid3 ? STMT_FINALLY : STMT_TRY, bce->offset());

    // Since an exception can be thrown at any place inside the try block,
    // we need to restore the stack and the scope chain before we transfer
    // the control to the exception handler.
    //
    // For that we store in a try note associated with the catch or
    // finally block the stack depth upon the try entry. The interpreter
    // uses this depth to properly unwind the stack and the scope chain.
    //
    int depth = bce->stackDepth;

    // Record the try location, then emit the try block.
    ptrdiff_t noteIndex = NewSrcNote(cx, bce, SRC_TRY);
    if (noteIndex < 0 || Emit1(cx, bce, JSOP_TRY) < 0)
        return false;
    ptrdiff_t tryStart = bce->offset();
    if (!EmitTree(cx, bce, pn->pn_kid1))
        return false;
    MOZ_ASSERT(depth == bce->stackDepth);

    // GOSUB to finally, if present.
    if (pn->pn_kid3) {
        if (EmitBackPatchOp(cx, bce, &stmtInfo.gosubs()) < 0)
            return false;
    }

    // Source note points to the jump at the end of the try block.
    if (!SetSrcNoteOffset(cx, bce, noteIndex, 0, bce->offset() - tryStart + JSOP_TRY_LENGTH))
        return false;

    // Emit jump over catch and/or finally.
    ptrdiff_t catchJump = -1;
    if (EmitBackPatchOp(cx, bce, &catchJump) < 0)
        return false;

    ptrdiff_t tryEnd = bce->offset();

    // If this try has a catch block, emit it.
    ParseNode* catchList = pn->pn_kid2;
    if (catchList) {
        MOZ_ASSERT(catchList->isKind(PNK_CATCHLIST));

        // The emitted code for a catch block looks like:
        //
        // [pushblockscope]             only if any local aliased
        // exception
        // if there is a catchguard:
        //   dup
        // setlocal 0; pop              assign or possibly destructure exception
        // if there is a catchguard:
        //   < catchguard code >
        //   ifne POST
        //   debugleaveblock
        //   [popblockscope]            only if any local aliased
        //   throwing                   pop exception to cx->exception
        //   goto <next catch block>
        //   POST: pop
        // < catch block contents >
        // debugleaveblock
        // [popblockscope]              only if any local aliased
        // goto <end of catch blocks>   non-local; finally applies
        //
        // If there's no catch block without a catchguard, the last <next catch
        // block> points to rethrow code.  This code will [gosub] to the finally
        // code if appropriate, and is also used for the catch-all trynote for
        // capturing exceptions thrown from catch{} blocks.
        //
        for (ParseNode* pn3 = catchList->pn_head; pn3; pn3 = pn3->pn_next) {
            MOZ_ASSERT(bce->stackDepth == depth);

            // Emit the lexical scope and catch body.
            MOZ_ASSERT(pn3->isKind(PNK_LEXICALSCOPE));
            if (!EmitTree(cx, bce, pn3))
                return false;

            // gosub <finally>, if required.
            if (pn->pn_kid3) {
                if (EmitBackPatchOp(cx, bce, &stmtInfo.gosubs()) < 0)
                    return false;
                MOZ_ASSERT(bce->stackDepth == depth);
            }

            // Jump over the remaining catch blocks.  This will get fixed
            // up to jump to after catch/finally.
            if (EmitBackPatchOp(cx, bce, &catchJump) < 0)
                return false;

            // If this catch block had a guard clause, patch the guard jump to
            // come here.
            if (stmtInfo.guardJump() != -1) {
                SetJumpOffsetAt(bce, stmtInfo.guardJump());
                stmtInfo.guardJump() = -1;

                // If this catch block is the last one, rethrow, delegating
                // execution of any finally block to the exception handler.
                if (!pn3->pn_next) {
                    if (Emit1(cx, bce, JSOP_EXCEPTION) < 0)
                        return false;
                    if (Emit1(cx, bce, JSOP_THROW) < 0)
                        return false;
                }
            }
        }
    }

    MOZ_ASSERT(bce->stackDepth == depth);

    // Emit the finally handler, if there is one.
    ptrdiff_t finallyStart = 0;
    if (pn->pn_kid3) {
        // Fix up the gosubs that might have been emitted before non-local
        // jumps to the finally code.
        if (!BackPatch(cx, bce, stmtInfo.gosubs(), bce->code().end(), JSOP_GOSUB))
            return false;

        finallyStart = bce->offset();

        // Indicate that we're emitting a subroutine body.
        stmtInfo.type = STMT_SUBROUTINE;
        if (!UpdateSourceCoordNotes(cx, bce, pn->pn_kid3->pn_pos.begin))
            return false;
        if (Emit1(cx, bce, JSOP_FINALLY) < 0 ||
            !EmitTree(cx, bce, pn->pn_kid3) ||
            Emit1(cx, bce, JSOP_RETSUB) < 0)
        {
            return false;
        }
        bce->hasTryFinally = true;
        MOZ_ASSERT(bce->stackDepth == depth);
    }
    if (!PopStatementBCE(cx, bce))
        return false;

    // ReconstructPCStack needs a NOP here to mark the end of the last catch block.
    if (Emit1(cx, bce, JSOP_NOP) < 0)
        return false;

    // Fix up the end-of-try/catch jumps to come here.
    if (!BackPatch(cx, bce, catchJump, bce->code().end(), JSOP_GOTO))
        return false;

    // Add the try note last, to let post-order give us the right ordering
    // (first to last for a given nesting level, inner to outer by level).
    if (catchList && !bce->tryNoteList.append(JSTRY_CATCH, depth, tryStart, tryEnd))
        return false;

    // If we've got a finally, mark try+catch region with additional
    // trynote to catch exceptions (re)thrown from a catch block or
    // for the try{}finally{} case.
    if (pn->pn_kid3 && !bce->tryNoteList.append(JSTRY_FINALLY, depth, tryStart, finallyStart))
        return false;

    return true;
}

static bool
EmitIf(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    StmtInfoBCE stmtInfo(cx);

    /* Initialize so we can detect else-if chains and avoid recursion. */
    stmtInfo.type = STMT_IF;
    ptrdiff_t beq = -1;
    ptrdiff_t jmp = -1;
    ptrdiff_t noteIndex = -1;

  if_again:
    /* Emit code for the condition before pushing stmtInfo. */
    if (!EmitTree(cx, bce, pn->pn_kid1))
        return false;
    ptrdiff_t top = bce->offset();
    if (stmtInfo.type == STMT_IF) {
        PushStatementBCE(bce, &stmtInfo, STMT_IF, top);
    } else {
        /*
         * We came here from the goto further below that detects else-if
         * chains, so we must mutate stmtInfo back into a STMT_IF record.
         * Also we need a note offset for SRC_IF_ELSE to help IonMonkey.
         */
        MOZ_ASSERT(stmtInfo.type == STMT_ELSE);
        stmtInfo.type = STMT_IF;
        stmtInfo.update = top;
        if (!SetSrcNoteOffset(cx, bce, noteIndex, 0, jmp - beq))
            return false;
    }

    /* Emit an annotated branch-if-false around the then part. */
    ParseNode* pn3 = pn->pn_kid3;
    noteIndex = NewSrcNote(cx, bce, pn3 ? SRC_IF_ELSE : SRC_IF);
    if (noteIndex < 0)
        return false;
    beq = EmitJump(cx, bce, JSOP_IFEQ, 0);
    if (beq < 0)
        return false;

    /* Emit code for the then and optional else parts. */
    if (!EmitTree(cx, bce, pn->pn_kid2))
        return false;
    if (pn3) {
        /* Modify stmtInfo so we know we're in the else part. */
        stmtInfo.type = STMT_ELSE;

        /*
         * Emit a JSOP_BACKPATCH op to jump from the end of our then part
         * around the else part.  The PopStatementBCE call at the bottom of
         * this function will fix up the backpatch chain linked from
         * stmtInfo.breaks.
         */
        jmp = EmitGoto(cx, bce, &stmtInfo, &stmtInfo.breaks);
        if (jmp < 0)
            return false;

        /* Ensure the branch-if-false comes here, then emit the else. */
        SetJumpOffsetAt(bce, beq);
        if (pn3->isKind(PNK_IF)) {
            pn = pn3;
            goto if_again;
        }

        if (!EmitTree(cx, bce, pn3))
            return false;

        /*
         * Annotate SRC_IF_ELSE with the offset from branch to jump, for
         * IonMonkey's benefit.  We can't just "back up" from the pc
         * of the else clause, because we don't know whether an extended
         * jump was required to leap from the end of the then clause over
         * the else clause.
         */
        if (!SetSrcNoteOffset(cx, bce, noteIndex, 0, jmp - beq))
            return false;
    } else {
        /* No else part, fixup the branch-if-false to come here. */
        SetJumpOffsetAt(bce, beq);
    }
    return PopStatementBCE(cx, bce);
}

/*
 * pnLet represents one of:
 *
 *   let-expression:   (let (x = y) EXPR)
 *   let-statement:    let (x = y) { ... }
 *
 * For a let-expression 'let (x = a, [y,z] = b) e', EmitLet produces:
 *
 *  bytecode          stackDepth  srcnotes
 *  evaluate a        +1
 *  evaluate b        +1
 *  dup               +1
 *  destructure y
 *  pick 1
 *  dup               +1
 *  destructure z
 *  pick 1
 *  pop               -1
 *  setlocal 2        -1
 *  setlocal 1        -1
 *  setlocal 0        -1
 *  pushblockscope (if needed)
 *  evaluate e        +1
 *  debugleaveblock
 *  popblockscope (if needed)
 *
 * Note that, since pushblockscope simply changes fp->scopeChain and does not
 * otherwise touch the stack, evaluation of the let-var initializers must leave
 * the initial value in the let-var's future slot.
 */
/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
 * the comment on EmitSwitch.
 */
MOZ_NEVER_INLINE static bool
EmitLet(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pnLet)
{
    MOZ_ASSERT(pnLet->isArity(PN_BINARY));
    ParseNode* varList = pnLet->pn_left;
    MOZ_ASSERT(varList->isArity(PN_LIST));
    ParseNode* letBody = pnLet->pn_right;
    MOZ_ASSERT(letBody->isLexical() && letBody->isKind(PNK_LEXICALSCOPE));

    int letHeadDepth = bce->stackDepth;

    if (!EmitVariables(cx, bce, varList, PushInitialValues, true))
        return false;

    /* Push storage for hoisted let decls (e.g. 'let (x) { let y }'). */
    uint32_t valuesPushed = bce->stackDepth - letHeadDepth;
    StmtInfoBCE stmtInfo(cx);
    if (!EnterBlockScope(cx, bce, &stmtInfo, letBody->pn_objbox, JSOP_UNINITIALIZED, valuesPushed))
        return false;

    if (!EmitTree(cx, bce, letBody->pn_expr))
        return false;

    if (!LeaveNestedScope(cx, bce, &stmtInfo))
        return false;

    return true;
}

/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
 * the comment on EmitSwitch.
 */
MOZ_NEVER_INLINE static bool
EmitLexicalScope(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_LEXICALSCOPE));

    StmtInfoBCE stmtInfo(cx);
    if (!EnterBlockScope(cx, bce, &stmtInfo, pn->pn_objbox, JSOP_UNINITIALIZED, 0))
        return false;

    if (!EmitTree(cx, bce, pn->pn_expr))
        return false;

    if (!LeaveNestedScope(cx, bce, &stmtInfo))
        return false;

    return true;
}

static bool
EmitWith(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    StmtInfoBCE stmtInfo(cx);
    if (!EmitTree(cx, bce, pn->pn_left))
        return false;
    if (!EnterNestedScope(cx, bce, &stmtInfo, pn->pn_binary_obj, STMT_WITH))
        return false;
    if (!EmitTree(cx, bce, pn->pn_right))
        return false;
    if (!LeaveNestedScope(cx, bce, &stmtInfo))
        return false;
    return true;
}

/**
 * EmitIterator expects the iterable to already be on the stack.
 * It will replace that stack value with the corresponding iterator
 */
static bool
EmitIterator(ExclusiveContext* cx, BytecodeEmitter* bce)
{
    // Convert iterable to iterator.
    if (Emit1(cx, bce, JSOP_DUP) < 0)                          // OBJ OBJ
        return false;
    if (Emit2(cx, bce, JSOP_SYMBOL, jsbytecode(JS::SymbolCode::iterator)) < 0) // OBJ OBJ @@ITERATOR
        return false;
    if (!EmitElemOpBase(cx, bce, JSOP_CALLELEM))               // OBJ ITERFN
        return false;
    if (Emit1(cx, bce, JSOP_SWAP) < 0)                         // ITERFN OBJ
        return false;
    if (EmitCall(cx, bce, JSOP_CALL, 0) < 0)                   // ITER
        return false;
    CheckTypeSet(cx, bce, JSOP_CALL);
    return true;
}

static bool
EmitForInOrOfVariables(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, bool* letDecl)
{
    *letDecl = pn->isKind(PNK_LEXICALSCOPE);
    MOZ_ASSERT_IF(*letDecl, pn->isLexical());

    // If the left part is 'var x', emit code to define x if necessary using a
    // prolog opcode, but do not emit a pop. If it is 'let x', EnterBlockScope
    // will initialize let bindings in EmitForOf and EmitForIn with
    // undefineds.
    //
    // Due to the horror of legacy comprehensions, there is a third case where
    // we have PNK_LET without a lexical scope, because those expressions are
    // parsed with single lexical scope for the entire comprehension. In this
    // case we must initialize the lets to not trigger dead zone checks via
    // InitializeVars.
    if (!*letDecl) {
        bce->emittingForInit = true;
        if (pn->isKind(PNK_VAR)) {
            if (!EmitVariables(cx, bce, pn, DefineVars))
                return false;
        } else {
            MOZ_ASSERT(pn->isKind(PNK_LET));
            if (!EmitVariables(cx, bce, pn, InitializeVars))
                return false;
        }
        bce->emittingForInit = false;
    }

    return true;
}


/**
 * If type is STMT_FOR_OF_LOOP, it emits bytecode for for-of loop.
 * pn should be PNK_FOR, and pn->pn_left should be PNK_FOROF.
 *
 * If type is STMT_SPREAD, it emits bytecode for spread operator.
 * pn should be nullptr.
 * Please refer the comment above EmitSpread for additional information about
 * stack convention.
 */
static bool
EmitForOf(ExclusiveContext* cx, BytecodeEmitter* bce, StmtType type, ParseNode* pn, ptrdiff_t top)
{
    MOZ_ASSERT(type == STMT_FOR_OF_LOOP || type == STMT_SPREAD);
    MOZ_ASSERT_IF(type == STMT_FOR_OF_LOOP, pn && pn->pn_left->isKind(PNK_FOROF));
    MOZ_ASSERT_IF(type == STMT_SPREAD, !pn);

    ParseNode* forHead = pn ? pn->pn_left : nullptr;
    ParseNode* forHeadExpr = forHead ? forHead->pn_kid3 : nullptr;
    ParseNode* forBody = pn ? pn->pn_right : nullptr;

    ParseNode* pn1 = forHead ? forHead->pn_kid1 : nullptr;
    bool letDecl = false;
    if (pn1 && !EmitForInOrOfVariables(cx, bce, pn1, &letDecl))
        return false;

    if (type == STMT_FOR_OF_LOOP) {
        // For-of loops run with two values on the stack: the iterator and the
        // current result object.

        // Compile the object expression to the right of 'of'.
        if (!EmitTree(cx, bce, forHeadExpr))
            return false;
        if (!EmitIterator(cx, bce))
            return false;

        // Push a dummy result so that we properly enter iteration midstream.
        if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)                // ITER RESULT
            return false;
    }

    // Enter the block before the loop body, after evaluating the obj.
    // Initialize let bindings with undefined when entering, as the name
    // assigned to is a plain assignment.
    StmtInfoBCE letStmt(cx);
    if (letDecl) {
        if (!EnterBlockScope(cx, bce, &letStmt, pn1->pn_objbox, JSOP_UNDEFINED, 0))
            return false;
    }

    LoopStmtInfo stmtInfo(cx);
    PushLoopStatement(bce, &stmtInfo, type, top);

    // Jump down to the loop condition to minimize overhead assuming at least
    // one iteration, as the other loop forms do.  Annotate so IonMonkey can
    // find the loop-closing jump.
    int noteIndex = NewSrcNote(cx, bce, SRC_FOR_OF);
    if (noteIndex < 0)
        return false;
    ptrdiff_t jmp = EmitJump(cx, bce, JSOP_GOTO, 0);
    if (jmp < 0)
        return false;

    top = bce->offset();
    SET_STATEMENT_TOP(&stmtInfo, top);
    if (EmitLoopHead(cx, bce, nullptr) < 0)
        return false;

    if (type == STMT_SPREAD)
        bce->stackDepth++;

#ifdef DEBUG
    int loopDepth = bce->stackDepth;
#endif

    // Emit code to assign result.value to the iteration variable.
    if (type == STMT_FOR_OF_LOOP) {
        if (Emit1(cx, bce, JSOP_DUP) < 0)                      // ITER RESULT RESULT
            return false;
    }
    if (!EmitAtomOp(cx, cx->names().value, JSOP_GETPROP, bce)) // ... RESULT VALUE
        return false;
    if (type == STMT_FOR_OF_LOOP) {
        if (!EmitAssignment(cx, bce, forHead->pn_kid2, JSOP_NOP, nullptr)) // ITER RESULT VALUE
            return false;
        if (Emit1(cx, bce, JSOP_POP) < 0)                      // ITER RESULT
            return false;

        // The stack should be balanced around the assignment opcode sequence.
        MOZ_ASSERT(bce->stackDepth == loopDepth);

        // Emit code for the loop body.
        if (!EmitTree(cx, bce, forBody))
            return false;

        // Set loop and enclosing "update" offsets, for continue.
        StmtInfoBCE* stmt = &stmtInfo;
        do {
            stmt->update = bce->offset();
        } while ((stmt = stmt->down) != nullptr && stmt->type == STMT_LABEL);
    } else {
        if (Emit1(cx, bce, JSOP_INITELEM_INC) < 0)             // ITER ARR (I+1)
            return false;

        MOZ_ASSERT(bce->stackDepth == loopDepth - 1);

        // STMT_SPREAD never contain continue, so do not set "update" offset.
    }

    // COME FROM the beginning of the loop to here.
    SetJumpOffsetAt(bce, jmp);
    if (!EmitLoopEntry(cx, bce, forHeadExpr))
        return false;

    if (type == STMT_FOR_OF_LOOP) {
        if (Emit1(cx, bce, JSOP_POP) < 0)                      // ITER
            return false;
        if (Emit1(cx, bce, JSOP_DUP) < 0)                      // ITER ITER
            return false;
    } else {
        if (!EmitDupAt(cx, bce, bce->stackDepth - 1 - 2))      // ITER ARR I ITER
            return false;
    }
    if (!EmitIteratorNext(cx, bce, forHead))                   // ... RESULT
        return false;
    if (Emit1(cx, bce, JSOP_DUP) < 0)                          // ... RESULT RESULT
        return false;
    if (!EmitAtomOp(cx, cx->names().done, JSOP_GETPROP, bce))  // ... RESULT DONE?
        return false;

    ptrdiff_t beq = EmitJump(cx, bce, JSOP_IFEQ, top - bce->offset()); // ... RESULT
    if (beq < 0)
        return false;

    MOZ_ASSERT(bce->stackDepth == loopDepth);

    // Let Ion know where the closing jump of this loop is.
    if (!SetSrcNoteOffset(cx, bce, (unsigned)noteIndex, 0, beq - jmp))
        return false;

    // Fixup breaks and continues.
    // For STMT_SPREAD, just pop pc->topStmt.
    if (!PopStatementBCE(cx, bce))
        return false;

    if (!bce->tryNoteList.append(JSTRY_FOR_OF, bce->stackDepth, top, bce->offset()))
        return false;

    if (letDecl) {
        if (!LeaveNestedScope(cx, bce, &letStmt))
            return false;
    }

    if (type == STMT_SPREAD) {
        if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)3) < 0)      // ARR I RESULT ITER
            return false;
    }

    // Pop the result and the iter.
    EMIT_UINT16_IMM_OP(JSOP_POPN, 2);

    return true;
}

static bool
EmitForIn(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, ptrdiff_t top)
{
    ParseNode* forHead = pn->pn_left;
    ParseNode* forBody = pn->pn_right;

    ParseNode* pn1 = forHead->pn_kid1;
    bool letDecl = false;
    if (pn1 && !EmitForInOrOfVariables(cx, bce, pn1, &letDecl))
        return false;

    /* Compile the object expression to the right of 'in'. */
    if (!EmitTree(cx, bce, forHead->pn_kid3))
        return false;

    /*
     * Emit a bytecode to convert top of stack value to the iterator
     * object depending on the loop variant (for-in, for-each-in, or
     * destructuring for-in).
     */
    MOZ_ASSERT(pn->isOp(JSOP_ITER));
    if (Emit2(cx, bce, JSOP_ITER, (uint8_t) pn->pn_iflags) < 0)
        return false;

    // For-in loops have both the iterator and the value on the stack. Push
    // undefined to balance the stack.
    if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
        return false;

    // Enter the block before the loop body, after evaluating the obj.
    // Initialize let bindings with undefined when entering, as the name
    // assigned to is a plain assignment.
    StmtInfoBCE letStmt(cx);
    if (letDecl) {
        if (!EnterBlockScope(cx, bce, &letStmt, pn1->pn_objbox, JSOP_UNDEFINED, 0))
            return false;
    }

    LoopStmtInfo stmtInfo(cx);
    PushLoopStatement(bce, &stmtInfo, STMT_FOR_IN_LOOP, top);

    /* Annotate so IonMonkey can find the loop-closing jump. */
    int noteIndex = NewSrcNote(cx, bce, SRC_FOR_IN);
    if (noteIndex < 0)
        return false;

    /*
     * Jump down to the loop condition to minimize overhead assuming at
     * least one iteration, as the other loop forms do.
     */
    ptrdiff_t jmp = EmitJump(cx, bce, JSOP_GOTO, 0);
    if (jmp < 0)
        return false;

    top = bce->offset();
    SET_STATEMENT_TOP(&stmtInfo, top);
    if (EmitLoopHead(cx, bce, nullptr) < 0)
        return false;

#ifdef DEBUG
    int loopDepth = bce->stackDepth;
#endif

    // Emit code to assign the enumeration value to the left hand side, but
    // also leave it on the stack.
    if (!EmitAssignment(cx, bce, forHead->pn_kid2, JSOP_NOP, nullptr))
        return false;

    /* The stack should be balanced around the assignment opcode sequence. */
    MOZ_ASSERT(bce->stackDepth == loopDepth);

    /* Emit code for the loop body. */
    if (!EmitTree(cx, bce, forBody))
        return false;

    /* Set loop and enclosing "update" offsets, for continue. */
    StmtInfoBCE* stmt = &stmtInfo;
    do {
        stmt->update = bce->offset();
    } while ((stmt = stmt->down) != nullptr && stmt->type == STMT_LABEL);

    /*
     * Fixup the goto that starts the loop to jump down to JSOP_MOREITER.
     */
    SetJumpOffsetAt(bce, jmp);
    if (!EmitLoopEntry(cx, bce, nullptr))
        return false;
    if (Emit1(cx, bce, JSOP_POP) < 0)
        return false;
    if (Emit1(cx, bce, JSOP_MOREITER) < 0)
        return false;
    if (Emit1(cx, bce, JSOP_ISNOITER) < 0)
        return false;
    ptrdiff_t beq = EmitJump(cx, bce, JSOP_IFEQ, top - bce->offset());
    if (beq < 0)
        return false;

    /* Set the srcnote offset so we can find the closing jump. */
    if (!SetSrcNoteOffset(cx, bce, (unsigned)noteIndex, 0, beq - jmp))
        return false;

    // Fix up breaks and continues.
    if (!PopStatementBCE(cx, bce))
        return false;

    // Pop the enumeration value.
    if (Emit1(cx, bce, JSOP_POP) < 0)
        return false;

    if (!bce->tryNoteList.append(JSTRY_FOR_IN, bce->stackDepth, top, bce->offset()))
        return false;
    if (Emit1(cx, bce, JSOP_ENDITER) < 0)
        return false;

    if (letDecl) {
        if (!LeaveNestedScope(cx, bce, &letStmt))
            return false;
    }

    return true;
}

static bool
EmitNormalFor(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, ptrdiff_t top)
{
    LoopStmtInfo stmtInfo(cx);
    PushLoopStatement(bce, &stmtInfo, STMT_FOR_LOOP, top);

    ParseNode* forHead = pn->pn_left;
    ParseNode* forBody = pn->pn_right;

    /* C-style for (init; cond; update) ... loop. */
    JSOp op = JSOP_POP;
    ParseNode* pn3 = forHead->pn_kid1;
    if (!pn3) {
        // No initializer, but emit a nop so that there's somewhere to put the
        // SRC_FOR annotation that IonBuilder will look for.
        op = JSOP_NOP;
    } else {
        bce->emittingForInit = true;
        if (!UpdateSourceCoordNotes(cx, bce, pn3->pn_pos.begin))
            return false;
        if (!EmitTree(cx, bce, pn3))
            return false;
        bce->emittingForInit = false;
    }

    /*
     * NB: the SRC_FOR note has offsetBias 1 (JSOP_{NOP,POP}_LENGTH).
     * Use tmp to hold the biased srcnote "top" offset, which differs
     * from the top local variable by the length of the JSOP_GOTO
     * emitted in between tmp and top if this loop has a condition.
     */
    int noteIndex = NewSrcNote(cx, bce, SRC_FOR);
    if (noteIndex < 0 || Emit1(cx, bce, op) < 0)
        return false;
    ptrdiff_t tmp = bce->offset();

    ptrdiff_t jmp = -1;
    if (forHead->pn_kid2) {
        /* Goto the loop condition, which branches back to iterate. */
        jmp = EmitJump(cx, bce, JSOP_GOTO, 0);
        if (jmp < 0)
            return false;
    } else {
        if (op != JSOP_NOP && Emit1(cx, bce, JSOP_NOP) < 0)
            return false;
    }

    top = bce->offset();
    SET_STATEMENT_TOP(&stmtInfo, top);

    /* Emit code for the loop body. */
    if (EmitLoopHead(cx, bce, forBody) < 0)
        return false;
    if (jmp == -1 && !EmitLoopEntry(cx, bce, forBody))
        return false;
    if (!EmitTree(cx, bce, forBody))
        return false;

    /* Set the second note offset so we can find the update part. */
    MOZ_ASSERT(noteIndex != -1);
    ptrdiff_t tmp2 = bce->offset();

    /* Set loop and enclosing "update" offsets, for continue. */
    StmtInfoBCE* stmt = &stmtInfo;
    do {
        stmt->update = bce->offset();
    } while ((stmt = stmt->down) != nullptr && stmt->type == STMT_LABEL);

    /* Check for update code to do before the condition (if any). */
    pn3 = forHead->pn_kid3;
    if (pn3) {
        if (!UpdateSourceCoordNotes(cx, bce, pn3->pn_pos.begin))
            return false;
        op = JSOP_POP;
        if (!EmitTree(cx, bce, pn3))
            return false;

        /* Always emit the POP or NOP to help IonBuilder. */
        if (Emit1(cx, bce, op) < 0)
            return false;

        /* Restore the absolute line number for source note readers. */
        uint32_t lineNum = bce->parser->tokenStream.srcCoords.lineNum(pn->pn_pos.end);
        if (bce->currentLine() != lineNum) {
            if (NewSrcNote2(cx, bce, SRC_SETLINE, ptrdiff_t(lineNum)) < 0)
                return false;
            bce->current->currentLine = lineNum;
            bce->current->lastColumn = 0;
        }
    }

    ptrdiff_t tmp3 = bce->offset();

    if (forHead->pn_kid2) {
        /* Fix up the goto from top to target the loop condition. */
        MOZ_ASSERT(jmp >= 0);
        SetJumpOffsetAt(bce, jmp);
        if (!EmitLoopEntry(cx, bce, forHead->pn_kid2))
            return false;

        if (!EmitTree(cx, bce, forHead->pn_kid2))
            return false;
    }

    /* Set the first note offset so we can find the loop condition. */
    if (!SetSrcNoteOffset(cx, bce, (unsigned)noteIndex, 0, tmp3 - tmp))
        return false;
    if (!SetSrcNoteOffset(cx, bce, (unsigned)noteIndex, 1, tmp2 - tmp))
        return false;
    /* The third note offset helps us find the loop-closing jump. */
    if (!SetSrcNoteOffset(cx, bce, (unsigned)noteIndex, 2, bce->offset() - tmp))
        return false;

    /* If no loop condition, just emit a loop-closing jump. */
    op = forHead->pn_kid2 ? JSOP_IFNE : JSOP_GOTO;
    if (EmitJump(cx, bce, op, top - bce->offset()) < 0)
        return false;

    if (!bce->tryNoteList.append(JSTRY_LOOP, bce->stackDepth, top, bce->offset()))
        return false;

    /* Now fixup all breaks and continues. */
    return PopStatementBCE(cx, bce);
}

static inline bool
EmitFor(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, ptrdiff_t top)
{
    if (pn->pn_left->isKind(PNK_FORIN))
        return EmitForIn(cx, bce, pn, top);

    if (pn->pn_left->isKind(PNK_FOROF))
        return EmitForOf(cx, bce, STMT_FOR_OF_LOOP, pn, top);

    MOZ_ASSERT(pn->pn_left->isKind(PNK_FORHEAD));
    return EmitNormalFor(cx, bce, pn, top);
}

static MOZ_NEVER_INLINE bool
EmitFunc(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    FunctionBox* funbox = pn->pn_funbox;
    RootedFunction fun(cx, funbox->function());
    MOZ_ASSERT_IF(fun->isInterpretedLazy(), fun->lazyScript());

    /*
     * Set the EMITTEDFUNCTION flag in function definitions once they have been
     * emitted. Function definitions that need hoisting to the top of the
     * function will be seen by EmitFunc in two places.
     */
    if (pn->pn_dflags & PND_EMITTEDFUNCTION) {
        MOZ_ASSERT_IF(fun->hasScript(), fun->nonLazyScript());
        MOZ_ASSERT(pn->functionIsHoisted());
        MOZ_ASSERT(bce->sc->isFunctionBox());
        return true;
    }

    pn->pn_dflags |= PND_EMITTEDFUNCTION;

    /*
     * Mark as singletons any function which will only be executed once, or
     * which is inner to a lambda we only expect to run once. In the latter
     * case, if the lambda runs multiple times then CloneFunctionObject will
     * make a deep clone of its contents.
     */
    if (fun->isInterpreted()) {
        bool singleton =
            bce->script->compileAndGo() &&
            fun->isInterpreted() &&
            (bce->checkSingletonContext() ||
             (!bce->isInLoop() && bce->isRunOnceLambda()));
        if (!JSFunction::setTypeForScriptedFunction(cx, fun, singleton))
            return false;

        if (fun->isInterpretedLazy()) {
            if (!fun->lazyScript()->sourceObject()) {
                JSObject* scope = EnclosingStaticScope(bce);
                JSObject* source = bce->script->sourceObject();
                fun->lazyScript()->setParent(scope, &source->as<ScriptSourceObject>());
            }
            if (bce->emittingRunOnceLambda)
                fun->lazyScript()->setTreatAsRunOnce();
        } else {
            SharedContext* outersc = bce->sc;

            if (outersc->isFunctionBox() && outersc->asFunctionBox()->mightAliasLocals())
                funbox->setMightAliasLocals();      // inherit mightAliasLocals from parent
            MOZ_ASSERT_IF(outersc->strict, funbox->strict);

            // Inherit most things (principals, version, etc) from the parent.
            Rooted<JSScript*> parent(cx, bce->script);
            CompileOptions options(cx, bce->parser->options());
            options.setMutedErrors(parent->mutedErrors())
                   .setCompileAndGo(parent->compileAndGo())
                   .setSelfHostingMode(parent->selfHosted())
                   .setNoScriptRval(false)
                   .setForEval(false)
                   .setVersion(parent->getVersion());

            Rooted<JSObject*> enclosingScope(cx, EnclosingStaticScope(bce));
            Rooted<JSObject*> sourceObject(cx, bce->script->sourceObject());
            Rooted<JSScript*> script(cx, JSScript::Create(cx, enclosingScope, false, options,
                                                          parent->staticLevel() + 1,
                                                          sourceObject,
                                                          funbox->bufStart, funbox->bufEnd));
            if (!script)
                return false;

            script->bindings = funbox->bindings;

            uint32_t lineNum = bce->parser->tokenStream.srcCoords.lineNum(pn->pn_pos.begin);
            BytecodeEmitter bce2(bce, bce->parser, funbox, script, /* lazyScript = */ js::NullPtr(),
                                 bce->insideEval, bce->evalCaller,
                                 /* evalStaticScope = */ js::NullPtr(),
                                 bce->hasGlobalScope, lineNum, bce->emitterMode);
            if (!bce2.init())
                return false;

            /* We measured the max scope depth when we parsed the function. */
            if (!EmitFunctionScript(cx, &bce2, pn->pn_body))
                return false;

            if (funbox->usesArguments && funbox->usesApply && funbox->usesThis)
                script->setUsesArgumentsApplyAndThis();
        }
    } else {
        MOZ_ASSERT(IsAsmJSModuleNative(fun->native()));
    }

    /* Make the function object a literal in the outer script's pool. */
    unsigned index = bce->objectList.add(pn->pn_funbox);

    /* Non-hoisted functions simply emit their respective op. */
    if (!pn->functionIsHoisted()) {
        /* JSOP_LAMBDA_ARROW is always preceded by JSOP_THIS. */
        MOZ_ASSERT(fun->isArrow() == (pn->getOp() == JSOP_LAMBDA_ARROW));
        if (fun->isArrow() && Emit1(cx, bce, JSOP_THIS) < 0)
            return false;
        return EmitIndex32(cx, pn->getOp(), index, bce);
    }

    /*
     * For a script we emit the code as we parse. Thus the bytecode for
     * top-level functions should go in the prolog to predefine their
     * names in the variable object before the already-generated main code
     * is executed. This extra work for top-level scripts is not necessary
     * when we emit the code for a function. It is fully parsed prior to
     * invocation of the emitter and calls to EmitTree for function
     * definitions can be scheduled before generating the rest of code.
     */
    if (!bce->sc->isFunctionBox()) {
        MOZ_ASSERT(pn->pn_cookie.isFree());
        MOZ_ASSERT(pn->getOp() == JSOP_NOP);
        MOZ_ASSERT(!bce->topStmt);
        bce->switchToProlog();
        if (!EmitIndex32(cx, JSOP_DEFFUN, index, bce))
            return false;
        if (!UpdateSourceCoordNotes(cx, bce, pn->pn_pos.begin))
            return false;
        bce->switchToMain();
    } else {
#ifdef DEBUG
        BindingIter bi(bce->script);
        while (bi->name() != fun->atom())
            bi++;
        MOZ_ASSERT(bi->kind() == Binding::VARIABLE || bi->kind() == Binding::CONSTANT ||
                   bi->kind() == Binding::ARGUMENT);
        MOZ_ASSERT(bi.argOrLocalIndex() < JS_BIT(20));
#endif
        pn->pn_index = index;
        if (!EmitIndexOp(cx, JSOP_LAMBDA, index, bce))
            return false;
        MOZ_ASSERT(pn->getOp() == JSOP_GETLOCAL || pn->getOp() == JSOP_GETARG);
        JSOp setOp = pn->getOp() == JSOP_GETLOCAL ? JSOP_SETLOCAL : JSOP_SETARG;
        if (!EmitVarOp(cx, pn, setOp, bce))
            return false;
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
    }

    return true;
}

static bool
EmitDo(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    /* Emit an annotated nop so IonBuilder can recognize the 'do' loop. */
    ptrdiff_t noteIndex = NewSrcNote(cx, bce, SRC_WHILE);
    if (noteIndex < 0 || Emit1(cx, bce, JSOP_NOP) < 0)
        return false;

    ptrdiff_t noteIndex2 = NewSrcNote(cx, bce, SRC_WHILE);
    if (noteIndex2 < 0)
        return false;

    /* Compile the loop body. */
    ptrdiff_t top = EmitLoopHead(cx, bce, pn->pn_left);
    if (top < 0)
        return false;

    LoopStmtInfo stmtInfo(cx);
    PushLoopStatement(bce, &stmtInfo, STMT_DO_LOOP, top);

    if (!EmitLoopEntry(cx, bce, nullptr))
        return false;

    if (!EmitTree(cx, bce, pn->pn_left))
        return false;

    /* Set loop and enclosing label update offsets, for continue. */
    ptrdiff_t off = bce->offset();
    StmtInfoBCE* stmt = &stmtInfo;
    do {
        stmt->update = off;
    } while ((stmt = stmt->down) != nullptr && stmt->type == STMT_LABEL);

    /* Compile the loop condition, now that continues know where to go. */
    if (!EmitTree(cx, bce, pn->pn_right))
        return false;

    ptrdiff_t beq = EmitJump(cx, bce, JSOP_IFNE, top - bce->offset());
    if (beq < 0)
        return false;

    if (!bce->tryNoteList.append(JSTRY_LOOP, bce->stackDepth, top, bce->offset()))
        return false;

    /*
     * Update the annotations with the update and back edge positions, for
     * IonBuilder.
     *
     * Be careful: We must set noteIndex2 before noteIndex in case the noteIndex
     * note gets bigger.
     */
    if (!SetSrcNoteOffset(cx, bce, noteIndex2, 0, beq - top))
        return false;
    if (!SetSrcNoteOffset(cx, bce, noteIndex, 0, 1 + (off - top)))
        return false;

    return PopStatementBCE(cx, bce);
}

static bool
EmitWhile(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, ptrdiff_t top)
{
    /*
     * Minimize bytecodes issued for one or more iterations by jumping to
     * the condition below the body and closing the loop if the condition
     * is true with a backward branch. For iteration count i:
     *
     *  i    test at the top                 test at the bottom
     *  =    ===============                 ==================
     *  0    ifeq-pass                       goto; ifne-fail
     *  1    ifeq-fail; goto; ifne-pass      goto; ifne-pass; ifne-fail
     *  2    2*(ifeq-fail; goto); ifeq-pass  goto; 2*ifne-pass; ifne-fail
     *  . . .
     *  N    N*(ifeq-fail; goto); ifeq-pass  goto; N*ifne-pass; ifne-fail
     */
    LoopStmtInfo stmtInfo(cx);
    PushLoopStatement(bce, &stmtInfo, STMT_WHILE_LOOP, top);

    ptrdiff_t noteIndex = NewSrcNote(cx, bce, SRC_WHILE);
    if (noteIndex < 0)
        return false;

    ptrdiff_t jmp = EmitJump(cx, bce, JSOP_GOTO, 0);
    if (jmp < 0)
        return false;

    top = EmitLoopHead(cx, bce, pn->pn_right);
    if (top < 0)
        return false;

    if (!EmitTree(cx, bce, pn->pn_right))
        return false;

    SetJumpOffsetAt(bce, jmp);
    if (!EmitLoopEntry(cx, bce, pn->pn_left))
        return false;
    if (!EmitTree(cx, bce, pn->pn_left))
        return false;

    ptrdiff_t beq = EmitJump(cx, bce, JSOP_IFNE, top - bce->offset());
    if (beq < 0)
        return false;

    if (!bce->tryNoteList.append(JSTRY_LOOP, bce->stackDepth, top, bce->offset()))
        return false;

    if (!SetSrcNoteOffset(cx, bce, noteIndex, 0, beq - jmp))
        return false;

    return PopStatementBCE(cx, bce);
}

static bool
EmitBreak(ExclusiveContext* cx, BytecodeEmitter* bce, PropertyName* label)
{
    StmtInfoBCE* stmt = bce->topStmt;
    SrcNoteType noteType;
    if (label) {
        while (stmt->type != STMT_LABEL || stmt->label != label)
            stmt = stmt->down;
        noteType = SRC_BREAK2LABEL;
    } else {
        while (!stmt->isLoop() && stmt->type != STMT_SWITCH)
            stmt = stmt->down;
        noteType = (stmt->type == STMT_SWITCH) ? SRC_SWITCHBREAK : SRC_BREAK;
    }

    return EmitGoto(cx, bce, stmt, &stmt->breaks, noteType) >= 0;
}

static bool
EmitContinue(ExclusiveContext* cx, BytecodeEmitter* bce, PropertyName* label)
{
    StmtInfoBCE* stmt = bce->topStmt;
    if (label) {
        /* Find the loop statement enclosed by the matching label. */
        StmtInfoBCE* loop = nullptr;
        while (stmt->type != STMT_LABEL || stmt->label != label) {
            if (stmt->isLoop())
                loop = stmt;
            stmt = stmt->down;
        }
        stmt = loop;
    } else {
        while (!stmt->isLoop())
            stmt = stmt->down;
    }

    return EmitGoto(cx, bce, stmt, &stmt->continues, SRC_CONTINUE) >= 0;
}

static bool
InTryBlockWithFinally(BytecodeEmitter* bce)
{
    for (StmtInfoBCE* stmt = bce->topStmt; stmt; stmt = stmt->down) {
        if (stmt->type == STMT_FINALLY)
            return true;
    }
    return false;
}

static bool
EmitReturn(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    if (!UpdateSourceCoordNotes(cx, bce, pn->pn_pos.begin))
        return false;

    if (bce->sc->isFunctionBox() && bce->sc->asFunctionBox()->isStarGenerator()) {
        if (!EmitPrepareIteratorResult(cx, bce))
            return false;
    }

    /* Push a return value */
    if (ParseNode* pn2 = pn->pn_left) {
        if (!EmitTree(cx, bce, pn2))
            return false;
    } else {
        /* No explicit return value provided */
        if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
            return false;
    }

    if (bce->sc->isFunctionBox() && bce->sc->asFunctionBox()->isStarGenerator()) {
        if (!EmitFinishIteratorResult(cx, bce, true))
            return false;
    }

    /*
     * EmitNonLocalJumpFixup may add fixup bytecode to close open try
     * blocks having finally clauses and to exit intermingled let blocks.
     * We can't simply transfer control flow to our caller in that case,
     * because we must gosub to those finally clauses from inner to outer,
     * with the correct stack pointer (i.e., after popping any with,
     * for/in, etc., slots nested inside the finally's try).
     *
     * In this case we mutate JSOP_RETURN into JSOP_SETRVAL and add an
     * extra JSOP_RETRVAL after the fixups.
     */
    ptrdiff_t top = bce->offset();

    bool isGenerator = bce->sc->isFunctionBox() && bce->sc->asFunctionBox()->isGenerator();
    bool useGenRVal = false;
    if (isGenerator) {
        if (bce->sc->asFunctionBox()->isStarGenerator() && InTryBlockWithFinally(bce)) {
            // Emit JSOP_SETALIASEDVAR .genrval to store the return value on the
            // scope chain, so it's not lost when we yield in a finally block.
            useGenRVal = true;
            MOZ_ASSERT(pn->pn_right);
            if (!EmitTree(cx, bce, pn->pn_right))
                return false;
            if (Emit1(cx, bce, JSOP_POP) < 0)
                return false;
        } else {
            if (Emit1(cx, bce, JSOP_SETRVAL) < 0)
                return false;
        }
    } else {
        if (Emit1(cx, bce, JSOP_RETURN) < 0)
            return false;
    }

    NonLocalExitScope nle(cx, bce);

    if (!nle.prepareForNonLocalJump(nullptr))
        return false;

    if (isGenerator) {
        ScopeCoordinate sc;
        // We know that .generator and .genrval are on the top scope chain node,
        // as we just exited nested scopes.
        sc.setHops(0);
        if (useGenRVal) {
            MOZ_ALWAYS_TRUE(LookupAliasedNameSlot(bce, bce->script, cx->names().dotGenRVal, &sc));
            if (!EmitAliasedVarOp(cx, JSOP_GETALIASEDVAR, sc, DontCheckLexical, bce))
                return false;
            if (Emit1(cx, bce, JSOP_SETRVAL) < 0)
                return false;
        }

        MOZ_ALWAYS_TRUE(LookupAliasedNameSlot(bce, bce->script, cx->names().dotGenerator, &sc));
        if (!EmitAliasedVarOp(cx, JSOP_GETALIASEDVAR, sc, DontCheckLexical, bce))
            return false;
        if (!EmitYieldOp(cx, bce, JSOP_FINALYIELDRVAL))
            return false;
    } else if (top + static_cast<ptrdiff_t>(JSOP_RETURN_LENGTH) != bce->offset()) {
        bce->code()[top] = JSOP_SETRVAL;
        if (Emit1(cx, bce, JSOP_RETRVAL) < 0)
            return false;
    }

    return true;
}

static bool
EmitYield(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    MOZ_ASSERT(bce->sc->isFunctionBox());

    if (pn->getOp() == JSOP_YIELD) {
        if (bce->sc->asFunctionBox()->isStarGenerator()) {
            if (!EmitPrepareIteratorResult(cx, bce))
                return false;
        }
        if (pn->pn_left) {
            if (!EmitTree(cx, bce, pn->pn_left))
                return false;
        } else {
            if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
                return false;
        }
        if (bce->sc->asFunctionBox()->isStarGenerator()) {
            if (!EmitFinishIteratorResult(cx, bce, false))
                return false;
        }
    } else {
        MOZ_ASSERT(pn->getOp() == JSOP_INITIALYIELD);
    }

    if (!EmitTree(cx, bce, pn->pn_right))
        return false;

    if (!EmitYieldOp(cx, bce, pn->getOp()))
        return false;

    if (pn->getOp() == JSOP_INITIALYIELD && Emit1(cx, bce, JSOP_POP) < 0)
        return false;

    return true;
}

static bool
EmitYieldStar(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* iter, ParseNode* gen)
{
    MOZ_ASSERT(bce->sc->isFunctionBox());
    MOZ_ASSERT(bce->sc->asFunctionBox()->isStarGenerator());

    if (!EmitTree(cx, bce, iter))                                // ITERABLE
        return false;
    if (!EmitIterator(cx, bce))                                  // ITER
        return false;

    // Initial send value is undefined.
    if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)                      // ITER RECEIVED
        return false;

    int depth = bce->stackDepth;
    MOZ_ASSERT(depth >= 2);

    ptrdiff_t initialSend = -1;
    if (EmitBackPatchOp(cx, bce, &initialSend) < 0)              // goto initialSend
        return false;

    // Try prologue.                                             // ITER RESULT
    StmtInfoBCE stmtInfo(cx);
    PushStatementBCE(bce, &stmtInfo, STMT_TRY, bce->offset());
    ptrdiff_t noteIndex = NewSrcNote(cx, bce, SRC_TRY);
    ptrdiff_t tryStart = bce->offset();                          // tryStart:
    if (noteIndex < 0 || Emit1(cx, bce, JSOP_TRY) < 0)
        return false;
    MOZ_ASSERT(bce->stackDepth == depth);

    // Load the generator object.
    if (!EmitTree(cx, bce, gen))                                 // ITER RESULT GENOBJ
        return false;

    // Yield RESULT as-is, without re-boxing.
    if (!EmitYieldOp(cx, bce, JSOP_YIELD))                       // ITER RECEIVED
        return false;

    // Try epilogue.
    if (!SetSrcNoteOffset(cx, bce, noteIndex, 0, bce->offset() - tryStart))
        return false;
    ptrdiff_t subsequentSend = -1;
    if (EmitBackPatchOp(cx, bce, &subsequentSend) < 0)           // goto subsequentSend
        return false;
    ptrdiff_t tryEnd = bce->offset();                            // tryEnd:

    // Catch location.
    bce->stackDepth = uint32_t(depth);                           // ITER RESULT
    if (Emit1(cx, bce, JSOP_POP) < 0)                            // ITER
        return false;
    // THROW? = 'throw' in ITER
    if (Emit1(cx, bce, JSOP_EXCEPTION) < 0)                      // ITER EXCEPTION
        return false;
    if (Emit1(cx, bce, JSOP_SWAP) < 0)                           // EXCEPTION ITER
        return false;
    if (Emit1(cx, bce, JSOP_DUP) < 0)                            // EXCEPTION ITER ITER
        return false;
    if (!EmitAtomOp(cx, cx->names().throw_, JSOP_STRING, bce))   // EXCEPTION ITER ITER "throw"
        return false;
    if (Emit1(cx, bce, JSOP_SWAP) < 0)                           // EXCEPTION ITER "throw" ITER
        return false;
    if (Emit1(cx, bce, JSOP_IN) < 0)                             // EXCEPTION ITER THROW?
        return false;
    // if (THROW?) goto delegate
    ptrdiff_t checkThrow = EmitJump(cx, bce, JSOP_IFNE, 0);      // EXCEPTION ITER
    if (checkThrow < 0)
        return false;
    if (Emit1(cx, bce, JSOP_POP) < 0)                            // EXCEPTION
        return false;
    if (Emit1(cx, bce, JSOP_THROW) < 0)                          // throw EXCEPTION
        return false;

    SetJumpOffsetAt(bce, checkThrow);                            // delegate:
    // RESULT = ITER.throw(EXCEPTION)                            // EXCEPTION ITER
    bce->stackDepth = uint32_t(depth);
    if (Emit1(cx, bce, JSOP_DUP) < 0)                            // EXCEPTION ITER ITER
        return false;
    if (Emit1(cx, bce, JSOP_DUP) < 0)                            // EXCEPTION ITER ITER ITER
        return false;
    if (!EmitAtomOp(cx, cx->names().throw_, JSOP_CALLPROP, bce)) // EXCEPTION ITER ITER THROW
        return false;
    if (Emit1(cx, bce, JSOP_SWAP) < 0)                           // EXCEPTION ITER THROW ITER
        return false;
    if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)3) < 0)            // ITER THROW ITER EXCEPTION
        return false;
    if (EmitCall(cx, bce, JSOP_CALL, 1, iter) < 0)               // ITER RESULT
        return false;
    CheckTypeSet(cx, bce, JSOP_CALL);
    MOZ_ASSERT(bce->stackDepth == depth);
    ptrdiff_t checkResult = -1;
    if (EmitBackPatchOp(cx, bce, &checkResult) < 0)              // goto checkResult
        return false;

    // Catch epilogue.
    if (!PopStatementBCE(cx, bce))
        return false;
    // This is a peace offering to ReconstructPCStack.  See the note in EmitTry.
    if (Emit1(cx, bce, JSOP_NOP) < 0)
        return false;
    if (!bce->tryNoteList.append(JSTRY_CATCH, depth, tryStart + JSOP_TRY_LENGTH, tryEnd))
        return false;

    // After the try/catch block: send the received value to the iterator.
    if (!BackPatch(cx, bce, initialSend, bce->code().end(), JSOP_GOTO)) // initialSend:
        return false;
    if (!BackPatch(cx, bce, subsequentSend, bce->code().end(), JSOP_GOTO)) // subsequentSend:
        return false;

    // Send location.
    // result = iter.next(received)                              // ITER RECEIVED
    if (Emit1(cx, bce, JSOP_SWAP) < 0)                           // RECEIVED ITER
        return false;
    if (Emit1(cx, bce, JSOP_DUP) < 0)                            // RECEIVED ITER ITER
        return false;
    if (Emit1(cx, bce, JSOP_DUP) < 0)                            // RECEIVED ITER ITER ITER
        return false;
    if (!EmitAtomOp(cx, cx->names().next, JSOP_CALLPROP, bce))   // RECEIVED ITER ITER NEXT
        return false;
    if (Emit1(cx, bce, JSOP_SWAP) < 0)                           // RECEIVED ITER NEXT ITER
        return false;
    if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)3) < 0)            // ITER NEXT ITER RECEIVED
        return false;
    if (EmitCall(cx, bce, JSOP_CALL, 1, iter) < 0)               // ITER RESULT
        return false;
    CheckTypeSet(cx, bce, JSOP_CALL);
    MOZ_ASSERT(bce->stackDepth == depth);

    if (!BackPatch(cx, bce, checkResult, bce->code().end(), JSOP_GOTO)) // checkResult:
        return false;
    // if (!result.done) goto tryStart;                          // ITER RESULT
    if (Emit1(cx, bce, JSOP_DUP) < 0)                            // ITER RESULT RESULT
        return false;
    if (!EmitAtomOp(cx, cx->names().done, JSOP_GETPROP, bce))    // ITER RESULT DONE
        return false;
    // if (!DONE) goto tryStart;
    if (EmitJump(cx, bce, JSOP_IFEQ, tryStart - bce->offset()) < 0) // ITER RESULT
        return false;

    // result.value
    if (Emit1(cx, bce, JSOP_SWAP) < 0)                           // RESULT ITER
        return false;
    if (Emit1(cx, bce, JSOP_POP) < 0)                            // RESULT
        return false;
    if (!EmitAtomOp(cx, cx->names().value, JSOP_GETPROP, bce))   // VALUE
        return false;

    MOZ_ASSERT(bce->stackDepth == depth - 1);

    return true;
}

static bool
EmitStatementList(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, ptrdiff_t top)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));

    StmtInfoBCE stmtInfo(cx);
    PushStatementBCE(bce, &stmtInfo, STMT_BLOCK, top);

    ParseNode* pnchild = pn->pn_head;

    if (pn->pn_xflags & PNX_DESTRUCT)
        pnchild = pnchild->pn_next;

    for (ParseNode* pn2 = pnchild; pn2; pn2 = pn2->pn_next) {
        if (!EmitTree(cx, bce, pn2))
            return false;
    }

    return PopStatementBCE(cx, bce);
}

static bool
EmitStatement(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_SEMI));

    ParseNode* pn2 = pn->pn_kid;
    if (!pn2)
        return true;

    if (!UpdateSourceCoordNotes(cx, bce, pn->pn_pos.begin))
        return false;

    /*
     * Top-level or called-from-a-native JS_Execute/EvaluateScript,
     * debugger, and eval frames may need the value of the ultimate
     * expression statement as the script's result, despite the fact
     * that it appears useless to the compiler.
     *
     * API users may also set the JSOPTION_NO_SCRIPT_RVAL option when
     * calling JS_Compile* to suppress JSOP_SETRVAL.
     */
    bool wantval = false;
    bool useful = false;
    if (bce->sc->isFunctionBox()) {
        MOZ_ASSERT(!bce->script->noScriptRval());
    } else {
        useful = wantval = !bce->script->noScriptRval();
    }

    /* Don't eliminate expressions with side effects. */
    if (!useful) {
        if (!CheckSideEffects(cx, bce, pn2, &useful))
            return false;

        /*
         * Don't eliminate apparently useless expressions if they are
         * labeled expression statements.  The pc->topStmt->update test
         * catches the case where we are nesting in EmitTree for a labeled
         * compound statement.
         */
        if (bce->topStmt &&
            bce->topStmt->type == STMT_LABEL &&
            bce->topStmt->update >= bce->offset())
        {
            useful = true;
        }
    }

    if (useful) {
        JSOp op = wantval ? JSOP_SETRVAL : JSOP_POP;
        MOZ_ASSERT_IF(pn2->isKind(PNK_ASSIGN), pn2->isOp(JSOP_NOP));
        if (!EmitTree(cx, bce, pn2))
            return false;
        if (Emit1(cx, bce, op) < 0)
            return false;
    } else if (pn->isDirectivePrologueMember()) {
        // Don't complain about directive prologue members; just don't emit
        // their code.
    } else {
        if (JSAtom* atom = pn->isStringExprStatement()) {
            // Warn if encountering a non-directive prologue member string
            // expression statement, that is inconsistent with the current
            // directive prologue.  That is, a script *not* starting with
            // "use strict" should warn for any "use strict" statements seen
            // later in the script, because such statements are misleading.
            const char* directive = nullptr;
            if (atom == cx->names().useStrict) {
                if (!bce->sc->strict)
                    directive = js_useStrict_str;
            } else if (atom == cx->names().useAsm) {
                if (bce->sc->isFunctionBox()) {
                    JSFunction* fun = bce->sc->asFunctionBox()->function();
                    if (fun->isNative() && IsAsmJSModuleNative(fun->native()))
                        directive = js_useAsm_str;
                }
            }

            if (directive) {
                if (!bce->reportStrictWarning(pn2, JSMSG_CONTRARY_NONDIRECTIVE, directive))
                    return false;
            }
        } else {
            bce->current->currentLine = bce->parser->tokenStream.srcCoords.lineNum(pn2->pn_pos.begin);
            bce->current->lastColumn = 0;
            if (!bce->reportStrictWarning(pn2, JSMSG_USELESS_EXPR))
                return false;
        }
    }

    return true;
}

static bool
EmitDelete(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    /*
     * Under ECMA 3, deleting a non-reference returns true -- but alas we
     * must evaluate the operand if it appears it might have side effects.
     */
    ParseNode* pn2 = pn->pn_kid;
    switch (pn2->getKind()) {
      case PNK_NAME:
        if (!BindNameToSlot(cx, bce, pn2))
            return false;
        if (!EmitAtomOp(cx, pn2, pn2->getOp(), bce))
            return false;
        break;
      case PNK_DOT:
      {
        JSOp delOp = bce->sc->strict ? JSOP_STRICTDELPROP : JSOP_DELPROP;
        if (!EmitPropOp(cx, pn2, delOp, bce))
            return false;
        break;
      }
      case PNK_ELEM:
      {
        JSOp delOp = bce->sc->strict ? JSOP_STRICTDELELEM : JSOP_DELELEM;
        if (!EmitElemOp(cx, pn2, delOp, bce))
            return false;
        break;
      }
      default:
      {
        /*
         * If useless, just emit JSOP_TRUE; otherwise convert delete foo()
         * to foo(), true (a comma expression).
         */
        bool useful = false;
        if (!CheckSideEffects(cx, bce, pn2, &useful))
            return false;

        if (useful) {
            MOZ_ASSERT_IF(pn2->isKind(PNK_CALL), !(pn2->pn_xflags & PNX_SETCALL));
            if (!EmitTree(cx, bce, pn2))
                return false;
            if (Emit1(cx, bce, JSOP_POP) < 0)
                return false;
        }

        if (Emit1(cx, bce, JSOP_TRUE) < 0)
            return false;
      }
    }

    return true;
}

static bool
EmitArray(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, uint32_t count);

static bool
EmitSelfHostedCallFunction(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    // Special-casing of callFunction to emit bytecode that directly
    // invokes the callee with the correct |this| object and arguments.
    // callFunction(fun, thisArg, arg0, arg1) thus becomes:
    // - emit lookup for fun
    // - emit lookup for thisArg
    // - emit lookups for arg0, arg1
    //
    // argc is set to the amount of actually emitted args and the
    // emitting of args below is disabled by setting emitArgs to false.
    if (pn->pn_count < 3) {
        bce->reportError(pn, JSMSG_MORE_ARGS_NEEDED, "callFunction", "1", "s");
        return false;
    }

    ParseNode* pn2 = pn->pn_head;
    ParseNode* funNode = pn2->pn_next;
    if (!EmitTree(cx, bce, funNode))
        return false;

    ParseNode* thisArg = funNode->pn_next;
    if (!EmitTree(cx, bce, thisArg))
        return false;

    bool oldEmittingForInit = bce->emittingForInit;
    bce->emittingForInit = false;

    for (ParseNode* argpn = thisArg->pn_next; argpn; argpn = argpn->pn_next) {
        if (!EmitTree(cx, bce, argpn))
            return false;
    }

    bce->emittingForInit = oldEmittingForInit;

    uint32_t argc = pn->pn_count - 3;
    if (EmitCall(cx, bce, pn->getOp(), argc) < 0)
        return false;

    CheckTypeSet(cx, bce, pn->getOp());
    return true;
}

static bool
EmitSelfHostedResumeGenerator(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    // Syntax: resumeGenerator(gen, value, 'next'|'throw'|'close')
    if (pn->pn_count != 4) {
        bce->reportError(pn, JSMSG_MORE_ARGS_NEEDED, "resumeGenerator", "1", "s");
        return false;
    }

    ParseNode* funNode = pn->pn_head;  // The resumeGenerator node.

    ParseNode* genNode = funNode->pn_next;
    if (!EmitTree(cx, bce, genNode))
        return false;

    ParseNode* valNode = genNode->pn_next;
    if (!EmitTree(cx, bce, valNode))
        return false;

    ParseNode* kindNode = valNode->pn_next;
    MOZ_ASSERT(kindNode->isKind(PNK_STRING));
    uint16_t operand = GeneratorObject::getResumeKind(cx, kindNode->pn_atom);
    MOZ_ASSERT(!kindNode->pn_next);

    if (EmitCall(cx, bce, JSOP_RESUME, operand) < 0)
        return false;

    return true;
}

static bool
EmitSelfHostedForceInterpreter(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    if (Emit1(cx, bce, JSOP_FORCEINTERPRETER) < 0)
        return false;
    if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
        return false;
    return true;
}

static bool
EmitCallOrNew(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    bool callop = pn->isKind(PNK_CALL) || pn->isKind(PNK_TAGGED_TEMPLATE);
    /*
     * Emit callable invocation or operator new (constructor call) code.
     * First, emit code for the left operand to evaluate the callable or
     * constructable object expression.
     *
     * For operator new, we emit JSOP_GETPROP instead of JSOP_CALLPROP, etc.
     * This is necessary to interpose the lambda-initialized method read
     * barrier -- see the code in jsinterp.cpp for JSOP_LAMBDA followed by
     * JSOP_{SET,INIT}PROP.
     *
     * Then (or in a call case that has no explicit reference-base
     * object) we emit JSOP_UNDEFINED to produce the undefined |this|
     * value required for calls (which non-strict mode functions
     * will box into the global object).
     */
    uint32_t argc = pn->pn_count - 1;

    if (argc >= ARGC_LIMIT) {
        bce->parser->tokenStream.reportError(callop
                                             ? JSMSG_TOO_MANY_FUN_ARGS
                                             : JSMSG_TOO_MANY_CON_ARGS);
        return false;
    }

    ParseNode* pn2 = pn->pn_head;
    bool spread = JOF_OPTYPE(pn->getOp()) == JOF_BYTE;
    switch (pn2->getKind()) {
      case PNK_NAME:
        if (bce->emitterMode == BytecodeEmitter::SelfHosting && !spread) {
            // We shouldn't see foo(bar) = x in self-hosted code.
            MOZ_ASSERT(!(pn->pn_xflags & PNX_SETCALL));

            // Calls to "forceInterpreter", "callFunction" or "resumeGenerator"
            // in self-hosted code generate inline bytecode.
            if (pn2->name() == cx->names().callFunction)
                return EmitSelfHostedCallFunction(cx, bce, pn);
            if (pn2->name() == cx->names().resumeGenerator)
                return EmitSelfHostedResumeGenerator(cx, bce, pn);
            if (pn2->name() == cx->names().forceInterpreter)
                return EmitSelfHostedForceInterpreter(cx, bce, pn);
            // Fall through.
        }
        if (!EmitNameOp(cx, bce, pn2, callop))
            return false;
        break;
      case PNK_DOT:
        if (!EmitPropOp(cx, pn2, callop ? JSOP_CALLPROP : JSOP_GETPROP, bce))
            return false;
        break;
      case PNK_ELEM:
        if (!EmitElemOp(cx, pn2, callop ? JSOP_CALLELEM : JSOP_GETELEM, bce))
            return false;
        if (callop) {
            if (Emit1(cx, bce, JSOP_SWAP) < 0)
                return false;
        }
        break;
      case PNK_FUNCTION:
        /*
         * Top level lambdas which are immediately invoked should be
         * treated as only running once. Every time they execute we will
         * create new types and scripts for their contents, to increase
         * the quality of type information within them and enable more
         * backend optimizations. Note that this does not depend on the
         * lambda being invoked at most once (it may be named or be
         * accessed via foo.caller indirection), as multiple executions
         * will just cause the inner scripts to be repeatedly cloned.
         */
        MOZ_ASSERT(!bce->emittingRunOnceLambda);
        if (bce->checkSingletonContext() || (!bce->isInLoop() && bce->isRunOnceLambda())) {
            bce->emittingRunOnceLambda = true;
            if (!EmitTree(cx, bce, pn2))
                return false;
            bce->emittingRunOnceLambda = false;
        } else {
            if (!EmitTree(cx, bce, pn2))
                return false;
        }
        callop = false;
        break;
      default:
        if (!EmitTree(cx, bce, pn2))
            return false;
        callop = false;             /* trigger JSOP_UNDEFINED after */
        break;
    }
    if (!callop) {
        JSOp thisop = pn->isKind(PNK_GENEXP) ? JSOP_THIS : JSOP_UNDEFINED;
        if (Emit1(cx, bce, thisop) < 0)
            return false;
    }

    /*
     * Emit code for each argument in order, then emit the JSOP_*CALL or
     * JSOP_NEW bytecode with a two-byte immediate telling how many args
     * were pushed on the operand stack.
     */
    bool oldEmittingForInit = bce->emittingForInit;
    bce->emittingForInit = false;
    if (!spread) {
        for (ParseNode* pn3 = pn2->pn_next; pn3; pn3 = pn3->pn_next) {
            if (!EmitTree(cx, bce, pn3))
                return false;
        }
    } else {
        if (!EmitArray(cx, bce, pn2->pn_next, argc))
            return false;
    }
    bce->emittingForInit = oldEmittingForInit;

    if (!spread) {
        if (EmitCall(cx, bce, pn->getOp(), argc, pn) < 0)
            return false;
    } else {
        if (Emit1(cx, bce, pn->getOp()) < 0)
            return false;
    }
    CheckTypeSet(cx, bce, pn->getOp());
    if (pn->isOp(JSOP_EVAL) ||
        pn->isOp(JSOP_STRICTEVAL) ||
        pn->isOp(JSOP_SPREADEVAL) ||
        pn->isOp(JSOP_STRICTSPREADEVAL))
    {
        uint32_t lineNum = bce->parser->tokenStream.srcCoords.lineNum(pn->pn_pos.begin);
        EMIT_UINT16_IMM_OP(JSOP_LINENO, lineNum);
    }
    if (pn->pn_xflags & PNX_SETCALL) {
        if (Emit1(cx, bce, JSOP_SETCALL) < 0)
            return false;
    }
    return true;
}

static bool
EmitLogical(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));

    /*
     * JSOP_OR converts the operand on the stack to boolean, leaves the original
     * value on the stack and jumps if true; otherwise it falls into the next
     * bytecode, which pops the left operand and then evaluates the right operand.
     * The jump goes around the right operand evaluation.
     *
     * JSOP_AND converts the operand on the stack to boolean and jumps if false;
     * otherwise it falls into the right operand's bytecode.
     */

    /* Left-associative operator chain: avoid too much recursion. */
    ParseNode* pn2 = pn->pn_head;
    if (!EmitTree(cx, bce, pn2))
        return false;
    ptrdiff_t top = EmitJump(cx, bce, JSOP_BACKPATCH, 0);
    if (top < 0)
        return false;
    if (Emit1(cx, bce, JSOP_POP) < 0)
        return false;

    /* Emit nodes between the head and the tail. */
    ptrdiff_t jmp = top;
    while ((pn2 = pn2->pn_next)->pn_next) {
        if (!EmitTree(cx, bce, pn2))
            return false;
        ptrdiff_t off = EmitJump(cx, bce, JSOP_BACKPATCH, 0);
        if (off < 0)
            return false;
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
        SET_JUMP_OFFSET(bce->code(jmp), off - jmp);
        jmp = off;
    }
    if (!EmitTree(cx, bce, pn2))
        return false;

    pn2 = pn->pn_head;
    ptrdiff_t off = bce->offset();
    do {
        jsbytecode* pc = bce->code(top);
        ptrdiff_t tmp = GET_JUMP_OFFSET(pc);
        SET_JUMP_OFFSET(pc, off - top);
        *pc = pn->getOp();
        top += tmp;
    } while ((pn2 = pn2->pn_next)->pn_next);

    return true;
}

/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
 * the comment on EmitSwitch.
 */
MOZ_NEVER_INLINE static bool
EmitIncOrDec(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    /* Emit lvalue-specialized code for ++/-- operators. */
    ParseNode* pn2 = pn->pn_kid;
    switch (pn2->getKind()) {
      case PNK_DOT:
        if (!EmitPropIncDec(cx, pn, bce))
            return false;
        break;
      case PNK_ELEM:
        if (!EmitElemIncDec(cx, pn, bce))
            return false;
        break;
      case PNK_CALL:
        MOZ_ASSERT(pn2->pn_xflags & PNX_SETCALL);
        if (!EmitTree(cx, bce, pn2))
            return false;
        break;
      default:
        MOZ_ASSERT(pn2->isKind(PNK_NAME));
        pn2->setOp(JSOP_SETNAME);
        if (!BindNameToSlot(cx, bce, pn2))
            return false;
        JSOp op = pn2->getOp();
        bool maySet;
        switch (op) {
          case JSOP_SETLOCAL:
          case JSOP_SETARG:
          case JSOP_SETALIASEDVAR:
          case JSOP_SETNAME:
          case JSOP_STRICTSETNAME:
          case JSOP_SETGNAME:
          case JSOP_STRICTSETGNAME:
            maySet = true;
            break;
          default:
            maySet = false;
        }
        if (op == JSOP_CALLEE) {
            if (Emit1(cx, bce, op) < 0)
                return false;
        } else if (!pn2->pn_cookie.isFree()) {
            if (maySet) {
                if (!EmitVarIncDec(cx, pn, bce))
                    return false;
            } else {
                if (!EmitVarOp(cx, pn2, op, bce))
                    return false;
            }
        } else {
            MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);
            if (maySet) {
                if (!EmitNameIncDec(cx, pn, bce))
                    return false;
            } else {
                if (!EmitAtomOp(cx, pn2, op, bce))
                    return false;
            }
            break;
        }
        if (pn2->isConst()) {
            if (Emit1(cx, bce, JSOP_POS) < 0)
                return false;
            bool post;
            JSOp binop = GetIncDecInfo(pn->getKind(), &post);
            if (!post) {
                if (Emit1(cx, bce, JSOP_ONE) < 0)
                    return false;
                if (Emit1(cx, bce, binop) < 0)
                    return false;
            }
        }
    }
    return true;
}

/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
 * the comment on EmitSwitch.
 */
MOZ_NEVER_INLINE static bool
EmitLabeledStatement(ExclusiveContext* cx, BytecodeEmitter* bce, const LabeledStatement* pn)
{
    /*
     * Emit a JSOP_LABEL instruction. The argument is the offset to the statement
     * following the labeled statement.
     */
    jsatomid index;
    if (!bce->makeAtomIndex(pn->label(), &index))
        return false;

    ptrdiff_t top = EmitJump(cx, bce, JSOP_LABEL, 0);
    if (top < 0)
        return false;

    /* Emit code for the labeled statement. */
    StmtInfoBCE stmtInfo(cx);
    PushStatementBCE(bce, &stmtInfo, STMT_LABEL, bce->offset());
    stmtInfo.label = pn->label();
    if (!EmitTree(cx, bce, pn->statement()))
        return false;
    if (!PopStatementBCE(cx, bce))
        return false;

    /* Patch the JSOP_LABEL offset. */
    SetJumpOffsetAt(bce, top);
    return true;
}

static bool
EmitSyntheticStatements(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, ptrdiff_t top)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    StmtInfoBCE stmtInfo(cx);
    PushStatementBCE(bce, &stmtInfo, STMT_SEQ, top);
    ParseNode* pn2 = pn->pn_head;
    if (pn->pn_xflags & PNX_DESTRUCT)
        pn2 = pn2->pn_next;
    for (; pn2; pn2 = pn2->pn_next) {
        if (!EmitTree(cx, bce, pn2))
            return false;
    }
    return PopStatementBCE(cx, bce);
}

static bool
EmitConditionalExpression(ExclusiveContext* cx, BytecodeEmitter* bce, ConditionalExpression& conditional)
{
    /* Emit the condition, then branch if false to the else part. */
    if (!EmitTree(cx, bce, &conditional.condition()))
        return false;
    ptrdiff_t noteIndex = NewSrcNote(cx, bce, SRC_COND);
    if (noteIndex < 0)
        return false;
    ptrdiff_t beq = EmitJump(cx, bce, JSOP_IFEQ, 0);
    if (beq < 0 || !EmitTree(cx, bce, &conditional.thenExpression()))
        return false;

    /* Jump around else, fixup the branch, emit else, fixup jump. */
    ptrdiff_t jmp = EmitJump(cx, bce, JSOP_GOTO, 0);
    if (jmp < 0)
        return false;
    SetJumpOffsetAt(bce, beq);

    /*
     * Because each branch pushes a single value, but our stack budgeting
     * analysis ignores branches, we now have to adjust bce->stackDepth to
     * ignore the value pushed by the first branch.  Execution will follow
     * only one path, so we must decrement bce->stackDepth.
     *
     * Failing to do this will foil code, such as let expression and block
     * code generation, which must use the stack depth to compute local
     * stack indexes correctly.
     */
    MOZ_ASSERT(bce->stackDepth > 0);
    bce->stackDepth--;
    if (!EmitTree(cx, bce, &conditional.elseExpression()))
        return false;
    SetJumpOffsetAt(bce, jmp);
    return SetSrcNoteOffset(cx, bce, noteIndex, 0, jmp - beq);
}

/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
 * the comment on EmitSwitch.
 */
MOZ_NEVER_INLINE static bool
EmitObject(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    if (!(pn->pn_xflags & PNX_NONCONST) && pn->pn_head && bce->checkSingletonContext())
        return EmitSingletonInitialiser(cx, bce, pn);

    /*
     * Emit code for {p:a, '%q':b, 2:c} that is equivalent to constructing
     * a new object and defining (in source order) each property on the object
     * (or mutating the object's [[Prototype]], in the case of __proto__).
     */
    ptrdiff_t offset = bce->offset();
    if (!EmitNewInit(cx, bce, JSProto_Object))
        return false;

    /*
     * Try to construct the shape of the object as we go, so we can emit a
     * JSOP_NEWOBJECT with the final shape instead.
     */
    RootedPlainObject obj(cx);
    if (bce->script->compileAndGo()) {
        gc::AllocKind kind = GuessObjectGCKind(pn->pn_count);
        obj = NewBuiltinClassInstance<PlainObject>(cx, kind, TenuredObject);
        if (!obj)
            return false;
    }

    for (ParseNode* propdef = pn->pn_head; propdef; propdef = propdef->pn_next) {
        if (!UpdateSourceCoordNotes(cx, bce, propdef->pn_pos.begin))
            return false;

        // Handle __proto__: v specially because *only* this form, and no other
        // involving "__proto__", performs [[Prototype]] mutation.
        if (propdef->isKind(PNK_MUTATEPROTO)) {
            if (!EmitTree(cx, bce, propdef->pn_kid))
                return false;
            obj = nullptr;
            if (!Emit1(cx, bce, JSOP_MUTATEPROTO))
                return false;
            continue;
        }

        /* Emit an index for t[2] for later consumption by JSOP_INITELEM. */
        ParseNode* key = propdef->pn_left;
        bool isIndex = false;
        if (key->isKind(PNK_NUMBER)) {
            if (!EmitNumberOp(cx, key->pn_dval, bce))
                return false;
            isIndex = true;
        } else if (key->isKind(PNK_OBJECT_PROPERTY_NAME) || key->isKind(PNK_STRING)) {
            // The parser already checked for atoms representing indexes and
            // used PNK_NUMBER instead, but also watch for ids which TI treats
            // as indexes for simpliciation of downstream analysis.
            jsid id = NameToId(key->pn_atom->asPropertyName());
            if (id != IdToTypeId(id)) {
                if (!EmitTree(cx, bce, key))
                    return false;
                isIndex = true;
            }
        } else {
            MOZ_ASSERT(key->isKind(PNK_COMPUTED_NAME));
            if (!EmitTree(cx, bce, key->pn_kid))
                return false;
            isIndex = true;
        }

        /* Emit code for the property initializer. */
        if (!EmitTree(cx, bce, propdef->pn_right))
            return false;

        JSOp op = propdef->getOp();
        MOZ_ASSERT(op == JSOP_INITPROP ||
                   op == JSOP_INITPROP_GETTER ||
                   op == JSOP_INITPROP_SETTER);

        if (op == JSOP_INITPROP_GETTER || op == JSOP_INITPROP_SETTER)
            obj = nullptr;

        if (isIndex) {
            obj = nullptr;
            switch (op) {
              case JSOP_INITPROP:        op = JSOP_INITELEM;        break;
              case JSOP_INITPROP_GETTER: op = JSOP_INITELEM_GETTER; break;
              case JSOP_INITPROP_SETTER: op = JSOP_INITELEM_SETTER; break;
              default: MOZ_CRASH("Invalid op");
            }
            if (Emit1(cx, bce, op) < 0)
                return false;
        } else {
            MOZ_ASSERT(key->isKind(PNK_OBJECT_PROPERTY_NAME) || key->isKind(PNK_STRING));

            jsatomid index;
            if (!bce->makeAtomIndex(key->pn_atom, &index))
                return false;

            if (obj) {
                MOZ_ASSERT(!obj->inDictionaryMode());
                Rooted<jsid> id(cx, AtomToId(key->pn_atom));
                RootedValue undefinedValue(cx, UndefinedValue());
                if (!NativeDefineProperty(cx, obj, id, undefinedValue, nullptr, nullptr,
                                          JSPROP_ENUMERATE))
                {
                    return false;
                }
                if (obj->inDictionaryMode())
                    obj = nullptr;
            }

            if (!EmitIndex32(cx, op, index, bce))
                return false;
        }
    }

    if (obj) {
        /*
         * The object survived and has a predictable shape: update the original
         * bytecode.
         */
        ObjectBox* objbox = bce->parser->newObjectBox(obj);
        if (!objbox)
            return false;

        static_assert(JSOP_NEWINIT_LENGTH == JSOP_NEWOBJECT_LENGTH,
                      "newinit and newobject must have equal length to edit in-place");

        uint32_t index = bce->objectList.add(objbox);
        jsbytecode* code = bce->code(offset);
        code[0] = JSOP_NEWOBJECT;
        code[1] = jsbytecode(index >> 24);
        code[2] = jsbytecode(index >> 16);
        code[3] = jsbytecode(index >> 8);
        code[4] = jsbytecode(index);
    }

    return true;
}

static bool
EmitArrayComp(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    if (!EmitNewInit(cx, bce, JSProto_Array))
        return false;

    /*
     * Pass the new array's stack index to the PNK_ARRAYPUSH case via
     * bce->arrayCompDepth, then simply traverse the PNK_FOR node and
     * its kids under pn2 to generate this comprehension.
     */
    MOZ_ASSERT(bce->stackDepth > 0);
    uint32_t saveDepth = bce->arrayCompDepth;
    bce->arrayCompDepth = (uint32_t) (bce->stackDepth - 1);
    if (!EmitTree(cx, bce, pn->pn_head))
        return false;
    bce->arrayCompDepth = saveDepth;

    return true;
}

/**
 * EmitSpread expects the current index (I) of the array, the array itself and the iterator to be
 * on the stack in that order (iterator on the bottom).
 * It will pop the iterator and I, then iterate over the iterator by calling |.next()|
 * and put the results into the I-th element of array with incrementing I, then
 * push the result I (it will be original I + iteration count).
 * The stack after iteration will look like |ARRAY INDEX|.
 */
static bool
EmitSpread(ExclusiveContext* cx, BytecodeEmitter* bce)
{
    return EmitForOf(cx, bce, STMT_SPREAD, nullptr, -1);
}

static bool
EmitArray(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn, uint32_t count)
{
    /*
     * Emit code for [a, b, c] that is equivalent to constructing a new
     * array and in source order evaluating each element value and adding
     * it to the array, without invoking latent setters.  We use the
     * JSOP_NEWINIT and JSOP_INITELEM_ARRAY bytecodes to ignore setters and
     * to avoid dup'ing and popping the array as each element is added, as
     * JSOP_SETELEM/JSOP_SETPROP would do.
     */

    int32_t nspread = 0;
    for (ParseNode* elt = pn; elt; elt = elt->pn_next) {
        if (elt->isKind(PNK_SPREAD))
            nspread++;
    }

    ptrdiff_t off = EmitN(cx, bce, JSOP_NEWARRAY, 3);                    // ARRAY
    if (off < 0)
        return false;
    CheckTypeSet(cx, bce, JSOP_NEWARRAY);
    jsbytecode* pc = bce->code(off);

    // For arrays with spread, this is a very pessimistic allocation, the
    // minimum possible final size.
    SET_UINT24(pc, count - nspread);

    ParseNode* pn2 = pn;
    jsatomid atomIndex;
    bool afterSpread = false;
    for (atomIndex = 0; pn2; atomIndex++, pn2 = pn2->pn_next) {
        if (!afterSpread && pn2->isKind(PNK_SPREAD)) {
            afterSpread = true;
            if (!EmitNumberOp(cx, atomIndex, bce))                       // ARRAY INDEX
                return false;
        }
        if (!UpdateSourceCoordNotes(cx, bce, pn2->pn_pos.begin))
            return false;
        if (pn2->isKind(PNK_ELISION)) {
            if (Emit1(cx, bce, JSOP_HOLE) < 0)
                return false;
        } else {
            ParseNode* expr = pn2->isKind(PNK_SPREAD) ? pn2->pn_kid : pn2;
            if (!EmitTree(cx, bce, expr))                                // ARRAY INDEX? VALUE
                return false;
        }
        if (pn2->isKind(PNK_SPREAD)) {
            if (!EmitIterator(cx, bce))                                  // ARRAY INDEX ITER
                return false;
            if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)2) < 0)            // INDEX ITER ARRAY
                return false;
            if (Emit2(cx, bce, JSOP_PICK, (jsbytecode)2) < 0)            // ITER ARRAY INDEX
                return false;
            if (!EmitSpread(cx, bce))                                    // ARRAY INDEX
                return false;
        } else if (afterSpread) {
            if (Emit1(cx, bce, JSOP_INITELEM_INC) < 0)
                return false;
        } else {
            off = EmitN(cx, bce, JSOP_INITELEM_ARRAY, 3);
            if (off < 0)
                return false;
            SET_UINT24(bce->code(off), atomIndex);
        }
    }
    MOZ_ASSERT(atomIndex == count);
    if (afterSpread) {
        if (Emit1(cx, bce, JSOP_POP) < 0)                                // ARRAY
            return false;
    }
    return true;
}

static bool
EmitUnary(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    if (!UpdateSourceCoordNotes(cx, bce, pn->pn_pos.begin))
        return false;
    /* Unary op, including unary +/-. */
    JSOp op = pn->getOp();
    ParseNode* pn2 = pn->pn_kid;

    if (op == JSOP_TYPEOF && !pn2->isKind(PNK_NAME))
        op = JSOP_TYPEOFEXPR;

    bool oldEmittingForInit = bce->emittingForInit;
    bce->emittingForInit = false;
    if (!EmitTree(cx, bce, pn2))
        return false;

    bce->emittingForInit = oldEmittingForInit;
    return Emit1(cx, bce, op) >= 0;
}

static bool
EmitDefaults(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_ARGSBODY));

    ParseNode* arg, *pnlast = pn->last();
    for (arg = pn->pn_head; arg != pnlast; arg = arg->pn_next) {
        if (!(arg->pn_dflags & PND_DEFAULT))
            continue;
        if (!BindNameToSlot(cx, bce, arg))
            return false;
        if (!EmitVarOp(cx, arg, JSOP_GETARG, bce))
            return false;
        if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
            return false;
        if (Emit1(cx, bce, JSOP_STRICTEQ) < 0)
            return false;
        // Emit source note to enable ion compilation.
        if (NewSrcNote(cx, bce, SRC_IF) < 0)
            return false;
        ptrdiff_t jump = EmitJump(cx, bce, JSOP_IFEQ, 0);
        if (jump < 0)
            return false;
        if (!EmitTree(cx, bce, arg->expr()))
            return false;
        if (!EmitVarOp(cx, arg, JSOP_SETARG, bce))
            return false;
        if (Emit1(cx, bce, JSOP_POP) < 0)
            return false;
        SET_JUMP_OFFSET(bce->code(jump), bce->offset() - jump);
    }

    return true;
}

bool
frontend::EmitTree(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn)
{
    JS_CHECK_RECURSION(cx, return false);

    EmitLevelManager elm(bce);

    bool ok = true;
    ptrdiff_t top = bce->offset();
    pn->pn_offset = top;

    /* Emit notes to tell the current bytecode's source line number. */
    if (!UpdateLineNumberNotes(cx, bce, pn->pn_pos.begin))
        return false;

    switch (pn->getKind()) {
      case PNK_FUNCTION:
        ok = EmitFunc(cx, bce, pn);
        break;

      case PNK_ARGSBODY:
      {
        RootedFunction fun(cx, bce->sc->asFunctionBox()->function());
        ParseNode* pnlast = pn->last();

        // Carefully emit everything in the right order:
        // 1. Destructuring
        // 2. Defaults
        // 3. Functions
        ParseNode* pnchild = pnlast->pn_head;
        if (pnlast->pn_xflags & PNX_DESTRUCT) {
            // Assign the destructuring arguments before defining any functions,
            // see bug 419662.
            MOZ_ASSERT(pnchild->isKind(PNK_SEMI));
            MOZ_ASSERT(pnchild->pn_kid->isKind(PNK_VAR) || pnchild->pn_kid->isKind(PNK_GLOBALCONST));
            if (!EmitTree(cx, bce, pnchild))
                return false;
            pnchild = pnchild->pn_next;
        }
        bool hasDefaults = bce->sc->asFunctionBox()->hasDefaults();
        if (hasDefaults) {
            ParseNode* rest = nullptr;
            bool restIsDefn = false;
            if (fun->hasRest()) {
                MOZ_ASSERT(!bce->sc->asFunctionBox()->argumentsHasLocalBinding());

                // Defaults with a rest parameter need special handling. The
                // rest parameter needs to be undefined while defaults are being
                // processed. To do this, we create the rest argument and let it
                // sit on the stack while processing defaults. The rest
                // parameter's slot is set to undefined for the course of
                // default processing.
                rest = pn->pn_head;
                while (rest->pn_next != pnlast)
                    rest = rest->pn_next;
                restIsDefn = rest->isDefn();
                if (Emit1(cx, bce, JSOP_REST) < 0)
                    return false;
                CheckTypeSet(cx, bce, JSOP_REST);

                // Only set the rest parameter if it's not aliased by a nested
                // function in the body.
                if (restIsDefn) {
                    if (Emit1(cx, bce, JSOP_UNDEFINED) < 0)
                        return false;
                    if (!BindNameToSlot(cx, bce, rest))
                        return false;
                    if (!EmitVarOp(cx, rest, JSOP_SETARG, bce))
                        return false;
                    if (Emit1(cx, bce, JSOP_POP) < 0)
                        return false;
                }
            }
            if (!EmitDefaults(cx, bce, pn))
                return false;
            if (fun->hasRest()) {
                if (restIsDefn && !EmitVarOp(cx, rest, JSOP_SETARG, bce))
                    return false;
                if (Emit1(cx, bce, JSOP_POP) < 0)
                    return false;
            }
        }
        for (ParseNode* pn2 = pn->pn_head; pn2 != pnlast; pn2 = pn2->pn_next) {
            // Only bind the parameter if it's not aliased by a nested function
            // in the body.
            if (!pn2->isDefn())
                continue;
            if (!BindNameToSlot(cx, bce, pn2))
                return false;
            if (pn2->pn_next == pnlast && fun->hasRest() && !hasDefaults) {
                // Fill rest parameter. We handled the case with defaults above.
                MOZ_ASSERT(!bce->sc->asFunctionBox()->argumentsHasLocalBinding());
                bce->switchToProlog();
                if (Emit1(cx, bce, JSOP_REST) < 0)
                    return false;
                CheckTypeSet(cx, bce, JSOP_REST);
                if (!EmitVarOp(cx, pn2, JSOP_SETARG, bce))
                    return false;
                if (Emit1(cx, bce, JSOP_POP) < 0)
                    return false;
                bce->switchToMain();
            }
        }
        if (pnlast->pn_xflags & PNX_FUNCDEFS) {
            // This block contains top-level function definitions. To ensure
            // that we emit the bytecode defining them before the rest of code
            // in the block we use a separate pass over functions. During the
            // main pass later the emitter will add JSOP_NOP with source notes
            // for the function to preserve the original functions position
            // when decompiling.
            //
            // Currently this is used only for functions, as compile-as-we go
            // mode for scripts does not allow separate emitter passes.
            for (ParseNode* pn2 = pnchild; pn2; pn2 = pn2->pn_next) {
                if (pn2->isKind(PNK_FUNCTION) && pn2->functionIsHoisted()) {
                    if (!EmitTree(cx, bce, pn2))
                        return false;
                }
            }
        }
        ok = EmitTree(cx, bce, pnlast);
        break;
      }

      case PNK_IF:
        ok = EmitIf(cx, bce, pn);
        break;

      case PNK_SWITCH:
        ok = EmitSwitch(cx, bce, pn);
        break;

      case PNK_WHILE:
        ok = EmitWhile(cx, bce, pn, top);
        break;

      case PNK_DOWHILE:
        ok = EmitDo(cx, bce, pn);
        break;

      case PNK_FOR:
        ok = EmitFor(cx, bce, pn, top);
        break;

      case PNK_BREAK:
        ok = EmitBreak(cx, bce, pn->as<BreakStatement>().label());
        break;

      case PNK_CONTINUE:
        ok = EmitContinue(cx, bce, pn->as<ContinueStatement>().label());
        break;

      case PNK_WITH:
        ok = EmitWith(cx, bce, pn);
        break;

      case PNK_TRY:
        if (!EmitTry(cx, bce, pn))
            return false;
        break;

      case PNK_CATCH:
        if (!EmitCatch(cx, bce, pn))
            return false;
        break;

      case PNK_VAR:
      case PNK_GLOBALCONST:
        if (!EmitVariables(cx, bce, pn, InitializeVars))
            return false;
        break;

      case PNK_RETURN:
        ok = EmitReturn(cx, bce, pn);
        break;

      case PNK_YIELD_STAR:
        ok = EmitYieldStar(cx, bce, pn->pn_left, pn->pn_right);
        break;

      case PNK_GENERATOR:
        if (Emit1(cx, bce, JSOP_GENERATOR) < 0)
            return false;
        break;

      case PNK_YIELD:
        ok = EmitYield(cx, bce, pn);
        break;

      case PNK_STATEMENTLIST:
        ok = EmitStatementList(cx, bce, pn, top);
        break;

      case PNK_SEQ:
        ok = EmitSyntheticStatements(cx, bce, pn, top);
        break;

      case PNK_SEMI:
        ok = EmitStatement(cx, bce, pn);
        break;

      case PNK_LABEL:
        ok = EmitLabeledStatement(cx, bce, &pn->as<LabeledStatement>());
        break;

      case PNK_COMMA:
      {
        for (ParseNode* pn2 = pn->pn_head; ; pn2 = pn2->pn_next) {
            if (!UpdateSourceCoordNotes(cx, bce, pn2->pn_pos.begin))
                return false;
            if (!EmitTree(cx, bce, pn2))
                return false;
            if (!pn2->pn_next)
                break;
            if (Emit1(cx, bce, JSOP_POP) < 0)
                return false;
        }
        break;
      }

      case PNK_ASSIGN:
      case PNK_ADDASSIGN:
      case PNK_SUBASSIGN:
      case PNK_BITORASSIGN:
      case PNK_BITXORASSIGN:
      case PNK_BITANDASSIGN:
      case PNK_LSHASSIGN:
      case PNK_RSHASSIGN:
      case PNK_URSHASSIGN:
      case PNK_MULASSIGN:
      case PNK_DIVASSIGN:
      case PNK_MODASSIGN:
        if (!EmitAssignment(cx, bce, pn->pn_left, pn->getOp(), pn->pn_right))
            return false;
        break;

      case PNK_CONDITIONAL:
        ok = EmitConditionalExpression(cx, bce, pn->as<ConditionalExpression>());
        break;

      case PNK_OR:
      case PNK_AND:
        ok = EmitLogical(cx, bce, pn);
        break;

      case PNK_ADD:
      case PNK_SUB:
      case PNK_BITOR:
      case PNK_BITXOR:
      case PNK_BITAND:
      case PNK_STRICTEQ:
      case PNK_EQ:
      case PNK_STRICTNE:
      case PNK_NE:
      case PNK_LT:
      case PNK_LE:
      case PNK_GT:
      case PNK_GE:
      case PNK_IN:
      case PNK_INSTANCEOF:
      case PNK_LSH:
      case PNK_RSH:
      case PNK_URSH:
      case PNK_STAR:
      case PNK_DIV:
      case PNK_MOD: {
        MOZ_ASSERT(pn->isArity(PN_LIST));
        /* Left-associative operator chain: avoid too much recursion. */
        ParseNode* subexpr = pn->pn_head;
        if (!EmitTree(cx, bce, subexpr))
            return false;
        JSOp op = pn->getOp();
        while ((subexpr = subexpr->pn_next) != nullptr) {
            if (!EmitTree(cx, bce, subexpr))
                return false;
            if (Emit1(cx, bce, op) < 0)
                return false;
        }
        break;
      }

      case PNK_THROW:
      case PNK_TYPEOF:
      case PNK_VOID:
      case PNK_NOT:
      case PNK_BITNOT:
      case PNK_POS:
      case PNK_NEG:
        ok = EmitUnary(cx, bce, pn);
        break;

      case PNK_PREINCREMENT:
      case PNK_PREDECREMENT:
      case PNK_POSTINCREMENT:
      case PNK_POSTDECREMENT:
        ok = EmitIncOrDec(cx, bce, pn);
        break;

      case PNK_DELETE:
        ok = EmitDelete(cx, bce, pn);
        break;

      case PNK_DOT:
        ok = EmitPropOp(cx, pn, JSOP_GETPROP, bce);
        break;

      case PNK_ELEM:
        ok = EmitElemOp(cx, pn, JSOP_GETELEM, bce);
        break;

      case PNK_NEW:
      case PNK_TAGGED_TEMPLATE:
      case PNK_CALL:
      case PNK_GENEXP:
        ok = EmitCallOrNew(cx, bce, pn);
        break;

      case PNK_LEXICALSCOPE:
        ok = EmitLexicalScope(cx, bce, pn);
        break;

      case PNK_LETBLOCK:
      case PNK_LETEXPR:
        ok = EmitLet(cx, bce, pn);
        break;

      case PNK_CONST:
      case PNK_LET:
        ok = EmitVariables(cx, bce, pn, InitializeVars);
        break;

      case PNK_IMPORT:
      case PNK_EXPORT:
      case PNK_EXPORT_FROM:
       // TODO: Implement emitter support for modules
       bce->reportError(nullptr, JSMSG_MODULES_NOT_IMPLEMENTED);
       return false;

      case PNK_ARRAYPUSH: {
        /*
         * The array object's stack index is in bce->arrayCompDepth. See below
         * under the array initialiser code generator for array comprehension
         * special casing. Note that the array object is a pure stack value,
         * unaliased by blocks, so we can EmitUnaliasedVarOp.
         */
        if (!EmitTree(cx, bce, pn->pn_kid))
            return false;
        if (!EmitDupAt(cx, bce, bce->arrayCompDepth))
            return false;
        if (Emit1(cx, bce, JSOP_ARRAYPUSH) < 0)
            return false;
        break;
      }

      case PNK_CALLSITEOBJ:
            ok = EmitCallSiteObject(cx, bce, pn);
            break;

      case PNK_ARRAY:
        if (!(pn->pn_xflags & PNX_NONCONST) && pn->pn_head) {
            if (bce->checkSingletonContext()) {
                // Bake in the object entirely if it will only be created once.
                ok = EmitSingletonInitialiser(cx, bce, pn);
                break;
            }

            // If the array consists entirely of primitive values, make a
            // template object with copy on write elements that can be reused
            // every time the initializer executes.
            if (bce->emitterMode != BytecodeEmitter::SelfHosting && pn->pn_count != 0) {
                RootedValue value(cx);
                if (!pn->getConstantValue(cx, ParseNode::DontAllowNestedObjects, &value))
                    return false;
                if (!value.isMagic(JS_GENERIC_MAGIC)) {
                    // Note: the group of the template object might not yet reflect
                    // that the object has copy on write elements. When the
                    // interpreter or JIT compiler fetches the template, it should
                    // use ObjectGroup::getOrFixupCopyOnWriteObject to make sure the
                    // group for the template is accurate. We don't do this here as we
                    // want to use ObjectGroup::allocationSiteGroup, which requires a
                    // finished script.
                    NativeObject* obj = &value.toObject().as<NativeObject>();
                    if (!ObjectElements::MakeElementsCopyOnWrite(cx, obj))
                        return false;

                    ObjectBox* objbox = bce->parser->newObjectBox(obj);
                    if (!objbox)
                        return false;

                    ok = EmitObjectOp(cx, objbox, JSOP_NEWARRAY_COPYONWRITE, bce);
                    break;
                }
            }
        }

        ok = EmitArray(cx, bce, pn->pn_head, pn->pn_count);
        break;

       case PNK_ARRAYCOMP:
        ok = EmitArrayComp(cx, bce, pn);
        break;

      case PNK_OBJECT:
        ok = EmitObject(cx, bce, pn);
        break;

      case PNK_NAME:
        if (!EmitNameOp(cx, bce, pn, false))
            return false;
        break;

      case PNK_TEMPLATE_STRING_LIST:
        ok = EmitTemplateString(cx, bce, pn);
        break;

      case PNK_TEMPLATE_STRING:
      case PNK_STRING:
        ok = EmitAtomOp(cx, pn, JSOP_STRING, bce);
        break;

      case PNK_NUMBER:
        ok = EmitNumberOp(cx, pn->pn_dval, bce);
        break;

      case PNK_REGEXP:
        ok = EmitRegExp(cx, bce->regexpList.add(pn->as<RegExpLiteral>().objbox()), bce);
        break;

      case PNK_TRUE:
      case PNK_FALSE:
      case PNK_THIS:
      case PNK_NULL:
        if (Emit1(cx, bce, pn->getOp()) < 0)
            return false;
        break;

      case PNK_DEBUGGER:
        if (!UpdateSourceCoordNotes(cx, bce, pn->pn_pos.begin))
            return false;
        if (Emit1(cx, bce, JSOP_DEBUGGER) < 0)
            return false;
        break;

      case PNK_NOP:
        MOZ_ASSERT(pn->getArity() == PN_NULLARY);
        break;

      default:
        MOZ_ASSERT(0);
    }

    /* bce->emitLevel == 1 means we're last on the stack, so finish up. */
    if (ok && bce->emitLevel == 1) {
        if (!UpdateSourceCoordNotes(cx, bce, pn->pn_pos.end))
            return false;
    }

    return ok;
}

static int
AllocSrcNote(ExclusiveContext* cx, SrcNotesVector& notes)
{
    // Start it off moderately large to avoid repeated resizings early on.
    // ~99% of cases fit within 256 bytes.
    if (notes.capacity() == 0 && !notes.reserve(256))
        return -1;

    jssrcnote dummy = 0;
    if (!notes.append(dummy)) {
        js_ReportOutOfMemory(cx);
        return -1;
    }
    return notes.length() - 1;
}

int
frontend::NewSrcNote(ExclusiveContext* cx, BytecodeEmitter* bce, SrcNoteType type)
{
    SrcNotesVector& notes = bce->notes();
    int index;

    index = AllocSrcNote(cx, notes);
    if (index < 0)
        return -1;

    /*
     * Compute delta from the last annotated bytecode's offset.  If it's too
     * big to fit in sn, allocate one or more xdelta notes and reset sn.
     */
    ptrdiff_t offset = bce->offset();
    ptrdiff_t delta = offset - bce->lastNoteOffset();
    bce->current->lastNoteOffset = offset;
    if (delta >= SN_DELTA_LIMIT) {
        do {
            ptrdiff_t xdelta = Min(delta, SN_XDELTA_MASK);
            SN_MAKE_XDELTA(&notes[index], xdelta);
            delta -= xdelta;
            index = AllocSrcNote(cx, notes);
            if (index < 0)
                return -1;
        } while (delta >= SN_DELTA_LIMIT);
    }

    /*
     * Initialize type and delta, then allocate the minimum number of notes
     * needed for type's arity.  Usually, we won't need more, but if an offset
     * does take two bytes, SetSrcNoteOffset will grow notes.
     */
    SN_MAKE_NOTE(&notes[index], type, delta);
    for (int n = (int)js_SrcNoteSpec[type].arity; n > 0; n--) {
        if (NewSrcNote(cx, bce, SRC_NULL) < 0)
            return -1;
    }
    return index;
}

int
frontend::NewSrcNote2(ExclusiveContext* cx, BytecodeEmitter* bce, SrcNoteType type, ptrdiff_t offset)
{
    int index;

    index = NewSrcNote(cx, bce, type);
    if (index >= 0) {
        if (!SetSrcNoteOffset(cx, bce, index, 0, offset))
            return -1;
    }
    return index;
}

int
frontend::NewSrcNote3(ExclusiveContext* cx, BytecodeEmitter* bce, SrcNoteType type, ptrdiff_t offset1,
            ptrdiff_t offset2)
{
    int index;

    index = NewSrcNote(cx, bce, type);
    if (index >= 0) {
        if (!SetSrcNoteOffset(cx, bce, index, 0, offset1))
            return -1;
        if (!SetSrcNoteOffset(cx, bce, index, 1, offset2))
            return -1;
    }
    return index;
}

bool
frontend::AddToSrcNoteDelta(ExclusiveContext* cx, BytecodeEmitter* bce, jssrcnote* sn, ptrdiff_t delta)
{
    /*
     * Called only from FinishTakingSrcNotes to add to main script note
     * deltas, and only by a small positive amount.
     */
    MOZ_ASSERT(bce->current == &bce->main);
    MOZ_ASSERT((unsigned) delta < (unsigned) SN_XDELTA_LIMIT);

    ptrdiff_t base = SN_DELTA(sn);
    ptrdiff_t limit = SN_IS_XDELTA(sn) ? SN_XDELTA_LIMIT : SN_DELTA_LIMIT;
    ptrdiff_t newdelta = base + delta;
    if (newdelta < limit) {
        SN_SET_DELTA(sn, newdelta);
    } else {
        jssrcnote xdelta;
        SN_MAKE_XDELTA(&xdelta, delta);
        if (!(sn = bce->main.notes.insert(sn, xdelta)))
            return false;
    }
    return true;
}

static bool
SetSrcNoteOffset(ExclusiveContext* cx, BytecodeEmitter* bce, unsigned index, unsigned which,
                 ptrdiff_t offset)
{
    if (!SN_REPRESENTABLE_OFFSET(offset)) {
        ReportStatementTooLarge(bce->parser->tokenStream, bce->topStmt);
        return false;
    }

    SrcNotesVector& notes = bce->notes();

    /* Find the offset numbered which (i.e., skip exactly which offsets). */
    jssrcnote* sn = notes.begin() + index;
    MOZ_ASSERT(SN_TYPE(sn) != SRC_XDELTA);
    MOZ_ASSERT((int) which < js_SrcNoteSpec[SN_TYPE(sn)].arity);
    for (sn++; which; sn++, which--) {
        if (*sn & SN_4BYTE_OFFSET_FLAG)
            sn += 3;
    }

    /*
     * See if the new offset requires four bytes either by being too big or if
     * the offset has already been inflated (in which case, we need to stay big
     * to not break the srcnote encoding if this isn't the last srcnote).
     */
    if (offset > (ptrdiff_t)SN_4BYTE_OFFSET_MASK || (*sn & SN_4BYTE_OFFSET_FLAG)) {
        /* Maybe this offset was already set to a four-byte value. */
        if (!(*sn & SN_4BYTE_OFFSET_FLAG)) {
            /* Insert three dummy bytes that will be overwritten shortly. */
            jssrcnote dummy = 0;
            if (!(sn = notes.insert(sn, dummy)) ||
                !(sn = notes.insert(sn, dummy)) ||
                !(sn = notes.insert(sn, dummy)))
            {
                js_ReportOutOfMemory(cx);
                return false;
            }
        }
        *sn++ = (jssrcnote)(SN_4BYTE_OFFSET_FLAG | (offset >> 24));
        *sn++ = (jssrcnote)(offset >> 16);
        *sn++ = (jssrcnote)(offset >> 8);
    }
    *sn = (jssrcnote)offset;
    return true;
}

/*
 * Finish taking source notes in cx's notePool.
 * If successful, the final source note count is stored in the out outparam.
 */
bool
frontend::FinishTakingSrcNotes(ExclusiveContext* cx, BytecodeEmitter* bce, uint32_t* out)
{
    MOZ_ASSERT(bce->current == &bce->main);

    unsigned prologCount = bce->prolog.notes.length();
    if (prologCount && bce->prolog.currentLine != bce->firstLine) {
        bce->switchToProlog();
        if (NewSrcNote2(cx, bce, SRC_SETLINE, (ptrdiff_t)bce->firstLine) < 0)
            return false;
        bce->switchToMain();
    } else {
        /*
         * Either no prolog srcnotes, or no line number change over prolog.
         * We don't need a SRC_SETLINE, but we may need to adjust the offset
         * of the first main note, by adding to its delta and possibly even
         * prepending SRC_XDELTA notes to it to account for prolog bytecodes
         * that came at and after the last annotated bytecode.
         */
        ptrdiff_t offset = bce->prologOffset() - bce->prolog.lastNoteOffset;
        MOZ_ASSERT(offset >= 0);
        if (offset > 0 && bce->main.notes.length() != 0) {
            /* NB: Use as much of the first main note's delta as we can. */
            jssrcnote* sn = bce->main.notes.begin();
            ptrdiff_t delta = SN_IS_XDELTA(sn)
                            ? SN_XDELTA_MASK - (*sn & SN_XDELTA_MASK)
                            : SN_DELTA_MASK - (*sn & SN_DELTA_MASK);
            if (offset < delta)
                delta = offset;
            for (;;) {
                if (!AddToSrcNoteDelta(cx, bce, sn, delta))
                    return false;
                offset -= delta;
                if (offset == 0)
                    break;
                delta = Min(offset, SN_XDELTA_MASK);
                sn = bce->main.notes.begin();
            }
        }
    }

    // The prolog count might have changed, so we can't reuse prologCount.
    // The + 1 is to account for the final SN_MAKE_TERMINATOR that is appended
    // when the notes are copied to their final destination by CopySrcNotes.
    *out = bce->prolog.notes.length() + bce->main.notes.length() + 1;
    return true;
}

void
frontend::CopySrcNotes(BytecodeEmitter* bce, jssrcnote* destination, uint32_t nsrcnotes)
{
    unsigned prologCount = bce->prolog.notes.length();
    unsigned mainCount = bce->main.notes.length();
    unsigned totalCount = prologCount + mainCount;
    MOZ_ASSERT(totalCount == nsrcnotes - 1);
    if (prologCount)
        PodCopy(destination, bce->prolog.notes.begin(), prologCount);
    PodCopy(destination + prologCount, bce->main.notes.begin(), mainCount);
    SN_MAKE_TERMINATOR(&destination[totalCount]);
}

void
CGConstList::finish(ConstArray* array)
{
    MOZ_ASSERT(length() == array->length);

    for (unsigned i = 0; i < length(); i++)
        array->vector[i] = list[i];
}

/*
 * Find the index of the given object for code generator.
 *
 * Since the emitter refers to each parsed object only once, for the index we
 * use the number of already indexes objects. We also add the object to a list
 * to convert the list to a fixed-size array when we complete code generation,
 * see js::CGObjectList::finish below.
 *
 * Most of the objects go to BytecodeEmitter::objectList but for regexp we use
 * a separated BytecodeEmitter::regexpList. In this way the emitted index can
 * be directly used to store and fetch a reference to a cloned RegExp object
 * that shares the same JSRegExp private data created for the object literal in
 * objbox. We need a cloned object to hold lastIndex and other direct
 * properties that should not be shared among threads sharing a precompiled
 * function or script.
 *
 * If the code being compiled is function code, allocate a reserved slot in
 * the cloned function object that shares its precompiled script with other
 * cloned function objects and with the compiler-created clone-parent. There
 * are nregexps = script->regexps()->length such reserved slots in each
 * function object cloned from fun->object. NB: during compilation, a funobj
 * slots element must never be allocated, because JSObject::allocSlot could
 * hand out one of the slots that should be given to a regexp clone.
 *
 * If the code being compiled is global code, the cloned regexp are stored in
 * fp->vars slot and to protect regexp slots from GC we set fp->nvars to
 * nregexps.
 *
 * The slots initially contain undefined or null. We populate them lazily when
 * JSOP_REGEXP is executed for the first time.
 *
 * Why clone regexp objects?  ECMA specifies that when a regular expression
 * literal is scanned, a RegExp object is created.  In the spec, compilation
 * and execution happen indivisibly, but in this implementation and many of
 * its embeddings, code is precompiled early and re-executed in multiple
 * threads, or using multiple global objects, or both, for efficiency.
 *
 * In such cases, naively following ECMA leads to wrongful sharing of RegExp
 * objects, which makes for collisions on the lastIndex property (especially
 * for global regexps) and on any ad-hoc properties.  Also, __proto__ refers to
 * the pre-compilation prototype, a pigeon-hole problem for instanceof tests.
 */
unsigned
CGObjectList::add(ObjectBox* objbox)
{
    MOZ_ASSERT(!objbox->emitLink);
    objbox->emitLink = lastbox;
    lastbox = objbox;
    return length++;
}

unsigned
CGObjectList::indexOf(JSObject* obj)
{
    MOZ_ASSERT(length > 0);
    unsigned index = length - 1;
    for (ObjectBox* box = lastbox; box->object != obj; box = box->emitLink)
        index--;
    return index;
}

void
CGObjectList::finish(ObjectArray* array)
{
    MOZ_ASSERT(length <= INDEX_LIMIT);
    MOZ_ASSERT(length == array->length);

    js::HeapPtrNativeObject* cursor = array->vector + array->length;
    ObjectBox* objbox = lastbox;
    do {
        --cursor;
        MOZ_ASSERT(!*cursor);
        *cursor = objbox->object;
    } while ((objbox = objbox->emitLink) != nullptr);
    MOZ_ASSERT(cursor == array->vector);
}

ObjectBox*
CGObjectList::find(uint32_t index)
{
    MOZ_ASSERT(index < length);
    ObjectBox* box = lastbox;
    for (unsigned n = length - 1; n > index; n--)
        box = box->emitLink;
    return box;
}

bool
CGTryNoteList::append(JSTryNoteKind kind, uint32_t stackDepth, size_t start, size_t end)
{
    MOZ_ASSERT(start <= end);
    MOZ_ASSERT(size_t(uint32_t(start)) == start);
    MOZ_ASSERT(size_t(uint32_t(end)) == end);

    JSTryNote note;
    note.kind = kind;
    note.stackDepth = stackDepth;
    note.start = uint32_t(start);
    note.length = uint32_t(end - start);

    return list.append(note);
}

void
CGTryNoteList::finish(TryNoteArray* array)
{
    MOZ_ASSERT(length() == array->length);

    for (unsigned i = 0; i < length(); i++)
        array->vector[i] = list[i];
}

bool
CGBlockScopeList::append(uint32_t scopeObject, uint32_t offset, uint32_t parent)
{
    BlockScopeNote note;
    mozilla::PodZero(&note);

    note.index = scopeObject;
    note.start = offset;
    note.parent = parent;

    return list.append(note);
}

uint32_t
CGBlockScopeList::findEnclosingScope(uint32_t index)
{
    MOZ_ASSERT(index < length());
    MOZ_ASSERT(list[index].index != BlockScopeNote::NoBlockScopeIndex);

    DebugOnly<uint32_t> pos = list[index].start;
    while (index--) {
        MOZ_ASSERT(list[index].start <= pos);
        if (list[index].length == 0) {
            // We are looking for the nearest enclosing live scope.  If the
            // scope contains POS, it should still be open, so its length should
            // be zero.
            return list[index].index;
        } else {
            // Conversely, if the length is not zero, it should not contain
            // POS.
            MOZ_ASSERT(list[index].start + list[index].length <= pos);
        }
    }

    return BlockScopeNote::NoBlockScopeIndex;
}

void
CGBlockScopeList::recordEnd(uint32_t index, uint32_t offset)
{
    MOZ_ASSERT(index < length());
    MOZ_ASSERT(offset >= list[index].start);
    MOZ_ASSERT(list[index].length == 0);

    list[index].length = offset - list[index].start;
}

void
CGBlockScopeList::finish(BlockScopeArray* array)
{
    MOZ_ASSERT(length() == array->length);

    for (unsigned i = 0; i < length(); i++)
        array->vector[i] = list[i];
}

void
CGYieldOffsetList::finish(YieldOffsetArray& array, uint32_t prologLength)
{
    MOZ_ASSERT(length() == array.length());

    for (unsigned i = 0; i < length(); i++)
        array[i] = prologLength + list[i];
}

/*
 * We should try to get rid of offsetBias (always 0 or 1, where 1 is
 * JSOP_{NOP,POP}_LENGTH), which is used only by SRC_FOR.
 */
const JSSrcNoteSpec js_SrcNoteSpec[] = {
#define DEFINE_SRC_NOTE_SPEC(sym, name, arity) { name, arity },
    FOR_EACH_SRC_NOTE_TYPE(DEFINE_SRC_NOTE_SPEC)
#undef DEFINE_SRC_NOTE_SPEC
};

static int
SrcNoteArity(jssrcnote* sn)
{
    MOZ_ASSERT(SN_TYPE(sn) < SRC_LAST);
    return js_SrcNoteSpec[SN_TYPE(sn)].arity;
}

JS_FRIEND_API(unsigned)
js_SrcNoteLength(jssrcnote* sn)
{
    unsigned arity;
    jssrcnote* base;

    arity = SrcNoteArity(sn);
    for (base = sn++; arity; sn++, arity--) {
        if (*sn & SN_4BYTE_OFFSET_FLAG)
            sn += 3;
    }
    return sn - base;
}

JS_FRIEND_API(ptrdiff_t)
js_GetSrcNoteOffset(jssrcnote* sn, unsigned which)
{
    /* Find the offset numbered which (i.e., skip exactly which offsets). */
    MOZ_ASSERT(SN_TYPE(sn) != SRC_XDELTA);
    MOZ_ASSERT((int) which < SrcNoteArity(sn));
    for (sn++; which; sn++, which--) {
        if (*sn & SN_4BYTE_OFFSET_FLAG)
            sn += 3;
    }
    if (*sn & SN_4BYTE_OFFSET_FLAG) {
        return (ptrdiff_t)(((uint32_t)(sn[0] & SN_4BYTE_OFFSET_MASK) << 24)
                           | (sn[1] << 16)
                           | (sn[2] << 8)
                           | sn[3]);
    }
    return (ptrdiff_t)*sn;
}
