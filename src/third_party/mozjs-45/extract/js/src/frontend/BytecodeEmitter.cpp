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
#include "mozilla/Maybe.h"
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

using mozilla::Maybe;
using mozilla::Some;
using mozilla::DebugOnly;
using mozilla::NumberIsInt32;
using mozilla::PodCopy;
using mozilla::UniquePtr;

struct frontend::StmtInfoBCE : public StmtInfoBase
{
    StmtInfoBCE*    enclosing;
    StmtInfoBCE*    enclosingScope;

    ptrdiff_t       update;         /* loop update offset (top if none) */
    ptrdiff_t       breaks;         /* offset of last break in loop */
    ptrdiff_t       continues;      /* offset of last continue in loop */
    uint32_t        blockScopeIndex; /* index of scope in BlockScopeArray */

    explicit StmtInfoBCE(ExclusiveContext* cx) : StmtInfoBase(cx) {}

    void setTop(ptrdiff_t top) {
        update = top;
        breaks = -1;
        continues = -1;
    }

    /*
     * To reuse space, alias two of the ptrdiff_t fields for use during
     * try/catch/finally code generation and backpatching.
     *
     * Only a loop, switch, or label statement info record can have breaks and
     * continues, and only a for loop has an update backpatch chain, so it's
     * safe to overlay these for the "trying" StmtTypes.
     */

    ptrdiff_t& gosubs() {
        MOZ_ASSERT(type == StmtType::FINALLY);
        return breaks;
    }

    ptrdiff_t& guardJump() {
        MOZ_ASSERT(type == StmtType::TRY || type == StmtType::FINALLY);
        return continues;
    }
};

struct frontend::LoopStmtInfo : public StmtInfoBCE
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

BytecodeEmitter::BytecodeEmitter(BytecodeEmitter* parent,
                                 Parser<FullParseHandler>* parser, SharedContext* sc,
                                 HandleScript script, Handle<LazyScript*> lazyScript,
                                 bool insideEval, HandleScript evalCaller,
                                 bool insideNonGlobalEval, uint32_t lineNum,
                                 EmitterMode emitterMode)
  : sc(sc),
    cx(sc->context),
    parent(parent),
    script(cx, script),
    lazyScript(cx, lazyScript),
    prologue(cx, lineNum),
    main(cx, lineNum),
    current(&main),
    parser(parser),
    evalCaller(evalCaller),
    stmtStack(cx),
    atomIndices(cx),
    firstLine(lineNum),
    localsToFrameSlots_(cx),
    stackDepth(0), maxStackDepth(0),
    arrayCompDepth(0),
    emitLevel(0),
    constList(cx),
    tryNoteList(cx),
    blockScopeList(cx),
    yieldOffsetList(cx),
    typesetCount(0),
    hasSingletons(false),
    hasTryFinally(false),
    emittingForInit(false),
    emittingRunOnceLambda(false),
    insideEval(insideEval),
    insideNonGlobalEval(insideNonGlobalEval),
    insideModule(false),
    emitterMode(emitterMode)
{
    MOZ_ASSERT_IF(evalCaller, insideEval);
    MOZ_ASSERT_IF(emitterMode == LazyFunction, lazyScript);
}

