/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Same-thread compilation and evaluation APIs. */

#include "js/CompilationAndEvaluation.h"

#include "mozilla/Maybe.h"      // mozilla::None, mozilla::Some
#include "mozilla/TextUtils.h"  // mozilla::IsAscii
#include "mozilla/Utf8.h"       // mozilla::Utf8Unit

#include <utility>  // std::move

#include "jstypes.h"  // JS_PUBLIC_API

#include "frontend/BytecodeCompilation.h"  // frontend::CompileGlobalScript
#include "frontend/CompilationStencil.h"  // for frontened::{CompilationStencil, BorrowingCompilationStencil, CompilationGCOutput}
#include "frontend/FullParseHandler.h"    // frontend::FullParseHandler
#include "frontend/ParseContext.h"        // frontend::UsedNameTracker
#include "frontend/Parser.h"       // frontend::Parser, frontend::ParseGoal
#include "js/CharacterEncoding.h"  // JS::UTF8Chars, JS::UTF8CharsToNewTwoByteCharsZ
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/RootingAPI.h"            // JS::Rooted
#include "js/SourceText.h"            // JS::SourceText
#include "js/TypeDecls.h"          // JS::HandleObject, JS::MutableHandleScript
#include "js/Utility.h"            // js::MallocArena, JS::UniqueTwoByteChars
#include "js/Value.h"              // JS::Value
#include "util/CompleteFile.h"     // js::FileContents, js::ReadCompleteFile
#include "util/StringBuffer.h"     // js::StringBuffer
#include "vm/EnvironmentObject.h"  // js::CreateNonSyntacticEnvironmentChain
#include "vm/FunctionFlags.h"      // js::FunctionFlags
#include "vm/Interpreter.h"        // js::Execute
#include "vm/JSContext.h"          // JSContext

#include "debugger/DebugAPI-inl.h"  // js::DebugAPI
#include "vm/JSContext-inl.h"       // JSContext::check

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

template <typename Unit>
static JSScript* CompileSourceBuffer(JSContext* cx,
                                     const ReadOnlyCompileOptions& options,
                                     SourceText<Unit>& srcBuf) {
  ScopeKind scopeKind =
      options.nonSyntacticScope ? ScopeKind::NonSyntactic : ScopeKind::Global;

  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return frontend::CompileGlobalScript(cx, options, srcBuf, scopeKind);
}

JSScript* JS::Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
                      SourceText<char16_t>& srcBuf) {
  return CompileSourceBuffer(cx, options, srcBuf);
}

JSScript* JS::Compile(JSContext* cx, const ReadOnlyCompileOptions& options,
                      SourceText<Utf8Unit>& srcBuf) {
  return CompileSourceBuffer(cx, options, srcBuf);
}

template <typename Unit>
static JSScript* CompileSourceBufferAndStartIncrementalEncoding(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<Unit>& srcBuf) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  ScopeKind scopeKind =
      options.nonSyntacticScope ? ScopeKind::NonSyntactic : ScopeKind::Global;

  Rooted<frontend::CompilationInput> input(cx,
                                           frontend::CompilationInput(options));
  auto stencil = frontend::CompileGlobalScriptToExtensibleStencil(
      cx, input.get(), srcBuf, scopeKind);
  if (!stencil) {
    return nullptr;
  }

  RootedScript script(cx);
  {
    frontend::BorrowingCompilationStencil borrowingStencil(*stencil);

    Rooted<frontend::CompilationGCOutput> gcOutput(cx);
    if (!frontend::InstantiateStencils(cx, input.get(), borrowingStencil,
                                       gcOutput.get())) {
      return nullptr;
    }

    script = gcOutput.get().script;
    if (!script) {
      return nullptr;
    }
  }

  MOZ_DIAGNOSTIC_ASSERT(options.useStencilXDR);
  if (!script->scriptSource()->startIncrementalEncoding(cx, options,
                                                        std::move(stencil))) {
    return nullptr;
  }

  return script;
}

JSScript* JS::CompileAndStartIncrementalEncoding(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf) {
  return CompileSourceBufferAndStartIncrementalEncoding(cx, options, srcBuf);
}

