/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_Parser_h
#define frontend_Parser_h

/*
 * JS parser definitions.
 */

#include "jspubtd.h"

#include "frontend/BytecodeCompiler.h"
#include "frontend/FullParseHandler.h"
#include "frontend/ParseMaps.h"
#include "frontend/ParseNode.h"
#include "frontend/SharedContext.h"
#include "frontend/SyntaxParseHandler.h"

namespace js {
namespace frontend {

struct StmtInfoPC : public StmtInfoBase {
    StmtInfoPC*     down;          /* info for enclosing statement */
    StmtInfoPC*     downScope;     /* next enclosing lexical scope */

    uint32_t        blockid;        /* for simplified dominance computation */
    uint32_t        innerBlockScopeDepth; /* maximum depth of nested block scopes, in slots */

    // Lexical declarations inside switches are tricky because the block id
    // doesn't convey dominance information. Record what index the current
    // case's lexical declarations start at so we may generate dead zone
    // checks for other cases' declarations.
    //
    // Only valid if type is STMT_SWITCH.
    uint16_t        firstDominatingLexicalInCase;

    explicit StmtInfoPC(ExclusiveContext* cx)
      : StmtInfoBase(cx),
        innerBlockScopeDepth(0),
        firstDominatingLexicalInCase(0)
    {}
};

typedef HashSet<JSAtom*, DefaultHasher<JSAtom*>, LifoAllocPolicy<Fallible>> FuncStmtSet;
class SharedContext;

typedef Vector<Definition*, 16> DeclVector;

struct GenericParseContext
{
    // Enclosing function or global context.
    GenericParseContext* parent;

    // Context shared between parsing and bytecode generation.
    SharedContext* sc;

    // The following flags are set when a particular code feature is detected
    // in a function.

    // Function has 'return <expr>;'
    bool funHasReturnExpr:1;

    // Function has 'return;'
    bool funHasReturnVoid:1;

    // The following flags are set when parsing enters a particular region of
    // source code, and cleared when that region is exited.

    // true while parsing init expr of for; exclude 'in'
    bool parsingForInit:1;

    // true while we are within a with-statement in the current ParseContext
    // chain (which stops at the top-level or an eval()
    bool parsingWith:1;

    GenericParseContext(GenericParseContext* parent, SharedContext* sc)
      : parent(parent),
        sc(sc),
        funHasReturnExpr(false),
        funHasReturnVoid(false),
        parsingForInit(false),
        parsingWith(parent ? parent->parsingWith : false)
    {}
};

template <typename ParseHandler>
bool
GenerateBlockId(TokenStream& ts, ParseContext<ParseHandler>* pc, uint32_t& blockid);

/*
 * The struct ParseContext stores information about the current parsing context,
 * which is part of the parser state (see the field Parser::pc). The current
 * parsing context is either the global context, or the function currently being
 * parsed. When the parser encounters a function definition, it creates a new
 * ParseContext, makes it the new current context, and sets its parent to the
 * context in which it encountered the definition.
 */
template <typename ParseHandler>
struct ParseContext : public GenericParseContext
{
    typedef StmtInfoPC StmtInfo;
    typedef typename ParseHandler::Node Node;
    typedef typename ParseHandler::DefinitionNode DefinitionNode;

    uint32_t        bodyid;         /* block number of program/function body */
    uint32_t        blockidGen;     /* preincremented block number generator */

    StmtInfoPC*     topStmt;       /* top of statement info stack */
    StmtInfoPC*     topScopeStmt;  /* top lexical scope statement */
    Rooted<NestedScopeObject*> staticScope;  /* compile time scope chain */
    Node            maybeFunction;  /* sc->isFunctionBox, the pn where pn->pn_funbox == sc */

    const unsigned  staticLevel;    /* static compilation unit nesting level */

    // lastYieldOffset stores the offset of the last yield that was parsed.
    // NoYieldOffset is its initial value.
    static const uint32_t NoYieldOffset = UINT32_MAX;
    uint32_t         lastYieldOffset;

