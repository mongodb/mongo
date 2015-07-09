/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BytecodeCompiler.h"

#include "jscntxt.h"
#include "jsscript.h"

#include "asmjs/AsmJSLink.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/FoldConstants.h"
#include "frontend/NameFunctions.h"
#include "frontend/Parser.h"
#include "vm/GlobalObject.h"
#include "vm/TraceLogging.h"

#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "frontend/Parser-inl.h"

using namespace js;
using namespace js::frontend;
using mozilla::Maybe;

static bool
CheckLength(ExclusiveContext* cx, SourceBufferHolder& srcBuf)
{
    // Note this limit is simply so we can store sourceStart and sourceEnd in
    // JSScript as 32-bits. It could be lifted fairly easily, since the compiler
    // is using size_t internally already.
    if (srcBuf.length() > UINT32_MAX) {
        if (cx->isJSContext())
            JS_ReportErrorNumber(cx->asJSContext(), js_GetErrorMessage, nullptr,
                                 JSMSG_SOURCE_TOO_LONG);
        return false;
    }
    return true;
}

static bool
SetDisplayURL(ExclusiveContext* cx, TokenStream& tokenStream, ScriptSource* ss)
{
    if (tokenStream.hasDisplayURL()) {
        if (!ss->setDisplayURL(cx, tokenStream.displayURL()))
            return false;
    }
    return true;
}

static bool
SetSourceMap(ExclusiveContext* cx, TokenStream& tokenStream, ScriptSource* ss)
{
    if (tokenStream.hasSourceMapURL()) {
        MOZ_ASSERT(!ss->hasSourceMapURL());
        if (!ss->setSourceMapURL(cx, tokenStream.sourceMapURL()))
            return false;
    }
    return true;
}

static bool
CheckArgumentsWithinEval(JSContext* cx, Parser<FullParseHandler>& parser, HandleFunction fun)
{
    if (fun->hasRest()) {
        // It's an error to use |arguments| in a function that has a rest
        // parameter.
        parser.report(ParseError, false, nullptr, JSMSG_ARGUMENTS_AND_REST);
        return false;
    }

    // Force construction of arguments objects for functions that use
    // |arguments| within an eval.
    RootedScript script(cx, fun->getOrCreateScript(cx));
    if (!script)
        return false;
    if (script->argumentsHasVarBinding()) {
        if (!JSScript::argumentsOptimizationFailed(cx, script))
            return false;
    }

    // It's an error to use |arguments| in a legacy generator expression.
    if (script->isGeneratorExp() && script->isLegacyGenerator()) {
        parser.report(ParseError, false, nullptr, JSMSG_BAD_GENEXP_BODY, js_arguments_str);
        return false;
    }

    return true;
}

static bool
MaybeCheckEvalFreeVariables(ExclusiveContext* cxArg, HandleScript evalCaller, HandleObject scopeChain,
                            Parser<FullParseHandler>& parser,
                            ParseContext<FullParseHandler>& pc)
{
    if (!evalCaller || !evalCaller->functionOrCallerFunction())
        return true;

    // Eval scripts are only compiled on the main thread.
    JSContext* cx = cxArg->asJSContext();

    // Watch for uses of 'arguments' within the evaluated script, both as
    // free variables and as variables redeclared with 'var'.
    RootedFunction fun(cx, evalCaller->functionOrCallerFunction());
    HandlePropertyName arguments = cx->names().arguments;
    for (AtomDefnRange r = pc.lexdeps->all(); !r.empty(); r.popFront()) {
        if (r.front().key() == arguments) {
            if (!CheckArgumentsWithinEval(cx, parser, fun))
                return false;
        }
    }
    for (AtomDefnListMap::Range r = pc.decls().all(); !r.empty(); r.popFront()) {
        if (r.front().key() == arguments) {
            if (!CheckArgumentsWithinEval(cx, parser, fun))
                return false;
        }
    }

    // If the eval'ed script contains any debugger statement, force construction
    // of arguments objects for the caller script and any other scripts it is
    // transitively nested inside. The debugger can access any variable on the
    // scope chain.
    if (pc.sc->hasDebuggerStatement()) {
        RootedObject scope(cx, scopeChain);
        while (scope->is<ScopeObject>() || scope->is<DebugScopeObject>()) {
            if (scope->is<CallObject>() && !scope->as<CallObject>().isForEval()) {
                RootedScript script(cx, scope->as<CallObject>().callee().getOrCreateScript(cx));
                if (!script)
                    return false;
                if (script->argumentsHasVarBinding()) {
                    if (!JSScript::argumentsOptimizationFailed(cx, script))
                        return false;
                }
            }
            scope = scope->enclosingScope();
        }
    }

    return true;
}

