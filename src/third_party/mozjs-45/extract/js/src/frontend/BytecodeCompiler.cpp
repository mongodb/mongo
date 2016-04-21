/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BytecodeCompiler.h"

#include "jscntxt.h"
#include "jsscript.h"

#include "asmjs/AsmJSLink.h"
#include "builtin/ModuleObject.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/FoldConstants.h"
#include "frontend/NameFunctions.h"
#include "frontend/Parser.h"
#include "vm/GlobalObject.h"
#include "vm/TraceLogging.h"

#include "jsobjinlines.h"
#include "jsscriptinlines.h"

#include "frontend/Parser-inl.h"
#include "vm/ScopeObject-inl.h"

using namespace js;
using namespace js::frontend;
using mozilla::Maybe;

class MOZ_STACK_CLASS AutoCompilationTraceLogger
{
  public:
    AutoCompilationTraceLogger(ExclusiveContext* cx, const TraceLoggerTextId id,
            const ReadOnlyCompileOptions& options);

  private:
    TraceLoggerThread* logger;
    TraceLoggerEvent event;
    AutoTraceLog scriptLogger;
    AutoTraceLog typeLogger;
};

// The BytecodeCompiler class contains resources common to compiling scripts and
// function bodies.
class MOZ_STACK_CLASS BytecodeCompiler
{
  public:
    // Construct an object passing mandatory arguments.
    BytecodeCompiler(ExclusiveContext* cx,
                     LifoAlloc* alloc,
                     const ReadOnlyCompileOptions& options,
                     SourceBufferHolder& sourceBuffer,
                     Handle<ScopeObject*> enclosingStaticScope,
                     TraceLoggerTextId logId);

    // Call setters for optional arguments.
    void maybeSetSourceCompressor(SourceCompressionTask* sourceCompressor);
    void setSourceArgumentsNotIncluded();

    JSScript* compileScript(HandleObject scopeChain, HandleScript evalCaller);
    ModuleObject* compileModule();
    bool compileFunctionBody(MutableHandleFunction fun, Handle<PropertyNameVector> formals,
                             GeneratorKind generatorKind);

    ScriptSourceObject* sourceObjectPtr() const;

  private:
    bool checkLength();
    bool createScriptSource();
    bool maybeCompressSource();
    bool canLazilyParse();
    bool createParser();
    bool createSourceAndParser();
    bool createScript(HandleObject staticScope, bool savedCallerFun = false);
    bool createEmitter(SharedContext* sharedContext, HandleScript evalCaller = nullptr,
                       bool insideNonGlobalEval = false);
    bool isEvalCompilationUnit();
    bool isNonGlobalEvalCompilationUnit();
    bool isNonSyntacticCompilationUnit();
    bool saveCallerFun(HandleScript evalCaller);
    bool handleParseFailure(const Directives& newDirectives);
    bool prepareAndEmitTree(ParseNode** pn);
    bool checkArgumentsWithinEval(JSContext* cx, HandleFunction fun);
    bool maybeCheckEvalFreeVariables(HandleScript evalCaller, HandleObject scopeChain,
                                     ParseContext<FullParseHandler>& pc);
    bool maybeSetDisplayURL(TokenStream& tokenStream);
    bool maybeSetSourceMap(TokenStream& tokenStream);
    bool maybeSetSourceMapFromOptions();
    bool emitFinalReturn();
    bool initGlobalOrEvalBindings(ParseContext<FullParseHandler>& pc);
    bool maybeCompleteCompressSource();

    AutoCompilationTraceLogger traceLogger;
    AutoKeepAtoms keepAtoms;

    ExclusiveContext* cx;
    LifoAlloc* alloc;
    const ReadOnlyCompileOptions& options;
    SourceBufferHolder& sourceBuffer;

    Rooted<ScopeObject*> enclosingStaticScope;
    bool sourceArgumentsNotIncluded;

    RootedScriptSource sourceObject;
    ScriptSource* scriptSource;

    Maybe<SourceCompressionTask> maybeSourceCompressor;
    SourceCompressionTask* sourceCompressor;

