/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SharedContext_h
#define frontend_SharedContext_h

#include "jspubtd.h"
#include "jstypes.h"

#include "builtin/ModuleObject.h"
#include "ds/InlineTable.h"
#include "frontend/ParseNode.h"
#include "frontend/TokenStream.h"
#include "vm/BytecodeUtil.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSAtom.h"
#include "vm/JSScript.h"

namespace js {
namespace frontend {

class ParseContext;
class ParseNode;

enum class StatementKind : uint8_t
{
    Label,
    Block,
    If,
    Switch,
    With,
    Catch,
    Try,
    Finally,
    ForLoopLexicalHead,
    ForLoop,
    ForInLoop,
    ForOfLoop,
    DoLoop,
    WhileLoop,
    Class,

    // Used only by BytecodeEmitter.
    Spread
};

static inline bool
StatementKindIsLoop(StatementKind kind)
{
    return kind == StatementKind::ForLoop ||
           kind == StatementKind::ForInLoop ||
           kind == StatementKind::ForOfLoop ||
           kind == StatementKind::DoLoop ||
           kind == StatementKind::WhileLoop ||
           kind == StatementKind::Spread;
}

static inline bool
StatementKindIsUnlabeledBreakTarget(StatementKind kind)
{
    return StatementKindIsLoop(kind) || kind == StatementKind::Switch;
}

// List of directives that may be encountered in a Directive Prologue (ES5 15.1).
class Directives
{
    bool strict_;
    bool asmJS_;

  public:
    explicit Directives(bool strict) : strict_(strict), asmJS_(false) {}
    explicit Directives(ParseContext* parent);

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
// have a lexical this-binding so their ThisBinding is the same as the
// ThisBinding of their enclosing scope and can be any value.
enum class ThisBinding : uint8_t { Global, Function, Module };

class GlobalSharedContext;
class EvalSharedContext;
class ModuleSharedContext;

/*
 * The struct SharedContext is part of the current parser context (see
 * ParseContext). It stores information that is reused between the parser and
 * the bytecode emitter.
 */
class SharedContext
{
  public:
    JSContext* const context;

  protected:
    enum class Kind : uint8_t {
        FunctionBox,
        Global,
        Eval,
        Module
    };

    Kind kind_;

    ThisBinding thisBinding_;

  public:
    bool strictScript:1;
    bool localStrict:1;
    bool extraWarnings:1;

  protected:
    bool allowNewTarget_:1;
    bool allowSuperProperty_:1;
    bool allowSuperCall_:1;
    bool inWith_:1;
    bool needsThisTDZChecks_:1;

    // True if "use strict"; appears in the body instead of being inherited.
    bool hasExplicitUseStrict_:1;

    // The (static) bindings of this script need to support dynamic name
    // read/write access. Here, 'dynamic' means dynamic dictionary lookup on
    // the scope chain for a dynamic set of keys. The primary examples are:
    //  - direct eval
    //  - function::
    //  - with
    // since both effectively allow any name to be accessed. Non-examples are:
    //  - upvars of nested functions
    //  - function statement
    // since the set of assigned name is known dynamically.
    //
    // Note: access through the arguments object is not considered dynamic
    // binding access since it does not go through the normal name lookup
    // mechanism. This is debatable and could be changed (although care must be
    // taken not to turn off the whole 'arguments' optimization). To answer the
    // more general "is this argument aliased" question, script->needsArgsObj
    // should be tested (see JSScript::argIsAliased).
    bool bindingsAccessedDynamically_:1;

    // Whether this script, or any of its inner scripts contains a debugger
    // statement which could potentially read or write anywhere along the
    // scope chain.
    bool hasDebuggerStatement_:1;

    // A direct eval occurs in the body of the script.
    bool hasDirectEval_:1;

    void computeAllowSyntax(Scope* scope);
    void computeInWith(Scope* scope);
    void computeThisBinding(Scope* scope);

  public:
    SharedContext(JSContext* cx, Kind kind, Directives directives, bool extraWarnings)
      : context(cx),
        kind_(kind),
        thisBinding_(ThisBinding::Global),
        strictScript(directives.strict()),
        localStrict(false),
        extraWarnings(extraWarnings),
        allowNewTarget_(false),
        allowSuperProperty_(false),
        allowSuperCall_(false),
        inWith_(false),
        needsThisTDZChecks_(false),
        hasExplicitUseStrict_(false),
        bindingsAccessedDynamically_(false),
        hasDebuggerStatement_(false),
        hasDirectEval_(false)
    { }

    // If this is the outermost SharedContext, the Scope that encloses
    // it. Otherwise nullptr.
    virtual Scope* compilationEnclosingScope() const = 0;

