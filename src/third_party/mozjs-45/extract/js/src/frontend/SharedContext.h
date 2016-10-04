/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SharedContext_h
#define frontend_SharedContext_h

#include "jsatom.h"
#include "jsopcode.h"
#include "jspubtd.h"
#include "jsscript.h"
#include "jstypes.h"

#include "builtin/ModuleObject.h"
#include "frontend/ParseMaps.h"
#include "frontend/ParseNode.h"
#include "frontend/TokenStream.h"
#include "vm/ScopeObject.h"

namespace js {
namespace frontend {

// These flags apply to both global and function contexts.
class AnyContextFlags
{
    // This class's data is all private and so only visible to these friends.
    friend class SharedContext;

    // True if "use strict"; appears in the body instead of being inherited.
    bool            hasExplicitUseStrict:1;

    // The (static) bindings of this script need to support dynamic name
    // read/write access. Here, 'dynamic' means dynamic dictionary lookup on
    // the scope chain for a dynamic set of keys. The primary examples are:
    //  - direct eval
    //  - function::
    //  - with
    // since both effectively allow any name to be accessed. Non-examples are:
    //  - upvars of nested functions
    //  - function statement
    // since the set of assigned name is known dynamically. 'with' could be in
    // the non-example category, provided the set of all free variables within
    // the with block was noted. However, we do not optimize 'with' so, for
    // simplicity, 'with' is treated like eval.
    //
    // Note: access through the arguments object is not considered dynamic
    // binding access since it does not go through the normal name lookup
    // mechanism. This is debatable and could be changed (although care must be
    // taken not to turn off the whole 'arguments' optimization). To answer the
    // more general "is this argument aliased" question, script->needsArgsObj
    // should be tested (see JSScript::argIsAlised).
    //
    bool            bindingsAccessedDynamically:1;

    // Whether this script, or any of its inner scripts contains a debugger
    // statement which could potentially read or write anywhere along the
    // scope chain.
    bool            hasDebuggerStatement:1;

    // A direct eval occurs in the body of the script.
    bool            hasDirectEval:1;

  public:
    AnyContextFlags()
     :  hasExplicitUseStrict(false),
        bindingsAccessedDynamically(false),
        hasDebuggerStatement(false),
        hasDirectEval(false)
    { }
};

class FunctionContextFlags
{
    // This class's data is all private and so only visible to these friends.
    friend class FunctionBox;

    // The function or a function that encloses it may define new local names
    // at runtime through means other than calling eval.
    bool mightAliasLocals:1;

    // This function does something that can extend the set of bindings in its
    // call objects --- it does a direct eval in non-strict code, or includes a
    // function statement (as opposed to a function definition).
    //
    // This flag is *not* inherited by enclosed or enclosing functions; it
    // applies only to the function in whose flags it appears.
    //
    bool hasExtensibleScope:1;

    // This function refers directly to its name in a way which requires the
    // name to be a separate object on the scope chain.
    bool needsDeclEnvObject:1;

    // Technically, every function has a binding named 'arguments'. Internally,
    // this binding is only added when 'arguments' is mentioned by the function
    // body. This flag indicates whether 'arguments' has been bound either
    // through implicit use:
    //   function f() { return arguments }
    // or explicit redeclaration:
    //   function f() { var arguments; return arguments }
    //
    // Note 1: overwritten arguments (function() { arguments = 3 }) will cause
    // this flag to be set but otherwise require no special handling:
    // 'arguments' is just a local variable and uses of 'arguments' will just
    // read the local's current slot which may have been assigned. The only
    // special semantics is that the initial value of 'arguments' is the
    // arguments object (not undefined, like normal locals).
    //
    // Note 2: if 'arguments' is bound as a formal parameter, there will be an
    // 'arguments' in Bindings, but, as the "LOCAL" in the name indicates, this
    // flag will not be set. This is because, as a formal, 'arguments' will
    // have no special semantics: the initial value is unconditionally the
    // actual argument (or undefined if nactual < nformal).
    //
    bool argumentsHasLocalBinding:1;

