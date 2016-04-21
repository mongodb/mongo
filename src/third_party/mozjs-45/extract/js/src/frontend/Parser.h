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

class ModuleObject;
class StaticFunctionBoxScopeObject;

namespace frontend {

struct StmtInfoPC : public StmtInfoBase
{
    static const unsigned BlockIdLimit = 1 << ParseNode::NumBlockIdBits;

    StmtInfoPC*     enclosing;
    StmtInfoPC*     enclosingScope;

    uint32_t        blockid;        /* for simplified dominance computation */
    uint32_t        innerBlockScopeDepth; /* maximum depth of nested block scopes, in slots */

    // Lexical declarations inside switches are tricky because the block id
    // doesn't convey dominance information. Record what index the current
    // case's lexical declarations start at so we may generate dead zone
    // checks for other cases' declarations.
    //
    // Only valid if type is StmtType::SWITCH.
    uint16_t        firstDominatingLexicalInCase;

    explicit StmtInfoPC(ExclusiveContext* cx)
      : StmtInfoBase(cx),
        blockid(BlockIdLimit),
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

    GenericParseContext(GenericParseContext* parent, SharedContext* sc)
      : parent(parent),
        sc(sc),
        funHasReturnExpr(false),
        funHasReturnVoid(false)
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
struct MOZ_STACK_CLASS ParseContext : public GenericParseContext
{
    typedef typename ParseHandler::Node Node;
    typedef typename ParseHandler::DefinitionNode DefinitionNode;

    uint32_t        bodyid;         /* block number of program/function body */

    StmtInfoStack<StmtInfoPC> stmtStack;

    Node            maybeFunction;  /* sc->isFunctionBox, the pn where pn->pn_funbox == sc */

    // If sc->isFunctionBox(), this is used to temporarily link up the
    // FunctionBox with the JSFunction so the static scope chain may be walked
    // without a JSScript.
    mozilla::Maybe<JSFunction::AutoParseUsingFunctionBox> parseUsingFunctionBox;

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
    bool isMethod() const {
        return sc->isFunctionBox() && sc->asFunctionBox()->function()->isMethod();
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
    void updateDecl(TokenStream& ts, JSAtom* atom, Node newDecl);

    // After a script has been parsed, the parser generates the code's
    // "bindings". Bindings are a data-structure, ultimately stored in the
    // compiled JSScript, that serve three purposes:
    //
    //  - After parsing, the ParseContext is destroyed and 'decls' along with
    //    it. Mostly, the emitter just uses the binding information stored in
    //    the use/def nodes, but the emitter occasionally needs 'bindings' for
    //    various scope-related queries.
    //
    //  - For functions, bindings provide the initial js::Shape to use when
    //    creating a dynamic scope object (js::CallObject). This shape is used
    //    during dynamic name lookup.
    //
    //  - Sometimes a script's bindings are accessed at runtime to retrieve the
    //    contents of the lexical scope (e.g., from the debugger).
    //
    //  - For global and eval scripts, ES6 15.1.8 specifies that if there are
    //    name conflicts in the script, *no* bindings from the script are
    //    instantiated. So, record the vars and lexical bindings to check for
    //    redeclarations in the prologue.
    bool generateBindings(ExclusiveContext* cx, TokenStream& ts, LifoAlloc& alloc,
                          MutableHandle<Bindings> bindings) const;

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
    Rooted<TraceableVector<JSFunction*>> innerFunctions;

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
                 Node maybeFunction, SharedContext* sc, Directives* newDirectives)
      : GenericParseContext(parent, sc),
        bodyid(0),           // initialized in init()
        stmtStack(prs->context),
        maybeFunction(maybeFunction),
        lastYieldOffset(NoYieldOffset),
        blockScopeDepth(0),
        blockNode(ParseHandler::null()),
        decls_(prs->context, prs->alloc),
        args_(prs->context),
        vars_(prs->context),
        bodyLevelLexicals_(prs->context),
        parserPC(&prs->pc),
        oldpc(prs->pc),
        lexdeps(prs->context),
        funcStmts(nullptr),
        innerFunctions(prs->context, TraceableVector<JSFunction*>(prs->context)),
        newDirectives(newDirectives),
        inDeclDestructuring(false)
    {
        prs->pc = this;
        if (sc->isFunctionBox())
            parseUsingFunctionBox.emplace(prs->context, sc->asFunctionBox());
    }

