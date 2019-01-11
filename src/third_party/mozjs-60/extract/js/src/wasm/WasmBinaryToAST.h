/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
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

#ifndef wasmbinarytoast_h
#define wasmbinarytoast_h

#include "ds/LifoAlloc.h"

#include "wasm/WasmAST.h"
#include "wasm/WasmTypes.h"

namespace js {
namespace wasm {

bool
BinaryToAst(JSContext* cx, const uint8_t* bytes, uint32_t length, LifoAlloc& lifo,
            AstModule** module);

} // end wasm namespace
} // end js namespace

#endif // namespace wasmbinarytoast_h
