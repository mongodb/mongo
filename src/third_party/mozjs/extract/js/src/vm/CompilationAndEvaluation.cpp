/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Same-thread compilation and evaluation APIs. */

#include "js/CompilationAndEvaluation.h"

#include "mozilla/Maybe.h"  // mozilla::None, mozilla::Some
#include "mozilla/Utf8.h"   // mozilla::Utf8Unit

#include <utility>  // std::move

#include "jsapi.h"    // JS_WrapValue
#include "jstypes.h"  // JS_PUBLIC_API

#include "debugger/DebugAPI.h"
#include "frontend/BytecodeCompiler.h"  // frontend::{CompileGlobalScript, CompileStandaloneFunction, CompileStandaloneFunctionInNonSyntacticScope}
#include "frontend/CompilationStencil.h"  // for frontened::{CompilationStencil, BorrowingCompilationStencil, CompilationGCOutput, InitialStencilAndDelazifications}
#include "frontend/FrontendContext.h"     // js::AutoReportFrontendContext
#include "frontend/Parser.h"      // frontend::Parser, frontend::ParseGoal
#include "frontend/StencilXdr.h"  // js::XDRStencilEncoder
#include "js/CharacterEncoding.h"  // JS::UTF8Chars, JS::ConstUTF8CharsZ, JS::UTF8CharsToNewTwoByteCharsZ
#include "js/ColumnNumber.h"            // JS::ColumnNumberOneOrigin
#include "js/EnvironmentChain.h"        // JS::EnvironmentChain
#include "js/experimental/JSStencil.h"  // JS::Stencil
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "js/RootingAPI.h"              // JS::Rooted
#include "js/SourceText.h"              // JS::SourceText
#include "js/Transcoding.h"  // JS::TranscodeBuffer, JS::IsTranscodeFailureResult
#include "js/TypeDecls.h"    // JS::HandleObject, JS::MutableHandleScript
#include "js/Utility.h"      // js::MallocArena, JS::UniqueTwoByteChars
#include "js/Value.h"        // JS::Value
#include "util/CompleteFile.h"     // js::FileContents, js::ReadCompleteFile
#include "util/Identifier.h"       // js::IsIdentifier
#include "util/StringBuilder.h"    // js::StringBuilder
#include "vm/EnvironmentObject.h"  // js::CreateNonSyntacticEnvironmentChain
#include "vm/ErrorReporting.h"  // js::ErrorMetadata, js::ReportCompileErrorLatin1
#include "vm/Interpreter.h"     // js::Execute
#include "vm/JSContext.h"       // JSContext
#include "vm/JSScript.h"        // js::ScriptSourceObject
#include "vm/Xdr.h"             // XDRResult

#include "vm/JSContext-inl.h"  // JSContext::check

using mozilla::Utf8Unit;

using JS::CompileOptions;
using JS::HandleObject;
using JS::ReadOnlyCompileOptions;
using JS::SourceOwnership;
using JS::SourceText;
using JS::UniqueTwoByteChars;
using JS::UTF8Chars;
using JS::UTF8CharsToNewTwoByteCharsZ;

using namespace js;

JS_PUBLIC_API void JS::detail::ReportSourceTooLong(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_SOURCE_TOO_LONG);
}

static void ReportSourceTooLongImpl(JS::FrontendContext* fc, ...) {
  va_list args;
  va_start(args, fc);

  js::ErrorMetadata metadata;
  metadata.filename = JS::ConstUTF8CharsZ("<unknown>");
  metadata.lineNumber = 0;
  metadata.columnNumber = JS::ColumnNumberOneOrigin();
  metadata.lineLength = 0;
  metadata.tokenOffset = 0;
  metadata.isMuted = false;

  js::ReportCompileErrorLatin1VA(fc, std::move(metadata), nullptr,
                                 JSMSG_SOURCE_TOO_LONG, &args);

  va_end(args);
}

JS_PUBLIC_API void JS::detail::ReportSourceTooLong(JS::FrontendContext* fc) {
  ReportSourceTooLongImpl(fc);
}

template <typename Unit>
static JSScript* CompileSourceBuffer(JSContext* cx,
                                     const ReadOnlyCompileOptions& options,
                                     SourceText<Unit>& srcBuf) {
  ScopeKind scopeKind =
      options.nonSyntacticScope ? ScopeKind::NonSyntactic : ScopeKind::Global;

  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  JS::Rooted<JSScript*> script(cx);
  {
    AutoReportFrontendContext fc(cx);
    script = frontend::CompileGlobalScript(cx, &fc, options, srcBuf, scopeKind);
  }
  return script;
}

