/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeCompiler_h
#define frontend_BytecodeCompiler_h

#include "NamespaceImports.h"

#include "vm/String.h"

class JSLinearString;

namespace js {

class LazyScript;
class LifoAlloc;
class ModuleObject;
class ScriptSourceObject;
class ScopeObject;
struct SourceCompressionTask;

namespace frontend {

JSScript*
CompileScript(ExclusiveContext* cx, LifoAlloc* alloc,
              HandleObject scopeChain, Handle<ScopeObject*> enclosingStaticScope,
              HandleScript evalCaller, const ReadOnlyCompileOptions& options,
              SourceBufferHolder& srcBuf, JSString* source_ = nullptr,
              SourceCompressionTask* extraSct = nullptr,
              ScriptSourceObject** sourceObjectOut = nullptr);

ModuleObject *
CompileModule(JSContext *cx, HandleObject obj, const ReadOnlyCompileOptions &options,
              SourceBufferHolder &srcBuf);

bool
CompileLazyFunction(JSContext* cx, Handle<LazyScript*> lazy, const char16_t* chars, size_t length);

/*
 * enclosingStaticScope is a static enclosing scope (e.g. a StaticWithObject).
 * Must be null if the enclosing scope is a global.
 */
bool
CompileFunctionBody(JSContext* cx, MutableHandleFunction fun,
                    const ReadOnlyCompileOptions& options,
                    Handle<PropertyNameVector> formals, JS::SourceBufferHolder& srcBuf,
                    Handle<ScopeObject*> enclosingStaticScope);

// As above, but defaults to the global lexical scope as the enclosing static
// scope.
bool
CompileFunctionBody(JSContext* cx, MutableHandleFunction fun,
                    const ReadOnlyCompileOptions& options,
                    Handle<PropertyNameVector> formals, JS::SourceBufferHolder& srcBuf);

bool
CompileStarGeneratorBody(JSContext* cx, MutableHandleFunction fun,
                         const ReadOnlyCompileOptions& options,
                         Handle<PropertyNameVector> formals, JS::SourceBufferHolder& srcBuf);

ScriptSourceObject*
CreateScriptSourceObject(ExclusiveContext* cx, const ReadOnlyCompileOptions& options);

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
IsIdentifier(const char16_t* chars, size_t length);

/* True if str is a keyword. Defined in TokenStream.cpp. */
bool
IsKeyword(JSLinearString* str);

/* GC marking. Defined in Parser.cpp. */
void
MarkParser(JSTracer* trc, JS::AutoGCRooter* parser);

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_BytecodeCompiler_h */