JSScript* JS::CompileAndStartIncrementalEncoding(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<Utf8Unit>& srcBuf) {
  return CompileSourceBufferAndStartIncrementalEncoding(cx, options, srcBuf);
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

  // Return true on any out-of-memory error or non-EOF-related syntax error, so
  // our caller doesn't try to collect more buffered source.
  bool result = true;

  using frontend::FullParseHandler;
  using frontend::ParseGoal;
  using frontend::Parser;

  CompileOptions options(cx);
  Rooted<frontend::CompilationInput> input(cx,
                                           frontend::CompilationInput(options));
  if (!input.get().initForGlobal(cx)) {
    return false;
  }

  LifoAllocScope allocScope(&cx->tempLifoAlloc());
  frontend::CompilationState compilationState(cx, allocScope, input.get());
  if (!compilationState.init(cx)) {
    return false;
  }

  JS::AutoSuppressWarningReporter suppressWarnings(cx);
  Parser<FullParseHandler, char16_t> parser(cx, options, chars.get(), length,
                                            /* foldConstants = */ true,
                                            compilationState,
                                            /* syntaxParser = */ nullptr);
  if (!parser.checkOptions() || !parser.parse()) {
    // We ran into an error. If it was because we ran out of source, we
    // return false so our caller knows to try to collect more buffered
    // source.
    if (parser.isUnexpectedEOF()) {
      result = false;
    }

    cx->clearPendingException();
  }

  return result;
}

class FunctionCompiler {
 private:
  JSContext* const cx_;
  RootedAtom nameAtom_;
  StringBuffer funStr_;

  uint32_t parameterListEnd_ = 0;
  bool nameIsIdentifier_ = true;

 public:
  explicit FunctionCompiler(JSContext* cx)
      : cx_(cx), nameAtom_(cx), funStr_(cx) {
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
      nameIsIdentifier_ = js::frontend::IsIdentifier(
          reinterpret_cast<const Latin1Char*>(name), nameLen);
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
    MOZ_ASSERT(FunctionConstructorMedialSigils[0] == ')');

    return funStr_.append(FunctionConstructorMedialSigils);
  }

  template <typename Unit>
  [[nodiscard]] inline bool addFunctionBody(const SourceText<Unit>& srcBuf) {
    return funStr_.append(srcBuf.get(), srcBuf.length());
  }

