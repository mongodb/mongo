/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ScopeExit.h"

#include "jsapi.h"
#include "jspubtd.h"

#include "fuzz-tests/tests.h"
#include "js/CallAndConstruct.h"
#include "js/PropertyAndElement.h"  // JS_Enumerate, JS_GetProperty, JS_GetPropertyById, JS_HasProperty, JS_SetProperty
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/TypedArrayObject.h"

#include "wasm/WasmCompile.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmTable.h"

#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;
using namespace js::wasm;

// These are defined and pre-initialized by the harness (in tests.cpp).
extern JS::PersistentRootedObject gGlobal;
extern JSContext* gCx;

static bool gIsWasmSmith = false;
extern "C" {
size_t gluesmith(uint8_t* data, size_t size, uint8_t* out, size_t maxsize);
}

static int testWasmInit(int* argc, char*** argv) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(gCx);
  if (!wasmHasSupport ||
      !GlobalObject::getOrCreateConstructor(gCx, JSProto_WebAssembly)) {
    MOZ_CRASH("Failed to initialize wasm support");
  }

  return 0;
}

static int testWasmSmithInit(int* argc, char*** argv) {
  gIsWasmSmith = true;
  return testWasmInit(argc, argv);
}

static bool emptyNativeFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setUndefined();
  return true;
}

static bool callExportedFunc(HandleFunction func,
                             MutableHandleValue lastReturnVal) {
  // TODO: We can specify a thisVal here.
  RootedValue thisVal(gCx, UndefinedValue());
  JS::RootedValueVector args(gCx);

  if (!lastReturnVal.isNull() && !lastReturnVal.isUndefined() &&
      !args.append(lastReturnVal)) {
    return false;
  }

  RootedValue returnVal(gCx);
  if (!Call(gCx, thisVal, func, args, &returnVal)) {
    gCx->clearPendingException();
  } else {
    lastReturnVal.set(returnVal);
  }

  return true;
}

template <typename T>
static bool assignImportKind(const Import& import, HandleObject obj,
                             HandleObject lastExportsObj,
                             JS::Handle<JS::IdVector> lastExportIds,
                             size_t* currentExportId, size_t exportsLength,
                             HandleValue defaultValue) {
  RootedId fieldName(gCx);
  if (!import.field.toPropertyKey(gCx, &fieldName)) {
    return false;
  }
  bool assigned = false;
  while (*currentExportId < exportsLength) {
    RootedValue propVal(gCx);
    if (!JS_GetPropertyById(gCx, lastExportsObj,
                            lastExportIds[*currentExportId], &propVal)) {
      return false;
    }

    (*currentExportId)++;

    if (propVal.isObject() && propVal.toObject().is<T>()) {
      if (!JS_SetPropertyById(gCx, obj, fieldName, propVal)) {
        return false;
      }

      assigned = true;
      break;
    }
  }
  if (!assigned) {
    if (!JS_SetPropertyById(gCx, obj, fieldName, defaultValue)) {
      return false;
    }
  }
  return true;
}

static bool FuzzerBuildId(JS::BuildIdCharVector* buildId) {
  const char buildid[] = "testWasmFuzz";
  return buildId->append(buildid, sizeof(buildid));
}

