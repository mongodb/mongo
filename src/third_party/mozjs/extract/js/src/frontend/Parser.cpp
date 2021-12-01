/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS parser.
 *
 * This is a recursive-descent parser for the JavaScript language specified by
 * "The ECMAScript Language Specification" (Standard ECMA-262).  It uses
 * lexical and semantic feedback to disambiguate non-LL(1) structures.  It
 * generates trees of nodes induced by the recursive parsing (not precise
 * syntax trees, see Parser.h).  After tree construction, it rewrites trees to
 * fold constants and evaluate compile-time expressions.
 *
 * This parser attempts no error recovery.
 */

#include "frontend/Parser.h"

#include "mozilla/Range.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TypeTraits.h"

#include "jsapi.h"
#include "jstypes.h"

#include "builtin/ModuleObject.h"
#include "builtin/SelfHostingDefines.h"
#include "frontend/BytecodeCompiler.h"
#include "frontend/FoldConstants.h"
#include "frontend/TokenStream.h"
#include "irregexp/RegExpParser.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/RegExpObject.h"
#include "vm/StringType.h"
#include "wasm/AsmJS.h"

#include "frontend/ParseContext-inl.h"
#include "frontend/ParseNode-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::Maybe;
using mozilla::Move;
using mozilla::Nothing;
using mozilla::PodCopy;
using mozilla::PodZero;
using mozilla::Some;

using JS::AutoGCRooter;

