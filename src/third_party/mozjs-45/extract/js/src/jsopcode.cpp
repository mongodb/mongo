/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS bytecode descriptors, disassemblers, and (expression) decompilers.
 */

#include "jsopcodeinlines.h"

#define __STDC_FORMAT_MACROS

#include "mozilla/SizePrintfMacros.h"

#include <algorithm>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsfun.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jsscript.h"
#include "jsstr.h"
#include "jstypes.h"
#include "jsutil.h"

#include "asmjs/AsmJSModule.h"
#include "frontend/BytecodeCompiler.h"
#include "frontend/SourceNotes.h"
#include "gc/GCInternals.h"
#include "js/CharacterEncoding.h"
#include "vm/CodeCoverage.h"
#include "vm/Opcodes.h"
#include "vm/ScopeObject.h"
#include "vm/Shape.h"
#include "vm/StringBuffer.h"

#include "jscntxtinlines.h"
#include "jscompartmentinlines.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"

using namespace js;
using namespace js::gc;

using JS::AutoCheckCannotGC;

using js::frontend::IsIdentifier;

/*
 * Index limit must stay within 32 bits.
 */
JS_STATIC_ASSERT(sizeof(uint32_t) * JS_BITS_PER_BYTE >= INDEX_LIMIT_LOG2 + 1);

const JSCodeSpec js::CodeSpec[] = {
#define MAKE_CODESPEC(op,val,name,token,length,nuses,ndefs,format)  {length,nuses,ndefs,format},
    FOR_EACH_OPCODE(MAKE_CODESPEC)
#undef MAKE_CODESPEC
};

const unsigned js::NumCodeSpecs = JS_ARRAY_LENGTH(CodeSpec);

/*
 * Each element of the array is either a source literal associated with JS
 * bytecode or null.
 */
static const char * const CodeToken[] = {
#define TOKEN(op, val, name, token, ...)  token,
    FOR_EACH_OPCODE(TOKEN)
#undef TOKEN
};

/*
 * Array of JS bytecode names used by PC count JSON, DEBUG-only Disassemble
 * and JIT debug spew.
 */
const char * const js::CodeName[] = {
#define OPNAME(op, val, name, ...)  name,
    FOR_EACH_OPCODE(OPNAME)
#undef OPNAME
};

/************************************************************************/

#define COUNTS_LEN 16

size_t
js::GetVariableBytecodeLength(jsbytecode* pc)
{
    JSOp op = JSOp(*pc);
    MOZ_ASSERT(CodeSpec[op].length == -1);
    switch (op) {
      case JSOP_TABLESWITCH: {
        /* Structure: default-jump case-low case-high case1-jump ... */
        pc += JUMP_OFFSET_LEN;
        int32_t low = GET_JUMP_OFFSET(pc);
        pc += JUMP_OFFSET_LEN;
        int32_t high = GET_JUMP_OFFSET(pc);
        unsigned ncases = unsigned(high - low + 1);
        return 1 + 3 * JUMP_OFFSET_LEN + ncases * JUMP_OFFSET_LEN;
      }
      default:
        MOZ_CRASH("Unexpected op");
    }
}

unsigned
js::StackUses(JSScript* script, jsbytecode* pc)
{
    JSOp op = (JSOp) *pc;
    const JSCodeSpec& cs = CodeSpec[op];
    if (cs.nuses >= 0)
        return cs.nuses;

    MOZ_ASSERT(CodeSpec[op].nuses == -1);
    switch (op) {
      case JSOP_POPN:
        return GET_UINT16(pc);
      case JSOP_NEW:
      case JSOP_SUPERCALL:
        return 2 + GET_ARGC(pc) + 1;
      default:
        /* stack: fun, this, [argc arguments] */
        MOZ_ASSERT(op == JSOP_CALL || op == JSOP_EVAL || op == JSOP_CALLITER ||
                   op == JSOP_STRICTEVAL || op == JSOP_FUNCALL || op == JSOP_FUNAPPLY);
        return 2 + GET_ARGC(pc);
    }
}

unsigned
js::StackDefs(JSScript* script, jsbytecode* pc)
{
    JSOp op = (JSOp) *pc;
    const JSCodeSpec& cs = CodeSpec[op];
    MOZ_ASSERT(cs.ndefs >= 0);
    return cs.ndefs;
}

const char * PCCounts::numExecName = "interp";

void
js::DumpIonScriptCounts(Sprinter* sp, jit::IonScriptCounts* ionCounts)
{
    Sprint(sp, "IonScript [%lu blocks]:\n", ionCounts->numBlocks());
    for (size_t i = 0; i < ionCounts->numBlocks(); i++) {
        const jit::IonBlockCounts& block = ionCounts->block(i);
        Sprint(sp, "BB #%lu [%05u]", block.id(), block.offset());
        if (block.description())
            Sprint(sp, " [inlined %s]", block.description());
        for (size_t j = 0; j < block.numSuccessors(); j++)
            Sprint(sp, " -> #%lu", block.successor(j));
        Sprint(sp, " :: %llu hits\n", block.hitCount());
        Sprint(sp, "%s\n", block.code());
    }
}

void
js::DumpPCCounts(JSContext* cx, HandleScript script, Sprinter* sp)
{
    MOZ_ASSERT(script->hasScriptCounts());

#ifdef DEBUG
    jsbytecode* pc = script->code();
    while (pc < script->codeEnd()) {
        jsbytecode* next = GetNextPc(pc);

        if (!Disassemble1(cx, script, pc, script->pcToOffset(pc), true, sp))
            return;

        Sprint(sp, "                  {");
        PCCounts* counts = script->maybeGetPCCounts(pc);
        double val = counts ? counts->numExec() : 0.0;
        if (val)
            Sprint(sp, "\"%s\": %.0f", PCCounts::numExecName, val);
        Sprint(sp, "}\n");

        pc = next;
    }
#endif

    jit::IonScriptCounts* ionCounts = script->getIonCounts();

    while (ionCounts) {
        DumpIonScriptCounts(sp, ionCounts);
        ionCounts = ionCounts->previous();
    }
}

void
js::DumpCompartmentPCCounts(JSContext* cx)
{
    for (ZoneCellIter i(cx->zone(), gc::AllocKind::SCRIPT); !i.done(); i.next()) {
        RootedScript script(cx, i.get<JSScript>());
        if (script->compartment() != cx->compartment())
            continue;

        if (script->hasScriptCounts()) {
            Sprinter sprinter(cx);
            if (!sprinter.init())
                return;

            fprintf(stdout, "--- SCRIPT %s:%" PRIuSIZE " ---\n", script->filename(), script->lineno());
            DumpPCCounts(cx, script, &sprinter);
            fputs(sprinter.string(), stdout);
            fprintf(stdout, "--- END SCRIPT %s:%" PRIuSIZE " ---\n", script->filename(), script->lineno());
        }
    }
}

/////////////////////////////////////////////////////////////////////
// Bytecode Parser
/////////////////////////////////////////////////////////////////////

namespace {

class BytecodeParser
{
    class Bytecode
    {
      public:
        Bytecode() { mozilla::PodZero(this); }

        // Whether this instruction has been analyzed to get its output defines
        // and stack.
        bool parsed : 1;

        // Stack depth before this opcode.
        uint32_t stackDepth;

        // Pointer to array of |stackDepth| offsets.  An element at position N
        // in the array is the offset of the opcode that defined the
        // corresponding stack slot.  The top of the stack is at position
        // |stackDepth - 1|.
        uint32_t* offsetStack;

        bool captureOffsetStack(LifoAlloc& alloc, const uint32_t* stack, uint32_t depth) {
            stackDepth = depth;
            offsetStack = alloc.newArray<uint32_t>(stackDepth);
            if (!offsetStack)
                return false;
            if (stackDepth) {
                for (uint32_t n = 0; n < stackDepth; n++)
                    offsetStack[n] = stack[n];
            }
            return true;
        }

