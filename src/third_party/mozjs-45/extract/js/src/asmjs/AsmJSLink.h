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

#ifndef asmjs_AsmJSLink_h
#define asmjs_AsmJSLink_h

#include "NamespaceImports.h"

namespace js {

// Create a new JSFunction to replace originalFun as the representation of the
// function defining the succesfully-validated module 'moduleObj'.
extern JSFunction*
NewAsmJSModuleFunction(ExclusiveContext* cx, JSFunction* originalFun, HandleObject moduleObj);

// Return whether this is the js::Native returned by NewAsmJSModuleFunction.
extern bool
IsAsmJSModuleNative(JSNative native);

// Return whether the given value is a function containing "use asm" that has
// been validated according to the asm.js spec.
extern bool
IsAsmJSModule(JSContext* cx, unsigned argc, JS::Value* vp);
extern bool
IsAsmJSModule(HandleFunction fun);

extern JSString*
AsmJSModuleToString(JSContext* cx, HandleFunction fun, bool addParenToLambda);

// Return whether the given value is a function containing "use asm" that was
// loaded directly from the cache (and hence was validated previously).
extern bool
IsAsmJSModuleLoadedFromCache(JSContext* cx, unsigned argc, Value* vp);

// Return whether the given value is a nested function in an asm.js module that
// has been both compile- and link-time validated.
extern bool
IsAsmJSFunction(JSContext* cx, unsigned argc, JS::Value* vp);
extern bool
IsAsmJSFunction(HandleFunction fun);

extern JSString*
AsmJSFunctionToString(JSContext* cx, HandleFunction fun);

} // namespace js

#endif // asmjs_AsmJSLink_h