    Maybe<Parser<SyntaxParseHandler>> syntaxParser;
    Maybe<Parser<FullParseHandler>> parser;

    Directives directives;
    TokenStream::Position startPosition;

    RootedScript script;
    Maybe<BytecodeEmitter> emitter;
};

AutoCompilationTraceLogger::AutoCompilationTraceLogger(ExclusiveContext* cx,
        const TraceLoggerTextId id, const ReadOnlyCompileOptions& options)
  : logger(cx->isJSContext() ? TraceLoggerForMainThread(cx->asJSContext()->runtime())
                             : TraceLoggerForCurrentThread()),
    event(logger, TraceLogger_AnnotateScripts, options),
    scriptLogger(logger, event),
    typeLogger(logger, id)
{}

BytecodeCompiler::BytecodeCompiler(ExclusiveContext* cx,
                                   LifoAlloc* alloc,
                                   const ReadOnlyCompileOptions& options,
                                   SourceBufferHolder& sourceBuffer,
                                   Handle<ScopeObject*> enclosingStaticScope,
                                   TraceLoggerTextId logId)
  : traceLogger(cx, logId, options),
    keepAtoms(cx->perThreadData),
    cx(cx),
    alloc(alloc),
    options(options),
    sourceBuffer(sourceBuffer),
    enclosingStaticScope(cx, enclosingStaticScope),
    sourceArgumentsNotIncluded(false),
    sourceObject(cx),
    scriptSource(nullptr),
    sourceCompressor(nullptr),
    directives(options.strictOption),
    startPosition(keepAtoms),
    script(cx)
{
}

void
BytecodeCompiler::maybeSetSourceCompressor(SourceCompressionTask* sourceCompressor)
{
    this->sourceCompressor = sourceCompressor;
}

void
BytecodeCompiler::setSourceArgumentsNotIncluded()
{
    sourceArgumentsNotIncluded = true;
}

bool
BytecodeCompiler::checkLength()
{
    // Note this limit is simply so we can store sourceStart and sourceEnd in
    // JSScript as 32-bits. It could be lifted fairly easily, since the compiler
    // is using size_t internally already.
    if (sourceBuffer.length() > UINT32_MAX) {
        if (cx->isJSContext())
            JS_ReportErrorNumber(cx->asJSContext(), GetErrorMessage, nullptr,
                                 JSMSG_SOURCE_TOO_LONG);
        return false;
    }
    return true;
}

bool
BytecodeCompiler::createScriptSource()
{
    if (!checkLength())
        return false;

    sourceObject = CreateScriptSourceObject(cx, options);
    if (!sourceObject)
        return false;

    scriptSource = sourceObject->source();
    return true;
}

bool
BytecodeCompiler::maybeCompressSource()
{
    if (!sourceCompressor) {
        maybeSourceCompressor.emplace(cx);
        sourceCompressor = maybeSourceCompressor.ptr();
    }

    if (!cx->compartment()->options().discardSource()) {
        if (options.sourceIsLazy) {
            scriptSource->setSourceRetrievable();
        } else if (!scriptSource->setSourceCopy(cx, sourceBuffer, sourceArgumentsNotIncluded,
                                                sourceCompressor))
        {
            return false;
        }
    }

    return true;
}

bool
BytecodeCompiler::canLazilyParse()
{
    return options.canLazilyParse &&
           !HasNonSyntacticStaticScopeChain(enclosingStaticScope) &&
           !cx->compartment()->options().disableLazyParsing() &&
           !cx->compartment()->options().discardSource() &&
           !options.sourceIsLazy &&
           !cx->lcovEnabled();
}

bool
BytecodeCompiler::createParser()
{
    if (canLazilyParse()) {
        syntaxParser.emplace(cx, alloc, options, sourceBuffer.get(), sourceBuffer.length(),
                             /* foldConstants = */ false, (Parser<SyntaxParseHandler>*) nullptr,
                             (LazyScript*) nullptr);

        if (!syntaxParser->checkOptions())
            return false;
    }

    parser.emplace(cx, alloc, options, sourceBuffer.get(), sourceBuffer.length(),
                   /* foldConstants = */ true, syntaxParser.ptrOr(nullptr), nullptr);
    parser->sct = sourceCompressor;
    parser->ss = scriptSource;
    if (!parser->checkOptions())
        return false;

    parser->tokenStream.tell(&startPosition);
    return true;
}