    // In many cases where 'arguments' has a local binding (as described above)
    // we do not need to actually create an arguments object in the function
    // prologue: instead we can analyze how 'arguments' is used (using the
    // simple dataflow analysis in analyzeSSA) to determine that uses of
    // 'arguments' can just read from the stack frame directly. However, the
    // dataflow analysis only looks at how JSOP_ARGUMENTS is used, so it will
    // be unsound in several cases. The frontend filters out such cases by
    // setting this flag which eagerly sets script->needsArgsObj to true.
    //
    bool definitelyNeedsArgsObj:1;

    bool needsHomeObject:1;
    bool isDerivedClassConstructor:1;

    // Whether this function has a .this binding. If true, we need to emit
    // JSOP_FUNCTIONTHIS in the prologue to initialize it.
    bool hasThisBinding:1;

  public:
    FunctionContextFlags()
     :  mightAliasLocals(false),
        hasExtensibleScope(false),
        needsDeclEnvObject(false),
        argumentsHasLocalBinding(false),
        definitelyNeedsArgsObj(false),
        needsHomeObject(false),
        isDerivedClassConstructor(false),
        hasThisBinding(false)
    { }
};

// List of directives that may be encountered in a Directive Prologue (ES5 15.1).
class Directives
{
    bool strict_;
    bool asmJS_;

  public:
    explicit Directives(bool strict) : strict_(strict), asmJS_(false) {}
    template <typename ParseHandler> explicit Directives(ParseContext<ParseHandler>* parent);

    void setStrict() { strict_ = true; }
    bool strict() const { return strict_; }

    void setAsmJS() { asmJS_ = true; }
    bool asmJS() const { return asmJS_; }

    Directives& operator=(Directives rhs) {
        strict_ = rhs.strict_;
        asmJS_ = rhs.asmJS_;
        return *this;
    }
    bool operator==(const Directives& rhs) const {
        return strict_ == rhs.strict_ && asmJS_ == rhs.asmJS_;
    }
    bool operator!=(const Directives& rhs) const {
        return !(*this == rhs);
    }
};

// The kind of this-binding for the current scope. Note that arrow functions
// (and generator expression lambdas) have a lexical this-binding so their
// ThisBinding is the same as the ThisBinding of their enclosing scope and can
// be any value.
enum class ThisBinding { Global, Function, Module };

/*
 * The struct SharedContext is part of the current parser context (see
 * ParseContext). It stores information that is reused between the parser and
 * the bytecode emitter. Note however, that this information is not shared
 * between the two; they simply reuse the same data structure.
 */
class SharedContext
{
  public:
    ExclusiveContext* const context;
    AnyContextFlags anyCxFlags;
    bool strictScript;
    bool localStrict;
    bool extraWarnings;

  private:
    ThisBinding thisBinding_;

    bool allowNewTarget_;
    bool allowSuperProperty_;
    bool allowSuperCall_;
    bool inWith_;
    bool needsThisTDZChecks_;
    bool superScopeAlreadyNeedsHomeObject_;

  public:
    SharedContext(ExclusiveContext* cx, Directives directives,
                  bool extraWarnings)
      : context(cx),
        anyCxFlags(),
        strictScript(directives.strict()),
        localStrict(false),
        extraWarnings(extraWarnings),
        thisBinding_(ThisBinding::Global),
        allowNewTarget_(false),
        allowSuperProperty_(false),
        allowSuperCall_(false),
        inWith_(false),
        needsThisTDZChecks_(false),
        superScopeAlreadyNeedsHomeObject_(false)
    { }

    // The unfortunate reason that staticScope() is a virtual is because
    // GlobalSharedContext and FunctionBox have different lifetimes.
    // GlobalSharedContexts are stack allocated and thus may use RootedObject
    // for the static scope. FunctionBoxes are LifoAlloc'd and need to
    // manually trace their static scope.
    virtual JSObject* staticScope() const = 0;
    void computeAllowSyntax(JSObject* staticScope);
    void computeInWith(JSObject* staticScope);
    void computeThisBinding(JSObject* staticScope);

