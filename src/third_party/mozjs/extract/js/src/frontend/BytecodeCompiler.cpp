/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BytecodeCompiler.h"

#include "mozilla/Attributes.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/Maybe.h"
#include "mozilla/Utf8.h"     // mozilla::Utf8Unit
#include "mozilla/Variant.h"  // mozilla::Variant

#include "builtin/ModuleObject.h"
#include "frontend/BytecodeCompilation.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/EitherParser.h"
#include "frontend/ErrorReporter.h"
#include "frontend/FoldConstants.h"
#ifdef JS_ENABLE_SMOOSH
#  include "frontend/Frontend2.h"  // Smoosh
#endif
#include "frontend/ModuleSharedContext.h"
#include "frontend/Parser.h"
#include "js/SourceText.h"
#include "vm/FunctionFlags.h"          // FunctionFlags
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind
#include "vm/GlobalObject.h"
#include "vm/HelperThreadState.h"  // ParseTask
#include "vm/JSContext.h"
#include "vm/JSScript.h"       // ScriptSource, UncompressedSourceCache
#include "vm/ModuleBuilder.h"  // js::ModuleBuilder
#include "vm/Time.h"           // AutoIncrementalTimer
#include "vm/TraceLogging.h"
#include "wasm/AsmJS.h"

#include "debugger/DebugAPI-inl.h"  // DebugAPI
#include "vm/EnvironmentObject-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Utf8Unit;

using JS::CompileOptions;
using JS::ReadOnlyCompileOptions;
using JS::SourceText;

// RAII class to check the frontend reports an exception when it fails to
// compile a script.
class MOZ_RAII AutoAssertReportedException {
#ifdef DEBUG
  JSContext* cx_;
  bool check_;

 public:
  explicit AutoAssertReportedException(JSContext* cx) : cx_(cx), check_(true) {}
  void reset() { check_ = false; }
  ~AutoAssertReportedException() {
    if (!check_) {
      return;
    }

    if (!cx_->isHelperThreadContext()) {
      MOZ_ASSERT(cx_->isExceptionPending());
      return;
    }

    ParseTask* task = cx_->parseTask();
    MOZ_ASSERT(task->outOfMemory || task->overRecursed ||
               !task->errors.empty());
  }
#else
 public:
  explicit AutoAssertReportedException(JSContext*) {}
  void reset() {}
#endif
};

static bool EmplaceEmitter(CompilationState& compilationState,
                           Maybe<BytecodeEmitter>& emitter,
                           const EitherParser& parser, SharedContext* sc);

template <typename Unit>
class MOZ_STACK_CLASS frontend::SourceAwareCompiler {
 protected:
  SourceText<Unit>& sourceBuffer_;

  CompilationState compilationState_;

  Maybe<Parser<SyntaxParseHandler, Unit>> syntaxParser;
  Maybe<Parser<FullParseHandler, Unit>> parser;

  using TokenStreamPosition = frontend::TokenStreamPosition<Unit>;

 protected:
  explicit SourceAwareCompiler(JSContext* cx, LifoAllocScope& allocScope,
                               CompilationInput& input,
                               SourceText<Unit>& sourceBuffer)
      : sourceBuffer_(sourceBuffer), compilationState_(cx, allocScope, input) {
    MOZ_ASSERT(sourceBuffer_.get() != nullptr);
  }

  [[nodiscard]] bool init(JSContext* cx,
                          InheritThis inheritThis = InheritThis::No,
                          JSObject* enclosingEnv = nullptr) {
    if (!compilationState_.init(cx, inheritThis, enclosingEnv)) {
      return false;
    }

    return createSourceAndParser(cx);
  }

  // Call this before calling compile{Global,Eval}Script.
  [[nodiscard]] bool createSourceAndParser(JSContext* cx);

  void assertSourceAndParserCreated() const {
    MOZ_ASSERT(compilationState_.source != nullptr);
    MOZ_ASSERT(parser.isSome());
  }

  void assertSourceParserAndScriptCreated() { assertSourceAndParserCreated(); }

  [[nodiscard]] bool emplaceEmitter(Maybe<BytecodeEmitter>& emitter,
                                    SharedContext* sharedContext) {
    return EmplaceEmitter(compilationState_, emitter,
                          EitherParser(parser.ptr()), sharedContext);
  }

  bool canHandleParseFailure(const Directives& newDirectives);

  void handleParseFailure(
      const Directives& newDirectives, TokenStreamPosition& startPosition,
      CompilationState::CompilationStatePosition& startStatePosition);

 public:
  CompilationState& compilationState() { return compilationState_; };

  ExtensibleCompilationStencil& stencil() { return compilationState_; }
};

template <typename Unit>
class MOZ_STACK_CLASS frontend::ScriptCompiler
    : public SourceAwareCompiler<Unit> {
  using Base = SourceAwareCompiler<Unit>;

 protected:
  using Base::compilationState_;
  using Base::parser;
  using Base::sourceBuffer_;

  using Base::assertSourceParserAndScriptCreated;
  using Base::canHandleParseFailure;
  using Base::emplaceEmitter;
  using Base::handleParseFailure;

  using typename Base::TokenStreamPosition;

 public:
  explicit ScriptCompiler(JSContext* cx, LifoAllocScope& allocScope,
                          CompilationInput& input,
                          SourceText<Unit>& sourceBuffer)
      : Base(cx, allocScope, input, sourceBuffer) {}

  using Base::init;
  using Base::stencil;

  [[nodiscard]] bool compile(JSContext* cx, SharedContext* sc);
};