bool
BytecodeCompiler::createSourceAndParser()
{
    return createScriptSource() &&
           maybeCompressSource() &&
           createParser();
}

bool
BytecodeCompiler::createScript(HandleObject staticScope, bool savedCallerFun)
{
    script = JSScript::Create(cx, staticScope, savedCallerFun, options,
                              sourceObject, /* sourceStart = */ 0, sourceBuffer.length());

    return script != nullptr;
}

bool
BytecodeCompiler::createEmitter(SharedContext* sharedContext, HandleScript evalCaller,
                                bool insideNonGlobalEval)
{
    BytecodeEmitter::EmitterMode emitterMode =
        options.selfHostingMode ? BytecodeEmitter::SelfHosting : BytecodeEmitter::Normal;
    emitter.emplace(/* parent = */ nullptr, parser.ptr(), sharedContext, script,
                    /* lazyScript = */ nullptr, options.forEval, evalCaller,
                    insideNonGlobalEval, options.lineno, emitterMode);
    return emitter->init();
}

bool
BytecodeCompiler::isEvalCompilationUnit()
{
    return enclosingStaticScope->is<StaticEvalObject>();
}

bool
BytecodeCompiler::isNonGlobalEvalCompilationUnit()
{
    if (!isEvalCompilationUnit())
        return false;
    StaticEvalObject& eval = enclosingStaticScope->as<StaticEvalObject>();
    JSObject* enclosing = eval.enclosingScopeForStaticScopeIter();
    return !IsStaticGlobalLexicalScope(enclosing);
}

bool
BytecodeCompiler::isNonSyntacticCompilationUnit()
{
    return enclosingStaticScope->is<StaticNonSyntacticScopeObjects>();
}

bool
BytecodeCompiler::saveCallerFun(HandleScript evalCaller)
{
    /*
     * An eval script in a caller frame needs to have its enclosing
     * function captured in case it refers to an upvar, and someone
     * wishes to decompile it while it's running.
     *
     * This ends up as script->objects()->vector[0] in the compiled script.
     */
    RootedFunction fun(cx, evalCaller->functionOrCallerFunction());
    MOZ_ASSERT_IF(fun->strict(), options.strictOption);
    Directives directives(/* strict = */ options.strictOption);
    ObjectBox* funbox = parser->newFunctionBox(/* fn = */ nullptr, fun,
                                               directives, fun->generatorKind(),
                                               enclosingStaticScope);
    if (!funbox)
        return false;

    emitter->objectList.add(funbox);
    return true;
}

bool
BytecodeCompiler::handleParseFailure(const Directives& newDirectives)
{
    if (parser->hadAbortedSyntaxParse()) {
        // Hit some unrecoverable ambiguity during an inner syntax parse.
        // Syntax parsing has now been disabled in the parser, so retry
        // the parse.
        parser->clearAbortedSyntaxParse();
    } else if (parser->tokenStream.hadError() || directives == newDirectives) {
        return false;
    }

    parser->tokenStream.seek(startPosition);

    // Assignment must be monotonic to prevent reparsing iloops
    MOZ_ASSERT_IF(directives.strict(), newDirectives.strict());
    MOZ_ASSERT_IF(directives.asmJS(), newDirectives.asmJS());
    directives = newDirectives;
    return true;
}

bool
BytecodeCompiler::prepareAndEmitTree(ParseNode** ppn)
{
    if (!FoldConstants(cx, ppn, parser.ptr()) ||
        !NameFunctions(cx, *ppn) ||
        !emitter->updateLocalsToFrameSlots() ||
        !emitter->emitTree(*ppn))
    {
        return false;
    }

    return true;
}

bool
BytecodeCompiler::maybeSetDisplayURL(TokenStream& tokenStream)
{
    if (tokenStream.hasDisplayURL()) {
        if (!scriptSource->setDisplayURL(cx, tokenStream.displayURL()))
            return false;
    }
    return true;
}

