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
#include "frontend/Parser.h"
#include "frontend/SharedContext.h"
#include "frontend/SourceNotes.h"

namespace js {

class ScopeObject;

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

struct CGBlockScopeNote : public BlockScopeNote
{
    // The end offset. Used to compute the length; may need adjusting first if
    // in the prologue.
    uint32_t end;

    // Is the start offset in the prologue?
    bool startInPrologue;

    // Is the end offset in the prologue?
    bool endInPrologue;
};

struct CGBlockScopeList {
    Vector<CGBlockScopeNote> list;
    explicit CGBlockScopeList(ExclusiveContext* cx) : list(cx) {}

    bool append(uint32_t scopeObject, uint32_t offset, bool inPrologue, uint32_t parent);
    uint32_t findEnclosingScope(uint32_t index);
    void recordEnd(uint32_t index, uint32_t offset, bool inPrologue);
    size_t length() const { return list.length(); }
    void finish(BlockScopeArray* array, uint32_t prologueLength);
};

struct CGYieldOffsetList {
    Vector<uint32_t> list;
    explicit CGYieldOffsetList(ExclusiveContext* cx) : list(cx) {}

    bool append(uint32_t offset) { return list.append(offset); }
    size_t length() const { return list.length(); }
    void finish(YieldOffsetArray& array, uint32_t prologueLength);
};

struct LoopStmtInfo;
struct StmtInfoBCE;

// Use zero inline elements because these go on the stack and affect how many
// nested functions are possible.
typedef Vector<jsbytecode, 0> BytecodeVector;
typedef Vector<jssrcnote, 0> SrcNotesVector;

// This enum tells BytecodeEmitter::emitVariables and the destructuring
// methods how emit the given Parser::variables parse tree.
enum VarEmitOption {
    // The normal case. Emit code to evaluate initializer expressions and
    // assign them to local variables. Also emit JSOP_DEF{VAR,LET,CONST}
    // opcodes in the prologue if the declaration occurs at toplevel.
    InitializeVars,

    // Emit only JSOP_DEFVAR opcodes, in the prologue, if necessary. This is
    // used in one case: `for (var $BindingPattern in/of obj)`. If we're at
    // toplevel, the variable(s) must be defined with JSOP_DEFVAR, but they're
    // populated inside the loop, via emitAssignment.
    DefineVars,

    // Emit code to evaluate initializer expressions and leave those values on
    // the stack. This is used to implement `for (let/const ...;;)` and
    // deprecated `let` blocks.
    PushInitialValues
};

struct BytecodeEmitter
{
    SharedContext* const sc;      /* context shared between parsing and bytecode generation */

    ExclusiveContext* const cx;

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
    EmitSection prologue, main, *current;

    /* the parser */
    Parser<FullParseHandler>* const parser;

    HandleScript    evalCaller;     /* scripted caller info for eval and dbgapi */

    StmtInfoStack<StmtInfoBCE> stmtStack;

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

    unsigned        emitLevel;      /* emitTree recursion level */

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

    const bool      insideNonGlobalEval:1;  /* True if this is a direct eval
                                               call in some non-global scope. */

    bool            insideModule:1;     /* True if compiling inside a module. */

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
                    bool insideNonGlobalEval, uint32_t lineNum, EmitterMode emitterMode = Normal);
    bool init();
    bool updateLocalsToFrameSlots();

    StmtInfoBCE* innermostStmt() const { return stmtStack.innermost(); }
    StmtInfoBCE* innermostScopeStmt() const { return stmtStack.innermostScopeStmt(); }
    JSObject* innermostStaticScope() const;
    JSObject* blockScopeOfDef(Definition* dn) const {
        return parser->blockScopes[dn->pn_blockid];
    }

    bool atBodyLevel() const;
    uint32_t computeHops(ParseNode* pn, BytecodeEmitter** bceOfDefOut);
    bool isAliasedName(BytecodeEmitter* bceOfDef, ParseNode* pn);
    bool computeDefinitionIsAliased(BytecodeEmitter* bceOfDef, Definition* dn, JSOp* op);

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

    // Check whether our function is in a run-once context (a toplevel
    // run-one script or a run-once lambda).
    bool checkRunOnceContext();

    bool needsImplicitThis();

    void tellDebuggerAboutCompiledScript(ExclusiveContext* cx);

    inline TokenStream* tokenStream();