        // When control-flow merges, intersect the stacks, marking slots that
        // are defined by different offsets with the UINT32_MAX sentinel.
        // This is sufficient for forward control-flow.  It doesn't grok loops
        // -- for that you would have to iterate to a fixed point -- but there
        // shouldn't be operands on the stack at a loop back-edge anyway.
        void mergeOffsetStack(const uint32_t* stack, uint32_t depth) {
            MOZ_ASSERT(depth == stackDepth);
            for (uint32_t n = 0; n < stackDepth; n++)
                if (offsetStack[n] != stack[n])
                    offsetStack[n] = UINT32_MAX;
        }
    };

    JSContext* cx_;
    LifoAllocScope allocScope_;
    RootedScript script_;

    Bytecode** codeArray_;

  public:
    BytecodeParser(JSContext* cx, JSScript* script)
      : cx_(cx),
        allocScope_(&cx->tempLifoAlloc()),
        script_(cx, script),
        codeArray_(nullptr) { }

    bool parse();

#ifdef DEBUG
    bool isReachable(uint32_t offset) { return maybeCode(offset); }
    bool isReachable(const jsbytecode* pc) { return maybeCode(pc); }
#endif

    uint32_t stackDepthAtPC(uint32_t offset) {
        // Sometimes the code generator in debug mode asks about the stack depth
        // of unreachable code (bug 932180 comment 22).  Assume that unreachable
        // code has no operands on the stack.
        return getCode(offset).stackDepth;
    }
    uint32_t stackDepthAtPC(const jsbytecode* pc) { return stackDepthAtPC(script_->pcToOffset(pc)); }

    uint32_t offsetForStackOperand(uint32_t offset, int operand) {
        Bytecode& code = getCode(offset);
        if (operand < 0) {
            operand += code.stackDepth;
            MOZ_ASSERT(operand >= 0);
        }
        MOZ_ASSERT(uint32_t(operand) < code.stackDepth);
        return code.offsetStack[operand];
    }
    jsbytecode* pcForStackOperand(jsbytecode* pc, int operand) {
        uint32_t offset = offsetForStackOperand(script_->pcToOffset(pc), operand);
        if (offset == UINT32_MAX)
            return nullptr;
        return script_->offsetToPC(offsetForStackOperand(script_->pcToOffset(pc), operand));
    }

  private:
    LifoAlloc& alloc() {
        return allocScope_.alloc();
    }

    void reportOOM() {
        allocScope_.releaseEarly();
        ReportOutOfMemory(cx_);
    }

    uint32_t numSlots() {
        return 1 + script_->nfixed() +
               (script_->functionNonDelazifying() ? script_->functionNonDelazifying()->nargs() : 0);
    }

    uint32_t maximumStackDepth() {
        return script_->nslots() - script_->nfixed();
    }

    Bytecode& getCode(uint32_t offset) {
        MOZ_ASSERT(offset < script_->length());
        MOZ_ASSERT(codeArray_[offset]);
        return *codeArray_[offset];
    }
    Bytecode& getCode(const jsbytecode* pc) { return getCode(script_->pcToOffset(pc)); }

    Bytecode* maybeCode(uint32_t offset) {
        MOZ_ASSERT(offset < script_->length());
        return codeArray_[offset];
    }
    Bytecode* maybeCode(const jsbytecode* pc) { return maybeCode(script_->pcToOffset(pc)); }

    uint32_t simulateOp(JSOp op, uint32_t offset, uint32_t* offsetStack, uint32_t stackDepth);

    inline bool addJump(uint32_t offset, uint32_t* currentOffset,
                        uint32_t stackDepth, const uint32_t* offsetStack);
};

}  // anonymous namespace

uint32_t
BytecodeParser::simulateOp(JSOp op, uint32_t offset, uint32_t* offsetStack, uint32_t stackDepth)
{
    uint32_t nuses = GetUseCount(script_, offset);
    uint32_t ndefs = GetDefCount(script_, offset);

    MOZ_ASSERT(stackDepth >= nuses);
    stackDepth -= nuses;
    MOZ_ASSERT(stackDepth + ndefs <= maximumStackDepth());

    // Mark the current offset as defining its values on the offset stack,
    // unless it just reshuffles the stack.  In that case we want to preserve
    // the opcode that generated the original value.
    switch (op) {
      default:
        for (uint32_t n = 0; n != ndefs; ++n)
            offsetStack[stackDepth + n] = offset;
        break;

      case JSOP_CASE:
        /* Keep the switch value. */
        MOZ_ASSERT(ndefs == 1);
        break;

      case JSOP_DUP:
        MOZ_ASSERT(ndefs == 2);
        if (offsetStack)
            offsetStack[stackDepth + 1] = offsetStack[stackDepth];
        break;

      case JSOP_DUP2:
        MOZ_ASSERT(ndefs == 4);
        if (offsetStack) {
            offsetStack[stackDepth + 2] = offsetStack[stackDepth];
            offsetStack[stackDepth + 3] = offsetStack[stackDepth + 1];
        }
        break;

      case JSOP_DUPAT: {
        MOZ_ASSERT(ndefs == 1);
        jsbytecode* pc = script_->offsetToPC(offset);
        unsigned n = GET_UINT24(pc);
        MOZ_ASSERT(n < stackDepth);
        if (offsetStack)
            offsetStack[stackDepth] = offsetStack[stackDepth - 1 - n];
        break;
      }

      case JSOP_SWAP:
        MOZ_ASSERT(ndefs == 2);
        if (offsetStack) {
            uint32_t tmp = offsetStack[stackDepth + 1];
            offsetStack[stackDepth + 1] = offsetStack[stackDepth];
            offsetStack[stackDepth] = tmp;
        }
        break;
    }
    stackDepth += ndefs;
    return stackDepth;
}

bool
BytecodeParser::addJump(uint32_t offset, uint32_t* currentOffset,
                        uint32_t stackDepth, const uint32_t* offsetStack)
{
    MOZ_ASSERT(offset < script_->length());

    Bytecode*& code = codeArray_[offset];
    if (!code) {
        code = alloc().new_<Bytecode>();
        if (!code ||
            !code->captureOffsetStack(alloc(), offsetStack, stackDepth))
        {
            reportOOM();
            return false;
        }
    } else {
        code->mergeOffsetStack(offsetStack, stackDepth);
    }

    if (offset < *currentOffset && !code->parsed) {
        // Backedge in a while/for loop, whose body has not been parsed due
        // to a lack of fallthrough at the loop head. Roll back the offset
        // to analyze the body.
        *currentOffset = offset;
    }

    return true;
}