    // Most functions start off being parsed as non-generators.
    // Non-generators transition to LegacyGenerator on parsing "yield" in JS 1.7.
    // An ES6 generator is marked as a "star generator" before its body is parsed.
    GeneratorKind generatorKind() const {
        return sc->isFunctionBox() ? sc->asFunctionBox()->generatorKind() : NotGenerator;
    }
    bool isGenerator() const { return generatorKind() != NotGenerator; }
    bool isLegacyGenerator() const { return generatorKind() == LegacyGenerator; }
    bool isStarGenerator() const { return generatorKind() == StarGenerator; }

    bool isArrowFunction() const {
        return sc->isFunctionBox() && sc->asFunctionBox()->function()->isArrow();
    }

    uint32_t        blockScopeDepth; /* maximum depth of nested block scopes, in slots */
    Node            blockNode;      /* parse node for a block with let declarations
                                       (block with its own lexical scope)  */

  private:
    AtomDecls<ParseHandler> decls_;     /* function, const, and var declarations */
    DeclVector      args_;              /* argument definitions */
    DeclVector      vars_;              /* var/const definitions */
    DeclVector      bodyLevelLexicals_; /* lexical definitions at body-level */

    bool checkLocalsOverflow(TokenStream& ts);

  public:
    const AtomDecls<ParseHandler>& decls() const {
        return decls_;
    }

    uint32_t numArgs() const {
        MOZ_ASSERT(sc->isFunctionBox());
        return args_.length();
    }

    /*
     * This function adds a definition to the lexical scope represented by this
     * ParseContext.
     *
     * Pre-conditions:
     *  + The caller must have already taken care of name collisions:
     *    - For non-let definitions, this means 'name' isn't in 'decls'.
     *    - For let definitions, this means 'name' isn't already a name in the
     *      current block.
     *  + The given 'pn' is either a placeholder (created by a previous unbound
     *    use) or an un-bound un-linked name node.
     *  + The given 'kind' is one of ARG, CONST, VAR, or LET. In particular,
     *    NAMED_LAMBDA is handled in an ad hoc special case manner (see
     *    LeaveFunction) that we should consider rewriting.
     *
     * Post-conditions:
     *  + pc->decls().lookupFirst(name) == pn
     *  + The given name 'pn' has been converted in-place into a
     *    non-placeholder definition.
     *  + If this is a function scope (sc->inFunction), 'pn' is bound to a
     *    particular local/argument slot.
     *  + PND_CONST is set for Definition::COSNT
     *  + Pre-existing uses of pre-existing placeholders have been linked to
     *    'pn' if they are in the scope of 'pn'.
     *  + Pre-existing placeholders in the scope of 'pn' have been removed.
     */
    bool define(TokenStream& ts, HandlePropertyName name, Node pn, Definition::Kind);

    /*
     * Let definitions may shadow same-named definitions in enclosing scopes.
     * To represesent this, 'decls' is not a plain map, but actually:
     *   decls :: name -> stack of definitions
     * New bindings are pushed onto the stack, name lookup always refers to the
     * top of the stack, and leaving a block scope calls popLetDecl for each
     * name in the block's scope.
     */
    void popLetDecl(JSAtom* atom);

    /* See the sad story in defineArg. */
    void prepareToAddDuplicateArg(HandlePropertyName name, DefinitionNode prevDecl);

    /* See the sad story in MakeDefIntoUse. */
    void updateDecl(JSAtom* atom, Node newDecl);

    /*
     * After a function body has been parsed, the parser generates the
     * function's "bindings". Bindings are a data-structure, ultimately stored
     * in the compiled JSScript, that serve three purposes:
     *  - After parsing, the ParseContext is destroyed and 'decls' along with
     *    it. Mostly, the emitter just uses the binding information stored in
     *    the use/def nodes, but the emitter occasionally needs 'bindings' for
     *    various scope-related queries.
     *  - Bindings provide the initial js::Shape to use when creating a dynamic
     *    scope object (js::CallObject) for the function. This shape is used
     *    during dynamic name lookup.
     *  - Sometimes a script's bindings are accessed at runtime to retrieve the
     *    contents of the lexical scope (e.g., from the debugger).
     */
    bool generateFunctionBindings(ExclusiveContext* cx, TokenStream& ts,
                                  LifoAlloc& alloc,
                                  InternalHandle<Bindings*> bindings) const;