#ifdef JS_ENABLE_SMOOSH
[[nodiscard]] static bool TrySmoosh(
    JSContext* cx, CompilationInput& input,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    UniquePtr<ExtensibleCompilationStencil>& stencilOut) {
  MOZ_ASSERT(!stencilOut);

  if (!cx->options().trySmoosh()) {
    return true;
  }

  JSRuntime* rt = cx->runtime();
  if (!Smoosh::tryCompileGlobalScriptToExtensibleStencil(cx, input, srcBuf,
                                                         stencilOut)) {
    return false;
  }

  if (cx->options().trackNotImplemented()) {
    if (stencilOut) {
      rt->parserWatcherFile.put("1");
    } else {
      rt->parserWatcherFile.put("0");
    }
  }

  if (!stencilOut) {
    fprintf(stderr, "Falling back!\n");
    return true;
  }

  return stencilOut->source->assignSource(cx, input.options, srcBuf);
}

[[nodiscard]] static bool TrySmoosh(
    JSContext* cx, CompilationInput& input, JS::SourceText<char16_t>& srcBuf,
    UniquePtr<ExtensibleCompilationStencil>& stencilOut) {
  MOZ_ASSERT(!stencilOut);
  return true;
}
#endif  // JS_ENABLE_SMOOSH

using BytecodeCompilerOutput =
    mozilla::Variant<UniquePtr<ExtensibleCompilationStencil>,
                     UniquePtr<CompilationStencil>, CompilationGCOutput*>;

// Compile global script, and return it as one of:
//   * ExtensibleCompilationStencil (without instantiation)
//   * CompilationStencil (without instantiation, has no external dependency)
//   * CompilationGCOutput (with instantiation).
template <typename Unit>
[[nodiscard]] static bool CompileGlobalScriptToStencilAndMaybeInstantiate(
    JSContext* cx, CompilationInput& input, JS::SourceText<Unit>& srcBuf,
    ScopeKind scopeKind, BytecodeCompilerOutput& output) {
#ifdef JS_ENABLE_SMOOSH
  {
    UniquePtr<ExtensibleCompilationStencil> extensibleStencil;
    if (!TrySmoosh(cx, input, srcBuf, extensibleStencil)) {
      return false;
    }
    if (extensibleStencil) {
      if (output.is<UniquePtr<ExtensibleCompilationStencil>>()) {
        output.as<UniquePtr<ExtensibleCompilationStencil>>() =
            std::move(extensibleStencil);
      } else if (output.is<UniquePtr<CompilationStencil>>()) {
        auto stencil =
            cx->make_unique<frontend::CompilationStencil>(input.source);
        if (!stencil) {
          return false;
        }

        if (!stencil->steal(cx, std::move(*extensibleStencil))) {
          return false;
        }

        output.as<UniquePtr<CompilationStencil>>() = std::move(stencil);
      } else {
        BorrowingCompilationStencil borrowingStencil(*extensibleStencil);
        if (!InstantiateStencils(cx, input, borrowingStencil,
                                 *(output.as<CompilationGCOutput*>()))) {
          return false;
        }
      }
      return true;
    }
  }
#endif  // JS_ENABLE_SMOOSH

  if (input.options.selfHostingMode) {
    if (!input.initForSelfHostingGlobal(cx)) {
      return false;
    }
  } else {
    if (!input.initForGlobal(cx)) {
      return false;
    }
  }

  AutoAssertReportedException assertException(cx);

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  ScriptCompiler<Unit> compiler(cx, allocScope, input, srcBuf);
  if (!compiler.init(cx)) {
    return false;
  }

  SourceExtent extent = SourceExtent::makeGlobalExtent(
      srcBuf.length(), input.options.lineno, input.options.column);

  GlobalSharedContext globalsc(cx, scopeKind, input.options,
                               compiler.compilationState().directives, extent);

  if (!compiler.compile(cx, &globalsc)) {
    return false;
  }

  if (output.is<UniquePtr<ExtensibleCompilationStencil>>()) {
    auto stencil = cx->make_unique<ExtensibleCompilationStencil>(
        std::move(compiler.stencil()));
    if (!stencil) {
      return false;
    }
    output.as<UniquePtr<ExtensibleCompilationStencil>>() = std::move(stencil);
  } else if (output.is<UniquePtr<CompilationStencil>>()) {
    AutoGeckoProfilerEntry pseudoFrame(cx, "script emit",
                                       JS::ProfilingCategoryPair::JS_Parsing);

    auto stencil = cx->make_unique<CompilationStencil>(input.source);
    if (!stencil) {
      return false;
    }

    if (!stencil->steal(cx, std::move(compiler.stencil()))) {
      return false;
    }

    output.as<UniquePtr<CompilationStencil>>() = std::move(stencil);
  } else {
    BorrowingCompilationStencil borrowingStencil(compiler.stencil());
    if (!InstantiateStencils(cx, input, borrowingStencil,
                             *(output.as<CompilationGCOutput*>()))) {
      return false;
    }
  }

  assertException.reset();
  return true;
}

template <typename Unit>
static UniquePtr<CompilationStencil> CompileGlobalScriptToStencilImpl(
    JSContext* cx, CompilationInput& input, JS::SourceText<Unit>& srcBuf,
    ScopeKind scopeKind) {
  using OutputType = UniquePtr<CompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!CompileGlobalScriptToStencilAndMaybeInstantiate(cx, input, srcBuf,
                                                       scopeKind, output)) {
    return nullptr;
  }
  return std::move(output.as<OutputType>());
}

UniquePtr<CompilationStencil> frontend::CompileGlobalScriptToStencil(
    JSContext* cx, CompilationInput& input, JS::SourceText<char16_t>& srcBuf,
    ScopeKind scopeKind) {
  return CompileGlobalScriptToStencilImpl(cx, input, srcBuf, scopeKind);
}

