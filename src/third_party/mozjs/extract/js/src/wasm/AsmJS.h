/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

#ifndef asmjs_asmjs_h
#define asmjs_asmjs_h

#include "NamespaceImports.h"

namespace js {

namespace frontend {

class ParseContext;
class ParseNode;

template <class ParseHandler, typename CharT> class Parser;
class FullParseHandler;

}

using AsmJSParser = frontend::Parser<frontend::FullParseHandler, char16_t>;

// This function takes over parsing of a function starting with "use asm". The
// return value indicates whether an error was reported which the caller should
// propagate. If no error was reported, the function may still fail to validate
// as asm.js. In this case, the parser.tokenStream has been advanced an
// indeterminate amount and the entire function should be reparsed from the
// beginning.

extern MOZ_MUST_USE bool
CompileAsmJS(JSContext* cx, AsmJSParser& parser, frontend::ParseNode* stmtList,
             bool* validated);

// asm.js module/export queries:

extern bool
IsAsmJSModuleNative(JSNative native);

extern bool
IsAsmJSModule(JSFunction* fun);

extern bool
IsAsmJSFunction(JSFunction* fun);

extern bool
IsAsmJSStrictModeModuleOrFunction(JSFunction* fun);

extern bool
InstantiateAsmJS(JSContext* cx, unsigned argc, JS::Value* vp);

// asm.js testing natives:

extern bool
IsAsmJSCompilationAvailable(JSContext* cx, unsigned argc, JS::Value* vp);

extern bool
IsAsmJSModule(JSContext* cx, unsigned argc, JS::Value* vp);

extern bool
IsAsmJSModuleLoadedFromCache(JSContext* cx, unsigned argc, Value* vp);

extern bool
IsAsmJSFunction(JSContext* cx, unsigned argc, JS::Value* vp);

// asm.js toString/toSource support:

extern JSString*
AsmJSFunctionToString(JSContext* cx, HandleFunction fun);

extern JSString*
AsmJSModuleToString(JSContext* cx, HandleFunction fun, bool isToSource);

// asm.js heap:

extern bool
IsValidAsmJSHeapLength(uint32_t length);

} // namespace js

#endif // asmjs_asmjs_h