  private:
    ParseContext**  parserPC;     /* this points to the Parser's active pc
                                       and holds either |this| or one of
                                       |this|'s descendents */

    // Value for parserPC to restore at the end. Use 'parent' instead for
    // information about the parse chain, this may be nullptr if
    // parent != nullptr.
    ParseContext<ParseHandler>* oldpc;

  public:
    OwnedAtomDefnMapPtr lexdeps;    /* unresolved lexical name dependencies */

    FuncStmtSet*    funcStmts;     /* Set of (non-top-level) function statements
                                       that will alias any top-level bindings with
                                       the same name. */

    // All inner functions in this context. Only filled in when parsing syntax.
    AutoFunctionVector innerFunctions;

    // In a function context, points to a Directive struct that can be updated
    // to reflect new directives encountered in the Directive Prologue that
    // require reparsing the function. In global/module/generator-tail contexts,
    // we don't need to reparse when encountering a DirectivePrologue so this
    // pointer may be nullptr.
    Directives* newDirectives;

    // Set when parsing a declaration-like destructuring pattern.  This flag
    // causes PrimaryExpr to create PN_NAME parse nodes for variable references
    // which are not hooked into any definition's use chain, added to any tree
    // context's AtomList, etc. etc.  checkDestructuring will do that work
    // later.
    //
    // The comments atop checkDestructuring explain the distinction between
    // assignment-like and declaration-like destructuring patterns, and why
    // they need to be treated differently.
    bool            inDeclDestructuring:1;

    ParseContext(Parser<ParseHandler>* prs, GenericParseContext* parent,
                 Node maybeFunction, SharedContext* sc,
                 Directives* newDirectives,
                 unsigned staticLevel, uint32_t bodyid, uint32_t blockScopeDepth)
      : GenericParseContext(parent, sc),
        bodyid(0),           // initialized in init()
        blockidGen(bodyid),  // used to set |bodyid| and subsequently incremented in init()
        topStmt(nullptr),
        topScopeStmt(nullptr),
        staticScope(prs->context),
        maybeFunction(maybeFunction),
        staticLevel(staticLevel),
        lastYieldOffset(NoYieldOffset),
        blockScopeDepth(blockScopeDepth),
        blockNode(ParseHandler::null()),
        decls_(prs->context, prs->alloc),
        args_(prs->context),
        vars_(prs->context),
        bodyLevelLexicals_(prs->context),
        parserPC(&prs->pc),
        oldpc(prs->pc),
        lexdeps(prs->context),
        funcStmts(nullptr),
        innerFunctions(prs->context),
        newDirectives(newDirectives),
        inDeclDestructuring(false)
    {
        prs->pc = this;
    }

    ~ParseContext();

    bool init(TokenStream& ts);

    unsigned blockid() { return topStmt ? topStmt->blockid : bodyid; }

    // True if we are at the topmost level of a entire script or function body.
    // For example, while parsing this code we would encounter f1 and f2 at
    // body level, but we would not encounter f3 or f4 at body level:
    //
    //   function f1() { function f2() { } }
    //   if (cond) { function f3() { if (cond) { function f4() { } } } }
    //
    bool atBodyLevel() { return !topStmt; }

    // True if this is the ParseContext for the body of a function created by
    // the Function constructor.
    bool isFunctionConstructorBody() const {
        return sc->isFunctionBox() && staticLevel == 0;
    }

