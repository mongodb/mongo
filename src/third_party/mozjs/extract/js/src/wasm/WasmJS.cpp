/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

#include "wasm/WasmJS.h"

#include "mozilla/EndianUtils.h"
#include "mozilla/Maybe.h"

#include <algorithm>
#include <cstdint>

#include "jsapi.h"
#include "jsexn.h"

#include "ds/IdValuePair.h"            // js::IdValuePair
#include "frontend/FrontendContext.h"  // AutoReportFrontendContext
#include "gc/GCContext.h"
#include "jit/AtomicOperations.h"
#include "jit/FlushICache.h"
#include "jit/JitContext.h"
#include "jit/JitOptions.h"
#include "jit/Simulator.h"
#include "js/ColumnNumber.h"  // JS::ColumnNumberOneOrigin
#include "js/ForOfIterator.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Printf.h"
#include "js/PropertyAndElement.h"  // JS_DefineProperty, JS_GetProperty
#include "js/PropertySpec.h"        // JS_{PS,FN}{,_END}
#include "js/Stack.h"               // BuildStackString
#include "js/StreamConsumer.h"
#include "util/StringBuilder.h"
#include "util/Text.h"
#include "vm/ErrorObject.h"
#include "vm/FunctionFlags.h"      // js::FunctionFlags
#include "vm/GlobalObject.h"       // js::GlobalObject
#include "vm/HelperThreadState.h"  // js::PromiseHelperTask
#include "vm/Interpreter.h"
#include "vm/JSFunction.h"
#include "vm/PlainObject.h"    // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/SharedArrayObject.h"
#include "vm/StringType.h"
#include "vm/Warnings.h"  // js::WarnNumberASCII
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmBuiltinModule.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmPI.h"
#include "wasm/WasmProcess.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmValidate.h"

#include "gc/GCContext-inl.h"
#include "gc/StableCellHasher-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "wasm/WasmInstance-inl.h"

/*
 * [SMDOC] WebAssembly code rules (evolving)
 *
 * TlsContext.get() is only to be invoked from functions that have been invoked
 *   _directly_ by generated code as cold(!) Builtin calls, from code that is
 *   only used by signal handlers, or from helper functions that have been
 *   called _directly_ from a simulator.  All other code shall pass in a
 *   JSContext* to functions that need it, or an Instance* or Instance* since
 * the context is available through them.
 *
 *   Code that uses TlsContext.get() shall annotate each such call with the
 *   reason why the call is OK.
 */

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;
using mozilla::Span;

static bool ThrowCompileOutOfMemory(JSContext* cx) {
  // Most OOMs during compilation are due to large contiguous allocations,
  // and future allocations are likely to succeed. Throwing a proper error
  // object is nicer for users in these circumstances.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_OUT_OF_MEMORY);
  return false;
}

// ============================================================================
// Imports

static bool ThrowBadImportArg(JSContext* cx) {
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_IMPORT_ARG);
  return false;
}

static bool ThrowBadImportType(JSContext* cx, const CacheableName& field,
                               const char* str) {
  UniqueChars fieldQuoted = field.toQuotedString(cx);
  if (!fieldQuoted) {
    ReportOutOfMemory(cx);
    return false;
  }
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_IMPORT_TYPE, fieldQuoted.get(), str);
  return false;
}

// For now reject cross-compartment wrappers. These have more complicated realm
// semantics (we use nonCCWRealm in a few places) and may require unwrapping to
// test for specific function types.
static bool IsCallableNonCCW(const Value& v) {
  return IsCallable(v) && !IsCrossCompartmentWrapper(&v.toObject());
}

static bool IsWasmSuspendingWrapper(const Value& v) {
  return v.isObject() && js::IsWasmSuspendingObject(&v.toObject());
}

bool js::wasm::GetImports(JSContext* cx, const Module& module,
                          HandleObject importObj, ImportValues* imports) {
  const ModuleMetadata& moduleMeta = module.moduleMeta();
  const CodeMetadata& codeMeta = module.codeMeta();
  const BuiltinModuleIds& builtinModules = codeMeta.features().builtinModules;

  if (!moduleMeta.imports.empty() && !importObj) {
    return ThrowBadImportArg(cx);
  }

  BuiltinModuleInstances builtinInstances(cx);
  RootedValue importModuleValue(cx);
  RootedObject importModuleObject(cx);
  bool isImportedStringModule = false;
  RootedValue importFieldValue(cx);

  uint32_t tagIndex = 0;
  const TagDescVector& tags = codeMeta.tags;
  uint32_t globalIndex = 0;
  const GlobalDescVector& globals = codeMeta.globals;
  uint32_t tableIndex = 0;
  const TableDescVector& tables = codeMeta.tables;
  for (const Import& import : moduleMeta.imports) {
    Maybe<BuiltinModuleId> builtinModule =
        ImportMatchesBuiltinModule(import.module.utf8Bytes(), builtinModules);
    if (builtinModule) {
      if (*builtinModule == BuiltinModuleId::JSStringConstants) {
        isImportedStringModule = true;
        importModuleObject = nullptr;
      } else {
        MutableHandle<JSObject*> builtinInstance =
            builtinInstances[*builtinModule];
        if (!builtinInstance && !wasm::InstantiateBuiltinModule(
                                    cx, *builtinModule, builtinInstance)) {
          return false;
        }
        isImportedStringModule = false;
        importModuleObject = builtinInstance;
      }
    } else {
      RootedId moduleName(cx);
      if (!import.module.toPropertyKey(cx, &moduleName)) {
        return false;
      }

      if (!GetProperty(cx, importObj, importObj, moduleName,
                       &importModuleValue)) {
        return false;
      }

      if (!importModuleValue.isObject()) {
        UniqueChars moduleQuoted = import.module.toQuotedString(cx);
        if (!moduleQuoted) {
          ReportOutOfMemory(cx);
          return false;
        }
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_BAD_IMPORT_FIELD,
                                 moduleQuoted.get());
        return false;
      }

      isImportedStringModule = false;
      importModuleObject = &importModuleValue.toObject();
    }
    MOZ_RELEASE_ASSERT(!isImportedStringModule ||
                       import.kind == DefinitionKind::Global);

    if (isImportedStringModule) {
      RootedString stringConstant(cx, import.field.toJSString(cx));
      if (!stringConstant) {
        ReportOutOfMemory(cx);
        return false;
      }
      importFieldValue = StringValue(stringConstant);
    } else {
      RootedId fieldName(cx);
      if (!import.field.toPropertyKey(cx, &fieldName)) {
        return false;
      }
      if (!GetProperty(cx, importModuleObject, importModuleObject, fieldName,
                       &importFieldValue)) {
        return false;
      }
    }

    switch (import.kind) {
      case DefinitionKind::Function: {
        if (!IsCallableNonCCW(importFieldValue) &&
            !IsWasmSuspendingWrapper(importFieldValue)) {
          return ThrowBadImportType(cx, import.field, "Function");
        }

        if (!imports->funcs.append(&importFieldValue.toObject())) {
          ReportOutOfMemory(cx);
          return false;
        }

        break;
      }
      case DefinitionKind::Table: {
        const uint32_t index = tableIndex++;
        if (!importFieldValue.isObject() ||
            !importFieldValue.toObject().is<WasmTableObject>()) {
          return ThrowBadImportType(cx, import.field, "Table");
        }

        Rooted<WasmTableObject*> obj(
            cx, &importFieldValue.toObject().as<WasmTableObject>());
        if (obj->table().elemType() != tables[index].elemType) {
          JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                   JSMSG_WASM_BAD_TBL_TYPE_LINK);
          return false;
        }

        if (!imports->tables.append(obj)) {
          ReportOutOfMemory(cx);
          return false;
        }
        break;
      }
      case DefinitionKind::Memory: {
        if (!importFieldValue.isObject() ||
            !importFieldValue.toObject().is<WasmMemoryObject>()) {
          return ThrowBadImportType(cx, import.field, "Memory");
        }

        if (!imports->memories.append(
                &importFieldValue.toObject().as<WasmMemoryObject>())) {
          ReportOutOfMemory(cx);
          return false;
        }
        break;
      }
      case DefinitionKind::Tag: {
        const uint32_t index = tagIndex++;
        if (!importFieldValue.isObject() ||
            !importFieldValue.toObject().is<WasmTagObject>()) {
          return ThrowBadImportType(cx, import.field, "Tag");
        }

        Rooted<WasmTagObject*> obj(
            cx, &importFieldValue.toObject().as<WasmTagObject>());

        // Checks whether the signature of the imported exception object matches
        // the signature declared in the exception import's TagDesc.
        if (!TagType::matches(*obj->tagType(), *tags[index].type)) {
          UniqueChars fieldQuoted = import.field.toQuotedString(cx);
          UniqueChars moduleQuoted = import.module.toQuotedString(cx);
          if (!fieldQuoted || !moduleQuoted) {
            ReportOutOfMemory(cx);
            return false;
          }
          JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                   JSMSG_WASM_BAD_TAG_SIG, moduleQuoted.get(),
                                   fieldQuoted.get());
          return false;
        }

        if (!imports->tagObjs.append(obj)) {
          ReportOutOfMemory(cx);
          return false;
        }
        break;
      }
      case DefinitionKind::Global: {
        const uint32_t index = globalIndex++;
        const GlobalDesc& global = globals[index];
        MOZ_ASSERT(global.importIndex() == index);

        RootedVal val(cx);
        if (importFieldValue.isObject() &&
            importFieldValue.toObject().is<WasmGlobalObject>()) {
          Rooted<WasmGlobalObject*> obj(
              cx, &importFieldValue.toObject().as<WasmGlobalObject>());

          if (obj->isMutable() != global.isMutable()) {
            JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                     JSMSG_WASM_BAD_GLOB_MUT_LINK);
            return false;
          }

          bool matches = global.isMutable()
                             ? obj->type() == global.type()
                             : ValType::isSubTypeOf(obj->type(), global.type());
          if (!matches) {
            JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                     JSMSG_WASM_BAD_GLOB_TYPE_LINK);
            return false;
          }

          if (imports->globalObjs.length() <= index &&
              !imports->globalObjs.resize(index + 1)) {
            ReportOutOfMemory(cx);
            return false;
          }
          imports->globalObjs[index] = obj;
          val = obj->val();
        } else {
          if (!global.type().isRefType()) {
            if (global.type() == ValType::I64 && !importFieldValue.isBigInt()) {
              return ThrowBadImportType(cx, import.field, "BigInt");
            }
            if (global.type() != ValType::I64 && !importFieldValue.isNumber()) {
              return ThrowBadImportType(cx, import.field, "Number");
            }
          }

          if (global.isMutable()) {
            JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                     JSMSG_WASM_BAD_GLOB_MUT_LINK);
            return false;
          }

          if (!Val::fromJSValue(cx, global.type(), importFieldValue, &val)) {
            return false;
          }
        }

        if (!imports->globalValues.append(val)) {
          ReportOutOfMemory(cx);
          return false;
        }

        break;
      }
    }
  }

  MOZ_ASSERT(globalIndex == globals.length() ||
             !globals[globalIndex].isImport());

  return true;
}

static bool DescribeScriptedCaller(JSContext* cx, ScriptedCaller* caller,
                                   const char* introducer) {
  // Note: JS::DescribeScriptedCaller returns whether a scripted caller was
  // found, not whether an error was thrown. This wrapper function converts
  // back to the more ordinary false-if-error form.

  JS::AutoFilename af;
  if (JS::DescribeScriptedCaller(&af, cx, &caller->line)) {
    caller->filename =
        FormatIntroducedFilename(af.get(), caller->line, introducer);
    if (!caller->filename) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  return true;
}

static SharedCompileArgs InitCompileArgs(JSContext* cx, FeatureOptions options,
                                         const char* introducer) {
  ScriptedCaller scriptedCaller;
  if (!DescribeScriptedCaller(cx, &scriptedCaller, introducer)) {
    return nullptr;
  }

  return CompileArgs::buildAndReport(cx, std::move(scriptedCaller), options);
}

// ============================================================================
// Testing / Fuzzing support

bool wasm::Eval(JSContext* cx, Handle<TypedArrayObject*> code,
                HandleObject importObj,
                MutableHandle<WasmInstanceObject*> instanceObj) {
  if (!GlobalObject::ensureConstructor(cx, cx->global(), JSProto_WebAssembly)) {
    return false;
  }

  FeatureOptions options;
  SharedCompileArgs compileArgs = InitCompileArgs(cx, options, "wasm_eval");
  if (!compileArgs) {
    return false;
  }

  BytecodeSource source((uint8_t*)code->dataPointerEither().unwrap(),
                        code->byteLength().valueOr(0));
  UniqueChars error;
  UniqueCharsVector warnings;
  SharedModule module = CompileBuffer(
      *compileArgs, BytecodeBufferOrSource(source), &error, &warnings, nullptr);
  if (!module) {
    if (error) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_COMPILE_ERROR, error.get());
      return false;
    }
    return ThrowCompileOutOfMemory(cx);
  }

  Rooted<ImportValues> imports(cx);
  if (!GetImports(cx, *module, importObj, imports.address())) {
    return false;
  }

  return module->instantiate(cx, imports.get(), nullptr, instanceObj);
}

struct MOZ_STACK_CLASS SerializeListener : JS::OptimizedEncodingListener {
  // MOZ_STACK_CLASS means these can be nops.
  MozExternalRefCountType MOZ_XPCOM_ABI AddRef() override { return 0; }
  MozExternalRefCountType MOZ_XPCOM_ABI Release() override { return 0; }

  mozilla::DebugOnly<bool> called = false;
  Bytes* serialized;
  explicit SerializeListener(Bytes* serialized) : serialized(serialized) {}

  void storeOptimizedEncoding(const uint8_t* bytes, size_t length) override {
    MOZ_ASSERT(!called);
    called = true;
    if (serialized->resizeUninitialized(length)) {
      memcpy(serialized->begin(), bytes, length);
    }
  }
};

bool wasm::CompileAndSerialize(JSContext* cx,
                               const BytecodeSource& bytecodeSource,
                               Bytes* serialized) {
  // The caller must check that code caching is available
  MOZ_ASSERT(CodeCachingAvailable(cx));

  // Create and manually fill in compile args for code caching
  MutableCompileArgs compileArgs = js_new<CompileArgs>();
  if (!compileArgs) {
    return false;
  }

  // The caller has ensured CodeCachingAvailable(). Moreover, we want to ensure
  // we go straight to tier-2 so that we synchronously call
  // JS::OptimizedEncodingListener::storeOptimizedEncoding().
  compileArgs->baselineEnabled = false;
  compileArgs->forceTiering = false;

  // We always pick Ion here, and we depend on CodeCachingAvailable() having
  // determined that Ion is available, see comments at CodeCachingAvailable().
  // To do better, we need to pass information about which compiler that should
  // be used into CompileAndSerialize().
  compileArgs->ionEnabled = true;

  // Select features that are enabled. This is guaranteed to be consistent with
  // our compiler selection, as code caching is only available if ion is
  // available, and ion is only available if it's not disabled by enabled
  // features.
  compileArgs->features = FeatureArgs::build(cx, FeatureOptions());

  SerializeListener listener(serialized);

  UniqueChars error;
  UniqueCharsVector warnings;
  SharedModule module =
      CompileBuffer(*compileArgs, BytecodeBufferOrSource(bytecodeSource),
                    &error, &warnings, &listener);
  if (!module) {
    fprintf(stderr, "Compilation error: %s\n", error ? error.get() : "oom");
    return false;
  }

  MOZ_ASSERT(module->code().hasCompleteTier(Tier::Serialized));
  MOZ_ASSERT(listener.called);
  return !listener.serialized->empty();
}

bool wasm::DeserializeModule(JSContext* cx, const Bytes& serialized,
                             MutableHandleObject moduleObj) {
  MutableModule module =
      Module::deserialize(serialized.begin(), serialized.length());
  if (!module) {
    ReportOutOfMemory(cx);
    return false;
  }

  moduleObj.set(module->createObject(cx));
  return !!moduleObj;
}

// ============================================================================
// Common functions

// '[EnforceRange] unsigned long' types are coerced with
//    ConvertToInt(v, 32, 'unsigned')
// defined in Web IDL Section 3.2.4.9.
//
// This just generalizes that to an arbitrary limit that is representable as an
// integer in double form.

static bool EnforceRange(JSContext* cx, HandleValue v, const char* kind,
                         const char* noun, uint64_t max, uint64_t* val) {
  // Step 4.
  double x;
  if (!ToNumber(cx, v, &x)) {
    return false;
  }

  // Step 5.
  if (mozilla::IsNegativeZero(x)) {
    x = 0.0;
  }

  // Step 6.1.
  if (!std::isfinite(x)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_ENFORCE_RANGE, kind, noun);
    return false;
  }

  // Step 6.2.
  x = JS::ToInteger(x);

  // Step 6.3.
  if (x < 0 || x > double(max)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_ENFORCE_RANGE, kind, noun);
    return false;
  }

  *val = uint64_t(x);
  MOZ_ASSERT(double(*val) == x);
  return true;
}

static bool EnforceRangeU32(JSContext* cx, HandleValue v, const char* kind,
                            const char* noun, uint32_t* u32) {
  uint64_t u64 = 0;
  if (!EnforceRange(cx, v, kind, noun, uint64_t(UINT32_MAX), &u64)) {
    return false;
  }
  *u32 = uint32_t(u64);
  return true;
}

static bool EnforceRangeU64(JSContext* cx, HandleValue v, const char* kind,
                            const char* noun, uint64_t* u64) {
  // The max is Number.MAX_SAFE_INTEGER
  return EnforceRange(cx, v, kind, noun, (1LL << 53) - 1, u64);
}

static bool EnforceRangeBigInt64(JSContext* cx, HandleValue v, const char* kind,
                                 const char* noun, uint64_t* u64) {
  RootedBigInt bi(cx, ToBigInt(cx, v));
  if (!bi) {
    return false;
  }
  if (!BigInt::isUint64(bi, u64)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_ENFORCE_RANGE, kind, noun);
    return false;
  }
  return true;
}

static bool EnforceAddressValue(JSContext* cx, HandleValue v,
                                AddressType addressType, const char* kind,
                                const char* noun, uint64_t* result) {
  switch (addressType) {
    case AddressType::I32: {
      uint32_t result32;
      if (!EnforceRangeU32(cx, v, kind, noun, &result32)) {
        return false;
      }
      *result = uint64_t(result32);
      return true;
    }
    case AddressType::I64:
      return EnforceRangeBigInt64(cx, v, kind, noun, result);
    default:
      MOZ_CRASH("unknown address type");
  }
}

// The AddressValue typedef, a union of number and bigint, is used in the JS API
// spec for memory and table arguments, where number is used for memory32 and
// bigint is used for memory64.
[[nodiscard]] static bool CreateAddressValue(JSContext* cx, uint64_t value,
                                             AddressType addressType,
                                             MutableHandleValue addressValue) {
  switch (addressType) {
    case AddressType::I32:
      MOZ_ASSERT(value <= UINT32_MAX);
      addressValue.set(NumberValue(value));
      return true;
    case AddressType::I64: {
      BigInt* bi = BigInt::createFromUint64(cx, value);
      if (!bi) {
        return false;
      }
      addressValue.set(BigIntValue(bi));
      return true;
    }
    default:
      MOZ_CRASH("unknown address type");
  }
}

// Gets an AddressValue property ("initial" or "maximum") from a
// MemoryDescriptor or TableDescriptor. The values returned by this should be
// run through CheckLimits to enforce the validation limits prescribed by the
// spec.
static bool GetDescriptorAddressValue(JSContext* cx, HandleObject obj,
                                      const char* name, const char* noun,
                                      const char* msg, AddressType addressType,
                                      bool* found, uint64_t* value) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));

  RootedValue val(cx);
  if (!GetProperty(cx, obj, obj, id, &val)) {
    return false;
  }

  if (val.isUndefined()) {
    *found = false;
    return true;
  }
  *found = true;

  return EnforceAddressValue(cx, val, addressType, noun, msg, value);
}

static bool GetLimits(JSContext* cx, HandleObject obj, LimitsKind kind,
                      Limits* limits) {
  limits->addressType = AddressType::I32;

  // Limits may specify an alternate address type, and we need this to check the
  // ranges for initial and maximum, so look for the address type first.
#ifdef ENABLE_WASM_MEMORY64
  // Get the address type field
  JSAtom* addressTypeAtom = Atomize(cx, "address", strlen("address"));
  if (!addressTypeAtom) {
    return false;
  }
  RootedId addressTypeId(cx, AtomToId(addressTypeAtom));
  RootedValue addressTypeVal(cx);
  if (!GetProperty(cx, obj, obj, addressTypeId, &addressTypeVal)) {
    return false;
  }

  // The address type has a default value
  if (!addressTypeVal.isUndefined()) {
    if (!ToAddressType(cx, addressTypeVal, &limits->addressType)) {
      return false;
    }

    if (limits->addressType == AddressType::I64 && !Memory64Available(cx)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_WASM_NO_MEM64_LINK);
      return false;
    }
  }
#endif

  const char* noun = ToString(kind);
  uint64_t limit = 0;

  bool haveInitial = false;
  if (!GetDescriptorAddressValue(cx, obj, "initial", noun, "initial size",
                                 limits->addressType, &haveInitial, &limit)) {
    return false;
  }
  if (haveInitial) {
    limits->initial = limit;
  }

  bool haveMinimum = false;
#ifdef ENABLE_WASM_TYPE_REFLECTIONS
  if (!GetDescriptorAddressValue(cx, obj, "minimum", noun, "initial size",
                                 limits->addressType, &haveMinimum, &limit)) {
    return false;
  }
  if (haveMinimum) {
    limits->initial = limit;
  }
