/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeCompiler_h
#define frontend_BytecodeCompiler_h

#include "mozilla/AlreadyAddRefed.h"  // already_AddRefed
#include "mozilla/Maybe.h"            // mozilla::Maybe
#include "mozilla/Utf8.h"             // mozilla::Utf8Unit

#include <stdint.h>  // uint32_t

#include "ds/LifoAlloc.h"                 // js::LifoAlloc
#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/ScriptIndex.h"         // ScriptIndex
#include "js/CompileOptions.h"  // JS::ReadOnlyCompileOptions, JS::PrefableCompileOptions
#include "js/GCVector.h"    // JS::StackGCVector
#include "js/Id.h"          // JS::PropertyKey
#include "js/RootingAPI.h"  // JS::Handle
#include "js/SourceText.h"  // JS::SourceText
#include "js/UniquePtr.h"   // js::UniquePtr
#include "js/Value.h"       // JS::Value
#include "vm/ScopeKind.h"   // js::ScopeKind

/*
 * Structure of all of the support classes.
 *
 * Parser: described in Parser.h.
 *
 * BytecodeCompiler.cpp: BytecodeCompiler.h
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

class JSFunction;
class JSObject;
class JSScript;
struct JSContext;

namespace js {

class ModuleObject;
class FrontendContext;
class Scope;

namespace frontend {

struct CompilationInput;
struct CompilationStencil;
struct ExtensibleCompilationStencil;
struct CompilationGCOutput;
class ScopeBindingCache;

// Compile a script of the given source using the given options.
extern already_AddRefed<CompilationStencil> CompileGlobalScriptToStencil(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<char16_t>& srcBuf, ScopeKind scopeKind);

extern already_AddRefed<CompilationStencil> CompileGlobalScriptToStencil(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf, ScopeKind scopeKind);

extern UniquePtr<ExtensibleCompilationStencil>
CompileGlobalScriptToExtensibleStencil(JSContext* maybeCx, FrontendContext* fc,
                                       CompilationInput& input,
                                       ScopeBindingCache* scopeCache,
                                       JS::SourceText<char16_t>& srcBuf,
                                       ScopeKind scopeKind);

extern UniquePtr<ExtensibleCompilationStencil>
CompileGlobalScriptToExtensibleStencil(
    JSContext* maybeCx, FrontendContext* fc, CompilationInput& input,
    ScopeBindingCache* scopeCache, JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    ScopeKind scopeKind);

[[nodiscard]] extern bool InstantiateStencils(JSContext* cx,
                                              CompilationInput& input,
                                              const CompilationStencil& stencil,
                                              CompilationGCOutput& gcOutput);

// Perform CompileGlobalScriptToStencil and InstantiateStencils at the
// same time, skipping some extra copy.
extern JSScript* CompileGlobalScript(JSContext* cx, FrontendContext* fc,
                                     const JS::ReadOnlyCompileOptions& options,
                                     JS::SourceText<char16_t>& srcBuf,
                                     ScopeKind scopeKind);

extern JSScript* CompileGlobalScript(JSContext* cx, FrontendContext* fc,
                                     const JS::ReadOnlyCompileOptions& options,
                                     JS::SourceText<mozilla::Utf8Unit>& srcBuf,
                                     ScopeKind scopeKind);

// Compile a script with a list of known extra bindings.
//
// Bindings should be passed by a pair of unwrappedBindingKeys and
// unwrappedBindingValues.
//
// If any of the bindings are accessed by the script, a WithEnvironmentObject
// is created for the bindings and returned via env out parameter. Otherwise,
// global lexical is returned. In both case, the same env must be used to
// evaluate the script.
//
// Both unwrappedBindingKeys and unwrappedBindingValues can come from different
// realm than the current realm.
//
// If a binding is shadowed by the global variables declared by the script,
// or the existing global variables, the binding is not stored into the
// resulting WithEnvironmentObject.
extern JSScript* CompileGlobalScriptWithExtraBindings(
    JSContext* cx, FrontendContext* fc,
    const JS::ReadOnlyCompileOptions& options, JS::SourceText<char16_t>& srcBuf,
    JS::Handle<JS::StackGCVector<JS::PropertyKey>> unwrappedBindingKeys,
    JS::Handle<JS::StackGCVector<JS::Value>> unwrappedBindingValues,
    JS::MutableHandle<JSObject*> env);

// Compile a script for eval of the given source using the given options and
// enclosing scope/environment.
extern JSScript* CompileEvalScript(JSContext* cx,
                                   const JS::ReadOnlyCompileOptions& options,
                                   JS::SourceText<char16_t>& srcBuf,
                                   JS::Handle<js::Scope*> enclosingScope,
                                   JS::Handle<JSObject*> enclosingEnv);

// Compile a module of the given source using the given options.
ModuleObject* CompileModule(JSContext* cx, FrontendContext* fc,
                            const JS::ReadOnlyCompileOptions& options,
                            JS::SourceText<char16_t>& srcBuf);
ModuleObject* CompileModule(JSContext* cx, FrontendContext* fc,
                            const JS::ReadOnlyCompileOptions& options,
                            JS::SourceText<mozilla::Utf8Unit>& srcBuf);

// Parse a module of the given source.  This is an internal API; if you want to
// compile a module as a user, use CompileModule above.
already_AddRefed<CompilationStencil> ParseModuleToStencil(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<char16_t>& srcBuf);
already_AddRefed<CompilationStencil> ParseModuleToStencil(
    JSContext* maybeCx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf);

UniquePtr<ExtensibleCompilationStencil> ParseModuleToExtensibleStencil(
    JSContext* cx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
    JS::SourceText<char16_t>& srcBuf);
UniquePtr<ExtensibleCompilationStencil> ParseModuleToExtensibleStencil(
    JSContext* cx, FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    CompilationInput& input, ScopeBindingCache* scopeCache,
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
    frontend::FunctionSyntaxKind syntaxKind, JS::Handle<Scope*> enclosingScope);

extern bool DelazifyCanonicalScriptedFunction(JSContext* cx,
                                              FrontendContext* fc,
                                              JS::Handle<JSFunction*> fun);

enum class DelazifyFailureReason {
  Compressed,
  Other,
};

extern already_AddRefed<CompilationStencil> DelazifyCanonicalScriptedFunction(
    FrontendContext* fc, js::LifoAlloc& tempLifoAlloc,
    const JS::PrefableCompileOptions& prefableOptions,
    ScopeBindingCache* scopeCache, CompilationStencil& context,
    ScriptIndex scriptIndex, DelazifyFailureReason* failureReason);

// Certain compile options will disable the syntax parser entirely.
inline bool CanLazilyParse(const JS::ReadOnlyCompileOptions& options) {
  return !options.discardSource && !options.sourceIsLazy &&
         !options.forceFullParse();
}

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_BytecodeCompiler_h */