bool
BytecodeEmitter::init()
{
    return atomIndices.ensureMap(cx);
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

bool
BytecodeEmitter::emitCheck(ptrdiff_t delta, ptrdiff_t* offset)
{
    *offset = code().length();

    // Start it off moderately large to avoid repeated resizings early on.
    // ~98% of cases fit within 1024 bytes.
    if (code().capacity() == 0 && !code().reserve(1024))
        return false;

    if (!code().growBy(delta)) {
        ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

void
BytecodeEmitter::updateDepth(ptrdiff_t target)
{
    jsbytecode* pc = code(target);

    int nuses = StackUses(nullptr, pc);
    int ndefs = StackDefs(nullptr, pc);

    stackDepth -= nuses;
    MOZ_ASSERT(stackDepth >= 0);
    stackDepth += ndefs;
    if ((uint32_t)stackDepth > maxStackDepth)
        maxStackDepth = stackDepth;
}

#ifdef DEBUG
bool
BytecodeEmitter::checkStrictOrSloppy(JSOp op)
{
    if (IsCheckStrictOp(op) && !sc->strict())
        return false;
    if (IsCheckSloppyOp(op) && sc->strict())
        return false;
    return true;
}
#endif

bool
BytecodeEmitter::emit1(JSOp op)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    ptrdiff_t offset;
    if (!emitCheck(1, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emit2(JSOp op, uint8_t op1)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    ptrdiff_t offset;
    if (!emitCheck(2, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    code[1] = jsbytecode(op1);
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emit3(JSOp op, jsbytecode op1, jsbytecode op2)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    /* These should filter through emitVarOp. */
    MOZ_ASSERT(!IsArgOp(op));
    MOZ_ASSERT(!IsLocalOp(op));

    ptrdiff_t offset;
    if (!emitCheck(3, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    code[1] = op1;
    code[2] = op2;
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emitN(JSOp op, size_t extra, ptrdiff_t* offset)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));
    ptrdiff_t length = 1 + ptrdiff_t(extra);

    ptrdiff_t off;
    if (!emitCheck(length, &off))
        return false;

    jsbytecode* code = this->code(off);
    code[0] = jsbytecode(op);
    /* The remaining |extra| bytes are set by the caller */

    /*
     * Don't updateDepth if op's use-count comes from the immediate
     * operand yet to be stored in the extra bytes after op.
     */
    if (CodeSpec[op].nuses >= 0)
        updateDepth(off);

    if (offset)
        *offset = off;
    return true;
}

bool
BytecodeEmitter::emitJump(JSOp op, ptrdiff_t off, ptrdiff_t* jumpOffset)
{
    ptrdiff_t offset;
    if (!emitCheck(5, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    SET_JUMP_OFFSET(code, off);
    updateDepth(offset);
    if (jumpOffset)
        *jumpOffset = offset;
    return true;
}

bool
BytecodeEmitter::emitCall(JSOp op, uint16_t argc, ParseNode* pn)
{
    if (pn && !updateSourceCoordNotes(pn->pn_pos.begin))
        return false;
    return emit3(op, ARGC_HI(argc), ARGC_LO(argc));
}

bool
BytecodeEmitter::emitDupAt(unsigned slotFromTop)
{
    MOZ_ASSERT(slotFromTop < unsigned(stackDepth));

    if (slotFromTop >= JS_BIT(24)) {
        reportError(nullptr, JSMSG_TOO_MANY_LOCALS);
        return false;
    }

    ptrdiff_t off;
    if (!emitN(JSOP_DUPAT, 3, &off))
        return false;

    jsbytecode* pc = code(off);
    SET_UINT24(pc, slotFromTop);
    return true;
}

/* XXX too many "... statement" L10N gaffes below -- fix via js.msg! */
const char js_with_statement_str[] = "with statement";
const char js_finally_block_str[]  = "finally block";

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

static_assert(MOZ_ARRAY_LENGTH(statementName) == uint16_t(StmtType::LIMIT),
              "statementName array and StmtType enum must be consistent");

static const char*
StatementName(StmtInfoBCE* stmt)
{
    if (!stmt)
        return js_script_str;
    return statementName[uint16_t(stmt->type)];
}

static void
ReportStatementTooLarge(TokenStream& ts, StmtInfoBCE* stmt)
{
    ts.reportError(JSMSG_NEED_DIET, StatementName(stmt));
}

/*
 * Emit a backpatch op with offset pointing to the previous jump of this type,
 * so that we can walk back up the chain fixing up the op and jump offset.
 */
bool
BytecodeEmitter::emitBackPatchOp(ptrdiff_t* lastp)
{
    ptrdiff_t delta = offset() - *lastp;
    *lastp = offset();
    MOZ_ASSERT(delta > 0);
    return emitJump(JSOP_BACKPATCH, delta);
}

static inline unsigned
LengthOfSetLine(unsigned line)
{
    return 1 /* SN_SETLINE */ + (line > SN_4BYTE_OFFSET_MASK ? 4 : 1);
}

/* Updates line number notes, not column notes. */
bool
BytecodeEmitter::updateLineNumberNotes(uint32_t offset)
{
    TokenStream* ts = &parser->tokenStream;
    bool onThisLine;
    if (!ts->srcCoords.isOnThisLine(offset, currentLine(), &onThisLine))
        return ts->reportError(JSMSG_OUT_OF_MEMORY);
    if (!onThisLine) {
        unsigned line = ts->srcCoords.lineNum(offset);
        unsigned delta = line - currentLine();

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
        current->currentLine = line;
        current->lastColumn  = 0;
        if (delta >= LengthOfSetLine(line)) {
            if (!newSrcNote2(SRC_SETLINE, ptrdiff_t(line)))
                return false;
        } else {
            do {
                if (!newSrcNote(SRC_NEWLINE))
                    return false;
            } while (--delta != 0);
        }
    }
    return true;
}

/* Updates the line number and column number information in the source notes. */
bool
BytecodeEmitter::updateSourceCoordNotes(uint32_t offset)
{
    if (!updateLineNumberNotes(offset))
        return false;

    uint32_t columnIndex = parser->tokenStream.srcCoords.columnIndex(offset);
    ptrdiff_t colspan = ptrdiff_t(columnIndex) - ptrdiff_t(current->lastColumn);
    if (colspan != 0) {
        // If the column span is so large that we can't store it, then just
        // discard this information. This can happen with minimized or otherwise
        // machine-generated code. Even gigantic column numbers are still
        // valuable if you have a source map to relate them to something real;
        // but it's better to fail soft here.
        if (!SN_REPRESENTABLE_COLSPAN(colspan))
            return true;
        if (!newSrcNote2(SRC_COLSPAN, SN_COLSPAN_TO_OFFSET(colspan)))
            return false;
        current->lastColumn = columnIndex;
    }
    return true;
}

bool
BytecodeEmitter::emitLoopHead(ParseNode* nextpn)
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
        if (!updateSourceCoordNotes(nextpn->pn_pos.begin))
            return false;
    }

    return emit1(JSOP_LOOPHEAD);
}

bool
BytecodeEmitter::emitLoopEntry(ParseNode* nextpn)
{
    if (nextpn) {
        /* Update the line number, as for LOOPHEAD. */
        MOZ_ASSERT_IF(nextpn->isKind(PNK_STATEMENTLIST), nextpn->isArity(PN_LIST));
        if (nextpn->isKind(PNK_STATEMENTLIST) && nextpn->pn_head)
            nextpn = nextpn->pn_head;
        if (!updateSourceCoordNotes(nextpn->pn_pos.begin))
            return false;
    }

    LoopStmtInfo* loop = LoopStmtInfo::fromStmtInfo(innermostStmt());
    MOZ_ASSERT(loop->loopDepth > 0);

    uint8_t loopDepthAndFlags = PackLoopEntryDepthHintAndFlags(loop->loopDepth, loop->canIonOsr);
    return emit2(JSOP_LOOPENTRY, loopDepthAndFlags);
}

void
BytecodeEmitter::checkTypeSet(JSOp op)
{
    if (CodeSpec[op].format & JOF_TYPESET) {
        if (typesetCount < UINT16_MAX)
            typesetCount++;
    }
}

bool
BytecodeEmitter::emitUint16Operand(JSOp op, uint32_t operand)
{
    MOZ_ASSERT(operand <= UINT16_MAX);
    if (!emit3(op, UINT16_HI(operand), UINT16_LO(operand)))
        return false;
    checkTypeSet(op);
    return true;
}

bool
BytecodeEmitter::emitUint32Operand(JSOp op, uint32_t operand)
{
    ptrdiff_t off;
    if (!emitN(op, 4, &off))
        return false;
    SET_UINT32(code(off), operand);
    checkTypeSet(op);
    return true;
}

bool
BytecodeEmitter::flushPops(int* npops)
{
    MOZ_ASSERT(*npops != 0);
    if (!emitUint16Operand(JSOP_POPN, *npops))
        return false;

    *npops = 0;
    return true;
}

namespace {

class NonLocalExitScope {
    BytecodeEmitter* bce;
    const uint32_t savedScopeIndex;
    const int savedDepth;
    uint32_t openScopeIndex;

    NonLocalExitScope(const NonLocalExitScope&) = delete;

  public:
    explicit NonLocalExitScope(BytecodeEmitter* bce_)
      : bce(bce_),
        savedScopeIndex(bce->blockScopeList.length()),
        savedDepth(bce->stackDepth),
        openScopeIndex(UINT32_MAX)
    {
        if (StmtInfoBCE* stmt = bce->innermostScopeStmt())
            openScopeIndex = stmt->blockScopeIndex;
    }
    ~NonLocalExitScope() {
        for (uint32_t n = savedScopeIndex; n < bce->blockScopeList.length(); n++)
            bce->blockScopeList.recordEnd(n, bce->offset(), bce->inPrologue());
        bce->stackDepth = savedDepth;
    }

    bool popScopeForNonLocalExit(uint32_t blockScopeIndex) {
        uint32_t scopeObjectIndex = bce->blockScopeList.findEnclosingScope(blockScopeIndex);
        uint32_t parent = openScopeIndex;

        if (!bce->blockScopeList.append(scopeObjectIndex, bce->offset(), bce->inPrologue(), parent))
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

#define FLUSH_POPS() if (npops && !bce->flushPops(&npops)) return false

    for (StmtInfoBCE* stmt = bce->innermostStmt(); stmt != toStmt; stmt = stmt->enclosing) {
        switch (stmt->type) {
          case StmtType::FINALLY:
            FLUSH_POPS();
            if (!bce->emitBackPatchOp(&stmt->gosubs()))
                return false;
            break;

          case StmtType::WITH:
            if (!bce->emit1(JSOP_LEAVEWITH))
                return false;
            MOZ_ASSERT(stmt->linksScope());
            if (!popScopeForNonLocalExit(stmt->blockScopeIndex))
                return false;
            break;

          case StmtType::FOR_OF_LOOP:
            npops += 2;
            break;

          case StmtType::FOR_IN_LOOP:
            /* The iterator and the current value are on the stack. */
            npops += 1;
            FLUSH_POPS();
            if (!bce->emit1(JSOP_ENDITER))
                return false;
            break;

          case StmtType::SPREAD:
            MOZ_ASSERT_UNREACHABLE("can't break/continue/return from inside a spread");
            break;

          case StmtType::SUBROUTINE:
            /*
             * There's a [exception or hole, retsub pc-index] pair and the
             * possible return value on the stack that we need to pop.
             */
            npops += 3;
            break;

          default:;
        }

        if (stmt->isBlockScope) {
            StaticBlockObject& blockObj = stmt->staticBlock();
            if (blockObj.needsClone()) {
                if (!bce->emit1(JSOP_POPBLOCKSCOPE))
                    return false;
            } else {
                if (!bce->emit1(JSOP_DEBUGLEAVEBLOCK))
                    return false;
            }
            if (!popScopeForNonLocalExit(stmt->blockScopeIndex))
                return false;
        }
    }

    FLUSH_POPS();
    return true;

#undef FLUSH_POPS
}

}  // anonymous namespace

bool
BytecodeEmitter::emitGoto(StmtInfoBCE* toStmt, ptrdiff_t* lastp, SrcNoteType noteType)
{
    NonLocalExitScope nle(this);

    if (!nle.prepareForNonLocalJump(toStmt))
        return false;

    if (noteType != SRC_NULL) {
        if (!newSrcNote(noteType))
            return false;
    }

    return emitBackPatchOp(lastp);
}

void
BytecodeEmitter::backPatch(ptrdiff_t last, jsbytecode* target, jsbytecode op)
{
    jsbytecode* pc = code(last);
    jsbytecode* stop = code(-1);
    while (pc != stop) {
        ptrdiff_t delta = GET_JUMP_OFFSET(pc);
        ptrdiff_t span = target - pc;
        SET_JUMP_OFFSET(pc, span);
        *pc = op;
        pc -= delta;
    }
}

void
BytecodeEmitter::pushStatementInner(StmtInfoBCE* stmt, StmtType type, ptrdiff_t top)
{
    stmt->setTop(top);
    stmtStack.push(stmt, type);
}

void
BytecodeEmitter::pushStatement(StmtInfoBCE* stmt, StmtType type, ptrdiff_t top)
{
    pushStatementInner(stmt, type, top);
    MOZ_ASSERT(!stmt->isLoop());
}

void
BytecodeEmitter::pushLoopStatement(LoopStmtInfo* stmt, StmtType type, ptrdiff_t top)
{
    pushStatementInner(stmt, type, top);
    MOZ_ASSERT(stmt->isLoop());

    LoopStmtInfo* enclosingLoop = nullptr;
    for (StmtInfoBCE* outer = stmt->enclosing; outer; outer = outer->enclosing) {
        if (outer->isLoop()) {
            enclosingLoop = LoopStmtInfo::fromStmtInfo(outer);
            break;
        }
    }

    stmt->stackDepth = this->stackDepth;
    stmt->loopDepth = enclosingLoop ? enclosingLoop->loopDepth + 1 : 1;

    int loopSlots;
    if (type == StmtType::SPREAD)
        loopSlots = 3;
    else if (type == StmtType::FOR_IN_LOOP || type == StmtType::FOR_OF_LOOP)
        loopSlots = 2;
    else
        loopSlots = 0;

    MOZ_ASSERT(loopSlots <= stmt->stackDepth);

    if (enclosingLoop) {
        stmt->canIonOsr = (enclosingLoop->canIonOsr &&
                           stmt->stackDepth == enclosingLoop->stackDepth + loopSlots);
    } else {
        stmt->canIonOsr = stmt->stackDepth == loopSlots;
    }
}

JSObject*
BytecodeEmitter::innermostStaticScope() const
{
    if (StmtInfoBCE* stmt = innermostScopeStmt())
        return stmt->staticScope;
    return sc->staticScope();
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

bool
BytecodeEmitter::computeAliasedSlots(Handle<StaticBlockObject*> blockObj)
{
    uint32_t numAliased = script->bindings.numAliasedBodyLevelLocals();

    for (unsigned i = 0; i < blockObj->numVariables(); i++) {
        Definition* dn = blockObj->definitionParseNode(i);

        MOZ_ASSERT(dn->isDefn());

        uint32_t index = dn->pn_scopecoord.slot();
        uint32_t slot;

        if (isAliasedName(this, dn)) {
            slot = blockObj->blockIndexToSlot(index);
            blockObj->setAliased(i, true);
        } else {
            // blockIndexToLocalIndex returns the frame slot following the
            // unaliased locals. We add numAliased so that the slot value
            // comes after all (aliased and unaliased) body level locals.
            slot = numAliased + blockObj->blockIndexToLocalIndex(index);
            blockObj->setAliased(i, false);
        }

        if (!dn->pn_scopecoord.setSlot(parser->tokenStream, slot))
            return false;

#ifdef DEBUG
        for (ParseNode* pnu = dn->dn_uses; pnu; pnu = pnu->pn_link) {
            MOZ_ASSERT(pnu->pn_lexdef == dn);
            MOZ_ASSERT(!(pnu->pn_dflags & PND_BOUND));
            MOZ_ASSERT(pnu->pn_scopecoord.isFree());
        }
#endif

    }

    MOZ_ASSERT_IF(sc->allLocalsAliased(), AllLocalsAliased(*blockObj));

    return true;
}

void
BytecodeEmitter::computeLocalOffset(Handle<StaticBlockObject*> blockObj)
{
    unsigned nbodyfixed = !sc->isGlobalContext()
                          ? script->bindings.numUnaliasedBodyLevelLocals()
                          : 0;
    unsigned localOffset = nbodyfixed;

    if (StmtInfoBCE* stmt = innermostScopeStmt()) {
        Rooted<NestedScopeObject*> outer(cx, stmt->staticScope);
        for (; outer; outer = outer->enclosingNestedScope()) {
            if (outer->is<StaticBlockObject>() && !IsStaticGlobalLexicalScope(outer)) {
                StaticBlockObject& outerBlock = outer->as<StaticBlockObject>();
                localOffset = outerBlock.localOffset() + outerBlock.numVariables();
                break;
            }
        }
    }

    MOZ_ASSERT(localOffset + blockObj->numVariables()
               <= nbodyfixed + script->bindings.numBlockScoped());

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
// block scope, if the block has no aliased locals.  This allows DebugScopes
// to invalidate any association between a debugger scope object, which can
// proxy access to unaliased stack locals, and the actual live frame.  In
// normal, non-debug mode, this opcode does not cause any baseline code to be
// emitted.
//
// If the block has aliased locals, no DEBUGLEAVEBLOCK is emitted, and
// POPBLOCKSCOPE itself balances the debug scope mapping. This gets around a
// comedic situation where DEBUGLEAVEBLOCK may remove a block scope from the
// debug scope map, but the immediate following POPBLOCKSCOPE adds it back due
// to an onStep hook.
//
// Enter a nested scope with enterNestedScope.  It will emit
// PUSHBLOCKSCOPE/ENTERWITH if needed, and arrange to record the PC bounds of
// the scope.  Leave a nested scope with leaveNestedScope, which, for blocks,
// will emit DEBUGLEAVEBLOCK and may emit POPBLOCKSCOPE.  (For "with" scopes it
// emits LEAVEWITH, of course.)  Pass enterNestedScope a fresh StmtInfoBCE
// object, and pass that same object to the corresponding leaveNestedScope.  If
// the statement is a block scope, pass StmtType::BLOCK as stmtType; otherwise for
// with scopes pass StmtType::WITH.
//
bool
BytecodeEmitter::enterNestedScope(StmtInfoBCE* stmt, ObjectBox* objbox, StmtType stmtType)
{
    Rooted<NestedScopeObject*> scopeObj(cx, &objbox->object->as<NestedScopeObject>());
    uint32_t scopeObjectIndex = objectList.add(objbox);

    switch (stmtType) {
      case StmtType::BLOCK: {
        Rooted<StaticBlockObject*> blockObj(cx, &scopeObj->as<StaticBlockObject>());

        computeLocalOffset(blockObj);

        if (!computeAliasedSlots(blockObj))
            return false;

        if (blockObj->needsClone()) {
            if (!emitInternedObjectOp(scopeObjectIndex, JSOP_PUSHBLOCKSCOPE))
                return false;
        }

        // Non-global block scopes are non-extensible. At this point the
        // Parser has added all bindings to the StaticBlockObject, so we make
        // it non-extensible.
        if (!blockObj->makeNonExtensible(cx))
            return false;
        break;
      }
      case StmtType::WITH:
        MOZ_ASSERT(scopeObj->is<StaticWithObject>());
        if (!emitInternedObjectOp(scopeObjectIndex, JSOP_ENTERWITH))
            return false;
        break;
      default:
        MOZ_CRASH("Unexpected scope statement");
    }

    uint32_t parent = BlockScopeNote::NoBlockScopeIndex;
    if (StmtInfoBCE* stmt = innermostScopeStmt())
        parent = stmt->blockScopeIndex;

    stmt->blockScopeIndex = blockScopeList.length();
    if (!blockScopeList.append(scopeObjectIndex, offset(), inPrologue(), parent))
        return false;

    pushStatement(stmt, stmtType, offset());
    scopeObj->initEnclosingScope(innermostStaticScope());
    stmtStack.linkAsInnermostScopeStmt(stmt, *scopeObj);
    MOZ_ASSERT(stmt->linksScope());
    stmt->isBlockScope = (stmtType == StmtType::BLOCK);

    return true;
}

// Patches |breaks| and |continues| unless the top statement info record
// represents a try-catch-finally suite.
void
BytecodeEmitter::popStatement()
{
    if (!innermostStmt()->isTrying()) {
        backPatch(innermostStmt()->breaks, code().end(), JSOP_GOTO);
        backPatch(innermostStmt()->continues, code(innermostStmt()->update), JSOP_GOTO);
    }

    stmtStack.pop();
}

bool
BytecodeEmitter::leaveNestedScope(StmtInfoBCE* stmt)
{
    MOZ_ASSERT(stmt == innermostScopeStmt());
    MOZ_ASSERT(stmt->isBlockScope == !(stmt->type == StmtType::WITH));
    uint32_t blockScopeIndex = stmt->blockScopeIndex;

#ifdef DEBUG
    MOZ_ASSERT(blockScopeList.list[blockScopeIndex].length == 0);
    uint32_t blockObjIndex = blockScopeList.list[blockScopeIndex].index;
    ObjectBox* blockObjBox = objectList.find(blockObjIndex);
    NestedScopeObject* staticScope = &blockObjBox->object->as<NestedScopeObject>();
    MOZ_ASSERT(stmt->staticScope == staticScope);
    MOZ_ASSERT_IF(!stmt->isBlockScope, staticScope->is<StaticWithObject>());
#endif

    popStatement();

    if (stmt->isBlockScope) {
        if (stmt->staticScope->as<StaticBlockObject>().needsClone()) {
            if (!emit1(JSOP_POPBLOCKSCOPE))
                return false;
        } else {
            if (!emit1(JSOP_DEBUGLEAVEBLOCK))
                return false;
        }
    } else {
        if (!emit1(JSOP_LEAVEWITH))
            return false;
    }

    blockScopeList.recordEnd(blockScopeIndex, offset(), inPrologue());

    return true;
}

bool
BytecodeEmitter::emitIndex32(JSOp op, uint32_t index)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    const size_t len = 1 + UINT32_INDEX_LEN;
    MOZ_ASSERT(len == size_t(CodeSpec[op].length));

    ptrdiff_t offset;
    if (!emitCheck(len, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    SET_UINT32_INDEX(code, index);
    updateDepth(offset);
    checkTypeSet(op);
    return true;
}

bool
BytecodeEmitter::emitIndexOp(JSOp op, uint32_t index)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    const size_t len = CodeSpec[op].length;
    MOZ_ASSERT(len >= 1 + UINT32_INDEX_LEN);

    ptrdiff_t offset;
    if (!emitCheck(len, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    SET_UINT32_INDEX(code, index);
    updateDepth(offset);
    checkTypeSet(op);
    return true;
}

bool
BytecodeEmitter::emitAtomOp(JSAtom* atom, JSOp op)
{
    MOZ_ASSERT(atom);
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);

    // .generator lookups should be emitted as JSOP_GETALIASEDVAR instead of
    // JSOP_GETNAME etc, to bypass |with| objects on the scope chain.
    // It's safe to emit .this lookups though because |with| objects skip
    // those.
    MOZ_ASSERT_IF(op == JSOP_GETNAME || op == JSOP_GETGNAME,
                  !sc->isDotVariable(atom) || atom == cx->names().dotThis);

    if (op == JSOP_GETPROP && atom == cx->names().length) {
        /* Specialize length accesses for the interpreter. */
        op = JSOP_LENGTH;
    }

    jsatomid index;
    if (!makeAtomIndex(atom, &index))
        return false;

    return emitIndexOp(op, index);
}

bool
BytecodeEmitter::emitAtomOp(ParseNode* pn, JSOp op)
{
    MOZ_ASSERT(pn->pn_atom != nullptr);
    return emitAtomOp(pn->pn_atom, op);
}

bool
BytecodeEmitter::emitInternedObjectOp(uint32_t index, JSOp op)
{
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_OBJECT);
    MOZ_ASSERT(index < objectList.length);
    return emitIndex32(op, index);
}

bool
BytecodeEmitter::emitObjectOp(ObjectBox* objbox, JSOp op)
{
    return emitInternedObjectOp(objectList.add(objbox), op);
}

bool
BytecodeEmitter::emitObjectPairOp(ObjectBox* objbox1, ObjectBox* objbox2, JSOp op)
{
    uint32_t index = objectList.add(objbox1);
    objectList.add(objbox2);
    return emitInternedObjectOp(index, op);
}

bool
BytecodeEmitter::emitRegExp(uint32_t index)
{
    return emitIndex32(JSOP_REGEXP, index);
}

bool
BytecodeEmitter::emitLocalOp(JSOp op, uint32_t slot)
{
    MOZ_ASSERT(JOF_OPTYPE(op) != JOF_SCOPECOORD);
    MOZ_ASSERT(IsLocalOp(op));

    ptrdiff_t off;
    if (!emitN(op, LOCALNO_LEN, &off))
        return false;

    SET_LOCALNO(code(off), slot);
    return true;
}

bool
BytecodeEmitter::emitUnaliasedVarOp(JSOp op, uint32_t slot, MaybeCheckLexical checkLexical)
{
    MOZ_ASSERT(JOF_OPTYPE(op) != JOF_SCOPECOORD);

    if (IsLocalOp(op)) {
        // Only unaliased locals have stack slots assigned to them. Convert the
        // var index (which includes unaliased and aliased locals) to the stack
        // slot index.
        MOZ_ASSERT(localsToFrameSlots_[slot] <= slot);
        slot = localsToFrameSlots_[slot];

        if (checkLexical) {
            MOZ_ASSERT(op != JSOP_INITLEXICAL);
            if (!emitLocalOp(JSOP_CHECKLEXICAL, slot))
                return false;
        }

        return emitLocalOp(op, slot);
    }

    MOZ_ASSERT(IsArgOp(op));
    ptrdiff_t off;
    if (!emitN(op, ARGNO_LEN, &off))
        return false;

    SET_ARGNO(code(off), slot);
    return true;
}

bool
BytecodeEmitter::emitScopeCoordOp(JSOp op, ScopeCoordinate sc)
{
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_SCOPECOORD);

    unsigned n = SCOPECOORD_HOPS_LEN + SCOPECOORD_SLOT_LEN;
    MOZ_ASSERT(int(n) + 1 /* op */ == CodeSpec[op].length);

    ptrdiff_t off;
    if (!emitN(op, n, &off))
        return false;

    jsbytecode* pc = code(off);
    SET_SCOPECOORD_HOPS(pc, sc.hops());
    pc += SCOPECOORD_HOPS_LEN;
    SET_SCOPECOORD_SLOT(pc, sc.slot());
    pc += SCOPECOORD_SLOT_LEN;
    checkTypeSet(op);
    return true;
}

bool
BytecodeEmitter::emitAliasedVarOp(JSOp op, ScopeCoordinate sc, MaybeCheckLexical checkLexical)
{
    if (checkLexical) {
        MOZ_ASSERT(op != JSOP_INITALIASEDLEXICAL);
        if (!emitScopeCoordOp(JSOP_CHECKALIASEDLEXICAL, sc))
            return false;
    }

    return emitScopeCoordOp(op, sc);
}

bool
BytecodeEmitter::lookupAliasedName(HandleScript script, PropertyName* name, uint32_t* pslot,
                                   ParseNode* pn)
{
    LazyScript::FreeVariable* freeVariables = nullptr;
    uint32_t lexicalBegin = 0;
    uint32_t numFreeVariables = 0;
    if (emitterMode == BytecodeEmitter::LazyFunction) {
        freeVariables = lazyScript->freeVariables();
        lexicalBegin = script->bindings.lexicalBegin();
        numFreeVariables = lazyScript->numFreeVariables();
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

bool
BytecodeEmitter::lookupAliasedNameSlot(PropertyName* name, ScopeCoordinate* sc)
{
    uint32_t slot;
    if (!lookupAliasedName(script, name, &slot))
        return false;

    sc->setSlot(slot);
    return true;
}

static inline MaybeCheckLexical
NodeNeedsCheckLexical(ParseNode* pn)
{
    return pn->isHoistedLexicalUse() ? CheckLexical : DontCheckLexical;
}

static inline JSOp
UnaliasedVarOpToAliasedVarOp(JSOp op)
{
    switch (op) {
      case JSOP_GETARG: case JSOP_GETLOCAL: return JSOP_GETALIASEDVAR;
      case JSOP_SETARG: case JSOP_SETLOCAL: return JSOP_SETALIASEDVAR;
      case JSOP_INITLEXICAL: return JSOP_INITALIASEDLEXICAL;
      default: MOZ_CRASH("unexpected var op");
    }
}

static inline JSOp
CheckSetConstOp(JSOp op, ParseNode* pn)
{
    if (pn->resolve()->isConst()) {
        switch (op) {
          case JSOP_GETLOCAL: case JSOP_GETALIASEDVAR: break;
          case JSOP_INITLEXICAL: case JSOP_INITALIASEDLEXICAL: break;
          case JSOP_SETLOCAL: return JSOP_THROWSETCONST;
          case JSOP_SETALIASEDVAR: return JSOP_THROWSETALIASEDCONST;
          default: MOZ_CRASH("unexpected set var op");
        }
    }
    return op;
}

bool
BytecodeEmitter::emitVarOp(ParseNode* pn, JSOp op)
{
    MOZ_ASSERT(pn->isKind(PNK_FUNCTION) || pn->isKind(PNK_NAME));
    MOZ_ASSERT(!pn->pn_scopecoord.isFree());

    if (pn->isDefn()) {
        // The hop count needs to be computed even for definitions, due to
        // cases like
        //
        // function outer() {
        //   function inner() { x = 42; }
        //   try {
        //   } catch (e) {
        //     G = function () { e = 43; };
        //     var x;
        //   }
        // }
        //
        // The definition for x is emitted when the block scope for the catch
        // is innermost. Moreover, that block scope has aliased bindings, so
        // there is a non-0 hop count.
        if (pn->pn_scopecoord.isHopsUnknown()) {
            BytecodeEmitter* bceOfDef;
            uint32_t hops = computeHops(pn, &bceOfDef);
            MOZ_ASSERT(bceOfDef == this);
            if (!pn->pn_scopecoord.setHops(parser->tokenStream, hops))
                return false;
        }

#ifdef DEBUG
        BytecodeEmitter* bceOfDef;
        uint32_t hops = computeHops(pn, &bceOfDef);
        MOZ_ASSERT(bceOfDef == this);
        MOZ_ASSERT(hops == pn->pn_scopecoord.hops());
#endif

        if (!computeDefinitionIsAliased(this, pn->resolve(), &op))
            return false;
    }

    // Aliased names had their JSOp changed by bindNameToSlot or above.
    if (IsAliasedVarOp(op)) {
        ScopeCoordinate sc;
        sc.setHops(pn->pn_scopecoord.hops());
        sc.setSlot(pn->pn_scopecoord.slot());
        return emitAliasedVarOp(CheckSetConstOp(op, pn), sc, NodeNeedsCheckLexical(pn));
    }

#ifdef DEBUG
    BytecodeEmitter* bceOfDef;
    // Call computeHops to get bceOfDef.
    (void) computeHops(pn, &bceOfDef);
    MOZ_ASSERT(!isAliasedName(bceOfDef, pn));
#endif
    MOZ_ASSERT_IF(pn->isKind(PNK_NAME), IsArgOp(op) || IsLocalOp(op));
    MOZ_ASSERT(pn->isUsed() || pn->isDefn());
    return emitUnaliasedVarOp(CheckSetConstOp(op, pn), pn->pn_scopecoord.slot(),
                              NodeNeedsCheckLexical(pn));
}

static JSOp
GetIncDecInfo(ParseNodeKind kind, bool* post)
{
    MOZ_ASSERT(kind == PNK_POSTINCREMENT || kind == PNK_PREINCREMENT ||
               kind == PNK_POSTDECREMENT || kind == PNK_PREDECREMENT);
    *post = kind == PNK_POSTINCREMENT || kind == PNK_POSTDECREMENT;
    return (kind == PNK_POSTINCREMENT || kind == PNK_PREINCREMENT) ? JSOP_ADD : JSOP_SUB;
}

bool
BytecodeEmitter::emitVarIncDec(ParseNode* pn)
{
    JSOp op = pn->pn_kid->getOp();
    MOZ_ASSERT(IsArgOp(op) || IsLocalOp(op) || IsAliasedVarOp(op));
    MOZ_ASSERT(pn->pn_kid->isKind(PNK_NAME));
    MOZ_ASSERT(!pn->pn_kid->pn_scopecoord.isFree());

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

    if (!emitVarOp(pn->pn_kid, getOp))                       // V
        return false;
    if (!emit1(JSOP_POS))                                    // N
        return false;
    if (post && !emit1(JSOP_DUP))                            // N? N
        return false;
    if (!emit1(JSOP_ONE))                                    // N? N 1
        return false;
    if (!emit1(binop))                                       // N? N+1
        return false;
    if (!emitVarOp(pn->pn_kid, setOp))                       // N? N+1
        return false;
    if (post && !emit1(JSOP_POP))                            // RESULT
        return false;

    return true;
}

bool
BytecodeEmitter::atBodyLevel() const
{
    // 'eval' and non-syntactic scripts are always under an invisible lexical
    // scope, but since it is not syntactic, it should still be considered at
    // body level.
    if (sc->staticScope()->is<StaticEvalObject>()) {
        bool bl = !innermostStmt()->enclosing;
        MOZ_ASSERT_IF(bl, innermostStmt()->type == StmtType::BLOCK);
        MOZ_ASSERT_IF(bl, innermostStmt()->staticScope
                                         ->as<StaticBlockObject>()
                                         .enclosingStaticScope() == sc->staticScope());
        return bl;
    }
    return !innermostStmt() || sc->isModuleBox();
}

uint32_t
BytecodeEmitter::computeHops(ParseNode* pn, BytecodeEmitter** bceOfDefOut)
{
    Definition* dn = pn->resolve();
    MOZ_ASSERT(dn->isDefn());
    MOZ_ASSERT(!dn->isPlaceholder());
    MOZ_ASSERT(dn->isBound());

    uint32_t hops = 0;
    BytecodeEmitter* bceOfDef = this;
    StaticScopeIter<NoGC> ssi(innermostStaticScope());
    JSObject* defScope = blockScopeOfDef(dn);
    while (ssi.staticScope() != defScope) {
        if (ssi.hasSyntacticDynamicScopeObject())
            hops++;
        if (ssi.type() == StaticScopeIter<NoGC>::Function) {
            MOZ_ASSERT(dn->isClosed());
            bceOfDef = bceOfDef->parent;
        }
        ssi++;
    }

    *bceOfDefOut = bceOfDef;
    return hops;
}

bool
BytecodeEmitter::isAliasedName(BytecodeEmitter* bceOfDef, ParseNode* pn)
{
    // If the definition is in another function, it's definitely aliased.
    if (bceOfDef != this)
        return true;

    Definition* dn = pn->resolve();
    switch (dn->kind()) {
      case Definition::LET:
      case Definition::CONSTANT:
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
        return script->formalIsAliased(pn->pn_scopecoord.slot());
      case Definition::VAR:
        MOZ_ASSERT_IF(sc->allLocalsAliased(), script->localIsAliased(pn->pn_scopecoord.slot()));
        return script->localIsAliased(pn->pn_scopecoord.slot());
      case Definition::PLACEHOLDER:
      case Definition::NAMED_LAMBDA:
      case Definition::MISSING:
      case Definition::IMPORT:
        MOZ_CRASH("unexpected dn->kind");
    }
    return false;
}

bool
BytecodeEmitter::computeDefinitionIsAliased(BytecodeEmitter* bceOfDef, Definition* dn, JSOp* op)
{
    if (dn->isKnownAliased()) {
        *op = UnaliasedVarOpToAliasedVarOp(*op);
    } else if (isAliasedName(bceOfDef, dn)) {
        // Translate the frame slot to a slot on the dynamic scope
        // object. Aliased block bindings do not need adjusting; see
        // computeAliasedSlots.
        uint32_t slot = dn->pn_scopecoord.slot();
        if (blockScopeOfDef(dn)->is<JSFunction>() ||
            blockScopeOfDef(dn)->is<ModuleObject>())
        {
            MOZ_ASSERT(IsArgOp(*op) || slot < bceOfDef->script->bindings.numBodyLevelLocals());
            MOZ_ALWAYS_TRUE(bceOfDef->lookupAliasedName(bceOfDef->script, dn->name(), &slot));
        }
        if (!dn->pn_scopecoord.setSlot(parser->tokenStream, slot))
            return false;

        *op = UnaliasedVarOpToAliasedVarOp(*op);

        // Mark the definition as having already computed alias information.
        dn->pn_dflags |= PND_KNOWNALIASED;
    }

    return true;
}

JSOp
BytecodeEmitter::strictifySetNameOp(JSOp op)
{
    switch (op) {
      case JSOP_SETNAME:
        if (sc->strict())
            op = JSOP_STRICTSETNAME;
        break;
      case JSOP_SETGNAME:
        if (sc->strict())
            op = JSOP_STRICTSETGNAME;
        break;
        default:;
    }
    return op;
}

void
BytecodeEmitter::strictifySetNameNode(ParseNode* pn)
{
    pn->setOp(strictifySetNameOp(pn->getOp()));
}

/*
 * Try to convert a *NAME op with a free name to a more specialized GNAME,
 * INTRINSIC or ALIASEDVAR op, which optimize accesses on that name.
 * Return true if a conversion was made.
 */
bool
BytecodeEmitter::tryConvertFreeName(ParseNode* pn)
{
    /*
     * In self-hosting mode, JSOP_*NAME is unconditionally converted to
     * JSOP_*INTRINSIC. This causes lookups to be redirected to the special
     * intrinsics holder in the global object, into which any missing values are
     * cloned lazily upon first access.
     */
    if (emitterMode == BytecodeEmitter::SelfHosting) {
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
    if (emitterMode == BytecodeEmitter::LazyFunction) {
        // The only statements within a lazy function which can push lexical
        // scopes are try/catch blocks. Use generic ops in this case.
        for (StmtInfoBCE* stmt = innermostStmt(); stmt; stmt = stmt->enclosing) {
            if (stmt->type == StmtType::CATCH)
                return true;
        }

        // Walk the static scope chain and look for an aliased binding with
        // the name pn->pn_atom.
        uint32_t hops = 0;
        Maybe<uint32_t> slot;
        FunctionBox* funbox = sc->asFunctionBox();
        PropertyName* name = pn->pn_atom->asPropertyName();
        for (StaticScopeIter<NoGC> ssi(funbox->staticScope()); !ssi.done(); ssi++) {
            // Don't optimize names through non-global eval. For global eval
            // we can use GNAME ops.
            if (ssi.type() == StaticScopeIter<NoGC>::Eval) {
                if (ssi.eval().isNonGlobal())
                    return false;
                MOZ_ASSERT(!slot.isSome());
                break;
            }

            if (!ssi.hasSyntacticDynamicScopeObject())
                continue;

            // Look up for name in function and block scopes.
            if (ssi.type() == StaticScopeIter<NoGC>::Function) {
                RootedScript funScript(cx, ssi.funScript());
                if (funScript->funHasExtensibleScope() || ssi.fun().atom() == pn->pn_atom)
                    return false;

                // Skip the current function, since we're trying to convert a
                // free name.
                if (script != funScript) {
                    uint32_t slot_;
                    if (lookupAliasedName(funScript, name, &slot_, pn)) {
                        slot = Some(slot_);
                        break;
                    }
                }
            } else if (ssi.type() == StaticScopeIter<NoGC>::Module) {
                RootedScript moduleScript(cx, ssi.moduleScript());
                uint32_t slot_;
                if (lookupAliasedName(moduleScript, name, &slot_, pn)) {
                    slot = Some(slot_);
                    break;
                }

                // Convert module import accesses to use JSOP_GETIMPORT.
                RootedModuleEnvironmentObject env(cx, ssi.module().environment());
                RootedPropertyName propName(cx, name);
                MOZ_ASSERT(env);
                if (env->hasImportBinding(propName)) {
                    if (pn->getOp() == JSOP_GETNAME) {
                        pn->setOp(JSOP_GETIMPORT);
                        return true;
                    }
                    return false;
                }
            } else if (ssi.type() == StaticScopeIter<NoGC>::Block) {
                RootedShape shape(cx, ssi.block().lookupAliasedName(name));
                if (shape) {
                    // Don't optimize setting a 'const' binding. Let the slow
                    // path do the error checking.
                    if (!shape->writable() && pn->getOp() == JSOP_SETNAME)
                        return false;
                    slot = Some(shape->slot());
                    pn->pn_dflags |= PND_LEXICAL;
                    break;
                }
            } else {
                MOZ_ASSERT(ssi.type() != StaticScopeIter<NoGC>::With);
            }

            hops++;
        }

        // If we found a scope binding name, convert the name op to an aliased
        // var op.
        if (slot.isSome()) {
            JSOp op;
            switch (pn->getOp()) {
              case JSOP_GETNAME: op = JSOP_GETALIASEDVAR; break;
              case JSOP_SETNAME: op = JSOP_SETALIASEDVAR; break;
              default: return false;
            }
            pn->setOp(op);
            MOZ_ALWAYS_TRUE(pn->pn_scopecoord.set(parser->tokenStream, hops, *slot));
            return true;
        }
    }

    // Unbound names aren't recognizable global-property references if the
    // script is inside a non-global eval call.
    if (insideNonGlobalEval)
        return false;

    // If we are inside a module then unbound names in a function may refer to
    // imports, so we can't use GNAME ops here.
    if (insideModule)
        return false;

    // Skip trying to use GNAME ops if we know our script has a non-syntactic
    // scope, since they'll just get treated as NAME ops anyway.
    if (script->hasNonSyntacticScope())
        return false;

    // Deoptimized names also aren't necessarily globals.
    if (pn->isDeoptimized())
        return false;

    if (sc->isFunctionBox()) {
        // Unbound names in function code may not be globals if new locals can
        // be added to this function (or an enclosing one) to alias a global
        // reference.
        FunctionBox* funbox = sc->asFunctionBox();
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
    //
    // Though actually, we don't even need an inner eval.  We could just as well
    // have a lambda inside that outer strict mode eval and it would run into
    // the same issue.
    if (insideEval && sc->strict())
        return false;

    JSOp op;
    switch (pn->getOp()) {
      case JSOP_GETNAME: op = JSOP_GETGNAME; break;
      case JSOP_SETNAME: op = strictifySetNameOp(JSOP_SETGNAME); break;
      default: MOZ_CRASH("gname");
    }
    pn->setOp(op);
    MOZ_ASSERT_IF(op == JSOP_INITGLEXICAL,
                  IsStaticGlobalLexicalScope(blockScopeOfDef(pn->resolve())));
    return true;
}

/*
 * BindNameToSlotHelper attempts to optimize name gets and sets to stack slot
 * loads and stores, given the compile-time information in |this| and a PNK_NAME
 * node pn.  It returns false on error, true on success.
 *
 * The caller can test pn->pn_scopecoord.isFree() to tell whether optimization
 * occurred, in which case bindNameToSlotHelper also updated pn->pn_op.  If
 * pn->pn_cookie.isFree() is still true on return, pn->pn_op still may have
 * been optimized, e.g., from JSOP_GETNAME to JSOP_CALLEE.  Whether or not
 * pn->pn_op was modified, if this function finds an argument or local variable
 * name, PND_CONST will be set in pn_dflags for read-only properties after a
 * successful return.
 *
 * NB: if you add more opcodes specialized from JSOP_GETNAME, etc., don't forget
 * to update the special cases in EmitFor (for-in) and emitAssignment (= and
 * op=, e.g. +=).
 */
bool
BytecodeEmitter::bindNameToSlotHelper(ParseNode* pn)
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
        MOZ_ASSERT(pn->pn_scopecoord.isFree());
        dn = pn->pn_lexdef;
        MOZ_ASSERT(dn->isDefn());
        pn->pn_dflags |= (dn->pn_dflags & PND_CONST);
    } else if (pn->isDefn()) {
        dn = (Definition*) pn;
    } else {
        return true;
    }

    if (dn->pn_scopecoord.isFree()) {
        if (evalCaller) {
            MOZ_ASSERT(script->treatAsRunOnce() || sc->isFunctionBox());

            /*
             * Don't generate upvars on the left side of a for loop. See
             * bug 470758.
             */
            if (emittingForInit)
                return true;

            /*
             * If this is an eval in the global scope, then unbound variables
             * must be globals, so try to use GNAME ops.
             */
            if (!evalCaller->functionOrCallerFunction() && tryConvertFreeName(pn)) {
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
        if (!tryConvertFreeName(pn))
            return true;

        pn->pn_dflags |= PND_BOUND;
        return true;
    }

    /*
     * At this point, we are only dealing with uses that have already been
     * bound to definitions via pn_lexdef. The rest of this routine converts
     * the parse node of the use from its initial JSOP_*NAME* op to a
     * LOCAL/ARG op. This requires setting the node's pn_scopecoord with a
     * pair (hops, slot) where 'hops' is the number of dynamic scopes between
     * the use and the def and 'slot' is the index to emit as the immediate of
     * the ARG/LOCAL op. For example, in this code:
     *
     *   function(a,b,x) { return x }
     *   function(y) { function() { return y } }
     *
     * x will get (hops = 0, slot = 2) and y will get (hops = 1, slot = 0).
     */
    MOZ_ASSERT(!pn->isDefn());
    MOZ_ASSERT(pn->isUsed());
    MOZ_ASSERT(pn->pn_lexdef);
    MOZ_ASSERT(pn->pn_scopecoord.isFree());

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
      case Definition::CONSTANT:
      case Definition::LET:
        switch (op) {
          case JSOP_GETNAME:
            op = JSOP_GETLOCAL; break;
          case JSOP_SETNAME:
          case JSOP_STRICTSETNAME:
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
        JSFunction* fun = sc->asFunctionBox()->function();
        if (blockScopeOfDef(dn) != fun)
            return true;

        MOZ_ASSERT(fun->isLambda());
        MOZ_ASSERT(pn->pn_atom == fun->atom());

        /*
         * Leave pn->isOp(JSOP_GETNAME) if this->fun needs a CallObject to
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
         * needsCallObject, ensuring that the function name is represented in
         * the scope chain so that assignment will throw a TypeError.
         */
        if (!sc->asFunctionBox()->needsCallObject()) {
            op = JSOP_CALLEE;
            pn->pn_dflags |= PND_CONST;
        }

        pn->setOp(op);
        pn->pn_dflags |= PND_BOUND;
        return true;
      }

      case Definition::PLACEHOLDER:
        return true;

      case Definition::IMPORT:
        if (op == JSOP_GETNAME)
            pn->setOp(JSOP_GETIMPORT);
        return true;

      case Definition::MISSING:
        MOZ_CRASH("unexpected definition kind");
    }

    // The hop count is the number of dynamic scopes during execution that must
    // be skipped to access the binding.
    BytecodeEmitter* bceOfDef;
    uint32_t slot = dn->pn_scopecoord.slot();
    uint32_t hops = computeHops(pn, &bceOfDef);

    /*
     * Explicitly disallow accessing var/let bindings in global scope from
     * nested functions. The reason for this limitation is that, since the
     * global script is not included in the static scope chain (1. because it
     * has no object to stand in the static scope chain, 2. to minimize memory
     * bloat where a single live function keeps its whole global script
     * alive.), ScopeCoordinateToTypeSet is not able to find the var/let's
     * associated TypeSet.
     */
    if (bceOfDef != this && bceOfDef->sc->isGlobalContext())
        return true;

    if (!pn->pn_scopecoord.set(parser->tokenStream, hops, slot))
        return false;

    if (!computeDefinitionIsAliased(bceOfDef, dn, &op))
        return false;

    // Re-set the slot on if it is aliased, since the slot would have been
    // translated on dn.
    if (IsAliasedVarOp(op)) {
        MOZ_ASSERT(dn->isKnownAliased());
        pn->pn_scopecoord.setSlot(parser->tokenStream, dn->pn_scopecoord.slot());
    }

    MOZ_ASSERT(!pn->isOp(op));
    pn->setOp(op);
    pn->pn_dflags |= PND_BOUND;
    return true;
}

/*
 * Attempts to bind the name, then checks that no dynamic scope lookup ops are
 * emitted in self-hosting mode. NAME ops do lookups off current scope chain,
 * and we do not want to allow self-hosted code to use the dynamic scope.
 */
bool
BytecodeEmitter::bindNameToSlot(ParseNode* pn)
{
    if (!bindNameToSlotHelper(pn))
        return false;

    strictifySetNameNode(pn);

    if (emitterMode == BytecodeEmitter::SelfHosting && !pn->isBound()) {
        reportError(pn, JSMSG_SELFHOSTED_UNBOUND_NAME);
        return false;
    }

    return true;
}

bool
BytecodeEmitter::checkSideEffects(ParseNode* pn, bool* answer)
{
    JS_CHECK_RECURSION(cx, return false);

 restart:

    switch (pn->getKind()) {
      // Trivial cases with no side effects.
      case PNK_NOP:
      case PNK_STRING:
      case PNK_TEMPLATE_STRING:
      case PNK_REGEXP:
      case PNK_TRUE:
      case PNK_FALSE:
      case PNK_NULL:
      case PNK_ELISION:
      case PNK_GENERATOR:
      case PNK_NUMBER:
      case PNK_OBJECT_PROPERTY_NAME:
        MOZ_ASSERT(pn->isArity(PN_NULLARY));
        *answer = false;
        return true;

      // |this| can throw in derived class constructors, including nested arrow
      // functions or eval.
      case PNK_THIS:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = sc->needsThisTDZChecks();
        return true;

      // Trivial binary nodes with more token pos holders.
      case PNK_NEWTARGET:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        MOZ_ASSERT(pn->pn_left->isKind(PNK_POSHOLDER));
        MOZ_ASSERT(pn->pn_right->isKind(PNK_POSHOLDER));
        *answer = false;
        return true;

      case PNK_BREAK:
      case PNK_CONTINUE:
      case PNK_DEBUGGER:
        MOZ_ASSERT(pn->isArity(PN_NULLARY));
        *answer = true;
        return true;

      // Watch out for getters!
      case PNK_DOT:
        MOZ_ASSERT(pn->isArity(PN_NAME));
        *answer = true;
        return true;

      // Unary cases with side effects only if the child has them.
      case PNK_TYPEOFEXPR:
      case PNK_VOID:
      case PNK_NOT:
      case PNK_COMPUTED_NAME:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        return checkSideEffects(pn->pn_kid, answer);

      // Looking up or evaluating the associated name could throw.
      case PNK_TYPEOFNAME:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // These unary cases have side effects on the enclosing object/array,
      // sure.  But that's not the question this function answers: it's
      // whether the operation may have a side effect on something *other* than
      // the result of the overall operation in which it's embedded.  The
      // answer to that is no, for an object literal having a mutated prototype
      // and an array comprehension containing no other effectful operations
      // only produce a value, without affecting anything else.
      case PNK_MUTATEPROTO:
      case PNK_ARRAYPUSH:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        return checkSideEffects(pn->pn_kid, answer);

      // Unary cases with obvious side effects.
      case PNK_PREINCREMENT:
      case PNK_POSTINCREMENT:
      case PNK_PREDECREMENT:
      case PNK_POSTDECREMENT:
      case PNK_THROW:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // These might invoke valueOf/toString, even with a subexpression without
      // side effects!  Consider |+{ valueOf: null, toString: null }|.
      case PNK_BITNOT:
      case PNK_POS:
      case PNK_NEG:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // This invokes the (user-controllable) iterator protocol.
      case PNK_SPREAD:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      case PNK_YIELD_STAR:
      case PNK_YIELD:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      // Deletion generally has side effects, even if isolated cases have none.
      case PNK_DELETENAME:
      case PNK_DELETEPROP:
      case PNK_DELETEELEM:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // Deletion of a non-Reference expression has side effects only through
      // evaluating the expression.
      case PNK_DELETEEXPR: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        ParseNode* expr = pn->pn_kid;
        return checkSideEffects(expr, answer);
      }

      case PNK_SEMI:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        if (ParseNode* expr = pn->pn_kid)
            return checkSideEffects(expr, answer);
        *answer = false;
        return true;

      // Binary cases with obvious side effects.
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
      case PNK_POWASSIGN:
      case PNK_SETTHIS:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      case PNK_STATEMENTLIST:
      case PNK_CATCHLIST:
      // Strict equality operations and logical operators are well-behaved and
      // perform no conversions.
      case PNK_OR:
      case PNK_AND:
      case PNK_STRICTEQ:
      case PNK_STRICTNE:
      // Any subexpression of a comma expression could be effectful.
      case PNK_COMMA:
        MOZ_ASSERT(pn->pn_count > 0);
      // Subcomponents of a literal may be effectful.
      case PNK_ARRAY:
      case PNK_OBJECT:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        for (ParseNode* item = pn->pn_head; item; item = item->pn_next) {
            if (!checkSideEffects(item, answer))
                return false;
            if (*answer)
                return true;
        }
        return true;

      // Most other binary operations (parsed as lists in SpiderMonkey) may
      // perform conversions triggering side effects.  Math operations perform
      // ToNumber and may fail invoking invalid user-defined toString/valueOf:
      // |5 < { toString: null }|.  |instanceof| throws if provided a
      // non-object constructor: |null instanceof null|.  |in| throws if given
      // a non-object RHS: |5 in null|.
      case PNK_BITOR:
      case PNK_BITXOR:
      case PNK_BITAND:
      case PNK_EQ:
      case PNK_NE:
      case PNK_LT:
      case PNK_LE:
      case PNK_GT:
      case PNK_GE:
      case PNK_INSTANCEOF:
      case PNK_IN:
      case PNK_LSH:
      case PNK_RSH:
      case PNK_URSH:
      case PNK_ADD:
      case PNK_SUB:
      case PNK_STAR:
      case PNK_DIV:
      case PNK_MOD:
      case PNK_POW:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_count >= 2);
        *answer = true;
        return true;

      case PNK_COLON:
      case PNK_CASE:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (!checkSideEffects(pn->pn_left, answer))
            return false;
        if (*answer)
            return true;
        return checkSideEffects(pn->pn_right, answer);

      // More getters.
      case PNK_ELEM:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      // These affect visible names in this code, or in other code.
      case PNK_IMPORT:
      case PNK_EXPORT_FROM:
      case PNK_EXPORT_DEFAULT:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      // Likewise.
      case PNK_EXPORT:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // Every part of a loop might be effect-free, but looping infinitely *is*
      // an effect.  (Language lawyer trivia: C++ says threads can be assumed
      // to exit or have side effects, C++14 [intro.multithread]p27, so a C++
      // implementation's equivalent of the below could set |*answer = false;|
      // if all loop sub-nodes set |*answer = false|!)
      case PNK_DOWHILE:
      case PNK_WHILE:
      case PNK_FOR:
      case PNK_COMPREHENSIONFOR:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      // Declarations affect the name set of the relevant scope.
      case PNK_VAR:
      case PNK_CONST:
      case PNK_LET:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        *answer = true;
        return true;

      case PNK_IF:
      case PNK_CONDITIONAL:
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        if (!checkSideEffects(pn->pn_kid1, answer))
            return false;
        if (*answer)
            return true;
        if (!checkSideEffects(pn->pn_kid2, answer))
            return false;
        if (*answer)
            return true;
        if ((pn = pn->pn_kid3))
            goto restart;
        return true;

      // Function calls can invoke non-local code.
      case PNK_NEW:
      case PNK_CALL:
      case PNK_TAGGED_TEMPLATE:
      case PNK_SUPERCALL:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        *answer = true;
        return true;

      // Classes typically introduce names.  Even if no name is introduced,
      // the heritage and/or class body (through computed property names)
      // usually have effects.
      case PNK_CLASS:
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        *answer = true;
        return true;

      // |with| calls |ToObject| on its expression and so throws if that value
      // is null/undefined.
      case PNK_WITH:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      case PNK_RETURN:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      case PNK_NAME:
        MOZ_ASSERT(pn->isArity(PN_NAME));
        *answer = true;
        return true;

      // Shorthands could trigger getters: the |x| in the object literal in
      // |with ({ get x() { throw 42; } }) ({ x });|, for example, triggers
      // one.  (Of course, it isn't necessary to use |with| for a shorthand to
      // trigger a getter.)
      case PNK_SHORTHAND:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      case PNK_FUNCTION:
        MOZ_ASSERT(pn->isArity(PN_CODE));
        /*
         * A named function, contrary to ES3, is no longer effectful, because
         * we bind its name lexically (using JSOP_CALLEE) instead of creating
         * an Object instance and binding a readonly, permanent property in it
         * (the object and binding can be detected and hijacked or captured).
         * This is a bug fix to ES3; it is fixed in ES3.1 drafts.
         */
        *answer = false;
        return true;

      case PNK_MODULE:
        *answer = false;
        return true;

      // Generator expressions have no side effects on their own.
      case PNK_GENEXP:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        *answer = false;
        return true;

      case PNK_TRY:
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        if (!checkSideEffects(pn->pn_kid1, answer))
            return false;
        if (*answer)
            return true;
        if (ParseNode* catchList = pn->pn_kid2) {
            MOZ_ASSERT(catchList->isKind(PNK_CATCHLIST));
            if (!checkSideEffects(catchList, answer))
                return false;
            if (*answer)
                return true;
        }
        if (ParseNode* finallyBlock = pn->pn_kid3) {
            if (!checkSideEffects(finallyBlock, answer))
                return false;
        }
        return true;

      case PNK_CATCH:
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        if (!checkSideEffects(pn->pn_kid1, answer))
            return false;
        if (*answer)
            return true;
        if (ParseNode* cond = pn->pn_kid2) {
            if (!checkSideEffects(cond, answer))
                return false;
            if (*answer)
                return true;
        }
        return checkSideEffects(pn->pn_kid3, answer);

      case PNK_SWITCH:
      case PNK_LETBLOCK:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (!checkSideEffects(pn->pn_left, answer))
            return false;
        return *answer || checkSideEffects(pn->pn_right, answer);

      case PNK_LABEL:
      case PNK_LEXICALSCOPE:
        MOZ_ASSERT(pn->isArity(PN_NAME));
        return checkSideEffects(pn->expr(), answer);

      // We could methodically check every interpolated expression, but it's
      // probably not worth the trouble.  Treat template strings as effect-free
      // only if they don't contain any substitutions.
      case PNK_TEMPLATE_STRING_LIST:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_count > 0);
        MOZ_ASSERT((pn->pn_count % 2) == 1,
                   "template strings must alternate template and substitution "
                   "parts");
        *answer = pn->pn_count > 1;
        return true;

      case PNK_ARRAYCOMP:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_count == 1);
        return checkSideEffects(pn->pn_head, answer);

      case PNK_ARGSBODY:
        *answer = true;
        return true;

      case PNK_FORIN:           // by PNK_FOR/PNK_COMPREHENSIONFOR
      case PNK_FOROF:           // by PNK_FOR/PNK_COMPREHENSIONFOR
      case PNK_FORHEAD:         // by PNK_FOR/PNK_COMPREHENSIONFOR
      case PNK_CLASSMETHOD:     // by PNK_CLASS
      case PNK_CLASSNAMES:      // by PNK_CLASS
      case PNK_CLASSMETHODLIST: // by PNK_CLASS
      case PNK_IMPORT_SPEC_LIST: // by PNK_IMPORT
      case PNK_IMPORT_SPEC:      // by PNK_IMPORT
      case PNK_EXPORT_BATCH_SPEC:// by PNK_EXPORT
      case PNK_EXPORT_SPEC_LIST: // by PNK_EXPORT
      case PNK_EXPORT_SPEC:      // by PNK_EXPORT
      case PNK_CALLSITEOBJ:      // by PNK_TAGGED_TEMPLATE
      case PNK_POSHOLDER:        // by PNK_NEWTARGET
      case PNK_SUPERBASE:        // by PNK_ELEM and others
        MOZ_CRASH("handled by parent nodes");

      case PNK_LIMIT: // invalid sentinel value
        MOZ_CRASH("invalid node kind");
    }

    MOZ_CRASH("invalid, unenumerated ParseNodeKind value encountered in "
              "BytecodeEmitter::checkSideEffects");
}

bool
BytecodeEmitter::isInLoop()
{
    for (StmtInfoBCE* stmt = innermostStmt(); stmt; stmt = stmt->enclosing) {
        if (stmt->isLoop())
            return true;
    }
    return false;
}

bool
BytecodeEmitter::checkSingletonContext()
{
    if (!script->treatAsRunOnce() || sc->isFunctionBox() || isInLoop())
        return false;
    hasSingletons = true;
    return true;
}

bool
BytecodeEmitter::checkRunOnceContext()
{
    return checkSingletonContext() || (!isInLoop() && isRunOnceLambda());
}

bool
BytecodeEmitter::needsImplicitThis()
{
    // Short-circuit if there is an enclosing 'with' static scope.
    if (sc->inWith())
        return true;

    // Otherwise walk the statement stack.
    for (StmtInfoBCE* stmt = innermostStmt(); stmt; stmt = stmt->enclosing) {
        if (stmt->type == StmtType::WITH)
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
    bool result = tokenStream()->reportStrictModeErrorNumberVA(pos.begin, sc->strict(),
                                                               errorNumber, args);
    va_end(args);
    return result;
}

bool
BytecodeEmitter::emitNewInit(JSProtoKey key)
{
    const size_t len = 1 + UINT32_INDEX_LEN;
    ptrdiff_t offset;
    if (!emitCheck(len, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = JSOP_NEWINIT;
    code[1] = jsbytecode(key);
    code[2] = 0;
    code[3] = 0;
    code[4] = 0;
    updateDepth(offset);
    checkTypeSet(JSOP_NEWINIT);
    return true;
}

bool
BytecodeEmitter::iteratorResultShape(unsigned* shape)
{
    // No need to do any guessing for the object kind, since we know exactly how
    // many properties we plan to have.
    gc::AllocKind kind = gc::GetGCObjectKind(2);
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx, kind, TenuredObject));
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

    ObjectBox* objbox = parser->newObjectBox(obj);
    if (!objbox)
        return false;

    *shape = objectList.add(objbox);

    return true;
}

bool
BytecodeEmitter::emitPrepareIteratorResult()
{
    unsigned shape;
    if (!iteratorResultShape(&shape))
        return false;
    return emitIndex32(JSOP_NEWOBJECT, shape);
}

bool
BytecodeEmitter::emitFinishIteratorResult(bool done)
{
    jsatomid value_id;
    if (!makeAtomIndex(cx->names().value, &value_id))
        return false;
    jsatomid done_id;
    if (!makeAtomIndex(cx->names().done, &done_id))
        return false;

    if (!emitIndex32(JSOP_INITPROP, value_id))
        return false;
    if (!emit1(done ? JSOP_TRUE : JSOP_FALSE))
        return false;
    if (!emitIndex32(JSOP_INITPROP, done_id))
        return false;
    return true;
}

bool
BytecodeEmitter::emitNameOp(ParseNode* pn, bool callContext)
{
    if (!bindNameToSlot(pn))
        return false;

    JSOp op = pn->getOp();

    if (op == JSOP_CALLEE) {
        if (!emit1(op))
            return false;
    } else {
        if (!pn->pn_scopecoord.isFree()) {
            MOZ_ASSERT(JOF_OPTYPE(op) != JOF_ATOM);
            if (!emitVarOp(pn, op))
                return false;
        } else {
            if (!emitAtomOp(pn, op))
                return false;
        }
    }

    /* Need to provide |this| value for call */
    if (callContext) {
        if (op == JSOP_GETNAME || op == JSOP_GETGNAME) {
            JSOp thisOp = needsImplicitThis() ? JSOP_IMPLICITTHIS : JSOP_GIMPLICITTHIS;
            if (!emitAtomOp(pn, thisOp))
                return false;
        } else {
            if (!emit1(JSOP_UNDEFINED))
                return false;
        }
    }

    return true;
}

bool
BytecodeEmitter::emitPropLHS(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_DOT));
    MOZ_ASSERT(!pn->as<PropertyAccess>().isSuper());

    ParseNode* pn2 = pn->maybeExpr();

    /*
     * If the object operand is also a dotted property reference, reverse the
     * list linked via pn_expr temporarily so we can iterate over it from the
     * bottom up (reversing again as we go), to avoid excessive recursion.
     */
    if (pn2->isKind(PNK_DOT) && !pn2->as<PropertyAccess>().isSuper()) {
        ParseNode* pndot = pn2;
        ParseNode* pnup = nullptr;
        ParseNode* pndown;
        for (;;) {
            /* Reverse pndot->pn_expr to point up, not down. */
            MOZ_ASSERT(!pndot->isUsed());
            pndown = pndot->pn_expr;
            pndot->pn_expr = pnup;
            if (!pndown->isKind(PNK_DOT) || pndown->as<PropertyAccess>().isSuper())
                break;
            pnup = pndot;
            pndot = pndown;
        }

        /* pndown is a primary expression, not a dotted property reference. */
        if (!emitTree(pndown))
            return false;

        do {
            /* Walk back up the list, emitting annotated name ops. */
            if (!emitAtomOp(pndot, JSOP_GETPROP))
                return false;

            /* Reverse the pn_expr link again. */
            pnup = pndot->pn_expr;
            pndot->pn_expr = pndown;
            pndown = pndot;
        } while ((pndot = pnup) != nullptr);
        return true;
    }

    // The non-optimized case.
    return emitTree(pn2);
}

bool
BytecodeEmitter::emitSuperPropLHS(ParseNode* superBase, bool isCall)
{
    if (!emitGetThisForSuperBase(superBase))
        return false;
    if (isCall && !emit1(JSOP_DUP))
        return false;
    if (!emit1(JSOP_SUPERBASE))
        return false;
    return true;
}

bool
BytecodeEmitter::emitPropOp(ParseNode* pn, JSOp op)
{
    MOZ_ASSERT(pn->isArity(PN_NAME));

    if (!emitPropLHS(pn))
        return false;

    if (op == JSOP_CALLPROP && !emit1(JSOP_DUP))
        return false;

    if (!emitAtomOp(pn, op))
        return false;

    if (op == JSOP_CALLPROP && !emit1(JSOP_SWAP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitSuperPropOp(ParseNode* pn, JSOp op, bool isCall)
{
    ParseNode* base = &pn->as<PropertyAccess>().expression();
    if (!emitSuperPropLHS(base, isCall))
        return false;

    if (!emitAtomOp(pn, op))
        return false;

    if (isCall && !emit1(JSOP_SWAP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitPropIncDec(ParseNode* pn)
{
    MOZ_ASSERT(pn->pn_kid->isKind(PNK_DOT));

    bool post;
    bool isSuper = pn->pn_kid->as<PropertyAccess>().isSuper();
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    if (isSuper) {
        ParseNode* base = &pn->pn_kid->as<PropertyAccess>().expression();
        if (!emitSuperPropLHS(base))                // THIS OBJ
            return false;
        if (!emit1(JSOP_DUP2))                      // THIS OBJ THIS OBJ
            return false;
    } else {
        if (!emitPropLHS(pn->pn_kid))               // OBJ
            return false;
        if (!emit1(JSOP_DUP))                       // OBJ OBJ
            return false;
    }
    if (!emitAtomOp(pn->pn_kid, isSuper? JSOP_GETPROP_SUPER : JSOP_GETPROP)) // OBJ V
        return false;
    if (!emit1(JSOP_POS))                           // OBJ N
        return false;
    if (post && !emit1(JSOP_DUP))                   // OBJ N? N
        return false;
    if (!emit1(JSOP_ONE))                           // OBJ N? N 1
        return false;
    if (!emit1(binop))                              // OBJ N? N+1
        return false;

    if (post) {
        if (!emit2(JSOP_PICK, 2 + isSuper))        // N? N+1 OBJ
            return false;
        if (!emit1(JSOP_SWAP))                     // N? OBJ N+1
            return false;
        if (isSuper) {
            if (!emit2(JSOP_PICK, 3))              // N THIS N+1 OBJ
                return false;
            if (!emit1(JSOP_SWAP))                 // N THIS OBJ N+1
                return false;
        }
    }

    JSOp setOp = isSuper ? sc->strict() ? JSOP_STRICTSETPROP_SUPER : JSOP_SETPROP_SUPER
                         : sc->strict() ? JSOP_STRICTSETPROP : JSOP_SETPROP;
    if (!emitAtomOp(pn->pn_kid, setOp))             // N? N+1
        return false;
    if (post && !emit1(JSOP_POP))                   // RESULT
        return false;

    return true;
}

bool
BytecodeEmitter::emitNameIncDec(ParseNode* pn)
{
    const JSCodeSpec* cs = &CodeSpec[pn->pn_kid->getOp()];

    bool global = (cs->format & JOF_GNAME);
    bool post;
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    if (!emitAtomOp(pn->pn_kid, global ? JSOP_BINDGNAME : JSOP_BINDNAME))  // OBJ
        return false;
    if (!emitAtomOp(pn->pn_kid, global ? JSOP_GETGNAME : JSOP_GETNAME))    // OBJ V
        return false;
    if (!emit1(JSOP_POS))                      // OBJ N
        return false;
    if (post && !emit1(JSOP_DUP))              // OBJ N? N
        return false;
    if (!emit1(JSOP_ONE))                      // OBJ N? N 1
        return false;
    if (!emit1(binop))                         // OBJ N? N+1
        return false;

    if (post) {
        if (!emit2(JSOP_PICK, 2))              // N? N+1 OBJ
            return false;
        if (!emit1(JSOP_SWAP))                 // N? OBJ N+1
            return false;
    }

    JSOp setOp = strictifySetNameOp(global ? JSOP_SETGNAME : JSOP_SETNAME);
    if (!emitAtomOp(pn->pn_kid, setOp))        // N? N+1
        return false;
    if (post && !emit1(JSOP_POP))              // RESULT
        return false;

    return true;
}

bool
BytecodeEmitter::emitElemOperands(ParseNode* pn, EmitElemOption opts)
{
    MOZ_ASSERT(pn->isArity(PN_BINARY));

    if (!emitTree(pn->pn_left))
        return false;

    if (opts == EmitElemOption::IncDec) {
        if (!emit1(JSOP_CHECKOBJCOERCIBLE))
            return false;
    } else if (opts == EmitElemOption::Call) {
        if (!emit1(JSOP_DUP))
            return false;
    }

    if (!emitTree(pn->pn_right))
        return false;

    if (opts == EmitElemOption::Set && !emit2(JSOP_PICK, 2))
        return false;
    return true;
}

bool
BytecodeEmitter::emitSuperElemOperands(ParseNode* pn, EmitElemOption opts)
{
    MOZ_ASSERT(pn->isKind(PNK_ELEM) && pn->as<PropertyByValue>().isSuper());

    // The ordering here is somewhat screwy. We need to evaluate the propval
    // first, by spec. Do a little dance to not emit more than one JSOP_THIS.
    // Since JSOP_THIS might throw in derived class constructors, we cannot
    // just push it earlier as the receiver. We have to swap it down instead.

    if (!emitTree(pn->pn_right))
        return false;

    // We need to convert the key to an object id first, so that we do not do
    // it inside both the GETELEM and the SETELEM.
    if (opts == EmitElemOption::IncDec && !emit1(JSOP_TOID))
        return false;

    if (!emitGetThisForSuperBase(pn->pn_left))
        return false;

    if (opts == EmitElemOption::Call) {
        if (!emit1(JSOP_SWAP))
            return false;

        // We need another |this| on top, also
        if (!emitDupAt(1))
            return false;
    }

    if (!emit1(JSOP_SUPERBASE))
        return false;

    if (opts == EmitElemOption::Set && !emit2(JSOP_PICK, 3))
        return false;

    return true;
}

bool
BytecodeEmitter::emitElemOpBase(JSOp op)
{
    if (!emit1(op))
        return false;

    checkTypeSet(op);
    return true;
}

bool
BytecodeEmitter::emitElemOp(ParseNode* pn, JSOp op)
{
    EmitElemOption opts = EmitElemOption::Get;
    if (op == JSOP_CALLELEM)
        opts = EmitElemOption::Call;
    else if (op == JSOP_SETELEM || op == JSOP_STRICTSETELEM)
        opts = EmitElemOption::Set;

    return emitElemOperands(pn, opts) && emitElemOpBase(op);
}

bool
BytecodeEmitter::emitSuperElemOp(ParseNode* pn, JSOp op, bool isCall)
{
    EmitElemOption opts = EmitElemOption::Get;
    if (isCall)
        opts = EmitElemOption::Call;
    else if (op == JSOP_SETELEM_SUPER || op == JSOP_STRICTSETELEM_SUPER)
        opts = EmitElemOption::Set;

    if (!emitSuperElemOperands(pn, opts))
        return false;
    if (!emitElemOpBase(op))
        return false;

    if (isCall && !emit1(JSOP_SWAP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitElemIncDec(ParseNode* pn)
{
    MOZ_ASSERT(pn->pn_kid->isKind(PNK_ELEM));

    bool isSuper = pn->pn_kid->as<PropertyByValue>().isSuper();

    if (isSuper) {
        if (!emitSuperElemOperands(pn->pn_kid, EmitElemOption::IncDec))
            return false;
    } else {
        if (!emitElemOperands(pn->pn_kid, EmitElemOption::IncDec))
            return false;
    }

    bool post;
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    JSOp getOp;
    if (isSuper) {
        // There's no such thing as JSOP_DUP3, so we have to be creative.
        // Note that pushing things again is no fewer JSOps.
        if (!emitDupAt(2))                              // KEY THIS OBJ KEY
            return false;
        if (!emitDupAt(2))                              // KEY THIS OBJ KEY THIS
            return false;
        if (!emitDupAt(2))                              // KEY THIS OBJ KEY THIS OBJ
            return false;
        getOp = JSOP_GETELEM_SUPER;
    } else {
        // We need to convert the key to an object id first, so that we do not do
        // it inside both the GETELEM and the SETELEM. In the super case, this is
        // done by emitSuperElemOperands.
                                                        // OBJ KEY*
        if (!emit1(JSOP_TOID))                          // OBJ KEY
            return false;
        if (!emit1(JSOP_DUP2))                          // OBJ KEY OBJ KEY
            return false;
        getOp = JSOP_GETELEM;
    }
    if (!emitElemOpBase(getOp))                         // OBJ KEY V
        return false;
    if (!emit1(JSOP_POS))                               // OBJ KEY N
        return false;
    if (post && !emit1(JSOP_DUP))                       // OBJ KEY N? N
        return false;
    if (!emit1(JSOP_ONE))                               // OBJ KEY N? N 1
        return false;
    if (!emit1(binop))                                  // OBJ KEY N? N+1
        return false;

    if (post) {
        if (isSuper) {
            // We have one more value to rotate around, because of |this|
            // on the stack
            if (!emit2(JSOP_PICK, 4))
                return false;
        }
        if (!emit2(JSOP_PICK, 3 + isSuper))             // KEY N N+1 OBJ
            return false;
        if (!emit2(JSOP_PICK, 3 + isSuper))             // N N+1 OBJ KEY
            return false;
        if (!emit2(JSOP_PICK, 2 + isSuper))             // N OBJ KEY N+1
            return false;
    }

    JSOp setOp = isSuper ? (sc->strict() ? JSOP_STRICTSETELEM_SUPER : JSOP_SETELEM_SUPER)
                         : (sc->strict() ? JSOP_STRICTSETELEM : JSOP_SETELEM);
    if (!emitElemOpBase(setOp))                         // N? N+1
        return false;
    if (post && !emit1(JSOP_POP))                       // RESULT
        return false;

    return true;
}

bool
BytecodeEmitter::emitNumberOp(double dval)
{
    int32_t ival;
    if (NumberIsInt32(dval, &ival)) {
        if (ival == 0)
            return emit1(JSOP_ZERO);
        if (ival == 1)
            return emit1(JSOP_ONE);
        if ((int)(int8_t)ival == ival)
            return emit2(JSOP_INT8, uint8_t(int8_t(ival)));

        uint32_t u = uint32_t(ival);
        if (u < JS_BIT(16)) {
            if (!emitUint16Operand(JSOP_UINT16, u))
                return false;
        } else if (u < JS_BIT(24)) {
            ptrdiff_t off;
            if (!emitN(JSOP_UINT24, 3, &off))
                return false;
            SET_UINT24(code(off), u);
        } else {
            ptrdiff_t off;
            if (!emitN(JSOP_INT32, 4, &off))
                return false;
            SET_INT32(code(off), ival);
        }
        return true;
    }

    if (!constList.append(DoubleValue(dval)))
        return false;

    return emitIndex32(JSOP_DOUBLE, constList.length() - 1);
}

void
BytecodeEmitter::setJumpOffsetAt(ptrdiff_t off)
{
    SET_JUMP_OFFSET(code(off), offset() - off);
}

bool
BytecodeEmitter::pushInitialConstants(JSOp op, unsigned n)
{
    MOZ_ASSERT(op == JSOP_UNDEFINED || op == JSOP_UNINITIALIZED);

    for (unsigned i = 0; i < n; ++i) {
        if (!emit1(op))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::initializeBlockScopedLocalsFromStack(Handle<StaticBlockObject*> blockObj)
{
    for (unsigned i = blockObj->numVariables(); i > 0; --i) {
        if (blockObj->isAliased(i - 1)) {
            ScopeCoordinate sc;
            sc.setHops(0);
            sc.setSlot(BlockObject::RESERVED_SLOTS + i - 1);
            if (!emitAliasedVarOp(JSOP_INITALIASEDLEXICAL, sc, DontCheckLexical))
                return false;
        } else {
            // blockIndexToLocalIndex returns the slot index after the unaliased
            // locals stored in the frame. EmitUnaliasedVarOp expects the slot index
            // to include both unaliased and aliased locals, so we have to add the
            // number of aliased locals.
            uint32_t numAliased = script->bindings.numAliasedBodyLevelLocals();
            unsigned local = blockObj->blockIndexToLocalIndex(i - 1) + numAliased;
            if (!emitUnaliasedVarOp(JSOP_INITLEXICAL, local, DontCheckLexical))
                return false;
        }
        if (!emit1(JSOP_POP))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::enterBlockScope(StmtInfoBCE* stmtInfo, ObjectBox* objbox, JSOp initialValueOp,
                                 unsigned alreadyPushed)
{
    // This is so terrible. The eval body-level lexical scope needs to be
    // emitted in the prologue so DEFFUN can pick up the right scope chain.
    bool isEvalBodyLexicalScope = sc->staticScope()->is<StaticEvalObject>() &&
                                  !innermostStmt();
    if (isEvalBodyLexicalScope) {
        MOZ_ASSERT(code().length() == 0);
        switchToPrologue();
    }

    // Initial values for block-scoped locals. Whether it is undefined or the
    // JS_UNINITIALIZED_LEXICAL magic value depends on the context. The
    // current way we emit for-in and for-of heads means its let bindings will
    // always be initialized, so we can initialize them to undefined.
    Rooted<StaticBlockObject*> blockObj(cx, &objbox->object->as<StaticBlockObject>());
    if (!pushInitialConstants(initialValueOp, blockObj->numVariables() - alreadyPushed))
        return false;

    if (!enterNestedScope(stmtInfo, objbox, StmtType::BLOCK))
        return false;

    if (!initializeBlockScopedLocalsFromStack(blockObj))
        return false;

    if (isEvalBodyLexicalScope)
        switchToMain();

    return true;
}

/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047.
 * LLVM is deciding to inline this function which uses a lot of stack space
 * into emitTree which is recursive and uses relatively little stack space.
 */
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitSwitch(ParseNode* pn)
{
    ParseNode* cases = pn->pn_right;
    MOZ_ASSERT(cases->isKind(PNK_LEXICALSCOPE) || cases->isKind(PNK_STATEMENTLIST));

    // Emit code for the discriminant.
    if (!emitTree(pn->pn_left))
        return false;

    StmtInfoBCE stmtInfo(cx);
    ptrdiff_t top;
    if (cases->isKind(PNK_LEXICALSCOPE)) {
        if (!enterBlockScope(&stmtInfo, cases->pn_objbox, JSOP_UNINITIALIZED, 0))
            return false;

        stmtInfo.type = StmtType::SWITCH;
        stmtInfo.update = top = offset();

        // Advance |cases| to refer to the switch case list.
        cases = cases->expr();
    } else {
        MOZ_ASSERT(cases->isKind(PNK_STATEMENTLIST));
        top = offset();
        pushStatement(&stmtInfo, StmtType::SWITCH, top);
    }

    // Switch bytecodes run from here till end of final case.
    uint32_t caseCount = cases->pn_count;
    if (caseCount > JS_BIT(16)) {
        parser->tokenStream.reportError(JSMSG_TOO_MANY_CASES);
        return false;
    }

    // Try for most optimal, fall back if not dense ints.
    JSOp switchOp = JSOP_TABLESWITCH;
    uint32_t tableLength = 0;
    int32_t low, high;
    bool hasDefault = false;
    CaseClause* firstCase = cases->pn_head ? &cases->pn_head->as<CaseClause>() : nullptr;
    if (caseCount == 0 ||
        (caseCount == 1 && (hasDefault = firstCase->isDefault())))
    {
        caseCount = 0;
        low = 0;
        high = -1;
    } else {
        Vector<jsbitmap, 128, SystemAllocPolicy> intmap;
        int32_t intmapBitLength = 0;

        low  = JSVAL_INT_MAX;
        high = JSVAL_INT_MIN;

        for (CaseClause* caseNode = firstCase; caseNode; caseNode = caseNode->next()) {
            if (caseNode->isDefault()) {
                hasDefault = true;
                caseCount--;  // one of the "cases" was the default
                continue;
            }

            if (switchOp == JSOP_CONDSWITCH)
                continue;

            MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);

            ParseNode* caseValue = caseNode->caseExpression();

            if (caseValue->getKind() != PNK_NUMBER) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }

            int32_t i;
            if (!NumberIsInt32(caseValue->pn_dval, &i)) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }

            if (unsigned(i + int(JS_BIT(15))) >= unsigned(JS_BIT(16))) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }
            if (i < low)
                low = i;
            if (i > high)
                high = i;

            // Check for duplicates, which require a JSOP_CONDSWITCH.
            // We bias i by 65536 if it's negative, and hope that's a rare
            // case (because it requires a malloc'd bitmap).
            if (i < 0)
                i += JS_BIT(16);
            if (i >= intmapBitLength) {
                size_t newLength = (i / JS_BITMAP_NBITS) + 1;
                if (!intmap.resize(newLength))
                    return false;
                intmapBitLength = newLength * JS_BITMAP_NBITS;
            }
            if (JS_TEST_BIT(intmap, i)) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }
            JS_SET_BIT(intmap, i);
        }

        // Compute table length and select condswitch instead if overlarge or
        // more than half-sparse.
        if (switchOp == JSOP_TABLESWITCH) {
            tableLength = uint32_t(high - low + 1);
            if (tableLength >= JS_BIT(16) || tableLength > 2 * caseCount)
                switchOp = JSOP_CONDSWITCH;
        }
    }

    // The note has one or two offsets: first tells total switch code length;
    // second (if condswitch) tells offset to first JSOP_CASE.
    unsigned noteIndex;
    size_t switchSize;
    if (switchOp == JSOP_CONDSWITCH) {
        // 0 bytes of immediate for unoptimized switch.
        switchSize = 0;
        if (!newSrcNote3(SRC_CONDSWITCH, 0, 0, &noteIndex))
            return false;
    } else {
        MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);

        // 3 offsets (len, low, high) before the table, 1 per entry.
        switchSize = size_t(JUMP_OFFSET_LEN * (3 + tableLength));
        if (!newSrcNote2(SRC_TABLESWITCH, 0, &noteIndex))
            return false;
    }

    // Emit switchOp followed by switchSize bytes of jump or lookup table.
    if (!emitN(switchOp, switchSize))
        return false;

    Vector<CaseClause*, 32, SystemAllocPolicy> table;

    ptrdiff_t condSwitchDefaultOff = -1;
    if (switchOp == JSOP_CONDSWITCH) {
        unsigned caseNoteIndex;
        bool beforeCases = true;
        ptrdiff_t prevCaseOffset;

        // Emit code for evaluating cases and jumping to case statements.
        for (CaseClause* caseNode = firstCase; caseNode; caseNode = caseNode->next()) {
            ParseNode* caseValue = caseNode->caseExpression();

            // If the expression is a literal, suppress line number emission so
            // that debugging works more naturally.
            if (caseValue) {
                if (!emitTree(caseValue,
                              caseValue->isLiteral() ? SUPPRESS_LINENOTE : EMIT_LINENOTE))
                {
                    return false;
                }
            }

            if (!beforeCases) {
                // prevCaseOffset is the previous JSOP_CASE's bytecode offset.
                if (!setSrcNoteOffset(caseNoteIndex, 0, offset() - prevCaseOffset))
                    return false;
            }
            if (!caseValue) {
                // This is the default clause.
                continue;
            }

            if (!newSrcNote2(SRC_NEXTCASE, 0, &caseNoteIndex))
                return false;
            if (!emitJump(JSOP_CASE, 0, &prevCaseOffset))
                return false;
            caseNode->setOffset(prevCaseOffset);

            if (beforeCases) {
                // Switch note's second offset is to first JSOP_CASE.
                unsigned noteCount = notes().length();
                if (!setSrcNoteOffset(noteIndex, 1, prevCaseOffset - top))
                    return false;
                unsigned noteCountDelta = notes().length() - noteCount;
                if (noteCountDelta != 0)
                    caseNoteIndex += noteCountDelta;
                beforeCases = false;
            }
        }

        // If we didn't have an explicit default (which could fall in between
        // cases, preventing us from fusing this setSrcNoteOffset with the call
        // in the loop above), link the last case to the implicit default for
        // the benefit of IonBuilder.
        if (!hasDefault &&
            !beforeCases &&
            !setSrcNoteOffset(caseNoteIndex, 0, offset() - prevCaseOffset))
        {
            return false;
        }

        // Emit default even if no explicit default statement.
        if (!emitJump(JSOP_DEFAULT, 0, &condSwitchDefaultOff))
            return false;
    } else {
        MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);
        jsbytecode* pc = code(top + JUMP_OFFSET_LEN);

        // Fill in switch bounds, which we know fit in 16-bit offsets.
        SET_JUMP_OFFSET(pc, low);
        pc += JUMP_OFFSET_LEN;
        SET_JUMP_OFFSET(pc, high);
        pc += JUMP_OFFSET_LEN;

        if (tableLength != 0) {
            if (!table.growBy(tableLength))
                return false;

            for (CaseClause* caseNode = firstCase; caseNode; caseNode = caseNode->next()) {
                if (ParseNode* caseValue = caseNode->caseExpression()) {
                    MOZ_ASSERT(caseValue->isKind(PNK_NUMBER));

                    int32_t i = int32_t(caseValue->pn_dval);
                    MOZ_ASSERT(double(i) == caseValue->pn_dval);

                    i -= low;
                    MOZ_ASSERT(uint32_t(i) < tableLength);
                    MOZ_ASSERT(!table[i]);
                    table[i] = caseNode;
                }
            }
        }
    }

    ptrdiff_t defaultOffset = -1;

    // Emit code for each case's statements.
    for (CaseClause* caseNode = firstCase; caseNode; caseNode = caseNode->next()) {
        if (switchOp == JSOP_CONDSWITCH && !caseNode->isDefault())
            setJumpOffsetAt(caseNode->offset());

        // If this is emitted as a TABLESWITCH, we'll need to know this case's
        // offset later when emitting the table. Store it in the node's
        // pn_offset (giving the field a different meaning vs. how we used it
        // on the immediately preceding line of code).
        ptrdiff_t here = offset();
        caseNode->setOffset(here);
        if (caseNode->isDefault())
            defaultOffset = here - top;

        if (!emitTree(caseNode->statementList()))
            return false;
    }

    if (!hasDefault) {
        // If no default case, offset for default is to end of switch.
        defaultOffset = offset() - top;
    }
    MOZ_ASSERT(defaultOffset != -1);

    // Set the default offset (to end of switch if no default).
    jsbytecode* pc;
    if (switchOp == JSOP_CONDSWITCH) {
        pc = nullptr;
        SET_JUMP_OFFSET(code(condSwitchDefaultOff), defaultOffset - (condSwitchDefaultOff - top));
    } else {
        pc = code(top);
        SET_JUMP_OFFSET(pc, defaultOffset);
        pc += JUMP_OFFSET_LEN;
    }

    // Set the SRC_SWITCH note's offset operand to tell end of switch.
    if (!setSrcNoteOffset(noteIndex, 0, offset() - top))
        return false;

    if (switchOp == JSOP_TABLESWITCH) {
        // Skip over the already-initialized switch bounds.
        pc += 2 * JUMP_OFFSET_LEN;

        // Fill in the jump table, if there is one.
        for (uint32_t i = 0; i < tableLength; i++) {
            CaseClause* caseNode = table[i];
            ptrdiff_t off = caseNode ? caseNode->offset() - top : 0;
            SET_JUMP_OFFSET(pc, off);
            pc += JUMP_OFFSET_LEN;
        }
    }

    if (pn->pn_right->isKind(PNK_LEXICALSCOPE)) {
        if (!leaveNestedScope(&stmtInfo))
            return false;
    } else {
        popStatement();
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

bool
BytecodeEmitter::emitYieldOp(JSOp op)
{
    if (op == JSOP_FINALYIELDRVAL)
        return emit1(JSOP_FINALYIELDRVAL);

    MOZ_ASSERT(op == JSOP_INITIALYIELD || op == JSOP_YIELD);

    ptrdiff_t off;
    if (!emitN(op, 3, &off))
        return false;

    uint32_t yieldIndex = yieldOffsetList.length();
    if (yieldIndex >= JS_BIT(24)) {
        reportError(nullptr, JSMSG_TOO_MANY_YIELDS);
        return false;
    }

    SET_UINT24(code(off), yieldIndex);

    if (!yieldOffsetList.append(offset()))
        return false;

    return emit1(JSOP_DEBUGAFTERYIELD);
}

bool
BytecodeEmitter::emitCreateFunctionThis()
{
    // Do nothing if the function doesn't have a this-binding (this happens for
    // instance if it doesn't use this/eval or if it's an arrow function).
    if (!sc->asFunctionBox()->hasThisBinding())
        return true;

    switchToPrologue();

    if (!emit1(JSOP_FUNCTIONTHIS))
        return false;

    BindingIter bi = Bindings::thisBinding(cx, script);
    if (!emitStoreToTopScope(bi))
        return false;
    if (!emit1(JSOP_POP))
        return false;

    switchToMain();
    return true;
}

bool
BytecodeEmitter::emitSetThis(ParseNode* pn)
{
    // PNK_SETTHIS is used to update |this| after a super() call in a derived
    // class constructor.

    MOZ_ASSERT(pn->isKind(PNK_SETTHIS));

    ParseNode* name = pn->pn_left;
    MOZ_ASSERT(name->isKind(PNK_NAME));

    if (!emitTree(pn->pn_right))
        return false;

    if (!bindNameToSlot(name))
        return false;

    JSOp setOp = name->getOp();

    // Handle the eval case. Only accept the strict variant, as eval in a
    // derived class constructor must be strict.
    if (setOp == JSOP_STRICTSETNAME) {
        if (!emitAtomOp(name, JSOP_GETNAME))
            return false;
        if (!emit1(JSOP_CHECKTHISREINIT))
            return false;
        if (!emit1(JSOP_POP))
            return false;

        if (!emitAtomOp(name, JSOP_BINDNAME))
            return false;
        if (!emit1(JSOP_SWAP))
            return false;

        return emitAtomOp(name, setOp);
    }

    JSOp getOp;
    switch (setOp) {
      case JSOP_SETLOCAL:
        getOp = JSOP_GETLOCAL;
        setOp = JSOP_INITLEXICAL;
        break;
      case JSOP_SETALIASEDVAR:
        getOp = JSOP_GETALIASEDVAR;
        setOp = JSOP_INITALIASEDLEXICAL;
        break;
      default: MOZ_CRASH("Unexpected op");
    }

    // First, get the original |this| and throw if we already initialized it.
    if (!emitVarOp(name, getOp))
        return false;
    if (!emit1(JSOP_CHECKTHISREINIT))
        return false;
    if (!emit1(JSOP_POP))
        return false;

    // Emit the set.
    return emitVarOp(name, setOp);
}

static bool
IsModuleOnScopeChain(JSObject* obj)
{
    for (StaticScopeIter<NoGC> ssi(obj); !ssi.done(); ssi++) {
        if (ssi.type() == StaticScopeIter<NoGC>::Module)
            return true;
    }
    return false;
}

bool
BytecodeEmitter::emitFunctionScript(ParseNode* body)
{
    if (!updateLocalsToFrameSlots())
        return false;

    /*
     * IonBuilder has assumptions about what may occur immediately after
     * script->main (e.g., in the case of destructuring params). Thus, put the
     * following ops into the range [script->code, script->main). Note:
     * execution starts from script->code, so this has no semantic effect.
     */

    FunctionBox* funbox = sc->asFunctionBox();

    // Link the function and the script to each other, so that StaticScopeIter
    // may walk the scope chain of currently compiling scripts.
    JSScript::linkToFunctionFromEmitter(cx, script, funbox);

    // Determine whether the function is defined inside a module.
    insideModule = IsModuleOnScopeChain(sc->staticScope());

    if (funbox->argumentsHasLocalBinding()) {
        MOZ_ASSERT(offset() == 0);  /* See JSScript::argumentsBytecode. */
        switchToPrologue();
        if (!emit1(JSOP_ARGUMENTS))
            return false;
        BindingIter bi = Bindings::argumentsBinding(cx, script);
        if (!emitStoreToTopScope(bi))
            return false;
        if (!emit1(JSOP_POP))
            return false;
        switchToMain();
    }

    if (!emitCreateFunctionThis())
        return false;

    /*
     * Emit a prologue for run-once scripts which will deoptimize JIT code if
     * the script ends up running multiple times via foo.caller related
     * shenanigans.
     */
    bool runOnce = isRunOnceLambda();
    if (runOnce) {
        switchToPrologue();
        if (!emit1(JSOP_RUNONCE))
            return false;
        switchToMain();
    }

    if (!emitTree(body))
        return false;

    if (sc->isFunctionBox()) {
        if (sc->asFunctionBox()->isGenerator()) {
            // If we fall off the end of a generator, do a final yield.
            if (sc->asFunctionBox()->isStarGenerator() && !emitPrepareIteratorResult())
                return false;

            if (!emit1(JSOP_UNDEFINED))
                return false;

            if (sc->asFunctionBox()->isStarGenerator() && !emitFinishIteratorResult(true))
                return false;

            if (!emit1(JSOP_SETRVAL))
                return false;

            ScopeCoordinate sc;
            // We know that .generator is on the top scope chain node, as we are
            // at the function end.
            sc.setHops(0);
            MOZ_ALWAYS_TRUE(lookupAliasedNameSlot(cx->names().dotGenerator, &sc));
            if (!emitAliasedVarOp(JSOP_GETALIASEDVAR, sc, DontCheckLexical))
                return false;

            // No need to check for finally blocks, etc as in EmitReturn.
            if (!emitYieldOp(JSOP_FINALYIELDRVAL))
                return false;
        } else {
            // Non-generator functions just return |undefined|. The JSOP_RETRVAL
            // emitted below will do that, except if the script has a finally
            // block: there can be a non-undefined value in the return value
            // slot. Make sure the return value is |undefined|.
            if (hasTryFinally) {
                if (!emit1(JSOP_UNDEFINED))
                    return false;
                if (!emit1(JSOP_SETRVAL))
                    return false;
            }
        }
    }

    if (sc->isFunctionBox() && sc->asFunctionBox()->isDerivedClassConstructor()) {
        BindingIter bi = Bindings::thisBinding(cx, script);
        if (!emitLoadFromTopScope(bi))
            return false;
        if (!emit1(JSOP_CHECKRETURN))
            return false;
    }

    // Always end the script with a JSOP_RETRVAL. Some other parts of the codebase
    // depend on this opcode, e.g. InterpreterRegs::setToEndOfScript.
    if (!emit1(JSOP_RETRVAL))
        return false;

    // If all locals are aliased, the frame's block slots won't be used, so we
    // can set numBlockScoped = 0. This is nice for generators as it ensures
    // nfixed == 0, so we don't have to initialize any local slots when resuming
    // a generator.
    if (sc->allLocalsAliased())
        script->bindings.setAllLocalsAliased();

    if (!JSScript::fullyInitFromEmitter(cx, script, this))
        return false;

    /*
     * If this function is only expected to run once, mark the script so that
     * initializers created within it may be given more precise types.
     */
    if (runOnce) {
        script->setTreatAsRunOnce();
        MOZ_ASSERT(!script->hasRunOnce());
    }

    tellDebuggerAboutCompiledScript(cx);

    return true;
}

bool
BytecodeEmitter::emitModuleScript(ParseNode* body)
{
    insideModule = true;

    if (!updateLocalsToFrameSlots())
        return false;

    /*
     * IonBuilder has assumptions about what may occur immediately after
     * script->main (e.g., in the case of destructuring params). Thus, put the
     * following ops into the range [script->code, script->main). Note:
     * execution starts from script->code, so this has no semantic effect.
     */

    ModuleBox* modulebox = sc->asModuleBox();
    MOZ_ASSERT(modulebox);

    // Link the module and the script to each other, so that StaticScopeIter
    // may walk the scope chain of currently compiling scripts.
    JSScript::linkToModuleFromEmitter(cx, script, modulebox);

    if (!emitTree(body))
        return false;

    // Always end the script with a JSOP_RETRVAL. Some other parts of the codebase
    // depend on this opcode, e.g. InterpreterRegs::setToEndOfScript.
    if (!emit1(JSOP_RETRVAL))
        return false;

    // If all locals are aliased, the frame's block slots won't be used, so we
    // can set numBlockScoped = 0. This is nice for generators as it ensures
    // nfixed == 0, so we don't have to initialize any local slots when resuming
    // a generator.
    if (sc->allLocalsAliased())
        script->bindings.setAllLocalsAliased();

    if (!JSScript::fullyInitFromEmitter(cx, script, this))
        return false;

    /*
     * Since modules are only run once. Mark the script so that initializers
     * created within it may be given more precise types.
     */
    script->setTreatAsRunOnce();
    MOZ_ASSERT(!script->hasRunOnce());

    tellDebuggerAboutCompiledScript(cx);

    return true;
}

bool
BytecodeEmitter::maybeEmitVarDecl(JSOp prologueOp, ParseNode* pn, jsatomid* result)
{
    jsatomid atomIndex;

    if (!pn->pn_scopecoord.isFree()) {
        atomIndex = pn->pn_scopecoord.slot();
    } else {
        if (!makeAtomIndex(pn->pn_atom, &atomIndex))
            return false;
    }

    if (JOF_OPTYPE(pn->getOp()) == JOF_ATOM &&
        (!sc->isFunctionBox() || sc->asFunctionBox()->needsCallObject()))
    {
        switchToPrologue();
        if (!updateSourceCoordNotes(pn->pn_pos.begin))
            return false;
        if (!emitIndexOp(prologueOp, atomIndex))
            return false;
        switchToMain();
    }

    if (result)
        *result = atomIndex;
    return true;
}

template <BytecodeEmitter::DestructuringDeclEmitter EmitName>
bool
BytecodeEmitter::emitDestructuringDeclsWithEmitter(JSOp prologueOp, ParseNode* pattern)
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
                if (!EmitName(this, prologueOp, target))
                    return false;
            } else {
                if (!emitDestructuringDeclsWithEmitter<EmitName>(prologueOp, target))
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
            if (!EmitName(this, prologueOp, target))
                return false;
        } else {
            if (!emitDestructuringDeclsWithEmitter<EmitName>(prologueOp, target))
                return false;
        }
    }
    return true;
}

static bool
EmitDestructuringDecl(BytecodeEmitter* bce, JSOp prologueOp, ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_NAME));
    if (!bce->bindNameToSlot(pn))
        return false;

    MOZ_ASSERT(!pn->isOp(JSOP_CALLEE));
    return bce->maybeEmitVarDecl(prologueOp, pn, nullptr);
}

bool
BytecodeEmitter::emitDestructuringDecls(JSOp prologueOp, ParseNode* pattern)
{
    return emitDestructuringDeclsWithEmitter<EmitDestructuringDecl>(prologueOp, pattern);
}

static bool
EmitInitializeDestructuringDecl(BytecodeEmitter* bce, JSOp prologueOp, ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_NAME));
    MOZ_ASSERT(pn->isBound());
    return bce->emitVarOp(pn, pn->getOp());
}

bool
BytecodeEmitter::emitInitializeDestructuringDecls(JSOp prologueOp, ParseNode* pattern)
{
    return emitDestructuringDeclsWithEmitter<EmitInitializeDestructuringDecl>(prologueOp, pattern);
}

bool
BytecodeEmitter::emitDestructuringLHS(ParseNode* target, VarEmitOption emitOption)
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
        if (!emitDestructuringOpsHelper(target, emitOption))
            return false;
        if (emitOption == InitializeVars) {
            // Per its post-condition, emitDestructuringOpsHelper has left the
            // to-be-destructured value on top of the stack.
            if (!emit1(JSOP_POP))
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
            if (!bindNameToSlot(target))
                return false;

            switch (target->getOp()) {
              case JSOP_SETNAME:
              case JSOP_STRICTSETNAME:
              case JSOP_SETGNAME:
              case JSOP_STRICTSETGNAME:
              case JSOP_INITGLEXICAL: {
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
                if (!makeAtomIndex(target->pn_atom, &atomIndex))
                    return false;

                // INITGLEXICAL always initializes a binding on the global
                // lexical scope and does not need a BINDGNAME.
                if (!target->isOp(JSOP_INITGLEXICAL)) {
                    bool global = target->isOp(JSOP_SETGNAME) || target->isOp(JSOP_STRICTSETGNAME);
                    JSOp bindOp = global ? JSOP_BINDGNAME : JSOP_BINDNAME;
                    if (!emitIndex32(bindOp, atomIndex))
                        return false;
                    if (!emit1(JSOP_SWAP))
                        return false;
                }

                if (!emitIndexOp(target->getOp(), atomIndex))
                    return false;
                break;
              }

              case JSOP_SETLOCAL:
              case JSOP_SETARG:
              case JSOP_SETALIASEDVAR:
              case JSOP_INITLEXICAL:
              case JSOP_INITALIASEDLEXICAL:
                if (!emitVarOp(target, target->getOp()))
                    return false;
                break;

              default:
                MOZ_CRASH("emitDestructuringLHS: bad name op");
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
            JSOp setOp;
            if (target->as<PropertyAccess>().isSuper()) {
                if (!emitSuperPropLHS(&target->as<PropertyAccess>().expression()))
                    return false;
                if (!emit2(JSOP_PICK, 2))
                    return false;
                setOp = sc->strict() ? JSOP_STRICTSETPROP_SUPER : JSOP_SETPROP_SUPER;
            } else {
                if (!emitTree(target->pn_expr))
                    return false;
                if (!emit1(JSOP_SWAP))
                    return false;
                setOp = sc->strict() ? JSOP_STRICTSETPROP : JSOP_SETPROP;
            }
            if (!emitAtomOp(target, setOp))
                return false;
            break;
          }

          case PNK_ELEM:
          {
            // See the comment at `case PNK_DOT:` above. This case,
            // `[a[x]] = [b]`, is handled much the same way. The JSOP_SWAP
            // is emitted by emitElemOperands.
            if (target->as<PropertyByValue>().isSuper()) {
                JSOp setOp = sc->strict() ? JSOP_STRICTSETELEM_SUPER : JSOP_SETELEM_SUPER;
                if (!emitSuperElemOp(target, setOp))
                    return false;
            } else {
                JSOp setOp = sc->strict() ? JSOP_STRICTSETELEM : JSOP_SETELEM;
                if (!emitElemOp(target, setOp))
                    return false;
            }
            break;
          }

          case PNK_CALL:
            MOZ_ASSERT(target->pn_xflags & PNX_SETCALL);
            if (!emitTree(target))
                return false;

            // Pop the call return value. Below, we pop the RHS too, balancing
            // the stack --- presumably for the benefit of bytecode
            // analysis. (The interpreter will never reach these instructions
            // since we just emitted JSOP_SETCALL, which always throws. It's
            // possible no analyses actually depend on this either.)
            if (!emit1(JSOP_POP))
                return false;
            break;

          default:
            MOZ_CRASH("emitDestructuringLHS: bad lhs kind");
        }

        // Pop the assigned value.
        if (!emit1(JSOP_POP))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitIteratorNext(ParseNode* pn)
{
    MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting,
               ".next() iteration is prohibited in self-hosted code because it "
               "can run user-modifiable iteration code");

    if (!emit1(JSOP_DUP))                                 // ... ITER ITER
        return false;
    if (!emitAtomOp(cx->names().next, JSOP_CALLPROP))     // ... ITER NEXT
        return false;
    if (!emit1(JSOP_SWAP))                                // ... NEXT ITER
        return false;
    if (!emitCall(JSOP_CALL, 0, pn))                      // ... RESULT
        return false;
    checkTypeSet(JSOP_CALL);
    return true;
}

bool
BytecodeEmitter::emitDefault(ParseNode* defaultExpr)
{
    if (!emit1(JSOP_DUP))                                 // VALUE VALUE
        return false;
    if (!emit1(JSOP_UNDEFINED))                           // VALUE VALUE UNDEFINED
        return false;
    if (!emit1(JSOP_STRICTEQ))                            // VALUE EQL?
        return false;
    // Emit source note to enable ion compilation.
    if (!newSrcNote(SRC_IF))
        return false;
    ptrdiff_t jump;
    if (!emitJump(JSOP_IFEQ, 0, &jump))                   // VALUE
        return false;
    if (!emit1(JSOP_POP))                                 // .
        return false;
    if (!emitTree(defaultExpr))                           // DEFAULTVALUE
        return false;
    setJumpOffsetAt(jump);
    return true;
}

bool
BytecodeEmitter::emitDestructuringOpsArrayHelper(ParseNode* pattern, VarEmitOption emitOption)
{
    MOZ_ASSERT(pattern->isKind(PNK_ARRAY));
    MOZ_ASSERT(pattern->isArity(PN_LIST));
    MOZ_ASSERT(this->stackDepth != 0);

    /*
     * Use an iterator to destructure the RHS, instead of index lookup.
     * InitializeVars expects us to leave the *original* value on the stack.
     */
    if (emitOption == InitializeVars) {
        if (!emit1(JSOP_DUP))                                     // ... OBJ OBJ
            return false;
    }
    if (!emitIterator())                                          // ... OBJ? ITER
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
            if (!emitUint32Operand(JSOP_NEWARRAY, 0))             // ... OBJ? ITER ARRAY
                return false;
            if (!emitNumberOp(0))                                 // ... OBJ? ITER ARRAY INDEX
                return false;
            if (!emitSpread())                                    // ... OBJ? ARRAY INDEX
                return false;
            if (!emit1(JSOP_POP))                                 // ... OBJ? ARRAY
                return false;
            needToPopIterator = false;
        } else {
            if (!emit1(JSOP_DUP))                                 // ... OBJ? ITER ITER
                return false;
            if (!emitIteratorNext(pattern))                       // ... OBJ? ITER RESULT
                return false;
            if (!emit1(JSOP_DUP))                                 // ... OBJ? ITER RESULT RESULT
                return false;
            if (!emitAtomOp(cx->names().done, JSOP_GETPROP))      // ... OBJ? ITER RESULT DONE?
                return false;

            // Emit (result.done ? undefined : result.value)
            // This is mostly copied from emitConditionalExpression, except that this code
            // does not push new values onto the stack.
            unsigned noteIndex;
            if (!newSrcNote(SRC_COND, &noteIndex))
                return false;
            ptrdiff_t beq;
            if (!emitJump(JSOP_IFEQ, 0, &beq))
                return false;

            if (!emit1(JSOP_POP))                                 // ... OBJ? ITER
                return false;
            if (!emit1(JSOP_UNDEFINED))                           // ... OBJ? ITER UNDEFINED
                return false;

            /* Jump around else, fixup the branch, emit else, fixup jump. */
            ptrdiff_t jmp;
            if (!emitJump(JSOP_GOTO, 0, &jmp))
                return false;
            setJumpOffsetAt(beq);

            if (!emitAtomOp(cx->names().value, JSOP_GETPROP))     // ... OBJ? ITER VALUE
                return false;

            setJumpOffsetAt(jmp);
            if (!setSrcNoteOffset(noteIndex, 0, jmp - beq))
                return false;
        }

        if (pndefault && !emitDefault(pndefault))
            return false;

        // Destructure into the pattern the element contains.
        ParseNode* subpattern = elem;
        if (subpattern->isKind(PNK_ELISION)) {
            // The value destructuring into an elision just gets ignored.
            if (!emit1(JSOP_POP))                                 // ... OBJ? ITER
                return false;
            continue;
        }

        int32_t depthBefore = this->stackDepth;
        if (!emitDestructuringLHS(subpattern, emitOption))
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
            MOZ_ASSERT((this->stackDepth - this->stackDepth) >= -1);
            uint32_t pickDistance = uint32_t((this->stackDepth + 1) - depthBefore);
            if (pickDistance > 0) {
                if (pickDistance > UINT8_MAX) {
                    reportError(subpattern, JSMSG_TOO_MANY_LOCALS);
                    return false;
                }
                if (!emit2(JSOP_PICK, (uint8_t)pickDistance))
                    return false;
            }
        }
    }

    if (needToPopIterator && !emit1(JSOP_POP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitComputedPropertyName(ParseNode* computedPropName)
{
    MOZ_ASSERT(computedPropName->isKind(PNK_COMPUTED_NAME));
    return emitTree(computedPropName->pn_kid) && emit1(JSOP_TOID);
}

bool
BytecodeEmitter::emitDestructuringOpsObjectHelper(ParseNode* pattern, VarEmitOption emitOption)
{
    MOZ_ASSERT(pattern->isKind(PNK_OBJECT));
    MOZ_ASSERT(pattern->isArity(PN_LIST));

    MOZ_ASSERT(this->stackDepth > 0);                             // ... RHS

    if (!emitRequireObjectCoercible())                            // ... RHS
        return false;

    for (ParseNode* member = pattern->pn_head; member; member = member->pn_next) {
        // Duplicate the value being destructured to use as a reference base.
        if (!emit1(JSOP_DUP))                                     // ... RHS RHS
            return false;

        // Now push the property name currently being matched, which is the
        // current property name "label" on the left of a colon in the object
        // initialiser.
        bool needsGetElem = true;

        ParseNode* subpattern;
        if (member->isKind(PNK_MUTATEPROTO)) {
            if (!emitAtomOp(cx->names().proto, JSOP_GETPROP))     // ... RHS PROP
                return false;
            needsGetElem = false;
            subpattern = member->pn_kid;
        } else {
            MOZ_ASSERT(member->isKind(PNK_COLON) || member->isKind(PNK_SHORTHAND));

            ParseNode* key = member->pn_left;
            if (key->isKind(PNK_NUMBER)) {
                if (!emitNumberOp(key->pn_dval))                  // ... RHS RHS KEY
                    return false;
            } else if (key->isKind(PNK_OBJECT_PROPERTY_NAME) || key->isKind(PNK_STRING)) {
                PropertyName* name = key->pn_atom->asPropertyName();

                // The parser already checked for atoms representing indexes and
                // used PNK_NUMBER instead, but also watch for ids which TI treats
                // as indexes for simplification of downstream analysis.
                jsid id = NameToId(name);
                if (id != IdToTypeId(id)) {
                    if (!emitTree(key))                           // ... RHS RHS KEY
                        return false;
                } else {
                    if (!emitAtomOp(name, JSOP_GETPROP))          // ...RHS PROP
                        return false;
                    needsGetElem = false;
                }
            } else {
                if (!emitComputedPropertyName(key))               // ... RHS RHS KEY
                    return false;
            }

            subpattern = member->pn_right;
        }

        // Get the property value if not done already.
        if (needsGetElem && !emitElemOpBase(JSOP_GETELEM))        // ... RHS PROP
            return false;

        if (subpattern->isKind(PNK_ASSIGN)) {
            if (!emitDefault(subpattern->pn_right))
                return false;
            subpattern = subpattern->pn_left;
        }

        // Destructure PROP per this member's subpattern.
        int32_t depthBefore = this->stackDepth;
        if (!emitDestructuringLHS(subpattern, emitOption))
            return false;

        // If emitOption is InitializeVars, destructuring initialized each
        // target in the subpattern's LHS as it went, then popped PROP.  We've
        // correctly returned to the loop-entry stack, and we continue to the
        // next member.
        if (emitOption == InitializeVars)                         // ... RHS
            continue;

        MOZ_ASSERT(emitOption == PushInitialValues);

        // emitDestructuringLHS removed PROP, and it pushed a value per target
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
        MOZ_ASSERT((this->stackDepth - this->stackDepth) >= -1);
        uint32_t pickDistance = uint32_t((this->stackDepth + 1) - depthBefore);
        if (pickDistance > 0) {
            if (pickDistance > UINT8_MAX) {
                reportError(subpattern, JSMSG_TOO_MANY_LOCALS);
                return false;
            }
            if (!emit2(JSOP_PICK, (uint8_t)pickDistance))
                return false;
        }
    }

    if (emitOption == PushInitialValues) {
        // Per the above loop invariant, the value being destructured into this
        // object pattern is atop the stack.  Pop it to achieve the
        // post-condition.
        if (!emit1(JSOP_POP))                                 // ... <pattern's target name values, seriatim>
            return false;
    }

    return true;
}

/*
 * Recursive helper for emitDestructuringOps.
 * EmitDestructuringOpsHelper assumes the to-be-destructured value has been
 * pushed on the stack and emits code to destructure each part of a [] or {}
 * lhs expression.
 *
 * If emitOption is InitializeVars, the initial to-be-destructured value is
 * left untouched on the stack and the overall depth is not changed.
 *
 * If emitOption is PushInitialValues, the to-be-destructured value is replaced
 * with the initial values of the N (where 0 <= N) variables assigned in the
 * lhs expression. (Same post-condition as emitDestructuringLHS)
 */
bool
BytecodeEmitter::emitDestructuringOpsHelper(ParseNode* pattern, VarEmitOption emitOption)
{
    MOZ_ASSERT(emitOption != DefineVars);

    if (pattern->isKind(PNK_ARRAY))
        return emitDestructuringOpsArrayHelper(pattern, emitOption);
    return emitDestructuringOpsObjectHelper(pattern, emitOption);
}

bool
BytecodeEmitter::emitDestructuringOps(ParseNode* pattern, bool isLet)
{
    /*
     * Call our recursive helper to emit the destructuring assignments and
     * related stack manipulations.
     */
    VarEmitOption emitOption = isLet ? PushInitialValues : InitializeVars;
    return emitDestructuringOpsHelper(pattern, emitOption);
}

bool
BytecodeEmitter::emitTemplateString(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));

    bool pushedString = false;

    for (ParseNode* pn2 = pn->pn_head; pn2 != NULL; pn2 = pn2->pn_next) {
        bool isString = (pn2->getKind() == PNK_STRING || pn2->getKind() == PNK_TEMPLATE_STRING);

        // Skip empty strings. These are very common: a template string like
        // `${a}${b}` has three empty strings and without this optimization
        // we'd emit four JSOP_ADD operations instead of just one.
        if (isString && pn2->pn_atom->empty())
            continue;

        if (!isString) {
            // We update source notes before emitting the expression
            if (!updateSourceCoordNotes(pn2->pn_pos.begin))
                return false;
        }

        if (!emitTree(pn2))
            return false;

        if (!isString) {
            // We need to convert the expression to a string
            if (!emit1(JSOP_TOSTRING))
                return false;
        }

        if (pushedString) {
            // We've pushed two strings onto the stack. Add them together, leaving just one.
            if (!emit1(JSOP_ADD))
                return false;
        } else {
            pushedString = true;
        }
    }

    if (!pushedString) {
        // All strings were empty, this can happen for something like `${""}`.
        // Just push an empty string.
        if (!emitAtomOp(cx->names().empty, JSOP_STRING))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitVariables(ParseNode* pn, VarEmitOption emitOption)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));

    ParseNode* next;
    for (ParseNode* binding = pn->pn_head; binding; binding = next) {
        if (!updateSourceCoordNotes(binding->pn_pos.begin))
            return false;
        next = binding->pn_next;

        if (binding->isKind(PNK_ARRAY) || binding->isKind(PNK_OBJECT)) {
            // Destructuring BindingPattern in a `for` loop head:
            //     for (let [x, y] of pts) ...;
            // or in a deprecated comprehension:
            //     a = [x*y for (let [x, y] of pts)];
            //
            // (ES6 calls this a ForDeclaration. When emitting code for a plain
            // LexicalDeclaration, like `let [x, y] = pt;`, binding will be a
            // PNK_ASSIGN node, not a PNK_ARRAY node.  `let [x, y];` without an
            // initializer is a SyntaxError.)

            MOZ_ASSERT(pn->pn_count == 1);
            if (emitOption == DefineVars) {
                // Emit JSOP_DEFVAR instructions if needed, but not
                // destructuring ops. Each iteration of the for-loop is
                // responsible for initializing these variables, so it's
                // the caller's responsibility.
                if (!emitDestructuringDecls(pn->getOp(), binding))
                    return false;
            } else {
                // We're emitting destructuring let binding initialization
                // for a legacy comprehension expression. See
                // emitForInOrOfVariables.

                // Lexical bindings cannot be used before they are
                // initialized. Similar to the JSOP_INITLEXICAL case below.
                MOZ_ASSERT(emitOption == InitializeVars);
                if (!emit1(JSOP_UNDEFINED))
                    return false;
                if (!emitInitializeDestructuringDecls(pn->getOp(), binding))
                    return false;
                if (!emit1(JSOP_POP))
                    return false;
            }
        } else if (binding->isKind(PNK_ASSIGN)) {
            /*
             * A destructuring initialiser assignment preceded by var will
             * never occur to the left of 'in' in a for-in loop.  As with 'for
             * (var x = i in o)...', this will cause the entire 'var [a, b] =
             * i' to be hoisted out of the loop.
             */
            MOZ_ASSERT(binding->isOp(JSOP_NOP));
            MOZ_ASSERT(emitOption != DefineVars);

            /*
             * To allow the front end to rewrite var f = x; as f = x; when a
             * function f(){} precedes the var, detect simple name assignment
             * here and initialize the name.
             */
            if (binding->pn_left->isKind(PNK_NAME)) {
                if (!emitSingleVariable(pn, binding->pn_left, binding->pn_right, emitOption))
                    return false;
            } else {
                ParseNode* initializer = binding->pn_left;
                if (!emitDestructuringDecls(pn->getOp(), initializer))
                    return false;

                if (!emitTree(binding->pn_right))
                    return false;

                if (!emitDestructuringOpsHelper(initializer, emitOption))
                    return false;

                if (emitOption == InitializeVars) {
                    if (!emit1(JSOP_POP))
                        return false;
                }
            }
        } else {
            if (!emitSingleVariable(pn, binding, binding->maybeExpr(), emitOption))
                return false;
        }
    }
    return true;
}

bool
BytecodeEmitter::emitSingleVariable(ParseNode* pn, ParseNode* binding, ParseNode* initializer,
                                    VarEmitOption emitOption)
{
    // NB: if this var redeclares an existing binding, then `binding` is linked
    // on its definition's use-chain and pn_expr has been overlayed with
    // pn_lexdef.
    MOZ_ASSERT(binding->isKind(PNK_NAME));
    if (!bindNameToSlot(binding))
        return false;

    JSOp op = binding->getOp();
    MOZ_ASSERT(op != JSOP_CALLEE);
    MOZ_ASSERT(!binding->pn_scopecoord.isFree() || !pn->isOp(JSOP_NOP));

    jsatomid atomIndex;
    if (!maybeEmitVarDecl(pn->getOp(), binding, &atomIndex))
        return false;

    if (initializer) {
        MOZ_ASSERT(emitOption != DefineVars);
        if (op == JSOP_SETNAME ||
            op == JSOP_STRICTSETNAME ||
            op == JSOP_SETGNAME ||
            op == JSOP_STRICTSETGNAME)
        {
            MOZ_ASSERT(emitOption != PushInitialValues);
            JSOp bindOp;
            if (op == JSOP_SETNAME || op == JSOP_STRICTSETNAME)
                bindOp = JSOP_BINDNAME;
            else
                bindOp = JSOP_BINDGNAME;
            if (!emitIndex32(bindOp, atomIndex))
                return false;
        }

        bool oldEmittingForInit = emittingForInit;
        emittingForInit = false;
        if (!emitTree(initializer))
            return false;
        emittingForInit = oldEmittingForInit;
    } else if (op == JSOP_INITLEXICAL ||
               op == JSOP_INITGLEXICAL ||
               emitOption == PushInitialValues)
    {
        // 'let' bindings cannot be used before they are
        // initialized. JSOP_INITLEXICAL distinguishes the binding site.
        MOZ_ASSERT(emitOption != DefineVars);
        if (!emit1(JSOP_UNDEFINED))
            return false;
    } else {
        // The declaration is like `var x;`. Nothing to do.
        return true;
    }

    // If we are not initializing, nothing to pop. If we are initializing
    // lets, we must emit the pops.
    if (emitOption == InitializeVars) {
        MOZ_ASSERT_IF(binding->isDefn(), initializer == binding->pn_expr);
        if (!binding->pn_scopecoord.isFree()) {
            if (!emitVarOp(binding, op))
                return false;
        } else {
            if (!emitIndexOp(op, atomIndex))
                return false;
        }
        if (!emit1(JSOP_POP))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitAssignment(ParseNode* lhs, JSOp op, ParseNode* rhs)
{
    /*
     * Check left operand type and generate specialized code for it.
     * Specialize to avoid ECMA "reference type" values on the operand
     * stack, which impose pervasive runtime "GetValue" costs.
     */
    jsatomid atomIndex = (jsatomid) -1;
    uint8_t offset = 1;

    switch (lhs->getKind()) {
      case PNK_NAME:
        if (!bindNameToSlot(lhs))
            return false;
        if (lhs->pn_scopecoord.isFree()) {
            if (!makeAtomIndex(lhs->pn_atom, &atomIndex))
                return false;

            JSOp bindOp;
            if (lhs->isOp(JSOP_SETNAME) || lhs->isOp(JSOP_STRICTSETNAME)) {
                bindOp = JSOP_BINDNAME;
            } else if (lhs->isOp(JSOP_SETGNAME) || lhs->isOp(JSOP_STRICTSETGNAME)) {
                bindOp = JSOP_BINDGNAME;
            } else {
                MOZ_ASSERT(lhs->isOp(JSOP_SETINTRINSIC));
                break;
            }
            if (!emitIndex32(bindOp, atomIndex))
                return false;
            offset++;
        }
        break;
      case PNK_DOT:
        if (lhs->as<PropertyAccess>().isSuper()) {
            if (!emitSuperPropLHS(&lhs->as<PropertyAccess>().expression()))
                return false;
            offset += 2;
        } else {
            if (!emitTree(lhs->expr()))
                return false;
            offset += 1;
        }
        if (!makeAtomIndex(lhs->pn_atom, &atomIndex))
            return false;
        break;
      case PNK_ELEM:
        MOZ_ASSERT(lhs->isArity(PN_BINARY));
        if (lhs->as<PropertyByValue>().isSuper()) {
            if (!emitSuperElemOperands(lhs))
                return false;
            offset += 3;
        } else {
            if (!emitTree(lhs->pn_left))
                return false;
            if (!emitTree(lhs->pn_right))
                return false;
            offset += 2;
        }
        break;
      case PNK_ARRAY:
      case PNK_OBJECT:
        break;
      case PNK_CALL:
        MOZ_ASSERT(lhs->pn_xflags & PNX_SETCALL);
        if (!emitTree(lhs))
            return false;
        if (!emit1(JSOP_POP))
            return false;
        break;
      default:
        MOZ_ASSERT(0);
    }

    if (op != JSOP_NOP) {
        MOZ_ASSERT(rhs);
        switch (lhs->getKind()) {
          case PNK_NAME:
            if (lhs->isConst() && lhs->isOp(JSOP_CALLEE)) {
                if (!emit1(JSOP_CALLEE))
                    return false;
            } else if (lhs->isConst() && (lhs->isOp(JSOP_GETNAME) || lhs->isOp(JSOP_GETGNAME))) {
                if (!emitIndex32(lhs->getOp(), atomIndex))
                    return false;
            } else if (lhs->isOp(JSOP_SETNAME) || lhs->isOp(JSOP_STRICTSETNAME)) {
                if (!emit1(JSOP_DUP))
                    return false;
                if (!emitIndex32(JSOP_GETXPROP, atomIndex))
                    return false;
            } else if (lhs->isOp(JSOP_SETGNAME) || lhs->isOp(JSOP_STRICTSETGNAME)) {
                MOZ_ASSERT(lhs->pn_scopecoord.isFree());
                if (!emitAtomOp(lhs, JSOP_GETGNAME))
                    return false;
            } else if (lhs->isOp(JSOP_SETINTRINSIC)) {
                MOZ_ASSERT(lhs->pn_scopecoord.isFree());
                if (!emitAtomOp(lhs, JSOP_GETINTRINSIC))
                    return false;
            } else {
                JSOp op;
                switch (lhs->getOp()) {
                  case JSOP_SETARG: op = JSOP_GETARG; break;
                  case JSOP_SETLOCAL: op = JSOP_GETLOCAL; break;
                  case JSOP_SETALIASEDVAR: op = JSOP_GETALIASEDVAR; break;
                  default: MOZ_CRASH("Bad op");
                }
                if (!emitVarOp(lhs, op))
                    return false;
            }
            break;
          case PNK_DOT: {
            JSOp getOp;
            if (lhs->as<PropertyAccess>().isSuper()) {
                if (!emit1(JSOP_DUP2))
                    return false;
                getOp = JSOP_GETPROP_SUPER;
            } else {
                if (!emit1(JSOP_DUP))
                    return false;
                bool isLength = (lhs->pn_atom == cx->names().length);
                getOp = isLength ? JSOP_LENGTH : JSOP_GETPROP;
            }
            if (!emitIndex32(getOp, atomIndex))
                return false;
            break;
          }
          case PNK_ELEM: {
            JSOp elemOp;
            if (lhs->as<PropertyByValue>().isSuper()) {
                if (!emitDupAt(2))
                    return false;
                if (!emitDupAt(2))
                    return false;
                if (!emitDupAt(2))
                    return false;
                elemOp = JSOP_GETELEM_SUPER;
            } else {
                if (!emit1(JSOP_DUP2))
                    return false;
                elemOp = JSOP_GETELEM;
            }
            if (!emitElemOpBase(elemOp))
                return false;
            break;
          }
          case PNK_CALL:
            /*
             * We just emitted a JSOP_SETCALL (which will always throw) and
             * popped the call's return value. Push a random value to make sure
             * the stack depth is correct.
             */
            MOZ_ASSERT(lhs->pn_xflags & PNX_SETCALL);
            if (!emit1(JSOP_NULL))
                return false;
            break;
          default:;
        }
    }

    /* Now emit the right operand (it may affect the namespace). */
    if (rhs) {
        if (!emitTree(rhs))
            return false;
    } else {
        /*
         * The value to assign is the next enumeration value in a for-in or
         * for-of loop.  That value has already been emitted: by JSOP_ITERNEXT
         * in the for-in case, or via a GETPROP "value" on the result object in
         * the for-of case.  If offset == 1, that slot is already at the top of
         * the stack. Otherwise, rearrange the stack to put that value on top.
         */
        if (offset != 1 && !emit2(JSOP_PICK, offset - 1))
            return false;
    }

    /* If += etc., emit the binary operator with a source note. */
    if (op != JSOP_NOP) {
        if (!lhs->isKind(PNK_NAME)) {
            if (!newSrcNote(SRC_ASSIGNOP))
                return false;
        }
        if (!emit1(op))
            return false;
    }

    /* Finally, emit the specialized assignment bytecode. */
    switch (lhs->getKind()) {
      case PNK_NAME:
        if (lhs->isOp(JSOP_SETARG) || lhs->isOp(JSOP_SETLOCAL) || lhs->isOp(JSOP_SETALIASEDVAR)) {
            if (!emitVarOp(lhs, lhs->getOp()))
                return false;
        } else {
            if (!emitIndexOp(lhs->getOp(), atomIndex))
                return false;
        }
        break;
      case PNK_DOT:
      {
        JSOp setOp = lhs->as<PropertyAccess>().isSuper() ?
                       (sc->strict() ? JSOP_STRICTSETPROP_SUPER : JSOP_SETPROP_SUPER) :
                       (sc->strict() ? JSOP_STRICTSETPROP : JSOP_SETPROP);
        if (!emitIndexOp(setOp, atomIndex))
            return false;
        break;
      }
      case PNK_CALL:
        /* Do nothing. The JSOP_SETCALL we emitted will always throw. */
        MOZ_ASSERT(lhs->pn_xflags & PNX_SETCALL);
        break;
      case PNK_ELEM:
      {
        JSOp setOp = lhs->as<PropertyByValue>().isSuper() ?
                       sc->strict() ? JSOP_STRICTSETELEM_SUPER : JSOP_SETELEM_SUPER :
                       sc->strict() ? JSOP_STRICTSETELEM : JSOP_SETELEM;
        if (!emit1(setOp))
            return false;
        break;
      }
      case PNK_ARRAY:
      case PNK_OBJECT:
        if (!emitDestructuringOps(lhs))
            return false;
        break;
      default:
        MOZ_ASSERT(0);
    }
    return true;
}

bool
ParseNode::getConstantValue(ExclusiveContext* cx, AllowConstantObjects allowObjects, MutableHandleValue vp,
                            Value* compare, size_t ncompare, NewObjectKind newKind)
{
    MOZ_ASSERT(newKind == TenuredObject || newKind == SingletonObject);

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
        unsigned count;
        ParseNode* pn;

        if (allowObjects == DontAllowObjects) {
            vp.setMagic(JS_GENERIC_MAGIC);
            return true;
        }

        ObjectGroup::NewArrayKind arrayKind = ObjectGroup::NewArrayKind::Normal;
        if (allowObjects == ForCopyOnWriteArray) {
            arrayKind = ObjectGroup::NewArrayKind::CopyOnWrite;
            allowObjects = DontAllowObjects;
        }

        if (getKind() == PNK_CALLSITEOBJ) {
            count = pn_count - 1;
            pn = pn_head->pn_next;
        } else {
            MOZ_ASSERT(isOp(JSOP_NEWINIT) && !(pn_xflags & PNX_NONCONST));
            count = pn_count;
            pn = pn_head;
        }

        AutoValueVector values(cx);
        if (!values.appendN(MagicValue(JS_ELEMENTS_HOLE), count))
            return false;
        size_t idx;
        for (idx = 0; pn; idx++, pn = pn->pn_next) {
            if (!pn->getConstantValue(cx, allowObjects, values[idx], values.begin(), idx))
                return false;
            if (values[idx].isMagic(JS_GENERIC_MAGIC)) {
                vp.setMagic(JS_GENERIC_MAGIC);
                return true;
            }
        }
        MOZ_ASSERT(idx == count);

        JSObject* obj = ObjectGroup::newArrayObject(cx, values.begin(), values.length(),
                                                    newKind, arrayKind);
        if (!obj)
            return false;

        if (!CombineArrayElementTypes(cx, obj, compare, ncompare))
            return false;

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
        MOZ_ASSERT(allowObjects == AllowObjects);

        Rooted<IdValueVector> properties(cx, IdValueVector(cx));

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

            RootedId id(cx);
            if (!ValueToId<CanGC>(cx, idvalue, &id))
                return false;

            if (!properties.append(IdValuePair(id, value)))
                return false;
        }

        JSObject* obj = ObjectGroup::newPlainObject(cx, properties.begin(), properties.length(),
                                                    newKind);
        if (!obj)
            return false;

        if (!CombinePlainObjectPropertyTypes(cx, obj, compare, ncompare))
            return false;

        vp.setObject(*obj);
        return true;
      }
      default:
        MOZ_CRASH("Unexpected node");
    }
    return false;
}

bool
BytecodeEmitter::emitSingletonInitialiser(ParseNode* pn)
{
    NewObjectKind newKind = (pn->getKind() == PNK_OBJECT) ? SingletonObject : TenuredObject;

    RootedValue value(cx);
    if (!pn->getConstantValue(cx, ParseNode::AllowObjects, &value, nullptr, 0, newKind))
        return false;

    MOZ_ASSERT_IF(newKind == SingletonObject, value.toObject().isSingleton());

    ObjectBox* objbox = parser->newObjectBox(&value.toObject());
    if (!objbox)
        return false;

    return emitObjectOp(objbox, JSOP_OBJECT);
}

bool
BytecodeEmitter::emitCallSiteObject(ParseNode* pn)
{
    RootedValue value(cx);
    if (!pn->getConstantValue(cx, ParseNode::AllowObjects, &value))
        return false;

    MOZ_ASSERT(value.isObject());

    ObjectBox* objbox1 = parser->newObjectBox(&value.toObject());
    if (!objbox1)
        return false;

    if (!pn->as<CallSiteNode>().getRawArrayValue(cx, &value))
        return false;

    MOZ_ASSERT(value.isObject());

    ObjectBox* objbox2 = parser->newObjectBox(&value.toObject());
    if (!objbox2)
        return false;

    return emitObjectPairOp(objbox1, objbox2, JSOP_CALLSITEOBJ);
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

bool
BytecodeEmitter::emitCatch(ParseNode* pn)
{
    /*
     * Morph StmtType::BLOCK to StmtType::CATCH, note the block entry code offset,
     * and save the block object atom.
     */
    StmtInfoBCE* stmt = innermostStmt();
    MOZ_ASSERT(stmt->type == StmtType::BLOCK && stmt->isBlockScope);
    stmt->type = StmtType::CATCH;

    /* Go up one statement info record to the TRY or FINALLY record. */
    stmt = stmt->enclosing;
    MOZ_ASSERT(stmt->type == StmtType::TRY || stmt->type == StmtType::FINALLY);

    /* Pick up the pending exception and bind it to the catch variable. */
    if (!emit1(JSOP_EXCEPTION))
        return false;

    /*
     * Dup the exception object if there is a guard for rethrowing to use
     * it later when rethrowing or in other catches.
     */
    if (pn->pn_kid2 && !emit1(JSOP_DUP))
        return false;

    ParseNode* pn2 = pn->pn_kid1;
    switch (pn2->getKind()) {
      case PNK_ARRAY:
      case PNK_OBJECT:
        if (!emitDestructuringOps(pn2))
            return false;
        if (!emit1(JSOP_POP))
            return false;
        break;

      case PNK_NAME:
        /* Inline and specialize bindNameToSlot for pn2. */
        MOZ_ASSERT(!pn2->pn_scopecoord.isFree());
        if (!emitVarOp(pn2, JSOP_INITLEXICAL))
            return false;
        if (!emit1(JSOP_POP))
            return false;
        break;

      default:
        MOZ_ASSERT(0);
    }

    // If there is a guard expression, emit it and arrange to jump to the next
    // catch block if the guard expression is false.
    if (pn->pn_kid2) {
        if (!emitTree(pn->pn_kid2))
            return false;

        // If the guard expression is false, fall through, pop the block scope,
        // and jump to the next catch block.  Otherwise jump over that code and
        // pop the dupped exception.
        ptrdiff_t guardCheck;
        if (!emitJump(JSOP_IFNE, 0, &guardCheck))
            return false;

        {
            NonLocalExitScope nle(this);

            // Move exception back to cx->exception to prepare for
            // the next catch.
            if (!emit1(JSOP_THROWING))
                return false;

            // Leave the scope for this catch block.
            if (!nle.prepareForNonLocalJump(stmt))
                return false;

            // Jump to the next handler.  The jump target is backpatched by emitTry.
            ptrdiff_t guardJump;
            if (!emitJump(JSOP_GOTO, 0, &guardJump))
                return false;
            stmt->guardJump() = guardJump;
        }

        // Back to normal control flow.
        setJumpOffsetAt(guardCheck);

        // Pop duplicated exception object as we no longer need it.
        if (!emit1(JSOP_POP))
            return false;
    }

    /* Emit the catch body. */
    return emitTree(pn->pn_kid3);
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See the
// comment on EmitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitTry(ParseNode* pn)
{
    StmtInfoBCE stmtInfo(cx);

    // Push stmtInfo to track jumps-over-catches and gosubs-to-finally
    // for later fixup.
    //
    // When a finally block is active (StmtType::FINALLY in our parse context),
    // non-local jumps (including jumps-over-catches) result in a GOSUB
    // being written into the bytecode stream and fixed-up later (c.f.
    // emitBackPatchOp and backPatch).
    //
    pushStatement(&stmtInfo, pn->pn_kid3 ? StmtType::FINALLY : StmtType::TRY, offset());

    // Since an exception can be thrown at any place inside the try block,
    // we need to restore the stack and the scope chain before we transfer
    // the control to the exception handler.
    //
    // For that we store in a try note associated with the catch or
    // finally block the stack depth upon the try entry. The interpreter
    // uses this depth to properly unwind the stack and the scope chain.
    //
    int depth = stackDepth;

    // Record the try location, then emit the try block.
    unsigned noteIndex;
    if (!newSrcNote(SRC_TRY, &noteIndex))
        return false;
    if (!emit1(JSOP_TRY))
        return false;

    ptrdiff_t tryStart = offset();
    if (!emitTree(pn->pn_kid1))
        return false;
    MOZ_ASSERT(depth == stackDepth);

    // GOSUB to finally, if present.
    if (pn->pn_kid3) {
        if (!emitBackPatchOp(&stmtInfo.gosubs()))
            return false;
    }

    // Source note points to the jump at the end of the try block.
    if (!setSrcNoteOffset(noteIndex, 0, offset() - tryStart + JSOP_TRY_LENGTH))
        return false;

    // Emit jump over catch and/or finally.
    ptrdiff_t catchJump = -1;
    if (!emitBackPatchOp(&catchJump))
        return false;

    ptrdiff_t tryEnd = offset();

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
            MOZ_ASSERT(this->stackDepth == depth);

            // Clear the frame's return value that might have been set by the
            // try block:
            //
            //   eval("try { 1; throw 2 } catch(e) {}"); // undefined, not 1
            if (!emit1(JSOP_UNDEFINED))
                return false;
            if (!emit1(JSOP_SETRVAL))
                return false;

            // Emit the lexical scope and catch body.
            MOZ_ASSERT(pn3->isKind(PNK_LEXICALSCOPE));
            if (!emitTree(pn3))
                return false;

            // gosub <finally>, if required.
            if (pn->pn_kid3) {
                if (!emitBackPatchOp(&stmtInfo.gosubs()))
                    return false;
                MOZ_ASSERT(this->stackDepth == depth);
            }

            // Jump over the remaining catch blocks.  This will get fixed
            // up to jump to after catch/finally.
            if (!emitBackPatchOp(&catchJump))
                return false;

            // If this catch block had a guard clause, patch the guard jump to
            // come here.
            if (stmtInfo.guardJump() != -1) {
                setJumpOffsetAt(stmtInfo.guardJump());
                stmtInfo.guardJump() = -1;

                // If this catch block is the last one, rethrow, delegating
                // execution of any finally block to the exception handler.
                if (!pn3->pn_next) {
                    if (!emit1(JSOP_EXCEPTION))
                        return false;
                    if (!emit1(JSOP_THROW))
                        return false;
                }
            }
        }
    }

    MOZ_ASSERT(this->stackDepth == depth);

    // Emit the finally handler, if there is one.
    ptrdiff_t finallyStart = 0;
    if (pn->pn_kid3) {
        // Fix up the gosubs that might have been emitted before non-local
        // jumps to the finally code.
        backPatch(stmtInfo.gosubs(), code().end(), JSOP_GOSUB);

        finallyStart = offset();

        // Indicate that we're emitting a subroutine body.
        stmtInfo.type = StmtType::SUBROUTINE;
        if (!updateSourceCoordNotes(pn->pn_kid3->pn_pos.begin))
            return false;
        if (!emit1(JSOP_FINALLY))
            return false;
        if (!emit1(JSOP_GETRVAL))
            return false;

        // Clear the frame's return value to make break/continue return
        // correct value even if there's no other statement before them:
        //
        //   eval("x: try { 1 } finally { break x; }");  // undefined, not 1
        if (!emit1(JSOP_UNDEFINED))
            return false;
        if (!emit1(JSOP_SETRVAL))
            return false;

        if (!emitTree(pn->pn_kid3))
            return false;
        if (!emit1(JSOP_SETRVAL))
            return false;
        if (!emit1(JSOP_RETSUB))
            return false;
        hasTryFinally = true;
        MOZ_ASSERT(this->stackDepth == depth);
    }
    popStatement();

    // ReconstructPCStack needs a NOP here to mark the end of the last catch block.
    if (!emit1(JSOP_NOP))
        return false;

    // Fix up the end-of-try/catch jumps to come here.
    backPatch(catchJump, code().end(), JSOP_GOTO);

    // Add the try note last, to let post-order give us the right ordering
    // (first to last for a given nesting level, inner to outer by level).
    if (catchList && !tryNoteList.append(JSTRY_CATCH, depth, tryStart, tryEnd))
        return false;

    // If we've got a finally, mark try+catch region with additional
    // trynote to catch exceptions (re)thrown from a catch block or
    // for the try{}finally{} case.
    if (pn->pn_kid3 && !tryNoteList.append(JSTRY_FINALLY, depth, tryStart, finallyStart))
        return false;

    return true;
}

bool
BytecodeEmitter::emitIf(ParseNode* pn)
{
    StmtInfoBCE stmtInfo(cx);

    /* Initialize so we can detect else-if chains and avoid recursion. */
    stmtInfo.type = StmtType::IF;
    ptrdiff_t beq = -1;
    ptrdiff_t jmp = -1;
    unsigned noteIndex = -1;

  if_again:
    /* Emit code for the condition before pushing stmtInfo. */
    if (!emitTree(pn->pn_kid1))
        return false;
    ptrdiff_t top = offset();
    if (stmtInfo.type == StmtType::IF) {
        pushStatement(&stmtInfo, StmtType::IF, top);
    } else {
        /*
         * We came here from the goto further below that detects else-if
         * chains, so we must mutate stmtInfo back into a StmtType::IF record.
         * Also we need a note offset for SRC_IF_ELSE to help IonMonkey.
         */
        MOZ_ASSERT(stmtInfo.type == StmtType::ELSE);
        stmtInfo.type = StmtType::IF;
        stmtInfo.update = top;
        if (!setSrcNoteOffset(noteIndex, 0, jmp - beq))
            return false;
    }

    /* Emit an annotated branch-if-false around the then part. */
    ParseNode* pn3 = pn->pn_kid3;
    if (!newSrcNote(pn3 ? SRC_IF_ELSE : SRC_IF, &noteIndex))
        return false;
    if (!emitJump(JSOP_IFEQ, 0, &beq))
        return false;

    /* Emit code for the then and optional else parts. */
    if (!emitTree(pn->pn_kid2))
        return false;
    if (pn3) {
        /* Modify stmtInfo so we know we're in the else part. */
        stmtInfo.type = StmtType::ELSE;

        /*
         * Emit a JSOP_BACKPATCH op to jump from the end of our then part
         * around the else part.  The popStatement call at the bottom of
         * this function will fix up the backpatch chain linked from
         * stmtInfo.breaks.
         */
        if (!emitGoto(&stmtInfo, &stmtInfo.breaks))
            return false;
        jmp = stmtInfo.breaks;

        /* Ensure the branch-if-false comes here, then emit the else. */
        setJumpOffsetAt(beq);
        if (pn3->isKind(PNK_IF)) {
            pn = pn3;
            goto if_again;
        }

        if (!emitTree(pn3))
            return false;

        /*
         * Annotate SRC_IF_ELSE with the offset from branch to jump, for
         * IonMonkey's benefit.  We can't just "back up" from the pc
         * of the else clause, because we don't know whether an extended
         * jump was required to leap from the end of the then clause over
         * the else clause.
         */
        if (!setSrcNoteOffset(noteIndex, 0, jmp - beq))
            return false;
    } else {
        /* No else part, fixup the branch-if-false to come here. */
        setJumpOffsetAt(beq);
    }

    popStatement();
    return true;
}

/*
 * pnLet represents a let-statement: let (x = y) { ... }
 *
 */
/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
 * the comment on emitSwitch.
 */
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitLetBlock(ParseNode* pnLet)
{
    MOZ_ASSERT(pnLet->isArity(PN_BINARY));
    ParseNode* varList = pnLet->pn_left;
    MOZ_ASSERT(varList->isArity(PN_LIST));
    ParseNode* letBody = pnLet->pn_right;
    MOZ_ASSERT(letBody->isLexical() && letBody->isKind(PNK_LEXICALSCOPE));

    int letHeadDepth = this->stackDepth;

    if (!emitVariables(varList, PushInitialValues))
        return false;

    /* Push storage for hoisted let decls (e.g. 'let (x) { let y }'). */
    uint32_t valuesPushed = this->stackDepth - letHeadDepth;
    StmtInfoBCE stmtInfo(cx);
    if (!enterBlockScope(&stmtInfo, letBody->pn_objbox, JSOP_UNINITIALIZED, valuesPushed))
        return false;

    if (!emitTree(letBody->pn_expr))
        return false;

    if (!leaveNestedScope(&stmtInfo))
        return false;

    return true;
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitLexicalScope(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_LEXICALSCOPE));

    StmtInfoBCE stmtInfo(cx);
    if (!enterBlockScope(&stmtInfo, pn->pn_objbox, JSOP_UNINITIALIZED, 0))
        return false;

    if (!emitTree(pn->pn_expr))
        return false;

    if (!leaveNestedScope(&stmtInfo))
        return false;

    return true;
}

bool
BytecodeEmitter::emitWith(ParseNode* pn)
{
    StmtInfoBCE stmtInfo(cx);
    if (!emitTree(pn->pn_left))
        return false;
    if (!enterNestedScope(&stmtInfo, pn->pn_binary_obj, StmtType::WITH))
        return false;
    if (!emitTree(pn->pn_right))
        return false;
    if (!leaveNestedScope(&stmtInfo))
        return false;
    return true;
}

bool
BytecodeEmitter::emitRequireObjectCoercible()
{
    // For simplicity, handle this in self-hosted code, at cost of 13 bytes of
    // bytecode versus 1 byte for a dedicated opcode.  As more places need this
    // behavior, we may want to reconsider this tradeoff.

#ifdef DEBUG
    auto depth = this->stackDepth;
#endif
    MOZ_ASSERT(depth > 0);                 // VAL
    if (!emit1(JSOP_DUP))                  // VAL VAL
        return false;

    // Note that "intrinsic" is a misnomer: we're calling a *self-hosted*
    // function that's not an intrinsic!  But it nonetheless works as desired.
    if (!emitAtomOp(cx->names().RequireObjectCoercible,
                    JSOP_GETINTRINSIC))    // VAL VAL REQUIREOBJECTCOERCIBLE
    {
        return false;
    }
    if (!emit1(JSOP_UNDEFINED))            // VAL VAL REQUIREOBJECTCOERCIBLE UNDEFINED
        return false;
    if (!emit2(JSOP_PICK, 2))              // VAL REQUIREOBJECTCOERCIBLE UNDEFINED VAL
        return false;
    if (!emitCall(JSOP_CALL, 1))           // VAL IGNORED
        return false;
    checkTypeSet(JSOP_CALL);

    if (!emit1(JSOP_POP))                  // VAL
        return false;

    MOZ_ASSERT(depth == this->stackDepth);
    return true;
}

bool
BytecodeEmitter::emitIterator()
{
    // Convert iterable to iterator.
    if (!emit1(JSOP_DUP))                                         // OBJ OBJ
        return false;
    if (!emit2(JSOP_SYMBOL, uint8_t(JS::SymbolCode::iterator)))   // OBJ OBJ @@ITERATOR
        return false;
    if (!emitElemOpBase(JSOP_CALLELEM))                           // OBJ ITERFN
        return false;
    if (!emit1(JSOP_SWAP))                                        // ITERFN OBJ
        return false;
    if (!emitCall(JSOP_CALLITER, 0))                              // ITER
        return false;
    checkTypeSet(JSOP_CALLITER);
    return true;
}

bool
BytecodeEmitter::emitForInOrOfVariables(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_VAR) || pn->isKind(PNK_LET));

    // ES6 specifies that loop variables get a fresh binding in each iteration.
    // This is currently implemented for C-style for(;;) loops, but not
    // for-in/of loops, though a similar approach should work. See bug 449811.
    //
    // In `for (let x in/of EXPR)`, ES6 specifies that EXPR is evaluated in a
    // scope containing an uninitialized `x`. If EXPR accesses `x`, we should
    // get a ReferenceError due to the TDZ violation. This is not yet
    // implemented. See bug 1069480.
    //
    // If the left part is 'var x', emit code to define x if necessary using a
    // prologue opcode, but do not emit a pop. If it's 'let x', we initialize
    // the lets to not trigger dead zone checks, via InitializeVars.  (The
    // frontend currently assumes use of a 'let', dominated by its declaration,
    // needs no TDZ check, so we can't just fix the TDZ bug above by not
    // initializing here.)
    emittingForInit = true;
    if (pn->isKind(PNK_VAR)) {
        if (!emitVariables(pn, DefineVars))
            return false;
    } else {
        MOZ_ASSERT(pn->isKind(PNK_LET));
        if (!emitVariables(pn, InitializeVars))
            return false;
    }
    emittingForInit = false;

    return true;
}