JSScript* JS::Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
                      SourceText<char16_t>& srcBuf) {
  return CompileSourceBuffer(cx, options, srcBuf);
}

JSScript* JS::Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
                      SourceText<Utf8Unit>& srcBuf) {
  return CompileSourceBuffer(cx, options, srcBuf);
}

static bool StartCollectingDelazifications(JSContext* cx,
                                           JS::Handle<ScriptSourceObject*> sso,
                                           JS::Stencil* stencil,
                                           bool& alreadyStarted) {
  if (sso->isCollectingDelazifications()) {
    alreadyStarted = true;
    return true;
  }

  alreadyStarted = false;

  // We don't support asm.js in XDR.
  // Failures are reported by the FinishCollectingDelazifications function
  // below.
  if (stencil->getInitial()->hasAsmJS()) {
    return true;
  }

  if (!sso->maybeGetStencils()) {
    RefPtr stencils = stencil;
    sso->setStencils(stencils.forget());
  } else {
    MOZ_ASSERT(sso->maybeGetStencils() == stencil);
  }
  sso->setCollectingDelazifications();
  return true;
}

JS_PUBLIC_API bool JS::StartCollectingDelazifications(
    JSContext* cx, JS::Handle<JSScript*> script, JS::Stencil* stencil,
    bool& alreadyStarted) {
  JS::Rooted<ScriptSourceObject*> sso(cx, script->sourceObject());
  return ::StartCollectingDelazifications(cx, sso, stencil, alreadyStarted);
}

JS_PUBLIC_API bool JS::StartCollectingDelazifications(
    JSContext* cx, JS::Handle<JSObject*> module, JS::Stencil* stencil,
    bool& alreadyStarted) {
  JS::Rooted<ScriptSourceObject*> sso(
      cx, module->as<ModuleObject>().scriptSourceObject());
  return ::StartCollectingDelazifications(cx, sso, stencil, alreadyStarted);
}

static bool FinishCollectingDelazifications(JSContext* cx,
                                            JS::Handle<ScriptSourceObject*> sso,
                                            JS::TranscodeBuffer& buffer) {
  if (!sso->isCollectingDelazifications()) {
    JS_ReportErrorASCII(cx, "Not collecting delazifications");
    return false;
  }

  RefPtr<frontend::InitialStencilAndDelazifications> stencils =
      sso->maybeGetStencils();
  sso->unsetCollectingDelazifications();

  AutoReportFrontendContext fc(cx);
  UniquePtr<frontend::CompilationStencil> stencilHolder;
  const frontend::CompilationStencil* stencil;

  if (stencils->canLazilyParse()) {
    stencilHolder.reset(stencils->getMerged(&fc));
    if (!stencilHolder) {
      return false;
    }
    stencil = stencilHolder.get();
  } else {
    stencil = stencils->getInitial();
  }

  XDRStencilEncoder encoder(&fc, buffer);
  XDRResult res = encoder.codeStencil(sso->source(), *stencil);
  if (res.isErr()) {
    if (JS::IsTranscodeFailureResult(res.unwrapErr())) {
      fc.clearAutoReport();
      JS_ReportErrorASCII(cx, "XDR encoding failure");
    }
    return false;
  }
  return true;
}

static bool FinishCollectingDelazifications(JSContext* cx,
                                            JS::Handle<ScriptSourceObject*> sso,
                                            JS::Stencil** stencilOut) {
  if (!sso->isCollectingDelazifications()) {
    JS_ReportErrorASCII(cx, "Not collecting delazifications");
    return false;
  }

  RefPtr<frontend::InitialStencilAndDelazifications> stencils =
      sso->maybeGetStencils();
  sso->unsetCollectingDelazifications();

  stencils.forget(stencilOut);
  return true;
}

JS_PUBLIC_API bool JS::FinishCollectingDelazifications(
    JSContext* cx, JS::HandleScript script, JS::TranscodeBuffer& buffer) {
  JS::Rooted<ScriptSourceObject*> sso(cx, script->sourceObject());
  return ::FinishCollectingDelazifications(cx, sso, buffer);
}