bool
BytecodeParser::parse()
{
    MOZ_ASSERT(!codeArray_);

    uint32_t length = script_->length();
    codeArray_ = alloc().newArray<Bytecode*>(length);

    if (!codeArray_) {
        reportOOM();
        return false;
    }

    mozilla::PodZero(codeArray_, length);

    // Fill in stack depth and definitions at initial bytecode.
    Bytecode* startcode = alloc().new_<Bytecode>();
    if (!startcode) {
        reportOOM();
        return false;
    }

    // Fill in stack depth and definitions at initial bytecode.
    uint32_t* offsetStack = alloc().newArray<uint32_t>(maximumStackDepth());
    if (maximumStackDepth() && !offsetStack) {
        reportOOM();
        return false;
    }

    startcode->stackDepth = 0;
    codeArray_[0] = startcode;

    uint32_t offset, nextOffset = 0;
    while (nextOffset < length) {
        offset = nextOffset;

        Bytecode* code = maybeCode(offset);
        jsbytecode* pc = script_->offsetToPC(offset);

        JSOp op = (JSOp)*pc;
        MOZ_ASSERT(op < JSOP_LIMIT);

        // Immediate successor of this bytecode.
        uint32_t successorOffset = offset + GetBytecodeLength(pc);

        // Next bytecode to analyze.  This is either the successor, or is an
        // earlier bytecode if this bytecode has a loop backedge.
        nextOffset = successorOffset;

        if (!code) {
            // Haven't found a path by which this bytecode is reachable.
            continue;
        }

        if (code->parsed) {
            // No need to reparse.
            continue;
        }

        code->parsed = true;

        uint32_t stackDepth = simulateOp(op, offset, offsetStack, code->stackDepth);

        switch (op) {
          case JSOP_TABLESWITCH: {
            uint32_t defaultOffset = offset + GET_JUMP_OFFSET(pc);
            jsbytecode* pc2 = pc + JUMP_OFFSET_LEN;
            int32_t low = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;
            int32_t high = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;

            if (!addJump(defaultOffset, &nextOffset, stackDepth, offsetStack))
                return false;

            for (int32_t i = low; i <= high; i++) {
                uint32_t targetOffset = offset + GET_JUMP_OFFSET(pc2);
                if (targetOffset != offset) {
                    if (!addJump(targetOffset, &nextOffset, stackDepth, offsetStack))
                        return false;
                }
                pc2 += JUMP_OFFSET_LEN;
            }
            break;
          }

          case JSOP_TRY: {
            // Everything between a try and corresponding catch or finally is conditional.
            // Note that there is no problem with code which is skipped by a thrown
            // exception but is not caught by a later handler in the same function:
            // no more code will execute, and it does not matter what is defined.
            JSTryNote* tn = script_->trynotes()->vector;
            JSTryNote* tnlimit = tn + script_->trynotes()->length;
            for (; tn < tnlimit; tn++) {
                uint32_t startOffset = script_->mainOffset() + tn->start;
                if (startOffset == offset + 1) {
                    uint32_t catchOffset = startOffset + tn->length;
                    if (tn->kind == JSTRY_CATCH || tn->kind == JSTRY_FINALLY) {
                        if (!addJump(catchOffset, &nextOffset, stackDepth, offsetStack))
                            return false;
                    }
                }
            }
            break;
          }

          default:
            break;
        }

        // Check basic jump opcodes, which may or may not have a fallthrough.
        if (IsJumpOpcode(op)) {
            // Case instructions do not push the lvalue back when branching.
            uint32_t newStackDepth = stackDepth;
            if (op == JSOP_CASE)
                newStackDepth--;

            uint32_t targetOffset = offset + GET_JUMP_OFFSET(pc);
            if (!addJump(targetOffset, &nextOffset, newStackDepth, offsetStack))
                return false;
        }

        // Handle any fallthrough from this opcode.
        if (BytecodeFallsThrough(op)) {
            MOZ_ASSERT(successorOffset < script_->length());

            Bytecode*& nextcode = codeArray_[successorOffset];

            if (!nextcode) {
                nextcode = alloc().new_<Bytecode>();
                if (!nextcode) {
                    reportOOM();
                    return false;
                }
                if (!nextcode->captureOffsetStack(alloc(), offsetStack, stackDepth)) {
                    reportOOM();
                    return false;
                }
            } else {
                nextcode->mergeOffsetStack(offsetStack, stackDepth);
            }
        }
    }

    return true;
}

#ifdef DEBUG

bool
js::ReconstructStackDepth(JSContext* cx, JSScript* script, jsbytecode* pc, uint32_t* depth, bool* reachablePC)
{
    BytecodeParser parser(cx, script);
    if (!parser.parse())
        return false;

    *reachablePC = parser.isReachable(pc);

    if (*reachablePC)
        *depth = parser.stackDepthAtPC(pc);

    return true;
}

/*
 * If pc != nullptr, include a prefix indicating whether the PC is at the
 * current line. If showAll is true, include the source note type and the
 * entry stack depth.
 */
static bool
DisassembleAtPC(JSContext* cx, JSScript* scriptArg, bool lines,
                jsbytecode* pc, bool showAll, Sprinter* sp)
{
    RootedScript script(cx, scriptArg);
    BytecodeParser parser(cx, script);

    if (showAll && !parser.parse())
        return false;

    if (showAll)
        Sprint(sp, "%s:%" PRIuSIZE "\n", script->filename(), script->lineno());

    if (pc != nullptr)
        sp->put("    ");
    if (showAll)
        sp->put("sn stack ");
    sp->put("loc   ");
    if (lines)
        sp->put("line");
    sp->put("  op\n");

    if (pc != nullptr)
        sp->put("    ");
    if (showAll)
        sp->put("-- ----- ");
    sp->put("----- ");
    if (lines)
        sp->put("----");
    sp->put("  --\n");

    jsbytecode* next = script->code();
    jsbytecode* end = script->codeEnd();
    while (next < end) {
        if (next == script->main())
            sp->put("main:\n");
        if (pc != nullptr) {
            if (pc == next)
                sp->put("--> ");
            else
                sp->put("    ");
        }
        if (showAll) {
            jssrcnote* sn = GetSrcNote(cx, script, next);
            if (sn) {
                MOZ_ASSERT(!SN_IS_TERMINATOR(sn));
                jssrcnote* next = SN_NEXT(sn);
                while (!SN_IS_TERMINATOR(next) && SN_DELTA(next) == 0) {
                    Sprint(sp, "%02u\n    ", SN_TYPE(sn));
                    sn = next;
                    next = SN_NEXT(sn);
                }
                Sprint(sp, "%02u ", SN_TYPE(sn));
            }
            else
                sp->put("   ");
            if (parser.isReachable(next))
                Sprint(sp, "%05u ", parser.stackDepthAtPC(next));
            else
                Sprint(sp, "      ");
        }
        unsigned len = Disassemble1(cx, script, next, script->pcToOffset(next), lines, sp);
        if (!len)
            return false;
        next += len;
    }
    return true;
}

bool
js::Disassemble(JSContext* cx, HandleScript script, bool lines, Sprinter* sp)
{
    return DisassembleAtPC(cx, script, lines, nullptr, false, sp);
}

JS_FRIEND_API(bool)
js::DumpPC(JSContext* cx)
{
    gc::AutoSuppressGC suppressGC(cx);
    Sprinter sprinter(cx);
    if (!sprinter.init())
        return false;
    ScriptFrameIter iter(cx);
    if (iter.done()) {
        fprintf(stdout, "Empty stack.\n");
        return true;
    }
    RootedScript script(cx, iter.script());
    bool ok = DisassembleAtPC(cx, script, true, iter.pc(), false, &sprinter);
    fprintf(stdout, "%s", sprinter.string());
    return ok;
}

JS_FRIEND_API(bool)
js::DumpScript(JSContext* cx, JSScript* scriptArg)
{
    gc::AutoSuppressGC suppressGC(cx);
    Sprinter sprinter(cx);
    if (!sprinter.init())
        return false;
    RootedScript script(cx, scriptArg);
    bool ok = Disassemble(cx, script, true, &sprinter);
    fprintf(stdout, "%s", sprinter.string());
    return ok;
}