    inline bool useAsmOrInsideUseAsm() const {
        return sc->isFunctionBox() && sc->asFunctionBox()->useAsmOrInsideUseAsm();
    }
};

template <typename ParseHandler>
inline
Directives::Directives(ParseContext<ParseHandler>* parent)
  : strict_(parent->sc->strict),
    asmJS_(parent->useAsmOrInsideUseAsm())
{}

template <typename ParseHandler>
struct BindData;

class CompExprTransplanter;

enum LetContext { LetExpression, LetStatement };
enum VarContext { HoistVars, DontHoistVars };
enum FunctionType { Getter, Setter, Normal };

template <typename ParseHandler>
class Parser : private JS::AutoGCRooter, public StrictModeGetter
{
  public:
    ExclusiveContext* const context;
    LifoAlloc& alloc;

    TokenStream         tokenStream;
    LifoAlloc::Mark     tempPoolMark;

    /* list of parsed objects for GC tracing */
    ObjectBox* traceListHead;

    /* innermost parse context (stack-allocated) */
    ParseContext<ParseHandler>* pc;

    /* Compression token for aborting. */
    SourceCompressionTask* sct;

    ScriptSource*       ss;

    /* Root atoms and objects allocated for the parsed tree. */
    AutoKeepAtoms       keepAtoms;

    /* Perform constant-folding; must be true when interfacing with the emitter. */
    const bool          foldConstants:1;

  private:
#if DEBUG
    /* Our fallible 'checkOptions' member function has been called. */
    bool checkOptionsCalled:1;
#endif

    /*
     * Not all language constructs can be handled during syntax parsing. If it
     * is not known whether the parse succeeds or fails, this bit is set and
     * the parse will return false.
     */
    bool abortedSyntaxParse:1;

    /* Unexpected end of input, i.e. TOK_EOF not at top-level. */
    bool isUnexpectedEOF_:1;

    typedef typename ParseHandler::Node Node;
    typedef typename ParseHandler::DefinitionNode DefinitionNode;

  public:
    /* State specific to the kind of parse being performed. */
    ParseHandler handler;

  private:
    bool reportHelper(ParseReportKind kind, bool strict, uint32_t offset,
                      unsigned errorNumber, va_list args);
  public:
    bool report(ParseReportKind kind, bool strict, Node pn, unsigned errorNumber, ...);
    bool reportNoOffset(ParseReportKind kind, bool strict, unsigned errorNumber, ...);
    bool reportWithOffset(ParseReportKind kind, bool strict, uint32_t offset, unsigned errorNumber,
                          ...);

    Parser(ExclusiveContext* cx, LifoAlloc* alloc, const ReadOnlyCompileOptions& options,
           const char16_t* chars, size_t length, bool foldConstants,
           Parser<SyntaxParseHandler>* syntaxParser,
           LazyScript* lazyOuterFunction);
    ~Parser();

    bool checkOptions();

    // A Parser::Mark is the extension of the LifoAlloc::Mark to the entire
    // Parser's state. Note: clients must still take care that any ParseContext
    // that points into released ParseNodes is destroyed.
    class Mark
    {
        friend class Parser;
        LifoAlloc::Mark mark;
        ObjectBox* traceListHead;
    };
    Mark mark() const {
        Mark m;
        m.mark = alloc.mark();
        m.traceListHead = traceListHead;
        return m;
    }
    void release(Mark m) {
        alloc.release(m.mark);
        traceListHead = m.traceListHead;
    }

    friend void js::frontend::MarkParser(JSTracer* trc, JS::AutoGCRooter* parser);

    const char* getFilename() const { return tokenStream.getFilename(); }
    JSVersion versionNumber() const { return tokenStream.versionNumber(); }

    /*
     * Parse a top-level JS script.
     */
    Node parse(JSObject* chain);

    /*
     * Allocate a new parsed object or function container from
     * cx->tempLifoAlloc.
     */
    ObjectBox* newObjectBox(NativeObject* obj);
    FunctionBox* newFunctionBox(Node fn, JSFunction* fun, ParseContext<ParseHandler>* pc,
                                Directives directives, GeneratorKind generatorKind);

