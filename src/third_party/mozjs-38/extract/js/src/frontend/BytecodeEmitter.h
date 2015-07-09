/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeEmitter_h
#define frontend_BytecodeEmitter_h

/*
 * JS bytecode generation.
 */

#include "jscntxt.h"
#include "jsopcode.h"
#include "jsscript.h"

#include "frontend/ParseMaps.h"
#include "frontend/SourceNotes.h"

namespace js {

class StaticEvalObject;

namespace frontend {

class FullParseHandler;
class ObjectBox;
class ParseNode;
template <typename ParseHandler> class Parser;
class SharedContext;
class TokenStream;

class CGConstList {
    Vector<Value> list;
  public:
    explicit CGConstList(ExclusiveContext* cx) : list(cx) {}
    bool append(Value v) { MOZ_ASSERT_IF(v.isString(), v.toString()->isAtom()); return list.append(v); }
    size_t length() const { return list.length(); }
    void finish(ConstArray* array);
};

struct CGObjectList {
    uint32_t            length;     /* number of emitted so far objects */
    ObjectBox*          lastbox;   /* last emitted object */

    CGObjectList() : length(0), lastbox(nullptr) {}

    unsigned add(ObjectBox* objbox);
    unsigned indexOf(JSObject* obj);
    void finish(ObjectArray* array);
    ObjectBox* find(uint32_t index);
};

struct CGTryNoteList {
    Vector<JSTryNote> list;
    explicit CGTryNoteList(ExclusiveContext* cx) : list(cx) {}

    bool append(JSTryNoteKind kind, uint32_t stackDepth, size_t start, size_t end);
    size_t length() const { return list.length(); }
    void finish(TryNoteArray* array);
};

struct CGBlockScopeList {
    Vector<BlockScopeNote> list;
    explicit CGBlockScopeList(ExclusiveContext* cx) : list(cx) {}

    bool append(uint32_t scopeObject, uint32_t offset, uint32_t parent);
    uint32_t findEnclosingScope(uint32_t index);
    void recordEnd(uint32_t index, uint32_t offset);
    size_t length() const { return list.length(); }
    void finish(BlockScopeArray* array);
};

struct CGYieldOffsetList {
    Vector<uint32_t> list;
    explicit CGYieldOffsetList(ExclusiveContext* cx) : list(cx) {}

    bool append(uint32_t offset) { return list.append(offset); }
    size_t length() const { return list.length(); }
    void finish(YieldOffsetArray& array, uint32_t prologLength);
};

struct StmtInfoBCE;

// Use zero inline elements because these go on the stack and affect how many
// nested functions are possible.
typedef Vector<jsbytecode, 0> BytecodeVector;
typedef Vector<jssrcnote, 0> SrcNotesVector;

struct BytecodeEmitter
{
    typedef StmtInfoBCE StmtInfo;

    SharedContext*  const sc;      /* context shared between parsing and bytecode generation */

    BytecodeEmitter* const parent;  /* enclosing function or global context */

    Rooted<JSScript*> script;       /* the JSScript we're ultimately producing */

    Rooted<LazyScript*> lazyScript; /* the lazy script if mode is LazyFunction,
                                        nullptr otherwise. */

    struct EmitSection {
        BytecodeVector code;        /* bytecode */
        SrcNotesVector notes;       /* source notes, see below */
        ptrdiff_t   lastNoteOffset; /* code offset for last source note */
        uint32_t    currentLine;    /* line number for tree-based srcnote gen */
        uint32_t    lastColumn;     /* zero-based column index on currentLine of
                                       last SRC_COLSPAN-annotated opcode */

        EmitSection(ExclusiveContext* cx, uint32_t lineNum)
          : code(cx), notes(cx), lastNoteOffset(0), currentLine(lineNum), lastColumn(0)
        {}
    };
    EmitSection prolog, main, *current;

    /* the parser */
    Parser<FullParseHandler>* const parser;

    HandleScript    evalCaller;     /* scripted caller info for eval and dbgapi */
    Handle<StaticEvalObject*> evalStaticScope;
                                   /* compile time scope for eval; does not imply stmt stack */

    StmtInfoBCE*    topStmt;       /* top of statement info stack */
    StmtInfoBCE*    topScopeStmt;  /* top lexical scope statement */
    Rooted<NestedScopeObject*> staticScope;
                                    /* compile time scope chain */

    OwnedAtomIndexMapPtr atomIndices; /* literals indexed for mapping */
    unsigned        firstLine;      /* first line, for JSScript::initFromEmitter */

    /*
     * Only unaliased locals have stack slots assigned to them. This vector is
     * used to map a local index (which includes unaliased and aliased locals)
     * to its stack slot index.
     */
    Vector<uint32_t, 16> localsToFrameSlots_;

    int32_t         stackDepth;     /* current stack depth in script frame */
    uint32_t        maxStackDepth;  /* maximum stack depth so far */

    uint32_t        arrayCompDepth; /* stack depth of array in comprehension */

    unsigned        emitLevel;      /* js::frontend::EmitTree recursion level */

    CGConstList     constList;      /* constants to be included with the script */

    CGObjectList    objectList;     /* list of emitted objects */
    CGObjectList    regexpList;     /* list of emitted regexp that will be
                                       cloned during execution */
    CGTryNoteList   tryNoteList;    /* list of emitted try notes */
    CGBlockScopeList blockScopeList;/* list of emitted block scope notes */

    /*
     * For each yield op, map the yield index (stored as bytecode operand) to
     * the offset of the next op.
     */
    CGYieldOffsetList yieldOffsetList;