static bool
ToDisassemblySource(JSContext* cx, HandleValue v, JSAutoByteString* bytes)
{
    if (v.isString()) {
        Sprinter sprinter(cx);
        if (!sprinter.init())
            return false;
        char* nbytes = QuoteString(&sprinter, v.toString(), '"');
        if (!nbytes)
            return false;
        nbytes = JS_sprintf_append(nullptr, "%s", nbytes);
        if (!nbytes) {
            ReportOutOfMemory(cx);
            return false;
        }
        bytes->initBytes(nbytes);
        return true;
    }

    JSRuntime* rt = cx->runtime();
    if (rt->isHeapBusy() || !rt->gc.isAllocAllowed()) {
        char* source = JS_sprintf_append(nullptr, "<value>");
        if (!source) {
            ReportOutOfMemory(cx);
            return false;
        }
        bytes->initBytes(source);
        return true;
    }

    if (v.isObject()) {
        JSObject& obj = v.toObject();
        if (obj.is<StaticBlockObject>()) {
            Rooted<StaticBlockObject*> block(cx, &obj.as<StaticBlockObject>());
            char* source = JS_sprintf_append(nullptr, "depth %d {", block->localOffset());
            if (!source) {
                ReportOutOfMemory(cx);
                return false;
            }

            Shape::Range<CanGC> r(cx, block->lastProperty());

            while (!r.empty()) {
                Rooted<Shape*> shape(cx, &r.front());
                JSAtom* atom = JSID_IS_INT(shape->propid())
                               ? cx->names().empty
                               : JSID_TO_ATOM(shape->propid());

                JSAutoByteString bytes;
                if (!AtomToPrintableString(cx, atom, &bytes))
                    return false;

                r.popFront();
                source = JS_sprintf_append(source, "%s: %d%s",
                                           bytes.ptr(),
                                           block->shapeToIndex(*shape),
                                           !r.empty() ? ", " : "");
                if (!source) {
                    ReportOutOfMemory(cx);
                    return false;
                }
            }

            source = JS_sprintf_append(source, "}");
            if (!source) {
                ReportOutOfMemory(cx);
                return false;
            }
            bytes->initBytes(source);
            return true;
        }

        if (obj.is<JSFunction>()) {
            RootedFunction fun(cx, &obj.as<JSFunction>());
            JSString* str = JS_DecompileFunction(cx, fun, JS_DONT_PRETTY_PRINT);
            if (!str)
                return false;
            return bytes->encodeLatin1(cx, str);
        }

        if (obj.is<RegExpObject>()) {
            JSString* source = obj.as<RegExpObject>().toString(cx);
            if (!source)
                return false;
            return bytes->encodeLatin1(cx, source);
        }
    }

    return !!ValueToPrintable(cx, v, bytes, true);
}

unsigned
js::Disassemble1(JSContext* cx, HandleScript script, jsbytecode* pc,
                 unsigned loc, bool lines, Sprinter* sp)
{
    JSOp op = (JSOp)*pc;
    if (op >= JSOP_LIMIT) {
        char numBuf1[12], numBuf2[12];
        JS_snprintf(numBuf1, sizeof numBuf1, "%d", op);
        JS_snprintf(numBuf2, sizeof numBuf2, "%d", JSOP_LIMIT);
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                             JSMSG_BYTECODE_TOO_BIG, numBuf1, numBuf2);
        return 0;
    }
    const JSCodeSpec* cs = &CodeSpec[op];
    ptrdiff_t len = (ptrdiff_t) cs->length;
    Sprint(sp, "%05u:", loc);
    if (lines)
        Sprint(sp, "%4u", PCToLineNumber(script, pc));
    Sprint(sp, "  %s", CodeName[op]);

    switch (JOF_TYPE(cs->format)) {
      case JOF_BYTE:
          // Scan the trynotes to find the associated catch block
          // and make the try opcode look like a jump instruction
          // with an offset. This simplifies code coverage analysis
          // based on this disassembled output.
          if (op == JSOP_TRY) {
              TryNoteArray* trynotes = script->trynotes();
              uint32_t i;
              for(i = 0; i < trynotes->length; i++) {
                  JSTryNote note = trynotes->vector[i];
                  if (note.kind == JSTRY_CATCH && note.start == loc + 1) {
                      Sprint(sp, " %u (%+d)",
                             (unsigned int) (loc+note.length+1),
                             (int) (note.length+1));
                      break;
                  }
              }
          }
        break;

      case JOF_JUMP: {
        ptrdiff_t off = GET_JUMP_OFFSET(pc);
        Sprint(sp, " %u (%+d)", loc + (int) off, (int) off);
        break;
      }

      case JOF_SCOPECOORD: {
        RootedValue v(cx,
            StringValue(ScopeCoordinateName(cx->runtime()->scopeCoordinateNameCache, script, pc)));
        JSAutoByteString bytes;
        if (!ToDisassemblySource(cx, v, &bytes))
            return 0;
        ScopeCoordinate sc(pc);
        Sprint(sp, " %s (hops = %u, slot = %u)", bytes.ptr(), sc.hops(), sc.slot());
        break;
      }

      case JOF_ATOM: {
        RootedValue v(cx, StringValue(script->getAtom(GET_UINT32_INDEX(pc))));
        JSAutoByteString bytes;
        if (!ToDisassemblySource(cx, v, &bytes))
            return 0;
        Sprint(sp, " %s", bytes.ptr());
        break;
      }

      case JOF_DOUBLE: {
        RootedValue v(cx, script->getConst(GET_UINT32_INDEX(pc)));
        JSAutoByteString bytes;
        if (!ToDisassemblySource(cx, v, &bytes))
            return 0;
        Sprint(sp, " %s", bytes.ptr());
        break;
      }

      case JOF_OBJECT: {
        /* Don't call obj.toSource if analysis/inference is active. */
        if (script->zone()->types.activeAnalysis) {
            Sprint(sp, " object");
            break;
        }

        JSObject* obj = script->getObject(GET_UINT32_INDEX(pc));
        {
            JSAutoByteString bytes;
            RootedValue v(cx, ObjectValue(*obj));
            if (!ToDisassemblySource(cx, v, &bytes))
                return 0;
            Sprint(sp, " %s", bytes.ptr());
        }
        break;
      }

      case JOF_REGEXP: {
        JSObject* obj = script->getRegExp(GET_UINT32_INDEX(pc));
        JSAutoByteString bytes;
        RootedValue v(cx, ObjectValue(*obj));
        if (!ToDisassemblySource(cx, v, &bytes))
            return 0;
        Sprint(sp, " %s", bytes.ptr());
        break;
      }

      case JOF_TABLESWITCH:
      {
        int32_t i, low, high;

        ptrdiff_t off = GET_JUMP_OFFSET(pc);
        jsbytecode* pc2 = pc + JUMP_OFFSET_LEN;
        low = GET_JUMP_OFFSET(pc2);
        pc2 += JUMP_OFFSET_LEN;
        high = GET_JUMP_OFFSET(pc2);
        pc2 += JUMP_OFFSET_LEN;
        Sprint(sp, " defaultOffset %d low %d high %d", int(off), low, high);
        for (i = low; i <= high; i++) {
            off = GET_JUMP_OFFSET(pc2);
            Sprint(sp, "\n\t%d: %d", i, int(off));
            pc2 += JUMP_OFFSET_LEN;
        }
        len = 1 + pc2 - pc;
        break;
      }

      case JOF_QARG:
        Sprint(sp, " %u", GET_ARGNO(pc));
        break;

      case JOF_LOCAL:
        Sprint(sp, " %u", GET_LOCALNO(pc));
        break;

      case JOF_UINT32:
        Sprint(sp, " %u", GET_UINT32(pc));
        break;

      {
        int i;

      case JOF_UINT16:
        i = (int)GET_UINT16(pc);
        goto print_int;

      case JOF_UINT24:
        MOZ_ASSERT(len == 4);
        i = (int)GET_UINT24(pc);
        goto print_int;

      case JOF_UINT8:
        i = GET_UINT8(pc);
        goto print_int;

      case JOF_INT8:
        i = GET_INT8(pc);
        goto print_int;

      case JOF_INT32:
        MOZ_ASSERT(op == JSOP_INT32);
        i = GET_INT32(pc);
      print_int:
        Sprint(sp, " %d", i);
        break;
      }

      default: {
        char numBuf[12];
        JS_snprintf(numBuf, sizeof numBuf, "%lx", (unsigned long) cs->format);
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                             JSMSG_UNKNOWN_FORMAT, numBuf);
        return 0;
      }
    }
    sp->put("\n");
    return len;
}

#endif /* DEBUG */