    ~ParseContext();

    bool init(Parser<ParseHandler>& parser);

    unsigned blockid() { return stmtStack.innermost() ? stmtStack.innermost()->blockid : bodyid; }

    StmtInfoPC* innermostStmt() const { return stmtStack.innermost(); }
    StmtInfoPC* innermostScopeStmt() const { return stmtStack.innermostScopeStmt(); }
    JSObject* innermostStaticScope() const {
        if (StmtInfoPC* stmt = innermostScopeStmt())
            return stmt->staticScope;
        return sc->staticScope();
    }

    // True if we are at the topmost level of a entire script or function body.
    // For example, while parsing this code we would encounter f1 and f2 at
    // body level, but we would not encounter f3 or f4 at body level:
    //
    //   function f1() { function f2() { } }
    //   if (cond) { function f3() { if (cond) { function f4() { } } } }
    //
    bool atBodyLevel() {
        // 'eval' and non-syntactic scripts are always under an invisible
        // lexical scope, but since it is not syntactic, it should still be
        // considered at body level.
        if (sc->staticScope()->is<StaticEvalObject>()) {
            bool bl = !innermostStmt()->enclosing;
            MOZ_ASSERT_IF(bl, innermostStmt()->type == StmtType::BLOCK);
            MOZ_ASSERT_IF(bl, innermostStmt()->staticScope
                                             ->template as<StaticBlockObject>()
                                             .enclosingStaticScope() == sc->staticScope());
            return bl;
        }
        return !innermostStmt();
    }

    bool atGlobalLevel() {
        return atBodyLevel() && sc->isGlobalContext() && !innermostScopeStmt();
    }

    // True if we are at the topmost level of a module only.
    bool atModuleLevel() {
        return atBodyLevel() && sc->isModuleBox();
    }

    // True if the current lexical scope is the topmost level of a module.
    bool atModuleScope() {
        return sc->isModuleBox() && !innermostScopeStmt();
    }

    // True if this is the ParseContext for the body of a function created by
    // the Function constructor.
    bool isFunctionConstructorBody() const {
        return sc->isFunctionBox() && !parent && sc->asFunctionBox()->function()->isLambda();
    }

    inline bool useAsmOrInsideUseAsm() const {
        return sc->isFunctionBox() && sc->asFunctionBox()->useAsmOrInsideUseAsm();
    }
};

template <typename ParseHandler>
inline
Directives::Directives(ParseContext<ParseHandler>* parent)
  : strict_(parent->sc->strict()),
    asmJS_(parent->useAsmOrInsideUseAsm())
{}

template <typename ParseHandler>
struct BindData;

class CompExprTransplanter;

enum VarContext { HoistVars, DontHoistVars };
enum PropListType { ObjectLiteral, ClassBody, DerivedClassBody };
enum class PropertyType {
    Normal,
    Shorthand,
    Getter,
    GetterNoExpressionClosure,
    Setter,
    SetterNoExpressionClosure,
    Method,
    GeneratorMethod,
    Constructor,
    DerivedConstructor
};

// Specify a value for an ES6 grammar parametrization.  We have no enum for
// [Return] because its behavior is exactly equivalent to checking whether
// we're in a function box -- easier and simpler than passing an extra
// parameter everywhere.
enum YieldHandling { YieldIsName, YieldIsKeyword };
enum InHandling { InAllowed, InProhibited };
enum DefaultHandling { NameRequired, AllowDefaultName };
enum TripledotHandling { TripledotAllowed, TripledotProhibited };

template <typename ParseHandler>
class Parser : private JS::AutoGCRooter, public StrictModeGetter
{
    class MOZ_STACK_CLASS AutoPushStmtInfoPC
    {
        Parser<ParseHandler>& parser_;
        StmtInfoPC stmt_;

      public:
        AutoPushStmtInfoPC(Parser<ParseHandler>& parser, StmtType type);
        AutoPushStmtInfoPC(Parser<ParseHandler>& parser, StmtType type,
                           NestedScopeObject& staticScope);
        ~AutoPushStmtInfoPC();