#endif

  if (!(haveInitial || haveMinimum)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_MISSING_REQUIRED, "initial");
    return false;
  }
  if (haveInitial && haveMinimum) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_SUPPLY_ONLY_ONE, "minimum", "initial");
    return false;
  }

  bool haveMaximum = false;
  if (!GetDescriptorAddressValue(cx, obj, "maximum", noun, "maximum size",
                                 limits->addressType, &haveMaximum, &limit)) {
    return false;
  }
  if (haveMaximum) {
    limits->maximum = Some(limit);
  }

  limits->shared = Shareable::False;

  // Memory limits may be shared.
  if (kind == LimitsKind::Memory) {
    // Get the shared field
    JSAtom* sharedAtom = Atomize(cx, "shared", strlen("shared"));
    if (!sharedAtom) {
      return false;
    }
    RootedId sharedId(cx, AtomToId(sharedAtom));

    RootedValue sharedVal(cx);
    if (!GetProperty(cx, obj, obj, sharedId, &sharedVal)) {
      return false;
    }

    // shared's default value is false, which is already the value set above.
    if (!sharedVal.isUndefined()) {
      limits->shared =
          ToBoolean(sharedVal) ? Shareable::True : Shareable::False;

      if (limits->shared == Shareable::True) {
        if (!haveMaximum) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_WASM_MISSING_MAXIMUM, noun);
          return false;
        }

        if (!cx->realm()
                 ->creationOptions()
                 .getSharedMemoryAndAtomicsEnabled()) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_WASM_NO_SHMEM_LINK);
          return false;
        }
      }
    }
  }

  return true;
}

static bool CheckLimits(JSContext* cx, uint64_t validationMax, LimitsKind kind,
                        Limits* limits) {
  const char* noun = ToString(kind);

  // There are several layers of validation and error-throwing here, including
  // one which is currently not defined by the JS API spec:
  //
  // - [EnforceRange] on parameters (must be TypeError)
  // - A check that initial <= maximum (must be RangeError)
  // - Either a mem_alloc or table_alloc operation, which has two components:
  //   - A pre-condition that the given memory or table type is valid
  //     (not specified, RangeError in practice)
  //   - The actual allocation (should report OOM if it fails)
  //
  // There are two questions currently left open by the spec: when is the memory
  // or table type validated, and if it is invalid, what type of exception does
  // it throw? In practice, all browsers throw RangeError, and by the time you
  // read this the spec will hopefully have been updated to reflect this. See
  // the following issue: https://github.com/WebAssembly/spec/issues/1792

  // Check that initial <= maximum
  if (limits->maximum.isSome() && *limits->maximum < limits->initial) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_MAX_LT_INITIAL, noun);
    return false;
  }

  // Check wasm validation limits
  if (limits->initial > validationMax) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_RANGE,
                             noun, "initial size");
    return false;
  }
  if (limits->maximum.isSome() && *limits->maximum > validationMax) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_RANGE,
                             noun, "maximum size");
    return false;
  }

  return true;
}

template <class Class, const char* name>
static JSObject* CreateWasmConstructor(JSContext* cx, JSProtoKey key) {
  Rooted<JSAtom*> className(cx, Atomize(cx, name, strlen(name)));
  if (!className) {
    return nullptr;
  }

  return NewNativeConstructor(cx, Class::construct, 1, className);
}

static JSObject* GetWasmConstructorPrototype(JSContext* cx,
                                             const CallArgs& callArgs,
                                             JSProtoKey key) {
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, callArgs, key, &proto)) {
    return nullptr;
  }
  if (!proto) {
    proto = GlobalObject::getOrCreatePrototype(cx, key);
  }
  return proto;
}

[[nodiscard]] static bool ParseValTypes(JSContext* cx, HandleValue src,
                                        ValTypeVector& dest) {
  JS::ForOfIterator iterator(cx);

  if (!iterator.init(src, JS::ForOfIterator::ThrowOnNonIterable)) {
    return false;
  }

  RootedValue nextParam(cx);
  while (true) {
    bool done;
    if (!iterator.next(&nextParam, &done)) {
      return false;
    }
    if (done) {
      break;
    }

    ValType valType;
    if (!ToValType(cx, nextParam, &valType) || !dest.append(valType)) {
      return false;
    }
  }
  return true;
}

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
template <typename T>
static JSString* TypeToString(JSContext* cx, T type) {
  UniqueChars chars = ToString(type, nullptr);
  if (!chars) {
    return nullptr;
  }
  return NewStringCopyUTF8Z(
      cx, JS::ConstUTF8CharsZ(chars.get(), strlen(chars.get())));
}

#  ifdef ENABLE_WASM_MEMORY64
static JSString* AddressTypeToString(JSContext* cx, AddressType type) {
  return JS_NewStringCopyZ(cx, ToString(type));
}
#  endif

[[nodiscard]] static JSObject* ValTypesToArray(JSContext* cx,
                                               const ValTypeVector& valTypes) {
  Rooted<ArrayObject*> arrayObj(cx, NewDenseEmptyArray(cx));
  if (!arrayObj) {
    return nullptr;
  }
  for (ValType valType : valTypes) {
    RootedString type(cx, TypeToString(cx, valType));
    if (!type) {
      return nullptr;
    }
    if (!NewbornArrayPush(cx, arrayObj, StringValue(type))) {
      return nullptr;
    }
  }
  return arrayObj;
}

