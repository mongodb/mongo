/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

#ifndef wasm_static_type_defs
#define wasm_static_type_defs

namespace js {
namespace wasm {

class TypeDef;

// Simple type definitions used in builtins with a static lifetime.
//
// TODO: this class is very simple and won't scale well with many type
// definitions. Rethink this if we have more than several type definitions.
struct StaticTypeDefs {
  static const TypeDef* arrayMutI16;

  [[nodiscard]] static bool init();
  static void destroy();
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_static_type_defs