    bool isFunctionBox() const { return kind_ == Kind::FunctionBox; }
    inline FunctionBox* asFunctionBox();
    bool isModuleContext() const { return kind_ == Kind::Module; }
    inline ModuleSharedContext* asModuleContext();
    bool isGlobalContext() const { return kind_ == Kind::Global; }
    inline GlobalSharedContext* asGlobalContext();
    bool isEvalContext() const { return kind_ == Kind::Eval; }
    inline EvalSharedContext* asEvalContext();

    ThisBinding thisBinding()          const { return thisBinding_; }

    bool allowNewTarget()              const { return allowNewTarget_; }
    bool allowSuperProperty()          const { return allowSuperProperty_; }
    bool allowSuperCall()              const { return allowSuperCall_; }
    bool inWith()                      const { return inWith_; }
    bool needsThisTDZChecks()          const { return needsThisTDZChecks_; }

    bool hasExplicitUseStrict()        const { return hasExplicitUseStrict_; }
    bool bindingsAccessedDynamically() const { return bindingsAccessedDynamically_; }
    bool hasDebuggerStatement()        const { return hasDebuggerStatement_; }
    bool hasDirectEval()               const { return hasDirectEval_; }

    void setExplicitUseStrict()           { hasExplicitUseStrict_        = true; }
    void setBindingsAccessedDynamically() { bindingsAccessedDynamically_ = true; }
    void setHasDebuggerStatement()        { hasDebuggerStatement_        = true; }
    void setHasDirectEval()               { hasDirectEval_               = true; }

    inline bool allBindingsClosedOver();

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
    ScopeKind scopeKind_;

  public:
    Rooted<GlobalScope::Data*> bindings;

    GlobalSharedContext(JSContext* cx, ScopeKind scopeKind, Directives directives,
                        bool extraWarnings)
      : SharedContext(cx, Kind::Global, directives, extraWarnings),
        scopeKind_(scopeKind),
        bindings(cx)
    {
        MOZ_ASSERT(scopeKind == ScopeKind::Global || scopeKind == ScopeKind::NonSyntactic);
        thisBinding_ = ThisBinding::Global;
    }

    Scope* compilationEnclosingScope() const override {
        return nullptr;
    }

    ScopeKind scopeKind() const {
        return scopeKind_;
    }
};

inline GlobalSharedContext*
SharedContext::asGlobalContext()
{
    MOZ_ASSERT(isGlobalContext());
    return static_cast<GlobalSharedContext*>(this);
}

class MOZ_STACK_CLASS EvalSharedContext : public SharedContext
{
    RootedScope enclosingScope_;

  public:
    Rooted<EvalScope::Data*> bindings;

    EvalSharedContext(JSContext* cx, JSObject* enclosingEnv, Scope* enclosingScope,
                      Directives directives, bool extraWarnings);

    Scope* compilationEnclosingScope() const override {
        return enclosingScope_;
    }
};

inline EvalSharedContext*
SharedContext::asEvalContext()
{
    MOZ_ASSERT(isEvalContext());
    return static_cast<EvalSharedContext*>(this);
}

class FunctionBox : public ObjectBox, public SharedContext
{
    // The parser handles tracing the fields below via the ObjectBox linked
    // list.

    Scope* enclosingScope_;

    // Names from the named lambda scope, if a named lambda.
    LexicalScope::Data* namedLambdaBindings_;

    // Names from the function scope.
    FunctionScope::Data* functionScopeBindings_;

    // Names from the extra 'var' scope of the function, if the parameter list
    // has expressions.
    VarScope::Data* extraVarScopeBindings_;

    void initWithEnclosingScope(Scope* enclosingScope);

  public:
    ParseNode*      functionNode;           /* back pointer used by asm.js for error messages */
    uint32_t        bufStart;
    uint32_t        bufEnd;
    uint32_t        startLine;
    uint32_t        startColumn;
    uint32_t        toStringStart;
    uint32_t        toStringEnd;
    uint16_t        length;

    bool            isGenerator_:1;         /* generator function or async generator */
    bool            isAsync_:1;             /* async function or async generator */
    bool            hasDestructuringArgs:1; /* parameter list contains destructuring expression */
    bool            hasParameterExprs:1;    /* parameter list contains expressions */
    bool            hasDirectEvalInParameterExpr:1; /* parameter list contains direct eval */
    bool            hasDuplicateParameters:1; /* parameter list contains duplicate names */
    bool            useAsm:1;               /* see useAsmOrInsideUseAsm */
    bool            isAnnexB:1;             /* need to emit a synthesized Annex B assignment */
    bool            wasEmitted:1;           /* Bytecode has been emitted for this function. */