JS_PUBLIC_API bool JS::FinishCollectingDelazifications(
    JSContext* cx, JS::HandleScript script, JS::Stencil** stencilOut) {
  JS::Rooted<ScriptSourceObject*> sso(cx, script->sourceObject());
  return ::FinishCollectingDelazifications(cx, sso, stencilOut);
}

JS_PUBLIC_API bool JS::FinishCollectingDelazifications(
    JSContext* cx, JS::Handle<JSObject*> module, JS::TranscodeBuffer& buffer) {
  JS::Rooted<ScriptSourceObject*> sso(
      cx, module->as<ModuleObject>().scriptSourceObject());
  return ::FinishCollectingDelazifications(cx, sso, buffer);
}

JS_PUBLIC_API void JS::AbortCollectingDelazifications(JS::HandleScript script) {
  if (!script) {
    return;
  }
  script->sourceObject()->unsetCollectingDelazifications();
}

JS_PUBLIC_API void JS::AbortCollectingDelazifications(
    JS::Handle<JSObject*> module) {
  module->as<ModuleObject>()
      .scriptSourceObject()
      ->unsetCollectingDelazifications();
}

JSScript* JS::CompileUtf8File(JSContext* cx,
                              const ReadOnlyCompileOptions& options,
                              FILE* file) {
  FileContents buffer(cx);
  if (!ReadCompleteFile(cx, file, buffer)) {
    return nullptr;
  }

  SourceText<Utf8Unit> srcBuf;
  if (!srcBuf.init(cx, reinterpret_cast<const char*>(buffer.begin()),
                   buffer.length(), SourceOwnership::Borrowed)) {
    return nullptr;
  }

  return CompileSourceBuffer(cx, options, srcBuf);
}

JSScript* JS::CompileUtf8Path(JSContext* cx,
                              const ReadOnlyCompileOptions& optionsArg,
                              const char* filename) {
  AutoFile file;
  if (!file.open(cx, filename)) {
    return nullptr;
  }

  CompileOptions options(cx, optionsArg);
  options.setFileAndLine(filename, 1);
  return CompileUtf8File(cx, options, file.fp());
}

JS_PUBLIC_API bool JS_Utf8BufferIsCompilableUnit(JSContext* cx,
                                                 HandleObject obj,
                                                 const char* utf8,
                                                 size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  cx->clearPendingException();

  JS::UniqueTwoByteChars chars{
      UTF8CharsToNewTwoByteCharsZ(cx, UTF8Chars(utf8, length), &length,
                                  js::MallocArena)
          .get()};
  if (!chars) {
    return true;
  }

  using frontend::FullParseHandler;
  using frontend::ParseGoal;
  using frontend::Parser;

  AutoReportFrontendContext fc(cx,
                               AutoReportFrontendContext::Warning::Suppress);
  CompileOptions options(cx);
  Rooted<frontend::CompilationInput> input(cx,
                                           frontend::CompilationInput(options));
  if (!input.get().initForGlobal(&fc)) {
    return false;
  }

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  js::frontend::NoScopeBindingCache scopeCache;
  frontend::CompilationState compilationState(&fc, allocScope, input.get());
  if (!compilationState.init(&fc, &scopeCache)) {
    return false;
  }

  // Warnings and errors during parsing shouldn't be reported.
  fc.clearAutoReport();

  Parser<FullParseHandler, char16_t> parser(&fc, options, chars.get(), length,
                                            compilationState,
                                            /* syntaxParser = */ nullptr);
  if (!parser.checkOptions() || parser.parse().isErr()) {
    // We ran into an error. If it was because we ran out of source, we
    // return false so our caller knows to try to collect more buffered
    // source.
    if (parser.isUnexpectedEOF()) {
      return false;
    }
  }

  // Return true on any out-of-memory error or non-EOF-related syntax error, so
  // our caller doesn't try to collect more buffered source.
  return true;
}

class FunctionCompiler {
 private:
  JSContext* const cx_;
  Rooted<JSAtom*> nameAtom_;
  StringBuilder funStr_;

  uint32_t parameterListEnd_ = 0;
  bool nameIsIdentifier_ = true;

 public:
  explicit FunctionCompiler(JSContext* cx, FrontendContext* fc)
      : cx_(cx), nameAtom_(cx), funStr_(fc) {
    AssertHeapIsIdle();
    CHECK_THREAD(cx);
    MOZ_ASSERT(!cx->zone()->isAtomsZone());
  }