static JSObject* FuncTypeToObject(JSContext* cx, const FuncType& type) {
  Rooted<IdValueVector> props(cx, IdValueVector(cx));

  RootedObject parametersObj(cx, ValTypesToArray(cx, type.args()));
  if (!parametersObj ||
      !props.append(IdValuePair(NameToId(cx->names().parameters),
                                ObjectValue(*parametersObj)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  RootedObject resultsObj(cx, ValTypesToArray(cx, type.results()));
  if (!resultsObj || !props.append(IdValuePair(NameToId(cx->names().results),
                                               ObjectValue(*resultsObj)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return NewPlainObjectWithUniqueNames(cx, props);
}

static JSObject* TableTypeToObject(JSContext* cx, AddressType addressType,
                                   RefType type, uint64_t initial,
                                   Maybe<uint64_t> maximum) {
  Rooted<IdValueVector> props(cx, IdValueVector(cx));

  RootedString elementType(cx, TypeToString(cx, type));
  if (!elementType || !props.append(IdValuePair(NameToId(cx->names().element),
                                                StringValue(elementType)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  if (maximum.isSome()) {
    RootedId maximumId(cx, NameToId(cx->names().maximum));
    RootedValue maximumValue(cx);
    if (!CreateAddressValue(cx, maximum.value(), addressType, &maximumValue)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    if (!props.append(IdValuePair(maximumId, maximumValue))) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
  }

  RootedId minimumId(cx, NameToId(cx->names().minimum));
  RootedValue minimumValue(cx);
  if (!CreateAddressValue(cx, initial, addressType, &minimumValue)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  if (!props.append(IdValuePair(minimumId, minimumValue))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

#  ifdef ENABLE_WASM_MEMORY64
  RootedString at(cx, AddressTypeToString(cx, addressType));
  if (!at) {
    return nullptr;
  }
  if (!props.append(
          IdValuePair(NameToId(cx->names().address), StringValue(at)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
#  endif

  return NewPlainObjectWithUniqueNames(cx, props);
}

static JSObject* MemoryTypeToObject(JSContext* cx, bool shared,
                                    wasm::AddressType addressType,
                                    wasm::Pages minPages,
                                    Maybe<wasm::Pages> maxPages) {
  Rooted<IdValueVector> props(cx, IdValueVector(cx));
  if (maxPages) {
    RootedId maximumId(cx, NameToId(cx->names().maximum));
    RootedValue maximumValue(cx);
    if (!CreateAddressValue(cx, maxPages.value().value(), addressType,
                            &maximumValue)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    if (!props.append(IdValuePair(maximumId, maximumValue))) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
  }

  RootedId minimumId(cx, NameToId(cx->names().minimum));
  RootedValue minimumValue(cx);
  if (!CreateAddressValue(cx, minPages.value(), addressType, &minimumValue)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  if (!props.append(IdValuePair(minimumId, minimumValue))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

#  ifdef ENABLE_WASM_MEMORY64
  RootedString at(cx, AddressTypeToString(cx, addressType));
  if (!at) {
    return nullptr;
  }
  if (!props.append(
          IdValuePair(NameToId(cx->names().address), StringValue(at)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
#  endif

  if (!props.append(
          IdValuePair(NameToId(cx->names().shared), BooleanValue(shared)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return NewPlainObjectWithUniqueNames(cx, props);
}

static JSObject* GlobalTypeToObject(JSContext* cx, ValType type,
                                    bool isMutable) {
  Rooted<IdValueVector> props(cx, IdValueVector(cx));

  if (!props.append(IdValuePair(NameToId(cx->names().mutable_),
                                BooleanValue(isMutable)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  RootedString valueType(cx, TypeToString(cx, type));
  if (!valueType || !props.append(IdValuePair(NameToId(cx->names().value),
                                              StringValue(valueType)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return NewPlainObjectWithUniqueNames(cx, props);
}

static JSObject* TagTypeToObject(JSContext* cx,
                                 const wasm::ValTypeVector& params) {
  Rooted<IdValueVector> props(cx, IdValueVector(cx));

  RootedObject parametersObj(cx, ValTypesToArray(cx, params));
  if (!parametersObj ||
      !props.append(IdValuePair(NameToId(cx->names().parameters),
                                ObjectValue(*parametersObj)))) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return NewPlainObjectWithUniqueNames(cx, props);
}
#endif  // ENABLE_WASM_TYPE_REFLECTIONS

// ============================================================================
// WebAssembly.Module class and methods

const JSClassOps WasmModuleObject::classOps_ = {
    nullptr,                     // addProperty
    nullptr,                     // delProperty
    nullptr,                     // enumerate
    nullptr,                     // newEnumerate
    nullptr,                     // resolve
    nullptr,                     // mayResolve
    WasmModuleObject::finalize,  // finalize
    nullptr,                     // call
    nullptr,                     // construct
    nullptr,                     // trace
};

const JSClass WasmModuleObject::class_ = {
    "WebAssembly.Module",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(WasmModuleObject::RESERVED_SLOTS) |
        JSCLASS_FOREGROUND_FINALIZE,
    &WasmModuleObject::classOps_,
    &WasmModuleObject::classSpec_,
};

const JSClass& WasmModuleObject::protoClass_ = PlainObject::class_;

static constexpr char WasmModuleName[] = "Module";

const ClassSpec WasmModuleObject::classSpec_ = {
    CreateWasmConstructor<WasmModuleObject, WasmModuleName>,
    GenericCreatePrototype<WasmModuleObject>,
    WasmModuleObject::static_methods,
    nullptr,
    WasmModuleObject::methods,
    WasmModuleObject::properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

const JSPropertySpec WasmModuleObject::properties[] = {
    JS_STRING_SYM_PS(toStringTag, "WebAssembly.Module", JSPROP_READONLY),
    JS_PS_END,
};

const JSFunctionSpec WasmModuleObject::methods[] = {
    JS_FS_END,
};

const JSFunctionSpec WasmModuleObject::static_methods[] = {
    JS_FN("imports", WasmModuleObject::imports, 1, JSPROP_ENUMERATE),
    JS_FN("exports", WasmModuleObject::exports, 1, JSPROP_ENUMERATE),
    JS_FN("customSections", WasmModuleObject::customSections, 2,
          JSPROP_ENUMERATE),
    JS_FS_END,
};

/* static */
void WasmModuleObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  const Module& module = obj->as<WasmModuleObject>().module();
  size_t codeMemory = module.tier1CodeMemoryUsed();
  if (codeMemory) {
    obj->zone()->decJitMemory(codeMemory);
  }
  gcx->release(obj, &module, module.gcMallocBytesExcludingCode(),
               MemoryUse::WasmModule);
}

static bool IsModuleObject(JSObject* obj, const Module** module) {
  WasmModuleObject* mobj = obj->maybeUnwrapIf<WasmModuleObject>();
  if (!mobj) {
    return false;
  }

  *module = &mobj->module();
  return true;
}

static bool GetModuleArg(JSContext* cx, const CallArgs& args,
                         uint32_t numRequired, const char* name,
                         const Module** module) {
  if (!args.requireAtLeast(cx, name, numRequired)) {
    return false;
  }

  if (!args[0].isObject() || !IsModuleObject(&args[0].toObject(), module)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_MOD_ARG);
    return false;
  }

  return true;
}

struct KindNames {
  Rooted<PropertyName*> kind;
  Rooted<PropertyName*> table;
  Rooted<PropertyName*> memory;
  Rooted<PropertyName*> tag;
  Rooted<PropertyName*> type;

  explicit KindNames(JSContext* cx)
      : kind(cx), table(cx), memory(cx), tag(cx), type(cx) {}
};

static bool InitKindNames(JSContext* cx, KindNames* names) {
  JSAtom* kind = Atomize(cx, "kind", strlen("kind"));
  if (!kind) {
    return false;
  }
  names->kind = kind->asPropertyName();

  JSAtom* table = Atomize(cx, "table", strlen("table"));
  if (!table) {
    return false;
  }
  names->table = table->asPropertyName();

  JSAtom* memory = Atomize(cx, "memory", strlen("memory"));
  if (!memory) {
    return false;
  }
  names->memory = memory->asPropertyName();

  JSAtom* tag = Atomize(cx, "tag", strlen("tag"));
  if (!tag) {
    return false;
  }
  names->tag = tag->asPropertyName();

  JSAtom* type = Atomize(cx, "type", strlen("type"));
  if (!type) {
    return false;
  }
  names->type = type->asPropertyName();

  return true;
}

static JSString* KindToString(JSContext* cx, const KindNames& names,
                              DefinitionKind kind) {
  switch (kind) {
    case DefinitionKind::Function:
      return cx->names().function;
    case DefinitionKind::Table:
      return names.table;
    case DefinitionKind::Memory:
      return names.memory;
    case DefinitionKind::Global:
      return cx->names().global;
    case DefinitionKind::Tag:
      return names.tag;
  }

  MOZ_CRASH("invalid kind");
}

/* static */
bool WasmModuleObject::imports(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  const Module* module;
  if (!GetModuleArg(cx, args, 1, "WebAssembly.Module.imports", &module)) {
    return false;
  }

  KindNames names(cx);
  if (!InitKindNames(cx, &names)) {
    return false;
  }

  const ModuleMetadata& moduleMeta = module->moduleMeta();

  RootedValueVector elems(cx);
  if (!elems.reserve(moduleMeta.imports.length())) {
    return false;
  }

#if defined(ENABLE_WASM_JS_STRING_BUILTINS) || \
    defined(ENABLE_WASM_TYPE_REFLECTIONS)
  const CodeMetadata& codeMeta = module->codeMeta();
#endif

#if defined(ENABLE_WASM_TYPE_REFLECTIONS)
  size_t numFuncImport = 0;
  size_t numMemoryImport = 0;
  size_t numGlobalImport = 0;
  size_t numTableImport = 0;
  size_t numTagImport = 0;
#endif  // ENABLE_WASM_TYPE_REFLECTIONS

  for (const Import& import : moduleMeta.imports) {
#ifdef ENABLE_WASM_JS_STRING_BUILTINS
    Maybe<BuiltinModuleId> builtinModule = ImportMatchesBuiltinModule(
        import.module.utf8Bytes(), codeMeta.features().builtinModules);
    if (builtinModule) {
      continue;
    }
#endif

    Rooted<IdValueVector> props(cx, IdValueVector(cx));
    if (!props.reserve(3)) {
      return false;
    }

    JSString* moduleStr = import.module.toAtom(cx);
    if (!moduleStr) {
      return false;
    }
    props.infallibleAppend(
        IdValuePair(NameToId(cx->names().module), StringValue(moduleStr)));

    JSString* nameStr = import.field.toAtom(cx);
    if (!nameStr) {
      return false;
    }
    props.infallibleAppend(
        IdValuePair(NameToId(cx->names().name), StringValue(nameStr)));

    JSString* kindStr = KindToString(cx, names, import.kind);
    if (!kindStr) {
      return false;
    }
    props.infallibleAppend(
        IdValuePair(NameToId(names.kind), StringValue(kindStr)));

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
    RootedObject typeObj(cx);
    switch (import.kind) {
      case DefinitionKind::Function: {
        size_t funcIndex = numFuncImport++;
        const FuncType& funcType = codeMeta.getFuncType(funcIndex);
        typeObj = FuncTypeToObject(cx, funcType);
        break;
      }
      case DefinitionKind::Table: {
        size_t tableIndex = numTableImport++;
        const TableDesc& table = codeMeta.tables[tableIndex];
        typeObj =
            TableTypeToObject(cx, table.addressType(), table.elemType,
                              table.initialLength(), table.maximumLength());
        break;
      }
      case DefinitionKind::Memory: {
        size_t memoryIndex = numMemoryImport++;
        const MemoryDesc& memory = codeMeta.memories[memoryIndex];
        typeObj =
            MemoryTypeToObject(cx, memory.isShared(), memory.addressType(),
                               memory.initialPages(), memory.maximumPages());
        break;
      }
      case DefinitionKind::Global: {
        size_t globalIndex = numGlobalImport++;
        const GlobalDesc& global = codeMeta.globals[globalIndex];
        typeObj = GlobalTypeToObject(cx, global.type(), global.isMutable());
        break;
      }
      case DefinitionKind::Tag: {
        size_t tagIndex = numTagImport++;
        const TagDesc& tag = codeMeta.tags[tagIndex];
        typeObj = TagTypeToObject(cx, tag.type->argTypes());
        break;
      }
    }

    if (!typeObj || !props.append(IdValuePair(NameToId(names.type),
                                              ObjectValue(*typeObj)))) {
      ReportOutOfMemory(cx);
      return false;
    }
#endif  // ENABLE_WASM_TYPE_REFLECTIONS

    JSObject* obj = NewPlainObjectWithUniqueNames(cx, props);
    if (!obj) {
      return false;
    }

    elems.infallibleAppend(ObjectValue(*obj));
  }

  JSObject* arr = NewDenseCopiedArray(cx, elems.length(), elems.begin());
  if (!arr) {
    return false;
  }

  args.rval().setObject(*arr);
  return true;
}

/* static */
bool WasmModuleObject::exports(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  const Module* module;
  if (!GetModuleArg(cx, args, 1, "WebAssembly.Module.exports", &module)) {
    return false;
  }

  KindNames names(cx);
  if (!InitKindNames(cx, &names)) {
    return false;
  }

  const ModuleMetadata& moduleMeta = module->moduleMeta();

  RootedValueVector elems(cx);
  if (!elems.reserve(moduleMeta.exports.length())) {
    return false;
  }

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
  const CodeMetadata& codeMeta = module->codeMeta();
#endif  // ENABLE_WASM_TYPE_REFLECTIONS

  for (const Export& exp : moduleMeta.exports) {
    Rooted<IdValueVector> props(cx, IdValueVector(cx));
    if (!props.reserve(2)) {
      return false;
    }

    JSString* nameStr = exp.fieldName().toAtom(cx);
    if (!nameStr) {
      return false;
    }
    props.infallibleAppend(
        IdValuePair(NameToId(cx->names().name), StringValue(nameStr)));

    JSString* kindStr = KindToString(cx, names, exp.kind());
    if (!kindStr) {
      return false;
    }
    props.infallibleAppend(
        IdValuePair(NameToId(names.kind), StringValue(kindStr)));

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
    RootedObject typeObj(cx);
    switch (exp.kind()) {
      case DefinitionKind::Function: {
        const FuncType& funcType =
            module->codeMeta().getFuncType(exp.funcIndex());
        typeObj = FuncTypeToObject(cx, funcType);
        break;
      }
      case DefinitionKind::Table: {
        const TableDesc& table = codeMeta.tables[exp.tableIndex()];
        typeObj =
            TableTypeToObject(cx, table.addressType(), table.elemType,
                              table.initialLength(), table.maximumLength());
        break;
      }
      case DefinitionKind::Memory: {
        const MemoryDesc& memory = codeMeta.memories[exp.memoryIndex()];
        typeObj =
            MemoryTypeToObject(cx, memory.isShared(), memory.addressType(),
                               memory.initialPages(), memory.maximumPages());
        break;
      }
      case DefinitionKind::Global: {
        const GlobalDesc& global = codeMeta.globals[exp.globalIndex()];
        typeObj = GlobalTypeToObject(cx, global.type(), global.isMutable());
        break;
      }
      case DefinitionKind::Tag: {
        const TagDesc& tag = codeMeta.tags[exp.tagIndex()];
        typeObj = TagTypeToObject(cx, tag.type->argTypes());
        break;
      }
    }

    if (!typeObj || !props.append(IdValuePair(NameToId(names.type),
                                              ObjectValue(*typeObj)))) {
      ReportOutOfMemory(cx);
      return false;
    }
#endif  // ENABLE_WASM_TYPE_REFLECTIONS

    JSObject* obj = NewPlainObjectWithUniqueNames(cx, props);
    if (!obj) {
      return false;
    }

    elems.infallibleAppend(ObjectValue(*obj));
  }

  JSObject* arr = NewDenseCopiedArray(cx, elems.length(), elems.begin());
  if (!arr) {
    return false;
  }

  args.rval().setObject(*arr);
  return true;
}

/* static */
bool WasmModuleObject::customSections(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  const Module* module;
  if (!GetModuleArg(cx, args, 2, "WebAssembly.Module.customSections",
                    &module)) {
    return false;
  }

  Vector<char, 8> name(cx);
  {
    RootedString str(cx, ToString(cx, args.get(1)));
    if (!str) {
      return false;
    }

    Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
    if (!linear) {
      return false;
    }

    if (!name.initLengthUninitialized(
            JS::GetDeflatedUTF8StringLength(linear))) {
      return false;
    }

    (void)JS::DeflateStringToUTF8Buffer(linear,
                                        Span(name.begin(), name.length()));
  }

  RootedValueVector elems(cx);
  Rooted<ArrayBufferObject*> buf(cx);
  for (const CustomSection& cs : module->moduleMeta().customSections) {
    if (name.length() != cs.name.length()) {
      continue;
    }
    if (memcmp(name.begin(), cs.name.begin(), name.length()) != 0) {
      continue;
    }

    buf = ArrayBufferObject::createZeroed(cx, cs.payload->length());
    if (!buf) {
      return false;
    }

    memcpy(buf->dataPointer(), cs.payload->begin(), cs.payload->length());
    if (!elems.append(ObjectValue(*buf))) {
      return false;
    }
  }

  JSObject* arr = NewDenseCopiedArray(cx, elems.length(), elems.begin());
  if (!arr) {
    return false;
  }

  args.rval().setObject(*arr);
  return true;
}

/* static */
WasmModuleObject* WasmModuleObject::create(JSContext* cx, const Module& module,
                                           HandleObject proto) {
  AutoSetNewObjectMetadata metadata(cx);
  auto* obj = NewObjectWithGivenProto<WasmModuleObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  // The pipeline state on some architectures may retain stale instructions
  // even after we invalidate the instruction cache. There is no generally
  // available method to broadcast this pipeline flush to all threads after
  // we've compiled new code, so conservatively perform one here when we're
  // receiving a module that may have been compiled from another thread.
  //
  // The cost of this flush is expected to minimal enough to not be worth
  // optimizing away in the case the module was compiled on this thread.
  jit::FlushExecutionContext();

  // This accounts for module allocation size (excluding code which is handled
  // separately - see below). This assumes that the size of associated data
  // doesn't change for the life of the WasmModuleObject. The size is counted
  // once per WasmModuleObject referencing a Module.
  InitReservedSlot(obj, MODULE_SLOT, const_cast<Module*>(&module),
                   module.gcMallocBytesExcludingCode(), MemoryUse::WasmModule);
  module.AddRef();

  // Bug 1569888: We account for the first tier here; the second tier, if
  // different, also needs to be accounted for.
  size_t codeMemory = module.tier1CodeMemoryUsed();
  if (codeMemory) {
    cx->zone()->incJitMemory(codeMemory);
  }
  return obj;
}

struct MOZ_STACK_CLASS AutoPinBufferSourceLength {
  explicit AutoPinBufferSourceLength(JSContext* cx, JSObject* bufferSource)
      : bufferSource_(cx, bufferSource),
        wasPinned_(!JS::PinArrayBufferOrViewLength(bufferSource_, true)) {}
  ~AutoPinBufferSourceLength() {
    if (!wasPinned_) {
      JS::PinArrayBufferOrViewLength(bufferSource_, false);
    }
  }

 private:
  Rooted<JSObject*> bufferSource_;
  bool wasPinned_;
};

static bool GetBytecodeSource(JSContext* cx, Handle<JSObject*> obj,
                              unsigned errorNumber, BytecodeSource* bytecode) {
  JSObject* unwrapped = CheckedUnwrapStatic(obj);

  SharedMem<uint8_t*> dataPointer;
  size_t byteLength;
  if (!unwrapped ||
      !IsBufferSource(cx, unwrapped, /*allowShared*/ false,
                      /*allowResizable*/ false, &dataPointer, &byteLength)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber);
    return false;
  }

  *bytecode = BytecodeSource(dataPointer.unwrap(), byteLength);
  return true;
}

static bool GetBytecodeBuffer(JSContext* cx, Handle<JSObject*> obj,
                              unsigned errorNumber, BytecodeBuffer* bytecode) {
  BytecodeSource source;
  if (!GetBytecodeSource(cx, obj, errorNumber, &source)) {
    return false;
  }
  AutoPinBufferSourceLength pin(cx, obj);
  if (!BytecodeBuffer::fromSource(source, bytecode)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

static bool ReportCompileWarnings(JSContext* cx,
                                  const UniqueCharsVector& warnings) {
  // Avoid spamming the console.
  size_t numWarnings = std::min<size_t>(warnings.length(), 3);

  for (size_t i = 0; i < numWarnings; i++) {
    if (!WarnNumberASCII(cx, JSMSG_WASM_COMPILE_WARNING, warnings[i].get())) {
      return false;
    }
  }

  if (warnings.length() > numWarnings) {
    if (!WarnNumberASCII(cx, JSMSG_WASM_COMPILE_WARNING,
                         "other warnings suppressed")) {
      return false;
    }
  }

  return true;
}

/* static */
bool WasmModuleObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs callArgs = CallArgsFromVp(argc, vp);

  Log(cx, "sync new Module() started");

  if (!ThrowIfNotConstructing(cx, callArgs, "Module")) {
    return false;
  }

  JS::RootedVector<JSString*> parameterStrings(cx);
  JS::RootedVector<Value> parameterArgs(cx);
  bool canCompileStrings = false;
  if (!cx->isRuntimeCodeGenEnabled(JS::RuntimeCode::WASM, nullptr,
                                   JS::CompilationType::Undefined,
                                   parameterStrings, nullptr, parameterArgs,
                                   NullHandleValue, &canCompileStrings)) {
    return false;
  }
  if (!canCompileStrings) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_WASM, "WebAssembly.Module");
    return false;
  }

  if (!callArgs.requireAtLeast(cx, "WebAssembly.Module", 1)) {
    return false;
  }

  if (!callArgs[0].isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_BUF_ARG);
    return false;
  }

  FeatureOptions options;
  if (!options.init(cx, callArgs.get(1))) {
    return false;
  }

  SharedCompileArgs compileArgs =
      InitCompileArgs(cx, options, "WebAssembly.Module");
  if (!compileArgs) {
    return false;
  }

  BytecodeSource source;
  Rooted<JSObject*> sourceObj(cx, &callArgs[0].toObject());
  if (!GetBytecodeSource(cx, sourceObj, JSMSG_WASM_BAD_BUF_ARG, &source)) {
    return false;
  }

  UniqueChars error;
  UniqueCharsVector warnings;
  SharedModule module;
  {
    AutoPinBufferSourceLength pin(cx, sourceObj.get());
    module = CompileBuffer(*compileArgs, BytecodeBufferOrSource(source), &error,
                           &warnings, nullptr);
  }

  if (!ReportCompileWarnings(cx, warnings)) {
    return false;
  }
  if (!module) {
    if (error) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_COMPILE_ERROR, error.get());
      return false;
    }
    return ThrowCompileOutOfMemory(cx);
  }

  RootedObject proto(
      cx, GetWasmConstructorPrototype(cx, callArgs, JSProto_WasmModule));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }

  RootedObject moduleObj(cx, WasmModuleObject::create(cx, *module, proto));
  if (!moduleObj) {
    return false;
  }

  Log(cx, "sync new Module() succeded");

  callArgs.rval().setObject(*moduleObj);
  return true;
}

const Module& WasmModuleObject::module() const {
  MOZ_ASSERT(is<WasmModuleObject>());
  return *(const Module*)getReservedSlot(MODULE_SLOT).toPrivate();
}

// ============================================================================
// WebAssembly.Instance class and methods

const JSClassOps WasmInstanceObject::classOps_ = {
    nullptr,                       // addProperty
    nullptr,                       // delProperty
    nullptr,                       // enumerate
    nullptr,                       // newEnumerate
    nullptr,                       // resolve
    nullptr,                       // mayResolve
    WasmInstanceObject::finalize,  // finalize
    nullptr,                       // call
    nullptr,                       // construct
    WasmInstanceObject::trace,     // trace
};

const JSClass WasmInstanceObject::class_ = {
    "WebAssembly.Instance",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(WasmInstanceObject::RESERVED_SLOTS) |
        JSCLASS_FOREGROUND_FINALIZE,
    &WasmInstanceObject::classOps_,
    &WasmInstanceObject::classSpec_,
};

const JSClass& WasmInstanceObject::protoClass_ = PlainObject::class_;

static constexpr char WasmInstanceName[] = "Instance";

const ClassSpec WasmInstanceObject::classSpec_ = {
    CreateWasmConstructor<WasmInstanceObject, WasmInstanceName>,
    GenericCreatePrototype<WasmInstanceObject>,
    WasmInstanceObject::static_methods,
    nullptr,
    WasmInstanceObject::methods,
    WasmInstanceObject::properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

static bool IsInstance(HandleValue v) {
  return v.isObject() && v.toObject().is<WasmInstanceObject>();
}

/* static */
bool WasmInstanceObject::exportsGetterImpl(JSContext* cx,
                                           const CallArgs& args) {
  args.rval().setObject(
      args.thisv().toObject().as<WasmInstanceObject>().exportsObj());
  return true;
}

/* static */
bool WasmInstanceObject::exportsGetter(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstance, exportsGetterImpl>(cx, args);
}

const JSPropertySpec WasmInstanceObject::properties[] = {
    JS_PSG("exports", WasmInstanceObject::exportsGetter, JSPROP_ENUMERATE),
    JS_STRING_SYM_PS(toStringTag, "WebAssembly.Instance", JSPROP_READONLY),
    JS_PS_END,
};

const JSFunctionSpec WasmInstanceObject::methods[] = {
    JS_FS_END,
};

const JSFunctionSpec WasmInstanceObject::static_methods[] = {
    JS_FS_END,
};

bool WasmInstanceObject::isNewborn() const {
  MOZ_ASSERT(is<WasmInstanceObject>());
  return getReservedSlot(INSTANCE_SLOT).isUndefined();
}

// WeakScopeMap maps from function index to js::Scope. This maps is weak
// to avoid holding scope objects alive. The scopes are normally created
// during debugging.
//
// This is defined here in order to avoid recursive dependency between
// WasmJS.h and Scope.h.
using WasmFunctionScopeMap =
    JS::WeakCache<GCHashMap<uint32_t, WeakHeapPtr<WasmFunctionScope*>,
                            DefaultHasher<uint32_t>, CellAllocPolicy>>;
class WasmInstanceObject::UnspecifiedScopeMap {
 public:
  WasmFunctionScopeMap& asWasmFunctionScopeMap() {
    return *(WasmFunctionScopeMap*)this;
  }
};

/* static */
void WasmInstanceObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  WasmInstanceObject& instance = obj->as<WasmInstanceObject>();
  gcx->delete_(obj, &instance.scopes().asWasmFunctionScopeMap(),
               MemoryUse::WasmInstanceScopes);
  gcx->delete_(obj, &instance.indirectGlobals(),
               MemoryUse::WasmInstanceGlobals);
  if (!instance.isNewborn()) {
    if (instance.instance().debugEnabled()) {
      instance.instance().debug().finalize(gcx);
    }
    Instance::destroy(&instance.instance());
    gcx->removeCellMemory(obj, sizeof(Instance),
                          MemoryUse::WasmInstanceInstance);
  }
}

/* static */
void WasmInstanceObject::trace(JSTracer* trc, JSObject* obj) {
  WasmInstanceObject& instanceObj = obj->as<WasmInstanceObject>();
  instanceObj.indirectGlobals().trace(trc);
  if (!instanceObj.isNewborn()) {
    instanceObj.instance().tracePrivate(trc);
  }
}

/* static */
WasmInstanceObject* WasmInstanceObject::create(
    JSContext* cx, const SharedCode& code,
    const DataSegmentVector& dataSegments,
    const ModuleElemSegmentVector& elemSegments, uint32_t instanceDataLength,
    Handle<WasmMemoryObjectVector> memories, SharedTableVector&& tables,
    const JSObjectVector& funcImports, const GlobalDescVector& globals,
    const ValVector& globalImportValues,
    const WasmGlobalObjectVector& globalObjs,
    const WasmTagObjectVector& tagObjs, HandleObject proto,
    UniqueDebugState maybeDebug) {
  UniquePtr<WasmFunctionScopeMap> scopes =
      js::MakeUnique<WasmFunctionScopeMap>(cx->zone(), cx->zone());
  if (!scopes) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  // Note that `scopes` is a WeakCache, auto-linked into a sweep list on the
  // Zone, and so does not require rooting.

  uint32_t indirectGlobals = 0;

  for (uint32_t i = 0; i < globalObjs.length(); i++) {
    if (globalObjs[i] && globals[i].isIndirect()) {
      indirectGlobals++;
    }
  }

  Rooted<UniquePtr<GlobalObjectVector>> indirectGlobalObjs(
      cx, js::MakeUnique<GlobalObjectVector>(cx->zone()));
  if (!indirectGlobalObjs || !indirectGlobalObjs->resize(indirectGlobals)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  {
    uint32_t next = 0;
    for (uint32_t i = 0; i < globalObjs.length(); i++) {
      if (globalObjs[i] && globals[i].isIndirect()) {
        (*indirectGlobalObjs)[next++] = globalObjs[i];
      }
    }
  }

  Instance* instance = nullptr;
  Rooted<WasmInstanceObject*> obj(cx);

  {
    // We must delay creating metadata for this object until after all its
    // slots have been initialized. We must also create the metadata before
    // calling Instance::init as that may allocate new objects.
    AutoSetNewObjectMetadata metadata(cx);
    obj = NewObjectWithGivenProto<WasmInstanceObject>(cx, proto);
    if (!obj) {
      return nullptr;
    }

    MOZ_ASSERT(obj->isTenured(), "assumed by WasmTableObject write barriers");

    InitReservedSlot(obj, SCOPES_SLOT, scopes.release(),
                     MemoryUse::WasmInstanceScopes);

    InitReservedSlot(obj, GLOBALS_SLOT, indirectGlobalObjs.release(),
                     MemoryUse::WasmInstanceGlobals);

    obj->initReservedSlot(INSTANCE_SCOPE_SLOT, UndefinedValue());

    // The INSTANCE_SLOT may not be initialized if Instance allocation fails,
    // leading to an observable "newborn" state in tracing/finalization.
    MOZ_ASSERT(obj->isNewborn());

    // Create this just before constructing Instance to avoid rooting hazards.
    instance = Instance::create(cx, obj, code, instanceDataLength,
                                std::move(tables), std::move(maybeDebug));
    if (!instance) {
      return nullptr;
    }

    InitReservedSlot(obj, INSTANCE_SLOT, instance,
                     MemoryUse::WasmInstanceInstance);
    MOZ_ASSERT(!obj->isNewborn());
  }

  if (!instance->init(cx, funcImports, globalImportValues, memories, globalObjs,
                      tagObjs, dataSegments, elemSegments)) {
    return nullptr;
  }

  return obj;
}

void WasmInstanceObject::initExportsObj(JSObject& exportsObj) {
  MOZ_ASSERT(getReservedSlot(EXPORTS_OBJ_SLOT).isUndefined());
  setReservedSlot(EXPORTS_OBJ_SLOT, ObjectValue(exportsObj));
}

static bool GetImportArg(JSContext* cx, HandleValue importArg,
                         MutableHandleObject importObj) {
  if (!importArg.isUndefined()) {
    if (!importArg.isObject()) {
      return ThrowBadImportArg(cx);
    }
    importObj.set(&importArg.toObject());
  }
  return true;
}

/* static */
bool WasmInstanceObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Log(cx, "sync new Instance() started");

  if (!ThrowIfNotConstructing(cx, args, "Instance")) {
    return false;
  }

  if (!args.requireAtLeast(cx, "WebAssembly.Instance", 1)) {
    return false;
  }

  const Module* module;
  if (!args[0].isObject() || !IsModuleObject(&args[0].toObject(), &module)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_MOD_ARG);
    return false;
  }

  RootedObject importObj(cx);
  if (!GetImportArg(cx, args.get(1), &importObj)) {
    return false;
  }

  RootedObject proto(
      cx, GetWasmConstructorPrototype(cx, args, JSProto_WasmInstance));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<ImportValues> imports(cx);
  if (!GetImports(cx, *module, importObj, imports.address())) {
    return false;
  }

  Rooted<WasmInstanceObject*> instanceObj(cx);
  if (!module->instantiate(cx, imports.get(), proto, &instanceObj)) {
    return false;
  }

  Log(cx, "sync new Instance() succeeded");

  args.rval().setObject(*instanceObj);
  return true;
}

Instance& WasmInstanceObject::instance() const {
  MOZ_ASSERT(!isNewborn());
  return *(Instance*)getReservedSlot(INSTANCE_SLOT).toPrivate();
}

JSObject& WasmInstanceObject::exportsObj() const {
  return getReservedSlot(EXPORTS_OBJ_SLOT).toObject();
}

WasmInstanceObject::UnspecifiedScopeMap& WasmInstanceObject::scopes() const {
  return *(UnspecifiedScopeMap*)(getReservedSlot(SCOPES_SLOT).toPrivate());
}

WasmInstanceObject::GlobalObjectVector& WasmInstanceObject::indirectGlobals()
    const {
  return *(GlobalObjectVector*)getReservedSlot(GLOBALS_SLOT).toPrivate();
}

/* static */
bool WasmInstanceObject::getExportedFunction(
    JSContext* cx, Handle<WasmInstanceObject*> instanceObj, uint32_t funcIndex,
    MutableHandleFunction fun) {
  Instance& instance = instanceObj->instance();
  return instance.getExportedFunction(cx, funcIndex, fun);
}

/* static */
WasmInstanceScope* WasmInstanceObject::getScope(
    JSContext* cx, Handle<WasmInstanceObject*> instanceObj) {
  if (!instanceObj->getReservedSlot(INSTANCE_SCOPE_SLOT).isUndefined()) {
    return (WasmInstanceScope*)instanceObj->getReservedSlot(INSTANCE_SCOPE_SLOT)
        .toGCThing();
  }

  Rooted<WasmInstanceScope*> instanceScope(
      cx, WasmInstanceScope::create(cx, instanceObj));
  if (!instanceScope) {
    return nullptr;
  }

  instanceObj->setReservedSlot(INSTANCE_SCOPE_SLOT,
                               PrivateGCThingValue(instanceScope));

  return instanceScope;
}

/* static */
WasmFunctionScope* WasmInstanceObject::getFunctionScope(
    JSContext* cx, Handle<WasmInstanceObject*> instanceObj,
    uint32_t funcIndex) {
  if (auto p =
          instanceObj->scopes().asWasmFunctionScopeMap().lookup(funcIndex)) {
    return p->value();
  }

  Rooted<WasmInstanceScope*> instanceScope(
      cx, WasmInstanceObject::getScope(cx, instanceObj));
  if (!instanceScope) {
    return nullptr;
  }

  Rooted<WasmFunctionScope*> funcScope(
      cx, WasmFunctionScope::create(cx, instanceScope, funcIndex));
  if (!funcScope) {
    return nullptr;
  }

  if (!instanceObj->scopes().asWasmFunctionScopeMap().putNew(funcIndex,
                                                             funcScope)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return funcScope;
}

// ============================================================================
// WebAssembly.Memory class and methods

const JSClassOps WasmMemoryObject::classOps_ = {
    nullptr,                     // addProperty
    nullptr,                     // delProperty
    nullptr,                     // enumerate
    nullptr,                     // newEnumerate
    nullptr,                     // resolve
    nullptr,                     // mayResolve
    WasmMemoryObject::finalize,  // finalize
    nullptr,                     // call
    nullptr,                     // construct
    nullptr,                     // trace
};

const JSClass WasmMemoryObject::class_ = {
    "WebAssembly.Memory",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(WasmMemoryObject::RESERVED_SLOTS) |
        JSCLASS_FOREGROUND_FINALIZE,
    &WasmMemoryObject::classOps_,
    &WasmMemoryObject::classSpec_,
};

const JSClass& WasmMemoryObject::protoClass_ = PlainObject::class_;

static constexpr char WasmMemoryName[] = "Memory";

static JSObject* CreateWasmMemoryPrototype(JSContext* cx, JSProtoKey key) {
  RootedObject proto(cx, GlobalObject::createBlankPrototype(
                             cx, cx->global(), &WasmMemoryObject::protoClass_));
  if (!proto) {
    return nullptr;
  }
  if (MemoryControlAvailable(cx)) {
    if (!JS_DefineFunctions(cx, proto,
                            WasmMemoryObject::memoryControlMethods)) {
      return nullptr;
    }
  }
  return proto;
}

const ClassSpec WasmMemoryObject::classSpec_ = {
    CreateWasmConstructor<WasmMemoryObject, WasmMemoryName>,
    CreateWasmMemoryPrototype,
    WasmMemoryObject::static_methods,
    nullptr,
    WasmMemoryObject::methods,
    WasmMemoryObject::properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

/* static */
void WasmMemoryObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  WasmMemoryObject& memory = obj->as<WasmMemoryObject>();
  if (memory.hasObservers()) {
    gcx->delete_(obj, &memory.observers(), MemoryUse::WasmMemoryObservers);
  }
}

/* static */
WasmMemoryObject* WasmMemoryObject::create(
    JSContext* cx, Handle<ArrayBufferObjectMaybeShared*> buffer, bool isHuge,
    HandleObject proto) {
  AutoSetNewObjectMetadata metadata(cx);
  auto* obj = NewObjectWithGivenProto<WasmMemoryObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  obj->initReservedSlot(BUFFER_SLOT, ObjectValue(*buffer));
  obj->initReservedSlot(ISHUGE_SLOT, BooleanValue(isHuge));
  MOZ_ASSERT(!obj->hasObservers());

  return obj;
}

/* static */
bool WasmMemoryObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "Memory")) {
    return false;
  }

  if (!args.requireAtLeast(cx, "WebAssembly.Memory", 1)) {
    return false;
  }

  if (!args.get(0).isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_DESC_ARG, "memory");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());
  Limits limits;
  if (!GetLimits(cx, obj, LimitsKind::Memory, &limits) ||
      !CheckLimits(cx, MaxMemoryPagesValidation(limits.addressType),
                   LimitsKind::Memory, &limits)) {
    return false;
  }

  if (Pages(limits.initial) > MaxMemoryPages(limits.addressType)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_MEM_IMP_LIMIT);
    return false;
  }
  MemoryDesc memory(limits);

  Rooted<ArrayBufferObjectMaybeShared*> buffer(cx,
                                               CreateWasmBuffer(cx, memory));
  if (!buffer) {
    return false;
  }

  RootedObject proto(cx,
                     GetWasmConstructorPrototype(cx, args, JSProto_WasmMemory));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<WasmMemoryObject*> memoryObj(
      cx, WasmMemoryObject::create(
              cx, buffer, IsHugeMemoryEnabled(limits.addressType), proto));
  if (!memoryObj) {
    return false;
  }

  args.rval().setObject(*memoryObj);
  return true;
}

static bool IsMemory(HandleValue v) {
  return v.isObject() && v.toObject().is<WasmMemoryObject>();
}

/* static */
ArrayBufferObjectMaybeShared* WasmMemoryObject::refreshBuffer(
    JSContext* cx, Handle<WasmMemoryObject*> memoryObj,
    Handle<ArrayBufferObjectMaybeShared*> buffer) {
  if (memoryObj->isShared()) {
    size_t memoryLength = memoryObj->volatileMemoryLength();
    MOZ_ASSERT_IF(!buffer->is<GrowableSharedArrayBufferObject>(),
                  memoryLength >= buffer->byteLength());

    // The `length` field on a fixed length SAB cannot change even if
    // the underlying memory has grown. The spec therefore requires that
    // accessing the buffer property will create a new fixed length SAB
    // with the current length if the underlying raw buffer's length has
    // changed. We don't need to do this for growable SAB.
    if (!buffer->is<GrowableSharedArrayBufferObject>() &&
        memoryLength > buffer->byteLength()) {
      Rooted<SharedArrayBufferObject*> newBuffer(
          cx, SharedArrayBufferObject::New(
                  cx, memoryObj->sharedArrayRawBuffer(), memoryLength));
      MOZ_ASSERT(newBuffer->is<FixedLengthSharedArrayBufferObject>());
      if (!newBuffer) {
        return nullptr;
      }
      // OK to addReference after we try to allocate because the memoryObj
      // keeps the rawBuffer alive.
      if (!memoryObj->sharedArrayRawBuffer()->addReference()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_SC_SAB_REFCNT_OFLO);
        return nullptr;
      }
      memoryObj->setReservedSlot(BUFFER_SLOT, ObjectValue(*newBuffer));
      return newBuffer;
    }
  }
  return buffer;
}

/* static */
bool WasmMemoryObject::bufferGetterImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmMemoryObject*> memoryObj(
      cx, &args.thisv().toObject().as<WasmMemoryObject>());

  Rooted<ArrayBufferObjectMaybeShared*> buffer(cx, &memoryObj->buffer());
  MOZ_RELEASE_ASSERT(buffer->isWasm() && !buffer->isPreparedForAsmJS());

  ArrayBufferObjectMaybeShared* refreshedBuffer =
      WasmMemoryObject::refreshBuffer(cx, memoryObj, buffer);
  if (!refreshedBuffer) {
    return false;
  }

  args.rval().setObject(*refreshedBuffer);
  return true;
}

/* static */
bool WasmMemoryObject::bufferGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsMemory, bufferGetterImpl>(cx, args);
}

const JSPropertySpec WasmMemoryObject::properties[] = {
    JS_PSG("buffer", WasmMemoryObject::bufferGetter, JSPROP_ENUMERATE),
    JS_STRING_SYM_PS(toStringTag, "WebAssembly.Memory", JSPROP_READONLY),
    JS_PS_END,
};

/* static */
bool WasmMemoryObject::growImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmMemoryObject*> memory(
      cx, &args.thisv().toObject().as<WasmMemoryObject>());

  if (!args.requireAtLeast(cx, "WebAssembly.Memory.grow", 1)) {
    return false;
  }

  uint64_t delta;
  if (!EnforceAddressValue(cx, args.get(0), memory->addressType(), "Memory",
                           "grow delta", &delta)) {
    return false;
  }

  uint32_t ret = grow(memory, delta, cx);

  if (ret == uint32_t(-1)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_GROW,
                             "memory");
    return false;
  }

  RootedValue result(cx);
  if (!CreateAddressValue(cx, ret, memory->addressType(), &result)) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().set(result);
  return true;
}

/* static */
bool WasmMemoryObject::grow(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsMemory, growImpl>(cx, args);
}

/* static */
bool WasmMemoryObject::discardImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmMemoryObject*> memory(
      cx, &args.thisv().toObject().as<WasmMemoryObject>());

  if (!args.requireAtLeast(cx, "WebAssembly.Memory.discard", 2)) {
    return false;
  }

  uint64_t byteOffset;
  if (!EnforceRangeU64(cx, args.get(0), "Memory", "byte offset", &byteOffset)) {
    return false;
  }

  uint64_t byteLen;
  if (!EnforceRangeU64(cx, args.get(1), "Memory", "length", &byteLen)) {
    return false;
  }

  if (byteOffset % wasm::PageSize != 0 || byteLen % wasm::PageSize != 0) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_UNALIGNED_ACCESS);
    return false;
  }

  if (!wasm::MemoryBoundsCheck(byteOffset, byteLen,
                               memory->volatileMemoryLength())) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  discard(memory, byteOffset, byteLen, cx);

  args.rval().setUndefined();
  return true;
}

/* static */
bool WasmMemoryObject::discard(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsMemory, discardImpl>(cx, args);
}

#ifdef ENABLE_WASM_RESIZABLE_ARRAYBUFFER
/* static */
bool WasmMemoryObject::toFixedLengthBufferImpl(JSContext* cx,
                                               const CallArgs& args) {
  Rooted<WasmMemoryObject*> memory(
      cx, &args.thisv().toObject().as<WasmMemoryObject>());

  Rooted<ArrayBufferObjectMaybeShared*> buffer(cx, &memory->buffer());
  MOZ_RELEASE_ASSERT(buffer->isWasm() && !buffer->isPreparedForAsmJS());
  // If IsFixedLengthArrayBuffer(buffer) is true, return buffer.
  if (!buffer->isResizable()) {
    ArrayBufferObjectMaybeShared* refreshedBuffer =
        refreshBuffer(cx, memory, buffer);
    args.rval().set(ObjectValue(*refreshedBuffer));
    return true;
  }

  Rooted<ArrayBufferObjectMaybeShared*> fixedBuffer(cx);
  if (memory->isShared()) {
    Rooted<SharedArrayBufferObject*> oldBuffer(
        cx, &buffer->as<SharedArrayBufferObject>());
    fixedBuffer.set(SharedArrayBufferObject::createFromWasmObject<
                    FixedLengthSharedArrayBufferObject>(cx, oldBuffer));
  } else {
    Rooted<ArrayBufferObject*> oldBuffer(cx, &buffer->as<ArrayBufferObject>());
    fixedBuffer.set(
        ArrayBufferObject::createFromWasmObject<FixedLengthArrayBufferObject>(
            cx, oldBuffer));
  }

  if (!fixedBuffer) {
    return false;
  }
  memory->setReservedSlot(BUFFER_SLOT, ObjectValue(*fixedBuffer));
  args.rval().set(ObjectValue(*fixedBuffer));
  return true;
}

/* static */
bool WasmMemoryObject::toFixedLengthBuffer(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsMemory, toFixedLengthBufferImpl>(cx, args);
}

/* static */
bool WasmMemoryObject::toResizableBufferImpl(JSContext* cx,
                                             const CallArgs& args) {
  Rooted<WasmMemoryObject*> memory(
      cx, &args.thisv().toObject().as<WasmMemoryObject>());

  Rooted<ArrayBufferObjectMaybeShared*> buffer(cx, &memory->buffer());
  // If IsFixedLengthArrayBuffer(buffer) is false, return buffer.
  if (buffer->isResizable()) {
    args.rval().set(ObjectValue(*buffer));
    return true;
  }

  if (buffer->wasmSourceMaxPages().isNothing()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_MEMORY_NOT_RESIZABLE);
    return false;
  }

  Rooted<ArrayBufferObjectMaybeShared*> resizableBuffer(cx);
  if (memory->isShared()) {
    Rooted<SharedArrayBufferObject*> oldBuffer(
        cx, &buffer->as<SharedArrayBufferObject>());
    resizableBuffer.set(SharedArrayBufferObject::createFromWasmObject<
                        GrowableSharedArrayBufferObject>(cx, oldBuffer));
  } else {
    Rooted<ArrayBufferObject*> oldBuffer(cx, &buffer->as<ArrayBufferObject>());
    resizableBuffer.set(
        ArrayBufferObject::createFromWasmObject<ResizableArrayBufferObject>(
            cx, oldBuffer));
  }

  if (!resizableBuffer) {
    return false;
  }
  memory->setReservedSlot(BUFFER_SLOT, ObjectValue(*resizableBuffer));
  args.rval().set(ObjectValue(*resizableBuffer));
  return true;
}

/* static */
bool WasmMemoryObject::toResizableBuffer(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsMemory, toResizableBufferImpl>(cx, args);
}
#endif  // ENABLE_WASM_RESIZABLE_ARRAYBUFFER

const JSFunctionSpec WasmMemoryObject::methods[] = {
#ifdef ENABLE_WASM_TYPE_REFLECTIONS
    JS_FN("type", WasmMemoryObject::type, 0, JSPROP_ENUMERATE),
#endif
    JS_FN("grow", WasmMemoryObject::grow, 1, JSPROP_ENUMERATE),
#ifdef ENABLE_WASM_RESIZABLE_ARRAYBUFFER
    JS_FN("toFixedLengthBuffer", WasmMemoryObject::toFixedLengthBuffer, 0,
          JSPROP_ENUMERATE),
    JS_FN("toResizableBuffer", WasmMemoryObject::toResizableBuffer, 0,
          JSPROP_ENUMERATE),
#endif
    JS_FS_END,
};

const JSFunctionSpec WasmMemoryObject::memoryControlMethods[] = {
    JS_FN("discard", WasmMemoryObject::discard, 2, JSPROP_ENUMERATE),
    JS_FS_END,
};

const JSFunctionSpec WasmMemoryObject::static_methods[] = {
    JS_FS_END,
};

ArrayBufferObjectMaybeShared& WasmMemoryObject::buffer() const {
  return getReservedSlot(BUFFER_SLOT)
      .toObject()
      .as<ArrayBufferObjectMaybeShared>();
}

WasmSharedArrayRawBuffer* WasmMemoryObject::sharedArrayRawBuffer() const {
  MOZ_ASSERT(isShared());
  return buffer().as<SharedArrayBufferObject>().rawWasmBufferObject();
}

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
bool WasmMemoryObject::typeImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmMemoryObject*> memoryObj(
      cx, &args.thisv().toObject().as<WasmMemoryObject>());
  RootedObject typeObj(cx, MemoryTypeToObject(cx, memoryObj->isShared(),
                                              memoryObj->addressType(),
                                              memoryObj->volatilePages(),
                                              memoryObj->sourceMaxPages()));
  if (!typeObj) {
    return false;
  }
  args.rval().setObject(*typeObj);
  return true;
}

bool WasmMemoryObject::type(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsMemory, typeImpl>(cx, args);
}
#endif

size_t WasmMemoryObject::volatileMemoryLength() const {
  if (isShared()) {
    return sharedArrayRawBuffer()->volatileByteLength();
  }
  return buffer().byteLength();
}

wasm::Pages WasmMemoryObject::volatilePages() const {
  if (isShared()) {
    return sharedArrayRawBuffer()->volatileWasmPages();
  }
  return buffer().wasmPages();
}

wasm::Pages WasmMemoryObject::clampedMaxPages() const {
  if (isShared()) {
    return sharedArrayRawBuffer()->wasmClampedMaxPages();
  }
  return buffer().wasmClampedMaxPages();
}

Maybe<wasm::Pages> WasmMemoryObject::sourceMaxPages() const {
  if (isShared()) {
    return Some(sharedArrayRawBuffer()->wasmSourceMaxPages());
  }
  return buffer().wasmSourceMaxPages();
}

wasm::AddressType WasmMemoryObject::addressType() const {
  if (isShared()) {
    return sharedArrayRawBuffer()->wasmAddressType();
  }
  return buffer().wasmAddressType();
}

bool WasmMemoryObject::isShared() const {
  return buffer().is<SharedArrayBufferObject>();
}

bool WasmMemoryObject::hasObservers() const {
  return !getReservedSlot(OBSERVERS_SLOT).isUndefined();
}

WasmMemoryObject::InstanceSet& WasmMemoryObject::observers() const {
  MOZ_ASSERT(hasObservers());
  return *reinterpret_cast<InstanceSet*>(
      getReservedSlot(OBSERVERS_SLOT).toPrivate());
}

WasmMemoryObject::InstanceSet* WasmMemoryObject::getOrCreateObservers(
    JSContext* cx) {
  if (!hasObservers()) {
    auto observers = MakeUnique<InstanceSet>(cx->zone(), cx->zone());
    if (!observers) {
      ReportOutOfMemory(cx);
      return nullptr;
    }

    InitReservedSlot(this, OBSERVERS_SLOT, observers.release(),
                     MemoryUse::WasmMemoryObservers);
  }

  return &observers();
}

bool WasmMemoryObject::isHuge() const {
  return getReservedSlot(ISHUGE_SLOT).toBoolean();
}

bool WasmMemoryObject::movingGrowable() const {
  return !isHuge() && !buffer().wasmSourceMaxPages();
}

size_t WasmMemoryObject::boundsCheckLimit() const {
  if (!buffer().isWasm() || isHuge()) {
    return buffer().byteLength();
  }
  size_t mappedSize = buffer().wasmMappedSize();
#if !defined(JS_64BIT)
  // See clamping performed in CreateSpecificWasmBuffer().  On 32-bit systems
  // we do not want to overflow a uint32_t.  For the other 64-bit compilers,
  // all constraints are implied by the largest accepted value for a memory's
  // max field.
  MOZ_ASSERT(mappedSize < UINT32_MAX);
#endif
  MOZ_ASSERT(mappedSize % wasm::PageSize == 0);
  MOZ_ASSERT(mappedSize >= wasm::GuardSize);
  MOZ_ASSERT(wasm::IsValidBoundsCheckImmediate(mappedSize - wasm::GuardSize));
  size_t limit = mappedSize - wasm::GuardSize;
  MOZ_ASSERT(limit <= MaxMemoryBoundsCheckLimit(addressType()));
  return limit;
}

bool WasmMemoryObject::addMovingGrowObserver(JSContext* cx,
                                             WasmInstanceObject* instance) {
  MOZ_ASSERT(movingGrowable());

  InstanceSet* observers = getOrCreateObservers(cx);
  if (!observers) {
    return false;
  }

  // A memory can be imported multiple times into an instance, but we only
  // register the instance as an observer once.
  if (!observers->put(instance)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

/* static */
uint64_t WasmMemoryObject::growShared(Handle<WasmMemoryObject*> memory,
                                      uint64_t delta) {
  WasmSharedArrayRawBuffer* rawBuf = memory->sharedArrayRawBuffer();
  WasmSharedArrayRawBuffer::Lock lock(rawBuf);

  Pages oldNumPages = rawBuf->volatileWasmPages();
  Pages newPages = oldNumPages;
  if (!newPages.checkedIncrement(Pages(delta))) {
    return uint64_t(int64_t(-1));
  }

  if (!rawBuf->wasmGrowToPagesInPlace(lock, memory->addressType(), newPages)) {
    return uint64_t(int64_t(-1));
  }
  // New buffer objects will be created lazily in all agents (including in
  // this agent) by bufferGetterImpl, above, so no more work to do here.

  return oldNumPages.value();
}

/* static */
uint64_t WasmMemoryObject::grow(Handle<WasmMemoryObject*> memory,
                                uint64_t delta, JSContext* cx) {
  if (memory->isShared()) {
    return growShared(memory, delta);
  }

  Rooted<ArrayBufferObject*> oldBuf(cx,
                                    &memory->buffer().as<ArrayBufferObject>());

#if !defined(JS_64BIT)
  // TODO (large ArrayBuffer): See more information at the definition of
  // MaxMemoryBytes().
  MOZ_ASSERT(MaxMemoryBytes(memory->addressType()) <= UINT32_MAX,
             "Avoid 32-bit overflows");
#endif

  Pages oldNumPages = oldBuf->wasmPages();
  Pages newPages = oldNumPages;
  if (!newPages.checkedIncrement(Pages(delta))) {
    return uint64_t(int64_t(-1));
  }

  ArrayBufferObject* newBuf;
  if (memory->movingGrowable()) {
    MOZ_ASSERT(!memory->isHuge());
    newBuf = ArrayBufferObject::wasmMovingGrowToPages(memory->addressType(),
                                                      newPages, oldBuf, cx);
  } else {
    newBuf = ArrayBufferObject::wasmGrowToPagesInPlace(memory->addressType(),
                                                       newPages, oldBuf, cx);
  }
  if (!newBuf) {
    return uint64_t(int64_t(-1));
  }

  memory->setReservedSlot(BUFFER_SLOT, ObjectValue(*newBuf));

  // Only notify moving-grow-observers after the BUFFER_SLOT has been updated
  // since observers will call buffer().
  if (memory->hasObservers()) {
    for (InstanceSet::Range r = memory->observers().all(); !r.empty();
         r.popFront()) {
      r.front()->instance().onMovingGrowMemory(memory);
    }
  }

  return oldNumPages.value();
}

/* static */
void WasmMemoryObject::discard(Handle<WasmMemoryObject*> memory,
                               uint64_t byteOffset, uint64_t byteLen,
                               JSContext* cx) {
  if (memory->isShared()) {
    Rooted<SharedArrayBufferObject*> buf(
        cx, &memory->buffer().as<SharedArrayBufferObject>());
    SharedArrayBufferObject::wasmDiscard(buf, byteOffset, byteLen);
  } else {
    Rooted<ArrayBufferObject*> buf(cx,
                                   &memory->buffer().as<ArrayBufferObject>());
    ArrayBufferObject::wasmDiscard(buf, byteOffset, byteLen);
  }
}

bool js::wasm::IsSharedWasmMemoryObject(JSObject* obj) {
  WasmMemoryObject* mobj = obj->maybeUnwrapIf<WasmMemoryObject>();
  return mobj && mobj->isShared();
}

// ============================================================================
// WebAssembly.Table class and methods

const JSClassOps WasmTableObject::classOps_ = {
    nullptr,                    // addProperty
    nullptr,                    // delProperty
    nullptr,                    // enumerate
    nullptr,                    // newEnumerate
    nullptr,                    // resolve
    nullptr,                    // mayResolve
    WasmTableObject::finalize,  // finalize
    nullptr,                    // call
    nullptr,                    // construct
    WasmTableObject::trace,     // trace
};

const JSClass WasmTableObject::class_ = {
    "WebAssembly.Table",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(WasmTableObject::RESERVED_SLOTS) |
        JSCLASS_FOREGROUND_FINALIZE,
    &WasmTableObject::classOps_,
    &WasmTableObject::classSpec_,
};

const JSClass& WasmTableObject::protoClass_ = PlainObject::class_;

static constexpr char WasmTableName[] = "Table";

const ClassSpec WasmTableObject::classSpec_ = {
    CreateWasmConstructor<WasmTableObject, WasmTableName>,
    GenericCreatePrototype<WasmTableObject>,
    WasmTableObject::static_methods,
    nullptr,
    WasmTableObject::methods,
    WasmTableObject::properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

bool WasmTableObject::isNewborn() const {
  MOZ_ASSERT(is<WasmTableObject>());
  return getReservedSlot(TABLE_SLOT).isUndefined();
}

/* static */
void WasmTableObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  WasmTableObject& tableObj = obj->as<WasmTableObject>();
  if (!tableObj.isNewborn()) {
    auto& table = tableObj.table();
    gcx->release(obj, &table, table.gcMallocBytes(), MemoryUse::WasmTableTable);
  }
}

/* static */
void WasmTableObject::trace(JSTracer* trc, JSObject* obj) {
  WasmTableObject& tableObj = obj->as<WasmTableObject>();
  if (!tableObj.isNewborn()) {
    tableObj.table().tracePrivate(trc);
  }
}

// Return the JS value to use when a parameter to a function requiring a table
// value is omitted. An implementation of [1].
//
// [1]
// https://webassembly.github.io/spec/js-api/#defaultvalue
static Value RefTypeDefaultValue(wasm::RefType tableType) {
  return tableType.isExtern() ? UndefinedValue() : NullValue();
}

/* static */
WasmTableObject* WasmTableObject::create(JSContext* cx, Limits limits,
                                         wasm::RefType tableType,
                                         HandleObject proto) {
  AutoSetNewObjectMetadata metadata(cx);
  Rooted<WasmTableObject*> obj(
      cx, NewObjectWithGivenProto<WasmTableObject>(cx, proto));
  if (!obj) {
    return nullptr;
  }

  MOZ_ASSERT(obj->isNewborn());

  TableDesc td(limits, tableType, Nothing(),
               /*isAsmJS*/ false,
               /*isImported=*/true, /*isExported=*/true);

  SharedTable table = Table::create(cx, td, obj);
  if (!table) {
    return nullptr;
  }

  size_t size = table->gcMallocBytes();
  InitReservedSlot(obj, TABLE_SLOT, table.forget().take(), size,
                   MemoryUse::WasmTableTable);

  MOZ_ASSERT(!obj->isNewborn());
  return obj;
}

/* static */
bool WasmTableObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "Table")) {
    return false;
  }

  if (!args.requireAtLeast(cx, "WebAssembly.Table", 1)) {
    return false;
  }

  if (!args.get(0).isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_DESC_ARG, "table");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());

  JSAtom* elementAtom = Atomize(cx, "element", strlen("element"));
  if (!elementAtom) {
    return false;
  }
  RootedId elementId(cx, AtomToId(elementAtom));

  RootedValue elementVal(cx);
  if (!GetProperty(cx, obj, obj, elementId, &elementVal)) {
    return false;
  }

  RefType tableType;
  if (!ToRefType(cx, elementVal, &tableType)) {
    return false;
  }

  Limits limits;
  if (!GetLimits(cx, obj, LimitsKind::Table, &limits) ||
      !CheckLimits(cx, MaxTableElemsValidation(limits.addressType),
                   LimitsKind::Table, &limits)) {
    return false;
  }

  if (limits.initial > MaxTableElemsRuntime) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_TABLE_IMP_LIMIT);
    return false;
  }

  RootedObject proto(cx,
                     GetWasmConstructorPrototype(cx, args, JSProto_WasmTable));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<WasmTableObject*> table(
      cx, WasmTableObject::create(cx, limits, tableType, proto));
  if (!table) {
    return false;
  }

  // Initialize the table to a default value
  RootedValue initValue(
      cx, args.length() < 2 ? RefTypeDefaultValue(tableType) : args[1]);
  if (!CheckRefType(cx, tableType, initValue)) {
    return false;
  }

  // Skip initializing the table if the fill value is null, as that is the
  // default value.
  if (!initValue.isNull() &&
      !table->fillRange(cx, 0, limits.initial, initValue)) {
    return false;
  }
#ifdef DEBUG
  // Assert that null is the default value of a new table.
  if (initValue.isNull()) {
    table->table().assertRangeNull(0, limits.initial);
  }
  if (!tableType.isNullable()) {
    table->table().assertRangeNotNull(0, limits.initial);
  }
#endif

  args.rval().setObject(*table);
  return true;
}

static bool IsTable(HandleValue v) {
  return v.isObject() && v.toObject().is<WasmTableObject>();
}

/* static */
bool WasmTableObject::lengthGetterImpl(JSContext* cx, const CallArgs& args) {
  const WasmTableObject& tableObj =
      args.thisv().toObject().as<WasmTableObject>();
  RootedValue length(cx);
  if (!CreateAddressValue(cx, tableObj.table().length(),
                          tableObj.table().addressType(), &length)) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().set(length);
  return true;
}

/* static */
bool WasmTableObject::lengthGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTable, lengthGetterImpl>(cx, args);
}

const JSPropertySpec WasmTableObject::properties[] = {
    JS_PSG("length", WasmTableObject::lengthGetter, JSPROP_ENUMERATE),
    JS_STRING_SYM_PS(toStringTag, "WebAssembly.Table", JSPROP_READONLY),
    JS_PS_END,
};

// Gets an AddressValue parameter for a table. This differs from our general
// EnforceAddressValue because our table implementation still uses 32-bit sizes
// internally, and this function therefore returns a uint32_t. Values outside
// the 32-bit range will be clamped to UINT32_MAX, which will always trigger
// bounds checks for all Table uses of AddressValue. See
// MacroAssembler::wasmClampTable64Address and its uses.
//
// isAddress should be true if the value is an actual address, and false if it
// is a different quantity (e.g. a grow delta).
static bool EnforceTableAddressValue(JSContext* cx, HandleValue v,
                                     const Table& table, const char* noun,
                                     uint32_t* result, bool isAddress) {
  uint64_t result64;
  if (!EnforceAddressValue(cx, v, table.addressType(), "Table", noun,
                           &result64)) {
    return false;
  }

  static_assert(MaxTableElemsRuntime < UINT32_MAX);
  *result = result64 > UINT32_MAX ? UINT32_MAX : uint32_t(result64);

  if (isAddress && *result >= table.length()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_BAD_RANGE, "Table", noun);
    return false;
  }

  return true;
}

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
/* static */
bool WasmTableObject::typeImpl(JSContext* cx, const CallArgs& args) {
  Table& table = args.thisv().toObject().as<WasmTableObject>().table();
  RootedObject typeObj(
      cx, TableTypeToObject(cx, table.addressType(), table.elemType(),
                            table.length(), table.maximum()));
  if (!typeObj) {
    return false;
  }
  args.rval().setObject(*typeObj);
  return true;
}

/* static */
bool WasmTableObject::type(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTable, typeImpl>(cx, args);
}
#endif

/* static */
bool WasmTableObject::getImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmTableObject*> tableObj(
      cx, &args.thisv().toObject().as<WasmTableObject>());
  const Table& table = tableObj->table();

  if (!args.requireAtLeast(cx, "WebAssembly.Table.get", 1)) {
    return false;
  }

  uint32_t address;
  if (!EnforceTableAddressValue(cx, args.get(0), table, "get address", &address,
                                /*isAddress=*/true)) {
    return false;
  }

  return table.getValue(cx, address, args.rval());
}

/* static */
bool WasmTableObject::get(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTable, getImpl>(cx, args);
}

/* static */
bool WasmTableObject::setImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmTableObject*> tableObj(
      cx, &args.thisv().toObject().as<WasmTableObject>());
  Table& table = tableObj->table();

  if (!args.requireAtLeast(cx, "WebAssembly.Table.set", 1)) {
    return false;
  }

  uint32_t address;
  if (!EnforceTableAddressValue(cx, args.get(0), table, "set address", &address,
                                /*isAddress=*/true)) {
    return false;
  }

  RootedValue fillValue(
      cx, args.length() < 2 ? RefTypeDefaultValue(table.elemType()) : args[1]);
  if (!tableObj->fillRange(cx, address, 1, fillValue)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/* static */
bool WasmTableObject::set(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTable, setImpl>(cx, args);
}

/* static */
bool WasmTableObject::growImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmTableObject*> tableObj(
      cx, &args.thisv().toObject().as<WasmTableObject>());
  Table& table = tableObj->table();

  if (!args.requireAtLeast(cx, "WebAssembly.Table.grow", 1)) {
    return false;
  }

  uint32_t delta;
  if (!EnforceTableAddressValue(cx, args.get(0), table, "grow delta", &delta,
                                /*isAddress=*/false)) {
    return false;
  }

  RootedValue fillValue(
      cx, args.length() < 2 ? RefTypeDefaultValue(table.elemType()) : args[1]);
  if (!CheckRefType(cx, table.elemType(), fillValue)) {
    return false;
  }

  uint32_t oldLength = table.grow(delta);

  if (oldLength == uint32_t(-1)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_GROW,
                             "table");
    return false;
  }

  // Skip filling the grown range of the table if the fill value is null, as
  // that is the default value.
  if (!fillValue.isNull() &&
      !tableObj->fillRange(cx, oldLength, delta, fillValue)) {
    return false;
  }
#ifdef DEBUG
  // Assert that null is the default value of the grown range.
  if (fillValue.isNull()) {
    table.assertRangeNull(oldLength, delta);
  }
  if (!table.elemType().isNullable()) {
    table.assertRangeNotNull(oldLength, delta);
  }
#endif

  RootedValue result(cx);
  if (!CreateAddressValue(cx, oldLength, table.addressType(), &result)) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().set(result);
  return true;
}