    uint16_t        typesetCount;   /* Number of JOF_TYPESET opcodes generated */

    bool            hasSingletons:1;    /* script contains singleton initializer JSOP_OBJECT */

    bool            hasTryFinally:1;    /* script contains finally block */

    bool            emittingForInit:1;  /* true while emitting init expr of for; exclude 'in' */

    bool            emittingRunOnceLambda:1; /* true while emitting a lambda which is only
                                                expected to run once. */

    bool isRunOnceLambda();

    bool            insideEval:1;       /* True if compiling an eval-expression or a function
                                           nested inside an eval. */

    const bool      hasGlobalScope:1;   /* frontend::CompileScript's scope chain is the
                                           global object */

    enum EmitterMode {
        Normal,

        /*
         * Emit JSOP_GETINTRINSIC instead of JSOP_GETNAME and assert that
         * JSOP_GETNAME and JSOP_*GNAME don't ever get emitted. See the comment
         * for the field |selfHostingMode| in Parser.h for details.
         */
        SelfHosting,

        /*
         * Check the static scope chain of the root function for resolving free
         * variable accesses in the script.
         */
        LazyFunction
    };

    const EmitterMode emitterMode;

    /*
     * Note that BytecodeEmitters are magic: they own the arena "top-of-stack"
     * space above their tempMark points. This means that you cannot alloc from
     * tempLifoAlloc and save the pointer beyond the next BytecodeEmitter
     * destruction.
     */
    BytecodeEmitter(BytecodeEmitter* parent, Parser<FullParseHandler>* parser, SharedContext* sc,
                    HandleScript script, Handle<LazyScript*> lazyScript,
                    bool insideEval, HandleScript evalCaller,
                    Handle<StaticEvalObject*> evalStaticScope, bool hasGlobalScope,
                    uint32_t lineNum, EmitterMode emitterMode = Normal);
    bool init();
    bool updateLocalsToFrameSlots();

    bool isAliasedName(ParseNode* pn);

    MOZ_ALWAYS_INLINE
    bool makeAtomIndex(JSAtom* atom, jsatomid* indexp) {
        AtomIndexAddPtr p = atomIndices->lookupForAdd(atom);
        if (p) {
            *indexp = p.value();
            return true;
        }

        jsatomid index = atomIndices->count();
        if (!atomIndices->add(p, atom, index))
            return false;

        *indexp = index;
        return true;
    }

    bool isInLoop();
    bool checkSingletonContext();

    bool needsImplicitThis();

    void tellDebuggerAboutCompiledScript(ExclusiveContext* cx);

    inline TokenStream* tokenStream();

    BytecodeVector& code() const { return current->code; }
    jsbytecode* code(ptrdiff_t offset) const { return current->code.begin() + offset; }
    ptrdiff_t offset() const { return current->code.end() - current->code.begin(); }
    ptrdiff_t prologOffset() const { return prolog.code.end() - prolog.code.begin(); }
    void switchToMain() { current = &main; }
    void switchToProlog() { current = &prolog; }

    SrcNotesVector& notes() const { return current->notes; }
    ptrdiff_t lastNoteOffset() const { return current->lastNoteOffset; }
    unsigned currentLine() const { return current->currentLine; }
    unsigned lastColumn() const { return current->lastColumn; }

    bool reportError(ParseNode* pn, unsigned errorNumber, ...);
    bool reportStrictWarning(ParseNode* pn, unsigned errorNumber, ...);
    bool reportStrictModeError(ParseNode* pn, unsigned errorNumber, ...);
};

/*
 * Emit one bytecode.
 */
ptrdiff_t
Emit1(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op);

/*
 * Emit two bytecodes, an opcode (op) with a byte of immediate operand (op1).
 */
ptrdiff_t
Emit2(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, jsbytecode op1);

/*
 * Emit three bytecodes, an opcode with two bytes of immediate operands.
 */
ptrdiff_t
Emit3(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, jsbytecode op1, jsbytecode op2);

/*
 * Emit (1 + extra) bytecodes, for N bytes of op and its immediate operand.
 */
ptrdiff_t
EmitN(ExclusiveContext* cx, BytecodeEmitter* bce, JSOp op, size_t extra);

/*
 * Emit code into bce for the tree rooted at pn.
 */
bool
EmitTree(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* pn);

/*
 * Emit function code using bce for the tree rooted at body.
 */
bool
EmitFunctionScript(ExclusiveContext* cx, BytecodeEmitter* bce, ParseNode* body);

/*
 * Append a new source note of the given type (and therefore size) to bce's
 * notes dynamic array, updating bce->noteCount. Return the new note's index
 * within the array pointed at by bce->current->notes. Return -1 if out of
 * memory.
 */
int
NewSrcNote(ExclusiveContext* cx, BytecodeEmitter* bce, SrcNoteType type);

int
NewSrcNote2(ExclusiveContext* cx, BytecodeEmitter* bce, SrcNoteType type, ptrdiff_t offset);

int
NewSrcNote3(ExclusiveContext* cx, BytecodeEmitter* bce, SrcNoteType type, ptrdiff_t offset1,
               ptrdiff_t offset2);

/* NB: this function can add at most one extra extended delta note. */
bool
AddToSrcNoteDelta(ExclusiveContext* cx, BytecodeEmitter* bce, jssrcnote* sn, ptrdiff_t delta);

bool
FinishTakingSrcNotes(ExclusiveContext* cx, BytecodeEmitter* bce, uint32_t* out);

void
CopySrcNotes(BytecodeEmitter* bce, jssrcnote* destination, uint32_t nsrcnotes);

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_BytecodeEmitter_h */
