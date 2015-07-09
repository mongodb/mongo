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

  public:
    FunctionContextFlags()
     :  mightAliasLocals(false),
        hasExtensibleScope(false),
        needsDeclEnvObject(false),
        argumentsHasLocalBinding(false),
        definitelyNeedsArgsObj(false)
    { }
};

class GlobalSharedContext;

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
    bool strict;
    bool extraWarnings;

    // If it's function code, funbox must be non-nullptr and scopeChain must be
    // nullptr. If it's global code, funbox must be nullptr.
    SharedContext(ExclusiveContext* cx, Directives directives, bool extraWarnings)
      : context(cx),
        anyCxFlags(),
        strict(directives.strict()),
        extraWarnings(extraWarnings)
    {}

    virtual ObjectBox* toObjectBox() = 0;
    inline bool isGlobalSharedContext() { return toObjectBox() == nullptr; }
    inline bool isFunctionBox() { return toObjectBox() && toObjectBox()->isFunctionBox(); }
    inline GlobalSharedContext* asGlobalSharedContext();
    inline FunctionBox* asFunctionBox();

    bool hasExplicitUseStrict()        const { return anyCxFlags.hasExplicitUseStrict; }
    bool bindingsAccessedDynamically() const { return anyCxFlags.bindingsAccessedDynamically; }
    bool hasDebuggerStatement()        const { return anyCxFlags.hasDebuggerStatement; }
    bool hasDirectEval()               const { return anyCxFlags.hasDirectEval; }

    void setExplicitUseStrict()           { anyCxFlags.hasExplicitUseStrict        = true; }
    void setBindingsAccessedDynamically() { anyCxFlags.bindingsAccessedDynamically = true; }
    void setHasDebuggerStatement()        { anyCxFlags.hasDebuggerStatement        = true; }
    void setHasDirectEval()               { anyCxFlags.hasDirectEval               = true; }

    inline bool allLocalsAliased();

    // JSOPTION_EXTRA_WARNINGS warnings or strict mode errors.
    bool needStrictChecks() {
        return strict || extraWarnings;
    }

    bool isDotVariable(JSAtom* atom) const {
        return atom == context->names().dotGenerator || atom == context->names().dotGenRVal;
    }
};

class GlobalSharedContext : public SharedContext
{
  private:
    const RootedObject scopeChain_; /* scope chain object for the script */

  public:
    GlobalSharedContext(ExclusiveContext* cx, JSObject* scopeChain,
                        Directives directives, bool extraWarnings)
      : SharedContext(cx, directives, extraWarnings),
        scopeChain_(cx, scopeChain)
    {}

    ObjectBox* toObjectBox() { return nullptr; }
    JSObject* scopeChain() const { return scopeChain_; }
};

inline GlobalSharedContext*
SharedContext::asGlobalSharedContext()
{
    MOZ_ASSERT(isGlobalSharedContext());
    return static_cast<GlobalSharedContext*>(this);
}

class FunctionBox : public ObjectBox, public SharedContext
{
  public:
    Bindings        bindings;               /* bindings for this function */
    uint32_t        bufStart;
    uint32_t        bufEnd;
    uint32_t        startLine;
    uint32_t        startColumn;
    uint16_t        length;

    uint8_t         generatorKindBits_;     /* The GeneratorKind of this function. */
    bool            inWith:1;               /* some enclosing scope is a with-statement */
    bool            inGenexpLambda:1;       /* lambda from generator expression */
    bool            hasDestructuringArgs:1; /* arguments list contains destructuring expression */
    bool            useAsm:1;               /* see useAsmOrInsideUseAsm */
    bool            insideUseAsm:1;         /* see useAsmOrInsideUseAsm */

    // Fields for use in heuristics.
    bool            usesArguments:1;  /* contains a free use of 'arguments' */
    bool            usesApply:1;      /* contains an f.apply() call */
    bool            usesThis:1;       /* contains 'this' */

    FunctionContextFlags funCxFlags;

    template <typename ParseHandler>
    FunctionBox(ExclusiveContext* cx, ObjectBox* traceListHead, JSFunction* fun,
                ParseContext<ParseHandler>* pc, Directives directives,
                bool extraWarnings, GeneratorKind generatorKind);

    ObjectBox* toObjectBox() { return this; }
    JSFunction* function() const { return &object->as<JSFunction>(); }

    GeneratorKind generatorKind() const { return GeneratorKindFromBits(generatorKindBits_); }
    bool isGenerator() const { return generatorKind() != NotGenerator; }
    bool isLegacyGenerator() const { return generatorKind() == LegacyGenerator; }
    bool isStarGenerator() const { return generatorKind() == StarGenerator; }

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
    bool argumentsHasLocalBinding() const { return funCxFlags.argumentsHasLocalBinding; }
    bool definitelyNeedsArgsObj()   const { return funCxFlags.definitelyNeedsArgsObj; }

    void setMightAliasLocals()             { funCxFlags.mightAliasLocals         = true; }
    void setHasExtensibleScope()           { funCxFlags.hasExtensibleScope       = true; }
    void setNeedsDeclEnvObject()           { funCxFlags.needsDeclEnvObject       = true; }
    void setArgumentsHasLocalBinding()     { funCxFlags.argumentsHasLocalBinding = true; }
    void setDefinitelyNeedsArgsObj()       { MOZ_ASSERT(funCxFlags.argumentsHasLocalBinding);
                                             funCxFlags.definitelyNeedsArgsObj   = true; }