/* static */
bool WasmTableObject::grow(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTable, growImpl>(cx, args);
}

const JSFunctionSpec WasmTableObject::methods[] = {
#ifdef ENABLE_WASM_TYPE_REFLECTIONS
    JS_FN("type", WasmTableObject::type, 0, JSPROP_ENUMERATE),
#endif
    JS_FN("get", WasmTableObject::get, 1, JSPROP_ENUMERATE),
    JS_FN("set", WasmTableObject::set, 2, JSPROP_ENUMERATE),
    JS_FN("grow", WasmTableObject::grow, 1, JSPROP_ENUMERATE),
    JS_FS_END,
};

const JSFunctionSpec WasmTableObject::static_methods[] = {
    JS_FS_END,
};

Table& WasmTableObject::table() const {
  return *(Table*)getReservedSlot(TABLE_SLOT).toPrivate();
}

bool WasmTableObject::fillRange(JSContext* cx, uint32_t index, uint32_t length,
                                HandleValue value) const {
  Table& tab = table();

  // All consumers are required to either bounds check or statically be in
  // bounds
  MOZ_ASSERT(uint64_t(index) + uint64_t(length) <= tab.length());

  RootedAnyRef any(cx, AnyRef::null());
  if (!wasm::CheckRefType(cx, tab.elemType(), value, &any)) {
    return false;
  }
  switch (tab.repr()) {
    case TableRepr::Func:
      MOZ_RELEASE_ASSERT(!tab.isAsmJS());
      tab.fillFuncRef(index, length, FuncRef::fromAnyRefUnchecked(any.get()),
                      cx);
      break;
    case TableRepr::Ref:
      tab.fillAnyRef(index, length, any);
      break;
  }
  return true;
}