    virtual ObjectBox* toObjectBox() { return nullptr; }
    bool isObjectBox() { return toObjectBox() != nullptr; }
    bool isFunctionBox() { return isObjectBox() && toObjectBox()->isFunctionBox(); }
    inline FunctionBox* asFunctionBox();
    bool isModuleBox() { return isObjectBox() && toObjectBox()->isModuleBox(); }
    inline ModuleBox* asModuleBox();
    bool isGlobalContext() { return !toObjectBox(); }

    ThisBinding thisBinding()          const { return thisBinding_; }

    bool allowNewTarget()              const { return allowNewTarget_; }
    bool allowSuperProperty()          const { return allowSuperProperty_; }
    bool allowSuperCall()              const { return allowSuperCall_; }
    bool inWith()                      const { return inWith_; }
    bool needsThisTDZChecks()          const { return needsThisTDZChecks_; }

    void markSuperScopeNeedsHomeObject();

    bool hasExplicitUseStrict()        const { return anyCxFlags.hasExplicitUseStrict; }
    bool bindingsAccessedDynamically() const { return anyCxFlags.bindingsAccessedDynamically; }
    bool hasDebuggerStatement()        const { return anyCxFlags.hasDebuggerStatement; }
    bool hasDirectEval()               const { return anyCxFlags.hasDirectEval; }

    void setExplicitUseStrict()           { anyCxFlags.hasExplicitUseStrict        = true; }
    void setBindingsAccessedDynamically() { anyCxFlags.bindingsAccessedDynamically = true; }
    void setHasDebuggerStatement()        { anyCxFlags.hasDebuggerStatement        = true; }
    void setHasDirectEval()               { anyCxFlags.hasDirectEval               = true; }

    inline bool allLocalsAliased();

    bool strict() const {
        return strictScript || localStrict;
    }
    bool setLocalStrictMode(bool strict) {
        bool retVal = localStrict;
        localStrict = strict;
        return retVal;
    }

    // JSOPTION_EXTRA_WARNINGS warnings or strict mode errors.
    bool needStrictChecks() const {
        return strict() || extraWarnings;
    }

    bool isDotVariable(JSAtom* atom) const {
        return atom == context->names().dotGenerator || atom == context->names().dotThis;
    }
};

class MOZ_STACK_CLASS GlobalSharedContext : public SharedContext
{
    Rooted<ScopeObject*> staticScope_;

  public:
    GlobalSharedContext(ExclusiveContext* cx, ScopeObject* staticScope, Directives directives,
                        bool extraWarnings, JSFunction* maybeEvalCaller = nullptr)
      : SharedContext(cx, directives, extraWarnings),
        staticScope_(cx, staticScope)
    {
        computeAllowSyntax(staticScope);
        computeInWith(staticScope);

        // If we're executing a Debugger eval-in-frame, staticScope is always a
        // non-function scope, so we have to compute our ThisBinding based on
        // the actual callee.
        if (maybeEvalCaller)
            computeThisBinding(maybeEvalCaller);
        else
            computeThisBinding(staticScope);
    }

    JSObject* staticScope() const override { return staticScope_; }
};

class FunctionBox : public ObjectBox, public SharedContext
{
  public:
    Bindings        bindings;               /* bindings for this function */
    JSObject*       enclosingStaticScope_;
    uint32_t        bufStart;
    uint32_t        bufEnd;
    uint32_t        startLine;
    uint32_t        startColumn;
    uint16_t        length;

    uint8_t         generatorKindBits_;     /* The GeneratorKind of this function. */
    bool            inGenexpLambda:1;       /* lambda from generator expression */
    bool            hasDestructuringArgs:1; /* arguments list contains destructuring expression */
    bool            useAsm:1;               /* see useAsmOrInsideUseAsm */
    bool            insideUseAsm:1;         /* see useAsmOrInsideUseAsm */
    bool            wasEmitted:1;           /* Bytecode has been emitted for this function. */

