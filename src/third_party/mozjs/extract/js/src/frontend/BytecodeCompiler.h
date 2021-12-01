/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeCompiler_h
#define frontend_BytecodeCompiler_h

#include "mozilla/Maybe.h"

#include "NamespaceImports.h"

#include "vm/Scope.h"
#include "vm/StringType.h"
#include "vm/TraceLogging.h"

class JSLinearString;

namespace js {

class LazyScript;
class LifoAlloc;
class ModuleObject;
class ScriptSourceObject;

namespace frontend {

class ErrorReporter;
class FunctionBox;
class ParseNode;

JSScript*
CompileGlobalScript(JSContext* cx, LifoAlloc& alloc, ScopeKind scopeKind,
                    const ReadOnlyCompileOptions& options,
                    SourceBufferHolder& srcBuf,
                    ScriptSourceObject** sourceObjectOut = nullptr);

JSScript*
CompileEvalScript(JSContext* cx, LifoAlloc& alloc,
                  HandleObject scopeChain, HandleScope enclosingScope,
                  const ReadOnlyCompileOptions& options,
                  SourceBufferHolder& srcBuf,
                  ScriptSourceObject** sourceObjectOut = nullptr);

ModuleObject*
CompileModule(JSContext* cx, const ReadOnlyCompileOptions& options,
              SourceBufferHolder& srcBuf);

ModuleObject*
CompileModule(JSContext* cx, const ReadOnlyCompileOptions& options,
              SourceBufferHolder& srcBuf, LifoAlloc& alloc,
              ScriptSourceObject** sourceObjectOut = nullptr);

MOZ_MUST_USE bool
CompileLazyFunction(JSContext* cx, Handle<LazyScript*> lazy, const char16_t* chars, size_t length);

//
// Compile a single function. The source in srcBuf must match the ECMA-262
// FunctionExpression production.
//
// If nonzero, parameterListEnd is the offset within srcBuf where the parameter
// list is expected to end. During parsing, if we find that it ends anywhere
// else, it's a SyntaxError. This is used to implement the Function constructor;
// it's how we detect that these weird cases are SyntaxErrors:
//
//     Function("/*", "*/x) {")
//     Function("x){ if (3", "return x;}")
//
MOZ_MUST_USE bool
CompileStandaloneFunction(JSContext* cx, MutableHandleFunction fun,
                          const ReadOnlyCompileOptions& options,
                          JS::SourceBufferHolder& srcBuf,
                          const mozilla::Maybe<uint32_t>& parameterListEnd,
                          HandleScope enclosingScope = nullptr);

MOZ_MUST_USE bool
CompileStandaloneGenerator(JSContext* cx, MutableHandleFunction fun,
                           const ReadOnlyCompileOptions& options,
                           JS::SourceBufferHolder& srcBuf,
                           const mozilla::Maybe<uint32_t>& parameterListEnd);

MOZ_MUST_USE bool
CompileStandaloneAsyncFunction(JSContext* cx, MutableHandleFunction fun,
                               const ReadOnlyCompileOptions& options,
                               JS::SourceBufferHolder& srcBuf,
                               const mozilla::Maybe<uint32_t>& parameterListEnd);

MOZ_MUST_USE bool
CompileStandaloneAsyncGenerator(JSContext* cx, MutableHandleFunction fun,
                                const ReadOnlyCompileOptions& options,
                                JS::SourceBufferHolder& srcBuf,
                                const mozilla::Maybe<uint32_t>& parameterListEnd);

ScriptSourceObject*
CreateScriptSourceObject(JSContext* cx, const ReadOnlyCompileOptions& options,
                         const mozilla::Maybe<uint32_t>& parameterListEnd = mozilla::Nothing());

/*
 * True if str consists of an IdentifierStart character, followed by one or
 * more IdentifierPart characters, i.e. it matches the IdentifierName production
 * in the language spec.
 *
 * This returns true even if str is a keyword like "if".
 *
 * Defined in TokenStream.cpp.
 */
bool
IsIdentifier(JSLinearString* str);

/*
 * As above, but taking chars + length.
 */
bool
IsIdentifier(const char* chars, size_t length);
bool
IsIdentifier(const char16_t* chars, size_t length);

/* True if str is a keyword. Defined in TokenStream.cpp. */
bool
IsKeyword(JSLinearString* str);

/* Trace all GC things reachable from parser. Defined in Parser.cpp. */
void
TraceParser(JSTracer* trc, JS::AutoGCRooter* parser);

#if defined(JS_BUILD_BINAST)

/* Trace all GC things reachable from binjs parser. Defined in BinSource.cpp. */
void
TraceBinParser(JSTracer* trc, JS::AutoGCRooter* parser);

#endif // defined(JS_BUILD_BINAST)

class MOZ_STACK_CLASS AutoFrontendTraceLog
{
#ifdef JS_TRACE_LOGGING
    TraceLoggerThread* logger_;
    mozilla::Maybe<TraceLoggerEvent> frontendEvent_;
    mozilla::Maybe<AutoTraceLog> frontendLog_;
    mozilla::Maybe<AutoTraceLog> typeLog_;
#endif

  public:
    AutoFrontendTraceLog(JSContext* cx, const TraceLoggerTextId id,
                         const ErrorReporter& reporter);

    AutoFrontendTraceLog(JSContext* cx, const TraceLoggerTextId id,
                         const ErrorReporter& reporter, FunctionBox* funbox);

    AutoFrontendTraceLog(JSContext* cx, const TraceLoggerTextId id,
                         const ErrorReporter& reporter, ParseNode* pn);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_BytecodeCompiler_h */