    /*
     * Create a new function object given a name (which is optional if this is
     * a function expression).
     */
    JSFunction* newFunction(HandleAtom atom, FunctionSyntaxKind kind, HandleObject proto);

    void trace(JSTracer* trc);

    bool hadAbortedSyntaxParse() {
        return abortedSyntaxParse;
    }
    void clearAbortedSyntaxParse() {
        abortedSyntaxParse = false;
    }

    bool isUnexpectedEOF() const { return isUnexpectedEOF_; }

  private:
    Parser* thisForCtor() { return this; }

    JSAtom * stopStringCompression();

    Node stringLiteral();
    Node noSubstitutionTemplate();
    Node templateLiteral();
    bool taggedTemplate(Node nodeList, TokenKind tt);
    bool appendToCallSiteObj(Node callSiteObj);
    bool addExprAndGetNextTemplStrToken(Node nodeList, TokenKind* ttp);

    inline Node newName(PropertyName* name);
    inline Node newYieldExpression(uint32_t begin, Node expr, bool isYieldStar = false);

    inline bool abortIfSyntaxParser();

  public:

    /* Public entry points for parsing. */
    Node statement(bool canHaveDirectives = false);
    bool maybeParseDirective(Node list, Node pn, bool* cont);

    // Parse a function, given only its body. Used for the Function and
    // Generator constructors.
    Node standaloneFunctionBody(HandleFunction fun, const AutoNameVector& formals,
                                GeneratorKind generatorKind,
                                Directives inheritedDirectives, Directives* newDirectives);

    // Parse a function, given only its arguments and body. Used for lazily
    // parsed functions.
    Node standaloneLazyFunction(HandleFunction fun, unsigned staticLevel, bool strict,
                                GeneratorKind generatorKind);

    /*
     * Parse a function body.  Pass StatementListBody if the body is a list of
     * statements; pass ExpressionBody if the body is a single expression.
     */
    enum FunctionBodyType { StatementListBody, ExpressionBody };
    Node functionBody(FunctionSyntaxKind kind, FunctionBodyType type);

    bool functionArgsAndBodyGeneric(Node pn, HandleFunction fun, FunctionType type,
                                    FunctionSyntaxKind kind);

    // Determine whether |yield| is a valid name in the current context, or
    // whether it's prohibited due to strictness, JS version, or occurrence
    // inside a star generator.
    bool checkYieldNameValidity();
    bool yieldExpressionsSupported() {
        return versionNumber() >= JSVERSION_1_7 || pc->isGenerator();
    }

    virtual bool strictMode() { return pc->sc->strict; }

    const ReadOnlyCompileOptions& options() const {
        return tokenStream.options();
    }

  private:
    enum InvokedPrediction { PredictUninvoked = false, PredictInvoked = true };

  private:
    /*
     * JS parsers, from lowest to highest precedence.
     *
     * Each parser must be called during the dynamic scope of a ParseContext
     * object, pointed to by this->pc.
     *
     * Each returns a parse node tree or null on error.
     *
     * Parsers whose name has a '1' suffix leave the TokenStream state
     * pointing to the token one past the end of the parsed fragment.  For a
     * number of the parsers this is convenient and avoids a lot of
     * unnecessary ungetting and regetting of tokens.
     *
     * Some parsers have two versions:  an always-inlined version (with an 'i'
     * suffix) and a never-inlined version (with an 'n' suffix).
     */
    Node functionStmt();
    Node functionExpr(InvokedPrediction invoked = PredictUninvoked);
    Node statements();

    Node blockStatement();
    Node ifStatement();
    Node doWhileStatement();
    Node whileStatement();
    Node forStatement();
    Node switchStatement();
    Node continueStatement();
    Node breakStatement();
    Node returnStatement();
    Node withStatement();
    Node labeledStatement();
    Node throwStatement();
    Node tryStatement();
    Node debuggerStatement();