UniquePtr<CompilationStencil> frontend::CompileGlobalScriptToStencil(
    JSContext* cx, CompilationInput& input, JS::SourceText<Utf8Unit>& srcBuf,
    ScopeKind scopeKind) {
  return CompileGlobalScriptToStencilImpl(cx, input, srcBuf, scopeKind);
}

template <typename Unit>
static UniquePtr<ExtensibleCompilationStencil>
CompileGlobalScriptToExtensibleStencilImpl(JSContext* cx,
                                           CompilationInput& input,
                                           JS::SourceText<Unit>& srcBuf,
                                           ScopeKind scopeKind) {
  using OutputType = UniquePtr<ExtensibleCompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!CompileGlobalScriptToStencilAndMaybeInstantiate(cx, input, srcBuf,
                                                       scopeKind, output)) {
    return nullptr;
  }
  return std::move(output.as<OutputType>());
}

UniquePtr<ExtensibleCompilationStencil>
frontend::CompileGlobalScriptToExtensibleStencil(
    JSContext* cx, CompilationInput& input, JS::SourceText<char16_t>& srcBuf,
    ScopeKind scopeKind) {
  return CompileGlobalScriptToExtensibleStencilImpl(cx, input, srcBuf,
                                                    scopeKind);
}

UniquePtr<ExtensibleCompilationStencil>
frontend::CompileGlobalScriptToExtensibleStencil(
    JSContext* cx, CompilationInput& input, JS::SourceText<Utf8Unit>& srcBuf,
    ScopeKind scopeKind) {
  return CompileGlobalScriptToExtensibleStencilImpl(cx, input, srcBuf,
                                                    scopeKind);
}

bool frontend::InstantiateStencils(JSContext* cx, CompilationInput& input,
                                   const CompilationStencil& stencil,
                                   CompilationGCOutput& gcOutput) {
  {
    AutoGeckoProfilerEntry pseudoFrame(cx, "stencil instantiate",
                                       JS::ProfilingCategoryPair::JS_Parsing);

    if (!CompilationStencil::instantiateStencils(cx, input, stencil,
                                                 gcOutput)) {
      return false;
    }
  }

  // Enqueue an off-thread source compression task after finishing parsing.
  if (!cx->isHelperThreadContext()) {
    if (!stencil.source->tryCompressOffThread(cx)) {
      return false;
    }

    Rooted<JSScript*> script(cx, gcOutput.script);
    if (!input.options.hideFromNewScriptInitial()) {
      DebugAPI::onNewScript(cx, script);
    }
  }

  return true;
}

bool frontend::PrepareForInstantiate(JSContext* cx, CompilationInput& input,
                                     const CompilationStencil& stencil,
                                     CompilationGCOutput& gcOutput) {
  AutoGeckoProfilerEntry pseudoFrame(cx, "stencil instantiate",
                                     JS::ProfilingCategoryPair::JS_Parsing);

  return CompilationStencil::prepareForInstantiate(cx, input, stencil,
                                                   gcOutput);
}

template <typename Unit>
static JSScript* CompileGlobalScriptImpl(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<Unit>& srcBuf, ScopeKind scopeKind) {
  Rooted<CompilationInput> input(cx, CompilationInput(options));
  Rooted<CompilationGCOutput> gcOutput(cx);
  BytecodeCompilerOutput output(gcOutput.address());
  if (!CompileGlobalScriptToStencilAndMaybeInstantiate(cx, input.get(), srcBuf,
                                                       scopeKind, output)) {
    return nullptr;
  }
  return gcOutput.get().script;
}

JSScript* frontend::CompileGlobalScript(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, ScopeKind scopeKind) {
  return CompileGlobalScriptImpl(cx, options, srcBuf, scopeKind);
}

JSScript* frontend::CompileGlobalScript(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<Utf8Unit>& srcBuf, ScopeKind scopeKind) {
  return CompileGlobalScriptImpl(cx, options, srcBuf, scopeKind);
}

template <typename Unit>
static JSScript* CompileEvalScriptImpl(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    SourceText<Unit>& srcBuf, JS::Handle<js::Scope*> enclosingScope,
    JS::Handle<JSObject*> enclosingEnv) {
  AutoAssertReportedException assertException(cx);

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  if (!input.get().initForEval(cx, enclosingScope)) {
    return nullptr;
  }

  LifoAllocScope allocScope(&cx->tempLifoAlloc());

  ScriptCompiler<Unit> compiler(cx, allocScope, input.get(), srcBuf);
  if (!compiler.init(cx, InheritThis::Yes, enclosingEnv)) {
    return nullptr;
  }

  uint32_t len = srcBuf.length();
  SourceExtent extent =
      SourceExtent::makeGlobalExtent(len, options.lineno, options.column);
  EvalSharedContext evalsc(cx, compiler.compilationState(), extent);
  if (!compiler.compile(cx, &evalsc)) {
    return nullptr;
  }

  Rooted<CompilationGCOutput> gcOutput(cx);
  {
    BorrowingCompilationStencil borrowingStencil(compiler.stencil());
    if (!InstantiateStencils(cx, input.get(), borrowingStencil,
                             gcOutput.get())) {
      return nullptr;
    }
  }

  assertException.reset();
  return gcOutput.get().script;
}

JSScript* frontend::CompileEvalScript(JSContext* cx,
                                      const JS::ReadOnlyCompileOptions& options,
                                      JS::SourceText<char16_t>& srcBuf,
                                      JS::Handle<js::Scope*> enclosingScope,
                                      JS::Handle<JSObject*> enclosingEnv) {
  return CompileEvalScriptImpl(cx, options, srcBuf, enclosingScope,
                               enclosingEnv);
}