namespace {
/*
 * The expression decompiler is invoked by error handling code to produce a
 * string representation of the erroring expression. As it's only a debugging
 * tool, it only supports basic expressions. For anything complicated, it simply
 * puts "(intermediate value)" into the error result.
 *
 * Here's the basic algorithm:
 *
 * 1. Find the stack location of the value whose expression we wish to
 * decompile. The error handler can explicitly pass this as an
 * argument. Otherwise, we search backwards down the stack for the offending
 * value.
 *
 * 2. Instantiate and run a BytecodeParser for the current frame. This creates a
 * stack of pcs parallel to the interpreter stack; given an interpreter stack
 * location, the corresponding pc stack location contains the opcode that pushed
 * the value in the interpreter. Now, with the result of step 1, we have the
 * opcode responsible for pushing the value we want to decompile.
 *
 * 3. Pass the opcode to decompilePC. decompilePC is the main decompiler
 * routine, responsible for a string representation of the expression that
 * generated a certain stack location. decompilePC looks at one opcode and
 * returns the JS source equivalent of that opcode.
 *
 * 4. Expressions can, of course, contain subexpressions. For example, the
 * literals "4" and "5" are subexpressions of the addition operator in "4 +
 * 5". If we need to decompile a subexpression, we call decompilePC (step 2)
 * recursively on the operands' pcs. The result is a depth-first traversal of
 * the expression tree.
 *
 */
struct ExpressionDecompiler
{
    JSContext* cx;
    RootedScript script;
    RootedFunction fun;
    BytecodeParser parser;
    Sprinter sprinter;

    ExpressionDecompiler(JSContext* cx, JSScript* script, JSFunction* fun)
        : cx(cx),
          script(cx, script),
          fun(cx, fun),
          parser(cx, script),
          sprinter(cx)
    {}
    bool init();
    bool decompilePCForStackOperand(jsbytecode* pc, int i);
    bool decompilePC(jsbytecode* pc);
    JSAtom* getLocal(uint32_t local, jsbytecode* pc);
    JSAtom* getArg(unsigned slot);
    JSAtom* loadAtom(jsbytecode* pc);
    bool quote(JSString* s, uint32_t quote);
    bool write(const char* s);
    bool write(JSString* str);
    bool getOutput(char** out);
};

bool
ExpressionDecompiler::decompilePCForStackOperand(jsbytecode* pc, int i)
{
    pc = parser.pcForStackOperand(pc, i);
    if (!pc)
        return write("(intermediate value)");
    return decompilePC(pc);
}

bool
ExpressionDecompiler::decompilePC(jsbytecode* pc)
{
    MOZ_ASSERT(script->containsPC(pc));

    JSOp op = (JSOp)*pc;

    if (const char* token = CodeToken[op]) {
        // Handle simple cases of binary and unary operators.
        switch (CodeSpec[op].nuses) {
          case 2: {
            jssrcnote* sn = GetSrcNote(cx, script, pc);
            if (!sn || SN_TYPE(sn) != SRC_ASSIGNOP)
                return write("(") &&
                       decompilePCForStackOperand(pc, -2) &&
                       write(" ") &&
                       write(token) &&
                       write(" ") &&
                       decompilePCForStackOperand(pc, -1) &&
                       write(")");
            break;
          }
          case 1:
            return write(token) &&
                   write("(") &&
                   decompilePCForStackOperand(pc, -1) &&
                   write(")");
          default:
            break;
        }
    }

    switch (op) {
      case JSOP_GETGNAME:
      case JSOP_GETNAME:
      case JSOP_GETINTRINSIC:
        return write(loadAtom(pc));
      case JSOP_GETARG: {
        unsigned slot = GET_ARGNO(pc);
        JSAtom* atom = getArg(slot);
        return write(atom);
      }
      case JSOP_GETLOCAL: {
        uint32_t i = GET_LOCALNO(pc);
        if (JSAtom* atom = getLocal(i, pc))
            return write(atom);
        return write("(intermediate value)");
      }
      case JSOP_GETALIASEDVAR: {
        JSAtom* atom = ScopeCoordinateName(cx->runtime()->scopeCoordinateNameCache, script, pc);
        MOZ_ASSERT(atom);
        return write(atom);
      }
      case JSOP_LENGTH:
      case JSOP_GETPROP:
      case JSOP_CALLPROP: {
        RootedAtom prop(cx, (op == JSOP_LENGTH) ? cx->names().length : loadAtom(pc));
        if (!decompilePCForStackOperand(pc, -1))
            return false;
        if (IsIdentifier(prop)) {
            return write(".") &&
                   quote(prop, '\0');
        }
        return write("[") &&
               quote(prop, '\'') &&
               write("]");
      }
      case JSOP_GETPROP_SUPER:
      {
        RootedAtom prop(cx, loadAtom(pc));
        return write("super.") &&
               quote(prop, '\0');
      }
      case JSOP_GETELEM:
      case JSOP_CALLELEM:
        return decompilePCForStackOperand(pc, -2) &&
               write("[") &&
               decompilePCForStackOperand(pc, -1) &&
               write("]");
      case JSOP_GETELEM_SUPER:
        return write("super[") &&
               decompilePCForStackOperand(pc, -3) &&
               write("]");
      case JSOP_NULL:
        return write(js_null_str);
      case JSOP_TRUE:
        return write(js_true_str);
      case JSOP_FALSE:
        return write(js_false_str);
      case JSOP_ZERO:
      case JSOP_ONE:
      case JSOP_INT8:
      case JSOP_UINT16:
      case JSOP_UINT24:
      case JSOP_INT32:
        return sprinter.printf("%d", GetBytecodeInteger(pc)) >= 0;
      case JSOP_STRING:
        return quote(loadAtom(pc), '"');
      case JSOP_SYMBOL: {
        unsigned i = uint8_t(pc[1]);
        MOZ_ASSERT(i < JS::WellKnownSymbolLimit);
        if (i < JS::WellKnownSymbolLimit)
            return write(cx->names().wellKnownSymbolDescriptions()[i]);
        break;
      }
      case JSOP_UNDEFINED:
        return write(js_undefined_str);
      case JSOP_GLOBALTHIS:
        // |this| could convert to a very long object initialiser, so cite it by
        // its keyword name.
        return write(js_this_str);
      case JSOP_NEWTARGET:
        return write("new.target");
      case JSOP_CALL:
      case JSOP_CALLITER:
      case JSOP_FUNCALL:
        return decompilePCForStackOperand(pc, -int32_t(GET_ARGC(pc) + 2)) &&
               write("(...)");
      case JSOP_SPREADCALL:
        return decompilePCForStackOperand(pc, -int32_t(3)) &&
               write("(...)");
      case JSOP_NEWARRAY:
        return write("[]");
      case JSOP_REGEXP:
      case JSOP_OBJECT:
      case JSOP_NEWARRAY_COPYONWRITE: {
        JSObject* obj = (op == JSOP_REGEXP)
                        ? script->getRegExp(GET_UINT32_INDEX(pc))
                        : script->getObject(GET_UINT32_INDEX(pc));
        RootedValue objv(cx, ObjectValue(*obj));
        JSString* str = ValueToSource(cx, objv);
        if (!str)
            return false;
        return write(str);
      }
      case JSOP_VOID:
        return write("void ") && decompilePCForStackOperand(pc, -1);
      default:
        break;
    }
    return write("(intermediate value)");
}

bool
ExpressionDecompiler::init()
{
    assertSameCompartment(cx, script);

    if (!sprinter.init())
        return false;

    if (!parser.parse())
        return false;

    return true;
}

bool
ExpressionDecompiler::write(const char* s)
{
    return sprinter.put(s) >= 0;
}

bool
ExpressionDecompiler::write(JSString* str)
{
    if (str == cx->names().dotThis)
        return write("this");
    return sprinter.putString(str) >= 0;
}

bool
ExpressionDecompiler::quote(JSString* s, uint32_t quote)
{
    return QuoteString(&sprinter, s, quote) != nullptr;
}

JSAtom*
ExpressionDecompiler::loadAtom(jsbytecode* pc)
{
    return script->getAtom(GET_UINT32_INDEX(pc));
}

JSAtom*
ExpressionDecompiler::getArg(unsigned slot)
{
    MOZ_ASSERT(fun);
    MOZ_ASSERT(slot < script->bindings.numArgs());

    for (BindingIter bi(script); bi; bi++) {
        MOZ_ASSERT(bi->kind() == Binding::ARGUMENT);
        if (bi.argIndex() == slot)
            return bi->name();
    }

    MOZ_CRASH("No binding");
}

JSAtom*
ExpressionDecompiler::getLocal(uint32_t local, jsbytecode* pc)
{
    MOZ_ASSERT(local < script->nfixed());
    if (local < script->nbodyfixed()) {
        for (BindingIter bi(script); bi; bi++) {
            if (bi->kind() != Binding::ARGUMENT && !bi->aliased() && bi.frameIndex() == local)
                return bi->name();
        }

        MOZ_CRASH("No binding");
    }
    for (NestedScopeObject* chain = script->getStaticBlockScope(pc);
         chain;
         chain = chain->enclosingNestedScope())
    {
        if (!chain->is<StaticBlockObject>())
            continue;
        StaticBlockObject& block = chain->as<StaticBlockObject>();
        if (local < block.localOffset())
            continue;
        local -= block.localOffset();
        if (local >= block.numVariables())
            return nullptr;
        for (Shape::Range<NoGC> r(block.lastProperty()); !r.empty(); r.popFront()) {
            const Shape& shape = r.front();
            if (block.shapeToIndex(shape) == local)
                return JSID_TO_ATOM(shape.propid());
        }
        break;
    }
    return nullptr;
}

bool
ExpressionDecompiler::getOutput(char** res)
{
    ptrdiff_t len = sprinter.stringEnd() - sprinter.stringAt(0);
    *res = cx->pod_malloc<char>(len + 1);
    if (!*res)
        return false;
    js_memcpy(*res, sprinter.stringAt(0), len);
    (*res)[len] = 0;
    return true;
}

}  // anonymous namespace