    // Fields for use in heuristics.
    bool            usesArguments:1;  /* contains a free use of 'arguments' */
    bool            usesApply:1;      /* contains an f.apply() call */
    bool            usesThis:1;       /* contains 'this' */

    FunctionContextFlags funCxFlags;

    template <typename ParseHandler>
    FunctionBox(ExclusiveContext* cx, ObjectBox* traceListHead, JSFunction* fun,
                JSObject* enclosingStaticScope, ParseContext<ParseHandler>* pc,
                Directives directives, bool extraWarnings, GeneratorKind generatorKind);

    ObjectBox* toObjectBox() override { return this; }
    JSFunction* function() const { return &object->as<JSFunction>(); }
    JSObject* staticScope() const override { return function(); }
    JSObject* enclosingStaticScope() const { return enclosingStaticScope_; }

    GeneratorKind generatorKind() const { return GeneratorKindFromBits(generatorKindBits_); }
    bool isGenerator() const { return generatorKind() != NotGenerator; }
    bool isLegacyGenerator() const { return generatorKind() == LegacyGenerator; }
    bool isStarGenerator() const { return generatorKind() == StarGenerator; }
    bool isArrow() const { return function()->isArrow(); }

    void setGeneratorKind(GeneratorKind kind) {
        // A generator kind can be set at initialization, or when "yield" is
        // first seen.  In both cases the transition can only happen from
        // NotGenerator.
        MOZ_ASSERT(!isGenerator());
        generatorKindBits_ = GeneratorKindAsBits(kind);
    }

    bool mightAliasLocals()         const { return funCxFlags.mightAliasLocals; }
    bool hasExtensibleScope()       const { return funCxFlags.hasExtensibleScope; }
    bool needsDeclEnvObject()       const { return funCxFlags.needsDeclEnvObject; }
    bool hasThisBinding()           const { return funCxFlags.hasThisBinding; }
    bool argumentsHasLocalBinding() const { return funCxFlags.argumentsHasLocalBinding; }
    bool definitelyNeedsArgsObj()   const { return funCxFlags.definitelyNeedsArgsObj; }
    bool needsHomeObject()          const { return funCxFlags.needsHomeObject; }
    bool isDerivedClassConstructor() const { return funCxFlags.isDerivedClassConstructor; }

    void setMightAliasLocals()             { funCxFlags.mightAliasLocals         = true; }
    void setHasExtensibleScope()           { funCxFlags.hasExtensibleScope       = true; }
    void setNeedsDeclEnvObject()           { funCxFlags.needsDeclEnvObject       = true; }
    void setHasThisBinding()               { funCxFlags.hasThisBinding           = true; }
    void setArgumentsHasLocalBinding()     { funCxFlags.argumentsHasLocalBinding = true; }
    void setDefinitelyNeedsArgsObj()       { MOZ_ASSERT(funCxFlags.argumentsHasLocalBinding);
                                             funCxFlags.definitelyNeedsArgsObj   = true; }
    void setNeedsHomeObject()              { MOZ_ASSERT(function()->allowSuperProperty());
                                             funCxFlags.needsHomeObject          = true; }
    void setDerivedClassConstructor()      { MOZ_ASSERT(function()->isClassConstructor());
                                             funCxFlags.isDerivedClassConstructor = true; }

    bool hasDefaults() const {
        return length != function()->nargs() - function()->hasRest();
    }

    bool hasMappedArgsObj() const {
        return !strict() && !function()->hasRest() && !hasDefaults() && !hasDestructuringArgs;
    }

    // Return whether this or an enclosing function is being parsed and
    // validated as asm.js. Note: if asm.js validation fails, this will be false
    // while the function is being reparsed. This flag can be used to disable
    // certain parsing features that are necessary in general, but unnecessary
    // for validated asm.js.
    bool useAsmOrInsideUseAsm() const {
        return useAsm || insideUseAsm;
    }

    void setStart(const TokenStream& tokenStream) {
        bufStart = tokenStream.currentToken().pos.begin;
        startLine = tokenStream.getLineno();
        startColumn = tokenStream.getColumn();
    }