template <typename Unit>
class MOZ_STACK_CLASS frontend::ModuleCompiler final
    : public SourceAwareCompiler<Unit> {
  using Base = SourceAwareCompiler<Unit>;

  using Base::assertSourceParserAndScriptCreated;
  using Base::compilationState_;
  using Base::emplaceEmitter;
  using Base::parser;

 public:
  explicit ModuleCompiler(JSContext* cx, LifoAllocScope& allocScope,
                          CompilationInput& input,
                          SourceText<Unit>& sourceBuffer)
      : Base(cx, allocScope, input, sourceBuffer) {}

  using Base::init;
  using Base::stencil;

  [[nodiscard]] bool compile(JSContext* cx);
};

template <typename Unit>
class MOZ_STACK_CLASS frontend::StandaloneFunctionCompiler final
    : public SourceAwareCompiler<Unit> {
  using Base = SourceAwareCompiler<Unit>;

  using Base::assertSourceAndParserCreated;
  using Base::canHandleParseFailure;
  using Base::compilationState_;
  using Base::emplaceEmitter;
  using Base::handleParseFailure;
  using Base::parser;
  using Base::sourceBuffer_;

  using typename Base::TokenStreamPosition;

 public:
  explicit StandaloneFunctionCompiler(JSContext* cx, LifoAllocScope& allocScope,
                                      CompilationInput& input,
                                      SourceText<Unit>& sourceBuffer)
      : Base(cx, allocScope, input, sourceBuffer) {}

  using Base::init;
  using Base::stencil;

 private:
  FunctionNode* parse(JSContext* cx, FunctionSyntaxKind syntaxKind,
                      GeneratorKind generatorKind, FunctionAsyncKind asyncKind,
                      const Maybe<uint32_t>& parameterListEnd);

 public:
  [[nodiscard]] bool compile(JSContext* cx, FunctionSyntaxKind syntaxKind,
                             GeneratorKind generatorKind,
                             FunctionAsyncKind asyncKind,
                             const Maybe<uint32_t>& parameterListEnd);
};

AutoFrontendTraceLog::AutoFrontendTraceLog(JSContext* cx,
                                           const TraceLoggerTextId id,
                                           const ErrorReporter& errorReporter)
#ifdef JS_TRACE_LOGGING
    : logger_(TraceLoggerForCurrentThread(cx)) {
  if (!logger_) {
    return;
  }

  // If the tokenizer hasn't yet gotten any tokens, use the line and column
  // numbers from CompileOptions.
  uint32_t line, column;
  if (errorReporter.hasTokenizationStarted()) {
    line = errorReporter.options().lineno;
    column = errorReporter.options().column;
  } else {
    errorReporter.currentLineAndColumn(&line, &column);
  }
  frontendEvent_.emplace(TraceLogger_Frontend, errorReporter.getFilename(),
                         line, column);
  frontendLog_.emplace(logger_, *frontendEvent_);
  typeLog_.emplace(logger_, id);
}
#else
{
}
#endif

AutoFrontendTraceLog::AutoFrontendTraceLog(JSContext* cx,
                                           const TraceLoggerTextId id,
                                           const ErrorReporter& errorReporter,
                                           FunctionBox* funbox)
#ifdef JS_TRACE_LOGGING
    : logger_(TraceLoggerForCurrentThread(cx)) {
  if (!logger_) {
    return;
  }

  frontendEvent_.emplace(TraceLogger_Frontend, errorReporter.getFilename(),
                         funbox->extent().lineno, funbox->extent().column);
  frontendLog_.emplace(logger_, *frontendEvent_);
  typeLog_.emplace(logger_, id);
}
#else
{
}
#endif

AutoFrontendTraceLog::AutoFrontendTraceLog(JSContext* cx,
                                           const TraceLoggerTextId id,
                                           const ErrorReporter& errorReporter,
                                           ParseNode* pn)
#ifdef JS_TRACE_LOGGING
    : logger_(TraceLoggerForCurrentThread(cx)) {
  if (!logger_) {
    return;
  }

  uint32_t line, column;
  errorReporter.lineAndColumnAt(pn->pn_pos.begin, &line, &column);
  frontendEvent_.emplace(TraceLogger_Frontend, errorReporter.getFilename(),
                         line, column);
  frontendLog_.emplace(logger_, *frontendEvent_);
  typeLog_.emplace(logger_, id);
}
#else
{
}
#endif

template <typename Unit>
bool frontend::SourceAwareCompiler<Unit>::createSourceAndParser(JSContext* cx) {
  const auto& options = compilationState_.input.options;

  if (!compilationState_.source->assignSource(cx, options, sourceBuffer_)) {
    return false;
  }

  MOZ_ASSERT(compilationState_.canLazilyParse ==
             CanLazilyParse(compilationState_.input.options));
  if (compilationState_.canLazilyParse) {
    syntaxParser.emplace(cx, options, sourceBuffer_.units(),
                         sourceBuffer_.length(),
                         /* foldConstants = */ false, compilationState_,
                         /* syntaxParser = */ nullptr);
    if (!syntaxParser->checkOptions()) {
      return false;
    }
  }

  parser.emplace(cx, options, sourceBuffer_.units(), sourceBuffer_.length(),
                 /* foldConstants = */ true, compilationState_,
                 syntaxParser.ptrOr(nullptr));
  parser->ss = compilationState_.source.get();
  return parser->checkOptions();
}

static bool EmplaceEmitter(CompilationState& compilationState,
                           Maybe<BytecodeEmitter>& emitter,
                           const EitherParser& parser, SharedContext* sc) {
  BytecodeEmitter::EmitterMode emitterMode =
      sc->selfHosted() ? BytecodeEmitter::SelfHosting : BytecodeEmitter::Normal;
  emitter.emplace(/* parent = */ nullptr, parser, sc, compilationState,
                  emitterMode);
  return emitter->init();
}