bool
BytecodeEmitter::emitForOf(StmtType type, ParseNode* pn)
{
    MOZ_ASSERT(type == StmtType::FOR_OF_LOOP || type == StmtType::SPREAD);
#ifdef DEBUG
    if (type == StmtType::FOR_OF_LOOP) {
        MOZ_ASSERT(pn);
        MOZ_ASSERT(pn->pn_left->isKind(PNK_FOROF));
    } else {
        MOZ_ASSERT(!pn);
    }
#endif

    ptrdiff_t top = offset();
    ParseNode* forHead = pn ? pn->pn_left : nullptr;
    ParseNode* forHeadExpr = forHead ? forHead->pn_kid3 : nullptr;
    ParseNode* forBody = pn ? pn->pn_right : nullptr;

    ParseNode* loopDecl = forHead ? forHead->pn_kid1 : nullptr;
    if (loopDecl && !emitForInOrOfVariables(loopDecl))
        return false;

    if (type == StmtType::FOR_OF_LOOP) {
        // For-of loops run with two values on the stack: the iterator and the
        // current result object.

        // Compile the object expression to the right of 'of'.
        if (!emitTree(forHeadExpr))
            return false;
        if (!emitIterator())
            return false;

        // Push a dummy result so that we properly enter iteration midstream.
        if (!emit1(JSOP_UNDEFINED))                // ITER RESULT
            return false;
    }

    LoopStmtInfo stmtInfo(cx);
    pushLoopStatement(&stmtInfo, type, top);

    // Jump down to the loop condition to minimize overhead assuming at least
    // one iteration, as the other loop forms do.  Annotate so IonMonkey can
    // find the loop-closing jump.
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR_OF, &noteIndex))
        return false;
    ptrdiff_t jmp;
    if (!emitJump(JSOP_GOTO, 0, &jmp))
        return false;

    top = offset();
    stmtInfo.setTop(top);
    if (!emitLoopHead(nullptr))
        return false;

    if (type == StmtType::SPREAD)
        this->stackDepth++;

