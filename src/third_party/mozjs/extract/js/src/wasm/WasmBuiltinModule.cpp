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

#include "wasm/WasmBuiltinModule.h"

#include "util/Text.h"
#include "vm/GlobalObject.h"

#include "wasm/WasmBuiltinModuleGenerated.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmStaticTypeDefs.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::wasm;

BuiltinModuleFuncs* BuiltinModuleFuncs::singleton_ = nullptr;

[[nodiscard]] bool BuiltinModuleFunc::init(const RefPtr<TypeContext>& types,
                                           mozilla::Span<const ValType> params,
                                           Maybe<ValType> result,
                                           bool usesMemory,
                                           const SymbolicAddressSignature* sig,
                                           const char* exportName) {
  // This builtin must not have been initialized yet.
  MOZ_ASSERT(!recGroup_);

  // Initialize the basic fields
  exportName_ = exportName;
  sig_ = sig;
  usesMemory_ = usesMemory;

  // Create a function type for the given params and result
  ValTypeVector paramVec;
  if (!paramVec.append(params.data(), params.data() + params.size())) {
    return false;
  }
  ValTypeVector resultVec;
  if (result.isSome() && !resultVec.append(*result)) {
    return false;
  }
  const TypeDef* typeDef =
      types->addType(FuncType(std::move(paramVec), std::move(resultVec)));
  if (!typeDef) {
    return false;
  }
  recGroup_ = &typeDef->recGroup();
  return true;
}