template <typename Unit>
bool frontend::SourceAwareCompiler<Unit>::canHandleParseFailure(
    const Directives& newDirectives) {
  // Try to reparse if no parse errors were thrown and the directives changed.
  //
  // NOTE:
  // Only the following two directive changes force us to reparse the script:
  // - The "use asm" directive was encountered.
  // - The "use strict" directive was encountered and duplicate parameter names
  //   are present. We reparse in this case to display the error at the correct
  //   source location. See |Parser::hasValidSimpleStrictParameterNames()|.
  return !parser->anyChars.hadError() &&
         compilationState_.directives != newDirectives;
}

template <typename Unit>
void frontend::SourceAwareCompiler<Unit>::handleParseFailure(
    const Directives& newDirectives, TokenStreamPosition& startPosition,
    CompilationState::CompilationStatePosition& startStatePosition) {
  MOZ_ASSERT(canHandleParseFailure(newDirectives));

  // Rewind to starting position to retry.
  parser->tokenStream.rewind(startPosition);
  compilationState_.rewind(startStatePosition);

  // Assignment must be monotonic to prevent reparsing iloops
  MOZ_ASSERT_IF(compilationState_.directives.strict(), newDirectives.strict());
  MOZ_ASSERT_IF(compilationState_.directives.asmJS(), newDirectives.asmJS());
  compilationState_.directives = newDirectives;
}

template <typename Unit>
bool frontend::ScriptCompiler<Unit>::compile(JSContext* cx, SharedContext* sc) {
  assertSourceParserAndScriptCreated();

  TokenStreamPosition startPosition(parser->tokenStream);

  // Emplace the topLevel stencil
  MOZ_ASSERT(compilationState_.scriptData.length() ==
             CompilationStencil::TopLevelIndex);
  if (!compilationState_.appendScriptStencilAndData(cx)) {
    return false;
  }

  ParseNode* pn;
  {
    AutoGeckoProfilerEntry pseudoFrame(cx, "script parsing",
                                       JS::ProfilingCategoryPair::JS_Parsing);
    if (sc->isEvalContext()) {
      pn = parser->evalBody(sc->asEvalContext());
    } else {
      pn = parser->globalBody(sc->asGlobalContext());
    }
  }

  if (!pn) {
    // Global and eval scripts don't get reparsed after a new directive was
    // encountered:
    // - "use strict" doesn't require any special error reporting for scripts.
    // - "use asm" directives don't have an effect in global/eval contexts.
    MOZ_ASSERT(!canHandleParseFailure(compilationState_.directives));
    return false;
  }

  {
    // Successfully parsed. Emit the script.
    AutoGeckoProfilerEntry pseudoFrame(cx, "script emit",
                                       JS::ProfilingCategoryPair::JS_Parsing);

    Maybe<BytecodeEmitter> emitter;
    if (!emplaceEmitter(emitter, sc)) {
      return false;
    }

    if (!emitter->emitScript(pn)) {
      return false;
    }
  }

  MOZ_ASSERT_IF(!cx->isHelperThreadContext(), !cx->isExceptionPending());

  return true;
}

template <typename Unit>
bool frontend::ModuleCompiler<Unit>::compile(JSContext* cx) {
  // Emplace the topLevel stencil
  MOZ_ASSERT(compilationState_.scriptData.length() ==
             CompilationStencil::TopLevelIndex);
  if (!compilationState_.appendScriptStencilAndData(cx)) {
    return false;
  }

  ModuleBuilder builder(cx, parser.ptr());

  const auto& options = compilationState_.input.options;

  uint32_t len = this->sourceBuffer_.length();
  SourceExtent extent =
      SourceExtent::makeGlobalExtent(len, options.lineno, options.column);
  ModuleSharedContext modulesc(cx, options, builder, extent);

  ParseNode* pn = parser->moduleBody(&modulesc);
  if (!pn) {
    return false;
  }

  Maybe<BytecodeEmitter> emitter;
  if (!emplaceEmitter(emitter, &modulesc)) {
    return false;
  }

  if (!emitter->emitScript(pn->as<ModuleNode>().body())) {
    return false;
  }

  StencilModuleMetadata& moduleMetadata = *compilationState_.moduleMetadata;

  builder.finishFunctionDecls(moduleMetadata);

  MOZ_ASSERT_IF(!cx->isHelperThreadContext(), !cx->isExceptionPending());
  return true;
}

// Parse a standalone JS function, which might appear as the value of an
// event handler attribute in an HTML <INPUT> tag, or in a Function()
// constructor.
template <typename Unit>
FunctionNode* frontend::StandaloneFunctionCompiler<Unit>::parse(
    JSContext* cx, FunctionSyntaxKind syntaxKind, GeneratorKind generatorKind,
    FunctionAsyncKind asyncKind, const Maybe<uint32_t>& parameterListEnd) {
  assertSourceAndParserCreated();

  TokenStreamPosition startPosition(parser->tokenStream);
  auto startStatePosition = compilationState_.getPosition();

  // Speculatively parse using the default directives implied by the context.
  // If a directive is encountered (e.g., "use strict") that changes how the
  // function should have been parsed, we backup and reparse with the new set
  // of directives.

  FunctionNode* fn;
  for (;;) {
    Directives newDirectives = compilationState_.directives;
    fn = parser->standaloneFunction(parameterListEnd, syntaxKind, generatorKind,
                                    asyncKind, compilationState_.directives,
                                    &newDirectives);
    if (fn) {
      break;
    }

    // Maybe we encountered a new directive. See if we can try again.
    if (!canHandleParseFailure(newDirectives)) {
      return nullptr;
    }

    handleParseFailure(newDirectives, startPosition, startStatePosition);
  }

  return fn;
}