#ifdef DEBUG
    int loopDepth = this->stackDepth;
#endif

    // Emit code to assign result.value to the iteration variable.
    if (type == StmtType::FOR_OF_LOOP) {
        if (!emit1(JSOP_DUP))                             // ITER RESULT RESULT
            return false;
    }
    if (!emitAtomOp(cx->names().value, JSOP_GETPROP))     // ... RESULT VALUE
        return false;
    if (type == StmtType::FOR_OF_LOOP) {
        if (!emitAssignment(forHead->pn_kid2, JSOP_NOP, nullptr)) // ITER RESULT VALUE
            return false;
        if (!emit1(JSOP_POP))                             // ITER RESULT
            return false;

        // The stack should be balanced around the assignment opcode sequence.
        MOZ_ASSERT(this->stackDepth == loopDepth);

        // Emit code for the loop body.
        if (!emitTree(forBody))
            return false;

        // Set loop and enclosing "update" offsets, for continue.
        StmtInfoBCE* stmt = &stmtInfo;
        do {
            stmt->update = offset();
        } while ((stmt = stmt->enclosing) != nullptr && stmt->type == StmtType::LABEL);
    } else {
        if (!emit1(JSOP_INITELEM_INC))                    // ITER ARR (I+1)
            return false;

        MOZ_ASSERT(this->stackDepth == loopDepth - 1);

        // StmtType::SPREAD never contain continue, so do not set "update" offset.
    }

    // COME FROM the beginning of the loop to here.
    setJumpOffsetAt(jmp);
    if (!emitLoopEntry(forHeadExpr))
        return false;

    if (type == StmtType::FOR_OF_LOOP) {
        if (!emit1(JSOP_POP))                             // ITER
            return false;
        if (!emit1(JSOP_DUP))                             // ITER ITER
            return false;
    } else {
        if (!emitDupAt(2))                                // ITER ARR I ITER
            return false;
    }
    if (!emitIteratorNext(forHead))                       // ... RESULT
        return false;
    if (!emit1(JSOP_DUP))                                 // ... RESULT RESULT
        return false;
    if (!emitAtomOp(cx->names().done, JSOP_GETPROP))      // ... RESULT DONE?
        return false;

    ptrdiff_t beq;
    if (!emitJump(JSOP_IFEQ, top - offset(), &beq))       // ... RESULT
        return false;

    MOZ_ASSERT(this->stackDepth == loopDepth);

    // Let Ion know where the closing jump of this loop is.
    if (!setSrcNoteOffset(noteIndex, 0, beq - jmp))
        return false;

    // Fixup breaks and continues.
    // For StmtType::SPREAD, just pop innermostStmt().
    popStatement();

    if (!tryNoteList.append(JSTRY_FOR_OF, stackDepth, top, offset()))
        return false;

    if (type == StmtType::SPREAD) {
        if (!emit2(JSOP_PICK, 3))      // ARR I RESULT ITER
            return false;
    }

    // Pop the result and the iter.
    return emitUint16Operand(JSOP_POPN, 2);
}

