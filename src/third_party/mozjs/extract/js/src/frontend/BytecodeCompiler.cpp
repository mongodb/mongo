/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BytecodeCompiler.h"

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/Utf8.h"     // mozilla::Utf8Unit
#include "mozilla/Variant.h"  // mozilla::Variant

#include "debugger/DebugAPI.h"
#include "ds/LifoAlloc.h"
#include "frontend/BytecodeCompilation.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/CompilationStencil.h"
#include "frontend/EitherParser.h"
#ifdef JS_ENABLE_SMOOSH
#  include "frontend/Frontend2.h"  // Smoosh
#endif
#include "frontend/FrontendContext.h"  // AutoReportFrontendContext
#include "frontend/ModuleSharedContext.h"
#include "js/experimental/JSStencil.h"
#include "js/Modules.h"  // JS::ImportAssertionVector
#include "js/SourceText.h"
#include "js/UniquePtr.h"
#include "vm/FunctionFlags.h"          // FunctionFlags
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind
#include "vm/HelperThreads.h"  // StartOffThreadDelazification, WaitForAllDelazifyTasks
#include "vm/JSContext.h"
#include "vm/JSScript.h"       // ScriptSource, UncompressedSourceCache
#include "vm/ModuleBuilder.h"  // js::ModuleBuilder
#include "vm/Time.h"           // AutoIncrementalTimer
#include "wasm/AsmJS.h"

#include "vm/GeckoProfiler-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;
using mozilla::Utf8Unit;

using JS::CompileOptions;
using JS::ReadOnlyCompileOptions;
using JS::SourceText;

// RAII class to check the frontend reports an exception when it fails to
// compile a script.
class MOZ_RAII AutoAssertReportedException {
#ifdef DEBUG
  JSContext* maybeCx_;
  FrontendContext* fc_;
  bool check_;

 public:
  explicit AutoAssertReportedException(JSContext* maybeCx, FrontendContext* fc)
      : maybeCx_(maybeCx), fc_(fc), check_(true) {}
  void reset() { check_ = false; }
  ~AutoAssertReportedException() {
    if (!check_) {
      return;
    }

    // Error while compiling self-hosted code isn't set as an exception.
    // TODO: Remove this once all errors are added to frontend context.
    if (maybeCx_ && !maybeCx_->runtime()->hasInitializedSelfHosting()) {
      return;
    }

    // TODO: Remove this once JSContext is removed from frontend.
    if (maybeCx_ && !maybeCx_->isHelperThreadContext()) {
      MOZ_ASSERT(maybeCx_->isExceptionPending() || fc_->hadErrors());
    } else {
      MOZ_ASSERT(fc_->hadErrors());
    }
  }
#else
 public:
  explicit AutoAssertReportedException(JSContext*, FrontendContext*) {}
  void reset() {}
#endif
};

static bool EmplaceEmitter(CompilationState& compilationState,
                           Maybe<BytecodeEmitter>& emitter, FrontendContext* fc,
                           const EitherParser& parser, SharedContext* sc);

template <typename Unit>
class MOZ_STACK_CLASS SourceAwareCompiler {
 protected:
  SourceText<Unit>& sourceBuffer_;

  CompilationState compilationState_;

  Maybe<Parser<SyntaxParseHandler, Unit>> syntaxParser;
  Maybe<Parser<FullParseHandler, Unit>> parser;
  FrontendContext* fc_ = nullptr;

  using TokenStreamPosition = frontend::TokenStreamPosition<Unit>;

 protected:
  explicit SourceAwareCompiler(FrontendContext* fc,
                               LifoAllocScope& parserAllocScope,
                               CompilationInput& input,
                               SourceText<Unit>& sourceBuffer)
      : sourceBuffer_(sourceBuffer),
        compilationState_(fc, parserAllocScope, input) {
    MOZ_ASSERT(sourceBuffer_.get() != nullptr);
  }

  [[nodiscard]] bool init(FrontendContext* fc, ScopeBindingCache* scopeCache,
                          InheritThis inheritThis = InheritThis::No,
                          JSObject* enclosingEnv = nullptr) {
    if (!compilationState_.init(fc, scopeCache, inheritThis, enclosingEnv)) {
      return false;
    }

    return createSourceAndParser(fc);
  }

  // Call this before calling compile{Global,Eval}Script.
  [[nodiscard]] bool createSourceAndParser(FrontendContext* fc);

  void assertSourceAndParserCreated() const {
    MOZ_ASSERT(compilationState_.source != nullptr);
    MOZ_ASSERT(parser.isSome());
  }

  void assertSourceParserAndScriptCreated() { assertSourceAndParserCreated(); }