// Compile a standalone JS function.
template <typename Unit>
bool frontend::StandaloneFunctionCompiler<Unit>::compile(
    JSContext* cx, FunctionSyntaxKind syntaxKind, GeneratorKind generatorKind,
    FunctionAsyncKind asyncKind, const Maybe<uint32_t>& parameterListEnd) {
  FunctionNode* parsedFunction =
      parse(cx, syntaxKind, generatorKind, asyncKind, parameterListEnd);
  if (!parsedFunction) {
    return false;
  }

  FunctionBox* funbox = parsedFunction->funbox();

  if (funbox->isInterpreted()) {
    Maybe<BytecodeEmitter> emitter;
    if (!emplaceEmitter(emitter, funbox)) {
      return false;
    }

    if (!emitter->emitFunctionScript(parsedFunction)) {
      return false;
    }

    // The parser extent has stripped off the leading `function...` but
    // we want the SourceExtent used in the final standalone script to
    // start from the beginning of the buffer, and use the provided
    // line and column.
    const auto& options = compilationState_.input.options;
    compilationState_.scriptExtra[CompilationStencil::TopLevelIndex].extent =
        SourceExtent{/* sourceStart = */ 0,
                     sourceBuffer_.length(),
                     funbox->extent().toStringStart,
                     funbox->extent().toStringEnd,
                     options.lineno,
                     options.column};
  } else {
    // The asm.js module was created by parser. Instantiation below will
    // allocate the JSFunction that wraps it.
    MOZ_ASSERT(funbox->isAsmJSModule());
    MOZ_ASSERT(compilationState_.asmJS->moduleMap.has(funbox->index()));
    MOZ_ASSERT(compilationState_.scriptData[CompilationStencil::TopLevelIndex]
                   .functionFlags.isAsmJSNative());
  }

  return true;
}

// Compile module, and return it as one of:
//   * ExtensibleCompilationStencil (without instantiation)
//   * CompilationStencil (without instantiation, has no external dependency)
//   * CompilationGCOutput (with instantiation).
template <typename Unit>
[[nodiscard]] static bool ParseModuleToStencilAndMaybeInstantiate(
    JSContext* cx, CompilationInput& input, SourceText<Unit>& srcBuf,
    BytecodeCompilerOutput& output) {
  MOZ_ASSERT(srcBuf.get());

  if (!input.initForModule(cx)) {
    return false;
  }

  AutoAssertReportedException assertException(cx);

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  ModuleCompiler<Unit> compiler(cx, allocScope, input, srcBuf);
  if (!compiler.init(cx)) {
    return false;
  }

  if (!compiler.compile(cx)) {
    return false;
  }

  if (output.is<UniquePtr<ExtensibleCompilationStencil>>()) {
    auto stencil = cx->make_unique<ExtensibleCompilationStencil>(
        std::move(compiler.stencil()));
    if (!stencil) {
      return false;
    }
    output.as<UniquePtr<ExtensibleCompilationStencil>>() = std::move(stencil);
  } else if (output.is<UniquePtr<CompilationStencil>>()) {
    AutoGeckoProfilerEntry pseudoFrame(cx, "script emit",
                                       JS::ProfilingCategoryPair::JS_Parsing);

    auto stencil = cx->make_unique<CompilationStencil>(input.source);
    if (!stencil) {
      return false;
    }

    if (!stencil->steal(cx, std::move(compiler.stencil()))) {
      return false;
    }

    output.as<UniquePtr<CompilationStencil>>() = std::move(stencil);
  } else {
    BorrowingCompilationStencil borrowingStencil(compiler.stencil());
    if (!InstantiateStencils(cx, input, borrowingStencil,
                             *(output.as<CompilationGCOutput*>()))) {
      return false;
    }
  }

  assertException.reset();
  return true;
}

template <typename Unit>
UniquePtr<CompilationStencil> ParseModuleToStencilImpl(
    JSContext* cx, CompilationInput& input, SourceText<Unit>& srcBuf) {
  using OutputType = UniquePtr<CompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!ParseModuleToStencilAndMaybeInstantiate(cx, input, srcBuf, output)) {
    return nullptr;
  }
  return std::move(output.as<OutputType>());
}

UniquePtr<CompilationStencil> frontend::ParseModuleToStencil(
    JSContext* cx, CompilationInput& input, SourceText<char16_t>& srcBuf) {
  return ParseModuleToStencilImpl(cx, input, srcBuf);
}

UniquePtr<CompilationStencil> frontend::ParseModuleToStencil(
    JSContext* cx, CompilationInput& input, SourceText<Utf8Unit>& srcBuf) {
  return ParseModuleToStencilImpl(cx, input, srcBuf);
}

template <typename Unit>
UniquePtr<ExtensibleCompilationStencil> ParseModuleToExtensibleStencilImpl(
    JSContext* cx, CompilationInput& input, SourceText<Unit>& srcBuf) {
  using OutputType = UniquePtr<ExtensibleCompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!ParseModuleToStencilAndMaybeInstantiate(cx, input, srcBuf, output)) {
    return nullptr;
  }
  return std::move(output.as<OutputType>());
}

UniquePtr<ExtensibleCompilationStencil>
frontend::ParseModuleToExtensibleStencil(JSContext* cx, CompilationInput& input,
                                         SourceText<char16_t>& srcBuf) {
  return ParseModuleToExtensibleStencilImpl(cx, input, srcBuf);
}