namespace js {
namespace frontend {

using DeclaredNamePtr = ParseContext::Scope::DeclaredNamePtr;
using AddDeclaredNamePtr = ParseContext::Scope::AddDeclaredNamePtr;
using BindingIter = ParseContext::Scope::BindingIter;
using UsedNamePtr = UsedNameTracker::UsedNameMap::Ptr;

// Read a token. Report an error and return null() if that token doesn't match
// to the condition.  Do not use MUST_MATCH_TOKEN_INTERNAL directly.
#define MUST_MATCH_TOKEN_INTERNAL(cond, modifier, errorReport)                              \
    JS_BEGIN_MACRO                                                                          \
        TokenKind token;                                                                    \
        if (!tokenStream.getToken(&token, modifier))                                        \
            return null();                                                                  \
        if (!(cond)) {                                                                      \
            errorReport;                                                                    \
            return null();                                                                  \
        }                                                                                   \
    JS_END_MACRO

#define MUST_MATCH_TOKEN_MOD(tt, modifier, errorNumber) \
    MUST_MATCH_TOKEN_INTERNAL(token == tt, modifier, error(errorNumber))

#define MUST_MATCH_TOKEN(tt, errorNumber) \
    MUST_MATCH_TOKEN_MOD(tt, TokenStream::None, errorNumber)

#define MUST_MATCH_TOKEN_FUNC_MOD(func, modifier, errorNumber) \
    MUST_MATCH_TOKEN_INTERNAL((func)(token), modifier, error(errorNumber))

#define MUST_MATCH_TOKEN_FUNC(func, errorNumber) \
    MUST_MATCH_TOKEN_FUNC_MOD(func, TokenStream::None, errorNumber)

#define MUST_MATCH_TOKEN_MOD_WITH_REPORT(tt, modifier, errorReport) \
    MUST_MATCH_TOKEN_INTERNAL(token == tt, modifier, errorReport)

template <class T, class U>
static inline void
PropagateTransitiveParseFlags(const T* inner, U* outer)
{
    if (inner->bindingsAccessedDynamically())
        outer->setBindingsAccessedDynamically();
    if (inner->hasDebuggerStatement())
        outer->setHasDebuggerStatement();
    if (inner->hasDirectEval())
        outer->setHasDirectEval();
}

static const char*
DeclarationKindString(DeclarationKind kind)
{
    switch (kind) {
      case DeclarationKind::PositionalFormalParameter:
      case DeclarationKind::FormalParameter:
        return "formal parameter";
      case DeclarationKind::CoverArrowParameter:
        return "cover arrow parameter";
      case DeclarationKind::Var:
        return "var";
      case DeclarationKind::Let:
        return "let";
      case DeclarationKind::Const:
        return "const";
      case DeclarationKind::Class:
        return "class";
      case DeclarationKind::Import:
        return "import";
      case DeclarationKind::BodyLevelFunction:
      case DeclarationKind::ModuleBodyLevelFunction:
      case DeclarationKind::LexicalFunction:
      case DeclarationKind::SloppyLexicalFunction:
        return "function";
      case DeclarationKind::VarForAnnexBLexicalFunction:
        return "annex b var";
      case DeclarationKind::ForOfVar:
        return "var in for-of";
      case DeclarationKind::SimpleCatchParameter:
      case DeclarationKind::CatchParameter:
        return "catch parameter";
    }

    MOZ_CRASH("Bad DeclarationKind");
}

static bool
StatementKindIsBraced(StatementKind kind)
{
    return kind == StatementKind::Block ||
           kind == StatementKind::Switch ||
           kind == StatementKind::Try ||
           kind == StatementKind::Catch ||
           kind == StatementKind::Finally ||
           kind == StatementKind::Class;
}

void
ParseContext::Scope::dump(ParseContext* pc)
{
    JSContext* cx = pc->sc()->context;

    fprintf(stdout, "ParseScope %p", this);

    fprintf(stdout, "\n  decls:\n");
    for (DeclaredNameMap::Range r = declared_->all(); !r.empty(); r.popFront()) {
        JSAutoByteString bytes;
        if (!AtomToPrintableString(cx, r.front().key(), &bytes))
            return;
        DeclaredNameInfo& info = r.front().value().wrapped;
        fprintf(stdout, "    %s %s%s\n",
                DeclarationKindString(info.kind()),
                bytes.ptr(),
                info.closedOver() ? " (closed over)" : "");
    }

    fprintf(stdout, "\n");
}

bool
ParseContext::Scope::addPossibleAnnexBFunctionBox(ParseContext* pc, FunctionBox* funbox)
{
    if (!possibleAnnexBFunctionBoxes_) {
        if (!possibleAnnexBFunctionBoxes_.acquire(pc->sc()->context))
            return false;
    }

    return possibleAnnexBFunctionBoxes_->append(funbox);
}

bool
ParseContext::Scope::propagateAndMarkAnnexBFunctionBoxes(ParseContext* pc)
{
    // Strict mode doesn't have wack Annex B function semantics.
    if (pc->sc()->strict() ||
        !possibleAnnexBFunctionBoxes_ ||
        possibleAnnexBFunctionBoxes_->empty())
    {
        return true;
    }

    if (this == &pc->varScope()) {
        // Base case: actually declare the Annex B vars and mark applicable
        // function boxes as Annex B.
        RootedPropertyName name(pc->sc()->context);
        Maybe<DeclarationKind> redeclaredKind;
        uint32_t unused;
        for (FunctionBox* funbox : *possibleAnnexBFunctionBoxes_) {
            if (pc->annexBAppliesToLexicalFunctionInInnermostScope(funbox)) {
                name = funbox->function()->explicitName()->asPropertyName();
                if (!pc->tryDeclareVar(name,
                                       DeclarationKind::VarForAnnexBLexicalFunction,
                                       DeclaredNameInfo::npos, &redeclaredKind, &unused))
                {
                    return false;
                }

                MOZ_ASSERT(!redeclaredKind);
                funbox->isAnnexB = true;
            }
        }
    } else {
        // Inner scope case: propagate still applicable function boxes to the
        // enclosing scope.
        for (FunctionBox* funbox : *possibleAnnexBFunctionBoxes_) {
            if (pc->annexBAppliesToLexicalFunctionInInnermostScope(funbox)) {
                if (!enclosing()->addPossibleAnnexBFunctionBox(pc, funbox))
                    return false;
            }
        }
    }

    return true;
}

static bool
DeclarationKindIsCatchParameter(DeclarationKind kind)
{
    return kind == DeclarationKind::SimpleCatchParameter ||
           kind == DeclarationKind::CatchParameter;
}

bool
ParseContext::Scope::addCatchParameters(ParseContext* pc, Scope& catchParamScope)
{
    if (pc->useAsmOrInsideUseAsm())
        return true;

    for (DeclaredNameMap::Range r = catchParamScope.declared_->all(); !r.empty(); r.popFront()) {
        DeclarationKind kind = r.front().value()->kind();
        uint32_t pos = r.front().value()->pos();
        MOZ_ASSERT(DeclarationKindIsCatchParameter(kind));
        JSAtom* name = r.front().key();
        AddDeclaredNamePtr p = lookupDeclaredNameForAdd(name);
        MOZ_ASSERT(!p);
        if (!addDeclaredName(pc, p, name, kind, pos))
            return false;
    }

    return true;
}

void
ParseContext::Scope::removeCatchParameters(ParseContext* pc, Scope& catchParamScope)
{
    if (pc->useAsmOrInsideUseAsm())
        return;

    for (DeclaredNameMap::Range r = catchParamScope.declared_->all(); !r.empty(); r.popFront()) {
        DeclaredNamePtr p = declared_->lookup(r.front().key());
        MOZ_ASSERT(p);

        // This check is needed because the catch body could have declared
        // vars, which would have been added to catchParamScope.
        if (DeclarationKindIsCatchParameter(r.front().value()->kind()))
            declared_->remove(p);
    }
}

void
SharedContext::computeAllowSyntax(Scope* scope)
{
    for (ScopeIter si(scope); si; si++) {
        if (si.kind() == ScopeKind::Function) {
            JSFunction* fun = si.scope()->as<FunctionScope>().canonicalFunction();
            if (fun->isArrow())
                continue;
            allowNewTarget_ = true;
            allowSuperProperty_ = fun->allowSuperProperty();
            allowSuperCall_ = fun->isDerivedClassConstructor();
            return;
        }
    }
}

void
SharedContext::computeThisBinding(Scope* scope)
{
    for (ScopeIter si(scope); si; si++) {
        if (si.kind() == ScopeKind::Module) {
            thisBinding_ = ThisBinding::Module;
            return;
        }

        if (si.kind() == ScopeKind::Function) {
            JSFunction* fun = si.scope()->as<FunctionScope>().canonicalFunction();

            // Arrow functions don't have their own `this` binding.
            if (fun->isArrow())
                continue;

            // Derived class constructors (including nested arrow functions and
            // eval) need TDZ checks when accessing |this|.
            if (fun->isDerivedClassConstructor())
                needsThisTDZChecks_ = true;

            thisBinding_ = ThisBinding::Function;
            return;
        }
    }

    thisBinding_ = ThisBinding::Global;
}

void
SharedContext::computeInWith(Scope* scope)
{
    for (ScopeIter si(scope); si; si++) {
        if (si.kind() == ScopeKind::With) {
            inWith_ = true;
            break;
        }
    }
}

EvalSharedContext::EvalSharedContext(JSContext* cx, JSObject* enclosingEnv,
                                     Scope* enclosingScope, Directives directives,
                                     bool extraWarnings)
  : SharedContext(cx, Kind::Eval, directives, extraWarnings),
    enclosingScope_(cx, enclosingScope),
    bindings(cx)
{
    computeAllowSyntax(enclosingScope);
    computeInWith(enclosingScope);
    computeThisBinding(enclosingScope);

    // If this eval is in response to Debugger.Frame.eval, we may have been
    // passed an incomplete scope chain. In order to better determine the 'this'
    // binding type, we traverse the environment chain, looking for a CallObject
    // and recompute the binding type based on its body scope.
    //
    // NOTE: A non-debug eval in a non-syntactic environment will also trigger
    // this code. In that case, we should still compute the same binding type.
    if (enclosingEnv && enclosingScope->hasOnChain(ScopeKind::NonSyntactic)) {
        JSObject* env = enclosingEnv;
        while (env) {
            // Look at target of any DebugEnvironmentProxy, but be sure to use
            // enclosingEnvironment() of the proxy itself.
            JSObject* unwrapped = env;
            if (env->is<DebugEnvironmentProxy>())
                unwrapped = &env->as<DebugEnvironmentProxy>().environment();

            if (unwrapped->is<CallObject>()) {
                JSFunction* callee = &unwrapped->as<CallObject>().callee();
                computeThisBinding(callee->nonLazyScript()->bodyScope());
                break;
            }

            env = env->enclosingEnvironment();
        }
    }
}

bool
ParseContext::init()
{
    if (scriptId_ == UINT32_MAX) {
        errorReporter_.reportErrorNoOffset(JSMSG_NEED_DIET, js_script_str);
        return false;
    }

    JSContext* cx = sc()->context;

    if (isFunctionBox()) {
        // Named lambdas always need a binding for their own name. If this
        // binding is closed over when we finish parsing the function in
        // finishExtraFunctionScopes, the function box needs to be marked as
        // needing a dynamic DeclEnv object.
        RootedFunction fun(cx, functionBox()->function());
        if (fun->isNamedLambda()) {
            if (!namedLambdaScope_->init(this))
                return false;
            AddDeclaredNamePtr p =
                namedLambdaScope_->lookupDeclaredNameForAdd(fun->explicitName());
            MOZ_ASSERT(!p);
            if (!namedLambdaScope_->addDeclaredName(this, p, fun->explicitName(),
                                                    DeclarationKind::Const,
                                                    DeclaredNameInfo::npos))
            {
                return false;
            }
        }

        if (!functionScope_->init(this))
            return false;

        if (!positionalFormalParameterNames_.acquire(cx))
            return false;
    }

    if (!closedOverBindingsForLazy_.acquire(cx))
        return false;

    return true;
}

bool
UsedNameTracker::noteUse(JSContext* cx, JSAtom* name, uint32_t scriptId, uint32_t scopeId)
{
    if (UsedNameMap::AddPtr p = map_.lookupForAdd(name)) {
        if (!p->value().noteUsedInScope(scriptId, scopeId))
            return false;
    } else {
        UsedNameInfo info(cx);
        if (!info.noteUsedInScope(scriptId, scopeId))
            return false;
        if (!map_.add(p, name, Move(info)))
            return false;
    }

    return true;
}

void
UsedNameTracker::UsedNameInfo::resetToScope(uint32_t scriptId, uint32_t scopeId)
{
    while (!uses_.empty()) {
        Use& innermost = uses_.back();
        if (innermost.scopeId < scopeId)
            break;
        MOZ_ASSERT(innermost.scriptId >= scriptId);
        uses_.popBack();
    }
}

void
UsedNameTracker::rewind(RewindToken token)
{
    scriptCounter_ = token.scriptId;
    scopeCounter_ = token.scopeId;

    for (UsedNameMap::Range r = map_.all(); !r.empty(); r.popFront())
        r.front().value().resetToScope(token.scriptId, token.scopeId);
}

FunctionBox::FunctionBox(JSContext* cx, ObjectBox* traceListHead,
                         JSFunction* fun, uint32_t toStringStart,
                         Directives directives, bool extraWarnings,
                         GeneratorKind generatorKind, FunctionAsyncKind asyncKind)
  : ObjectBox(fun, traceListHead),
    SharedContext(cx, Kind::FunctionBox, directives, extraWarnings),
    enclosingScope_(nullptr),
    namedLambdaBindings_(nullptr),
    functionScopeBindings_(nullptr),
    extraVarScopeBindings_(nullptr),
    functionNode(nullptr),
    bufStart(0),
    bufEnd(0),
    startLine(1),
    startColumn(0),
    toStringStart(toStringStart),
    toStringEnd(0),
    length(0),
    isGenerator_(generatorKind == GeneratorKind::Generator),
    isAsync_(asyncKind == FunctionAsyncKind::AsyncFunction),
    hasDestructuringArgs(false),
    hasParameterExprs(false),
    hasDirectEvalInParameterExpr(false),
    hasDuplicateParameters(false),
    useAsm(false),
    isAnnexB(false),
    wasEmitted(false),
    declaredArguments(false),
    usesArguments(false),
    usesApply(false),
    usesThis(false),
    usesReturn(false),
    hasRest_(false),
    isExprBody_(false),
    hasExtensibleScope_(false),
    argumentsHasLocalBinding_(false),
    definitelyNeedsArgsObj_(false),
    needsHomeObject_(false),
    isDerivedClassConstructor_(false),
    hasThisBinding_(false),
    hasInnerFunctions_(false)
{
    // Functions created at parse time may be set singleton after parsing and
    // baked into JIT code, so they must be allocated tenured. They are held by
    // the JSScript so cannot be collected during a minor GC anyway.
    MOZ_ASSERT(fun->isTenured());
}

void
FunctionBox::initFromLazyFunction()
{
    JSFunction* fun = function();
    if (fun->lazyScript()->isDerivedClassConstructor())
        setDerivedClassConstructor();
    if (fun->lazyScript()->needsHomeObject())
        setNeedsHomeObject();
    enclosingScope_ = fun->lazyScript()->enclosingScope();
    initWithEnclosingScope(enclosingScope_);
}

void
FunctionBox::initStandaloneFunction(Scope* enclosingScope)
{
    // Standalone functions are Function or Generator constructors and are
    // always scoped to the global.
    MOZ_ASSERT(enclosingScope->is<GlobalScope>());
    enclosingScope_ = enclosingScope;
    allowNewTarget_ = true;
    thisBinding_ = ThisBinding::Function;
}

void
FunctionBox::initWithEnclosingParseContext(ParseContext* enclosing, FunctionSyntaxKind kind)
{
    SharedContext* sc = enclosing->sc();
    useAsm = sc->isFunctionBox() && sc->asFunctionBox()->useAsmOrInsideUseAsm();

    JSFunction* fun = function();

    // Arrow functions don't have their own `this` binding.
    if (fun->isArrow()) {
        allowNewTarget_ = sc->allowNewTarget();
        allowSuperProperty_ = sc->allowSuperProperty();
        allowSuperCall_ = sc->allowSuperCall();
        needsThisTDZChecks_ = sc->needsThisTDZChecks();
        thisBinding_ = sc->thisBinding();
    } else {
        allowNewTarget_ = true;
        allowSuperProperty_ = fun->allowSuperProperty();

        if (kind == ClassConstructor || kind == DerivedClassConstructor) {
            auto stmt = enclosing->findInnermostStatement<ParseContext::ClassStatement>();
            MOZ_ASSERT(stmt);
            stmt->constructorBox = this;

            if (kind == DerivedClassConstructor) {
                setDerivedClassConstructor();
                allowSuperCall_ = true;
                needsThisTDZChecks_ = true;
            }
        }

        thisBinding_ = ThisBinding::Function;
    }

    if (sc->inWith()) {
        inWith_ = true;
    } else {
        auto isWith = [](ParseContext::Statement* stmt) {
            return stmt->kind() == StatementKind::With;
        };

        inWith_ = enclosing->findInnermostStatement(isWith);
    }
}

void
FunctionBox::initWithEnclosingScope(Scope* enclosingScope)
{
    if (!function()->isArrow()) {
        allowNewTarget_ = true;
        allowSuperProperty_ = function()->allowSuperProperty();

        if (isDerivedClassConstructor()) {
            setDerivedClassConstructor();
            allowSuperCall_ = true;
            needsThisTDZChecks_ = true;
        }

        thisBinding_ = ThisBinding::Function;
    } else {
        computeAllowSyntax(enclosingScope);
        computeThisBinding(enclosingScope);
    }

    computeInWith(enclosingScope);
}

template <class ParseHandler, typename CharT>
inline typename GeneralParser<ParseHandler, CharT>::FinalParser*
GeneralParser<ParseHandler, CharT>::asFinalParser()
{
    static_assert(mozilla::IsBaseOf<GeneralParser<ParseHandler, CharT>, FinalParser>::value,
                  "inheritance relationship required by the static_cast<> below");

    return static_cast<FinalParser*>(this);
}

template <class ParseHandler, typename CharT>
inline const typename GeneralParser<ParseHandler, CharT>::FinalParser*
GeneralParser<ParseHandler, CharT>::asFinalParser() const
{
    static_assert(mozilla::IsBaseOf<GeneralParser<ParseHandler, CharT>, FinalParser>::value,
                  "inheritance relationship required by the static_cast<> below");

    return static_cast<const FinalParser*>(this);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::error(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    if (tokenStream.computeErrorMetadata(&metadata, pos().begin))
        ReportCompileError(context, Move(metadata), nullptr, JSREPORT_ERROR, errorNumber, args);

    va_end(args);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::errorWithNotes(UniquePtr<JSErrorNotes> notes,
                                                   unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    if (tokenStream.computeErrorMetadata(&metadata, pos().begin)) {
        ReportCompileError(context, Move(metadata), Move(notes), JSREPORT_ERROR, errorNumber,
                           args);
    }

    va_end(args);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::errorAt(uint32_t offset, unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    if (tokenStream.computeErrorMetadata(&metadata, offset))
        ReportCompileError(context, Move(metadata), nullptr, JSREPORT_ERROR, errorNumber, args);

    va_end(args);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::errorWithNotesAt(UniquePtr<JSErrorNotes> notes,
                                                     uint32_t offset, unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    if (tokenStream.computeErrorMetadata(&metadata, offset)) {
        ReportCompileError(context, Move(metadata), Move(notes), JSREPORT_ERROR, errorNumber,
                           args);
    }

    va_end(args);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::warning(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    bool result =
        tokenStream.computeErrorMetadata(&metadata, pos().begin) &&
        anyChars.compileWarning(Move(metadata), nullptr, JSREPORT_WARNING, errorNumber, args);

    va_end(args);
    return result;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::warningAt(uint32_t offset, unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    bool result = tokenStream.computeErrorMetadata(&metadata, offset);
    if (result) {
        result =
            anyChars.compileWarning(Move(metadata), nullptr, JSREPORT_WARNING, errorNumber, args);
    }

    va_end(args);
    return result;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::extraWarning(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    bool result =
        tokenStream.reportExtraWarningErrorNumberVA(nullptr, pos().begin, errorNumber, &args);

    va_end(args);
    return result;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::extraWarningAt(uint32_t offset, unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    bool result =
        tokenStream.reportExtraWarningErrorNumberVA(nullptr, offset, errorNumber, &args);

    va_end(args);
    return result;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::strictModeError(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    bool res =
        tokenStream.reportStrictModeErrorNumberVA(nullptr, pos().begin, pc->sc()->strict(),
                                                  errorNumber, &args);

    va_end(args);
    return res;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::strictModeErrorAt(uint32_t offset, unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    bool res =
        tokenStream.reportStrictModeErrorNumberVA(nullptr, offset, pc->sc()->strict(),
                                                  errorNumber, &args);

    va_end(args);
    return res;
}

bool
ParserBase::warningNoOffset(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    anyChars.computeErrorMetadataNoOffset(&metadata);

    bool result =
        anyChars.compileWarning(Move(metadata), nullptr, JSREPORT_WARNING, errorNumber, args);

    va_end(args);
    return result;
}

void
ParserBase::errorNoOffset(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    anyChars.computeErrorMetadataNoOffset(&metadata);

    ReportCompileError(context, Move(metadata), nullptr, JSREPORT_ERROR, errorNumber, args);

    va_end(args);
}

ParserBase::ParserBase(JSContext* cx, LifoAlloc& alloc,
                       const ReadOnlyCompileOptions& options,
                       bool foldConstants,
                       UsedNameTracker& usedNames)
  : AutoGCRooter(cx, PARSER),
    context(cx),
    alloc(alloc),
    anyChars(cx, options, thisForCtor()),
    traceListHead(nullptr),
    pc(nullptr),
    usedNames(usedNames),
    ss(nullptr),
    keepAtoms(cx),
    foldConstants(foldConstants),
#ifdef DEBUG
    checkOptionsCalled(false),
#endif
    isUnexpectedEOF_(false),
    awaitHandling_(AwaitIsName)
{
    cx->frontendCollectionPool().addActiveCompilation();
    tempPoolMark = alloc.mark();
}

bool
ParserBase::checkOptions()
{
#ifdef DEBUG
    checkOptionsCalled = true;
#endif

    return anyChars.checkOptions();
}

ParserBase::~ParserBase()
{
    MOZ_ASSERT(checkOptionsCalled);

    alloc.release(tempPoolMark);

    /*
     * The parser can allocate enormous amounts of memory for large functions.
     * Eagerly free the memory now (which otherwise won't be freed until the
     * next GC) to avoid unnecessary OOMs.
     */
    alloc.freeAllIfHugeAndUnused();

    context->frontendCollectionPool().removeActiveCompilation();
}

template <class ParseHandler>
PerHandlerParser<ParseHandler>::PerHandlerParser(JSContext* cx, LifoAlloc& alloc,
                                                 const ReadOnlyCompileOptions& options,
                                                 bool foldConstants, UsedNameTracker& usedNames,
                                                 LazyScript* lazyOuterFunction)
  : ParserBase(cx, alloc, options, foldConstants, usedNames),
    handler(cx, alloc, lazyOuterFunction)
{

}

template <class ParseHandler, typename CharT>
GeneralParser<ParseHandler, CharT>::GeneralParser(JSContext* cx, LifoAlloc& alloc,
                                                  const ReadOnlyCompileOptions& options,
                                                  const CharT* chars, size_t length,
                                                  bool foldConstants,
                                                  UsedNameTracker& usedNames,
                                                  SyntaxParser* syntaxParser,
                                                  LazyScript* lazyOuterFunction)
  : Base(cx, alloc, options, foldConstants, usedNames, lazyOuterFunction),
    tokenStream(cx, options, chars, length)
{
    // The Mozilla specific JSOPTION_EXTRA_WARNINGS option adds extra warnings
    // which are not generated if functions are parsed lazily. Note that the
    // standard "use strict" does not inhibit lazy parsing.
    if (options.extraWarningsOption)
        disableSyntaxParser();
    else
        setSyntaxParser(syntaxParser);
}

template <typename CharT>
void
Parser<SyntaxParseHandler, CharT>::setAwaitHandling(AwaitHandling awaitHandling)
{
    this->awaitHandling_ = awaitHandling;
}

template <typename CharT>
void
Parser<FullParseHandler, CharT>::setAwaitHandling(AwaitHandling awaitHandling)
{
    this->awaitHandling_ = awaitHandling;
    if (SyntaxParser* syntaxParser = getSyntaxParser())
        syntaxParser->setAwaitHandling(awaitHandling);
}

template <class ParseHandler, typename CharT>
inline void
GeneralParser<ParseHandler, CharT>::setAwaitHandling(AwaitHandling awaitHandling)
{
    asFinalParser()->setAwaitHandling(awaitHandling);
}

ObjectBox*
ParserBase::newObjectBox(JSObject* obj)
{
    MOZ_ASSERT(obj);

    /*
     * We use JSContext.tempLifoAlloc to allocate parsed objects and place them
     * on a list in this Parser to ensure GC safety. Thus the tempLifoAlloc
     * arenas containing the entries must be alive until we are done with
     * scanning, parsing and code generation for the whole script or top-level
     * function.
     */

    ObjectBox* objbox = alloc.template new_<ObjectBox>(obj, traceListHead);
    if (!objbox) {
        ReportOutOfMemory(context);
        return nullptr;
    }

    traceListHead = objbox;

    return objbox;
}

template <class ParseHandler>
FunctionBox*
PerHandlerParser<ParseHandler>::newFunctionBox(Node fn, JSFunction* fun, uint32_t toStringStart,
                                               Directives inheritedDirectives,
                                               GeneratorKind generatorKind,
                                               FunctionAsyncKind asyncKind)
{
    MOZ_ASSERT(fun);

    /*
     * We use JSContext.tempLifoAlloc to allocate parsed objects and place them
     * on a list in this Parser to ensure GC safety. Thus the tempLifoAlloc
     * arenas containing the entries must be alive until we are done with
     * scanning, parsing and code generation for the whole script or top-level
     * function.
     */
    FunctionBox* funbox =
        alloc.new_<FunctionBox>(context, traceListHead, fun, toStringStart, inheritedDirectives,
                                options().extraWarningsOption, generatorKind, asyncKind);
    if (!funbox) {
        ReportOutOfMemory(context);
        return nullptr;
    }

    traceListHead = funbox;
    if (fn)
        handler.setFunctionBox(fn, funbox);

    return funbox;
}

ModuleSharedContext::ModuleSharedContext(JSContext* cx, ModuleObject* module,
                                         Scope* enclosingScope, ModuleBuilder& builder)
  : SharedContext(cx, Kind::Module, Directives(true), false),
    module_(cx, module),
    enclosingScope_(cx, enclosingScope),
    bindings(cx),
    builder(builder)
{
    thisBinding_ = ThisBinding::Module;
}

void
ParserBase::trace(JSTracer* trc)
{
    ObjectBox::TraceList(trc, traceListHead);
}

void
TraceParser(JSTracer* trc, AutoGCRooter* parser)
{
    static_cast<ParserBase*>(parser)->trace(trc);
}

/*
 * Parse a top-level JS script.
 */
template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::parse()
{
    MOZ_ASSERT(checkOptionsCalled);

    Directives directives(options().strictOption);
    GlobalSharedContext globalsc(context, ScopeKind::Global,
                                 directives, options().extraWarningsOption);
    SourceParseContext globalpc(this, &globalsc, /* newDirectives = */ nullptr);
    if (!globalpc.init())
        return null();

    ParseContext::VarScope varScope(this);
    if (!varScope.init(pc))
        return null();

    Node pn = statementList(YieldIsName);
    if (!pn)
        return null();

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();
    if (tt != TokenKind::Eof) {
        error(JSMSG_GARBAGE_AFTER_INPUT, "script", TokenKindToDesc(tt));
        return null();
    }
    if (foldConstants) {
        if (!FoldConstants(context, &pn, this))
            return null();
    }

    return pn;
}

/*
 * Strict mode forbids introducing new definitions for 'eval', 'arguments',
 * 'let', 'static', 'yield', or for any strict mode reserved word.
 */
bool
ParserBase::isValidStrictBinding(PropertyName* name)
{
    TokenKind tt = ReservedWordTokenKind(name);
    if (tt == TokenKind::Name) {
        return name != context->names().eval &&
               name != context->names().arguments;
    }
    return tt != TokenKind::Let &&
           tt != TokenKind::Static &&
           tt != TokenKind::Yield &&
           !TokenKindIsStrictReservedWord(tt);
}

/*
 * Returns true if all parameter names are valid strict mode binding names and
 * no duplicate parameter names are present.
 */
bool
ParserBase::hasValidSimpleStrictParameterNames()
{
    MOZ_ASSERT(pc->isFunctionBox() && pc->functionBox()->hasSimpleParameterList());

    if (pc->functionBox()->hasDuplicateParameters)
        return false;

    for (auto* name : pc->positionalFormalParameterNames()) {
        MOZ_ASSERT(name);
        if (!isValidStrictBinding(name->asPropertyName()))
            return false;
    }
    return true;
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::reportMissingClosing(unsigned errorNumber, unsigned noteNumber,
                                                         uint32_t openedPos)
{
    auto notes = MakeUnique<JSErrorNotes>();
    if (!notes) {
        ReportOutOfMemory(pc->sc()->context);
        return;
    }

    uint32_t line, column;
    anyChars.srcCoords.lineNumAndColumnIndex(openedPos, &line, &column);

    const size_t MaxWidth = sizeof("4294967295");
    char columnNumber[MaxWidth];
    SprintfLiteral(columnNumber, "%" PRIu32, column);
    char lineNumber[MaxWidth];
    SprintfLiteral(lineNumber, "%" PRIu32, line);

    if (!notes->addNoteASCII(pc->sc()->context,
                             getFilename(), line, column,
                             GetErrorMessage, nullptr,
                             noteNumber, lineNumber, columnNumber))
    {
        return;
    }

    errorWithNotes(Move(notes), errorNumber);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::reportRedeclaration(HandlePropertyName name,
                                                        DeclarationKind prevKind,
                                                        TokenPos pos, uint32_t prevPos)
{
    JSAutoByteString bytes;
    if (!AtomToPrintableString(context, name, &bytes))
        return;

    if (prevPos == DeclaredNameInfo::npos) {
        errorAt(pos.begin, JSMSG_REDECLARED_VAR, DeclarationKindString(prevKind), bytes.ptr());
        return;
    }

    auto notes = MakeUnique<JSErrorNotes>();
    if (!notes) {
        ReportOutOfMemory(pc->sc()->context);
        return;
    }

    uint32_t line, column;
    anyChars.srcCoords.lineNumAndColumnIndex(prevPos, &line, &column);

    const size_t MaxWidth = sizeof("4294967295");
    char columnNumber[MaxWidth];
    SprintfLiteral(columnNumber, "%" PRIu32, column);
    char lineNumber[MaxWidth];
    SprintfLiteral(lineNumber, "%" PRIu32, line);

    if (!notes->addNoteASCII(pc->sc()->context,
                             getFilename(), line, column,
                             GetErrorMessage, nullptr,
                             JSMSG_REDECLARED_PREV,
                             lineNumber, columnNumber))
    {
        return;
    }

    errorWithNotesAt(Move(notes), pos.begin, JSMSG_REDECLARED_VAR,
                     DeclarationKindString(prevKind), bytes.ptr());
}

// notePositionalFormalParameter is called for both the arguments of a regular
// function definition and the arguments specified by the Function
// constructor.
//
// The 'disallowDuplicateParams' bool indicates whether the use of another
// feature (destructuring or default arguments) disables duplicate arguments.
// (ECMA-262 requires us to support duplicate parameter names, but, for newer
// features, we consider the code to have "opted in" to higher standards and
// forbid duplicates.)
template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::notePositionalFormalParameter(Node fn, HandlePropertyName name,
                                                                  uint32_t beginPos,
                                                                  bool disallowDuplicateParams,
                                                                  bool* duplicatedParam)
{
    if (AddDeclaredNamePtr p = pc->functionScope().lookupDeclaredNameForAdd(name)) {
        if (disallowDuplicateParams) {
            error(JSMSG_BAD_DUP_ARGS);
            return false;
        }

        // Strict-mode disallows duplicate args. We may not know whether we are
        // in strict mode or not (since the function body hasn't been parsed).
        // In such cases, report will queue up the potential error and return
        // 'true'.
        if (pc->sc()->needStrictChecks()) {
            JSAutoByteString bytes;
            if (!AtomToPrintableString(context, name, &bytes))
                return false;
            if (!strictModeError(JSMSG_DUPLICATE_FORMAL, bytes.ptr()))
                return false;
        }

        *duplicatedParam = true;
    } else {
        DeclarationKind kind = DeclarationKind::PositionalFormalParameter;
        if (!pc->functionScope().addDeclaredName(pc, p, name, kind, beginPos))
            return false;
    }

    if (!pc->positionalFormalParameterNames().append(name)) {
        ReportOutOfMemory(context);
        return false;
    }

    Node paramNode = newName(name);
    if (!paramNode)
        return false;

    handler.addFunctionFormalParameter(fn, paramNode);
    return true;
}

template <class ParseHandler>
bool
PerHandlerParser<ParseHandler>::noteDestructuredPositionalFormalParameter(Node fn, Node destruct)
{
    // Append an empty name to the positional formals vector to keep track of
    // argument slots when making FunctionScope::Data.
    if (!pc->positionalFormalParameterNames().append(nullptr)) {
        ReportOutOfMemory(context);
        return false;
    }

    handler.addFunctionFormalParameter(fn, destruct);
    return true;
}

static bool
DeclarationKindIsVar(DeclarationKind kind)
{
    return kind == DeclarationKind::Var ||
           kind == DeclarationKind::BodyLevelFunction ||
           kind == DeclarationKind::VarForAnnexBLexicalFunction ||
           kind == DeclarationKind::ForOfVar;
}

Maybe<DeclarationKind>
ParseContext::isVarRedeclaredInEval(HandlePropertyName name, DeclarationKind kind)
{
    MOZ_ASSERT(DeclarationKindIsVar(kind));
    MOZ_ASSERT(sc()->isEvalContext());

    // In the case of eval, we also need to check enclosing VM scopes to see
    // if the var declaration is allowed in the context.
    //
    // This check is necessary in addition to
    // js::CheckEvalDeclarationConflicts because we only know during parsing
    // if a var is bound by for-of.
    js::Scope* enclosingScope = sc()->compilationEnclosingScope();
    js::Scope* varScope = EvalScope::nearestVarScopeForDirectEval(enclosingScope);
    MOZ_ASSERT(varScope);
    for (ScopeIter si(enclosingScope); si; si++) {
        for (js::BindingIter bi(si.scope()); bi; bi++) {
            if (bi.name() != name)
                continue;

            switch (bi.kind()) {
              case BindingKind::Let: {
                  // Annex B.3.5 allows redeclaring simple (non-destructured)
                  // catch parameters with var declarations, except when it
                  // appears in a for-of.
                  bool annexB35Allowance = si.kind() == ScopeKind::SimpleCatch &&
                                           kind != DeclarationKind::ForOfVar;
                  if (!annexB35Allowance) {
                      return Some(ScopeKindIsCatch(si.kind())
                                  ? DeclarationKind::CatchParameter
                                  : DeclarationKind::Let);
                  }
                  break;
              }

              case BindingKind::Const:
                return Some(DeclarationKind::Const);

              case BindingKind::Import:
              case BindingKind::FormalParameter:
              case BindingKind::Var:
              case BindingKind::NamedLambdaCallee:
                break;
            }
        }

        if (si.scope() == varScope)
            break;
    }

    return Nothing();
}

Maybe<DeclarationKind>
ParseContext::isVarRedeclaredInInnermostScope(HandlePropertyName name, DeclarationKind kind)
{
    Maybe<DeclarationKind> redeclaredKind;
    uint32_t unused;
    MOZ_ALWAYS_TRUE(tryDeclareVarHelper<DryRunInnermostScopeOnly>(name, kind,
                                                                  DeclaredNameInfo::npos,
                                                                  &redeclaredKind, &unused));
    return redeclaredKind;
}

bool
ParseContext::tryDeclareVar(HandlePropertyName name, DeclarationKind kind,
                            uint32_t beginPos, Maybe<DeclarationKind>* redeclaredKind,
                            uint32_t* prevPos)
{
    return tryDeclareVarHelper<NotDryRun>(name, kind, beginPos, redeclaredKind, prevPos);
}

static bool
DeclarationKindIsParameter(DeclarationKind kind)
{
    return kind == DeclarationKind::PositionalFormalParameter ||
           kind == DeclarationKind::FormalParameter;
}

template <ParseContext::DryRunOption dryRunOption>
bool
ParseContext::tryDeclareVarHelper(HandlePropertyName name, DeclarationKind kind,
                                  uint32_t beginPos, Maybe<DeclarationKind>* redeclaredKind,
                                  uint32_t* prevPos)
{
    MOZ_ASSERT(DeclarationKindIsVar(kind));

    // It is an early error if a 'var' declaration appears inside a
    // scope contour that has a lexical declaration of the same name. For
    // example, the following are early errors:
    //
    //   { let x; var x; }
    //   { { var x; } let x; }
    //
    // And the following are not:
    //
    //   { var x; var x; }
    //   { { let x; } var x; }

    for (ParseContext::Scope* scope = innermostScope();
         scope != varScope().enclosing();
         scope = scope->enclosing())
    {
        if (AddDeclaredNamePtr p = scope->lookupDeclaredNameForAdd(name)) {
            DeclarationKind declaredKind = p->value()->kind();
            if (DeclarationKindIsVar(declaredKind)) {
                // Any vars that are redeclared as body-level functions must
                // be recorded as body-level functions.
                //
                // In the case of global and eval scripts, GlobalDeclaration-
                // Instantiation [1] and EvalDeclarationInstantiation [2]
                // check for the declarability of global var and function
                // bindings via CanDeclareVar [3] and CanDeclareGlobal-
                // Function [4]. CanDeclareGlobalFunction is strictly more
                // restrictive than CanDeclareGlobalVar, so record the more
                // restrictive kind. These semantics are implemented in
                // CheckCanDeclareGlobalBinding.
                //
                // For a var previously declared as ForOfVar, this previous
                // DeclarationKind is used only to check for if the
                // 'arguments' binding should be declared. Since body-level
                // functions shadow 'arguments' [5], it is correct to alter
                // the kind to BodyLevelFunction. See
                // declareFunctionArgumentsObject.
                //
                // VarForAnnexBLexicalFunction declarations are declared when
                // the var scope exits. It is not possible for a var to be
                // previously declared as VarForAnnexBLexicalFunction and
                // checked for redeclaration.
                //
                // [1] ES 15.1.11
                // [2] ES 18.2.1.3
                // [3] ES 8.1.1.4.15
                // [4] ES 8.1.1.4.16
                // [5] ES 9.2.12
                if (dryRunOption == NotDryRun && kind == DeclarationKind::BodyLevelFunction) {
                    MOZ_ASSERT(declaredKind != DeclarationKind::VarForAnnexBLexicalFunction);
                    p->value()->alterKind(kind);
                }
            } else if (!DeclarationKindIsParameter(declaredKind)) {
                // Annex B.3.5 allows redeclaring simple (non-destructured)
                // catch parameters with var declarations, except when it
                // appears in a for-of.
                bool annexB35Allowance = declaredKind == DeclarationKind::SimpleCatchParameter &&
                                         kind != DeclarationKind::ForOfVar;

                // Annex B.3.3 allows redeclaring functions in the same block.
                bool annexB33Allowance = declaredKind == DeclarationKind::SloppyLexicalFunction &&
                                         kind == DeclarationKind::VarForAnnexBLexicalFunction &&
                                         scope == innermostScope();

                if (!annexB35Allowance && !annexB33Allowance) {
                    *redeclaredKind = Some(declaredKind);
                    *prevPos = p->value()->pos();
                    return true;
                }
            } else if (kind == DeclarationKind::VarForAnnexBLexicalFunction) {
                MOZ_ASSERT(DeclarationKindIsParameter(declaredKind));

                // Annex B.3.3.1 disallows redeclaring parameter names.
                // We don't need to set *prevPos here since this case is not
                // an error.
                *redeclaredKind = Some(declaredKind);
                return true;
            }
        } else if (dryRunOption == NotDryRun) {
            if (!scope->addDeclaredName(this, p, name, kind, beginPos))
                return false;
        }

        // DryRunOption is used for propagating Annex B functions: we don't
        // want to declare the synthesized Annex B vars until we exit the var
        // scope and know that no early errors would have occurred. In order
        // to avoid quadratic search, we only check for var redeclarations in
        // the innermost scope when doing a dry run.
        if (dryRunOption == DryRunInnermostScopeOnly)
            break;
    }

    if (!sc()->strict() && sc()->isEvalContext() &&
        (dryRunOption == NotDryRun || innermostScope() == &varScope()))
    {
        *redeclaredKind = isVarRedeclaredInEval(name, kind);
        // We don't have position information at runtime.
        *prevPos = DeclaredNameInfo::npos;
    }

    return true;
}

bool
ParseContext::annexBAppliesToLexicalFunctionInInnermostScope(FunctionBox* funbox)
{
    MOZ_ASSERT(!sc()->strict());

    RootedPropertyName name(sc()->context, funbox->function()->explicitName()->asPropertyName());
    Maybe<DeclarationKind> redeclaredKind =
        isVarRedeclaredInInnermostScope(name, DeclarationKind::VarForAnnexBLexicalFunction);

    if (!redeclaredKind && isFunctionBox()) {
        Scope& funScope = functionScope();
        if (&funScope != &varScope()) {
            // Annex B.3.3.1 disallows redeclaring parameter names. In the
            // presence of parameter expressions, parameter names are on the
            // function scope, which encloses the var scope. This means the
            // isVarRedeclaredInInnermostScope call above would not catch this
            // case, so test it manually.
            if (AddDeclaredNamePtr p = funScope.lookupDeclaredNameForAdd(name)) {
                DeclarationKind declaredKind = p->value()->kind();
                if (DeclarationKindIsParameter(declaredKind))
                    redeclaredKind = Some(declaredKind);
                else
                    MOZ_ASSERT(FunctionScope::isSpecialName(sc()->context, name));
            }
        }
    }

    // If an early error would have occurred already, this function should not
    // exhibit Annex B.3.3 semantics.
    return !redeclaredKind;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::checkLexicalDeclarationDirectlyWithinBlock(ParseContext::Statement& stmt,
                                                                               DeclarationKind kind,
                                                                               TokenPos pos)
{
    MOZ_ASSERT(DeclarationKindIsLexical(kind));

    // It is an early error to declare a lexical binding not directly
    // within a block.
    if (!StatementKindIsBraced(stmt.kind()) &&
        stmt.kind() != StatementKind::ForLoopLexicalHead)
    {
        errorAt(pos.begin,
                stmt.kind() == StatementKind::Label
                ? JSMSG_LEXICAL_DECL_LABEL
                : JSMSG_LEXICAL_DECL_NOT_IN_BLOCK,
                DeclarationKindString(kind));
        return false;
    }

    return true;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::noteDeclaredName(HandlePropertyName name, DeclarationKind kind,
                                                     TokenPos pos)
{
    // The asm.js validator does all its own symbol-table management so, as an
    // optimization, avoid doing any work here.
    if (pc->useAsmOrInsideUseAsm())
        return true;

    switch (kind) {
      case DeclarationKind::Var:
      case DeclarationKind::BodyLevelFunction:
      case DeclarationKind::ForOfVar: {
        Maybe<DeclarationKind> redeclaredKind;
        uint32_t prevPos;
        if (!pc->tryDeclareVar(name, kind, pos.begin, &redeclaredKind, &prevPos))
            return false;

        if (redeclaredKind) {
            reportRedeclaration(name, *redeclaredKind, pos, prevPos);
            return false;
        }

        break;
      }

      case DeclarationKind::ModuleBodyLevelFunction: {
          MOZ_ASSERT(pc->atModuleLevel());

          AddDeclaredNamePtr p = pc->varScope().lookupDeclaredNameForAdd(name);
          if (p) {
              reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
              return false;
          }

          if (!pc->varScope().addDeclaredName(pc, p, name, kind, pos.begin))
              return false;

          // Body-level functions in modules are always closed over.
          pc->varScope().lookupDeclaredName(name)->value()->setClosedOver();

          break;
      }

      case DeclarationKind::FormalParameter: {
        // It is an early error if any non-positional formal parameter name
        // (e.g., destructuring formal parameter) is duplicated.

        AddDeclaredNamePtr p = pc->functionScope().lookupDeclaredNameForAdd(name);
        if (p) {
            error(JSMSG_BAD_DUP_ARGS);
            return false;
        }

        if (!pc->functionScope().addDeclaredName(pc, p, name, kind, pos.begin))
            return false;

        break;
      }

      case DeclarationKind::LexicalFunction: {
        ParseContext::Scope* scope = pc->innermostScope();
        AddDeclaredNamePtr p = scope->lookupDeclaredNameForAdd(name);
        if (p) {
            reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
            return false;
        }

        if (!scope->addDeclaredName(pc, p, name, kind, pos.begin))
            return false;

        break;
      }

      case DeclarationKind::SloppyLexicalFunction: {
        // Functions in block have complex allowances in sloppy mode for being
        // labelled that other lexical declarations do not have. Those checks
        // are more complex than calling checkLexicalDeclarationDirectlyWithin-
        // Block and are done in checkFunctionDefinition.

        ParseContext::Scope* scope = pc->innermostScope();
        if (AddDeclaredNamePtr p = scope->lookupDeclaredNameForAdd(name)) {
            // It is usually an early error if there is another declaration
            // with the same name in the same scope.
            //
            // Sloppy lexical functions may redeclare other sloppy lexical
            // functions for web compatibility reasons.
            if (p->value()->kind() != DeclarationKind::SloppyLexicalFunction) {
                reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
                return false;
            }
        } else {
            if (!scope->addDeclaredName(pc, p, name, kind, pos.begin))
                return false;
        }

        break;
      }

      case DeclarationKind::Let:
      case DeclarationKind::Const:
      case DeclarationKind::Class:
        // The BoundNames of LexicalDeclaration and ForDeclaration must not
        // contain 'let'. (CatchParameter is the only lexical binding form
        // without this restriction.)
        if (name == context->names().let) {
            errorAt(pos.begin, JSMSG_LEXICAL_DECL_DEFINES_LET);
            return false;
        }

        MOZ_FALLTHROUGH;

      case DeclarationKind::Import:
        // Module code is always strict, so 'let' is always a keyword and never a name.
        MOZ_ASSERT(name != context->names().let);
        MOZ_FALLTHROUGH;

      case DeclarationKind::SimpleCatchParameter:
      case DeclarationKind::CatchParameter: {
        if (ParseContext::Statement* stmt = pc->innermostStatement()) {
            if (!checkLexicalDeclarationDirectlyWithinBlock(*stmt, kind, pos))
                return false;
        }

        ParseContext::Scope* scope = pc->innermostScope();

        // For body-level lexically declared names in a function, it is an
        // early error if there is a formal parameter of the same name. This
        // needs a special check if there is an extra var scope due to
        // parameter expressions.
        if (pc->isFunctionExtraBodyVarScopeInnermost()) {
            DeclaredNamePtr p = pc->functionScope().lookupDeclaredName(name);
            if (p && DeclarationKindIsParameter(p->value()->kind())) {
                reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
                return false;
            }
        }

        // It is an early error if there is another declaration with the same
        // name in the same scope.
        AddDeclaredNamePtr p = scope->lookupDeclaredNameForAdd(name);
        if (p) {
            reportRedeclaration(name, p->value()->kind(), pos, p->value()->pos());
            return false;
        }

        if (!scope->addDeclaredName(pc, p, name, kind, pos.begin))
            return false;

        break;
      }

      case DeclarationKind::CoverArrowParameter:
        // CoverArrowParameter is only used as a placeholder declaration kind.
        break;

      case DeclarationKind::PositionalFormalParameter:
        MOZ_CRASH("Positional formal parameter names should use "
                  "notePositionalFormalParameter");
        break;

      case DeclarationKind::VarForAnnexBLexicalFunction:
        MOZ_CRASH("Synthesized Annex B vars should go through "
                  "tryDeclareVarForAnnexBLexicalFunction");
        break;
    }

    return true;
}

bool
ParserBase::noteUsedNameInternal(HandlePropertyName name)
{
    // The asm.js validator does all its own symbol-table management so, as an
    // optimization, avoid doing any work here.
    if (pc->useAsmOrInsideUseAsm())
        return true;

    // Global bindings are properties and not actual bindings; we don't need
    // to know if they are closed over. So no need to track used name at the
    // global scope. It is not incorrect to track them, this is an
    // optimization.
    ParseContext::Scope* scope = pc->innermostScope();
    if (pc->sc()->isGlobalContext() && scope == &pc->varScope())
        return true;

    return usedNames.noteUse(context, name, pc->scriptId(), scope->id());
}

bool
ParserBase::hasUsedName(HandlePropertyName name)
{
    if (UsedNamePtr p = usedNames.lookup(name))
        return p->value().isUsedInScript(pc->scriptId());
    return false;
}

template <class ParseHandler>
bool
PerHandlerParser<ParseHandler>::propagateFreeNamesAndMarkClosedOverBindings(ParseContext::Scope& scope)
{
    // Now that we have all the declared names in the scope, check which
    // functions should exhibit Annex B semantics.
    if (!scope.propagateAndMarkAnnexBFunctionBoxes(pc))
        return false;

    if (handler.canSkipLazyClosedOverBindings()) {
        // Scopes are nullptr-delimited in the LazyScript closed over bindings
        // array.
        while (JSAtom* name = handler.nextLazyClosedOverBinding())
            scope.lookupDeclaredName(name)->value()->setClosedOver();
        return true;
    }

    bool isSyntaxParser = mozilla::IsSame<ParseHandler, SyntaxParseHandler>::value;
    uint32_t scriptId = pc->scriptId();
    uint32_t scopeId = scope.id();
    for (BindingIter bi = scope.bindings(pc); bi; bi++) {
        if (UsedNamePtr p = usedNames.lookup(bi.name())) {
            bool closedOver;
            p->value().noteBoundInScope(scriptId, scopeId, &closedOver);
            if (closedOver) {
                bi.setClosedOver();

                if (isSyntaxParser && !pc->closedOverBindingsForLazy().append(bi.name())) {
                    ReportOutOfMemory(context);
                    return false;
                }
            }
        }
    }

    // Append a nullptr to denote end-of-scope.
    if (isSyntaxParser && !pc->closedOverBindingsForLazy().append(nullptr)) {
        ReportOutOfMemory(context);
        return false;
    }

    return true;
}

template <typename CharT>
bool
Parser<FullParseHandler, CharT>::checkStatementsEOF()
{
    // This is designed to be paired with parsing a statement list at the top
    // level.
    //
    // The statementList() call breaks on TokenKind::Rc, so make sure we've
    // reached EOF here.
    TokenKind tt;
    if (!tokenStream.peekToken(&tt, TokenStream::Operand))
        return false;
    if (tt != TokenKind::Eof) {
        error(JSMSG_UNEXPECTED_TOKEN, "expression", TokenKindToDesc(tt));
        return false;
    }
    return true;
}

template <typename Scope>
typename Scope::Data*
NewEmptyBindingData(JSContext* cx, LifoAlloc& alloc, uint32_t numBindings)
{
    size_t allocSize = Scope::sizeOfData(numBindings);
    typename Scope::Data* bindings = static_cast<typename Scope::Data*>(alloc.alloc(allocSize));
    if (!bindings) {
        ReportOutOfMemory(cx);
        return nullptr;
    }
    PodZero(bindings);
    return bindings;
}

Maybe<GlobalScope::Data*>
NewGlobalScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc)
{

    Vector<BindingName> funs(context);
    Vector<BindingName> vars(context);
    Vector<BindingName> lets(context);
    Vector<BindingName> consts(context);

    bool allBindingsClosedOver = pc->sc()->allBindingsClosedOver();
    for (BindingIter bi = scope.bindings(pc); bi; bi++) {
        BindingName binding(bi.name(), allBindingsClosedOver || bi.closedOver());
        switch (bi.kind()) {
          case BindingKind::Var:
            if (bi.declarationKind() == DeclarationKind::BodyLevelFunction) {
                if (!funs.append(binding))
                    return Nothing();
            } else {
                if (!vars.append(binding))
                    return Nothing();
            }
            break;
          case BindingKind::Let:
            if (!lets.append(binding))
                return Nothing();
            break;
          case BindingKind::Const:
            if (!consts.append(binding))
                return Nothing();
            break;
          default:
            MOZ_CRASH("Bad global scope BindingKind");
        }
    }

    GlobalScope::Data* bindings = nullptr;
    uint32_t numBindings = funs.length() + vars.length() + lets.length() + consts.length();

    if (numBindings > 0) {
        bindings = NewEmptyBindingData<GlobalScope>(context, alloc, numBindings);
        if (!bindings)
            return Nothing();

        // The ordering here is important. See comments in GlobalScope.
        BindingName* start = bindings->names;
        BindingName* cursor = start;

        PodCopy(cursor, funs.begin(), funs.length());
        cursor += funs.length();

        bindings->varStart = cursor - start;
        PodCopy(cursor, vars.begin(), vars.length());
        cursor += vars.length();

        bindings->letStart = cursor - start;
        PodCopy(cursor, lets.begin(), lets.length());
        cursor += lets.length();

        bindings->constStart = cursor - start;
        PodCopy(cursor, consts.begin(), consts.length());
        bindings->length = numBindings;
    }

    return Some(bindings);
}

Maybe<GlobalScope::Data*>
ParserBase::newGlobalScopeData(ParseContext::Scope& scope)
{
    return NewGlobalScopeData(context, scope, alloc, pc);
}

Maybe<ModuleScope::Data*>
NewModuleScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc)
{
    Vector<BindingName> imports(context);
    Vector<BindingName> vars(context);
    Vector<BindingName> lets(context);
    Vector<BindingName> consts(context);

    bool allBindingsClosedOver = pc->sc()->allBindingsClosedOver();
    for (BindingIter bi = scope.bindings(pc); bi; bi++) {
        // Imports are indirect bindings and must not be given known slots.
        BindingName binding(bi.name(), (allBindingsClosedOver || bi.closedOver()) &&
                                       bi.kind() != BindingKind::Import);
        switch (bi.kind()) {
          case BindingKind::Import:
            if (!imports.append(binding))
                return Nothing();
            break;
          case BindingKind::Var:
            if (!vars.append(binding))
                return Nothing();
            break;
          case BindingKind::Let:
            if (!lets.append(binding))
                return Nothing();
            break;
          case BindingKind::Const:
            if (!consts.append(binding))
                return Nothing();
            break;
          default:
            MOZ_CRASH("Bad module scope BindingKind");
        }
    }

    ModuleScope::Data* bindings = nullptr;
    uint32_t numBindings = imports.length() + vars.length() + lets.length() + consts.length();

    if (numBindings > 0) {
        bindings = NewEmptyBindingData<ModuleScope>(context, alloc, numBindings);
        if (!bindings)
            return Nothing();

        // The ordering here is important. See comments in ModuleScope.
        BindingName* start = bindings->names;
        BindingName* cursor = start;

        PodCopy(cursor, imports.begin(), imports.length());
        cursor += imports.length();

        bindings->varStart = cursor - start;
        PodCopy(cursor, vars.begin(), vars.length());
        cursor += vars.length();

        bindings->letStart = cursor - start;
        PodCopy(cursor, lets.begin(), lets.length());
        cursor += lets.length();

        bindings->constStart = cursor - start;
        PodCopy(cursor, consts.begin(), consts.length());
        bindings->length = numBindings;
    }

    return Some(bindings);
}

Maybe<ModuleScope::Data*>
ParserBase::newModuleScopeData(ParseContext::Scope& scope)
{
    return NewModuleScopeData(context, scope, alloc, pc);
}

Maybe<EvalScope::Data*>
NewEvalScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc)
{
    Vector<BindingName> funs(context);
    Vector<BindingName> vars(context);

    for (BindingIter bi = scope.bindings(pc); bi; bi++) {
        // Eval scopes only contain 'var' bindings. Make all bindings aliased
        // for now.
        MOZ_ASSERT(bi.kind() == BindingKind::Var);
        BindingName binding(bi.name(), true);
        if (bi.declarationKind() == DeclarationKind::BodyLevelFunction) {
            if (!funs.append(binding))
                return Nothing();
        } else {
            if (!vars.append(binding))
                return Nothing();
        }
    }

    EvalScope::Data* bindings = nullptr;
    uint32_t numBindings = funs.length() + vars.length();

    if (numBindings > 0) {
        bindings = NewEmptyBindingData<EvalScope>(context, alloc, numBindings);
        if (!bindings)
            return Nothing();

        BindingName* start = bindings->names;
        BindingName* cursor = start;

        // Keep track of what vars are functions. This is only used in BCE to omit
        // superfluous DEFVARs.
        PodCopy(cursor, funs.begin(), funs.length());
        cursor += funs.length();

        bindings->varStart = cursor - start;
        PodCopy(cursor, vars.begin(), vars.length());
        bindings->length = numBindings;
    }

    return Some(bindings);
}

Maybe<EvalScope::Data*>
ParserBase::newEvalScopeData(ParseContext::Scope& scope)
{
    return NewEvalScopeData(context, scope, alloc, pc);
}

Maybe<FunctionScope::Data*>
NewFunctionScopeData(JSContext* context, ParseContext::Scope& scope, bool hasParameterExprs, LifoAlloc& alloc, ParseContext* pc)
{
    Vector<BindingName> positionalFormals(context);
    Vector<BindingName> formals(context);
    Vector<BindingName> vars(context);

    bool allBindingsClosedOver = pc->sc()->allBindingsClosedOver();
    bool hasDuplicateParams = pc->functionBox()->hasDuplicateParameters;

    // Positional parameter names must be added in order of appearance as they are
    // referenced using argument slots.
    for (size_t i = 0; i < pc->positionalFormalParameterNames().length(); i++) {
        JSAtom* name = pc->positionalFormalParameterNames()[i];

        BindingName bindName;
        if (name) {
            DeclaredNamePtr p = scope.lookupDeclaredName(name);

            // Do not consider any positional formal parameters closed over if
            // there are parameter defaults. It is the binding in the defaults
            // scope that is closed over instead.
            bool closedOver = allBindingsClosedOver ||
                              (p && p->value()->closedOver());

            // If the parameter name has duplicates, only the final parameter
            // name should be on the environment, as otherwise the environment
            // object would have multiple, same-named properties.
            if (hasDuplicateParams) {
                for (size_t j = pc->positionalFormalParameterNames().length() - 1; j > i; j--) {
                    if (pc->positionalFormalParameterNames()[j] == name) {
                        closedOver = false;
                        break;
                    }
                }
            }

            bindName = BindingName(name, closedOver);
        }

        if (!positionalFormals.append(bindName))
            return Nothing();
    }

    for (BindingIter bi = scope.bindings(pc); bi; bi++) {
        BindingName binding(bi.name(), allBindingsClosedOver || bi.closedOver());
        switch (bi.kind()) {
          case BindingKind::FormalParameter:
            // Positional parameter names are already handled above.
            if (bi.declarationKind() == DeclarationKind::FormalParameter) {
                if (!formals.append(binding))
                    return Nothing();
            }
            break;
          case BindingKind::Var:
            // The only vars in the function scope when there are parameter
            // exprs, which induces a separate var environment, should be the
            // special bindings.
            MOZ_ASSERT_IF(hasParameterExprs, FunctionScope::isSpecialName(context, bi.name()));
            if (!vars.append(binding))
                return Nothing();
            break;
          default:
            break;
        }
    }

    FunctionScope::Data* bindings = nullptr;
    uint32_t numBindings = positionalFormals.length() + formals.length() + vars.length();

    if (numBindings > 0) {
        bindings = NewEmptyBindingData<FunctionScope>(context, alloc, numBindings);
        if (!bindings)
            return Nothing();

        // The ordering here is important. See comments in FunctionScope.
        BindingName* start = bindings->names;
        BindingName* cursor = start;

        PodCopy(cursor, positionalFormals.begin(), positionalFormals.length());
        cursor += positionalFormals.length();

        bindings->nonPositionalFormalStart = cursor - start;
        PodCopy(cursor, formals.begin(), formals.length());
        cursor += formals.length();

        bindings->varStart = cursor - start;
        PodCopy(cursor, vars.begin(), vars.length());
        bindings->length = numBindings;
    }

    return Some(bindings);
}

Maybe<FunctionScope::Data*>
ParserBase::newFunctionScopeData(ParseContext::Scope& scope, bool hasParameterExprs)
{
    return NewFunctionScopeData(context, scope, hasParameterExprs, alloc, pc);
}

Maybe<VarScope::Data*>
NewVarScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc)
{
    Vector<BindingName> vars(context);

    bool allBindingsClosedOver = pc->sc()->allBindingsClosedOver();

    for (BindingIter bi = scope.bindings(pc); bi; bi++) {
        if (bi.kind() == BindingKind::Var) {
            BindingName binding(bi.name(), allBindingsClosedOver || bi.closedOver());
            if (!vars.append(binding))
                return Nothing();
        }
    }

    VarScope::Data* bindings = nullptr;
    uint32_t numBindings = vars.length();

    if (numBindings > 0) {
        bindings = NewEmptyBindingData<VarScope>(context, alloc, numBindings);
        if (!bindings)
            return Nothing();

        // The ordering here is important. See comments in FunctionScope.
        BindingName* start = bindings->names;
        BindingName* cursor = start;

        PodCopy(cursor, vars.begin(), vars.length());
        bindings->length = numBindings;
    }

    return Some(bindings);
}

Maybe<VarScope::Data*>
ParserBase::newVarScopeData(ParseContext::Scope& scope)
{
    return NewVarScopeData(context, scope, alloc, pc);
}

Maybe<LexicalScope::Data*>
NewLexicalScopeData(JSContext* context, ParseContext::Scope& scope, LifoAlloc& alloc, ParseContext* pc)
{
    Vector<BindingName> lets(context);
    Vector<BindingName> consts(context);

    // Unlike other scopes with bindings which are body-level, it is unknown
    // if pc->sc()->allBindingsClosedOver() is correct at the time of
    // finishing parsing a lexical scope.
    //
    // Instead, pc->sc()->allBindingsClosedOver() is checked in
    // EmitterScope::enterLexical. Also see comment there.
    for (BindingIter bi = scope.bindings(pc); bi; bi++) {
        BindingName binding(bi.name(), bi.closedOver());
        switch (bi.kind()) {
          case BindingKind::Let:
            if (!lets.append(binding))
                return Nothing();
            break;
          case BindingKind::Const:
            if (!consts.append(binding))
                return Nothing();
            break;
          default:
            break;
        }
    }

    LexicalScope::Data* bindings = nullptr;
    uint32_t numBindings = lets.length() + consts.length();

    if (numBindings > 0) {
        bindings = NewEmptyBindingData<LexicalScope>(context, alloc, numBindings);
        if (!bindings)
            return Nothing();

        // The ordering here is important. See comments in LexicalScope.
        BindingName* cursor = bindings->names;
        BindingName* start = cursor;

        PodCopy(cursor, lets.begin(), lets.length());
        cursor += lets.length();

        bindings->constStart = cursor - start;
        PodCopy(cursor, consts.begin(), consts.length());
        bindings->length = numBindings;
    }

    return Some(bindings);
}

Maybe<LexicalScope::Data*>
ParserBase::newLexicalScopeData(ParseContext::Scope& scope)
{
    return NewLexicalScopeData(context, scope, alloc, pc);
}

template <>
SyntaxParseHandler::Node
PerHandlerParser<SyntaxParseHandler>::finishLexicalScope(ParseContext::Scope& scope, Node body)
{
    if (!propagateFreeNamesAndMarkClosedOverBindings(scope))
        return null();

    return body;
}

template <>
ParseNode*
PerHandlerParser<FullParseHandler>::finishLexicalScope(ParseContext::Scope& scope, ParseNode* body)
{
    if (!propagateFreeNamesAndMarkClosedOverBindings(scope))
        return nullptr;

    Maybe<LexicalScope::Data*> bindings = newLexicalScopeData(scope);
    if (!bindings)
        return nullptr;

    return handler.newLexicalScope(*bindings, body);
}

template <typename CharT>
ParseNode*
Parser<FullParseHandler, CharT>::evalBody(EvalSharedContext* evalsc)
{
    SourceParseContext evalpc(this, evalsc, /* newDirectives = */ nullptr);
    if (!evalpc.init())
        return nullptr;

    ParseContext::VarScope varScope(this);
    if (!varScope.init(pc))
        return nullptr;

    ParseNode* body;
    {
        // All evals have an implicit non-extensible lexical scope.
        ParseContext::Scope lexicalScope(this);
        if (!lexicalScope.init(pc))
            return nullptr;

        body = statementList(YieldIsName);
        if (!body)
            return nullptr;

        if (!checkStatementsEOF())
            return nullptr;

        body = finishLexicalScope(lexicalScope, body);
        if (!body)
            return nullptr;
    }

#ifdef DEBUG
    if (evalpc.superScopeNeedsHomeObject() && evalsc->compilationEnclosingScope()) {
        // If superScopeNeedsHomeObject_ is set and we are an entry-point
        // ParseContext, then we must be emitting an eval script, and the
        // outer function must already be marked as needing a home object
        // since it contains an eval.
        ScopeIter si(evalsc->compilationEnclosingScope());
        for (; si; si++) {
            if (si.kind() == ScopeKind::Function) {
                JSFunction* fun = si.scope()->as<FunctionScope>().canonicalFunction();
                if (fun->isArrow())
                    continue;
                MOZ_ASSERT(fun->allowSuperProperty());
                MOZ_ASSERT(fun->nonLazyScript()->needsHomeObject());
                break;
            }
        }
        MOZ_ASSERT(!si.done(),
                   "Eval must have found an enclosing function box scope that allows super.property");
    }
#endif

    if (!FoldConstants(context, &body, this))
        return nullptr;

    // For eval scripts, since all bindings are automatically considered
    // closed over, we don't need to call propagateFreeNamesAndMarkClosed-
    // OverBindings. However, Annex B.3.3 functions still need to be marked.
    if (!varScope.propagateAndMarkAnnexBFunctionBoxes(pc))
        return nullptr;

    Maybe<EvalScope::Data*> bindings = newEvalScopeData(pc->varScope());
    if (!bindings)
        return nullptr;
    evalsc->bindings = *bindings;

    return body;
}

template <typename CharT>
ParseNode*
Parser<FullParseHandler, CharT>::globalBody(GlobalSharedContext* globalsc)
{
    SourceParseContext globalpc(this, globalsc, /* newDirectives = */ nullptr);
    if (!globalpc.init())
        return nullptr;

    ParseContext::VarScope varScope(this);
    if (!varScope.init(pc))
        return nullptr;

    ParseNode* body = statementList(YieldIsName);
    if (!body)
        return nullptr;

    if (!checkStatementsEOF())
        return nullptr;

    if (!FoldConstants(context, &body, this))
        return nullptr;

    // For global scripts, whether bindings are closed over or not doesn't
    // matter, so no need to call propagateFreeNamesAndMarkClosedOver-
    // Bindings. However, Annex B.3.3 functions still need to be marked.
    if (!varScope.propagateAndMarkAnnexBFunctionBoxes(pc))
        return nullptr;

    Maybe<GlobalScope::Data*> bindings = newGlobalScopeData(pc->varScope());
    if (!bindings)
        return nullptr;
    globalsc->bindings = *bindings;

    return body;
}

template <typename CharT>
ParseNode*
Parser<FullParseHandler, CharT>::moduleBody(ModuleSharedContext* modulesc)
{
    MOZ_ASSERT(checkOptionsCalled);

    SourceParseContext modulepc(this, modulesc, nullptr);
    if (!modulepc.init())
        return null();

    ParseContext::VarScope varScope(this);
    if (!varScope.init(pc))
        return nullptr;

    Node mn = handler.newModule(pos());
    if (!mn)
        return null();

    AutoAwaitIsKeyword<FullParseHandler, CharT> awaitIsKeyword(this, AwaitIsModuleKeyword);
    ParseNode* pn = statementList(YieldIsName);
    if (!pn)
        return null();

    MOZ_ASSERT(pn->isKind(ParseNodeKind::StatementList));
    mn->pn_body = pn;

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();
    if (tt != TokenKind::Eof) {
        error(JSMSG_GARBAGE_AFTER_INPUT, "module", TokenKindToDesc(tt));
        return null();
    }

    if (!modulesc->builder.buildTables())
        return null();

    // Check exported local bindings exist and mark them as closed over.
    for (auto entry : modulesc->builder.localExportEntries()) {
        JSAtom* name = entry->localName();
        MOZ_ASSERT(name);

        DeclaredNamePtr p = modulepc.varScope().lookupDeclaredName(name);
        if (!p) {
            JSAutoByteString str;
            if (!AtomToPrintableString(context, name, &str))
                return null();

            errorAt(TokenStream::NoOffset, JSMSG_MISSING_EXPORT, str.ptr());
            return null();
        }

        p->value()->setClosedOver();
    }

    if (!FoldConstants(context, &pn, this))
        return null();

    if (!propagateFreeNamesAndMarkClosedOverBindings(modulepc.varScope()))
        return null();

    Maybe<ModuleScope::Data*> bindings = newModuleScopeData(modulepc.varScope());
    if (!bindings)
        return nullptr;

    modulesc->bindings = *bindings;
    return mn;
}

template <typename CharT>
SyntaxParseHandler::Node
Parser<SyntaxParseHandler, CharT>::moduleBody(ModuleSharedContext* modulesc)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return SyntaxParseHandler::NodeFailure;
}

bool
ParserBase::hasUsedFunctionSpecialName(HandlePropertyName name)
{
    MOZ_ASSERT(name == context->names().arguments || name == context->names().dotThis);
    return hasUsedName(name) || pc->functionBox()->bindingsAccessedDynamically();
}

template <class ParseHandler>
bool
PerHandlerParser<ParseHandler>::declareFunctionThis()
{
    // The asm.js validator does all its own symbol-table management so, as an
    // optimization, avoid doing any work here.
    if (pc->useAsmOrInsideUseAsm())
        return true;

    // Derived class constructors emit JSOP_CHECKRETURN, which requires
    // '.this' to be bound.
    FunctionBox* funbox = pc->functionBox();
    HandlePropertyName dotThis = context->names().dotThis;

    bool declareThis;
    if (handler.canSkipLazyClosedOverBindings())
        declareThis = funbox->function()->lazyScript()->hasThisBinding();
    else
        declareThis = hasUsedFunctionSpecialName(dotThis) || funbox->isDerivedClassConstructor();

    if (declareThis) {
        ParseContext::Scope& funScope = pc->functionScope();
        AddDeclaredNamePtr p = funScope.lookupDeclaredNameForAdd(dotThis);
        MOZ_ASSERT(!p);
        if (!funScope.addDeclaredName(pc, p, dotThis, DeclarationKind::Var,
                                      DeclaredNameInfo::npos))
        {
            return false;
        }
        funbox->setHasThisBinding();
    }

    return true;
}

template <class ParseHandler>
typename ParseHandler::Node
PerHandlerParser<ParseHandler>::newInternalDotName(HandlePropertyName name)
{
    Node nameNode = newName(name);
    if (!nameNode)
        return null();
    if (!noteUsedName(name))
        return null();
    return nameNode;
}

template <class ParseHandler>
typename ParseHandler::Node
PerHandlerParser<ParseHandler>::newThisName()
{
    return newInternalDotName(context->names().dotThis);
}

template <class ParseHandler>
typename ParseHandler::Node
PerHandlerParser<ParseHandler>::newDotGeneratorName()
{
    return newInternalDotName(context->names().dotGenerator);
}

bool
ParserBase::declareDotGeneratorName()
{
    // The special '.generator' binding must be on the function scope, as
    // generators expect to find it on the CallObject.
    ParseContext::Scope& funScope = pc->functionScope();
    HandlePropertyName dotGenerator = context->names().dotGenerator;
    AddDeclaredNamePtr p = funScope.lookupDeclaredNameForAdd(dotGenerator);
    if (!p && !funScope.addDeclaredName(pc, p, dotGenerator, DeclarationKind::Var,
                                        DeclaredNameInfo::npos))
    {
        return false;
    }
    return true;
}

template <class ParseHandler>
bool
PerHandlerParser<ParseHandler>::finishFunctionScopes(bool isStandaloneFunction)
{
    FunctionBox* funbox = pc->functionBox();

    if (funbox->hasParameterExprs) {
        if (!propagateFreeNamesAndMarkClosedOverBindings(pc->functionScope()))
            return false;
    }

    if (funbox->function()->isNamedLambda() && !isStandaloneFunction) {
        if (!propagateFreeNamesAndMarkClosedOverBindings(pc->namedLambdaScope()))
            return false;
    }

    return true;
}

template <>
bool
PerHandlerParser<FullParseHandler>::finishFunction(bool isStandaloneFunction /* = false */)
{
    if (!finishFunctionScopes(isStandaloneFunction))
        return false;

    FunctionBox* funbox = pc->functionBox();
    bool hasParameterExprs = funbox->hasParameterExprs;

    if (hasParameterExprs) {
        Maybe<VarScope::Data*> bindings = newVarScopeData(pc->varScope());
        if (!bindings)
            return false;
        funbox->extraVarScopeBindings().set(*bindings);
    }

    {
        Maybe<FunctionScope::Data*> bindings = newFunctionScopeData(pc->functionScope(),
                                                                    hasParameterExprs);
        if (!bindings)
            return false;
        funbox->functionScopeBindings().set(*bindings);
    }

    if (funbox->function()->isNamedLambda() && !isStandaloneFunction) {
        Maybe<LexicalScope::Data*> bindings = newLexicalScopeData(pc->namedLambdaScope());
        if (!bindings)
            return false;
        funbox->namedLambdaBindings().set(*bindings);
    }

    return true;
}

template <>
bool
PerHandlerParser<SyntaxParseHandler>::finishFunction(bool isStandaloneFunction /* = false */)
{
    // The LazyScript for a lazily parsed function needs to know its set of
    // free variables and inner functions so that when it is fully parsed, we
    // can skip over any already syntax parsed inner functions and still
    // retain correct scope information.

    if (!finishFunctionScopes(isStandaloneFunction))
        return false;

    // There are too many bindings or inner functions to be saved into the
    // LazyScript. Do a full parse.
    if (pc->closedOverBindingsForLazy().length() >= LazyScript::NumClosedOverBindingsLimit ||
        pc->innerFunctionsForLazy.length() >= LazyScript::NumInnerFunctionsLimit)
    {
        MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
        return false;
    }

    FunctionBox* funbox = pc->functionBox();
    RootedFunction fun(context, funbox->function());
    LazyScript* lazy = LazyScript::Create(context, fun, pc->closedOverBindingsForLazy(),
                                          pc->innerFunctionsForLazy,
                                          funbox->bufStart, funbox->bufEnd,
                                          funbox->toStringStart,
                                          funbox->startLine, funbox->startColumn);
    if (!lazy)
        return false;

    // Flags that need to be copied into the JSScript when we do the full
    // parse.
    if (pc->sc()->strict())
        lazy->setStrict();
    lazy->setGeneratorKind(funbox->generatorKind());
    lazy->setAsyncKind(funbox->asyncKind());
    if (funbox->hasRest())
        lazy->setHasRest();
    if (funbox->isExprBody())
        lazy->setIsExprBody();
    if (funbox->isLikelyConstructorWrapper())
        lazy->setLikelyConstructorWrapper();
    if (funbox->isDerivedClassConstructor())
        lazy->setIsDerivedClassConstructor();
    if (funbox->needsHomeObject())
        lazy->setNeedsHomeObject();
    if (funbox->declaredArguments)
        lazy->setShouldDeclareArguments();
    if (funbox->hasThisBinding())
        lazy->setHasThisBinding();

    // Flags that need to copied back into the parser when we do the full
    // parse.
    PropagateTransitiveParseFlags(funbox, lazy);

    fun->initLazyScript(lazy);
    return true;
}

static YieldHandling
GetYieldHandling(GeneratorKind generatorKind)
{
    if (generatorKind == GeneratorKind::NotGenerator)
        return YieldIsName;
    return YieldIsKeyword;
}

static AwaitHandling
GetAwaitHandling(FunctionAsyncKind asyncKind)
{
    if (asyncKind == FunctionAsyncKind::SyncFunction)
        return AwaitIsName;
    return AwaitIsKeyword;
}

template <typename CharT>
ParseNode*
Parser<FullParseHandler, CharT>::standaloneFunction(HandleFunction fun,
                                                    HandleScope enclosingScope,
                                                    const Maybe<uint32_t>& parameterListEnd,
                                                    GeneratorKind generatorKind,
                                                    FunctionAsyncKind asyncKind,
                                                    Directives inheritedDirectives,
                                                    Directives* newDirectives)
{
    MOZ_ASSERT(checkOptionsCalled);

    // Skip prelude.
    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();
    if (asyncKind == FunctionAsyncKind::AsyncFunction) {
        MOZ_ASSERT(tt == TokenKind::Async);
        if (!tokenStream.getToken(&tt, TokenStream::Operand))
            return null();
    }
    MOZ_ASSERT(tt == TokenKind::Function);

    if (!tokenStream.getToken(&tt))
        return null();
    if (generatorKind == GeneratorKind::Generator) {
        MOZ_ASSERT(tt == TokenKind::Mul);
        if (!tokenStream.getToken(&tt))
            return null();
    }

    // Skip function name, if present.
    if (TokenKindIsPossibleIdentifierName(tt)) {
        MOZ_ASSERT(anyChars.currentName() == fun->explicitName());
    } else {
        MOZ_ASSERT(fun->explicitName() == nullptr);
        anyChars.ungetToken();
    }

    Node fn = handler.newFunctionStatement(pos());
    if (!fn)
        return null();

    ParseNode* argsbody = handler.newList(ParseNodeKind::ParamsBody, pos());
    if (!argsbody)
        return null();
    fn->pn_body = argsbody;

    FunctionBox* funbox = newFunctionBox(fn, fun, /* toStringStart = */ 0, inheritedDirectives,
                                         generatorKind, asyncKind);
    if (!funbox)
        return null();
    funbox->initStandaloneFunction(enclosingScope);

    SourceParseContext funpc(this, funbox, newDirectives);
    if (!funpc.init())
        return null();
    funpc.setIsStandaloneFunctionBody();

    YieldHandling yieldHandling = GetYieldHandling(generatorKind);
    AwaitHandling awaitHandling = GetAwaitHandling(asyncKind);
    AutoAwaitIsKeyword<FullParseHandler, CharT> awaitIsKeyword(this, awaitHandling);
    if (!functionFormalParametersAndBody(InAllowed, yieldHandling, &fn, Statement,
                                         parameterListEnd, /* isStandaloneFunction = */ true))
    {
        return null();
    }

    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();
    if (tt != TokenKind::Eof) {
        error(JSMSG_GARBAGE_AFTER_INPUT, "function body", TokenKindToDesc(tt));
        return null();
    }

    if (!FoldConstants(context, &fn, this))
        return null();

    return fn;
}

template <class ParseHandler>
bool
PerHandlerParser<ParseHandler>::declareFunctionArgumentsObject()
{
    FunctionBox* funbox = pc->functionBox();
    ParseContext::Scope& funScope = pc->functionScope();
    ParseContext::Scope& varScope = pc->varScope();

    bool hasExtraBodyVarScope = &funScope != &varScope;

    // Time to implement the odd semantics of 'arguments'.
    HandlePropertyName argumentsName = context->names().arguments;

    bool tryDeclareArguments;
    if (handler.canSkipLazyClosedOverBindings())
        tryDeclareArguments = funbox->function()->lazyScript()->shouldDeclareArguments();
    else
        tryDeclareArguments = hasUsedFunctionSpecialName(argumentsName);

    // ES 9.2.12 steps 19 and 20 say formal parameters, lexical bindings,
    // and body-level functions named 'arguments' shadow the arguments
    // object.
    //
    // So even if there wasn't a free use of 'arguments' but there is a var
    // binding of 'arguments', we still might need the arguments object.
    //
    // If we have an extra var scope due to parameter expressions and the body
    // declared 'var arguments', we still need to declare 'arguments' in the
    // function scope.
    DeclaredNamePtr p = varScope.lookupDeclaredName(argumentsName);
    if (p && (p->value()->kind() == DeclarationKind::Var ||
              p->value()->kind() == DeclarationKind::ForOfVar))
    {
        if (hasExtraBodyVarScope)
            tryDeclareArguments = true;
        else
            funbox->usesArguments = true;
    }

    if (tryDeclareArguments) {
        AddDeclaredNamePtr p = funScope.lookupDeclaredNameForAdd(argumentsName);
        if (!p) {
            if (!funScope.addDeclaredName(pc, p, argumentsName, DeclarationKind::Var,
                                          DeclaredNameInfo::npos))
            {
                return false;
            }
            funbox->declaredArguments = true;
            funbox->usesArguments = true;
        } else if (hasExtraBodyVarScope) {
            // Formal parameters shadow the arguments object.
            return true;
        }
    }

    // Compute if we need an arguments object.
    if (funbox->usesArguments) {
        // There is an 'arguments' binding. Is the arguments object definitely
        // needed?
        //
        // Also see the flags' comments in ContextFlags.
        funbox->setArgumentsHasLocalBinding();

        // Dynamic scope access destroys all hope of optimization.
        if (pc->sc()->bindingsAccessedDynamically())
            funbox->setDefinitelyNeedsArgsObj();

        // If a script contains the debugger statement either directly or
        // within an inner function, the arguments object should be created
        // eagerly so the Debugger API may observe bindings.
        if (pc->sc()->hasDebuggerStatement())
            funbox->setDefinitelyNeedsArgsObj();
    }

    return true;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::functionBody(InHandling inHandling,
                                                 YieldHandling yieldHandling,
                                                 FunctionSyntaxKind kind, FunctionBodyType type)
{
    MOZ_ASSERT(pc->isFunctionBox());

#ifdef DEBUG
    uint32_t startYieldOffset = pc->lastYieldOffset;
#endif

    Node pn;
    if (type == StatementListBody) {
        bool inheritedStrict = pc->sc()->strict();
        pn = statementList(yieldHandling);
        if (!pn)
            return null();

        // When we transitioned from non-strict to strict mode, we need to
        // validate that all parameter names are valid strict mode names.
        if (!inheritedStrict && pc->sc()->strict()) {
            MOZ_ASSERT(pc->sc()->hasExplicitUseStrict(),
                       "strict mode should only change when a 'use strict' directive is present");
            if (!hasValidSimpleStrictParameterNames()) {
                // Request that this function be reparsed as strict to report
                // the invalid parameter name at the correct source location.
                pc->newDirectives->setStrict();
                return null();
            }
        }
    } else {
        MOZ_ASSERT(type == ExpressionBody);

        // Async functions are implemented as generators, and generators are
        // assumed to be statement lists, to prepend initial `yield`.
        Node stmtList = null();
        if (pc->isAsync()) {
            stmtList = handler.newStatementList(pos());
            if (!stmtList)
                return null();
        }

        Node kid = assignExpr(inHandling, yieldHandling, TripledotProhibited);
        if (!kid)
            return null();

        pn = handler.newExpressionBody(kid);
        if (!pn)
            return null();

        if (pc->isAsync()) {
            handler.addStatementToList(stmtList, pn);
            pn = stmtList;
        }
    }

    MOZ_ASSERT_IF(!pc->isGenerator() && !pc->isAsync(), pc->lastYieldOffset == startYieldOffset);
    MOZ_ASSERT_IF(pc->isGenerator(), kind != Arrow);
    MOZ_ASSERT_IF(pc->isGenerator(), type == StatementListBody);

    if (pc->needsDotGeneratorName()) {
        MOZ_ASSERT_IF(!pc->isAsync(), type == StatementListBody);
        if (!declareDotGeneratorName())
            return null();
        Node generator = newDotGeneratorName();
        if (!generator)
            return null();
        if (!handler.prependInitialYield(pn, generator))
            return null();
    }

    // Declare the 'arguments' and 'this' bindings if necessary before
    // finishing up the scope so these special bindings get marked as closed
    // over if necessary. Arrow functions don't have these bindings.
    if (kind != Arrow) {
        if (!declareFunctionArgumentsObject())
            return null();
        if (!declareFunctionThis())
            return null();
    }

    return finishLexicalScope(pc->varScope(), pn);
}

JSFunction*
ParserBase::newFunction(HandleAtom atom, FunctionSyntaxKind kind,
                        GeneratorKind generatorKind, FunctionAsyncKind asyncKind,
                        HandleObject proto)
{
    MOZ_ASSERT_IF(kind == Statement, atom != nullptr);

    RootedFunction fun(context);

    gc::AllocKind allocKind = gc::AllocKind::FUNCTION;
    JSFunction::Flags flags;
#ifdef DEBUG
    bool isGlobalSelfHostedBuiltin = false;
#endif
    switch (kind) {
      case AssignmentExpression:
      case PrimaryExpression:
        flags = (generatorKind == GeneratorKind::NotGenerator &&
                 asyncKind == FunctionAsyncKind::SyncFunction
                 ? JSFunction::INTERPRETED_LAMBDA
                 : JSFunction::INTERPRETED_LAMBDA_GENERATOR_OR_ASYNC);
        break;
      case Arrow:
        flags = JSFunction::INTERPRETED_LAMBDA_ARROW;
        allocKind = gc::AllocKind::FUNCTION_EXTENDED;
        break;
      case Method:
        flags = (generatorKind == GeneratorKind::NotGenerator &&
                 asyncKind == FunctionAsyncKind::SyncFunction
                 ? JSFunction::INTERPRETED_METHOD
                 : JSFunction::INTERPRETED_METHOD_GENERATOR_OR_ASYNC);
        allocKind = gc::AllocKind::FUNCTION_EXTENDED;
        break;
      case ClassConstructor:
      case DerivedClassConstructor:
        flags = JSFunction::INTERPRETED_CLASS_CONSTRUCTOR;
        allocKind = gc::AllocKind::FUNCTION_EXTENDED;
        break;
      case Getter:
      case GetterNoExpressionClosure:
        flags = JSFunction::INTERPRETED_GETTER;
        allocKind = gc::AllocKind::FUNCTION_EXTENDED;
        break;
      case Setter:
      case SetterNoExpressionClosure:
        flags = JSFunction::INTERPRETED_SETTER;
        allocKind = gc::AllocKind::FUNCTION_EXTENDED;
        break;
      default:
        MOZ_ASSERT(kind == Statement);
#ifdef DEBUG
        if (options().selfHostingMode && !pc->isFunctionBox()) {
            isGlobalSelfHostedBuiltin = true;
            allocKind = gc::AllocKind::FUNCTION_EXTENDED;
        }
#endif
        flags = (generatorKind == GeneratorKind::NotGenerator &&
                 asyncKind == FunctionAsyncKind::SyncFunction
                 ? JSFunction::INTERPRETED_NORMAL
                 : JSFunction::INTERPRETED_GENERATOR_OR_ASYNC);
    }

    // We store the async wrapper in a slot for later access.
    if (asyncKind == FunctionAsyncKind::AsyncFunction)
        allocKind = gc::AllocKind::FUNCTION_EXTENDED;

    fun = NewFunctionWithProto(context, nullptr, 0, flags, nullptr, atom, proto,
                               allocKind, TenuredObject);
    if (!fun)
        return nullptr;
    if (options().selfHostingMode) {
        fun->setIsSelfHostedBuiltin();
#ifdef DEBUG
        if (isGlobalSelfHostedBuiltin)
            fun->setExtendedSlot(HAS_SELFHOSTED_CANONICAL_NAME_SLOT, BooleanValue(false));
#endif
    }
    return fun;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::matchOrInsertSemicolon()
{
    TokenKind tt = TokenKind::Eof;
    if (!tokenStream.peekTokenSameLine(&tt, TokenStream::Operand))
        return false;
    if (tt != TokenKind::Eof &&
        tt != TokenKind::Eol &&
        tt != TokenKind::Semi &&
        tt != TokenKind::Rc)
    {
        /*
         * When current token is `await` and it's outside of async function,
         * it's possibly intended to be an await expression.
         *
         *   await f();
         *        ^
         *        |
         *        tried to insert semicolon here
         *
         * Detect this situation and throw an understandable error.  Otherwise
         * we'd throw a confusing "unexpected token: (unexpected token)" error.
         */
        if (!pc->isAsync() && anyChars.currentToken().type == TokenKind::Await) {
            error(JSMSG_AWAIT_OUTSIDE_ASYNC);
            return false;
        }
        if (!yieldExpressionsSupported() && anyChars.currentToken().type == TokenKind::Yield) {
            error(JSMSG_YIELD_OUTSIDE_GENERATOR);
            return false;
        }

        /* Advance the scanner for proper error location reporting. */
        tokenStream.consumeKnownToken(tt, TokenStream::Operand);
        error(JSMSG_UNEXPECTED_TOKEN_NO_EXPECT, TokenKindToDesc(tt));
        return false;
    }
    bool matched;
    return tokenStream.matchToken(&matched, TokenKind::Semi, TokenStream::Operand);
}

bool
ParserBase::leaveInnerFunction(ParseContext* outerpc)
{
    MOZ_ASSERT(pc != outerpc);

    // If the current function allows super.property but cannot have a home
    // object, i.e., it is an arrow function, we need to propagate the flag to
    // the outer ParseContext.
    if (pc->superScopeNeedsHomeObject()) {
        if (!pc->isArrowFunction())
            MOZ_ASSERT(pc->functionBox()->needsHomeObject());
        else
            outerpc->setSuperScopeNeedsHomeObject();
    }

    // Lazy functions inner to another lazy function need to be remembered by
    // the inner function so that if the outer function is eventually parsed
    // we do not need any further parsing or processing of the inner function.
    //
    // Append the inner function here unconditionally; the vector is only used
    // if the Parser using outerpc is a syntax parsing. See
    // GeneralParser<SyntaxParseHandler>::finishFunction.
    if (!outerpc->innerFunctionsForLazy.append(pc->functionBox()->function()))
        return false;

    PropagateTransitiveParseFlags(pc->functionBox(), outerpc->sc());

    return true;
}

JSAtom*
ParserBase::prefixAccessorName(PropertyType propType, HandleAtom propAtom)
{
    RootedAtom prefix(context);
    if (propType == PropertyType::Setter || propType == PropertyType::SetterNoExpressionClosure) {
        prefix = context->names().setPrefix;
    } else {
        MOZ_ASSERT(propType == PropertyType::Getter || propType == PropertyType::GetterNoExpressionClosure);
        prefix = context->names().getPrefix;
    }

    RootedString str(context, ConcatStrings<CanGC>(context, prefix, propAtom));
    if (!str)
        return nullptr;

    return AtomizeString(context, str);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::functionArguments(YieldHandling yieldHandling,
                                                      FunctionSyntaxKind kind,
                                                      Node funcpn)
{
    FunctionBox* funbox = pc->functionBox();

    bool parenFreeArrow = false;
    // Modifier for the following tokens.
    // TokenStream::None for the following cases:
    //   async a => 1
    //         ^
    //
    //   (a) => 1
    //   ^
    //
    //   async (a) => 1
    //         ^
    //
    //   function f(a) {}
    //             ^
    //
    // TokenStream::Operand for the following case:
    //   a => 1
    //   ^
    Modifier firstTokenModifier = TokenStream::None;

    // Modifier for the the first token in each argument.
    // can be changed to TokenStream::None for the following case:
    //   async a => 1
    //         ^
    Modifier argModifier = TokenStream::Operand;
    if (kind == Arrow) {
        TokenKind tt;
        // In async function, the first token after `async` is already gotten
        // with TokenStream::None.
        // In sync function, the first token is already gotten with
        // TokenStream::Operand.
        firstTokenModifier = funbox->isAsync() ? TokenStream::None : TokenStream::Operand;
        if (!tokenStream.peekToken(&tt, firstTokenModifier))
            return false;
        if (TokenKindIsPossibleIdentifier(tt)) {
            parenFreeArrow = true;
            argModifier = firstTokenModifier;
        }
    }

    TokenPos firstTokenPos;
    if (!parenFreeArrow) {
        TokenKind tt;
        if (!tokenStream.getToken(&tt, firstTokenModifier))
            return false;
        if (tt != TokenKind::Lp) {
            error(kind == Arrow ? JSMSG_BAD_ARROW_ARGS : JSMSG_PAREN_BEFORE_FORMAL);
            return false;
        }

        firstTokenPos = pos();

        // Record the start of function source (for FunctionToString). If we
        // are parenFreeArrow, we will set this below, after consuming the NAME.
        funbox->setStart(anyChars);
    } else {
        // When delazifying, we may not have a current token and pos() is
        // garbage. In that case, substitute the first token's position.
        if (!tokenStream.peekTokenPos(&firstTokenPos, firstTokenModifier))
            return false;
    }

    Node argsbody = handler.newList(ParseNodeKind::ParamsBody, firstTokenPos);
    if (!argsbody)
        return false;
    handler.setFunctionFormalParametersAndBody(funcpn, argsbody);

    bool hasArguments = false;
    if (parenFreeArrow) {
        hasArguments = true;
    } else {
        bool matched;
        if (!tokenStream.matchToken(&matched, TokenKind::Rp, TokenStream::Operand))
            return false;
        if (!matched)
            hasArguments = true;
    }
    if (hasArguments) {
        bool hasRest = false;
        bool hasDefault = false;
        bool duplicatedParam = false;
        bool disallowDuplicateParams = kind == Arrow || kind == Method || kind == ClassConstructor;
        AtomVector& positionalFormals = pc->positionalFormalParameterNames();

        if (IsGetterKind(kind)) {
            error(JSMSG_ACCESSOR_WRONG_ARGS, "getter", "no", "s");
            return false;
        }

        while (true) {
            if (hasRest) {
                error(JSMSG_PARAMETER_AFTER_REST);
                return false;
            }

            TokenKind tt;
            if (!tokenStream.getToken(&tt, argModifier))
                return false;
            argModifier = TokenStream::Operand;
            MOZ_ASSERT_IF(parenFreeArrow, TokenKindIsPossibleIdentifier(tt));

            if (tt == TokenKind::TripleDot) {
                if (IsSetterKind(kind)) {
                    error(JSMSG_ACCESSOR_WRONG_ARGS, "setter", "one", "");
                    return false;
                }

                disallowDuplicateParams = true;
                if (duplicatedParam) {
                    // Has duplicated args before the rest parameter.
                    error(JSMSG_BAD_DUP_ARGS);
                    return false;
                }

                hasRest = true;
                funbox->setHasRest();

                if (!tokenStream.getToken(&tt))
                    return false;

                if (!TokenKindIsPossibleIdentifier(tt) &&
                    tt != TokenKind::Lb &&
                    tt != TokenKind::Lc)
                {
                    error(JSMSG_NO_REST_NAME);
                    return false;
                }
            }

            switch (tt) {
              case TokenKind::Lb:
              case TokenKind::Lc: {
                disallowDuplicateParams = true;
                if (duplicatedParam) {
                    // Has duplicated args before the destructuring parameter.
                    error(JSMSG_BAD_DUP_ARGS);
                    return false;
                }

                funbox->hasDestructuringArgs = true;

                Node destruct = destructuringDeclarationWithoutYieldOrAwait(
                    DeclarationKind::FormalParameter,
                    yieldHandling, tt);
                if (!destruct)
                    return false;

                if (!noteDestructuredPositionalFormalParameter(funcpn, destruct))
                    return false;

                break;
              }

              default: {
                if (!TokenKindIsPossibleIdentifier(tt)) {
                    error(JSMSG_MISSING_FORMAL);
                    return false;
                }

                if (parenFreeArrow)
                    funbox->setStart(anyChars);

                RootedPropertyName name(context, bindingIdentifier(yieldHandling));
                if (!name)
                    return false;

                if (!notePositionalFormalParameter(funcpn, name, pos().begin,
                                                   disallowDuplicateParams, &duplicatedParam))
                {
                    return false;
                }
                if (duplicatedParam)
                    funbox->hasDuplicateParameters = true;

                break;
              }
            }

            if (positionalFormals.length() >= ARGNO_LIMIT) {
                error(JSMSG_TOO_MANY_FUN_ARGS);
                return false;
            }

            // The next step is to detect arguments with default expressions,
            // e.g. |function parseInt(str, radix = 10) {}|.  But if we have a
            // parentheses-free arrow function, |a => ...|, the '=' necessary
            // for a default expression would really be an assignment operator:
            // that is, |a = b => 42;| would parse as |a = (b => 42);|.  So we
            // should stop parsing arguments here.
            if (parenFreeArrow)
                break;

            bool matched;
            if (!tokenStream.matchToken(&matched, TokenKind::Assign, TokenStream::Operand))
                return false;
            if (matched) {
                // A default argument without parentheses would look like:
                // a = expr => body, but both operators are right-associative, so
                // that would have been parsed as a = (expr => body) instead.
                // Therefore it's impossible to get here with parenFreeArrow.
                MOZ_ASSERT(!parenFreeArrow);

                if (hasRest) {
                    error(JSMSG_REST_WITH_DEFAULT);
                    return false;
                }
                disallowDuplicateParams = true;
                if (duplicatedParam) {
                    error(JSMSG_BAD_DUP_ARGS);
                    return false;
                }

                if (!hasDefault) {
                    hasDefault = true;

                    // The Function.length property is the number of formals
                    // before the first default argument.
                    funbox->length = positionalFormals.length() - 1;
                }
                funbox->hasParameterExprs = true;

                Node def_expr = assignExprWithoutYieldOrAwait(yieldHandling);
                if (!def_expr)
                    return false;
                if (!handler.setLastFunctionFormalParameterDefault(funcpn, def_expr))
                    return false;
            }

            // Setter syntax uniquely requires exactly one argument.
            if (IsSetterKind(kind))
                break;

            if (!tokenStream.matchToken(&matched, TokenKind::Comma, TokenStream::Operand))
                return false;
            if (!matched)
                break;

            if (!hasRest) {
                if (!tokenStream.peekToken(&tt, TokenStream::Operand))
                    return null();
                if (tt == TokenKind::Rp)
                    break;
            }
        }

        if (!parenFreeArrow) {
            TokenKind tt;
            if (!tokenStream.getToken(&tt, TokenStream::Operand))
                return false;
            if (tt != TokenKind::Rp) {
                if (IsSetterKind(kind)) {
                    error(JSMSG_ACCESSOR_WRONG_ARGS, "setter", "one", "");
                    return false;
                }

                error(JSMSG_PAREN_AFTER_FORMAL);
                return false;
            }
        }

        if (!hasDefault)
            funbox->length = positionalFormals.length() - hasRest;

        if (funbox->hasParameterExprs && funbox->hasDirectEval())
            funbox->hasDirectEvalInParameterExpr = true;

        funbox->function()->setArgCount(positionalFormals.length());
    } else if (IsSetterKind(kind)) {
        error(JSMSG_ACCESSOR_WRONG_ARGS, "setter", "one", "");
        return false;
    }

    return true;
}

template <typename CharT>
bool
Parser<FullParseHandler, CharT>::skipLazyInnerFunction(ParseNode* funcNode, uint32_t toStringStart,
                                                       FunctionSyntaxKind kind, bool tryAnnexB)
{
    // When a lazily-parsed function is called, we only fully parse (and emit)
    // that function, not any of its nested children. The initial syntax-only
    // parse recorded the free variables of nested functions and their extents,
    // so we can skip over them after accounting for their free variables.

    RootedFunction fun(context, handler.nextLazyInnerFunction());
    FunctionBox* funbox =
        newFunctionBox(funcNode, fun, toStringStart, Directives(/* strict = */ false),
                       fun->generatorKind(), fun->asyncKind());
    if (!funbox)
        return false;

    LazyScript* lazy = fun->lazyScript();
    if (lazy->needsHomeObject())
        funbox->setNeedsHomeObject();
    if (lazy->isExprBody())
        funbox->setIsExprBody();

    PropagateTransitiveParseFlags(lazy, pc->sc());

    if (!tokenStream.advance(fun->lazyScript()->end()))
        return false;

    if (allowExpressionClosures()) {
        // Only expression closure can be Statement kind.
        // If we remove expression closure, we can remove isExprBody flag from
        // LazyScript and JSScript.
        if (kind == Statement && funbox->isExprBody()) {
            if (!matchOrInsertSemicolon())
                return false;
        }
    }

    // Append possible Annex B function box only upon successfully parsing.
    if (tryAnnexB && !pc->innermostScope()->addPossibleAnnexBFunctionBox(pc, funbox))
        return false;

    return true;
}

template <typename CharT>
bool
Parser<SyntaxParseHandler, CharT>::skipLazyInnerFunction(Node funcNode, uint32_t toStringStart,
                                                         FunctionSyntaxKind kind,
                                                         bool tryAnnexB)
{
    MOZ_CRASH("Cannot skip lazy inner functions when syntax parsing");
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::skipLazyInnerFunction(Node funcNode, uint32_t toStringStart,
                                                          FunctionSyntaxKind kind,
                                                          bool tryAnnexB)
{
    return asFinalParser()->skipLazyInnerFunction(funcNode, toStringStart, kind, tryAnnexB);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::addExprAndGetNextTemplStrToken(YieldHandling yieldHandling,
                                                                   Node nodeList,
                                                                   TokenKind* ttp)
{
    Node pn = expr(InAllowed, yieldHandling, TripledotProhibited);
    if (!pn)
        return false;
    handler.addList(nodeList, pn);

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return false;
    if (tt != TokenKind::Rc) {
        error(JSMSG_TEMPLSTR_UNTERM_EXPR);
        return false;
    }

    return tokenStream.getToken(ttp, TokenStream::TemplateTail);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::taggedTemplate(YieldHandling yieldHandling, Node nodeList,
                                                   TokenKind tt)
{
    Node callSiteObjNode = handler.newCallSiteObject(pos().begin);
    if (!callSiteObjNode)
        return false;
    handler.addList(nodeList, callSiteObjNode);

    while (true) {
        if (!appendToCallSiteObj(callSiteObjNode))
            return false;
        if (tt != TokenKind::TemplateHead)
            break;

        if (!addExprAndGetNextTemplStrToken(yieldHandling, nodeList, &tt))
            return false;
    }
    handler.setEndPosition(nodeList, callSiteObjNode);
    return true;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::templateLiteral(YieldHandling yieldHandling)
{
    Node pn = noSubstitutionUntaggedTemplate();
    if (!pn)
        return null();

    Node nodeList = handler.newList(ParseNodeKind::TemplateStringList, pn);
    if (!nodeList)
        return null();

    TokenKind tt;
    do {
        if (!addExprAndGetNextTemplStrToken(yieldHandling, nodeList, &tt))
            return null();

        pn = noSubstitutionUntaggedTemplate();
        if (!pn)
            return null();

        handler.addList(nodeList, pn);
    } while (tt == TokenKind::TemplateHead);
    return nodeList;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::functionDefinition(Node funcNode, uint32_t toStringStart,
                                                       InHandling inHandling, YieldHandling yieldHandling,
                                                       HandleAtom funName, FunctionSyntaxKind kind,
                                                       GeneratorKind generatorKind,
                                                       FunctionAsyncKind asyncKind,
                                                       bool tryAnnexB /* = false */)
{
    MOZ_ASSERT_IF(kind == Statement, funName);

    // When fully parsing a LazyScript, we do not fully reparse its inner
    // functions, which are also lazy. Instead, their free variables and
    // source extents are recorded and may be skipped.
    if (handler.canSkipLazyInnerFunctions()) {
        if (!skipLazyInnerFunction(funcNode, toStringStart, kind, tryAnnexB))
            return null();

        return funcNode;
    }

    RootedObject proto(context);
    if (generatorKind == GeneratorKind::Generator ||
        asyncKind == FunctionAsyncKind::AsyncFunction)
    {
        proto = GlobalObject::getOrCreateGeneratorFunctionPrototype(context, context->global());
        if (!proto)
            return null();
    }
    RootedFunction fun(context, newFunction(funName, kind, generatorKind, asyncKind, proto));
    if (!fun)
        return null();

    // Speculatively parse using the directives of the parent parsing context.
    // If a directive is encountered (e.g., "use strict") that changes how the
    // function should have been parsed, we backup and reparse with the new set
    // of directives.
    Directives directives(pc);
    Directives newDirectives = directives;

    Position start(keepAtoms);
    tokenStream.tell(&start);

    // Parse the inner function. The following is a loop as we may attempt to
    // reparse a function due to failed syntax parsing and encountering new
    // "use foo" directives.
    while (true) {
        if (trySyntaxParseInnerFunction(&funcNode, fun, toStringStart, inHandling, yieldHandling,
                                        kind, generatorKind, asyncKind, tryAnnexB, directives,
                                        &newDirectives))
        {
            break;
        }

        // Return on error.
        if (anyChars.hadError() || directives == newDirectives)
            return null();

        // Assignment must be monotonic to prevent infinitely attempting to
        // reparse.
        MOZ_ASSERT_IF(directives.strict(), newDirectives.strict());
        MOZ_ASSERT_IF(directives.asmJS(), newDirectives.asmJS());
        directives = newDirectives;

        tokenStream.seek(start);

        // functionFormalParametersAndBody may have already set pn->pn_body before failing.
        handler.setFunctionFormalParametersAndBody(funcNode, null());
    }

    return funcNode;
}

template <typename CharT>
bool
Parser<FullParseHandler, CharT>::trySyntaxParseInnerFunction(ParseNode** funcNode,
                                                             HandleFunction fun,
                                                             uint32_t toStringStart,
                                                             InHandling inHandling,
                                                             YieldHandling yieldHandling,
                                                             FunctionSyntaxKind kind,
                                                             GeneratorKind generatorKind,
                                                             FunctionAsyncKind asyncKind,
                                                             bool tryAnnexB,
                                                             Directives inheritedDirectives,
                                                             Directives* newDirectives)
{
    // Try a syntax parse for this inner function.
    do {
        // If we're assuming this function is an IIFE, always perform a full
        // parse to avoid the overhead of a lazy syntax-only parse. Although
        // the prediction may be incorrect, IIFEs are common enough that it
        // pays off for lots of code.
        if ((*funcNode)->isLikelyIIFE() &&
            generatorKind == GeneratorKind::NotGenerator &&
            asyncKind == FunctionAsyncKind::SyncFunction)
        {
            break;
        }

        SyntaxParser* syntaxParser = getSyntaxParser();
        if (!syntaxParser)
            break;

        UsedNameTracker::RewindToken token = usedNames.getRewindToken();

        // Move the syntax parser to the current position in the stream.
        Position position(keepAtoms);
        tokenStream.tell(&position);
        if (!syntaxParser->tokenStream.seek(position, anyChars))
            return false;

        // Make a FunctionBox before we enter the syntax parser, because |pn|
        // still expects a FunctionBox to be attached to it during BCE, and
        // the syntax parser cannot attach one to it.
        FunctionBox* funbox = newFunctionBox(*funcNode, fun, toStringStart, inheritedDirectives,
                                             generatorKind, asyncKind);
        if (!funbox)
            return false;
        funbox->initWithEnclosingParseContext(pc, kind);

        SyntaxParseHandler::Node syntaxNode =
            syntaxParser->innerFunctionForFunctionBox(SyntaxParseHandler::NodeGeneric, pc, funbox,
                                                      inHandling, yieldHandling, kind,
                                                      newDirectives);
        if (!syntaxNode) {
            if (syntaxParser->hadAbortedSyntaxParse()) {
                // Try again with a full parse. UsedNameTracker needs to be
                // rewound to just before we tried the syntax parse for
                // correctness.
                syntaxParser->clearAbortedSyntaxParse();
                usedNames.rewind(token);
                MOZ_ASSERT_IF(!syntaxParser->context->helperThread(),
                              !syntaxParser->context->isExceptionPending());
                break;
            }
            return false;
        }

        // Advance this parser over tokens processed by the syntax parser.
        syntaxParser->tokenStream.tell(&position);
        if (!tokenStream.seek(position, syntaxParser->anyChars))
            return false;

        // Update the end position of the parse node.
        (*funcNode)->pn_pos.end = anyChars.currentToken().pos.end;

        // Append possible Annex B function box only upon successfully parsing.
        if (tryAnnexB) {
            if (!pc->innermostScope()->addPossibleAnnexBFunctionBox(pc, funbox))
                return false;
        }

        return true;
    } while (false);

    // We failed to do a syntax parse above, so do the full parse.
    Node innerFunc =
        innerFunction(*funcNode, pc, fun, toStringStart, inHandling, yieldHandling, kind,
                      generatorKind, asyncKind, tryAnnexB, inheritedDirectives, newDirectives);
    if (!innerFunc)
        return false;

    *funcNode = innerFunc;
    return true;
}

template <typename CharT>
bool
Parser<SyntaxParseHandler, CharT>::trySyntaxParseInnerFunction(Node* funcNode, HandleFunction fun,
                                                               uint32_t toStringStart,
                                                               InHandling inHandling,
                                                               YieldHandling yieldHandling,
                                                               FunctionSyntaxKind kind,
                                                               GeneratorKind generatorKind,
                                                               FunctionAsyncKind asyncKind,
                                                               bool tryAnnexB,
                                                               Directives inheritedDirectives,
                                                               Directives* newDirectives)
{
    // This is already a syntax parser, so just parse the inner function.
    Node innerFunc =
        innerFunction(*funcNode, pc, fun, toStringStart, inHandling, yieldHandling, kind,
                      generatorKind, asyncKind, tryAnnexB, inheritedDirectives, newDirectives);

    if (!innerFunc)
        return false;

    *funcNode = innerFunc;
    return true;
}

template <class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::trySyntaxParseInnerFunction(Node* funcNode, HandleFunction fun,
                                                                uint32_t toStringStart,
                                                                InHandling inHandling,
                                                                YieldHandling yieldHandling,
                                                                FunctionSyntaxKind kind,
                                                                GeneratorKind generatorKind,
                                                                FunctionAsyncKind asyncKind,
                                                                bool tryAnnexB,
                                                                Directives inheritedDirectives,
                                                                Directives* newDirectives)
{
    return asFinalParser()->trySyntaxParseInnerFunction(funcNode, fun, toStringStart, inHandling,
                                                        yieldHandling, kind, generatorKind,
                                                        asyncKind, tryAnnexB, inheritedDirectives,
                                                        newDirectives);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::innerFunctionForFunctionBox(Node funcNode,
                                                                ParseContext* outerpc,
                                                                FunctionBox* funbox,
                                                                InHandling inHandling,
                                                                YieldHandling yieldHandling,
                                                                FunctionSyntaxKind kind,
                                                                Directives* newDirectives)
{
    // Note that it is possible for outerpc != this->pc, as we may be
    // attempting to syntax parse an inner function from an outer full
    // parser. In that case, outerpc is a SourceParseContext from the full parser
    // instead of the current top of the stack of the syntax parser.

    // Push a new ParseContext.
    SourceParseContext funpc(this, funbox, newDirectives);
    if (!funpc.init())
        return null();

    if (!functionFormalParametersAndBody(inHandling, yieldHandling, &funcNode, kind))
        return null();

    if (!leaveInnerFunction(outerpc))
        return null();

    return funcNode;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::innerFunction(Node funcNode, ParseContext* outerpc,
                                                  HandleFunction fun, uint32_t toStringStart,
                                                  InHandling inHandling,
                                                  YieldHandling yieldHandling,
                                                  FunctionSyntaxKind kind,
                                                  GeneratorKind generatorKind,
                                                  FunctionAsyncKind asyncKind, bool tryAnnexB,
                                                  Directives inheritedDirectives,
                                                  Directives* newDirectives)
{
    // Note that it is possible for outerpc != this->pc, as we may be
    // attempting to syntax parse an inner function from an outer full
    // parser. In that case, outerpc is a SourceParseContext from the full parser
    // instead of the current top of the stack of the syntax parser.

    FunctionBox* funbox = newFunctionBox(funcNode, fun, toStringStart, inheritedDirectives,
                                         generatorKind, asyncKind);
    if (!funbox)
        return null();
    funbox->initWithEnclosingParseContext(outerpc, kind);

    Node innerFunc =
        innerFunctionForFunctionBox(funcNode, outerpc, funbox, inHandling, yieldHandling, kind,
                                     newDirectives);
    if (!innerFunc)
        return null();

    // Append possible Annex B function box only upon successfully parsing.
    if (tryAnnexB) {
        if (!pc->innermostScope()->addPossibleAnnexBFunctionBox(pc, funbox))
            return null();
    }

    return innerFunc;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::appendToCallSiteObj(Node callSiteObj)
{
    Node cookedNode = noSubstitutionTaggedTemplate();
    if (!cookedNode)
        return false;

    JSAtom* atom = tokenStream.getRawTemplateStringAtom();
    if (!atom)
        return false;
    Node rawNode = handler.newTemplateStringLiteral(atom, pos());
    if (!rawNode)
        return false;

    handler.addToCallSiteObject(callSiteObj, rawNode, cookedNode);
    return true;
}

template <typename CharT>
ParseNode*
Parser<FullParseHandler, CharT>::standaloneLazyFunction(HandleFunction fun, uint32_t toStringStart,
                                                        bool strict, GeneratorKind generatorKind,
                                                        FunctionAsyncKind asyncKind)
{
    MOZ_ASSERT(checkOptionsCalled);

    Node pn = handler.newFunctionStatement(pos());
    if (!pn)
        return null();

    Directives directives(strict);
    FunctionBox* funbox = newFunctionBox(pn, fun, toStringStart, directives, generatorKind,
                                         asyncKind);
    if (!funbox)
        return null();
    funbox->initFromLazyFunction();

    Directives newDirectives = directives;
    SourceParseContext funpc(this, funbox, &newDirectives);
    if (!funpc.init())
        return null();

    // Our tokenStream has no current token, so pn's position is garbage.
    // Substitute the position of the first token in our source.  If the
    // function is a not-async arrow, use TokenStream::Operand to keep
    // verifyConsistentModifier from complaining (we will use
    // TokenStream::Operand in functionArguments).
    Modifier modifier = (fun->isArrow() && asyncKind == FunctionAsyncKind::SyncFunction)
                        ? TokenStream::Operand
                        : TokenStream::None;
    if (!tokenStream.peekTokenPos(&pn->pn_pos, modifier))
        return null();

    YieldHandling yieldHandling = GetYieldHandling(generatorKind);
    FunctionSyntaxKind syntaxKind = Statement;
    if (fun->isClassConstructor())
        syntaxKind = ClassConstructor;
    else if (fun->isMethod())
        syntaxKind = Method;
    else if (fun->isGetter())
        syntaxKind = Getter;
    else if (fun->isSetter())
        syntaxKind = Setter;
    else if (fun->isArrow())
        syntaxKind = Arrow;

    if (!functionFormalParametersAndBody(InAllowed, yieldHandling, &pn, syntaxKind)) {
        MOZ_ASSERT(directives == newDirectives);
        return null();
    }

    if (!FoldConstants(context, &pn, this))
        return null();

    return pn;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::functionFormalParametersAndBody(InHandling inHandling,
                                                                    YieldHandling yieldHandling,
                                                                    Node* pn,
                                                                    FunctionSyntaxKind kind,
                                                                    const Maybe<uint32_t>& parameterListEnd /* = Nothing() */,
                                                                    bool isStandaloneFunction /* = false */)
{
    // Given a properly initialized parse context, try to parse an actual
    // function without concern for conversion to strict mode, use of lazy
    // parsing and such.

    FunctionBox* funbox = pc->functionBox();
    RootedFunction fun(context, funbox->function());

    // See below for an explanation why arrow function parameters and arrow
    // function bodies are parsed with different yield/await settings.
    {
        AwaitHandling awaitHandling = funbox->isAsync() || (kind == Arrow && awaitIsKeyword())
                                      ? AwaitIsKeyword
                                      : AwaitIsName;
        AutoAwaitIsKeyword<ParseHandler, CharT> awaitIsKeyword(this, awaitHandling);
        if (!functionArguments(yieldHandling, kind, *pn))
            return false;
    }

    Maybe<ParseContext::VarScope> varScope;
    if (funbox->hasParameterExprs) {
        varScope.emplace(this);
        if (!varScope->init(pc))
            return false;
    } else {
        pc->functionScope().useAsVarScope(pc);
    }

    if (kind == Arrow) {
        bool matched;
        if (!tokenStream.matchToken(&matched, TokenKind::Arrow))
            return false;
        if (!matched) {
            error(JSMSG_BAD_ARROW_ARGS);
            return false;
        }
    }

    // When parsing something for new Function() we have to make sure to
    // only treat a certain part of the source as a parameter list.
    if (parameterListEnd.isSome() && parameterListEnd.value() != pos().begin) {
        error(JSMSG_UNEXPECTED_PARAMLIST_END);
        return false;
    }

    // Parse the function body.
    FunctionBodyType bodyType = StatementListBody;
    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return false;
    uint32_t openedPos = 0;
    if (tt != TokenKind::Lc) {
        if (kind != Arrow) {
            if (funbox->isGenerator() || funbox->isAsync() || kind == Method ||
                kind == GetterNoExpressionClosure || kind == SetterNoExpressionClosure ||
                IsConstructorKind(kind) || kind == PrimaryExpression)
            {
                error(JSMSG_CURLY_BEFORE_BODY);
                return false;
            }

            if (allowExpressionClosures()) {
                this->addTelemetry(DeprecatedLanguageExtension::ExpressionClosure);
                if (!warnOnceAboutExprClosure())
                    return false;
                handler.noteExpressionClosure(pn);
            } else {
                error(JSMSG_CURLY_BEFORE_BODY);
                return false;
            }
        }

        anyChars.ungetToken();
        bodyType = ExpressionBody;
        funbox->setIsExprBody();
    } else {
        openedPos = pos().begin;
    }

    // Arrow function parameters inherit yieldHandling from the enclosing
    // context, but the arrow body doesn't. E.g. in |(a = yield) => yield|,
    // |yield| in the parameters is either a name or keyword, depending on
    // whether the arrow function is enclosed in a generator function or not.
    // Whereas the |yield| in the function body is always parsed as a name.
    // The same goes when parsing |await| in arrow functions.
    YieldHandling bodyYieldHandling = GetYieldHandling(pc->generatorKind());
    AwaitHandling bodyAwaitHandling = GetAwaitHandling(pc->asyncKind());
    bool inheritedStrict = pc->sc()->strict();
    Node body;
    {
        AutoAwaitIsKeyword<ParseHandler, CharT> awaitIsKeyword(this, bodyAwaitHandling);
        body = functionBody(inHandling, bodyYieldHandling, kind, bodyType);
        if (!body)
            return false;
    }

    // Revalidate the function name when we transitioned to strict mode.
    if ((kind == Statement || IsFunctionExpression(kind)) &&
        fun->explicitName() &&
        !inheritedStrict &&
        pc->sc()->strict())
    {
        MOZ_ASSERT(pc->sc()->hasExplicitUseStrict(),
                   "strict mode should only change when a 'use strict' directive is present");

        PropertyName* propertyName = fun->explicitName()->asPropertyName();
        YieldHandling nameYieldHandling;
        if (IsFunctionExpression(kind)) {
            // Named lambda has binding inside it.
            nameYieldHandling = bodyYieldHandling;
        } else {
            // Otherwise YieldHandling cannot be checked at this point
            // because of different context.
            // It should already be checked before this point.
            nameYieldHandling = YieldIsName;
        }

        // We already use the correct await-handling at this point, therefore
        // we don't need call AutoAwaitIsKeyword here.

        uint32_t nameOffset = handler.getFunctionNameOffset(*pn, anyChars);
        if (!checkBindingIdentifier(propertyName, nameOffset, nameYieldHandling))
            return false;
    }

    if (bodyType == StatementListBody) {
        MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rc, TokenStream::Operand,
                                         reportMissingClosing(JSMSG_CURLY_AFTER_BODY,
                                                              JSMSG_CURLY_OPENED, openedPos));
        funbox->setEnd(anyChars);
    } else {
        MOZ_ASSERT_IF(!allowExpressionClosures(), kind == Arrow);

        if (anyChars.hadError())
            return false;
        funbox->setEnd(anyChars);
        if (kind == Statement && !matchOrInsertSemicolon())
            return false;
    }

    if (IsMethodDefinitionKind(kind) && pc->superScopeNeedsHomeObject())
        funbox->setNeedsHomeObject();

    if (!finishFunction(isStandaloneFunction))
        return false;

    handler.setEndPosition(body, pos().begin);
    handler.setEndPosition(*pn, pos().end);
    handler.setFunctionBody(*pn, body);

    return true;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::functionStmt(uint32_t toStringStart,
                                                 YieldHandling yieldHandling,
                                                 DefaultHandling defaultHandling,
                                                 FunctionAsyncKind asyncKind)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Function));

    // In sloppy mode, Annex B.3.2 allows labelled function declarations.
    // Otherwise it's a parse error.
    ParseContext::Statement* declaredInStmt = pc->innermostStatement();
    if (declaredInStmt && declaredInStmt->kind() == StatementKind::Label) {
        MOZ_ASSERT(!pc->sc()->strict(),
                   "labeled functions shouldn't be parsed in strict mode");

        // Find the innermost non-label statement.  Report an error if it's
        // unbraced: functions can't appear in it.  Otherwise the statement
        // (or its absence) determines the scope the function's bound in.
        while (declaredInStmt && declaredInStmt->kind() == StatementKind::Label)
            declaredInStmt = declaredInStmt->enclosing();

        if (declaredInStmt && !StatementKindIsBraced(declaredInStmt->kind())) {
            error(JSMSG_SLOPPY_FUNCTION_LABEL);
            return null();
        }
    }

    TokenKind tt;
    if (!tokenStream.getToken(&tt))
        return null();

    GeneratorKind generatorKind = GeneratorKind::NotGenerator;
    if (tt == TokenKind::Mul) {
        generatorKind = GeneratorKind::Generator;
        if (!tokenStream.getToken(&tt))
            return null();
    }

    RootedPropertyName name(context);
    if (TokenKindIsPossibleIdentifier(tt)) {
        name = bindingIdentifier(yieldHandling);
        if (!name)
            return null();
    } else if (defaultHandling == AllowDefaultName) {
        name = context->names().default_;
        anyChars.ungetToken();
    } else {
        /* Unnamed function expressions are forbidden in statement context. */
        error(JSMSG_UNNAMED_FUNCTION_STMT);
        return null();
    }

    // Note the declared name and check for early errors.
    DeclarationKind kind;
    if (declaredInStmt) {
        MOZ_ASSERT(declaredInStmt->kind() != StatementKind::Label);
        MOZ_ASSERT(StatementKindIsBraced(declaredInStmt->kind()));

        kind = (!pc->sc()->strict() &&
                generatorKind == GeneratorKind::NotGenerator &&
                asyncKind == FunctionAsyncKind::SyncFunction)
                ? DeclarationKind::SloppyLexicalFunction
                : DeclarationKind::LexicalFunction;
    } else {
        kind = pc->atModuleLevel()
               ? DeclarationKind::ModuleBodyLevelFunction
               : DeclarationKind::BodyLevelFunction;
    }

    if (!noteDeclaredName(name, kind, pos()))
        return null();

    Node pn = handler.newFunctionStatement(pos());
    if (!pn)
        return null();

    // Under sloppy mode, try Annex B.3.3 semantics. If making an additional
    // 'var' binding of the same name does not throw an early error, do so.
    // This 'var' binding would be assigned the function object when its
    // declaration is reached, not at the start of the block.
    //
    // This semantics is implemented upon Scope exit in
    // Scope::propagateAndMarkAnnexBFunctionBoxes.
    bool tryAnnexB = kind == DeclarationKind::SloppyLexicalFunction;

    YieldHandling newYieldHandling = GetYieldHandling(generatorKind);
    return functionDefinition(pn, toStringStart, InAllowed, newYieldHandling, name, Statement,
                              generatorKind, asyncKind, tryAnnexB);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::functionExpr(uint32_t toStringStart,
                                                 ExpressionClosure expressionClosureHandling,
                                                 InvokedPrediction invoked,
                                                 FunctionAsyncKind asyncKind)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Function));

    AutoAwaitIsKeyword<ParseHandler, CharT> awaitIsKeyword(this, GetAwaitHandling(asyncKind));
    GeneratorKind generatorKind = GeneratorKind::NotGenerator;
    TokenKind tt;
    if (!tokenStream.getToken(&tt))
        return null();

    if (tt == TokenKind::Mul) {
        generatorKind = GeneratorKind::Generator;
        if (!tokenStream.getToken(&tt))
            return null();
    }

    YieldHandling yieldHandling = GetYieldHandling(generatorKind);

    RootedPropertyName name(context);
    if (TokenKindIsPossibleIdentifier(tt)) {
        name = bindingIdentifier(yieldHandling);
        if (!name)
            return null();
    } else {
        anyChars.ungetToken();
    }

    Node pn = handler.newFunctionExpression(pos());
    if (!pn)
        return null();

    if (invoked)
        pn = handler.setLikelyIIFE(pn);

    FunctionSyntaxKind kind = expressionClosureHandling == ExpressionClosure::Allowed
                              ? AssignmentExpression
                              : PrimaryExpression;

    return functionDefinition(pn, toStringStart, InAllowed, yieldHandling, name, kind,
                              generatorKind, asyncKind);
}

/*
 * Return true if this node, known to be an unparenthesized string literal,
 * could be the string of a directive in a Directive Prologue. Directive
 * strings never contain escape sequences or line continuations.
 * isEscapeFreeStringLiteral, below, checks whether the node itself could be
 * a directive.
 */
static inline bool
IsEscapeFreeStringLiteral(const TokenPos& pos, JSAtom* str)
{
    /*
     * If the string's length in the source code is its length as a value,
     * accounting for the quotes, then it must not contain any escape
     * sequences or line continuations.
     */
    return pos.begin + str->length() + 2 == pos.end;
}

template <typename CharT>
bool
Parser<SyntaxParseHandler, CharT>::asmJS(Node list)
{
    // While asm.js could technically be validated and compiled during syntax
    // parsing, we have no guarantee that some later JS wouldn't abort the
    // syntax parse and cause us to re-parse (and re-compile) the asm.js module.
    // For simplicity, unconditionally abort the syntax parse when "use asm" is
    // encountered so that asm.js is always validated/compiled exactly once
    // during a full parse.
    JS_ALWAYS_FALSE(abortIfSyntaxParser());

    // Record that the current script source constains some AsmJS, to disable
    // any incremental encoder, as AsmJS cannot be encoded with XDR at the
    // moment.
    if (ss)
        ss->setContainsAsmJS();
    return false;
}

template <typename CharT>
bool
Parser<FullParseHandler, CharT>::asmJS(Node list)
{
    // Disable syntax parsing in anything nested inside the asm.js module.
    disableSyntaxParser();

    // We should be encountering the "use asm" directive for the first time; if
    // the directive is already, we must have failed asm.js validation and we're
    // reparsing. In that case, don't try to validate again. A non-null
    // newDirectives means we're not in a normal function.
    if (!pc->newDirectives || pc->newDirectives->asmJS())
        return true;

    // If there is no ScriptSource, then we are doing a non-compiling parse and
    // so we shouldn't (and can't, without a ScriptSource) compile.
    if (ss == nullptr)
        return true;

    ss->setContainsAsmJS();
    pc->functionBox()->useAsm = true;

    // Attempt to validate and compile this asm.js module. On success, the
    // tokenStream has been advanced to the closing }. On failure, the
    // tokenStream is in an indeterminate state and we must reparse the
    // function from the beginning. Reparsing is triggered by marking that a
    // new directive has been encountered and returning 'false'.
    bool validated;
    if (!CompileAsmJS(context, *this, list, &validated))
        return false;
    if (!validated) {
        pc->newDirectives->setAsmJS();
        return false;
    }

    return true;
}

template <class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::asmJS(Node list)
{
    return asFinalParser()->asmJS(list);
}

/*
 * Recognize Directive Prologue members and directives. Assuming |pn| is a
 * candidate for membership in a directive prologue, recognize directives and
 * set |pc|'s flags accordingly. If |pn| is indeed part of a prologue, set its
 * |pn_prologue| flag.
 *
 * Note that the following is a strict mode function:
 *
 * function foo() {
 *   "blah" // inserted semi colon
 *        "blurgh"
 *   "use\x20loose"
 *   "use strict"
 * }
 *
 * That is, even though "use\x20loose" can never be a directive, now or in the
 * future (because of the hex escape), the Directive Prologue extends through it
 * to the "use strict" statement, which is indeed a directive.
 */
template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::maybeParseDirective(Node list, Node possibleDirective,
                                                        bool* cont)
{
    TokenPos directivePos;
    JSAtom* directive = handler.isStringExprStatement(possibleDirective, &directivePos);

    *cont = !!directive;
    if (!*cont)
        return true;

    if (IsEscapeFreeStringLiteral(directivePos, directive)) {
        // Mark this statement as being a possibly legitimate part of a
        // directive prologue, so the bytecode emitter won't warn about it being
        // useless code. (We mustn't just omit the statement entirely yet, as it
        // could be producing the value of an eval or JSScript execution.)
        //
        // Note that even if the string isn't one we recognize as a directive,
        // the emitter still shouldn't flag it as useless, as it could become a
        // directive in the future. We don't want to interfere with people
        // taking advantage of directive-prologue-enabled features that appear
        // in other browsers first.
        handler.setInDirectivePrologue(possibleDirective);

        if (directive == context->names().useStrict) {
            // Functions with non-simple parameter lists (destructuring,
            // default or rest parameters) must not contain a "use strict"
            // directive.
            if (pc->isFunctionBox()) {
                FunctionBox* funbox = pc->functionBox();
                if (!funbox->hasSimpleParameterList()) {
                    const char* parameterKind = funbox->hasDestructuringArgs
                                                ? "destructuring"
                                                : funbox->hasParameterExprs
                                                ? "default"
                                                : "rest";
                    errorAt(directivePos.begin, JSMSG_STRICT_NON_SIMPLE_PARAMS, parameterKind);
                    return false;
                }
            }

            // We're going to be in strict mode. Note that this scope explicitly
            // had "use strict";
            pc->sc()->setExplicitUseStrict();
            if (!pc->sc()->strict()) {
                // We keep track of the one possible strict violation that could
                // occur in the directive prologue -- octal escapes -- and
                // complain now.
                if (anyChars.sawOctalEscape()) {
                    error(JSMSG_DEPRECATED_OCTAL);
                    return false;
                }
                pc->sc()->strictScript = true;
            }
        } else if (directive == context->names().useAsm) {
            if (pc->isFunctionBox())
                return asmJS(list);
            return warningAt(directivePos.begin, JSMSG_USE_ASM_DIRECTIVE_FAIL);
        }
    }
    return true;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::statementList(YieldHandling yieldHandling)
{
    if (!CheckRecursionLimit(context))
        return null();

    Node pn = handler.newStatementList(pos());
    if (!pn)
        return null();

    bool canHaveDirectives = pc->atBodyLevel();
    if (canHaveDirectives)
        anyChars.clearSawOctalEscape();
    bool afterReturn = false;
    bool warnedAboutStatementsAfterReturn = false;
    uint32_t statementBegin = 0;
    for (;;) {
        TokenKind tt = TokenKind::Eof;
        if (!tokenStream.peekToken(&tt, TokenStream::Operand)) {
            if (anyChars.isEOF())
                isUnexpectedEOF_ = true;
            return null();
        }
        if (tt == TokenKind::Eof || tt == TokenKind::Rc) {
            TokenPos pos;
            if (!tokenStream.peekTokenPos(&pos, TokenStream::Operand)) {
                return null();
            }
            handler.setListEndPosition(pn, pos);
            break;
        }
        if (afterReturn) {
            if (!tokenStream.peekOffset(&statementBegin, TokenStream::Operand))
                return null();
        }
        Node next = statementListItem(yieldHandling, canHaveDirectives);
        if (!next) {
            if (anyChars.isEOF())
                isUnexpectedEOF_ = true;
            return null();
        }
        if (!warnedAboutStatementsAfterReturn) {
            if (afterReturn) {
                if (!handler.isStatementPermittedAfterReturnStatement(next)) {
                    if (!warningAt(statementBegin, JSMSG_STMT_AFTER_RETURN))
                        return null();

                    warnedAboutStatementsAfterReturn = true;
                }
            } else if (handler.isReturnStatement(next)) {
                afterReturn = true;
            }
        }

        if (canHaveDirectives) {
            if (!maybeParseDirective(pn, next, &canHaveDirectives))
                return null();
        }

        handler.addStatementToList(pn, next);
    }

    return pn;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::condition(InHandling inHandling, YieldHandling yieldHandling)
{
    MUST_MATCH_TOKEN(TokenKind::Lp, JSMSG_PAREN_BEFORE_COND);

    Node pn = exprInParens(inHandling, yieldHandling, TripledotProhibited);
    if (!pn)
        return null();

    MUST_MATCH_TOKEN_MOD(TokenKind::Rp, TokenStream::Operand, JSMSG_PAREN_AFTER_COND);

    /* Check for (a = b) and warn about possible (a == b) mistype. */
    if (handler.isUnparenthesizedAssignment(pn)) {
        if (!extraWarning(JSMSG_EQUAL_AS_ASSIGN))
            return null();
    }
    return pn;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::matchLabel(YieldHandling yieldHandling,
                                               MutableHandle<PropertyName*> label)
{
    TokenKind tt = TokenKind::Eof;
    if (!tokenStream.peekTokenSameLine(&tt, TokenStream::Operand))
        return false;

    if (TokenKindIsPossibleIdentifier(tt)) {
        tokenStream.consumeKnownToken(tt, TokenStream::Operand);

        label.set(labelIdentifier(yieldHandling));
        if (!label)
            return false;
    } else {
        label.set(nullptr);
    }
    return true;
}

template <class ParseHandler, typename CharT>
GeneralParser<ParseHandler, CharT>::PossibleError::PossibleError(GeneralParser<ParseHandler, CharT>& parser)
  : parser_(parser)
{}

template <class ParseHandler, typename CharT>
typename GeneralParser<ParseHandler, CharT>::PossibleError::Error&
GeneralParser<ParseHandler, CharT>::PossibleError::error(ErrorKind kind)
{
    if (kind == ErrorKind::Expression)
        return exprError_;
    if (kind == ErrorKind::Destructuring)
        return destructuringError_;
    MOZ_ASSERT(kind == ErrorKind::DestructuringWarning);
    return destructuringWarning_;
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::PossibleError::setResolved(ErrorKind kind)
{
    error(kind).state_ = ErrorState::None;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::PossibleError::hasError(ErrorKind kind)
{
    return error(kind).state_ == ErrorState::Pending;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::PossibleError::hasPendingDestructuringError()
{
    return hasError(ErrorKind::Destructuring);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::PossibleError::setPending(ErrorKind kind, const TokenPos& pos,
                                                              unsigned errorNumber)
{
    // Don't overwrite a previously recorded error.
    if (hasError(kind))
        return;

    // If we report an error later, we'll do it from the position where we set
    // the state to pending.
    Error& err = error(kind);
    err.offset_ = pos.begin;
    err.errorNumber_ = errorNumber;
    err.state_ = ErrorState::Pending;
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::PossibleError::setPendingDestructuringErrorAt(const TokenPos& pos,
                                                                                  unsigned errorNumber)
{
    setPending(ErrorKind::Destructuring, pos, errorNumber);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::PossibleError::setPendingDestructuringWarningAt(const TokenPos& pos,
                                                                                    unsigned errorNumber)
{
    setPending(ErrorKind::DestructuringWarning, pos, errorNumber);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::PossibleError::setPendingExpressionErrorAt(const TokenPos& pos,
                                                                               unsigned errorNumber)
{
    setPending(ErrorKind::Expression, pos, errorNumber);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::PossibleError::checkForError(ErrorKind kind)
{
    if (!hasError(kind))
        return true;

    Error& err = error(kind);
    parser_.errorAt(err.offset_, err.errorNumber_);
    return false;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::PossibleError::checkForWarning(ErrorKind kind)
{
    if (!hasError(kind))
        return true;

    Error& err = error(kind);
    return parser_.extraWarningAt(err.offset_, err.errorNumber_);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::PossibleError::checkForDestructuringErrorOrWarning()
{
    // Clear pending expression error, because we're definitely not in an
    // expression context.
    setResolved(ErrorKind::Expression);

    // Report any pending destructuring error or warning.
    return checkForError(ErrorKind::Destructuring) &&
           checkForWarning(ErrorKind::DestructuringWarning);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::PossibleError::checkForExpressionError()
{
    // Clear pending destructuring error or warning, because we're definitely
    // not in a destructuring context.
    setResolved(ErrorKind::Destructuring);
    setResolved(ErrorKind::DestructuringWarning);

    // Report any pending expression error.
    return checkForError(ErrorKind::Expression);
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::PossibleError::transferErrorTo(ErrorKind kind,
                                                                   PossibleError* other)
{
    if (hasError(kind) && !other->hasError(kind)) {
        Error& err = error(kind);
        Error& otherErr = other->error(kind);
        otherErr.offset_ = err.offset_;
        otherErr.errorNumber_ = err.errorNumber_;
        otherErr.state_ = err.state_;
    }
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::PossibleError::transferErrorsTo(PossibleError* other)
{
    MOZ_ASSERT(other);
    MOZ_ASSERT(this != other);
    MOZ_ASSERT(&parser_ == &other->parser_,
               "Can't transfer fields to an instance which belongs to a different parser");

    transferErrorTo(ErrorKind::Destructuring, other);
    transferErrorTo(ErrorKind::Expression, other);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::bindingInitializer(Node lhs, DeclarationKind kind,
                                                       YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Assign));

    if (kind == DeclarationKind::FormalParameter)
        pc->functionBox()->hasParameterExprs = true;

    Node rhs = assignExpr(InAllowed, yieldHandling, TripledotProhibited);
    if (!rhs)
        return null();

    handler.checkAndSetIsDirectRHSAnonFunction(rhs);

    Node assign = handler.newAssignment(ParseNodeKind::Assign, lhs, rhs);
    if (!assign)
        return null();

    if (foldConstants && !FoldConstants(context, &assign, this))
        return null();

    return assign;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::bindingIdentifier(DeclarationKind kind,
                                                      YieldHandling yieldHandling)
{
    RootedPropertyName name(context, bindingIdentifier(yieldHandling));
    if (!name)
        return null();

    Node binding = newName(name);
    if (!binding || !noteDeclaredName(name, kind, pos()))
        return null();

    return binding;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::bindingIdentifierOrPattern(DeclarationKind kind,
                                                               YieldHandling yieldHandling,
                                                               TokenKind tt)
{
    if (tt == TokenKind::Lb)
        return arrayBindingPattern(kind, yieldHandling);

    if (tt == TokenKind::Lc)
        return objectBindingPattern(kind, yieldHandling);

    if (!TokenKindIsPossibleIdentifierName(tt)) {
        error(JSMSG_NO_VARIABLE_NAME);
        return null();
    }

    return bindingIdentifier(kind, yieldHandling);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::objectBindingPattern(DeclarationKind kind,
                                                         YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lc));

    if (!CheckRecursionLimit(context))
        return null();

    uint32_t begin = pos().begin;
    Node literal = handler.newObjectLiteral(begin);
    if (!literal)
        return null();

    Maybe<DeclarationKind> declKind = Some(kind);
    RootedAtom propAtom(context);
    for (;;) {
        TokenKind tt;
        if (!tokenStream.peekToken(&tt))
            return null();
        if (tt == TokenKind::Rc) {
            anyChars.addModifierException(TokenStream::OperandIsNone);
            break;
        }

        if (tt == TokenKind::TripleDot) {
            tokenStream.consumeKnownToken(TokenKind::TripleDot);
            uint32_t begin = pos().begin;

            TokenKind tt;
            if (!tokenStream.getToken(&tt))
                return null();

            if (!TokenKindIsPossibleIdentifierName(tt)) {
                error(JSMSG_NO_VARIABLE_NAME);
                return null();
            }

            Node inner = bindingIdentifier(kind, yieldHandling);
            if (!inner)
                return null();

            if (!handler.addSpreadProperty(literal, begin, inner))
                return null();
        } else {
            TokenPos namePos = anyChars.nextToken().pos;

            PropertyType propType;
            Node propName = propertyName(yieldHandling, declKind, literal, &propType, &propAtom);
            if (!propName)
                return null();

            if (propType == PropertyType::Normal) {
                // Handle e.g., |var {p: x} = o| and |var {p: x=0} = o|.

                if (!tokenStream.getToken(&tt, TokenStream::Operand))
                    return null();

                Node binding = bindingIdentifierOrPattern(kind, yieldHandling, tt);
                if (!binding)
                    return null();

                bool hasInitializer;
                if (!tokenStream.matchToken(&hasInitializer, TokenKind::Assign, TokenStream::Operand))
                    return null();

                Node bindingExpr = hasInitializer
                                   ? bindingInitializer(binding, kind, yieldHandling)
                                   : binding;
                if (!bindingExpr)
                    return null();

                if (!handler.addPropertyDefinition(literal, propName, bindingExpr))
                    return null();
            } else if (propType == PropertyType::Shorthand) {
                // Handle e.g., |var {x, y} = o| as destructuring shorthand
                // for |var {x: x, y: y} = o|.
                MOZ_ASSERT(TokenKindIsPossibleIdentifierName(tt));

                Node binding = bindingIdentifier(kind, yieldHandling);
                if (!binding)
                    return null();

                if (!handler.addShorthand(literal, propName, binding))
                    return null();
            } else if (propType == PropertyType::CoverInitializedName) {
                // Handle e.g., |var {x=1, y=2} = o| as destructuring
                // shorthand with default values.
                MOZ_ASSERT(TokenKindIsPossibleIdentifierName(tt));

                Node binding = bindingIdentifier(kind, yieldHandling);
                if (!binding)
                    return null();

                tokenStream.consumeKnownToken(TokenKind::Assign);

                Node bindingExpr = bindingInitializer(binding, kind, yieldHandling);
                if (!bindingExpr)
                    return null();

                if (!handler.addPropertyDefinition(literal, propName, bindingExpr))
                    return null();
            } else {
                errorAt(namePos.begin, JSMSG_NO_VARIABLE_NAME);
                return null();
            }
        }

        bool matched;
        if (!tokenStream.matchToken(&matched, TokenKind::Comma, TokenStream::Operand))
            return null();
        if (!matched)
            break;
        if (tt == TokenKind::TripleDot) {
            error(JSMSG_REST_WITH_COMMA);
            return null();
        }
    }

    MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rc, TokenStream::Operand,
                                     reportMissingClosing(JSMSG_CURLY_AFTER_LIST,
                                                          JSMSG_CURLY_OPENED, begin));

    handler.setEndPosition(literal, pos().end);
    return literal;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::arrayBindingPattern(DeclarationKind kind,
                                                        YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lb));

    if (!CheckRecursionLimit(context))
        return null();

    uint32_t begin = pos().begin;
    Node literal = handler.newArrayLiteral(begin);
    if (!literal)
        return null();

     uint32_t index = 0;
     for (; ; index++) {
         if (index >= NativeObject::MAX_DENSE_ELEMENTS_COUNT) {
             error(JSMSG_ARRAY_INIT_TOO_BIG);
             return null();
         }

         TokenKind tt;
         if (!tokenStream.getToken(&tt))
             return null();

         if (tt == TokenKind::Rb) {
             anyChars.ungetToken();
             anyChars.addModifierException(TokenStream::OperandIsNone);
             break;
         }

         if (tt == TokenKind::Comma) {
             if (!handler.addElision(literal, pos()))
                 return null();
         } else if (tt == TokenKind::TripleDot) {
             uint32_t begin = pos().begin;

             TokenKind tt;
             if (!tokenStream.getToken(&tt))
                 return null();

             Node inner = bindingIdentifierOrPattern(kind, yieldHandling, tt);
             if (!inner)
                 return null();

             if (!handler.addSpreadElement(literal, begin, inner))
                 return null();
         } else {
             Node binding = bindingIdentifierOrPattern(kind, yieldHandling, tt);
             if (!binding)
                 return null();

             bool hasInitializer;
             if (!tokenStream.matchToken(&hasInitializer, TokenKind::Assign, TokenStream::Operand))
                 return null();

             Node element = hasInitializer
                            ? bindingInitializer(binding, kind, yieldHandling)
                            : binding;
             if (!element)
                 return null();

             handler.addArrayElement(literal, element);
         }

         if (tt != TokenKind::Comma) {
             // If we didn't already match TokenKind::Comma in above case.
             bool matched;
             if (!tokenStream.matchToken(&matched, TokenKind::Comma, TokenStream::Operand))
                 return null();
             if (!matched)
                 break;

             if (tt == TokenKind::TripleDot) {
                 error(JSMSG_REST_WITH_COMMA);
                 return null();
             }
         }
     }

     MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rb, TokenStream::Operand,
                                      reportMissingClosing(JSMSG_BRACKET_AFTER_LIST,
                                                           JSMSG_BRACKET_OPENED, begin));

    handler.setEndPosition(literal, pos().end);
    return literal;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::destructuringDeclaration(DeclarationKind kind,
                                                             YieldHandling yieldHandling,
                                                             TokenKind tt)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(tt));
    MOZ_ASSERT(tt == TokenKind::Lb || tt == TokenKind::Lc);

    return tt == TokenKind::Lb
           ? arrayBindingPattern(kind, yieldHandling)
           : objectBindingPattern(kind, yieldHandling);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::destructuringDeclarationWithoutYieldOrAwait(DeclarationKind kind,
                                                                                YieldHandling yieldHandling,
                                                                                TokenKind tt)
{
    uint32_t startYieldOffset = pc->lastYieldOffset;
    uint32_t startAwaitOffset = pc->lastAwaitOffset;
    Node res = destructuringDeclaration(kind, yieldHandling, tt);
    if (res) {
        if (pc->lastYieldOffset != startYieldOffset) {
            errorAt(pc->lastYieldOffset, JSMSG_YIELD_IN_DEFAULT);
            return null();
        }
        if (pc->lastAwaitOffset != startAwaitOffset) {
            errorAt(pc->lastAwaitOffset, JSMSG_AWAIT_IN_DEFAULT);
            return null();
        }
    }
    return res;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::blockStatement(YieldHandling yieldHandling,
                                                   unsigned errorNumber)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lc));
    uint32_t openedPos = pos().begin;

    ParseContext::Statement stmt(pc, StatementKind::Block);
    ParseContext::Scope scope(this);
    if (!scope.init(pc))
        return null();

    Node list = statementList(yieldHandling);
    if (!list)
        return null();

    MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rc, TokenStream::Operand,
                                     reportMissingClosing(errorNumber, JSMSG_CURLY_OPENED,
                                                          openedPos));

    return finishLexicalScope(scope, list);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::expressionAfterForInOrOf(ParseNodeKind forHeadKind,
                                                             YieldHandling yieldHandling)
{
    MOZ_ASSERT(forHeadKind == ParseNodeKind::ForIn || forHeadKind == ParseNodeKind::ForOf);
    Node pn = forHeadKind == ParseNodeKind::ForOf
           ? assignExpr(InAllowed, yieldHandling, TripledotProhibited)
           : expr(InAllowed, yieldHandling, TripledotProhibited);
    return pn;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::declarationPattern(DeclarationKind declKind, TokenKind tt,
                                                       bool initialDeclaration,
                                                       YieldHandling yieldHandling,
                                                       ParseNodeKind* forHeadKind,
                                                       Node* forInOrOfExpression)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lb) ||
               anyChars.isCurrentTokenType(TokenKind::Lc));

    Node pattern = destructuringDeclaration(declKind, yieldHandling, tt);
    if (!pattern)
        return null();

    if (initialDeclaration && forHeadKind) {
        bool isForIn, isForOf;
        if (!matchInOrOf(&isForIn, &isForOf))
            return null();

        if (isForIn) {
            *forHeadKind = ParseNodeKind::ForIn;
        } else if (isForOf) {
            *forHeadKind = ParseNodeKind::ForOf;

            // Annex B.3.5 has different early errors for vars in for-of loops.
            if (declKind == DeclarationKind::Var)
                declKind = DeclarationKind::ForOfVar;
        } else {
            *forHeadKind = ParseNodeKind::ForHead;
        }

        if (*forHeadKind != ParseNodeKind::ForHead) {
            *forInOrOfExpression = expressionAfterForInOrOf(*forHeadKind, yieldHandling);
            if (!*forInOrOfExpression)
                return null();

            return pattern;
        }
    }

    MUST_MATCH_TOKEN_MOD(TokenKind::Assign, TokenStream::Operand, JSMSG_BAD_DESTRUCT_DECL);

    Node init = assignExpr(forHeadKind ? InProhibited : InAllowed,
                           yieldHandling, TripledotProhibited);
    if (!init)
        return null();

    handler.checkAndSetIsDirectRHSAnonFunction(init);

    return handler.newAssignment(ParseNodeKind::Assign, pattern, init);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::initializerInNameDeclaration(Node binding,
                                                                 DeclarationKind declKind,
                                                                 bool initialDeclaration,
                                                                 YieldHandling yieldHandling,
                                                                 ParseNodeKind* forHeadKind,
                                                                 Node* forInOrOfExpression)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Assign));

    uint32_t initializerOffset;
    if (!tokenStream.peekOffset(&initializerOffset, TokenStream::Operand))
        return false;

    Node initializer = assignExpr(forHeadKind ? InProhibited : InAllowed,
                                  yieldHandling, TripledotProhibited);
    if (!initializer)
        return false;

    handler.checkAndSetIsDirectRHSAnonFunction(initializer);

    if (forHeadKind && initialDeclaration) {
        bool isForIn, isForOf;
        if (!matchInOrOf(&isForIn, &isForOf))
            return false;

        // An initialized declaration can't appear in a for-of:
        //
        //   for (var/let/const x = ... of ...); // BAD
        if (isForOf) {
            errorAt(initializerOffset, JSMSG_OF_AFTER_FOR_LOOP_DECL);
            return false;
        }

        if (isForIn) {
            // Lexical declarations in for-in loops can't be initialized:
            //
            //   for (let/const x = ... in ...); // BAD
            if (DeclarationKindIsLexical(declKind)) {
                errorAt(initializerOffset, JSMSG_IN_AFTER_LEXICAL_FOR_DECL);
                return false;
            }

            // This leaves only initialized for-in |var| declarations.  ES6
            // forbids these; later ES un-forbids in non-strict mode code.
            *forHeadKind = ParseNodeKind::ForIn;
            if (!strictModeErrorAt(initializerOffset, JSMSG_INVALID_FOR_IN_DECL_WITH_INIT))
                return false;

            *forInOrOfExpression = expressionAfterForInOrOf(ParseNodeKind::ForIn, yieldHandling);
            if (!*forInOrOfExpression)
                return false;
        } else {
            *forHeadKind = ParseNodeKind::ForHead;
        }
    }

    return handler.finishInitializerAssignment(binding, initializer);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::declarationName(DeclarationKind declKind, TokenKind tt,
                                                    bool initialDeclaration,
                                                    YieldHandling yieldHandling,
                                                    ParseNodeKind* forHeadKind,
                                                    Node* forInOrOfExpression)
{
    // Anything other than possible identifier is an error.
    if (!TokenKindIsPossibleIdentifier(tt)) {
        error(JSMSG_NO_VARIABLE_NAME);
        return null();
    }

    RootedPropertyName name(context, bindingIdentifier(yieldHandling));
    if (!name)
        return null();

    Node binding = newName(name);
    if (!binding)
        return null();

    TokenPos namePos = pos();

    // The '=' context after a variable name in a declaration is an opportunity
    // for ASI, and thus for the next token to start an ExpressionStatement:
    //
    //  var foo   // VariableDeclaration
    //  /bar/g;   // ExpressionStatement
    //
    // Therefore get the token here as Operand.
    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Assign, TokenStream::Operand))
        return null();

    if (matched) {
        if (!initializerInNameDeclaration(binding, declKind, initialDeclaration,
                                          yieldHandling, forHeadKind, forInOrOfExpression))
        {
            return null();
        }
    } else {
        if (initialDeclaration && forHeadKind) {
            bool isForIn, isForOf;
            if (!matchInOrOf(&isForIn, &isForOf))
                return null();

            if (isForIn) {
                *forHeadKind = ParseNodeKind::ForIn;
            } else if (isForOf) {
                *forHeadKind = ParseNodeKind::ForOf;

                // Annex B.3.5 has different early errors for vars in for-of loops.
                if (declKind == DeclarationKind::Var)
                    declKind = DeclarationKind::ForOfVar;
            } else {
                *forHeadKind = ParseNodeKind::ForHead;
            }
        }

        if (forHeadKind && *forHeadKind != ParseNodeKind::ForHead) {
            *forInOrOfExpression = expressionAfterForInOrOf(*forHeadKind, yieldHandling);
            if (!*forInOrOfExpression)
                return null();
        } else {
            // Normal const declarations, and const declarations in for(;;)
            // heads, must be initialized.
            if (declKind == DeclarationKind::Const) {
                errorAt(namePos.begin, JSMSG_BAD_CONST_DECL);
                return null();
            }
        }
    }

    // Note the declared name after knowing whether or not we are in a for-of
    // loop, due to special early error semantics in Annex B.3.5.
    if (!noteDeclaredName(name, declKind, namePos))
        return null();

    return binding;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::declarationList(YieldHandling yieldHandling,
                                                    ParseNodeKind kind,
                                                    ParseNodeKind* forHeadKind /* = nullptr */,
                                                    Node* forInOrOfExpression /* = nullptr */)
{
    MOZ_ASSERT(kind == ParseNodeKind::Var ||
               kind == ParseNodeKind::Let ||
               kind == ParseNodeKind::Const);

    DeclarationKind declKind;
    switch (kind) {
      case ParseNodeKind::Var:
        declKind = DeclarationKind::Var;
        break;
      case ParseNodeKind::Const:
        declKind = DeclarationKind::Const;
        break;
      case ParseNodeKind::Let:
        declKind = DeclarationKind::Let;
        break;
      default:
        MOZ_CRASH("Unknown declaration kind");
    }

    Node decl = handler.newDeclarationList(kind, pos());
    if (!decl)
        return null();

    bool moreDeclarations;
    bool initialDeclaration = true;
    do {
        MOZ_ASSERT_IF(!initialDeclaration && forHeadKind,
                      *forHeadKind == ParseNodeKind::ForHead);

        TokenKind tt;
        if (!tokenStream.getToken(&tt))
            return null();

        Node binding = (tt == TokenKind::Lb || tt == TokenKind::Lc)
                       ? declarationPattern(declKind, tt, initialDeclaration, yieldHandling,
                                            forHeadKind, forInOrOfExpression)
                       : declarationName(declKind, tt, initialDeclaration, yieldHandling,
                                         forHeadKind, forInOrOfExpression);
        if (!binding)
            return null();

        handler.addList(decl, binding);

        // If we have a for-in/of loop, the above call matches the entirety
        // of the loop head (up to the closing parenthesis).
        if (forHeadKind && *forHeadKind != ParseNodeKind::ForHead)
            break;

        initialDeclaration = false;

        if (!tokenStream.matchToken(&moreDeclarations, TokenKind::Comma, TokenStream::Operand))
            return null();
    } while (moreDeclarations);

    return decl;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::lexicalDeclaration(YieldHandling yieldHandling,
                                                       DeclarationKind kind)
{
    MOZ_ASSERT(kind == DeclarationKind::Const || kind == DeclarationKind::Let);

    /*
     * Parse body-level lets without a new block object. ES6 specs
     * that an execution environment's initial lexical environment
     * is the VariableEnvironment, i.e., body-level lets are in
     * the same environment record as vars.
     *
     * However, they cannot be parsed exactly as vars, as ES6
     * requires that uninitialized lets throw ReferenceError on use.
     *
     * See 8.1.1.1.6 and the note in 13.2.1.
     */
    Node decl = declarationList(yieldHandling,
                                kind == DeclarationKind::Const
                                ? ParseNodeKind::Const
                                : ParseNodeKind::Let);
    if (!decl || !matchOrInsertSemicolon())
        return null();

    return decl;
}

template <typename CharT>
bool
Parser<FullParseHandler, CharT>::namedImportsOrNamespaceImport(TokenKind tt, Node importSpecSet)
{
    if (tt == TokenKind::Lc) {
        while (true) {
            // Handle the forms |import {} from 'a'| and
            // |import { ..., } from 'a'| (where ... is non empty), by
            // escaping the loop early if the next token is }.
            if (!tokenStream.getToken(&tt))
                return false;

            if (tt == TokenKind::Rc)
                break;

            if (!TokenKindIsPossibleIdentifierName(tt)) {
                error(JSMSG_NO_IMPORT_NAME);
                return false;
            }

            Rooted<PropertyName*> importName(context, anyChars.currentName());
            TokenPos importNamePos = pos();

            bool matched;
            if (!tokenStream.matchToken(&matched, TokenKind::As))
                return null();

            if (matched) {
                TokenKind afterAs;
                if (!tokenStream.getToken(&afterAs))
                    return false;

                if (!TokenKindIsPossibleIdentifierName(afterAs)) {
                    error(JSMSG_NO_BINDING_NAME);
                    return false;
                }
            } else {
                // Keywords cannot be bound to themselves, so an import name
                // that is a keyword is a syntax error if it is not followed
                // by the keyword 'as'.
                // See the ImportSpecifier production in ES6 section 15.2.2.
                if (IsKeyword(importName)) {
                    error(JSMSG_AS_AFTER_RESERVED_WORD, ReservedWordToCharZ(importName));
                    return false;
                }
            }

            RootedPropertyName bindingAtom(context, importedBinding());
            if (!bindingAtom)
                return false;

            Node bindingName = newName(bindingAtom);
            if (!bindingName)
                return false;
            if (!noteDeclaredName(bindingAtom, DeclarationKind::Import, pos()))
                return false;

            Node importNameNode = newName(importName, importNamePos);
            if (!importNameNode)
                return false;

            Node importSpec = handler.newImportSpec(importNameNode, bindingName);
            if (!importSpec)
                return false;

            handler.addList(importSpecSet, importSpec);

            TokenKind next;
            if (!tokenStream.getToken(&next))
                return false;

            if (next == TokenKind::Rc)
                break;

            if (next != TokenKind::Comma) {
                error(JSMSG_RC_AFTER_IMPORT_SPEC_LIST);
                return false;
            }
        }
    } else {
        MOZ_ASSERT(tt == TokenKind::Mul);

        MUST_MATCH_TOKEN(TokenKind::As, JSMSG_AS_AFTER_IMPORT_STAR);

        MUST_MATCH_TOKEN_FUNC(TokenKindIsPossibleIdentifierName, JSMSG_NO_BINDING_NAME);

        Node importName = newName(context->names().star);
        if (!importName)
            return false;

        // Namespace imports are are not indirect bindings but lexical
        // definitions that hold a module namespace object. They are treated
        // as const variables which are initialized during the
        // ModuleInstantiate step.
        RootedPropertyName bindingName(context, importedBinding());
        if (!bindingName)
            return false;
        Node bindingNameNode = newName(bindingName);
        if (!bindingNameNode)
            return false;
        if (!noteDeclaredName(bindingName, DeclarationKind::Const, pos()))
            return false;

        // The namespace import name is currently required to live on the
        // environment.
        pc->varScope().lookupDeclaredName(bindingName)->value()->setClosedOver();

        Node importSpec = handler.newImportSpec(importName, bindingNameNode);
        if (!importSpec)
            return false;

        handler.addList(importSpecSet, importSpec);
    }

    return true;
}

template<typename CharT>
ParseNode*
Parser<FullParseHandler, CharT>::importDeclaration()
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Import));

    if (!pc->atModuleLevel()) {
        error(JSMSG_IMPORT_DECL_AT_TOP_LEVEL);
        return null();
    }

    uint32_t begin = pos().begin;
    TokenKind tt;
    if (!tokenStream.getToken(&tt))
        return null();

    Node importSpecSet = handler.newList(ParseNodeKind::ImportSpecList, pos());
    if (!importSpecSet)
        return null();

    if (tt == TokenKind::String) {
        // Handle the form |import 'a'| by leaving the list empty. This is
        // equivalent to |import {} from 'a'|.
        importSpecSet->pn_pos.end = importSpecSet->pn_pos.begin;
    } else {
        if (tt == TokenKind::Lc || tt == TokenKind::Mul) {
            if (!namedImportsOrNamespaceImport(tt, importSpecSet))
                return null();
        } else if (TokenKindIsPossibleIdentifierName(tt)) {
            // Handle the form |import a from 'b'|, by adding a single import
            // specifier to the list, with 'default' as the import name and
            // 'a' as the binding name. This is equivalent to
            // |import { default as a } from 'b'|.
            Node importName = newName(context->names().default_);
            if (!importName)
                return null();

            RootedPropertyName bindingAtom(context, importedBinding());
            if (!bindingAtom)
                return null();

            Node bindingName = newName(bindingAtom);
            if (!bindingName)
                return null();

            if (!noteDeclaredName(bindingAtom, DeclarationKind::Import, pos()))
                return null();

            Node importSpec = handler.newImportSpec(importName, bindingName);
            if (!importSpec)
                return null();

            handler.addList(importSpecSet, importSpec);

            if (!tokenStream.peekToken(&tt))
                return null();

            if (tt == TokenKind::Comma) {
                tokenStream.consumeKnownToken(tt);
                if (!tokenStream.getToken(&tt))
                    return null();

                if (tt != TokenKind::Lc && tt != TokenKind::Mul) {
                    error(JSMSG_NAMED_IMPORTS_OR_NAMESPACE_IMPORT);
                    return null();
                }

                if (!namedImportsOrNamespaceImport(tt, importSpecSet))
                    return null();
            }
        } else {
            error(JSMSG_DECLARATION_AFTER_IMPORT);
            return null();
        }

        MUST_MATCH_TOKEN(TokenKind::From, JSMSG_FROM_AFTER_IMPORT_CLAUSE);

        MUST_MATCH_TOKEN(TokenKind::String, JSMSG_MODULE_SPEC_AFTER_FROM);
    }

    Node moduleSpec = stringLiteral();
    if (!moduleSpec)
        return null();

    if (!matchOrInsertSemicolon())
        return null();

    ParseNode* node =
        handler.newImportDeclaration(importSpecSet, moduleSpec, TokenPos(begin, pos().end));
    if (!node || !pc->sc()->asModuleContext()->builder.processImport(node))
        return null();

    return node;
}

template<typename CharT>
inline SyntaxParseHandler::Node
Parser<SyntaxParseHandler, CharT>::importDeclaration()
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return SyntaxParseHandler::NodeFailure;
}

template <class ParseHandler, typename CharT>
inline typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::importDeclaration()
{
    return asFinalParser()->importDeclaration();
}

template<typename CharT>
bool
Parser<FullParseHandler, CharT>::checkExportedName(JSAtom* exportName)
{
    if (!pc->sc()->asModuleContext()->builder.hasExportedName(exportName))
        return true;

    JSAutoByteString str;
    if (!AtomToPrintableString(context, exportName, &str))
        return false;

    error(JSMSG_DUPLICATE_EXPORT_NAME, str.ptr());
    return false;
}

template<typename CharT>
inline bool
Parser<SyntaxParseHandler, CharT>::checkExportedName(JSAtom* exportName)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkExportedName(JSAtom* exportName)
{
    return asFinalParser()->checkExportedName(exportName);
}

template<typename CharT>
bool
Parser<FullParseHandler, CharT>::checkExportedNamesForArrayBinding(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Array));
    MOZ_ASSERT(pn->isArity(PN_LIST));

    for (ParseNode* node = pn->pn_head; node; node = node->pn_next) {
        if (node->isKind(ParseNodeKind::Elision))
            continue;

        ParseNode* binding;
        if (node->isKind(ParseNodeKind::Spread))
            binding = node->pn_kid;
        else if (node->isKind(ParseNodeKind::Assign))
            binding = node->pn_left;
        else
            binding = node;

        if (!checkExportedNamesForDeclaration(binding))
            return false;
    }

    return true;
}

template<typename CharT>
inline bool
Parser<SyntaxParseHandler, CharT>::checkExportedNamesForArrayBinding(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkExportedNamesForArrayBinding(Node node)
{
    return asFinalParser()->checkExportedNamesForArrayBinding(node);
}

template<typename CharT>
bool
Parser<FullParseHandler, CharT>::checkExportedNamesForObjectBinding(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Object));
    MOZ_ASSERT(pn->isArity(PN_LIST));

    for (ParseNode* node = pn->pn_head; node; node = node->pn_next) {
        MOZ_ASSERT(node->isKind(ParseNodeKind::MutateProto) ||
                   node->isKind(ParseNodeKind::Colon) ||
                   node->isKind(ParseNodeKind::Shorthand));

        ParseNode* target = node->isKind(ParseNodeKind::MutateProto)
            ? node->pn_kid
            : node->pn_right;

        if (target->isKind(ParseNodeKind::Assign))
            target = target->pn_left;

        if (!checkExportedNamesForDeclaration(target))
            return false;
    }

    return true;
}

template<typename CharT>
inline bool
Parser<SyntaxParseHandler, CharT>::checkExportedNamesForObjectBinding(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkExportedNamesForObjectBinding(Node node)
{
    return asFinalParser()->checkExportedNamesForObjectBinding(node);
}

template<typename CharT>
bool
Parser<FullParseHandler, CharT>::checkExportedNamesForDeclaration(ParseNode* node)
{
    if (node->isKind(ParseNodeKind::Name)) {
        if (!checkExportedName(node->pn_atom))
            return false;
    } else if (node->isKind(ParseNodeKind::Array)) {
        if (!checkExportedNamesForArrayBinding(node))
            return false;
    } else {
        MOZ_ASSERT(node->isKind(ParseNodeKind::Object));
        if (!checkExportedNamesForObjectBinding(node))
            return false;
    }

    return true;
}

template<typename CharT>
inline bool
Parser<SyntaxParseHandler, CharT>::checkExportedNamesForDeclaration(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkExportedNamesForDeclaration(Node node)
{
    return asFinalParser()->checkExportedNamesForDeclaration(node);
}

template<typename CharT>
bool
Parser<FullParseHandler, CharT>::checkExportedNamesForDeclarationList(ParseNode* node)
{
    MOZ_ASSERT(node->isArity(PN_LIST));
    for (ParseNode* binding = node->pn_head; binding; binding = binding->pn_next) {
        if (binding->isKind(ParseNodeKind::Assign))
            binding = binding->pn_left;
        else
            MOZ_ASSERT(binding->isKind(ParseNodeKind::Name));

        if (!checkExportedNamesForDeclaration(binding))
            return false;
    }

    return true;
}

template<typename CharT>
inline bool
Parser<SyntaxParseHandler, CharT>::checkExportedNamesForDeclarationList(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkExportedNamesForDeclarationList(Node node)
{
    return asFinalParser()->checkExportedNamesForDeclarationList(node);
}

template<typename CharT>
inline bool
Parser<FullParseHandler, CharT>::checkExportedNameForClause(ParseNode* node)
{
    return checkExportedName(node->pn_atom);
}

template<typename CharT>
inline bool
Parser<SyntaxParseHandler, CharT>::checkExportedNameForClause(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkExportedNameForClause(Node node)
{
    return asFinalParser()->checkExportedNameForClause(node);
}

template<typename CharT>
bool
Parser<FullParseHandler, CharT>::checkExportedNameForFunction(ParseNode* node)
{
    return checkExportedName(node->pn_funbox->function()->explicitName());
}

template<typename CharT>
inline bool
Parser<SyntaxParseHandler, CharT>::checkExportedNameForFunction(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkExportedNameForFunction(Node node)
{
    return asFinalParser()->checkExportedNameForFunction(node);
}

template<typename CharT>
bool
Parser<FullParseHandler, CharT>::checkExportedNameForClass(ParseNode* node)
{
    const ClassNode& cls = node->as<ClassNode>();
    MOZ_ASSERT(cls.names());
    return checkExportedName(cls.names()->innerBinding()->pn_atom);
}

template<typename CharT>
inline bool
Parser<SyntaxParseHandler, CharT>::checkExportedNameForClass(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkExportedNameForClass(Node node)
{
    return asFinalParser()->checkExportedNameForClass(node);
}

template<>
inline bool
PerHandlerParser<FullParseHandler>::processExport(ParseNode* node)
{
    return pc->sc()->asModuleContext()->builder.processExport(node);
}

template<>
inline bool
PerHandlerParser<SyntaxParseHandler>::processExport(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<>
inline bool
PerHandlerParser<FullParseHandler>::processExportFrom(ParseNode* node)
{
    return pc->sc()->asModuleContext()->builder.processExportFrom(node);
}

template<>
inline bool
PerHandlerParser<SyntaxParseHandler>::processExportFrom(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportFrom(uint32_t begin, Node specList)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::From));

    if (!abortIfSyntaxParser())
        return null();

    MUST_MATCH_TOKEN(TokenKind::String, JSMSG_MODULE_SPEC_AFTER_FROM);

    Node moduleSpec = stringLiteral();
    if (!moduleSpec)
        return null();

    if (!matchOrInsertSemicolon())
        return null();

    Node node = handler.newExportFromDeclaration(begin, specList, moduleSpec);
    if (!node)
        return null();

    if (!processExportFrom(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportBatch(uint32_t begin)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Mul));

    Node kid = handler.newList(ParseNodeKind::ExportSpecList, pos());
    if (!kid)
        return null();

    // Handle the form |export *| by adding a special export batch
    // specifier to the list.
    Node exportSpec = handler.newExportBatchSpec(pos());
    if (!exportSpec)
        return null();

    handler.addList(kid, exportSpec);

    MUST_MATCH_TOKEN(TokenKind::From, JSMSG_FROM_AFTER_EXPORT_STAR);

    return exportFrom(begin, kid);
}

template<typename CharT>
bool
Parser<FullParseHandler, CharT>::checkLocalExportNames(ParseNode* node)
{
    // ES 2017 draft 15.2.3.1.
    for (ParseNode* next = node->pn_head; next; next = next->pn_next) {
        ParseNode* name = next->pn_left;
        MOZ_ASSERT(name->isKind(ParseNodeKind::Name));

        RootedPropertyName ident(context, name->pn_atom->asPropertyName());
        if (!checkLocalExportName(ident, name->pn_pos.begin))
            return false;
    }

    return true;
}

template<typename CharT>
bool
Parser<SyntaxParseHandler, CharT>::checkLocalExportNames(Node node)
{
    MOZ_ALWAYS_FALSE(abortIfSyntaxParser());
    return false;
}

template<class ParseHandler, typename CharT>
inline bool
GeneralParser<ParseHandler, CharT>::checkLocalExportNames(Node node)
{
    return asFinalParser()->checkLocalExportNames(node);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportClause(uint32_t begin)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lc));

    Node kid = handler.newList(ParseNodeKind::ExportSpecList, pos());
    if (!kid)
        return null();

    TokenKind tt;
    while (true) {
        // Handle the forms |export {}| and |export { ..., }| (where ... is non
        // empty), by escaping the loop early if the next token is }.
        if (!tokenStream.getToken(&tt))
            return null();

        if (tt == TokenKind::Rc)
            break;

        if (!TokenKindIsPossibleIdentifierName(tt)) {
            error(JSMSG_NO_BINDING_NAME);
            return null();
        }

        Node bindingName = newName(anyChars.currentName());
        if (!bindingName)
            return null();

        bool foundAs;
        if (!tokenStream.matchToken(&foundAs, TokenKind::As))
            return null();
        if (foundAs)
            MUST_MATCH_TOKEN_FUNC(TokenKindIsPossibleIdentifierName, JSMSG_NO_EXPORT_NAME);

        Node exportName = newName(anyChars.currentName());
        if (!exportName)
            return null();

        if (!checkExportedNameForClause(exportName))
            return null();

        Node exportSpec = handler.newExportSpec(bindingName, exportName);
        if (!exportSpec)
            return null();

        handler.addList(kid, exportSpec);

        TokenKind next;
        if (!tokenStream.getToken(&next))
            return null();

        if (next == TokenKind::Rc)
            break;

        if (next != TokenKind::Comma) {
            error(JSMSG_RC_AFTER_EXPORT_SPEC_LIST);
            return null();
        }
    }

    // Careful!  If |from| follows, even on a new line, it must start a
    // FromClause:
    //
    //   export { x }
    //   from "foo"; // a single ExportDeclaration
    //
    // But if it doesn't, we might have an ASI opportunity in Operand context:
    //
    //   export { x }   // ExportDeclaration, terminated by ASI
    //   fro\u006D      // ExpressionStatement, the name "from"
    //
    // In that case let matchOrInsertSemicolon sort out ASI or any necessary
    // error.
    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::From, TokenStream::Operand))
        return null();

    if (matched)
        return exportFrom(begin, kid);

    if (!matchOrInsertSemicolon())
        return null();

    if (!checkLocalExportNames(kid))
        return null();

    Node node = handler.newExportDeclaration(kid, TokenPos(begin, pos().end));
    if (!node)
        return null();

    if (!processExport(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportVariableStatement(uint32_t begin)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Var));

    Node kid = declarationList(YieldIsName, ParseNodeKind::Var);
    if (!kid)
        return null();
    if (!matchOrInsertSemicolon())
        return null();
    if (!checkExportedNamesForDeclarationList(kid))
        return null();

    Node node = handler.newExportDeclaration(kid, TokenPos(begin, pos().end));
    if (!node)
        return null();

    if (!processExport(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportFunctionDeclaration(uint32_t begin,
                                                              uint32_t toStringStart,
                                                              FunctionAsyncKind asyncKind /* = SyncFunction */)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Function));

    Node kid = functionStmt(toStringStart, YieldIsName, NameRequired, asyncKind);
    if (!kid)
        return null();

    if (!checkExportedNameForFunction(kid))
        return null();

    Node node = handler.newExportDeclaration(kid, TokenPos(begin, pos().end));
    if (!node)
        return null();

    if (!processExport(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportClassDeclaration(uint32_t begin)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Class));

    Node kid = classDefinition(YieldIsName, ClassStatement, NameRequired);
    if (!kid)
        return null();

    if (!checkExportedNameForClass(kid))
        return null();

    Node node = handler.newExportDeclaration(kid, TokenPos(begin, pos().end));
    if (!node)
        return null();

    if (!processExport(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportLexicalDeclaration(uint32_t begin, DeclarationKind kind)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(kind == DeclarationKind::Const || kind == DeclarationKind::Let);
    MOZ_ASSERT_IF(kind == DeclarationKind::Const, anyChars.isCurrentTokenType(TokenKind::Const));
    MOZ_ASSERT_IF(kind == DeclarationKind::Let, anyChars.isCurrentTokenType(TokenKind::Let));

    Node kid = lexicalDeclaration(YieldIsName, kind);
    if (!kid)
        return null();
    if (!checkExportedNamesForDeclarationList(kid))
        return null();

    Node node = handler.newExportDeclaration(kid, TokenPos(begin, pos().end));
    if (!node)
        return null();

    if (!processExport(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportDefaultFunctionDeclaration(uint32_t begin,
                                                                     uint32_t toStringStart,
                                                                     FunctionAsyncKind asyncKind /* = SyncFunction */)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Function));

    Node kid = functionStmt(toStringStart, YieldIsName, AllowDefaultName, asyncKind);
    if (!kid)
        return null();

    Node node = handler.newExportDefaultDeclaration(kid, null(), TokenPos(begin, pos().end));
    if (!node)
        return null();

    if (!processExport(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportDefaultClassDeclaration(uint32_t begin)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Class));

    Node kid = classDefinition(YieldIsName, ClassStatement, AllowDefaultName);
    if (!kid)
        return null();

    Node node = handler.newExportDefaultDeclaration(kid, null(), TokenPos(begin, pos().end));
    if (!node)
        return null();

    if (!processExport(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportDefaultAssignExpr(uint32_t begin)
{
    if (!abortIfSyntaxParser())
        return null();

    HandlePropertyName name = context->names().default_;
    Node nameNode = newName(name);
    if (!nameNode)
        return null();
    if (!noteDeclaredName(name, DeclarationKind::Const, pos()))
        return null();

    Node kid = assignExpr(InAllowed, YieldIsName, TripledotProhibited);
    if (!kid)
        return null();

    handler.checkAndSetIsDirectRHSAnonFunction(kid);

    if (!matchOrInsertSemicolon())
        return null();

    Node node = handler.newExportDefaultDeclaration(kid, nameNode, TokenPos(begin, pos().end));
    if (!node)
        return null();

    if (!processExport(node))
        return null();

    return node;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportDefault(uint32_t begin)
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Default));

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();

    if (!checkExportedName(context->names().default_))
        return null();

    switch (tt) {
      case TokenKind::Function:
        return exportDefaultFunctionDeclaration(begin, pos().begin);

      case TokenKind::Async: {
        TokenKind nextSameLine = TokenKind::Eof;
        if (!tokenStream.peekTokenSameLine(&nextSameLine))
            return null();

        if (nextSameLine == TokenKind::Function) {
            uint32_t toStringStart = pos().begin;
            tokenStream.consumeKnownToken(TokenKind::Function);
            return exportDefaultFunctionDeclaration(begin, toStringStart,
                                                    FunctionAsyncKind::AsyncFunction);
        }

        anyChars.ungetToken();
        return exportDefaultAssignExpr(begin);
      }

      case TokenKind::Class:
        return exportDefaultClassDeclaration(begin);

      default:
        anyChars.ungetToken();
        return exportDefaultAssignExpr(begin);
    }
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exportDeclaration()
{
    if (!abortIfSyntaxParser())
        return null();

    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Export));

    if (!pc->atModuleLevel()) {
        error(JSMSG_EXPORT_DECL_AT_TOP_LEVEL);
        return null();
    }

    uint32_t begin = pos().begin;

    TokenKind tt;
    if (!tokenStream.getToken(&tt))
        return null();
    switch (tt) {
      case TokenKind::Mul:
        return exportBatch(begin);

      case TokenKind::Lc:
        return exportClause(begin);

      case TokenKind::Var:
        return exportVariableStatement(begin);

      case TokenKind::Function:
        return exportFunctionDeclaration(begin, pos().begin);

      case TokenKind::Async: {
        TokenKind nextSameLine = TokenKind::Eof;
        if (!tokenStream.peekTokenSameLine(&nextSameLine))
            return null();

        if (nextSameLine == TokenKind::Function) {
            uint32_t toStringStart = pos().begin;
            tokenStream.consumeKnownToken(TokenKind::Function);
            return exportFunctionDeclaration(begin, toStringStart,
                                             FunctionAsyncKind::AsyncFunction);
        }

        error(JSMSG_DECLARATION_AFTER_EXPORT);
        return null();
      }

      case TokenKind::Class:
        return exportClassDeclaration(begin);

      case TokenKind::Const:
        return exportLexicalDeclaration(begin, DeclarationKind::Const);

      case TokenKind::Let:
        return exportLexicalDeclaration(begin, DeclarationKind::Let);

      case TokenKind::Default:
        return exportDefault(begin);

      default:
        error(JSMSG_DECLARATION_AFTER_EXPORT);
        return null();
    }
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::expressionStatement(YieldHandling yieldHandling,
                                                        InvokedPrediction invoked)
{
    anyChars.ungetToken();
    Node pnexpr = expr(InAllowed, yieldHandling, TripledotProhibited,
                       /* possibleError = */ nullptr, invoked);
    if (!pnexpr)
        return null();
    if (!matchOrInsertSemicolon())
        return null();
    return handler.newExprStatement(pnexpr, pos().end);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::consequentOrAlternative(YieldHandling yieldHandling)
{
    TokenKind next;
    if (!tokenStream.peekToken(&next, TokenStream::Operand))
        return null();

    // Annex B.3.4 says that unbraced FunctionDeclarations under if/else in
    // non-strict code act as if they were braced: |if (x) function f() {}|
    // parses as |if (x) { function f() {} }|.
    //
    // Careful!  FunctionDeclaration doesn't include generators or async
    // functions.
    if (next == TokenKind::Function) {
        tokenStream.consumeKnownToken(next, TokenStream::Operand);

        // Parser::statement would handle this, but as this function handles
        // every other error case, it seems best to handle this.
        if (pc->sc()->strict()) {
            error(JSMSG_FORBIDDEN_AS_STATEMENT, "function declarations");
            return null();
        }

        TokenKind maybeStar;
        if (!tokenStream.peekToken(&maybeStar))
            return null();

        if (maybeStar == TokenKind::Mul) {
            error(JSMSG_FORBIDDEN_AS_STATEMENT, "generator declarations");
            return null();
        }

        ParseContext::Statement stmt(pc, StatementKind::Block);
        ParseContext::Scope scope(this);
        if (!scope.init(pc))
            return null();

        TokenPos funcPos = pos();
        Node fun = functionStmt(pos().begin, yieldHandling, NameRequired);
        if (!fun)
            return null();

        Node block = handler.newStatementList(funcPos);
        if (!block)
            return null();

        handler.addStatementToList(block, fun);
        return finishLexicalScope(scope, block);
    }

    return statement(yieldHandling);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::ifStatement(YieldHandling yieldHandling)
{
    Vector<Node, 4> condList(context), thenList(context);
    Vector<uint32_t, 4> posList(context);
    Node elseBranch;

    ParseContext::Statement stmt(pc, StatementKind::If);

    while (true) {
        uint32_t begin = pos().begin;

        /* An IF node has three kids: condition, then, and optional else. */
        Node cond = condition(InAllowed, yieldHandling);
        if (!cond)
            return null();

        TokenKind tt;
        if (!tokenStream.peekToken(&tt, TokenStream::Operand))
            return null();
        if (tt == TokenKind::Semi) {
            if (!extraWarning(JSMSG_EMPTY_CONSEQUENT))
                return null();
        }

        Node thenBranch = consequentOrAlternative(yieldHandling);
        if (!thenBranch)
            return null();

        if (!condList.append(cond) || !thenList.append(thenBranch) || !posList.append(begin))
            return null();

        bool matched;
        if (!tokenStream.matchToken(&matched, TokenKind::Else, TokenStream::Operand))
            return null();
        if (matched) {
            if (!tokenStream.matchToken(&matched, TokenKind::If, TokenStream::Operand))
                return null();
            if (matched)
                continue;
            elseBranch = consequentOrAlternative(yieldHandling);
            if (!elseBranch)
                return null();
        } else {
            elseBranch = null();
        }
        break;
    }

    for (int i = condList.length() - 1; i >= 0; i--) {
        elseBranch = handler.newIfStatement(posList[i], condList[i], thenList[i], elseBranch);
        if (!elseBranch)
            return null();
    }

    return elseBranch;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::doWhileStatement(YieldHandling yieldHandling)
{
    uint32_t begin = pos().begin;
    ParseContext::Statement stmt(pc, StatementKind::DoLoop);
    Node body = statement(yieldHandling);
    if (!body)
        return null();
    MUST_MATCH_TOKEN_MOD(TokenKind::While, TokenStream::Operand, JSMSG_WHILE_AFTER_DO);
    Node cond = condition(InAllowed, yieldHandling);
    if (!cond)
        return null();

    // The semicolon after do-while is even more optional than most
    // semicolons in JS.  Web compat required this by 2004:
    //   http://bugzilla.mozilla.org/show_bug.cgi?id=238945
    // ES3 and ES5 disagreed, but ES6 conforms to Web reality:
    //   https://bugs.ecmascript.org/show_bug.cgi?id=157
    // To parse |do {} while (true) false| correctly, use Operand.
    bool ignored;
    if (!tokenStream.matchToken(&ignored, TokenKind::Semi, TokenStream::Operand))
        return null();
    return handler.newDoWhileStatement(body, cond, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::whileStatement(YieldHandling yieldHandling)
{
    uint32_t begin = pos().begin;
    ParseContext::Statement stmt(pc, StatementKind::WhileLoop);
    Node cond = condition(InAllowed, yieldHandling);
    if (!cond)
        return null();
    Node body = statement(yieldHandling);
    if (!body)
        return null();
    return handler.newWhileStatement(begin, cond, body);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::matchInOrOf(bool* isForInp, bool* isForOfp)
{
    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return false;

    *isForInp = tt == TokenKind::In;
    *isForOfp = tt == TokenKind::Of;
    if (!*isForInp && !*isForOfp)
        anyChars.ungetToken();

    MOZ_ASSERT_IF(*isForInp || *isForOfp, *isForInp != *isForOfp);
    return true;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::forHeadStart(YieldHandling yieldHandling,
                                                 ParseNodeKind* forHeadKind, Node* forInitialPart,
                                                 Maybe<ParseContext::Scope>& forLoopLexicalScope,
                                                 Node* forInOrOfExpression)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lp));

    TokenKind tt;
    if (!tokenStream.peekToken(&tt, TokenStream::Operand))
        return null();

    // Super-duper easy case: |for (;| is a C-style for-loop with no init
    // component.
    if (tt == TokenKind::Semi) {
        *forInitialPart = null();
        *forHeadKind = ParseNodeKind::ForHead;
        return true;
    }

    // Parsing after |for (var| is also relatively simple (from this method's
    // point of view).  No block-related work complicates matters, so delegate
    // to Parser::declaration.
    if (tt == TokenKind::Var) {
        tokenStream.consumeKnownToken(tt, TokenStream::Operand);

        // Pass null for block object because |var| declarations don't use one.
        *forInitialPart = declarationList(yieldHandling, ParseNodeKind::Var, forHeadKind,
                                          forInOrOfExpression);
        return *forInitialPart != null();
    }

    // Otherwise we have a lexical declaration or an expression.

    // For-in loop backwards compatibility requires that |let| starting a
    // for-loop that's not a (new to ES6) for-of loop, in non-strict mode code,
    // parse as an identifier.  (|let| in for-of is always a declaration.)
    bool parsingLexicalDeclaration = false;
    bool letIsIdentifier = false;
    if (tt == TokenKind::Const) {
        parsingLexicalDeclaration = true;
        tokenStream.consumeKnownToken(tt, TokenStream::Operand);
    } else if (tt == TokenKind::Let) {
        // We could have a {For,Lexical}Declaration, or we could have a
        // LeftHandSideExpression with lookahead restrictions so it's not
        // ambiguous with the former.  Check for a continuation of the former
        // to decide which we have.
        tokenStream.consumeKnownToken(TokenKind::Let, TokenStream::Operand);

        TokenKind next;
        if (!tokenStream.peekToken(&next))
            return false;

        parsingLexicalDeclaration = nextTokenContinuesLetDeclaration(next);
        if (!parsingLexicalDeclaration) {
            anyChars.ungetToken();
            letIsIdentifier = true;
        }
    }

    if (parsingLexicalDeclaration) {
        forLoopLexicalScope.emplace(this);
        if (!forLoopLexicalScope->init(pc))
            return null();

        // Push a temporary ForLoopLexicalHead Statement that allows for
        // lexical declarations, as they are usually allowed only in braced
        // statements.
        ParseContext::Statement forHeadStmt(pc, StatementKind::ForLoopLexicalHead);

        *forInitialPart = declarationList(yieldHandling,
                                          tt == TokenKind::Const
                                          ? ParseNodeKind::Const
                                          : ParseNodeKind::Let,
                                          forHeadKind, forInOrOfExpression);
        return *forInitialPart != null();
    }

    uint32_t exprOffset;
    if (!tokenStream.peekOffset(&exprOffset, TokenStream::Operand))
        return false;

    // Finally, handle for-loops that start with expressions.  Pass
    // |InProhibited| so that |in| isn't parsed in a RelationalExpression as a
    // binary operator.  |in| makes it a for-in loop, *not* an |in| expression.
    PossibleError possibleError(*this);
    *forInitialPart = expr(InProhibited, yieldHandling, TripledotProhibited, &possibleError);
    if (!*forInitialPart)
        return false;

    bool isForIn, isForOf;
    if (!matchInOrOf(&isForIn, &isForOf))
        return false;

    // If we don't encounter 'in'/'of', we have a for(;;) loop.  We've handled
    // the init expression; the caller handles the rest.
    if (!isForIn && !isForOf) {
        if (!possibleError.checkForExpressionError())
            return false;

        *forHeadKind = ParseNodeKind::ForHead;
        return true;
    }

    MOZ_ASSERT(isForIn != isForOf);

    // In a for-of loop, 'let' that starts the loop head is a |let| keyword,
    // per the [lookahead ≠ let] restriction on the LeftHandSideExpression
    // variant of such loops.  Expressions that start with |let| can't be used
    // here.
    //
    //   var let = {};
    //   for (let.prop of [1]) // BAD
    //     break;
    //
    // See ES6 13.7.
    if (isForOf && letIsIdentifier) {
        errorAt(exprOffset, JSMSG_LET_STARTING_FOROF_LHS);
        return false;
    }

    *forHeadKind = isForIn ? ParseNodeKind::ForIn : ParseNodeKind::ForOf;

    // Verify the left-hand side expression doesn't have a forbidden form.
    if (handler.isUnparenthesizedDestructuringPattern(*forInitialPart)) {
        if (!possibleError.checkForDestructuringErrorOrWarning())
            return false;
    } else if (handler.isName(*forInitialPart)) {
        if (const char* chars = nameIsArgumentsOrEval(*forInitialPart)) {
            // |chars| is "arguments" or "eval" here.
            if (!strictModeErrorAt(exprOffset, JSMSG_BAD_STRICT_ASSIGN, chars))
                return false;
        }

        handler.adjustGetToSet(*forInitialPart);
    } else if (handler.isPropertyAccess(*forInitialPart)) {
        // Permitted: no additional testing/fixup needed.
    } else if (handler.isFunctionCall(*forInitialPart)) {
        if (!strictModeErrorAt(exprOffset, JSMSG_BAD_FOR_LEFTSIDE))
            return false;
    } else {
        errorAt(exprOffset, JSMSG_BAD_FOR_LEFTSIDE);
        return false;
    }

    if (!possibleError.checkForExpressionError())
        return false;

    // Finally, parse the iterated expression, making the for-loop's closing
    // ')' the next token.
    *forInOrOfExpression = expressionAfterForInOrOf(*forHeadKind, yieldHandling);
    return *forInOrOfExpression != null();
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::forStatement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::For));

    uint32_t begin = pos().begin;

    ParseContext::Statement stmt(pc, StatementKind::ForLoop);

    IteratorKind iterKind = IteratorKind::Sync;
    unsigned iflags = 0;

    if (pc->isAsync()) {
        bool matched;
        if (!tokenStream.matchToken(&matched, TokenKind::Await))
            return null();

        if (matched) {
            iflags |= JSITER_FORAWAITOF;
            iterKind = IteratorKind::Async;
        }
    }

    MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Lp, TokenStream::None,
                                     error((token == TokenKind::Await && !pc->isAsync())
                                           ? JSMSG_FOR_AWAIT_OUTSIDE_ASYNC
                                           : JSMSG_PAREN_AFTER_FOR));

    // ParseNodeKind::ForHead, ParseNodeKind::ForIn, or
    // ParseNodeKind::ForOf depending on the loop type.
    ParseNodeKind headKind;

    // |x| in either |for (x; ...; ...)| or |for (x in/of ...)|.
    Node startNode;

    // The next two variables are used to implement `for (let/const ...)`.
    //
    // We generate an implicit block, wrapping the whole loop, to store loop
    // variables declared this way. Note that if the loop uses `for (var...)`
    // instead, those variables go on some existing enclosing scope, so no
    // implicit block scope is created.
    //
    // Both variables remain null/none if the loop is any other form.

    // The static block scope for the implicit block scope.
    Maybe<ParseContext::Scope> forLoopLexicalScope;

    // The expression being iterated over, for for-in/of loops only.  Unused
    // for for(;;) loops.
    Node iteratedExpr;

    // Parse the entirety of the loop-head for a for-in/of loop (so the next
    // token is the closing ')'):
    //
    //   for (... in/of ...) ...
    //                     ^next token
    //
    // ...OR, parse up to the first ';' in a C-style for-loop:
    //
    //   for (...; ...; ...) ...
    //           ^next token
    //
    // In either case the subsequent token can be consistently accessed using
    // TokenStream::None semantics.
    if (!forHeadStart(yieldHandling, &headKind, &startNode, forLoopLexicalScope, &iteratedExpr))
        return null();

    MOZ_ASSERT(headKind == ParseNodeKind::ForIn ||
               headKind == ParseNodeKind::ForOf ||
               headKind == ParseNodeKind::ForHead);

    if (iterKind == IteratorKind::Async && headKind != ParseNodeKind::ForOf) {
        errorAt(begin, JSMSG_FOR_AWAIT_NOT_OF);
        return null();
    }

    Node forHead;
    if (headKind == ParseNodeKind::ForHead) {
        Node init = startNode;

        // Look for an operand: |for (;| means we might have already examined
        // this semicolon with that modifier.
        MUST_MATCH_TOKEN_MOD(TokenKind::Semi, TokenStream::Operand, JSMSG_SEMI_AFTER_FOR_INIT);

        TokenKind tt;
        if (!tokenStream.peekToken(&tt, TokenStream::Operand))
            return null();

        Node test;
        if (tt == TokenKind::Semi) {
            test = null();
        } else {
            test = expr(InAllowed, yieldHandling, TripledotProhibited);
            if (!test)
                return null();
        }

        MUST_MATCH_TOKEN_MOD(TokenKind::Semi, TokenStream::Operand, JSMSG_SEMI_AFTER_FOR_COND);

        if (!tokenStream.peekToken(&tt, TokenStream::Operand))
            return null();

        Node update;
        if (tt == TokenKind::Rp) {
            update = null();
        } else {
            update = expr(InAllowed, yieldHandling, TripledotProhibited);
            if (!update)
                return null();
        }

        MUST_MATCH_TOKEN_MOD(TokenKind::Rp, TokenStream::Operand, JSMSG_PAREN_AFTER_FOR_CTRL);

        TokenPos headPos(begin, pos().end);
        forHead = handler.newForHead(init, test, update, headPos);
        if (!forHead)
            return null();
    } else {
        MOZ_ASSERT(headKind == ParseNodeKind::ForIn || headKind == ParseNodeKind::ForOf);

        // |target| is the LeftHandSideExpression or declaration to which the
        // per-iteration value (an arbitrary value exposed by the iteration
        // protocol, or a string naming a property) is assigned.
        Node target = startNode;

        // Parse the rest of the for-in/of head.
        if (headKind == ParseNodeKind::ForIn)
            stmt.refineForKind(StatementKind::ForInLoop);
        else
            stmt.refineForKind(StatementKind::ForOfLoop);

        // Parser::declaration consumed everything up to the closing ')'.  That
        // token follows an {Assignment,}Expression and so must be interpreted
        // as an operand to be consistent with normal expression tokenizing.
        MUST_MATCH_TOKEN_MOD(TokenKind::Rp, TokenStream::Operand, JSMSG_PAREN_AFTER_FOR_CTRL);

        TokenPos headPos(begin, pos().end);
        forHead = handler.newForInOrOfHead(headKind, target, iteratedExpr, headPos);
        if (!forHead)
            return null();
    }

    Node body = statement(yieldHandling);
    if (!body)
        return null();

    Node forLoop = handler.newForStatement(begin, forHead, body, iflags);
    if (!forLoop)
        return null();

    if (forLoopLexicalScope)
        return finishLexicalScope(*forLoopLexicalScope, forLoop);

    return forLoop;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::switchStatement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Switch));
    uint32_t begin = pos().begin;

    MUST_MATCH_TOKEN(TokenKind::Lp, JSMSG_PAREN_BEFORE_SWITCH);

    Node discriminant = exprInParens(InAllowed, yieldHandling, TripledotProhibited);
    if (!discriminant)
        return null();

    MUST_MATCH_TOKEN_MOD(TokenKind::Rp, TokenStream::Operand, JSMSG_PAREN_AFTER_SWITCH);
    MUST_MATCH_TOKEN(TokenKind::Lc, JSMSG_CURLY_BEFORE_SWITCH);

    ParseContext::Statement stmt(pc, StatementKind::Switch);
    ParseContext::Scope scope(this);
    if (!scope.init(pc))
        return null();

    Node caseList = handler.newStatementList(pos());
    if (!caseList)
        return null();

    bool seenDefault = false;
    TokenKind tt;
    while (true) {
        if (!tokenStream.getToken(&tt, TokenStream::Operand))
            return null();
        if (tt == TokenKind::Rc)
            break;
        uint32_t caseBegin = pos().begin;

        Node caseExpr;
        switch (tt) {
          case TokenKind::Default:
            if (seenDefault) {
                error(JSMSG_TOO_MANY_DEFAULTS);
                return null();
            }
            seenDefault = true;
            caseExpr = null();  // The default case has pn_left == nullptr.
            break;

          case TokenKind::Case:
            caseExpr = expr(InAllowed, yieldHandling, TripledotProhibited);
            if (!caseExpr)
                return null();
            break;

          default:
            error(JSMSG_BAD_SWITCH);
            return null();
        }

        MUST_MATCH_TOKEN_MOD(TokenKind::Colon, TokenStream::Operand, JSMSG_COLON_AFTER_CASE);

        Node body = handler.newStatementList(pos());
        if (!body)
            return null();

        bool afterReturn = false;
        bool warnedAboutStatementsAfterReturn = false;
        uint32_t statementBegin = 0;
        while (true) {
            if (!tokenStream.peekToken(&tt, TokenStream::Operand))
                return null();
            if (tt == TokenKind::Rc || tt == TokenKind::Case || tt == TokenKind::Default)
                break;
            if (afterReturn) {
                if (!tokenStream.peekOffset(&statementBegin, TokenStream::Operand))
                    return null();
            }
            Node stmt = statementListItem(yieldHandling);
            if (!stmt)
                return null();
            if (!warnedAboutStatementsAfterReturn) {
                if (afterReturn) {
                    if (!handler.isStatementPermittedAfterReturnStatement(stmt)) {
                        if (!warningAt(statementBegin, JSMSG_STMT_AFTER_RETURN))
                            return null();

                        warnedAboutStatementsAfterReturn = true;
                    }
                } else if (handler.isReturnStatement(stmt)) {
                    afterReturn = true;
                }
            }
            handler.addStatementToList(body, stmt);
        }

        Node casepn = handler.newCaseOrDefault(caseBegin, caseExpr, body);
        if (!casepn)
            return null();
        handler.addCaseStatementToList(caseList, casepn);
    }

    caseList = finishLexicalScope(scope, caseList);
    if (!caseList)
        return null();

    handler.setEndPosition(caseList, pos().end);

    return handler.newSwitchStatement(begin, discriminant, caseList);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::continueStatement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Continue));
    uint32_t begin = pos().begin;

    RootedPropertyName label(context);
    if (!matchLabel(yieldHandling, &label))
        return null();

    auto validity = pc->checkContinueStatement(label);
    if (validity.isErr()) {
        switch (validity.unwrapErr()) {
          case ParseContext::ContinueStatementError::NotInALoop:
            errorAt(begin, JSMSG_BAD_CONTINUE);
            break;
          case ParseContext::ContinueStatementError::LabelNotFound:
            error(JSMSG_LABEL_NOT_FOUND);
            break;
        }
        return null();
    }

    if (!matchOrInsertSemicolon())
        return null();

    return handler.newContinueStatement(label, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::breakStatement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Break));
    uint32_t begin = pos().begin;

    RootedPropertyName label(context);
    if (!matchLabel(yieldHandling, &label))
        return null();

    // Labeled 'break' statements target the nearest labeled statements (could
    // be any kind) with the same label. Unlabeled 'break' statements target
    // the innermost loop or switch statement.
    if (label) {
        auto hasSameLabel = [&label](ParseContext::LabelStatement* stmt) {
            return stmt->label() == label;
        };

        if (!pc->template findInnermostStatement<ParseContext::LabelStatement>(hasSameLabel)) {
            error(JSMSG_LABEL_NOT_FOUND);
            return null();
        }
    } else {
        auto isBreakTarget = [](ParseContext::Statement* stmt) {
            return StatementKindIsUnlabeledBreakTarget(stmt->kind());
        };

        if (!pc->findInnermostStatement(isBreakTarget)) {
            errorAt(begin, JSMSG_TOUGH_BREAK);
            return null();
        }
    }

    if (!matchOrInsertSemicolon())
        return null();

    return handler.newBreakStatement(label, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::returnStatement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Return));
    uint32_t begin = pos().begin;

    MOZ_ASSERT(pc->isFunctionBox());
    pc->functionBox()->usesReturn = true;

    // Parse an optional operand.
    //
    // This is ugly, but we don't want to require a semicolon.
    Node exprNode;
    TokenKind tt = TokenKind::Eof;
    if (!tokenStream.peekTokenSameLine(&tt, TokenStream::Operand))
        return null();
    switch (tt) {
      case TokenKind::Eol:
      case TokenKind::Eof:
      case TokenKind::Semi:
      case TokenKind::Rc:
        exprNode = null();
        break;
      default: {
        exprNode = expr(InAllowed, yieldHandling, TripledotProhibited);
        if (!exprNode)
            return null();
      }
    }

    if (!matchOrInsertSemicolon())
        return null();

    return handler.newReturnStatement(exprNode, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::yieldExpression(InHandling inHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Yield));
    uint32_t begin = pos().begin;

    MOZ_ASSERT(pc->isGenerator());
    MOZ_ASSERT(pc->isFunctionBox());

    pc->lastYieldOffset = begin;

    Node exprNode;
    ParseNodeKind kind = ParseNodeKind::Yield;
    TokenKind tt = TokenKind::Eof;
    if (!tokenStream.peekTokenSameLine(&tt, TokenStream::Operand))
        return null();
    switch (tt) {
      // TokenKind::Eol is special; it implements the [no LineTerminator here]
      // quirk in the grammar.
      case TokenKind::Eol:
      // The rest of these make up the complete set of tokens that can
      // appear after any of the places where AssignmentExpression is used
      // throughout the grammar.  Conveniently, none of them can also be the
      // start an expression.
      case TokenKind::Eof:
      case TokenKind::Semi:
      case TokenKind::Rc:
      case TokenKind::Rb:
      case TokenKind::Rp:
      case TokenKind::Colon:
      case TokenKind::Comma:
      case TokenKind::In:
        // No value.
        exprNode = null();
        anyChars.addModifierException(TokenStream::NoneIsOperand);
        break;
      case TokenKind::Mul:
        kind = ParseNodeKind::YieldStar;
        tokenStream.consumeKnownToken(TokenKind::Mul, TokenStream::Operand);
        MOZ_FALLTHROUGH;
      default:
        exprNode = assignExpr(inHandling, YieldIsKeyword, TripledotProhibited);
        if (!exprNode)
            return null();
    }
    if (kind == ParseNodeKind::YieldStar)
        return handler.newYieldStarExpression(begin, exprNode);
    return handler.newYieldExpression(begin, exprNode);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::withStatement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::With));
    uint32_t begin = pos().begin;

    // Usually we want the constructs forbidden in strict mode code to be a
    // subset of those that ContextOptions::extraWarnings() warns about, and we
    // use strictModeError directly.  But while 'with' is forbidden in strict
    // mode code, it doesn't even merit a warning in non-strict code.  See
    // https://bugzilla.mozilla.org/show_bug.cgi?id=514576#c1.
    if (pc->sc()->strict()) {
        if (!strictModeError(JSMSG_STRICT_CODE_WITH))
            return null();
    }

    MUST_MATCH_TOKEN(TokenKind::Lp, JSMSG_PAREN_BEFORE_WITH);

    Node objectExpr = exprInParens(InAllowed, yieldHandling, TripledotProhibited);
    if (!objectExpr)
        return null();

    MUST_MATCH_TOKEN_MOD(TokenKind::Rp, TokenStream::Operand, JSMSG_PAREN_AFTER_WITH);

    Node innerBlock;
    {
        ParseContext::Statement stmt(pc, StatementKind::With);
        innerBlock = statement(yieldHandling);
        if (!innerBlock)
            return null();
    }

    pc->sc()->setBindingsAccessedDynamically();

    return handler.newWithStatement(begin, objectExpr, innerBlock);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::labeledItem(YieldHandling yieldHandling)
{
    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();

    if (tt == TokenKind::Function) {
        TokenKind next;
        if (!tokenStream.peekToken(&next))
            return null();

        // GeneratorDeclaration is only matched by HoistableDeclaration in
        // StatementListItem, so generators can't be inside labels.
        if (next == TokenKind::Mul) {
            error(JSMSG_GENERATOR_LABEL);
            return null();
        }

        // Per 13.13.1 it's a syntax error if LabelledItem: FunctionDeclaration
        // is ever matched.  Per Annex B.3.2 that modifies this text, this
        // applies only to strict mode code.
        if (pc->sc()->strict()) {
            error(JSMSG_FUNCTION_LABEL);
            return null();
        }

        return functionStmt(pos().begin, yieldHandling, NameRequired);
    }

    anyChars.ungetToken();
    return statement(yieldHandling);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::labeledStatement(YieldHandling yieldHandling)
{
    RootedPropertyName label(context, labelIdentifier(yieldHandling));
    if (!label)
        return null();

    auto hasSameLabel = [&label](ParseContext::LabelStatement* stmt) {
        return stmt->label() == label;
    };

    uint32_t begin = pos().begin;

    if (pc->template findInnermostStatement<ParseContext::LabelStatement>(hasSameLabel)) {
        errorAt(begin, JSMSG_DUPLICATE_LABEL);
        return null();
    }

    tokenStream.consumeKnownToken(TokenKind::Colon);

    /* Push a label struct and parse the statement. */
    ParseContext::LabelStatement stmt(pc, label);
    Node pn = labeledItem(yieldHandling);
    if (!pn)
        return null();

    return handler.newLabeledStatement(label, pn, begin);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::throwStatement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Throw));
    uint32_t begin = pos().begin;

    /* ECMA-262 Edition 3 says 'throw [no LineTerminator here] Expr'. */
    TokenKind tt = TokenKind::Eof;
    if (!tokenStream.peekTokenSameLine(&tt, TokenStream::Operand))
        return null();
    if (tt == TokenKind::Eof || tt == TokenKind::Semi || tt == TokenKind::Rc) {
        error(JSMSG_MISSING_EXPR_AFTER_THROW);
        return null();
    }
    if (tt == TokenKind::Eol) {
        error(JSMSG_LINE_BREAK_AFTER_THROW);
        return null();
    }

    Node throwExpr = expr(InAllowed, yieldHandling, TripledotProhibited);
    if (!throwExpr)
        return null();

    if (!matchOrInsertSemicolon())
        return null();

    return handler.newThrowStatement(throwExpr, TokenPos(begin, pos().end));
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::tryStatement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Try));
    uint32_t begin = pos().begin;

    /*
     * try nodes are ternary.
     * kid1 is the try statement
     * kid2 is the catch node list or null
     * kid3 is the finally statement
     *
     * catch nodes are binary.
     * left is the catch-name/pattern or null
     * right is the catch block
     *
     * catch lvalue nodes are either:
     *   a single identifier
     *   TokenKind::Rb or TokenKind::Rc for a destructuring left-hand side
     *
     * finally nodes are TokenKind::Lc statement lists.
     */

    Node innerBlock;
    {
        MUST_MATCH_TOKEN(TokenKind::Lc, JSMSG_CURLY_BEFORE_TRY);

        uint32_t openedPos = pos().begin;

        ParseContext::Statement stmt(pc, StatementKind::Try);
        ParseContext::Scope scope(this);
        if (!scope.init(pc))
            return null();

        innerBlock = statementList(yieldHandling);
        if (!innerBlock)
            return null();

        innerBlock = finishLexicalScope(scope, innerBlock);
        if (!innerBlock)
            return null();

        MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rc, TokenStream::Operand,
                                         reportMissingClosing(JSMSG_CURLY_AFTER_TRY,
                                                              JSMSG_CURLY_OPENED, openedPos));
    }

    Node catchScope = null();
    TokenKind tt;
    if (!tokenStream.getToken(&tt))
        return null();
    if (tt == TokenKind::Catch) {
        /*
         * Create a lexical scope node around the whole catch clause,
         * including the head.
         */
        ParseContext::Statement stmt(pc, StatementKind::Catch);
        ParseContext::Scope scope(this);
        if (!scope.init(pc))
            return null();

        /*
         * Legal catch forms are:
         *   catch (lhs) {
         *   catch {
         * where lhs is a name or a destructuring left-hand side.
         */
        bool omittedBinding;
        if (!tokenStream.matchToken(&omittedBinding, TokenKind::Lc))
            return null();

        Node catchName;
        if (omittedBinding) {
            catchName = null();
        } else {
            MUST_MATCH_TOKEN(TokenKind::Lp, JSMSG_PAREN_BEFORE_CATCH);

            if (!tokenStream.getToken(&tt))
                return null();
            switch (tt) {
              case TokenKind::Lb:
              case TokenKind::Lc:
                catchName = destructuringDeclaration(DeclarationKind::CatchParameter,
                                                     yieldHandling, tt);
                if (!catchName)
                    return null();
                break;

              default: {
                if (!TokenKindIsPossibleIdentifierName(tt)) {
                    error(JSMSG_CATCH_IDENTIFIER);
                    return null();
                }

                catchName = bindingIdentifier(DeclarationKind::SimpleCatchParameter,
                                              yieldHandling);
                if (!catchName)
                    return null();
                break;
              }
            }

            MUST_MATCH_TOKEN_MOD(TokenKind::Rp, TokenStream::Operand, JSMSG_PAREN_AFTER_CATCH);

            MUST_MATCH_TOKEN(TokenKind::Lc, JSMSG_CURLY_BEFORE_CATCH);
        }

        Node catchBody = catchBlockStatement(yieldHandling, scope);
        if (!catchBody)
            return null();

        catchScope = finishLexicalScope(scope, catchBody);
        if (!catchScope)
            return null();

        if (!handler.setupCatchScope(catchScope, catchName, catchBody))
            return null();
        handler.setEndPosition(catchScope, pos().end);

        if (!tokenStream.getToken(&tt, TokenStream::Operand))
            return null();
    }

    Node finallyBlock = null();

    if (tt == TokenKind::Finally) {
        MUST_MATCH_TOKEN(TokenKind::Lc, JSMSG_CURLY_BEFORE_FINALLY);

        uint32_t openedPos = pos().begin;

        ParseContext::Statement stmt(pc, StatementKind::Finally);
        ParseContext::Scope scope(this);
        if (!scope.init(pc))
            return null();

        finallyBlock = statementList(yieldHandling);
        if (!finallyBlock)
            return null();

        finallyBlock = finishLexicalScope(scope, finallyBlock);
        if (!finallyBlock)
            return null();

        MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rc, TokenStream::Operand,
                                         reportMissingClosing(JSMSG_CURLY_AFTER_FINALLY,
                                                              JSMSG_CURLY_OPENED, openedPos));
    } else {
        anyChars.ungetToken();
    }
    if (!catchScope && !finallyBlock) {
        error(JSMSG_CATCH_OR_FINALLY);
        return null();
    }

    return handler.newTryStatement(begin, innerBlock, catchScope, finallyBlock);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::catchBlockStatement(YieldHandling yieldHandling,
                                                        ParseContext::Scope& catchParamScope)
{
    uint32_t openedPos = pos().begin;

    ParseContext::Statement stmt(pc, StatementKind::Block);

    // ES 13.15.7 CatchClauseEvaluation
    //
    // Step 8 means that the body of a catch block always has an additional
    // lexical scope.
    ParseContext::Scope scope(this);
    if (!scope.init(pc))
        return null();

    // The catch parameter names cannot be redeclared inside the catch
    // block, so declare the name in the inner scope.
    if (!scope.addCatchParameters(pc, catchParamScope))
        return null();

    Node list = statementList(yieldHandling);
    if (!list)
        return null();

    MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rc, TokenStream::Operand,
                                     reportMissingClosing(JSMSG_CURLY_AFTER_CATCH,
                                                          JSMSG_CURLY_OPENED, openedPos));

    // The catch parameter names are not bound in the body scope, so remove
    // them before generating bindings.
    scope.removeCatchParameters(pc, catchParamScope);
    return finishLexicalScope(scope, list);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::debuggerStatement()
{
    TokenPos p;
    p.begin = pos().begin;
    if (!matchOrInsertSemicolon())
        return null();
    p.end = pos().end;

    pc->sc()->setBindingsAccessedDynamically();
    pc->sc()->setHasDebuggerStatement();

    return handler.newDebuggerStatement(p);
}

static AccessorType
ToAccessorType(PropertyType propType)
{
    switch (propType) {
      case PropertyType::Getter:
      case PropertyType::GetterNoExpressionClosure:
        return AccessorType::Getter;
      case PropertyType::Setter:
      case PropertyType::SetterNoExpressionClosure:
        return AccessorType::Setter;
      case PropertyType::Normal:
      case PropertyType::Method:
      case PropertyType::GeneratorMethod:
      case PropertyType::AsyncMethod:
      case PropertyType::AsyncGeneratorMethod:
      case PropertyType::Constructor:
      case PropertyType::DerivedConstructor:
        return AccessorType::None;
      default:
        MOZ_CRASH("unexpected property type");
    }
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::classDefinition(YieldHandling yieldHandling,
                                                    ClassContext classContext,
                                                    DefaultHandling defaultHandling)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Class));

    uint32_t classStartOffset = pos().begin;
    bool savedStrictness = setLocalStrictMode(true);

    TokenKind tt;
    if (!tokenStream.getToken(&tt))
        return null();

    RootedPropertyName name(context);
    if (TokenKindIsPossibleIdentifier(tt)) {
        name = bindingIdentifier(yieldHandling);
        if (!name)
            return null();
    } else if (classContext == ClassStatement) {
        if (defaultHandling == AllowDefaultName) {
            name = context->names().default_;
            anyChars.ungetToken();
        } else {
            // Class statements must have a bound name
            error(JSMSG_UNNAMED_CLASS_STMT);
            return null();
        }
    } else {
        // Make sure to put it back, whatever it was
        anyChars.ungetToken();
    }

    // Push a ParseContext::ClassStatement to keep track of the constructor
    // funbox.
    ParseContext::ClassStatement classStmt(pc);

    RootedAtom propAtom(context);

    // A named class creates a new lexical scope with a const binding of the
    // class name for the "inner name".
    Maybe<ParseContext::Statement> innerScopeStmt;
    Maybe<ParseContext::Scope> innerScope;
    if (name) {
        innerScopeStmt.emplace(pc, StatementKind::Block);
        innerScope.emplace(this);
        if (!innerScope->init(pc))
            return null();
    }

    // Because the binding definitions keep track of their blockId, we need to
    // create at least the inner binding later. Keep track of the name's position
    // in order to provide it for the nodes created later.
    TokenPos namePos = pos();

    Node classHeritage = null();
    bool hasHeritage;
    if (!tokenStream.matchToken(&hasHeritage, TokenKind::Extends))
        return null();
    if (hasHeritage) {
        if (!tokenStream.getToken(&tt))
            return null();
        classHeritage = memberExpr(yieldHandling, TripledotProhibited,
                                   ExpressionClosure::Forbidden, tt);
        if (!classHeritage)
            return null();
    }

    MUST_MATCH_TOKEN(TokenKind::Lc, JSMSG_CURLY_BEFORE_CLASS);

    Node classMethods = handler.newClassMethodList(pos().begin);
    if (!classMethods)
        return null();

    Maybe<DeclarationKind> declKind = Nothing();
    for (;;) {
        TokenKind tt;
        if (!tokenStream.getToken(&tt))
            return null();
        if (tt == TokenKind::Rc)
            break;

        if (tt == TokenKind::Semi)
            continue;

        bool isStatic = false;
        if (tt == TokenKind::Static) {
            if (!tokenStream.peekToken(&tt))
                return null();
            if (tt == TokenKind::Rc) {
                tokenStream.consumeKnownToken(tt);
                error(JSMSG_UNEXPECTED_TOKEN, "property name", TokenKindToDesc(tt));
                return null();
            }

            if (tt != TokenKind::Lp)
                isStatic = true;
            else
                anyChars.ungetToken();
        } else {
            anyChars.ungetToken();
        }

        uint32_t nameOffset;
        if (!tokenStream.peekOffset(&nameOffset))
            return null();

        PropertyType propType;
        Node propName = propertyName(yieldHandling, declKind, classMethods, &propType, &propAtom);
        if (!propName)
            return null();

        if (propType != PropertyType::Getter && propType != PropertyType::Setter &&
            propType != PropertyType::Method && propType != PropertyType::GeneratorMethod &&
            propType != PropertyType::AsyncMethod &&
            propType != PropertyType::AsyncGeneratorMethod)
        {
            errorAt(nameOffset, JSMSG_BAD_METHOD_DEF);
            return null();
        }

        if (propType == PropertyType::Getter)
            propType = PropertyType::GetterNoExpressionClosure;
        if (propType == PropertyType::Setter)
            propType = PropertyType::SetterNoExpressionClosure;

        bool isConstructor = !isStatic && propAtom == context->names().constructor;
        if (isConstructor) {
            if (propType != PropertyType::Method) {
                errorAt(nameOffset, JSMSG_BAD_METHOD_DEF);
                return null();
            }
            if (classStmt.constructorBox) {
                errorAt(nameOffset, JSMSG_DUPLICATE_PROPERTY, "constructor");
                return null();
            }
            propType = hasHeritage ? PropertyType::DerivedConstructor : PropertyType::Constructor;
        } else if (isStatic && propAtom == context->names().prototype) {
            errorAt(nameOffset, JSMSG_BAD_METHOD_DEF);
            return null();
        }

        RootedAtom funName(context);
        switch (propType) {
          case PropertyType::GetterNoExpressionClosure:
          case PropertyType::SetterNoExpressionClosure:
            if (!anyChars.isCurrentTokenType(TokenKind::Rb)) {
                funName = prefixAccessorName(propType, propAtom);
                if (!funName)
                    return null();
            }
            break;
          case PropertyType::Constructor:
          case PropertyType::DerivedConstructor:
            funName = name;
            break;
          default:
            if (!anyChars.isCurrentTokenType(TokenKind::Rb))
                funName = propAtom;
        }

        // Calling toString on constructors need to return the source text for
        // the entire class. The end offset is unknown at this point in
        // parsing and will be amended when class parsing finishes below.
        Node fn = methodDefinition(isConstructor ? classStartOffset : nameOffset,
                                   propType, funName);
        if (!fn)
            return null();

        handler.checkAndSetIsDirectRHSAnonFunction(fn);

        AccessorType atype = ToAccessorType(propType);
        if (!handler.addClassMethodDefinition(classMethods, propName, fn, atype, isStatic))
            return null();
    }

    // Amend the toStringEnd offset for the constructor now that we've
    // finished parsing the class.
    uint32_t classEndOffset = pos().end;
    if (FunctionBox* ctorbox = classStmt.constructorBox) {
        if (ctorbox->function()->isInterpretedLazy())
            ctorbox->function()->lazyScript()->setToStringEnd(classEndOffset);
        ctorbox->toStringEnd = classEndOffset;
    }

    Node nameNode = null();
    Node methodsOrBlock = classMethods;
    if (name) {
        // The inner name is immutable.
        if (!noteDeclaredName(name, DeclarationKind::Const, namePos))
            return null();

        Node innerName = newName(name, namePos);
        if (!innerName)
            return null();

        Node classBlock = finishLexicalScope(*innerScope, classMethods);
        if (!classBlock)
            return null();

        methodsOrBlock = classBlock;

        // Pop the inner scope.
        innerScope.reset();
        innerScopeStmt.reset();

        Node outerName = null();
        if (classContext == ClassStatement) {
            // The outer name is mutable.
            if (!noteDeclaredName(name, DeclarationKind::Class, namePos))
                return null();

            outerName = newName(name, namePos);
            if (!outerName)
                return null();
        }

        nameNode = handler.newClassNames(outerName, innerName, namePos);
        if (!nameNode)
            return null();
    }

    MOZ_ALWAYS_TRUE(setLocalStrictMode(savedStrictness));

    return handler.newClass(nameNode, classHeritage, methodsOrBlock,
                            TokenPos(classStartOffset, classEndOffset));
}

bool
ParserBase::nextTokenContinuesLetDeclaration(TokenKind next)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Let));
    MOZ_ASSERT(anyChars.nextToken().type == next);

    TokenStreamShared::verifyConsistentModifier(TokenStreamShared::None, anyChars.nextToken());

    // Destructuring continues a let declaration.
    if (next == TokenKind::Lb || next == TokenKind::Lc)
        return true;

    // A "let" edge case deserves special comment.  Consider this:
    //
    //   let     // not an ASI opportunity
    //   let;
    //
    // Static semantics in §13.3.1.1 turn a LexicalDeclaration that binds
    // "let" into an early error.  Does this retroactively permit ASI so
    // that we should parse this as two ExpressionStatements?   No.  ASI
    // resolves during parsing.  Static semantics only apply to the full
    // parse tree with ASI applied.  No backsies!

    // Otherwise a let declaration must have a name.
    return TokenKindIsPossibleIdentifier(next);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::variableStatement(YieldHandling yieldHandling)
{
    Node vars = declarationList(yieldHandling, ParseNodeKind::Var);
    if (!vars)
        return null();
    if (!matchOrInsertSemicolon())
        return null();
    return vars;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::statement(YieldHandling yieldHandling)
{
    MOZ_ASSERT(checkOptionsCalled);

    if (!CheckRecursionLimit(context))
        return null();

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();

    switch (tt) {
      // BlockStatement[?Yield, ?Return]
      case TokenKind::Lc:
        return blockStatement(yieldHandling);

      // VariableStatement[?Yield]
      case TokenKind::Var:
        return variableStatement(yieldHandling);

      // EmptyStatement
      case TokenKind::Semi:
        return handler.newEmptyStatement(pos());

      // ExpressionStatement[?Yield].

      case TokenKind::Yield: {
        // Don't use a ternary operator here due to obscure linker issues
        // around using static consts in the arms of a ternary.
        Modifier modifier;
        if (yieldExpressionsSupported())
            modifier = TokenStream::Operand;
        else
            modifier = TokenStream::None;

        TokenKind next;
        if (!tokenStream.peekToken(&next, modifier))
            return null();

        if (next == TokenKind::Colon)
            return labeledStatement(yieldHandling);

        return expressionStatement(yieldHandling);
      }

      default: {
        // Avoid getting next token with None.
        if (tt == TokenKind::Await && pc->isAsync())
            return expressionStatement(yieldHandling);

        if (!TokenKindIsPossibleIdentifier(tt))
            return expressionStatement(yieldHandling);

        TokenKind next;
        if (!tokenStream.peekToken(&next))
            return null();

        // |let| here can only be an Identifier, not a declaration.  Give nicer
        // errors for declaration-looking typos.
        if (tt == TokenKind::Let) {
            bool forbiddenLetDeclaration = false;

            if (next == TokenKind::Lb) {
                // Enforce ExpressionStatement's 'let [' lookahead restriction.
                forbiddenLetDeclaration = true;
            } else if (next == TokenKind::Lc || TokenKindIsPossibleIdentifier(next)) {
                // 'let {' and 'let foo' aren't completely forbidden, if ASI
                // causes 'let' to be the entire Statement.  But if they're
                // same-line, we can aggressively give a better error message.
                //
                // Note that this ignores 'yield' as TokenKind::Yield: we'll handle it
                // correctly but with a worse error message.
                TokenKind nextSameLine;
                if (!tokenStream.peekTokenSameLine(&nextSameLine))
                    return null();

                MOZ_ASSERT(TokenKindIsPossibleIdentifier(nextSameLine) ||
                           nextSameLine == TokenKind::Lc ||
                           nextSameLine == TokenKind::Eol);

                forbiddenLetDeclaration = nextSameLine != TokenKind::Eol;
            }

            if (forbiddenLetDeclaration) {
                error(JSMSG_FORBIDDEN_AS_STATEMENT, "lexical declarations");
                return null();
            }
        } else if (tt == TokenKind::Async) {
            // Peek only on the same line: ExpressionStatement's lookahead
            // restriction is phrased as
            //
            //   [lookahead ∉ { {, function, async [no LineTerminator here] function, class, let [ }]
            //
            // meaning that code like this is valid:
            //
            //   if (true)
            //     async       // ASI opportunity
            //   function clownshoes() {}
            TokenKind maybeFunction;
            if (!tokenStream.peekTokenSameLine(&maybeFunction))
                return null();

            if (maybeFunction == TokenKind::Function) {
                error(JSMSG_FORBIDDEN_AS_STATEMENT, "async function declarations");
                return null();
            }

            // Otherwise this |async| begins an ExpressionStatement or is a
            // label name.
        }

        // NOTE: It's unfortunately allowed to have a label named 'let' in
        //       non-strict code.  💯
        if (next == TokenKind::Colon)
            return labeledStatement(yieldHandling);

        return expressionStatement(yieldHandling);
      }

      case TokenKind::New:
        return expressionStatement(yieldHandling, PredictInvoked);

      // IfStatement[?Yield, ?Return]
      case TokenKind::If:
        return ifStatement(yieldHandling);

      // BreakableStatement[?Yield, ?Return]
      //
      // BreakableStatement[Yield, Return]:
      //   IterationStatement[?Yield, ?Return]
      //   SwitchStatement[?Yield, ?Return]
      case TokenKind::Do:
        return doWhileStatement(yieldHandling);

      case TokenKind::While:
        return whileStatement(yieldHandling);

      case TokenKind::For:
        return forStatement(yieldHandling);

      case TokenKind::Switch:
        return switchStatement(yieldHandling);

      // ContinueStatement[?Yield]
      case TokenKind::Continue:
        return continueStatement(yieldHandling);

      // BreakStatement[?Yield]
      case TokenKind::Break:
        return breakStatement(yieldHandling);

      // [+Return] ReturnStatement[?Yield]
      case TokenKind::Return:
        // The Return parameter is only used here, and the effect is easily
        // detected this way, so don't bother passing around an extra parameter
        // everywhere.
        if (!pc->isFunctionBox()) {
            error(JSMSG_BAD_RETURN_OR_YIELD, js_return_str);
            return null();
        }
        return returnStatement(yieldHandling);

      // WithStatement[?Yield, ?Return]
      case TokenKind::With:
        return withStatement(yieldHandling);

      // LabelledStatement[?Yield, ?Return]
      // This is really handled by default and TokenKind::Yield cases above.

      // ThrowStatement[?Yield]
      case TokenKind::Throw:
        return throwStatement(yieldHandling);

      // TryStatement[?Yield, ?Return]
      case TokenKind::Try:
        return tryStatement(yieldHandling);

      // DebuggerStatement
      case TokenKind::Debugger:
        return debuggerStatement();

      // |function| is forbidden by lookahead restriction (unless as child
      // statement of |if| or |else|, but Parser::consequentOrAlternative
      // handles that).
      case TokenKind::Function:
        error(JSMSG_FORBIDDEN_AS_STATEMENT, "function declarations");
        return null();

      // |class| is also forbidden by lookahead restriction.
      case TokenKind::Class:
        error(JSMSG_FORBIDDEN_AS_STATEMENT, "classes");
        return null();

      // ImportDeclaration (only inside modules)
      case TokenKind::Import:
        return importDeclaration();

      // ExportDeclaration (only inside modules)
      case TokenKind::Export:
        return exportDeclaration();

      // Miscellaneous error cases arguably better caught here than elsewhere.

      case TokenKind::Catch:
        error(JSMSG_CATCH_WITHOUT_TRY);
        return null();

      case TokenKind::Finally:
        error(JSMSG_FINALLY_WITHOUT_TRY);
        return null();

      // NOTE: default case handled in the ExpressionStatement section.
    }
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::statementListItem(YieldHandling yieldHandling,
                                                      bool canHaveDirectives /* = false */)
{
    MOZ_ASSERT(checkOptionsCalled);

    if (!CheckRecursionLimit(context))
        return null();

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();

    switch (tt) {
      // BlockStatement[?Yield, ?Return]
      case TokenKind::Lc:
        return blockStatement(yieldHandling);

      // VariableStatement[?Yield]
      case TokenKind::Var:
        return variableStatement(yieldHandling);

      // EmptyStatement
      case TokenKind::Semi:
        return handler.newEmptyStatement(pos());

      // ExpressionStatement[?Yield].
      //
      // These should probably be handled by a single ExpressionStatement
      // function in a default, not split up this way.
      case TokenKind::String:
        if (!canHaveDirectives && anyChars.currentToken().atom() == context->names().useAsm) {
            if (!abortIfSyntaxParser())
                return null();
            if (!warning(JSMSG_USE_ASM_DIRECTIVE_FAIL))
                return null();
        }
        return expressionStatement(yieldHandling);

      case TokenKind::Yield: {
        // Don't use a ternary operator here due to obscure linker issues
        // around using static consts in the arms of a ternary.
        Modifier modifier;
        if (yieldExpressionsSupported())
            modifier = TokenStream::Operand;
        else
            modifier = TokenStream::None;

        TokenKind next;
        if (!tokenStream.peekToken(&next, modifier))
            return null();

        if (next == TokenKind::Colon)
            return labeledStatement(yieldHandling);

        return expressionStatement(yieldHandling);
      }

      default: {
        // Avoid getting next token with None.
        if (tt == TokenKind::Await && pc->isAsync())
            return expressionStatement(yieldHandling);

        if (!TokenKindIsPossibleIdentifier(tt))
            return expressionStatement(yieldHandling);

        TokenKind next;
        if (!tokenStream.peekToken(&next))
            return null();

        if (tt == TokenKind::Let && nextTokenContinuesLetDeclaration(next))
            return lexicalDeclaration(yieldHandling, DeclarationKind::Let);

        if (tt == TokenKind::Async) {
            TokenKind nextSameLine = TokenKind::Eof;
            if (!tokenStream.peekTokenSameLine(&nextSameLine))
                return null();
            if (nextSameLine == TokenKind::Function) {
                uint32_t toStringStart = pos().begin;
                tokenStream.consumeKnownToken(TokenKind::Function);
                return functionStmt(toStringStart, yieldHandling, NameRequired,
                                    FunctionAsyncKind::AsyncFunction);
            }
        }

        if (next == TokenKind::Colon)
            return labeledStatement(yieldHandling);

        return expressionStatement(yieldHandling);
      }

      case TokenKind::New:
        return expressionStatement(yieldHandling, PredictInvoked);

      // IfStatement[?Yield, ?Return]
      case TokenKind::If:
        return ifStatement(yieldHandling);

      // BreakableStatement[?Yield, ?Return]
      //
      // BreakableStatement[Yield, Return]:
      //   IterationStatement[?Yield, ?Return]
      //   SwitchStatement[?Yield, ?Return]
      case TokenKind::Do:
        return doWhileStatement(yieldHandling);

      case TokenKind::While:
        return whileStatement(yieldHandling);

      case TokenKind::For:
        return forStatement(yieldHandling);

      case TokenKind::Switch:
        return switchStatement(yieldHandling);

      // ContinueStatement[?Yield]
      case TokenKind::Continue:
        return continueStatement(yieldHandling);

      // BreakStatement[?Yield]
      case TokenKind::Break:
        return breakStatement(yieldHandling);

      // [+Return] ReturnStatement[?Yield]
      case TokenKind::Return:
        // The Return parameter is only used here, and the effect is easily
        // detected this way, so don't bother passing around an extra parameter
        // everywhere.
        if (!pc->isFunctionBox()) {
            error(JSMSG_BAD_RETURN_OR_YIELD, js_return_str);
            return null();
        }
        return returnStatement(yieldHandling);

      // WithStatement[?Yield, ?Return]
      case TokenKind::With:
        return withStatement(yieldHandling);

      // LabelledStatement[?Yield, ?Return]
      // This is really handled by default and TokenKind::Yield cases above.

      // ThrowStatement[?Yield]
      case TokenKind::Throw:
        return throwStatement(yieldHandling);

      // TryStatement[?Yield, ?Return]
      case TokenKind::Try:
        return tryStatement(yieldHandling);

      // DebuggerStatement
      case TokenKind::Debugger:
        return debuggerStatement();

      // Declaration[Yield]:

      //   HoistableDeclaration[?Yield, ~Default]
      case TokenKind::Function:
        return functionStmt(pos().begin, yieldHandling, NameRequired);

      //   ClassDeclaration[?Yield, ~Default]
      case TokenKind::Class:
        return classDefinition(yieldHandling, ClassStatement, NameRequired);

      //   LexicalDeclaration[In, ?Yield]
      //     LetOrConst BindingList[?In, ?Yield]
      case TokenKind::Const:
        // [In] is the default behavior, because for-loops specially parse
        // their heads to handle |in| in this situation.
        return lexicalDeclaration(yieldHandling, DeclarationKind::Const);

      // ImportDeclaration (only inside modules)
      case TokenKind::Import:
        return importDeclaration();

      // ExportDeclaration (only inside modules)
      case TokenKind::Export:
        return exportDeclaration();

      // Miscellaneous error cases arguably better caught here than elsewhere.

      case TokenKind::Catch:
        error(JSMSG_CATCH_WITHOUT_TRY);
        return null();

      case TokenKind::Finally:
        error(JSMSG_FINALLY_WITHOUT_TRY);
        return null();

      // NOTE: default case handled in the ExpressionStatement section.
    }
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::expr(InHandling inHandling, YieldHandling yieldHandling,
                                         TripledotHandling tripledotHandling,
                                         PossibleError* possibleError /* = nullptr */,
                                         InvokedPrediction invoked /* = PredictUninvoked */)
{
    Node pn = assignExpr(inHandling, yieldHandling, tripledotHandling,
                         possibleError, invoked);
    if (!pn)
        return null();

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Comma, TokenStream::Operand))
        return null();
    if (!matched)
        return pn;

    Node seq = handler.newCommaExpressionList(pn);
    if (!seq)
        return null();
    while (true) {
        // Trailing comma before the closing parenthesis is valid in an arrow
        // function parameters list: `(a, b, ) => body`. Check if we are
        // directly under CoverParenthesizedExpressionAndArrowParameterList,
        // and the next two tokens are closing parenthesis and arrow. If all
        // are present allow the trailing comma.
        if (tripledotHandling == TripledotAllowed) {
            TokenKind tt;
            if (!tokenStream.peekToken(&tt, TokenStream::Operand))
                return null();

            if (tt == TokenKind::Rp) {
                tokenStream.consumeKnownToken(TokenKind::Rp, TokenStream::Operand);

                if (!tokenStream.peekToken(&tt))
                    return null();
                if (tt != TokenKind::Arrow) {
                    error(JSMSG_UNEXPECTED_TOKEN, "expression", TokenKindToDesc(TokenKind::Rp));
                    return null();
                }

                anyChars.ungetToken();  // put back right paren
                break;
            }
        }

        // Additional calls to assignExpr should not reuse the possibleError
        // which had been passed into the function. Otherwise we would lose
        // information needed to determine whether or not we're dealing with
        // a non-recoverable situation.
        PossibleError possibleErrorInner(*this);
        pn = assignExpr(inHandling, yieldHandling, tripledotHandling,
                        &possibleErrorInner);
        if (!pn)
            return null();

        if (!possibleError) {
            // Report any pending expression error.
            if (!possibleErrorInner.checkForExpressionError())
                return null();
        } else {
            possibleErrorInner.transferErrorsTo(possibleError);
        }

        handler.addList(seq, pn);

        if (!tokenStream.matchToken(&matched, TokenKind::Comma, TokenStream::Operand))
            return null();
        if (!matched)
            break;
    }
    return seq;
}

static ParseNodeKind
BinaryOpTokenKindToParseNodeKind(TokenKind tok)
{
    MOZ_ASSERT(TokenKindIsBinaryOp(tok));
    return ParseNodeKind(size_t(ParseNodeKind::BinOpFirst) + (size_t(tok) - size_t(TokenKind::BinOpFirst)));
}

static const int PrecedenceTable[] = {
    1, /* ParseNodeKind::PipeLine */
    2, /* ParseNodeKind::Or */
    3, /* ParseNodeKind::And */
    4, /* ParseNodeKind::BitOr */
    5, /* ParseNodeKind::BitXor */
    6, /* ParseNodeKind::BitAnd */
    7, /* ParseNodeKind::StrictEq */
    7, /* ParseNodeKind::Eq */
    7, /* ParseNodeKind::StrictNe */
    7, /* ParseNodeKind::Ne */
    8, /* ParseNodeKind::Lt */
    8, /* ParseNodeKind::Le */
    8, /* ParseNodeKind::Gt */
    8, /* ParseNodeKind::Ge */
    8, /* ParseNodeKind::InstanceOf */
    8, /* ParseNodeKind::In */
    9, /* ParseNodeKind::Lsh */
    9, /* ParseNodeKind::Rsh */
    9, /* ParseNodeKind::Ursh */
    10, /* ParseNodeKind::Add */
    10, /* ParseNodeKind::Sub */
    11, /* ParseNodeKind::Star */
    11, /* ParseNodeKind::Div */
    11, /* ParseNodeKind::Mod */
    12  /* ParseNodeKind::Pow */
};

static const int PRECEDENCE_CLASSES = 12;

static int
Precedence(ParseNodeKind pnk)
{
    // Everything binds tighter than ParseNodeKind::Limit, because we want
    // to reduce all nodes to a single node when we reach a token that is not
    // another binary operator.
    if (pnk == ParseNodeKind::Limit)
        return 0;

    MOZ_ASSERT(pnk >= ParseNodeKind::BinOpFirst);
    MOZ_ASSERT(pnk <= ParseNodeKind::BinOpLast);
    return PrecedenceTable[size_t(pnk) - size_t(ParseNodeKind::BinOpFirst)];
}

template <class ParseHandler, typename CharT>
MOZ_ALWAYS_INLINE typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::orExpr(InHandling inHandling, YieldHandling yieldHandling,
                                           TripledotHandling tripledotHandling,
                                           ExpressionClosure expressionClosureHandling,
                                           PossibleError* possibleError,
                                           InvokedPrediction invoked /* = PredictUninvoked */)
{
    // Shift-reduce parser for the binary operator part of the JS expression
    // syntax.

    // Conceptually there's just one stack, a stack of pairs (lhs, op).
    // It's implemented using two separate arrays, though.
    Node nodeStack[PRECEDENCE_CLASSES];
    ParseNodeKind kindStack[PRECEDENCE_CLASSES];
    int depth = 0;
    Node pn;
    for (;;) {
        pn = unaryExpr(yieldHandling, tripledotHandling, expressionClosureHandling, possibleError,
                       invoked);
        if (!pn)
            return null();

        if (handler.isExpressionClosure(pn))
            return pn;

        expressionClosureHandling = ExpressionClosure::Forbidden;

        // If a binary operator follows, consume it and compute the
        // corresponding operator.
        TokenKind tok;
        if (!tokenStream.getToken(&tok))
            return null();

        ParseNodeKind pnk;
        if (tok == TokenKind::In ? inHandling == InAllowed : TokenKindIsBinaryOp(tok)) {
            // We're definitely not in a destructuring context, so report any
            // pending expression error now.
            if (possibleError && !possibleError->checkForExpressionError())
                return null();
            // Report an error for unary expressions on the LHS of **.
            if (tok == TokenKind::Pow && handler.isUnparenthesizedUnaryExpression(pn)) {
                error(JSMSG_BAD_POW_LEFTSIDE);
                return null();
            }
            pnk = BinaryOpTokenKindToParseNodeKind(tok);
        } else {
            tok = TokenKind::Eof;
            pnk = ParseNodeKind::Limit;
        }

        // From this point on, destructuring defaults are definitely an error.
        possibleError = nullptr;

        // If pnk has precedence less than or equal to another operator on the
        // stack, reduce. This combines nodes on the stack until we form the
        // actual lhs of pnk.
        //
        // The >= in this condition works because it is appendOrCreateList's
        // job to decide if the operator in question is left- or
        // right-associative, and build the corresponding tree.
        while (depth > 0 && Precedence(kindStack[depth - 1]) >= Precedence(pnk)) {
            depth--;
            ParseNodeKind combiningPnk = kindStack[depth];
            pn = handler.appendOrCreateList(combiningPnk, nodeStack[depth], pn, pc);
            if (!pn)
                return null();
        }

        if (pnk == ParseNodeKind::Limit)
            break;

        nodeStack[depth] = pn;
        kindStack[depth] = pnk;
        depth++;
        MOZ_ASSERT(depth <= PRECEDENCE_CLASSES);
    }

    // When the next token is no longer a binary operator, it's potentially the
    // start of an expression.  Add a modifier exception so that the next token
    // modifier can be Operand.
    anyChars.ungetToken();
    anyChars.addModifierException(TokenStream::OperandIsNone);

    MOZ_ASSERT(depth == 0);
    return pn;
}

template <class ParseHandler, typename CharT>
MOZ_ALWAYS_INLINE typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::condExpr(InHandling inHandling, YieldHandling yieldHandling,
                                             TripledotHandling tripledotHandling,
                                             ExpressionClosure expressionClosureHandling,
                                             PossibleError* possibleError,
                                             InvokedPrediction invoked /* = PredictUninvoked */)
{
    Node condition = orExpr(inHandling, yieldHandling, tripledotHandling,
                            expressionClosureHandling, possibleError, invoked);
    if (!condition)
        return null();

    if (handler.isExpressionClosure(condition))
        return condition;

    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Hook))
        return null();
    if (!matched)
        return condition;

    Node thenExpr = assignExpr(InAllowed, yieldHandling, TripledotProhibited);
    if (!thenExpr)
        return null();

    MUST_MATCH_TOKEN_MOD(TokenKind::Colon, TokenStream::Operand, JSMSG_COLON_IN_COND);

    Node elseExpr = assignExpr(inHandling, yieldHandling, TripledotProhibited);
    if (!elseExpr)
        return null();

    return handler.newConditional(condition, thenExpr, elseExpr);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::assignExpr(InHandling inHandling, YieldHandling yieldHandling,
                                               TripledotHandling tripledotHandling,
                                               PossibleError* possibleError /* = nullptr */,
                                               InvokedPrediction invoked /* = PredictUninvoked */)
{
    if (!CheckRecursionLimit(context))
        return null();

    // It's very common at this point to have a "detectably simple" expression,
    // i.e. a name/number/string token followed by one of the following tokens
    // that obviously isn't part of an expression: , ; : ) ] }
    //
    // (In Parsemark this happens 81.4% of the time;  in code with large
    // numeric arrays, such as some Kraken benchmarks, it happens more often.)
    //
    // In such cases, we can avoid the full expression parsing route through
    // assignExpr(), condExpr(), orExpr(), unaryExpr(), memberExpr(), and
    // primaryExpr().

    TokenKind firstToken;
    if (!tokenStream.getToken(&firstToken, TokenStream::Operand))
        return null();

    TokenPos exprPos = pos();

    bool endsExpr;

    // This only handles identifiers that *never* have special meaning anywhere
    // in the language.  Contextual keywords, reserved words in strict mode,
    // and other hard cases are handled outside this fast path.
    if (firstToken == TokenKind::Name) {
        if (!tokenStream.nextTokenEndsExpr(&endsExpr))
            return null();
        if (endsExpr) {
            Rooted<PropertyName*> name(context, identifierReference(yieldHandling));
            if (!name)
                return null();

            return identifierReference(name);
        }
    }

    if (firstToken == TokenKind::Number) {
        if (!tokenStream.nextTokenEndsExpr(&endsExpr))
            return null();
        if (endsExpr)
            return newNumber(anyChars.currentToken());
    }

    if (firstToken == TokenKind::String) {
        if (!tokenStream.nextTokenEndsExpr(&endsExpr))
            return null();
        if (endsExpr)
            return stringLiteral();
    }

    if (firstToken == TokenKind::Yield && yieldExpressionsSupported())
        return yieldExpression(inHandling);

    bool maybeAsyncArrow = false;
    if (firstToken == TokenKind::Async) {
        TokenKind nextSameLine = TokenKind::Eof;
        if (!tokenStream.peekTokenSameLine(&nextSameLine))
            return null();

        if (TokenKindIsPossibleIdentifier(nextSameLine))
            maybeAsyncArrow = true;
    }

    anyChars.ungetToken();

    // Save the tokenizer state in case we find an arrow function and have to
    // rewind.
    Position start(keepAtoms);
    tokenStream.tell(&start);

    PossibleError possibleErrorInner(*this);
    Node lhs;
    TokenKind tokenAfterLHS;
    bool isArrow;
    if (maybeAsyncArrow) {
        tokenStream.consumeKnownToken(TokenKind::Async, TokenStream::Operand);

        TokenKind tokenAfterAsync;
        if (!tokenStream.getToken(&tokenAfterAsync))
            return null();
        MOZ_ASSERT(TokenKindIsPossibleIdentifier(tokenAfterAsync));

        // Check yield validity here.
        RootedPropertyName name(context, bindingIdentifier(yieldHandling));
        if (!name)
            return null();

        if (!tokenStream.peekTokenSameLine(&tokenAfterLHS))
            return null();
        if (tokenAfterLHS != TokenKind::Arrow) {
            error(JSMSG_UNEXPECTED_TOKEN,
                  "'=>' on the same line after an argument list", TokenKindToDesc(tokenAfterLHS));
            return null();
        }

        isArrow = true;
    } else {
        lhs = condExpr(inHandling, yieldHandling, tripledotHandling, ExpressionClosure::Allowed,
                       &possibleErrorInner, invoked);
        if (!lhs)
            return null();

        // Use Operand here because the ConditionalExpression parsed above
        // could be the entirety of this AssignmentExpression, and then ASI
        // permits this token to be a regular expression.
        if (!tokenStream.peekTokenSameLine(&tokenAfterLHS, TokenStream::Operand))
            return null();

        isArrow = tokenAfterLHS == TokenKind::Arrow;
    }

    if (isArrow) {
        tokenStream.seek(start);

        TokenKind next;
        if (!tokenStream.getToken(&next, TokenStream::Operand))
            return null();
        uint32_t toStringStart = pos().begin;
        anyChars.ungetToken();

        FunctionAsyncKind asyncKind = FunctionAsyncKind::SyncFunction;

        if (next == TokenKind::Async) {
            tokenStream.consumeKnownToken(next, TokenStream::Operand);

            TokenKind nextSameLine = TokenKind::Eof;
            if (!tokenStream.peekTokenSameLine(&nextSameLine))
                return null();

            // The AsyncArrowFunction production are
            //   async [no LineTerminator here] AsyncArrowBindingIdentifier ...
            //   async [no LineTerminator here] ArrowFormalParameters ...
            if (TokenKindIsPossibleIdentifier(nextSameLine) || nextSameLine == TokenKind::Lp)
                asyncKind = FunctionAsyncKind::AsyncFunction;
            else
                anyChars.ungetToken();
        }

        Node pn = handler.newArrowFunction(pos());
        if (!pn)
            return null();

        return functionDefinition(pn, toStringStart, inHandling, yieldHandling, nullptr, Arrow,
                                  GeneratorKind::NotGenerator, asyncKind);
    }

    MOZ_ALWAYS_TRUE(tokenStream.getToken(&tokenAfterLHS, TokenStream::Operand));

    ParseNodeKind kind;
    switch (tokenAfterLHS) {
      case TokenKind::Assign:       kind = ParseNodeKind::Assign;       break;
      case TokenKind::AddAssign:    kind = ParseNodeKind::AddAssign;    break;
      case TokenKind::SubAssign:    kind = ParseNodeKind::SubAssign;    break;
      case TokenKind::BitOrAssign:  kind = ParseNodeKind::BitOrAssign;  break;
      case TokenKind::BitXorAssign: kind = ParseNodeKind::BitXorAssign; break;
      case TokenKind::BitAndAssign: kind = ParseNodeKind::BitAndAssign; break;
      case TokenKind::LshAssign:    kind = ParseNodeKind::LshAssign;    break;
      case TokenKind::RshAssign:    kind = ParseNodeKind::RshAssign;    break;
      case TokenKind::UrshAssign:   kind = ParseNodeKind::UrshAssign;   break;
      case TokenKind::MulAssign:    kind = ParseNodeKind::MulAssign;    break;
      case TokenKind::DivAssign:    kind = ParseNodeKind::DivAssign;    break;
      case TokenKind::ModAssign:    kind = ParseNodeKind::ModAssign;    break;
      case TokenKind::PowAssign:    kind = ParseNodeKind::PowAssign;    break;

      default:
        MOZ_ASSERT(!anyChars.isCurrentTokenAssignment());
        if (!possibleError) {
            if (!possibleErrorInner.checkForExpressionError())
                return null();
        } else {
            possibleErrorInner.transferErrorsTo(possibleError);
        }

        anyChars.ungetToken();
        return lhs;
    }

    // Verify the left-hand side expression doesn't have a forbidden form.
    if (handler.isUnparenthesizedDestructuringPattern(lhs)) {
        if (kind != ParseNodeKind::Assign) {
            error(JSMSG_BAD_DESTRUCT_ASS);
            return null();
        }

        if (!possibleErrorInner.checkForDestructuringErrorOrWarning())
            return null();
    } else if (handler.isName(lhs)) {
        if (const char* chars = nameIsArgumentsOrEval(lhs)) {
            // |chars| is "arguments" or "eval" here.
            if (!strictModeErrorAt(exprPos.begin, JSMSG_BAD_STRICT_ASSIGN, chars))
                return null();
        }

        handler.adjustGetToSet(lhs);
    } else if (handler.isPropertyAccess(lhs)) {
        // Permitted: no additional testing/fixup needed.
    } else if (handler.isFunctionCall(lhs)) {
        if (!strictModeErrorAt(exprPos.begin, JSMSG_BAD_LEFTSIDE_OF_ASS))
            return null();

        if (possibleError)
            possibleError->setPendingDestructuringErrorAt(exprPos, JSMSG_BAD_DESTRUCT_TARGET);
    } else {
        errorAt(exprPos.begin, JSMSG_BAD_LEFTSIDE_OF_ASS);
        return null();
    }

    if (!possibleErrorInner.checkForExpressionError())
        return null();

    Node rhs = assignExpr(inHandling, yieldHandling, TripledotProhibited);
    if (!rhs)
        return null();

    if (kind == ParseNodeKind::Assign)
        handler.checkAndSetIsDirectRHSAnonFunction(rhs);

    return handler.newAssignment(kind, lhs, rhs);
}

template <class ParseHandler>
bool
PerHandlerParser<ParseHandler>::isValidSimpleAssignmentTarget(Node node,
                                                              FunctionCallBehavior behavior /* = ForbidAssignmentToFunctionCalls */)
{
    // Note that this method implements *only* a boolean test.  Reporting an
    // error for the various syntaxes that fail this, and warning for the
    // various syntaxes that "pass" this but should not, occurs elsewhere.

    if (handler.isName(node)) {
        if (!pc->sc()->strict())
            return true;

        return !nameIsArgumentsOrEval(node);
    }

    if (handler.isPropertyAccess(node))
        return true;

    if (behavior == PermitAssignmentToFunctionCalls) {
        if (handler.isFunctionCall(node))
            return true;
    }

    return false;
}

template <class ParseHandler>
const char*
PerHandlerParser<ParseHandler>::nameIsArgumentsOrEval(Node node)
{
    MOZ_ASSERT(handler.isName(node), "must only call this function on known names");

    if (handler.isEvalName(node, context))
        return js_eval_str;
    if (handler.isArgumentsName(node, context))
        return js_arguments_str;
    return nullptr;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::checkIncDecOperand(Node operand, uint32_t operandOffset)
{
    if (handler.isName(operand)) {
        if (const char* chars = nameIsArgumentsOrEval(operand)) {
            if (!strictModeErrorAt(operandOffset, JSMSG_BAD_STRICT_ASSIGN, chars))
                return false;
        }
    } else if (handler.isPropertyAccess(operand)) {
        // Permitted: no additional testing/fixup needed.
    } else if (handler.isFunctionCall(operand)) {
        // Assignment to function calls is forbidden in ES6.  We're still
        // somewhat concerned about sites using this in dead code, so forbid it
        // only in strict mode code (or if the werror option has been set), and
        // otherwise warn.
        if (!strictModeErrorAt(operandOffset, JSMSG_BAD_INCOP_OPERAND))
            return false;
    } else {
        errorAt(operandOffset, JSMSG_BAD_INCOP_OPERAND);
        return false;
    }

    MOZ_ASSERT(isValidSimpleAssignmentTarget(operand, PermitAssignmentToFunctionCalls),
               "inconsistent increment/decrement operand validation");
    return true;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::unaryOpExpr(YieldHandling yieldHandling, ParseNodeKind kind,
                                                uint32_t begin)
{
    Node kid = unaryExpr(yieldHandling, TripledotProhibited, ExpressionClosure::Forbidden);
    if (!kid)
        return null();
    return handler.newUnary(kind, begin, kid);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::unaryExpr(YieldHandling yieldHandling,
                                              TripledotHandling tripledotHandling,
                                              ExpressionClosure expressionClosureHandling,
                                              PossibleError* possibleError /* = nullptr */,
                                              InvokedPrediction invoked /* = PredictUninvoked */)
{
    if (!CheckRecursionLimit(context))
        return null();

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();
    uint32_t begin = pos().begin;
    switch (tt) {
      case TokenKind::Void:
        return unaryOpExpr(yieldHandling, ParseNodeKind::Void, begin);
      case TokenKind::Not:
        return unaryOpExpr(yieldHandling, ParseNodeKind::Not, begin);
      case TokenKind::BitNot:
        return unaryOpExpr(yieldHandling, ParseNodeKind::BitNot, begin);
      case TokenKind::Add:
        return unaryOpExpr(yieldHandling, ParseNodeKind::Pos, begin);
      case TokenKind::Sub:
        return unaryOpExpr(yieldHandling, ParseNodeKind::Neg, begin);

      case TokenKind::TypeOf: {
        // The |typeof| operator is specially parsed to distinguish its
        // application to a name, from its application to a non-name
        // expression:
        //
        //   // Looks up the name, doesn't find it and so evaluates to
        //   // "undefined".
        //   assertEq(typeof nonExistentName, "undefined");
        //
        //   // Evaluates expression, triggering a runtime ReferenceError for
        //   // the undefined name.
        //   typeof (1, nonExistentName);
        Node kid = unaryExpr(yieldHandling, TripledotProhibited, ExpressionClosure::Forbidden);
        if (!kid)
            return null();

        return handler.newTypeof(begin, kid);
      }

      case TokenKind::Inc:
      case TokenKind::Dec:
      {
        TokenKind tt2;
        if (!tokenStream.getToken(&tt2, TokenStream::Operand))
            return null();

        uint32_t operandOffset = pos().begin;
        Node operand =
            memberExpr(yieldHandling, TripledotProhibited, ExpressionClosure::Forbidden, tt2);
        if (!operand || !checkIncDecOperand(operand, operandOffset))
            return null();
        ParseNodeKind pnk = (tt == TokenKind::Inc)
                            ? ParseNodeKind::PreIncrement
                            : ParseNodeKind::PreDecrement;
        return handler.newUpdate(pnk, begin, operand);
      }

      case TokenKind::Delete: {
        uint32_t exprOffset;
        if (!tokenStream.peekOffset(&exprOffset, TokenStream::Operand))
            return null();

        Node expr = unaryExpr(yieldHandling, TripledotProhibited, ExpressionClosure::Forbidden);
        if (!expr)
            return null();

        // Per spec, deleting any unary expression is valid -- it simply
        // returns true -- except for one case that is illegal in strict mode.
        if (handler.isName(expr)) {
            if (!strictModeErrorAt(exprOffset, JSMSG_DEPRECATED_DELETE_OPERAND))
                return null();

            pc->sc()->setBindingsAccessedDynamically();
        }

        return handler.newDelete(begin, expr);
      }

      case TokenKind::Await: {
        if (pc->isAsync()) {
            Node kid = unaryExpr(yieldHandling, tripledotHandling, ExpressionClosure::Forbidden,
                                 possibleError, invoked);
            if (!kid)
                return null();
            pc->lastAwaitOffset = begin;
            return handler.newAwaitExpression(begin, kid);
        }
      }

        MOZ_FALLTHROUGH;

      default: {
        Node expr = memberExpr(yieldHandling, tripledotHandling, expressionClosureHandling, tt,
                               /* allowCallSyntax = */ true, possibleError, invoked);
        if (!expr)
            return null();

        if (handler.isExpressionClosure(expr))
            return expr;

        /* Don't look across a newline boundary for a postfix incop. */
        if (!tokenStream.peekTokenSameLine(&tt))
            return null();

        if (tt != TokenKind::Inc && tt != TokenKind::Dec)
            return expr;

        tokenStream.consumeKnownToken(tt);
        if (!checkIncDecOperand(expr, begin))
            return null();

        ParseNodeKind pnk = (tt == TokenKind::Inc)
                            ? ParseNodeKind::PostIncrement
                            : ParseNodeKind::PostDecrement;
        return handler.newUpdate(pnk, begin, expr);
      }
    }
}


template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::assignExprWithoutYieldOrAwait(YieldHandling yieldHandling)
{
    uint32_t startYieldOffset = pc->lastYieldOffset;
    uint32_t startAwaitOffset = pc->lastAwaitOffset;
    Node res = assignExpr(InAllowed, yieldHandling, TripledotProhibited);
    if (res) {
        if (pc->lastYieldOffset != startYieldOffset) {
            errorAt(pc->lastYieldOffset, JSMSG_YIELD_IN_DEFAULT);
            return null();
        }
        if (pc->lastAwaitOffset != startAwaitOffset) {
            errorAt(pc->lastAwaitOffset, JSMSG_AWAIT_IN_DEFAULT);
            return null();
        }
    }
    return res;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::argumentList(YieldHandling yieldHandling, Node listNode,
                                                 bool* isSpread,
                                                 PossibleError* possibleError /* = nullptr */)
{
    bool matched;
    if (!tokenStream.matchToken(&matched, TokenKind::Rp, TokenStream::Operand))
        return false;
    if (matched) {
        handler.setEndPosition(listNode, pos().end);
        return true;
    }

    while (true) {
        bool spread = false;
        uint32_t begin = 0;
        if (!tokenStream.matchToken(&matched, TokenKind::TripleDot, TokenStream::Operand))
            return false;
        if (matched) {
            spread = true;
            begin = pos().begin;
            *isSpread = true;
        }

        Node argNode = assignExpr(InAllowed, yieldHandling, TripledotProhibited, possibleError);
        if (!argNode)
            return false;
        if (spread) {
            argNode = handler.newSpread(begin, argNode);
            if (!argNode)
                return false;
        }

        handler.addList(listNode, argNode);

        bool matched;
        if (!tokenStream.matchToken(&matched, TokenKind::Comma, TokenStream::Operand))
            return false;
        if (!matched)
            break;

        TokenKind tt;
        if (!tokenStream.peekToken(&tt, TokenStream::Operand))
            return null();
        if (tt == TokenKind::Rp)
            break;
    }

    MUST_MATCH_TOKEN_MOD(TokenKind::Rp, TokenStream::Operand, JSMSG_PAREN_AFTER_ARGS);

    handler.setEndPosition(listNode, pos().end);
    return true;
}

bool
ParserBase::checkAndMarkSuperScope()
{
    if (!pc->sc()->allowSuperProperty())
        return false;

    pc->setSuperScopeNeedsHomeObject();
    return true;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::memberExpr(YieldHandling yieldHandling,
                                               TripledotHandling tripledotHandling,
                                               ExpressionClosure expressionClosureHandling,
                                               TokenKind tt, bool allowCallSyntax /* = true */,
                                               PossibleError* possibleError /* = nullptr */,
                                               InvokedPrediction invoked /* = PredictUninvoked */)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(tt));

    Node lhs;

    if (!CheckRecursionLimit(context))
        return null();

    /* Check for new expression first. */
    if (tt == TokenKind::New) {
        uint32_t newBegin = pos().begin;
        // Make sure this wasn't a |new.target| in disguise.
        Node newTarget;
        if (!tryNewTarget(newTarget))
            return null();
        if (newTarget) {
            lhs = newTarget;
        } else {
            // Gotten by tryNewTarget
            tt = anyChars.currentToken().type;
            Node ctorExpr = memberExpr(yieldHandling, TripledotProhibited,
                                       ExpressionClosure::Forbidden, tt,
                                       /* allowCallSyntax = */ false,
                                       /* possibleError = */ nullptr, PredictInvoked);
            if (!ctorExpr)
                return null();

            lhs = handler.newNewExpression(newBegin, ctorExpr);
            if (!lhs)
                return null();

            bool matched;
            if (!tokenStream.matchToken(&matched, TokenKind::Lp))
                return null();
            if (matched) {
                bool isSpread = false;
                if (!argumentList(yieldHandling, lhs, &isSpread))
                    return null();
                if (isSpread)
                    handler.setOp(lhs, JSOP_SPREADNEW);
            }
        }
    } else if (tt == TokenKind::Super) {
        Node thisName = newThisName();
        if (!thisName)
            return null();
        lhs = handler.newSuperBase(thisName, pos());
        if (!lhs)
            return null();
    } else {
        lhs = primaryExpr(yieldHandling, tripledotHandling, expressionClosureHandling, tt,
                          possibleError, invoked);
        if (!lhs)
            return null();

        if (handler.isExpressionClosure(lhs))
            return lhs;
    }

    MOZ_ASSERT_IF(handler.isSuperBase(lhs), anyChars.isCurrentTokenType(TokenKind::Super));

    while (true) {
        if (!tokenStream.getToken(&tt))
            return null();
        if (tt == TokenKind::Eof)
            break;

        Node nextMember;
        if (tt == TokenKind::Dot) {
            if (!tokenStream.getToken(&tt))
                return null();
            if (TokenKindIsPossibleIdentifierName(tt)) {
                PropertyName* field = anyChars.currentName();
                if (handler.isSuperBase(lhs) && !checkAndMarkSuperScope()) {
                    error(JSMSG_BAD_SUPERPROP, "property");
                    return null();
                }
                nextMember = handler.newPropertyAccess(lhs, field, pos().end);
                if (!nextMember)
                    return null();
            } else {
                error(JSMSG_NAME_AFTER_DOT);
                return null();
            }
        } else if (tt == TokenKind::Lb) {
            Node propExpr = expr(InAllowed, yieldHandling, TripledotProhibited);
            if (!propExpr)
                return null();

            MUST_MATCH_TOKEN_MOD(TokenKind::Rb, TokenStream::Operand, JSMSG_BRACKET_IN_INDEX);

            if (handler.isSuperBase(lhs) && !checkAndMarkSuperScope()) {
                error(JSMSG_BAD_SUPERPROP, "member");
                return null();
            }
            nextMember = handler.newPropertyByValue(lhs, propExpr, pos().end);
            if (!nextMember)
                return null();
        } else if ((allowCallSyntax && tt == TokenKind::Lp) ||
                   tt == TokenKind::TemplateHead ||
                   tt == TokenKind::NoSubsTemplate)
        {
            if (handler.isSuperBase(lhs)) {
                if (!pc->sc()->allowSuperCall()) {
                    error(JSMSG_BAD_SUPERCALL);
                    return null();
                }

                if (tt != TokenKind::Lp) {
                    error(JSMSG_BAD_SUPER);
                    return null();
                }

                nextMember = handler.newSuperCall(lhs);
                if (!nextMember)
                    return null();

                // Despite the fact that it's impossible to have |super()| in a
                // generator, we still inherit the yieldHandling of the
                // memberExpression, per spec. Curious.
                bool isSpread = false;
                if (!argumentList(yieldHandling, nextMember, &isSpread))
                    return null();

                if (isSpread)
                    handler.setOp(nextMember, JSOP_SPREADSUPERCALL);

                Node thisName = newThisName();
                if (!thisName)
                    return null();

                nextMember = handler.newSetThis(thisName, nextMember);
                if (!nextMember)
                    return null();
            } else {
                if (options().selfHostingMode && handler.isPropertyAccess(lhs)) {
                    error(JSMSG_SELFHOSTED_METHOD_CALL);
                    return null();
                }

                TokenPos nextMemberPos = pos();
                nextMember = tt == TokenKind::Lp
                             ? handler.newCall(nextMemberPos)
                             : handler.newTaggedTemplate(nextMemberPos);
                if (!nextMember)
                    return null();

                JSOp op = JSOP_CALL;
                bool maybeAsyncArrow = false;
                if (PropertyName* prop = handler.maybeDottedProperty(lhs)) {
                    // Use the JSOP_FUN{APPLY,CALL} optimizations given the
                    // right syntax.
                    if (prop == context->names().apply) {
                        op = JSOP_FUNAPPLY;
                        if (pc->isFunctionBox())
                            pc->functionBox()->usesApply = true;
                    } else if (prop == context->names().call) {
                        op = JSOP_FUNCALL;
                    }
                } else if (tt == TokenKind::Lp) {
                    if (handler.isAsyncKeyword(lhs, context)) {
                        // |async (| can be the start of an async arrow
                        // function, so we need to defer reporting possible
                        // errors from destructuring syntax. To give better
                        // error messages, we only allow the AsyncArrowHead
                        // part of the CoverCallExpressionAndAsyncArrowHead
                        // syntax when the initial name is "async".
                        maybeAsyncArrow = true;
                    } else if (handler.isEvalName(lhs, context)) {
                        // Select the right EVAL op and flag pc as having a
                        // direct eval.
                        op = pc->sc()->strict() ? JSOP_STRICTEVAL : JSOP_EVAL;
                        pc->sc()->setBindingsAccessedDynamically();
                        pc->sc()->setHasDirectEval();

                        // In non-strict mode code, direct calls to eval can
                        // add variables to the call object.
                        if (pc->isFunctionBox() && !pc->sc()->strict())
                            pc->functionBox()->setHasExtensibleScope();

                        // If we're in a method, mark the method as requiring
                        // support for 'super', since direct eval code can use
                        // it. (If we're not in a method, that's fine, so
                        // ignore the return value.)
                        checkAndMarkSuperScope();
                    }
                }

                handler.setBeginPosition(nextMember, lhs);
                handler.addList(nextMember, lhs);

                if (tt == TokenKind::Lp) {
                    bool isSpread = false;
                    PossibleError* asyncPossibleError = maybeAsyncArrow ? possibleError : nullptr;
                    if (!argumentList(yieldHandling, nextMember, &isSpread, asyncPossibleError))
                        return null();
                    if (isSpread) {
                        if (op == JSOP_EVAL)
                            op = JSOP_SPREADEVAL;
                        else if (op == JSOP_STRICTEVAL)
                            op = JSOP_STRICTSPREADEVAL;
                        else
                            op = JSOP_SPREADCALL;
                    }
                } else {
                    if (!taggedTemplate(yieldHandling, nextMember, tt))
                        return null();
                }
                handler.setOp(nextMember, op);
            }
        } else {
            anyChars.ungetToken();
            if (handler.isSuperBase(lhs))
                break;
            return lhs;
        }

        lhs = nextMember;
    }

    if (handler.isSuperBase(lhs)) {
        error(JSMSG_BAD_SUPER);
        return null();
    }

    return lhs;
}

template <class ParseHandler>
inline typename ParseHandler::Node
PerHandlerParser<ParseHandler>::newName(PropertyName* name)
{
    return newName(name, pos());
}

template <class ParseHandler>
inline typename ParseHandler::Node
PerHandlerParser<ParseHandler>::newName(PropertyName* name, TokenPos pos)
{
    return handler.newName(name, pos, context);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::checkLabelOrIdentifierReference(PropertyName* ident,
                                                                    uint32_t offset,
                                                                    YieldHandling yieldHandling,
                                                                    TokenKind hint /* = TokenKind::Limit */)
{
    TokenKind tt;
    if (hint == TokenKind::Limit) {
        tt = ReservedWordTokenKind(ident);
    } else {
        MOZ_ASSERT(hint == ReservedWordTokenKind(ident), "hint doesn't match actual token kind");
        tt = hint;
    }

    if (tt == TokenKind::Name)
        return true;
    if (TokenKindIsContextualKeyword(tt)) {
        if (tt == TokenKind::Yield) {
            if (yieldHandling == YieldIsKeyword) {
                errorAt(offset, JSMSG_RESERVED_ID, "yield");
                return false;
            }
            if (pc->sc()->needStrictChecks()) {
                if (!strictModeErrorAt(offset, JSMSG_RESERVED_ID, "yield"))
                    return false;
            }
            return true;
        }
        if (tt == TokenKind::Await) {
            if (awaitIsKeyword()) {
                errorAt(offset, JSMSG_RESERVED_ID, "await");
                return false;
            }
            return true;
        }
        if (pc->sc()->needStrictChecks()) {
            if (tt == TokenKind::Let) {
                if (!strictModeErrorAt(offset, JSMSG_RESERVED_ID, "let"))
                    return false;
                return true;
            }
            if (tt == TokenKind::Static) {
                if (!strictModeErrorAt(offset, JSMSG_RESERVED_ID, "static"))
                    return false;
                return true;
            }
        }
        return true;
    }
    if (TokenKindIsStrictReservedWord(tt)) {
        if (pc->sc()->needStrictChecks()) {
            if (!strictModeErrorAt(offset, JSMSG_RESERVED_ID, ReservedWordToCharZ(tt)))
                return false;
        }
        return true;
    }
    if (TokenKindIsKeyword(tt) || TokenKindIsReservedWordLiteral(tt)) {
        errorAt(offset, JSMSG_INVALID_ID, ReservedWordToCharZ(tt));
        return false;
    }
    if (TokenKindIsFutureReservedWord(tt)) {
        errorAt(offset, JSMSG_RESERVED_ID, ReservedWordToCharZ(tt));
        return false;
    }
    MOZ_ASSERT_UNREACHABLE("Unexpected reserved word kind.");
    return false;
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::checkBindingIdentifier(PropertyName* ident, uint32_t offset,
                                                           YieldHandling yieldHandling,
                                                           TokenKind hint /* = TokenKind::Limit */)
{
    if (pc->sc()->needStrictChecks()) {
        if (ident == context->names().arguments) {
            if (!strictModeErrorAt(offset, JSMSG_BAD_STRICT_ASSIGN, "arguments"))
                return false;
            return true;
        }

        if (ident == context->names().eval) {
            if (!strictModeErrorAt(offset, JSMSG_BAD_STRICT_ASSIGN, "eval"))
                return false;
            return true;
        }
    }

    return checkLabelOrIdentifierReference(ident, offset, yieldHandling, hint);
}

template <class ParseHandler, typename CharT>
PropertyName*
GeneralParser<ParseHandler, CharT>::labelOrIdentifierReference(YieldHandling yieldHandling)
{
    // ES 2017 draft 12.1.1.
    //   StringValue of IdentifierName normalizes any Unicode escape sequences
    //   in IdentifierName hence such escapes cannot be used to write an
    //   Identifier whose code point sequence is the same as a ReservedWord.
    //
    // Use PropertyName* instead of TokenKind to reflect the normalization.

    // Unless the name contains escapes, we can reuse the current TokenKind
    // to determine if the name is a restricted identifier.
    TokenKind hint = !anyChars.currentNameHasEscapes()
                     ? anyChars.currentToken().type
                     : TokenKind::Limit;
    RootedPropertyName ident(context, anyChars.currentName());
    if (!checkLabelOrIdentifierReference(ident, pos().begin, yieldHandling, hint))
        return nullptr;
    return ident;
}

template <class ParseHandler, typename CharT>
PropertyName*
GeneralParser<ParseHandler, CharT>::bindingIdentifier(YieldHandling yieldHandling)
{
    TokenKind hint = !anyChars.currentNameHasEscapes()
                     ? anyChars.currentToken().type
                     : TokenKind::Limit;
    RootedPropertyName ident(context, anyChars.currentName());
    if (!checkBindingIdentifier(ident, pos().begin, yieldHandling, hint))
        return nullptr;
    return ident;
}

template <class ParseHandler>
typename ParseHandler::Node
PerHandlerParser<ParseHandler>::identifierReference(Handle<PropertyName*> name)
{
    Node pn = newName(name);
    if (!pn)
        return null();

    if (!noteUsedName(name))
        return null();

    return pn;
}

template <class ParseHandler>
typename ParseHandler::Node
PerHandlerParser<ParseHandler>::stringLiteral()
{
    return handler.newStringLiteral(anyChars.currentToken().atom(), pos());
}

template <class ParseHandler>
typename ParseHandler::Node
PerHandlerParser<ParseHandler>::noSubstitutionTaggedTemplate()
{
    if (anyChars.hasInvalidTemplateEscape()) {
        anyChars.clearInvalidTemplateEscape();
        return handler.newRawUndefinedLiteral(pos());
    }

    return handler.newTemplateStringLiteral(anyChars.currentToken().atom(), pos());
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::noSubstitutionUntaggedTemplate()
{
    if (!tokenStream.checkForInvalidTemplateEscapeError())
        return null();

    return handler.newTemplateStringLiteral(anyChars.currentToken().atom(), pos());
}

template <typename CharT>
ParseNode*
Parser<FullParseHandler, CharT>::newRegExp()
{
    MOZ_ASSERT(!options().selfHostingMode);

    static_assert(mozilla::IsSame<CharT, char16_t>::value,
                  "code below will need changing for UTF-8 handling");

    // Create the regexp and check its syntax.
    const CharT* chars = tokenStream.getTokenbuf().begin();
    size_t length = tokenStream.getTokenbuf().length();
    RegExpFlag flags = anyChars.currentToken().regExpFlags();

    Rooted<RegExpObject*> reobj(context);
    reobj = RegExpObject::create(context, chars, length, flags, anyChars, alloc, TenuredObject);
    if (!reobj)
        return null();

    return handler.newRegExp(reobj, pos(), *this);
}

template <typename CharT>
SyntaxParseHandler::Node
Parser<SyntaxParseHandler, CharT>::newRegExp()
{
    MOZ_ASSERT(!options().selfHostingMode);

    static_assert(mozilla::IsSame<CharT, char16_t>::value,
                  "code below will need changing for UTF-8 handling");

    // Only check the regexp's syntax, but don't create a regexp object.
    const CharT* chars = tokenStream.getTokenbuf().begin();
    size_t length = tokenStream.getTokenbuf().length();
    RegExpFlag flags = anyChars.currentToken().regExpFlags();

    mozilla::Range<const CharT> source(chars, length);
    if (!js::irregexp::ParsePatternSyntax(anyChars, alloc, source, flags & UnicodeFlag))
        return null();

    return handler.newRegExp(SyntaxParseHandler::NodeGeneric, pos(), *this);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::newRegExp()
{
    return asFinalParser()->newRegExp();
}

// |exprPossibleError| is the PossibleError state within |expr|,
// |possibleError| is the surrounding PossibleError state.
template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::checkDestructuringAssignmentTarget(Node expr, TokenPos exprPos,
                                                                       PossibleError* exprPossibleError,
                                                                       PossibleError* possibleError,
                                                                       TargetBehavior behavior)
{
    // Report any pending expression error if we're definitely not in a
    // destructuring context or the possible destructuring target is a
    // property accessor.
    if (!possibleError || handler.isPropertyAccess(expr))
        return exprPossibleError->checkForExpressionError();

    // |expr| may end up as a destructuring assignment target, so we need to
    // validate it's either a name or can be parsed as a nested destructuring
    // pattern. Property accessors are also valid assignment targets, but
    // those are already handled above.

    exprPossibleError->transferErrorsTo(possibleError);

    // Return early if a pending destructuring error is already present.
    if (possibleError->hasPendingDestructuringError())
        return true;

    if (handler.isName(expr)) {
        checkDestructuringAssignmentName(expr, exprPos, possibleError);
        return true;
    }

    if (handler.isUnparenthesizedDestructuringPattern(expr)) {
        if (behavior == TargetBehavior::ForbidAssignmentPattern)
            possibleError->setPendingDestructuringErrorAt(exprPos, JSMSG_BAD_DESTRUCT_TARGET);
        return true;
    }

    // Parentheses are forbidden around destructuring *patterns* (but allowed
    // around names). Use our nicer error message for parenthesized, nested
    // patterns if nested destructuring patterns are allowed.
    if (handler.isParenthesizedDestructuringPattern(expr) &&
        behavior != TargetBehavior::ForbidAssignmentPattern)
    {
        possibleError->setPendingDestructuringErrorAt(exprPos, JSMSG_BAD_DESTRUCT_PARENS);
    } else {
        possibleError->setPendingDestructuringErrorAt(exprPos, JSMSG_BAD_DESTRUCT_TARGET);
    }

    return true;
}

template <class ParseHandler, typename CharT>
void
GeneralParser<ParseHandler, CharT>::checkDestructuringAssignmentName(Node name, TokenPos namePos,
                                                                     PossibleError* possibleError)
{
#ifdef DEBUG
    // GCC 8.0.1 crashes if this is a one-liner.
    bool isName = handler.isName(name);
    MOZ_ASSERT(isName);
#endif

    // Return early if a pending destructuring error is already present.
    if (possibleError->hasPendingDestructuringError())
        return;

    if (pc->sc()->needStrictChecks()) {
        if (handler.isArgumentsName(name, context)) {
            if (pc->sc()->strict()) {
                possibleError->setPendingDestructuringErrorAt(namePos,
                                                              JSMSG_BAD_STRICT_ASSIGN_ARGUMENTS);
            } else {
                possibleError->setPendingDestructuringWarningAt(namePos,
                                                                JSMSG_BAD_STRICT_ASSIGN_ARGUMENTS);
            }
            return;
        }

        if (handler.isEvalName(name, context)) {
            if (pc->sc()->strict()) {
                possibleError->setPendingDestructuringErrorAt(namePos,
                                                              JSMSG_BAD_STRICT_ASSIGN_EVAL);
            } else {
                possibleError->setPendingDestructuringWarningAt(namePos,
                                                                JSMSG_BAD_STRICT_ASSIGN_EVAL);
            }
            return;
        }
    }
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::checkDestructuringAssignmentElement(Node expr, TokenPos exprPos,
                                                                        PossibleError* exprPossibleError,
                                                                        PossibleError* possibleError)
{
    // ES2018 draft rev 0719f44aab93215ed9a626b2f45bd34f36916834
    // 12.15.5 Destructuring Assignment
    //
    // AssignmentElement[Yield, Await]:
    //   DestructuringAssignmentTarget[?Yield, ?Await]
    //   DestructuringAssignmentTarget[?Yield, ?Await] Initializer[+In, ?Yield, ?Await]

    // If |expr| is an assignment element with an initializer expression, its
    // destructuring assignment target was already validated in assignExpr().
    // Otherwise we need to check that |expr| is a valid destructuring target.
    if (handler.isUnparenthesizedAssignment(expr)) {
        // Report any pending expression error if we're definitely not in a
        // destructuring context.
        if (!possibleError)
            return exprPossibleError->checkForExpressionError();

        exprPossibleError->transferErrorsTo(possibleError);
        return true;
    }
    return checkDestructuringAssignmentTarget(expr, exprPos, exprPossibleError, possibleError);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::arrayInitializer(YieldHandling yieldHandling,
                                                     PossibleError* possibleError)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lb));

    uint32_t begin = pos().begin;
    Node literal = handler.newArrayLiteral(begin);
    if (!literal)
        return null();

    TokenKind tt;
    if (!tokenStream.getToken(&tt, TokenStream::Operand))
        return null();

    if (tt == TokenKind::Rb) {
        /*
         * Mark empty arrays as non-constant, since we cannot easily
         * determine their type.
         */
        handler.setListFlag(literal, PNX_NONCONST);
    } else {
        anyChars.ungetToken();

        for (uint32_t index = 0; ; index++) {
            if (index >= NativeObject::MAX_DENSE_ELEMENTS_COUNT) {
                error(JSMSG_ARRAY_INIT_TOO_BIG);
                return null();
            }

            TokenKind tt;
            if (!tokenStream.peekToken(&tt, TokenStream::Operand))
                return null();
            if (tt == TokenKind::Rb)
                break;

            if (tt == TokenKind::Comma) {
                tokenStream.consumeKnownToken(TokenKind::Comma, TokenStream::Operand);
                if (!handler.addElision(literal, pos()))
                    return null();
                continue;
            }

            if (tt == TokenKind::TripleDot) {
                tokenStream.consumeKnownToken(TokenKind::TripleDot, TokenStream::Operand);
                uint32_t begin = pos().begin;

                TokenPos innerPos;
                if (!tokenStream.peekTokenPos(&innerPos, TokenStream::Operand))
                    return null();

                PossibleError possibleErrorInner(*this);
                Node inner = assignExpr(InAllowed, yieldHandling, TripledotProhibited,
                                        &possibleErrorInner);
                if (!inner)
                    return null();
                if (!checkDestructuringAssignmentTarget(inner, innerPos, &possibleErrorInner,
                                                        possibleError))
                {
                    return null();
                }

                if (!handler.addSpreadElement(literal, begin, inner))
                    return null();
            } else {
                TokenPos elementPos;
                if (!tokenStream.peekTokenPos(&elementPos, TokenStream::Operand))
                    return null();

                PossibleError possibleErrorInner(*this);
                Node element = assignExpr(InAllowed, yieldHandling, TripledotProhibited,
                                          &possibleErrorInner);
                if (!element)
                    return null();
                if (!checkDestructuringAssignmentElement(element, elementPos, &possibleErrorInner,
                                                         possibleError))
                {
                    return null();
                }
                if (foldConstants && !FoldConstants(context, &element, this))
                    return null();
                handler.addArrayElement(literal, element);
            }

            bool matched;
            if (!tokenStream.matchToken(&matched, TokenKind::Comma, TokenStream::Operand))
                return null();
            if (!matched)
                break;

            if (tt == TokenKind::TripleDot && possibleError)
                possibleError->setPendingDestructuringErrorAt(pos(), JSMSG_REST_WITH_COMMA);
        }

        MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rb, TokenStream::Operand,
                                         reportMissingClosing(JSMSG_BRACKET_AFTER_LIST,
                                                              JSMSG_BRACKET_OPENED, begin));
    }

    handler.setEndPosition(literal, pos().end);
    return literal;
}

static JSAtom*
DoubleToAtom(JSContext* cx, double value)
{
    // This is safe because doubles can not be moved.
    Value tmp = DoubleValue(value);
    return ToAtom<CanGC>(cx, HandleValue::fromMarkedLocation(&tmp));
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::propertyName(YieldHandling yieldHandling,
                                                 const Maybe<DeclarationKind>& maybeDecl,
                                                 Node propList,
                                                 PropertyType* propType,
                                                 MutableHandleAtom propAtom)
{
    TokenKind ltok;
    if (!tokenStream.getToken(&ltok))
        return null();

    MOZ_ASSERT(ltok != TokenKind::Rc, "caller should have handled TokenKind::Rc");

    bool isGenerator = false;
    bool isAsync = false;

    if (ltok == TokenKind::Async) {
        // AsyncMethod[Yield, Await]:
        //   async [no LineTerminator here] PropertyName[?Yield, ?Await] ...
        //
        //  AsyncGeneratorMethod[Yield, Await]:
        //    async [no LineTerminator here] * PropertyName[?Yield, ?Await] ...
        //
        // PropertyName:
        //   LiteralPropertyName
        //   ComputedPropertyName[?Yield, ?Await]
        //
        // LiteralPropertyName:
        //   IdentifierName
        //   StringLiteral
        //   NumericLiteral
        //
        // ComputedPropertyName[Yield, Await]:
        //   [ ...
        TokenKind tt = TokenKind::Eof;
        if (!tokenStream.peekTokenSameLine(&tt))
            return null();
        if (tt == TokenKind::String || tt == TokenKind::Number || tt == TokenKind::Lb ||
            TokenKindIsPossibleIdentifierName(tt) || tt == TokenKind::Mul)
        {
            isAsync = true;
            tokenStream.consumeKnownToken(tt);
            ltok = tt;
        }
    }

    if (ltok == TokenKind::Mul) {
        isGenerator = true;
        if (!tokenStream.getToken(&ltok))
            return null();
    }

    propAtom.set(nullptr);
    Node propName;
    switch (ltok) {
      case TokenKind::Number:
        propAtom.set(DoubleToAtom(context, anyChars.currentToken().number()));
        if (!propAtom.get())
            return null();
        propName = newNumber(anyChars.currentToken());
        if (!propName)
            return null();
        break;

      case TokenKind::String: {
        propAtom.set(anyChars.currentToken().atom());
        uint32_t index;
        if (propAtom->isIndex(&index)) {
            propName = handler.newNumber(index, NoDecimal, pos());
            if (!propName)
                return null();
            break;
        }
        propName = stringLiteral();
        if (!propName)
            return null();
        break;
      }

      case TokenKind::Lb:
        propName = computedPropertyName(yieldHandling, maybeDecl, propList);
        if (!propName)
            return null();
        break;

      default: {
        if (!TokenKindIsPossibleIdentifierName(ltok)) {
            error(JSMSG_UNEXPECTED_TOKEN, "property name", TokenKindToDesc(ltok));
            return null();
        }

        propAtom.set(anyChars.currentName());
        // Do not look for accessor syntax on generator or async methods.
        if (isGenerator || isAsync || !(ltok == TokenKind::Get || ltok == TokenKind::Set)) {
            propName = handler.newObjectLiteralPropertyName(propAtom, pos());
            if (!propName)
                return null();
            break;
        }

        *propType = ltok == TokenKind::Get ? PropertyType::Getter : PropertyType::Setter;

        // We have parsed |get| or |set|. Look for an accessor property
        // name next.
        TokenKind tt;
        if (!tokenStream.peekToken(&tt))
            return null();
        if (TokenKindIsPossibleIdentifierName(tt)) {
            tokenStream.consumeKnownToken(tt);

            propAtom.set(anyChars.currentName());
            return handler.newObjectLiteralPropertyName(propAtom, pos());
        }
        if (tt == TokenKind::String) {
            tokenStream.consumeKnownToken(TokenKind::String);

            propAtom.set(anyChars.currentToken().atom());

            uint32_t index;
            if (propAtom->isIndex(&index)) {
                propAtom.set(DoubleToAtom(context, index));
                if (!propAtom.get())
                    return null();
                return handler.newNumber(index, NoDecimal, pos());
            }
            return stringLiteral();
        }
        if (tt == TokenKind::Number) {
            tokenStream.consumeKnownToken(TokenKind::Number);

            propAtom.set(DoubleToAtom(context, anyChars.currentToken().number()));
            if (!propAtom.get())
                return null();
            return newNumber(anyChars.currentToken());
        }
        if (tt == TokenKind::Lb) {
            tokenStream.consumeKnownToken(TokenKind::Lb);

            return computedPropertyName(yieldHandling, maybeDecl, propList);
        }

        // Not an accessor property after all.
        propName = handler.newObjectLiteralPropertyName(propAtom.get(), pos());
        if (!propName)
            return null();
        break;
      }
    }

    TokenKind tt;
    if (!tokenStream.getToken(&tt))
        return null();

    if (tt == TokenKind::Colon) {
        if (isGenerator || isAsync) {
            error(JSMSG_BAD_PROP_ID);
            return null();
        }
        *propType = PropertyType::Normal;
        return propName;
    }

    if (TokenKindIsPossibleIdentifierName(ltok) &&
        (tt == TokenKind::Comma || tt == TokenKind::Rc || tt == TokenKind::Assign))
    {
        if (isGenerator || isAsync) {
            error(JSMSG_BAD_PROP_ID);
            return null();
        }

        anyChars.ungetToken();
        anyChars.addModifierException(TokenStream::OperandIsNone);
        *propType = tt == TokenKind::Assign
                    ? PropertyType::CoverInitializedName
                    : PropertyType::Shorthand;
        return propName;
    }

    if (tt == TokenKind::Lp) {
        anyChars.ungetToken();

        if (isGenerator && isAsync)
            *propType = PropertyType::AsyncGeneratorMethod;
        else if (isGenerator)
            *propType = PropertyType::GeneratorMethod;
        else if (isAsync)
            *propType = PropertyType::AsyncMethod;
        else
            *propType = PropertyType::Method;
        return propName;
    }

    error(JSMSG_COLON_AFTER_ID);
    return null();
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::computedPropertyName(YieldHandling yieldHandling,
                                                         const Maybe<DeclarationKind>& maybeDecl,
                                                         Node literal)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lb));

    uint32_t begin = pos().begin;

    if (maybeDecl) {
        if (*maybeDecl == DeclarationKind::FormalParameter)
            pc->functionBox()->hasParameterExprs = true;
    } else {
        handler.setListFlag(literal, PNX_NONCONST);
    }

    Node assignNode = assignExpr(InAllowed, yieldHandling, TripledotProhibited);
    if (!assignNode)
        return null();

    MUST_MATCH_TOKEN_MOD(TokenKind::Rb, TokenStream::Operand, JSMSG_COMP_PROP_UNTERM_EXPR);
    return handler.newComputedName(assignNode, begin, pos().end);
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::objectLiteral(YieldHandling yieldHandling,
                                                  PossibleError* possibleError)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lc));

    uint32_t openedPos = pos().begin;

    Node literal = handler.newObjectLiteral(pos().begin);
    if (!literal)
        return null();

    bool seenPrototypeMutation = false;
    bool seenCoverInitializedName = false;
    Maybe<DeclarationKind> declKind = Nothing();
    RootedAtom propAtom(context);
    for (;;) {
        TokenKind tt;
        if (!tokenStream.peekToken(&tt))
            return null();
        if (tt == TokenKind::Rc) {
            anyChars.addModifierException(TokenStream::OperandIsNone);
            break;
        }

        if (tt == TokenKind::TripleDot) {
            tokenStream.consumeKnownToken(TokenKind::TripleDot);
            uint32_t begin = pos().begin;

            TokenPos innerPos;
            if (!tokenStream.peekTokenPos(&innerPos, TokenStream::Operand))
                return null();

            PossibleError possibleErrorInner(*this);
            Node inner = assignExpr(InAllowed, yieldHandling, TripledotProhibited,
                                    &possibleErrorInner);
            if (!inner)
                return null();
            if (!checkDestructuringAssignmentTarget(inner, innerPos, &possibleErrorInner,
                                                    possibleError,
                                                    TargetBehavior::ForbidAssignmentPattern))
            {
                return null();
            }
            if (!handler.addSpreadProperty(literal, begin, inner))
                return null();
        } else {
            TokenPos namePos = anyChars.nextToken().pos;

            PropertyType propType;
            Node propName = propertyName(yieldHandling, declKind, literal, &propType, &propAtom);
            if (!propName)
                return null();

            if (propType == PropertyType::Normal) {
                TokenPos exprPos;
                if (!tokenStream.peekTokenPos(&exprPos, TokenStream::Operand))
                    return null();

                PossibleError possibleErrorInner(*this);
                Node propExpr = assignExpr(InAllowed, yieldHandling, TripledotProhibited,
                                           &possibleErrorInner);
                if (!propExpr)
                    return null();

                handler.checkAndSetIsDirectRHSAnonFunction(propExpr);

                if (!checkDestructuringAssignmentElement(propExpr, exprPos, &possibleErrorInner,
                                                         possibleError))
                {
                    return null();
                }

                if (foldConstants && !FoldConstants(context, &propExpr, this))
                    return null();

                if (propAtom == context->names().proto) {
                    if (seenPrototypeMutation) {
                        // Directly report the error when we're definitely not
                        // in a destructuring context.
                        if (!possibleError) {
                            errorAt(namePos.begin, JSMSG_DUPLICATE_PROTO_PROPERTY);
                            return null();
                        }

                        // Otherwise delay error reporting until we've
                        // determined whether or not we're destructuring.
                        possibleError->setPendingExpressionErrorAt(namePos,
                                                                   JSMSG_DUPLICATE_PROTO_PROPERTY);
                    }
                    seenPrototypeMutation = true;

                    // Note: this occurs *only* if we observe TokenKind::Colon!  Only
                    // __proto__: v mutates [[Prototype]].  Getters, setters,
                    // method/generator definitions, computed property name
                    // versions of all of these, and shorthands do not.
                    if (!handler.addPrototypeMutation(literal, namePos.begin, propExpr))
                        return null();
                } else {
                    if (!handler.isConstant(propExpr))
                        handler.setListFlag(literal, PNX_NONCONST);

                    if (!handler.addPropertyDefinition(literal, propName, propExpr))
                        return null();
                }
            } else if (propType == PropertyType::Shorthand) {
                /*
                 * Support, e.g., |({x, y} = o)| as destructuring shorthand
                 * for |({x: x, y: y} = o)|, and |var o = {x, y}| as
                 * initializer shorthand for |var o = {x: x, y: y}|.
                 */
                Rooted<PropertyName*> name(context, identifierReference(yieldHandling));
                if (!name)
                    return null();

                Node nameExpr = identifierReference(name);
                if (!nameExpr)
                    return null();

                if (possibleError)
                    checkDestructuringAssignmentName(nameExpr, namePos, possibleError);

                if (!handler.addShorthand(literal, propName, nameExpr))
                    return null();
            } else if (propType == PropertyType::CoverInitializedName) {
                /*
                 * Support, e.g., |({x=1, y=2} = o)| as destructuring
                 * shorthand with default values, as per ES6 12.14.5
                 */
                Rooted<PropertyName*> name(context, identifierReference(yieldHandling));
                if (!name)
                    return null();

                Node lhs = identifierReference(name);
                if (!lhs)
                    return null();

                tokenStream.consumeKnownToken(TokenKind::Assign);

                if (!seenCoverInitializedName) {
                    // "shorthand default" or "CoverInitializedName" syntax is
                    // only valid in the case of destructuring.
                    seenCoverInitializedName = true;

                    if (!possibleError) {
                        // Destructuring defaults are definitely not allowed
                        // in this object literal, because of something the
                        // caller knows about the preceding code. For example,
                        // maybe the preceding token is an operator:
                        // |x + {y=z}|.
                        error(JSMSG_COLON_AFTER_ID);
                        return null();
                    }

                    // Here we set a pending error so that later in the parse,
                    // once we've determined whether or not we're
                    // destructuring, the error can be reported or ignored
                    // appropriately.
                    possibleError->setPendingExpressionErrorAt(pos(), JSMSG_COLON_AFTER_ID);
                }

                if (const char* chars = nameIsArgumentsOrEval(lhs)) {
                    // |chars| is "arguments" or "eval" here.
                    if (!strictModeErrorAt(namePos.begin, JSMSG_BAD_STRICT_ASSIGN, chars))
                        return null();
                }

                Node rhs = assignExpr(InAllowed, yieldHandling, TripledotProhibited);
                if (!rhs)
                    return null();

                handler.checkAndSetIsDirectRHSAnonFunction(rhs);

                Node propExpr = handler.newAssignment(ParseNodeKind::Assign, lhs, rhs);
                if (!propExpr)
                    return null();

                if (!handler.addPropertyDefinition(literal, propName, propExpr))
                    return null();
            } else {
                RootedAtom funName(context);
                if (!anyChars.isCurrentTokenType(TokenKind::Rb)) {
                    funName = propAtom;

                    if (propType == PropertyType::Getter || propType == PropertyType::Setter) {
                        funName = prefixAccessorName(propType, propAtom);
                        if (!funName)
                            return null();
                    }
                }

                Node fn = methodDefinition(namePos.begin, propType, funName);
                if (!fn)
                    return null();

                handler.checkAndSetIsDirectRHSAnonFunction(fn);

                AccessorType atype = ToAccessorType(propType);
                if (!handler.addObjectMethodDefinition(literal, propName, fn, atype))
                    return null();

                if (possibleError) {
                    possibleError->setPendingDestructuringErrorAt(namePos,
                                                                  JSMSG_BAD_DESTRUCT_TARGET);
                }
            }
        }

        bool matched;
        if (!tokenStream.matchToken(&matched, TokenKind::Comma, TokenStream::Operand))
            return null();
        if (!matched)
            break;
        if (tt == TokenKind::TripleDot && possibleError)
            possibleError->setPendingDestructuringErrorAt(pos(), JSMSG_REST_WITH_COMMA);
    }

    MUST_MATCH_TOKEN_MOD_WITH_REPORT(TokenKind::Rc, TokenStream::Operand,
                                     reportMissingClosing(JSMSG_CURLY_AFTER_LIST,
                                                          JSMSG_CURLY_OPENED, openedPos));

    handler.setEndPosition(literal, pos().end);
    return literal;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::methodDefinition(uint32_t toStringStart, PropertyType propType,
                                                     HandleAtom funName)
{
    FunctionSyntaxKind kind;
    switch (propType) {
      case PropertyType::Getter:
        kind = Getter;
        break;

      case PropertyType::GetterNoExpressionClosure:
        kind = GetterNoExpressionClosure;
        break;

      case PropertyType::Setter:
        kind = Setter;
        break;

      case PropertyType::SetterNoExpressionClosure:
        kind = SetterNoExpressionClosure;
        break;

      case PropertyType::Method:
      case PropertyType::GeneratorMethod:
      case PropertyType::AsyncMethod:
      case PropertyType::AsyncGeneratorMethod:
        kind = Method;
        break;

      case PropertyType::Constructor:
        kind = ClassConstructor;
        break;

      case PropertyType::DerivedConstructor:
        kind = DerivedClassConstructor;
        break;

      default:
        MOZ_CRASH("unexpected property type");
    }

    GeneratorKind generatorKind = (propType == PropertyType::GeneratorMethod ||
                                   propType == PropertyType::AsyncGeneratorMethod)
                                  ? GeneratorKind::Generator
                                  : GeneratorKind::NotGenerator;

    FunctionAsyncKind asyncKind = (propType == PropertyType::AsyncMethod ||
                                   propType == PropertyType::AsyncGeneratorMethod)
                                  ? FunctionAsyncKind::AsyncFunction
                                  : FunctionAsyncKind::SyncFunction;

    YieldHandling yieldHandling = GetYieldHandling(generatorKind);

    Node pn = handler.newFunctionExpression(pos());
    if (!pn)
        return null();

    return functionDefinition(pn, toStringStart, InAllowed, yieldHandling, funName, kind,
                              generatorKind, asyncKind);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::tryNewTarget(Node &newTarget)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::New));

    newTarget = null();

    Node newHolder = handler.newPosHolder(pos());
    if (!newHolder)
        return false;

    uint32_t begin = pos().begin;

    // |new| expects to look for an operand, so we will honor that.
    TokenKind next;
    if (!tokenStream.getToken(&next, TokenStream::Operand))
        return false;

    // Don't unget the token, since lookahead cannot handle someone calling
    // getToken() with a different modifier. Callers should inspect currentToken().
    if (next != TokenKind::Dot)
        return true;

    if (!tokenStream.getToken(&next))
        return false;
    if (next != TokenKind::Target) {
        error(JSMSG_UNEXPECTED_TOKEN, "target", TokenKindToDesc(next));
        return false;
    }

    if (!pc->sc()->allowNewTarget()) {
        errorAt(begin, JSMSG_BAD_NEWTARGET);
        return false;
    }

    Node targetHolder = handler.newPosHolder(pos());
    if (!targetHolder)
        return false;

    newTarget = handler.newNewTarget(newHolder, targetHolder);
    return !!newTarget;
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::primaryExpr(YieldHandling yieldHandling,
                                                TripledotHandling tripledotHandling,
                                                ExpressionClosure expressionClosureHandling,
                                                TokenKind tt, PossibleError* possibleError,
                                                InvokedPrediction invoked /* = PredictUninvoked */)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(tt));
    if (!CheckRecursionLimit(context))
        return null();

    switch (tt) {
      case TokenKind::Function:
        return functionExpr(pos().begin, expressionClosureHandling, invoked,
                            FunctionAsyncKind::SyncFunction);

      case TokenKind::Class:
        return classDefinition(yieldHandling, ClassExpression, NameRequired);

      case TokenKind::Lb:
        return arrayInitializer(yieldHandling, possibleError);

      case TokenKind::Lc:
        return objectLiteral(yieldHandling, possibleError);

      case TokenKind::Lp: {
        TokenKind next;
        if (!tokenStream.peekToken(&next, TokenStream::Operand))
            return null();

        if (next == TokenKind::Rp) {
            // Not valid expression syntax, but this is valid in an arrow function
            // with no params: `() => body`.
            tokenStream.consumeKnownToken(TokenKind::Rp, TokenStream::Operand);

            if (!tokenStream.peekToken(&next))
                return null();
            if (next != TokenKind::Arrow) {
                error(JSMSG_UNEXPECTED_TOKEN, "expression", TokenKindToDesc(TokenKind::Rp));
                return null();
            }

            // Now just return something that will allow parsing to continue.
            // It doesn't matter what; when we reach the =>, we will rewind and
            // reparse the whole arrow function. See Parser::assignExpr.
            return handler.newNullLiteral(pos());
        }

        // Pass |possibleError| to support destructuring in arrow parameters.
        Node expr = exprInParens(InAllowed, yieldHandling, TripledotAllowed, possibleError);
        if (!expr)
            return null();
        MUST_MATCH_TOKEN_MOD(TokenKind::Rp, TokenStream::Operand, JSMSG_PAREN_IN_PAREN);
        return handler.parenthesize(expr);
      }

      case TokenKind::TemplateHead:
        return templateLiteral(yieldHandling);

      case TokenKind::NoSubsTemplate:
        return noSubstitutionUntaggedTemplate();

      case TokenKind::String:
        return stringLiteral();

      default: {
        if (!TokenKindIsPossibleIdentifier(tt)) {
            error(JSMSG_UNEXPECTED_TOKEN, "expression", TokenKindToDesc(tt));
            return null();
        }

        if (tt == TokenKind::Async) {
            TokenKind nextSameLine = TokenKind::Eof;
            if (!tokenStream.peekTokenSameLine(&nextSameLine))
                return null();

            if (nextSameLine == TokenKind::Function) {
                uint32_t toStringStart = pos().begin;
                tokenStream.consumeKnownToken(TokenKind::Function);
                return functionExpr(toStringStart, expressionClosureHandling, PredictUninvoked,
                                    FunctionAsyncKind::AsyncFunction);
            }
        }

        Rooted<PropertyName*> name(context, identifierReference(yieldHandling));
        if (!name)
            return null();

        return identifierReference(name);
      }

      case TokenKind::RegExp:
        return newRegExp();

      case TokenKind::Number:
        return newNumber(anyChars.currentToken());

      case TokenKind::True:
        return handler.newBooleanLiteral(true, pos());
      case TokenKind::False:
        return handler.newBooleanLiteral(false, pos());
      case TokenKind::This: {
        if (pc->isFunctionBox())
            pc->functionBox()->usesThis = true;
        Node thisName = null();
        if (pc->sc()->thisBinding() == ThisBinding::Function) {
            thisName = newThisName();
            if (!thisName)
                return null();
        }
        return handler.newThisLiteral(pos(), thisName);
      }
      case TokenKind::Null:
        return handler.newNullLiteral(pos());

      case TokenKind::TripleDot: {
        // This isn't valid expression syntax, but it's valid in an arrow
        // function as a trailing rest param: `(a, b, ...rest) => body`.  Check
        // if it's directly under
        // CoverParenthesizedExpressionAndArrowParameterList, and check for a
        // name, closing parenthesis, and arrow, and allow it only if all are
        // present.
        if (tripledotHandling != TripledotAllowed) {
            error(JSMSG_UNEXPECTED_TOKEN, "expression", TokenKindToDesc(tt));
            return null();
        }

        TokenKind next;
        if (!tokenStream.getToken(&next))
            return null();

        if (next == TokenKind::Lb || next == TokenKind::Lc) {
            // Validate, but don't store the pattern right now. The whole arrow
            // function is reparsed in functionFormalParametersAndBody().
            if (!destructuringDeclaration(DeclarationKind::CoverArrowParameter, yieldHandling,
                                          next))
            {
                return null();
            }
        } else {
            // This doesn't check that the provided name is allowed, e.g. if
            // the enclosing code is strict mode code, any of "let", "yield",
            // or "arguments" should be prohibited.  Argument-parsing code
            // handles that.
            if (!TokenKindIsPossibleIdentifier(next)) {
                error(JSMSG_UNEXPECTED_TOKEN, "rest argument name", TokenKindToDesc(next));
                return null();
            }
        }

        if (!tokenStream.getToken(&next))
            return null();
        if (next != TokenKind::Rp) {
            error(JSMSG_UNEXPECTED_TOKEN, "closing parenthesis", TokenKindToDesc(next));
            return null();
        }

        if (!tokenStream.peekToken(&next))
            return null();
        if (next != TokenKind::Arrow) {
            // Advance the scanner for proper error location reporting.
            tokenStream.consumeKnownToken(next);
            error(JSMSG_UNEXPECTED_TOKEN, "'=>' after argument list", TokenKindToDesc(next));
            return null();
        }

        anyChars.ungetToken();  // put back right paren

        // Return an arbitrary expression node. See case TokenKind::Rp above.
        return handler.newNullLiteral(pos());
      }
    }
}

template <class ParseHandler, typename CharT>
typename ParseHandler::Node
GeneralParser<ParseHandler, CharT>::exprInParens(InHandling inHandling,
                                                 YieldHandling yieldHandling,
                                                 TripledotHandling tripledotHandling,
                                                 PossibleError* possibleError /* = nullptr */)
{
    MOZ_ASSERT(anyChars.isCurrentTokenType(TokenKind::Lp));
    return expr(inHandling, yieldHandling, tripledotHandling, possibleError, PredictInvoked);
}

void
ParserBase::addTelemetry(DeprecatedLanguageExtension e)
{
    if (context->helperThread())
        return;
    context->compartment()->addTelemetry(getFilename(), e);
}

template <class ParseHandler, typename CharT>
bool
GeneralParser<ParseHandler, CharT>::warnOnceAboutExprClosure()
{
#ifndef RELEASE_OR_BETA
    if (context->helperThread())
        return true;

    if (!context->compartment()->warnedAboutExprClosure) {
        if (!warning(JSMSG_DEPRECATED_EXPR_CLOSURE))
            return false;
        context->compartment()->warnedAboutExprClosure = true;
    }
#endif
    return true;
}

template class PerHandlerParser<FullParseHandler>;
template class PerHandlerParser<SyntaxParseHandler>;
template class GeneralParser<FullParseHandler, char16_t>;
template class GeneralParser<SyntaxParseHandler, char16_t>;
template class Parser<FullParseHandler, char16_t>;
template class Parser<SyntaxParseHandler, char16_t>;

} /* namespace frontend */
} /* namespace js */