static inline bool
CanLazilyParse(ExclusiveContext* cx, const ReadOnlyCompileOptions& options)
{
    return options.canLazilyParse &&
        options.compileAndGo &&
        !cx->compartment()->options().discardSource() &&
        !options.sourceIsLazy;
}

static void
MarkFunctionsWithinEvalScript(JSScript* script)
{
    // Mark top level functions in an eval script as being within an eval and,
    // if applicable, inside a with statement.

    if (!script->hasObjects())
        return;

    ObjectArray* objects = script->objects();
    size_t start = script->innerObjectsStart();

    for (size_t i = start; i < objects->length; i++) {
        JSObject* obj = objects->vector[i];
        if (obj->is<JSFunction>()) {
            JSFunction* fun = &obj->as<JSFunction>();
            if (fun->hasScript())
                fun->nonLazyScript()->setDirectlyInsideEval();
            else if (fun->isInterpretedLazy())
                fun->lazyScript()->setDirectlyInsideEval();
        }
    }
}

ScriptSourceObject*
frontend::CreateScriptSourceObject(ExclusiveContext* cx, const ReadOnlyCompileOptions& options)
{
    ScriptSource* ss = cx->new_<ScriptSource>();
    if (!ss)
        return nullptr;
    ScriptSourceHolder ssHolder(ss);

    if (!ss->initFromOptions(cx, options))
        return nullptr;

    RootedScriptSource sso(cx, ScriptSourceObject::create(cx, ss));
    if (!sso)
        return nullptr;

    // Off-thread compilations do all their GC heap allocation, including the
    // SSO, in a temporary compartment. Hence, for the SSO to refer to the
    // gc-heap-allocated values in |options|, it would need cross-compartment
    // wrappers from the temporary compartment to the real compartment --- which
    // would then be inappropriate once we merged the temporary and real
    // compartments.
    //
    // Instead, we put off populating those SSO slots in off-thread compilations
    // until after we've merged compartments.
    if (cx->isJSContext()) {
        if (!ScriptSourceObject::initFromOptions(cx->asJSContext(), sso, options))
            return nullptr;
    }

    return sso;
}