    Node lexicalDeclaration(bool isConst);
    Node letDeclarationOrBlock();
    Node importDeclaration();
    Node exportDeclaration();
    Node expressionStatement(InvokedPrediction invoked = PredictUninvoked);
    Node variables(ParseNodeKind kind, bool* psimple = nullptr,
                   StaticBlockObject* blockObj = nullptr,
                   VarContext varContext = HoistVars);
    Node expr(InvokedPrediction invoked = PredictUninvoked);
    Node assignExpr(InvokedPrediction invoked = PredictUninvoked);
    Node assignExprWithoutYield(unsigned err);
    Node yieldExpression();
    Node condExpr1(InvokedPrediction invoked = PredictUninvoked);
    Node orExpr1(InvokedPrediction invoked = PredictUninvoked);
    Node unaryExpr(InvokedPrediction invoked = PredictUninvoked);
    Node memberExpr(TokenKind tt, bool allowCallSyntax,
                    InvokedPrediction invoked = PredictUninvoked);
    Node primaryExpr(TokenKind tt, InvokedPrediction invoked = PredictUninvoked);
    Node parenExprOrGeneratorComprehension();
    Node exprInParens();

    bool methodDefinition(Node literal, Node propname, FunctionType type, FunctionSyntaxKind kind,
                          GeneratorKind generatorKind, JSOp Op);

    /*
     * Additional JS parsers.
     */
    bool functionArguments(FunctionSyntaxKind kind, FunctionType type, Node* list, Node funcpn,
                           bool* hasRest);

    Node functionDef(HandlePropertyName name, FunctionType type, FunctionSyntaxKind kind,
                     GeneratorKind generatorKind, InvokedPrediction invoked = PredictUninvoked);
    bool functionArgsAndBody(Node pn, HandleFunction fun,
                             FunctionType type, FunctionSyntaxKind kind,
                             GeneratorKind generatorKind,
                             Directives inheritedDirectives, Directives* newDirectives);

    Node unaryOpExpr(ParseNodeKind kind, JSOp op, uint32_t begin);

    Node condition();

    Node generatorComprehensionLambda(GeneratorKind comprehensionKind, unsigned begin,
                                      Node innerStmt);

    Node legacyComprehensionTail(Node kid, unsigned blockid, GeneratorKind comprehensionKind,
                                 ParseContext<ParseHandler>* outerpc,
                                 unsigned innerBlockScopeDepth);
    Node legacyArrayComprehension(Node array);
    Node legacyGeneratorExpr(Node kid);

    Node comprehensionTail(GeneratorKind comprehensionKind);
    Node comprehensionIf(GeneratorKind comprehensionKind);
    Node comprehensionFor(GeneratorKind comprehensionKind);
    Node comprehension(GeneratorKind comprehensionKind);
    Node arrayComprehension(uint32_t begin);
    Node generatorComprehension(uint32_t begin);

    bool argumentList(Node listNode, bool* isSpread);
    Node deprecatedLetBlockOrExpression(LetContext letContext);
    Node destructuringExpr(BindData<ParseHandler>* data, TokenKind tt);
    Node destructuringExprWithoutYield(BindData<ParseHandler>* data, TokenKind tt, unsigned msg);

    Node identifierName();

    bool matchLabel(MutableHandle<PropertyName*> label);

    bool allowsForEachIn() {
#if !JS_HAS_FOR_EACH_IN
        return false;
#else
        return versionNumber() >= JSVERSION_1_6;
#endif
    }

    enum AssignmentFlavor {
        PlainAssignment,
        CompoundAssignment,
        KeyedDestructuringAssignment,
        IncDecAssignment
    };

    bool checkAndMarkAsAssignmentLhs(Node pn, AssignmentFlavor flavor);
    bool matchInOrOf(bool* isForInp, bool* isForOfp);