bool
BytecodeCompiler::maybeSetSourceMap(TokenStream& tokenStream)
{
    if (tokenStream.hasSourceMapURL()) {
        MOZ_ASSERT(!scriptSource->hasSourceMapURL());
        if (!scriptSource->setSourceMapURL(cx, tokenStream.sourceMapURL()))
            return false;
    }
    return true;
}

bool
BytecodeCompiler::maybeSetSourceMapFromOptions()
{
    /*
     * Source map URLs passed as a compile option (usually via a HTTP source map
     * header) override any source map urls passed as comment pragmas.
     */
    if (options.sourceMapURL()) {
        // Warn about the replacement, but use the new one.
        if (scriptSource->hasSourceMapURL()) {
            if(!parser->report(ParseWarning, false, nullptr, JSMSG_ALREADY_HAS_PRAGMA,
                              scriptSource->filename(), "//# sourceMappingURL"))
                return false;
        }

        if (!scriptSource->setSourceMapURL(cx, options.sourceMapURL()))
            return false;
    }

    return true;
}

bool
BytecodeCompiler::checkArgumentsWithinEval(JSContext* cx, HandleFunction fun)
{
    RootedScript script(cx, fun->getOrCreateScript(cx));
    if (!script)
        return false;

    // It's an error to use |arguments| in a legacy generator expression.
    if (script->isGeneratorExp() && script->isLegacyGenerator()) {
        parser->report(ParseError, false, nullptr, JSMSG_BAD_GENEXP_BODY, js_arguments_str);
        return false;
    }

    return true;
}