  [[nodiscard]] bool init(const char* name, unsigned nargs,
                          const char* const* argnames) {
    if (!funStr_.ensureTwoByteChars()) {
      return false;
    }
    if (!funStr_.append("function ")) {
      return false;
    }

    if (name) {
      size_t nameLen = strlen(name);

      nameAtom_ = Atomize(cx_, name, nameLen);
      if (!nameAtom_) {
        return false;
      }

      // If the name is an identifier, we can just add it to source text.
      // Otherwise we'll have to set it manually later.
      nameIsIdentifier_ =
          IsIdentifier(reinterpret_cast<const Latin1Char*>(name), nameLen);
      if (nameIsIdentifier_) {
        if (!funStr_.append(nameAtom_)) {
          return false;
        }
      }
    }

    if (!funStr_.append("(")) {
      return false;
    }

    for (unsigned i = 0; i < nargs; i++) {
      if (i != 0) {
        if (!funStr_.append(", ")) {
          return false;
        }
      }
      if (!funStr_.append(argnames[i], strlen(argnames[i]))) {
        return false;
      }
    }

    // Remember the position of ")".
    parameterListEnd_ = funStr_.length();
    static_assert(FunctionConstructorMedialSigils[0] == ')');

    return funStr_.append(FunctionConstructorMedialSigils.data(),
                          FunctionConstructorMedialSigils.length());
  }

  template <typename Unit>
  [[nodiscard]] inline bool addFunctionBody(const SourceText<Unit>& srcBuf) {
    return funStr_.append(srcBuf.get(), srcBuf.length());
  }

  JSFunction* finish(const JS::EnvironmentChain& envChain,
                     const ReadOnlyCompileOptions& optionsArg) {
    using js::frontend::FunctionSyntaxKind;

    if (!funStr_.append(FunctionConstructorFinalBrace.data(),
                        FunctionConstructorFinalBrace.length())) {
      return nullptr;
    }

    size_t newLen = funStr_.length();
    UniqueTwoByteChars stolen(funStr_.stealChars());
    if (!stolen) {
      return nullptr;
    }

    SourceText<char16_t> newSrcBuf;
    if (!newSrcBuf.init(cx_, std::move(stolen), newLen)) {
      return nullptr;
    }

    RootedObject enclosingEnv(cx_);
    ScopeKind kind;
    if (envChain.empty()) {
      // A compiled function has a burned-in environment chain, so if no exotic
      // environment was requested, we can use the global lexical environment
      // directly and not need to worry about any potential non-syntactic scope.
      enclosingEnv = &cx_->global()->lexicalEnvironment();
      kind = ScopeKind::Global;
    } else {
      enclosingEnv = CreateNonSyntacticEnvironmentChain(cx_, envChain);
      if (!enclosingEnv) {
        return nullptr;
      }
      kind = ScopeKind::NonSyntactic;
    }

    cx_->check(enclosingEnv);

    // Make sure the static scope chain matches up when we have a
    // non-syntactic scope.
    MOZ_ASSERT_IF(!enclosingEnv->is<GlobalLexicalEnvironmentObject>(),
                  kind == ScopeKind::NonSyntactic);

    CompileOptions options(cx_, optionsArg);
    options.setNonSyntacticScope(kind == ScopeKind::NonSyntactic);

    FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Statement;
    RootedFunction fun(cx_);
    if (kind == ScopeKind::NonSyntactic) {
      Rooted<Scope*> enclosingScope(
          cx_, GlobalScope::createEmpty(cx_, ScopeKind::NonSyntactic));
      if (!enclosingScope) {
        return nullptr;
      }

      fun = js::frontend::CompileStandaloneFunctionInNonSyntacticScope(
          cx_, options, newSrcBuf, mozilla::Some(parameterListEnd_), syntaxKind,
          enclosingScope);
    } else {
      fun = js::frontend::CompileStandaloneFunction(
          cx_, options, newSrcBuf, mozilla::Some(parameterListEnd_),
          syntaxKind);
    }
    if (!fun) {
      return nullptr;
    }

    // When the function name isn't a valid identifier, the generated function
    // source in srcBuf won't include the name, so name the function manually.
    if (!nameIsIdentifier_) {
      fun->setAtom(nameAtom_);
    }

    if (fun->isInterpreted()) {
      fun->initEnvironment(enclosingEnv);
    }

    return fun;
  }
};