    // Fields for use in heuristics.
    bool            declaredArguments:1;    /* the Parser declared 'arguments' */
    bool            usesArguments:1;        /* contains a free use of 'arguments' */
    bool            usesApply:1;            /* contains an f.apply() call */
    bool            usesThis:1;             /* contains 'this' */
    bool            usesReturn:1;           /* contains a 'return' statement */
    bool            hasRest_:1;             /* has rest parameter */
    bool            isExprBody_:1;          /* arrow function with expression
                                             * body or expression closure:
                                             * function(x) x*x */

    // This function does something that can extend the set of bindings in its
    // call objects --- it does a direct eval in non-strict code, or includes a
    // function statement (as opposed to a function definition).
    //
    // This flag is *not* inherited by enclosed or enclosing functions; it
    // applies only to the function in whose flags it appears.
    //
    bool hasExtensibleScope_:1;

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
    bool argumentsHasLocalBinding_:1;

    // In many cases where 'arguments' has a local binding (as described above)
    // we do not need to actually create an arguments object in the function
    // prologue: instead we can analyze how 'arguments' is used (using the
    // simple dataflow analysis in analyzeSSA) to determine that uses of
    // 'arguments' can just read from the stack frame directly. However, the
    // dataflow analysis only looks at how JSOP_ARGUMENTS is used, so it will
    // be unsound in several cases. The frontend filters out such cases by
    // setting this flag which eagerly sets script->needsArgsObj to true.
    //
    bool definitelyNeedsArgsObj_:1;

    bool needsHomeObject_:1;
    bool isDerivedClassConstructor_:1;

    // Whether this function has a .this binding. If true, we need to emit
    // JSOP_FUNCTIONTHIS in the prologue to initialize it.
    bool hasThisBinding_:1;

    // Whether this function has nested functions.
    bool hasInnerFunctions_:1;

    FunctionBox(JSContext* cx, ObjectBox* traceListHead, JSFunction* fun,
                uint32_t toStringStart, Directives directives, bool extraWarnings,
                GeneratorKind generatorKind, FunctionAsyncKind asyncKind);

    MutableHandle<LexicalScope::Data*> namedLambdaBindings() {
        MOZ_ASSERT(context->keepAtoms);
        return MutableHandle<LexicalScope::Data*>::fromMarkedLocation(&namedLambdaBindings_);
    }

    MutableHandle<FunctionScope::Data*> functionScopeBindings() {
        MOZ_ASSERT(context->keepAtoms);
        return MutableHandle<FunctionScope::Data*>::fromMarkedLocation(&functionScopeBindings_);
    }

    MutableHandle<VarScope::Data*> extraVarScopeBindings() {
        MOZ_ASSERT(context->keepAtoms);
        return MutableHandle<VarScope::Data*>::fromMarkedLocation(&extraVarScopeBindings_);
    }

    void initFromLazyFunction();
    void initStandaloneFunction(Scope* enclosingScope);
    void initWithEnclosingParseContext(ParseContext* enclosing, FunctionSyntaxKind kind);

    JSFunction* function() const { return &object->as<JSFunction>(); }

    Scope* compilationEnclosingScope() const override {
        // This method is used to distinguish the outermost SharedContext. If
        // a FunctionBox is the outermost SharedContext, it must be a lazy
        // function.
        MOZ_ASSERT_IF(function()->isInterpretedLazy(),
                      enclosingScope_ == function()->lazyScript()->enclosingScope());
        return enclosingScope_;
    }

    bool needsCallObjectRegardlessOfBindings() const {
        return hasExtensibleScope() ||
               needsHomeObject() ||
               isDerivedClassConstructor() ||
               isGenerator() ||
               isAsync();
    }

    bool hasExtraBodyVarScope() const {
        return hasParameterExprs &&
               (extraVarScopeBindings_ ||
                needsExtraBodyVarEnvironmentRegardlessOfBindings());
    }

    bool needsExtraBodyVarEnvironmentRegardlessOfBindings() const {
        MOZ_ASSERT(hasParameterExprs);
        return hasExtensibleScope() || needsDotGeneratorName();
    }

    bool isLikelyConstructorWrapper() const {
        return usesArguments && usesApply && usesThis && !usesReturn;
    }

    bool isGenerator() const { return isGenerator_; }
    GeneratorKind generatorKind() const {
        return isGenerator() ? GeneratorKind::Generator : GeneratorKind::NotGenerator;
    }

    bool isAsync() const { return isAsync_; }
    FunctionAsyncKind asyncKind() const {
        return isAsync() ? FunctionAsyncKind::AsyncFunction : FunctionAsyncKind::SyncFunction;
    }