UniquePtr<ExtensibleCompilationStencil>
frontend::ParseModuleToExtensibleStencil(JSContext* cx, CompilationInput& input,
                                         SourceText<Utf8Unit>& srcBuf) {
  return ParseModuleToExtensibleStencilImpl(cx, input, srcBuf);
}

template <typename Unit>
static ModuleObject* CompileModuleImpl(
    JSContext* cx, const JS::ReadOnlyCompileOptions& optionsInput,
    SourceText<Unit>& srcBuf) {
  AutoAssertReportedException assertException(cx);

  if (!GlobalObject::ensureModulePrototypesCreated(cx, cx->global())) {
    return nullptr;
  }

  CompileOptions options(cx, optionsInput);
  options.setModule();

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  Rooted<CompilationGCOutput> gcOutput(cx);
  BytecodeCompilerOutput output(gcOutput.address());
  if (!ParseModuleToStencilAndMaybeInstantiate(cx, input.get(), srcBuf,
                                               output)) {
    return nullptr;
  }

  assertException.reset();
  return gcOutput.get().module;
}

ModuleObject* frontend::CompileModule(JSContext* cx,
                                      const JS::ReadOnlyCompileOptions& options,
                                      SourceText<char16_t>& srcBuf) {
  return CompileModuleImpl(cx, options, srcBuf);
}

ModuleObject* frontend::CompileModule(JSContext* cx,
                                      const JS::ReadOnlyCompileOptions& options,
                                      SourceText<Utf8Unit>& srcBuf) {
  return CompileModuleImpl(cx, options, srcBuf);
}

template <typename Unit>
static bool CompileLazyFunction(JSContext* cx, CompilationInput& input,
                                const Unit* units, size_t length) {
  MOZ_ASSERT(input.source);

  AutoAssertReportedException assertException(cx);

  Rooted<JSFunction*> fun(cx, input.function());

  InheritThis inheritThis = fun->isArrow() ? InheritThis::Yes : InheritThis::No;

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  CompilationState compilationState(cx, allocScope, input);
  compilationState.setFunctionKey(input.extent());
  MOZ_ASSERT(!compilationState.isInitialStencil());
  if (!compilationState.init(cx, inheritThis)) {
    return false;
  }

  Parser<FullParseHandler, Unit> parser(cx, input.options, units, length,
                                        /* foldConstants = */ true,
                                        compilationState,
                                        /* syntaxParser = */ nullptr);
  if (!parser.checkOptions()) {
    return false;
  }

  FunctionNode* pn = parser.standaloneLazyFunction(
      fun, input.extent().toStringStart, input.strict(), input.generatorKind(),
      input.asyncKind());
  if (!pn) {
    return false;
  }

  BytecodeEmitter bce(/* parent = */ nullptr, &parser, pn->funbox(),
                      compilationState, BytecodeEmitter::LazyFunction);
  if (!bce.init(pn->pn_pos)) {
    return false;
  }

  if (!bce.emitFunctionScript(pn)) {
    return false;
  }

  // NOTE: Only allow relazification if there was no lazy PrivateScriptData.
  // This excludes non-leaf functions and all script class constructors.
  bool hadLazyScriptData = input.hasPrivateScriptData();
  bool isRelazifiableAfterDelazify = input.isRelazifiable();
  if (isRelazifiableAfterDelazify && !hadLazyScriptData) {
    compilationState.scriptData[CompilationStencil::TopLevelIndex]
        .setAllowRelazify();
  }

  mozilla::DebugOnly<uint32_t> lazyFlags =
      static_cast<uint32_t>(input.immutableFlags());

  Rooted<CompilationGCOutput> gcOutput(cx);
  {
    BorrowingCompilationStencil borrowingStencil(compilationState);
    if (!CompilationStencil::instantiateStencils(cx, input, borrowingStencil,
                                                 gcOutput.get())) {
      return false;
    }

    MOZ_ASSERT(lazyFlags == gcOutput.get().script->immutableFlags());
    MOZ_ASSERT(gcOutput.get().script->outermostScope()->hasOnChain(
                   ScopeKind::NonSyntactic) ==
               gcOutput.get().script->immutableFlags().hasFlag(
                   JSScript::ImmutableFlags::HasNonSyntacticScope));

    if (input.source->hasEncoder()) {
      MOZ_ASSERT(!js::UseOffThreadParseGlobal());
      if (!input.source->addDelazificationToIncrementalEncoding(
              cx, borrowingStencil)) {
        return false;
      }
    }
  }

  assertException.reset();
  return true;
}

template <typename Unit>
static bool DelazifyCanonicalScriptedFunctionImpl(JSContext* cx,
                                                  HandleFunction fun,
                                                  Handle<BaseScript*> lazy,
                                                  ScriptSource* ss) {
  MOZ_ASSERT(!lazy->hasBytecode(), "Script is already compiled!");
  MOZ_ASSERT(lazy->function() == fun);

  MOZ_DIAGNOSTIC_ASSERT(!fun->isGhost());

  AutoIncrementalTimer timer(cx->realm()->timers.delazificationTime);

  size_t sourceStart = lazy->sourceStart();
  size_t sourceLength = lazy->sourceEnd() - lazy->sourceStart();

  MOZ_ASSERT(ss->hasSourceText());

  // Parse and compile the script from source.
  UncompressedSourceCache::AutoHoldEntry holder;

  MOZ_ASSERT(ss->hasSourceType<Unit>());

  ScriptSource::PinnedUnits<Unit> units(cx, ss, holder, sourceStart,
                                        sourceLength);
  if (!units.get()) {
    return false;
  }

  JS::CompileOptions options(cx);
  options.setMutedErrors(lazy->mutedErrors())
      .setFileAndLine(lazy->filename(), lazy->lineno())
      .setColumn(lazy->column())
      .setScriptSourceOffset(lazy->sourceStart())
      .setNoScriptRval(false)
      .setSelfHostingMode(false);

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  input.get().initFromLazy(cx, lazy, ss);

  return CompileLazyFunction(cx, input.get(), units.get(), sourceLength);
}