bool
BytecodeEmitter::emitForIn(ParseNode* pn)
{
    ptrdiff_t top = offset();
    ParseNode* forHead = pn->pn_left;
    ParseNode* forBody = pn->pn_right;

    ParseNode* loopDecl = forHead->pn_kid1;
    if (loopDecl && !emitForInOrOfVariables(loopDecl))
        return false;

    /* Compile the object expression to the right of 'in'. */
    if (!emitTree(forHead->pn_kid3))
        return false;

    /*
     * Emit a bytecode to convert top of stack value to the iterator
     * object depending on the loop variant (for-in, for-each-in, or
     * destructuring for-in).
     */
    MOZ_ASSERT(pn->isOp(JSOP_ITER));
    if (!emit2(JSOP_ITER, (uint8_t) pn->pn_iflags))
        return false;

    // For-in loops have both the iterator and the value on the stack. Push
    // undefined to balance the stack.
    if (!emit1(JSOP_UNDEFINED))
        return false;

    LoopStmtInfo stmtInfo(cx);
    pushLoopStatement(&stmtInfo, StmtType::FOR_IN_LOOP, top);

    /* Annotate so IonMonkey can find the loop-closing jump. */
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR_IN, &noteIndex))
        return false;

    /*
     * Jump down to the loop condition to minimize overhead assuming at
     * least one iteration, as the other loop forms do.
     */
    ptrdiff_t jmp;
    if (!emitJump(JSOP_GOTO, 0, &jmp))
        return false;

    top = offset();
    stmtInfo.setTop(top);
    if (!emitLoopHead(nullptr))
        return false;

#ifdef DEBUG
    int loopDepth = this->stackDepth;
#endif

    // Emit code to assign the enumeration value to the left hand side, but
    // also leave it on the stack.
    if (!emitAssignment(forHead->pn_kid2, JSOP_NOP, nullptr))
        return false;

    /* The stack should be balanced around the assignment opcode sequence. */
    MOZ_ASSERT(this->stackDepth == loopDepth);

    /* Emit code for the loop body. */
    if (!emitTree(forBody))
        return false;

    /* Set loop and enclosing "update" offsets, for continue. */
    StmtInfoBCE* stmt = &stmtInfo;
    do {
        stmt->update = offset();
    } while ((stmt = stmt->enclosing) != nullptr && stmt->type == StmtType::LABEL);

    /*
     * Fixup the goto that starts the loop to jump down to JSOP_MOREITER.
     */
    setJumpOffsetAt(jmp);
    if (!emitLoopEntry(nullptr))
        return false;
    if (!emit1(JSOP_POP))
        return false;
    if (!emit1(JSOP_MOREITER))
        return false;
    if (!emit1(JSOP_ISNOITER))
        return false;
    ptrdiff_t beq;
    if (!emitJump(JSOP_IFEQ, top - offset(), &beq))
        return false;

    /* Set the srcnote offset so we can find the closing jump. */
    if (!setSrcNoteOffset(noteIndex, 0, beq - jmp))
        return false;

    // Fix up breaks and continues.
    popStatement();

    // Pop the enumeration value.
    if (!emit1(JSOP_POP))
        return false;

    if (!tryNoteList.append(JSTRY_FOR_IN, this->stackDepth, top, offset()))
        return false;
    if (!emit1(JSOP_ENDITER))
        return false;

    return true;
}