bool
BytecodeCompiler::maybeCheckEvalFreeVariables(HandleScript evalCaller, HandleObject scopeChain,
                                              ParseContext<FullParseHandler>& pc)
{
    if (!evalCaller || !evalCaller->functionOrCallerFunction())
        return true;

    // Eval scripts are only compiled on the main thread.
    JSContext* cx = this->cx->asJSContext();

    // Watch for uses of 'arguments' within the evaluated script, both as
    // free variables and as variables redeclared with 'var'.
    RootedFunction fun(cx, evalCaller->functionOrCallerFunction());
    HandlePropertyName arguments = cx->names().arguments;
    for (AtomDefnRange r = pc.lexdeps->all(); !r.empty(); r.popFront()) {
        if (r.front().key() == arguments) {
            if (!checkArgumentsWithinEval(cx, fun))
                return false;
        }
    }
    for (AtomDefnListMap::Range r = pc.decls().all(); !r.empty(); r.popFront()) {
        if (r.front().key() == arguments) {
            if (!checkArgumentsWithinEval(cx, fun))
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

bool
BytecodeCompiler::emitFinalReturn()
{
    /*
     * Nowadays the threaded interpreter needs a last return instruction, so we
     * do have to emit that here.
     */
    return emitter->emit1(JSOP_RETRVAL);
}

bool
BytecodeCompiler::initGlobalOrEvalBindings(ParseContext<FullParseHandler>& pc)
{
    Rooted<Bindings> bindings(cx, script->bindings);
    if (!pc.generateBindings(cx, parser->tokenStream, *alloc, &bindings))
        return false;
    script->bindings = bindings;
    return true;
}

bool
BytecodeCompiler::maybeCompleteCompressSource()
{
    return !maybeSourceCompressor || maybeSourceCompressor->complete();
}

JSScript*
BytecodeCompiler::compileScript(HandleObject scopeChain, HandleScript evalCaller)
{
    if (!createSourceAndParser())
        return nullptr;

    RootedFunction savedCallerFun(cx);
    if (evalCaller)
        savedCallerFun = evalCaller->functionOrCallerFunction();

    if (!createScript(enclosingStaticScope, savedCallerFun))
        return nullptr;

    GlobalSharedContext globalsc(cx, enclosingStaticScope, directives, options.extraWarningsOption,
                                 savedCallerFun);
    if (!createEmitter(&globalsc, evalCaller, isNonGlobalEvalCompilationUnit()))
        return nullptr;

    if (savedCallerFun && !saveCallerFun(evalCaller))
        return nullptr;

    for (;;) {
        ParseContext<FullParseHandler> pc(parser.ptr(),
                                          /* parent = */ nullptr,
                                          /* maybeFunction = */ nullptr,
                                          &globalsc,
                                          /* newDirectives = */ nullptr);
        if (!pc.init(*parser))
            return nullptr;

        ParseNode* pn;
        if (isEvalCompilationUnit())
            pn = parser->evalBody();
        else
            pn = parser->globalBody();

        // Successfully parsed. Emit the script.
        if (pn) {
            if (!initGlobalOrEvalBindings(pc))
                return nullptr;
            if (!maybeCheckEvalFreeVariables(evalCaller, scopeChain, pc))
                return nullptr;
            if (!prepareAndEmitTree(&pn))
                return nullptr;
            parser->handler.freeTree(pn);

            break;
        }

        // Maybe we aborted a syntax parse. See if we can try again.
        if (!handleParseFailure(directives))
            return nullptr;
    }

    if (!maybeSetDisplayURL(parser->tokenStream) ||
        !maybeSetSourceMap(parser->tokenStream) ||
        !maybeSetSourceMapFromOptions() ||
        !emitFinalReturn() ||
        !JSScript::fullyInitFromEmitter(cx, script, emitter.ptr()))
    {
        return nullptr;
    }

    emitter->tellDebuggerAboutCompiledScript(cx);

    if (!maybeCompleteCompressSource())
        return nullptr;

    MOZ_ASSERT_IF(cx->isJSContext(), !cx->asJSContext()->isExceptionPending());
    return script;
}

ModuleObject* BytecodeCompiler::compileModule()
{
    if (!createSourceAndParser())
        return nullptr;

    Rooted<ModuleObject*> module(cx, ModuleObject::create(cx, enclosingStaticScope));
    if (!module)
        return nullptr;

    if (!createScript(module))
        return nullptr;

    module->init(script);

    ParseNode* pn = parser->standaloneModule(module);
    if (!pn)
        return nullptr;

    if (!NameFunctions(cx, pn) ||
        !maybeSetDisplayURL(parser->tokenStream) ||
        !maybeSetSourceMap(parser->tokenStream))
    {
        return nullptr;
    }

    script->bindings = pn->pn_modulebox->bindings;

    RootedModuleEnvironmentObject dynamicScope(cx, ModuleEnvironmentObject::create(cx, module));
    if (!dynamicScope)
        return nullptr;

    module->setInitialEnvironment(dynamicScope);

    if (!createEmitter(pn->pn_modulebox) ||
        !emitter->emitModuleScript(pn->pn_body))
    {
        return nullptr;
    }

    ModuleBuilder builder(cx->asJSContext(), module);
    if (!builder.buildAndInit(pn))
        return nullptr;

    parser->handler.freeTree(pn);

    if (!maybeCompleteCompressSource())
        return nullptr;

    MOZ_ASSERT_IF(cx->isJSContext(), !cx->asJSContext()->isExceptionPending());
    return module;
}

bool
BytecodeCompiler::compileFunctionBody(MutableHandleFunction fun,
                                      Handle<PropertyNameVector> formals,
                                      GeneratorKind generatorKind)
{
    MOZ_ASSERT(fun);
    MOZ_ASSERT(fun->isTenured());

    fun->setArgCount(formals.length());

    if (!createSourceAndParser())
        return false;

    // Speculatively parse using the default directives implied by the context.
    // If a directive is encountered (e.g., "use strict") that changes how the
    // function should have been parsed, we backup and reparse with the new set
    // of directives.

    ParseNode* fn;
    do {
        Directives newDirectives = directives;
        fn = parser->standaloneFunctionBody(fun, formals, generatorKind, directives,
                                            &newDirectives, enclosingStaticScope);
        if (!fn && !handleParseFailure(newDirectives))
            return false;
    } while (!fn);

    if (!NameFunctions(cx, fn) ||
        !maybeSetDisplayURL(parser->tokenStream) ||
        !maybeSetSourceMap(parser->tokenStream))
    {
        return false;
    }

    if (fn->pn_funbox->function()->isInterpreted()) {
        MOZ_ASSERT(fun == fn->pn_funbox->function());

        if (!createScript(enclosingStaticScope))
            return false;

        script->bindings = fn->pn_funbox->bindings;

        if (!createEmitter(fn->pn_funbox) ||
            !emitter->emitFunctionScript(fn->pn_body))
        {
            return false;
        }
    } else {
        fun.set(fn->pn_funbox->function());
        MOZ_ASSERT(IsAsmJSModuleNative(fun->native()));
    }

    if (!maybeCompleteCompressSource())
        return false;

    return true;
}

ScriptSourceObject*
BytecodeCompiler::sourceObjectPtr() const
{
    return sourceObject.get();
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
                        Handle<ScopeObject*> enclosingStaticScope,
                        HandleScript evalCaller,
                        const ReadOnlyCompileOptions& options,
                        SourceBufferHolder& srcBuf,
                        JSString* source_ /* = nullptr */,
                        SourceCompressionTask* extraSct /* = nullptr */,
                        ScriptSourceObject** sourceObjectOut /* = nullptr */)
{
    MOZ_ASSERT(srcBuf.get());

    /*
     * The scripted callerFrame can only be given for compile-and-go scripts
     * and non-zero static level requires callerFrame.
     */
    MOZ_ASSERT_IF(evalCaller, options.isRunOnce);
    MOZ_ASSERT_IF(evalCaller, options.forEval);
    MOZ_ASSERT_IF(evalCaller && evalCaller->strict(), options.strictOption);

    MOZ_ASSERT_IF(sourceObjectOut, *sourceObjectOut == nullptr);

    BytecodeCompiler compiler(cx, alloc, options, srcBuf, enclosingStaticScope,
                              TraceLogger_ParserCompileScript);
    compiler.maybeSetSourceCompressor(extraSct);
    JSScript* script = compiler.compileScript(scopeChain, evalCaller);

    // frontend::CompileScript independently returns the
    // ScriptSourceObject (SSO) for the compile.  This is used by
    // off-main-thread script compilation (OMT-SC).
    //
    // OMT-SC cannot initialize the SSO when it is first constructed
    // because the SSO is allocated initially in a separate compartment.
    //
    // After OMT-SC, the separate compartment is merged with the main
    // compartment, at which point the JSScripts created become observable
    // by the debugger via memory-space scanning.
    //
    // Whatever happens to the top-level script compilation (even if it
    // fails and returns null), we must finish initializing the SSO.  This
    // is because there may be valid inner scripts observable by the debugger
    // which reference the partially-initialized SSO.
    if (sourceObjectOut)
        *sourceObjectOut = compiler.sourceObjectPtr();

    return script;
}

ModuleObject*
frontend::CompileModule(JSContext* cx, HandleObject obj,
                        const ReadOnlyCompileOptions& optionsInput,
                        SourceBufferHolder& srcBuf)
{
    MOZ_ASSERT(srcBuf.get());

    CompileOptions options(cx, optionsInput);
    options.maybeMakeStrictMode(true); // ES6 10.2.1 Module code is always strict mode code.
    options.setIsRunOnce(true);

    Rooted<ScopeObject*> staticScope(cx, &cx->global()->lexicalScope().staticBlock());
    BytecodeCompiler compiler(cx, &cx->tempLifoAlloc(), options, srcBuf, staticScope,
                              TraceLogger_ParserCompileModule);
    return compiler.compileModule();
}

bool
frontend::CompileLazyFunction(JSContext* cx, Handle<LazyScript*> lazy, const char16_t* chars, size_t length)
{
    MOZ_ASSERT(cx->compartment() == lazy->functionNonDelazifying()->compartment());

    CompileOptions options(cx, lazy->version());
    options.setMutedErrors(lazy->mutedErrors())
           .setFileAndLine(lazy->filename(), lazy->lineno())
           .setColumn(lazy->column())
           .setNoScriptRval(false)
           .setSelfHostingMode(false);

    AutoCompilationTraceLogger traceLogger(cx, TraceLogger_ParserCompileLazy, options);

    Parser<FullParseHandler> parser(cx, &cx->tempLifoAlloc(), options, chars, length,
                                    /* foldConstants = */ true, nullptr, lazy);
    if (!parser.checkOptions())
        return false;

    Rooted<JSFunction*> fun(cx, lazy->functionNonDelazifying());
    MOZ_ASSERT(!lazy->isLegacyGenerator());
    ParseNode* pn = parser.standaloneLazyFunction(fun, lazy->strict(), lazy->generatorKind());
    if (!pn)
        return false;

    if (!NameFunctions(cx, pn))
        return false;

    RootedObject enclosingScope(cx, lazy->enclosingScope());
    RootedScriptSource sourceObject(cx, lazy->sourceObject());
    MOZ_ASSERT(sourceObject);

    Rooted<JSScript*> script(cx, JSScript::Create(cx, enclosingScope, false, options,
                                                  sourceObject, lazy->begin(), lazy->end()));
    if (!script)
        return false;

    script->bindings = pn->pn_funbox->bindings;

    if (lazy->usesArgumentsApplyAndThis())
        script->setUsesArgumentsApplyAndThis();
    if (lazy->hasBeenCloned())
        script->setHasBeenCloned();

    /*
     * We just pass false for insideNonGlobalEval and insideEval, because we
     * don't actually know whether we are or not.  The only consumer of those
     * booleans is TryConvertFreeName, and it has special machinery to avoid
     * doing bad things when a lazy function is inside eval.
     */
    MOZ_ASSERT(!options.forEval);
    BytecodeEmitter bce(/* parent = */ nullptr, &parser, pn->pn_funbox, script, lazy,
                        /* insideEval = */ false, /* evalCaller = */ nullptr,
                        /* insideNonGlobalEval = */ false, options.lineno,
                        BytecodeEmitter::LazyFunction);
    if (!bce.init())
        return false;

    return bce.emitFunctionScript(pn->pn_body);
}

// Compile a JS function body, which might appear as the value of an event
// handler attribute in an HTML <INPUT> tag, or in a Function() constructor.
static bool
CompileFunctionBody(JSContext* cx, MutableHandleFunction fun, const ReadOnlyCompileOptions& options,
                    Handle<PropertyNameVector> formals, SourceBufferHolder& srcBuf,
                    Handle<ScopeObject*> enclosingStaticScope, GeneratorKind generatorKind)
{
    MOZ_ASSERT(!options.isRunOnce);

    // FIXME: make Function pass in two strings and parse them as arguments and
    // ProgramElements respectively.

    BytecodeCompiler compiler(cx, &cx->tempLifoAlloc(), options, srcBuf, enclosingStaticScope,
                              TraceLogger_ParserCompileFunction);
    compiler.setSourceArgumentsNotIncluded();
    return compiler.compileFunctionBody(fun, formals, generatorKind);
}

bool
frontend::CompileFunctionBody(JSContext* cx, MutableHandleFunction fun,
                              const ReadOnlyCompileOptions& options,
                              Handle<PropertyNameVector> formals, JS::SourceBufferHolder& srcBuf,
                              Handle<ScopeObject*> enclosingStaticScope)
{
    return CompileFunctionBody(cx, fun, options, formals, srcBuf,
                               enclosingStaticScope, NotGenerator);
}

bool
frontend::CompileFunctionBody(JSContext* cx, MutableHandleFunction fun,
                              const ReadOnlyCompileOptions& options,
                              Handle<PropertyNameVector> formals, JS::SourceBufferHolder& srcBuf)
{
    Rooted<ScopeObject*> staticLexical(cx, &cx->global()->lexicalScope().staticBlock());
    return CompileFunctionBody(cx, fun, options, formals, srcBuf, staticLexical, NotGenerator);
}


bool
frontend::CompileStarGeneratorBody(JSContext* cx, MutableHandleFunction fun,
                                   const ReadOnlyCompileOptions& options,
                                   Handle<PropertyNameVector> formals,
                                   JS::SourceBufferHolder& srcBuf)
{
    Rooted<ScopeObject*> staticLexical(cx, &cx->global()->lexicalScope().staticBlock());
    return CompileFunctionBody(cx, fun, options, formals, srcBuf, staticLexical, StarGenerator);
}