  JSFunction* finish(HandleObjectVector envChain,
                     const ReadOnlyCompileOptions& optionsArg) {
    using js::frontend::FunctionSyntaxKind;

    if (!funStr_.append(FunctionConstructorFinalBrace)) {
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
      enclosingEnv.set(&cx_->global()->lexicalEnvironment());
      kind = ScopeKind::Global;
    } else {
      if (!CreateNonSyntacticEnvironmentChain(cx_, envChain, &enclosingEnv)) {
        return nullptr;
      }
      kind = ScopeKind::NonSyntactic;
    }

    cx_->check(enclosingEnv);

    // Make sure the static scope chain matches up when we have a
    // non-syntactic scope.
    MOZ_ASSERT_IF(!IsGlobalLexicalEnvironment(enclosingEnv),
                  kind == ScopeKind::NonSyntactic);

    CompileOptions options(cx_, optionsArg);
    options.setNonSyntacticScope(kind == ScopeKind::NonSyntactic);

    FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Statement;
    RootedFunction fun(cx_);
    if (kind == ScopeKind::NonSyntactic) {
      RootedScope enclosingScope(
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
    JSContext* cx, HandleObjectVector envChain,
    const ReadOnlyCompileOptions& options, const char* name, unsigned nargs,
    const char* const* argnames, SourceText<char16_t>& srcBuf) {
  FunctionCompiler compiler(cx);
  if (!compiler.init(name, nargs, argnames) ||
      !compiler.addFunctionBody(srcBuf)) {
    return nullptr;
  }

  return compiler.finish(envChain, options);
}

JS_PUBLIC_API JSFunction* JS::CompileFunction(
    JSContext* cx, HandleObjectVector envChain,
    const ReadOnlyCompileOptions& options, const char* name, unsigned nargs,
    const char* const* argnames, SourceText<Utf8Unit>& srcBuf) {
  FunctionCompiler compiler(cx);
  if (!compiler.init(name, nargs, argnames) ||
      !compiler.addFunctionBody(srcBuf)) {
    return nullptr;
  }

  return compiler.finish(envChain, options);
}

JS_PUBLIC_API JSFunction* JS::CompileFunctionUtf8(
    JSContext* cx, HandleObjectVector envChain,
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
    JSContext* cx, Handle<JSScript*> script,
    const ReadOnlyCompileOptions& options, HandleValue privateValue,
    HandleString elementAttributeName, HandleScript introScript,
    HandleScript scriptOrModule) {
  RootedScriptSourceObject sso(cx, script->sourceObject());

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
      privateValueStore = scriptOrModule->sourceObject()->canonicalPrivate();
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

JS_PUBLIC_API void JS::SetSourceElementCallback(
    JSContext* cx, JSSourceElementCallback callback) {
  MOZ_ASSERT(cx->runtime());
  cx->runtime()->setSourceElementCallback(cx->runtime(), callback);
}

MOZ_NEVER_INLINE static bool ExecuteScript(JSContext* cx, HandleObject envChain,
                                           HandleScript script,
                                           MutableHandleValue rval) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(envChain, script);

  if (!IsGlobalLexicalEnvironment(envChain)) {
    MOZ_RELEASE_ASSERT(script->hasNonSyntacticScope());
  }

  return Execute(cx, script, envChain, rval);
}

static bool ExecuteScript(JSContext* cx, HandleObjectVector envChain,
                          HandleScript script, MutableHandleValue rval) {
  RootedObject env(cx);
  if (!CreateNonSyntacticEnvironmentChain(cx, envChain, &env)) {
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
    JSContext* cx, HandleObjectVector envChain, HandleScript scriptArg,
    MutableHandleValue rval) {
  return ExecuteScript(cx, envChain, scriptArg, rval);
}

MOZ_NEVER_INLINE JS_PUBLIC_API bool JS_ExecuteScript(
    JSContext* cx, HandleObjectVector envChain, HandleScript scriptArg) {
  RootedValue rval(cx);
  return ExecuteScript(cx, envChain, scriptArg, &rval);
}

JS_PUBLIC_API bool JS::CloneAndExecuteScript(JSContext* cx,
                                             HandleScript scriptArg,
                                             JS::MutableHandleValue rval) {
  CHECK_THREAD(cx);
  RootedScript script(cx, scriptArg);
  RootedObject globalLexical(cx, &cx->global()->lexicalEnvironment());
  if (script->realm() != cx->realm()) {
    script = CloneGlobalScript(cx, script);
    if (!script) {
      return false;
    }
  }
  return ExecuteScript(cx, globalLexical, script, rval);
}

JS_PUBLIC_API bool JS::CloneAndExecuteScript(JSContext* cx,
                                             JS::HandleObjectVector envChain,
                                             HandleScript scriptArg,
                                             JS::MutableHandleValue rval) {
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(scriptArg->hasNonSyntacticScope());
  RootedScript script(cx, scriptArg);
  if (script->realm() != cx->realm()) {
    script = CloneGlobalScript(cx, script);
    if (!script) {
      return false;
    }
  }
  return ExecuteScript(cx, envChain, script, rval);
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
  MOZ_ASSERT_IF(!IsGlobalLexicalEnvironment(env),
                scopeKind == ScopeKind::NonSyntactic);

  options.setNonSyntacticScope(scopeKind == ScopeKind::NonSyntactic);
  options.setIsRunOnce(true);

  RootedScript script(
      cx, frontend::CompileGlobalScript(cx, options, srcBuf, scopeKind));
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

JS_PUBLIC_API bool JS::Evaluate(JSContext* cx, HandleObjectVector envChain,
                                const ReadOnlyCompileOptions& options,
                                SourceText<char16_t>& srcBuf,
                                MutableHandleValue rval) {
  RootedObject env(cx);
  if (!CreateNonSyntacticEnvironmentChain(cx, envChain, &env)) {
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
