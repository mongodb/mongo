/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_TestingUtility_h
#define builtin_TestingUtility_h

#include "js/experimental/JSStencil.h"  // JS::Stencil
#include "js/RootingAPI.h"              // JS::Handle, JS::MutableHandle
#include "js/Utility.h"                 // JS::UniqueChars

struct JSContext;
class JSObject;
class JSString;

namespace JS {
class JS_PUBLIC_API CompileOptions;
}

namespace js {

class FrontendContext;
class ScriptSource;

namespace frontend {
struct CompilationStencil;
}  // namespace frontend

// Populate `options` fields from `opt` object.
//
// `opts` can have the following properties:
//   * isRunOnce (boolean): options.isRunOnce
//   * noScriptRval (boolean): options.noScriptRval
//   * fileName (string): options.filename_
//               enabled only when `fileNameBytes` is given, and
//               `fileNameBytes` is initialized to the filename bytes
//   * skipFileNameValidation (boolean): options.skipFileNameValidation_
//   * lineNumber (number): options.lineno
//   * columnNumber (number): options.column
//   * sourceIsLazy (boolean): options.sourceIsLazy
//   * forceFullParse (boolean): options.forceFullParse_
[[nodiscard]] bool ParseCompileOptions(JSContext* cx,
                                       JS::CompileOptions& options,
                                       JS::Handle<JSObject*> opts,
                                       JS::UniqueChars* fileNameBytes);

[[nodiscard]] bool ParseSourceOptions(
    JSContext* cx, JS::Handle<JSObject*> opts,
    JS::MutableHandle<JSString*> displayURL,
    JS::MutableHandle<JSString*> sourceMapURL);

[[nodiscard]] bool SetSourceOptions(JSContext* cx, FrontendContext* fc,
                                    ScriptSource* source,
                                    JS::Handle<JSString*> displayURL,
                                    JS::Handle<JSString*> sourceMapURL);

JSObject* CreateScriptPrivate(JSContext* cx,
                              JS::Handle<JSString*> path = nullptr);

[[nodiscard]] bool ParseDebugMetadata(
    JSContext* cx, JS::Handle<JSObject*> opts,
    JS::MutableHandle<JS::Value> privateValue,
    JS::MutableHandle<JSString*> elementAttributeName);

[[nodiscard]] JS::UniqueChars StringToLocale(JSContext* cx,
                                             JS::Handle<JSObject*> callee,
                                             JS::Handle<JSString*> str_);

// Validate the option for lazy-parsing agrees between the current global and
// the stencil.
bool ValidateLazinessOfStencilAndGlobal(JSContext* cx,
                                        const JS::Stencil* stencil);

bool ValidateModuleCompileOptions(JSContext* cx, JS::CompileOptions& options);

} /* namespace js */

#endif /* builtin_TestingUtility_h */