static bool
FindStartPC(JSContext* cx, const FrameIter& iter, int spindex, int skipStackHits, Value v,
            jsbytecode** valuepc)
{
    jsbytecode* current = *valuepc;

    if (spindex == JSDVG_IGNORE_STACK)
        return true;

    /*
     * FIXME: Fall back if iter.isIon(), since the stack snapshot may be for the
     * previous pc (see bug 831120).
     */
    if (iter.isIon())
        return true;

    *valuepc = nullptr;

    BytecodeParser parser(cx, iter.script());
    if (!parser.parse())
        return false;

    if (spindex < 0 && spindex + int(parser.stackDepthAtPC(current)) < 0)
        spindex = JSDVG_SEARCH_STACK;

    if (spindex == JSDVG_SEARCH_STACK) {
        size_t index = iter.numFrameSlots();

        // The decompiler may be called from inside functions that are not
        // called from script, but via the C++ API directly, such as
        // Invoke. In that case, the youngest script frame may have a
        // completely unrelated pc and stack depth, so we give up.
        if (index < size_t(parser.stackDepthAtPC(current)))
            return true;

        // We search from fp->sp to base to find the most recently calculated
        // value matching v under assumption that it is the value that caused
        // the exception.
        int stackHits = 0;
        Value s;
        do {
            if (!index)
                return true;
            s = iter.frameSlotValue(--index);
        } while (s != v || stackHits++ != skipStackHits);

        // If the current PC has fewer values on the stack than the index we are
        // looking for, the blamed value must be one pushed by the current
        // bytecode, so restore *valuepc.
        jsbytecode* pc = nullptr;
        if (index < size_t(parser.stackDepthAtPC(current)))
            pc = parser.pcForStackOperand(current, index);
        *valuepc = pc ? pc : current;
    } else {
        jsbytecode* pc = parser.pcForStackOperand(current, spindex);
        *valuepc = pc ? pc : current;
    }
    return true;
}

static bool
DecompileExpressionFromStack(JSContext* cx, int spindex, int skipStackHits, HandleValue v, char** res)
{
    MOZ_ASSERT(spindex < 0 ||
               spindex == JSDVG_IGNORE_STACK ||
               spindex == JSDVG_SEARCH_STACK);

    *res = nullptr;

#ifdef JS_MORE_DETERMINISTIC
    /*
     * Give up if we need deterministic behavior for differential testing.
     * IonMonkey doesn't use InterpreterFrames and this ensures we get the same
     * error messages.
     */
    return true;
#endif

    FrameIter frameIter(cx);

    if (frameIter.done() || !frameIter.hasScript())
        return true;

    RootedScript script(cx, frameIter.script());
    AutoCompartment ac(cx, &script->global());
    jsbytecode* valuepc = frameIter.pc();
    RootedFunction fun(cx, frameIter.isFunctionFrame()
                           ? frameIter.calleeTemplate()
                           : nullptr);

    MOZ_ASSERT(script->containsPC(valuepc));

    // Give up if in prologue.
    if (valuepc < script->main())
        return true;

    if (!FindStartPC(cx, frameIter, spindex, skipStackHits, v, &valuepc))
        return false;
    if (!valuepc)
        return true;

    ExpressionDecompiler ed(cx, script, fun);
    if (!ed.init())
        return false;
    if (!ed.decompilePC(valuepc))
        return false;

    return ed.getOutput(res);
}

typedef mozilla::UniquePtr<char[], JS::FreePolicy> UniquePtrChars;

UniquePtrChars
js::DecompileValueGenerator(JSContext* cx, int spindex, HandleValue v,
                            HandleString fallbackArg, int skipStackHits)
{
    RootedString fallback(cx, fallbackArg);
    {
        char* result;
        if (!DecompileExpressionFromStack(cx, spindex, skipStackHits, v, &result))
            return nullptr;
        if (result) {
            if (strcmp(result, "(intermediate value)"))
                return UniquePtrChars(result);
            js_free(result);
        }
    }
    if (!fallback) {
        if (v.isUndefined())
            return UniquePtrChars(JS_strdup(cx, js_undefined_str)); // Prevent users from seeing "(void 0)"
        fallback = ValueToSource(cx, v);
        if (!fallback)
            return UniquePtrChars(nullptr);
    }

    return UniquePtrChars(JS_EncodeString(cx, fallback));
}

static bool
DecompileArgumentFromStack(JSContext* cx, int formalIndex, char** res)
{
    MOZ_ASSERT(formalIndex >= 0);

    *res = nullptr;

#ifdef JS_MORE_DETERMINISTIC
    /* See note in DecompileExpressionFromStack. */
    return true;
#endif

    /*
     * Settle on the nearest script frame, which should be the builtin that
     * called the intrinsic.
     */
    FrameIter frameIter(cx);
    MOZ_ASSERT(!frameIter.done());

    /*
     * Get the second-to-top frame, the caller of the builtin that called the
     * intrinsic.
     */
    ++frameIter;
    if (frameIter.done() || !frameIter.hasScript())
        return true;

    RootedScript script(cx, frameIter.script());
    AutoCompartment ac(cx, &script->global());
    jsbytecode* current = frameIter.pc();
    RootedFunction fun(cx, frameIter.isFunctionFrame()
                           ? frameIter.calleeTemplate()
                           : nullptr);

    MOZ_ASSERT(script->containsPC(current));

    if (current < script->main())
        return true;

    /* Don't handle getters, setters or calls from fun.call/fun.apply. */
    if (JSOp(*current) != JSOP_CALL || static_cast<unsigned>(formalIndex) >= GET_ARGC(current))
        return true;

    BytecodeParser parser(cx, script);
    if (!parser.parse())
        return false;

    int formalStackIndex = parser.stackDepthAtPC(current) - GET_ARGC(current) + formalIndex;
    MOZ_ASSERT(formalStackIndex >= 0);
    if (uint32_t(formalStackIndex) >= parser.stackDepthAtPC(current))
        return true;

    ExpressionDecompiler ed(cx, script, fun);
    if (!ed.init())
        return false;
    if (!ed.decompilePCForStackOperand(current, formalStackIndex))
        return false;

    return ed.getOutput(res);
}

