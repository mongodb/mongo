/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
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

#ifndef wasm_type_decls_h
#define wasm_type_decls_h

#include "NamespaceImports.h"
#include "gc/Barrier.h"
#include "js/GCVector.h"
#include "js/HashTable.h"
#include "js/RootingAPI.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Vector.h"

namespace js {

using JSFunctionVector = GCVector<JSFunction*, 0, SystemAllocPolicy>;

class WasmMemoryObject;
using GCPtrWasmMemoryObject = GCPtr<WasmMemoryObject*>;
using RootedWasmMemoryObject = Rooted<WasmMemoryObject*>;
using HandleWasmMemoryObject = Handle<WasmMemoryObject*>;
using MutableHandleWasmMemoryObject = MutableHandle<WasmMemoryObject*>;

class WasmModuleObject;
using RootedWasmModuleObject = Rooted<WasmModuleObject*>;
using HandleWasmModuleObject = Handle<WasmModuleObject*>;
using MutableHandleWasmModuleObject = MutableHandle<WasmModuleObject*>;

class WasmInstanceObject;
using WasmInstanceObjectVector = GCVector<WasmInstanceObject*>;
using RootedWasmInstanceObject = Rooted<WasmInstanceObject*>;
using HandleWasmInstanceObject = Handle<WasmInstanceObject*>;
using MutableHandleWasmInstanceObject = MutableHandle<WasmInstanceObject*>;

class WasmTableObject;
using WasmTableObjectVector = GCVector<WasmTableObject*, 0, SystemAllocPolicy>;
using RootedWasmTableObject = Rooted<WasmTableObject*>;
using HandleWasmTableObject = Handle<WasmTableObject*>;
using MutableHandleWasmTableObject = MutableHandle<WasmTableObject*>;

class WasmGlobalObject;
using WasmGlobalObjectVector =
    GCVector<WasmGlobalObject*, 0, SystemAllocPolicy>;
using RootedWasmGlobalObject = Rooted<WasmGlobalObject*>;

class WasmExceptionObject;
using WasmExceptionObjectVector =
    GCVector<WasmExceptionObject*, 0, SystemAllocPolicy>;
using RootedWasmExceptionObject = Rooted<WasmExceptionObject*>;

class WasmRuntimeExceptionObject;
using RootedWasmRuntimeExceptionObject = Rooted<WasmRuntimeExceptionObject*>;

namespace wasm {

struct ModuleEnvironment;
class Decoder;
class Instance;

// Uint32Vector has initial size 8 on the basis that the dominant use cases
// (line numbers and control stacks) tend to have a small but nonzero number
// of elements.
using Uint32Vector = Vector<uint32_t, 8, SystemAllocPolicy>;

using Bytes = Vector<uint8_t, 0, SystemAllocPolicy>;
using UniqueBytes = UniquePtr<Bytes>;
using UniqueConstBytes = UniquePtr<const Bytes>;
using UTF8Bytes = Vector<char, 0, SystemAllocPolicy>;
using InstanceVector = Vector<Instance*, 0, SystemAllocPolicy>;
using UniqueCharsVector = Vector<UniqueChars, 0, SystemAllocPolicy>;
using RenumberMap =
    HashMap<uint32_t, uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_type_decls_h