    bool needsCallObject()
    {
        // Note: this should be kept in sync with JSFunction::needsCallObject().
        return bindings.hasAnyAliasedBindings() ||
               hasExtensibleScope() ||
               needsDeclEnvObject() ||
               needsHomeObject()    ||
               isDerivedClassConstructor() ||
               isGenerator();
    }
};

class ModuleBox : public ObjectBox, public SharedContext
{
  public:
    Bindings bindings;
    TraceableVector<JSAtom*> exportNames;

    template <typename ParseHandler>
    ModuleBox(ExclusiveContext* cx, ObjectBox* traceListHead, ModuleObject* module,
              ParseContext<ParseHandler>* pc);

    ObjectBox* toObjectBox() override { return this; }
    ModuleObject* module() const { return &object->as<ModuleObject>(); }
    JSObject* staticScope() const override { return module(); }
};

inline FunctionBox*
SharedContext::asFunctionBox()
{
    MOZ_ASSERT(isFunctionBox());
    return static_cast<FunctionBox*>(this);
}

inline ModuleBox*
SharedContext::asModuleBox()
{
    MOZ_ASSERT(isModuleBox());
    return static_cast<ModuleBox*>(this);
}


// In generators, we treat all locals as aliased so that they get stored on the
// heap.  This way there is less information to copy off the stack when
// suspending, and back on when resuming.  It also avoids the need to create and
// invalidate DebugScope proxies for unaliased locals in a generator frame, as
// the generator frame will be copied out to the heap and released only by GC.
inline bool
SharedContext::allLocalsAliased()
{
    return bindingsAccessedDynamically() || (isFunctionBox() && asFunctionBox()->isGenerator());
}


/*
 * NB: If you add a new type of statement that is a scope, add it between
 * STMT_WITH and STMT_CATCH, or you will break StmtInfoBase::linksScope. If you
 * add a non-looping statement type, add it before STMT_DO_LOOP or you will
 * break StmtInfoBase::isLoop().
 *
 * Also remember to keep the statementName array in BytecodeEmitter.cpp in
 * sync.
 */
enum class StmtType : uint16_t {
    LABEL,                 /* labeled statement:  L: s */
    IF,                    /* if (then) statement */
    ELSE,                  /* else clause of if statement */
    SEQ,                   /* synthetic sequence of statements */
    BLOCK,                 /* compound statement: { s1[;... sN] } */
    SWITCH,                /* switch statement */
    WITH,                  /* with statement */
    CATCH,                 /* catch block */
    TRY,                   /* try block */
    FINALLY,               /* finally block */
    SUBROUTINE,            /* gosub-target subroutine body */
    DO_LOOP,               /* do/while loop statement */
    FOR_LOOP,              /* for loop statement */
    FOR_IN_LOOP,           /* for/in loop statement */
    FOR_OF_LOOP,           /* for/of loop statement */
    WHILE_LOOP,            /* while loop statement */
    SPREAD,                /* spread operator (pseudo for/of) */
    LIMIT
};

/*
 * A comment on the encoding of the js::StmtType enum and StmtInfoBase
 * type-testing methods:
 *
 * StmtInfoBase::maybeScope() tells whether a statement type is always, or may
 * become, a lexical scope. It therefore includes block and switch (the two
 * low-numbered "maybe" scope types) and excludes with (with has dynamic scope
 * pending the "reformed with" in ES4/JS2). It includes all try-catch-finally
 * types, which are high-numbered maybe-scope types.
 *
 * StmtInfoBase::linksScope() tells whether a js::StmtInfo{PC,BCE} of the given
 * type eagerly links to other scoping statement info records. It excludes the
 * two early "maybe" types, block and switch, as well as the try and both
 * finally types, since try and the other trailing maybe-scope types don't need
 * block scope unless they contain let declarations.
 *
 * We treat WITH as a static scope because it prevents lexical binding from
 * continuing further up the static scope chain. With the lost "reformed with"
 * proposal for ES4, we would be able to model it statically, too.
 */

// StmtInfoPC is used by the Parser.  StmtInfoBCE is used by the
// BytecodeEmitter.  The two types have some overlap, encapsulated by
// StmtInfoBase.

struct StmtInfoBase
{
    // Statement type (StmtType).
    StmtType type;