JS_PUBLIC_API JSFunction* JS::CompileFunction(
    JSContext* cx, const EnvironmentChain& envChain,
    const ReadOnlyCompileOptions& options, const char* name, unsigned nargs,
    const char* const* argnames, SourceText<char16_t>& srcBuf) {
  ManualReportFrontendContext fc(cx);
  FunctionCompiler compiler(cx, &fc);
  if (!compiler.init(name, nargs, argnames) ||
      !compiler.addFunctionBody(srcBuf)) {
    fc.failure();
    return nullptr;
  }

  fc.ok();
  return compiler.finish(envChain, options);
}

JS_PUBLIC_API JSFunction* JS::CompileFunction(
    JSContext* cx, const EnvironmentChain& envChain,
    const ReadOnlyCompileOptions& options, const char* name, unsigned nargs,
    const char* const* argnames, SourceText<Utf8Unit>& srcBuf) {
  ManualReportFrontendContext fc(cx);
  FunctionCompiler compiler(cx, &fc);
  if (!compiler.init(name, nargs, argnames) ||
      !compiler.addFunctionBody(srcBuf)) {
    fc.failure();
    return nullptr;
  }

  fc.ok();
  return compiler.finish(envChain, options);
}

JS_PUBLIC_API JSFunction* JS::CompileFunctionUtf8(
    JSContext* cx, const EnvironmentChain& envChain,
    const ReadOnlyCompileOptions& options, const char* name, unsigned nargs,
    const char* const* argnames, const char* bytes, size_t length) {
  SourceText<Utf8Unit> srcBuf;
  if (!srcBuf.init(cx, bytes, length, SourceOwnership::Borrowed)) {
    return nullptr;
  }

  return CompileFunction(cx, envChain, options, name, nargs, argnames, srcBuf);
}

JS_PUBLIC_API void JS::ExposeScriptToDebugger(JSContext* cx,
                                              HandleScript script) {
  MOZ_ASSERT(cx);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));

  DebugAPI::onNewScript(cx, script);
}

JS_PUBLIC_API bool JS::UpdateDebugMetadata(
    JSContext* cx, Handle<JSScript*> script, const InstantiateOptions& options,
    HandleValue privateValue, HandleString elementAttributeName,
    HandleScript introScript, HandleScript scriptOrModule) {
  Rooted<ScriptSourceObject*> sso(cx, script->sourceObject());

  if (!ScriptSourceObject::initElementProperties(cx, sso,
                                                 elementAttributeName)) {
    return false;
  }

  // There is no equivalent of cross-compartment wrappers for scripts. If the
  // introduction script and ScriptSourceObject are in different compartments,
  // we would be creating a cross-compartment script reference, which is
  // forbidden. We can still store a CCW to the script source object though.
  RootedValue introductionScript(cx);
  if (introScript) {
    if (introScript->compartment() == cx->compartment()) {
      introductionScript.setPrivateGCThing(introScript);
    }
  }
  sso->setIntroductionScript(introductionScript);

  RootedValue privateValueStore(cx, UndefinedValue());
  if (privateValue.isUndefined()) {
    // Set the private value to that of the script or module that this source is
    // part of, if any.
    if (scriptOrModule) {
      privateValueStore = scriptOrModule->sourceObject()->getPrivate();
    }
  } else {
    privateValueStore = privateValue;
  }

  if (!privateValueStore.isUndefined()) {
    if (!JS_WrapValue(cx, &privateValueStore)) {
      return false;
    }
  }
  sso->setPrivate(cx->runtime(), privateValueStore);

  if (!options.hideScriptFromDebugger) {
    JS::ExposeScriptToDebugger(cx, script);
  }

  return true;
}

MOZ_NEVER_INLINE static bool ExecuteScript(JSContext* cx, HandleObject envChain,
                                           HandleScript script,
                                           MutableHandleValue rval) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(envChain, script);

  if (!envChain->is<GlobalLexicalEnvironmentObject>()) {
    MOZ_RELEASE_ASSERT(script->hasNonSyntacticScope());
  }

  return Execute(cx, script, envChain, rval);
}

static bool ExecuteScript(JSContext* cx, const JS::EnvironmentChain& envChain,
                          HandleScript script, MutableHandleValue rval) {
  RootedObject env(cx, CreateNonSyntacticEnvironmentChain(cx, envChain));
  if (!env) {
    return false;
  }

  return ExecuteScript(cx, env, script, rval);
}

