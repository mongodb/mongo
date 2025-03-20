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

#ifndef wasm_builtin_module_h
#define wasm_builtin_module_h

#include "mozilla/EnumeratedArray.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"

#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmTypeDef.h"

namespace js {
namespace wasm {

struct MOZ_STACK_CLASS BuiltinModuleInstances {
  explicit BuiltinModuleInstances(JSContext* cx)
      : selfTest(cx), intGemm(cx), jsString(cx) {}

  Rooted<JSObject*> selfTest;
  Rooted<JSObject*> intGemm;
  Rooted<JSObject*> jsString;

  MutableHandle<JSObject*> operator[](BuiltinModuleId module) {
    switch (module) {
      case BuiltinModuleId::SelfTest: {
        return &selfTest;
      }
      case BuiltinModuleId::IntGemm: {
        return &intGemm;
      }
      case BuiltinModuleId::JSString: {
        return &jsString;
      }
      default: {
        MOZ_CRASH();
      }
    }
  }
};

// An builtin module func is a natively implemented function that may be
// compiled into a 'builtin module', which may be instantiated with a provided
// memory yielding an exported WebAssembly function wrapping the builtin module.
class BuiltinModuleFunc {
 private:
  SharedRecGroup recGroup_;
  const char* exportName_;
  const SymbolicAddressSignature* sig_;
  bool usesMemory_;

 public:
  // Default constructor so this can be used in an EnumeratedArray.
  BuiltinModuleFunc() = default;

  // Initialize this builtin. Must only be called once.
  [[nodiscard]] bool init(const RefPtr<TypeContext>& types,
                          mozilla::Span<const ValType> params,
                          Maybe<ValType> result, bool usesMemory,
                          const SymbolicAddressSignature* sig,
                          const char* exportName);

  // The rec group for the function type for this builtin.
  const RecGroup* recGroup() const { return recGroup_.get(); }
  // The type definition for the function type for this builtin.
  const TypeDef* typeDef() const { return &recGroup_->type(0); }
  // The function type for this builtin.
  const FuncType* funcType() const { return &typeDef()->funcType(); }

  // The name of the func as it is exported
  const char* exportName() const { return exportName_; }
  // The signature of the builtin that implements this function.
  const SymbolicAddressSignature* sig() const { return sig_; }
  // Whether this function takes a pointer to the memory base as a hidden final
  // parameter. This parameter will show up in the SymbolicAddressSignature,
  // but not the function type. Compilers must pass the memoryBase to the
  // function call as the last parameter.
  bool usesMemory() const { return usesMemory_; }
};

// Static storage for all builtin module funcs in the system.
class BuiltinModuleFuncs {
  using Storage =
      mozilla::EnumeratedArray<BuiltinModuleFuncId, BuiltinModuleFunc,
                               size_t(BuiltinModuleFuncId::Limit)>;
  Storage funcs_;

  static BuiltinModuleFuncs* singleton_;

 public:
  [[nodiscard]] static bool init();
  static void destroy();

  // Get the BuiltinModuleFunc for an BuiltinModuleFuncId. BuiltinModuleFuncId
  // must be validated.
  static const BuiltinModuleFunc& getFromId(BuiltinModuleFuncId id) {
    return singleton_->funcs_[id];
  }
};

Maybe<BuiltinModuleId> ImportMatchesBuiltinModule(
    mozilla::Span<const char> importName, BuiltinModuleIds enabledBuiltins);
Maybe<const BuiltinModuleFunc*> ImportMatchesBuiltinModuleFunc(
    mozilla::Span<const char> importName, BuiltinModuleId module);

// Compile and return the builtin module for a particular
// builtin module.
bool CompileBuiltinModule(JSContext* cx, BuiltinModuleId module,
                          MutableHandle<WasmModuleObject*> result);

// Compile, instantiate and return the builtin module instance for a particular
// builtin module.
bool InstantiateBuiltinModule(JSContext* cx, BuiltinModuleId module,
                              MutableHandle<JSObject*> result);

}  // namespace wasm
}  // namespace js

#endif  // wasm_builtin_module_h