    // True if type is StmtType::BLOCK, StmtType::TRY, StmtType::SWITCH, or
    // StmtType::FINALLY and the block contains at least one let-declaration,
    // or if type is StmtType::CATCH.
    bool isBlockScope:1;

    // for (let ...) induced block scope
    bool isForLetBlock:1;

    // Block label.
    RootedAtom      label;

    // Compile-time scope chain node for this scope.
    Rooted<NestedScopeObject*> staticScope;

    explicit StmtInfoBase(ExclusiveContext* cx)
        : isBlockScope(false), isForLetBlock(false),
          label(cx), staticScope(cx)
    {}

    bool maybeScope() const {
        return StmtType::BLOCK <= type && type <= StmtType::SUBROUTINE &&
               type != StmtType::WITH;
    }

    bool linksScope() const {
        return !!staticScope;
    }

    bool canBeBlockScope() {
        return type == StmtType::BLOCK ||
               type == StmtType::SWITCH ||
               type == StmtType::TRY ||
               type == StmtType::FINALLY ||
               type == StmtType::CATCH;
    }

    StaticBlockObject& staticBlock() const {
        MOZ_ASSERT(staticScope);
        MOZ_ASSERT(isBlockScope);
        return staticScope->as<StaticBlockObject>();
    }

    bool isLoop() const {
        return type >= StmtType::DO_LOOP;
    }

    bool isTrying() const {
        return StmtType::TRY <= type && type <= StmtType::SUBROUTINE;
    }
};

template <class StmtInfo>
class MOZ_STACK_CLASS StmtInfoStack
{
    // Top of the stack.
    StmtInfo* innermostStmt_;

    // Top scope statement with a nested scope.
    StmtInfo* innermostScopeStmt_;

  public:
    explicit StmtInfoStack(ExclusiveContext* cx)
      : innermostStmt_(nullptr),
        innermostScopeStmt_(nullptr)
    { }

    StmtInfo* innermost() const { return innermostStmt_; }
    StmtInfo* innermostScopeStmt() const { return innermostScopeStmt_; }

    void push(StmtInfo* stmt, StmtType type) {
        stmt->type = type;
        stmt->isBlockScope = false;
        stmt->isForLetBlock = false;
        stmt->label = nullptr;
        stmt->staticScope = nullptr;
        stmt->enclosing = innermostStmt_;
        stmt->enclosingScope = nullptr;
        innermostStmt_ = stmt;
    }

    void pushNestedScope(StmtInfo* stmt, StmtType type, NestedScopeObject& staticScope) {
        push(stmt, type);
        linkAsInnermostScopeStmt(stmt, staticScope);
    }

    void pop() {
        StmtInfo* stmt = innermostStmt_;
        innermostStmt_ = stmt->enclosing;
        if (stmt->linksScope())
            innermostScopeStmt_ = stmt->enclosingScope;
    }

    void linkAsInnermostScopeStmt(StmtInfo* stmt, NestedScopeObject& staticScope) {
        MOZ_ASSERT(stmt != innermostScopeStmt_);
        MOZ_ASSERT(!stmt->enclosingScope);
        stmt->enclosingScope = innermostScopeStmt_;
        innermostScopeStmt_ = stmt;
        stmt->staticScope = &staticScope;
    }

    void makeInnermostLexicalScope(StaticBlockObject& blockObj) {
        MOZ_ASSERT(!innermostStmt_->isBlockScope);
        MOZ_ASSERT(innermostStmt_->canBeBlockScope());
        innermostStmt_->isBlockScope = true;
        linkAsInnermostScopeStmt(innermostStmt_, blockObj);
    }
};

} // namespace frontend

} // namespace js

#endif /* frontend_SharedContext_h */