char*
js::DecompileArgument(JSContext* cx, int formalIndex, HandleValue v)
{
    {
        char* result;
        if (!DecompileArgumentFromStack(cx, formalIndex, &result))
            return nullptr;
        if (result) {
            if (strcmp(result, "(intermediate value)"))
                return result;
            js_free(result);
        }
    }
    if (v.isUndefined())
        return JS_strdup(cx, js_undefined_str); // Prevent users from seeing "(void 0)"

    RootedString fallback(cx, ValueToSource(cx, v));
    if (!fallback)
        return nullptr;

    return JS_EncodeString(cx, fallback);
}

bool
js::CallResultEscapes(jsbytecode* pc)
{
    /*
     * If we see any of these sequences, the result is unused:
     * - call / pop
     *
     * If we see any of these sequences, the result is only tested for nullness:
     * - call / ifeq
     * - call / not / ifeq
     */

    if (*pc == JSOP_CALL)
        pc += JSOP_CALL_LENGTH;
    else if (*pc == JSOP_SPREADCALL)
        pc += JSOP_SPREADCALL_LENGTH;
    else
        return true;

    if (*pc == JSOP_POP)
        return false;

    if (*pc == JSOP_NOT)
        pc += JSOP_NOT_LENGTH;

    return *pc != JSOP_IFEQ;
}

extern bool
js::IsValidBytecodeOffset(JSContext* cx, JSScript* script, size_t offset)
{
    // This could be faster (by following jump instructions if the target is <= offset).
    for (BytecodeRange r(cx, script); !r.empty(); r.popFront()) {
        size_t here = r.frontOffset();
        if (here >= offset)
            return here == offset;
    }
    return false;
}

/*
 * There are three possible PCCount profiling states:
 *
 * 1. None: Neither scripts nor the runtime have count information.
 * 2. Profile: Active scripts have count information, the runtime does not.
 * 3. Query: Scripts do not have count information, the runtime does.
 *
 * When starting to profile scripts, counting begins immediately, with all JIT
 * code discarded and recompiled with counts as necessary. Active interpreter
 * frames will not begin profiling until they begin executing another script
 * (via a call or return).
 *
 * The below API functions manage transitions to new states, according
 * to the table below.
 *
 *                                  Old State
 *                          -------------------------
 * Function                 None      Profile   Query
 * --------
 * StartPCCountProfiling    Profile   Profile   Profile
 * StopPCCountProfiling     None      Query     Query
 * PurgePCCounts            None      None      None
 */

static void
ReleaseScriptCounts(FreeOp* fop)
{
    JSRuntime* rt = fop->runtime();
    MOZ_ASSERT(rt->scriptAndCountsVector);

    fop->delete_(rt->scriptAndCountsVector);
    rt->scriptAndCountsVector = nullptr;
}

JS_FRIEND_API(void)
js::StartPCCountProfiling(JSContext* cx)
{
    JSRuntime* rt = cx->runtime();

    if (rt->profilingScripts)
        return;

    if (rt->scriptAndCountsVector)
        ReleaseScriptCounts(rt->defaultFreeOp());

    ReleaseAllJITCode(rt->defaultFreeOp());

    rt->profilingScripts = true;
}

JS_FRIEND_API(void)
js::StopPCCountProfiling(JSContext* cx)
{
    JSRuntime* rt = cx->runtime();

    if (!rt->profilingScripts)
        return;
    MOZ_ASSERT(!rt->scriptAndCountsVector);

    ReleaseAllJITCode(rt->defaultFreeOp());

    auto* vec = cx->new_<PersistentRooted<ScriptAndCountsVector>>(cx,
        ScriptAndCountsVector(SystemAllocPolicy()));
    if (!vec)
        return;

    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        for (ZoneCellIter i(zone, AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            if (script->hasScriptCounts() && script->types()) {
                if (!vec->append(script))
                    return;
            }
        }
    }

    rt->profilingScripts = false;
    rt->scriptAndCountsVector = vec;
}

JS_FRIEND_API(void)
js::PurgePCCounts(JSContext* cx)
{
    JSRuntime* rt = cx->runtime();

    if (!rt->scriptAndCountsVector)
        return;
    MOZ_ASSERT(!rt->profilingScripts);

    ReleaseScriptCounts(rt->defaultFreeOp());
}

JS_FRIEND_API(size_t)
js::GetPCCountScriptCount(JSContext* cx)
{
    JSRuntime* rt = cx->runtime();

    if (!rt->scriptAndCountsVector)
        return 0;

    return rt->scriptAndCountsVector->length();
}

enum MaybeComma {NO_COMMA, COMMA};

static void
AppendJSONProperty(StringBuffer& buf, const char* name, MaybeComma comma = COMMA)
{
    if (comma)
        buf.append(',');

    buf.append('\"');
    buf.append(name, strlen(name));
    buf.append("\":", 2);
}

JS_FRIEND_API(JSString*)
js::GetPCCountScriptSummary(JSContext* cx, size_t index)
{
    JSRuntime* rt = cx->runtime();

    if (!rt->scriptAndCountsVector || index >= rt->scriptAndCountsVector->length()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BUFFER_TOO_SMALL);
        return nullptr;
    }

    const ScriptAndCounts& sac = (*rt->scriptAndCountsVector)[index];
    RootedScript script(cx, sac.script);

    /*
     * OOM on buffer appends here will not be caught immediately, but since
     * StringBuffer uses a TempAllocPolicy will trigger an exception on the
     * context if they occur, which we'll catch before returning.
     */
    StringBuffer buf(cx);

    buf.append('{');

    AppendJSONProperty(buf, "file", NO_COMMA);
    JSString* str = JS_NewStringCopyZ(cx, script->filename());
    if (!str || !(str = StringToSource(cx, str)))
        return nullptr;
    buf.append(str);

    AppendJSONProperty(buf, "line");
    NumberValueToStringBuffer(cx, Int32Value(script->lineno()), buf);

    if (script->functionNonDelazifying()) {
        JSAtom* atom = script->functionNonDelazifying()->displayAtom();
        if (atom) {
            AppendJSONProperty(buf, "name");
            if (!(str = StringToSource(cx, atom)))
                return nullptr;
            buf.append(str);
        }
    }

    uint64_t total = 0;

    jsbytecode* codeEnd = script->codeEnd();
    for (jsbytecode* pc = script->code(); pc < codeEnd; pc = GetNextPc(pc)) {
        const PCCounts* counts = sac.maybeGetPCCounts(pc);
        if (!counts)
            continue;
        total += counts->numExec();
    }

    AppendJSONProperty(buf, "totals");
    buf.append('{');

    AppendJSONProperty(buf, PCCounts::numExecName, NO_COMMA);
    NumberValueToStringBuffer(cx, DoubleValue(total), buf);

    uint64_t ionActivity = 0;
    jit::IonScriptCounts* ionCounts = sac.getIonCounts();
    while (ionCounts) {
        for (size_t i = 0; i < ionCounts->numBlocks(); i++)
            ionActivity += ionCounts->block(i).hitCount();
        ionCounts = ionCounts->previous();
    }
    if (ionActivity) {
        AppendJSONProperty(buf, "ion", COMMA);
        NumberValueToStringBuffer(cx, DoubleValue(ionActivity), buf);
    }

    buf.append('}');
    buf.append('}');

    if (cx->isExceptionPending())
        return nullptr;

    return buf.finishString();
}

