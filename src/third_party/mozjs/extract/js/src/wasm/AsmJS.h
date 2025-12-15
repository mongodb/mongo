/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2014 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_AsmJS_h
#define wasm_AsmJS_h

#include "mozilla/Utf8.h"  // mozilla::Utf8Unit

#include <stdint.h>  // uint32_t

#include "jstypes.h"             // JS_PUBLIC_API
#include "js/CallArgs.h"         // JSNative
#include "wasm/WasmShareable.h"  // ShareableBase

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSFunction;

namespace JS {

class JS_PUBLIC_API Value;

template <typename T>
class Handle;

}  // namespace JS

namespace js {

class FrontendContext;
class ScriptSource;

namespace frontend {

class ParserAtomsTable;
class ParseContext;
class ParseNode;

template <class ParseHandler, typename CharT>
class Parser;
class FullParseHandler;

}  // namespace frontend

template <typename Unit>
using AsmJSParser = frontend::Parser<frontend::FullParseHandler, Unit>;

// This function takes over parsing of a function starting with "use asm". The
// return value indicates whether an error was reported which the caller should
// propagate. If no error was reported, the function may still fail to validate
// as asm.js. In this case, the parser.tokenStream has been advanced an
// indeterminate amount and the entire function should be reparsed from the
// beginning.

[[nodiscard]] extern bool CompileAsmJS(FrontendContext* fc,
                                       frontend::ParserAtomsTable& parserAtoms,
                                       AsmJSParser<mozilla::Utf8Unit>& parser,
                                       frontend::ParseNode* stmtList,
                                       bool* validated);

[[nodiscard]] extern bool CompileAsmJS(FrontendContext* fc,
                                       frontend::ParserAtomsTable& parserAtoms,
                                       AsmJSParser<char16_t>& parser,
                                       frontend::ParseNode* stmtList,
                                       bool* validated);

// asm.js module/export queries:

extern bool IsAsmJSModuleNative(JSNative native);

extern bool IsAsmJSModule(JSFunction* fun);

extern bool IsAsmJSFunction(JSFunction* fun);

extern bool IsAsmJSStrictModeModuleOrFunction(JSFunction* fun);

extern bool InstantiateAsmJS(JSContext* cx, unsigned argc, JS::Value* vp);

// asm.js testing natives:

extern bool IsAsmJSCompilationAvailable(JSContext* cx, unsigned argc,
                                        JS::Value* vp);

extern bool IsAsmJSCompilationAvailable(JSContext* cx);

extern bool IsAsmJSModule(JSContext* cx, unsigned argc, JS::Value* vp);

extern bool IsAsmJSFunction(JSContext* cx, unsigned argc, JS::Value* vp);

// asm.js toString/toSource support:

extern JSString* AsmJSFunctionToString(JSContext* cx,
                                       JS::Handle<JSFunction*> fun);

extern JSString* AsmJSModuleToString(JSContext* cx, JS::Handle<JSFunction*> fun,
                                     bool isToSource);

// asm.js heap:

extern bool IsValidAsmJSHeapLength(size_t length);

// Minimally expose wasm::Code-lifetime state in AsmJS.cpp to ModuleGenerator
// and friends.  The only implementation of this interface is in, and private
// to, WasmJS.cpp.  In every place that stores or uses a CodeMetadataForAsmJS*
// (or smart-pointer equivalent), that pointer may be null, which indicates
// that the associated module is wasm, not asm.js.

struct CodeMetadataForAsmJSImpl;

struct CodeMetadataForAsmJS : public wasm::ShareableBase<CodeMetadataForAsmJS> {
  CodeMetadataForAsmJS() {};
  virtual ~CodeMetadataForAsmJS() = default;

  virtual const CodeMetadataForAsmJSImpl& asAsmJS() const = 0;

  virtual bool mutedErrors() const = 0;
  virtual const char16_t* displayURL() const = 0;
  virtual ScriptSource* maybeScriptSource() const = 0;
  virtual bool getFuncNameForAsmJS(uint32_t funcIndex,
                                   wasm::UTF8Bytes* name) const = 0;

  virtual size_t sizeOfExcludingThis(
      mozilla::MallocSizeOf mallocSizeOf) const = 0;
};

using MutableCodeMetadataForAsmJS = RefPtr<CodeMetadataForAsmJS>;
using SharedCodeMetadataForAsmJS = RefPtr<const CodeMetadataForAsmJS>;

}  // namespace js

#endif  // wasm_AsmJS_h