static int testWasmFuzz(const uint8_t* buf, size_t size) {
  auto gcGuard = mozilla::MakeScopeExit([&] {
    JS::PrepareForFullGC(gCx);
    JS::NonIncrementalGC(gCx, JS::GCOptions::Normal, JS::GCReason::API);
  });

  JS::SetProcessBuildIdOp(FuzzerBuildId);

  const size_t MINIMUM_MODULE_SIZE = 8;

  // The smallest valid wasm module is 8 bytes and we need 1 byte for size
  if (size < MINIMUM_MODULE_SIZE + 1) return 0;

  size_t currentIndex = 0;

  // Store the last non-empty exports object and its enumerated Ids here
  RootedObject lastExportsObj(gCx);
  JS::Rooted<JS::IdVector> lastExportIds(gCx, JS::IdVector(gCx));

  // Store the last return value so we can pass it in as an argument during
  // the next call (which can be on another module as well).
  RootedValue lastReturnVal(gCx);

  while (size - currentIndex >= MINIMUM_MODULE_SIZE + 1) {
    // Ensure we have no lingering exceptions from previous modules
    gCx->clearPendingException();

    uint16_t moduleLen;
    if (gIsWasmSmith) {
      // Jump over the optByte. Unlike with the regular format, for
      // wasm-smith we are fixing this and use byte 0 as opt-byte.
      // Eventually this will also be changed for the regular format.
      if (!currentIndex) {
        currentIndex++;
      }

      // Caller ensures the structural soundness of the input here
      moduleLen = *((uint16_t*)&buf[currentIndex]);
      currentIndex += 2;
    } else {
      moduleLen = buf[currentIndex];
      currentIndex++;
    }

    if (size - currentIndex < moduleLen) {
      moduleLen = size - currentIndex;
    }

    if (moduleLen < MINIMUM_MODULE_SIZE) {
      continue;
    }

    if (currentIndex == 1 || (gIsWasmSmith && currentIndex == 3)) {
      // If this is the first module we are reading, we use the first
      // few bytes to tweak some settings. These are fixed anyway and
      // overwritten later on.
      uint8_t optByte;
      if (gIsWasmSmith) {
        optByte = (uint8_t)buf[0];
      } else {
        optByte = (uint8_t)buf[currentIndex];
      }

      // Note that IonPlatformSupport() does not take into account whether
      // the compiler supports particular features that may have been enabled.
      bool enableWasmBaseline = ((optByte & 0xF0) == (1 << 7));
      bool enableWasmOptimizing =
          IonPlatformSupport() && ((optByte & 0xF0) == (1 << 6));
      bool enableWasmAwaitTier2 =
          (IonPlatformSupport()) && ((optByte & 0xF) == (1 << 3));

      if (!enableWasmBaseline && !enableWasmOptimizing) {
        // If nothing is selected explicitly, enable an optimizing compiler to
        // test more platform specific JIT code. However, on some platforms,
        // e.g. ARM64 on Windows, we do not have Ion available, so we need to
        // switch to baseline instead.
        if (IonPlatformSupport()) {
          enableWasmOptimizing = true;
        } else {
          enableWasmBaseline = true;
        }
      }

      if (enableWasmAwaitTier2) {
        // Tier 2 needs Baseline + Optimizing
        enableWasmBaseline = true;

        if (!enableWasmOptimizing) {
          enableWasmOptimizing = true;
        }
      }

      JS::ContextOptionsRef(gCx)
          .setWasmBaseline(enableWasmBaseline)
          .setWasmIon(enableWasmOptimizing)
          .setTestWasmAwaitTier2(enableWasmAwaitTier2);
    }

    // Expected header for a valid WebAssembly module
    uint32_t magic_header = 0x6d736100;
    uint32_t magic_version = 0x1;

    if (gIsWasmSmith) {
      // When using wasm-smith, magic values should already be there.
      // Checking this to make sure the data passed is sane.
      MOZ_RELEASE_ASSERT(*(uint32_t*)(&buf[currentIndex]) == magic_header,
                         "Magic header mismatch!");
      MOZ_RELEASE_ASSERT(*(uint32_t*)(&buf[currentIndex + 4]) == magic_version,
                         "Magic version mismatch!");
    }

    // We just skip over the first 8 bytes now because we fill them
    // with `magic_header` and `magic_version` anyway.
    currentIndex += 8;
    moduleLen -= 8;

    Rooted<WasmInstanceObject*> instanceObj(gCx);

    MutableBytes bytecode = gCx->new_<ShareableBytes>();
    if (!bytecode || !bytecode->append((uint8_t*)&magic_header, 4) ||
        !bytecode->append((uint8_t*)&magic_version, 4) ||
        !bytecode->append(&buf[currentIndex], moduleLen)) {
      return 0;
    }

    currentIndex += moduleLen;

    ScriptedCaller scriptedCaller;
    FeatureOptions options;
    SharedCompileArgs compileArgs =
        CompileArgs::buildAndReport(gCx, std::move(scriptedCaller), options);
    if (!compileArgs) {
      return 0;
    }

    UniqueChars error;
    UniqueCharsVector warnings;
    SharedModule module =
        CompileBuffer(*compileArgs, *bytecode, &error, &warnings);
    if (!module) {
      // We should always have a valid module if we are using wasm-smith. Check
      // that no error is reported, signalling an OOM.
      MOZ_RELEASE_ASSERT(!gIsWasmSmith || !error);
      continue;
    }

    // At this point we have a valid module and we should try to ensure
    // that its import requirements are met for instantiation.
    const ImportVector& importVec = module->imports();

    // Empty native function used to fill in function import slots if we
    // run out of functions exported by other modules.
    JS::RootedFunction emptyFunction(gCx);
    emptyFunction =
        JS_NewFunction(gCx, emptyNativeFunction, 0, 0, "emptyFunction");

    if (!emptyFunction) {
      return 0;
    }

    RootedValue emptyFunctionValue(gCx, ObjectValue(*emptyFunction));
    RootedValue nullValue(gCx, NullValue());

    RootedObject importObj(gCx, JS_NewPlainObject(gCx));

    if (!importObj) {
      return 0;
    }

    size_t exportsLength = lastExportIds.length();
    size_t currentFunctionExportId = 0;
    size_t currentTableExportId = 0;
    size_t currentMemoryExportId = 0;
    size_t currentGlobalExportId = 0;
    size_t currentTagExportId = 0;

    for (const Import& import : importVec) {
      RootedId moduleName(gCx);
      if (!import.module.toPropertyKey(gCx, &moduleName)) {
        return false;
      }
      RootedId fieldName(gCx);
      if (!import.field.toPropertyKey(gCx, &fieldName)) {
        return false;
      }

      // First try to get the namespace object, create one if this is the
      // first time.
      RootedValue v(gCx);
      if (!JS_GetPropertyById(gCx, importObj, moduleName, &v) ||
          !v.isObject()) {
        // Insert empty object at importObj[moduleName]
        RootedObject plainObj(gCx, JS_NewPlainObject(gCx));

        if (!plainObj) {
          return 0;
        }

        RootedValue plainVal(gCx, ObjectValue(*plainObj));
        if (!JS_SetPropertyById(gCx, importObj, moduleName, plainVal)) {
          return 0;
        }

        // Get the object we just inserted, store in v, ensure it is an
        // object (no proxies or other magic at work).
        if (!JS_GetPropertyById(gCx, importObj, moduleName, &v) ||
            !v.isObject()) {
          return 0;
        }
      }

      RootedObject obj(gCx, &v.toObject());
      bool found = false;
      if (JS_HasPropertyById(gCx, obj, fieldName, &found) && !found) {
        // Insert i-th export object that fits the type requirement
        // at `v[fieldName]`.

        switch (import.kind) {
          case DefinitionKind::Function:
            if (!assignImportKind<JSFunction>(
                    import, obj, lastExportsObj, lastExportIds,
                    &currentFunctionExportId, exportsLength,
                    emptyFunctionValue)) {
              return 0;
            }
            break;

          case DefinitionKind::Table:
            // TODO: Pass a dummy defaultValue
            if (!assignImportKind<WasmTableObject>(
                    import, obj, lastExportsObj, lastExportIds,
                    &currentTableExportId, exportsLength, nullValue)) {
              return 0;
            }
            break;

          case DefinitionKind::Memory:
            // TODO: Pass a dummy defaultValue
            if (!assignImportKind<WasmMemoryObject>(
                    import, obj, lastExportsObj, lastExportIds,
                    &currentMemoryExportId, exportsLength, nullValue)) {
              return 0;
            }
            break;

          case DefinitionKind::Global:
            // TODO: Pass a dummy defaultValue
            if (!assignImportKind<WasmGlobalObject>(
                    import, obj, lastExportsObj, lastExportIds,
                    &currentGlobalExportId, exportsLength, nullValue)) {
              return 0;
            }
            break;

          case DefinitionKind::Tag:
            // TODO: Pass a dummy defaultValue
            if (!assignImportKind<WasmTagObject>(
                    import, obj, lastExportsObj, lastExportIds,
                    &currentTagExportId, exportsLength, nullValue)) {
              return 0;
            }
            break;
        }
      }
    }

    Rooted<ImportValues> imports(gCx);
    if (!GetImports(gCx, *module, importObj, imports.address())) {
      continue;
    }

    if (!module->instantiate(gCx, imports.get(), nullptr, &instanceObj)) {
      continue;
    }

    // At this module we have a valid WebAssembly module instance.

    RootedObject exportsObj(gCx, &instanceObj->exportsObj());
    JS::Rooted<JS::IdVector> exportIds(gCx, JS::IdVector(gCx));
    if (!JS_Enumerate(gCx, exportsObj, &exportIds)) {
      continue;
    }

    if (!exportIds.length()) {
      continue;
    }

    // Store the last exports for re-use later
    lastExportsObj = exportsObj;
    lastExportIds.get() = std::move(exportIds.get());

    for (size_t i = 0; i < lastExportIds.length(); i++) {
      RootedValue propVal(gCx);
      if (!JS_GetPropertyById(gCx, exportsObj, lastExportIds[i], &propVal)) {
        return 0;
      }

      if (propVal.isObject()) {
        RootedObject propObj(gCx, &propVal.toObject());

        if (propObj->is<JSFunction>()) {
          RootedFunction func(gCx, &propObj->as<JSFunction>());

          if (!callExportedFunc(func, &lastReturnVal)) {
            return 0;
          }
        }

        if (propObj->is<WasmTableObject>()) {
          Rooted<WasmTableObject*> tableObj(gCx,
                                            &propObj->as<WasmTableObject>());
          size_t tableLen = tableObj->table().length();

          RootedValue tableGetVal(gCx);
          if (!JS_GetProperty(gCx, tableObj, "get", &tableGetVal)) {
            return 0;
          }
          RootedFunction tableGet(gCx,
                                  &tableGetVal.toObject().as<JSFunction>());

          for (size_t i = 0; i < tableLen; i++) {
            JS::RootedValueVector tableGetArgs(gCx);
            if (!tableGetArgs.append(NumberValue(uint32_t(i)))) {
              return 0;
            }

            RootedValue readFuncValue(gCx);
            if (!Call(gCx, tableObj, tableGet, tableGetArgs, &readFuncValue)) {
              return 0;
            }

            if (readFuncValue.isNull()) {
              continue;
            }

            RootedFunction callee(gCx,
                                  &readFuncValue.toObject().as<JSFunction>());

            if (!callExportedFunc(callee, &lastReturnVal)) {
              return 0;
            }
          }
        }

        if (propObj->is<WasmMemoryObject>()) {
          Rooted<WasmMemoryObject*> memory(gCx,
                                           &propObj->as<WasmMemoryObject>());
          size_t byteLen = memory->volatileMemoryLength();
          if (byteLen) {
            // Read the bounds of the buffer to ensure it is valid.
            // AddressSanitizer would detect any out-of-bounds here.
            uint8_t* rawMemory = memory->buffer().dataPointerEither().unwrap();
            volatile uint8_t rawMemByte = 0;
            rawMemByte += rawMemory[0];
            rawMemByte += rawMemory[byteLen - 1];
            (void)rawMemByte;
          }
        }

        if (propObj->is<WasmGlobalObject>()) {
          Rooted<WasmGlobalObject*> global(gCx,
                                           &propObj->as<WasmGlobalObject>());
          if (global->type() != ValType::I64) {
            global->val().get().toJSValue(gCx, &lastReturnVal);
          }
        }
      }
    }
  }

  return 0;
}