// ============================================================================
// WebAssembly.global class and methods

const JSClassOps WasmGlobalObject::classOps_ = {
    nullptr,                     // addProperty
    nullptr,                     // delProperty
    nullptr,                     // enumerate
    nullptr,                     // newEnumerate
    nullptr,                     // resolve
    nullptr,                     // mayResolve
    WasmGlobalObject::finalize,  // finalize
    nullptr,                     // call
    nullptr,                     // construct
    WasmGlobalObject::trace,     // trace
};

const JSClass WasmGlobalObject::class_ = {
    "WebAssembly.Global",
    JSCLASS_HAS_RESERVED_SLOTS(WasmGlobalObject::RESERVED_SLOTS) |
        JSCLASS_BACKGROUND_FINALIZE,
    &WasmGlobalObject::classOps_,
    &WasmGlobalObject::classSpec_,
};

const JSClass& WasmGlobalObject::protoClass_ = PlainObject::class_;

static constexpr char WasmGlobalName[] = "Global";

const ClassSpec WasmGlobalObject::classSpec_ = {
    CreateWasmConstructor<WasmGlobalObject, WasmGlobalName>,
    GenericCreatePrototype<WasmGlobalObject>,
    WasmGlobalObject::static_methods,
    nullptr,
    WasmGlobalObject::methods,
    WasmGlobalObject::properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

/* static */
void WasmGlobalObject::trace(JSTracer* trc, JSObject* obj) {
  WasmGlobalObject* global = reinterpret_cast<WasmGlobalObject*>(obj);
  if (global->isNewborn()) {
    // This can happen while we're allocating the object, in which case
    // every single slot of the object is not defined yet. In particular,
    // there's nothing to trace yet.
    return;
  }
  global->val().get().trace(trc);
}

/* static */
void WasmGlobalObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  WasmGlobalObject* global = reinterpret_cast<WasmGlobalObject*>(obj);
  if (!global->isNewborn()) {
    // Release the strong reference to the type definitions this global could
    // be referencing.
    global->type().Release();
    gcx->delete_(obj, &global->mutableVal(), MemoryUse::WasmGlobalCell);
  }
}

/* static */
WasmGlobalObject* WasmGlobalObject::create(JSContext* cx, HandleVal value,
                                           bool isMutable, HandleObject proto) {
  Rooted<WasmGlobalObject*> obj(
      cx, NewObjectWithGivenProto<WasmGlobalObject>(cx, proto));
  if (!obj) {
    return nullptr;
  }

  MOZ_ASSERT(obj->isNewborn());
  MOZ_ASSERT(obj->isTenured(), "assumed by global.set post barriers");

  GCPtrVal* val = js_new<GCPtrVal>(Val());
  if (!val) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  obj->initReservedSlot(MUTABLE_SLOT, JS::BooleanValue(isMutable));
  InitReservedSlot(obj, VAL_SLOT, val, MemoryUse::WasmGlobalCell);

  // It's simpler to initialize the cell after the object has been created,
  // to avoid needing to root the cell before the object creation.
  // We don't use `setVal` here because the assumes the cell has already
  // been initialized.
  obj->mutableVal() = value.get();
  // Acquire a strong reference to a type definition this global could
  // be referencing.
  obj->type().AddRef();

  MOZ_ASSERT(!obj->isNewborn());

  return obj;
}

/* static */
bool WasmGlobalObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "Global")) {
    return false;
  }

  if (!args.requireAtLeast(cx, "WebAssembly.Global", 1)) {
    return false;
  }

  if (!args.get(0).isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_DESC_ARG, "global");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());

  // Extract properties in lexicographic order per spec.

  RootedValue mutableVal(cx);
  if (!JS_GetProperty(cx, obj, "mutable", &mutableVal)) {
    return false;
  }

  RootedValue typeVal(cx);
  if (!JS_GetProperty(cx, obj, "value", &typeVal)) {
    return false;
  }

  ValType globalType;
  if (!ToValType(cx, typeVal, &globalType)) {
    return false;
  }

  if (!globalType.isExposable()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }

  bool isMutable = ToBoolean(mutableVal);

  // Extract the initial value, or provide a suitable default.
  RootedVal globalVal(cx, globalType);

  // Override with non-undefined value, if provided.
  RootedValue valueVal(cx);
  if (globalType.isRefType()) {
    valueVal.set(args.length() < 2 ? RefTypeDefaultValue(globalType.refType())
                                   : args[1]);
    if (!Val::fromJSValue(cx, globalType, valueVal, &globalVal)) {
      return false;
    }
  } else {
    valueVal.set(args.get(1));
    if (!valueVal.isUndefined() &&
        !Val::fromJSValue(cx, globalType, valueVal, &globalVal)) {
      return false;
    }
  }

  RootedObject proto(cx,
                     GetWasmConstructorPrototype(cx, args, JSProto_WasmGlobal));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }

  WasmGlobalObject* global =
      WasmGlobalObject::create(cx, globalVal, isMutable, proto);
  if (!global) {
    return false;
  }

  args.rval().setObject(*global);
  return true;
}

static bool IsGlobal(HandleValue v) {
  return v.isObject() && v.toObject().is<WasmGlobalObject>();
}

/* static */
bool WasmGlobalObject::valueGetterImpl(JSContext* cx, const CallArgs& args) {
  const WasmGlobalObject& globalObj =
      args.thisv().toObject().as<WasmGlobalObject>();
  if (!globalObj.type().isExposable()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }
  return globalObj.val().get().toJSValue(cx, args.rval());
}

/* static */
bool WasmGlobalObject::valueGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsGlobal, valueGetterImpl>(cx, args);
}

/* static */
bool WasmGlobalObject::valueSetterImpl(JSContext* cx, const CallArgs& args) {
  if (!args.requireAtLeast(cx, "WebAssembly.Global setter", 1)) {
    return false;
  }

  Rooted<WasmGlobalObject*> global(
      cx, &args.thisv().toObject().as<WasmGlobalObject>());
  if (!global->isMutable()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_GLOBAL_IMMUTABLE);
    return false;
  }

  RootedVal val(cx);
  if (!Val::fromJSValue(cx, global->type(), args.get(0), &val)) {
    return false;
  }
  global->setVal(val);

  args.rval().setUndefined();
  return true;
}

/* static */
bool WasmGlobalObject::valueSetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsGlobal, valueSetterImpl>(cx, args);
}

const JSPropertySpec WasmGlobalObject::properties[] = {
    JS_PSGS("value", WasmGlobalObject::valueGetter,
            WasmGlobalObject::valueSetter, JSPROP_ENUMERATE),
    JS_STRING_SYM_PS(toStringTag, "WebAssembly.Global", JSPROP_READONLY),
    JS_PS_END,
};

const JSFunctionSpec WasmGlobalObject::methods[] = {
#ifdef ENABLE_WASM_TYPE_REFLECTIONS
    JS_FN("type", WasmGlobalObject::type, 0, JSPROP_ENUMERATE),
#endif
    JS_FN("valueOf", WasmGlobalObject::valueGetter, 0, JSPROP_ENUMERATE),
    JS_FS_END,
};

const JSFunctionSpec WasmGlobalObject::static_methods[] = {
    JS_FS_END,
};

bool WasmGlobalObject::isMutable() const {
  return getReservedSlot(MUTABLE_SLOT).toBoolean();
}

ValType WasmGlobalObject::type() const { return val().get().type(); }

GCPtrVal& WasmGlobalObject::mutableVal() {
  return *reinterpret_cast<GCPtrVal*>(getReservedSlot(VAL_SLOT).toPrivate());
}

const GCPtrVal& WasmGlobalObject::val() const {
  return *reinterpret_cast<GCPtrVal*>(getReservedSlot(VAL_SLOT).toPrivate());
}

void WasmGlobalObject::setVal(wasm::HandleVal value) {
  MOZ_ASSERT(type() == value.get().type());
  mutableVal() = value;
}

void* WasmGlobalObject::addressOfCell() const {
  return (void*)&val().get().cell();
}

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
/* static */
bool WasmGlobalObject::typeImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmGlobalObject*> global(
      cx, &args.thisv().toObject().as<WasmGlobalObject>());
  RootedObject typeObj(
      cx, GlobalTypeToObject(cx, global->type(), global->isMutable()));
  if (!typeObj) {
    return false;
  }
  args.rval().setObject(*typeObj);
  return true;
}

/* static */
bool WasmGlobalObject::type(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsGlobal, typeImpl>(cx, args);
}
#endif

// ============================================================================
// WebAssembly.Tag class and methods

const JSClassOps WasmTagObject::classOps_ = {
    nullptr,                  // addProperty
    nullptr,                  // delProperty
    nullptr,                  // enumerate
    nullptr,                  // newEnumerate
    nullptr,                  // resolve
    nullptr,                  // mayResolve
    WasmTagObject::finalize,  // finalize
    nullptr,                  // call
    nullptr,                  // construct
    nullptr,                  // trace
};

const JSClass WasmTagObject::class_ = {
    "WebAssembly.Tag",
    JSCLASS_HAS_RESERVED_SLOTS(WasmTagObject::RESERVED_SLOTS) |
        JSCLASS_FOREGROUND_FINALIZE,
    &WasmTagObject::classOps_,
    &WasmTagObject::classSpec_,
};

const JSClass& WasmTagObject::protoClass_ = PlainObject::class_;

static constexpr char WasmTagName[] = "Tag";

const ClassSpec WasmTagObject::classSpec_ = {
    CreateWasmConstructor<WasmTagObject, WasmTagName>,
    GenericCreatePrototype<WasmTagObject>,
    WasmTagObject::static_methods,
    nullptr,
    WasmTagObject::methods,
    WasmTagObject::properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

/* static */
void WasmTagObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  WasmTagObject& tagObj = obj->as<WasmTagObject>();
  tagObj.tagType()->Release();
}

static bool IsTag(HandleValue v) {
  return v.isObject() && v.toObject().is<WasmTagObject>();
}

bool WasmTagObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "Tag")) {
    return false;
  }

  if (!args.requireAtLeast(cx, "WebAssembly.Tag", 1)) {
    return false;
  }

  if (!args.get(0).isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_DESC_ARG, "tag");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());
  RootedValue paramsVal(cx);
  if (!JS_GetProperty(cx, obj, "parameters", &paramsVal)) {
    return false;
  }

  ValTypeVector params;
  if (!ParseValTypes(cx, paramsVal, params)) {
    return false;
  }

  RefPtr<TypeContext> types = js_new<TypeContext>();
  if (!types) {
    ReportOutOfMemory(cx);
    return false;
  }
  const TypeDef* tagTypeDef =
      types->addType(FuncType(std::move(params), ValTypeVector()));
  if (!tagTypeDef) {
    ReportOutOfMemory(cx);
    return false;
  }

  wasm::MutableTagType tagType = js_new<wasm::TagType>();
  if (!tagType || !tagType->initialize(tagTypeDef)) {
    ReportOutOfMemory(cx);
    return false;
  }

  RootedObject proto(cx,
                     GetWasmConstructorPrototype(cx, args, JSProto_WasmTag));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<WasmTagObject*> tagObj(cx, WasmTagObject::create(cx, tagType, proto));
  if (!tagObj) {
    return false;
  }

  args.rval().setObject(*tagObj);
  return true;
}

/* static */
WasmTagObject* WasmTagObject::create(JSContext* cx,
                                     const wasm::SharedTagType& tagType,
                                     HandleObject proto) {
  Rooted<WasmTagObject*> obj(cx,
                             NewObjectWithGivenProto<WasmTagObject>(cx, proto));
  if (!obj) {
    return nullptr;
  }

  tagType.get()->AddRef();
  obj->initReservedSlot(TYPE_SLOT, PrivateValue((void*)tagType.get()));

  return obj;
}

const JSPropertySpec WasmTagObject::properties[] = {
    JS_STRING_SYM_PS(toStringTag, "WebAssembly.Tag", JSPROP_READONLY),
    JS_PS_END,
};

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
/* static */
bool WasmTagObject::typeImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmTagObject*> tag(cx, &args.thisv().toObject().as<WasmTagObject>());
  RootedObject typeObj(cx, TagTypeToObject(cx, tag->valueTypes()));
  if (!typeObj) {
    return false;
  }
  args.rval().setObject(*typeObj);
  return true;
}

/* static  */
bool WasmTagObject::type(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsTag, typeImpl>(cx, args);
}
#endif

