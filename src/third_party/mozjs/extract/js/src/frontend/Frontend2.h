/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_Frontend2_h
#define frontend_Frontend2_h

#include "mozilla/Utf8.h"  // mozilla::Utf8Unit

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t

#include "js/CompileOptions.h"  // JS::ReadOnlyCompileOptions
#include "js/RootingAPI.h"      // JS::Handle
#include "js/SourceText.h"      // JS::SourceText
#include "js/UniquePtr.h"       // js::UniquePtr
#include "vm/JSScript.h"        // JSScript

struct JSContext;

struct SmooshResult;

namespace js {

class ScriptSourceObject;
class FrontendContext;

namespace frontend {

struct CompilationInput;
struct ExtensibleCompilationStencil;
struct CompilationGCOutput;
struct CompilationState;

// This is declarated as a class mostly to solve dependency around `friend`
// declarations in the simple way.
class Smoosh {
 public:
  [[nodiscard]] static bool tryCompileGlobalScriptToExtensibleStencil(
      JSContext* cx, FrontendContext* fc, CompilationInput& input,
      JS::SourceText<mozilla::Utf8Unit>& srcBuf,
      UniquePtr<ExtensibleCompilationStencil>& stencilOut);
};

// Initialize SmooshMonkey globals, such as the logging system.
void InitSmoosh();

// Use the SmooshMonkey frontend to parse and free the generated AST. Returns
// true if no error were detected while parsing.
[[nodiscard]] bool SmooshParseScript(JSContext* cx, const uint8_t* bytes,
                                     size_t length);
[[nodiscard]] bool SmooshParseModule(JSContext* cx, const uint8_t* bytes,
                                     size_t length);

}  // namespace frontend

}  // namespace js

#endif /* frontend_Frontend2_h */