/* C-style `for (init; cond; update) ...` loop. */
bool
BytecodeEmitter::emitCStyleFor(ParseNode* pn)
{
    LoopStmtInfo stmtInfo(cx);
    pushLoopStatement(&stmtInfo, StmtType::FOR_LOOP, offset());

    ParseNode* forHead = pn->pn_left;
    ParseNode* forBody = pn->pn_right;

    // If the head of this for-loop declared any lexical variables, the parser
    // wrapped this PNK_FOR node in a PNK_LEXICALSCOPE representing the
    // implicit scope of those variables. By the time we get here, we have
    // already entered that scope. So far, so good.
    //
    // ### Scope freshening
    //
    // Each iteration of a `for (let V...)` loop creates a fresh loop variable
    // binding for V, even if the loop is a C-style `for(;;)` loop:
    //
    //     var funcs = [];
    //     for (let i = 0; i < 2; i++)
    //         funcs.push(function() { return i; });
    //     assertEq(funcs[0](), 0);  // the two closures capture...
    //     assertEq(funcs[1](), 1);  // ...two different `i` bindings
    //
    // This is implemented by "freshening" the implicit block -- changing the
    // scope chain to a fresh clone of the instantaneous block object -- each
    // iteration, just before evaluating the "update" in for(;;) loops.
    //
    // No freshening occurs in `for (const ...;;)` as there's no point: you
    // can't reassign consts. This is observable through the Debugger API. (The
    // ES6 spec also skips cloning the environment in this case.)
    bool forLoopRequiresFreshening = false;
    if (ParseNode* init = forHead->pn_kid1) {
        forLoopRequiresFreshening = init->isKind(PNK_LET);

        // Emit the `init` clause, whether it's an expression or a variable
        // declaration. (The loop variables were hoisted into an enclosing
        // scope, but we still need to emit code for the initializers.)
        emittingForInit = true;
        if (!updateSourceCoordNotes(init->pn_pos.begin))
            return false;
        if (!emitTree(init))
            return false;
        emittingForInit = false;

        if (!init->isKind(PNK_VAR) && !init->isKind(PNK_LET) && !init->isKind(PNK_CONST)) {
            // 'init' is an expression, not a declaration. emitTree left its
            // value on the stack.
            if (!emit1(JSOP_POP))
                return false;
        }
    }

    /*
     * NB: the SRC_FOR note has offsetBias 1 (JSOP_NOP_LENGTH).
     * Use tmp to hold the biased srcnote "top" offset, which differs
     * from the top local variable by the length of the JSOP_GOTO
     * emitted in between tmp and top if this loop has a condition.
     */
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR, &noteIndex))
        return false;
    if (!emit1(JSOP_NOP))
        return false;
    ptrdiff_t tmp = offset();

    ptrdiff_t jmp = -1;
    if (forHead->pn_kid2) {
        /* Goto the loop condition, which branches back to iterate. */
        if (!emitJump(JSOP_GOTO, 0, &jmp))
            return false;
    }

    ptrdiff_t top = offset();
    stmtInfo.setTop(top);

    /* Emit code for the loop body. */
    if (!emitLoopHead(forBody))
        return false;
    if (jmp == -1 && !emitLoopEntry(forBody))
        return false;
    if (!emitTree(forBody))
        return false;

    /* Set the second note offset so we can find the update part. */
    ptrdiff_t tmp2 = offset();

    // Set loop and enclosing "update" offsets, for continue.  Note that we
    // continue to immediately *before* the block-freshening: continuing must
    // refresh the block.
    StmtInfoBCE* stmt = &stmtInfo;
    do {
        stmt->update = offset();
    } while ((stmt = stmt->enclosing) != nullptr && stmt->type == StmtType::LABEL);

    // Freshen the block on the scope chain to expose distinct bindings for each loop
    // iteration.
    if (forLoopRequiresFreshening) {
        // The scope chain only includes an actual block *if* the scope object
        // is captured and therefore requires cloning.  Get the static block
        // object from the enclosing let-block statement (which *must* be the
        // let-statement for the guarding condition to have held) and freshen
        // if the block object needs cloning.
        StmtInfoBCE* enclosing = stmtInfo.enclosing;
        MOZ_ASSERT(enclosing->type == StmtType::BLOCK);
        MOZ_ASSERT(enclosing->isBlockScope);

        if (enclosing->staticScope->as<StaticBlockObject>().needsClone()) {
            if (!emit1(JSOP_FRESHENBLOCKSCOPE))
                return false;
        }
    }

    /* Check for update code to do before the condition (if any). */
    if (ParseNode* update = forHead->pn_kid3) {
        if (!updateSourceCoordNotes(update->pn_pos.begin))
            return false;
        if (!emitTree(update))
            return false;
        if (!emit1(JSOP_POP))
            return false;

        /* Restore the absolute line number for source note readers. */
        uint32_t lineNum = parser->tokenStream.srcCoords.lineNum(pn->pn_pos.end);
        if (currentLine() != lineNum) {
            if (!newSrcNote2(SRC_SETLINE, ptrdiff_t(lineNum)))
                return false;
            current->currentLine = lineNum;
            current->lastColumn = 0;
        }
    }

    ptrdiff_t tmp3 = offset();

    if (forHead->pn_kid2) {
        /* Fix up the goto from top to target the loop condition. */
        MOZ_ASSERT(jmp >= 0);
        setJumpOffsetAt(jmp);
        if (!emitLoopEntry(forHead->pn_kid2))
            return false;

        if (!emitTree(forHead->pn_kid2))
            return false;
    } else if (!forHead->pn_kid3) {
        // If there is no condition clause and no update clause, mark
        // the loop-ending "goto" with the location of the "for".
        // This ensures that the debugger will stop on each loop
        // iteration.
        if (!updateSourceCoordNotes(pn->pn_pos.begin))
            return false;
    }

    /* Set the first note offset so we can find the loop condition. */
    if (!setSrcNoteOffset(noteIndex, 0, tmp3 - tmp))
        return false;
    if (!setSrcNoteOffset(noteIndex, 1, tmp2 - tmp))
        return false;

    /* The third note offset helps us find the loop-closing jump. */
    if (!setSrcNoteOffset(noteIndex, 2, offset() - tmp))
        return false;

    /* If no loop condition, just emit a loop-closing jump. */
    if (!emitJump(forHead->pn_kid2 ? JSOP_IFNE : JSOP_GOTO, top - offset()))
        return false;

    if (!tryNoteList.append(JSTRY_LOOP, stackDepth, top, offset()))
        return false;

    /* Now fixup all breaks and continues. */
    popStatement();
    return true;
}

bool
BytecodeEmitter::emitFor(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_FOR));

    if (pn->pn_left->isKind(PNK_FORHEAD))
        return emitCStyleFor(pn);

    if (!updateLineNumberNotes(pn->pn_pos.begin))
        return false;

    if (pn->pn_left->isKind(PNK_FORIN))
        return emitForIn(pn);

    MOZ_ASSERT(pn->pn_left->isKind(PNK_FOROF));
    return emitForOf(StmtType::FOR_OF_LOOP, pn);
}

bool
BytecodeEmitter::emitComprehensionForInOrOfVariables(ParseNode* pn, bool* letBlockScope)
{
    // ES6 specifies that lexical for-loop variables get a fresh binding each
    // iteration, and that evaluation of the expression looped over occurs with
    // these variables uninitialized.  But these rules only apply to *standard*
    // for-in/of loops, and we haven't extended these requirements to
    // comprehension syntax.

    *letBlockScope = pn->isKind(PNK_LEXICALSCOPE);
    if (*letBlockScope) {
        // This is initially-ES7-tracked syntax, now with considerably
        // murkier outlook.  The |enterBlockScope()| precipitated by the
        // outparam-set here initializes the let-binding in
        // |emitComprehensionFor{In,Of}| with |undefined|, so there's nothing
        // to do here.
        MOZ_ASSERT(pn->isLexical());
    } else {
        // This is legacy comprehension syntax.  We'll have PNK_LET here, using
        // a lexical scope provided by/for the entire comprehension.  Name
        // analysis assumes declarations initialize lets, but as we're handling
        // this declaration manually, we must also initialize manually to avoid
        // triggering dead zone checks.
        MOZ_ASSERT(pn->isKind(PNK_LET));
        MOZ_ASSERT(pn->pn_count == 1);

        emittingForInit = true;
        if (!emitVariables(pn, InitializeVars))
            return false;
        emittingForInit = false;
    }

    return true;
}

bool
BytecodeEmitter::emitComprehensionForOf(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_COMPREHENSIONFOR));

    ParseNode* forHead = pn->pn_left;
    MOZ_ASSERT(forHead->isKind(PNK_FOROF));

    ParseNode* forHeadExpr = forHead->pn_kid3;
    ParseNode* forBody = pn->pn_right;

    ptrdiff_t top = offset();

    ParseNode* loopDecl = forHead->pn_kid1;
    bool letBlockScope = false;
    if (loopDecl && !emitComprehensionForInOrOfVariables(loopDecl, &letBlockScope))
        return false;

    // For-of loops run with two values on the stack: the iterator and the
    // current result object.

    // Compile the object expression to the right of 'of'.
    if (!emitTree(forHeadExpr))                // EXPR
        return false;
    if (!emitIterator())                       // ITER
        return false;

    // Push a dummy result so that we properly enter iteration midstream.
    if (!emit1(JSOP_UNDEFINED))                // ITER RESULT
        return false;

    // Enter the block before the loop body, after evaluating the obj.
    // Initialize let bindings with undefined when entering, as the name
    // assigned to is a plain assignment.
    StmtInfoBCE letStmt(cx);
    if (letBlockScope) {
        if (!enterBlockScope(&letStmt, loopDecl->pn_objbox, JSOP_UNDEFINED, 0))
            return false;
    }

    LoopStmtInfo stmtInfo(cx);
    pushLoopStatement(&stmtInfo, StmtType::FOR_OF_LOOP, top);

    // Jump down to the loop condition to minimize overhead assuming at least
    // one iteration, as the other loop forms do.  Annotate so IonMonkey can
    // find the loop-closing jump.
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR_OF, &noteIndex))
        return false;
    ptrdiff_t jmp;
    if (!emitJump(JSOP_GOTO, 0, &jmp))
        return false;

    top = offset();
    stmtInfo.setTop(top);
    if (!emitLoopHead(nullptr))
        return false;

#ifdef DEBUG
    int loopDepth = this->stackDepth;
#endif

    // Emit code to assign result.value to the iteration variable.
    if (!emit1(JSOP_DUP))                                 // ITER RESULT RESULT
        return false;
    if (!emitAtomOp(cx->names().value, JSOP_GETPROP))     // ITER RESULT VALUE
        return false;
    if (!emitAssignment(forHead->pn_kid2, JSOP_NOP, nullptr)) // ITER RESULT VALUE
        return false;
    if (!emit1(JSOP_POP))                                 // ITER RESULT
        return false;

    // The stack should be balanced around the assignment opcode sequence.
    MOZ_ASSERT(this->stackDepth == loopDepth);

    // Emit code for the loop body.
    if (!emitTree(forBody))
        return false;

    // Set loop and enclosing "update" offsets, for continue.
    StmtInfoBCE* stmt = &stmtInfo;
    do {
        stmt->update = offset();
    } while ((stmt = stmt->enclosing) != nullptr && stmt->type == StmtType::LABEL);

    // COME FROM the beginning of the loop to here.
    setJumpOffsetAt(jmp);
    if (!emitLoopEntry(forHeadExpr))
        return false;

    if (!emit1(JSOP_POP))                                 // ITER
        return false;
    if (!emit1(JSOP_DUP))                                 // ITER ITER
        return false;
    if (!emitIteratorNext(forHead))                       // ITER RESULT
        return false;
    if (!emit1(JSOP_DUP))                                 // ITER RESULT RESULT
        return false;
    if (!emitAtomOp(cx->names().done, JSOP_GETPROP))      // ITER RESULT DONE?
        return false;

    ptrdiff_t beq;
    if (!emitJump(JSOP_IFEQ, top - offset(), &beq))       // ITER RESULT
        return false;

    MOZ_ASSERT(this->stackDepth == loopDepth);

    // Let Ion know where the closing jump of this loop is.
    if (!setSrcNoteOffset(noteIndex, 0, beq - jmp))
        return false;

    // Fixup breaks and continues.
    popStatement();

    if (!tryNoteList.append(JSTRY_FOR_OF, stackDepth, top, offset()))
        return false;

    if (letBlockScope) {
        if (!leaveNestedScope(&letStmt))
            return false;
    }

    // Pop the result and the iter.
    return emitUint16Operand(JSOP_POPN, 2);               //
}

bool
BytecodeEmitter::emitComprehensionForIn(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_COMPREHENSIONFOR));

    ptrdiff_t top = offset();

    ParseNode* forHead = pn->pn_left;
    MOZ_ASSERT(forHead->isKind(PNK_FORIN));

    ParseNode* forBody = pn->pn_right;

    ParseNode* loopDecl = forHead->pn_kid1;
    bool letBlockScope = false;
    if (loopDecl && !emitComprehensionForInOrOfVariables(loopDecl, &letBlockScope))
        return false;

    /* Compile the object expression to the right of 'in'. */
    if (!emitTree(forHead->pn_kid3))
        return false;

    /*
     * Emit a bytecode to convert top of stack value to the iterator
     * object depending on the loop variant (for-in, for-each-in, or
     * destructuring for-in).
     */
    MOZ_ASSERT(pn->isOp(JSOP_ITER));
    if (!emit2(JSOP_ITER, (uint8_t) pn->pn_iflags))
        return false;

    // For-in loops have both the iterator and the value on the stack. Push
    // undefined to balance the stack.
    if (!emit1(JSOP_UNDEFINED))
        return false;

    // Enter the block before the loop body, after evaluating the obj.
    // Initialize let bindings with undefined when entering, as the name
    // assigned to is a plain assignment.
    StmtInfoBCE letStmt(cx);
    if (letBlockScope) {
        if (!enterBlockScope(&letStmt, loopDecl->pn_objbox, JSOP_UNDEFINED, 0))
            return false;
    }

    LoopStmtInfo stmtInfo(cx);
    pushLoopStatement(&stmtInfo, StmtType::FOR_IN_LOOP, top);

    /* Annotate so IonMonkey can find the loop-closing jump. */
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR_IN, &noteIndex))
        return false;

    /*
     * Jump down to the loop condition to minimize overhead assuming at
     * least one iteration, as the other loop forms do.
     */
    ptrdiff_t jmp;
    if (!emitJump(JSOP_GOTO, 0, &jmp))
        return false;

    top = offset();
    stmtInfo.setTop(top);
    if (!emitLoopHead(nullptr))
        return false;

#ifdef DEBUG
    int loopDepth = this->stackDepth;
#endif

    // Emit code to assign the enumeration value to the left hand side, but
    // also leave it on the stack.
    if (!emitAssignment(forHead->pn_kid2, JSOP_NOP, nullptr))
        return false;

    /* The stack should be balanced around the assignment opcode sequence. */
    MOZ_ASSERT(this->stackDepth == loopDepth);

    /* Emit code for the loop body. */
    if (!emitTree(forBody))
        return false;

    /* Set loop and enclosing "update" offsets, for continue. */
    StmtInfoBCE* stmt = &stmtInfo;
    do {
        stmt->update = offset();
    } while ((stmt = stmt->enclosing) != nullptr && stmt->type == StmtType::LABEL);

    /*
     * Fixup the goto that starts the loop to jump down to JSOP_MOREITER.
     */
    setJumpOffsetAt(jmp);
    if (!emitLoopEntry(nullptr))
        return false;
    if (!emit1(JSOP_POP))
        return false;
    if (!emit1(JSOP_MOREITER))
        return false;
    if (!emit1(JSOP_ISNOITER))
        return false;
    ptrdiff_t beq;
    if (!emitJump(JSOP_IFEQ, top - offset(), &beq))
        return false;

    /* Set the srcnote offset so we can find the closing jump. */
    if (!setSrcNoteOffset(noteIndex, 0, beq - jmp))
        return false;

    // Fix up breaks and continues.
    popStatement();

    // Pop the enumeration value.
    if (!emit1(JSOP_POP))
        return false;

    if (!tryNoteList.append(JSTRY_FOR_IN, this->stackDepth, top, offset()))
        return false;
    if (!emit1(JSOP_ENDITER))
        return false;

    if (letBlockScope) {
        if (!leaveNestedScope(&letStmt))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitComprehensionFor(ParseNode* compFor)
{
    MOZ_ASSERT(compFor->pn_left->isKind(PNK_FORIN) ||
               compFor->pn_left->isKind(PNK_FOROF));

    if (!updateLineNumberNotes(compFor->pn_pos.begin))
        return false;

    return compFor->pn_left->isKind(PNK_FORIN)
           ? emitComprehensionForIn(compFor)
           : emitComprehensionForOf(compFor);
}

MOZ_NEVER_INLINE bool
BytecodeEmitter::emitFunction(ParseNode* pn, bool needsProto)
{
    FunctionBox* funbox = pn->pn_funbox;
    RootedFunction fun(cx, funbox->function());
    MOZ_ASSERT_IF(fun->isInterpretedLazy(), fun->lazyScript());

    /*
     * Set the |wasEmitted| flag in the funbox once the function has been
     * emitted. Function definitions that need hoisting to the top of the
     * function will be seen by emitFunction in two places.
     */
    if (funbox->wasEmitted) {
        MOZ_ASSERT_IF(fun->hasScript(), fun->nonLazyScript());
        MOZ_ASSERT(pn->functionIsHoisted());
        MOZ_ASSERT(sc->isFunctionBox());
        return true;
    }

    funbox->wasEmitted = true;

    /*
     * Mark as singletons any function which will only be executed once, or
     * which is inner to a lambda we only expect to run once. In the latter
     * case, if the lambda runs multiple times then CloneFunctionObject will
     * make a deep clone of its contents.
     */
    if (fun->isInterpreted()) {
        bool singleton = checkRunOnceContext();
        if (!JSFunction::setTypeForScriptedFunction(cx, fun, singleton))
            return false;

        SharedContext* outersc = sc;
        if (fun->isInterpretedLazy()) {
            if (!fun->lazyScript()->sourceObject()) {
                JSObject* scope = innermostStaticScope();
                JSObject* source = script->sourceObject();
                fun->lazyScript()->setParent(scope, &source->as<ScriptSourceObject>());
            }
            if (emittingRunOnceLambda)
                fun->lazyScript()->setTreatAsRunOnce();
        } else {

            if (outersc->isFunctionBox() && outersc->asFunctionBox()->mightAliasLocals())
                funbox->setMightAliasLocals();      // inherit mightAliasLocals from parent
            MOZ_ASSERT_IF(outersc->strict(), funbox->strictScript);

            // Inherit most things (principals, version, etc) from the
            // parent.  Use default values for the rest.
            Rooted<JSScript*> parent(cx, script);
            MOZ_ASSERT(parent->getVersion() == parser->options().version);
            MOZ_ASSERT(parent->mutedErrors() == parser->options().mutedErrors());
            const TransitiveCompileOptions& transitiveOptions = parser->options();
            CompileOptions options(cx, transitiveOptions);

            Rooted<JSObject*> enclosingScope(cx, innermostStaticScope());
            Rooted<JSObject*> sourceObject(cx, script->sourceObject());
            Rooted<JSScript*> script(cx, JSScript::Create(cx, enclosingScope, false, options,
                                                          sourceObject,
                                                          funbox->bufStart, funbox->bufEnd));
            if (!script)
                return false;

            script->bindings = funbox->bindings;

            uint32_t lineNum = parser->tokenStream.srcCoords.lineNum(pn->pn_pos.begin);
            BytecodeEmitter bce2(this, parser, funbox, script, /* lazyScript = */ nullptr,
                                 insideEval, evalCaller,
                                 insideNonGlobalEval, lineNum, emitterMode);
            if (!bce2.init())
                return false;

            /* We measured the max scope depth when we parsed the function. */
            if (!bce2.emitFunctionScript(pn->pn_body))
                return false;

            if (funbox->usesArguments && funbox->usesApply && funbox->usesThis)
                script->setUsesArgumentsApplyAndThis();
        }
        if (outersc->isFunctionBox())
            outersc->asFunctionBox()->function()->nonLazyScript()->setHasInnerFunctions(true);
    } else {
        MOZ_ASSERT(IsAsmJSModuleNative(fun->native()));
    }

    /* Make the function object a literal in the outer script's pool. */
    unsigned index = objectList.add(pn->pn_funbox);

    /* Non-hoisted functions simply emit their respective op. */
    if (!pn->functionIsHoisted()) {
        /* JSOP_LAMBDA_ARROW is always preceded by a new.target */
        MOZ_ASSERT(fun->isArrow() == (pn->getOp() == JSOP_LAMBDA_ARROW));
        if (fun->isArrow()) {
            if (sc->allowNewTarget()) {
                if (!emit1(JSOP_NEWTARGET))
                    return false;
            } else {
                if (!emit1(JSOP_NULL))
                    return false;
            }
        }

        if (needsProto) {
            MOZ_ASSERT(pn->getOp() == JSOP_LAMBDA);
            pn->setOp(JSOP_FUNWITHPROTO);
        }
        return emitIndex32(pn->getOp(), index);
    }

    MOZ_ASSERT(!needsProto);

    /*
     * For scripts we put the bytecode for top-level functions in the prologue
     * to predefine their names in the variable object before the main code is
     * executed.
     *
     * Functions are fully parsed prior to invocation of the emitter and calls
     * to emitTree for function definitions are scheduled before generating
     * the rest of code.
     *
     * For modules, we record the function and instantiate the binding during
     * ModuleDeclarationInstantiation(), before the script is run.
     */
    if (sc->isGlobalContext()) {
        MOZ_ASSERT(pn->pn_scopecoord.isFree());
        MOZ_ASSERT(pn->getOp() == JSOP_NOP);
        MOZ_ASSERT(atBodyLevel());
        switchToPrologue();
        if (!emitIndex32(JSOP_DEFFUN, index))
            return false;
        if (!updateSourceCoordNotes(pn->pn_pos.begin))
            return false;
        switchToMain();
    } else if (sc->isFunctionBox()) {
#ifdef DEBUG
        BindingIter bi(script);
        while (bi->name() != fun->atom())
            bi++;
        MOZ_ASSERT(bi->kind() == Binding::VARIABLE || bi->kind() == Binding::CONSTANT ||
                   bi->kind() == Binding::ARGUMENT);
        MOZ_ASSERT(bi.argOrLocalIndex() < JS_BIT(20));
#endif
        if (!emitIndexOp(JSOP_LAMBDA, index))
            return false;
        MOZ_ASSERT(pn->getOp() == JSOP_GETLOCAL || pn->getOp() == JSOP_GETARG);
        JSOp setOp = pn->getOp() == JSOP_GETLOCAL ? JSOP_SETLOCAL : JSOP_SETARG;
        if (!emitVarOp(pn, setOp))
            return false;
        if (!emit1(JSOP_POP))
            return false;
    } else {
        RootedModuleObject module(cx, sc->asModuleBox()->module());
        RootedAtom name(cx, fun->atom());
        if (!module->noteFunctionDeclaration(cx, name, fun))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitDo(ParseNode* pn)
{
    /* Emit an annotated nop so IonBuilder can recognize the 'do' loop. */
    unsigned noteIndex;
    if (!newSrcNote(SRC_WHILE, &noteIndex))
        return false;
    if (!emit1(JSOP_NOP))
        return false;

    unsigned noteIndex2;
    if (!newSrcNote(SRC_WHILE, &noteIndex2))
        return false;

    /* Compile the loop body. */
    ptrdiff_t top = offset();
    if (!emitLoopHead(pn->pn_left))
        return false;

    LoopStmtInfo stmtInfo(cx);
    pushLoopStatement(&stmtInfo, StmtType::DO_LOOP, top);

    if (!emitLoopEntry(nullptr))
        return false;

    if (!emitTree(pn->pn_left))
        return false;

    /* Set loop and enclosing label update offsets, for continue. */
    ptrdiff_t off = offset();
    StmtInfoBCE* stmt = &stmtInfo;
    do {
        stmt->update = off;
    } while ((stmt = stmt->enclosing) != nullptr && stmt->type == StmtType::LABEL);

    /* Compile the loop condition, now that continues know where to go. */
    if (!emitTree(pn->pn_right))
        return false;

    ptrdiff_t beq;
    if (!emitJump(JSOP_IFNE, top - offset(), &beq))
        return false;

    if (!tryNoteList.append(JSTRY_LOOP, stackDepth, top, offset()))
        return false;

    /*
     * Update the annotations with the update and back edge positions, for
     * IonBuilder.
     *
     * Be careful: We must set noteIndex2 before noteIndex in case the noteIndex
     * note gets bigger.
     */
    if (!setSrcNoteOffset(noteIndex2, 0, beq - top))
        return false;
    if (!setSrcNoteOffset(noteIndex, 0, 1 + (off - top)))
        return false;

    popStatement();
    return true;
}

bool
BytecodeEmitter::emitWhile(ParseNode* pn)
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

    // If we have a single-line while, like "while (x) ;", we want to
    // emit the line note before the initial goto, so that the
    // debugger sees a single entry point.  This way, if there is a
    // breakpoint on the line, it will only fire once; and "next"ing
    // will skip the whole loop.  However, for the multi-line case we
    // want to emit the line note after the initial goto, so that
    // "cont" stops on each iteration -- but without a stop before the
    // first iteration.
    if (parser->tokenStream.srcCoords.lineNum(pn->pn_pos.begin) ==
        parser->tokenStream.srcCoords.lineNum(pn->pn_pos.end) &&
        !updateSourceCoordNotes(pn->pn_pos.begin))
        return false;

    LoopStmtInfo stmtInfo(cx);
    pushLoopStatement(&stmtInfo, StmtType::WHILE_LOOP, offset());

    unsigned noteIndex;
    if (!newSrcNote(SRC_WHILE, &noteIndex))
        return false;

    ptrdiff_t jmp;
    if (!emitJump(JSOP_GOTO, 0, &jmp))
        return false;

    ptrdiff_t top = offset();
    if (!emitLoopHead(pn->pn_right))
        return false;

    if (!emitTree(pn->pn_right))
        return false;

    setJumpOffsetAt(jmp);
    if (!emitLoopEntry(pn->pn_left))
        return false;
    if (!emitTree(pn->pn_left))
        return false;

    ptrdiff_t beq;
    if (!emitJump(JSOP_IFNE, top - offset(), &beq))
        return false;

    if (!tryNoteList.append(JSTRY_LOOP, stackDepth, top, offset()))
        return false;

    if (!setSrcNoteOffset(noteIndex, 0, beq - jmp))
        return false;

    popStatement();
    return true;
}

bool
BytecodeEmitter::emitBreak(PropertyName* label)
{
    StmtInfoBCE* stmt = innermostStmt();
    SrcNoteType noteType;
    if (label) {
        while (stmt->type != StmtType::LABEL || stmt->label != label)
            stmt = stmt->enclosing;
        noteType = SRC_BREAK2LABEL;
    } else {
        while (!stmt->isLoop() && stmt->type != StmtType::SWITCH)
            stmt = stmt->enclosing;
        noteType = (stmt->type == StmtType::SWITCH) ? SRC_SWITCHBREAK : SRC_BREAK;
    }

    return emitGoto(stmt, &stmt->breaks, noteType);
}

bool
BytecodeEmitter::emitContinue(PropertyName* label)
{
    StmtInfoBCE* stmt = innermostStmt();
    if (label) {
        /* Find the loop statement enclosed by the matching label. */
        StmtInfoBCE* loop = nullptr;
        while (stmt->type != StmtType::LABEL || stmt->label != label) {
            if (stmt->isLoop())
                loop = stmt;
            stmt = stmt->enclosing;
        }
        stmt = loop;
    } else {
        while (!stmt->isLoop())
            stmt = stmt->enclosing;
    }

    return emitGoto(stmt, &stmt->continues, SRC_CONTINUE);
}


bool
BytecodeEmitter::emitGetFunctionThis(ParseNode* pn)
{
    MOZ_ASSERT(sc->thisBinding() == ThisBinding::Function);
    MOZ_ASSERT(pn->isKind(PNK_NAME));
    MOZ_ASSERT(pn->name() == cx->names().dotThis);

    if (!emitTree(pn))
        return false;
    if (sc->needsThisTDZChecks() && !emit1(JSOP_CHECKTHIS))
        return false;

    return true;
}

bool
BytecodeEmitter::emitGetThisForSuperBase(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_SUPERBASE));
    return emitGetFunctionThis(pn->pn_kid);
}

bool
BytecodeEmitter::emitThisLiteral(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_THIS));

    if (ParseNode* thisName = pn->pn_kid)
        return emitGetFunctionThis(thisName);

    if (sc->thisBinding() == ThisBinding::Module)
        return emit1(JSOP_UNDEFINED);

    MOZ_ASSERT(sc->thisBinding() == ThisBinding::Global);
    return emit1(JSOP_GLOBALTHIS);
}

bool
BytecodeEmitter::emitLoadFromTopScope(BindingIter& bi)
{
    if (script->bindingIsAliased(bi)) {
        ScopeCoordinate sc;
        sc.setHops(0);
        sc.setSlot(0);
        MOZ_ALWAYS_TRUE(lookupAliasedNameSlot(bi->name(), &sc));
        return emitAliasedVarOp(JSOP_GETALIASEDVAR, sc, DontCheckLexical);
    }

    return emitUnaliasedVarOp(JSOP_GETLOCAL, bi.localIndex(), DontCheckLexical);
}

bool
BytecodeEmitter::emitStoreToTopScope(BindingIter& bi)
{
    if (script->bindingIsAliased(bi)) {
        ScopeCoordinate sc;
        sc.setHops(0);
        sc.setSlot(0);  // initialize to silence GCC warning
        MOZ_ALWAYS_TRUE(lookupAliasedNameSlot(bi->name(), &sc));
        return emitAliasedVarOp(JSOP_SETALIASEDVAR, sc, DontCheckLexical);
    }

    return emitUnaliasedVarOp(JSOP_SETLOCAL, bi.localIndex(), DontCheckLexical);
}