bool BuiltinModuleFuncs::init() {
  singleton_ = js_new<BuiltinModuleFuncs>();
  if (!singleton_) {
    return false;
  }

  RefPtr<TypeContext> types = js_new<TypeContext>();
  if (!types) {
    return false;
  }

#define VISIT_BUILTIN_FUNC(op, export, sa_name, abitype, entry, uses_memory,   \
                           ...)                                                \
  const ValType op##Params[] =                                                 \
      DECLARE_BUILTIN_MODULE_FUNC_PARAM_VALTYPES_##op;                         \
  Maybe<ValType> op##Result = DECLARE_BUILTIN_MODULE_FUNC_RESULT_VALTYPE_##op; \
  if (!singleton_->funcs_[BuiltinModuleFuncId::op].init(                       \
          types, mozilla::Span<const ValType>(op##Params), op##Result,         \
          uses_memory, &SASig##sa_name, export)) {                             \
    return false;                                                              \
  }
  FOR_EACH_BUILTIN_MODULE_FUNC(VISIT_BUILTIN_FUNC)
#undef VISIT_BUILTIN_FUNC

  return true;
}

void BuiltinModuleFuncs::destroy() {
  if (!singleton_) {
    return;
  }
  js_delete(singleton_);
  singleton_ = nullptr;
}

bool EncodeFuncBody(const BuiltinModuleFunc& builtinModuleFunc,
                    BuiltinModuleFuncId id, Bytes* body) {
  Encoder encoder(*body);
  if (!EncodeLocalEntries(encoder, ValTypeVector())) {
    return false;
  }
  const FuncType* funcType = builtinModuleFunc.funcType();
  for (uint32_t i = 0; i < funcType->args().length(); i++) {
    if (!encoder.writeOp(Op::LocalGet) || !encoder.writeVarU32(i)) {
      return false;
    }
  }
  if (!encoder.writeOp(MozOp::CallBuiltinModuleFunc)) {
    return false;
  }
  if (!encoder.writeVarU32(uint32_t(id))) {
    return false;
  }
  return encoder.writeOp(Op::End);
}

bool CompileBuiltinModule(JSContext* cx,
                          const mozilla::Span<BuiltinModuleFuncId> ids,
                          mozilla::Maybe<Shareable> memory,
                          MutableHandle<WasmModuleObject*> result) {
  // Create the options manually, enabling intrinsics
  FeatureOptions featureOptions;
  featureOptions.isBuiltinModule = true;

  // Initialize the compiler environment, choosing the best tier possible
  SharedCompileArgs compileArgs = CompileArgs::buildAndReport(
      cx, ScriptedCaller(), featureOptions, /* reportOOM */ true);
  if (!compileArgs) {
    return false;
  }
  CompilerEnvironment compilerEnv(
      CompileMode::Once, IonAvailable(cx) ? Tier::Optimized : Tier::Baseline,
      DebugEnabled::False);
  compilerEnv.computeParameters();

  // Build a module environment
  ModuleEnvironment moduleEnv(compileArgs->features);
  if (!moduleEnv.init()) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (memory.isSome()) {
    // Add (import (memory 0))
    CacheableName emptyString;
    CacheableName memoryString;
    if (!CacheableName::fromUTF8Chars("memory", &memoryString)) {
      ReportOutOfMemory(cx);
      return false;
    }
    if (!moduleEnv.imports.append(Import(std::move(emptyString),
                                         std::move(memoryString),
                                         DefinitionKind::Memory))) {
      ReportOutOfMemory(cx);
      return false;
    }
    if (!moduleEnv.memories.append(MemoryDesc(Limits(0, Nothing(), *memory)))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Add (type (func (params ...))) for each func. The function types will
  // be deduplicated by the runtime
  for (uint32_t funcIndex = 0; funcIndex < ids.size(); funcIndex++) {
    const BuiltinModuleFuncId& id = ids[funcIndex];
    const BuiltinModuleFunc& builtinModuleFunc =
        BuiltinModuleFuncs::getFromId(id);

    SharedRecGroup recGroup = builtinModuleFunc.recGroup();
    MOZ_ASSERT(recGroup->numTypes() == 1);
    if (!moduleEnv.types->addRecGroup(recGroup)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Add (func (type $i)) declarations. Do this after all types have been added
  // as the function declaration metadata uses pointers into the type vectors
  // that must be stable.
  for (uint32_t funcIndex = 0; funcIndex < ids.size(); funcIndex++) {
    FuncDesc decl(&(*moduleEnv.types)[funcIndex].funcType(), funcIndex);
    if (!moduleEnv.funcs.append(decl)) {
      ReportOutOfMemory(cx);
      return false;
    }
    moduleEnv.declareFuncExported(funcIndex, true, false);
  }

  // Add (export "$name" (func $i)) declarations.
  for (uint32_t funcIndex = 0; funcIndex < ids.size(); funcIndex++) {
    const BuiltinModuleFunc& builtinModuleFunc =
        BuiltinModuleFuncs::getFromId(ids[funcIndex]);

    CacheableName exportName;
    if (!CacheableName::fromUTF8Chars(builtinModuleFunc.exportName(),
                                      &exportName) ||
        !moduleEnv.exports.append(Export(std::move(exportName), funcIndex,
                                         DefinitionKind::Function))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Compile the module functions
  UniqueChars error;
  ModuleGenerator mg(*compileArgs, &moduleEnv, &compilerEnv, nullptr, &error,
                     nullptr);
  if (!mg.init(nullptr)) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Prepare and compile function bodies
  Vector<Bytes, 1, SystemAllocPolicy> bodies;
  if (!bodies.reserve(ids.size())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (uint32_t funcIndex = 0; funcIndex < ids.size(); funcIndex++) {
    BuiltinModuleFuncId id = ids[funcIndex];
    const BuiltinModuleFunc& builtinModuleFunc =
        BuiltinModuleFuncs::getFromId(ids[funcIndex]);

    // Compilation may be done using other threads, ModuleGenerator requires
    // that function bodies live until after finishFuncDefs().
    bodies.infallibleAppend(Bytes());
    Bytes& bytecode = bodies.back();

    // Encode function body that will call the builtinModuleFunc using our
    // builtin opcode, and launch a compile task
    if (!EncodeFuncBody(builtinModuleFunc, id, &bytecode) ||
        !mg.compileFuncDef(funcIndex, 0, bytecode.begin(),
                           bytecode.begin() + bytecode.length())) {
      // This must be an OOM and will be reported by the caller
      MOZ_ASSERT(!error);
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Finish and block on function compilation
  if (!mg.finishFuncDefs()) {
    // This must be an OOM and will be reported by the caller
    MOZ_ASSERT(!error);
    ReportOutOfMemory(cx);
    return false;
  }

  // Create a dummy bytecode vector, that will not be used
  SharedBytes bytecode = js_new<ShareableBytes>();
  if (!bytecode) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Finish the module
  SharedModule module = mg.finishModule(*bytecode, nullptr);
  if (!module) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Create a WasmModuleObject for the module, and return it
  RootedObject proto(
      cx, GlobalObject::getOrCreatePrototype(cx, JSProto_WasmModule));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }
  result.set(WasmModuleObject::create(cx, *module, proto));
  return !!result;
}

static BuiltinModuleFuncId SelfTestFuncs[] = {BuiltinModuleFuncId::I8VecMul};

#ifdef ENABLE_WASM_MOZ_INTGEMM
static BuiltinModuleFuncId IntGemmFuncs[] = {
    BuiltinModuleFuncId::I8PrepareB,
    BuiltinModuleFuncId::I8PrepareBFromTransposed,
    BuiltinModuleFuncId::I8PrepareBFromQuantizedTransposed,
    BuiltinModuleFuncId::I8PrepareA,
    BuiltinModuleFuncId::I8PrepareBias,
    BuiltinModuleFuncId::I8MultiplyAndAddBias,
    BuiltinModuleFuncId::I8SelectColumnsOfB};
#endif  // ENABLE_WASM_MOZ_INTGEMM

#ifdef ENABLE_WASM_JS_STRING_BUILTINS
static BuiltinModuleFuncId JSStringFuncs[] = {
    BuiltinModuleFuncId::StringTest,
    BuiltinModuleFuncId::StringCast,
    BuiltinModuleFuncId::StringFromCharCodeArray,
    BuiltinModuleFuncId::StringIntoCharCodeArray,
    BuiltinModuleFuncId::StringFromCharCode,
    BuiltinModuleFuncId::StringFromCodePoint,
    BuiltinModuleFuncId::StringCharCodeAt,
    BuiltinModuleFuncId::StringCodePointAt,
    BuiltinModuleFuncId::StringLength,
    BuiltinModuleFuncId::StringConcat,
    BuiltinModuleFuncId::StringSubstring,
    BuiltinModuleFuncId::StringEquals,
    BuiltinModuleFuncId::StringCompare};
static const char* JSStringModuleName = "wasm:js-string";
#endif  // ENABLE_WASM_JS_STRING_BUILTINS

Maybe<BuiltinModuleId> wasm::ImportMatchesBuiltinModule(
    Span<const char> importName, BuiltinModuleIds enabledBuiltins) {
#ifdef ENABLE_WASM_JS_STRING_BUILTINS
  if (enabledBuiltins.jsString &&
      importName == mozilla::MakeStringSpan(JSStringModuleName)) {
    return Some(BuiltinModuleId::JSString);
  }
#endif  // ENABLE_WASM_JS_STRING_BUILTINS
  // Not supported for implicit instantiation yet
  MOZ_RELEASE_ASSERT(!enabledBuiltins.selfTest && !enabledBuiltins.intGemm);
  return Nothing();
}

Maybe<const BuiltinModuleFunc*> wasm::ImportMatchesBuiltinModuleFunc(
    mozilla::Span<const char> importName, BuiltinModuleId module) {
#ifdef ENABLE_WASM_JS_STRING_BUILTINS
  // Not supported for implicit instantiation yet
  MOZ_RELEASE_ASSERT(module == BuiltinModuleId::JSString);
  for (BuiltinModuleFuncId funcId : JSStringFuncs) {
    const BuiltinModuleFunc& func = BuiltinModuleFuncs::getFromId(funcId);
    if (importName == mozilla::MakeStringSpan(func.exportName())) {
      return Some(&func);
    }
  }
#endif  // ENABLE_WASM_JS_STRING_BUILTINS
  return Nothing();
}

bool wasm::CompileBuiltinModule(JSContext* cx, BuiltinModuleId module,
                                MutableHandle<WasmModuleObject*> result) {
  switch (module) {
    case BuiltinModuleId::SelfTest:
      return CompileBuiltinModule(cx, SelfTestFuncs, Some(Shareable::False),
                                  result);
#ifdef ENABLE_WASM_MOZ_INTGEMM
    case BuiltinModuleId::IntGemm:
      return CompileBuiltinModule(cx, IntGemmFuncs, Some(Shareable::False),
                                  result);
#endif  // ENABLE_WASM_MOZ_INTGEMM
#ifdef ENABLE_WASM_JS_STRING_BUILTINS
    case BuiltinModuleId::JSString:
      return CompileBuiltinModule(cx, JSStringFuncs, Nothing(), result);
#endif  // ENABLE_WASM_JS_STRING_BUILTINS
    default:
      MOZ_CRASH();
  }
}

bool wasm::InstantiateBuiltinModule(JSContext* cx, BuiltinModuleId module,
                                    MutableHandleObject result) {
  Rooted<WasmModuleObject*> moduleObj(cx);
  if (!CompileBuiltinModule(cx, module, &moduleObj)) {
    ReportOutOfMemory(cx);
    return false;
  }
  ImportValues imports;
  Rooted<WasmInstanceObject*> instanceObj(cx);
  RootedObject instanceProto(cx);
  if (!moduleObj->module().instantiate(cx, imports, instanceProto,
                                       &instanceObj)) {
    MOZ_RELEASE_ASSERT(cx->isThrowingOutOfMemory());
    return false;
  }
  result.set(&instanceObj->exportsObj());
  return true;
}