        bool generateBlockId();
        bool makeInnermostLexicalScope(StaticBlockObject& blockObj);

        StmtInfoPC& operator*() { return stmt_; }
        StmtInfoPC* operator->() { return &stmt_; }
        operator StmtInfoPC*() { return &stmt_; }
    };

  public:
    ExclusiveContext* const context;
    LifoAlloc& alloc;

    TokenStream         tokenStream;
    LifoAlloc::Mark     tempPoolMark;

    /* list of parsed objects for GC tracing */
    ObjectBox* traceListHead;

    /* innermost parse context (stack-allocated) */
    ParseContext<ParseHandler>* pc;

    // List of all block scopes.
    AutoObjectVector blockScopes;

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

    void prepareNodeForMutation(Node node) { handler.prepareNodeForMutation(node); }
    void freeTree(Node node) { handler.freeTree(node); }

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
    Node parse();

    /*
     * Allocate a new parsed object or function container from
     * cx->tempLifoAlloc.
     */
    ObjectBox* newObjectBox(JSObject* obj);
    FunctionBox* newFunctionBox(Node fn, JSFunction* fun, ParseContext<ParseHandler>* outerpc,
                                Directives directives, GeneratorKind generatorKind,
                                JSObject* enclosingStaticScope);

    // Use when the funbox is the outermost.
    FunctionBox* newFunctionBox(Node fn, HandleFunction fun, Directives directives,
                                GeneratorKind generatorKind, HandleObject enclosingStaticScope)
    {
        return newFunctionBox(fn, fun, nullptr, directives, generatorKind,
                              enclosingStaticScope);
    }

    // Use when the funbox should be linked to the outerpc's innermost scope.
    FunctionBox* newFunctionBox(Node fn, HandleFunction fun, ParseContext<ParseHandler>* outerpc,
                                Directives directives, GeneratorKind generatorKind)
    {
        RootedObject enclosing(context, outerpc->innermostStaticScope());
        return newFunctionBox(fn, fun, outerpc, directives, generatorKind, enclosing);
    }

    ModuleBox* newModuleBox(Node pn, HandleModuleObject module);

    /*
     * Create a new function object given a name (which is optional if this is
     * a function expression).
     */
    JSFunction* newFunction(HandleAtom atom, FunctionSyntaxKind kind, GeneratorKind generatorKind,
                            HandleObject proto);

    bool generateBlockId(JSObject* staticScope, uint32_t* blockIdOut) {
        if (blockScopes.length() == StmtInfoPC::BlockIdLimit) {
            tokenStream.reportError(JSMSG_NEED_DIET, "program");
            return false;
        }
        MOZ_ASSERT(blockScopes.length() < StmtInfoPC::BlockIdLimit);
        *blockIdOut = blockScopes.length();
        return blockScopes.append(staticScope);
    }

    void trace(JSTracer* trc);

    bool hadAbortedSyntaxParse() {
        return abortedSyntaxParse;
    }
    void clearAbortedSyntaxParse() {
        abortedSyntaxParse = false;
    }

    bool isUnexpectedEOF() const { return isUnexpectedEOF_; }

    bool checkUnescapedName();

  private:
    Parser* thisForCtor() { return this; }

    JSAtom * stopStringCompression();

    Node stringLiteral();
    Node noSubstitutionTemplate();
    Node templateLiteral(YieldHandling yieldHandling);
    bool taggedTemplate(YieldHandling yieldHandling, Node nodeList, TokenKind tt);
    bool appendToCallSiteObj(Node callSiteObj);
    bool addExprAndGetNextTemplStrToken(YieldHandling yieldHandling, Node nodeList,
                                        TokenKind* ttp);
    bool checkStatementsEOF();

    inline Node newName(PropertyName* name);
    inline Node newYieldExpression(uint32_t begin, Node expr, bool isYieldStar = false);

    inline bool abortIfSyntaxParser();

  public:
    /* Public entry points for parsing. */
    Node statement(YieldHandling yieldHandling, bool canHaveDirectives = false);

    bool maybeParseDirective(Node list, Node pn, bool* cont);