JSScript*
frontend::CompileScript(ExclusiveContext* cx, LifoAlloc* alloc, HandleObject scopeChain,
                        HandleScript evalCaller,
                        Handle<StaticEvalObject*> evalStaticScope,
                        const ReadOnlyCompileOptions& options,
                        SourceBufferHolder& srcBuf,
                        JSString* source_ /* = nullptr */,
                        unsigned staticLevel /* = 0 */,
                        SourceCompressionTask* extraSct /* = nullptr */)
{
    MOZ_ASSERT(srcBuf.get());

    RootedString source(cx, source_);

    js::TraceLoggerThread* logger = nullptr;
    if (cx->isJSContext())
        logger = TraceLoggerForMainThread(cx->asJSContext()->runtime());
    else
        logger = TraceLoggerForCurrentThread();
    js::TraceLoggerEvent event(logger, TraceLogger_AnnotateScripts, options);
    js::AutoTraceLog scriptLogger(logger, event);
    js::AutoTraceLog typeLogger(logger, TraceLogger_ParserCompileScript);

    /*
     * The scripted callerFrame can only be given for compile-and-go scripts
     * and non-zero static level requires callerFrame.
     */
    MOZ_ASSERT_IF(evalCaller, options.compileAndGo);
    MOZ_ASSERT_IF(evalCaller, options.forEval);
    MOZ_ASSERT_IF(evalCaller && evalCaller->strict(), options.strictOption);
    MOZ_ASSERT_IF(staticLevel != 0, evalCaller);

    if (!CheckLength(cx, srcBuf))
        return nullptr;
    MOZ_ASSERT_IF(staticLevel != 0, !options.sourceIsLazy);

    RootedScriptSource sourceObject(cx, CreateScriptSourceObject(cx, options));
    if (!sourceObject)
        return nullptr;

    ScriptSource* ss = sourceObject->source();

    SourceCompressionTask mysct(cx);
    SourceCompressionTask* sct = extraSct ? extraSct : &mysct;

    if (!cx->compartment()->options().discardSource()) {
        if (options.sourceIsLazy)
            ss->setSourceRetrievable();
        else if (!ss->setSourceCopy(cx, srcBuf, false, sct))
            return nullptr;
    }

    bool canLazilyParse = CanLazilyParse(cx, options);

    Maybe<Parser<SyntaxParseHandler> > syntaxParser;
    if (canLazilyParse) {
        syntaxParser.emplace(cx, alloc, options, srcBuf.get(), srcBuf.length(),
                             /* foldConstants = */ false,
                             (Parser<SyntaxParseHandler>*) nullptr,
                             (LazyScript*) nullptr);

        if (!syntaxParser->checkOptions())
            return nullptr;
    }

    Parser<FullParseHandler> parser(cx, alloc, options, srcBuf.get(), srcBuf.length(),
                                    /* foldConstants = */ true,
                                    canLazilyParse ? syntaxParser.ptr() : nullptr, nullptr);
    parser.sct = sct;
    parser.ss = ss;

    if (!parser.checkOptions())
        return nullptr;

    Directives directives(options.strictOption);
    GlobalSharedContext globalsc(cx, scopeChain, directives, options.extraWarningsOption);

    bool savedCallerFun = evalCaller && evalCaller->functionOrCallerFunction();
    Rooted<JSScript*> script(cx, JSScript::Create(cx, evalStaticScope, savedCallerFun,
                                                  options, staticLevel, sourceObject, 0,
                                                  srcBuf.length()));
    if (!script)
        return nullptr;

    // We can specialize a bit for the given scope chain if that scope chain is the global object.
    JSObject* globalScope =
        scopeChain && scopeChain == &scopeChain->global() ? (JSObject*) scopeChain : nullptr;
    MOZ_ASSERT_IF(globalScope, globalScope->isNative());
    MOZ_ASSERT_IF(globalScope, JSCLASS_HAS_GLOBAL_FLAG_AND_SLOTS(globalScope->getClass()));

    BytecodeEmitter::EmitterMode emitterMode =
        options.selfHostingMode ? BytecodeEmitter::SelfHosting : BytecodeEmitter::Normal;
    BytecodeEmitter bce(/* parent = */ nullptr, &parser, &globalsc, script,
                        /* lazyScript = */ js::NullPtr(), options.forEval,
                        evalCaller, evalStaticScope, !!globalScope, options.lineno, emitterMode);
    if (!bce.init())
        return nullptr;

    // Syntax parsing may cause us to restart processing of top level
    // statements in the script. Use Maybe<> so that the parse context can be
    // reset when this occurs.
    Maybe<ParseContext<FullParseHandler> > pc;

    pc.emplace(&parser, (GenericParseContext*) nullptr, (ParseNode*) nullptr, &globalsc,
               (Directives*) nullptr, staticLevel, /* bodyid = */ 0,
               /* blockScopeDepth = */ 0);
    if (!pc->init(parser.tokenStream))
        return nullptr;

    if (savedCallerFun) {
        /*
         * An eval script in a caller frame needs to have its enclosing
         * function captured in case it refers to an upvar, and someone
         * wishes to decompile it while it's running.
         */
        JSFunction* fun = evalCaller->functionOrCallerFunction();
        MOZ_ASSERT_IF(fun->strict(), options.strictOption);
        Directives directives(/* strict = */ options.strictOption);
        ObjectBox* funbox = parser.newFunctionBox(/* fn = */ nullptr, fun, pc.ptr(),
                                                  directives, fun->generatorKind());
        if (!funbox)
            return nullptr;
        bce.objectList.add(funbox);
    }

    bool canHaveDirectives = true;
    for (;;) {
        TokenKind tt;
        if (!parser.tokenStream.peekToken(&tt, TokenStream::Operand))
            return nullptr;
        if (tt == TOK_EOF)
            break;

        TokenStream::Position pos(parser.keepAtoms);
        parser.tokenStream.tell(&pos);

        ParseNode* pn = parser.statement(canHaveDirectives);
        if (!pn) {
            if (parser.hadAbortedSyntaxParse()) {
                // Parsing inner functions lazily may lead the parser into an
                // unrecoverable state and may require starting over on the top
                // level statement. Restart the parse; syntax parsing has
                // already been disabled for the parser and the result will not
                // be ambiguous.
                parser.clearAbortedSyntaxParse();
                parser.tokenStream.seek(pos);

                // Destroying the parse context will destroy its free
                // variables, so check if any deoptimization is needed.
                if (!MaybeCheckEvalFreeVariables(cx, evalCaller, scopeChain, parser, *pc))
                    return nullptr;

                pc.reset();
                pc.emplace(&parser, (GenericParseContext*) nullptr, (ParseNode*) nullptr,
                           &globalsc, (Directives*) nullptr, staticLevel, /* bodyid = */ 0,
                           script->bindings.numBlockScoped());
                if (!pc->init(parser.tokenStream))
                    return nullptr;
                MOZ_ASSERT(parser.pc == pc.ptr());

                pn = parser.statement();
            }
            if (!pn) {
                MOZ_ASSERT(!parser.hadAbortedSyntaxParse());
                return nullptr;
            }
        }

        // Accumulate the maximum block scope depth, so that EmitTree can assert
        // when emitting JSOP_GETLOCAL that the local is indeed within the fixed
        // part of the stack frame.
        script->bindings.updateNumBlockScoped(pc->blockScopeDepth);

        if (canHaveDirectives) {
            if (!parser.maybeParseDirective(/* stmtList = */ nullptr, pn, &canHaveDirectives))
                return nullptr;
        }

        if (!FoldConstants(cx, &pn, &parser))
            return nullptr;

        if (!NameFunctions(cx, pn))
            return nullptr;

        if (!bce.updateLocalsToFrameSlots())
            return nullptr;

        if (!EmitTree(cx, &bce, pn))
            return nullptr;

        parser.handler.freeTree(pn);
    }

    if (!MaybeCheckEvalFreeVariables(cx, evalCaller, scopeChain, parser, *pc))
        return nullptr;

    if (!SetDisplayURL(cx, parser.tokenStream, ss))
        return nullptr;

    if (!SetSourceMap(cx, parser.tokenStream, ss))
        return nullptr;

    /*
     * Source map URLs passed as a compile option (usually via a HTTP source map
     * header) override any source map urls passed as comment pragmas.
     */
    if (options.sourceMapURL()) {
        // Warn about the replacement, but use the new one.
        if (ss->hasSourceMapURL()) {
            if(!parser.report(ParseWarning, false, nullptr, JSMSG_ALREADY_HAS_PRAGMA,
                              ss->filename(), "//# sourceMappingURL"))
                return nullptr;
        }

        if (!ss->setSourceMapURL(cx, options.sourceMapURL()))
            return nullptr;
    }

    /*
     * Nowadays the threaded interpreter needs a last return instruction, so we
     * do have to emit that here.
     */
    if (Emit1(cx, &bce, JSOP_RETRVAL) < 0)
        return nullptr;

    // Global/eval script bindings are always empty (all names are added to the
    // scope dynamically via JSOP_DEFFUN/VAR).  They may have block-scoped
    // locals, however, which are allocated to the fixed part of the stack
    // frame.
    InternalHandle<Bindings*> bindings(script, &script->bindings);
    if (!Bindings::initWithTemporaryStorage(cx, bindings, 0, 0, 0,
                                            pc->blockScopeDepth, 0, 0, nullptr))
    {
        return nullptr;
    }

    if (!JSScript::fullyInitFromEmitter(cx, script, &bce))
        return nullptr;

    // Note that this marking must happen before we tell Debugger
    // about the new script, in case Debugger delazifies the script's
    // inner functions.
    if (options.forEval)
        MarkFunctionsWithinEvalScript(script);

    bce.tellDebuggerAboutCompiledScript(cx);

    if (sct && !extraSct && !sct->complete())
        return nullptr;

    MOZ_ASSERT_IF(cx->isJSContext(), !cx->asJSContext()->isExceptionPending());
    return script;
}