static int testWasmSmithFuzz(const uint8_t* buf, size_t size) {
  // Define maximum sizes for the input to wasm-smith as well
  // as the resulting modules. The input to output size factor
  // of wasm-smith is somewhat variable but a factor of 4 seems
  // to roughly work out. The logic below also assumes that these
  // are powers of 2.
  const size_t maxInputSize = 1024;
  const size_t maxModuleSize = 4096;

  size_t maxModules = size / maxInputSize + 1;

  // We need 1 leading byte for options and 2 bytes for size per module
  uint8_t* out =
      new uint8_t[1 + maxModules * (maxModuleSize + sizeof(uint16_t))];

  auto deleteGuard = mozilla::MakeScopeExit([&] { delete[] out; });

  // Copy the opt-byte.
  out[0] = buf[0];

  size_t outIndex = 1;
  size_t currentIndex = 1;

  while (currentIndex < size) {
    size_t remaining = size - currentIndex;

    // We need to have at least a size and some byte to read.
    if (remaining <= sizeof(uint16_t)) {
      break;
    }

    // Determine size of the next input, limited to `maxInputSize`.
    uint16_t inSize =
        (*((uint16_t*)&buf[currentIndex]) & (maxInputSize - 1)) + 1;
    remaining -= sizeof(uint16_t);
    currentIndex += sizeof(uint16_t);

    // Cap to remaining bytes.
    inSize = remaining >= inSize ? inSize : remaining;

    size_t outSize =
        gluesmith((uint8_t*)&buf[currentIndex], inSize,
                  out + outIndex + sizeof(uint16_t), maxModuleSize);

    if (!outSize) {
      break;
    }

    currentIndex += inSize;

    // Write the size of the resulting module to our output buffer.
    *(uint16_t*)(&out[outIndex]) = (uint16_t)outSize;
    outIndex += sizeof(uint16_t) + outSize;
  }

  // If we lack at least one module, don't do anything.
  if (outIndex == 1) {
    return 0;
  }

  return testWasmFuzz(out, outIndex);
}

MOZ_FUZZING_INTERFACE_RAW(testWasmInit, testWasmFuzz, Wasm);
MOZ_FUZZING_INTERFACE_RAW(testWasmSmithInit, testWasmSmithFuzz, WasmSmith);