const JSFunctionSpec WasmTagObject::methods[] = {
#ifdef ENABLE_WASM_TYPE_REFLECTIONS
    JS_FN("type", WasmTagObject::type, 0, JSPROP_ENUMERATE),
#endif
    JS_FS_END,
};

const JSFunctionSpec WasmTagObject::static_methods[] = {
    JS_FS_END,
};

const TagType* WasmTagObject::tagType() const {
  return (const TagType*)getFixedSlot(TYPE_SLOT).toPrivate();
};

const wasm::ValTypeVector& WasmTagObject::valueTypes() const {
  return tagType()->argTypes();
};

// ============================================================================
// WebAssembly.Exception class and methods

const JSClassOps WasmExceptionObject::classOps_ = {
    nullptr,                        // addProperty
    nullptr,                        // delProperty
    nullptr,                        // enumerate
    nullptr,                        // newEnumerate
    nullptr,                        // resolve
    nullptr,                        // mayResolve
    WasmExceptionObject::finalize,  // finalize
    nullptr,                        // call
    nullptr,                        // construct
    WasmExceptionObject::trace,     // trace
};

const JSClass WasmExceptionObject::class_ = {
    "WebAssembly.Exception",
    JSCLASS_HAS_RESERVED_SLOTS(WasmExceptionObject::RESERVED_SLOTS) |
        JSCLASS_FOREGROUND_FINALIZE,
    &WasmExceptionObject::classOps_,
    &WasmExceptionObject::classSpec_,
};

const JSClass& WasmExceptionObject::protoClass_ = PlainObject::class_;

static constexpr char WasmExceptionName[] = "Exception";

const ClassSpec WasmExceptionObject::classSpec_ = {
    CreateWasmConstructor<WasmExceptionObject, WasmExceptionName>,
    GenericCreatePrototype<WasmExceptionObject>,
    WasmExceptionObject::static_methods,
    nullptr,
    WasmExceptionObject::methods,
    WasmExceptionObject::properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

/* static */
void WasmExceptionObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  WasmExceptionObject& exnObj = obj->as<WasmExceptionObject>();
  if (exnObj.isNewborn()) {
    return;
  }
  gcx->free_(obj, exnObj.typedMem(), exnObj.tagType()->tagSize(),
             MemoryUse::WasmExceptionData);
  exnObj.tagType()->Release();
}

/* static */
void WasmExceptionObject::trace(JSTracer* trc, JSObject* obj) {
  WasmExceptionObject& exnObj = obj->as<WasmExceptionObject>();
  if (exnObj.isNewborn()) {
    return;
  }

  wasm::SharedTagType tag = exnObj.tagType();
  const wasm::ValTypeVector& params = tag->argTypes();
  const wasm::TagOffsetVector& offsets = tag->argOffsets();
  uint8_t* typedMem = exnObj.typedMem();
  for (size_t i = 0; i < params.length(); i++) {
    ValType paramType = params[i];
    if (paramType.isRefRepr()) {
      GCPtr<wasm::AnyRef>* paramPtr =
          reinterpret_cast<GCPtr<AnyRef>*>(typedMem + offsets[i]);
      TraceNullableEdge(trc, paramPtr, "wasm exception param");
    }
  }
}

static bool IsException(HandleValue v) {
  return v.isObject() && v.toObject().is<WasmExceptionObject>();
}

struct ExceptionOptions {
  bool traceStack;

  ExceptionOptions() : traceStack(false) {}

  [[nodiscard]] bool init(JSContext* cx, HandleValue val);
};

bool ExceptionOptions::init(JSContext* cx, HandleValue val) {
  if (val.isNullOrUndefined()) {
    return true;
  }

  if (!val.isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_EXN_OPTIONS);
    return false;
  }
  RootedObject obj(cx, &val.toObject());

  // Get `traceStack` and coerce to boolean
  RootedValue traceStackVal(cx);
  if (!JS_GetProperty(cx, obj, "traceStack", &traceStackVal)) {
    return false;
  }
  traceStack = ToBoolean(traceStackVal);

  return true;
}

bool WasmExceptionObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "Exception")) {
    return false;
  }

  if (!args.requireAtLeast(cx, "WebAssembly.Exception", 2)) {
    return false;
  }

  if (!IsTag(args[0])) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_EXN_ARG);
    return false;
  }
  Rooted<WasmTagObject*> exnTag(cx, &args[0].toObject().as<WasmTagObject>());

  if (!args.get(1).isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_EXN_PAYLOAD);
    return false;
  }

  JS::ForOfIterator iterator(cx);
  if (!iterator.init(args.get(1), JS::ForOfIterator::ThrowOnNonIterable)) {
    return false;
  }

  // Get the optional 'options' parameter
  ExceptionOptions options;
  if (!options.init(cx, args.get(2))) {
    return false;
  }

  // Trace the stack if requested
  RootedObject stack(cx);
  if (options.traceStack && !CaptureStack(cx, &stack)) {
    return false;
  }

  RootedObject proto(
      cx, GetWasmConstructorPrototype(cx, args, JSProto_WasmException));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<WasmExceptionObject*> exnObj(
      cx, WasmExceptionObject::create(cx, exnTag, stack, proto));
  if (!exnObj) {
    return false;
  }

  wasm::SharedTagType tagType = exnObj->tagType();
  const wasm::ValTypeVector& params = tagType->argTypes();
  const wasm::TagOffsetVector& offsets = tagType->argOffsets();

  RootedValue nextArg(cx);
  for (size_t i = 0; i < params.length(); i++) {
    bool done;
    if (!iterator.next(&nextArg, &done)) {
      return false;
    }
    if (done) {
      UniqueChars expected(JS_smprintf("%zu", params.length()));
      UniqueChars got(JS_smprintf("%zu", i));
      if (!expected || !got) {
        ReportOutOfMemory(cx);
        return false;
      }

      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_BAD_EXN_PAYLOAD_LEN, expected.get(),
                               got.get());
      return false;
    }

    if (!exnObj->initArg(cx, offsets[i], params[i], nextArg)) {
      return false;
    }
  }

  args.rval().setObject(*exnObj);
  return true;
}

/* static */
WasmExceptionObject* WasmExceptionObject::create(JSContext* cx,
                                                 Handle<WasmTagObject*> tag,
                                                 HandleObject stack,
                                                 HandleObject proto) {
  Rooted<WasmExceptionObject*> obj(
      cx, NewObjectWithGivenProto<WasmExceptionObject>(cx, proto));
  if (!obj) {
    return nullptr;
  }
  const TagType* tagType = tag->tagType();

  // Allocate the data buffer before initializing the object so that an OOM
  // does not result in a partially constructed object.
  uint8_t* data = (uint8_t*)js_calloc(tagType->tagSize());
  if (!data) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  MOZ_ASSERT(obj->isNewborn());
  obj->initFixedSlot(TAG_SLOT, ObjectValue(*tag));
  tagType->AddRef();
  obj->initFixedSlot(TYPE_SLOT, PrivateValue((void*)tagType));
  InitReservedSlot(obj, DATA_SLOT, data, tagType->tagSize(),
                   MemoryUse::WasmExceptionData);
  obj->initFixedSlot(STACK_SLOT, ObjectOrNullValue(stack));

  MOZ_ASSERT(!obj->isNewborn());

  return obj;
}

WasmExceptionObject* WasmExceptionObject::wrapJSValue(JSContext* cx,
                                                      HandleValue value) {
  Rooted<WasmNamespaceObject*> wasm(cx, WasmNamespaceObject::getOrCreate(cx));
  if (!wasm) {
    return nullptr;
  }

  Rooted<AnyRef> valueAnyRef(cx);
  if (!AnyRef::fromJSValue(cx, value, &valueAnyRef)) {
    return nullptr;
  }

  Rooted<WasmTagObject*> wrappedJSValueTag(cx, wasm->wrappedJSValueTag());
  WasmExceptionObject* exn =
      WasmExceptionObject::create(cx, wrappedJSValueTag, nullptr, nullptr);
  if (!exn) {
    return nullptr;
  }
  MOZ_ASSERT(exn->isWrappedJSValue());

  exn->initRefArg(WrappedJSValueTagType_ValueOffset, valueAnyRef);
  return exn;
}

bool WasmExceptionObject::isNewborn() const {
  MOZ_ASSERT(is<WasmExceptionObject>());
  return getReservedSlot(DATA_SLOT).isUndefined();
}

bool WasmExceptionObject::isWrappedJSValue() const {
  return tagType() == sWrappedJSValueTagType;
}

Value WasmExceptionObject::wrappedJSValue() const {
  MOZ_ASSERT(isWrappedJSValue());
  return loadRefArg(WrappedJSValueTagType_ValueOffset).toJSValue();
}

const JSPropertySpec WasmExceptionObject::properties[] = {
    JS_PSG("stack", WasmExceptionObject::getStack, 0),
    JS_STRING_SYM_PS(toStringTag, "WebAssembly.Exception", JSPROP_READONLY),
    JS_PS_END,
};

/* static */
bool WasmExceptionObject::isImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmExceptionObject*> exnObj(
      cx, &args.thisv().toObject().as<WasmExceptionObject>());

  if (!args.requireAtLeast(cx, "WebAssembly.Exception.is", 1)) {
    return false;
  }

  if (!IsTag(args[0])) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_EXN_ARG);
    return false;
  }

  Rooted<WasmTagObject*> exnTag(cx,
                                &args.get(0).toObject().as<WasmTagObject>());
  args.rval().setBoolean(exnTag.get() == &exnObj->tag());

  return true;
}

/* static  */
bool WasmExceptionObject::isMethod(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsException, isImpl>(cx, args);
}

/* static */
bool WasmExceptionObject::getArgImpl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmExceptionObject*> exnObj(
      cx, &args.thisv().toObject().as<WasmExceptionObject>());

  if (!args.requireAtLeast(cx, "WebAssembly.Exception.getArg", 2)) {
    return false;
  }

  if (!IsTag(args[0])) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_EXN_ARG);
    return false;
  }

  Rooted<WasmTagObject*> exnTag(cx,
                                &args.get(0).toObject().as<WasmTagObject>());
  if (exnTag.get() != &exnObj->tag()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_EXN_TAG);
    return false;
  }

  uint32_t index;
  if (!EnforceRangeU32(cx, args.get(1), "Exception", "getArg index", &index)) {
    return false;
  }

  const wasm::ValTypeVector& params = exnTag->valueTypes();
  if (index >= params.length()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_RANGE,
                             "Exception", "getArg index");
    return false;
  }

  uint32_t offset = exnTag->tagType()->argOffsets()[index];
  RootedValue result(cx);
  if (!exnObj->loadArg(cx, offset, params[index], &result)) {
    return false;
  }
  args.rval().set(result);
  return true;
}

/* static  */
bool WasmExceptionObject::getArg(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsException, getArgImpl>(cx, args);
}

/* static */
bool WasmExceptionObject::getStack_impl(JSContext* cx, const CallArgs& args) {
  Rooted<WasmExceptionObject*> exnObj(
      cx, &args.thisv().toObject().as<WasmExceptionObject>());
  RootedObject savedFrameObj(cx, exnObj->stack());
  if (!savedFrameObj) {
    args.rval().setUndefined();
    return true;
  }
  JSPrincipals* principals = exnObj->realm()->principals();
  RootedString stackString(cx);
  if (!BuildStackString(cx, principals, savedFrameObj, &stackString)) {
    return false;
  }
  args.rval().setString(stackString);
  return true;
}

/* static */
bool WasmExceptionObject::getStack(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsException, getStack_impl>(cx, args);
}

JSObject* WasmExceptionObject::stack() const {
  return getReservedSlot(STACK_SLOT).toObjectOrNull();
}

uint8_t* WasmExceptionObject::typedMem() const {
  return (uint8_t*)getReservedSlot(DATA_SLOT).toPrivate();
}

bool WasmExceptionObject::loadArg(JSContext* cx, size_t offset,
                                  wasm::ValType type,
                                  MutableHandleValue vp) const {
  if (!type.isExposable()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }
  return ToJSValue(cx, typedMem() + offset, type, vp);
}

bool WasmExceptionObject::initArg(JSContext* cx, size_t offset,
                                  wasm::ValType type, HandleValue value) {
  if (!type.isExposable()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }

  // Avoid rooting hazard of `this` being live across `fromJSValue`
  // which may GC.
  uint8_t* dest = typedMem() + offset;
  RootedVal val(cx);
  if (!Val::fromJSValue(cx, type, value, &val)) {
    return false;
  }
  val.get().writeToHeapLocation(dest);
  return true;
}

void WasmExceptionObject::initRefArg(size_t offset, wasm::AnyRef ref) {
  uint8_t* dest = typedMem() + offset;
  *((GCPtr<AnyRef>*)dest) = ref;
}

wasm::AnyRef WasmExceptionObject::loadRefArg(size_t offset) const {
  uint8_t* src = typedMem() + offset;
  return *((GCPtr<AnyRef>*)src);
}

const JSFunctionSpec WasmExceptionObject::methods[] = {
    JS_FN("is", WasmExceptionObject::isMethod, 1, JSPROP_ENUMERATE),
    JS_FN("getArg", WasmExceptionObject::getArg, 2, JSPROP_ENUMERATE),
    JS_FS_END,
};

const JSFunctionSpec WasmExceptionObject::static_methods[] = {
    JS_FS_END,
};

const TagType* WasmExceptionObject::tagType() const {
  return (const TagType*)getReservedSlot(TYPE_SLOT).toPrivate();
}

WasmTagObject& WasmExceptionObject::tag() const {
  return getReservedSlot(TAG_SLOT).toObject().as<WasmTagObject>();
}

// ============================================================================
// WebAssembly.Function and methods
#if defined(ENABLE_WASM_TYPE_REFLECTIONS) || defined(ENABLE_WASM_JSPI)
[[nodiscard]] static bool IsWasmFunction(HandleValue v) {
  if (!v.isObject()) {
    return false;
  }
  if (!v.toObject().is<JSFunction>()) {
    return false;
  }
  return v.toObject().as<JSFunction>().isWasm();
}
#endif  // ENABLE_WASM_TYPE_REFLECTIONS || ENABLE_WASM_JSPI

#ifdef ENABLE_WASM_TYPE_REFLECTIONS
static JSObject* CreateWasmFunctionPrototype(JSContext* cx, JSProtoKey key) {
  // WasmFunction's prototype should inherit from JSFunction's prototype.
  RootedObject jsProto(cx, &cx->global()->getFunctionPrototype());
  return GlobalObject::createBlankPrototypeInheriting(cx, &PlainObject::class_,
                                                      jsProto);
}

bool WasmFunctionTypeImpl(JSContext* cx, const CallArgs& args) {
  RootedFunction function(cx, &args.thisv().toObject().as<JSFunction>());
  const FuncType& funcType = function->wasmTypeDef()->funcType();
  RootedObject typeObj(cx, FuncTypeToObject(cx, funcType));
  if (!typeObj) {
    return false;
  }
  args.rval().setObject(*typeObj);
  return true;
}

bool WasmFunctionType(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsWasmFunction, WasmFunctionTypeImpl>(cx, args);
}

static JSFunction* WasmFunctionCreate(JSContext* cx, HandleObject func,
                                      wasm::ValTypeVector&& params,
                                      wasm::ValTypeVector&& results,
                                      HandleObject proto) {
  MOZ_ASSERT(IsCallableNonCCW(ObjectValue(*func)));

  // We want to import the function to a wasm module and then export it again so
  // that it behaves exactly like a normal wasm function and can be used like
  // one in wasm tables. We synthesize such a module below, instantiate it, and
  // then return the exported function as the result.
  FeatureOptions options;
  ScriptedCaller scriptedCaller;
  SharedCompileArgs compileArgs =
      CompileArgs::buildAndReport(cx, std::move(scriptedCaller), options);
  if (!compileArgs) {
    return nullptr;
  }

  MutableModuleMetadata moduleMeta = js_new<ModuleMetadata>();
  if (!moduleMeta || !moduleMeta->init(*compileArgs)) {
    return nullptr;
  }
  MutableCodeMetadata codeMeta = moduleMeta->codeMeta;
  CompilerEnvironment compilerEnv(CompileMode::Once, Tier::Optimized,
                                  DebugEnabled::False);
  compilerEnv.computeParameters();

  FuncType funcType = FuncType(std::move(params), std::move(results));
  if (!codeMeta->types->addType(std::move(funcType))) {
    return nullptr;
  }

  // Add an (import (func ...))
  FuncDesc funcDesc = FuncDesc(0);
  if (!codeMeta->funcs.append(funcDesc)) {
    return nullptr;
  }
  codeMeta->numFuncImports = 1;
  codeMeta->funcImportsAreJS = true;

  // Add an (export (func 0))
  codeMeta->funcs[0].declareFuncExported(/* eager */ true,
                                         /* canRefFunc */ true);

  // We will be looking up and using the function in the future by index so the
  // name doesn't matter.
  CacheableName fieldName;
  if (!moduleMeta->exports.emplaceBack(std::move(fieldName), 0,
                                       DefinitionKind::Function)) {
    return nullptr;
  }

  if (!moduleMeta->prepareForCompile(compilerEnv.mode())) {
    return nullptr;
  }

  ModuleGenerator mg(*codeMeta, compilerEnv, compilerEnv.initialState(),
                     nullptr, nullptr, nullptr);
  if (!mg.initializeCompleteTier()) {
    return nullptr;
  }
  // We're not compiling any function definitions.
  if (!mg.finishFuncDefs()) {
    return nullptr;
  }
  SharedModule module = mg.finishModule(BytecodeBufferOrSource(), *moduleMeta,
                                        /*maybeCompleteTier2Listener=*/nullptr);
  if (!module) {
    return nullptr;
  }

  // Instantiate the module.
  Rooted<ImportValues> imports(cx);
  if (!imports.get().funcs.append(func)) {
    return nullptr;
  }
  Rooted<WasmInstanceObject*> instance(cx);
  if (!module->instantiate(cx, imports.get(), nullptr, &instance)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }

  // Get the exported function which wraps the JS function to return.
  RootedFunction wasmFunc(cx);
  if (!instance->getExportedFunction(cx, instance, 0, &wasmFunc)) {
    return nullptr;
  }
  return wasmFunc;
}

bool WasmFunctionConstruct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "WebAssembly.Function")) {
    return false;
  }

  if (!args.requireAtLeast(cx, "WebAssembly.Function", 2)) {
    return false;
  }

  if (!args[0].isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_DESC_ARG, "function");
    return false;
  }
  RootedObject typeObj(cx, &args[0].toObject());

  // Extract properties in lexicographic order per spec.

  RootedValue parametersVal(cx);
  if (!JS_GetProperty(cx, typeObj, "parameters", &parametersVal)) {
    return false;
  }

  ValTypeVector params;
  if (!ParseValTypes(cx, parametersVal, params)) {
    return false;
  }
  if (params.length() > MaxParams) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_FUNCTION_TYPE, "parameters");
    return false;
  }

  RootedValue resultsVal(cx);
  if (!JS_GetProperty(cx, typeObj, "results", &resultsVal)) {
    return false;
  }

  ValTypeVector results;
  if (!ParseValTypes(cx, resultsVal, results)) {
    return false;
  }
  if (results.length() > MaxResults) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_FUNCTION_TYPE, "results");
    return false;
  }

  // Get the target function

  if (!IsCallableNonCCW(args[1])) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_FUNCTION_VALUE);
    return false;
  }
  RootedObject func(cx, &args[1].toObject());

  RootedObject proto(
      cx, GetWasmConstructorPrototype(cx, args, JSProto_WasmFunction));
  if (!proto) {
    ReportOutOfMemory(cx);
    return false;
  }

  RootedFunction wasmFunc(cx, WasmFunctionCreate(cx, func, std::move(params),
                                                 std::move(results), proto));
  if (!wasmFunc) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().setObject(*wasmFunc);

  return true;
}

static constexpr char WasmFunctionName[] = "Function";

static JSObject* CreateWasmFunctionConstructor(JSContext* cx, JSProtoKey key) {
  RootedObject proto(cx, &cx->global()->getFunctionConstructor());

  Rooted<JSAtom*> className(
      cx, Atomize(cx, WasmFunctionName, strlen(WasmFunctionName)));
  if (!className) {
    return nullptr;
  }
  return NewFunctionWithProto(cx, WasmFunctionConstruct, 1,
                              FunctionFlags::NATIVE_CTOR, nullptr, className,
                              proto, gc::AllocKind::FUNCTION, TenuredObject);
}

const JSFunctionSpec WasmFunctionMethods[] = {
    JS_FN("type", WasmFunctionType, 0, 0),
    JS_FS_END,
};

const ClassSpec WasmFunctionClassSpec = {
    CreateWasmFunctionConstructor,
    CreateWasmFunctionPrototype,
    nullptr,
    nullptr,
    WasmFunctionMethods,
    nullptr,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

const JSClass js::WasmFunctionClass = {
    "WebAssembly.Function",
    0,
    JS_NULL_CLASS_OPS,
    &WasmFunctionClassSpec,
};

#endif

// ============================================================================
// WebAssembly class and static methods

static bool WebAssembly_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().WebAssembly);
  return true;
}

static bool RejectWithPendingException(JSContext* cx,
                                       Handle<PromiseObject*> promise) {
  if (!cx->isExceptionPending()) {
    return false;
  }

  RootedValue rejectionValue(cx);
  if (!GetAndClearException(cx, &rejectionValue)) {
    return false;
  }

  return PromiseObject::reject(cx, promise, rejectionValue);
}