    BytecodeVector& code() const { return current->code; }
    jsbytecode* code(ptrdiff_t offset) const { return current->code.begin() + offset; }
    ptrdiff_t offset() const { return current->code.end() - current->code.begin(); }
    ptrdiff_t prologueOffset() const { return prologue.code.end() - prologue.code.begin(); }
    void switchToMain() { current = &main; }
    void switchToPrologue() { current = &prologue; }
    bool inPrologue() const { return current == &prologue; }

    SrcNotesVector& notes() const { return current->notes; }
    ptrdiff_t lastNoteOffset() const { return current->lastNoteOffset; }
    unsigned currentLine() const { return current->currentLine; }
    unsigned lastColumn() const { return current->lastColumn; }

    bool reportError(ParseNode* pn, unsigned errorNumber, ...);
    bool reportStrictWarning(ParseNode* pn, unsigned errorNumber, ...);
    bool reportStrictModeError(ParseNode* pn, unsigned errorNumber, ...);

    // If pn contains a useful expression, return true with *answer set to true.
    // If pn contains a useless expression, return true with *answer set to
    // false. Return false on error.
    //
    // The caller should initialize *answer to false and invoke this function on
    // an expression statement or similar subtree to decide whether the tree
    // could produce code that has any side effects.  For an expression
    // statement, we define useless code as code with no side effects, because
    // the main effect, the value left on the stack after the code executes,
    // will be discarded by a pop bytecode.
    bool checkSideEffects(ParseNode* pn, bool* answer);

#ifdef DEBUG
    bool checkStrictOrSloppy(JSOp op);
#endif

    // Append a new source note of the given type (and therefore size) to the
    // notes dynamic array, updating noteCount. Return the new note's index
    // within the array pointed at by current->notes as outparam.
    bool newSrcNote(SrcNoteType type, unsigned* indexp = nullptr);
    bool newSrcNote2(SrcNoteType type, ptrdiff_t offset, unsigned* indexp = nullptr);
    bool newSrcNote3(SrcNoteType type, ptrdiff_t offset1, ptrdiff_t offset2,
                     unsigned* indexp = nullptr);

    void copySrcNotes(jssrcnote* destination, uint32_t nsrcnotes);
    bool setSrcNoteOffset(unsigned index, unsigned which, ptrdiff_t offset);

    // NB: this function can add at most one extra extended delta note.
    bool addToSrcNoteDelta(jssrcnote* sn, ptrdiff_t delta);

    // Finish taking source notes in cx's notePool. If successful, the final
    // source note count is stored in the out outparam.
    bool finishTakingSrcNotes(uint32_t* out);

    void setJumpOffsetAt(ptrdiff_t off);

    // Control whether emitTree emits a line number note.
    enum EmitLineNumberNote {
        EMIT_LINENOTE,
        SUPPRESS_LINENOTE
    };

    // Emit code for the tree rooted at pn.
    bool emitTree(ParseNode* pn, EmitLineNumberNote emitLineNote = EMIT_LINENOTE);

    // Emit function code for the tree rooted at body.
    bool emitFunctionScript(ParseNode* body);

    // Emit module code for the tree rooted at body.
    bool emitModuleScript(ParseNode* body);

    // If op is JOF_TYPESET (see the type barriers comment in TypeInference.h),
    // reserve a type set to store its result.
    void checkTypeSet(JSOp op);

    void updateDepth(ptrdiff_t target);
    bool updateLineNumberNotes(uint32_t offset);
    bool updateSourceCoordNotes(uint32_t offset);

    bool bindNameToSlot(ParseNode* pn);
    bool bindNameToSlotHelper(ParseNode* pn);

    void strictifySetNameNode(ParseNode* pn);
    JSOp strictifySetNameOp(JSOp op);

    bool tryConvertFreeName(ParseNode* pn);

    void popStatement();
    void pushStatement(StmtInfoBCE* stmt, StmtType type, ptrdiff_t top);
    void pushStatementInner(StmtInfoBCE* stmt, StmtType type, ptrdiff_t top);
    void pushLoopStatement(LoopStmtInfo* stmt, StmtType type, ptrdiff_t top);

    bool enterNestedScope(StmtInfoBCE* stmt, ObjectBox* objbox, StmtType stmtType);
    bool leaveNestedScope(StmtInfoBCE* stmt);

    bool enterBlockScope(StmtInfoBCE* stmtInfo, ObjectBox* objbox, JSOp initialValueOp,
                         unsigned alreadyPushed = 0);

    bool computeAliasedSlots(Handle<StaticBlockObject*> blockObj);