MOZ_NEVER_INLINE JS_PUBLIC_API bool JS_ExecuteScript(JSContext* cx,
                                                     HandleScript scriptArg,
                                                     MutableHandleValue rval) {
  RootedObject globalLexical(cx, &cx->global()->lexicalEnvironment());
  return ExecuteScript(cx, globalLexical, scriptArg, rval);
}

MOZ_NEVER_INLINE JS_PUBLIC_API bool JS_ExecuteScript(JSContext* cx,
                                                     HandleScript scriptArg) {
  RootedObject globalLexical(cx, &cx->global()->lexicalEnvironment());
  RootedValue rval(cx);
  return ExecuteScript(cx, globalLexical, scriptArg, &rval);
}

MOZ_NEVER_INLINE JS_PUBLIC_API bool JS_ExecuteScript(
    JSContext* cx, const JS::EnvironmentChain& envChain, HandleScript scriptArg,
    MutableHandleValue rval) {
  return ExecuteScript(cx, envChain, scriptArg, rval);
}

MOZ_NEVER_INLINE JS_PUBLIC_API bool JS_ExecuteScript(
    JSContext* cx, const JS::EnvironmentChain& envChain,
    HandleScript scriptArg) {
  RootedValue rval(cx);
  return ExecuteScript(cx, envChain, scriptArg, &rval);
}

template <typename Unit>
static bool EvaluateSourceBuffer(JSContext* cx, ScopeKind scopeKind,
                                 Handle<JSObject*> env,
                                 const ReadOnlyCompileOptions& optionsArg,
                                 SourceText<Unit>& srcBuf,
                                 MutableHandle<Value> rval) {
  CompileOptions options(cx, optionsArg);
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(env);
  MOZ_ASSERT_IF(!env->is<GlobalLexicalEnvironmentObject>(),
                scopeKind == ScopeKind::NonSyntactic);

  options.setNonSyntacticScope(scopeKind == ScopeKind::NonSyntactic);
  options.setIsRunOnce(true);

  AutoReportFrontendContext fc(cx);
  RootedScript script(
      cx, frontend::CompileGlobalScript(cx, &fc, options, srcBuf, scopeKind));
  if (!script) {
    return false;
  }

  return Execute(cx, script, env, rval);
}

JS_PUBLIC_API bool JS::Evaluate(JSContext* cx,
                                const ReadOnlyCompileOptions& options,
                                SourceText<Utf8Unit>& srcBuf,
                                MutableHandle<Value> rval) {
  RootedObject globalLexical(cx, &cx->global()->lexicalEnvironment());
  return EvaluateSourceBuffer(cx, ScopeKind::Global, globalLexical, options,
                              srcBuf, rval);
}

JS_PUBLIC_API bool JS::Evaluate(JSContext* cx,
                                const ReadOnlyCompileOptions& optionsArg,
                                SourceText<char16_t>& srcBuf,
                                MutableHandleValue rval) {
  RootedObject globalLexical(cx, &cx->global()->lexicalEnvironment());
  return EvaluateSourceBuffer(cx, ScopeKind::Global, globalLexical, optionsArg,
                              srcBuf, rval);
}

JS_PUBLIC_API bool JS::Evaluate(JSContext* cx, const EnvironmentChain& envChain,
                                const ReadOnlyCompileOptions& options,
                                SourceText<char16_t>& srcBuf,
                                MutableHandleValue rval) {
  RootedObject env(cx, CreateNonSyntacticEnvironmentChain(cx, envChain));
  if (!env) {
    return false;
  }

  return EvaluateSourceBuffer(cx, ScopeKind::NonSyntactic, env, options, srcBuf,
                              rval);
}

JS_PUBLIC_API bool JS::EvaluateUtf8Path(
    JSContext* cx, const ReadOnlyCompileOptions& optionsArg,
    const char* filename, MutableHandleValue rval) {
  FileContents buffer(cx);
  {
    AutoFile file;
    if (!file.open(cx, filename) || !file.readAll(cx, buffer)) {
      return false;
    }
  }

  CompileOptions options(cx, optionsArg);
  options.setFileAndLine(filename, 1);

  auto contents = reinterpret_cast<const char*>(buffer.begin());
  size_t length = buffer.length();

  JS::SourceText<Utf8Unit> srcBuf;
  if (!srcBuf.init(cx, contents, length, JS::SourceOwnership::Borrowed)) {
    return false;
  }

  return Evaluate(cx, options, srcBuf, rval);
}