static bool Reject(JSContext* cx, const CompileArgs& args,
                   Handle<PromiseObject*> promise, const UniqueChars& error) {
  if (!error) {
    ThrowCompileOutOfMemory(cx);
    return RejectWithPendingException(cx, promise);
  }

  RootedObject stack(cx, promise->allocationSite());
  RootedString fileName(cx);
  if (const char* filename = args.scriptedCaller.filename.get()) {
    fileName =
        JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(filename, strlen(filename)));
  } else {
    fileName = JS_GetEmptyString(cx);
  }
  if (!fileName) {
    return false;
  }

  uint32_t line = args.scriptedCaller.line;

  // Ideally we'd report a JSMSG_WASM_COMPILE_ERROR here, but there's no easy
  // way to create an ErrorObject for an arbitrary error code with multiple
  // replacements.
  UniqueChars str(JS_smprintf("wasm validation error: %s", error.get()));
  if (!str) {
    return false;
  }

  size_t len = strlen(str.get());
  RootedString message(cx, NewStringCopyN<CanGC>(cx, str.get(), len));
  if (!message) {
    return false;
  }

  // There's no error |cause| available here.
  auto cause = JS::NothingHandleValue;

  RootedObject errorObj(
      cx, ErrorObject::create(cx, JSEXN_WASMCOMPILEERROR, stack, fileName, 0,
                              line, JS::ColumnNumberOneOrigin(), nullptr,
                              message, cause));
  if (!errorObj) {
    return false;
  }

  RootedValue rejectionValue(cx, ObjectValue(*errorObj));
  return PromiseObject::reject(cx, promise, rejectionValue);
}

static void LogAsync(JSContext* cx, const char* funcName,
                     const Module& module) {
  Log(cx, "async %s succeeded%s", funcName,
      module.loggingDeserialized() ? " (loaded from cache)" : "");
}

enum class Ret { Pair, Instance };

class AsyncInstantiateTask : public OffThreadPromiseTask {
  SharedModule module_;
  PersistentRooted<ImportValues> imports_;
  Ret ret_;

 public:
  AsyncInstantiateTask(JSContext* cx, const Module& module, Ret ret,
                       Handle<PromiseObject*> promise)
      : OffThreadPromiseTask(cx, promise),
        module_(&module),
        imports_(cx),
        ret_(ret) {}

  ImportValues& imports() { return imports_.get(); }

  bool resolve(JSContext* cx, Handle<PromiseObject*> promise) override {
    RootedObject instanceProto(
        cx, &cx->global()->getPrototype(JSProto_WasmInstance));

    Rooted<WasmInstanceObject*> instanceObj(cx);
    if (!module_->instantiate(cx, imports_.get(), instanceProto,
                              &instanceObj)) {
      return RejectWithPendingException(cx, promise);
    }

    RootedValue resolutionValue(cx);
    if (ret_ == Ret::Instance) {
      resolutionValue = ObjectValue(*instanceObj);
    } else {
      RootedObject resultObj(cx, JS_NewPlainObject(cx));
      if (!resultObj) {
        return RejectWithPendingException(cx, promise);
      }

      RootedObject moduleProto(cx,
                               &cx->global()->getPrototype(JSProto_WasmModule));
      RootedObject moduleObj(
          cx, WasmModuleObject::create(cx, *module_, moduleProto));
      if (!moduleObj) {
        return RejectWithPendingException(cx, promise);
      }

      RootedValue val(cx, ObjectValue(*moduleObj));
      if (!JS_DefineProperty(cx, resultObj, "module", val, JSPROP_ENUMERATE)) {
        return RejectWithPendingException(cx, promise);
      }

      val = ObjectValue(*instanceObj);
      if (!JS_DefineProperty(cx, resultObj, "instance", val,
                             JSPROP_ENUMERATE)) {
        return RejectWithPendingException(cx, promise);
      }

      resolutionValue = ObjectValue(*resultObj);
    }

    if (!PromiseObject::resolve(cx, promise, resolutionValue)) {
      return RejectWithPendingException(cx, promise);
    }

    LogAsync(cx, "instantiate", *module_);
    return true;
  }
};

static bool AsyncInstantiate(JSContext* cx, const Module& module,
                             HandleObject importObj, Ret ret,
                             Handle<PromiseObject*> promise) {
  auto task = js::MakeUnique<AsyncInstantiateTask>(cx, module, ret, promise);
  if (!task || !task->init(cx)) {
    return false;
  }

  if (!GetImports(cx, module, importObj, &task->imports())) {
    return RejectWithPendingException(cx, promise);
  }

  OffThreadPromiseTask::DispatchResolveAndDestroy(std::move(task));
  return true;
}

static bool ResolveCompile(JSContext* cx, const Module& module,
                           Handle<PromiseObject*> promise) {
  RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmModule));
  RootedObject moduleObj(cx, WasmModuleObject::create(cx, module, proto));
  if (!moduleObj) {
    return RejectWithPendingException(cx, promise);
  }

  RootedValue resolutionValue(cx, ObjectValue(*moduleObj));
  if (!PromiseObject::resolve(cx, promise, resolutionValue)) {
    return RejectWithPendingException(cx, promise);
  }

  LogAsync(cx, "compile", module);
  return true;
}

struct CompileBufferTask : PromiseHelperTask {
  BytecodeBuffer bytecode;
  SharedCompileArgs compileArgs;
  UniqueChars error;
  UniqueCharsVector warnings;
  SharedModule module;
  bool instantiate;
  PersistentRootedObject importObj;

  CompileBufferTask(JSContext* cx, Handle<PromiseObject*> promise,
                    HandleObject importObj)
      : PromiseHelperTask(cx, promise),
        instantiate(true),
        importObj(cx, importObj) {}

  CompileBufferTask(JSContext* cx, Handle<PromiseObject*> promise)
      : PromiseHelperTask(cx, promise), instantiate(false) {}

  bool init(JSContext* cx, FeatureOptions options, const char* introducer) {
    compileArgs = InitCompileArgs(cx, options, introducer);
    if (!compileArgs) {
      return false;
    }
    return PromiseHelperTask::init(cx);
  }

  void execute() override {
    module = CompileBuffer(*compileArgs, BytecodeBufferOrSource(bytecode),
                           &error, &warnings, nullptr);
  }

  bool resolve(JSContext* cx, Handle<PromiseObject*> promise) override {
    if (!ReportCompileWarnings(cx, warnings)) {
      return false;
    }
    if (!module) {
      return Reject(cx, *compileArgs, promise, error);
    }
    if (instantiate) {
      return AsyncInstantiate(cx, *module, importObj, Ret::Pair, promise);
    }
    return ResolveCompile(cx, *module, promise);
  }
};

static bool RejectWithPendingException(JSContext* cx,
                                       Handle<PromiseObject*> promise,
                                       CallArgs& callArgs) {
  if (!RejectWithPendingException(cx, promise)) {
    return false;
  }

  callArgs.rval().setObject(*promise);
  return true;
}

static bool EnsurePromiseSupport(JSContext* cx) {
  if (!cx->runtime()->offThreadPromiseState.ref().initialized()) {
    JS_ReportErrorASCII(
        cx, "WebAssembly Promise APIs not supported in this runtime.");
    return false;
  }
  return true;
}

static bool WebAssembly_compile(JSContext* cx, unsigned argc, Value* vp) {
  if (!EnsurePromiseSupport(cx)) {
    return false;
  }

  Log(cx, "async compile() started");

  Rooted<PromiseObject*> promise(cx, PromiseObject::createSkippingExecutor(cx));
  if (!promise) {
    return false;
  }

  CallArgs callArgs = CallArgsFromVp(argc, vp);

  JS::RootedVector<JSString*> parameterStrings(cx);
  JS::RootedVector<Value> parameterArgs(cx);
  bool canCompileStrings = false;
  if (!cx->isRuntimeCodeGenEnabled(JS::RuntimeCode::WASM, nullptr,
                                   JS::CompilationType::Undefined,
                                   parameterStrings, nullptr, parameterArgs,
                                   NullHandleValue, &canCompileStrings)) {
    return RejectWithPendingException(cx, promise, callArgs);
  }
  if (!canCompileStrings) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_WASM, "WebAssembly.compile");
    return RejectWithPendingException(cx, promise, callArgs);
  }

  auto task = cx->make_unique<CompileBufferTask>(cx, promise);
  if (!task) {
    return false;
  }

  if (!callArgs.requireAtLeast(cx, "WebAssembly.compile", 1)) {
    return RejectWithPendingException(cx, promise, callArgs);
  }

  FeatureOptions options;
  if (!options.init(cx, callArgs.get(1))) {
    return RejectWithPendingException(cx, promise, callArgs);
  }

  if (!callArgs[0].isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_BUF_ARG);
    return RejectWithPendingException(cx, promise, callArgs);
  }

  Rooted<JSObject*> sourceObj(cx, &callArgs[0].toObject());
  if (!GetBytecodeBuffer(cx, sourceObj, JSMSG_WASM_BAD_BUF_ARG,
                         &task->bytecode)) {
    return RejectWithPendingException(cx, promise, callArgs);
  }

  if (!task->init(cx, options, "WebAssembly.compile")) {
    return false;
  }

  if (!StartOffThreadPromiseHelperTask(cx, std::move(task))) {
    return false;
  }

  callArgs.rval().setObject(*promise);
  return true;
}

static bool GetInstantiateArgs(JSContext* cx, const CallArgs& callArgs,
                               MutableHandleObject firstArg,
                               MutableHandleObject importObj,
                               MutableHandleValue featureOptions) {
  if (!callArgs.requireAtLeast(cx, "WebAssembly.instantiate", 1)) {
    return false;
  }

  if (!callArgs[0].isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_BUF_MOD_ARG);
    return false;
  }

  firstArg.set(&callArgs[0].toObject());

  if (!GetImportArg(cx, callArgs.get(1), importObj)) {
    return false;
  }

  featureOptions.set(callArgs.get(2));
  return true;
}

static bool WebAssembly_instantiate(JSContext* cx, unsigned argc, Value* vp) {
  if (!EnsurePromiseSupport(cx)) {
    return false;
  }

  Log(cx, "async instantiate() started");

  Rooted<PromiseObject*> promise(cx, PromiseObject::createSkippingExecutor(cx));
  if (!promise) {
    return false;
  }

  CallArgs callArgs = CallArgsFromVp(argc, vp);

  RootedObject firstArg(cx);
  RootedObject importObj(cx);
  RootedValue featureOptions(cx);
  if (!GetInstantiateArgs(cx, callArgs, &firstArg, &importObj,
                          &featureOptions)) {
    return RejectWithPendingException(cx, promise, callArgs);
  }

  const Module* module;
  if (IsModuleObject(firstArg, &module)) {
    if (!AsyncInstantiate(cx, *module, importObj, Ret::Instance, promise)) {
      return false;
    }
  } else {
    JS::RootedVector<JSString*> parameterStrings(cx);
    JS::RootedVector<Value> parameterArgs(cx);
    bool canCompileStrings = false;
    if (!cx->isRuntimeCodeGenEnabled(JS::RuntimeCode::WASM, nullptr,
                                     JS::CompilationType::Undefined,
                                     parameterStrings, nullptr, parameterArgs,
                                     NullHandleValue, &canCompileStrings)) {
      return RejectWithPendingException(cx, promise, callArgs);
    }
    if (!canCompileStrings) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_CSP_BLOCKED_WASM,
                                "WebAssembly.instantiate");
      return RejectWithPendingException(cx, promise, callArgs);
    }

    FeatureOptions options;
    if (!options.init(cx, featureOptions)) {
      return false;
    }

    auto task = cx->make_unique<CompileBufferTask>(cx, promise, importObj);
    if (!task || !task->init(cx, options, "WebAssembly.instantiate")) {
      return false;
    }

    if (!GetBytecodeBuffer(cx, firstArg, JSMSG_WASM_BAD_BUF_MOD_ARG,
                           &task->bytecode)) {
      return RejectWithPendingException(cx, promise, callArgs);
    }

    if (!StartOffThreadPromiseHelperTask(cx, std::move(task))) {
      return false;
    }
  }

  callArgs.rval().setObject(*promise);
  return true;
}

static bool WebAssembly_validate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs callArgs = CallArgsFromVp(argc, vp);

  if (!callArgs.requireAtLeast(cx, "WebAssembly.validate", 1)) {
    return false;
  }

  FeatureOptions options;
  if (!options.init(cx, callArgs.get(1))) {
    return false;
  }

  if (!callArgs[0].isObject()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_BUF_ARG);
    return false;
  }

  BytecodeSource source;
  Rooted<JSObject*> sourceObj(cx, &callArgs[0].toObject());
  if (!GetBytecodeSource(cx, sourceObj, JSMSG_WASM_BAD_BUF_ARG, &source)) {
    return false;
  }

  UniqueChars error;
  bool validated;
  {
    AutoPinBufferSourceLength pin(cx, sourceObj.get());
    validated = Validate(cx, source, options, &error);
  }

  // If the reason for validation failure was OOM (signalled by null error
  // message), report out-of-memory so that validate's return is always
  // correct.
  if (!validated && !error) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (error) {
    MOZ_ASSERT(!validated);
    Log(cx, "validate() failed with: %s", error.get());
  }

  callArgs.rval().setBoolean(validated);
  return true;
}

static bool EnsureStreamSupport(JSContext* cx) {
  // This should match wasm::StreamingCompilationAvailable().

  if (!EnsurePromiseSupport(cx)) {
    return false;
  }

  if (!CanUseExtraThreads()) {
    JS_ReportErrorASCII(
        cx, "WebAssembly.compileStreaming not supported with --no-threads");
    return false;
  }

  if (!cx->runtime()->consumeStreamCallback) {
    JS_ReportErrorASCII(cx,
                        "WebAssembly streaming not supported in this runtime");
    return false;
  }

  return true;
}

// This value is chosen and asserted to be disjoint from any host error code.
static const size_t StreamOOMCode = 0;

static bool RejectWithStreamErrorNumber(JSContext* cx, size_t errorCode,
                                        Handle<PromiseObject*> promise) {
  if (errorCode == StreamOOMCode) {
    ReportOutOfMemory(cx);
    return false;
  }

  cx->runtime()->reportStreamErrorCallback(cx, errorCode);
  return RejectWithPendingException(cx, promise);
}

class CompileStreamTask : public PromiseHelperTask, public JS::StreamConsumer {
  // The stream progresses monotonically through these states; the helper
  // thread wait()s for streamState_ to reach Closed.
  enum StreamState { Env, Code, Tail, Closed };
  ExclusiveWaitableData<StreamState> streamState_;

  // Immutable:
  const bool instantiate_;
  const PersistentRootedObject importObj_;

  // Immutable after noteResponseURLs() which is called at most once before
  // first call on stream thread:
  const MutableCompileArgs compileArgs_;

  // Immutable after Env state:
  MutableBytes envBytes_;
  BytecodeRange codeSection_;

  // The code section vector is resized once during the Env state and filled
  // in chunk by chunk during the Code state, updating the end-pointer after
  // each chunk:
  MutableBytes codeBytes_;
  uint8_t* codeBytesEnd_;
  ExclusiveBytesPtr exclusiveCodeBytesEnd_;

  // Immutable after Tail state:
  MutableBytes tailBytes_;
  ExclusiveStreamEndData exclusiveStreamEnd_;

  // Written once before Closed state and read in Closed state on main thread:
  SharedModule module_;
  Maybe<size_t> streamError_;
  UniqueChars compileError_;
  UniqueCharsVector warnings_;

  // Set on stream thread and read racily on helper thread to abort compilation:
  mozilla::Atomic<bool> streamFailed_;

  // Called on some thread before consumeChunk(), streamEnd(), streamError()):

  void noteResponseURLs(const char* url, const char* sourceMapUrl) override {
    if (url) {
      compileArgs_->scriptedCaller.filename = DuplicateString(url);
      compileArgs_->scriptedCaller.filenameIsURL = true;
    }
    if (sourceMapUrl) {
      compileArgs_->sourceMapURL = DuplicateString(sourceMapUrl);
    }
  }

  // Called on a stream thread:

  // Until StartOffThreadPromiseHelperTask succeeds, we are responsible for
  // dispatching ourselves back to the JS thread.
  //
  // Warning: After this function returns, 'this' can be deleted at any time, so
  // the caller must immediately return from the stream callback.
  void setClosedAndDestroyBeforeHelperThreadStarted() {
    streamState_.lock().get() = Closed;
    dispatchResolveAndDestroy();
  }

  // See setClosedAndDestroyBeforeHelperThreadStarted() comment.
  bool rejectAndDestroyBeforeHelperThreadStarted(size_t errorNumber) {
    MOZ_ASSERT(streamState_.lock() == Env);
    MOZ_ASSERT(!streamError_);
    streamError_ = Some(errorNumber);
    setClosedAndDestroyBeforeHelperThreadStarted();
    return false;
  }

  // Once StartOffThreadPromiseHelperTask succeeds, the helper thread will
  // dispatchResolveAndDestroy() after execute() returns, but execute()
  // wait()s for state to be Closed.
  //
  // Warning: After this function returns, 'this' can be deleted at any time, so
  // the caller must immediately return from the stream callback.
  void setClosedAndDestroyAfterHelperThreadStarted() {
    auto streamState = streamState_.lock();
    MOZ_ASSERT(streamState != Closed);
    streamState.get() = Closed;
    streamState.notify_one(/* stream closed */);
  }

  // See setClosedAndDestroyAfterHelperThreadStarted() comment.
  bool rejectAndDestroyAfterHelperThreadStarted(size_t errorNumber) {
    MOZ_ASSERT(!streamError_);
    streamError_ = Some(errorNumber);
    streamFailed_ = true;
    exclusiveCodeBytesEnd_.lock().notify_one();
    exclusiveStreamEnd_.lock().notify_one();
    setClosedAndDestroyAfterHelperThreadStarted();
    return false;
  }

  bool consumeChunk(const uint8_t* begin, size_t length) override {
    switch (streamState_.lock().get()) {
      case Env: {
        if (!envBytes_->append(begin, length)) {
          return rejectAndDestroyBeforeHelperThreadStarted(StreamOOMCode);
        }

        if (!StartsCodeSection(envBytes_->begin(), envBytes_->end(),
                               &codeSection_)) {
          return true;
        }

        uint32_t extraBytes = envBytes_->length() - codeSection_.start;
        if (extraBytes) {
          envBytes_->shrinkTo(codeSection_.start);
        }

        if (codeSection_.size() > MaxCodeSectionBytes) {
          return rejectAndDestroyBeforeHelperThreadStarted(StreamOOMCode);
        }

        if (!codeBytes_->vector.resize(codeSection_.size())) {
          return rejectAndDestroyBeforeHelperThreadStarted(StreamOOMCode);
        }

        codeBytesEnd_ = codeBytes_->begin();
        exclusiveCodeBytesEnd_.lock().get() = codeBytesEnd_;

        if (!StartOffThreadPromiseHelperTask(this)) {
          return rejectAndDestroyBeforeHelperThreadStarted(StreamOOMCode);
        }

        // Set the state to Code iff StartOffThreadPromiseHelperTask()
        // succeeds so that the state tells us whether we are before or
        // after the helper thread started.
        streamState_.lock().get() = Code;

        if (extraBytes) {
          return consumeChunk(begin + length - extraBytes, extraBytes);
        }

        return true;
      }
      case Code: {
        size_t copyLength =
            std::min<size_t>(length, codeBytes_->end() - codeBytesEnd_);
        memcpy(codeBytesEnd_, begin, copyLength);
        codeBytesEnd_ += copyLength;

        {
          auto codeStreamEnd = exclusiveCodeBytesEnd_.lock();
          codeStreamEnd.get() = codeBytesEnd_;
          codeStreamEnd.notify_one();
        }

        if (codeBytesEnd_ != codeBytes_->end()) {
          return true;
        }

        streamState_.lock().get() = Tail;

        if (uint32_t extraBytes = length - copyLength) {
          return consumeChunk(begin + copyLength, extraBytes);
        }

        return true;
      }
      case Tail: {
        if (!tailBytes_->append(begin, length)) {
          return rejectAndDestroyAfterHelperThreadStarted(StreamOOMCode);
        }

        return true;
      }
      case Closed:
        MOZ_CRASH("consumeChunk() in Closed state");
    }
    MOZ_CRASH("unreachable");
  }

  void streamEnd(
      JS::OptimizedEncodingListener* completeTier2Listener) override {
    switch (streamState_.lock().get()) {
      case Env: {
        BytecodeBuffer bytecode(envBytes_, nullptr, nullptr);
        module_ = CompileBuffer(*compileArgs_, BytecodeBufferOrSource(bytecode),
                                &compileError_, &warnings_, nullptr);
        setClosedAndDestroyBeforeHelperThreadStarted();
        return;
      }
      case Code:
      case Tail:
        // Unlock exclusiveStreamEnd_ before locking streamState_.
        {
          auto streamEnd = exclusiveStreamEnd_.lock();
          MOZ_ASSERT(!streamEnd->reached);
          streamEnd->reached = true;
          streamEnd->tailBytes = tailBytes_;
          streamEnd->completeTier2Listener = completeTier2Listener;
          streamEnd.notify_one();
        }
        setClosedAndDestroyAfterHelperThreadStarted();
        return;
      case Closed:
        MOZ_CRASH("streamEnd() in Closed state");
    }
  }

  void streamError(size_t errorCode) override {
    MOZ_ASSERT(errorCode != StreamOOMCode);
    switch (streamState_.lock().get()) {
      case Env:
        rejectAndDestroyBeforeHelperThreadStarted(errorCode);
        return;
      case Tail:
      case Code:
        rejectAndDestroyAfterHelperThreadStarted(errorCode);
        return;
      case Closed:
        MOZ_CRASH("streamError() in Closed state");
    }
  }