    // Parse the body of an eval.
    //
    // Eval scripts are distinguished from global scripts in that in ES6, per
    // 18.2.1.1 steps 9 and 10, all eval scripts are executed under a fresh
    // lexical scope.
    Node evalBody();

    // Parse the body of a global script.
    Node globalBody();

    // Parse a module.
    Node standaloneModule(Handle<ModuleObject*> module);

    // Parse a function, given only its body. Used for the Function and
    // Generator constructors.
    Node standaloneFunctionBody(HandleFunction fun, Handle<PropertyNameVector> formals,
                                GeneratorKind generatorKind,
                                Directives inheritedDirectives, Directives* newDirectives,
                                HandleObject enclosingStaticScope);

    // Parse a function, given only its arguments and body. Used for lazily
    // parsed functions.
    Node standaloneLazyFunction(HandleFunction fun, bool strict, GeneratorKind generatorKind);

    /*
     * Parse a function body.  Pass StatementListBody if the body is a list of
     * statements; pass ExpressionBody if the body is a single expression.
     */
    enum FunctionBodyType { StatementListBody, ExpressionBody };
    Node functionBody(InHandling inHandling, YieldHandling yieldHandling, FunctionSyntaxKind kind,
                      FunctionBodyType type);

    bool functionArgsAndBodyGeneric(InHandling inHandling, YieldHandling yieldHandling, Node pn,
                                    HandleFunction fun, FunctionSyntaxKind kind);

    // Determine whether |yield| is a valid name in the current context, or
    // whether it's prohibited due to strictness, JS version, or occurrence
    // inside a star generator.
    bool checkYieldNameValidity();
    bool yieldExpressionsSupported() {
        return versionNumber() >= JSVERSION_1_7 || pc->isGenerator();
    }

    virtual bool strictMode() { return pc->sc->strict(); }
    bool setLocalStrictMode(bool strict) {
        MOZ_ASSERT(tokenStream.debugHasNoLookahead());
        return pc->sc->setLocalStrictMode(strict);
    }

    const ReadOnlyCompileOptions& options() const {
        return tokenStream.options();
    }

  private:
    enum InvokedPrediction { PredictUninvoked = false, PredictInvoked = true };
    enum ForInitLocation { InForInit, NotInForInit };

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
    Node functionStmt(YieldHandling yieldHandling, DefaultHandling defaultHandling);
    Node functionExpr(InvokedPrediction invoked = PredictUninvoked);
    Node statements(YieldHandling yieldHandling);

    Node blockStatement(YieldHandling yieldHandling);
    Node ifStatement(YieldHandling yieldHandling);
    Node doWhileStatement(YieldHandling yieldHandling);
    Node whileStatement(YieldHandling yieldHandling);
    Node forStatement(YieldHandling yieldHandling);
    Node switchStatement(YieldHandling yieldHandling);
    Node continueStatement(YieldHandling yieldHandling);
    Node breakStatement(YieldHandling yieldHandling);
    Node returnStatement(YieldHandling yieldHandling);
    Node withStatement(YieldHandling yieldHandling);
    Node labeledStatement(YieldHandling yieldHandling);
    Node throwStatement(YieldHandling yieldHandling);
    Node tryStatement(YieldHandling yieldHandling);
    Node debuggerStatement();

    Node lexicalDeclaration(YieldHandling yieldHandling, bool isConst);
    Node importDeclaration();
    Node exportDeclaration();
    Node expressionStatement(YieldHandling yieldHandling,
                             InvokedPrediction invoked = PredictUninvoked);
    Node variables(YieldHandling yieldHandling,
                   ParseNodeKind kind,
                   ForInitLocation location,
                   bool* psimple = nullptr, StaticBlockObject* blockObj = nullptr,
                   VarContext varContext = HoistVars);
    Node expr(InHandling inHandling, YieldHandling yieldHandling,
              TripledotHandling tripledotHandling,
              InvokedPrediction invoked = PredictUninvoked);
    Node assignExpr(InHandling inHandling, YieldHandling yieldHandling,
                    TripledotHandling tripledotHandling,
                    InvokedPrediction invoked = PredictUninvoked);
    Node assignExprWithoutYield(YieldHandling yieldHandling, unsigned err);
    Node yieldExpression(InHandling inHandling);
    Node condExpr1(InHandling inHandling, YieldHandling yieldHandling,
                   TripledotHandling tripledotHandling,
                   InvokedPrediction invoked = PredictUninvoked);
    Node orExpr1(InHandling inHandling, YieldHandling yieldHandling,
                 TripledotHandling tripledotHandling,
                   InvokedPrediction invoked = PredictUninvoked);
    Node unaryExpr(YieldHandling yieldHandling, TripledotHandling tripledotHandling,
                   InvokedPrediction invoked = PredictUninvoked);
    Node memberExpr(YieldHandling yieldHandling, TripledotHandling tripledotHandling, TokenKind tt,
                    bool allowCallSyntax, InvokedPrediction invoked = PredictUninvoked);
    Node primaryExpr(YieldHandling yieldHandling, TripledotHandling tripledotHandling, TokenKind tt,
                     InvokedPrediction invoked = PredictUninvoked);
    Node exprInParens(InHandling inHandling, YieldHandling yieldHandling,
                      TripledotHandling tripledotHandling);