    bool checkFunctionArguments();
    bool makeDefIntoUse(Definition* dn, Node pn, JSAtom* atom);
    bool checkFunctionDefinition(HandlePropertyName funName, Node* pn, FunctionSyntaxKind kind,
                                 bool* pbodyProcessed);
    bool finishFunctionDefinition(Node pn, FunctionBox* funbox, Node prelude, Node body);
    bool addFreeVariablesFromLazyFunction(JSFunction* fun, ParseContext<ParseHandler>* pc);

    bool isValidForStatementLHS(Node pn1, JSVersion version, bool forDecl, bool forEach,
                                ParseNodeKind headKind);
    bool checkForHeadConstInitializers(Node pn1);
    bool checkAndMarkAsIncOperand(Node kid, TokenKind tt, bool preorder);
    bool checkStrictAssignment(Node lhs);
    bool checkStrictBinding(PropertyName* name, Node pn);
    bool defineArg(Node funcpn, HandlePropertyName name,
                   bool disallowDuplicateArgs = false, Node* duplicatedArg = nullptr);
    Node pushLexicalScope(StmtInfoPC* stmt);
    Node pushLexicalScope(Handle<StaticBlockObject*> blockObj, StmtInfoPC* stmt);
    Node pushLetScope(Handle<StaticBlockObject*> blockObj, StmtInfoPC* stmt);
    bool noteNameUse(HandlePropertyName name, Node pn);
    Node objectLiteral();
    Node computedPropertyName(Node literal);
    Node arrayInitializer();
    Node newRegExp();

    Node newBindingNode(PropertyName* name, bool functionScope, VarContext varContext = HoistVars);
    bool checkDestructuring(BindData<ParseHandler>* data, Node left);
    bool checkDestructuringObject(BindData<ParseHandler>* data, Node objectPattern);
    bool checkDestructuringArray(BindData<ParseHandler>* data, Node arrayPattern);
    bool bindDestructuringVar(BindData<ParseHandler>* data, Node pn);
    bool bindDestructuringLHS(Node pn);
    bool makeSetCall(Node pn, unsigned msg);
    Node cloneDestructuringDefault(Node opn);
    Node cloneLeftHandSide(Node opn);
    Node cloneParseTree(Node opn);

    Node newNumber(const Token& tok) {
        return handler.newNumber(tok.number(), tok.decimalPoint(), tok.pos);
    }

    static bool
    bindDestructuringArg(BindData<ParseHandler>* data,
                         HandlePropertyName name, Parser<ParseHandler>* parser);

    static bool
    bindLexical(BindData<ParseHandler>* data,
                HandlePropertyName name, Parser<ParseHandler>* parser);

    static bool
    bindVarOrGlobalConst(BindData<ParseHandler>* data,
                         HandlePropertyName name, Parser<ParseHandler>* parser);

    static Node null() { return ParseHandler::null(); }

    bool reportRedeclaration(Node pn, Definition::Kind redeclKind, HandlePropertyName name);
    bool reportBadReturn(Node pn, ParseReportKind kind, unsigned errnum, unsigned anonerrnum);
    DefinitionNode getOrCreateLexicalDependency(ParseContext<ParseHandler>* pc, JSAtom* atom);

    bool leaveFunction(Node fn, ParseContext<ParseHandler>* outerpc,
                       FunctionSyntaxKind kind = Expression);

    TokenPos pos() const { return tokenStream.currentToken().pos; }

    bool asmJS(Node list);

    void addTelemetry(JSCompartment::DeprecatedLanguageExtension e);

    friend class LegacyCompExprTransplanter;
    friend struct BindData<ParseHandler>;
};

/* Declare some required template specializations. */

template <>
bool
Parser<FullParseHandler>::checkAndMarkAsAssignmentLhs(ParseNode* pn, AssignmentFlavor flavor);

template <>
bool
Parser<SyntaxParseHandler>::checkAndMarkAsAssignmentLhs(Node pn, AssignmentFlavor flavor);

} /* namespace frontend */
} /* namespace js */

/*
 * Convenience macro to access Parser.tokenStream as a pointer.
 */
#define TS(p) (&(p)->tokenStream)

#endif /* frontend_Parser_h */