static bool
GetPCCountJSON(JSContext* cx, const ScriptAndCounts& sac, StringBuffer& buf)
{
    RootedScript script(cx, sac.script);

    buf.append('{');
    AppendJSONProperty(buf, "text", NO_COMMA);

    JSString* str = JS_DecompileScript(cx, script, nullptr, 0);
    if (!str || !(str = StringToSource(cx, str)))
        return false;

    buf.append(str);

    AppendJSONProperty(buf, "line");
    NumberValueToStringBuffer(cx, Int32Value(script->lineno()), buf);

    AppendJSONProperty(buf, "opcodes");
    buf.append('[');
    bool comma = false;

    SrcNoteLineScanner scanner(script->notes(), script->lineno());
    uint64_t hits = 0;

    jsbytecode* end = script->codeEnd();
    for (jsbytecode* pc = script->code(); pc < end; pc = GetNextPc(pc)) {
        size_t offset = script->pcToOffset(pc);
        JSOp op = JSOp(*pc);

        // If the current instruction is a jump target,
        // then update the number of hits.
        const PCCounts* counts = sac.maybeGetPCCounts(pc);
        if (counts)
            hits = counts->numExec();

        if (comma)
            buf.append(',');
        comma = true;

        buf.append('{');

        AppendJSONProperty(buf, "id", NO_COMMA);
        NumberValueToStringBuffer(cx, Int32Value(offset), buf);

        scanner.advanceTo(offset);

        AppendJSONProperty(buf, "line");
        NumberValueToStringBuffer(cx, Int32Value(scanner.getLine()), buf);

        {
            const char* name = CodeName[op];
            AppendJSONProperty(buf, "name");
            buf.append('\"');
            buf.append(name, strlen(name));
            buf.append('\"');
        }

        {
            ExpressionDecompiler ed(cx, script, script->functionDelazifying());
            if (!ed.init())
                return false;
            if (!ed.decompilePC(pc))
                return false;
            char* text;
            if (!ed.getOutput(&text))
                return false;
            AppendJSONProperty(buf, "text");
            JSString* str = JS_NewStringCopyZ(cx, text);
            js_free(text);
            if (!str || !(str = StringToSource(cx, str)))
                return false;
            buf.append(str);
        }

        AppendJSONProperty(buf, "counts");
        buf.append('{');

        if (hits > 0) {
            AppendJSONProperty(buf, PCCounts::numExecName, NO_COMMA);
            NumberValueToStringBuffer(cx, DoubleValue(hits), buf);
        }

        buf.append('}');
        buf.append('}');

        // If the current instruction has thrown,
        // then decrement the hit counts with the number of throws.
        counts = sac.maybeGetThrowCounts(pc);
        if (counts)
            hits -= counts->numExec();
    }

    buf.append(']');

    jit::IonScriptCounts* ionCounts = sac.getIonCounts();
    if (ionCounts) {
        AppendJSONProperty(buf, "ion");
        buf.append('[');
        bool comma = false;
        while (ionCounts) {
            if (comma)
                buf.append(',');
            comma = true;

            buf.append('[');
            for (size_t i = 0; i < ionCounts->numBlocks(); i++) {
                if (i)
                    buf.append(',');
                const jit::IonBlockCounts& block = ionCounts->block(i);

                buf.append('{');
                AppendJSONProperty(buf, "id", NO_COMMA);
                NumberValueToStringBuffer(cx, Int32Value(block.id()), buf);
                AppendJSONProperty(buf, "offset");
                NumberValueToStringBuffer(cx, Int32Value(block.offset()), buf);
                AppendJSONProperty(buf, "successors");
                buf.append('[');
                for (size_t j = 0; j < block.numSuccessors(); j++) {
                    if (j)
                        buf.append(',');
                    NumberValueToStringBuffer(cx, Int32Value(block.successor(j)), buf);
                }
                buf.append(']');
                AppendJSONProperty(buf, "hits");
                NumberValueToStringBuffer(cx, DoubleValue(block.hitCount()), buf);

                AppendJSONProperty(buf, "code");
                JSString* str = JS_NewStringCopyZ(cx, block.code());
                if (!str || !(str = StringToSource(cx, str)))
                    return false;
                buf.append(str);
                buf.append('}');
            }
            buf.append(']');

            ionCounts = ionCounts->previous();
        }
        buf.append(']');
    }

    buf.append('}');

    return !cx->isExceptionPending();
}

JS_FRIEND_API(JSString*)
js::GetPCCountScriptContents(JSContext* cx, size_t index)
{
    JSRuntime* rt = cx->runtime();

    if (!rt->scriptAndCountsVector || index >= rt->scriptAndCountsVector->length()) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BUFFER_TOO_SMALL);
        return nullptr;
    }

    const ScriptAndCounts& sac = (*rt->scriptAndCountsVector)[index];
    JSScript* script = sac.script;

    StringBuffer buf(cx);

    {
        AutoCompartment ac(cx, &script->global());
        if (!GetPCCountJSON(cx, sac, buf))
            return nullptr;
    }

    return buf.finishString();
}

static bool
GenerateLcovInfo(JSContext* cx, JSCompartment* comp, GenericPrinter& out)
{
    JSRuntime* rt = cx->runtime();

    // Collect the list of scripts which are part of the current compartment.
    {
        js::gc::AutoPrepareForTracing apft(rt, SkipAtoms);
    }
    Rooted<ScriptVector> topScripts(cx, ScriptVector(cx));
    for (ZonesIter zone(rt, SkipAtoms); !zone.done(); zone.next()) {
        for (ZoneCellIter i(zone, AllocKind::SCRIPT); !i.done(); i.next()) {
            JSScript* script = i.get<JSScript>();
            if (script->compartment() != comp ||
                !script->isTopLevel() ||
                !script->filename())
            {
                continue;
            }

            if (!topScripts.append(script))
                return false;
        }
    }

    if (topScripts.length() == 0)
        return true;

    // Collect code coverage info for one compartment.
    coverage::LCovCompartment compCover;
    for (JSScript* topLevel: topScripts) {
        RootedScript topScript(cx, topLevel);
        compCover.collectSourceFile(comp, &topScript->scriptSourceUnwrap());

        // We found the top-level script, visit all the functions reachable
        // from the top-level function, and delazify them.
        Rooted<ScriptVector> queue(cx, ScriptVector(cx));
        if (!queue.append(topLevel))
            return false;

        RootedScript script(cx);
        do {
            script = queue.popCopy();
            compCover.collectCodeCoverageInfo(comp, script->sourceObject(), script);

            // Iterate from the last to the first object in order to have
            // the functions them visited in the opposite order when popping
            // elements from the stack of remaining scripts, such that the
            // functions are more-less listed with increasing line numbers.
            if (!script->hasObjects())
                continue;
            size_t idx = script->objects()->length;
            while (idx--) {
                JSObject* obj = script->getObject(idx);

                // Only continue on JSFunction objects.
                if (!obj->is<JSFunction>())
                    continue;
                JSFunction& fun = obj->as<JSFunction>();

                // Let's skip asm.js for now.
                if (!fun.isInterpreted())
                    continue;

                // Queue the script in the list of script associated to the
                // current source.
                JSScript* childScript = fun.getOrCreateScript(cx);
                if (!childScript || !queue.append(childScript))
                    return false;
            }
        } while (!queue.empty());
    }

    bool isEmpty = true;
    compCover.exportInto(out, &isEmpty);
    if (out.hadOutOfMemory())
        return false;
    return true;
}

JS_FRIEND_API(char*)
js::GetCodeCoverageSummary(JSContext* cx, size_t* length)
{
    Sprinter out(cx);

    if (!out.init())
        return nullptr;

    if (!GenerateLcovInfo(cx, cx->compartment(), out)) {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    if (out.hadOutOfMemory()) {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    ptrdiff_t len = out.stringEnd() - out.string();
    char* res = cx->pod_malloc<char>(len + 1);
    if (!res) {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }

    js_memcpy(res, out.string(), len);
    res[len] = 0;
    if (length)
        *length = len;
    return res;
}