bool frontend::DelazifyCanonicalScriptedFunction(JSContext* cx,
                                                 HandleFunction fun) {
  AutoGeckoProfilerEntry pseudoFrame(cx, "script delazify",
                                     JS::ProfilingCategoryPair::JS_Parsing);

  Rooted<BaseScript*> lazy(cx, fun->baseScript());
  ScriptSource* ss = lazy->scriptSource();

  if (ss->hasSourceType<Utf8Unit>()) {
    // UTF-8 source text.
    return DelazifyCanonicalScriptedFunctionImpl<Utf8Unit>(cx, fun, lazy, ss);
  }

  MOZ_ASSERT(ss->hasSourceType<char16_t>());

  // UTF-16 source text.
  return DelazifyCanonicalScriptedFunctionImpl<char16_t>(cx, fun, lazy, ss);
}

static JSFunction* CompileStandaloneFunction(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, const Maybe<uint32_t>& parameterListEnd,
    FunctionSyntaxKind syntaxKind, GeneratorKind generatorKind,
    FunctionAsyncKind asyncKind, HandleScope enclosingScope = nullptr) {
  AutoAssertReportedException assertException(cx);

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  if (enclosingScope) {
    if (!input.get().initForStandaloneFunctionInNonSyntacticScope(
            cx, enclosingScope)) {
      return nullptr;
    }
  } else {
    if (!input.get().initForStandaloneFunction(cx)) {
      return nullptr;
    }
  }

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  InheritThis inheritThis = (syntaxKind == FunctionSyntaxKind::Arrow)
                                ? InheritThis::Yes
                                : InheritThis::No;
  StandaloneFunctionCompiler<char16_t> compiler(cx, allocScope, input.get(),
                                                srcBuf);
  if (!compiler.init(cx, inheritThis)) {
    return nullptr;
  }

  if (!compiler.compile(cx, syntaxKind, generatorKind, asyncKind,
                        parameterListEnd)) {
    return nullptr;
  }

  Rooted<CompilationGCOutput> gcOutput(cx);
  RefPtr<ScriptSource> source;
  {
    BorrowingCompilationStencil borrowingStencil(compiler.stencil());
    if (!CompilationStencil::instantiateStencils(
            cx, input.get(), borrowingStencil, gcOutput.get())) {
      return nullptr;
    }
    source = borrowingStencil.source;
  }

#ifdef DEBUG
  JSFunction* fun = gcOutput.get().functions[CompilationStencil::TopLevelIndex];
  MOZ_ASSERT(fun->hasBytecode() || IsAsmJSModule(fun));
#endif

  // Enqueue an off-thread source compression task after finishing parsing.
  if (!cx->isHelperThreadContext()) {
    if (!source->tryCompressOffThread(cx)) {
      return nullptr;
    }
  }

  // Note: If AsmJS successfully compiles, the into.script will still be
  // nullptr. In this case we have compiled to a native function instead of an
  // interpreted script.
  if (gcOutput.get().script) {
    if (parameterListEnd) {
      source->setParameterListEnd(*parameterListEnd);
    }

    MOZ_ASSERT(!cx->isHelperThreadContext());

    Rooted<JSScript*> script(cx, gcOutput.get().script);
    if (!options.hideFromNewScriptInitial()) {
      DebugAPI::onNewScript(cx, script);
    }
  }

  assertException.reset();
  return gcOutput.get().functions[CompilationStencil::TopLevelIndex];
}

JSFunction* frontend::CompileStandaloneFunction(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, const Maybe<uint32_t>& parameterListEnd,
    FunctionSyntaxKind syntaxKind) {
  return CompileStandaloneFunction(cx, options, srcBuf, parameterListEnd,
                                   syntaxKind, GeneratorKind::NotGenerator,
                                   FunctionAsyncKind::SyncFunction);
}

JSFunction* frontend::CompileStandaloneGenerator(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, const Maybe<uint32_t>& parameterListEnd,
    FunctionSyntaxKind syntaxKind) {
  return CompileStandaloneFunction(cx, options, srcBuf, parameterListEnd,
                                   syntaxKind, GeneratorKind::Generator,
                                   FunctionAsyncKind::SyncFunction);
}

JSFunction* frontend::CompileStandaloneAsyncFunction(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, const Maybe<uint32_t>& parameterListEnd,
    FunctionSyntaxKind syntaxKind) {
  return CompileStandaloneFunction(cx, options, srcBuf, parameterListEnd,
                                   syntaxKind, GeneratorKind::NotGenerator,
                                   FunctionAsyncKind::AsyncFunction);
}

JSFunction* frontend::CompileStandaloneAsyncGenerator(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, const Maybe<uint32_t>& parameterListEnd,
    FunctionSyntaxKind syntaxKind) {
  return CompileStandaloneFunction(cx, options, srcBuf, parameterListEnd,
                                   syntaxKind, GeneratorKind::Generator,
                                   FunctionAsyncKind::AsyncFunction);
}

JSFunction* frontend::CompileStandaloneFunctionInNonSyntacticScope(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, const Maybe<uint32_t>& parameterListEnd,
    FunctionSyntaxKind syntaxKind, HandleScope enclosingScope) {
  MOZ_ASSERT(enclosingScope);
  return CompileStandaloneFunction(cx, options, srcBuf, parameterListEnd,
                                   syntaxKind, GeneratorKind::NotGenerator,
                                   FunctionAsyncKind::SyncFunction,
                                   enclosingScope);
}