    bool tryNewTarget(Node& newTarget);
    bool checkAndMarkSuperScope();

    Node methodDefinition(YieldHandling yieldHandling, PropertyType propType,
                          HandlePropertyName funName);

    /*
     * Additional JS parsers.
     */
    bool functionArguments(YieldHandling yieldHandling, FunctionSyntaxKind kind,
                           Node funcpn, bool* hasRest);

    Node functionDef(InHandling inHandling, YieldHandling uieldHandling, HandlePropertyName name,
                     FunctionSyntaxKind kind, GeneratorKind generatorKind,
                     InvokedPrediction invoked = PredictUninvoked);
    bool functionArgsAndBody(InHandling inHandling, Node pn, HandleFunction fun,
                             FunctionSyntaxKind kind, GeneratorKind generatorKind,
                             Directives inheritedDirectives, Directives* newDirectives);

    Node unaryOpExpr(YieldHandling yieldHandling, ParseNodeKind kind, JSOp op, uint32_t begin);

    Node condition(InHandling inHandling, YieldHandling yieldHandling);

    /* comprehensions */
    Node legacyComprehensionTail(Node kid, unsigned blockid, GeneratorKind comprehensionKind,
                                 ParseContext<ParseHandler>* outerpc,
                                 unsigned innerBlockScopeDepth);
    Node legacyArrayComprehension(Node array);
    Node generatorComprehensionLambda(GeneratorKind comprehensionKind, unsigned begin,
                                      Node innerStmt);
    Node legacyGeneratorExpr(Node kid);
    Node comprehensionFor(GeneratorKind comprehensionKind);
    Node comprehensionIf(GeneratorKind comprehensionKind);
    Node comprehensionTail(GeneratorKind comprehensionKind);
    Node comprehension(GeneratorKind comprehensionKind);
    Node arrayComprehension(uint32_t begin);
    Node generatorComprehension(uint32_t begin);

    bool argumentList(YieldHandling yieldHandling, Node listNode, bool* isSpread);
    Node destructuringExpr(YieldHandling yieldHandling, BindData<ParseHandler>* data,
                           TokenKind tt);
    Node destructuringExprWithoutYield(YieldHandling yieldHandling, BindData<ParseHandler>* data,
                                       TokenKind tt, unsigned msg);

    Node newBoundImportForCurrentName();
    bool namedImportsOrNamespaceImport(TokenKind tt, Node importSpecSet);
    bool addExportName(JSAtom* exportName);

    enum ClassContext { ClassStatement, ClassExpression };
    Node classDefinition(YieldHandling yieldHandling, ClassContext classContext, DefaultHandling defaultHandling);

    Node identifierName(YieldHandling yieldHandling);

    bool matchLabel(YieldHandling yieldHandling, MutableHandle<PropertyName*> label);

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
        IncrementAssignment,
        DecrementAssignment
    };

    bool checkAndMarkAsAssignmentLhs(Node pn, AssignmentFlavor flavor);
    bool matchInOrOf(bool* isForInp, bool* isForOfp);

    bool checkFunctionArguments();

    bool defineFunctionThis();
    Node newThisName();