bool
frontend::CompileLazyFunction(JSContext* cx, Handle<LazyScript*> lazy, const char16_t* chars, size_t length)
{
    MOZ_ASSERT(cx->compartment() == lazy->functionNonDelazifying()->compartment());

    CompileOptions options(cx, lazy->version());
    options.setMutedErrors(lazy->mutedErrors())
           .setFileAndLine(lazy->filename(), lazy->lineno())
           .setColumn(lazy->column())
           .setCompileAndGo(true)
           .setNoScriptRval(false)
           .setSelfHostingMode(false);

    js::TraceLoggerThread* logger = js::TraceLoggerForMainThread(cx->runtime());
    js::TraceLoggerEvent event(logger, TraceLogger_AnnotateScripts, options);
    js::AutoTraceLog scriptLogger(logger, event);
    js::AutoTraceLog typeLogger(logger, TraceLogger_ParserCompileLazy);

    Parser<FullParseHandler> parser(cx, &cx->tempLifoAlloc(), options, chars, length,
                                    /* foldConstants = */ true, nullptr, lazy);
    if (!parser.checkOptions())
        return false;

    uint32_t staticLevel = lazy->staticLevel(cx);

    Rooted<JSFunction*> fun(cx, lazy->functionNonDelazifying());
    MOZ_ASSERT(!lazy->isLegacyGenerator());
    ParseNode* pn = parser.standaloneLazyFunction(fun, staticLevel, lazy->strict(),
                                                  lazy->generatorKind());
    if (!pn)
        return false;

    if (!NameFunctions(cx, pn))
        return false;

    RootedObject enclosingScope(cx, lazy->enclosingScope());
    RootedScriptSource sourceObject(cx, lazy->sourceObject());
    MOZ_ASSERT(sourceObject);

    Rooted<JSScript*> script(cx, JSScript::Create(cx, enclosingScope, false,
                                                  options, staticLevel,
                                                  sourceObject, lazy->begin(), lazy->end()));
    if (!script)
        return false;

    script->bindings = pn->pn_funbox->bindings;

    if (lazy->directlyInsideEval())
        script->setDirectlyInsideEval();
    if (lazy->usesArgumentsApplyAndThis())
        script->setUsesArgumentsApplyAndThis();
    if (lazy->hasBeenCloned())
        script->setHasBeenCloned();

    BytecodeEmitter bce(/* parent = */ nullptr, &parser, pn->pn_funbox, script, lazy,
                        options.forEval, /* evalCaller = */ js::NullPtr(),
                        /* evalStaticScope = */ js::NullPtr(),
                        /* hasGlobalScope = */ true, options.lineno,
                        BytecodeEmitter::LazyFunction);
    if (!bce.init())
        return false;

    return EmitFunctionScript(cx, &bce, pn->pn_body);
}

