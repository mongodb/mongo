/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeCompiler_h
#define frontend_BytecodeCompiler_h

#include "mozilla/Maybe.h"
#include "mozilla/Utf8.h"  // mozilla::Utf8Unit

#include "NamespaceImports.h"

#include "frontend/FunctionSyntaxKind.h"
#include "js/CompileOptions.h"  // JS::ReadOnlyCompileOptions
#include "js/SourceText.h"
#include "js/UniquePtr.h"  // js::UniquePtr
#include "vm/Scope.h"
#include "vm/TraceLogging.h"

/*
 * Structure of all of the support classes.
 *
 * Parser: described in Parser.h.
 *
 * BytecodeCompiler.cpp: BytecodeCompiler.h *and* BytecodeCompilation.h.
 * This is the "driver", the high-level operations like "compile this source to
 * bytecode". It calls the parser, bytecode emitter, etc.
 *
 * ParseContext.h and SharedContext.h: Both have similar purposes. They're split
 * because ParseContext contains information used only by the parser, and
 * SharedContext contains information used by both the parser and
 * BytecodeEmitter.
 *
 * SharedContext.h: class Directives: this contains boolean flags for tracking
 * if we're in asm.js or "use strict" code. The "use strict" bit is stored in
 * SharedContext, and additionally, the full Directives class is stored in
 * ParseContext - if a direcive is encountered while parsing, this is updated,
 * and checked in GeneralParser::functionDefinition, and if it changed, the
 * whole function is re-parsed with the new flags.
 *
 * SharedContext.h: abstract class SharedContext: This class contains two
 * different groups of flags:
 *
 * Parse context information. This is information conceptually "passed down"
 * into parsing sub-nodes. This is like "are we parsing strict code?", and so
 * the parser can make decisions of how to parse based off that.
 *
 * Gathered-while-parsing information. This is information conceptually
 * "returned up" from parsing sub-nodes. This is like "did we see a use strict
 * directive"?
 *
 * Additionally, subclasses (GlobalSharedContext, ModuleSharedContext,
 * EvalSharedContext, and FunctionBox) contain binding information, scope
 * information, and other such bits of data.
 *
 * ParseContext.h: class UsedNameTracker: Track which bindings are used in which
 * scopes. This helps determine which bindings are closed-over, which affects
 * how they're stored; and whether special bindings like `this` and `arguments`
 * can be optimized away.
 *
 * ParseContext.h: class ParseContext: Extremely complex class that serves a lot
 * of purposes, but it's a single class - essentially no derived classes - so
 * it's a little easier to comprehend all at once. (SourceParseContext does
 * derive from ParseContext, but they does nothing except adjust the
 * constructor's arguments).
 * Note it uses a thing called Nestable, which implements a stack of objects:
 * you can push (and pop) instances to a stack (linked list) as you parse
 * further into the parse tree. You may push to this stack via calling the
 * constructor with a GeneralParser as an argument (usually `this`), which
 * pushes itself onto `this->pc` (so it does get assigned/pushed, even though no
 * assignment ever appears directly in the parser)
 *
 * ParseContext contains a pointer to a SharedContext.
 *
 * There's a decent chunk of flags/data collection in here too, some "pass-down"
 * data and some "return-up" data.
 *
 * ParseContext also contains a significant number of *sub*-Nestables as fields
 * of itself (nestables inside nestables). Note you also push/pop to these via
 * passing `Parser->pc`, which the constructor of the sub-nestable knows which
 * ParseContext field to push to. The sub-nestables are:
 *
 * ParseContext::Statement: stack of statements.
 * `if (x) { while (true) { try { ..stack of [if, while, try].. } ... } }`
 *
 * ParseContext::LabelStatement: interspersed in Statement stack, for labeled
 * statements, for e.g. `label: while (true) { break label; }`
 *
 * ParseContext::ClassStatement: interspersed in Statement stack, for classes
 * the parser is currently inside of.
 *
 * ParseContext::Scope: Set of variables in each scope (stack of sets):
 * `{ let a; let b; { let c; } }`
 * (this gets complicated with `var`, etc., check the class for docs)
 */

class JSLinearString;

namespace js {

class ModuleObject;
class ScriptSourceObject;

namespace frontend {

struct CompilationStencil;
struct ExtensibleCompilationStencil;
struct CompilationGCOutput;
class ErrorReporter;
class FunctionBox;
class ParseNode;

// Compile a module of the given source using the given options.
ModuleObject* CompileModule(JSContext* cx,
                            const JS::ReadOnlyCompileOptions& options,
                            JS::SourceText<char16_t>& srcBuf);
ModuleObject* CompileModule(JSContext* cx,
                            const JS::ReadOnlyCompileOptions& options,
                            JS::SourceText<mozilla::Utf8Unit>& srcBuf);

// Parse a module of the given source.  This is an internal API; if you want to
// compile a module as a user, use CompileModule above.
UniquePtr<CompilationStencil> ParseModuleToStencil(
    JSContext* cx, CompilationInput& input, JS::SourceText<char16_t>& srcBuf);
UniquePtr<CompilationStencil> ParseModuleToStencil(
    JSContext* cx, CompilationInput& input,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf);

UniquePtr<ExtensibleCompilationStencil> ParseModuleToExtensibleStencil(
    JSContext* cx, CompilationInput& input, JS::SourceText<char16_t>& srcBuf);
UniquePtr<ExtensibleCompilationStencil> ParseModuleToExtensibleStencil(
    JSContext* cx, CompilationInput& input,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf);

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
[[nodiscard]] JSFunction* CompileStandaloneFunction(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind);

[[nodiscard]] JSFunction* CompileStandaloneGenerator(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind);

[[nodiscard]] JSFunction* CompileStandaloneAsyncFunction(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind);

[[nodiscard]] JSFunction* CompileStandaloneAsyncGenerator(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind);

// Compile a single function in given enclosing non-syntactic scope.
[[nodiscard]] JSFunction* CompileStandaloneFunctionInNonSyntacticScope(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf,
    const mozilla::Maybe<uint32_t>& parameterListEnd,
    frontend::FunctionSyntaxKind syntaxKind, HandleScope enclosingScope);

/*
 * True if str consists of an IdentifierStart character, followed by one or
 * more IdentifierPart characters, i.e. it matches the IdentifierName production
 * in the language spec.
 *
 * This returns true even if str is a keyword like "if".
 *
 * Defined in TokenStream.cpp.
 */
bool IsIdentifier(JSLinearString* str);

bool IsIdentifierNameOrPrivateName(JSLinearString* str);

/*
 * As above, but taking chars + length.
 */
bool IsIdentifier(const Latin1Char* chars, size_t length);
bool IsIdentifier(const char16_t* chars, size_t length);

/*
 * ASCII variant with known length.
 */
bool IsIdentifierASCII(char c);
bool IsIdentifierASCII(char c1, char c2);

bool IsIdentifierNameOrPrivateName(const Latin1Char* chars, size_t length);
bool IsIdentifierNameOrPrivateName(const char16_t* chars, size_t length);

/* True if str is a keyword. Defined in TokenStream.cpp. */
bool IsKeyword(TaggedParserAtomIndex atom);

class MOZ_STACK_CLASS AutoFrontendTraceLog {
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
