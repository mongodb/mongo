/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_TestingUtility_h
#define builtin_TestingUtility_h

#include "js/CompileOptions.h"  // JS::CompileOptions
#include "js/RootingAPI.h"      // JS::Handle, JS::MutableHandle
#include "js/Utility.h"         // JS::UniqueChars

struct JSContext;
class JSObject;

namespace js {

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

} /* namespace js */

#endif /* builtin_TestingUtility_h */