    bool needsFinalYield() const {
        return isGenerator() || isAsync();
    }
    bool needsDotGeneratorName() const {
        return isGenerator() || isAsync();
    }
    bool needsIteratorResult() const {
        return isGenerator();
    }

    bool isArrow() const { return function()->isArrow(); }

    bool hasRest() const { return hasRest_; }
    void setHasRest() {
        hasRest_ = true;
    }

    bool isExprBody() const { return isExprBody_; }
    void setIsExprBody() {
        isExprBody_ = true;
    }

    bool hasExtensibleScope()        const { return hasExtensibleScope_; }
    bool hasThisBinding()            const { return hasThisBinding_; }
    bool argumentsHasLocalBinding()  const { return argumentsHasLocalBinding_; }
    bool definitelyNeedsArgsObj()    const { return definitelyNeedsArgsObj_; }
    bool needsHomeObject()           const { return needsHomeObject_; }
    bool isDerivedClassConstructor() const { return isDerivedClassConstructor_; }
    bool hasInnerFunctions()         const { return hasInnerFunctions_; }

    void setHasExtensibleScope()           { hasExtensibleScope_       = true; }
    void setHasThisBinding()               { hasThisBinding_           = true; }
    void setArgumentsHasLocalBinding()     { argumentsHasLocalBinding_ = true; }
    void setDefinitelyNeedsArgsObj()       { MOZ_ASSERT(argumentsHasLocalBinding_);
                                             definitelyNeedsArgsObj_   = true; }
    void setNeedsHomeObject()              { MOZ_ASSERT(function()->allowSuperProperty());
                                             needsHomeObject_          = true; }
    void setDerivedClassConstructor()      { MOZ_ASSERT(function()->isClassConstructor());
                                             isDerivedClassConstructor_ = true; }
    void setHasInnerFunctions()            { hasInnerFunctions_         = true; }

    bool hasSimpleParameterList() const {
        return !hasRest() && !hasParameterExprs && !hasDestructuringArgs;
    }

    bool hasMappedArgsObj() const {
        return !strict() && hasSimpleParameterList();
    }

    // Return whether this or an enclosing function is being parsed and
    // validated as asm.js. Note: if asm.js validation fails, this will be false
    // while the function is being reparsed. This flag can be used to disable
    // certain parsing features that are necessary in general, but unnecessary
    // for validated asm.js.
    bool useAsmOrInsideUseAsm() const {
        return useAsm;
    }

    void setStart(const TokenStreamAnyChars& anyChars) {
        uint32_t offset = anyChars.currentToken().pos.begin;
        setStart(anyChars, offset);
    }

    void setStart(const TokenStreamAnyChars& anyChars, uint32_t offset) {
        bufStart = offset;
        anyChars.srcCoords.lineNumAndColumnIndex(offset, &startLine, &startColumn);
    }

    void setEnd(const TokenStreamAnyChars& anyChars) {
        // For all functions except class constructors, the buffer and
        // toString ending positions are the same. Class constructors override
        // the toString ending position with the end of the class definition.
        uint32_t offset = anyChars.currentToken().pos.end;
        bufEnd = offset;
        toStringEnd = offset;
    }

    void trace(JSTracer* trc) override;
};

inline FunctionBox*
SharedContext::asFunctionBox()
{
    MOZ_ASSERT(isFunctionBox());
    return static_cast<FunctionBox*>(this);
}

class MOZ_STACK_CLASS ModuleSharedContext : public SharedContext
{
    RootedModuleObject module_;
    RootedScope enclosingScope_;

  public:
    Rooted<ModuleScope::Data*> bindings;
    ModuleBuilder& builder;

    ModuleSharedContext(JSContext* cx, ModuleObject* module, Scope* enclosingScope,
                        ModuleBuilder& builder);

    HandleModuleObject module() const { return module_; }
    Scope* compilationEnclosingScope() const override { return enclosingScope_; }
};

inline ModuleSharedContext*
SharedContext::asModuleContext()
{
    MOZ_ASSERT(isModuleContext());
    return static_cast<ModuleSharedContext*>(this);
}

// In generators, we treat all bindings as closed so that they get stored on
// the heap.  This way there is less information to copy off the stack when
// suspending, and back on when resuming.  It also avoids the need to create
// and invalidate DebugScope proxies for unaliased locals in a generator
// frame, as the generator frame will be copied out to the heap and released
// only by GC.
inline bool
SharedContext::allBindingsClosedOver()
{
    return bindingsAccessedDynamically() ||
           (isFunctionBox() &&
            (asFunctionBox()->isGenerator() ||
             asFunctionBox()->isAsync()));
}

} // namespace frontend
} // namespace js

#endif /* frontend_SharedContext_h */
