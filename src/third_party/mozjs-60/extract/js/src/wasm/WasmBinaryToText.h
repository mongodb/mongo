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

#ifndef wasm_binary_to_text_h
#define wasm_binary_to_text_h

#include "NamespaceImports.h"

#include "gc/Rooting.h"
#include "js/Class.h"
#include "wasm/WasmCode.h"

namespace js {

class StringBuffer;

namespace wasm {

// Translate the given binary representation of a wasm module into the module's textual
// representation.

MOZ_MUST_USE bool
BinaryToText(JSContext* cx, const uint8_t* bytes, size_t length, StringBuffer& buffer,
             GeneratedSourceMap* sourceMap = nullptr);

}  // namespace wasm

}  // namespace js

#endif // namespace wasm_binary_to_text_h