    bool hasDefaults() const {
        return length != function()->nargs() - function()->hasRest();
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

    bool isHeavyweight()
    {
        // Note: this should be kept in sync with JSFunction::isHeavyweight().
        return bindings.hasAnyAliasedBindings() ||
               hasExtensibleScope() ||
               needsDeclEnvObject() ||
               isGenerator();
    }
};

inline FunctionBox*
SharedContext::asFunctionBox()
{
    MOZ_ASSERT(isFunctionBox());
    return static_cast<FunctionBox*>(this);
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
enum StmtType {
    STMT_LABEL,                 /* labeled statement:  L: s */
    STMT_IF,                    /* if (then) statement */
    STMT_ELSE,                  /* else clause of if statement */
    STMT_SEQ,                   /* synthetic sequence of statements */
    STMT_BLOCK,                 /* compound statement: { s1[;... sN] } */
    STMT_SWITCH,                /* switch statement */
    STMT_WITH,                  /* with statement */
    STMT_CATCH,                 /* catch block */
    STMT_TRY,                   /* try block */
    STMT_FINALLY,               /* finally block */
    STMT_SUBROUTINE,            /* gosub-target subroutine body */
    STMT_DO_LOOP,               /* do/while loop statement */
    STMT_FOR_LOOP,              /* for loop statement */
    STMT_FOR_IN_LOOP,           /* for/in loop statement */
    STMT_FOR_OF_LOOP,           /* for/of loop statement */
    STMT_WHILE_LOOP,            /* while loop statement */
    STMT_SPREAD,                /* spread operator (pseudo for/of) */
    STMT_LIMIT
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
// StmtInfoBase.  Several functions below (e.g. PushStatement) are templated to
// work with both types.

struct StmtInfoBase {
    // Statement type (StmtType).
    uint16_t        type;

    // True if type is STMT_BLOCK, STMT_TRY, STMT_SWITCH, or STMT_FINALLY and
    // the block contains at least one let-declaration, or if type is
    // STMT_CATCH.
    bool isBlockScope:1;

    // True if isBlockScope or type == STMT_WITH.
    bool isNestedScope:1;

    // for (let ...) induced block scope
    bool isForLetBlock:1;

    // Block label.
    RootedAtom      label;

    // Compile-time scope chain node for this scope.  Only set if
    // isNestedScope.
    Rooted<NestedScopeObject*> staticScope;

    explicit StmtInfoBase(ExclusiveContext* cx)
        : isBlockScope(false), isNestedScope(false), isForLetBlock(false),
          label(cx), staticScope(cx)
    {}

    bool maybeScope() const {
        return STMT_BLOCK <= type && type <= STMT_SUBROUTINE && type != STMT_WITH;
    }

    bool linksScope() const {
        return isNestedScope;
    }

    void setStaticScope() {
    }

    StaticBlockObject& staticBlock() const {
        MOZ_ASSERT(isNestedScope);
        MOZ_ASSERT(isBlockScope);
        return staticScope->as<StaticBlockObject>();
    }

    bool isLoop() const {
        return type >= STMT_DO_LOOP;
    }

    bool isTrying() const {
        return STMT_TRY <= type && type <= STMT_SUBROUTINE;
    }
};

// Push the C-stack-allocated struct at stmt onto the StmtInfoPC stack.
template <class ContextT>
void
PushStatement(ContextT* ct, typename ContextT::StmtInfo* stmt, StmtType type)
{
    stmt->type = type;
    stmt->isBlockScope = false;
    stmt->isNestedScope = false;
    stmt->isForLetBlock = false;
    stmt->label = nullptr;
    stmt->staticScope = nullptr;
    stmt->down = ct->topStmt;
    ct->topStmt = stmt;
    if (stmt->linksScope()) {
        stmt->downScope = ct->topScopeStmt;
        ct->topScopeStmt = stmt;
    } else {
        stmt->downScope = nullptr;
    }
}

template <class ContextT>
void
FinishPushNestedScope(ContextT* ct, typename ContextT::StmtInfo* stmt, NestedScopeObject& staticScope)
{
    stmt->isNestedScope = true;
    stmt->downScope = ct->topScopeStmt;
    ct->topScopeStmt = stmt;
    ct->staticScope = &staticScope;
    stmt->staticScope = &staticScope;
}

// Pop pc->topStmt. If the top StmtInfoPC struct is not stack-allocated, it
// is up to the caller to free it.  The dummy argument is just to make the
// template matching work.
template <class ContextT>
void
FinishPopStatement(ContextT* ct)
{
    typename ContextT::StmtInfo* stmt = ct->topStmt;
    ct->topStmt = stmt->down;
    if (stmt->linksScope()) {
        ct->topScopeStmt = stmt->downScope;
        if (stmt->isNestedScope) {
            MOZ_ASSERT(stmt->staticScope);
            ct->staticScope = stmt->staticScope->enclosingNestedScope();
        }
    }
}

/*
 * Find a lexically scoped variable (one declared by let, catch, or an array
 * comprehension) named by atom, looking in sc's compile-time scopes.
 *
 * If a WITH statement is reached along the scope stack, return its statement
 * info record, so callers can tell that atom is ambiguous. If slotp is not
 * null, then if atom is found, set *slotp to its stack slot, otherwise to -1.
 * This means that if slotp is not null, all the block objects on the lexical
 * scope chain must have had their depth slots computed by the code generator,
 * so the caller must be under EmitTree.
 *
 * In any event, directly return the statement info record in which atom was
 * found. Otherwise return null.
 */
template <class ContextT>
typename ContextT::StmtInfo*
LexicalLookup(ContextT* ct, HandleAtom atom, int* slotp, typename ContextT::StmtInfo* stmt);

} // namespace frontend

} // namespace js

#endif /* frontend_SharedContext_h */