// Compile a JS function body, which might appear as the value of an event
// handler attribute in an HTML <INPUT> tag, or in a Function() constructor.
static bool
CompileFunctionBody(JSContext* cx, MutableHandleFunction fun, const ReadOnlyCompileOptions& options,
                    const AutoNameVector& formals, SourceBufferHolder& srcBuf,
                    HandleObject enclosingScope, GeneratorKind generatorKind)
{
    js::TraceLoggerThread* logger = js::TraceLoggerForMainThread(cx->runtime());
    js::TraceLoggerEvent event(logger, TraceLogger_AnnotateScripts, options);
    js::AutoTraceLog scriptLogger(logger, event);
    js::AutoTraceLog typeLogger(logger, TraceLogger_ParserCompileFunction);

    // FIXME: make Function pass in two strings and parse them as arguments and
    // ProgramElements respectively.

    if (!CheckLength(cx, srcBuf))
        return false;

    RootedScriptSource sourceObject(cx, CreateScriptSourceObject(cx, options));
    if (!sourceObject)
        return false;
    ScriptSource* ss = sourceObject->source();

    SourceCompressionTask sct(cx);
    MOZ_ASSERT(!options.sourceIsLazy);
    if (!cx->compartment()->options().discardSource()) {
        if (!ss->setSourceCopy(cx, srcBuf, true, &sct))
            return false;
    }

    bool canLazilyParse = CanLazilyParse(cx, options);

    Maybe<Parser<SyntaxParseHandler> > syntaxParser;
    if (canLazilyParse) {
        syntaxParser.emplace(cx, &cx->tempLifoAlloc(),
                             options, srcBuf.get(), srcBuf.length(),
                             /* foldConstants = */ false,
                             (Parser<SyntaxParseHandler>*) nullptr,
                             (LazyScript*) nullptr);
        if (!syntaxParser->checkOptions())
            return false;
    }

    MOZ_ASSERT(!options.forEval);

    Parser<FullParseHandler> parser(cx, &cx->tempLifoAlloc(),
                                    options, srcBuf.get(), srcBuf.length(),
                                    /* foldConstants = */ true,
                                    canLazilyParse ? syntaxParser.ptr() : nullptr, nullptr);
    parser.sct = &sct;
    parser.ss = ss;

    if (!parser.checkOptions())
        return false;

    MOZ_ASSERT(fun);
    MOZ_ASSERT(fun->isTenured());

    fun->setArgCount(formals.length());

    // Speculatively parse using the default directives implied by the context.
    // If a directive is encountered (e.g., "use strict") that changes how the
    // function should have been parsed, we backup and reparse with the new set
    // of directives.
    Directives directives(options.strictOption);

    TokenStream::Position start(parser.keepAtoms);
    parser.tokenStream.tell(&start);

    ParseNode* fn;
    while (true) {
        Directives newDirectives = directives;
        fn = parser.standaloneFunctionBody(fun, formals, generatorKind, directives, &newDirectives);
        if (fn)
            break;

        if (parser.hadAbortedSyntaxParse()) {
            // Hit some unrecoverable ambiguity during an inner syntax parse.
            // Syntax parsing has now been disabled in the parser, so retry
            // the parse.
            parser.clearAbortedSyntaxParse();
        } else {
            if (parser.tokenStream.hadError() || directives == newDirectives)
                return false;

            // Assignment must be monotonic to prevent reparsing iloops
            MOZ_ASSERT_IF(directives.strict(), newDirectives.strict());
            MOZ_ASSERT_IF(directives.asmJS(), newDirectives.asmJS());
            directives = newDirectives;
        }

        parser.tokenStream.seek(start);
    }

    if (!NameFunctions(cx, fn))
        return false;

    if (!SetDisplayURL(cx, parser.tokenStream, ss))
        return false;

    if (!SetSourceMap(cx, parser.tokenStream, ss))
        return false;

    if (fn->pn_funbox->function()->isInterpreted()) {
        MOZ_ASSERT(fun == fn->pn_funbox->function());

        Rooted<JSScript*> script(cx, JSScript::Create(cx, enclosingScope, false, options,
                                                      /* staticLevel = */ 0, sourceObject,
                                                      /* sourceStart = */ 0, srcBuf.length()));
        if (!script)
            return false;

        script->bindings = fn->pn_funbox->bindings;

        /*
         * The reason for checking fun->environment() below is that certain
         * consumers of JS::CompileFunction, namely
         * EventListenerManager::CompileEventHandlerInternal, passes in a
         * nullptr environment. This compiled function is never used, but
         * instead is cloned immediately onto the right scope chain.
         */
        BytecodeEmitter funbce(/* parent = */ nullptr, &parser, fn->pn_funbox, script,
                               /* lazyScript = */ js::NullPtr(), /* insideEval = */ false,
                               /* evalCaller = */ js::NullPtr(),
                               /* evalStaticScope = */ js::NullPtr(),
                               fun->environment() && fun->environment()->is<GlobalObject>(),
                               options.lineno);
        if (!funbce.init())
            return false;

        if (!EmitFunctionScript(cx, &funbce, fn->pn_body))
            return false;
    } else {
        fun.set(fn->pn_funbox->function());
        MOZ_ASSERT(IsAsmJSModuleNative(fun->native()));
    }

    if (!sct.complete())
        return false;

    return true;
}

bool
frontend::CompileFunctionBody(JSContext* cx, MutableHandleFunction fun,
                              const ReadOnlyCompileOptions& options,
                              const AutoNameVector& formals, JS::SourceBufferHolder& srcBuf,
                              HandleObject enclosingStaticScope)
{
    return CompileFunctionBody(cx, fun, options, formals, srcBuf,
                               enclosingStaticScope, NotGenerator);
}

bool
frontend::CompileStarGeneratorBody(JSContext* cx, MutableHandleFunction fun,
                                   const ReadOnlyCompileOptions& options, const AutoNameVector& formals,
                                   JS::SourceBufferHolder& srcBuf)
{
    return CompileFunctionBody(cx, fun, options, formals, srcBuf, NullPtr(), StarGenerator);
}