    bool lookupAliasedName(HandleScript script, PropertyName* name, uint32_t* pslot,
                           ParseNode* pn = nullptr);
    bool lookupAliasedNameSlot(PropertyName* name, ScopeCoordinate* sc);

    // In a function, block-scoped locals go after the vars, and form part of the
    // fixed part of a stack frame.  Outside a function, there are no fixed vars,
    // but block-scoped locals still form part of the fixed part of a stack frame
    // and are thus addressable via GETLOCAL and friends.
    void computeLocalOffset(Handle<StaticBlockObject*> blockObj);

    bool flushPops(int* npops);

    bool emitCheck(ptrdiff_t delta, ptrdiff_t* offset);

    // Emit one bytecode.
    bool emit1(JSOp op);

    // Emit two bytecodes, an opcode (op) with a byte of immediate operand
    // (op1).
    bool emit2(JSOp op, uint8_t op1);

    // Emit three bytecodes, an opcode with two bytes of immediate operands.
    bool emit3(JSOp op, jsbytecode op1, jsbytecode op2);

    // Helper to emit JSOP_DUPAT. The argument is the value's depth on the
    // JS stack, as measured from the top.
    bool emitDupAt(unsigned slotFromTop);

    // Emit a bytecode followed by an uint16 immediate operand stored in
    // big-endian order.
    bool emitUint16Operand(JSOp op, uint32_t operand);

    // Emit a bytecode followed by an uint32 immediate operand.
    bool emitUint32Operand(JSOp op, uint32_t operand);

    // Emit (1 + extra) bytecodes, for N bytes of op and its immediate operand.
    bool emitN(JSOp op, size_t extra, ptrdiff_t* offset = nullptr);

    bool emitNumberOp(double dval);

    bool emitThisLiteral(ParseNode* pn);
    bool emitCreateFunctionThis();
    bool emitGetFunctionThis(ParseNode* pn);
    bool emitGetThisForSuperBase(ParseNode* pn);
    bool emitSetThis(ParseNode* pn);

    // These functions are used to emit GETLOCAL/GETALIASEDVAR or
    // SETLOCAL/SETALIASEDVAR for a particular binding. The CallObject must be
    // on top of the scope chain.
    bool emitLoadFromTopScope(BindingIter& bi);
    bool emitStoreToTopScope(BindingIter& bi);

    bool emitJump(JSOp op, ptrdiff_t off, ptrdiff_t* jumpOffset = nullptr);
    bool emitCall(JSOp op, uint16_t argc, ParseNode* pn = nullptr);

    bool emitLoopHead(ParseNode* nextpn);
    bool emitLoopEntry(ParseNode* nextpn);

    // Emit a backpatch op with offset pointing to the previous jump of this
    // type, so that we can walk back up the chain fixing up the op and jump
    // offset.
    bool emitBackPatchOp(ptrdiff_t* lastp);
    void backPatch(ptrdiff_t last, jsbytecode* target, jsbytecode op);

    bool emitGoto(StmtInfoBCE* toStmt, ptrdiff_t* lastp, SrcNoteType noteType = SRC_NULL);

    bool emitIndex32(JSOp op, uint32_t index);
    bool emitIndexOp(JSOp op, uint32_t index);

    bool emitAtomOp(JSAtom* atom, JSOp op);
    bool emitAtomOp(ParseNode* pn, JSOp op);

    bool emitArrayLiteral(ParseNode* pn);
    bool emitArray(ParseNode* pn, uint32_t count, JSOp op);
    bool emitArrayComp(ParseNode* pn);

    bool emitInternedObjectOp(uint32_t index, JSOp op);
    bool emitObjectOp(ObjectBox* objbox, JSOp op);
    bool emitObjectPairOp(ObjectBox* objbox1, ObjectBox* objbox2, JSOp op);
    bool emitRegExp(uint32_t index);

    MOZ_NEVER_INLINE bool emitFunction(ParseNode* pn, bool needsProto = false);
    MOZ_NEVER_INLINE bool emitObject(ParseNode* pn);

    bool emitPropertyList(ParseNode* pn, MutableHandlePlainObject objp, PropListType type);

    // To catch accidental misuse, emitUint16Operand/emit3 assert that they are
    // not used to unconditionally emit JSOP_GETLOCAL. Variable access should
    // instead be emitted using EmitVarOp. In special cases, when the caller
    // definitely knows that a given local slot is unaliased, this function may be
    // used as a non-asserting version of emitUint16Operand.
    bool emitLocalOp(JSOp op, uint32_t slot);

