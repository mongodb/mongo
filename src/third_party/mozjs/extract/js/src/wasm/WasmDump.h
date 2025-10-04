/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2023 Mozilla Foundation
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

#ifndef wasm_dump_h
#define wasm_dump_h

#include "js/Printer.h"

#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"

namespace js {
namespace wasm {

#ifdef DEBUG
extern void Dump(ValType type);
extern void Dump(ValType type, GenericPrinter& out);

extern void Dump(StorageType type);
extern void Dump(StorageType type, GenericPrinter& out);

extern void Dump(RefType type);
extern void Dump(RefType type, GenericPrinter& out);

extern void Dump(const FuncType& funcType);
extern void Dump(const FuncType& funcType, IndentedPrinter& out);

extern void Dump(const StructType& structType);
extern void Dump(const StructType& structType, IndentedPrinter& out);

extern void Dump(const ArrayType& arrayType);
extern void Dump(const ArrayType& arrayType, IndentedPrinter& out);

extern void Dump(const TypeDef& typeDef);
extern void Dump(const TypeDef& typeDef, IndentedPrinter& out);

extern void Dump(const RecGroup& recGroup);
extern void Dump(const RecGroup& recGroup, IndentedPrinter& out);

extern void Dump(const TypeContext& typeContext);
extern void Dump(const TypeContext& typeContext, IndentedPrinter& out);
#endif

}  // namespace wasm
}  // namespace js

#endif  // wasm_dump_h