bool
BytecodeEmitter::emitReturn(ParseNode* pn)
{
    if (!updateSourceCoordNotes(pn->pn_pos.begin))
        return false;

    if (sc->isFunctionBox() && sc->asFunctionBox()->isStarGenerator()) {
        if (!emitPrepareIteratorResult())
            return false;
    }

    /* Push a return value */
    if (ParseNode* pn2 = pn->pn_kid) {
        if (!emitTree(pn2))
            return false;
    } else {
        /* No explicit return value provided */
        if (!emit1(JSOP_UNDEFINED))
            return false;
    }

    if (sc->isFunctionBox() && sc->asFunctionBox()->isStarGenerator()) {
        if (!emitFinishIteratorResult(true))
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
    ptrdiff_t top = offset();

    bool isGenerator = sc->isFunctionBox() && sc->asFunctionBox()->isGenerator();
    bool isDerivedClassConstructor =
        sc->isFunctionBox() && sc->asFunctionBox()->isDerivedClassConstructor();

    if (!emit1((isGenerator || isDerivedClassConstructor) ? JSOP_SETRVAL : JSOP_RETURN))
        return false;

    NonLocalExitScope nle(this);

    if (!nle.prepareForNonLocalJump(nullptr))
        return false;

    if (isGenerator) {
        ScopeCoordinate sc;
        // We know that .generator is on the top scope chain node, as we just
        // exited nested scopes.
        sc.setHops(0);
        MOZ_ALWAYS_TRUE(lookupAliasedNameSlot(cx->names().dotGenerator, &sc));
        if (!emitAliasedVarOp(JSOP_GETALIASEDVAR, sc, DontCheckLexical))
            return false;
        if (!emitYieldOp(JSOP_FINALYIELDRVAL))
            return false;
    } else if (isDerivedClassConstructor) {
        MOZ_ASSERT(code()[top] == JSOP_SETRVAL);
        BindingIter bi = Bindings::thisBinding(cx, script);
        if (!emitLoadFromTopScope(bi))
            return false;
        if (!emit1(JSOP_CHECKRETURN))
            return false;
        if (!emit1(JSOP_RETRVAL))
            return false;
    } else if (top + static_cast<ptrdiff_t>(JSOP_RETURN_LENGTH) != offset()) {
        code()[top] = JSOP_SETRVAL;
        if (!emit1(JSOP_RETRVAL))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitYield(ParseNode* pn)
{
    MOZ_ASSERT(sc->isFunctionBox());

    if (pn->getOp() == JSOP_YIELD) {
        if (sc->asFunctionBox()->isStarGenerator()) {
            if (!emitPrepareIteratorResult())
                return false;
        }
        if (pn->pn_left) {
            if (!emitTree(pn->pn_left))
                return false;
        } else {
            if (!emit1(JSOP_UNDEFINED))
                return false;
        }
        if (sc->asFunctionBox()->isStarGenerator()) {
            if (!emitFinishIteratorResult(false))
                return false;
        }
    } else {
        MOZ_ASSERT(pn->getOp() == JSOP_INITIALYIELD);
    }

    if (!emitTree(pn->pn_right))
        return false;

    if (!emitYieldOp(pn->getOp()))
        return false;

    if (pn->getOp() == JSOP_INITIALYIELD && !emit1(JSOP_POP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitYieldStar(ParseNode* iter, ParseNode* gen)
{
    MOZ_ASSERT(sc->isFunctionBox());
    MOZ_ASSERT(sc->asFunctionBox()->isStarGenerator());

    if (!emitTree(iter))                                         // ITERABLE
        return false;
    if (!emitIterator())                                         // ITER
        return false;

    // Initial send value is undefined.
    if (!emit1(JSOP_UNDEFINED))                                  // ITER RECEIVED
        return false;

    int depth = stackDepth;
    MOZ_ASSERT(depth >= 2);

    ptrdiff_t initialSend = -1;
    if (!emitBackPatchOp(&initialSend))                          // goto initialSend
        return false;

    // Try prologue.                                             // ITER RESULT
    StmtInfoBCE stmtInfo(cx);
    pushStatement(&stmtInfo, StmtType::TRY, offset());
    unsigned noteIndex;
    if (!newSrcNote(SRC_TRY, &noteIndex))
        return false;
    ptrdiff_t tryStart = offset();                               // tryStart:
    if (!emit1(JSOP_TRY))
        return false;
    MOZ_ASSERT(this->stackDepth == depth);

    // Load the generator object.
    if (!emitTree(gen))                                          // ITER RESULT GENOBJ
        return false;

    // Yield RESULT as-is, without re-boxing.
    if (!emitYieldOp(JSOP_YIELD))                                // ITER RECEIVED
        return false;

    // Try epilogue.
    if (!setSrcNoteOffset(noteIndex, 0, offset() - tryStart))
        return false;
    ptrdiff_t subsequentSend = -1;
    if (!emitBackPatchOp(&subsequentSend))                       // goto subsequentSend
        return false;
    ptrdiff_t tryEnd = offset();                                 // tryEnd:

    // Catch location.
    stackDepth = uint32_t(depth);                                // ITER RESULT
    if (!emit1(JSOP_POP))                                        // ITER
        return false;
    // THROW? = 'throw' in ITER
    if (!emit1(JSOP_EXCEPTION))                                  // ITER EXCEPTION
        return false;
    if (!emit1(JSOP_SWAP))                                       // EXCEPTION ITER
        return false;
    if (!emit1(JSOP_DUP))                                        // EXCEPTION ITER ITER
        return false;
    if (!emitAtomOp(cx->names().throw_, JSOP_STRING))            // EXCEPTION ITER ITER "throw"
        return false;
    if (!emit1(JSOP_SWAP))                                       // EXCEPTION ITER "throw" ITER
        return false;
    if (!emit1(JSOP_IN))                                         // EXCEPTION ITER THROW?
        return false;
    // if (THROW?) goto delegate
    ptrdiff_t checkThrow;
    if (!emitJump(JSOP_IFNE, 0, &checkThrow))                    // EXCEPTION ITER
        return false;
    if (!emit1(JSOP_POP))                                        // EXCEPTION
        return false;
    if (!emit1(JSOP_THROW))                                      // throw EXCEPTION
        return false;

    setJumpOffsetAt(checkThrow);                                 // delegate:
    // RESULT = ITER.throw(EXCEPTION)                            // EXCEPTION ITER
    stackDepth = uint32_t(depth);
    if (!emit1(JSOP_DUP))                                        // EXCEPTION ITER ITER
        return false;
    if (!emit1(JSOP_DUP))                                        // EXCEPTION ITER ITER ITER
        return false;
    if (!emitAtomOp(cx->names().throw_, JSOP_CALLPROP))          // EXCEPTION ITER ITER THROW
        return false;
    if (!emit1(JSOP_SWAP))                                       // EXCEPTION ITER THROW ITER
        return false;
    if (!emit2(JSOP_PICK, 3))                                    // ITER THROW ITER EXCEPTION
        return false;
    if (!emitCall(JSOP_CALL, 1, iter))                           // ITER RESULT
        return false;
    checkTypeSet(JSOP_CALL);
    MOZ_ASSERT(this->stackDepth == depth);
    ptrdiff_t checkResult = -1;
    if (!emitBackPatchOp(&checkResult))                          // goto checkResult
        return false;

    // Catch epilogue.
    popStatement();

    // This is a peace offering to ReconstructPCStack.  See the note in EmitTry.
    if (!emit1(JSOP_NOP))
        return false;
    if (!tryNoteList.append(JSTRY_CATCH, depth, tryStart + JSOP_TRY_LENGTH, tryEnd))
        return false;

    // After the try/catch block: send the received value to the iterator.
    backPatch(initialSend, code().end(), JSOP_GOTO);  // initialSend:
    backPatch(subsequentSend, code().end(), JSOP_GOTO); // subsequentSend:

    // Send location.
    // result = iter.next(received)                              // ITER RECEIVED
    if (!emit1(JSOP_SWAP))                                       // RECEIVED ITER
        return false;
    if (!emit1(JSOP_DUP))                                        // RECEIVED ITER ITER
        return false;
    if (!emit1(JSOP_DUP))                                        // RECEIVED ITER ITER ITER
        return false;
    if (!emitAtomOp(cx->names().next, JSOP_CALLPROP))            // RECEIVED ITER ITER NEXT
        return false;
    if (!emit1(JSOP_SWAP))                                       // RECEIVED ITER NEXT ITER
        return false;
    if (!emit2(JSOP_PICK, 3))                                    // ITER NEXT ITER RECEIVED
        return false;
    if (!emitCall(JSOP_CALL, 1, iter))                           // ITER RESULT
        return false;
    checkTypeSet(JSOP_CALL);
    MOZ_ASSERT(this->stackDepth == depth);

    backPatch(checkResult, code().end(), JSOP_GOTO);             // checkResult:

    // if (!result.done) goto tryStart;                          // ITER RESULT
    if (!emit1(JSOP_DUP))                                        // ITER RESULT RESULT
        return false;
    if (!emitAtomOp(cx->names().done, JSOP_GETPROP))             // ITER RESULT DONE
        return false;
    // if (!DONE) goto tryStart;
    if (!emitJump(JSOP_IFEQ, tryStart - offset()))               // ITER RESULT
        return false;

    // result.value
    if (!emit1(JSOP_SWAP))                                       // RESULT ITER
        return false;
    if (!emit1(JSOP_POP))                                        // RESULT
        return false;
    if (!emitAtomOp(cx->names().value, JSOP_GETPROP))            // VALUE
        return false;

    MOZ_ASSERT(this->stackDepth == depth - 1);

    return true;
}

bool
BytecodeEmitter::emitStatementList(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    for (ParseNode* pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
        if (!emitTree(pn2))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitStatement(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_SEMI));

    ParseNode* pn2 = pn->pn_kid;
    if (!pn2)
        return true;

    if (!updateSourceCoordNotes(pn->pn_pos.begin))
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
    if (sc->isFunctionBox())
        MOZ_ASSERT(!script->noScriptRval());
    else
        useful = wantval = !script->noScriptRval();

    /* Don't eliminate expressions with side effects. */
    if (!useful) {
        if (!checkSideEffects(pn2, &useful))
            return false;

        /*
         * Don't eliminate apparently useless expressions if they are
         * labeled expression statements.  The innermostStmt()->update test
         * catches the case where we are nesting in emitTree for a labeled
         * compound statement.
         */
        if (innermostStmt() &&
            innermostStmt()->type == StmtType::LABEL &&
            innermostStmt()->update >= offset())
        {
            useful = true;
        }
    }

    if (useful) {
        JSOp op = wantval ? JSOP_SETRVAL : JSOP_POP;
        MOZ_ASSERT_IF(pn2->isKind(PNK_ASSIGN), pn2->isOp(JSOP_NOP));
        if (!emitTree(pn2))
            return false;
        if (!emit1(op))
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
                if (!sc->strictScript)
                    directive = js_useStrict_str;
            } else if (atom == cx->names().useAsm) {
                if (sc->isFunctionBox()) {
                    JSFunction* fun = sc->asFunctionBox()->function();
                    if (fun->isNative() && IsAsmJSModuleNative(fun->native()))
                        directive = js_useAsm_str;
                }
            }

            if (directive) {
                if (!reportStrictWarning(pn2, JSMSG_CONTRARY_NONDIRECTIVE, directive))
                    return false;
            }
        } else {
            current->currentLine = parser->tokenStream.srcCoords.lineNum(pn2->pn_pos.begin);
            current->lastColumn = 0;
            if (!reportStrictWarning(pn2, JSMSG_USELESS_EXPR))
                return false;
        }
    }

    return true;
}

bool
BytecodeEmitter::emitDeleteName(ParseNode* node)
{
    MOZ_ASSERT(node->isKind(PNK_DELETENAME));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode* nameExpr = node->pn_kid;
    MOZ_ASSERT(nameExpr->isKind(PNK_NAME));

    if (!bindNameToSlot(nameExpr))
        return false;

    MOZ_ASSERT(nameExpr->isOp(JSOP_DELNAME));
    return emitAtomOp(nameExpr, JSOP_DELNAME);
}

bool
BytecodeEmitter::emitDeleteProperty(ParseNode* node)
{
    MOZ_ASSERT(node->isKind(PNK_DELETEPROP));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode* propExpr = node->pn_kid;
    MOZ_ASSERT(propExpr->isKind(PNK_DOT));

    if (propExpr->as<PropertyAccess>().isSuper()) {
        // Still have to calculate the base, even though we are are going
        // to throw unconditionally, as calculating the base could also
        // throw.
        if (!emit1(JSOP_SUPERBASE))
            return false;

        return emitUint16Operand(JSOP_THROWMSG, JSMSG_CANT_DELETE_SUPER);
    }

    JSOp delOp = sc->strict() ? JSOP_STRICTDELPROP : JSOP_DELPROP;
    return emitPropOp(propExpr, delOp);
}

bool
BytecodeEmitter::emitDeleteElement(ParseNode* node)
{
    MOZ_ASSERT(node->isKind(PNK_DELETEELEM));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode* elemExpr = node->pn_kid;
    MOZ_ASSERT(elemExpr->isKind(PNK_ELEM));

    if (elemExpr->as<PropertyByValue>().isSuper()) {
        // Still have to calculate everything, even though we're gonna throw
        // since it may have side effects
        if (!emitTree(elemExpr->pn_right))
            return false;

        if (!emit1(JSOP_SUPERBASE))
            return false;
        if (!emitUint16Operand(JSOP_THROWMSG, JSMSG_CANT_DELETE_SUPER))
            return false;

        // Another wrinkle: Balance the stack from the emitter's point of view.
        // Execution will not reach here, as the last bytecode threw.
        return emit1(JSOP_POP);
    }

    JSOp delOp = sc->strict() ? JSOP_STRICTDELELEM : JSOP_DELELEM;
    return emitElemOp(elemExpr, delOp);
}

bool
BytecodeEmitter::emitDeleteExpression(ParseNode* node)
{
    MOZ_ASSERT(node->isKind(PNK_DELETEEXPR));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode* expression = node->pn_kid;

    // If useless, just emit JSOP_TRUE; otherwise convert |delete <expr>| to
    // effectively |<expr>, true|.
    bool useful = false;
    if (!checkSideEffects(expression, &useful))
        return false;

    if (useful) {
        MOZ_ASSERT_IF(expression->isKind(PNK_CALL), !(expression->pn_xflags & PNX_SETCALL));
        if (!emitTree(expression))
            return false;
        if (!emit1(JSOP_POP))
            return false;
    }

    return emit1(JSOP_TRUE);
}

bool
BytecodeEmitter::emitSelfHostedCallFunction(ParseNode* pn)
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
        reportError(pn, JSMSG_MORE_ARGS_NEEDED, "callFunction", "1", "s");
        return false;
    }

    ParseNode* pn2 = pn->pn_head;
    ParseNode* funNode = pn2->pn_next;
    if (!emitTree(funNode))
        return false;

    ParseNode* thisArg = funNode->pn_next;
    if (!emitTree(thisArg))
        return false;

    bool oldEmittingForInit = emittingForInit;
    emittingForInit = false;

    for (ParseNode* argpn = thisArg->pn_next; argpn; argpn = argpn->pn_next) {
        if (!emitTree(argpn))
            return false;
    }

    emittingForInit = oldEmittingForInit;

    uint32_t argc = pn->pn_count - 3;
    if (!emitCall(pn->getOp(), argc))
        return false;

    checkTypeSet(pn->getOp());
    return true;
}

bool
BytecodeEmitter::emitSelfHostedResumeGenerator(ParseNode* pn)
{
    // Syntax: resumeGenerator(gen, value, 'next'|'throw'|'close')
    if (pn->pn_count != 4) {
        reportError(pn, JSMSG_MORE_ARGS_NEEDED, "resumeGenerator", "1", "s");
        return false;
    }

    ParseNode* funNode = pn->pn_head;  // The resumeGenerator node.

    ParseNode* genNode = funNode->pn_next;
    if (!emitTree(genNode))
        return false;

    ParseNode* valNode = genNode->pn_next;
    if (!emitTree(valNode))
        return false;

    ParseNode* kindNode = valNode->pn_next;
    MOZ_ASSERT(kindNode->isKind(PNK_STRING));
    uint16_t operand = GeneratorObject::getResumeKind(cx, kindNode->pn_atom);
    MOZ_ASSERT(!kindNode->pn_next);

    if (!emitCall(JSOP_RESUME, operand))
        return false;

    return true;
}

bool
BytecodeEmitter::emitSelfHostedForceInterpreter(ParseNode* pn)
{
    if (!emit1(JSOP_FORCEINTERPRETER))
        return false;
    if (!emit1(JSOP_UNDEFINED))
        return false;
    return true;
}

bool
BytecodeEmitter::emitCallOrNew(ParseNode* pn)
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
        parser->tokenStream.reportError(callop
                                        ? JSMSG_TOO_MANY_FUN_ARGS
                                        : JSMSG_TOO_MANY_CON_ARGS);
        return false;
    }

    ParseNode* pn2 = pn->pn_head;
    bool spread = JOF_OPTYPE(pn->getOp()) == JOF_BYTE;
    switch (pn2->getKind()) {
      case PNK_NAME:
        if (emitterMode == BytecodeEmitter::SelfHosting && !spread) {
            // We shouldn't see foo(bar) = x in self-hosted code.
            MOZ_ASSERT(!(pn->pn_xflags & PNX_SETCALL));

            // Calls to "forceInterpreter", "callFunction" or "resumeGenerator"
            // in self-hosted code generate inline bytecode.
            if (pn2->name() == cx->names().callFunction)
                return emitSelfHostedCallFunction(pn);
            if (pn2->name() == cx->names().resumeGenerator)
                return emitSelfHostedResumeGenerator(pn);
            if (pn2->name() == cx->names().forceInterpreter)
                return emitSelfHostedForceInterpreter(pn);
            // Fall through.
        }
        if (!emitNameOp(pn2, callop))
            return false;
        break;
      case PNK_DOT:
        if (pn2->as<PropertyAccess>().isSuper()) {
            if (!emitSuperPropOp(pn2, JSOP_GETPROP_SUPER, /* isCall = */ callop))
                return false;
        } else {
            if (!emitPropOp(pn2, callop ? JSOP_CALLPROP : JSOP_GETPROP))
                return false;
        }
        break;
      case PNK_ELEM:
        if (pn2->as<PropertyByValue>().isSuper()) {
            if (!emitSuperElemOp(pn2, JSOP_GETELEM_SUPER, /* isCall = */ callop))
                return false;
        } else {
            if (!emitElemOp(pn2, callop ? JSOP_CALLELEM : JSOP_GETELEM))
                return false;
            if (callop) {
                if (!emit1(JSOP_SWAP))
                    return false;
            }
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
        MOZ_ASSERT(!emittingRunOnceLambda);
        if (checkRunOnceContext()) {
            emittingRunOnceLambda = true;
            if (!emitTree(pn2))
                return false;
            emittingRunOnceLambda = false;
        } else {
            if (!emitTree(pn2))
                return false;
        }
        callop = false;
        break;
      case PNK_SUPERBASE:
        MOZ_ASSERT(pn->isKind(PNK_SUPERCALL));
        MOZ_ASSERT(parser->handler.isSuperBase(pn2));
        if (!emit1(JSOP_SUPERFUN))
            return false;
        break;
      default:
        if (!emitTree(pn2))
            return false;
        callop = false;             /* trigger JSOP_UNDEFINED after */
        break;
    }
    if (!callop) {
        if (!emit1(JSOP_UNDEFINED))
            return false;
    }

    bool isNewOp = pn->getOp() == JSOP_NEW || pn->getOp() == JSOP_SPREADNEW ||
                   pn->getOp() == JSOP_SUPERCALL || pn->getOp() == JSOP_SPREADSUPERCALL;;

    /*
     * Emit code for each argument in order, then emit the JSOP_*CALL or
     * JSOP_NEW bytecode with a two-byte immediate telling how many args
     * were pushed on the operand stack.
     */
    bool oldEmittingForInit = emittingForInit;
    emittingForInit = false;
    if (!spread) {
        for (ParseNode* pn3 = pn2->pn_next; pn3; pn3 = pn3->pn_next) {
            if (!emitTree(pn3))
                return false;
        }

        if (isNewOp) {
            if (pn->isKind(PNK_SUPERCALL)) {
                if (!emit1(JSOP_NEWTARGET))
                    return false;
            } else {
                // Repush the callee as new.target
                if (!emitDupAt(argc + 1))
                    return false;
            }
        }
    } else {
        if (!emitArray(pn2->pn_next, argc, JSOP_SPREADCALLARRAY))
            return false;

        if (isNewOp) {
            if (pn->isKind(PNK_SUPERCALL)) {
                if (!emit1(JSOP_NEWTARGET))
                    return false;
            } else {
                if (!emitDupAt(2))
                    return false;
            }
        }
    }
    emittingForInit = oldEmittingForInit;

    if (!spread) {
        if (!emitCall(pn->getOp(), argc, pn))
            return false;
    } else {
        if (!emit1(pn->getOp()))
            return false;
    }
    checkTypeSet(pn->getOp());
    if (pn->isOp(JSOP_EVAL) ||
        pn->isOp(JSOP_STRICTEVAL) ||
        pn->isOp(JSOP_SPREADEVAL) ||
        pn->isOp(JSOP_STRICTSPREADEVAL))
    {
        uint32_t lineNum = parser->tokenStream.srcCoords.lineNum(pn->pn_pos.begin);
        if (!emitUint32Operand(JSOP_LINENO, lineNum))
            return false;
    }
    if (pn->pn_xflags & PNX_SETCALL) {
        if (!emitUint16Operand(JSOP_THROWMSG, JSMSG_BAD_LEFTSIDE_OF_ASS))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitRightAssociative(ParseNode* pn)
{
    // ** is the only right-associative operator.
    MOZ_ASSERT(pn->isKind(PNK_POW));
    MOZ_ASSERT(pn->isArity(PN_LIST));

    // Right-associative operator chain.
    for (ParseNode* subexpr = pn->pn_head; subexpr; subexpr = subexpr->pn_next) {
        if (!emitTree(subexpr))
            return false;
    }
    for (uint32_t i = 0; i < pn->pn_count - 1; i++) {
        if (!emit1(JSOP_POW))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitLeftAssociative(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));

    // Left-associative operator chain.
    if (!emitTree(pn->pn_head))
        return false;
    JSOp op = pn->getOp();
    ParseNode* nextExpr = pn->pn_head->pn_next;
    do {
        if (!emitTree(nextExpr))
            return false;
        if (!emit1(op))
            return false;
    } while ((nextExpr = nextExpr->pn_next));
    return true;
}

bool
BytecodeEmitter::emitLogical(ParseNode* pn)
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
    if (!emitTree(pn2))
        return false;
    ptrdiff_t top;
    if (!emitJump(JSOP_BACKPATCH, 0, &top))
        return false;
    if (!emit1(JSOP_POP))
        return false;

    /* Emit nodes between the head and the tail. */
    ptrdiff_t jmp = top;
    while ((pn2 = pn2->pn_next)->pn_next) {
        if (!emitTree(pn2))
            return false;
        ptrdiff_t off;
        if (!emitJump(JSOP_BACKPATCH, 0, &off))
            return false;
        if (!emit1(JSOP_POP))
            return false;
        SET_JUMP_OFFSET(code(jmp), off - jmp);
        jmp = off;
    }
    if (!emitTree(pn2))
        return false;

    pn2 = pn->pn_head;
    ptrdiff_t off = offset();
    do {
        jsbytecode* pc = code(top);
        ptrdiff_t tmp = GET_JUMP_OFFSET(pc);
        SET_JUMP_OFFSET(pc, off - top);
        *pc = pn->getOp();
        top += tmp;
    } while ((pn2 = pn2->pn_next)->pn_next);

    return true;
}

bool
BytecodeEmitter::emitSequenceExpr(ParseNode* pn)
{
    for (ParseNode* child = pn->pn_head; ; child = child->pn_next) {
        if (!updateSourceCoordNotes(child->pn_pos.begin))
            return false;
        if (!emitTree(child))
            return false;
        if (!child->pn_next)
            break;
        if (!emit1(JSOP_POP))
            return false;
    }
    return true;
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitIncOrDec(ParseNode* pn)
{
    /* Emit lvalue-specialized code for ++/-- operators. */
    ParseNode* pn2 = pn->pn_kid;
    switch (pn2->getKind()) {
      case PNK_DOT:
        if (!emitPropIncDec(pn))
            return false;
        break;
      case PNK_ELEM:
        if (!emitElemIncDec(pn))
            return false;
        break;
      case PNK_CALL:
        MOZ_ASSERT(pn2->pn_xflags & PNX_SETCALL);
        if (!emitTree(pn2))
            return false;
        break;
      default:
        MOZ_ASSERT(pn2->isKind(PNK_NAME));
        pn2->setOp(JSOP_SETNAME);
        if (!bindNameToSlot(pn2))
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
            if (!emit1(op))
                return false;
        } else if (!pn2->pn_scopecoord.isFree()) {
            if (maySet) {
                if (!emitVarIncDec(pn))
                    return false;
            } else {
                if (!emitVarOp(pn2, op))
                    return false;
            }
        } else {
            MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);
            if (maySet) {
                if (!emitNameIncDec(pn))
                    return false;
            } else {
                if (!emitAtomOp(pn2, op))
                    return false;
            }
            break;
        }
        if (pn2->isConst()) {
            if (!emit1(JSOP_POS))
                return false;
            bool post;
            JSOp binop = GetIncDecInfo(pn->getKind(), &post);
            if (!post) {
                if (!emit1(JSOP_ONE))
                    return false;
                if (!emit1(binop))
                    return false;
            }
        }
    }
    return true;
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitLabeledStatement(const LabeledStatement* pn)
{
    /*
     * Emit a JSOP_LABEL instruction. The argument is the offset to the statement
     * following the labeled statement.
     */
    jsatomid index;
    if (!makeAtomIndex(pn->label(), &index))
        return false;

    ptrdiff_t top;
    if (!emitJump(JSOP_LABEL, 0, &top))
        return false;

    /* Emit code for the labeled statement. */
    StmtInfoBCE stmtInfo(cx);
    pushStatement(&stmtInfo, StmtType::LABEL, offset());
    stmtInfo.label = pn->label();

    if (!emitTree(pn->statement()))
        return false;

    popStatement();

    /* Patch the JSOP_LABEL offset. */
    setJumpOffsetAt(top);
    return true;
}

bool
BytecodeEmitter::emitConditionalExpression(ConditionalExpression& conditional)
{
    /* Emit the condition, then branch if false to the else part. */
    if (!emitTree(&conditional.condition()))
        return false;

    unsigned noteIndex;
    if (!newSrcNote(SRC_COND, &noteIndex))
        return false;

    ptrdiff_t beq;
    if (!emitJump(JSOP_IFEQ, 0, &beq))
        return false;

    if (!emitTree(&conditional.thenExpression()))
        return false;

    /* Jump around else, fixup the branch, emit else, fixup jump. */
    ptrdiff_t jmp;
    if (!emitJump(JSOP_GOTO, 0, &jmp))
        return false;
    setJumpOffsetAt(beq);

    /*
     * Because each branch pushes a single value, but our stack budgeting
     * analysis ignores branches, we now have to adjust this->stackDepth to
     * ignore the value pushed by the first branch.  Execution will follow
     * only one path, so we must decrement this->stackDepth.
     *
     * Failing to do this will foil code, such as let block code generation,
     * which must use the stack depth to compute local stack indexes correctly.
     */
    MOZ_ASSERT(stackDepth > 0);
    stackDepth--;
    if (!emitTree(&conditional.elseExpression()))
        return false;
    setJumpOffsetAt(jmp);
    return setSrcNoteOffset(noteIndex, 0, jmp - beq);
}

bool
BytecodeEmitter::emitPropertyList(ParseNode* pn, MutableHandlePlainObject objp, PropListType type)
{
    for (ParseNode* propdef = pn->pn_head; propdef; propdef = propdef->pn_next) {
        if (!updateSourceCoordNotes(propdef->pn_pos.begin))
            return false;

        // Handle __proto__: v specially because *only* this form, and no other
        // involving "__proto__", performs [[Prototype]] mutation.
        if (propdef->isKind(PNK_MUTATEPROTO)) {
            MOZ_ASSERT(type == ObjectLiteral);
            if (!emitTree(propdef->pn_kid))
                return false;
            objp.set(nullptr);
            if (!emit1(JSOP_MUTATEPROTO))
                return false;
            continue;
        }

        bool extraPop = false;
        if (type == ClassBody && propdef->as<ClassMethod>().isStatic()) {
            extraPop = true;
            if (!emit1(JSOP_DUP2))
                return false;
            if (!emit1(JSOP_POP))
                return false;
        }

        /* Emit an index for t[2] for later consumption by JSOP_INITELEM. */
        ParseNode* key = propdef->pn_left;
        bool isIndex = false;
        if (key->isKind(PNK_NUMBER)) {
            if (!emitNumberOp(key->pn_dval))
                return false;
            isIndex = true;
        } else if (key->isKind(PNK_OBJECT_PROPERTY_NAME) || key->isKind(PNK_STRING)) {
            // EmitClass took care of constructor already.
            if (type == ClassBody && key->pn_atom == cx->names().constructor &&
                !propdef->as<ClassMethod>().isStatic())
            {
                continue;
            }

            // The parser already checked for atoms representing indexes and
            // used PNK_NUMBER instead, but also watch for ids which TI treats
            // as indexes for simpliciation of downstream analysis.
            jsid id = NameToId(key->pn_atom->asPropertyName());
            if (id != IdToTypeId(id)) {
                if (!emitTree(key))
                    return false;
                isIndex = true;
            }
        } else {
            if (!emitComputedPropertyName(key))
                return false;
            isIndex = true;
        }

        /* Emit code for the property initializer. */
        if (!emitTree(propdef->pn_right))
            return false;

        JSOp op = propdef->getOp();
        MOZ_ASSERT(op == JSOP_INITPROP ||
                   op == JSOP_INITPROP_GETTER ||
                   op == JSOP_INITPROP_SETTER);

        if (op == JSOP_INITPROP_GETTER || op == JSOP_INITPROP_SETTER)
            objp.set(nullptr);

        if (propdef->pn_right->isKind(PNK_FUNCTION) &&
            propdef->pn_right->pn_funbox->needsHomeObject())
        {
            MOZ_ASSERT(propdef->pn_right->pn_funbox->function()->allowSuperProperty());
            if (!emit2(JSOP_INITHOMEOBJECT, isIndex))
                return false;
        }

        // Class methods are not enumerable.
        if (type == ClassBody) {
            switch (op) {
              case JSOP_INITPROP:        op = JSOP_INITHIDDENPROP;          break;
              case JSOP_INITPROP_GETTER: op = JSOP_INITHIDDENPROP_GETTER;   break;
              case JSOP_INITPROP_SETTER: op = JSOP_INITHIDDENPROP_SETTER;   break;
              default: MOZ_CRASH("Invalid op");
            }
        }

        if (isIndex) {
            objp.set(nullptr);
            switch (op) {
              case JSOP_INITPROP:               op = JSOP_INITELEM;              break;
              case JSOP_INITHIDDENPROP:         op = JSOP_INITHIDDENELEM;        break;
              case JSOP_INITPROP_GETTER:        op = JSOP_INITELEM_GETTER;       break;
              case JSOP_INITHIDDENPROP_GETTER:  op = JSOP_INITHIDDENELEM_GETTER; break;
              case JSOP_INITPROP_SETTER:        op = JSOP_INITELEM_SETTER;       break;
              case JSOP_INITHIDDENPROP_SETTER:  op = JSOP_INITHIDDENELEM_SETTER; break;
              default: MOZ_CRASH("Invalid op");
            }
            if (!emit1(op))
                return false;
        } else {
            MOZ_ASSERT(key->isKind(PNK_OBJECT_PROPERTY_NAME) || key->isKind(PNK_STRING));

            jsatomid index;
            if (!makeAtomIndex(key->pn_atom, &index))
                return false;

            if (objp) {
                MOZ_ASSERT(type == ObjectLiteral);
                MOZ_ASSERT(!IsHiddenInitOp(op));
                MOZ_ASSERT(!objp->inDictionaryMode());
                Rooted<jsid> id(cx, AtomToId(key->pn_atom));
                RootedValue undefinedValue(cx, UndefinedValue());
                if (!NativeDefineProperty(cx, objp, id, undefinedValue, nullptr, nullptr,
                                          JSPROP_ENUMERATE))
                {
                    return false;
                }
                if (objp->inDictionaryMode())
                    objp.set(nullptr);
            }

            if (!emitIndex32(op, index))
                return false;
        }

        if (extraPop) {
            if (!emit1(JSOP_POP))
                return false;
        }
    }
    return true;
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitObject(ParseNode* pn)
{
    if (!(pn->pn_xflags & PNX_NONCONST) && pn->pn_head && checkSingletonContext())
        return emitSingletonInitialiser(pn);

    /*
     * Emit code for {p:a, '%q':b, 2:c} that is equivalent to constructing
     * a new object and defining (in source order) each property on the object
     * (or mutating the object's [[Prototype]], in the case of __proto__).
     */
    ptrdiff_t offset = this->offset();
    if (!emitNewInit(JSProto_Object))
        return false;

    /*
     * Try to construct the shape of the object as we go, so we can emit a
     * JSOP_NEWOBJECT with the final shape instead.
     */
    RootedPlainObject obj(cx);
    // No need to do any guessing for the object kind, since we know exactly
    // how many properties we plan to have.
    gc::AllocKind kind = gc::GetGCObjectKind(pn->pn_count);
    obj = NewBuiltinClassInstance<PlainObject>(cx, kind, TenuredObject);
    if (!obj)
        return false;

    if (!emitPropertyList(pn, &obj, ObjectLiteral))
        return false;

    if (obj) {
        /*
         * The object survived and has a predictable shape: update the original
         * bytecode.
         */
        ObjectBox* objbox = parser->newObjectBox(obj);
        if (!objbox)
            return false;

        static_assert(JSOP_NEWINIT_LENGTH == JSOP_NEWOBJECT_LENGTH,
                      "newinit and newobject must have equal length to edit in-place");

        uint32_t index = objectList.add(objbox);
        jsbytecode* code = this->code(offset);
        code[0] = JSOP_NEWOBJECT;
        code[1] = jsbytecode(index >> 24);
        code[2] = jsbytecode(index >> 16);
        code[3] = jsbytecode(index >> 8);
        code[4] = jsbytecode(index);
    }

    return true;
}

bool
BytecodeEmitter::emitArrayComp(ParseNode* pn)
{
    if (!emitNewInit(JSProto_Array))
        return false;

    /*
     * Pass the new array's stack index to the PNK_ARRAYPUSH case via
     * arrayCompDepth, then simply traverse the PNK_FOR node and
     * its kids under pn2 to generate this comprehension.
     */
    MOZ_ASSERT(stackDepth > 0);
    uint32_t saveDepth = arrayCompDepth;
    arrayCompDepth = (uint32_t) (stackDepth - 1);
    if (!emitTree(pn->pn_head))
        return false;
    arrayCompDepth = saveDepth;

    return true;
}

bool
BytecodeEmitter::emitSpread()
{
    return emitForOf(StmtType::SPREAD, nullptr);
}

bool
BytecodeEmitter::emitArrayLiteral(ParseNode* pn)
{
    if (!(pn->pn_xflags & PNX_NONCONST) && pn->pn_head) {
        if (checkSingletonContext()) {
            // Bake in the object entirely if it will only be created once.
            return emitSingletonInitialiser(pn);
        }

        // If the array consists entirely of primitive values, make a
        // template object with copy on write elements that can be reused
        // every time the initializer executes.
        if (emitterMode != BytecodeEmitter::SelfHosting && pn->pn_count != 0) {
            RootedValue value(cx);
            if (!pn->getConstantValue(cx, ParseNode::ForCopyOnWriteArray, &value))
                return false;
            if (!value.isMagic(JS_GENERIC_MAGIC)) {
                // Note: the group of the template object might not yet reflect
                // that the object has copy on write elements. When the
                // interpreter or JIT compiler fetches the template, it should
                // use ObjectGroup::getOrFixupCopyOnWriteObject to make sure the
                // group for the template is accurate. We don't do this here as we
                // want to use ObjectGroup::allocationSiteGroup, which requires a
                // finished script.
                JSObject* obj = &value.toObject();
                MOZ_ASSERT(obj->is<ArrayObject>() &&
                           obj->as<ArrayObject>().denseElementsAreCopyOnWrite());

                ObjectBox* objbox = parser->newObjectBox(obj);
                if (!objbox)
                    return false;

                return emitObjectOp(objbox, JSOP_NEWARRAY_COPYONWRITE);
            }
        }
    }

    return emitArray(pn->pn_head, pn->pn_count, JSOP_NEWARRAY);
}

bool
BytecodeEmitter::emitArray(ParseNode* pn, uint32_t count, JSOp op)
{

    /*
     * Emit code for [a, b, c] that is equivalent to constructing a new
     * array and in source order evaluating each element value and adding
     * it to the array, without invoking latent setters.  We use the
     * JSOP_NEWINIT and JSOP_INITELEM_ARRAY bytecodes to ignore setters and
     * to avoid dup'ing and popping the array as each element is added, as
     * JSOP_SETELEM/JSOP_SETPROP would do.
     */
    MOZ_ASSERT(op == JSOP_NEWARRAY || op == JSOP_SPREADCALLARRAY);

    uint32_t nspread = 0;
    for (ParseNode* elt = pn; elt; elt = elt->pn_next) {
        if (elt->isKind(PNK_SPREAD))
            nspread++;
    }

    // Array literal's length is limited to NELEMENTS_LIMIT in parser.
    static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT <= INT32_MAX,
                  "array literals' maximum length must not exceed limits "
                  "required by BaselineCompiler::emit_JSOP_NEWARRAY, "
                  "BaselineCompiler::emit_JSOP_INITELEM_ARRAY, "
                  "and DoSetElemFallback's handling of JSOP_INITELEM_ARRAY");
    MOZ_ASSERT(count >= nspread);
    MOZ_ASSERT(count <= NativeObject::MAX_DENSE_ELEMENTS_COUNT,
               "the parser must throw an error if the array exceeds maximum "
               "length");

    // For arrays with spread, this is a very pessimistic allocation, the
    // minimum possible final size.
    if (!emitUint32Operand(op, count - nspread))                    // ARRAY
        return false;

    ParseNode* pn2 = pn;
    uint32_t index;
    bool afterSpread = false;
    for (index = 0; pn2; index++, pn2 = pn2->pn_next) {
        if (!afterSpread && pn2->isKind(PNK_SPREAD)) {
            afterSpread = true;
            if (!emitNumberOp(index))                               // ARRAY INDEX
                return false;
        }
        if (!updateSourceCoordNotes(pn2->pn_pos.begin))
            return false;
        if (pn2->isKind(PNK_ELISION)) {
            if (!emit1(JSOP_HOLE))
                return false;
        } else {
            ParseNode* expr = pn2->isKind(PNK_SPREAD) ? pn2->pn_kid : pn2;
            if (!emitTree(expr))                                         // ARRAY INDEX? VALUE
                return false;
        }
        if (pn2->isKind(PNK_SPREAD)) {
            if (!emitIterator())                                         // ARRAY INDEX ITER
                return false;
            if (!emit2(JSOP_PICK, 2))                                    // INDEX ITER ARRAY
                return false;
            if (!emit2(JSOP_PICK, 2))                                    // ITER ARRAY INDEX
                return false;
            if (!emitSpread())                                           // ARRAY INDEX
                return false;
        } else if (afterSpread) {
            if (!emit1(JSOP_INITELEM_INC))
                return false;
        } else {
            if (!emitUint32Operand(JSOP_INITELEM_ARRAY, index))
                return false;
        }
    }
    MOZ_ASSERT(index == count);
    if (afterSpread) {
        if (!emit1(JSOP_POP))                                            // ARRAY
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitUnary(ParseNode* pn)
{
    if (!updateSourceCoordNotes(pn->pn_pos.begin))
        return false;

    /* Unary op, including unary +/-. */
    JSOp op = pn->getOp();
    ParseNode* pn2 = pn->pn_kid;

    bool oldEmittingForInit = emittingForInit;
    emittingForInit = false;
    if (!emitTree(pn2))
        return false;

    emittingForInit = oldEmittingForInit;
    return emit1(op);
}

bool
BytecodeEmitter::emitTypeof(ParseNode* node, JSOp op)
{
    MOZ_ASSERT(op == JSOP_TYPEOF || op == JSOP_TYPEOFEXPR);

    if (!updateSourceCoordNotes(node->pn_pos.begin))
        return false;

    bool oldEmittingForInit = emittingForInit;
    emittingForInit = false;
    if (!emitTree(node->pn_kid))
        return false;

    emittingForInit = oldEmittingForInit;
    return emit1(op);
}

bool
BytecodeEmitter::emitArgsBody(ParseNode *pn)
{
    RootedFunction fun(cx, sc->asFunctionBox()->function());
    ParseNode* pnlast = pn->last();

    // Carefully emit everything in the right order:
    // 1. Defaults and Destructuring for each argument
    // 2. Functions
    ParseNode* pnchild = pnlast->pn_head;
    bool hasDefaults = sc->asFunctionBox()->hasDefaults();
    ParseNode* rest = nullptr;
    bool restIsDefn = false;
    if (fun->hasRest() && hasDefaults) {
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
        if (!emit1(JSOP_REST))
            return false;
        checkTypeSet(JSOP_REST);

        // Only set the rest parameter if it's not aliased by a nested
        // function in the body.
        if (restIsDefn) {
            if (!emit1(JSOP_UNDEFINED))
                return false;
            if (!bindNameToSlot(rest))
                return false;
            if (!emitVarOp(rest, JSOP_SETARG))
                return false;
            if (!emit1(JSOP_POP))
                return false;
        }
    }
    if (!emitDefaultsAndDestructuring(pn))
        return false;
    if (fun->hasRest() && hasDefaults) {
        if (restIsDefn && !emitVarOp(rest, JSOP_SETARG))
            return false;
        if (!emit1(JSOP_POP))
            return false;
    }
    for (ParseNode* pn2 = pn->pn_head; pn2 != pnlast; pn2 = pn2->pn_next) {
        // Only bind the parameter if it's not aliased by a nested function
        // in the body.
        if (!pn2->isDefn())
            continue;
        if (!bindNameToSlot(pn2))
            return false;
        if (pn2->pn_next == pnlast && fun->hasRest() && !hasDefaults) {
            // Fill rest parameter. We handled the case with defaults above.
            switchToPrologue();
            if (!emit1(JSOP_REST))
                return false;
            checkTypeSet(JSOP_REST);
            if (!emitVarOp(pn2, JSOP_SETARG))
                return false;
            if (!emit1(JSOP_POP))
                return false;
            switchToMain();
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
                if (!emitTree(pn2))
                    return false;
            }
        }
    }
    return emitTree(pnlast);
}

bool
BytecodeEmitter::emitDefaultsAndDestructuring(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_ARGSBODY));

    ParseNode* pnlast = pn->last();
    for (ParseNode* arg = pn->pn_head; arg != pnlast; arg = arg->pn_next) {
        MOZ_ASSERT(arg->isKind(PNK_NAME) || arg->isKind(PNK_ASSIGN));
        ParseNode* argName = nullptr;
        ParseNode* defNode = nullptr;
        ParseNode* destruct = nullptr;
        if (arg->isKind(PNK_ASSIGN)) {
            argName = arg->pn_left;
            defNode = arg->pn_right;
        } else if (arg->pn_atom == cx->names().empty) {
            argName = arg;
            destruct = arg->expr();
            MOZ_ASSERT(destruct);
            if (destruct->isKind(PNK_ASSIGN)) {
                defNode = destruct->pn_right;
                destruct = destruct->pn_left;
            }
        }
        if (defNode) {
            if (!bindNameToSlot(argName))
                return false;
            if (!emitVarOp(argName, JSOP_GETARG))
                return false;
            if (!emit1(JSOP_UNDEFINED))
                return false;
            if (!emit1(JSOP_STRICTEQ))
                return false;
            // Emit source note to enable ion compilation.
            if (!newSrcNote(SRC_IF))
                return false;
            ptrdiff_t jump;
            if (!emitJump(JSOP_IFEQ, 0, &jump))
                return false;
            if (!emitTree(defNode))
                return false;
            if (!emitVarOp(argName, JSOP_SETARG))
                return false;
            if (!emit1(JSOP_POP))
                return false;
            SET_JUMP_OFFSET(code(jump), offset() - jump);
        }
        if (destruct) {
            if (!emitTree(argName))
                return false;
            if (!emitDestructuringOps(destruct, false))
                 return false;
            if (!emit1(JSOP_POP))
                return false;
        }
    }

    return true;
}


bool
BytecodeEmitter::emitLexicalInitialization(ParseNode* pn, JSOp globalDefOp)
{
    MOZ_ASSERT(pn->isKind(PNK_NAME));

    if (!bindNameToSlot(pn))
        return false;

    jsatomid atomIndex;
    if (!maybeEmitVarDecl(globalDefOp, pn, &atomIndex))
        return false;

    if (!pn->pn_scopecoord.isFree()) {
        if (!emitVarOp(pn, pn->getOp()))
            return false;
    } else {
        if (!emitIndexOp(pn->getOp(), atomIndex))
            return false;
    }

    return true;
}

// This follows ES6 14.5.14 (ClassDefinitionEvaluation) and ES6 14.5.15
// (BindingClassDeclarationEvaluation).
bool
BytecodeEmitter::emitClass(ParseNode* pn)
{
    ClassNode& classNode = pn->as<ClassNode>();

    ClassNames* names = classNode.names();

    ParseNode* heritageExpression = classNode.heritage();

    ParseNode* classMethods = classNode.methodList();
    ParseNode* constructor = nullptr;
    for (ParseNode* mn = classMethods->pn_head; mn; mn = mn->pn_next) {
        ClassMethod& method = mn->as<ClassMethod>();
        ParseNode& methodName = method.name();
        if (!method.isStatic() &&
            methodName.isKind(PNK_OBJECT_PROPERTY_NAME) &&
            methodName.pn_atom == cx->names().constructor)
        {
            constructor = &method.method();
            break;
        }
    }

    bool savedStrictness = sc->setLocalStrictMode(true);

    StmtInfoBCE stmtInfo(cx);
    if (names) {
        if (!enterBlockScope(&stmtInfo, classNode.scopeObject(), JSOP_UNINITIALIZED))
            return false;
    }

    // This is kind of silly. In order to the get the home object defined on
    // the constructor, we have to make it second, but we want the prototype
    // on top for EmitPropertyList, because we expect static properties to be
    // rarer. The result is a few more swaps than we would like. Such is life.
    if (heritageExpression) {
        if (!emitTree(heritageExpression))
            return false;
        if (!emit1(JSOP_CLASSHERITAGE))
            return false;
        if (!emit1(JSOP_OBJWITHPROTO))
            return false;

        // JSOP_CLASSHERITAGE leaves both protos on the stack. After
        // creating the prototype, swap it to the bottom to make the
        // constructor.
        if (!emit1(JSOP_SWAP))
            return false;
    } else {
        if (!emitNewInit(JSProto_Object))
            return false;
    }

    if (constructor) {
        if (!emitFunction(constructor, !!heritageExpression))
            return false;
        if (constructor->pn_funbox->needsHomeObject()) {
            if (!emit2(JSOP_INITHOMEOBJECT, 0))
                return false;
        }
    } else {
        JSAtom *name = names ? names->innerBinding()->pn_atom : cx->names().empty;
        if (heritageExpression) {
            if (!emitAtomOp(name, JSOP_DERIVEDCONSTRUCTOR))
                return false;
        } else {
            if (!emitAtomOp(name, JSOP_CLASSCONSTRUCTOR))
                return false;
        }
    }

    if (!emit1(JSOP_SWAP))
        return false;

    if (!emit1(JSOP_DUP2))
        return false;
    if (!emitAtomOp(cx->names().prototype, JSOP_INITLOCKEDPROP))
        return false;
    if (!emitAtomOp(cx->names().constructor, JSOP_INITHIDDENPROP))
        return false;

    RootedPlainObject obj(cx);
    if (!emitPropertyList(classMethods, &obj, ClassBody))
        return false;

    if (!emit1(JSOP_POP))
        return false;

    if (names) {
        // That DEFCONST is never gonna be used, but use it here for logical consistency.
        ParseNode* innerName = names->innerBinding();
        if (!emitLexicalInitialization(innerName, JSOP_DEFCONST))
            return false;

        if (!leaveNestedScope(&stmtInfo))
            return false;

        ParseNode* outerName = names->outerBinding();
        if (outerName) {
            if (!emitLexicalInitialization(outerName, JSOP_DEFLET))
                return false;
            // Only class statements make outer bindings, and they do not leave
            // themselves on the stack.
            if (!emit1(JSOP_POP))
                return false;
        }
    }

    MOZ_ALWAYS_TRUE(sc->setLocalStrictMode(savedStrictness));

    return true;
}

bool
BytecodeEmitter::emitTree(ParseNode* pn, EmitLineNumberNote emitLineNote)
{
    JS_CHECK_RECURSION(cx, return false);

    EmitLevelManager elm(this);

    /* Emit notes to tell the current bytecode's source line number.
       However, a couple trees require special treatment; see the
       relevant emitter functions for details. */
    if (emitLineNote == EMIT_LINENOTE && pn->getKind() != PNK_WHILE && pn->getKind() != PNK_FOR &&
        !updateLineNumberNotes(pn->pn_pos.begin))
        return false;

    switch (pn->getKind()) {
      case PNK_FUNCTION:
        if (!emitFunction(pn))
            return false;
        break;

      case PNK_ARGSBODY:
        if (!emitArgsBody(pn))
            return false;
        break;

      case PNK_IF:
        if (!emitIf(pn))
            return false;
        break;

      case PNK_SWITCH:
        if (!emitSwitch(pn))
            return false;
        break;

      case PNK_WHILE:
        if (!emitWhile(pn))
            return false;
        break;

      case PNK_DOWHILE:
        if (!emitDo(pn))
            return false;
        break;

      case PNK_FOR:
        if (!emitFor(pn))
            return false;
        break;

      case PNK_COMPREHENSIONFOR:
        if (!emitComprehensionFor(pn))
            return false;
        break;

      case PNK_BREAK:
        if (!emitBreak(pn->as<BreakStatement>().label()))
            return false;
        break;

      case PNK_CONTINUE:
        if (!emitContinue(pn->as<ContinueStatement>().label()))
            return false;
        break;

      case PNK_WITH:
        if (!emitWith(pn))
            return false;
        break;

      case PNK_TRY:
        if (!emitTry(pn))
            return false;
        break;

      case PNK_CATCH:
        if (!emitCatch(pn))
            return false;
        break;

      case PNK_VAR:
        if (!emitVariables(pn, InitializeVars))
            return false;
        break;

      case PNK_RETURN:
        if (!emitReturn(pn))
            return false;
        break;

      case PNK_YIELD_STAR:
        if (!emitYieldStar(pn->pn_left, pn->pn_right))
            return false;
        break;

      case PNK_GENERATOR:
        if (!emit1(JSOP_GENERATOR))
            return false;
        break;

      case PNK_YIELD:
        if (!emitYield(pn))
            return false;
        break;

      case PNK_STATEMENTLIST:
        if (!emitStatementList(pn))
            return false;
        break;

      case PNK_SEMI:
        if (!emitStatement(pn))
            return false;
        break;

      case PNK_LABEL:
        if (!emitLabeledStatement(&pn->as<LabeledStatement>()))
            return false;
        break;

      case PNK_COMMA:
        if (!emitSequenceExpr(pn))
            return false;
        break;

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
      case PNK_POWASSIGN:
        if (!emitAssignment(pn->pn_left, pn->getOp(), pn->pn_right))
            return false;
        break;

      case PNK_CONDITIONAL:
        if (!emitConditionalExpression(pn->as<ConditionalExpression>()))
            return false;
        break;

      case PNK_OR:
      case PNK_AND:
        if (!emitLogical(pn))
            return false;
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
      case PNK_MOD:
        if (!emitLeftAssociative(pn))
            return false;
        break;

      case PNK_POW:
        if (!emitRightAssociative(pn))
            return false;
        break;

      case PNK_TYPEOFNAME:
        if (!emitTypeof(pn, JSOP_TYPEOF))
            return false;
        break;

      case PNK_TYPEOFEXPR:
        if (!emitTypeof(pn, JSOP_TYPEOFEXPR))
            return false;
        break;

      case PNK_THROW:
      case PNK_VOID:
      case PNK_NOT:
      case PNK_BITNOT:
      case PNK_POS:
      case PNK_NEG:
        if (!emitUnary(pn))
            return false;
        break;

      case PNK_PREINCREMENT:
      case PNK_PREDECREMENT:
      case PNK_POSTINCREMENT:
      case PNK_POSTDECREMENT:
        if (!emitIncOrDec(pn))
            return false;
        break;

      case PNK_DELETENAME:
        if (!emitDeleteName(pn))
            return false;
        break;

      case PNK_DELETEPROP:
        if (!emitDeleteProperty(pn))
            return false;
        break;

      case PNK_DELETEELEM:
        if (!emitDeleteElement(pn))
            return false;
        break;

      case PNK_DELETEEXPR:
        if (!emitDeleteExpression(pn))
            return false;
        break;

      case PNK_DOT:
        if (pn->as<PropertyAccess>().isSuper()) {
            if (!emitSuperPropOp(pn, JSOP_GETPROP_SUPER))
                return false;
        } else {
            if (!emitPropOp(pn, JSOP_GETPROP))
                return false;
        }
        break;

      case PNK_ELEM:
        if (pn->as<PropertyByValue>().isSuper()) {
            if (!emitSuperElemOp(pn, JSOP_GETELEM_SUPER))
                return false;
        } else {
            if (!emitElemOp(pn, JSOP_GETELEM))
                return false;
        }
        break;

      case PNK_NEW:
      case PNK_TAGGED_TEMPLATE:
      case PNK_CALL:
      case PNK_GENEXP:
      case PNK_SUPERCALL:
        if (!emitCallOrNew(pn))
            return false;
        break;

      case PNK_LEXICALSCOPE:
        if (!emitLexicalScope(pn))
            return false;
        break;

      case PNK_LETBLOCK:
        if (!emitLetBlock(pn))
            return false;
        break;

      case PNK_CONST:
      case PNK_LET:
        if (!emitVariables(pn, InitializeVars))
            return false;
        break;

      case PNK_IMPORT:
        MOZ_ASSERT(sc->isModuleBox());
        break;

      case PNK_EXPORT:
        MOZ_ASSERT(sc->isModuleBox());
        if (pn->pn_kid->getKind() != PNK_EXPORT_SPEC_LIST) {
            if (!emitTree(pn->pn_kid))
                return false;
        }
        break;

      case PNK_EXPORT_DEFAULT:
        MOZ_ASSERT(sc->isModuleBox());
        if (!emitTree(pn->pn_kid))
            return false;
        if (pn->pn_right) {
            if (!emitLexicalInitialization(pn->pn_right, JSOP_DEFCONST))
                return false;
            if (!emit1(JSOP_POP))
                return false;
        }
        break;

      case PNK_EXPORT_FROM:
        MOZ_ASSERT(sc->isModuleBox());
        break;

      case PNK_ARRAYPUSH:
        /*
         * The array object's stack index is in arrayCompDepth. See below
         * under the array initialiser code generator for array comprehension
         * special casing. Note that the array object is a pure stack value,
         * unaliased by blocks, so we can emitUnaliasedVarOp.
         */
        if (!emitTree(pn->pn_kid))
            return false;
        if (!emitDupAt(this->stackDepth - 1 - arrayCompDepth))
            return false;
        if (!emit1(JSOP_ARRAYPUSH))
            return false;
        break;

      case PNK_CALLSITEOBJ:
        if (!emitCallSiteObject(pn))
            return false;
        break;

      case PNK_ARRAY:
        if (!emitArrayLiteral(pn))
            return false;
        break;

      case PNK_ARRAYCOMP:
        if (!emitArrayComp(pn))
            return false;
        break;

      case PNK_OBJECT:
        if (!emitObject(pn))
            return false;
        break;

      case PNK_NAME:
        if (!emitNameOp(pn, false))
            return false;
        break;

      case PNK_TEMPLATE_STRING_LIST:
        if (!emitTemplateString(pn))
            return false;
        break;

      case PNK_TEMPLATE_STRING:
      case PNK_STRING:
        if (!emitAtomOp(pn, JSOP_STRING))
            return false;
        break;

      case PNK_NUMBER:
        if (!emitNumberOp(pn->pn_dval))
            return false;
        break;

      case PNK_REGEXP:
        if (!emitRegExp(regexpList.add(pn->as<RegExpLiteral>().objbox())))
            return false;
        break;

      case PNK_TRUE:
      case PNK_FALSE:
      case PNK_NULL:
        if (!emit1(pn->getOp()))
            return false;
        break;

      case PNK_THIS:
        if (!emitThisLiteral(pn))
            return false;
        break;

      case PNK_DEBUGGER:
        if (!updateSourceCoordNotes(pn->pn_pos.begin))
            return false;
        if (!emit1(JSOP_DEBUGGER))
            return false;
        break;

      case PNK_NOP:
        MOZ_ASSERT(pn->getArity() == PN_NULLARY);
        break;

      case PNK_CLASS:
        if (!emitClass(pn))
            return false;
        break;

      case PNK_NEWTARGET:
        if (!emit1(JSOP_NEWTARGET))
            return false;
        break;

      case PNK_SETTHIS:
        if (!emitSetThis(pn))
            return false;
        break;

      case PNK_POSHOLDER:
        MOZ_ASSERT_UNREACHABLE("Should never try to emit PNK_POSHOLDER");

      default:
        MOZ_ASSERT(0);
    }

    /* bce->emitLevel == 1 means we're last on the stack, so finish up. */
    if (emitLevel == 1) {
        if (!updateSourceCoordNotes(pn->pn_pos.end))
            return false;
    }
    return true;
}

static bool
AllocSrcNote(ExclusiveContext* cx, SrcNotesVector& notes, unsigned* index)
{
    // Start it off moderately large to avoid repeated resizings early on.
    // ~99% of cases fit within 256 bytes.
    if (notes.capacity() == 0 && !notes.reserve(256))
        return false;

    if (!notes.growBy(1)) {
        ReportOutOfMemory(cx);
        return false;
    }

    *index = notes.length() - 1;
    return true;
}

bool
BytecodeEmitter::newSrcNote(SrcNoteType type, unsigned* indexp)
{
    SrcNotesVector& notes = this->notes();
    unsigned index;
    if (!AllocSrcNote(cx, notes, &index))
        return false;

    /*
     * Compute delta from the last annotated bytecode's offset.  If it's too
     * big to fit in sn, allocate one or more xdelta notes and reset sn.
     */
    ptrdiff_t offset = this->offset();
    ptrdiff_t delta = offset - lastNoteOffset();
    current->lastNoteOffset = offset;
    if (delta >= SN_DELTA_LIMIT) {
        do {
            ptrdiff_t xdelta = Min(delta, SN_XDELTA_MASK);
            SN_MAKE_XDELTA(&notes[index], xdelta);
            delta -= xdelta;
            if (!AllocSrcNote(cx, notes, &index))
                return false;
        } while (delta >= SN_DELTA_LIMIT);
    }

    /*
     * Initialize type and delta, then allocate the minimum number of notes
     * needed for type's arity.  Usually, we won't need more, but if an offset
     * does take two bytes, setSrcNoteOffset will grow notes.
     */
    SN_MAKE_NOTE(&notes[index], type, delta);
    for (int n = (int)js_SrcNoteSpec[type].arity; n > 0; n--) {
        if (!newSrcNote(SRC_NULL))
            return false;
    }

    if (indexp)
        *indexp = index;
    return true;
}

bool
BytecodeEmitter::newSrcNote2(SrcNoteType type, ptrdiff_t offset, unsigned* indexp)
{
    unsigned index;
    if (!newSrcNote(type, &index))
        return false;
    if (!setSrcNoteOffset(index, 0, offset))
        return false;
    if (indexp)
        *indexp = index;
    return true;
}

bool
BytecodeEmitter::newSrcNote3(SrcNoteType type, ptrdiff_t offset1, ptrdiff_t offset2,
                             unsigned* indexp)
{
    unsigned index;
    if (!newSrcNote(type, &index))
        return false;
    if (!setSrcNoteOffset(index, 0, offset1))
        return false;
    if (!setSrcNoteOffset(index, 1, offset2))
        return false;
    if (indexp)
        *indexp = index;
    return true;
}

bool
BytecodeEmitter::addToSrcNoteDelta(jssrcnote* sn, ptrdiff_t delta)
{
    /*
     * Called only from finishTakingSrcNotes to add to main script note
     * deltas, and only by a small positive amount.
     */
    MOZ_ASSERT(current == &main);
    MOZ_ASSERT((unsigned) delta < (unsigned) SN_XDELTA_LIMIT);

    ptrdiff_t base = SN_DELTA(sn);
    ptrdiff_t limit = SN_IS_XDELTA(sn) ? SN_XDELTA_LIMIT : SN_DELTA_LIMIT;
    ptrdiff_t newdelta = base + delta;
    if (newdelta < limit) {
        SN_SET_DELTA(sn, newdelta);
    } else {
        jssrcnote xdelta;
        SN_MAKE_XDELTA(&xdelta, delta);
        if (!main.notes.insert(sn, xdelta))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::setSrcNoteOffset(unsigned index, unsigned which, ptrdiff_t offset)
{
    if (!SN_REPRESENTABLE_OFFSET(offset)) {
        ReportStatementTooLarge(parser->tokenStream, innermostStmt());
        return false;
    }

    SrcNotesVector& notes = this->notes();

    /* Find the offset numbered which (i.e., skip exactly which offsets). */
    jssrcnote* sn = &notes[index];
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
                ReportOutOfMemory(cx);
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

bool
BytecodeEmitter::finishTakingSrcNotes(uint32_t* out)
{
    MOZ_ASSERT(current == &main);

    unsigned prologueCount = prologue.notes.length();
    if (prologueCount && prologue.currentLine != firstLine) {
        switchToPrologue();
        if (!newSrcNote2(SRC_SETLINE, ptrdiff_t(firstLine)))
            return false;
        switchToMain();
    } else {
        /*
         * Either no prologue srcnotes, or no line number change over prologue.
         * We don't need a SRC_SETLINE, but we may need to adjust the offset
         * of the first main note, by adding to its delta and possibly even
         * prepending SRC_XDELTA notes to it to account for prologue bytecodes
         * that came at and after the last annotated bytecode.
         */
        ptrdiff_t offset = prologueOffset() - prologue.lastNoteOffset;
        MOZ_ASSERT(offset >= 0);
        if (offset > 0 && main.notes.length() != 0) {
            /* NB: Use as much of the first main note's delta as we can. */
            jssrcnote* sn = main.notes.begin();
            ptrdiff_t delta = SN_IS_XDELTA(sn)
                            ? SN_XDELTA_MASK - (*sn & SN_XDELTA_MASK)
                            : SN_DELTA_MASK - (*sn & SN_DELTA_MASK);
            if (offset < delta)
                delta = offset;
            for (;;) {
                if (!addToSrcNoteDelta(sn, delta))
                    return false;
                offset -= delta;
                if (offset == 0)
                    break;
                delta = Min(offset, SN_XDELTA_MASK);
                sn = main.notes.begin();
            }
        }
    }

    // The prologue count might have changed, so we can't reuse prologueCount.
    // The + 1 is to account for the final SN_MAKE_TERMINATOR that is appended
    // when the notes are copied to their final destination by CopySrcNotes.
    *out = prologue.notes.length() + main.notes.length() + 1;
    return true;
}

void
BytecodeEmitter::copySrcNotes(jssrcnote* destination, uint32_t nsrcnotes)
{
    unsigned prologueCount = prologue.notes.length();
    unsigned mainCount = main.notes.length();
    unsigned totalCount = prologueCount + mainCount;
    MOZ_ASSERT(totalCount == nsrcnotes - 1);
    if (prologueCount)
        PodCopy(destination, prologue.notes.begin(), prologueCount);
    PodCopy(destination + prologueCount, main.notes.begin(), mainCount);
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

    js::HeapPtrObject* cursor = array->vector + array->length;
    ObjectBox* objbox = lastbox;
    do {
        --cursor;
        MOZ_ASSERT(!*cursor);
        MOZ_ASSERT(objbox->object->isTenured());
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
CGBlockScopeList::append(uint32_t scopeObject, uint32_t offset, bool inPrologue,
                         uint32_t parent)
{
    CGBlockScopeNote note;
    mozilla::PodZero(&note);

    note.index = scopeObject;
    note.start = offset;
    note.parent = parent;
    note.startInPrologue = inPrologue;

    return list.append(note);
}

uint32_t
CGBlockScopeList::findEnclosingScope(uint32_t index)
{
    MOZ_ASSERT(index < length());
    MOZ_ASSERT(list[index].index != BlockScopeNote::NoBlockScopeIndex);

    DebugOnly<bool> inPrologue = list[index].startInPrologue;
    DebugOnly<uint32_t> pos = list[index].start;
    while (index--) {
        MOZ_ASSERT_IF(inPrologue == list[index].startInPrologue, list[index].start <= pos);
        if (list[index].end == 0) {
            // We are looking for the nearest enclosing live scope.  If the
            // scope contains POS, it should still be open, so its length should
            // be zero.
            return list[index].index;
        }
        // Conversely, if the length is not zero, it should not contain POS.
        MOZ_ASSERT_IF(inPrologue == list[index].endInPrologue, list[index].end <= pos);
    }

    return BlockScopeNote::NoBlockScopeIndex;
}

void
CGBlockScopeList::recordEnd(uint32_t index, uint32_t offset, bool inPrologue)
{
    MOZ_ASSERT(index < length());
    MOZ_ASSERT(offset >= list[index].start);
    MOZ_ASSERT(list[index].length == 0);
    list[index].end = offset;
    list[index].endInPrologue = inPrologue;
}

void
CGBlockScopeList::finish(BlockScopeArray* array, uint32_t prologueLength)
{
    MOZ_ASSERT(length() == array->length);

    for (unsigned i = 0; i < length(); i++) {
        if (!list[i].startInPrologue)
            list[i].start += prologueLength;
        if (!list[i].endInPrologue)
            list[i].end += prologueLength;
        list[i].length = list[i].end - list[i].start;
        array->vector[i] = list[i];
    }
}

void
CGYieldOffsetList::finish(YieldOffsetArray& array, uint32_t prologueLength)
{
    MOZ_ASSERT(length() == array.length());

    for (unsigned i = 0; i < length(); i++)
        array[i] = prologueLength + list[i];
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
js::SrcNoteLength(jssrcnote* sn)
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
js::GetSrcNoteOffset(jssrcnote* sn, unsigned which)
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