    bool emitScopeCoordOp(JSOp op, ScopeCoordinate sc);
    bool emitAliasedVarOp(JSOp op, ParseNode* pn);
    bool emitAliasedVarOp(JSOp op, ScopeCoordinate sc, MaybeCheckLexical checkLexical);
    bool emitUnaliasedVarOp(JSOp op, uint32_t slot, MaybeCheckLexical checkLexical);

    bool emitVarOp(ParseNode* pn, JSOp op);
    bool emitVarIncDec(ParseNode* pn);

    bool emitNameOp(ParseNode* pn, bool callContext);
    bool emitNameIncDec(ParseNode* pn);

    bool maybeEmitVarDecl(JSOp prologueOp, ParseNode* pn, jsatomid* result);
    bool emitVariables(ParseNode* pn, VarEmitOption emitOption);
    bool emitSingleVariable(ParseNode* pn, ParseNode* binding, ParseNode* initializer,
                            VarEmitOption emitOption);

    bool emitNewInit(JSProtoKey key);
    bool emitSingletonInitialiser(ParseNode* pn);

    bool emitPrepareIteratorResult();
    bool emitFinishIteratorResult(bool done);
    bool iteratorResultShape(unsigned* shape);

    bool emitYield(ParseNode* pn);
    bool emitYieldOp(JSOp op);
    bool emitYieldStar(ParseNode* iter, ParseNode* gen);

    bool emitPropLHS(ParseNode* pn);
    bool emitPropOp(ParseNode* pn, JSOp op);
    bool emitPropIncDec(ParseNode* pn);

    bool emitComputedPropertyName(ParseNode* computedPropName);

    // Emit bytecode to put operands for a JSOP_GETELEM/CALLELEM/SETELEM/DELELEM
    // opcode onto the stack in the right order. In the case of SETELEM, the
    // value to be assigned must already be pushed.
    enum class EmitElemOption { Get, Set, Call, IncDec };
    bool emitElemOperands(ParseNode* pn, EmitElemOption opts);

    bool emitElemOpBase(JSOp op);
    bool emitElemOp(ParseNode* pn, JSOp op);
    bool emitElemIncDec(ParseNode* pn);

    bool emitCatch(ParseNode* pn);
    bool emitIf(ParseNode* pn);
    bool emitWith(ParseNode* pn);

    MOZ_NEVER_INLINE bool emitLabeledStatement(const LabeledStatement* pn);
    MOZ_NEVER_INLINE bool emitLetBlock(ParseNode* pnLet);
    MOZ_NEVER_INLINE bool emitLexicalScope(ParseNode* pn);
    MOZ_NEVER_INLINE bool emitSwitch(ParseNode* pn);
    MOZ_NEVER_INLINE bool emitTry(ParseNode* pn);

    // EmitDestructuringLHS assumes the to-be-destructured value has been pushed on
    // the stack and emits code to destructure a single lhs expression (either a
    // name or a compound []/{} expression).
    //
    // If emitOption is InitializeVars, the to-be-destructured value is assigned to
    // locals and ultimately the initial slot is popped (-1 total depth change).
    //
    // If emitOption is PushInitialValues, the to-be-destructured value is replaced
    // with the initial values of the N (where 0 <= N) variables assigned in the
    // lhs expression. (Same post-condition as EmitDestructuringOpsHelper)
    bool emitDestructuringLHS(ParseNode* target, VarEmitOption emitOption);

    bool emitDestructuringOps(ParseNode* pattern, bool isLet = false);
    bool emitDestructuringOpsHelper(ParseNode* pattern, VarEmitOption emitOption);
    bool emitDestructuringOpsArrayHelper(ParseNode* pattern, VarEmitOption emitOption);
    bool emitDestructuringOpsObjectHelper(ParseNode* pattern, VarEmitOption emitOption);

    typedef bool
    (*DestructuringDeclEmitter)(BytecodeEmitter* bce, JSOp prologueOp, ParseNode* pn);

    template <DestructuringDeclEmitter EmitName>
    bool emitDestructuringDeclsWithEmitter(JSOp prologueOp, ParseNode* pattern);

    bool emitDestructuringDecls(JSOp prologueOp, ParseNode* pattern);

    // Emit code to initialize all destructured names to the value on the top of
    // the stack.
    bool emitInitializeDestructuringDecls(JSOp prologueOp, ParseNode* pattern);