    bool makeDefIntoUse(Definition* dn, Node pn, HandleAtom atom);
    bool checkFunctionDefinition(HandlePropertyName funName, Node* pn, FunctionSyntaxKind kind,
                                 bool* pbodyProcessed);
    bool finishFunctionDefinition(Node pn, FunctionBox* funbox, Node body);
    bool addFreeVariablesFromLazyFunction(JSFunction* fun, ParseContext<ParseHandler>* pc);

    bool isValidForStatementLHS(Node pn1, JSVersion version, bool forDecl, bool forEach,
                                ParseNodeKind headKind);
    bool checkForHeadConstInitializers(Node pn1);

    // Use when the current token is TOK_NAME and is known to be 'let'.
    bool shouldParseLetDeclaration(bool* parseDeclOut);

    // Use when the lookahead token is TOK_NAME and is known to be 'let'. If a
    // let declaration should be parsed, the TOK_NAME token of 'let' is
    // consumed. Otherwise, the current token remains the TOK_NAME token of
    // 'let'.
    bool peekShouldParseLetDeclaration(bool* parseDeclOut, TokenStream::Modifier modifier);

  public:
    enum FunctionCallBehavior {
        PermitAssignmentToFunctionCalls,
        ForbidAssignmentToFunctionCalls
    };

    bool isValidSimpleAssignmentTarget(Node node,
                                       FunctionCallBehavior behavior = ForbidAssignmentToFunctionCalls);

  private:
    bool reportIfArgumentsEvalTarget(Node nameNode);
    bool reportIfNotValidSimpleAssignmentTarget(Node target, AssignmentFlavor flavor);

    bool checkAndMarkAsIncOperand(Node kid, AssignmentFlavor flavor);

    bool checkStrictAssignment(Node lhs);

    bool checkStrictBinding(PropertyName* name, Node pn);
    bool defineArg(Node funcpn, HandlePropertyName name,
                   bool disallowDuplicateArgs = false, Node* duplicatedArg = nullptr);
    Node pushLexicalScope(AutoPushStmtInfoPC& stmt);
    Node pushLexicalScope(Handle<StaticBlockObject*> blockObj, AutoPushStmtInfoPC& stmt);
    Node pushLetScope(Handle<StaticBlockObject*> blockObj, AutoPushStmtInfoPC& stmt);
    bool noteNameUse(HandlePropertyName name, Node pn);
    Node propertyName(YieldHandling yieldHandling, Node propList,
                      PropertyType* propType, MutableHandleAtom propAtom);
    Node computedPropertyName(YieldHandling yieldHandling, Node literal);
    Node arrayInitializer(YieldHandling yieldHandling);
    Node newRegExp();

    Node objectLiteral(YieldHandling yieldHandling);

    bool checkAndPrepareLexical(bool isConst, const TokenPos& errorPos);
    Node makeInitializedLexicalBinding(HandlePropertyName name, bool isConst, const TokenPos& pos);

    Node newBindingNode(PropertyName* name, bool functionScope, VarContext varContext = HoistVars);

    // Top-level entrypoint into destructuring pattern checking/name-analyzing.
    bool checkDestructuringPattern(BindData<ParseHandler>* data, Node pattern);

    // Recursive methods for checking/name-analyzing subcomponents of a
    // destructuring pattern.  The array/object methods *must* be passed arrays
    // or objects.  The name method may be passed anything but will report an
    // error if not passed a name.
    bool checkDestructuringArray(BindData<ParseHandler>* data, Node arrayPattern);
    bool checkDestructuringObject(BindData<ParseHandler>* data, Node objectPattern);
    bool checkDestructuringName(BindData<ParseHandler>* data, Node expr);

    bool bindInitialized(BindData<ParseHandler>* data, Node pn);
    bool bindUninitialized(BindData<ParseHandler>* data, Node pn);
    bool makeSetCall(Node node, unsigned errnum);
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
    bindVar(BindData<ParseHandler>* data,
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

    bool warnOnceAboutExprClosure();

    friend class LegacyCompExprTransplanter;
    friend struct BindData<ParseHandler>;
};

} /* namespace frontend */
} /* namespace js */

/*
 * Convenience macro to access Parser.tokenStream as a pointer.
 */
#define TS(p) (&(p)->tokenStream)

#endif /* frontend_Parser_h */