  [[nodiscard]] bool emplaceEmitter(Maybe<BytecodeEmitter>& emitter,
                                    SharedContext* sharedContext) {
    return EmplaceEmitter(compilationState_, emitter, fc_,
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
class MOZ_STACK_CLASS ScriptCompiler : public SourceAwareCompiler<Unit> {
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
  explicit ScriptCompiler(FrontendContext* fc, LifoAllocScope& parserAllocScope,
                          CompilationInput& input,
                          SourceText<Unit>& sourceBuffer)
      : Base(fc, parserAllocScope, input, sourceBuffer) {}

  using Base::init;
  using Base::stencil;

  [[nodiscard]] bool compile(JSContext* cx, SharedContext* sc);
};

#ifdef JS_ENABLE_SMOOSH
[[nodiscard]] static bool TrySmoosh(
    JSContext* cx, FrontendContext* fc, CompilationInput& input,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    UniquePtr<ExtensibleCompilationStencil>& stencilOut) {
  MOZ_ASSERT(!stencilOut);

  if (!cx->options().trySmoosh()) {
    return true;
  }

  JSRuntime* rt = cx->runtime();
  if (!Smoosh::tryCompileGlobalScriptToExtensibleStencil(cx, fc, input, srcBuf,
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

  return stencilOut->source->assignSource(fc, input.options, srcBuf);
}

[[nodiscard]] static bool TrySmoosh(
    JSContext* cx, FrontendContext* fc, CompilationInput& input,
    JS::SourceText<char16_t>& srcBuf,
    UniquePtr<ExtensibleCompilationStencil>& stencilOut) {
  MOZ_ASSERT(!stencilOut);
  return true;
}
#endif  // JS_ENABLE_SMOOSH

using BytecodeCompilerOutput =
    mozilla::Variant<UniquePtr<ExtensibleCompilationStencil>,
                     RefPtr<CompilationStencil>, CompilationGCOutput*>;

// Compile global script, and return it as one of:
//   * ExtensibleCompilationStencil (without instantiation)
//   * CompilationStencil (without instantiation, has no external dependency)
//   * CompilationGCOutput (with instantiation).
template <typename Unit>
[[nodiscard]] static bool CompileGlobalScriptToStencilAndMaybeInstantiate(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<Unit>& srcBuf, ScopeKind scopeKind,
    BytecodeCompilerOutput& output) {
#ifdef JS_ENABLE_SMOOSH
  if (maybeCx) {
    UniquePtr<ExtensibleCompilationStencil> extensibleStencil;
    if (!TrySmoosh(maybeCx, fc, input, srcBuf, extensibleStencil)) {
      return false;
    }
    if (extensibleStencil) {
      if (input.options.populateDelazificationCache() &&
          !maybeCx->isHelperThreadContext()) {
        BorrowingCompilationStencil borrowingStencil(*extensibleStencil);
        StartOffThreadDelazification(maybeCx, input.options, borrowingStencil);

        // When we are trying to validate whether on-demand delazification
        // generate the same stencil as concurrent delazification, we want to
        // parse everything eagerly off-thread ahead of re-parsing everything on
        // demand, to compare the outcome.
        if (input.options.waitForDelazificationCache()) {
          WaitForAllDelazifyTasks(maybeCx->runtime());
        }
      }
      if (output.is<UniquePtr<ExtensibleCompilationStencil>>()) {
        output.as<UniquePtr<ExtensibleCompilationStencil>>() =
            std::move(extensibleStencil);
      } else if (output.is<RefPtr<CompilationStencil>>()) {
        RefPtr<CompilationStencil> stencil =
            fc->getAllocator()->new_<frontend::CompilationStencil>(
                std::move(extensibleStencil));
        if (!stencil) {
          return false;
        }

        output.as<RefPtr<CompilationStencil>>() = std::move(stencil);
      } else {
        BorrowingCompilationStencil borrowingStencil(*extensibleStencil);
        if (!InstantiateStencils(maybeCx, input, borrowingStencil,
                                 *(output.as<CompilationGCOutput*>()))) {
          return false;
        }
      }
      return true;
    }
  }
#endif  // JS_ENABLE_SMOOSH

  if (input.options.selfHostingMode) {
    if (!input.initForSelfHostingGlobal(fc)) {
      return false;
    }
  } else {
    if (!input.initForGlobal(fc)) {
      return false;
    }
  }

  AutoAssertReportedException assertException(maybeCx, fc);

  LifoAllocScope parserAllocScope(&tempLifoAlloc);
  ScriptCompiler<Unit> compiler(fc, parserAllocScope, input, srcBuf);
  if (!compiler.init(fc, scopeCache)) {
    return false;
  }

  SourceExtent extent = SourceExtent::makeGlobalExtent(
      srcBuf.length(), input.options.lineno, input.options.column);

  GlobalSharedContext globalsc(fc, scopeKind, input.options,
                               compiler.compilationState().directives, extent);

  if (!compiler.compile(maybeCx, &globalsc)) {
    return false;
  }

  if (input.options.populateDelazificationCache() && maybeCx &&
      !maybeCx->isHelperThreadContext()) {
    BorrowingCompilationStencil borrowingStencil(compiler.stencil());
    StartOffThreadDelazification(maybeCx, input.options, borrowingStencil);

    // When we are trying to validate whether on-demand delazification
    // generate the same stencil as concurrent delazification, we want to
    // parse everything eagerly off-thread ahead of re-parsing everything on
    // demand, to compare the outcome.
    if (input.options.waitForDelazificationCache()) {
      WaitForAllDelazifyTasks(maybeCx->runtime());
    }
  }

  if (output.is<UniquePtr<ExtensibleCompilationStencil>>()) {
    auto stencil =
        fc->getAllocator()->make_unique<ExtensibleCompilationStencil>(
            std::move(compiler.stencil()));
    if (!stencil) {
      return false;
    }
    output.as<UniquePtr<ExtensibleCompilationStencil>>() = std::move(stencil);
  } else if (output.is<RefPtr<CompilationStencil>>()) {
    Maybe<AutoGeckoProfilerEntry> pseudoFrame;
    if (maybeCx) {
      pseudoFrame.emplace(maybeCx, "script emit",
                          JS::ProfilingCategoryPair::JS_Parsing);
    }

    auto extensibleStencil =
        fc->getAllocator()->make_unique<frontend::ExtensibleCompilationStencil>(
            std::move(compiler.stencil()));
    if (!extensibleStencil) {
      return false;
    }

    RefPtr<CompilationStencil> stencil =
        fc->getAllocator()->new_<CompilationStencil>(
            std::move(extensibleStencil));
    if (!stencil) {
      return false;
    }

    output.as<RefPtr<CompilationStencil>>() = std::move(stencil);
  } else {
    MOZ_ASSERT(maybeCx);
    BorrowingCompilationStencil borrowingStencil(compiler.stencil());
    if (!InstantiateStencils(maybeCx, input, borrowingStencil,
                             *(output.as<CompilationGCOutput*>()))) {
      return false;
    }
  }

  assertException.reset();
  return true;
}

template <typename Unit>
static already_AddRefed<CompilationStencil> CompileGlobalScriptToStencilImpl(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<Unit>& srcBuf, ScopeKind scopeKind) {
  using OutputType = RefPtr<CompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!CompileGlobalScriptToStencilAndMaybeInstantiate(
          maybeCx, fc, tempLifoAlloc, input, scopeCache, srcBuf, scopeKind,
          output)) {
    return nullptr;
  }
  return output.as<OutputType>().forget();
}

already_AddRefed<CompilationStencil> frontend::CompileGlobalScriptToStencil(
    JSContext* cx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<char16_t>& srcBuf, ScopeKind scopeKind) {
  return CompileGlobalScriptToStencilImpl(cx, fc, tempLifoAlloc, input,
                                          scopeCache, srcBuf, scopeKind);
}

already_AddRefed<CompilationStencil> frontend::CompileGlobalScriptToStencil(
    JSContext* cx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<Utf8Unit>& srcBuf, ScopeKind scopeKind) {
  return CompileGlobalScriptToStencilImpl(cx, fc, tempLifoAlloc, input,
                                          scopeCache, srcBuf, scopeKind);
}

template <typename Unit>
static UniquePtr<ExtensibleCompilationStencil>
CompileGlobalScriptToExtensibleStencilImpl(JSContext* maybeCx,
                                           FrontendContext* fc,
                                           CompilationInput& input,
                                           ScopeBindingCache* scopeCache,
                                           JS::SourceText<Unit>& srcBuf,
                                           ScopeKind scopeKind) {
  using OutputType = UniquePtr<ExtensibleCompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!CompileGlobalScriptToStencilAndMaybeInstantiate(
          maybeCx, fc, maybeCx->tempLifoAlloc(), input, scopeCache, srcBuf,
          scopeKind, output)) {
    return nullptr;
  }
  return std::move(output.as<OutputType>());
}

UniquePtr<ExtensibleCompilationStencil>
frontend::CompileGlobalScriptToExtensibleStencil(
    JSContext* maybeCx, FrontendContext* fc, CompilationInput& input,
    ScopeBindingCache* scopeCache, JS::SourceText<char16_t>& srcBuf,
    ScopeKind scopeKind) {
  return CompileGlobalScriptToExtensibleStencilImpl(
      maybeCx, fc, input, scopeCache, srcBuf, scopeKind);
}

UniquePtr<ExtensibleCompilationStencil>
frontend::CompileGlobalScriptToExtensibleStencil(
    JSContext* cx, FrontendContext* fc, CompilationInput& input,
    ScopeBindingCache* scopeCache, JS::SourceText<Utf8Unit>& srcBuf,
    ScopeKind scopeKind) {
  return CompileGlobalScriptToExtensibleStencilImpl(cx, fc, input, scopeCache,
                                                    srcBuf, scopeKind);
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
    const JS::InstantiateOptions instantiateOptions(input.options);
    FireOnNewScript(cx, instantiateOptions, script);
  }

  return true;
}

bool frontend::PrepareForInstantiate(JSContext* maybeCx, FrontendContext* fc,
                                     CompilationInput& input,
                                     const CompilationStencil& stencil,
                                     CompilationGCOutput& gcOutput) {
  Maybe<AutoGeckoProfilerEntry> pseudoFrame;
  if (maybeCx) {
    pseudoFrame.emplace(maybeCx, "stencil instantiate",
                        JS::ProfilingCategoryPair::JS_Parsing);
  }

  return CompilationStencil::prepareForInstantiate(fc, input.atomCache, stencil,
                                                   gcOutput);
}

template <typename Unit>
static JSScript* CompileGlobalScriptImpl(
    JSContext* cx, FrontendContext* fc,
    const JS::ReadOnlyCompileOptions& options, JS::SourceText<Unit>& srcBuf,
    ScopeKind scopeKind) {
  Rooted<CompilationInput> input(cx, CompilationInput(options));
  Rooted<CompilationGCOutput> gcOutput(cx);
  BytecodeCompilerOutput output(gcOutput.address());
  NoScopeBindingCache scopeCache;
  if (!CompileGlobalScriptToStencilAndMaybeInstantiate(
          cx, fc, cx->tempLifoAlloc(), input.get(), &scopeCache, srcBuf,
          scopeKind, output)) {
    return nullptr;
  }
  return gcOutput.get().script;
}

JSScript* frontend::CompileGlobalScript(
    JSContext* cx, FrontendContext* fc,
    const JS::ReadOnlyCompileOptions& options, JS::SourceText<char16_t>& srcBuf,
    ScopeKind scopeKind) {
  return CompileGlobalScriptImpl(cx, fc, options, srcBuf, scopeKind);
}

JSScript* frontend::CompileGlobalScript(
    JSContext* cx, FrontendContext* fc,
    const JS::ReadOnlyCompileOptions& options, JS::SourceText<Utf8Unit>& srcBuf,
    ScopeKind scopeKind) {
  return CompileGlobalScriptImpl(cx, fc, options, srcBuf, scopeKind);
}

template <typename Unit>
static JSScript* CompileEvalScriptImpl(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    SourceText<Unit>& srcBuf, JS::Handle<js::Scope*> enclosingScope,
    JS::Handle<JSObject*> enclosingEnv) {
  JS::Rooted<JSScript*> script(cx);
  {
    AutoReportFrontendContext fc(cx);
    AutoAssertReportedException assertException(cx, &fc);

    Rooted<CompilationInput> input(cx, CompilationInput(options));
    if (!input.get().initForEval(&fc, enclosingScope)) {
      return nullptr;
    }

    LifoAllocScope parserAllocScope(&cx->tempLifoAlloc());

    ScopeBindingCache* scopeCache = &cx->caches().scopeCache;
    ScriptCompiler<Unit> compiler(&fc, parserAllocScope, input.get(), srcBuf);
    if (!compiler.init(&fc, scopeCache, InheritThis::Yes, enclosingEnv)) {
      return nullptr;
    }

    uint32_t len = srcBuf.length();
    SourceExtent extent =
        SourceExtent::makeGlobalExtent(len, options.lineno, options.column);
    EvalSharedContext evalsc(&fc, compiler.compilationState(), extent);
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
    script = gcOutput.get().script;
  }
  return script;
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
class MOZ_STACK_CLASS ModuleCompiler final : public SourceAwareCompiler<Unit> {
  using Base = SourceAwareCompiler<Unit>;

  using Base::assertSourceParserAndScriptCreated;
  using Base::compilationState_;
  using Base::emplaceEmitter;
  using Base::parser;

 public:
  explicit ModuleCompiler(FrontendContext* fc, LifoAllocScope& parserAllocScope,
                          CompilationInput& input,
                          SourceText<Unit>& sourceBuffer)
      : Base(fc, parserAllocScope, input, sourceBuffer) {}

  using Base::init;
  using Base::stencil;

  [[nodiscard]] bool compile(JSContext* maybeCx, FrontendContext* fc);
};

template <typename Unit>
class MOZ_STACK_CLASS StandaloneFunctionCompiler final
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
  explicit StandaloneFunctionCompiler(FrontendContext* fc,
                                      LifoAllocScope& parserAllocScope,
                                      CompilationInput& input,
                                      SourceText<Unit>& sourceBuffer)
      : Base(fc, parserAllocScope, input, sourceBuffer) {}

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

template <typename Unit>
bool SourceAwareCompiler<Unit>::createSourceAndParser(FrontendContext* fc) {
  const auto& options = compilationState_.input.options;

  fc_ = fc;

  if (!compilationState_.source->assignSource(fc, options, sourceBuffer_)) {
    return false;
  }

  MOZ_ASSERT(compilationState_.canLazilyParse ==
             CanLazilyParse(compilationState_.input.options));
  if (compilationState_.canLazilyParse) {
    syntaxParser.emplace(fc_, options, sourceBuffer_.units(),
                         sourceBuffer_.length(),
                         /* foldConstants = */ false, compilationState_,
                         /* syntaxParser = */ nullptr);
    if (!syntaxParser->checkOptions()) {
      return false;
    }
  }

  parser.emplace(fc_, options, sourceBuffer_.units(), sourceBuffer_.length(),
                 /* foldConstants = */ true, compilationState_,
                 syntaxParser.ptrOr(nullptr));
  parser->ss = compilationState_.source.get();
  return parser->checkOptions();
}

static bool EmplaceEmitter(CompilationState& compilationState,
                           Maybe<BytecodeEmitter>& emitter, FrontendContext* fc,
                           const EitherParser& parser, SharedContext* sc) {
  BytecodeEmitter::EmitterMode emitterMode =
      sc->selfHosted() ? BytecodeEmitter::SelfHosting : BytecodeEmitter::Normal;
  emitter.emplace(fc, parser, sc, compilationState, emitterMode);
  return emitter->init();
}

template <typename Unit>
bool SourceAwareCompiler<Unit>::canHandleParseFailure(
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
void SourceAwareCompiler<Unit>::handleParseFailure(
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
bool ScriptCompiler<Unit>::compile(JSContext* maybeCx, SharedContext* sc) {
  assertSourceParserAndScriptCreated();

  TokenStreamPosition startPosition(parser->tokenStream);

  // Emplace the topLevel stencil
  MOZ_ASSERT(compilationState_.scriptData.length() ==
             CompilationStencil::TopLevelIndex);
  if (!compilationState_.appendScriptStencilAndData(sc->fc_)) {
    return false;
  }

  ParseNode* pn;
  {
    Maybe<AutoGeckoProfilerEntry> pseudoFrame;
    if (maybeCx) {
      pseudoFrame.emplace(maybeCx, "script parsing",
                          JS::ProfilingCategoryPair::JS_Parsing);
    }
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
    Maybe<AutoGeckoProfilerEntry> pseudoFrame;
    if (maybeCx) {
      pseudoFrame.emplace(maybeCx, "script emit",
                          JS::ProfilingCategoryPair::JS_Parsing);
    }

    Maybe<BytecodeEmitter> emitter;
    if (!emplaceEmitter(emitter, sc)) {
      return false;
    }

    if (!emitter->emitScript(pn)) {
      return false;
    }
  }

  MOZ_ASSERT(!this->fc_->hadErrors());

  return true;
}

template <typename Unit>
bool ModuleCompiler<Unit>::compile(JSContext* maybeCx, FrontendContext* fc) {
  // Emplace the topLevel stencil
  MOZ_ASSERT(compilationState_.scriptData.length() ==
             CompilationStencil::TopLevelIndex);
  if (!compilationState_.appendScriptStencilAndData(fc)) {
    return false;
  }

  ModuleBuilder builder(fc, parser.ptr());

  const auto& options = compilationState_.input.options;

  uint32_t len = this->sourceBuffer_.length();
  SourceExtent extent =
      SourceExtent::makeGlobalExtent(len, options.lineno, options.column);
  ModuleSharedContext modulesc(fc, options, builder, extent);

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

  MOZ_ASSERT(!this->fc_->hadErrors());

  return true;
}

// Parse a standalone JS function, which might appear as the value of an
// event handler attribute in an HTML <INPUT> tag, or in a Function()
// constructor.
template <typename Unit>
FunctionNode* StandaloneFunctionCompiler<Unit>::parse(
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
bool StandaloneFunctionCompiler<Unit>::compile(
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
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    SourceText<Unit>& srcBuf, BytecodeCompilerOutput& output) {
  MOZ_ASSERT(srcBuf.get());

  if (!input.initForModule(fc)) {
    return false;
  }

  AutoAssertReportedException assertException(maybeCx, fc);

  LifoAllocScope parserAllocScope(&tempLifoAlloc);
  ModuleCompiler<Unit> compiler(fc, parserAllocScope, input, srcBuf);
  if (!compiler.init(fc, scopeCache)) {
    return false;
  }

  if (!compiler.compile(maybeCx, fc)) {
    return false;
  }

  if (output.is<UniquePtr<ExtensibleCompilationStencil>>()) {
    auto stencil =
        fc->getAllocator()->make_unique<ExtensibleCompilationStencil>(
            std::move(compiler.stencil()));
    if (!stencil) {
      return false;
    }
    output.as<UniquePtr<ExtensibleCompilationStencil>>() = std::move(stencil);
  } else if (output.is<RefPtr<CompilationStencil>>()) {
    Maybe<AutoGeckoProfilerEntry> pseudoFrame;
    if (maybeCx) {
      pseudoFrame.emplace(maybeCx, "script emit",
                          JS::ProfilingCategoryPair::JS_Parsing);
    }

    auto extensibleStencil =
        fc->getAllocator()->make_unique<frontend::ExtensibleCompilationStencil>(
            std::move(compiler.stencil()));
    if (!extensibleStencil) {
      return false;
    }

    RefPtr<CompilationStencil> stencil =
        fc->getAllocator()->new_<CompilationStencil>(
            std::move(extensibleStencil));
    if (!stencil) {
      return false;
    }

    output.as<RefPtr<CompilationStencil>>() = std::move(stencil);
  } else {
    MOZ_ASSERT(maybeCx);
    BorrowingCompilationStencil borrowingStencil(compiler.stencil());
    if (!InstantiateStencils(maybeCx, input, borrowingStencil,
                             *(output.as<CompilationGCOutput*>()))) {
      return false;
    }
  }

  assertException.reset();
  return true;
}

template <typename Unit>
already_AddRefed<CompilationStencil> ParseModuleToStencilImpl(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    SourceText<Unit>& srcBuf) {
  using OutputType = RefPtr<CompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!ParseModuleToStencilAndMaybeInstantiate(
          maybeCx, fc, tempLifoAlloc, input, scopeCache, srcBuf, output)) {
    return nullptr;
  }
  return output.as<OutputType>().forget();
}

already_AddRefed<CompilationStencil> frontend::ParseModuleToStencil(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    SourceText<char16_t>& srcBuf) {
  return ParseModuleToStencilImpl(maybeCx, fc, tempLifoAlloc, input, scopeCache,
                                  srcBuf);
}

already_AddRefed<CompilationStencil> frontend::ParseModuleToStencil(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    SourceText<Utf8Unit>& srcBuf) {
  return ParseModuleToStencilImpl(maybeCx, fc, tempLifoAlloc, input, scopeCache,
                                  srcBuf);
}

template <typename Unit>
UniquePtr<ExtensibleCompilationStencil> ParseModuleToExtensibleStencilImpl(
    JSContext* cx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    SourceText<Unit>& srcBuf) {
  using OutputType = UniquePtr<ExtensibleCompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!ParseModuleToStencilAndMaybeInstantiate(cx, fc, tempLifoAlloc, input,
                                               scopeCache, srcBuf, output)) {
    return nullptr;
  }
  return std::move(output.as<OutputType>());
}

UniquePtr<ExtensibleCompilationStencil>
frontend::ParseModuleToExtensibleStencil(JSContext* cx, FrontendContext* fc,
                                         js::LifoAlloc& tempLifoAlloc,
                                         CompilationInput& input,
                                         ScopeBindingCache* scopeCache,
                                         SourceText<char16_t>& srcBuf) {
  return ParseModuleToExtensibleStencilImpl(cx, fc, tempLifoAlloc, input,
                                            scopeCache, srcBuf);
}

UniquePtr<ExtensibleCompilationStencil>
frontend::ParseModuleToExtensibleStencil(JSContext* cx, FrontendContext* fc,
                                         js::LifoAlloc& tempLifoAlloc,
                                         CompilationInput& input,
                                         ScopeBindingCache* scopeCache,
                                         SourceText<Utf8Unit>& srcBuf) {
  return ParseModuleToExtensibleStencilImpl(cx, fc, tempLifoAlloc, input,
                                            scopeCache, srcBuf);
}

template <typename Unit>
static ModuleObject* CompileModuleImpl(
    JSContext* cx, FrontendContext* fc,
    const JS::ReadOnlyCompileOptions& optionsInput, SourceText<Unit>& srcBuf) {
  AutoAssertReportedException assertException(cx, fc);

  CompileOptions options(cx, optionsInput);
  options.setModule();

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  Rooted<CompilationGCOutput> gcOutput(cx);
  BytecodeCompilerOutput output(gcOutput.address());

  NoScopeBindingCache scopeCache;
  if (!ParseModuleToStencilAndMaybeInstantiate(cx, fc, cx->tempLifoAlloc(),
                                               input.get(), &scopeCache, srcBuf,
                                               output)) {
    return nullptr;
  }

  assertException.reset();
  return gcOutput.get().module;
}

ModuleObject* frontend::CompileModule(JSContext* cx, FrontendContext* fc,
                                      const JS::ReadOnlyCompileOptions& options,
                                      SourceText<char16_t>& srcBuf) {
  return CompileModuleImpl(cx, fc, options, srcBuf);
}

ModuleObject* frontend::CompileModule(JSContext* cx, FrontendContext* fc,
                                      const JS::ReadOnlyCompileOptions& options,
                                      SourceText<Utf8Unit>& srcBuf) {
  return CompileModuleImpl(cx, fc, options, srcBuf);
}

static bool InstantiateLazyFunction(JSContext* cx, CompilationInput& input,
                                    CompilationStencil& stencil,
                                    BytecodeCompilerOutput& output) {
  // We do check the type, but do not write anything to it as this is not
  // necessary for lazy function, as the script is patched inside the
  // JSFunction when instantiating.
  MOZ_ASSERT(output.is<CompilationGCOutput*>());
  MOZ_ASSERT(!output.as<CompilationGCOutput*>());

  mozilla::DebugOnly<uint32_t> lazyFlags =
      static_cast<uint32_t>(input.immutableFlags());

  Rooted<CompilationGCOutput> gcOutput(cx);

  if (input.source->hasEncoder()) {
    if (!input.source->addDelazificationToIncrementalEncoding(cx, stencil)) {
      return false;
    }
  }

  if (!CompilationStencil::instantiateStencils(cx, input, stencil,
                                               gcOutput.get())) {
    return false;
  }

  // NOTE: After instantiation succeeds and bytecode is attached, the rest of
  //       this operation should be infallible. Any failure during
  //       delazification should restore the function back to a consistent
  //       lazy state.

  MOZ_ASSERT(lazyFlags == gcOutput.get().script->immutableFlags());
  MOZ_ASSERT(gcOutput.get().script->outermostScope()->hasOnChain(
                 ScopeKind::NonSyntactic) ==
             gcOutput.get().script->immutableFlags().hasFlag(
                 JSScript::ImmutableFlags::HasNonSyntacticScope));

  return true;
}

enum class GetCachedResult {
  // Similar to return false.
  Error,

  // We have not found any entry.
  NotFound,

  // We have found an entry, and set everything according to the desired
  // BytecodeCompilerOutput out-param.
  Found
};

// When we have a cache hit, the addPtr out-param would evaluate to a true-ish
// value.
static GetCachedResult GetCachedLazyFunctionStencilMaybeInstantiate(
    JSContext* cx, FrontendContext* fc, CompilationInput& input,
    BytecodeCompilerOutput& output) {
  RefPtr<CompilationStencil> stencil;
  {
    StencilCache& cache = cx->runtime()->caches().delazificationCache;
    auto guard = cache.isSourceCached(input.source);
    if (!guard) {
      return GetCachedResult::NotFound;
    }

    // Before releasing the guard, which is locking the cache, we increment the
    // reference counter such that we do not reclaim the CompilationStencil
    // while we are instantiating it.
    StencilContext key(input.source, input.extent());
    stencil = cache.lookup(guard, key);
    if (!stencil) {
      return GetCachedResult::NotFound;
    }
  }

  if (output.is<RefPtr<CompilationStencil>>()) {
    output.as<RefPtr<CompilationStencil>>() = stencil;
    return GetCachedResult::Found;
  }

  if (output.is<UniquePtr<ExtensibleCompilationStencil>>()) {
    auto extensible =
        fc->getAllocator()->make_unique<ExtensibleCompilationStencil>(input);
    if (!extensible) {
      return GetCachedResult::Error;
    }
    if (!extensible->cloneFrom(fc, *stencil)) {
      return GetCachedResult::Error;
    }

    output.as<UniquePtr<ExtensibleCompilationStencil>>() =
        std::move(extensible);
    return GetCachedResult::Found;
  }

  if (!InstantiateLazyFunction(cx, input, *stencil, output)) {
    return GetCachedResult::Error;
  }

  return GetCachedResult::Found;
}

template <typename Unit>
static bool CompileLazyFunctionToStencilMaybeInstantiate(
    JSContext* cx, FrontendContext* fc, CompilationInput& input,
    ScopeBindingCache* scopeCache, const Unit* units, size_t length,
    BytecodeCompilerOutput& output) {
  MOZ_ASSERT(input.source);

  AutoAssertReportedException assertException(cx, fc);
  if (input.options.consumeDelazificationCache()) {
    auto res =
        GetCachedLazyFunctionStencilMaybeInstantiate(cx, fc, input, output);
    switch (res) {
      case GetCachedResult::Error:
        return false;
      case GetCachedResult::Found:
        assertException.reset();
        return true;
      case GetCachedResult::NotFound:
        break;
    }
  }

  InheritThis inheritThis =
      input.functionFlags().isArrow() ? InheritThis::Yes : InheritThis::No;

  LifoAllocScope parserAllocScope(&cx->tempLifoAlloc());
  CompilationState compilationState(fc, parserAllocScope, input);
  compilationState.setFunctionKey(input.extent());
  MOZ_ASSERT(!compilationState.isInitialStencil());
  if (!compilationState.init(fc, scopeCache, inheritThis)) {
    return false;
  }

  Parser<FullParseHandler, Unit> parser(fc, input.options, units, length,
                                        /* foldConstants = */ true,
                                        compilationState,
                                        /* syntaxParser = */ nullptr);
  if (!parser.checkOptions()) {
    return false;
  }

  FunctionNode* pn = parser.standaloneLazyFunction(
      input, input.extent().toStringStart, input.strict(),
      input.generatorKind(), input.asyncKind());
  if (!pn) {
    return false;
  }

  BytecodeEmitter bce(fc, &parser, pn->funbox(), compilationState,
                      BytecodeEmitter::LazyFunction);
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

  if (input.options.checkDelazificationCache()) {
    using OutputType = RefPtr<CompilationStencil>;
    BytecodeCompilerOutput cached((OutputType()));
    auto res =
        GetCachedLazyFunctionStencilMaybeInstantiate(cx, fc, input, cached);
    if (res == GetCachedResult::Error) {
      return false;
    }
    // Cached results might be removed by GCs.
    if (res == GetCachedResult::Found) {
      auto& concurrentSharedData = cached.as<OutputType>().get()->sharedData;
      auto concurrentData =
          concurrentSharedData.isSingle()
              ? concurrentSharedData.asSingle()->get()->immutableData()
              : concurrentSharedData.asBorrow()
                    ->asSingle()
                    ->get()
                    ->immutableData();
      auto ondemandData =
          compilationState.sharedData.asSingle()->get()->immutableData();
      MOZ_RELEASE_ASSERT(concurrentData.Length() == ondemandData.Length(),
                         "Non-deterministic stencils");
      for (size_t i = 0; i < concurrentData.Length(); i++) {
        MOZ_RELEASE_ASSERT(concurrentData[i] == ondemandData[i],
                           "Non-deterministic stencils");
      }
    }
  }

  if (output.is<UniquePtr<ExtensibleCompilationStencil>>()) {
    auto stencil =
        fc->getAllocator()->make_unique<ExtensibleCompilationStencil>(
            std::move(compilationState));
    if (!stencil) {
      return false;
    }
    output.as<UniquePtr<ExtensibleCompilationStencil>>() = std::move(stencil);
  } else if (output.is<RefPtr<CompilationStencil>>()) {
    Maybe<AutoGeckoProfilerEntry> pseudoFrame;
    if (cx) {
      pseudoFrame.emplace(cx, "script emit",
                          JS::ProfilingCategoryPair::JS_Parsing);
    }

    auto extensibleStencil =
        fc->getAllocator()->make_unique<frontend::ExtensibleCompilationStencil>(
            std::move(compilationState));
    if (!extensibleStencil) {
      return false;
    }

    RefPtr<CompilationStencil> stencil =
        fc->getAllocator()->new_<CompilationStencil>(
            std::move(extensibleStencil));
    if (!stencil) {
      return false;
    }

    output.as<RefPtr<CompilationStencil>>() = std::move(stencil);
  } else {
    BorrowingCompilationStencil borrowingStencil(compilationState);
    if (!InstantiateLazyFunction(cx, input, borrowingStencil, output)) {
      return false;
    }
  }

  assertException.reset();
  return true;
}

template <typename Unit>
static bool DelazifyCanonicalScriptedFunctionImpl(
    JSContext* cx, FrontendContext* fc, ScopeBindingCache* scopeCache,
    HandleFunction fun, Handle<BaseScript*> lazy, ScriptSource* ss) {
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
      .setSelfHostingMode(false)
      .setEagerDelazificationStrategy(lazy->delazificationMode());

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  input.get().initFromLazy(cx, lazy, ss);

  CompilationGCOutput* unusedGcOutput = nullptr;
  BytecodeCompilerOutput output(unusedGcOutput);
  return CompileLazyFunctionToStencilMaybeInstantiate(
      cx, fc, input.get(), scopeCache, units.get(), sourceLength, output);
}

bool frontend::DelazifyCanonicalScriptedFunction(JSContext* cx,
                                                 FrontendContext* fc,
                                                 HandleFunction fun) {
  Maybe<AutoGeckoProfilerEntry> pseudoFrame;
  if (cx) {
    pseudoFrame.emplace(cx, "script delazify",
                        JS::ProfilingCategoryPair::JS_Parsing);
  }

  Rooted<BaseScript*> lazy(cx, fun->baseScript());
  ScriptSource* ss = lazy->scriptSource();
  ScopeBindingCache* scopeCache = &cx->caches().scopeCache;

  if (ss->hasSourceType<Utf8Unit>()) {
    // UTF-8 source text.
    return DelazifyCanonicalScriptedFunctionImpl<Utf8Unit>(cx, fc, scopeCache,
                                                           fun, lazy, ss);
  }

  MOZ_ASSERT(ss->hasSourceType<char16_t>());

  // UTF-16 source text.
  return DelazifyCanonicalScriptedFunctionImpl<char16_t>(cx, fc, scopeCache,
                                                         fun, lazy, ss);
}

template <typename Unit>
static already_AddRefed<CompilationStencil>
DelazifyCanonicalScriptedFunctionImpl(JSContext* cx, FrontendContext* fc,
                                      ScopeBindingCache* scopeCache,
                                      CompilationStencil& context,
                                      ScriptIndex scriptIndex) {
  ScriptStencilRef script{context, scriptIndex};
  const ScriptStencilExtra& extra = script.scriptExtra();

#if defined(EARLY_BETA_OR_EARLIER) || defined(DEBUG)
  const ScriptStencil& data = script.scriptData();
  MOZ_ASSERT(!data.hasSharedData(), "Script is already compiled!");
  MOZ_DIAGNOSTIC_ASSERT(!data.isGhost());
#endif

  Maybe<AutoIncrementalTimer> timer;
  if (cx->realm()) {
    timer.emplace(cx->realm()->timers.delazificationTime);
  }

  size_t sourceStart = extra.extent.sourceStart;
  size_t sourceLength = extra.extent.sourceEnd - sourceStart;

  ScriptSource* ss = context.source;
  MOZ_ASSERT(ss->hasSourceText());

  // Parse and compile the script from source.
  UncompressedSourceCache::AutoHoldEntry holder;

  MOZ_ASSERT(ss->hasSourceType<Unit>());

  ScriptSource::PinnedUnits<Unit> units(cx, ss, holder, sourceStart,
                                        sourceLength);
  if (!units.get()) {
    return nullptr;
  }

  JS::CompileOptions options(cx);
  options.setMutedErrors(ss->mutedErrors())
      .setFileAndLine(ss->filename(), extra.extent.lineno)
      .setColumn(extra.extent.column)
      .setScriptSourceOffset(sourceStart)
      .setNoScriptRval(false)
      .setSelfHostingMode(false);

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  input.get().initFromStencil(context, scriptIndex, ss);

  using OutputType = RefPtr<CompilationStencil>;
  BytecodeCompilerOutput output((OutputType()));
  if (!CompileLazyFunctionToStencilMaybeInstantiate(
          cx, fc, input.get(), scopeCache, units.get(), sourceLength, output)) {
    return nullptr;
  }
  return output.as<OutputType>().forget();
}

already_AddRefed<CompilationStencil>
frontend::DelazifyCanonicalScriptedFunction(JSContext* cx, FrontendContext* fc,
                                            ScopeBindingCache* scopeCache,
                                            CompilationStencil& context,
                                            ScriptIndex scriptIndex) {
  Maybe<AutoGeckoProfilerEntry> pseudoFrame;
  if (cx) {
    pseudoFrame.emplace(cx, "stencil script delazify",
                        JS::ProfilingCategoryPair::JS_Parsing);
  }

  ScriptSource* ss = context.source;
  if (ss->hasSourceType<Utf8Unit>()) {
    // UTF-8 source text.
    return DelazifyCanonicalScriptedFunctionImpl<Utf8Unit>(
        cx, fc, scopeCache, context, scriptIndex);
  }

  // UTF-16 source text.
  MOZ_ASSERT(ss->hasSourceType<char16_t>());
  return DelazifyCanonicalScriptedFunctionImpl<char16_t>(cx, fc, scopeCache,
                                                         context, scriptIndex);
}

static JSFunction* CompileStandaloneFunction(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, const Maybe<uint32_t>& parameterListEnd,
    FunctionSyntaxKind syntaxKind, GeneratorKind generatorKind,
    FunctionAsyncKind asyncKind, Handle<Scope*> enclosingScope = nullptr) {
  JS::Rooted<JSFunction*> fun(cx);
  {
    AutoReportFrontendContext fc(cx);
    AutoAssertReportedException assertException(cx, &fc);

    Rooted<CompilationInput> input(cx, CompilationInput(options));
    if (enclosingScope) {
      if (!input.get().initForStandaloneFunctionInNonSyntacticScope(
              &fc, enclosingScope)) {
        return nullptr;
      }
    } else {
      if (!input.get().initForStandaloneFunction(cx, &fc)) {
        return nullptr;
      }
    }

    LifoAllocScope parserAllocScope(&cx->tempLifoAlloc());
    InheritThis inheritThis = (syntaxKind == FunctionSyntaxKind::Arrow)
                                  ? InheritThis::Yes
                                  : InheritThis::No;
    ScopeBindingCache* scopeCache = &cx->caches().scopeCache;
    StandaloneFunctionCompiler<char16_t> compiler(&fc, parserAllocScope,
                                                  input.get(), srcBuf);
    if (!compiler.init(&fc, scopeCache, inheritThis)) {
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

    fun = gcOutput.get().getFunctionNoBaseIndex(
        CompilationStencil::TopLevelIndex);
    MOZ_ASSERT(fun->hasBytecode() || IsAsmJSModule(fun));

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

      const JS::InstantiateOptions instantiateOptions(options);
      Rooted<JSScript*> script(cx, gcOutput.get().script);
      FireOnNewScript(cx, instantiateOptions, script);
    }

    assertException.reset();
  }
  return fun;
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
    FunctionSyntaxKind syntaxKind, Handle<Scope*> enclosingScope) {
  MOZ_ASSERT(enclosingScope);
  return CompileStandaloneFunction(cx, options, srcBuf, parameterListEnd,
                                   syntaxKind, GeneratorKind::NotGenerator,
                                   FunctionAsyncKind::SyncFunction,
                                   enclosingScope);
}

void frontend::FireOnNewScript(JSContext* cx,
                               const JS::InstantiateOptions& options,
                               JS::Handle<JSScript*> script) {
  if (!options.hideFromNewScriptInitial()) {
    DebugAPI::onNewScript(cx, script);
  }
}