    // Throw a TypeError if the value atop the stack isn't convertible to an
    // object, with no overall effect on the stack.
    bool emitRequireObjectCoercible();

    // emitIterator expects the iterable to already be on the stack.
    // It will replace that stack value with the corresponding iterator
    bool emitIterator();

    // Pops iterator from the top of the stack. Pushes the result of |.next()|
    // onto the stack.
    bool emitIteratorNext(ParseNode* pn);

    // Check if the value on top of the stack is "undefined". If so, replace
    // that value on the stack with the value defined by |defaultExpr|.
    bool emitDefault(ParseNode* defaultExpr);

    bool emitCallSiteObject(ParseNode* pn);
    bool emitTemplateString(ParseNode* pn);
    bool emitAssignment(ParseNode* lhs, JSOp op, ParseNode* rhs);

    bool emitReturn(ParseNode* pn);
    bool emitStatement(ParseNode* pn);
    bool emitStatementList(ParseNode* pn);

    bool emitDeleteName(ParseNode* pn);
    bool emitDeleteProperty(ParseNode* pn);
    bool emitDeleteElement(ParseNode* pn);
    bool emitDeleteExpression(ParseNode* pn);

    // |op| must be JSOP_TYPEOF or JSOP_TYPEOFEXPR.
    bool emitTypeof(ParseNode* node, JSOp op);

    bool emitUnary(ParseNode* pn);
    bool emitRightAssociative(ParseNode* pn);
    bool emitLeftAssociative(ParseNode* pn);
    bool emitLogical(ParseNode* pn);
    bool emitSequenceExpr(ParseNode* pn);

    MOZ_NEVER_INLINE bool emitIncOrDec(ParseNode* pn);

    bool emitConditionalExpression(ConditionalExpression& conditional);

    bool emitCallOrNew(ParseNode* pn);
    bool emitSelfHostedCallFunction(ParseNode* pn);
    bool emitSelfHostedResumeGenerator(ParseNode* pn);
    bool emitSelfHostedForceInterpreter(ParseNode* pn);

    bool emitComprehensionFor(ParseNode* compFor);
    bool emitComprehensionForIn(ParseNode* pn);
    bool emitComprehensionForInOrOfVariables(ParseNode* pn, bool* letBlockScope);
    bool emitComprehensionForOf(ParseNode* pn);

    bool emitDo(ParseNode* pn);
    bool emitFor(ParseNode* pn);
    bool emitForIn(ParseNode* pn);
    bool emitForInOrOfVariables(ParseNode* pn);
    bool emitCStyleFor(ParseNode* pn);
    bool emitWhile(ParseNode* pn);

    bool emitBreak(PropertyName* label);
    bool emitContinue(PropertyName* label);

    bool emitArgsBody(ParseNode* pn);
    bool emitDefaultsAndDestructuring(ParseNode* pn);
    bool emitLexicalInitialization(ParseNode* pn, JSOp globalDefOp);

    bool pushInitialConstants(JSOp op, unsigned n);
    bool initializeBlockScopedLocalsFromStack(Handle<StaticBlockObject*> blockObj);

    // emitSpread expects the current index (I) of the array, the array itself
    // and the iterator to be on the stack in that order (iterator on the bottom).
    // It will pop the iterator and I, then iterate over the iterator by calling
    // |.next()| and put the results into the I-th element of array with
    // incrementing I, then push the result I (it will be original I +
    // iteration count). The stack after iteration will look like |ARRAY INDEX|.
    bool emitSpread();

    // If type is StmtType::FOR_OF_LOOP, emit bytecode for a for-of loop.
    // pn should be PNK_FOR, and pn->pn_left should be PNK_FOROF.
    //
    // If type is StmtType::SPREAD, emit bytecode for spread operator.
    // pn should be nullptr.
    //
    // Please refer the comment above emitSpread for additional information about
    // stack convention.
    bool emitForOf(StmtType type, ParseNode* pn);

    bool emitClass(ParseNode* pn);
    bool emitSuperPropLHS(ParseNode* superBase, bool isCall = false);
    bool emitSuperPropOp(ParseNode* pn, JSOp op, bool isCall = false);
    bool emitSuperElemOperands(ParseNode* pn, EmitElemOption opts = EmitElemOption::Get);
    bool emitSuperElemOp(ParseNode* pn, JSOp op, bool isCall = false);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_BytecodeEmitter_h */