  void consumeOptimizedEncoding(const uint8_t* begin, size_t length) override {
    module_ = Module::deserialize(begin, length);

    MOZ_ASSERT(streamState_.lock().get() == Env);
    setClosedAndDestroyBeforeHelperThreadStarted();
  }

  // Called on a helper thread:

  void execute() override {
    module_ = CompileStreaming(*compileArgs_, *envBytes_, *codeBytes_,
                               exclusiveCodeBytesEnd_, exclusiveStreamEnd_,
                               streamFailed_, &compileError_, &warnings_);

    // When execute() returns, the CompileStreamTask will be dispatched
    // back to its JS thread to call resolve() and then be destroyed. We
    // can't let this happen until the stream has been closed lest
    // consumeChunk() or streamEnd() be called on a dead object.
    auto streamState = streamState_.lock();
    while (streamState != Closed) {
      streamState.wait(/* stream closed */);
    }
  }

  // Called on a JS thread after streaming compilation completes/errors:

  bool resolve(JSContext* cx, Handle<PromiseObject*> promise) override {
    MOZ_ASSERT(streamState_.lock() == Closed);

    if (!ReportCompileWarnings(cx, warnings_)) {
      return false;
    }
    if (module_) {
      MOZ_ASSERT(!streamFailed_ && !streamError_ && !compileError_);
      if (instantiate_) {
        return AsyncInstantiate(cx, *module_, importObj_, Ret::Pair, promise);
      }
      return ResolveCompile(cx, *module_, promise);
    }

    if (streamError_) {
      return RejectWithStreamErrorNumber(cx, *streamError_, promise);
    }

    return Reject(cx, *compileArgs_, promise, compileError_);
  }

 public:
  CompileStreamTask(JSContext* cx, Handle<PromiseObject*> promise,
                    CompileArgs& compileArgs, bool instantiate,
                    HandleObject importObj)
      : PromiseHelperTask(cx, promise),
        streamState_(mutexid::WasmStreamStatus, Env),
        instantiate_(instantiate),
        importObj_(cx, importObj),
        compileArgs_(&compileArgs),
        codeSection_{},
        codeBytesEnd_(nullptr),
        exclusiveCodeBytesEnd_(mutexid::WasmCodeBytesEnd, nullptr),
        exclusiveStreamEnd_(mutexid::WasmStreamEnd),
        streamFailed_(false) {
    MOZ_ASSERT_IF(importObj_, instantiate_);
  }

  [[nodiscard]] bool init(JSContext* cx) {
    envBytes_ = cx->new_<ShareableBytes>();
    if (!envBytes_) {
      return false;
    }

    codeBytes_ = js_new<ShareableBytes>();
    if (!codeBytes_) {
      return false;
    }

    tailBytes_ = js_new<ShareableBytes>();
    if (!tailBytes_) {
      return false;
    }

    return PromiseHelperTask::init(cx);
  }
};

// A short-lived object that captures the arguments of a
// WebAssembly.{compileStreaming,instantiateStreaming} while waiting for
// the Promise<Response> to resolve to a (hopefully) Promise.
class ResolveResponseClosure : public NativeObject {
  static const unsigned COMPILE_ARGS_SLOT = 0;
  static const unsigned PROMISE_OBJ_SLOT = 1;
  static const unsigned INSTANTIATE_SLOT = 2;
  static const unsigned IMPORT_OBJ_SLOT = 3;
  static const JSClassOps classOps_;

  static void finalize(JS::GCContext* gcx, JSObject* obj) {
    auto& closure = obj->as<ResolveResponseClosure>();
    gcx->release(obj, &closure.compileArgs(),
                 MemoryUse::WasmResolveResponseClosure);
  }

 public:
  static const unsigned RESERVED_SLOTS = 4;
  static const JSClass class_;

  static ResolveResponseClosure* create(JSContext* cx, const CompileArgs& args,
                                        HandleObject promise, bool instantiate,
                                        HandleObject importObj) {
    MOZ_ASSERT_IF(importObj, instantiate);

    AutoSetNewObjectMetadata metadata(cx);
    auto* obj = NewObjectWithGivenProto<ResolveResponseClosure>(cx, nullptr);
    if (!obj) {
      return nullptr;
    }

    args.AddRef();
    InitReservedSlot(obj, COMPILE_ARGS_SLOT, const_cast<CompileArgs*>(&args),
                     MemoryUse::WasmResolveResponseClosure);
    obj->setReservedSlot(PROMISE_OBJ_SLOT, ObjectValue(*promise));
    obj->setReservedSlot(INSTANTIATE_SLOT, BooleanValue(instantiate));
    obj->setReservedSlot(IMPORT_OBJ_SLOT, ObjectOrNullValue(importObj));
    return obj;
  }

  CompileArgs& compileArgs() const {
    return *(CompileArgs*)getReservedSlot(COMPILE_ARGS_SLOT).toPrivate();
  }
  PromiseObject& promise() const {
    return getReservedSlot(PROMISE_OBJ_SLOT).toObject().as<PromiseObject>();
  }
  bool instantiate() const {
    return getReservedSlot(INSTANTIATE_SLOT).toBoolean();
  }
  JSObject* importObj() const {
    return getReservedSlot(IMPORT_OBJ_SLOT).toObjectOrNull();
  }
};

const JSClassOps ResolveResponseClosure::classOps_ = {
    nullptr,                           // addProperty
    nullptr,                           // delProperty
    nullptr,                           // enumerate
    nullptr,                           // newEnumerate
    nullptr,                           // resolve
    nullptr,                           // mayResolve
    ResolveResponseClosure::finalize,  // finalize
    nullptr,                           // call
    nullptr,                           // construct
    nullptr,                           // trace
};

const JSClass ResolveResponseClosure::class_ = {
    "WebAssembly ResolveResponseClosure",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(ResolveResponseClosure::RESERVED_SLOTS) |
        JSCLASS_FOREGROUND_FINALIZE,
    &ResolveResponseClosure::classOps_,
};

static ResolveResponseClosure* ToResolveResponseClosure(const CallArgs& args) {
  return &args.callee()
              .as<JSFunction>()
              .getExtendedSlot(0)
              .toObject()
              .as<ResolveResponseClosure>();
}

static bool RejectWithErrorNumber(JSContext* cx, uint32_t errorNumber,
                                  Handle<PromiseObject*> promise) {
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber);
  return RejectWithPendingException(cx, promise);
}

static bool ResolveResponse_OnFulfilled(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs callArgs = CallArgsFromVp(argc, vp);

  Rooted<ResolveResponseClosure*> closure(cx,
                                          ToResolveResponseClosure(callArgs));
  Rooted<PromiseObject*> promise(cx, &closure->promise());
  CompileArgs& compileArgs = closure->compileArgs();
  bool instantiate = closure->instantiate();
  Rooted<JSObject*> importObj(cx, closure->importObj());

  auto task = cx->make_unique<CompileStreamTask>(cx, promise, compileArgs,
                                                 instantiate, importObj);
  if (!task || !task->init(cx)) {
    return false;
  }

  if (!callArgs.get(0).isObject()) {
    return RejectWithErrorNumber(cx, JSMSG_WASM_BAD_RESPONSE_VALUE, promise);
  }

  RootedObject response(cx, &callArgs.get(0).toObject());
  if (!cx->runtime()->consumeStreamCallback(cx, response, JS::MimeType::Wasm,
                                            task.get())) {
    return RejectWithPendingException(cx, promise);
  }

  (void)task.release();

  callArgs.rval().setUndefined();
  return true;
}

static bool ResolveResponse_OnRejected(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<ResolveResponseClosure*> closure(cx, ToResolveResponseClosure(args));
  Rooted<PromiseObject*> promise(cx, &closure->promise());

  if (!PromiseObject::reject(cx, promise, args.get(0))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool ResolveResponse(JSContext* cx, Handle<Value> responsePromise,
                            Handle<Value> featureOptions,
                            Handle<PromiseObject*> resultPromise,
                            bool instantiate = false,
                            HandleObject importObj = nullptr) {
  MOZ_ASSERT_IF(importObj, instantiate);

  const char* introducer = instantiate ? "WebAssembly.instantiateStreaming"
                                       : "WebAssembly.compileStreaming";

  FeatureOptions options;
  if (!options.init(cx, featureOptions)) {
    return false;
  }

  SharedCompileArgs compileArgs = InitCompileArgs(cx, options, introducer);
  if (!compileArgs) {
    return false;
  }

  RootedObject closure(
      cx, ResolveResponseClosure::create(cx, *compileArgs, resultPromise,
                                         instantiate, importObj));
  if (!closure) {
    return false;
  }

  RootedFunction onResolved(
      cx, NewNativeFunction(cx, ResolveResponse_OnFulfilled, 1, nullptr,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!onResolved) {
    return false;
  }

  RootedFunction onRejected(
      cx, NewNativeFunction(cx, ResolveResponse_OnRejected, 1, nullptr,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!onRejected) {
    return false;
  }

  onResolved->setExtendedSlot(0, ObjectValue(*closure));
  onRejected->setExtendedSlot(0, ObjectValue(*closure));

  RootedObject resolve(cx,
                       PromiseObject::unforgeableResolve(cx, responsePromise));
  if (!resolve) {
    return false;
  }

  return JS::AddPromiseReactions(cx, resolve, onResolved, onRejected);
}

static bool WebAssembly_compileStreaming(JSContext* cx, unsigned argc,
                                         Value* vp) {
  if (!EnsureStreamSupport(cx)) {
    return false;
  }

  Log(cx, "async compileStreaming() started");

  Rooted<PromiseObject*> resultPromise(
      cx, PromiseObject::createSkippingExecutor(cx));
  if (!resultPromise) {
    return false;
  }

  CallArgs callArgs = CallArgsFromVp(argc, vp);

  JS::RootedVector<JSString*> parameterStrings(cx);
  JS::RootedVector<Value> parameterArgs(cx);
  bool canCompileStrings = false;
  if (!cx->isRuntimeCodeGenEnabled(JS::RuntimeCode::WASM, nullptr,
                                   JS::CompilationType::Undefined,
                                   parameterStrings, nullptr, parameterArgs,
                                   NullHandleValue, &canCompileStrings)) {
    return RejectWithPendingException(cx, resultPromise, callArgs);
  }
  if (!canCompileStrings) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_WASM,
                              "WebAssembly.compileStreaming");
    return RejectWithPendingException(cx, resultPromise, callArgs);
  }

  Rooted<Value> responsePromise(cx, callArgs.get(0));
  Rooted<Value> featureOptions(cx, callArgs.get(1));
  if (!ResolveResponse(cx, responsePromise, featureOptions, resultPromise)) {
    return RejectWithPendingException(cx, resultPromise, callArgs);
  }

  callArgs.rval().setObject(*resultPromise);
  return true;
}

static bool WebAssembly_instantiateStreaming(JSContext* cx, unsigned argc,
                                             Value* vp) {
  if (!EnsureStreamSupport(cx)) {
    return false;
  }

  Log(cx, "async instantiateStreaming() started");

  Rooted<PromiseObject*> resultPromise(
      cx, PromiseObject::createSkippingExecutor(cx));
  if (!resultPromise) {
    return false;
  }

  CallArgs callArgs = CallArgsFromVp(argc, vp);

  JS::RootedVector<JSString*> parameterStrings(cx);
  JS::RootedVector<Value> parameterArgs(cx);
  bool canCompileStrings = false;
  if (!cx->isRuntimeCodeGenEnabled(JS::RuntimeCode::WASM, nullptr,
                                   JS::CompilationType::Undefined,
                                   parameterStrings, nullptr, parameterArgs,
                                   NullHandleValue, &canCompileStrings)) {
    return RejectWithPendingException(cx, resultPromise, callArgs);
  }
  if (!canCompileStrings) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_WASM,
                              "WebAssembly.instantiateStreaming");
    return RejectWithPendingException(cx, resultPromise, callArgs);
  }

  Rooted<JSObject*> firstArg(cx);
  Rooted<JSObject*> importObj(cx);
  Rooted<Value> featureOptions(cx);
  if (!GetInstantiateArgs(cx, callArgs, &firstArg, &importObj,
                          &featureOptions)) {
    return RejectWithPendingException(cx, resultPromise, callArgs);
  }
  Rooted<Value> responsePromise(cx, ObjectValue(*firstArg.get()));

  if (!ResolveResponse(cx, responsePromise, featureOptions, resultPromise, true,
                       importObj)) {
    return RejectWithPendingException(cx, resultPromise, callArgs);
  }

  callArgs.rval().setObject(*resultPromise);
  return true;
}

#ifdef ENABLE_WASM_JSPI
const ClassSpec WasmSuspendingObject::classSpec_ = {
    GenericCreateConstructor<construct, 1, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<WasmSuspendingObject>,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

const JSClass WasmSuspendingObject::class_ = {
    "Suspending",
    JSCLASS_HAS_RESERVED_SLOTS(WasmSuspendingObject::RESERVED_SLOTS),
    JS_NULL_CLASS_OPS,
    &classSpec_,
};

const JSClass& WasmSuspendingObject::protoClass_ = PlainObject::class_;

/* static */
bool WasmSuspendingObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "WebAssembly.Suspending")) {
    return false;
  }

  if (!args.requireAtLeast(cx, "WebAssembly.Suspending", 1)) {
    return false;
  }

  if (!IsCallableNonCCW(args[0])) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_FUNCTION_VALUE);
    return false;
  }

  RootedObject callable(cx, &args[0].toObject());
  Rooted<WasmSuspendingObject*> suspending(
      cx, NewBuiltinClassInstance<WasmSuspendingObject>(cx));
  if (!suspending) {
    return false;
  }
  suspending->setWrappedFunction(callable);
  args.rval().setObject(*suspending);
  return true;
}

static bool WebAssembly_promising(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "WebAssembly.promising", 1)) {
    return false;
  }

  if (!IsWasmFunction(args[0])) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_FUNCTION_VALUE);
    return false;
  }

  RootedObject func(cx, &args[0].toObject());
  RootedFunction promise(
      cx, WasmPromisingFunctionCreate(cx, func, wasm::ValTypeVector(),
                                      wasm::ValTypeVector()));
  if (!promise) {
    return false;
  }
  args.rval().setObject(*promise);
  return true;
}

static const JSFunctionSpec WebAssembly_jspi_methods[] = {
    JS_FN("promising", WebAssembly_promising, 1, JSPROP_ENUMERATE),
    JS_FS_END,
};

bool js::IsWasmSuspendingObject(JSObject* obj) {
  return obj->is<WasmSuspendingObject>();
}

JSObject* js::MaybeUnwrapSuspendingObject(JSObject* wrapper) {
  if (!wrapper->is<WasmSuspendingObject>()) {
    return nullptr;
  }
  return wrapper->as<WasmSuspendingObject>().wrappedFunction();
}
#else
bool js::IsWasmSuspendingObject(JSObject* obj) { return false; }
#endif  // ENABLE_WASM_JSPI

#ifdef ENABLE_WASM_MOZ_INTGEMM

static bool WebAssembly_mozIntGemm(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<WasmModuleObject*> module(cx);
  if (!wasm::CompileBuiltinModule(cx, wasm::BuiltinModuleId::IntGemm,
                                  &module)) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().set(ObjectValue(*module.get()));
  return true;
}

static const JSFunctionSpec WebAssembly_mozIntGemm_methods[] = {
    JS_FN("mozIntGemm", WebAssembly_mozIntGemm, 0, JSPROP_ENUMERATE),
    JS_FS_END,
};

#endif  // ENABLE_WASM_MOZ_INTGEMM

static const JSFunctionSpec WebAssembly_static_methods[] = {
    JS_FN("toSource", WebAssembly_toSource, 0, 0),
    JS_FN("compile", WebAssembly_compile, 1, JSPROP_ENUMERATE),
    JS_FN("instantiate", WebAssembly_instantiate, 1, JSPROP_ENUMERATE),
    JS_FN("validate", WebAssembly_validate, 1, JSPROP_ENUMERATE),
    JS_FN("compileStreaming", WebAssembly_compileStreaming, 1,
          JSPROP_ENUMERATE),
    JS_FN("instantiateStreaming", WebAssembly_instantiateStreaming, 1,
          JSPROP_ENUMERATE),
    JS_FS_END,
};

static const JSPropertySpec WebAssembly_static_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "WebAssembly", JSPROP_READONLY),
    JS_PS_END,
};

static JSObject* CreateWebAssemblyObject(JSContext* cx, JSProtoKey key) {
  MOZ_RELEASE_ASSERT(HasSupport(cx));

  RootedObject proto(cx, &cx->global()->getObjectPrototype());
  return NewTenuredObjectWithGivenProto(cx, &WasmNamespaceObject::class_,
                                        proto);
}

struct NameAndProtoKey {
  const char* const name;
  JSProtoKey key;
};

static bool WebAssemblyDefineConstructor(JSContext* cx,
                                         Handle<WasmNamespaceObject*> wasm,
                                         NameAndProtoKey entry,
                                         MutableHandleValue ctorValue,
                                         MutableHandleId id) {
  JSObject* ctor = GlobalObject::getOrCreateConstructor(cx, entry.key);
  if (!ctor) {
    return false;
  }
  ctorValue.setObject(*ctor);

  JSAtom* className = Atomize(cx, entry.name, strlen(entry.name));
  if (!className) {
    return false;
  }
  id.set(AtomToId(className));

  return DefineDataProperty(cx, wasm, id, ctorValue, 0);
}

static bool WebAssemblyClassFinish(JSContext* cx, HandleObject object,
                                   HandleObject proto) {
  Handle<WasmNamespaceObject*> wasm = object.as<WasmNamespaceObject>();

  constexpr NameAndProtoKey entries[] = {
      {"Module", JSProto_WasmModule},
      {"Instance", JSProto_WasmInstance},
      {"Memory", JSProto_WasmMemory},
      {"Table", JSProto_WasmTable},
      {"Global", JSProto_WasmGlobal},
      {"CompileError", GetExceptionProtoKey(JSEXN_WASMCOMPILEERROR)},
      {"LinkError", GetExceptionProtoKey(JSEXN_WASMLINKERROR)},
      {"RuntimeError", GetExceptionProtoKey(JSEXN_WASMRUNTIMEERROR)},
#ifdef ENABLE_WASM_TYPE_REFLECTIONS
      {"Function", JSProto_WasmFunction},
#endif
  };
  RootedValue ctorValue(cx);
  RootedId id(cx);
  for (const auto& entry : entries) {
    if (!WebAssemblyDefineConstructor(cx, wasm, entry, &ctorValue, &id)) {
      return false;
    }
  }

  constexpr NameAndProtoKey exceptionEntries[] = {
      {"Tag", JSProto_WasmTag},
      {"Exception", JSProto_WasmException},
  };
  for (const auto& entry : exceptionEntries) {
    if (!WebAssemblyDefineConstructor(cx, wasm, entry, &ctorValue, &id)) {
      return false;
    }
  }

  RootedObject tagProto(
      cx, GlobalObject::getOrCreatePrototype(cx, JSProto_WasmTag));
  if (!tagProto) {
    ReportOutOfMemory(cx);
    return false;
  }

  SharedTagType wrappedJSValueTagType(sWrappedJSValueTagType);
  WasmTagObject* wrappedJSValueTagObject =
      WasmTagObject::create(cx, wrappedJSValueTagType, tagProto);
  if (!wrappedJSValueTagObject) {
    return false;
  }

  wasm->setWrappedJSValueTag(wrappedJSValueTagObject);

  if (ExnRefAvailable(cx)) {
    RootedId jsTagName(cx, NameToId(cx->names().jsTag));
    RootedValue jsTagValue(cx, ObjectValue(*wrappedJSValueTagObject));
    if (!DefineDataProperty(cx, wasm, jsTagName, jsTagValue,
                            JSPROP_READONLY | JSPROP_ENUMERATE)) {
      return false;
    }
  }

#ifdef ENABLE_WASM_JSPI
  constexpr NameAndProtoKey jspiEntries[] = {
      {"Suspending", JSProto_WasmSuspending},
      {"SuspendError", GetExceptionProtoKey(JSEXN_WASMSUSPENDERROR)},
  };
  if (JSPromiseIntegrationAvailable(cx)) {
    if (!JS_DefineFunctions(cx, wasm, WebAssembly_jspi_methods)) {
      return false;
    }
    for (const auto& entry : jspiEntries) {
      if (!WebAssemblyDefineConstructor(cx, wasm, entry, &ctorValue, &id)) {
        return false;
      }
    }
  }
#endif

#ifdef ENABLE_WASM_MOZ_INTGEMM
  if (MozIntGemmAvailable(cx) &&
      !JS_DefineFunctions(cx, wasm, WebAssembly_mozIntGemm_methods)) {
    return false;
  }
#endif

  return true;
}

WasmNamespaceObject* WasmNamespaceObject::getOrCreate(JSContext* cx) {
  JSObject* wasm =
      GlobalObject::getOrCreateConstructor(cx, JSProto_WebAssembly);
  if (!wasm) {
    return nullptr;
  }
  return &wasm->as<WasmNamespaceObject>();
}

static const ClassSpec WebAssemblyClassSpec = {
    CreateWebAssemblyObject,       nullptr, WebAssembly_static_methods,
    WebAssembly_static_properties, nullptr, nullptr,
    WebAssemblyClassFinish,
};

const JSClass js::WasmNamespaceObject::class_ = {
    "WebAssembly",
    JSCLASS_HAS_CACHED_PROTO(JSProto_WebAssembly) |
        JSCLASS_HAS_RESERVED_SLOTS(WasmNamespaceObject::RESERVED_SLOTS),
    JS_NULL_CLASS_OPS,
    &WebAssemblyClassSpec,
};

// Sundry
