/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/TestingFunctions.h"

#include "mozilla/Atomics.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#ifdef JS_HAS_INTL_API
#  include "mozilla/intl/ICU4CLibrary.h"
#  include "mozilla/intl/Locale.h"
#  include "mozilla/intl/String.h"
#  include "mozilla/intl/TimeZone.h"
#endif
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Span.h"
#include "mozilla/Sprintf.h"
#include "mozilla/TextUtils.h"
#include "mozilla/ThreadLocal.h"

#include <algorithm>
#include <cfloat>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <utility>

#if defined(XP_UNIX) && !defined(XP_DARWIN)
#  include <time.h>
#else
#  include <chrono>
#endif

#include "fdlibm.h"
#include "jsapi.h"
#include "jsfriendapi.h"

#ifdef JS_HAS_INTL_API
#  include "builtin/intl/CommonFunctions.h"
#  include "builtin/intl/FormatBuffer.h"
#  include "builtin/intl/SharedIntlData.h"
#endif
#include "builtin/BigInt.h"
#include "builtin/JSON.h"
#include "builtin/MapObject.h"
#include "builtin/Promise.h"
#include "builtin/TestingUtility.h"  // js::ParseCompileOptions, js::ParseDebugMetadata
#include "ds/IdValuePair.h"          // js::IdValuePair
#include "frontend/BytecodeCompiler.h"  // frontend::{CompileGlobalScriptToExtensibleStencil,ParseModuleToExtensibleStencil}
#include "frontend/CompilationStencil.h"  // frontend::CompilationStencil
#include "frontend/FrontendContext.h"     // AutoReportFrontendContext
#include "gc/GC.h"
#include "gc/GCEnum.h"
#include "gc/GCLock.h"
#include "gc/Zone.h"
#include "jit/BaselineJIT.h"
#include "jit/Disassemble.h"
#include "jit/InlinableNatives.h"
#include "jit/Invalidation.h"
#include "jit/Ion.h"
#include "jit/JitOptions.h"
#include "jit/JitRuntime.h"
#include "jit/TrialInlining.h"
#include "js/Array.h"        // JS::NewArrayObject
#include "js/ArrayBuffer.h"  // JS::{DetachArrayBuffer,GetArrayBufferLengthAndData,NewArrayBufferWithContents}
#include "js/CallAndConstruct.h"  // JS::Call, JS::IsCallable, JS::IsConstructor, JS_CallFunction
#include "js/CharacterEncoding.h"
#include "js/CompilationAndEvaluation.h"
#include "js/CompileOptions.h"
#include "js/Conversions.h"
#include "js/Date.h"
#include "js/experimental/CodeCoverage.h"  // js::GetCodeCoverageSummary
#include "js/experimental/CompileScript.h"  // JS::ParseGlobalScript, JS::PrepareForInstantiate
#include "js/experimental/JSStencil.h"         // JS::Stencil
#include "js/experimental/PCCountProfiling.h"  // JS::{Start,Stop}PCCountProfiling, JS::PurgePCCounts, JS::GetPCCountScript{Count,Summary,Contents}
#include "js/experimental/TypedData.h"         // JS_GetObjectAsUint8Array
#include "js/friend/DumpFunctions.h"  // js::Dump{Backtrace,Heap,Object}, JS::FormatStackDump, js::IgnoreNurseryObjects
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/WindowProxy.h"    // js::ToWindowProxyIfWindow
#include "js/GlobalObject.h"
#include "js/HashTable.h"
#include "js/Interrupt.h"
#include "js/LocaleSensitive.h"
#include "js/Prefs.h"
#include "js/Printf.h"
#include "js/PropertyAndElement.h"  // JS_DefineProperties, JS_DefineProperty, JS_DefinePropertyById, JS_Enumerate, JS_GetProperty, JS_GetPropertyById, JS_HasProperty, JS_SetElement, JS_SetProperty
#include "js/PropertySpec.h"
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "js/Stack.h"
#include "js/String.h"  // JS::GetLinearStringLength, JS::StringToLinearString
#include "js/StructuredClone.h"
#include "js/UbiNode.h"
#include "js/UbiNodeBreadthFirst.h"
#include "js/UbiNodeShortestPaths.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "js/Wrapper.h"
#include "threading/CpuCount.h"
#include "util/DifferentialTesting.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/BooleanObject.h"
#include "vm/DateObject.h"
#include "vm/DateTime.h"
#include "vm/ErrorObject.h"
#include "vm/GlobalObject.h"
#include "vm/HelperThreads.h"
#include "vm/HelperThreadState.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/NumberObject.h"
#include "vm/PlainObject.h"    // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject, js::PromiseSlot_*
#include "vm/ProxyObject.h"
#include "vm/RealmFuses.h"
#include "vm/SavedStacks.h"
#include "vm/ScopeKind.h"
#include "vm/Stack.h"
#include "vm/StencilCache.h"   // DelazificationCache
#include "vm/StencilObject.h"  // StencilObject, StencilXDRBufferObject
#include "vm/StringObject.h"
#include "vm/StringType.h"
#include "wasm/AsmJS.h"
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmBuiltinModule.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmValue.h"

#include "debugger/DebugAPI-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectFlags-inl.h"
#include "vm/StringType-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;

using mozilla::AssertedCast;
using mozilla::AsWritableChars;
using mozilla::Maybe;
using mozilla::Span;

using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::SourceOwnership;
using JS::SourceText;

// If fuzzingSafe is set, remove functionality that could cause problems with
// fuzzers. Set this via the environment variable MOZ_FUZZING_SAFE.
mozilla::Atomic<bool> js::fuzzingSafe(false);

// If disableOOMFunctions is set, disable functionality that causes artificial
// OOM conditions.
static mozilla::Atomic<bool> disableOOMFunctions(false);

static bool EnvVarIsDefined(const char* name) {
  const char* value = getenv(name);
  return value && *value;
}

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
static bool EnvVarAsInt(const char* name, int* valueOut) {
  if (!EnvVarIsDefined(name)) {
    return false;
  }

  *valueOut = atoi(getenv(name));
  return true;
}
#endif

static bool GetRealmConfiguration(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());
  RootedObject info(cx, JS_NewPlainObject(cx));
  if (!info) {
    return false;
  }
  if (args.length() > 1) {
    ReportUsageErrorASCII(cx, callee, "Must have zero or one arguments");
    return false;
  }
  if (args.length() == 1 && !args[0].isString()) {
    ReportUsageErrorASCII(cx, callee, "Argument must be a string");
    return false;
  }

  bool importAttributes = cx->options().importAttributes();
  if (!JS_SetProperty(cx, info, "importAttributes",
                      importAttributes ? TrueHandleValue : FalseHandleValue)) {
    return false;
  }

  if (args.length() == 1) {
    RootedString str(cx, ToString(cx, args[0]));
    if (!str) {
      return false;
    }
    RootedId id(cx);
    if (!JS_StringToId(cx, str, &id)) {
      return false;
    }

    bool hasProperty;
    if (JS_HasPropertyById(cx, info, id, &hasProperty) && hasProperty) {
      // Returning a true/false from GetProperty
      return GetProperty(cx, info, info, id, args.rval());
    }

    ReportUsageErrorASCII(cx, callee, "Invalid option name");
    return false;
  }

  args.rval().setObject(*info);
  return true;
}

static bool GetBuildConfiguration(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());
  RootedObject info(cx, JS_NewPlainObject(cx));
  if (!info) {
    return false;
  }
  if (args.length() > 1) {
    ReportUsageErrorASCII(cx, callee, "Must have zero or one arguments");
    return false;
  }
  if (args.length() == 1 && !args[0].isString()) {
    ReportUsageErrorASCII(cx, callee, "Argument must be a string");
    return false;
  }

  if (!JS_SetProperty(cx, info, "rooting-analysis", FalseHandleValue)) {
    return false;
  }

  if (!JS_SetProperty(cx, info, "exact-rooting", TrueHandleValue)) {
    return false;
  }

  if (!JS_SetProperty(cx, info, "trace-jscalls-api", FalseHandleValue)) {
    return false;
  }

  if (!JS_SetProperty(cx, info, "incremental-gc", TrueHandleValue)) {
    return false;
  }

  if (!JS_SetProperty(cx, info, "generational-gc", TrueHandleValue)) {
    return false;
  }

  if (!JS_SetProperty(cx, info, "oom-backtraces", FalseHandleValue)) {
    return false;
  }

  RootedValue value(cx);
#ifdef DEBUG
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "debug", value)) {
    return false;
  }

#ifdef RELEASE_OR_BETA
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "release_or_beta", value)) {
    return false;
  }

#ifdef EARLY_BETA_OR_EARLIER
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "early_beta_or_earlier", value)) {
    return false;
  }

#ifdef MOZ_CODE_COVERAGE
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "coverage", value)) {
    return false;
  }

#ifdef JS_HAS_CTYPES
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "has-ctypes", value)) {
    return false;
  }

#if defined(_M_IX86) || defined(__i386__)
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "x86", value)) {
    return false;
  }

#if defined(_M_X64) || defined(__x86_64__)
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "x64", value)) {
    return false;
  }

#ifdef JS_CODEGEN_ARM
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "arm", value)) {
    return false;
  }

#ifdef JS_SIMULATOR_ARM
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "arm-simulator", value)) {
    return false;
  }

#ifdef ANDROID
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "android", value)) {
    return false;
  }

#ifdef XP_WIN
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "windows", value)) {
    return false;
  }

#ifdef XP_MACOSX
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "osx", value)) {
    return false;
  }

#ifdef JS_CODEGEN_ARM64
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "arm64", value)) {
    return false;
  }

#ifdef JS_SIMULATOR_ARM64
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "arm64-simulator", value)) {
    return false;
  }

#ifdef JS_CODEGEN_MIPS32
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "mips32", value)) {
    return false;
  }

#ifdef JS_CODEGEN_MIPS64
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "mips64", value)) {
    return false;
  }

#ifdef JS_SIMULATOR_MIPS32
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "mips32-simulator", value)) {
    return false;
  }

#ifdef JS_SIMULATOR_MIPS64
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "mips64-simulator", value)) {
    return false;
  }

#ifdef JS_SIMULATOR
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "simulator", value)) {
    return false;
  }

#ifdef __wasi__
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "wasi", value)) {
    return false;
  }

#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "pbl", value)) {
    return false;
  }

#ifdef JS_CODEGEN_LOONG64
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "loong64", value)) {
    return false;
  }

#ifdef JS_SIMULATOR_LOONG64
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "loong64-simulator", value)) {
    return false;
  }

#ifdef JS_CODEGEN_RISCV64
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "riscv64", value)) {
    return false;
  }

#ifdef JS_SIMULATOR_RISCV64
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "riscv64-simulator", value)) {
    return false;
  }

#ifdef MOZ_ASAN
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "asan", value)) {
    return false;
  }

#ifdef MOZ_TSAN
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "tsan", value)) {
    return false;
  }

#ifdef MOZ_UBSAN
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "ubsan", value)) {
    return false;
  }

#ifdef JS_GC_ZEAL
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "has-gczeal", value)) {
    return false;
  }

#ifdef MOZ_PROFILING
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "profiling", value)) {
    return false;
  }

#ifdef INCLUDE_MOZILLA_DTRACE
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "dtrace", value)) {
    return false;
  }

#ifdef MOZ_VALGRIND
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "valgrind", value)) {
    return false;
  }

#ifdef JS_HAS_INTL_API
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "intl-api", value)) {
    return false;
  }

#if defined(SOLARIS)
  value = BooleanValue(false);
#else
  value = BooleanValue(true);
#endif
  if (!JS_SetProperty(cx, info, "mapped-array-buffer", value)) {
    return false;
  }

#ifdef MOZ_MEMORY
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "moz-memory", value)) {
    return false;
  }

  value.setInt32(sizeof(void*));
  if (!JS_SetProperty(cx, info, "pointer-byte-size", value)) {
    return false;
  }

#ifdef ENABLE_DECORATORS
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "decorators", value)) {
    return false;
  }

#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "explicit-resource-management", value)) {
    return false;
  }

#ifdef FUZZING
  value = BooleanValue(true);
#else
  value = BooleanValue(false);
#endif
  if (!JS_SetProperty(cx, info, "fuzzing-defined", value)) {
    return false;
  }

  value = Int32Value(JSFatInlineString::MAX_LENGTH_LATIN1);
  if (!JS_SetProperty(cx, info, "inline-latin1-chars", value)) {
    return false;
  }

  value = Int32Value(JSFatInlineString::MAX_LENGTH_TWO_BYTE);
  if (!JS_SetProperty(cx, info, "inline-two-byte-chars", value)) {
    return false;
  }

  value = Int32Value(JSThinInlineString::MAX_LENGTH_LATIN1);
  if (!JS_SetProperty(cx, info, "thin-inline-latin1-chars", value)) {
    return false;
  }

  value = Int32Value(JSThinInlineString::MAX_LENGTH_TWO_BYTE);
  if (!JS_SetProperty(cx, info, "thin-inline-two-byte-chars", value)) {
    return false;
  }

  if (js::ThinInlineAtom::EverInstantiated) {
    value = Int32Value(js::ThinInlineAtom::MAX_LENGTH_LATIN1);
    if (!JS_SetProperty(cx, info, "thin-inline-atom-latin1-chars", value)) {
      return false;
    }

    value = Int32Value(js::ThinInlineAtom::MAX_LENGTH_TWO_BYTE);
    if (!JS_SetProperty(cx, info, "thin-inline-atom-two-byte-chars", value)) {
      return false;
    }
  }

  value = Int32Value(js::FatInlineAtom::MAX_LENGTH_LATIN1);
  if (!JS_SetProperty(cx, info, "fat-inline-atom-latin1-chars", value)) {
    return false;
  }

  value = Int32Value(js::FatInlineAtom::MAX_LENGTH_TWO_BYTE);
  if (!JS_SetProperty(cx, info, "fat-inline-atom-two-byte-chars", value)) {
    return false;
  }

  if (args.length() == 1) {
    RootedString str(cx, ToString(cx, args[0]));
    if (!str) {
      return false;
    }
    RootedId id(cx);
    if (!JS_StringToId(cx, str, &id)) {
      return false;
    }

    bool hasProperty;
    if (JS_HasPropertyById(cx, info, id, &hasProperty) && hasProperty) {
      // Returning a true/false from GetProperty
      return GetProperty(cx, info, info, id, args.rval());
    }

    ReportUsageErrorASCII(cx, callee, "Invalid option name");
    return false;
  }

  args.rval().setObject(*info);
  return true;
}

static bool IsLCovEnabled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(coverage::IsLCovEnabled());
  return true;
}

static bool TrialInline(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setUndefined();

  FrameIter iter(cx);
  if (iter.done() || !iter.isBaseline() || iter.realm() != cx->realm()) {
    return true;
  }

  jit::BaselineFrame* frame = iter.abstractFramePtr().asBaselineFrame();
  if (!jit::CanIonCompileScript(cx, frame->script())) {
    return true;
  }

  return jit::DoTrialInlining(cx, frame);
}

static bool ReturnStringCopy(JSContext* cx, CallArgs& args,
                             const char* message) {
  JSString* str = JS_NewStringCopyZ(cx, message);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool MaybeGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JS_MaybeGC(cx);
  args.rval().setUndefined();
  return true;
}

static bool GC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  /*
   * If the first argument is 'zone', we collect any zones previously
   * scheduled for GC via schedulegc. If the first argument is an object, we
   * collect the object's zone (and any other zones scheduled for
   * GC). Otherwise, we collect all zones.
   */
  bool zone = false;
  if (args.length() >= 1) {
    Value arg = args[0];
    if (arg.isString()) {
      if (!JS_StringEqualsLiteral(cx, arg.toString(), "zone", &zone)) {
        return false;
      }
    } else if (arg.isObject()) {
      PrepareZoneForGC(cx, UncheckedUnwrap(&arg.toObject())->zone());
      zone = true;
    }
  }

  JS::GCOptions options = JS::GCOptions::Normal;
  JS::GCReason reason = JS::GCReason::API;
  if (args.length() >= 2) {
    Value arg = args[1];
    if (arg.isString()) {
      bool shrinking = false;
      bool last_ditch = false;
      if (!JS_StringEqualsLiteral(cx, arg.toString(), "shrinking",
                                  &shrinking)) {
        return false;
      }
      if (!JS_StringEqualsLiteral(cx, arg.toString(), "last-ditch",
                                  &last_ditch)) {
        return false;
      }
      if (shrinking) {
        options = JS::GCOptions::Shrink;
      } else if (last_ditch) {
        options = JS::GCOptions::Shrink;
        reason = JS::GCReason::LAST_DITCH;
      }
    }
  }

  size_t preBytes = cx->runtime()->gc.heapSize.bytes();

  if (zone) {
    PrepareForDebugGC(cx->runtime());
  } else {
    JS::PrepareForFullGC(cx);
  }

  JS::NonIncrementalGC(cx, options, reason);

  char buf[256] = {'\0'};
  if (!js::SupportDifferentialTesting()) {
    SprintfLiteral(buf, "before %zu, after %zu\n", preBytes,
                   cx->runtime()->gc.heapSize.bytes());
  }
  return ReturnStringCopy(cx, args, buf);
}

static bool MinorGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.get(0) == BooleanValue(true)) {
    gc::GCRuntime& gc = cx->runtime()->gc;
    if (gc.nursery().isEnabled()) {
      gc.storeBuffer().setAboutToOverflow(JS::GCReason::FULL_GENERIC_BUFFER);
    }
  }

  cx->minorGC(JS::GCReason::API);
  args.rval().setUndefined();
  return true;
}

#define PARAM_NAME_LIST_ENTRY(name, key, writable) " " name
#define GC_PARAMETER_ARGS_LIST FOR_EACH_GC_PARAM(PARAM_NAME_LIST_ENTRY)

static bool GCParameter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JSString* str = ToString(cx, args.get(0));
  if (!str) {
    return false;
  }

  UniqueChars name = EncodeLatin1(cx, str);
  if (!name) {
    return false;
  }

  JSGCParamKey param;
  bool writable;
  if (!GetGCParameterInfo(name.get(), &param, &writable)) {
    JS_ReportErrorASCII(
        cx, "the first argument must be one of:" GC_PARAMETER_ARGS_LIST);
    return false;
  }

  // Request mode.
  if (args.length() == 1) {
    uint32_t value = JS_GetGCParameter(cx, param);
    args.rval().setNumber(value);
    return true;
  }

  if (!writable) {
    JS_ReportErrorASCII(cx, "Attempt to change read-only parameter %s",
                        name.get());
    return false;
  }

  if (disableOOMFunctions) {
    switch (param) {
      case JSGC_MAX_BYTES:
      case JSGC_MAX_NURSERY_BYTES:
        args.rval().setUndefined();
        return true;
      default:
        break;
    }
  }

  double d;
  if (!ToNumber(cx, args[1], &d)) {
    return false;
  }

  if (d < 0 || d > UINT32_MAX) {
    JS_ReportErrorASCII(cx, "Parameter value out of range");
    return false;
  }

  uint32_t value = floor(d);
  bool ok = cx->runtime()->gc.setParameter(cx, param, value);
  if (!ok) {
    JS_ReportErrorASCII(cx, "Parameter value out of range");
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool RelazifyFunctions(JSContext* cx, unsigned argc, Value* vp) {
  // Relazifying functions on GC is usually only done for compartments that are
  // not active. To aid fuzzing, this testing function allows us to relazify
  // even if the compartment is active.

  CallArgs args = CallArgsFromVp(argc, vp);

  // Disable relazification of all scripts on stack. It is a pervasive
  // assumption in the engine that running scripts still have bytecode.
  for (AllScriptFramesIter i(cx); !i.done(); ++i) {
    i.script()->clearAllowRelazify();
  }

  cx->runtime()->allowRelazificationForTesting = true;

  JS::PrepareForFullGC(cx);
  JS::NonIncrementalGC(cx, JS::GCOptions::Shrink, JS::GCReason::API);

  cx->runtime()->allowRelazificationForTesting = false;

  args.rval().setUndefined();
  return true;
}

static bool IsProxy(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1) {
    JS_ReportErrorASCII(cx, "the function takes exactly one argument");
    return false;
  }
  if (!args[0].isObject()) {
    args.rval().setBoolean(false);
    return true;
  }
  args.rval().setBoolean(args[0].toObject().is<ProxyObject>());
  return true;
}

static bool WasmIsSupported(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  args.rval().setBoolean(wasmHasSupport && wasm::AnyCompilerAvailable(cx));
  return true;
}

static bool WasmIsSupportedByHardware(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(wasm::HasPlatformSupport());
  return true;
}

static bool WasmDebuggingEnabled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  args.rval().setBoolean(wasmHasSupport && wasm::BaselineAvailable(cx));
  return true;
}

static bool WasmStreamingEnabled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(wasm::StreamingCompilationAvailable(cx));
  return true;
}

static bool WasmCachingEnabled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(wasm::CodeCachingAvailable(cx));
  return true;
}

static bool WasmHugeMemorySupported(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
#ifdef WASM_SUPPORTS_HUGE_MEMORY
  args.rval().setBoolean(true);
#else
  args.rval().setBoolean(false);
#endif
  return true;
}

static bool WasmMaxMemoryPages(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() < 1) {
    JS_ReportErrorASCII(cx, "not enough arguments");
    return false;
  }
  if (!args.get(0).isString()) {
    JS_ReportErrorASCII(cx, "index type must be a string");
    return false;
  }
  RootedString s(cx, args.get(0).toString());
  Rooted<JSLinearString*> ls(cx, s->ensureLinear(cx));
  if (!ls) {
    return false;
  }
  if (StringEqualsLiteral(ls, "i32")) {
    args.rval().setInt32(
        int32_t(wasm::MaxMemoryPages(wasm::IndexType::I32).value()));
    return true;
  }
  if (StringEqualsLiteral(ls, "i64")) {
#ifdef ENABLE_WASM_MEMORY64
    if (wasm::Memory64Available(cx)) {
      args.rval().setInt32(
          int32_t(wasm::MaxMemoryPages(wasm::IndexType::I64).value()));
      return true;
    }
#endif
    JS_ReportErrorASCII(cx, "memory64 not enabled");
    return false;
  }
  JS_ReportErrorASCII(cx, "bad index type");
  return false;
}

static bool WasmThreadsEnabled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(wasm::ThreadsAvailable(cx));
  return true;
}

#define WASM_FEATURE(NAME, ...)                                              \
  static bool Wasm##NAME##Enabled(JSContext* cx, unsigned argc, Value* vp) { \
    CallArgs args = CallArgsFromVp(argc, vp);                                \
    args.rval().setBoolean(wasm::NAME##Available(cx));                       \
    return true;                                                             \
  }
JS_FOR_WASM_FEATURES(WASM_FEATURE);
#undef WASM_FEATURE

static bool WasmSimdEnabled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(wasm::SimdAvailable(cx));
  return true;
}

static bool WasmCompilersPresent(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  char buf[256];
  *buf = 0;
  if (wasm::BaselinePlatformSupport()) {
    strcat(buf, "baseline");
  }
  if (wasm::IonPlatformSupport()) {
    if (*buf) {
      strcat(buf, ",");
    }
    strcat(buf, "ion");
  }

  JSString* result = JS_NewStringCopyZ(cx, buf);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

static bool WasmCompileMode(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // This triplet of predicates will select zero or one baseline compiler and
  // zero or one optimizing compiler.
  bool baseline = wasm::BaselineAvailable(cx);
  bool ion = wasm::IonAvailable(cx);
  bool none = !baseline && !ion;
  bool tiered = baseline && ion;

  JSStringBuilder result(cx);
  if (none && !result.append("none")) {
    return false;
  }
  if (baseline && !result.append("baseline")) {
    return false;
  }
  if (tiered && !result.append("+")) {
    return false;
  }
  if (ion && !result.append("ion")) {
    return false;
  }
  if (JSString* str = result.finishString()) {
    args.rval().setString(str);
    return true;
  }
  return false;
}

static bool WasmBaselineDisabledByFeatures(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  bool isDisabled = false;
  JSStringBuilder reason(cx);
  if (!wasm::BaselineDisabledByFeatures(cx, &isDisabled, &reason)) {
    return false;
  }
  if (isDisabled) {
    JSString* result = reason.finishString();
    if (!result) {
      return false;
    }
    args.rval().setString(result);
  } else {
    args.rval().setBoolean(false);
  }
  return true;
}

static bool WasmIonDisabledByFeatures(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  bool isDisabled = false;
  JSStringBuilder reason(cx);
  if (!wasm::IonDisabledByFeatures(cx, &isDisabled, &reason)) {
    return false;
  }
  if (isDisabled) {
    JSString* result = reason.finishString();
    if (!result) {
      return false;
    }
    args.rval().setString(result);
  } else {
    args.rval().setBoolean(false);
  }
  return true;
}

#ifdef ENABLE_WASM_SIMD
#  ifdef DEBUG
static char lastAnalysisResult[1024];

namespace js {
namespace wasm {
void ReportSimdAnalysis(const char* data) {
  strncpy(lastAnalysisResult, data, sizeof(lastAnalysisResult));
  lastAnalysisResult[sizeof(lastAnalysisResult) - 1] = 0;
}
}  // namespace wasm
}  // namespace js

// Unstable API for white-box testing of SIMD optimizations.
//
// Current API: takes no arguments, returns a string describing the last Simd
// simplification applied.

static bool WasmSimdAnalysis(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSString* result =
      JS_NewStringCopyZ(cx, *lastAnalysisResult ? lastAnalysisResult : "none");
  if (!result) {
    return false;
  }
  args.rval().setString(result);
  *lastAnalysisResult = (char)0;
  return true;
}
#  endif
#endif

static bool WasmGlobalFromArrayBuffer(JSContext* cx, unsigned argc, Value* vp) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  if (!wasmHasSupport) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() < 2) {
    JS_ReportErrorASCII(cx, "not enough arguments");
    return false;
  }

  // Get the type of the value
  wasm::ValType valType;
  if (!wasm::ToValType(cx, args.get(0), &valType)) {
    return false;
  }

  // Get the array buffer for the value
  if (!args.get(1).isObject() ||
      !args.get(1).toObject().is<ArrayBufferObject>()) {
    JS_ReportErrorASCII(cx, "argument is not an array buffer");
    return false;
  }
  Rooted<ArrayBufferObject*> buffer(
      cx, &args.get(1).toObject().as<ArrayBufferObject>());

  // Only allow POD to be created from bytes
  switch (valType.kind()) {
    case wasm::ValType::I32:
    case wasm::ValType::I64:
    case wasm::ValType::F32:
    case wasm::ValType::F64:
    case wasm::ValType::V128:
      break;
    default:
      JS_ReportErrorASCII(
          cx, "invalid valtype for creating WebAssembly.Global from bytes");
      return false;
  }

  // Check we have all the bytes we need
  if (valType.size() != buffer->byteLength()) {
    JS_ReportErrorASCII(cx, "array buffer has incorrect size");
    return false;
  }

  // Copy the bytes from buffer into a tagged val
  wasm::RootedVal val(cx);
  val.get().initFromRootedLocation(valType, buffer->dataPointer());

  // Create the global object
  RootedObject proto(
      cx, GlobalObject::getOrCreatePrototype(cx, JSProto_WasmGlobal));
  if (!proto) {
    return false;
  }
  Rooted<WasmGlobalObject*> result(
      cx, WasmGlobalObject::create(cx, val, false, proto));
  if (!result) {
    return false;
  }

  args.rval().setObject(*result.get());
  return true;
}

enum class LaneInterp {
  I32x4,
  I64x2,
  F32x4,
  F64x2,
};

size_t LaneInterpLanes(LaneInterp interp) {
  switch (interp) {
    case LaneInterp::I32x4:
      return 4;
    case LaneInterp::I64x2:
      return 2;
    case LaneInterp::F32x4:
      return 4;
    case LaneInterp::F64x2:
      return 2;
    default:
      MOZ_ASSERT_UNREACHABLE();
      return 0;
  }
}

static bool ToLaneInterp(JSContext* cx, HandleValue v, LaneInterp* out) {
  RootedString interpStr(cx, ToString(cx, v));
  if (!interpStr) {
    return false;
  }
  Rooted<JSLinearString*> interpLinearStr(cx, interpStr->ensureLinear(cx));
  if (!interpLinearStr) {
    return false;
  }

  if (StringEqualsLiteral(interpLinearStr, "i32x4")) {
    *out = LaneInterp::I32x4;
    return true;
  } else if (StringEqualsLiteral(interpLinearStr, "i64x2")) {
    *out = LaneInterp::I64x2;
    return true;
  } else if (StringEqualsLiteral(interpLinearStr, "f32x4")) {
    *out = LaneInterp::F32x4;
    return true;
  } else if (StringEqualsLiteral(interpLinearStr, "f64x2")) {
    *out = LaneInterp::F64x2;
    return true;
  }

  JS_ReportErrorASCII(cx, "invalid lane interpretation");
  return false;
}

static bool WasmGlobalExtractLane(JSContext* cx, unsigned argc, Value* vp) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  if (!wasmHasSupport) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() < 3) {
    JS_ReportErrorASCII(cx, "not enough arguments");
    return false;
  }

  // Get the global value
  if (!args.get(0).isObject() ||
      !args.get(0).toObject().is<WasmGlobalObject>()) {
    JS_ReportErrorASCII(cx, "argument is not wasm value");
    return false;
  }
  Rooted<WasmGlobalObject*> global(
      cx, &args.get(0).toObject().as<WasmGlobalObject>());

  // Check that we have a v128 value
  if (global->type().kind() != wasm::ValType::V128) {
    JS_ReportErrorASCII(cx, "global is not a v128 value");
    return false;
  }
  wasm::V128 v128 = global->val().get().v128();

  // Get the passed interpretation of lanes
  LaneInterp interp;
  if (!ToLaneInterp(cx, args.get(1), &interp)) {
    return false;
  }

  // Get the lane to extract
  int32_t lane;
  if (!ToInt32(cx, args.get(2), &lane)) {
    return false;
  }

  // Check that the lane interp is valid
  if (lane < 0 || size_t(lane) >= LaneInterpLanes(interp)) {
    JS_ReportErrorASCII(cx, "invalid lane for interp");
    return false;
  }

  wasm::RootedVal val(cx);
  switch (interp) {
    case LaneInterp::I32x4: {
      uint32_t i;
      v128.extractLane<uint32_t>(lane, &i);
      val.set(wasm::Val(i));
      break;
    }
    case LaneInterp::I64x2: {
      uint64_t i;
      v128.extractLane<uint64_t>(lane, &i);
      val.set(wasm::Val(i));
      break;
    }
    case LaneInterp::F32x4: {
      float f;
      v128.extractLane<float>(lane, &f);
      val.set(wasm::Val(f));
      break;
    }
    case LaneInterp::F64x2: {
      double d;
      v128.extractLane<double>(lane, &d);
      val.set(wasm::Val(d));
      break;
    }
    default:
      MOZ_ASSERT_UNREACHABLE();
  }

  RootedObject proto(
      cx, GlobalObject::getOrCreatePrototype(cx, JSProto_WasmGlobal));
  Rooted<WasmGlobalObject*> result(
      cx, WasmGlobalObject::create(cx, val, false, proto));
  args.rval().setObject(*result.get());
  return true;
}

static bool WasmGlobalsEqual(JSContext* cx, unsigned argc, Value* vp) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  if (!wasmHasSupport) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() < 2) {
    JS_ReportErrorASCII(cx, "not enough arguments");
    return false;
  }

  if (!args.get(0).isObject() ||
      !args.get(0).toObject().is<WasmGlobalObject>() ||
      !args.get(1).isObject() ||
      !args.get(1).toObject().is<WasmGlobalObject>()) {
    JS_ReportErrorASCII(cx, "argument is not wasm value");
    return false;
  }

  Rooted<WasmGlobalObject*> a(cx,
                              &args.get(0).toObject().as<WasmGlobalObject>());
  Rooted<WasmGlobalObject*> b(cx,
                              &args.get(1).toObject().as<WasmGlobalObject>());

  if (a->type().kind() != b->type().kind()) {
    JS_ReportErrorASCII(cx, "globals are of different kind");
    return false;
  }

  bool result;
  const wasm::Val& aVal = a->val().get();
  const wasm::Val& bVal = b->val().get();
  switch (a->type().kind()) {
    case wasm::ValType::I32: {
      result = aVal.i32() == bVal.i32();
      break;
    }
    case wasm::ValType::I64: {
      result = aVal.i64() == bVal.i64();
      break;
    }
    case wasm::ValType::F32: {
      result = mozilla::BitwiseCast<uint32_t>(aVal.f32()) ==
               mozilla::BitwiseCast<uint32_t>(bVal.f32());
      break;
    }
    case wasm::ValType::F64: {
      result = mozilla::BitwiseCast<uint64_t>(aVal.f64()) ==
               mozilla::BitwiseCast<uint64_t>(bVal.f64());
      break;
    }
    case wasm::ValType::V128: {
      // Don't know the interpretation of the v128, so we only can do an exact
      // bitwise equality. Testing code can use wasmGlobalExtractLane to
      // workaround this if needed.
      result = aVal.v128() == bVal.v128();
      break;
    }
    case wasm::ValType::Ref: {
      result = aVal.ref() == bVal.ref();
      break;
    }
    default:
      JS_ReportErrorASCII(cx, "unsupported type");
      return false;
  }
  args.rval().setBoolean(result);
  return true;
}

// Flavors of NaN values for WebAssembly.
// See
// https://webassembly.github.io/spec/core/syntax/values.html#floating-point.
enum class NaNFlavor {
  // A canonical NaN value.
  //  - the sign bit is unspecified,
  //  - the 8-bit exponent is set to all 1s
  //  - the MSB of the payload is set to 1 (a quieted NaN) and all others to 0.
  Canonical,
  // An arithmetic NaN. This is the same as a canonical NaN including that the
  // payload MSB is set to 1, but one or more of the remaining payload bits MAY
  // BE set to 1 (a canonical NaN specifies all 0s).
  Arithmetic,
};

static bool IsNaNFlavor(uint32_t bits, NaNFlavor flavor) {
  switch (flavor) {
    case NaNFlavor::Canonical: {
      return (bits & 0x7fffffff) == 0x7fc00000;
    }
    case NaNFlavor::Arithmetic: {
      const uint32_t ArithmeticNaN = 0x7f800000;
      const uint32_t ArithmeticPayloadMSB = 0x00400000;
      bool isNaN = (bits & ArithmeticNaN) == ArithmeticNaN;
      bool isMSBSet = (bits & ArithmeticPayloadMSB) == ArithmeticPayloadMSB;
      return isNaN && isMSBSet;
    }
    default:
      MOZ_CRASH();
  }
}

static bool IsNaNFlavor(uint64_t bits, NaNFlavor flavor) {
  switch (flavor) {
    case NaNFlavor::Canonical: {
      return (bits & 0x7fffffffffffffff) == 0x7ff8000000000000;
    }
    case NaNFlavor::Arithmetic: {
      uint64_t ArithmeticNaN = 0x7ff0000000000000;
      uint64_t ArithmeticPayloadMSB = 0x0008000000000000;
      bool isNaN = (bits & ArithmeticNaN) == ArithmeticNaN;
      bool isMsbSet = (bits & ArithmeticPayloadMSB) == ArithmeticPayloadMSB;
      return isNaN && isMsbSet;
    }
    default:
      MOZ_CRASH();
  }
}

static bool ToNaNFlavor(JSContext* cx, HandleValue v, NaNFlavor* out) {
  RootedString flavorStr(cx, ToString(cx, v));
  if (!flavorStr) {
    return false;
  }
  Rooted<JSLinearString*> flavorLinearStr(cx, flavorStr->ensureLinear(cx));
  if (!flavorLinearStr) {
    return false;
  }

  if (StringEqualsLiteral(flavorLinearStr, "canonical_nan")) {
    *out = NaNFlavor::Canonical;
    return true;
  } else if (StringEqualsLiteral(flavorLinearStr, "arithmetic_nan")) {
    *out = NaNFlavor::Arithmetic;
    return true;
  }

  JS_ReportErrorASCII(cx, "invalid nan flavor");
  return false;
}

static bool WasmGlobalIsNaN(JSContext* cx, unsigned argc, Value* vp) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  if (!wasmHasSupport) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() < 2) {
    JS_ReportErrorASCII(cx, "not enough arguments");
    return false;
  }

  if (!args.get(0).isObject() ||
      !args.get(0).toObject().is<WasmGlobalObject>()) {
    JS_ReportErrorASCII(cx, "argument is not wasm value");
    return false;
  }
  Rooted<WasmGlobalObject*> global(
      cx, &args.get(0).toObject().as<WasmGlobalObject>());

  NaNFlavor flavor;
  if (!ToNaNFlavor(cx, args.get(1), &flavor)) {
    return false;
  }

  bool result;
  const wasm::Val& val = global->val().get();
  switch (global->type().kind()) {
    case wasm::ValType::F32: {
      result = IsNaNFlavor(mozilla::BitwiseCast<uint32_t>(val.f32()), flavor);
      break;
    }
    case wasm::ValType::F64: {
      result = IsNaNFlavor(mozilla::BitwiseCast<uint64_t>(val.f64()), flavor);
      break;
    }
    default:
      JS_ReportErrorASCII(cx, "global is not a floating point value");
      return false;
  }
  args.rval().setBoolean(result);
  return true;
}

static bool WasmGlobalToString(JSContext* cx, unsigned argc, Value* vp) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  if (!wasmHasSupport) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() < 1) {
    JS_ReportErrorASCII(cx, "not enough arguments");
    return false;
  }
  if (!args.get(0).isObject() ||
      !args.get(0).toObject().is<WasmGlobalObject>()) {
    JS_ReportErrorASCII(cx, "argument is not wasm value");
    return false;
  }
  Rooted<WasmGlobalObject*> global(
      cx, &args.get(0).toObject().as<WasmGlobalObject>());
  const wasm::Val& globalVal = global->val().get();

  UniqueChars result;
  switch (globalVal.type().kind()) {
    case wasm::ValType::I32: {
      result = JS_smprintf("i32:%" PRIx32, globalVal.i32());
      break;
    }
    case wasm::ValType::I64: {
      result = JS_smprintf("i64:%" PRIx64, globalVal.i64());
      break;
    }
    case wasm::ValType::F32: {
      result = JS_smprintf("f32:%f", globalVal.f32());
      break;
    }
    case wasm::ValType::F64: {
      result = JS_smprintf("f64:%lf", globalVal.f64());
      break;
    }
    case wasm::ValType::V128: {
      wasm::V128 v128 = globalVal.v128();
      result = JS_smprintf(
          "v128:%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x", v128.bytes[0],
          v128.bytes[1], v128.bytes[2], v128.bytes[3], v128.bytes[4],
          v128.bytes[5], v128.bytes[6], v128.bytes[7], v128.bytes[8],
          v128.bytes[9], v128.bytes[10], v128.bytes[11], v128.bytes[12],
          v128.bytes[13], v128.bytes[14], v128.bytes[15]);
      break;
    }
    case wasm::ValType::Ref: {
      result = JS_smprintf("ref:%" PRIxPTR, globalVal.ref().rawValue());
      break;
    }
    default:
      MOZ_ASSERT_UNREACHABLE();
  }

  args.rval().setString(JS_NewStringCopyZ(cx, result.get()));
  return true;
}

static bool WasmLosslessInvoke(JSContext* cx, unsigned argc, Value* vp) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  if (!wasmHasSupport) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() < 1) {
    JS_ReportErrorASCII(cx, "not enough arguments");
    return false;
  }
  if (!args.get(0).isObject() || !args.get(0).toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "argument is not an object");
    return false;
  }

  RootedFunction func(cx, &args[0].toObject().as<JSFunction>());
  if (!func || !wasm::IsWasmExportedFunction(func)) {
    JS_ReportErrorASCII(cx, "argument is not an exported wasm function");
    return false;
  }

  // Switch to the function's realm
  AutoRealm ar(cx, func);

  // Get the instance and funcIndex for calling the function
  wasm::Instance& instance = wasm::ExportedFunctionToInstance(func);
  uint32_t funcIndex = wasm::ExportedFunctionToFuncIndex(func);

  // Set up a modified call frame following the standard JS
  // [callee, this, arguments...] convention.
  RootedValueVector wasmCallFrame(cx);
  size_t len = 2 + args.length();
  if (!wasmCallFrame.resize(len)) {
    return false;
  }
  wasmCallFrame[0].set(ObjectValue(*func));
  wasmCallFrame[1].set(args.thisv());
  // Copy over the arguments needed to invoke the provided wasm function,
  // skipping the wasm function we're calling that is at `args.get(0)`.
  for (size_t i = 1; i < args.length(); i++) {
    size_t wasmArg = i - 1;
    wasmCallFrame[2 + wasmArg].set(args.get(i));
  }
  size_t wasmArgc = argc - 1;
  CallArgs wasmCallArgs(CallArgsFromVp(wasmArgc, wasmCallFrame.begin()));

  // Invoke the function with the new call frame
  bool result = instance.callExport(cx, funcIndex, wasmCallArgs,
                                    wasm::CoercionLevel::Lossless);
  // Assign the wasm rval to our rval
  args.rval().set(wasmCallArgs.rval());
  return result;
}

static bool ConvertToTier(JSContext* cx, HandleValue value,
                          const wasm::Code& code, wasm::Tier* tier) {
  RootedString option(cx, JS::ToString(cx, value));

  if (!option) {
    return false;
  }

  bool stableTier = false;
  bool bestTier = false;
  bool baselineTier = false;
  bool ionTier = false;

  if (!JS_StringEqualsLiteral(cx, option, "stable", &stableTier) ||
      !JS_StringEqualsLiteral(cx, option, "best", &bestTier) ||
      !JS_StringEqualsLiteral(cx, option, "baseline", &baselineTier) ||
      !JS_StringEqualsLiteral(cx, option, "ion", &ionTier)) {
    return false;
  }

  if (stableTier) {
    *tier = code.stableTier();
  } else if (bestTier) {
    *tier = code.bestTier();
  } else if (baselineTier) {
    *tier = wasm::Tier::Baseline;
  } else if (ionTier) {
    *tier = wasm::Tier::Optimized;
  } else {
    // You can omit the argument but you can't pass just anything you like
    return false;
  }

  return true;
}

static bool WasmExtractCode(JSContext* cx, unsigned argc, Value* vp) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  if (!wasmHasSupport) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }

  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.get(0).isObject()) {
    JS_ReportErrorASCII(cx, "argument is not an object");
    return false;
  }

  Rooted<WasmModuleObject*> module(
      cx, args[0].toObject().maybeUnwrapIf<WasmModuleObject>());
  if (!module) {
    JS_ReportErrorASCII(cx, "argument is not a WebAssembly.Module");
    return false;
  }

  wasm::Tier tier = module->module().code().stableTier();
  ;
  if (args.length() > 1 &&
      !ConvertToTier(cx, args[1], module->module().code(), &tier)) {
    args.rval().setNull();
    return false;
  }

  RootedValue result(cx);
  if (!module->module().extractCode(cx, tier, &result)) {
    return false;
  }

  args.rval().set(result);
  return true;
}

struct DisasmBuffer {
  JSStringBuilder builder;
  bool oom;
  explicit DisasmBuffer(JSContext* cx) : builder(cx), oom(false) {}
};

static bool HasDisassembler(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(jit::HasDisassembler());
  return true;
}

MOZ_THREAD_LOCAL(DisasmBuffer*) disasmBuf;

static void captureDisasmText(const char* text) {
  DisasmBuffer* buf = disasmBuf.get();
  if (!buf->builder.append(text, strlen(text)) || !buf->builder.append('\n')) {
    buf->oom = true;
  }
}

static bool DisassembleNative(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setUndefined();

  if (args.length() < 1) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_MORE_ARGS_NEEDED, "disnative", "1", "",
                              "0");
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "The first argument must be a function.");
    return false;
  }

  JSSprinter sprinter(cx);
  if (!sprinter.init()) {
    return false;
  }

  RootedFunction fun(cx, &args[0].toObject().as<JSFunction>());

  uint8_t* jit_begin = nullptr;
  uint8_t* jit_end = nullptr;

  if (fun->isAsmJSNative() || fun->isWasmWithJitEntry()) {
    if (fun->isAsmJSNative()) {
      return false;
    }
    sprinter.printf("; backend=asmjs\n");
    sprinter.printf("; backend=wasm\n");

    js::wasm::Instance& inst = fun->wasmInstance();
    const js::wasm::Code& code = inst.code();
    js::wasm::Tier tier = code.bestTier();

    const js::wasm::MetadataTier& meta = inst.metadata(tier);

    const js::wasm::CodeSegment& segment = code.segment(tier);
    const uint32_t funcIndex = code.getFuncIndex(&*fun);
    const js::wasm::FuncExport& func = meta.lookupFuncExport(funcIndex);
    const js::wasm::CodeRange& codeRange = meta.codeRange(func);

    jit_begin = segment.base() + codeRange.begin();
    jit_end = segment.base() + codeRange.end();
  } else if (fun->hasJitScript()) {
    JSScript* script = fun->nonLazyScript();
    if (script == nullptr) {
      return false;
    }

    js::jit::IonScript* ion =
        script->hasIonScript() ? script->ionScript() : nullptr;
    js::jit::BaselineScript* baseline =
        script->hasBaselineScript() ? script->baselineScript() : nullptr;
    if (ion && ion->method()) {
      sprinter.printf("; backend=ion\n");
      jit_begin = ion->method()->raw();
      jit_end = ion->method()->rawEnd();
    } else if (baseline) {
      sprinter.printf("; backend=baseline\n");
      jit_begin = baseline->method()->raw();
      jit_end = baseline->method()->rawEnd();
    }
  } else {
    JS_ReportErrorASCII(cx,
                        "The function hasn't been warmed up, hence no JIT code "
                        "to disassemble.");
    return false;
  }

  if (jit_begin == nullptr || jit_end == nullptr) {
    return false;
  }

#ifdef JS_CODEGEN_ARM
  // The ARM32 disassembler is currently not fuzzing-safe because it doesn't
  // handle constant pools correctly (bug 1875363).
  if (fuzzingSafe) {
    JS_ReportErrorASCII(cx, "disnative is not fuzzing-safe on ARM32");
    return false;
  }
#endif

  // Dump the raw code to a file before disassembling in case
  // finishString triggers a GC and discards the jitcode.
  if (!fuzzingSafe && args.length() > 1 && args[1].isString()) {
    RootedString str(cx, args[1].toString());
    JS::UniqueChars fileNameBytes = JS_EncodeStringToUTF8(cx, str);

    const char* fileName = fileNameBytes.get();
    if (!fileName) {
      ReportOutOfMemory(cx);
      return false;
    }

    FILE* f = fopen(fileName, "w");
    if (!f) {
      JS_ReportErrorASCII(cx, "Could not open file for writing.");
      return false;
    }

    uintptr_t expected_length = reinterpret_cast<uintptr_t>(jit_end) -
                                reinterpret_cast<uintptr_t>(jit_begin);
    if (expected_length != fwrite(jit_begin, jit_end - jit_begin, 1, f)) {
      JS_ReportErrorASCII(cx, "Did not write all function bytes to the file.");
      fclose(f);
      return false;
    }
    fclose(f);
  }

  DisasmBuffer buf(cx);
  disasmBuf.set(&buf);
  auto onFinish = mozilla::MakeScopeExit([&] { disasmBuf.set(nullptr); });

  jit::Disassemble(jit_begin, jit_end - jit_begin, &captureDisasmText);

  if (buf.oom) {
    ReportOutOfMemory(cx);
    return false;
  }
  JSString* sresult = buf.builder.finishString();
  if (!sresult) {
    ReportOutOfMemory(cx);
    return false;
  }
  sprinter.putString(cx, sresult);

  JSString* str = sprinter.release(cx);
  if (!str) {
    return false;
  }

  args[0].setUndefined();
  args.rval().setString(str);

  return true;
}

static bool ComputeTier(JSContext* cx, const wasm::Code& code,
                        HandleValue tierSelection, wasm::Tier* tier) {
  *tier = code.stableTier();
  if (!tierSelection.isUndefined() &&
      !ConvertToTier(cx, tierSelection, code, tier)) {
    JS_ReportErrorASCII(cx, "invalid tier");
    return false;
  }

  if (!code.hasTier(*tier)) {
    JS_ReportErrorASCII(cx, "function missing selected tier");
    return false;
  }

  return true;
}

template <typename DisasmFunction>
static bool DisassembleIt(JSContext* cx, bool asString, MutableHandleValue rval,
                          DisasmFunction&& disassembleIt) {
  if (asString) {
    DisasmBuffer buf(cx);
    disasmBuf.set(&buf);
    auto onFinish = mozilla::MakeScopeExit([&] { disasmBuf.set(nullptr); });
    disassembleIt(captureDisasmText);
    if (buf.oom) {
      ReportOutOfMemory(cx);
      return false;
    }
    JSString* sresult = buf.builder.finishString();
    if (!sresult) {
      ReportOutOfMemory(cx);
      return false;
    }
    rval.setString(sresult);
    return true;
  }

  disassembleIt([](const char* text) { fprintf(stderr, "%s\n", text); });
  return true;
}

static bool WasmDisassembleFunction(JSContext* cx, const HandleFunction& func,
                                    HandleValue tierSelection, bool asString,
                                    MutableHandleValue rval) {
  wasm::Instance& instance = wasm::ExportedFunctionToInstance(func);
  wasm::Tier tier;

  if (!ComputeTier(cx, instance.code(), tierSelection, &tier)) {
    return false;
  }

  uint32_t funcIndex = wasm::ExportedFunctionToFuncIndex(func);
  return DisassembleIt(
      cx, asString, rval, [&](void (*captureText)(const char*)) {
        instance.disassembleExport(cx, funcIndex, tier, captureText);
      });
}

static bool WasmDisassembleCode(JSContext* cx, const wasm::Code& code,
                                HandleValue tierSelection, int kindSelection,
                                bool asString, MutableHandleValue rval) {
  wasm::Tier tier;
  if (!ComputeTier(cx, code, tierSelection, &tier)) {
    return false;
  }

  return DisassembleIt(cx, asString, rval,
                       [&](void (*captureText)(const char*)) {
                         code.disassemble(cx, tier, kindSelection, captureText);
                       });
}

static bool WasmDisassemble(JSContext* cx, unsigned argc, Value* vp) {
  bool wasmHasSupport = WASM_HAS_SUPPORT(cx);
  if (!wasmHasSupport) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }

  CallArgs args = CallArgsFromVp(argc, vp);

  args.rval().set(UndefinedValue());

  if (!args.get(0).isObject()) {
    JS_ReportErrorASCII(cx, "argument is not an object");
    return false;
  }

  bool asString = false;
  RootedValue tierSelection(cx);
  int kindSelection = (1 << wasm::CodeRange::Function);
  if (args.length() > 1 && args[1].isObject()) {
    RootedObject options(cx, &args[1].toObject());
    RootedValue val(cx);

    if (!JS_GetProperty(cx, options, "asString", &val)) {
      return false;
    }
    asString = val.isBoolean() && val.toBoolean();

    if (!JS_GetProperty(cx, options, "tier", &tierSelection)) {
      return false;
    }

    if (!JS_GetProperty(cx, options, "kinds", &val)) {
      return false;
    }
    if (val.isString() && val.toString()->hasLatin1Chars()) {
      AutoStableStringChars stable(cx);
      if (!stable.init(cx, val.toString())) {
        return false;
      }
      const char* p = (const char*)(stable.latin1Chars());
      const char* end = p + val.toString()->length();
      int selection = 0;
      for (;;) {
        if (strncmp(p, "Function", 8) == 0) {
          selection |= (1 << wasm::CodeRange::Function);
          p += 8;
        } else if (strncmp(p, "InterpEntry", 11) == 0) {
          selection |= (1 << wasm::CodeRange::InterpEntry);
          p += 11;
        } else if (strncmp(p, "JitEntry", 8) == 0) {
          selection |= (1 << wasm::CodeRange::JitEntry);
          p += 8;
        } else if (strncmp(p, "ImportInterpExit", 16) == 0) {
          selection |= (1 << wasm::CodeRange::ImportInterpExit);
          p += 16;
        } else if (strncmp(p, "ImportJitExit", 13) == 0) {
          selection |= (1 << wasm::CodeRange::ImportJitExit);
          p += 13;
        } else if (strncmp(p, "all", 3) == 0) {
          selection = ~0;
          p += 3;
        } else {
          break;
        }
        if (p == end || *p != ',') {
          break;
        }
        p++;
      }
      if (p == end) {
        kindSelection = selection;
      } else {
        JS_ReportErrorASCII(cx, "argument object has invalid `kinds`");
        return false;
      }
    }
  }

  RootedFunction func(cx, args[0].toObject().maybeUnwrapIf<JSFunction>());
  if (func && wasm::IsWasmExportedFunction(func)) {
    return WasmDisassembleFunction(cx, func, tierSelection, asString,
                                   args.rval());
  }
  if (args[0].toObject().is<WasmModuleObject>()) {
    return WasmDisassembleCode(
        cx, args[0].toObject().as<WasmModuleObject>().module().code(),
        tierSelection, kindSelection, asString, args.rval());
  }
  if (args[0].toObject().is<WasmInstanceObject>()) {
    return WasmDisassembleCode(
        cx, args[0].toObject().as<WasmInstanceObject>().instance().code(),
        tierSelection, kindSelection, asString, args.rval());
  }
  JS_ReportErrorASCII(
      cx, "argument is not an exported wasm function or a wasm module");
  return false;
}

static bool ToIonDumpContents(JSContext* cx, HandleValue value,
                              wasm::IonDumpContents* contents) {
  RootedString option(cx, JS::ToString(cx, value));

  if (!option) {
    return false;
  }

  bool isEqual = false;
  if (!JS_StringEqualsLiteral(cx, option, "mir", &isEqual) || isEqual) {
    *contents = wasm::IonDumpContents::UnoptimizedMIR;
    return isEqual;
  } else if (!JS_StringEqualsLiteral(cx, option, "unopt-mir", &isEqual) ||
             isEqual) {
    *contents = wasm::IonDumpContents::UnoptimizedMIR;
    return isEqual;
  } else if (!JS_StringEqualsLiteral(cx, option, "opt-mir", &isEqual) ||
             isEqual) {
    *contents = wasm::IonDumpContents::OptimizedMIR;
    return isEqual;
  } else if (!JS_StringEqualsLiteral(cx, option, "lir", &isEqual) || isEqual) {
    *contents = wasm::IonDumpContents::LIR;
    return isEqual;
  } else {
    return false;
  }
}

static bool WasmDumpIon(JSContext* cx, unsigned argc, Value* vp) {
  if (!wasm::HasSupport(cx)) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }

  CallArgs args = CallArgsFromVp(argc, vp);

  args.rval().set(UndefinedValue());

  SharedMem<uint8_t*> dataPointer;
  size_t byteLength;
  if (!args.get(0).isObject() || !IsBufferSource(args.get(0).toObjectOrNull(),
                                                 &dataPointer, &byteLength)) {
    JS_ReportErrorASCII(cx, "argument is not a buffer source");
    return false;
  }

  uint32_t targetFuncIndex;
  if (!ToUint32(cx, args.get(1), &targetFuncIndex)) {
    JS_ReportErrorASCII(cx, "argument is not a func index");
    return false;
  }

  wasm::IonDumpContents contents = wasm::IonDumpContents::Default;
  if (args.length() > 2 && !ToIonDumpContents(cx, args.get(2), &contents)) {
    JS_ReportErrorASCII(cx, "argument is not a valid dump contents");
    return false;
  }

  wasm::MutableBytes bytecode = cx->new_<wasm::ShareableBytes>();
  if (!bytecode) {
    return false;
  }
  if (!bytecode->append(dataPointer.unwrap(), byteLength)) {
    ReportOutOfMemory(cx);
    return false;
  }

  UniqueChars error;
  JSSprinter out(cx);
  if (!out.init()) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!wasm::DumpIonFunctionInModule(*bytecode, targetFuncIndex, contents, out,
                                     &error)) {
    if (error) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_COMPILE_ERROR, error.get());
      return false;
    }
    ReportOutOfMemory(cx);
    return false;
  }

  JSString* str = out.release(cx);
  if (!str) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().set(StringValue(str));
  return true;
}

enum class Flag { Tier2Complete, Deserialized, ParsedBranchHints };

static bool WasmReturnFlag(JSContext* cx, unsigned argc, Value* vp, Flag flag) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.get(0).isObject()) {
    JS_ReportErrorASCII(cx, "argument is not an object");
    return false;
  }

  Rooted<WasmModuleObject*> module(
      cx, args[0].toObject().maybeUnwrapIf<WasmModuleObject>());
  if (!module) {
    JS_ReportErrorASCII(cx, "argument is not a WebAssembly.Module");
    return false;
  }

  bool b;
  switch (flag) {
    case Flag::Tier2Complete:
      b = !module->module().testingTier2Active();
      break;
    case Flag::Deserialized:
      b = module->module().loggingDeserialized();
      break;
    case Flag::ParsedBranchHints:
      b = module->module().metadata().parsedBranchHints;
      break;
  }

  args.rval().set(BooleanValue(b));
  return true;
}

static bool wasmMetadataAnalysis(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.get(0).isObject()) {
    JS_ReportErrorASCII(cx, "argument is not an object");
    return false;
  }

  if (args[0].toObject().is<WasmModuleObject>()) {
    HashMap<const char*, uint32_t, mozilla::CStringHasher, SystemAllocPolicy>
        hashmap = args[0]
                      .toObject()
                      .as<WasmModuleObject>()
                      .module()
                      .code()
                      .metadataAnalysis(cx);
    if (hashmap.empty()) {
      JS_ReportErrorASCII(cx, "Metadata analysis has failed");
      return false;
    }

    // metadataAnalysis returned a map of {key, value} with various statistics
    // convert it into a dictionary to be used by JS
    Rooted<IdValueVector> props(cx, IdValueVector(cx));

    for (auto iter = hashmap.iter(); !iter.done(); iter.next()) {
      const auto* key = iter.get().key();
      auto value = iter.get().value();

      JSString* string = JS_NewStringCopyZ(cx, key);
      if (!string) {
        return false;
      }

      if (!props.append(
              IdValuePair(NameToId(string->asLinear().toPropertyName(cx)),
                          NumberValue(value)))) {
        return false;
      }
    }

    JSObject* results = NewPlainObjectWithUniqueNames(cx, props);
    if (!results) {
      return false;
    }
    args.rval().setObject(*results);

    return true;
  }

  JS_ReportErrorASCII(
      cx, "argument is not an exported wasm function or a wasm module");

  return false;
}

static bool WasmHasTier2CompilationCompleted(JSContext* cx, unsigned argc,
                                             Value* vp) {
  return WasmReturnFlag(cx, argc, vp, Flag::Tier2Complete);
}

static bool WasmLoadedFromCache(JSContext* cx, unsigned argc, Value* vp) {
  return WasmReturnFlag(cx, argc, vp, Flag::Deserialized);
}

#ifdef ENABLE_WASM_BRANCH_HINTING
static bool WasmParsedBranchHints(JSContext* cx, unsigned argc, Value* vp) {
  return WasmReturnFlag(cx, argc, vp, Flag::ParsedBranchHints);
}
#endif  // ENABLE_WASM_BRANCH_HINTING

static bool WasmBuiltinI8VecMul(JSContext* cx, unsigned argc, Value* vp) {
  if (!wasm::HasSupport(cx)) {
    JS_ReportErrorASCII(cx, "wasm support unavailable");
    return false;
  }

  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<WasmModuleObject*> module(cx);
  if (!wasm::CompileBuiltinModule(cx, wasm::BuiltinModuleId::SelfTest,
                                  &module)) {
    return false;
  }
  args.rval().set(ObjectValue(*module.get()));
  return true;
}

#ifdef ENABLE_WASM_GC
static bool WasmGcReadField(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (!args.requireAtLeast(cx, "wasmGcReadField", 2)) {
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<WasmGcObject>()) {
    ReportUsageErrorASCII(cx, callee,
                          "First argument must be a WebAssembly GC object");
    return false;
  }

  int32_t fieldIndex;
  if (!JS::ToInt32(cx, args[1], &fieldIndex) || fieldIndex < 0) {
    ReportUsageErrorASCII(cx, callee,
                          "Second argument must be a non-negative integer");
    return false;
  }

  Rooted<WasmGcObject*> gcObject(cx, &args[0].toObject().as<WasmGcObject>());
  Rooted<Value> gcValue(cx);
  if (!WasmGcObject::loadValue(cx, gcObject, jsid::Int(int32_t(fieldIndex)),
                               &gcValue)) {
    return false;
  }

  args.rval().set(gcValue);
  return true;
}

static bool WasmGcArrayLength(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (!args.requireAtLeast(cx, "wasmGcArrayLength", 1)) {
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<WasmArrayObject>()) {
    ReportUsageErrorASCII(cx, callee,
                          "First argument must be a WebAssembly GC array");
    return false;
  }

  WasmArrayObject& arr = args[0].toObject().as<WasmArrayObject>();
  args.rval().setInt32(int32_t(arr.numElements_));
  return true;
}
#endif  // ENABLE_WASM_GC

static bool LargeArrayBufferSupported(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(ArrayBufferObject::ByteLengthLimit >
                         ArrayBufferObject::ByteLengthLimitForSmallBuffer);
  return true;
}

static bool IsLazyFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1) {
    JS_ReportErrorASCII(cx, "The function takes exactly one argument.");
    return false;
  }
  if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "The first argument should be a function.");
    return false;
  }
  JSFunction* fun = &args[0].toObject().as<JSFunction>();
  args.rval().setBoolean(fun->isInterpreted() && !fun->hasBytecode());
  return true;
}

static bool IsRelazifiableFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1) {
    JS_ReportErrorASCII(cx, "The function takes exactly one argument.");
    return false;
  }
  if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "The first argument should be a function.");
    return false;
  }

  JSFunction* fun = &args[0].toObject().as<JSFunction>();
  args.rval().setBoolean(fun->hasBytecode() &&
                         fun->nonLazyScript()->allowRelazify());
  return true;
}

static bool IsInStencilCache(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1) {
    JS_ReportErrorASCII(cx, "The function takes exactly one argument.");
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "The first argument should be a function.");
    return false;
  }

  if (fuzzingSafe) {
    // When running code concurrently to fill-up the stencil cache, the content
    // is not garanteed to be present.
    args.rval().setBoolean(false);
    return true;
  }

  JSFunction* fun = &args[0].toObject().as<JSFunction>();
  BaseScript* script = fun->baseScript();
  RefPtr<ScriptSource> ss = script->scriptSource();
  DelazificationCache& cache = DelazificationCache::getSingleton();
  auto guard = cache.isSourceCached(ss);
  if (!guard) {
    args.rval().setBoolean(false);
    return true;
  }

  StencilContext key(ss, script->extent());
  frontend::CompilationStencil* stencil = cache.lookup(guard, key);
  args.rval().setBoolean(bool(stencil));
  return true;
}

static bool WaitForStencilCache(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1) {
    JS_ReportErrorASCII(cx, "The function takes exactly one argument.");
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "The first argument should be a function.");
    return false;
  }
  args.rval().setUndefined();

  JSFunction* fun = &args[0].toObject().as<JSFunction>();
  BaseScript* script = fun->baseScript();
  RefPtr<ScriptSource> ss = script->scriptSource();
  DelazificationCache& cache = DelazificationCache::getSingleton();
  StencilContext key(ss, script->extent());

  AutoLockHelperThreadState lock;
  if (!HelperThreadState().isInitialized(lock)) {
    return true;
  }

  while (true) {
    {
      // This capture a Mutex that we have to release before using the wait
      // function.
      auto guard = cache.isSourceCached(ss);
      if (!guard) {
        return true;
      }

      frontend::CompilationStencil* stencil = cache.lookup(guard, key);
      if (stencil) {
        break;
      }
    }

    HelperThreadState().wait(lock);
  }
  return true;
}

static bool HasSameBytecodeData(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 2) {
    JS_ReportErrorASCII(cx, "The function takes exactly two argument.");
    return false;
  }

  auto GetSharedData = [](JSContext* cx,
                          HandleValue v) -> SharedImmutableScriptData* {
    if (!v.isObject()) {
      JS_ReportErrorASCII(cx, "The arguments must be interpreted functions.");
      return nullptr;
    }

    RootedObject obj(cx, CheckedUnwrapDynamic(&v.toObject(), cx));
    if (!obj) {
      return nullptr;
    }

    if (!obj->is<JSFunction>() || !obj->as<JSFunction>().isInterpreted()) {
      JS_ReportErrorASCII(cx, "The arguments must be interpreted functions.");
      return nullptr;
    }

    AutoRealm ar(cx, obj);
    RootedFunction fun(cx, &obj->as<JSFunction>());
    RootedScript script(cx, JSFunction::getOrCreateScript(cx, fun));
    if (!script) {
      return nullptr;
    }

    MOZ_ASSERT(script->sharedData());
    return script->sharedData();
  };

  // NOTE: We use RefPtr below to keep the data alive across possible GC since
  //       the functions may be in different Zones.

  RefPtr<SharedImmutableScriptData> sharedData1 = GetSharedData(cx, args[0]);
  if (!sharedData1) {
    return false;
  }

  RefPtr<SharedImmutableScriptData> sharedData2 = GetSharedData(cx, args[1]);
  if (!sharedData2) {
    return false;
  }

  args.rval().setBoolean(sharedData1 == sharedData2);
  return true;
}

static bool InternalConst(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() == 0) {
    JS_ReportErrorASCII(cx, "the function takes exactly one argument");
    return false;
  }

  JSString* str = ToString(cx, args[0]);
  if (!str) {
    return false;
  }
  JSLinearString* linear = JS_EnsureLinearString(cx, str);
  if (!linear) {
    return false;
  }

  if (JS_LinearStringEqualsLiteral(linear, "MARK_STACK_BASE_CAPACITY")) {
    args.rval().setNumber(uint32_t(js::MARK_STACK_BASE_CAPACITY));
  } else {
    JS_ReportErrorASCII(cx, "unknown const name");
    return false;
  }
  return true;
}

static bool GCPreserveCode(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 0) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  cx->runtime()->gc.setAlwaysPreserveCode();

  args.rval().setUndefined();
  return true;
}

#ifdef JS_GC_ZEAL

static bool ParseGCZealMode(JSContext* cx, const CallArgs& args,
                            uint8_t* zeal) {
  uint32_t value;
  if (!ToUint32(cx, args.get(0), &value)) {
    return false;
  }

  if (value > uint32_t(gc::ZealMode::Limit)) {
    JS_ReportErrorASCII(cx, "gczeal argument out of range");
    return false;
  }

  *zeal = static_cast<uint8_t>(value);
  return true;
}

static bool GCZeal(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 2) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }

  if (args.length() == 0) {
    uint32_t zealBits, unused1, unused2;
    cx->runtime()->gc.getZealBits(&zealBits, &unused1, &unused2);
    args.rval().setNumber(zealBits);
    return true;
  }

  uint8_t zeal;
  if (!ParseGCZealMode(cx, args, &zeal)) {
    return false;
  }

  uint32_t frequency = JS::ShellDefaultGCZealFrequency;
  if (args.length() >= 2) {
    if (!ToUint32(cx, args.get(1), &frequency)) {
      return false;
    }
  }

  JS::SetGCZeal(cx, zeal, frequency);
  args.rval().setUndefined();
  return true;
}

static bool UnsetGCZeal(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }

  uint8_t zeal;
  if (!ParseGCZealMode(cx, args, &zeal)) {
    return false;
  }

  JS::UnsetGCZeal(cx, zeal);
  args.rval().setUndefined();
  return true;
}

static bool ScheduleGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }

  if (args.length() == 0) {
    /* Fetch next zeal trigger only. */
  } else if (args[0].isNumber()) {
    /* Schedule a GC to happen after |arg| allocations. */
    JS::ScheduleGC(cx, std::max(int(args[0].toNumber()), 0));
  } else {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Bad argument - expecting number");
    return false;
  }

  uint32_t zealBits;
  uint32_t freq;
  uint32_t next;
  JS::GetGCZealBits(cx, &zealBits, &freq, &next);
  args.rval().setInt32(next);
  return true;
}

static bool SelectForGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  /*
   * The selectedForMarking set is intended to be manually marked at slice
   * start to detect missing pre-barriers. It is invalid for nursery things
   * to be in the set, so evict the nursery before adding items.
   */
  cx->runtime()->gc.evictNursery();

  for (unsigned i = 0; i < args.length(); i++) {
    if (args[i].isObject()) {
      if (!cx->runtime()->gc.selectForMarking(&args[i].toObject())) {
        return false;
      }
    }
  }

  args.rval().setUndefined();
  return true;
}

static bool VerifyPreBarriers(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 0) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }

  gc::VerifyBarriers(cx->runtime(), gc::PreBarrierVerifier);
  args.rval().setUndefined();
  return true;
}

static bool VerifyPostBarriers(JSContext* cx, unsigned argc, Value* vp) {
  // This is a no-op since the post barrier verifier was removed.
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }
  args.rval().setUndefined();
  return true;
}

static bool CurrentGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 0) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }

  RootedObject result(cx, JS_NewPlainObject(cx));
  if (!result) {
    return false;
  }

  js::gc::GCRuntime& gc = cx->runtime()->gc;
  const char* state = StateName(gc.state());

  RootedString str(cx, JS_NewStringCopyZ(cx, state));
  if (!str) {
    return false;
  }
  RootedValue val(cx, StringValue(str));
  if (!JS_DefineProperty(cx, result, "incrementalState", val,
                         JSPROP_ENUMERATE)) {
    return false;
  }

  if (gc.state() == js::gc::State::Sweep) {
    val = Int32Value(gc.getCurrentSweepGroupIndex());
    if (!JS_DefineProperty(cx, result, "sweepGroup", val, JSPROP_ENUMERATE)) {
      return false;
    }
  }

  val = BooleanValue(gc.isIncrementalGCInProgress() && gc.isShrinkingGC());
  if (!JS_DefineProperty(cx, result, "isShrinking", val, JSPROP_ENUMERATE)) {
    return false;
  }

  val = Int32Value(gc.gcNumber());
  if (!JS_DefineProperty(cx, result, "number", val, JSPROP_ENUMERATE)) {
    return false;
  }

  val = Int32Value(gc.minorGCCount());
  if (!JS_DefineProperty(cx, result, "minorCount", val, JSPROP_ENUMERATE)) {
    return false;
  }

  val = Int32Value(gc.majorGCCount());
  if (!JS_DefineProperty(cx, result, "majorCount", val, JSPROP_ENUMERATE)) {
    return false;
  }

  val = BooleanValue(gc.isFullGc());
  if (!JS_DefineProperty(cx, result, "isFull", val, JSPROP_ENUMERATE)) {
    return false;
  }

  val = BooleanValue(gc.isCompactingGc());
  if (!JS_DefineProperty(cx, result, "isCompacting", val, JSPROP_ENUMERATE)) {
    return false;
  }

#  ifdef DEBUG
  val = Int32Value(gc.testMarkQueuePos());
  if (!JS_DefineProperty(cx, result, "queuePos", val, JSPROP_ENUMERATE)) {
    return false;
  }
#  endif

  args.rval().setObject(*result);
  return true;
}

static bool DeterministicGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  cx->runtime()->gc.setDeterministic(ToBoolean(args[0]));
  args.rval().setUndefined();
  return true;
}

static bool DumpGCArenaInfo(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  js::gc::DumpArenaInfo();
  args.rval().setUndefined();
  return true;
}

static bool SetMarkStackLimit(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  int32_t value;
  if (!ToInt32(cx, args[0], &value) || value <= 0) {
    JS_ReportErrorASCII(cx, "Bad argument to SetMarkStackLimit");
    return false;
  }

  if (JS::IsIncrementalGCInProgress(cx)) {
    JS_ReportErrorASCII(
        cx, "Attempt to set markStackLimit while a GC is in progress");
    return false;
  }

  JSRuntime* runtime = cx->runtime();
  AutoLockGC lock(runtime);
  runtime->gc.setMarkStackLimit(value, lock);
  args.rval().setUndefined();
  return true;
}

#endif /* JS_GC_ZEAL */

static bool GCState(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }

  const char* state;

  if (args.length() == 1) {
    if (!args[0].isObject()) {
      RootedObject callee(cx, &args.callee());
      ReportUsageErrorASCII(cx, callee, "Expected object");
      return false;
    }

    JSObject* obj = UncheckedUnwrap(&args[0].toObject());
    state = gc::StateName(obj->zone()->gcState());
  } else {
    state = gc::StateName(cx->runtime()->gc.state());
  }

  return ReturnStringCopy(cx, args, state);
}

static bool ScheduleZoneForGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Expecting a single argument");
    return false;
  }

  if (args[0].isObject()) {
    // Ensure that |zone| is collected during the next GC.
    Zone* zone = UncheckedUnwrap(&args[0].toObject())->zone();
    PrepareZoneForGC(cx, zone);
  } else if (args[0].isString()) {
    // This allows us to schedule the atoms zone for GC.
    Zone* zone = args[0].toString()->zoneFromAnyThread();
    if (!CurrentThreadCanAccessZone(zone)) {
      RootedObject callee(cx, &args.callee());
      ReportUsageErrorASCII(cx, callee, "Specified zone not accessible for GC");
      return false;
    }
    PrepareZoneForGC(cx, zone);
  } else {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee,
                          "Bad argument - expecting object or string");
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool StartGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 2) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  auto budget = SliceBudget::unlimited();
  if (args.length() >= 1) {
    uint32_t work = 0;
    if (!ToUint32(cx, args[0], &work)) {
      return false;
    }
    budget = SliceBudget(WorkBudget(work));
  }

  bool shrinking = false;
  if (args.length() >= 2) {
    Value arg = args[1];
    if (arg.isString()) {
      if (!JS_StringEqualsLiteral(cx, arg.toString(), "shrinking",
                                  &shrinking)) {
        return false;
      }
    }
  }

  JSRuntime* rt = cx->runtime();
  if (rt->gc.isIncrementalGCInProgress()) {
    RootedObject callee(cx, &args.callee());
    JS_ReportErrorASCII(cx, "Incremental GC already in progress");
    return false;
  }

  JS::GCOptions options =
      shrinking ? JS::GCOptions::Shrink : JS::GCOptions::Normal;
  rt->gc.startDebugGC(options, budget);

  args.rval().setUndefined();
  return true;
}

static bool FinishGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 0) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  JSRuntime* rt = cx->runtime();
  if (rt->gc.isIncrementalGCInProgress()) {
    rt->gc.finishGC(JS::GCReason::DEBUG_GC);
  }

  args.rval().setUndefined();
  return true;
}

static bool GCSlice(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 2) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  auto budget = SliceBudget::unlimited();
  if (args.length() >= 1) {
    uint32_t work = 0;
    if (!ToUint32(cx, args[0], &work)) {
      return false;
    }
    budget = SliceBudget(WorkBudget(work));
  }

  bool dontStart = false;
  if (args.get(1).isObject()) {
    RootedObject options(cx, &args[1].toObject());
    RootedValue v(cx);
    if (!JS_GetProperty(cx, options, "dontStart", &v)) {
      return false;
    }
    dontStart = ToBoolean(v);
  }

  JSRuntime* rt = cx->runtime();
  if (rt->gc.isIncrementalGCInProgress()) {
    rt->gc.debugGCSlice(budget);
  } else if (!dontStart) {
    rt->gc.startDebugGC(JS::GCOptions::Normal, budget);
  }

  args.rval().setUndefined();
  return true;
}

static bool AbortGC(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 0) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  JS::AbortIncrementalGC(cx);
  args.rval().setUndefined();
  return true;
}

static bool FullCompartmentChecks(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  cx->runtime()->gc.setFullCompartmentChecks(ToBoolean(args[0]));
  args.rval().setUndefined();
  return true;
}

static bool NondeterministicGetWeakMapKeys(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }
  if (!args[0].isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE,
                              "nondeterministicGetWeakMapKeys", "WeakMap",
                              InformalValueTypeName(args[0]));
    return false;
  }
  RootedObject arr(cx);
  RootedObject mapObj(cx, &args[0].toObject());
  if (!JS_NondeterministicGetWeakMapKeys(cx, mapObj, &arr)) {
    return false;
  }
  if (!arr) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE,
                              "nondeterministicGetWeakMapKeys", "WeakMap",
                              args[0].toObject().getClass()->name);
    return false;
  }
  args.rval().setObject(*arr);
  return true;
}

class HasChildTracer final : public JS::CallbackTracer {
  RootedValue child_;
  bool found_;

  void onChild(JS::GCCellPtr thing, const char* name) override {
    if (thing.asCell() == child_.toGCThing()) {
      found_ = true;
    }
  }

 public:
  HasChildTracer(JSContext* cx, HandleValue child)
      : JS::CallbackTracer(cx, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::TraceKeysAndValues),
        child_(cx, child),
        found_(false) {}

  bool found() const { return found_; }
};

static bool HasChild(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedValue parent(cx, args.get(0));
  RootedValue child(cx, args.get(1));

  if (!parent.isGCThing() || !child.isGCThing()) {
    args.rval().setBoolean(false);
    return true;
  }

  HasChildTracer trc(cx, child);
  TraceChildren(&trc, JS::GCCellPtr(parent.toGCThing(), parent.traceKind()));
  args.rval().setBoolean(trc.found());
  return true;
}

static bool SetSavedStacksRNGState(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "setSavedStacksRNGState", 1)) {
    return false;
  }

  int32_t seed;
  if (!ToInt32(cx, args[0], &seed)) {
    return false;
  }

  // Either one or the other of the seed arguments must be non-zero;
  // make this true no matter what value 'seed' has.
  cx->realm()->savedStacks().setRNGState(seed, (seed + 1) * 33);
  return true;
}

static bool GetSavedFrameCount(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setNumber(cx->realm()->savedStacks().count());
  return true;
}

static bool ClearSavedFrames(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  js::SavedStacks& savedStacks = cx->realm()->savedStacks();
  savedStacks.clear();

  for (ActivationIterator iter(cx); !iter.done(); ++iter) {
    iter->clearLiveSavedFrameCache();
  }

  args.rval().setUndefined();
  return true;
}

static bool SaveStack(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JS::StackCapture capture((JS::AllFrames()));
  if (args.length() >= 1) {
    double maxDouble;
    if (!ToNumber(cx, args[0], &maxDouble)) {
      return false;
    }
    if (std::isnan(maxDouble) || maxDouble < 0 || maxDouble > UINT32_MAX) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[0],
                       nullptr, "not a valid maximum frame count");
      return false;
    }
    uint32_t max = uint32_t(maxDouble);
    if (max > 0) {
      capture = JS::StackCapture(JS::MaxFrames(max));
    }
  }

  RootedObject compartmentObject(cx);
  if (args.length() >= 2) {
    if (!args[1].isObject()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[0],
                       nullptr, "not an object");
      return false;
    }
    compartmentObject = UncheckedUnwrap(&args[1].toObject());
    if (!compartmentObject) {
      return false;
    }
  }

  RootedObject stack(cx);
  {
    Maybe<AutoRealm> ar;
    if (compartmentObject) {
      ar.emplace(cx, compartmentObject);
    }
    if (!JS::CaptureCurrentStack(cx, &stack, std::move(capture))) {
      return false;
    }
  }

  if (stack && !cx->compartment()->wrap(cx, &stack)) {
    return false;
  }

  args.rval().setObjectOrNull(stack);
  return true;
}

static bool CaptureFirstSubsumedFrame(JSContext* cx, unsigned argc,
                                      JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "captureFirstSubsumedFrame", 1)) {
    return false;
  }

  if (!args[0].isObject()) {
    JS_ReportErrorASCII(cx, "The argument must be an object");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());
  obj = CheckedUnwrapStatic(obj);
  if (!obj) {
    JS_ReportErrorASCII(cx, "Denied permission to object.");
    return false;
  }

  JS::StackCapture capture(
      JS::FirstSubsumedFrame(cx, obj->nonCCWRealm()->principals()));
  if (args.length() > 1) {
    capture.as<JS::FirstSubsumedFrame>().ignoreSelfHosted =
        JS::ToBoolean(args[1]);
  }

  JS::RootedObject capturedStack(cx);
  if (!JS::CaptureCurrentStack(cx, &capturedStack, std::move(capture))) {
    return false;
  }

  args.rval().setObjectOrNull(capturedStack);
  return true;
}

static bool CallFunctionFromNativeFrame(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1) {
    JS_ReportErrorASCII(cx, "The function takes exactly one argument.");
    return false;
  }
  if (!args[0].isObject() || !IsCallable(args[0])) {
    JS_ReportErrorASCII(cx, "The first argument should be a function.");
    return false;
  }

  RootedObject function(cx, &args[0].toObject());
  return Call(cx, UndefinedHandleValue, function, JS::HandleValueArray::empty(),
              args.rval());
}

static bool CallFunctionWithAsyncStack(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 3) {
    JS_ReportErrorASCII(cx, "The function takes exactly three arguments.");
    return false;
  }
  if (!args[0].isObject() || !IsCallable(args[0])) {
    JS_ReportErrorASCII(cx, "The first argument should be a function.");
    return false;
  }
  if (!args[1].isObject() || !args[1].toObject().is<SavedFrame>()) {
    JS_ReportErrorASCII(cx, "The second argument should be a SavedFrame.");
    return false;
  }
  if (!args[2].isString() || args[2].toString()->empty()) {
    JS_ReportErrorASCII(cx, "The third argument should be a non-empty string.");
    return false;
  }

  RootedObject function(cx, &args[0].toObject());
  RootedObject stack(cx, &args[1].toObject());
  RootedString asyncCause(cx, args[2].toString());
  UniqueChars utf8Cause = JS_EncodeStringToUTF8(cx, asyncCause);
  if (!utf8Cause) {
    MOZ_ASSERT(cx->isExceptionPending());
    return false;
  }

  JS::AutoSetAsyncStackForNewCalls sas(
      cx, stack, utf8Cause.get(),
      JS::AutoSetAsyncStackForNewCalls::AsyncCallKind::EXPLICIT);
  return Call(cx, UndefinedHandleValue, function, JS::HandleValueArray::empty(),
              args.rval());
}

static bool EnableTrackAllocations(JSContext* cx, unsigned argc, Value* vp) {
  SetAllocationMetadataBuilder(cx, &SavedStacks::metadataBuilder);
  return true;
}

static bool DisableTrackAllocations(JSContext* cx, unsigned argc, Value* vp) {
  SetAllocationMetadataBuilder(cx, nullptr);
  return true;
}

static bool SetTestFilenameValidationCallback(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Accept all filenames that start with "safe". In system code also accept
  // filenames starting with "system".
  auto testCb = [](JSContext* cx, const char* filename) -> bool {
    if (strstr(filename, "safe") == filename) {
      return true;
    }
    if (cx->realm()->isSystem() && strstr(filename, "system") == filename) {
      return true;
    }

    return false;
  };
  JS::SetFilenameValidationCallback(testCb);

  args.rval().setUndefined();
  return true;
}

static JSAtom* GetPropertiesAddedName(JSContext* cx) {
  const char* propName = "_propertiesAdded";
  return Atomize(cx, propName, strlen(propName));
}

static bool NewObjectWithAddPropertyHook(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  auto addPropHook = [](JSContext* cx, HandleObject obj, HandleId id,
                        HandleValue v) -> bool {
    Rooted<JSAtom*> propName(cx, GetPropertiesAddedName(cx));
    if (!propName) {
      return false;
    }
    // Don't do anything if we're adding the _propertiesAdded property.
    RootedId propId(cx, AtomToId(propName));
    if (id == propId) {
      return true;
    }
    // Increment _propertiesAdded.
    RootedValue val(cx);
    if (!JS_GetPropertyById(cx, obj, propId, &val)) {
      return false;
    }
    if (!val.isInt32() || val.toInt32() == INT32_MAX) {
      return true;
    }
    val.setInt32(val.toInt32() + 1);
    return JS_DefinePropertyById(cx, obj, propId, val, 0);
  };

  static const JSClassOps classOps = {
      addPropHook,  // addProperty
      nullptr,      // delProperty
      nullptr,      // enumerate
      nullptr,      // newEnumerate
      nullptr,      // resolve
      nullptr,      // mayResolve
      nullptr,      // finalize
      nullptr,      // call
      nullptr,      // construct
      nullptr,      // trace
  };
  static const JSClass cls = {
      "ObjectWithAddPropHook",
      0,
      &classOps,
  };

  RootedObject obj(cx, JS_NewObject(cx, &cls));
  if (!obj) {
    return false;
  }

  // Initialize _propertiesAdded to 0.
  Rooted<JSAtom*> propName(cx, GetPropertiesAddedName(cx));
  if (!propName) {
    return false;
  }
  RootedId propId(cx, AtomToId(propName));
  RootedValue val(cx, Int32Value(0));
  if (!JS_DefinePropertyById(cx, obj, propId, val, 0)) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool NewObjectWithCallHook(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  static auto hookShared = [](JSContext* cx, CallArgs& args) {
    Rooted<PlainObject*> obj(cx, NewPlainObject(cx));
    if (!obj) {
      return false;
    }

    // Define |this|. We can't expose the MagicValue to JS, so we use
    // "<is_constructing>" in that case.
    Rooted<Value> thisv(cx, args.thisv());
    if (thisv.isMagic(JS_IS_CONSTRUCTING)) {
      JSString* str = NewStringCopyZ<CanGC>(cx, "<is_constructing>");
      if (!str) {
        return false;
      }
      thisv.setString(str);
    }
    if (!DefineDataProperty(cx, obj, cx->names().this_, thisv,
                            JSPROP_ENUMERATE)) {
      return false;
    }

    // Define |callee|.
    if (!DefineDataProperty(cx, obj, cx->names().callee, args.calleev(),
                            JSPROP_ENUMERATE)) {
      return false;
    }

    // Define |arguments| array.
    Rooted<ArrayObject*> arr(
        cx, NewDenseCopiedArray(cx, args.length(), args.array()));
    if (!arr) {
      return false;
    }
    Rooted<Value> arrVal(cx, ObjectValue(*arr));
    if (!DefineDataProperty(cx, obj, cx->names().arguments, arrVal,
                            JSPROP_ENUMERATE)) {
      return false;
    }

    // Define |newTarget| if constructing.
    if (args.isConstructing()) {
      const char* propName = "newTarget";
      Rooted<JSAtom*> name(cx, Atomize(cx, propName, strlen(propName)));
      if (!name) {
        return false;
      }
      Rooted<PropertyKey> key(cx, NameToId(name->asPropertyName()));
      if (!DefineDataProperty(cx, obj, key, args.newTarget(),
                              JSPROP_ENUMERATE)) {
        return false;
      }
    }

    args.rval().setObject(*obj);
    return true;
  };

  static auto callHook = [](JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(!args.isConstructing());
    return hookShared(cx, args);
  };
  static auto constructHook = [](JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.isConstructing());
    return hookShared(cx, args);
  };

  static const JSClassOps classOps = {
      nullptr,        // addProperty
      nullptr,        // delProperty
      nullptr,        // enumerate
      nullptr,        // newEnumerate
      nullptr,        // resolve
      nullptr,        // mayResolve
      nullptr,        // finalize
      callHook,       // call
      constructHook,  // construct
      nullptr,        // trace
  };
  static const JSClass cls = {
      "ObjectWithCallHook",
      0,
      &classOps,
  };

  Rooted<JSObject*> obj(cx, JS_NewObject(cx, &cls));
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static constexpr JSClass ObjectWithManyReservedSlotsClass = {
    "ObjectWithManyReservedSlots", JSCLASS_HAS_RESERVED_SLOTS(40)};

static bool NewObjectWithManyReservedSlots(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  static constexpr size_t NumReservedSlots =
      JSCLASS_RESERVED_SLOTS(&ObjectWithManyReservedSlotsClass);
  static_assert(NumReservedSlots > NativeObject::MAX_FIXED_SLOTS);

  RootedObject obj(cx, JS_NewObject(cx, &ObjectWithManyReservedSlotsClass));
  if (!obj) {
    return false;
  }

  for (size_t i = 0; i < NumReservedSlots; i++) {
    JS_SetReservedSlot(obj, i, Int32Value(i));
  }

  args.rval().setObject(*obj);
  return true;
}

static bool CheckObjectWithManyReservedSlots(JSContext* cx, unsigned argc,
                                             Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1 || !args[0].isObject() ||
      args[0].toObject().getClass() != &ObjectWithManyReservedSlotsClass) {
    JS_ReportErrorASCII(cx,
                        "Expected object from newObjectWithManyReservedSlots");
    return false;
  }

  JSObject* obj = &args[0].toObject();

  static constexpr size_t NumReservedSlots =
      JSCLASS_RESERVED_SLOTS(&ObjectWithManyReservedSlotsClass);

  for (size_t i = 0; i < NumReservedSlots; i++) {
    MOZ_RELEASE_ASSERT(JS::GetReservedSlot(obj, i).toInt32() == int32_t(i));
  }

  args.rval().setUndefined();
  return true;
}

static bool GetWatchtowerLog(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<GCVector<Value>> values(cx, GCVector<Value>(cx));

  if (auto* log = cx->runtime()->watchtowerTestingLog.ref().get()) {
    Rooted<JSObject*> elem(cx);
    for (PlainObject* obj : *log) {
      elem = obj;
      if (!cx->compartment()->wrap(cx, &elem)) {
        return false;
      }
      if (!values.append(ObjectValue(*elem))) {
        return false;
      }
    }
    log->clearAndFree();
  }

  ArrayObject* arr = NewDenseCopiedArray(cx, values.length(), values.begin());
  if (!arr) {
    return false;
  }

  args.rval().setObject(*arr);
  return true;
}

static bool AddWatchtowerTarget(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1 || !args[0].isObject()) {
    JS_ReportErrorASCII(cx, "Expected a single object argument.");
    return false;
  }

  if (!cx->runtime()->watchtowerTestingLog.ref()) {
    auto vec = cx->make_unique<JSRuntime::RootedPlainObjVec>(cx);
    if (!vec) {
      return false;
    }
    cx->runtime()->watchtowerTestingLog = std::move(vec);
  }

  RootedObject obj(cx, &args[0].toObject());
  if (!JSObject::setUseWatchtowerTestingLog(cx, obj)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

struct TestExternalString : public JSExternalStringCallbacks {
  void finalize(JS::Latin1Char* chars) const override { js_free(chars); }
  void finalize(char16_t* chars) const override { js_free(chars); }
  size_t sizeOfBuffer(const JS::Latin1Char* chars,
                      mozilla::MallocSizeOf mallocSizeOf) const override {
    return mallocSizeOf(chars);
  }
  size_t sizeOfBuffer(const char16_t* chars,
                      mozilla::MallocSizeOf mallocSizeOf) const override {
    return mallocSizeOf(chars);
  }
};

static constexpr TestExternalString TestExternalStringCallbacks;

static bool NewString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString src(cx, ToString(cx, args.get(0)));
  if (!src) {
    return false;
  }

  gc::Heap heap = gc::Heap::Default;
  bool wantTwoByte = false;
  bool forceExternal = false;
  bool maybeExternal = false;
  uint32_t capacity = 0;

  if (args.get(1).isObject()) {
    RootedObject options(cx, &args[1].toObject());
    RootedValue v(cx);
    bool requestTenured = false;
    struct BoolSetting {
      const char* name;
      bool* value;
    };
    for (auto [name, setting] :
         {BoolSetting{"tenured", &requestTenured},
          BoolSetting{"twoByte", &wantTwoByte},
          BoolSetting{"external", &forceExternal},
          BoolSetting{"maybeExternal", &maybeExternal}}) {
      if (!JS_GetProperty(cx, options, name, &v)) {
        return false;
      }
      *setting = ToBoolean(v);  // false if not given (or otherwise undefined)
    }
    struct Uint32Setting {
      const char* name;
      uint32_t* value;
    };
    for (auto [name, setting] : {Uint32Setting{"capacity", &capacity}}) {
      if (!JS_GetProperty(cx, options, name, &v)) {
        return false;
      }
      int32_t i32;
      if (!ToInt32(cx, v, &i32)) {
        return false;
      }
      if (i32 < 0) {
        JS_ReportErrorASCII(cx, "nonnegative value required");
        return false;
      }
      *setting = static_cast<uint32_t>(i32);
    }

    heap = requestTenured ? gc::Heap::Tenured : gc::Heap::Default;
    if (forceExternal || maybeExternal) {
      wantTwoByte = true;
      if (capacity != 0) {
        JS_ReportErrorASCII(cx,
                            "strings cannot be both external and extensible");
        return false;
      }
    }
  }

  auto len = src->length();
  RootedString dest(cx);

  if (forceExternal || maybeExternal) {
    auto buf = cx->make_pod_array<char16_t>(len);
    if (!buf) {
      return false;
    }

    if (!JS_CopyStringChars(cx, mozilla::Range<char16_t>(buf.get(), len),
                            src)) {
      return false;
    }

    bool isExternal = true;
    if (forceExternal) {
      dest = JSExternalString::new_(cx, buf.get(), len,
                                    &TestExternalStringCallbacks);
    } else {
      dest = NewMaybeExternalString(
          cx, buf.get(), len, &TestExternalStringCallbacks, &isExternal, heap);
    }
    if (dest && isExternal) {
      (void)buf.release();  // Ownership was transferred.
    }
  } else {
    AutoStableStringChars stable(cx);
    if (!wantTwoByte && src->hasLatin1Chars()) {
      if (!stable.init(cx, src)) {
        return false;
      }
    } else {
      if (!stable.initTwoByte(cx, src)) {
        return false;
      }
    }
    if (capacity) {
      if (capacity < len) {
        capacity = len;
      }
      if (len == 0) {
        JS_ReportErrorASCII(cx, "Cannot set capacity of empty string");
        return false;
      }

      auto createLinearString = [&](const auto* chars) -> JSLinearString* {
        using CharT =
            std::remove_const_t<std::remove_pointer_t<decltype(chars)>>;

        if (JSInlineString::lengthFits<CharT>(len)) {
          JS_ReportErrorASCII(cx, "Cannot create small non-inline strings");
          return nullptr;
        }

        auto news =
            cx->make_pod_arena_array<CharT>(js::StringBufferArena, capacity);
        if (!news) {
          return nullptr;
        }
        mozilla::PodCopy(news.get(), chars, len);
        Rooted<JSString::OwnedChars<CharT>> owned(cx, std::move(news), len,
                                                  true);
        return JSLinearString::newValidLength<CanGC, CharT>(cx, &owned, heap);
      };

      if (stable.isLatin1()) {
        dest = createLinearString(stable.latin1Chars());
      } else {
        dest = createLinearString(stable.twoByteChars());
      }
      if (dest) {
        dest->asLinear().makeExtensible(capacity);
      }
    } else if (wantTwoByte) {
      dest = NewStringCopyNDontDeflate<CanGC>(cx, stable.twoByteChars(), len,
                                              heap);
    } else if (stable.isLatin1()) {
      dest = NewStringCopyN<CanGC>(cx, stable.latin1Chars(), len, heap);
    } else {
      // Normal behavior: auto-deflate to latin1 if possible.
      dest = NewStringCopyN<CanGC>(cx, stable.twoByteChars(), len, heap);
    }
  }

  if (!dest) {
    return false;
  }

  args.rval().setString(dest);
  return true;
}

static bool NewDependentString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString src(cx, ToString(cx, args.get(0)));
  if (!src) {
    return false;
  }

  uint64_t indexStart = 0;
  mozilla::Maybe<uint64_t> indexEnd;
  gc::Heap heap = gc::Heap::Default;
  mozilla::Maybe<gc::Heap> requiredHeap;

  if (!ToIndex(cx, args.get(1), &indexStart)) {
    return false;
  }

  Rooted<Value> options(cx);
  if (args.get(2).isObject()) {
    options = args[2];
  } else {
    uint64_t idx;
    if (args.hasDefined(2)) {
      if (!ToIndex(cx, args.get(2), &idx)) {
        return false;
      }
      indexEnd.emplace(idx);
    }
    options = args.get(3);
  }

  if (options.isObject()) {
    Rooted<Value> v(cx);
    Rooted<JSObject*> optObj(cx, &options.toObject());
    if (!JS_GetProperty(cx, optObj, "tenured", &v)) {
      return false;
    }
    if (v.isBoolean()) {
      requiredHeap.emplace(v.toBoolean() ? gc::Heap::Tenured
                                         : gc::Heap::Default);
      heap = *requiredHeap;
    }
  }

  if (indexEnd.isNothing()) {
    // Read the length now that no more JS code can run.
    indexEnd.emplace(src->length());
  }
  if (indexStart > src->length() || *indexEnd > src->length() ||
      indexStart >= *indexEnd) {
    JS_ReportErrorASCII(cx, "invalid dependent string bounds");
    return false;
  }
  if (!src->ensureLinear(cx)) {
    return false;
  }
  Rooted<JSString*> result(
      cx, js::NewDependentString(cx, src, indexStart, *indexEnd - indexStart,
                                 heap));
  if (!result) {
    return false;
  }
  if (!result->isDependent()) {
    JS_ReportErrorASCII(cx, "resulting string is not dependent (too short?)");
    return false;
  }

  if (requiredHeap.isSome()) {
    if ((*requiredHeap == gc::Heap::Tenured) != result->isTenured()) {
      if (result->isTenured()) {
        JS_ReportErrorASCII(cx, "nursery string created in tenured heap");
        return false;
      } else {
        JS_ReportErrorASCII(cx, "tenured string created in nursery heap");
        return false;
      }
    }
  }

  args.rval().setString(result);
  return true;
}

// Warning! This will let you create ropes that I'm not sure would be possible
// otherwise, specifically:
//
//   - a rope with a zero-length child
//   - a rope that would fit into an inline string
//
static bool NewRope(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.get(0).isString() || !args.get(1).isString()) {
    JS_ReportErrorASCII(cx, "newRope requires two string arguments.");
    return false;
  }

  gc::Heap heap = js::gc::Heap::Default;
  if (args.get(2).isObject()) {
    RootedObject options(cx, &args[2].toObject());
    RootedValue v(cx);
    if (!JS_GetProperty(cx, options, "nursery", &v)) {
      return false;
    }
    if (!v.isUndefined() && !ToBoolean(v)) {
      heap = js::gc::Heap::Tenured;
    }
  }

  RootedString left(cx, args[0].toString());
  RootedString right(cx, args[1].toString());
  size_t length = JS_GetStringLength(left) + JS_GetStringLength(right);
  if (length > JSString::MAX_LENGTH) {
    JS_ReportErrorASCII(cx, "rope length exceeds maximum string length");
    return false;
  }

  // Disallow creating ropes where one side is empty.
  if (left->empty() || right->empty()) {
    JS_ReportErrorASCII(cx, "rope child mustn't be the empty string");
    return false;
  }

  // Disallow creating ropes which fit into inline strings.
  if (left->hasLatin1Chars() && right->hasLatin1Chars()) {
    if (JSInlineString::lengthFits<JS::Latin1Char>(length)) {
      JS_ReportErrorASCII(cx, "Cannot create small non-inline ropes");
      return false;
    }
  } else {
    if (JSInlineString::lengthFits<char16_t>(length)) {
      JS_ReportErrorASCII(cx, "Cannot create small non-inline ropes");
      return false;
    }
  }

  auto* str = JSRope::new_<CanGC>(cx, left, right, length, heap);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool IsRope(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.get(0).isString()) {
    JS_ReportErrorASCII(cx, "isRope requires a string argument.");
    return false;
  }

  JSString* str = args[0].toString();
  args.rval().setBoolean(str->isRope());
  return true;
}

static bool EnsureLinearString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1 || !args[0].isString()) {
    JS_ReportErrorASCII(
        cx, "ensureLinearString takes exactly one string argument.");
    return false;
  }

  JSLinearString* linear = args[0].toString()->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  args.rval().setString(linear);
  return true;
}

static bool RepresentativeStringArray(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject array(cx, JS::NewArrayObject(cx, 0));
  if (!array) {
    return false;
  }

  if (!JSString::fillWithRepresentatives(cx, array.as<ArrayObject>())) {
    return false;
  }

  args.rval().setObject(*array);
  return true;
}

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

static bool OOMThreadTypes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setInt32(js::THREAD_TYPE_MAX);
  return true;
}

static bool CheckCanSimulateOOM(JSContext* cx) {
  if (js::oom::GetThreadType() != js::THREAD_TYPE_MAIN) {
    JS_ReportErrorASCII(
        cx, "Simulated OOM failure is only supported on the main thread");
    return false;
  }

  return true;
}

static bool SetupOOMFailure(JSContext* cx, bool failAlways, unsigned argc,
                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (disableOOMFunctions) {
    args.rval().setUndefined();
    return true;
  }

  if (args.length() < 1) {
    JS_ReportErrorASCII(cx, "Count argument required");
    return false;
  }

  if (args.length() > 2) {
    JS_ReportErrorASCII(cx, "Too many arguments");
    return false;
  }

  int32_t count;
  if (!JS::ToInt32(cx, args.get(0), &count)) {
    return false;
  }

  if (count <= 0) {
    JS_ReportErrorASCII(cx, "OOM cutoff should be positive");
    return false;
  }

  uint32_t targetThread = js::THREAD_TYPE_MAIN;
  if (args.length() > 1 && !ToUint32(cx, args[1], &targetThread)) {
    return false;
  }

  if (targetThread == js::THREAD_TYPE_NONE ||
      targetThread == js::THREAD_TYPE_WORKER ||
      targetThread >= js::THREAD_TYPE_MAX) {
    JS_ReportErrorASCII(cx, "Invalid thread type specified");
    return false;
  }

  if (!CheckCanSimulateOOM(cx)) {
    return false;
  }

  js::oom::simulator.simulateFailureAfter(js::oom::FailureSimulator::Kind::OOM,
                                          count, targetThread, failAlways);
  args.rval().setUndefined();
  return true;
}

static bool OOMAfterAllocations(JSContext* cx, unsigned argc, Value* vp) {
  return SetupOOMFailure(cx, true, argc, vp);
}

static bool OOMAtAllocation(JSContext* cx, unsigned argc, Value* vp) {
  return SetupOOMFailure(cx, false, argc, vp);
}

static bool ResetOOMFailure(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!CheckCanSimulateOOM(cx)) {
    return false;
  }

  args.rval().setBoolean(js::oom::HadSimulatedOOM());
  js::oom::simulator.reset();
  return true;
}

static size_t CountCompartments(JSContext* cx) {
  size_t count = 0;
  for (auto zone : cx->runtime()->gc.zones()) {
    count += zone->compartments().length();
  }
  return count;
}

// Iterative failure testing: test a function by simulating failures at indexed
// locations throughout the normal execution path and checking that the
// resulting state of the environment is consistent with the error result.
//
// For example, trigger OOM at every allocation point and test that the function
// either recovers and succeeds or raises an exception and fails.

class MOZ_STACK_CLASS IterativeFailureTest {
 public:
  struct FailureSimulator {
    virtual void setup(JSContext* cx) {}
    virtual void teardown(JSContext* cx) {}
    virtual void startSimulating(JSContext* cx, unsigned iteration,
                                 unsigned thread, bool keepFailing) = 0;
    virtual bool stopSimulating() = 0;
    virtual void cleanup(JSContext* cx) {}
  };

  IterativeFailureTest(JSContext* cx, FailureSimulator& simulator);
  bool initParams(const CallArgs& args);
  bool test();

 private:
  bool setup();
  bool testThread(unsigned thread);
  bool testIteration(unsigned thread, unsigned iteration,
                     bool& failureWasSimulated, MutableHandleValue exception);
  void cleanup();
  void teardown();

  JSContext* const cx;
  FailureSimulator& simulator;
  size_t compartmentCount;

  // Test parameters set by initParams.
  RootedFunction testFunction;
  unsigned threadStart = 0;
  unsigned threadEnd = 0;
  bool expectExceptionOnFailure = true;
  bool keepFailing = false;
  bool verbose = false;
};

bool RunIterativeFailureTest(
    JSContext* cx, const CallArgs& args,
    IterativeFailureTest::FailureSimulator& simulator) {
  IterativeFailureTest test(cx, simulator);
  return test.initParams(args) && test.test();
}

IterativeFailureTest::IterativeFailureTest(JSContext* cx,
                                           FailureSimulator& simulator)
    : cx(cx), simulator(simulator), testFunction(cx) {}

bool IterativeFailureTest::test() {
  if (disableOOMFunctions) {
    return true;
  }

  if (!setup()) {
    return false;
  }

  auto onExit = mozilla::MakeScopeExit([this] { teardown(); });

  for (unsigned thread = threadStart; thread <= threadEnd; thread++) {
    if (!testThread(thread)) {
      return false;
    }
  }

  return true;
}

bool IterativeFailureTest::setup() {
  if (!CheckCanSimulateOOM(cx)) {
    return false;
  }

  // Disallow nested tests.
  if (cx->runningOOMTest) {
    JS_ReportErrorASCII(
        cx, "Nested call to iterative failure test is not allowed.");
    return false;
  }
  cx->runningOOMTest = true;

  MOZ_ASSERT(!cx->isExceptionPending());

#  ifdef JS_GC_ZEAL
  JS::SetGCZeal(cx, 0, JS::ShellDefaultGCZealFrequency);
#  endif

  // Delazify the function here if necessary so we don't end up testing that.
  if (testFunction->isInterpreted() &&
      !JSFunction::getOrCreateScript(cx, testFunction)) {
    return false;
  }

  compartmentCount = CountCompartments(cx);

  simulator.setup(cx);

  return true;
}

bool IterativeFailureTest::testThread(unsigned thread) {
  if (verbose) {
    fprintf(stderr, "thread %u\n", thread);
  }

  RootedValue exception(cx);

  unsigned iteration = 1;
  bool failureWasSimulated;
  do {
    if (!testIteration(thread, iteration, failureWasSimulated, &exception)) {
      return false;
    }

    iteration++;
  } while (failureWasSimulated);

  if (verbose) {
    fprintf(stderr, "  finished after %u iterations\n", iteration - 1);
    if (!exception.isUndefined()) {
      RootedString str(cx, JS::ToString(cx, exception));
      if (!str) {
        fprintf(stderr, "  error while trying to print exception, giving up\n");
        return false;
      }
      UniqueChars bytes(JS_EncodeStringToUTF8(cx, str));
      if (!bytes) {
        return false;
      }
      fprintf(stderr, "  threw %s\n", bytes.get());
    }
  }

  return true;
}

bool IterativeFailureTest::testIteration(unsigned thread, unsigned iteration,
                                         bool& failureWasSimulated,
                                         MutableHandleValue exception) {
  if (verbose) {
    fprintf(stderr, "  iteration %u\n", iteration);
  }

  MOZ_RELEASE_ASSERT(!cx->isExceptionPending());

  simulator.startSimulating(cx, iteration, thread, keepFailing);

  RootedValue result(cx);
  bool ok = JS_CallFunction(cx, cx->global(), testFunction,
                            HandleValueArray::empty(), &result);

  failureWasSimulated = simulator.stopSimulating();

  if (ok && cx->isExceptionPending()) {
    MOZ_CRASH(
        "Thunk execution succeeded but an exception was raised - missing error "
        "check?");
  }

  if (!ok && !cx->isExceptionPending() && expectExceptionOnFailure) {
    MOZ_CRASH(
        "Thunk execution failed but no exception was raised - missing call to "
        "js::ReportOutOfMemory()?");
  }

  // Note that it is possible that the function throws an exception unconnected
  // to the simulated failure, in which case we ignore it. More correct would be
  // to have the caller pass some kind of exception specification and to check
  // the exception against it.
  if (!failureWasSimulated && cx->isExceptionPending()) {
    if (!cx->getPendingException(exception)) {
      return false;
    }
  }
  cx->clearPendingException();

  cleanup();

  return true;
}

void IterativeFailureTest::cleanup() {
  simulator.cleanup(cx);

  gc::FinishGC(cx);

  // Some tests create a new compartment or zone on every iteration. Our GC is
  // triggered by GC allocations and not by number of compartments or zones, so
  // these won't normally get cleaned up. The check here stops some tests
  // running out of memory. ("Gentlemen, you can't fight in here! This is the
  // War oom!")
  if (CountCompartments(cx) > compartmentCount + 100) {
    JS_GC(cx);
    compartmentCount = CountCompartments(cx);
  }
}

void IterativeFailureTest::teardown() {
  simulator.teardown(cx);

  cx->runningOOMTest = false;
}

bool IterativeFailureTest::initParams(const CallArgs& args) {
  if (args.length() < 1 || args.length() > 2) {
    JS_ReportErrorASCII(cx, "function takes between 1 and 2 arguments.");
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "The first argument must be the function to test.");
    return false;
  }
  testFunction = &args[0].toObject().as<JSFunction>();

  if (args.length() == 2) {
    if (args[1].isBoolean()) {
      expectExceptionOnFailure = args[1].toBoolean();
    } else if (args[1].isObject()) {
      RootedObject options(cx, &args[1].toObject());
      RootedValue value(cx);

      if (!JS_GetProperty(cx, options, "expectExceptionOnFailure", &value)) {
        return false;
      }
      if (!value.isUndefined()) {
        expectExceptionOnFailure = ToBoolean(value);
      }

      if (!JS_GetProperty(cx, options, "keepFailing", &value)) {
        return false;
      }
      if (!value.isUndefined()) {
        keepFailing = ToBoolean(value);
      }
    } else {
      JS_ReportErrorASCII(
          cx, "The optional second argument must be an object or a boolean.");
      return false;
    }
  }

  // There are some places where we do fail without raising an exception, so
  // we can't expose this to the fuzzers by default.
  if (fuzzingSafe) {
    expectExceptionOnFailure = false;
  }

  // Test all threads by default except worker threads.
  threadStart = oom::FirstThreadTypeToTest;
  threadEnd = oom::LastThreadTypeToTest;

  // Test a single thread type if specified by the OOM_THREAD environment
  // variable.
  int threadOption = 0;
  if (EnvVarAsInt("OOM_THREAD", &threadOption)) {
    if (threadOption < oom::FirstThreadTypeToTest ||
        threadOption > oom::LastThreadTypeToTest) {
      JS_ReportErrorASCII(cx, "OOM_THREAD value out of range.");
      return false;
    }

    threadStart = threadOption;
    threadEnd = threadOption;
  }

  verbose = EnvVarIsDefined("OOM_VERBOSE");

  return true;
}

struct OOMSimulator : public IterativeFailureTest::FailureSimulator {
  void setup(JSContext* cx) override { cx->runtime()->hadOutOfMemory = false; }

  void startSimulating(JSContext* cx, unsigned i, unsigned thread,
                       bool keepFailing) override {
    MOZ_ASSERT(!cx->runtime()->hadOutOfMemory);
    js::oom::simulator.simulateFailureAfter(
        js::oom::FailureSimulator::Kind::OOM, i, thread, keepFailing);
  }

  bool stopSimulating() override {
    bool handledOOM = js::oom::HadSimulatedOOM();
    js::oom::simulator.reset();
    return handledOOM;
  }

  void cleanup(JSContext* cx) override {
    cx->runtime()->hadOutOfMemory = false;
  }
};

static bool OOMTest(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  OOMSimulator simulator;
  if (!RunIterativeFailureTest(cx, args, simulator)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

struct StackOOMSimulator : public IterativeFailureTest::FailureSimulator {
  void startSimulating(JSContext* cx, unsigned i, unsigned thread,
                       bool keepFailing) override {
    js::oom::simulator.simulateFailureAfter(
        js::oom::FailureSimulator::Kind::StackOOM, i, thread, keepFailing);
  }

  bool stopSimulating() override {
    bool handledOOM = js::oom::HadSimulatedStackOOM();
    js::oom::simulator.reset();
    return handledOOM;
  }
};

static bool StackTest(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  StackOOMSimulator simulator;
  if (!RunIterativeFailureTest(cx, args, simulator)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

struct FailingIterruptSimulator
    : public IterativeFailureTest::FailureSimulator {
  JSInterruptCallback* prevEnd = nullptr;

  static bool failingInterruptCallback(JSContext* cx) { return false; }

  void setup(JSContext* cx) override {
    prevEnd = cx->interruptCallbacks().end();
    JS_AddInterruptCallback(cx, failingInterruptCallback);
  }

  void teardown(JSContext* cx) override {
    cx->interruptCallbacks().erase(prevEnd, cx->interruptCallbacks().end());
  }

  void startSimulating(JSContext* cx, unsigned i, unsigned thread,
                       bool keepFailing) override {
    js::oom::simulator.simulateFailureAfter(
        js::oom::FailureSimulator::Kind::Interrupt, i, thread, keepFailing);
  }

  bool stopSimulating() override {
    bool handledInterrupt = js::oom::HadSimulatedInterrupt();
    js::oom::simulator.reset();
    return handledInterrupt;
  }
};

static bool InterruptTest(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  FailingIterruptSimulator simulator;
  if (!RunIterativeFailureTest(cx, args, simulator)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

#endif  // defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

static bool SettlePromiseNow(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "settlePromiseNow", 1)) {
    return false;
  }
  if (!args[0].isObject() || !args[0].toObject().is<PromiseObject>()) {
    JS_ReportErrorASCII(cx, "first argument must be a Promise object");
    return false;
  }

  Rooted<PromiseObject*> promise(cx, &args[0].toObject().as<PromiseObject>());
  if (IsPromiseForAsyncFunctionOrGenerator(promise)) {
    JS_ReportErrorASCII(
        cx, "async function/generator's promise shouldn't be manually settled");
    return false;
  }

  if (promise->state() != JS::PromiseState::Pending) {
    JS_ReportErrorASCII(cx, "cannot settle an already-resolved promise");
    return false;
  }

  if (IsPromiseWithDefaultResolvingFunction(promise)) {
    SetAlreadyResolvedPromiseWithDefaultResolvingFunction(promise);
  }

  int32_t flags = promise->flags();
  promise->setFixedSlot(
      PromiseSlot_Flags,
      Int32Value(flags | PROMISE_FLAG_RESOLVED | PROMISE_FLAG_FULFILLED));
  promise->setFixedSlot(PromiseSlot_ReactionsOrResult, UndefinedValue());

  DebugAPI::onPromiseSettled(cx, promise);
  return true;
}

static bool GetWaitForAllPromise(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "getWaitForAllPromise", 1)) {
    return false;
  }
  if (!args[0].isObject() || !args[0].toObject().is<ArrayObject>() ||
      args[0].toObject().as<NativeObject>().isIndexed()) {
    JS_ReportErrorASCII(
        cx, "first argument must be a dense Array of Promise objects");
    return false;
  }
  Rooted<NativeObject*> list(cx, &args[0].toObject().as<NativeObject>());
  RootedObjectVector promises(cx);
  uint32_t count = list->getDenseInitializedLength();
  if (!promises.resize(count)) {
    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    RootedValue elem(cx, list->getDenseElement(i));
    if (!elem.isObject() || !elem.toObject().is<PromiseObject>()) {
      JS_ReportErrorASCII(
          cx, "Each entry in the passed-in Array must be a Promise");
      return false;
    }
    promises[i].set(&elem.toObject());
  }

  RootedObject resultPromise(cx, JS::GetWaitForAllPromise(cx, promises));
  if (!resultPromise) {
    return false;
  }

  args.rval().set(ObjectValue(*resultPromise));
  return true;
}

static bool ResolvePromise(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "resolvePromise", 2)) {
    return false;
  }
  if (!args[0].isObject() ||
      !UncheckedUnwrap(&args[0].toObject())->is<PromiseObject>()) {
    JS_ReportErrorASCII(
        cx, "first argument must be a maybe-wrapped Promise object");
    return false;
  }

  RootedObject promise(cx, &args[0].toObject());
  RootedValue resolution(cx, args[1]);
  mozilla::Maybe<AutoRealm> ar;
  if (IsWrapper(promise)) {
    promise = UncheckedUnwrap(promise);
    ar.emplace(cx, promise);
    if (!cx->compartment()->wrap(cx, &resolution)) {
      return false;
    }
  }

  if (IsPromiseForAsyncFunctionOrGenerator(promise)) {
    JS_ReportErrorASCII(
        cx,
        "async function/generator's promise shouldn't be manually resolved");
    return false;
  }

  bool result = JS::ResolvePromise(cx, promise, resolution);
  if (result) {
    args.rval().setUndefined();
  }
  return result;
}

static bool RejectPromise(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "rejectPromise", 2)) {
    return false;
  }
  if (!args[0].isObject() ||
      !UncheckedUnwrap(&args[0].toObject())->is<PromiseObject>()) {
    JS_ReportErrorASCII(
        cx, "first argument must be a maybe-wrapped Promise object");
    return false;
  }

  RootedObject promise(cx, &args[0].toObject());
  RootedValue reason(cx, args[1]);
  mozilla::Maybe<AutoRealm> ar;
  if (IsWrapper(promise)) {
    promise = UncheckedUnwrap(promise);
    ar.emplace(cx, promise);
    if (!cx->compartment()->wrap(cx, &reason)) {
      return false;
    }
  }

  if (IsPromiseForAsyncFunctionOrGenerator(promise)) {
    JS_ReportErrorASCII(
        cx,
        "async function/generator's promise shouldn't be manually rejected");
    return false;
  }

  bool result = JS::RejectPromise(cx, promise, reason);
  if (result) {
    args.rval().setUndefined();
  }
  return result;
}

static unsigned finalizeCount = 0;

static void finalize_counter_finalize(JS::GCContext* gcx, JSObject* obj) {
  ++finalizeCount;
}

static const JSClassOps FinalizeCounterClassOps = {
    nullptr,                    // addProperty
    nullptr,                    // delProperty
    nullptr,                    // enumerate
    nullptr,                    // newEnumerate
    nullptr,                    // resolve
    nullptr,                    // mayResolve
    finalize_counter_finalize,  // finalize
    nullptr,                    // call
    nullptr,                    // construct
    nullptr,                    // trace
};

static const JSClass FinalizeCounterClass = {
    "FinalizeCounter", JSCLASS_FOREGROUND_FINALIZE, &FinalizeCounterClassOps};

static bool MakeFinalizeObserver(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JSObject* obj =
      JS_NewObjectWithGivenProto(cx, &FinalizeCounterClass, nullptr);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool FinalizeCount(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setInt32(finalizeCount);
  return true;
}

static bool ResetFinalizeCount(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  finalizeCount = 0;
  args.rval().setUndefined();
  return true;
}

static bool DumpHeap(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  FILE* dumpFile = stdout;
  auto closeFile = mozilla::MakeScopeExit([&dumpFile] {
    if (dumpFile && dumpFile != stdout) {
      fclose(dumpFile);
    }
  });

  if (args.length() > 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }

  if (!args.get(0).isUndefined()) {
    RootedString str(cx, ToString(cx, args[0]));
    if (!str) {
      return false;
    }
    if (!fuzzingSafe) {
      UniqueChars fileNameBytes = JS_EncodeStringToUTF8(cx, str);
      if (!fileNameBytes) {
        return false;
      }
#ifdef XP_WIN
      UniqueWideChars wideFileNameBytes =
          JS::EncodeUtf8ToWide(cx, fileNameBytes.get());
      if (!wideFileNameBytes) {
        return false;
      }
      dumpFile = _wfopen(wideFileNameBytes.get(), L"w");
#else
      UniqueChars narrowFileNameBytes =
          JS::EncodeUtf8ToNarrow(cx, fileNameBytes.get());
      if (!narrowFileNameBytes) {
        return false;
      }
      dumpFile = fopen(narrowFileNameBytes.get(), "w");
#endif
      if (!dumpFile) {
        JS_ReportErrorUTF8(cx, "can't open %s", fileNameBytes.get());
        return false;
      }
    }
  }

  js::DumpHeap(cx, dumpFile, js::IgnoreNurseryObjects);

  args.rval().setUndefined();
  return true;
}

static bool Terminate(JSContext* cx, unsigned arg, Value* vp) {
  // Print a message to stderr in differential testing to help jsfunfuzz
  // find uncatchable-exception bugs.
  if (js::SupportDifferentialTesting()) {
    fprintf(stderr, "terminate called\n");
  }

  JS_ClearPendingException(cx);
  return false;
}

static bool ReadGeckoProfilingStack(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setUndefined();

  // Return boolean 'false' if profiler is not enabled.
  if (!cx->runtime()->geckoProfiler().enabled()) {
    args.rval().setBoolean(false);
    return true;
  }

  // Array holding physical jit stack frames.
  RootedObject stack(cx, NewDenseEmptyArray(cx));
  if (!stack) {
    return false;
  }

  // If profiler sampling has been suppressed, return an empty
  // stack.
  if (!cx->isProfilerSamplingEnabled()) {
    args.rval().setObject(*stack);
    return true;
  }

  struct InlineFrameInfo {
    InlineFrameInfo(const char* kind, UniqueChars label)
        : kind(kind), label(std::move(label)) {}
    const char* kind;
    UniqueChars label;
  };

  Vector<Vector<InlineFrameInfo, 0, TempAllocPolicy>, 0, TempAllocPolicy>
      frameInfo(cx);

  JS::ProfilingFrameIterator::RegisterState state;
  for (JS::ProfilingFrameIterator i(cx, state); !i.done(); ++i) {
    MOZ_ASSERT(i.stackAddress() != nullptr);

    if (!frameInfo.emplaceBack(cx)) {
      return false;
    }

    const size_t MaxInlineFrames = 16;
    JS::ProfilingFrameIterator::Frame frames[MaxInlineFrames];
    uint32_t nframes = i.extractStack(frames, 0, MaxInlineFrames);
    MOZ_ASSERT(nframes <= MaxInlineFrames);
    for (uint32_t i = 0; i < nframes; i++) {
      const char* frameKindStr = nullptr;
      switch (frames[i].kind) {
        case JS::ProfilingFrameIterator::Frame_BaselineInterpreter:
          frameKindStr = "baseline-interpreter";
          break;
        case JS::ProfilingFrameIterator::Frame_Baseline:
          frameKindStr = "baseline-jit";
          break;
        case JS::ProfilingFrameIterator::Frame_Ion:
          frameKindStr = "ion";
          break;
        case JS::ProfilingFrameIterator::Frame_WasmBaseline:
        case JS::ProfilingFrameIterator::Frame_WasmIon:
        case JS::ProfilingFrameIterator::Frame_WasmOther:
          frameKindStr = "wasm";
          break;
        default:
          frameKindStr = "unknown";
      }

      UniqueChars label =
          DuplicateStringToArena(js::StringBufferArena, cx, frames[i].label);
      if (!label) {
        return false;
      }

      if (!frameInfo.back().emplaceBack(frameKindStr, std::move(label))) {
        return false;
      }
    }
  }

  RootedObject inlineFrameInfo(cx);
  RootedString frameKind(cx);
  RootedString frameLabel(cx);
  RootedId idx(cx);

  const unsigned propAttrs = JSPROP_ENUMERATE;

  uint32_t physicalFrameNo = 0;
  for (auto& frame : frameInfo) {
    // Array holding all inline frames in a single physical jit stack frame.
    RootedObject inlineStack(cx, NewDenseEmptyArray(cx));
    if (!inlineStack) {
      return false;
    }

    uint32_t inlineFrameNo = 0;
    for (auto& inlineFrame : frame) {
      // Object holding frame info.
      RootedObject inlineFrameInfo(cx, NewPlainObject(cx));
      if (!inlineFrameInfo) {
        return false;
      }

      frameKind = NewStringCopyZ<CanGC>(cx, inlineFrame.kind);
      if (!frameKind) {
        return false;
      }

      if (!JS_DefineProperty(cx, inlineFrameInfo, "kind", frameKind,
                             propAttrs)) {
        return false;
      }

      frameLabel = NewLatin1StringZ(cx, std::move(inlineFrame.label));
      if (!frameLabel) {
        return false;
      }

      if (!JS_DefineProperty(cx, inlineFrameInfo, "label", frameLabel,
                             propAttrs)) {
        return false;
      }

      idx = PropertyKey::Int(inlineFrameNo);
      if (!JS_DefinePropertyById(cx, inlineStack, idx, inlineFrameInfo, 0)) {
        return false;
      }

      ++inlineFrameNo;
    }

    // Push inline array into main array.
    idx = PropertyKey::Int(physicalFrameNo);
    if (!JS_DefinePropertyById(cx, stack, idx, inlineStack, 0)) {
      return false;
    }

    ++physicalFrameNo;
  }

  args.rval().setObject(*stack);
  return true;
}

static bool ReadGeckoInterpProfilingStack(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setUndefined();

  // Return boolean 'false' if profiler is not enabled.
  if (!cx->runtime()->geckoProfiler().enabled()) {
    args.rval().setBoolean(false);
    return true;
  }

  // Array with information about each frame.
  Rooted<JSObject*> stack(cx, NewDenseEmptyArray(cx));
  if (!stack) {
    return false;
  }
  uint32_t stackIndex = 0;

  ProfilingStack* profStack = cx->geckoProfiler().getProfilingStack();
  MOZ_ASSERT(profStack);

  for (size_t i = 0; i < profStack->stackSize(); i++) {
    const auto& frame = profStack->frames[i];
    if (!frame.isJsFrame()) {
      continue;
    }

    // Skip fake JS frame pushed for js::RunScript by GeckoProfilerEntryMarker.
    const char* dynamicStr = frame.dynamicString();
    if (!dynamicStr) {
      continue;
    }

    Rooted<PlainObject*> frameInfo(cx, NewPlainObject(cx));
    if (!frameInfo) {
      return false;
    }

    Rooted<JSString*> dynamicString(
        cx, JS_NewStringCopyUTF8Z(
                cx, JS::ConstUTF8CharsZ(dynamicStr, strlen(dynamicStr))));
    if (!dynamicString) {
      return false;
    }
    if (!JS_DefineProperty(cx, frameInfo, "dynamicString", dynamicString,
                           JSPROP_ENUMERATE)) {
      return false;
    }

    if (!JS_DefineElement(cx, stack, stackIndex, frameInfo, JSPROP_ENUMERATE)) {
      return false;
    }
    stackIndex++;
  }

  args.rval().setObject(*stack);
  return true;
}

static bool EnableOsiPointRegisterChecks(JSContext*, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
#ifdef CHECK_OSIPOINT_REGISTERS
  jit::JitOptions.checkOsiPointRegisters = true;
#endif
  args.rval().setUndefined();
  return true;
}

static bool DisplayName(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.get(0).isObject() || !args[0].toObject().is<JSFunction>()) {
    RootedObject arg(cx, &args.callee());
    ReportUsageErrorASCII(cx, arg, "Must have one function argument");
    return false;
  }

  JSFunction* fun = &args[0].toObject().as<JSFunction>();
  JS::Rooted<JSAtom*> str(cx);
  if (!fun->getDisplayAtom(cx, &str)) {
    return false;
  }
  args.rval().setString(str ? str : cx->runtime()->emptyString.ref());
  return true;
}

static bool IsAvxPresent(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  int minVersion = 1;
  if (argc > 0 && args.get(0).isNumber()) {
    minVersion = std::max(1, int(args[0].toNumber()));
  }
  switch (minVersion) {
    case 1:
      args.rval().setBoolean(jit::Assembler::HasAVX());
      return true;
    case 2:
      args.rval().setBoolean(jit::Assembler::HasAVX2());
      return true;
  }
#endif
  args.rval().setBoolean(false);
  return true;
}

class ShellAllocationMetadataBuilder : public AllocationMetadataBuilder {
 public:
  ShellAllocationMetadataBuilder() = default;

  virtual JSObject* build(JSContext* cx, HandleObject,
                          AutoEnterOOMUnsafeRegion& oomUnsafe) const override;

  static const ShellAllocationMetadataBuilder metadataBuilder;
};

JSObject* ShellAllocationMetadataBuilder::build(
    JSContext* cx, HandleObject, AutoEnterOOMUnsafeRegion& oomUnsafe) const {
  RootedObject obj(cx, NewPlainObject(cx));
  if (!obj) {
    oomUnsafe.crash("ShellAllocationMetadataBuilder::build");
  }

  RootedObject stack(cx, NewDenseEmptyArray(cx));
  if (!stack) {
    oomUnsafe.crash("ShellAllocationMetadataBuilder::build");
  }

  static int createdIndex = 0;
  createdIndex++;

  if (!JS_DefineProperty(cx, obj, "index", createdIndex, 0)) {
    oomUnsafe.crash("ShellAllocationMetadataBuilder::build");
  }

  if (!JS_DefineProperty(cx, obj, "stack", stack, 0)) {
    oomUnsafe.crash("ShellAllocationMetadataBuilder::build");
  }

  int stackIndex = 0;
  RootedId id(cx);
  RootedValue callee(cx);
  for (NonBuiltinScriptFrameIter iter(cx); !iter.done(); ++iter) {
    if (iter.isFunctionFrame() && iter.compartment() == cx->compartment()) {
      id = PropertyKey::Int(stackIndex);
      RootedObject callee(cx, iter.callee(cx));
      if (!JS_DefinePropertyById(cx, stack, id, callee, JSPROP_ENUMERATE)) {
        oomUnsafe.crash("ShellAllocationMetadataBuilder::build");
      }
      stackIndex++;
    }
  }

  return obj;
}

const ShellAllocationMetadataBuilder
    ShellAllocationMetadataBuilder::metadataBuilder;

static bool EnableShellAllocationMetadataBuilder(JSContext* cx, unsigned argc,
                                                 Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  SetAllocationMetadataBuilder(
      cx, &ShellAllocationMetadataBuilder::metadataBuilder);

  args.rval().setUndefined();
  return true;
}

static bool GetAllocationMetadata(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1 || !args[0].isObject()) {
    JS_ReportErrorASCII(cx, "Argument must be an object");
    return false;
  }

  args.rval().setObjectOrNull(GetAllocationMetadata(&args[0].toObject()));
  return true;
}

static bool testingFunc_bailout(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // NOP when not in IonMonkey
  args.rval().setUndefined();
  return true;
}

static bool testingFunc_bailAfter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1 || !args[0].isInt32() || args[0].toInt32() < 0) {
    JS_ReportErrorASCII(
        cx, "Argument must be a positive number that fits in an int32");
    return false;
  }

#ifdef DEBUG
  if (auto* jitRuntime = cx->runtime()->jitRuntime()) {
    uint32_t bailAfter = args[0].toInt32();
    bool enableBailAfter = bailAfter > 0;
    if (jitRuntime->ionBailAfterEnabled() != enableBailAfter) {
      // Force JIT code to be recompiled with (or without) instrumentation.
      ReleaseAllJITCode(cx->gcContext());
      jitRuntime->setIonBailAfterEnabled(enableBailAfter);
    }
    jitRuntime->setIonBailAfterCounter(bailAfter);
  }
#endif

  args.rval().setUndefined();
  return true;
}

static bool testingFunc_invalidate(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // If the topmost frame is Ion/Warp, find the IonScript and invalidate it.
  FrameIter iter(cx);
  if (!iter.done() && iter.isIon()) {
    while (!iter.isPhysicalJitFrame()) {
      ++iter;
    }
    if (iter.script()->hasIonScript()) {
      js::jit::Invalidate(cx, iter.script());
    }
  }

  args.rval().setUndefined();
  return true;
}

static constexpr unsigned JitWarmupResetLimit = 20;
static_assert(JitWarmupResetLimit <=
                  unsigned(JSScript::MutableFlags::WarmupResets_MASK),
              "JitWarmupResetLimit exceeds max value");

static bool testingFunc_inJit(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!jit::IsBaselineJitEnabled(cx)) {
    return ReturnStringCopy(cx, args, "Baseline is disabled.");
  }

  // Use frame iterator to inspect caller.
  FrameIter iter(cx);

  // We may be invoked directly, not in a JS context, e.g. if inJit is added as
  // a callback on the event queue.
  if (iter.done()) {
    args.rval().setBoolean(false);
    return true;
  }

  if (iter.hasScript()) {
    // Detect repeated attempts to compile, resetting the counter if inJit
    // succeeds. Note: This script may have be inlined into its caller.
    if (iter.isJSJit()) {
      iter.script()->resetWarmUpResetCounter();
    } else if (iter.script()->getWarmUpResetCount() >= JitWarmupResetLimit) {
      return ReturnStringCopy(
          cx, args, "Compilation is being repeatedly prevented. Giving up.");
    }
  }

  // Returns true for any JIT (including WASM).
  MOZ_ASSERT_IF(iter.isJSJit(), cx->currentlyRunningInJit());
  args.rval().setBoolean(cx->currentlyRunningInJit());
  return true;
}

static bool testingFunc_inIon(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!jit::IsIonEnabled(cx)) {
    return ReturnStringCopy(cx, args, "Ion is disabled.");
  }

  // Use frame iterator to inspect caller.
  FrameIter iter(cx);

  // We may be invoked directly, not in a JS context, e.g. if inIon is added as
  // a callback on the event queue.
  if (iter.done()) {
    args.rval().setBoolean(false);
    return true;
  }

  if (iter.hasScript()) {
    // Detect repeated attempts to compile, resetting the counter if inIon
    // succeeds. Note: This script may have be inlined into its caller.
    if (iter.isIon()) {
      iter.script()->resetWarmUpResetCounter();
    } else if (!iter.script()->canIonCompile()) {
      return ReturnStringCopy(cx, args, "Unable to Ion-compile this script.");
    } else if (iter.script()->getWarmUpResetCount() >= JitWarmupResetLimit) {
      return ReturnStringCopy(
          cx, args, "Compilation is being repeatedly prevented. Giving up.");
    }
  }

  args.rval().setBoolean(iter.isIon());
  return true;
}

bool js::testingFunc_assertFloat32(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 2) {
    JS_ReportErrorASCII(cx, "Expects only 2 arguments");
    return false;
  }

  // NOP when not in IonMonkey
  args.rval().setUndefined();
  return true;
}

static bool TestingFunc_assertJitStackInvariants(JSContext* cx, unsigned argc,
                                                 Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  jit::AssertJitStackInvariants(cx);
  args.rval().setUndefined();
  return true;
}

bool js::testingFunc_assertRecoveredOnBailout(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 2) {
    JS_ReportErrorASCII(cx, "Expects only 2 arguments");
    return false;
  }

  // NOP when not in IonMonkey
  args.rval().setUndefined();
  return true;
}

static bool GetJitCompilerOptions(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject info(cx, JS_NewPlainObject(cx));
  if (!info) {
    return false;
  }

  uint32_t intValue = 0;
  RootedValue value(cx);

#define JIT_COMPILER_MATCH(key, string)                         \
  opt = JSJITCOMPILER_##key;                                    \
  if (JS_GetGlobalJitCompilerOption(cx, opt, &intValue)) {      \
    value.setInt32(intValue);                                   \
    if (!JS_SetProperty(cx, info, string, value)) return false; \
  }

  JSJitCompilerOption opt = JSJITCOMPILER_NOT_AN_OPTION;
  JIT_COMPILER_OPTIONS(JIT_COMPILER_MATCH);
#undef JIT_COMPILER_MATCH

  args.rval().setObject(*info);
  return true;
}

static bool SetIonCheckGraphCoherency(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  jit::JitOptions.checkGraphConsistency = ToBoolean(args.get(0));
  args.rval().setUndefined();
  return true;
}

// A JSObject that holds structured clone data, similar to the C++ class
// JSAutoStructuredCloneBuffer.
class CloneBufferObject : public NativeObject {
  static const JSPropertySpec props_[3];

  static const size_t DATA_SLOT = 0;
  static const size_t SYNTHETIC_SLOT = 1;
  static const size_t NUM_SLOTS = 2;

 public:
  static const JSClass class_;

  static CloneBufferObject* Create(JSContext* cx) {
    RootedObject obj(cx, JS_NewObject(cx, &class_));
    if (!obj) {
      return nullptr;
    }
    obj->as<CloneBufferObject>().setReservedSlot(DATA_SLOT,
                                                 PrivateValue(nullptr));
    obj->as<CloneBufferObject>().setReservedSlot(SYNTHETIC_SLOT,
                                                 BooleanValue(false));

    if (!JS_DefineProperties(cx, obj, props_)) {
      return nullptr;
    }

    return &obj->as<CloneBufferObject>();
  }

  static CloneBufferObject* Create(JSContext* cx,
                                   JSAutoStructuredCloneBuffer* buffer) {
    Rooted<CloneBufferObject*> obj(cx, Create(cx));
    if (!obj) {
      return nullptr;
    }
    auto data = js::MakeUnique<JSStructuredCloneData>(buffer->scope());
    if (!data) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    buffer->giveTo(data.get());
    obj->setData(data.release(), false);
    return obj;
  }

  JSStructuredCloneData* data() const {
    return static_cast<JSStructuredCloneData*>(
        getReservedSlot(DATA_SLOT).toPrivate());
  }

  bool isSynthetic() const {
    return getReservedSlot(SYNTHETIC_SLOT).toBoolean();
  }

  void setData(JSStructuredCloneData* aData, bool synthetic) {
    MOZ_ASSERT(!data());
    setReservedSlot(DATA_SLOT, PrivateValue(aData));
    setReservedSlot(SYNTHETIC_SLOT, BooleanValue(synthetic));
  }

  // Discard an owned clone buffer.
  void discard() {
    js_delete(data());
    setReservedSlot(DATA_SLOT, PrivateValue(nullptr));
  }

  static bool setCloneBuffer_impl(JSContext* cx, const CallArgs& args) {
    Rooted<CloneBufferObject*> obj(
        cx, &args.thisv().toObject().as<CloneBufferObject>());

    const char* data = nullptr;
    UniqueChars dataOwner;
    size_t nbytes;

    if (args.get(0).isObject() && args[0].toObject().is<ArrayBufferObject>()) {
      ArrayBufferObject* buffer = &args[0].toObject().as<ArrayBufferObject>();
      bool isSharedMemory;
      uint8_t* dataBytes = nullptr;
      JS::GetArrayBufferLengthAndData(buffer, &nbytes, &isSharedMemory,
                                      &dataBytes);
      MOZ_ASSERT(!isSharedMemory);
      data = reinterpret_cast<char*>(dataBytes);
    } else {
      JSString* str = JS::ToString(cx, args.get(0));
      if (!str) {
        return false;
      }
      dataOwner = JS_EncodeStringToLatin1(cx, str);
      if (!dataOwner) {
        return false;
      }
      data = dataOwner.get();
      nbytes = JS_GetStringLength(str);
    }

    if (nbytes == 0 || (nbytes % sizeof(uint64_t) != 0)) {
      JS_ReportErrorASCII(cx, "Invalid length for clonebuffer data");
      return false;
    }

    auto buf = js::MakeUnique<JSStructuredCloneData>(
        JS::StructuredCloneScope::DifferentProcess);
    if (!buf || !buf->Init(nbytes)) {
      ReportOutOfMemory(cx);
      return false;
    }

    MOZ_ALWAYS_TRUE(buf->AppendBytes(data, nbytes));
    obj->discard();
    obj->setData(buf.release(), true);

    args.rval().setUndefined();
    return true;
  }

  static bool is(HandleValue v) {
    return v.isObject() && v.toObject().is<CloneBufferObject>();
  }

  static bool setCloneBuffer(JSContext* cx, unsigned int argc, JS::Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, setCloneBuffer_impl>(cx, args);
  }

  static bool getData(JSContext* cx, Handle<CloneBufferObject*> obj,
                      JSStructuredCloneData** data) {
    if (!obj->data()) {
      *data = nullptr;
      return true;
    }

    bool hasTransferable;
    if (!JS_StructuredCloneHasTransferables(*obj->data(), &hasTransferable)) {
      return false;
    }

    if (hasTransferable) {
      JS_ReportErrorASCII(
          cx, "cannot retrieve structured clone buffer with transferables");
      return false;
    }

    *data = obj->data();
    return true;
  }

  static bool getCloneBuffer_impl(JSContext* cx, const CallArgs& args) {
    Rooted<CloneBufferObject*> obj(
        cx, &args.thisv().toObject().as<CloneBufferObject>());
    MOZ_ASSERT(args.length() == 0);

    JSStructuredCloneData* data;
    if (!getData(cx, obj, &data)) {
      return false;
    }

    if (data == nullptr) {
      args.rval().setUndefined();
      return true;
    }

    size_t size = data->Size();
    UniqueChars buffer(js_pod_malloc<char>(size));
    if (!buffer) {
      ReportOutOfMemory(cx);
      return false;
    }
    auto iter = data->Start();
    if (!data->ReadBytes(iter, buffer.get(), size)) {
      ReportOutOfMemory(cx);
      return false;
    }
    JSString* str = JS_NewStringCopyN(cx, buffer.get(), size);
    if (!str) {
      return false;
    }
    args.rval().setString(str);
    return true;
  }

  static bool getCloneBuffer(JSContext* cx, unsigned int argc, JS::Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getCloneBuffer_impl>(cx, args);
  }

  static bool getCloneBufferAsArrayBuffer_impl(JSContext* cx,
                                               const CallArgs& args) {
    Rooted<CloneBufferObject*> obj(
        cx, &args.thisv().toObject().as<CloneBufferObject>());
    MOZ_ASSERT(args.length() == 0);

    JSStructuredCloneData* data;
    if (!getData(cx, obj, &data)) {
      return false;
    }

    if (data == nullptr) {
      args.rval().setUndefined();
      return true;
    }

    size_t size = data->Size();
    UniqueChars buffer(js_pod_malloc<char>(size));
    if (!buffer) {
      ReportOutOfMemory(cx);
      return false;
    }
    auto iter = data->Start();
    if (!data->ReadBytes(iter, buffer.get(), size)) {
      ReportOutOfMemory(cx);
      return false;
    }

    JSObject* arrayBuffer =
        JS::NewArrayBufferWithContents(cx, size, std::move(buffer));
    if (!arrayBuffer) {
      return false;
    }

    args.rval().setObject(*arrayBuffer);
    return true;
  }

  static bool getCloneBufferAsArrayBuffer(JSContext* cx, unsigned int argc,
                                          JS::Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getCloneBufferAsArrayBuffer_impl>(cx, args);
  }

  static void Finalize(JS::GCContext* gcx, JSObject* obj) {
    obj->as<CloneBufferObject>().discard();
  }
};

static const JSClassOps CloneBufferObjectClassOps = {
    nullptr,                      // addProperty
    nullptr,                      // delProperty
    nullptr,                      // enumerate
    nullptr,                      // newEnumerate
    nullptr,                      // resolve
    nullptr,                      // mayResolve
    CloneBufferObject::Finalize,  // finalize
    nullptr,                      // call
    nullptr,                      // construct
    nullptr,                      // trace
};

const JSClass CloneBufferObject::class_ = {
    "CloneBuffer",
    JSCLASS_HAS_RESERVED_SLOTS(CloneBufferObject::NUM_SLOTS) |
        JSCLASS_FOREGROUND_FINALIZE,
    &CloneBufferObjectClassOps};

const JSPropertySpec CloneBufferObject::props_[] = {
    JS_PSGS("clonebuffer", getCloneBuffer, setCloneBuffer, 0),
    JS_PSGS("arraybuffer", getCloneBufferAsArrayBuffer, setCloneBuffer, 0),
    JS_PS_END};

static mozilla::Maybe<JS::StructuredCloneScope> ParseCloneScope(
    JSContext* cx, HandleString str) {
  mozilla::Maybe<JS::StructuredCloneScope> scope;

  JSLinearString* scopeStr = str->ensureLinear(cx);
  if (!scopeStr) {
    return scope;
  }

  if (StringEqualsLiteral(scopeStr, "SameProcess")) {
    scope.emplace(JS::StructuredCloneScope::SameProcess);
  } else if (StringEqualsLiteral(scopeStr, "DifferentProcess")) {
    scope.emplace(JS::StructuredCloneScope::DifferentProcess);
  } else if (StringEqualsLiteral(scopeStr, "DifferentProcessForIndexedDB")) {
    scope.emplace(JS::StructuredCloneScope::DifferentProcessForIndexedDB);
  }

  return scope;
}

// A custom object that is serializable and transferable using
// the engine's custom hooks. The callbacks log their activity
// to a JSRuntime-wide log (tagging actions with IDs to distinguish them).
class CustomSerializableObject : public NativeObject {
  static const size_t ID_SLOT = 0;
  static const size_t DETACHED_SLOT = 1;
  static const size_t BEHAVIOR_SLOT = 2;
  static const size_t NUM_SLOTS = 3;

  static constexpr size_t MAX_LOG_LEN = 100;

  // The activity log should be specific to a JSRuntime.
  struct ActivityLog {
    uint32_t buffer[MAX_LOG_LEN];
    size_t length = 0;

    static MOZ_THREAD_LOCAL(ActivityLog*) self;
    static ActivityLog* getThreadLog() {
      if (!self.initialized() || !self.get()) {
        self.infallibleInit();
        AutoEnterOOMUnsafeRegion oomUnsafe;
        self.set(js_new<ActivityLog>());
        if (!self.get()) {
          oomUnsafe.crash("allocating activity log");
        }
        if (!TlsContext.get()->runtime()->atExit(
                [](void* vpData) {
                  auto* log = static_cast<ActivityLog*>(vpData);
                  js_delete(log);
                },
                self.get())) {
          oomUnsafe.crash("atExit");
        }
      }
      return self.get();
    }

    static bool log(int32_t id, char action) {
      return getThreadLog()->logImpl(id, action);
    }

    bool logImpl(int32_t id, char action) {
      if (length + 2 > MAX_LOG_LEN) {
        return false;
      }
      buffer[length++] = id;
      buffer[length++] = uint32_t(action);
      return true;
    }
  };

 public:
  enum class Behavior {
    Nothing = 0,
    FailDuringReadTransfer = 1,
    FailDuringRead = 2
  };

  static constexpr JSClass class_ = {"CustomSerializable",
                                     JSCLASS_HAS_RESERVED_SLOTS(NUM_SLOTS)};

  static bool is(HandleValue v) {
    return v.isObject() && v.toObject().is<CustomSerializableObject>();
  }

  static CustomSerializableObject* Create(JSContext* cx, int32_t id,
                                          Behavior behavior) {
    Rooted<CustomSerializableObject*> obj(
        cx, static_cast<CustomSerializableObject*>(JS_NewObject(cx, &class_)));
    if (!obj) {
      return nullptr;
    }
    obj->setReservedSlot(ID_SLOT, Int32Value(id));
    obj->setReservedSlot(DETACHED_SLOT, BooleanValue(false));
    obj->setReservedSlot(BEHAVIOR_SLOT,
                         Int32Value(static_cast<int32_t>(behavior)));

    if (!JS_DefineProperty(cx, obj, "log", getLog, clearLog, 0)) {
      return nullptr;
    }

    return obj;
  }

 public:
  static uint32_t tag() { return JS_SCTAG_USER_MIN; }

  static bool log(int32_t id, char action) {
    return ActivityLog::log(id, action);
  }
  bool log(char action) {
    return log(getReservedSlot(ID_SLOT).toInt32(), action);
  }

  void detach() { setReservedSlot(DETACHED_SLOT, BooleanValue(true)); }
  bool isDetached() { return getReservedSlot(DETACHED_SLOT).toBoolean(); }

  uint32_t id() const { return getReservedSlot(ID_SLOT).toInt32(); }
  Behavior behavior() {
    return static_cast<Behavior>(getReservedSlot(BEHAVIOR_SLOT).toInt32());
  }

  static bool getLog(JSContext* cx, unsigned int argc, JS::Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<is, getLog_impl>(cx, args);
  }

  static bool getLog_impl(JSContext* cx, const CallArgs& args) {
    Rooted<CustomSerializableObject*> obj(
        cx, &args.thisv().toObject().as<CustomSerializableObject>());

    size_t len = ActivityLog::getThreadLog()->length;
    uint32_t* logBuffer = ActivityLog::getThreadLog()->buffer;

    Rooted<ArrayObject*> result(cx, NewDenseFullyAllocatedArray(cx, len));
    if (!result) {
      return false;
    }
    result->ensureDenseInitializedLength(0, len);

    for (size_t p = 0; p < len; p += 2) {
      int32_t id = int32_t(logBuffer[p]);
      char action = char(logBuffer[p + 1]);
      result->setDenseElement(p, Int32Value(id));
      JSString* str = JS_NewStringCopyN(cx, &action, 1);
      if (!str) {
        return false;
      }
      result->setDenseElement(p + 1, StringValue(str));
    }

    args.rval().setObject(*result);
    return true;
  }

  static bool clearLog(JSContext* cx, unsigned int argc, JS::Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.get(0).isNullOrUndefined()) {
      JS_ReportErrorASCII(cx, "log may only be assigned null/undefined");
      return false;
    }
    ActivityLog::getThreadLog()->length = 0;
    args.rval().setUndefined();
    return true;
  }

  static bool Write(JSContext* cx, JSStructuredCloneWriter* w,
                    JS::HandleObject aObj, bool* sameProcessScopeRequired,
                    void* closure) {
    Rooted<CustomSerializableObject*> obj(cx);

    if ((obj = aObj->maybeUnwrapIf<CustomSerializableObject>())) {
      obj->log('w');
      // Write a regular clone as a <tag, id> pair, followed by <0, behavior>.
      // Note that transferring will communicate the behavior via a different
      // mechanism.
      return JS_WriteUint32Pair(w, obj->tag(), obj->id()) &&
             JS_WriteUint32Pair(w, 0, static_cast<uint32_t>(obj->behavior()));
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SC_UNSUPPORTED_TYPE);
    return false;
  }

  static JSObject* Read(JSContext* cx, JSStructuredCloneReader* r,
                        const JS::CloneDataPolicy& cloneDataPolicy,
                        uint32_t tag, uint32_t id, void* closure) {
    uint32_t dummy, behaviorData;
    if (!JS_ReadUint32Pair(r, &dummy, &behaviorData)) {
      return nullptr;
    }
    if (dummy != 0 || id > INT32_MAX) {
      JS_ReportErrorASCII(cx, "out of range");
      return nullptr;
    }

    auto b = static_cast<Behavior>(behaviorData);
    Rooted<CustomSerializableObject*> obj(
        cx, Create(cx, static_cast<int32_t>(id), b));
    if (!obj) {
      return nullptr;
    }

    obj->log('r');
    if (obj->behavior() == Behavior::FailDuringRead) {
      JS_ReportErrorASCII(cx,
                          "Failed as requested in read during deserialization");
      return nullptr;
    }
    return obj;
  }

  static bool CanTransfer(JSContext* cx, JS::Handle<JSObject*> wrapped,
                          bool* sameProcessScopeRequired, void* closure) {
    Rooted<CustomSerializableObject*> obj(cx);

    if ((obj = wrapped->maybeUnwrapIf<CustomSerializableObject>())) {
      obj->log('?');
      // For now, all CustomSerializable objects are considered to be
      // transferable.
      return true;
    }

    return false;
  }

  static bool WriteTransfer(JSContext* cx, JS::Handle<JSObject*> aObj,
                            void* closure, uint32_t* tag,
                            JS::TransferableOwnership* ownership,
                            void** content, uint64_t* extraData) {
    Rooted<CustomSerializableObject*> obj(cx);

    if ((obj = aObj->maybeUnwrapIf<CustomSerializableObject>())) {
      if (obj->isDetached()) {
        JS_ReportErrorASCII(cx, "Attempted to transfer detached object");
        return false;
      }
      obj->log('W');
      *content = reinterpret_cast<void*>(obj->id());
      *extraData = static_cast<uint64_t>(obj->behavior());
      *tag = obj->tag();
      *ownership = JS::SCTAG_TMO_CUSTOM;
      obj->detach();
      return true;
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SC_NOT_TRANSFERABLE);
    return false;
  }

  static bool ReadTransfer(JSContext* cx, JSStructuredCloneReader* r,
                           const JS::CloneDataPolicy& cloneDataPolicy,
                           uint32_t tag, void* content, uint64_t extraData,
                           void* closure,
                           JS::MutableHandleObject returnObject) {
    if (tag == CustomSerializableObject::tag()) {
      int32_t id = int32_t(reinterpret_cast<uintptr_t>(content));
      Rooted<CustomSerializableObject*> obj(
          cx, CustomSerializableObject::Create(
                  cx, id, static_cast<Behavior>(extraData)));
      if (!obj) {
        return false;
      }
      obj->log('R');
      if (obj->behavior() == Behavior::FailDuringReadTransfer) {
        return false;
      }
      returnObject.set(obj);
      return true;
    }

    return false;
  }

  static void FreeTransfer(uint32_t tag, JS::TransferableOwnership ownership,
                           void* content, uint64_t extraData, void* closure) {
    CustomSerializableObject::log(uint32_t(reinterpret_cast<intptr_t>(content)),
                                  'F');
  }
};

MOZ_THREAD_LOCAL(CustomSerializableObject::ActivityLog*)
CustomSerializableObject::ActivityLog::self;

static bool MakeSerializable(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  int32_t id = 0;
  if (args.get(0).isInt32()) {
    id = args[0].toInt32();
    if (id < 0) {
      JS_ReportErrorASCII(cx, "id out of range");
      return false;
    }
  }
  CustomSerializableObject::Behavior behavior =
      CustomSerializableObject::Behavior::Nothing;
  if (args.get(1).isInt32()) {
    int32_t iv = args[1].toInt32();
    constexpr int32_t min =
        static_cast<int32_t>(CustomSerializableObject::Behavior::Nothing);
    constexpr int32_t max = static_cast<int32_t>(
        CustomSerializableObject::Behavior::FailDuringRead);
    if (iv < min || iv > max) {
      JS_ReportErrorASCII(cx, "behavior out of range");
      return false;
    }
    behavior = static_cast<CustomSerializableObject::Behavior>(iv);
  }

  JSObject* obj = CustomSerializableObject::Create(cx, id, behavior);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static JSStructuredCloneCallbacks gCloneCallbacks = {
    .read = CustomSerializableObject::Read,
    .write = CustomSerializableObject::Write,
    .reportError = nullptr,
    .readTransfer = CustomSerializableObject::ReadTransfer,
    .writeTransfer = CustomSerializableObject::WriteTransfer,
    .freeTransfer = CustomSerializableObject::FreeTransfer,
    .canTransfer = CustomSerializableObject::CanTransfer,
    .sabCloned = nullptr};

bool js::testingFunc_serialize(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (js::SupportDifferentialTesting()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee,
                          "Function unavailable in differential testing mode.");
    return false;
  }

  mozilla::Maybe<JSAutoStructuredCloneBuffer> clonebuf;
  JS::CloneDataPolicy policy;

  if (!args.get(2).isUndefined()) {
    RootedObject opts(cx, ToObject(cx, args.get(2)));
    if (!opts) {
      return false;
    }

    RootedValue v(cx);
    if (!JS_GetProperty(cx, opts, "SharedArrayBuffer", &v)) {
      return false;
    }

    if (!v.isUndefined()) {
      JSString* str = JS::ToString(cx, v);
      if (!str) {
        return false;
      }
      JSLinearString* poli = str->ensureLinear(cx);
      if (!poli) {
        return false;
      }

      if (StringEqualsLiteral(poli, "allow")) {
        policy.allowSharedMemoryObjects();
        policy.allowIntraClusterClonableSharedObjects();
      } else if (StringEqualsLiteral(poli, "deny")) {
        // default
      } else {
        JS_ReportErrorASCII(cx, "Invalid policy value for 'SharedArrayBuffer'");
        return false;
      }
    }

    if (!JS_GetProperty(cx, opts, "scope", &v)) {
      return false;
    }

    if (!v.isUndefined()) {
      RootedString str(cx, JS::ToString(cx, v));
      if (!str) {
        return false;
      }
      auto scope = ParseCloneScope(cx, str);
      if (!scope) {
        JS_ReportErrorASCII(cx, "Invalid structured clone scope");
        return false;
      }
      clonebuf.emplace(*scope, &gCloneCallbacks, nullptr);
    }
  }

  if (!clonebuf) {
    clonebuf.emplace(JS::StructuredCloneScope::SameProcess, &gCloneCallbacks,
                     nullptr);
  }

  if (!clonebuf->write(cx, args.get(0), args.get(1), policy)) {
    return false;
  }

  RootedObject obj(cx, CloneBufferObject::Create(cx, clonebuf.ptr()));
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool Deserialize(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (js::SupportDifferentialTesting()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee,
                          "Function unavailable in differential testing mode.");
    return false;
  }

  if (!args.get(0).isObject() || !args[0].toObject().is<CloneBufferObject>()) {
    JS_ReportErrorASCII(cx, "deserialize requires a clonebuffer argument");
    return false;
  }
  Rooted<CloneBufferObject*> obj(cx,
                                 &args[0].toObject().as<CloneBufferObject>());

  JS::CloneDataPolicy policy;

  JS::StructuredCloneScope scope =
      obj->isSynthetic() ? JS::StructuredCloneScope::DifferentProcess
                         : JS::StructuredCloneScope::SameProcess;
  if (args.get(1).isObject()) {
    RootedObject opts(cx, &args[1].toObject());
    if (!opts) {
      return false;
    }

    RootedValue v(cx);
    if (!JS_GetProperty(cx, opts, "SharedArrayBuffer", &v)) {
      return false;
    }

    if (!v.isUndefined()) {
      JSString* str = JS::ToString(cx, v);
      if (!str) {
        return false;
      }
      JSLinearString* poli = str->ensureLinear(cx);
      if (!poli) {
        return false;
      }

      if (StringEqualsLiteral(poli, "allow")) {
        policy.allowSharedMemoryObjects();
        policy.allowIntraClusterClonableSharedObjects();
      } else if (StringEqualsLiteral(poli, "deny")) {
        // default
      } else {
        JS_ReportErrorASCII(cx, "Invalid policy value for 'SharedArrayBuffer'");
        return false;
      }
    }

    if (!JS_GetProperty(cx, opts, "scope", &v)) {
      return false;
    }

    if (!v.isUndefined()) {
      RootedString str(cx, JS::ToString(cx, v));
      if (!str) {
        return false;
      }
      auto maybeScope = ParseCloneScope(cx, str);
      if (!maybeScope) {
        JS_ReportErrorASCII(cx, "Invalid structured clone scope");
        return false;
      }

      if (*maybeScope < scope) {
        JS_ReportErrorASCII(cx,
                            "Cannot use less restrictive scope "
                            "than the deserialized clone buffer's scope");
        return false;
      }

      scope = *maybeScope;
    }
  }

  // Clone buffer was already consumed?
  if (!obj->data()) {
    JS_ReportErrorASCII(cx,
                        "deserialize given invalid clone buffer "
                        "(transferables already consumed?)");
    return false;
  }

  bool hasTransferable;
  if (!JS_StructuredCloneHasTransferables(*obj->data(), &hasTransferable)) {
    return false;
  }

  RootedValue deserialized(cx);
  if (!JS_ReadStructuredClone(cx, *obj->data(), JS_STRUCTURED_CLONE_VERSION,
                              scope, &deserialized, policy, &gCloneCallbacks,
                              nullptr)) {
    return false;
  }
  args.rval().set(deserialized);

  // Consume any clone buffer with transferables; throw an error if it is
  // deserialized again.
  if (hasTransferable) {
    obj->discard();
  }

  return true;
}

static bool DetachArrayBuffer(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1) {
    JS_ReportErrorASCII(cx, "detachArrayBuffer() requires a single argument");
    return false;
  }

  if (!args[0].isObject()) {
    JS_ReportErrorASCII(cx, "detachArrayBuffer must be passed an object");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());
  if (!JS::DetachArrayBuffer(cx, obj)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool EnsureNonInline(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSObject*> callee(cx, &args.callee());

  if (!args.get(0).isObject()) {
    js::ReportUsageErrorASCII(cx, callee, "Single object argument required");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());
  if (!JS::EnsureNonInlineArrayBufferOrView(cx, obj)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool PinArrayBufferOrViewLength(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSObject*> callee(cx, &args.callee());

  if (!args.get(0).isObject()) {
    js::ReportUsageErrorASCII(
        cx, callee, "ArrayBuffer or ArrayBufferView argument required");
    return false;
  }
  RootedObject obj(cx, &args[0].toObject());
  if (!obj->canUnwrapAs<ArrayBufferViewObject>() &&
      !obj->canUnwrapAs<ArrayBufferObjectMaybeShared>()) {
    js::ReportUsageErrorASCII(
        cx, callee, "ArrayBuffer or ArrayBufferView argument required");
    return false;
  }

  bool pin = args.get(1).isUndefined() ? true : ToBoolean(args.get(1));

  args.rval().setBoolean(JS::PinArrayBufferOrViewLength(obj, pin));
  return true;
}

static bool JSONStringify(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedValue value(cx, args.get(0));
  RootedValue behaviorVal(cx, args.get(1));
  StringifyBehavior behavior = StringifyBehavior::Normal;
  if (behaviorVal.isString()) {
    bool matches;
#define MATCH(name)                                                           \
  if (!JS_StringEqualsLiteral(cx, behaviorVal.toString(), #name, &matches)) { \
    return false;                                                             \
  }                                                                           \
  if (matches) {                                                              \
    behavior = StringifyBehavior::name;                                       \
  }
    MATCH(Normal)
    MATCH(FastOnly)
    MATCH(SlowOnly)
    MATCH(Compare)
#undef MATCH
  }

  JSStringBuilder sb(cx);
  if (!Stringify(cx, &value, nullptr, UndefinedValue(), sb, behavior)) {
    return false;
  }

  if (!sb.empty()) {
    JSString* str = sb.finishString();
    if (!str) {
      return false;
    }
    args.rval().setString(str);
  } else {
    args.rval().setUndefined();
  }

  return true;
}

static bool HelperThreadCount(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (js::SupportDifferentialTesting()) {
    // Always return 0 to get consistent output with and without --no-threads.
    args.rval().setInt32(0);
    return true;
  }

  if (CanUseExtraThreads()) {
    args.rval().setInt32(GetHelperThreadCount());
  } else {
    args.rval().setInt32(0);
  }
  return true;
}

static bool EnableShapeConsistencyChecks(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
#ifdef DEBUG
  NativeObject::enableShapeConsistencyChecks();
#endif
  args.rval().setUndefined();
  return true;
}

// ShapeSnapshot holds information about an object's properties. This is used
// for checking object and shape changes between two points in time.
class ShapeSnapshot {
  HeapPtr<JSObject*> object_;
  HeapPtr<Shape*> shape_;
  HeapPtr<BaseShape*> baseShape_;
  ObjectFlags objectFlags_;

  GCVector<HeapPtr<Value>, 8> slots_;

  struct PropertySnapshot {
    HeapPtr<PropMap*> propMap;
    uint32_t propMapIndex;
    HeapPtr<PropertyKey> key;
    PropertyInfo prop;

    explicit PropertySnapshot(PropMap* map, uint32_t index)
        : propMap(map),
          propMapIndex(index),
          key(map->getKey(index)),
          prop(map->getPropertyInfo(index)) {}

    void trace(JSTracer* trc) {
      TraceEdge(trc, &propMap, "propMap");
      TraceEdge(trc, &key, "key");
    }

    bool operator==(const PropertySnapshot& other) const {
      return propMap == other.propMap && propMapIndex == other.propMapIndex &&
             key == other.key && prop == other.prop;
    }
    bool operator!=(const PropertySnapshot& other) const {
      return !operator==(other);
    }
  };
  GCVector<PropertySnapshot, 8> properties_;

 public:
  explicit ShapeSnapshot(JSContext* cx) : slots_(cx), properties_(cx) {}
  void checkSelf(JSContext* cx) const;
  void check(JSContext* cx, const ShapeSnapshot& other) const;
  bool init(JSObject* obj);
  void trace(JSTracer* trc);

  JSObject* object() const { return object_; }
};

// A JSObject that holds a ShapeSnapshot.
class ShapeSnapshotObject : public NativeObject {
  static constexpr size_t SnapshotSlot = 0;
  static constexpr size_t ReservedSlots = 1;

 public:
  static const JSClassOps classOps_;
  static const JSClass class_;

  bool hasSnapshot() const {
    // The snapshot may not be present yet if we GC during initialization.
    return !getReservedSlot(SnapshotSlot).isUndefined();
  }

  ShapeSnapshot& snapshot() const {
    void* ptr = getReservedSlot(SnapshotSlot).toPrivate();
    MOZ_ASSERT(ptr);
    return *static_cast<ShapeSnapshot*>(ptr);
  }

  static ShapeSnapshotObject* create(JSContext* cx, HandleObject obj);

  static void finalize(JS::GCContext* gcx, JSObject* obj) {
    if (obj->as<ShapeSnapshotObject>().hasSnapshot()) {
      js_delete(&obj->as<ShapeSnapshotObject>().snapshot());
    }
  }
  static void trace(JSTracer* trc, JSObject* obj) {
    if (obj->as<ShapeSnapshotObject>().hasSnapshot()) {
      obj->as<ShapeSnapshotObject>().snapshot().trace(trc);
    }
  }
};

/*static */ const JSClassOps ShapeSnapshotObject::classOps_ = {
    nullptr,                        // addProperty
    nullptr,                        // delProperty
    nullptr,                        // enumerate
    nullptr,                        // newEnumerate
    nullptr,                        // resolve
    nullptr,                        // mayResolve
    ShapeSnapshotObject::finalize,  // finalize
    nullptr,                        // call
    nullptr,                        // construct
    ShapeSnapshotObject::trace,     // trace
};

/*static */ const JSClass ShapeSnapshotObject::class_ = {
    "ShapeSnapshotObject",
    JSCLASS_HAS_RESERVED_SLOTS(ShapeSnapshotObject::ReservedSlots) |
        JSCLASS_BACKGROUND_FINALIZE,
    &ShapeSnapshotObject::classOps_};

bool ShapeSnapshot::init(JSObject* obj) {
  object_ = obj;
  shape_ = obj->shape();
  baseShape_ = shape_->base();
  objectFlags_ = shape_->objectFlags();

  if (obj->is<NativeObject>()) {
    NativeObject* nobj = &obj->as<NativeObject>();

    // Snapshot the slot values.
    size_t slotSpan = nobj->slotSpan();
    if (!slots_.growBy(slotSpan)) {
      return false;
    }
    for (size_t i = 0; i < slotSpan; i++) {
      slots_[i] = nobj->getSlot(i);
    }

    // Snapshot property information.
    if (uint32_t len = nobj->shape()->propMapLength(); len > 0) {
      PropMap* map = nobj->shape()->propMap();
      while (true) {
        for (uint32_t i = 0; i < len; i++) {
          if (!map->hasKey(i)) {
            continue;
          }
          if (!properties_.append(PropertySnapshot(map, i))) {
            return false;
          }
        }
        if (!map->hasPrevious()) {
          break;
        }
        map = map->asLinked()->previous();
        len = PropMap::Capacity;
      }
    }
  }

  return true;
}

void ShapeSnapshot::trace(JSTracer* trc) {
  TraceEdge(trc, &object_, "object");
  TraceEdge(trc, &shape_, "shape");
  TraceEdge(trc, &baseShape_, "baseShape");
  slots_.trace(trc);
  properties_.trace(trc);
}

void ShapeSnapshot::checkSelf(JSContext* cx) const {
  // Assertions based on a single snapshot.

  // Non-dictionary shapes must not be mutated.
  if (!shape_->isDictionary()) {
    MOZ_RELEASE_ASSERT(shape_->base() == baseShape_);
    MOZ_RELEASE_ASSERT(shape_->objectFlags() == objectFlags_);
  }

  for (const PropertySnapshot& propSnapshot : properties_) {
    PropMap* propMap = propSnapshot.propMap;
    uint32_t propMapIndex = propSnapshot.propMapIndex;
    PropertyInfo prop = propSnapshot.prop;

    // Skip if the map no longer matches the snapshotted data. This can
    // only happen for dictionary maps because they can be mutated or compacted
    // after a shape change.
    if (!propMap->hasKey(propMapIndex) ||
        PropertySnapshot(propMap, propMapIndex) != propSnapshot) {
      MOZ_RELEASE_ASSERT(propMap->isDictionary());
      MOZ_RELEASE_ASSERT(object_->shape() != shape_);
      continue;
    }

    // Ensure ObjectFlags depending on property information are set if needed.
    ObjectFlags expectedFlags = GetObjectFlagsForNewProperty(
        shape_->getObjectClass(), shape_->objectFlags(), propSnapshot.key,
        prop.flags(), cx);
    MOZ_RELEASE_ASSERT(expectedFlags == objectFlags_);

    // Accessors must have a PrivateGCThingValue(GetterSetter*) slot value.
    if (prop.isAccessorProperty()) {
      Value slotVal = slots_[prop.slot()];
      MOZ_RELEASE_ASSERT(slotVal.isPrivateGCThing());
      MOZ_RELEASE_ASSERT(slotVal.toGCThing()->is<GetterSetter>());
    }

    // Data properties must not have a PrivateGCThingValue slot value.
    if (prop.isDataProperty()) {
      Value slotVal = slots_[prop.slot()];
      MOZ_RELEASE_ASSERT(!slotVal.isPrivateGCThing());
    }
  }
}

void ShapeSnapshot::check(JSContext* cx, const ShapeSnapshot& later) const {
  checkSelf(cx);
  later.checkSelf(cx);

  if (object_ != later.object_) {
    // Snapshots are for different objects. Assert dictionary shapes aren't
    // shared.
    if (object_->is<NativeObject>()) {
      NativeObject* nobj = &object_->as<NativeObject>();
      if (nobj->inDictionaryMode()) {
        MOZ_RELEASE_ASSERT(nobj->shape() != later.shape_);
      }
    }
    return;
  }

  // We have two snapshots for the same object. Check the shape information
  // wasn't changed in invalid ways.

  // If the Shape is still the same, the object must have the same BaseShape,
  // ObjectFlags and property information.
  if (shape_ == later.shape_) {
    MOZ_RELEASE_ASSERT(objectFlags_ == later.objectFlags_);
    MOZ_RELEASE_ASSERT(baseShape_ == later.baseShape_);
    MOZ_RELEASE_ASSERT(slots_.length() == later.slots_.length());
    MOZ_RELEASE_ASSERT(properties_.length() == later.properties_.length());

    for (size_t i = 0; i < properties_.length(); i++) {
      MOZ_RELEASE_ASSERT(properties_[i] == later.properties_[i]);
      // Non-configurable accessor properties and non-configurable, non-writable
      // data properties shouldn't have had their slot mutated.
      PropertyInfo prop = properties_[i].prop;
      if (!prop.configurable()) {
        if (prop.isAccessorProperty() ||
            (prop.isDataProperty() && !prop.writable())) {
          size_t slot = prop.slot();
          MOZ_RELEASE_ASSERT(slots_[slot] == later.slots_[slot]);
        }
      }
    }
  }

  // Object flags should not be lost. The exception is the Indexed flag, it
  // can be cleared when densifying elements, so clear that flag first.
  {
    ObjectFlags flags = objectFlags_;
    ObjectFlags flagsLater = later.objectFlags_;
    flags.clearFlag(ObjectFlag::Indexed);
    flagsLater.clearFlag(ObjectFlag::Indexed);
    MOZ_RELEASE_ASSERT((flags.toRaw() & flagsLater.toRaw()) == flags.toRaw());
  }

  // If the HadGetterSetterChange flag wasn't set, all GetterSetter slots must
  // be unchanged.
  if (!later.objectFlags_.hasFlag(ObjectFlag::HadGetterSetterChange)) {
    for (size_t i = 0; i < slots_.length(); i++) {
      if (slots_[i].isPrivateGCThing() &&
          slots_[i].toGCThing()->is<GetterSetter>()) {
        MOZ_RELEASE_ASSERT(i < later.slots_.length());
        MOZ_RELEASE_ASSERT(later.slots_[i] == slots_[i]);
      }
    }
  }
}

// static
ShapeSnapshotObject* ShapeSnapshotObject::create(JSContext* cx,
                                                 HandleObject obj) {
  Rooted<UniquePtr<ShapeSnapshot>> snapshot(cx,
                                            cx->make_unique<ShapeSnapshot>(cx));
  if (!snapshot || !snapshot->init(obj)) {
    return nullptr;
  }

  auto* snapshotObj = NewObjectWithGivenProto<ShapeSnapshotObject>(cx, nullptr);
  if (!snapshotObj) {
    return nullptr;
  }
  snapshotObj->initReservedSlot(SnapshotSlot, PrivateValue(snapshot.release()));
  return snapshotObj;
}

static bool CreateShapeSnapshot(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.get(0).isObject()) {
    JS_ReportErrorASCII(cx, "createShapeSnapshot requires an object argument");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());
  auto* res = ShapeSnapshotObject::create(cx, obj);
  if (!res) {
    return false;
  }

  res->snapshot().check(cx, res->snapshot());

  args.rval().setObject(*res);
  return true;
}

static bool CheckShapeSnapshot(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.get(0).isObject() ||
      !args[0].toObject().is<ShapeSnapshotObject>()) {
    JS_ReportErrorASCII(cx, "checkShapeSnapshot requires a snapshot argument");
    return false;
  }

  // Get the object to use from the snapshot if the second argument is not an
  // object.
  RootedObject obj(cx);
  if (args.get(1).isObject()) {
    obj = &args[1].toObject();
  } else {
    auto& snapshot = args[0].toObject().as<ShapeSnapshotObject>().snapshot();
    obj = snapshot.object();
  }

  RootedObject otherSnapshot(cx, ShapeSnapshotObject::create(cx, obj));
  if (!otherSnapshot) {
    return false;
  }

  auto& snapshot1 = args[0].toObject().as<ShapeSnapshotObject>().snapshot();
  auto& snapshot2 = otherSnapshot->as<ShapeSnapshotObject>().snapshot();
  snapshot1.check(cx, snapshot2);

  args.rval().setUndefined();
  return true;
}

#if defined(DEBUG) || defined(JS_JITSPEW)
static bool DumpObject(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject obj(cx, ToObject(cx, args.get(0)));
  if (!obj) {
    return false;
  }

  DumpObject(obj);

  args.rval().setUndefined();
  return true;
}

static bool DumpValue(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.get(0).get().dump();
  args.rval().setUndefined();
  return true;
}

static bool DumpValueToString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JSSprinter out(cx);
  if (!out.init()) {
    return false;
  }
  args.get(0).get().dump(out);

  JSString* rep = out.release(cx);
  if (!rep) {
    return false;
  }

  args.rval().setString(rep);
  return true;
}
#endif

static bool SharedMemoryEnabled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(
      cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled());
  return true;
}

static bool SharedArrayRawBufferRefcount(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() != 1 || !args[0].isObject()) {
    JS_ReportErrorASCII(cx, "Expected SharedArrayBuffer object");
    return false;
  }
  RootedObject obj(cx, &args[0].toObject());
  if (!obj->is<SharedArrayBufferObject>()) {
    JS_ReportErrorASCII(cx, "Expected SharedArrayBuffer object");
    return false;
  }
  args.rval().setInt32(
      obj->as<SharedArrayBufferObject>().rawBufferObject()->refcount());
  return true;
}

#ifdef NIGHTLY_BUILD
static bool ObjectAddress(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (js::SupportDifferentialTesting()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee,
                          "Function unavailable in differential testing mode.");
    return false;
  }

  if (args.length() != 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }
  if (!args[0].isObject()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Expected object");
    return false;
  }

  void* ptr = js::UncheckedUnwrap(&args[0].toObject(), true);
  char buffer[64];
  SprintfLiteral(buffer, "%p", ptr);

  return ReturnStringCopy(cx, args, buffer);
}

static bool SharedAddress(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (js::SupportDifferentialTesting()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee,
                          "Function unavailable in differential testing mode.");
    return false;
  }

  if (args.length() != 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }
  if (!args[0].isObject()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Expected object");
    return false;
  }

  RootedObject obj(cx, CheckedUnwrapStatic(&args[0].toObject()));
  if (!obj) {
    ReportAccessDenied(cx);
    return false;
  }
  if (!obj->is<SharedArrayBufferObject>()) {
    JS_ReportErrorASCII(cx, "Argument must be a SharedArrayBuffer");
    return false;
  }
  char buffer[64];
  uint32_t nchar = SprintfLiteral(
      buffer, "%p",
      obj->as<SharedArrayBufferObject>().dataPointerShared().unwrap(
          /*safeish*/));

  JSString* str = JS_NewStringCopyN(cx, buffer, nchar);
  if (!str) {
    return false;
  }

  args.rval().setString(str);

  return true;
}
#endif

static bool HasInvalidatedTeleporting(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1 || !args[0].isObject()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Expected single object argument");
    return false;
  }

  args.rval().setBoolean(args[0].toObject().hasInvalidatedTeleporting());
  return true;
}

static bool DumpBacktrace(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  DumpBacktrace(cx);
  args.rval().setUndefined();
  return true;
}

static bool GetBacktrace(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  bool showArgs = false;
  bool showLocals = false;
  bool showThisProps = false;

  if (args.length() > 1) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "Too many arguments");
    return false;
  }

  if (args.length() == 1) {
    RootedObject cfg(cx, ToObject(cx, args[0]));
    if (!cfg) {
      return false;
    }
    RootedValue v(cx);

    if (!JS_GetProperty(cx, cfg, "args", &v)) {
      return false;
    }
    showArgs = ToBoolean(v);

    if (!JS_GetProperty(cx, cfg, "locals", &v)) {
      return false;
    }
    showLocals = ToBoolean(v);

    if (!JS_GetProperty(cx, cfg, "thisprops", &v)) {
      return false;
    }
    showThisProps = ToBoolean(v);
  }

  JS::UniqueChars buf =
      JS::FormatStackDump(cx, showArgs, showLocals, showThisProps);
  if (!buf) {
    return false;
  }

  size_t len;
  UniqueTwoByteChars ucbuf(JS::LossyUTF8CharsToNewTwoByteCharsZ(
                               cx, JS::UTF8Chars(buf.get(), strlen(buf.get())),
                               &len, js::MallocArena)
                               .get());
  if (!ucbuf) {
    return false;
  }
  JSString* str = JS_NewUCStringCopyN(cx, ucbuf.get(), len);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool ReportOutOfMemory(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JS_ReportOutOfMemory(cx);
  cx->clearPendingException();
  args.rval().setUndefined();
  return true;
}

static bool ThrowOutOfMemory(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportOutOfMemory(cx);
  return false;
}

static bool ReportLargeAllocationFailure(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  size_t bytes = JSRuntime::LARGE_ALLOCATION;
  if (args.length() >= 1) {
    if (!args[0].isInt32()) {
      RootedObject callee(cx, &args.callee());
      ReportUsageErrorASCII(cx, callee,
                            "First argument must be an integer if specified.");
      return false;
    }
    bytes = args[0].toInt32();
  }

  void* buf = cx->runtime()->onOutOfMemoryCanGC(AllocFunction::Malloc,
                                                js::MallocArena, bytes);

  js_free(buf);
  args.rval().setUndefined();
  return true;
}

namespace heaptools {

using EdgeName = UniqueTwoByteChars;

// An edge to a node from its predecessor in a path through the graph.
class BackEdge {
  // The node from which this edge starts.
  JS::ubi::Node predecessor_;

  // The name of this edge.
  EdgeName name_;

 public:
  BackEdge() : name_(nullptr) {}
  // Construct an initialized back edge, taking ownership of |name|.
  BackEdge(JS::ubi::Node predecessor, EdgeName name)
      : predecessor_(predecessor), name_(std::move(name)) {}
  BackEdge(BackEdge&& rhs)
      : predecessor_(rhs.predecessor_), name_(std::move(rhs.name_)) {}
  BackEdge& operator=(BackEdge&& rhs) {
    MOZ_ASSERT(&rhs != this);
    this->~BackEdge();
    new (this) BackEdge(std::move(rhs));
    return *this;
  }

  EdgeName forgetName() { return std::move(name_); }
  JS::ubi::Node predecessor() const { return predecessor_; }

 private:
  // No copy constructor or copying assignment.
  BackEdge(const BackEdge&) = delete;
  BackEdge& operator=(const BackEdge&) = delete;
};

// A path-finding handler class for use with JS::ubi::BreadthFirst.
struct FindPathHandler {
  using NodeData = BackEdge;
  using Traversal = JS::ubi::BreadthFirst<FindPathHandler>;

  FindPathHandler(JSContext* cx, JS::ubi::Node start, JS::ubi::Node target,
                  MutableHandle<GCVector<Value>> nodes, Vector<EdgeName>& edges)
      : cx(cx),
        start(start),
        target(target),
        foundPath(false),
        nodes(nodes),
        edges(edges) {}

  bool operator()(Traversal& traversal, JS::ubi::Node origin,
                  const JS::ubi::Edge& edge, BackEdge* backEdge, bool first) {
    // We take care of each node the first time we visit it, so there's
    // nothing to be done on subsequent visits.
    if (!first) {
      return true;
    }

    // Record how we reached this node. This is the last edge on a
    // shortest path to this node.
    EdgeName edgeName =
        DuplicateStringToArena(js::StringBufferArena, cx, edge.name.get());
    if (!edgeName) {
      return false;
    }
    *backEdge = BackEdge(origin, std::move(edgeName));

    // Have we reached our final target node?
    if (edge.referent == target) {
      // Record the path that got us here, which must be a shortest path.
      if (!recordPath(traversal, backEdge)) {
        return false;
      }
      foundPath = true;
      traversal.stop();
    }

    return true;
  }

  // We've found a path to our target. Walk the backlinks to produce the
  // (reversed) path, saving the path in |nodes| and |edges|. |nodes| is
  // rooted, so it can hold the path's nodes as we leave the scope of
  // the AutoCheckCannotGC. Note that nodes are added to |visited| after we
  // return from operator() so we have to pass the target BackEdge* to this
  // function.
  bool recordPath(Traversal& traversal, BackEdge* targetBackEdge) {
    JS::ubi::Node here = target;

    do {
      BackEdge* backEdge = targetBackEdge;
      if (here != target) {
        Traversal::NodeMap::Ptr p = traversal.visited.lookup(here);
        MOZ_ASSERT(p);
        backEdge = &p->value();
      }
      JS::ubi::Node predecessor = backEdge->predecessor();
      if (!nodes.append(predecessor.exposeToJS()) ||
          !edges.append(backEdge->forgetName())) {
        return false;
      }
      here = predecessor;
    } while (here != start);

    return true;
  }

  JSContext* cx;

  // The node we're starting from.
  JS::ubi::Node start;

  // The node we're looking for.
  JS::ubi::Node target;

  // True if we found a path to target, false if we didn't.
  bool foundPath;

  // The nodes and edges of the path --- should we find one. The path is
  // stored in reverse order, because that's how it's easiest for us to
  // construct it:
  // - edges[i] is the name of the edge from nodes[i] to nodes[i-1].
  // - edges[0] is the name of the edge from nodes[0] to the target.
  // - The last node, nodes[n-1], is the start node.
  MutableHandle<GCVector<Value>> nodes;
  Vector<EdgeName>& edges;
};

}  // namespace heaptools

static bool FindPath(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "findPath", 2)) {
    return false;
  }

  // We don't ToString non-objects given as 'start' or 'target', because this
  // test is all about object identity, and ToString doesn't preserve that.
  // Non-GCThing endpoints don't make much sense.
  if (!args[0].isObject() && !args[0].isString() && !args[0].isSymbol()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[0],
                     nullptr, "not an object, string, or symbol");
    return false;
  }

  if (!args[1].isObject() && !args[1].isString() && !args[1].isSymbol()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[0],
                     nullptr, "not an object, string, or symbol");
    return false;
  }

  Rooted<GCVector<Value>> nodes(cx, GCVector<Value>(cx));
  Vector<heaptools::EdgeName> edges(cx);

  {
    // We can't tolerate the GC moving things around while we're searching
    // the heap. Check that nothing we do causes a GC.
    JS::AutoCheckCannotGC autoCannotGC;

    JS::ubi::Node start(args[0]), target(args[1]);

    heaptools::FindPathHandler handler(cx, start, target, &nodes, edges);
    heaptools::FindPathHandler::Traversal traversal(cx, handler, autoCannotGC);
    if (!traversal.addStart(start)) {
      ReportOutOfMemory(cx);
      return false;
    }

    if (!traversal.traverse()) {
      if (!cx->isExceptionPending()) {
        ReportOutOfMemory(cx);
      }
      return false;
    }

    if (!handler.foundPath) {
      // We didn't find any paths from the start to the target.
      args.rval().setUndefined();
      return true;
    }
  }

  // |nodes| and |edges| contain the path from |start| to |target|, reversed.
  // Construct a JavaScript array describing the path from the start to the
  // target. Each element has the form:
  //
  //   {
  //     node: <object or string or symbol>,
  //     edge: <string describing outgoing edge from node>
  //   }
  //
  // or, if the node is some internal thing that isn't a proper JavaScript
  // value:
  //
  //   { node: undefined, edge: <string> }
  size_t length = nodes.length();
  Rooted<ArrayObject*> result(cx, NewDenseFullyAllocatedArray(cx, length));
  if (!result) {
    return false;
  }
  result->ensureDenseInitializedLength(0, length);

  // Walk |nodes| and |edges| in the stored order, and construct the result
  // array in start-to-target order.
  for (size_t i = 0; i < length; i++) {
    // Build an object describing the node and edge.
    RootedObject obj(cx, NewPlainObject(cx));
    if (!obj) {
      return false;
    }

    // Only define the "node" property if we're not fuzzing, to prevent the
    // fuzzers from messing with internal objects that we don't want to expose
    // to arbitrary JS.
    if (!fuzzingSafe) {
      RootedValue wrapped(cx, nodes[i]);
      if (!cx->compartment()->wrap(cx, &wrapped)) {
        return false;
      }

      if (!JS_DefineProperty(cx, obj, "node", wrapped, JSPROP_ENUMERATE)) {
        return false;
      }
    }

    heaptools::EdgeName edgeName = std::move(edges[i]);

    size_t edgeNameLength = js_strlen(edgeName.get());
    RootedString edgeStr(
        cx, NewString<CanGC>(cx, std::move(edgeName), edgeNameLength));
    if (!edgeStr) {
      return false;
    }

    if (!JS_DefineProperty(cx, obj, "edge", edgeStr, JSPROP_ENUMERATE)) {
      return false;
    }

    result->setDenseElement(length - i - 1, ObjectValue(*obj));
  }

  args.rval().setObject(*result);
  return true;
}

static bool ShortestPaths(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "shortestPaths", 1)) {
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<ArrayObject>()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[0],
                     nullptr, "not an array object");
    return false;
  }

  Rooted<ArrayObject*> objs(cx, &args[0].toObject().as<ArrayObject>());

  RootedValue start(cx, NullValue());
  int32_t maxNumPaths = 3;

  if (!args.get(1).isUndefined()) {
    if (!args[1].isObject()) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[1],
                       nullptr, "not an options object");
      return false;
    }

    RootedObject options(cx, &args[1].toObject());
    bool exists;
    if (!JS_HasProperty(cx, options, "start", &exists)) {
      return false;
    }
    if (exists) {
      if (!JS_GetProperty(cx, options, "start", &start)) {
        return false;
      }

      // Non-GCThing endpoints don't make much sense.
      if (!start.isGCThing()) {
        ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, start,
                         nullptr, "not a GC thing");
        return false;
      }
    }

    RootedValue v(cx, Int32Value(maxNumPaths));
    if (!JS_HasProperty(cx, options, "maxNumPaths", &exists)) {
      return false;
    }
    if (exists) {
      if (!JS_GetProperty(cx, options, "maxNumPaths", &v)) {
        return false;
      }
      if (!JS::ToInt32(cx, v, &maxNumPaths)) {
        return false;
      }
    }
    if (maxNumPaths <= 0) {
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, v,
                       nullptr, "not greater than 0");
      return false;
    }
  }

  // Ensure we have at least one target.
  size_t length = objs->getDenseInitializedLength();
  if (length == 0) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[0],
                     nullptr,
                     "not a dense array object with one or more elements");
    return false;
  }

  for (size_t i = 0; i < length; i++) {
    RootedValue el(cx, objs->getDenseElement(i));
    if (!el.isGCThing()) {
      JS_ReportErrorASCII(cx, "Each target must be a GC thing");
      return false;
    }
  }

  // We accumulate the results into a GC-stable form, due to the fact that the
  // JS::ubi::ShortestPaths lifetime (when operating on the live heap graph)
  // is bounded within an AutoCheckCannotGC.
  Rooted<GCVector<GCVector<GCVector<Value>>>> values(
      cx, GCVector<GCVector<GCVector<Value>>>(cx));
  Vector<Vector<Vector<JS::ubi::EdgeName>>> names(cx);

  {
    JS::ubi::Node root;

    JS::ubi::RootList rootList(cx, true);
    if (start.isNull()) {
      auto [ok, nogc] = rootList.init();
      (void)nogc;  // Old compilers get anxious about nogc being unused.
      if (!ok) {
        ReportOutOfMemory(cx);
        return false;
      }
      root = JS::ubi::Node(&rootList);
    } else {
      root = JS::ubi::Node(start);
    }
    JS::AutoCheckCannotGC noGC(cx);

    JS::ubi::NodeSet targets;

    for (size_t i = 0; i < length; i++) {
      RootedValue val(cx, objs->getDenseElement(i));
      JS::ubi::Node node(val);
      if (!targets.put(node)) {
        ReportOutOfMemory(cx);
        return false;
      }
    }

    auto maybeShortestPaths = JS::ubi::ShortestPaths::Create(
        cx, noGC, maxNumPaths, root, std::move(targets));
    if (maybeShortestPaths.isNothing()) {
      ReportOutOfMemory(cx);
      return false;
    }
    auto& shortestPaths = *maybeShortestPaths;

    for (size_t i = 0; i < length; i++) {
      if (!values.append(GCVector<GCVector<Value>>(cx)) ||
          !names.append(Vector<Vector<JS::ubi::EdgeName>>(cx))) {
        return false;
      }

      RootedValue val(cx, objs->getDenseElement(i));
      JS::ubi::Node target(val);

      bool ok = shortestPaths.forEachPath(target, [&](JS::ubi::Path& path) {
        Rooted<GCVector<Value>> pathVals(cx, GCVector<Value>(cx));
        Vector<JS::ubi::EdgeName> pathNames(cx);

        for (auto& part : path) {
          if (!pathVals.append(part->predecessor().exposeToJS()) ||
              !pathNames.append(std::move(part->name()))) {
            return false;
          }
        }

        return values.back().append(std::move(pathVals.get())) &&
               names.back().append(std::move(pathNames));
      });

      if (!ok) {
        return false;
      }
    }
  }

  MOZ_ASSERT(values.length() == names.length());
  MOZ_ASSERT(values.length() == length);

  Rooted<ArrayObject*> results(cx, NewDenseFullyAllocatedArray(cx, length));
  if (!results) {
    return false;
  }
  results->ensureDenseInitializedLength(0, length);

  for (size_t i = 0; i < length; i++) {
    size_t numPaths = values[i].length();
    MOZ_ASSERT(names[i].length() == numPaths);

    Rooted<ArrayObject*> pathsArray(cx,
                                    NewDenseFullyAllocatedArray(cx, numPaths));
    if (!pathsArray) {
      return false;
    }
    pathsArray->ensureDenseInitializedLength(0, numPaths);

    for (size_t j = 0; j < numPaths; j++) {
      size_t pathLength = values[i][j].length();
      MOZ_ASSERT(names[i][j].length() == pathLength);

      Rooted<ArrayObject*> path(cx,
                                NewDenseFullyAllocatedArray(cx, pathLength));
      if (!path) {
        return false;
      }
      path->ensureDenseInitializedLength(0, pathLength);

      for (size_t k = 0; k < pathLength; k++) {
        Rooted<PlainObject*> part(cx, NewPlainObject(cx));
        if (!part) {
          return false;
        }

        RootedValue predecessor(cx, values[i][j][k]);
        if (!cx->compartment()->wrap(cx, &predecessor) ||
            !JS_DefineProperty(cx, part, "predecessor", predecessor,
                               JSPROP_ENUMERATE)) {
          return false;
        }

        if (names[i][j][k]) {
          RootedString edge(cx,
                            NewStringCopyZ<CanGC>(cx, names[i][j][k].get()));
          if (!edge ||
              !JS_DefineProperty(cx, part, "edge", edge, JSPROP_ENUMERATE)) {
            return false;
          }
        }

        path->setDenseElement(k, ObjectValue(*part));
      }

      pathsArray->setDenseElement(j, ObjectValue(*path));
    }

    results->setDenseElement(i, ObjectValue(*pathsArray));
  }

  args.rval().setObject(*results);
  return true;
}

static bool EvalReturningScope(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "evalReturningScope", 1)) {
    return false;
  }

  RootedString str(cx, ToString(cx, args[0]));
  if (!str) {
    return false;
  }

  JS::AutoFilename filename;
  uint32_t lineno;

  JS::DescribeScriptedCaller(cx, &filename, &lineno);

  // CompileOption should be created in the target global's realm.
  RootedObject global(cx);
  Maybe<JS::CompileOptions> maybeOptions;
  if (args.hasDefined(1)) {
    global = ToObject(cx, args[1]);
    if (!global) {
      return false;
    }

    global = CheckedUnwrapDynamic(global, cx, /* stopAtWindowProxy = */ false);
    if (!global) {
      JS_ReportErrorASCII(cx, "Permission denied to access global");
      return false;
    }
    if (!global->is<GlobalObject>()) {
      JS_ReportErrorASCII(cx, "Argument must be a global object");
      return false;
    }

    JSAutoRealm ar(cx, global);
    maybeOptions.emplace(cx);
  } else {
    global = JS::CurrentGlobalOrNull(cx);
    maybeOptions.emplace(cx);
  }

  CompileOptions& options = maybeOptions.ref();
  options.setFileAndLine(filename.get(), lineno);
  options.setNoScriptRval(true);
  options.setNonSyntacticScope(true);

  AutoStableStringChars linearChars(cx);
  if (!linearChars.initTwoByte(cx, str)) {
    return false;
  }
  JS::SourceText<char16_t> srcBuf;
  if (!srcBuf.initMaybeBorrowed(cx, linearChars)) {
    return false;
  }

  RootedObject varObj(cx);

  {
    // ExecuteInFrameScriptEnvironment requires the script be in the same
    // realm as the global. The script compilation should be done after
    // switching globals.
    AutoRealm ar(cx, global);

    RootedScript script(cx, JS::Compile(cx, options, srcBuf));
    if (!script) {
      return false;
    }

    JS::RootedObject obj(cx, JS_NewPlainObject(cx));
    if (!obj) {
      return false;
    }

    RootedObject lexicalScope(cx);
    if (!js::ExecuteInFrameScriptEnvironment(cx, obj, script, &lexicalScope)) {
      return false;
    }

    varObj = lexicalScope->enclosingEnvironment()->enclosingEnvironment();
    MOZ_ASSERT(varObj->is<NonSyntacticVariablesObject>());
  }

  RootedValue varObjVal(cx, ObjectValue(*varObj));
  if (!cx->compartment()->wrap(cx, &varObjVal)) {
    return false;
  }

  args.rval().set(varObjVal);
  return true;
}

static bool ByteSize(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  mozilla::MallocSizeOf mallocSizeOf = cx->runtime()->debuggerMallocSizeOf;

  {
    // We can't tolerate the GC moving things around while we're using a
    // ubi::Node. Check that nothing we do causes a GC.
    JS::AutoCheckCannotGC autoCannotGC;

    JS::ubi::Node node = args.get(0);
    if (node) {
      args.rval().setNumber(uint32_t(node.size(mallocSizeOf)));
    } else {
      args.rval().setUndefined();
    }
  }
  return true;
}

static bool ByteSizeOfScript(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "byteSizeOfScript", 1)) {
    return false;
  }
  if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "Argument must be a Function object");
    return false;
  }

  RootedFunction fun(cx, &args[0].toObject().as<JSFunction>());
  if (fun->isNativeFun()) {
    JS_ReportErrorASCII(cx, "Argument must be a scripted function");
    return false;
  }

  RootedScript script(cx, JSFunction::getOrCreateScript(cx, fun));
  if (!script) {
    return false;
  }

  mozilla::MallocSizeOf mallocSizeOf = cx->runtime()->debuggerMallocSizeOf;

  {
    // We can't tolerate the GC moving things around while we're using a
    // ubi::Node. Check that nothing we do causes a GC.
    JS::AutoCheckCannotGC autoCannotGC;

    JS::ubi::Node node = script;
    if (node) {
      args.rval().setNumber(uint32_t(node.size(mallocSizeOf)));
    } else {
      args.rval().setUndefined();
    }
  }
  return true;
}

static bool SetImmutablePrototype(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.get(0).isObject()) {
    JS_ReportErrorASCII(cx, "setImmutablePrototype: object expected");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());

  bool succeeded;
  if (!js::SetImmutablePrototype(cx, obj, &succeeded)) {
    return false;
  }

  args.rval().setBoolean(succeeded);
  return true;
}

#if defined(DEBUG) || defined(JS_JITSPEW)
static bool DumpStringRepresentation(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(cx, ToString(cx, args.get(0)));
  if (!str) {
    return false;
  }

  Fprinter out(stderr);
  str->dumpRepresentation(out);

  args.rval().setUndefined();
  return true;
}

static bool GetStringRepresentation(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(cx, ToString(cx, args.get(0)));
  if (!str) {
    return false;
  }

  JSSprinter out(cx);
  if (!out.init()) {
    return false;
  }
  str->dumpRepresentation(out);

  JSString* rep = out.release(cx);
  if (!rep) {
    return false;
  }

  args.rval().setString(rep);
  return true;
}
#endif

static bool ParseCompileOptionsForModule(JSContext* cx,
                                         JS::CompileOptions& options,
                                         JS::Handle<JSObject*> opts,
                                         bool& isModule) {
  JS::Rooted<JS::Value> v(cx);

  if (!JS_GetProperty(cx, opts, "module", &v)) {
    return false;
  }
  if (!v.isUndefined() && JS::ToBoolean(v)) {
    options.setModule();
    isModule = true;

    if (!ValidateModuleCompileOptions(cx, options)) {
      return false;
    }
  } else {
    isModule = false;
  }

  return true;
}

static bool ParseCompileOptionsForInstantiate(JSContext* cx,
                                              JS::CompileOptions& options,
                                              JS::Handle<JSObject*> opts,
                                              bool& prepareForInstantiate) {
  JS::Rooted<JS::Value> v(cx);

  if (!JS_GetProperty(cx, opts, "prepareForInstantiate", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    prepareForInstantiate = JS::ToBoolean(v);
  } else {
    prepareForInstantiate = false;
  }

  return true;
}

static bool CompileToStencil(JSContext* cx, uint32_t argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "compileToStencil", 1)) {
    return false;
  }
  if (!args[0].isString()) {
    const char* typeName = InformalValueTypeName(args[0]);
    JS_ReportErrorASCII(cx, "expected string to parse, got %s", typeName);
    return false;
  }

  RootedString src(cx, ToString<CanGC>(cx, args[0]));
  if (!src) {
    return false;
  }

  /* Linearize the string to obtain a char16_t* range. */
  AutoStableStringChars linearChars(cx);
  if (!linearChars.initTwoByte(cx, src)) {
    return false;
  }
  JS::SourceText<char16_t> srcBuf;
  if (!srcBuf.initMaybeBorrowed(cx, linearChars)) {
    return false;
  }

  CompileOptions options(cx);
  options.setFile("<compileToStencil>");

  RootedString displayURL(cx);
  RootedString sourceMapURL(cx);
  UniqueChars fileNameBytes;
  bool isModule = false;
  bool prepareForInstantiate = false;
  if (args.length() == 2) {
    if (!args[1].isObject()) {
      JS_ReportErrorASCII(
          cx, "compileToStencil: The 2nd argument must be an object");
      return false;
    }

    RootedObject opts(cx, &args[1].toObject());

    if (!js::ParseCompileOptions(cx, options, opts, &fileNameBytes)) {
      return false;
    }
    if (!ParseCompileOptionsForModule(cx, options, opts, isModule)) {
      return false;
    }
    if (!ParseCompileOptionsForInstantiate(cx, options, opts,
                                           prepareForInstantiate)) {
      return false;
    }
    if (!js::ParseSourceOptions(cx, opts, &displayURL, &sourceMapURL)) {
      return false;
    }
  }

  AutoReportFrontendContext fc(cx);
  RefPtr<JS::Stencil> stencil;
  if (isModule) {
    stencil = JS::CompileModuleScriptToStencil(&fc, options, srcBuf);
  } else {
    stencil = JS::CompileGlobalScriptToStencil(&fc, options, srcBuf);
  }
  if (!stencil) {
    return false;
  }

  if (!SetSourceOptions(cx, &fc, stencil->source, displayURL, sourceMapURL)) {
    return false;
  }

  JS::InstantiationStorage storage;
  if (prepareForInstantiate) {
    if (!JS::PrepareForInstantiate(&fc, *stencil, storage)) {
      return false;
    }
  }

  Rooted<js::StencilObject*> stencilObj(
      cx, js::StencilObject::create(cx, std::move(stencil)));
  if (!stencilObj) {
    return false;
  }

  args.rval().setObject(*stencilObj);
  return true;
}

static bool EvalStencil(JSContext* cx, uint32_t argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "evalStencil", 1)) {
    return false;
  }

  /* Prepare the input byte array. */
  if (!args[0].isObject()) {
    JS_ReportErrorASCII(cx, "evalStencil: Stencil object expected");
    return false;
  }
  Rooted<js::StencilObject*> stencilObj(
      cx, args[0].toObject().maybeUnwrapIf<js::StencilObject>());
  if (!stencilObj) {
    JS_ReportErrorASCII(cx, "evalStencil: Stencil object expected");
    return false;
  }

  if (stencilObj->stencil()->isModule()) {
    JS_ReportErrorASCII(cx,
                        "evalStencil: Module stencil cannot be evaluated. Use "
                        "instantiateModuleStencil instead");
    return false;
  }

  CompileOptions options(cx);
  UniqueChars fileNameBytes;
  Rooted<JS::Value> privateValue(cx);
  Rooted<JSString*> elementAttributeName(cx);
  if (args.length() == 2) {
    if (!args[1].isObject()) {
      JS_ReportErrorASCII(cx,
                          "evalStencil: The 2nd argument must be an object");
      return false;
    }

    RootedObject opts(cx, &args[1].toObject());

    if (!js::ParseCompileOptions(cx, options, opts, &fileNameBytes)) {
      return false;
    }
    if (!js::ParseDebugMetadata(cx, opts, &privateValue,
                                &elementAttributeName)) {
      return false;
    }
  }

  bool useDebugMetadata = !privateValue.isUndefined() || elementAttributeName;

  JS::InstantiateOptions instantiateOptions(options);
  if (useDebugMetadata) {
    instantiateOptions.hideScriptFromDebugger = true;
  }

  if (!js::ValidateLazinessOfStencilAndGlobal(cx, *stencilObj->stencil())) {
    return false;
  }

  RootedScript script(cx, JS::InstantiateGlobalStencil(cx, instantiateOptions,
                                                       stencilObj->stencil()));
  if (!script) {
    return false;
  }

  if (useDebugMetadata) {
    instantiateOptions.hideScriptFromDebugger = false;
    if (!JS::UpdateDebugMetadata(cx, script, instantiateOptions, privateValue,
                                 elementAttributeName, nullptr, nullptr)) {
      return false;
    }
  }

  RootedValue retVal(cx);
  if (!JS_ExecuteScript(cx, script, &retVal)) {
    return false;
  }

  args.rval().set(retVal);
  return true;
}

static bool CompileToStencilXDR(JSContext* cx, uint32_t argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "compileToStencilXDR", 1)) {
    return false;
  }

  RootedString src(cx, ToString<CanGC>(cx, args[0]));
  if (!src) {
    return false;
  }

  /* Linearize the string to obtain a char16_t* range. */
  AutoStableStringChars linearChars(cx);
  if (!linearChars.initTwoByte(cx, src)) {
    return false;
  }
  JS::SourceText<char16_t> srcBuf;
  if (!srcBuf.initMaybeBorrowed(cx, linearChars)) {
    return false;
  }

  CompileOptions options(cx);
  options.setFile("<compileToStencilXDR>");

  RootedString displayURL(cx);
  RootedString sourceMapURL(cx);
  UniqueChars fileNameBytes;
  bool isModule = false;
  if (args.length() == 2) {
    if (!args[1].isObject()) {
      JS_ReportErrorASCII(
          cx, "compileToStencilXDR: The 2nd argument must be an object");
      return false;
    }

    RootedObject opts(cx, &args[1].toObject());

    if (!js::ParseCompileOptions(cx, options, opts, &fileNameBytes)) {
      return false;
    }
    if (!ParseCompileOptionsForModule(cx, options, opts, isModule)) {
      return false;
    }
    if (!js::ParseSourceOptions(cx, opts, &displayURL, &sourceMapURL)) {
      return false;
    }
  }

  /* Compile the script text to stencil. */
  AutoReportFrontendContext fc(cx);
  frontend::NoScopeBindingCache scopeCache;
  Rooted<frontend::CompilationInput> input(cx,
                                           frontend::CompilationInput(options));
  UniquePtr<frontend::ExtensibleCompilationStencil> stencil;
  if (isModule) {
    stencil = frontend::ParseModuleToExtensibleStencil(
        cx, &fc, cx->tempLifoAlloc(), input.get(), &scopeCache, srcBuf);
  } else {
    stencil = frontend::CompileGlobalScriptToExtensibleStencil(
        cx, &fc, input.get(), &scopeCache, srcBuf, ScopeKind::Global);
  }
  if (!stencil) {
    return false;
  }

  if (!SetSourceOptions(cx, &fc, stencil->source, displayURL, sourceMapURL)) {
    return false;
  }

  /* Serialize the stencil to XDR. */
  JS::TranscodeBuffer xdrBytes;
  {
    frontend::BorrowingCompilationStencil borrowingStencil(*stencil);
    bool succeeded = false;
    if (!borrowingStencil.serializeStencils(cx, input.get(), xdrBytes,
                                            &succeeded)) {
      return false;
    }
    if (!succeeded) {
      fc.clearAutoReport();
      JS_ReportErrorASCII(cx, "Encoding failure");
      return false;
    }
  }

  Rooted<StencilXDRBufferObject*> xdrObj(
      cx,
      StencilXDRBufferObject::create(cx, xdrBytes.begin(), xdrBytes.length()));
  if (!xdrObj) {
    return false;
  }

  args.rval().setObject(*xdrObj);
  return true;
}

static bool EvalStencilXDR(JSContext* cx, uint32_t argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "evalStencilXDR", 1)) {
    return false;
  }

  /* Prepare the input byte array. */
  if (!args[0].isObject()) {
    JS_ReportErrorASCII(cx, "evalStencilXDR: Stencil XDR object expected");
    return false;
  }
  Rooted<StencilXDRBufferObject*> xdrObj(
      cx, args[0].toObject().maybeUnwrapIf<StencilXDRBufferObject>());
  if (!xdrObj) {
    JS_ReportErrorASCII(cx, "evalStencilXDR: Stencil XDR object expected");
    return false;
  }
  MOZ_ASSERT(xdrObj->hasBuffer());

  CompileOptions options(cx);
  UniqueChars fileNameBytes;
  Rooted<JS::Value> privateValue(cx);
  Rooted<JSString*> elementAttributeName(cx);
  if (args.length() == 2) {
    if (!args[1].isObject()) {
      JS_ReportErrorASCII(cx,
                          "evalStencilXDR: The 2nd argument must be an object");
      return false;
    }

    RootedObject opts(cx, &args[1].toObject());

    if (!js::ParseCompileOptions(cx, options, opts, &fileNameBytes)) {
      return false;
    }
    if (!js::ParseDebugMetadata(cx, opts, &privateValue,
                                &elementAttributeName)) {
      return false;
    }
  }

  /* Prepare the CompilationStencil for decoding. */
  AutoReportFrontendContext fc(cx);
  frontend::CompilationStencil stencil(nullptr);

  /* Deserialize the stencil from XDR. */
  JS::TranscodeRange xdrRange(xdrObj->buffer(), xdrObj->bufferLength());
  bool succeeded = false;
  if (!stencil.deserializeStencils(&fc, options, xdrRange, &succeeded)) {
    return false;
  }
  if (!succeeded) {
    fc.clearAutoReport();
    JS_ReportErrorASCII(cx, "Decoding failure");
    return false;
  }

  if (stencil.isModule()) {
    fc.clearAutoReport();
    JS_ReportErrorASCII(cx,
                        "evalStencilXDR: Module stencil cannot be evaluated. "
                        "Use instantiateModuleStencilXDR instead");
    return false;
  }

  if (!js::ValidateLazinessOfStencilAndGlobal(cx, stencil)) {
    return false;
  }

  bool useDebugMetadata = !privateValue.isUndefined() || elementAttributeName;

  JS::InstantiateOptions instantiateOptions(options);
  if (useDebugMetadata) {
    instantiateOptions.hideScriptFromDebugger = true;
  }
  RootedScript script(
      cx, JS::InstantiateGlobalStencil(cx, instantiateOptions, &stencil));
  if (!script) {
    return false;
  }

  if (useDebugMetadata) {
    instantiateOptions.hideScriptFromDebugger = false;
    if (!JS::UpdateDebugMetadata(cx, script, instantiateOptions, privateValue,
                                 elementAttributeName, nullptr, nullptr)) {
      return false;
    }
  }

  RootedValue retVal(cx);
  if (!JS_ExecuteScript(cx, script, &retVal)) {
    return false;
  }

  args.rval().set(retVal);
  return true;
}

static bool GetExceptionInfo(JSContext* cx, uint32_t argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "getExceptionInfo", 1)) {
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
    JS_ReportErrorASCII(cx, "getExceptionInfo: expected function argument");
    return false;
  }

  RootedValue rval(cx);
  if (JS_CallFunctionValue(cx, nullptr, args[0], JS::HandleValueArray::empty(),
                           &rval)) {
    // Function didn't throw.
    args.rval().setNull();
    return true;
  }

  // We currently don't support interrupts or forced returns.
  if (!cx->isExceptionPending()) {
    JS_ReportErrorASCII(cx, "getExceptionInfo: unsupported exception status");
    return false;
  }

  RootedValue excVal(cx);
  Rooted<SavedFrame*> stack(cx);
  if (!GetAndClearExceptionAndStack(cx, &excVal, &stack)) {
    return false;
  }

  RootedValue stackVal(cx);
  if (stack) {
    RootedString stackString(cx);
    if (!BuildStackString(cx, cx->realm()->principals(), stack, &stackString)) {
      return false;
    }
    stackVal.setString(stackString);
  } else {
    stackVal.setNull();
  }

  RootedObject obj(cx, NewPlainObject(cx));
  if (!obj) {
    return false;
  }

  if (!JS_DefineProperty(cx, obj, "exception", excVal, JSPROP_ENUMERATE)) {
    return false;
  }

  if (!JS_DefineProperty(cx, obj, "stack", stackVal, JSPROP_ENUMERATE)) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

class AllocationMarkerObject : public NativeObject {
 public:
  static const JSClass class_;
};

const JSClass AllocationMarkerObject::class_ = {"AllocationMarker"};

static bool AllocationMarker(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  bool allocateInsideNursery = true;
  if (args.length() > 0 && args[0].isObject()) {
    RootedObject options(cx, &args[0].toObject());

    RootedValue nurseryVal(cx);
    if (!JS_GetProperty(cx, options, "nursery", &nurseryVal)) {
      return false;
    }
    allocateInsideNursery = ToBoolean(nurseryVal);
  }

  JSObject* obj =
      allocateInsideNursery
          ? NewObjectWithGivenProto<AllocationMarkerObject>(cx, nullptr)
          : NewTenuredObjectWithGivenProto<AllocationMarkerObject>(cx, nullptr);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

namespace gcCallback {

struct MajorGC {
  int32_t depth;
  int32_t phases;
};

static void majorGC(JSContext* cx, JSGCStatus status, JS::GCReason reason,
                    void* data) {
  auto info = static_cast<MajorGC*>(data);
  if (!(info->phases & (1 << status))) {
    return;
  }

  if (info->depth > 0) {
    info->depth--;
    JS::PrepareForFullGC(cx);
    JS::NonIncrementalGC(cx, JS::GCOptions::Normal, JS::GCReason::API);
    info->depth++;
  }
}

struct MinorGC {
  int32_t phases;
  bool active;
};

static void minorGC(JSContext* cx, JSGCStatus status, JS::GCReason reason,
                    void* data) {
  auto info = static_cast<MinorGC*>(data);
  if (!(info->phases & (1 << status))) {
    return;
  }

  if (info->active) {
    info->active = false;
    if (cx->zone() && !cx->zone()->isAtomsZone()) {
      cx->runtime()->gc.evictNursery(JS::GCReason::DEBUG_GC);
    }
    info->active = true;
  }
}

// Process global, should really be runtime-local.
static MajorGC majorGCInfo;
static MinorGC minorGCInfo;

static void enterNullRealm(JSContext* cx, JSGCStatus status,
                           JS::GCReason reason, void* data) {
  JSAutoNullableRealm enterRealm(cx, nullptr);
}

} /* namespace gcCallback */

static bool SetGCCallback(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1) {
    JS_ReportErrorASCII(cx, "Wrong number of arguments");
    return false;
  }

  RootedObject opts(cx, ToObject(cx, args[0]));
  if (!opts) {
    return false;
  }

  RootedValue v(cx);
  if (!JS_GetProperty(cx, opts, "action", &v)) {
    return false;
  }

  JSString* str = JS::ToString(cx, v);
  if (!str) {
    return false;
  }
  Rooted<JSLinearString*> action(cx, str->ensureLinear(cx));
  if (!action) {
    return false;
  }

  int32_t phases = 0;
  if (StringEqualsLiteral(action, "minorGC") ||
      StringEqualsLiteral(action, "majorGC")) {
    if (!JS_GetProperty(cx, opts, "phases", &v)) {
      return false;
    }
    if (v.isUndefined()) {
      phases = (1 << JSGC_END);
    } else {
      JSString* str = JS::ToString(cx, v);
      if (!str) {
        return false;
      }
      JSLinearString* phasesStr = str->ensureLinear(cx);
      if (!phasesStr) {
        return false;
      }

      if (StringEqualsLiteral(phasesStr, "begin")) {
        phases = (1 << JSGC_BEGIN);
      } else if (StringEqualsLiteral(phasesStr, "end")) {
        phases = (1 << JSGC_END);
      } else if (StringEqualsLiteral(phasesStr, "both")) {
        phases = (1 << JSGC_BEGIN) | (1 << JSGC_END);
      } else {
        JS_ReportErrorASCII(cx, "Invalid callback phase");
        return false;
      }
    }
  }

  if (StringEqualsLiteral(action, "minorGC")) {
    gcCallback::minorGCInfo.phases = phases;
    gcCallback::minorGCInfo.active = true;
    JS_SetGCCallback(cx, gcCallback::minorGC, &gcCallback::minorGCInfo);
  } else if (StringEqualsLiteral(action, "majorGC")) {
    if (!JS_GetProperty(cx, opts, "depth", &v)) {
      return false;
    }
    int32_t depth = 1;
    if (!v.isUndefined()) {
      if (!ToInt32(cx, v, &depth)) {
        return false;
      }
    }
    if (depth < 0) {
      JS_ReportErrorASCII(cx, "Nesting depth cannot be negative");
      return false;
    }
    if (depth + gcstats::MAX_PHASE_NESTING >
        gcstats::Statistics::MAX_SUSPENDED_PHASES) {
      JS_ReportErrorASCII(cx, "Nesting depth too large, would overflow");
      return false;
    }

    gcCallback::majorGCInfo.phases = phases;
    gcCallback::majorGCInfo.depth = depth;
    JS_SetGCCallback(cx, gcCallback::majorGC, &gcCallback::majorGCInfo);
  } else if (StringEqualsLiteral(action, "enterNullRealm")) {
    JS_SetGCCallback(cx, gcCallback::enterNullRealm, nullptr);
  } else {
    JS_ReportErrorASCII(cx, "Unknown GC callback action");
    return false;
  }

  args.rval().setUndefined();
  return true;
}

#ifdef DEBUG
static bool EnqueueMark(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  gc::GCRuntime* gc = &cx->runtime()->gc;

  if (args.get(0).isString()) {
    RootedString val(cx, args[0].toString());
    if (!val->ensureLinear(cx)) {
      return false;
    }
    if (!gc->appendTestMarkQueue(StringValue(val))) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
  } else if (args.get(0).isObject()) {
    if (!gc->appendTestMarkQueue(args[0])) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
  } else {
    JS_ReportErrorASCII(cx, "Argument must be a string or object");
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static bool GetMarkQueue(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  const auto& queue = cx->runtime()->gc.getTestMarkQueue();

  RootedObject result(cx, JS::NewArrayObject(cx, queue.length()));
  if (!result) {
    return false;
  }
  for (size_t i = 0; i < queue.length(); i++) {
    RootedValue val(cx, queue[i]);
    if (!JS_WrapValue(cx, &val)) {
      return false;
    }
    if (!JS_SetElement(cx, result, i, val)) {
      return false;
    }
  }

  args.rval().setObject(*result);
  return true;
}

static bool ClearMarkQueue(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  cx->runtime()->gc.clearTestMarkQueue();
  args.rval().setUndefined();
  return true;
}
#endif  // DEBUG

static bool NurseryStringsEnabled(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(cx->zone()->allocNurseryStrings());
  return true;
}

static bool IsNurseryAllocated(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.get(0).isGCThing()) {
    JS_ReportErrorASCII(
        cx, "The function takes one argument, which must be a GC thing");
    return false;
  }

  args.rval().setBoolean(IsInsideNursery(args[0].toGCThing()));
  return true;
}

static bool NumAllocSitesPretenured(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setInt32(cx->realm()->numAllocSitesPretenured);
  return true;
}

static bool GetLcovInfo(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 1) {
    JS_ReportErrorASCII(cx, "Wrong number of arguments");
    return false;
  }

  if (!coverage::IsLCovEnabled()) {
    JS_ReportErrorASCII(cx, "Coverage not enabled for process.");
    return false;
  }

  RootedObject global(cx);
  if (args.hasDefined(0)) {
    global = ToObject(cx, args[0]);
    if (!global) {
      JS_ReportErrorASCII(cx, "Permission denied to access global");
      return false;
    }
    global = CheckedUnwrapDynamic(global, cx, /* stopAtWindowProxy = */ false);
    if (!global) {
      ReportAccessDenied(cx);
      return false;
    }
    if (!global->is<GlobalObject>()) {
      JS_ReportErrorASCII(cx, "Argument must be a global object");
      return false;
    }
  } else {
    global = JS::CurrentGlobalOrNull(cx);
  }

  size_t length = 0;
  UniqueChars content;
  {
    AutoRealm ar(cx, global);
    content = js::GetCodeCoverageSummary(cx, &length);
  }

  if (!content) {
    return false;
  }

  JSString* str =
      JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(content.get(), length));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

#ifdef DEBUG
static bool SetRNGState(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "SetRNGState", 2)) {
    return false;
  }

  double d0;
  if (!ToNumber(cx, args[0], &d0)) {
    return false;
  }

  double d1;
  if (!ToNumber(cx, args[1], &d1)) {
    return false;
  }

  uint64_t seed0 = static_cast<uint64_t>(d0);
  uint64_t seed1 = static_cast<uint64_t>(d1);

  if (seed0 == 0 && seed1 == 0) {
    JS_ReportErrorASCII(cx, "RNG requires non-zero seed");
    return false;
  }

  cx->realm()->getOrCreateRandomNumberGenerator().setState(seed0, seed1);

  args.rval().setUndefined();
  return true;
}
#endif

static bool GetTimeZone(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (args.length() != 0) {
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

#ifndef __wasi__
  auto getTimeZone = [](std::time_t* now) -> const char* {
    std::tm local{};
#  if defined(_WIN32)
    _tzset();
    if (localtime_s(&local, now) == 0) {
      return _tzname[local.tm_isdst > 0];
    }
#  else
    tzset();
#    if defined(HAVE_LOCALTIME_R)
    if (localtime_r(now, &local)) {
#    else
    std::tm* localtm = std::localtime(now);
    if (localtm) {
      *local = *localtm;
#    endif /* HAVE_LOCALTIME_R */

#    if defined(HAVE_TM_ZONE_TM_GMTOFF)
      return local.tm_zone;
#    else
      return tzname[local.tm_isdst > 0];
#    endif /* HAVE_TM_ZONE_TM_GMTOFF */
    }
#  endif   /* _WIN32 */
    return nullptr;
  };

  std::time_t now = std::time(nullptr);
  if (now != static_cast<std::time_t>(-1)) {
    if (const char* tz = getTimeZone(&now)) {
      return ReturnStringCopy(cx, args, tz);
    }
  }
#endif /* __wasi__ */
  args.rval().setUndefined();
  return true;
}

/*
 * Validate time zone input. Accepts the following formats:
 *  - "America/Chicago" (raw time zone)
 *  - ":America/Chicago"
 *  - "/this-part-is-ignored/zoneinfo/America/Chicago"
 *  - ":/this-part-is-ignored/zoneinfo/America/Chicago"
 *  - "/etc/localtime"
 *  - ":/etc/localtime"
 * Once the raw time zone is parsed out of the string, it is checked
 * against the time zones from GetAvailableTimeZones(). Throws an
 * Error if the time zone is invalid.
 */
#if defined(JS_HAS_INTL_API) && !defined(__wasi__)
static bool ValidateTimeZone(JSContext* cx, const char* timeZone) {
  static constexpr char zoneInfo[] = "/zoneinfo/";
  static constexpr size_t zoneInfoLength = sizeof(zoneInfo) - 1;

  size_t i = 0;
  if (timeZone[i] == ':') {
    ++i;
  }
  const char* zoneInfoPtr = strstr(timeZone, zoneInfo);
  const char* timeZonePart = timeZone[i] == '/' && zoneInfoPtr
                                 ? zoneInfoPtr + zoneInfoLength
                                 : timeZone + i;

  if (!*timeZonePart) {
    JS_ReportErrorASCII(cx, "Invalid time zone format");
    return false;
  }

  if (!strcmp(timeZonePart, "/etc/localtime")) {
    return true;
  }

  auto timeZones = mozilla::intl::TimeZone::GetAvailableTimeZones();
  if (timeZones.isErr()) {
    intl::ReportInternalError(cx, timeZones.unwrapErr());
    return false;
  }
  for (auto timeZoneName : timeZones.unwrap()) {
    if (timeZoneName.isErr()) {
      intl::ReportInternalError(cx);
      return false;
    }

    if (!strcmp(timeZonePart, timeZoneName.unwrap().data())) {
      return true;
    }
  }

  JS_ReportErrorASCII(cx, "Unsupported time zone name: %s", timeZonePart);
  return false;
}
#endif

static bool SetTimeZone(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (args.length() != 1) {
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  if (!args[0].isString() && !args[0].isUndefined()) {
    ReportUsageErrorASCII(cx, callee,
                          "First argument should be a string or undefined");
    return false;
  }

#ifndef __wasi__
  auto setTimeZone = [](const char* value) {
#  if defined(_WIN32)
    return _putenv_s("TZ", value) == 0;
#  else
    return setenv("TZ", value, true) == 0;
#  endif /* _WIN32 */
  };

  auto unsetTimeZone = []() {
#  if defined(_WIN32)
    return _putenv_s("TZ", "") == 0;
#  else
    return unsetenv("TZ") == 0;
#  endif /* _WIN32 */
  };

  if (args[0].isString() && !args[0].toString()->empty()) {
    Rooted<JSLinearString*> str(cx, args[0].toString()->ensureLinear(cx));
    if (!str) {
      return false;
    }

    if (!StringIsAscii(str)) {
      ReportUsageErrorASCII(cx, callee,
                            "First argument contains non-ASCII characters");
      return false;
    }

    UniqueChars timeZone = JS_EncodeStringToASCII(cx, str);
    if (!timeZone) {
      return false;
    }

    const char* timeZoneStr = timeZone.get();
#  ifdef JS_HAS_INTL_API
    if (!ValidateTimeZone(cx, timeZoneStr)) {
      return false;
    }
#  endif

    if (!setTimeZone(timeZoneStr)) {
      JS_ReportErrorASCII(cx, "Failed to set 'TZ' environment variable");
      return false;
    }
  } else {
    if (!unsetTimeZone()) {
      JS_ReportErrorASCII(cx, "Failed to unset 'TZ' environment variable");
      return false;
    }
  }

#  if defined(_WIN32)
  _tzset();
#  else
  tzset();
#  endif /* _WIN32 */

  JS::ResetTimeZone();

#endif /* __wasi__ */
  args.rval().setUndefined();
  return true;
}

static bool GetCoreCount(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (args.length() != 0) {
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  args.rval().setInt32(GetCPUCount());
  return true;
}

static bool GetDefaultLocale(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (args.length() != 0) {
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  UniqueChars locale = JS_GetDefaultLocale(cx);
  if (!locale) {
    return false;
  }

  return ReturnStringCopy(cx, args, locale.get());
}

static bool SetDefaultLocale(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (args.length() != 1) {
    ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
    return false;
  }

  if (!args[0].isString() && !args[0].isUndefined()) {
    ReportUsageErrorASCII(cx, callee,
                          "First argument should be a string or undefined");
    return false;
  }

  if (args[0].isString() && !args[0].toString()->empty()) {
    RootedString str(cx, args[0].toString());
    UniqueChars locale = StringToLocale(cx, callee, str);
    if (!locale) {
      return false;
    }

    if (!JS_SetDefaultLocale(cx->runtime(), locale.get())) {
      ReportOutOfMemory(cx);
      return false;
    }
  } else {
    JS_ResetDefaultLocale(cx->runtime());
  }

  args.rval().setUndefined();
  return true;
}

#ifdef AFLFUZZ
static bool AflLoop(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  uint32_t max_cnt;
  if (!ToUint32(cx, args.get(0), &max_cnt)) {
    return false;
  }

  args.rval().setBoolean(!!__AFL_LOOP(max_cnt));
  return true;
}
#endif

static bool MonotonicNow(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  double now;

// The std::chrono symbols are too new to be present in STL on all platforms we
// care about, so use raw POSIX clock APIs when it might be necessary.
#if defined(XP_UNIX) && !defined(XP_DARWIN)
  auto ComputeNow = [](const timespec& ts) {
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  };

  timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    // Use a monotonic clock if available.
    now = ComputeNow(ts);
  } else {
    // Use a realtime clock as fallback.
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
      // Fail if no clock is available.
      JS_ReportErrorASCII(cx, "can't retrieve system clock");
      return false;
    }

    now = ComputeNow(ts);

    // Manually enforce atomicity on a non-monotonic clock.
    {
      static mozilla::Atomic<bool, mozilla::ReleaseAcquire> spinLock;
      while (!spinLock.compareExchange(false, true)) {
        continue;
      }

      static double lastNow = -FLT_MAX;
      now = lastNow = std::max(now, lastNow);

      spinLock = false;
    }
  }
#else
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  using std::chrono::steady_clock;
  now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
            .count();
#endif  // XP_UNIX && !XP_DARWIN

  args.rval().setNumber(now);
  return true;
}

static bool TimeSinceCreation(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  double when =
      (mozilla::TimeStamp::Now() - mozilla::TimeStamp::ProcessCreation())
          .ToMilliseconds();
  args.rval().setNumber(when);
  return true;
}

static bool GetInnerMostEnvironmentObject(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  FrameIter iter(cx);
  if (iter.done()) {
    args.rval().setNull();
    return true;
  }

  args.rval().setObjectOrNull(iter.environmentChain(cx));
  return true;
}

static bool GetEnclosingEnvironmentObject(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "getEnclosingEnvironmentObject", 1)) {
    return false;
  }

  if (!args[0].isObject()) {
    args.rval().setUndefined();
    return true;
  }

  JSObject* envObj = &args[0].toObject();

  if (envObj->is<EnvironmentObject>()) {
    EnvironmentObject* env = &envObj->as<EnvironmentObject>();
    args.rval().setObject(env->enclosingEnvironment());
    return true;
  }

  if (envObj->is<DebugEnvironmentProxy>()) {
    DebugEnvironmentProxy* envProxy = &envObj->as<DebugEnvironmentProxy>();
    args.rval().setObject(envProxy->enclosingEnvironment());
    return true;
  }

  args.rval().setNull();
  return true;
}

static bool GetEnvironmentObjectType(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "getEnvironmentObjectType", 1)) {
    return false;
  }

  if (!args[0].isObject()) {
    args.rval().setUndefined();
    return true;
  }

  JSObject* envObj = &args[0].toObject();

  if (envObj->is<EnvironmentObject>()) {
    EnvironmentObject* env = &envObj->as<EnvironmentObject>();
    JSString* str = JS_NewStringCopyZ(cx, env->typeString());
    args.rval().setString(str);
    return true;
  }
  if (envObj->is<DebugEnvironmentProxy>()) {
    DebugEnvironmentProxy* envProxy = &envObj->as<DebugEnvironmentProxy>();
    EnvironmentObject* env = &envProxy->environment();
    char buf[256] = {'\0'};
    SprintfLiteral(buf, "[DebugProxy] %s", env->typeString());
    JSString* str = JS_NewStringCopyZ(cx, buf);
    args.rval().setString(str);
    return true;
  }

  args.rval().setUndefined();
  return true;
}

static bool AssertRealmFuseInvariants(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  // Note: This will crash if any invariant isn't held, so it's sufficient to
  // simply return true always.
  cx->realm()->realmFuses.assertInvariants(cx);
  args.rval().setUndefined();
  return true;
}

static bool GetFuseState(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  cx->realm()->realmFuses.assertInvariants(cx);

  RootedObject returnObj(cx, JS_NewPlainObject(cx));
  if (!returnObj) {
    return false;
  }

  RootedObject fuseObj(cx);
  RootedString intactStr(cx, NewStringCopyZ<CanGC>(cx, "intact"));
  if (!intactStr) {
    return false;
  }

  RootedValue intactValue(cx);

#define FUSE(Name, LowerName)                                                \
  fuseObj = JS_NewPlainObject(cx);                                           \
  if (!fuseObj) {                                                            \
    return false;                                                            \
  }                                                                          \
  intactValue.setBoolean(cx->realm()->realmFuses.LowerName.intact());        \
  if (!JS_DefineProperty(cx, fuseObj, "intact", intactValue,                 \
                         JSPROP_ENUMERATE)) {                                \
    return false;                                                            \
  }                                                                          \
  if (!JS_DefineProperty(cx, returnObj, #Name, fuseObj, JSPROP_ENUMERATE)) { \
    return false;                                                            \
  }

  FOR_EACH_REALM_FUSE(FUSE)
#undef FUSE

  // Register hasSeenUndefinedFuse
  fuseObj = JS_NewPlainObject(cx);
  if (!fuseObj) {
    return false;
  }
  intactValue.setBoolean(
      cx->runtime()->hasSeenObjectEmulateUndefinedFuse.ref().intact());
  if (!JS_DefineProperty(cx, fuseObj, "intact", intactValue,
                         JSPROP_ENUMERATE)) {
    return false;
  }

  if (!JS_DefineProperty(cx, returnObj, "hasSeenObjectEmulateUndefinedFuse",
                         fuseObj, JSPROP_ENUMERATE)) {
    return false;
  }

  args.rval().setObject(*returnObj);
  return true;
}

static bool PopAllFusesInRealm(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  MOZ_ASSERT(cx->realm());

  RealmFuses& realmFuses = cx->realm()->realmFuses;

#define FUSE(Name, LowerName) realmFuses.LowerName.popFuse(cx, realmFuses);
  FOR_EACH_REALM_FUSE(FUSE)
#undef FUSE

  args.rval().setUndefined();
  return true;
}

static bool GetAllPrefNames(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedValueVector values(cx);

  auto addPref = [cx, &values](const char* name) {
    JSString* s = JS_NewStringCopyZ(cx, name);
    if (!s) {
      return false;
    }
    return values.append(StringValue(s));
  };

#define ADD_NAME(NAME, CPP_NAME, TYPE, SETTER, IS_STARTUP_PREF) \
  if (!addPref(NAME)) {                                         \
    return false;                                               \
  }
  FOR_EACH_JS_PREF(ADD_NAME)
#undef ADD_NAME

  ArrayObject* arr = NewDenseCopiedArray(cx, values.length(), values.begin());
  if (!arr) {
    return false;
  }

  args.rval().setObject(*arr);
  return true;
}

static bool GetPrefValue(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "getPrefValue", 1)) {
    return false;
  }

  if (!args[0].isString()) {
    JS_ReportErrorASCII(cx, "expected string argument");
    return false;
  }

  Rooted<JSLinearString*> name(cx, args[0].toString()->ensureLinear(cx));
  if (!name) {
    return false;
  }

  auto setReturnValue = [&args](auto value) {
    using T = decltype(value);
    if constexpr (std::is_same_v<T, bool>) {
      args.rval().setBoolean(value);
    } else {
      static_assert(std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>);
      args.rval().setNumber(value);
    }
  };

  // Search for a matching pref and return its value.
#define CHECK_PREF(NAME, CPP_NAME, TYPE, SETTER, IS_STARTUP_PREF) \
  if (StringEqualsAscii(name, NAME)) {                            \
    setReturnValue(JS::Prefs::CPP_NAME());                        \
    return true;                                                  \
  }
  FOR_EACH_JS_PREF(CHECK_PREF)
#undef CHECK_PREF

  JS_ReportErrorASCII(cx, "invalid pref name");
  return false;
}

static bool GetErrorNotes(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "getErrorNotes", 1)) {
    return false;
  }

  if (!args[0].isObject() || !args[0].toObject().is<ErrorObject>()) {
    args.rval().setNull();
    return true;
  }

  JSErrorReport* report = args[0].toObject().as<ErrorObject>().getErrorReport();
  if (!report) {
    args.rval().setNull();
    return true;
  }

  RootedObject notesArray(cx, CreateErrorNotesArray(cx, report));
  if (!notesArray) {
    return false;
  }

  args.rval().setObject(*notesArray);
  return true;
}

static bool IsConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (args.length() < 1) {
    args.rval().setBoolean(false);
  } else {
    args.rval().setBoolean(IsConstructor(args[0]));
  }
  return true;
}

static bool SetTimeResolution(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (!args.requireAtLeast(cx, "setTimeResolution", 2)) {
    return false;
  }

  if (!args[0].isInt32()) {
    ReportUsageErrorASCII(cx, callee, "First argument must be an Int32.");
    return false;
  }
  int32_t resolution = args[0].toInt32();

  if (!args[1].isBoolean()) {
    ReportUsageErrorASCII(cx, callee, "Second argument must be a Boolean");
    return false;
  }
  bool jitter = args[1].toBoolean();

  JS::SetTimeResolutionUsec(resolution, jitter);

  args.rval().setUndefined();
  return true;
}

static bool ScriptedCallerGlobal(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject obj(cx, JS::GetScriptedCallerGlobal(cx));
  if (!obj) {
    args.rval().setNull();
    return true;
  }

  obj = ToWindowProxyIfWindow(obj);

  if (!cx->compartment()->wrap(cx, &obj)) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool ObjectGlobal(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (!args.get(0).isObject()) {
    ReportUsageErrorASCII(cx, callee, "Argument must be an object");
    return false;
  }

  RootedObject obj(cx, &args[0].toObject());
  if (IsCrossCompartmentWrapper(obj)) {
    args.rval().setNull();
    return true;
  }

  obj = ToWindowProxyIfWindow(&obj->nonCCWGlobal());

  args.rval().setObject(*obj);
  return true;
}

static bool IsSameCompartment(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (!args.get(0).isObject() || !args.get(1).isObject()) {
    ReportUsageErrorASCII(cx, callee, "Both arguments must be objects");
    return false;
  }

  RootedObject obj1(cx, UncheckedUnwrap(&args[0].toObject()));
  RootedObject obj2(cx, UncheckedUnwrap(&args[1].toObject()));

  args.rval().setBoolean(obj1->compartment() == obj2->compartment());
  return true;
}

static bool FirstGlobalInCompartment(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (!args.get(0).isObject()) {
    ReportUsageErrorASCII(cx, callee, "Argument must be an object");
    return false;
  }

  RootedObject obj(cx, UncheckedUnwrap(&args[0].toObject()));
  obj = ToWindowProxyIfWindow(GetFirstGlobalInCompartment(obj->compartment()));

  if (!cx->compartment()->wrap(cx, &obj)) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

static bool AssertCorrectRealm(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_RELEASE_ASSERT(cx->realm() == args.callee().as<JSFunction>().realm());
  args.rval().setUndefined();
  return true;
}

static bool GlobalLexicals(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<GlobalLexicalEnvironmentObject*> globalLexical(
      cx, &cx->global()->lexicalEnvironment());

  RootedIdVector props(cx);
  if (!GetPropertyKeys(cx, globalLexical, JSITER_HIDDEN, &props)) {
    return false;
  }

  RootedObject res(cx, JS_NewPlainObject(cx));
  if (!res) {
    return false;
  }

  RootedValue val(cx);
  for (size_t i = 0; i < props.length(); i++) {
    HandleId id = props[i];
    if (!JS_GetPropertyById(cx, globalLexical, id, &val)) {
      return false;
    }
    if (val.isMagic(JS_UNINITIALIZED_LEXICAL)) {
      continue;
    }
    if (!JS_DefinePropertyById(cx, res, id, val, JSPROP_ENUMERATE)) {
      return false;
    }
  }

  args.rval().setObject(*res);
  return true;
}

static bool EncodeAsUtf8InBuffer(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "encodeAsUtf8InBuffer", 2)) {
    return false;
  }

  RootedObject callee(cx, &args.callee());

  if (!args[0].isString()) {
    ReportUsageErrorASCII(cx, callee, "First argument must be a String");
    return false;
  }

  // Create the amounts array early so that the raw pointer into Uint8Array
  // data has as short a lifetime as possible
  Rooted<ArrayObject*> array(cx, NewDenseFullyAllocatedArray(cx, 2));
  if (!array) {
    return false;
  }
  array->ensureDenseInitializedLength(0, 2);

  JSObject* obj = args[1].isObject() ? &args[1].toObject() : nullptr;
  Rooted<JS::Uint8Array> view(cx, JS::Uint8Array::unwrap(obj));
  if (!view) {
    ReportUsageErrorASCII(cx, callee, "Second argument must be a Uint8Array");
    return false;
  }

  mozilla::Span<uint8_t> span;
  bool isSharedMemory = false;
  {
    // The hazard analysis does not track the data pointer, so it can neither
    // tell that `data` is dead if ReportUsageErrorASCII is called, nor that
    // its live range ends at the call to AsWritableChars(). Construct a
    // temporary scope to hide from the analysis. This should really be replaced
    // with a safer mechanism.
    JS::AutoCheckCannotGC nogc(cx);
    if (!view.isDetached()) {
      span = view.get().getData(&isSharedMemory, nogc);
    }
  }

  if (isSharedMemory ||  // exclude views of SharedArrayBuffers
      !span.data()) {    // exclude views of detached ArrayBuffers
    ReportUsageErrorASCII(
        cx, callee,
        "Second argument must be an unshared, non-detached Uint8Array");
    return false;
  }

  Maybe<std::tuple<size_t, size_t>> amounts =
      JS_EncodeStringToUTF8BufferPartial(cx, args[0].toString(),
                                         AsWritableChars(span));
  if (!amounts) {
    ReportOutOfMemory(cx);
    return false;
  }

  auto [unitsRead, bytesWritten] = *amounts;

  array->initDenseElement(0, Int32Value(AssertedCast<int32_t>(unitsRead)));
  array->initDenseElement(1, Int32Value(AssertedCast<int32_t>(bytesWritten)));

  args.rval().setObject(*array);
  return true;
}

JSScript* js::TestingFunctionArgumentToScript(
    JSContext* cx, HandleValue v, JSFunction** funp /* = nullptr */) {
  if (v.isString()) {
    // To convert a string to a script, compile it. Parse it as an ES6 Program.
    Rooted<JSString*> str(cx, v.toString());
    AutoStableStringChars linearChars(cx);
    if (!linearChars.initTwoByte(cx, str)) {
      return nullptr;
    }
    SourceText<char16_t> source;
    if (!source.initMaybeBorrowed(cx, linearChars)) {
      return nullptr;
    }

    CompileOptions options(cx);
    return JS::Compile(cx, options, source);
  }

  RootedFunction fun(cx, JS_ValueToFunction(cx, v));
  if (!fun) {
    return nullptr;
  }

  if (!fun->isInterpreted()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TESTING_SCRIPTS_ONLY);
    return nullptr;
  }

  JSScript* script = JSFunction::getOrCreateScript(cx, fun);
  if (!script) {
    return nullptr;
  }

  if (funp) {
    *funp = fun;
  }

  return script;
}

static bool BaselineCompile(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  RootedScript script(cx);
  if (args.length() == 0) {
    NonBuiltinScriptFrameIter iter(cx);
    if (iter.done()) {
      ReportUsageErrorASCII(cx, callee,
                            "no script argument and no script caller");
      return false;
    }
    script = iter.script();
  } else {
    script = TestingFunctionArgumentToScript(cx, args[0]);
    if (!script) {
      return false;
    }
  }

  bool forceDebug = false;
  if (args.length() > 1) {
    if (args.length() > 2) {
      ReportUsageErrorASCII(cx, callee, "too many arguments");
      return false;
    }
    if (!args[1].isBoolean() && !args[1].isUndefined()) {
      ReportUsageErrorASCII(
          cx, callee, "forceDebugInstrumentation argument should be boolean");
      return false;
    }
    forceDebug = ToBoolean(args[1]);
  }

  const char* returnedStr = nullptr;
  do {
    // In order to check for differential behaviour, baselineCompile should have
    // the same output whether --no-baseline is used or not.
    if (js::SupportDifferentialTesting()) {
      returnedStr = "skipped (differential testing)";
      break;
    }

    AutoRealm ar(cx, script);
    if (script->hasBaselineScript()) {
      if (forceDebug && !script->baselineScript()->hasDebugInstrumentation()) {
        // There isn't an easy way to do this for a script that might be on
        // stack right now. See
        // js::jit::RecompileOnStackBaselineScriptsForDebugMode.
        ReportUsageErrorASCII(
            cx, callee, "unsupported case: recompiling script for debug mode");
        return false;
      }

      args.rval().setUndefined();
      return true;
    }

    if (!jit::IsBaselineJitEnabled(cx)) {
      returnedStr = "baseline disabled";
      break;
    }
    if (!script->canBaselineCompile()) {
      returnedStr = "can't compile";
      break;
    }
    if (!cx->zone()->ensureJitZoneExists(cx)) {
      return false;
    }

    jit::MethodStatus status = jit::BaselineCompile(cx, script, forceDebug);
    switch (status) {
      case jit::Method_Error:
        return false;
      case jit::Method_CantCompile:
        returnedStr = "can't compile";
        break;
      case jit::Method_Skipped:
        returnedStr = "skipped";
        break;
      case jit::Method_Compiled:
        args.rval().setUndefined();
    }
  } while (false);

  if (returnedStr) {
    return ReturnStringCopy(cx, args, returnedStr);
  }

  return true;
}

static bool ClearKeptObjects(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JS::ClearKeptObjects(cx);
  args.rval().setUndefined();
  return true;
}

static bool NumberToDouble(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "numberToDouble", 1)) {
    return false;
  }

  if (!args[0].isNumber()) {
    RootedObject callee(cx, &args.callee());
    ReportUsageErrorASCII(cx, callee, "argument must be a number");
    return false;
  }

  args.rval().setDouble(args[0].toNumber());
  return true;
}

static bool GetICUOptions(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject info(cx, JS_NewPlainObject(cx));
  if (!info) {
    return false;
  }

#ifdef JS_HAS_INTL_API
  RootedString str(cx);

  str = NewStringCopy<CanGC>(cx, mozilla::intl::ICU4CLibrary::GetVersion());
  if (!str || !JS_DefineProperty(cx, info, "version", str, JSPROP_ENUMERATE)) {
    return false;
  }

  str = NewStringCopy<CanGC>(cx, mozilla::intl::String::GetUnicodeVersion());
  if (!str || !JS_DefineProperty(cx, info, "unicode", str, JSPROP_ENUMERATE)) {
    return false;
  }

  str = NewStringCopyZ<CanGC>(cx, mozilla::intl::Locale::GetDefaultLocale());
  if (!str || !JS_DefineProperty(cx, info, "locale", str, JSPROP_ENUMERATE)) {
    return false;
  }

  auto tzdataVersion = mozilla::intl::TimeZone::GetTZDataVersion();
  if (tzdataVersion.isErr()) {
    intl::ReportInternalError(cx, tzdataVersion.unwrapErr());
    return false;
  }

  str = NewStringCopy<CanGC>(cx, tzdataVersion.unwrap());
  if (!str || !JS_DefineProperty(cx, info, "tzdata", str, JSPROP_ENUMERATE)) {
    return false;
  }

  intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> buf(cx);

  if (auto ok = DateTimeInfo::timeZoneId(DateTimeInfo::ForceUTC::No, buf);
      ok.isErr()) {
    intl::ReportInternalError(cx, ok.unwrapErr());
    return false;
  }

  str = buf.toString(cx);
  if (!str || !JS_DefineProperty(cx, info, "timezone", str, JSPROP_ENUMERATE)) {
    return false;
  }

  if (auto ok = mozilla::intl::TimeZone::GetHostTimeZone(buf); ok.isErr()) {
    intl::ReportInternalError(cx, ok.unwrapErr());
    return false;
  }

  str = buf.toString(cx);
  if (!str ||
      !JS_DefineProperty(cx, info, "host-timezone", str, JSPROP_ENUMERATE)) {
    return false;
  }
#endif

  args.rval().setObject(*info);
  return true;
}

static bool GetAvailableLocalesOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (!args.requireAtLeast(cx, "getAvailableLocalesOf", 1)) {
    return false;
  }

  HandleValue arg = args[0];
  if (!arg.isString()) {
    ReportUsageErrorASCII(cx, callee, "First argument must be a string");
    return false;
  }

  ArrayObject* result;
#ifdef JS_HAS_INTL_API
  using SupportedLocaleKind = js::intl::SharedIntlData::SupportedLocaleKind;

  SupportedLocaleKind kind;
  {
    JSLinearString* typeStr = arg.toString()->ensureLinear(cx);
    if (!typeStr) {
      return false;
    }

    if (StringEqualsLiteral(typeStr, "Collator")) {
      kind = SupportedLocaleKind::Collator;
    } else if (StringEqualsLiteral(typeStr, "DateTimeFormat")) {
      kind = SupportedLocaleKind::DateTimeFormat;
    } else if (StringEqualsLiteral(typeStr, "DisplayNames")) {
      kind = SupportedLocaleKind::DisplayNames;
    } else if (StringEqualsLiteral(typeStr, "ListFormat")) {
      kind = SupportedLocaleKind::ListFormat;
    } else if (StringEqualsLiteral(typeStr, "NumberFormat")) {
      kind = SupportedLocaleKind::NumberFormat;
    } else if (StringEqualsLiteral(typeStr, "PluralRules")) {
      kind = SupportedLocaleKind::PluralRules;
    } else if (StringEqualsLiteral(typeStr, "RelativeTimeFormat")) {
      kind = SupportedLocaleKind::RelativeTimeFormat;
    } else if (StringEqualsLiteral(typeStr, "Segmenter")) {
      kind = SupportedLocaleKind::Segmenter;
    } else {
      ReportUsageErrorASCII(cx, callee, "Unsupported Intl constructor name");
      return false;
    }
  }

  intl::SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();
  result = sharedIntlData.availableLocalesOf(cx, kind);
#else
  result = NewDenseEmptyArray(cx);
#endif
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

static bool IsSmallFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject callee(cx, &args.callee());

  if (!args.requireAtLeast(cx, "IsSmallFunction", 1)) {
    return false;
  }

  HandleValue arg = args[0];
  if (!arg.isObject() || !arg.toObject().is<JSFunction>()) {
    ReportUsageErrorASCII(cx, callee, "First argument must be a function");
    return false;
  }

  RootedFunction fun(cx, &args[0].toObject().as<JSFunction>());
  if (!fun->isInterpreted()) {
    ReportUsageErrorASCII(cx, callee,
                          "First argument must be an interpreted function");
    return false;
  }

  JSScript* script = JSFunction::getOrCreateScript(cx, fun);
  if (!script) {
    return false;
  }

  args.rval().setBoolean(jit::JitOptions.isSmallFunction(script));
  return true;
}

static bool PCCountProfiling_Start(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JS::StartPCCountProfiling(cx);

  args.rval().setUndefined();
  return true;
}

static bool PCCountProfiling_Stop(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JS::StopPCCountProfiling(cx);

  args.rval().setUndefined();
  return true;
}

static bool PCCountProfiling_Purge(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JS::PurgePCCounts(cx);

  args.rval().setUndefined();
  return true;
}

static bool PCCountProfiling_ScriptCount(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  size_t length = JS::GetPCCountScriptCount(cx);

  args.rval().setNumber(double(length));
  return true;
}

static bool PCCountProfiling_ScriptSummary(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "summary", 1)) {
    return false;
  }

  uint32_t index;
  if (!JS::ToUint32(cx, args[0], &index)) {
    return false;
  }

  JSString* str = JS::GetPCCountScriptSummary(cx, index);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool PCCountProfiling_ScriptContents(JSContext* cx, unsigned argc,
                                            Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  if (!args.requireAtLeast(cx, "contents", 1)) {
    return false;
  }

  uint32_t index;
  if (!JS::ToUint32(cx, args[0], &index)) {
    return false;
  }

  JSString* str = JS::GetPCCountScriptContents(cx, index);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static bool NukeCCW(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() != 1 || !args[0].isObject() ||
      !IsCrossCompartmentWrapper(&args[0].toObject())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INVALID_ARGS,
                              "nukeCCW");
    return false;
  }

  NukeCrossCompartmentWrapper(cx, &args[0].toObject());
  args.rval().setUndefined();
  return true;
}

static bool FdLibM_Pow(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double x;
  if (!JS::ToNumber(cx, args.get(0), &x)) {
    return false;
  }

  double y;
  if (!JS::ToNumber(cx, args.get(1), &y)) {
    return false;
  }

  // Because C99 and ECMA specify different behavior for pow(), we need to wrap
  // the fdlibm call to make it ECMA compliant.
  if (!std::isfinite(y) && (x == 1.0 || x == -1.0)) {
    args.rval().setNaN();
  } else {
    args.rval().setDouble(fdlibm_pow(x, y));
  }
  return true;
}

// clang-format off
static const JSFunctionSpecWithHelp TestingFunctions[] = {
    JS_FN_HELP("gc", ::GC, 0, 0,
"gc([obj] | 'zone' [, ('shrinking' | 'last-ditch') ])",
"  Run the garbage collector.\n"
"  The first parameter describes which zones to collect: if an object is\n"
"  given, GC only its zone. If 'zone' is given, GC any zones that were\n"
"  scheduled via schedulegc.\n"
"  The second parameter is optional and may be 'shrinking' to perform a\n"
"  shrinking GC or 'last-ditch' for a shrinking, last-ditch GC."),

    JS_FN_HELP("minorgc", ::MinorGC, 0, 0,
"minorgc([aboutToOverflow])",
"  Run a minor collector on the Nursery. When aboutToOverflow is true, marks\n"
"  the store buffer as about-to-overflow before collecting."),

    JS_FN_HELP("maybegc", ::MaybeGC, 0, 0,
"maybegc()",
"  Hint to the engine that now is an ok time to run the garbage collector.\n"),

    JS_FN_HELP("gcparam", GCParameter, 2, 0,
"gcparam(name [, value])",
"  Wrapper for JS_[GS]etGCParameter. The name is one of:" GC_PARAMETER_ARGS_LIST),

    JS_FN_HELP("hasDisassembler", HasDisassembler, 0, 0,
"hasDisassembler()",
"  Return true if a disassembler is present (for disnative and wasmDis)."),

    JS_FN_HELP("disnative", DisassembleNative, 2, 0,
"disnative(fun,[path])",
"  Disassemble a function into its native code. Optionally write the native code bytes to a file on disk.\n"),

    JS_FN_HELP("relazifyFunctions", RelazifyFunctions, 0, 0,
"relazifyFunctions(...)",
"  Perform a GC and allow relazification of functions. Accepts the same\n"
"  arguments as gc()."),

    JS_FN_HELP("getBuildConfiguration", GetBuildConfiguration, 1, 0,
"getBuildConfiguration([option])",
"  Query the options SpiderMonkey was built with, or return an object\n"
"  with the options if no argument is given."),

    JS_FN_HELP("getRealmConfiguration", GetRealmConfiguration, 1, 0,
"getRealmConfiguration([option])",
"  Query the runtime options SpiderMonkey is running with, or return an\n."
"  object with the options if no argument is given."),

    JS_FN_HELP("isLcovEnabled", ::IsLCovEnabled, 0, 0,
"isLcovEnabled()",
"  Return true if JS LCov support is enabled."),

  JS_FN_HELP("trialInline", TrialInline, 0, 0,
"trialInline()",
"  Perform trial-inlining for the caller's frame if it's a BaselineFrame."),

    JS_FN_HELP("hasChild", HasChild, 0, 0,
"hasChild(parent, child)",
"  Return true if |child| is a child of |parent|, as determined by a call to\n"
"  TraceChildren"),

    JS_FN_HELP("setSavedStacksRNGState", SetSavedStacksRNGState, 1, 0,
"setSavedStacksRNGState(seed)",
"  Set this compartment's SavedStacks' RNG state.\n"),

    JS_FN_HELP("getSavedFrameCount", GetSavedFrameCount, 0, 0,
"getSavedFrameCount()",
"  Return the number of SavedFrame instances stored in this compartment's\n"
"  SavedStacks cache."),

    JS_FN_HELP("clearSavedFrames", ClearSavedFrames, 0, 0,
"clearSavedFrames()",
"  Empty the current compartment's cache of SavedFrame objects, so that\n"
"  subsequent stack captures allocate fresh objects to represent frames.\n"
"  Clear the current stack's LiveSavedFrameCaches."),

    JS_FN_HELP("saveStack", SaveStack, 0, 0,
"saveStack([maxDepth [, compartment]])",
"  Capture a stack. If 'maxDepth' is given, capture at most 'maxDepth' number\n"
"  of frames. If 'compartment' is given, allocate the js::SavedFrame instances\n"
"  with the given object's compartment."),

    JS_FN_HELP("captureFirstSubsumedFrame", CaptureFirstSubsumedFrame, 1, 0,
"saveStack(object [, shouldIgnoreSelfHosted = true]])",
"  Capture a stack back to the first frame whose principals are subsumed by the\n"
"  object's compartment's principals. If 'shouldIgnoreSelfHosted' is given,\n"
"  control whether self-hosted frames are considered when checking principals."),

    JS_FN_HELP("callFunctionFromNativeFrame", CallFunctionFromNativeFrame, 1, 0,
"callFunctionFromNativeFrame(function)",
"  Call 'function' with a (C++-)native frame on stack.\n"
"  Required for testing that SaveStack properly handles native frames."),

    JS_FN_HELP("callFunctionWithAsyncStack", CallFunctionWithAsyncStack, 0, 0,
"callFunctionWithAsyncStack(function, stack, asyncCause)",
"  Call 'function', using the provided stack as the async stack responsible\n"
"  for the call, and propagate its return value or the exception it throws.\n"
"  The function is called with no arguments, and 'this' is 'undefined'. The\n"
"  specified |asyncCause| is attached to the provided stack frame."),

    JS_FN_HELP("enableTrackAllocations", EnableTrackAllocations, 0, 0,
"enableTrackAllocations()",
"  Start capturing the JS stack at every allocation. Note that this sets an\n"
"  object metadata callback that will override any other object metadata\n"
"  callback that may be set."),

    JS_FN_HELP("disableTrackAllocations", DisableTrackAllocations, 0, 0,
"disableTrackAllocations()",
"  Stop capturing the JS stack at every allocation."),

    JS_FN_HELP("setTestFilenameValidationCallback", SetTestFilenameValidationCallback, 0, 0,
"setTestFilenameValidationCallback()",
"  Set the filename validation callback to a callback that accepts only\n"
"  filenames starting with 'safe' or (only in system realms) 'system'."),

    JS_FN_HELP("newObjectWithAddPropertyHook", NewObjectWithAddPropertyHook, 0, 0,
"newObjectWithAddPropertyHook()",
"  Returns a new object with an addProperty JSClass hook. This hook\n"
"  increments the value of the _propertiesAdded data property on the object\n"
"  when a new property is added."),

    JS_FN_HELP("newObjectWithCallHook", NewObjectWithCallHook, 0, 0,
"newObjectWithCallHook()",
"  Returns a new object with call/construct JSClass hooks. These hooks return\n"
"  a new object that contains the Values supplied by the caller."),

    JS_FN_HELP("newObjectWithManyReservedSlots", NewObjectWithManyReservedSlots, 0, 0,
"newObjectWithManyReservedSlots()",
"  Returns a new object with many reserved slots. The slots are initialized to int32\n"
"  values. checkObjectWithManyReservedSlots can be used to check the slots still\n"
"  hold these values."),

    JS_FN_HELP("checkObjectWithManyReservedSlots", CheckObjectWithManyReservedSlots, 1, 0,
"checkObjectWithManyReservedSlots(obj)",
"  Checks the reserved slots set by newObjectWithManyReservedSlots still hold the expected\n"
"  values."),

    JS_FN_HELP("getWatchtowerLog", GetWatchtowerLog, 0, 0,
"getWatchtowerLog()",
"  Returns the Watchtower log recording object changes for objects for which\n"
"  addWatchtowerTarget was called. The internal log is cleared. The return\n"
"  value is an array of plain objects with the following properties:\n"
"  - kind: a string describing the kind of mutation, for example \"add-prop\"\n"
"  - object: the object being mutated\n"
"  - extra: an extra value, for example the name of the property being added"),

    JS_FN_HELP("addWatchtowerTarget", AddWatchtowerTarget, 1, 0,
"addWatchtowerTarget(object)",
"  Invoke the watchtower callback for changes to this object."),

    JS_FN_HELP("newString", NewString, 2, 0,
"newString(str[, options])",
"  Copies str's chars and returns a new string. Valid options:\n"
"  \n"
"   - tenured: allocate directly into the tenured heap.\n"
"  \n"
"   - twoByte: create a \"two byte\" string, not a latin1 string, regardless of the\n"
"      input string's characters. Latin1 will be used by default if possible\n"
"      (again regardless of the input string.)\n"
"  \n"
"   - external: create an external string. External strings are always twoByte and\n"
"     tenured.\n"
"  \n"
"   - maybeExternal: create an external string, unless the data fits within an\n"
"     inline string. Inline strings may be nursery-allocated."),

    JS_FN_HELP("newDependentString", NewDependentString, 2, 0,
"newDependentString(str, indexStart[, indexEnd] [, options])",
"  Essentially the same as str.substring() but insist on\n"
"  creating a dependent string and failing if not. Also has options to\n"
"  control the heap the string object is allocated into:\n"
"  \n"
"   - tenured: if true, allocate in the tenured heap or throw. If false,\n"
"     allocate in the nursery or throw."),

    JS_FN_HELP("ensureLinearString", EnsureLinearString, 1, 0,
"ensureLinearString(str)",
"  Ensures str is a linear (non-rope) string and returns it."),

    JS_FN_HELP("representativeStringArray", RepresentativeStringArray, 0, 0,
"representativeStringArray()",
"  Returns an array of strings that represent the various internal string\n"
"  types and character encodings."),

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

    JS_FN_HELP("oomThreadTypes", OOMThreadTypes, 0, 0,
"oomThreadTypes()",
"  Get the number of thread types that can be used as an argument for\n"
"  oomAfterAllocations() and oomAtAllocation()."),

    JS_FN_HELP("oomAfterAllocations", OOMAfterAllocations, 2, 0,
"oomAfterAllocations(count [,threadType])",
"  After 'count' js_malloc memory allocations, fail every following allocation\n"
"  (return nullptr). The optional thread type limits the effect to the\n"
"  specified type of helper thread."),

    JS_FN_HELP("oomAtAllocation", OOMAtAllocation, 2, 0,
"oomAtAllocation(count [,threadType])",
"  After 'count' js_malloc memory allocations, fail the next allocation\n"
"  (return nullptr). The optional thread type limits the effect to the\n"
"  specified type of helper thread."),

    JS_FN_HELP("resetOOMFailure", ResetOOMFailure, 0, 0,
"resetOOMFailure()",
"  Remove the allocation failure scheduled by either oomAfterAllocations() or\n"
"  oomAtAllocation() and return whether any allocation had been caused to fail."),

    JS_FN_HELP("oomTest", OOMTest, 0, 0,
"oomTest(function, [expectExceptionOnFailure = true | options])",
"  Test that the passed function behaves correctly under OOM conditions by\n"
"  repeatedly executing it and simulating allocation failure at successive\n"
"  allocations until the function completes without seeing a failure.\n"
"  By default this tests that an exception is raised if execution fails, but\n"
"  this can be disabled by passing false as the optional second parameter.\n"
"  This is also disabled when --fuzzing-safe is specified.\n"
"  Alternatively an object can be passed to set the following options:\n"
"    expectExceptionOnFailure: bool - as described above.\n"
"    keepFailing: bool - continue to fail after first simulated failure.\n"
"\n"
"  WARNING: By design, oomTest assumes the test-function follows the same\n"
"  code path each time it is called, right up to the point where OOM occurs.\n"
"  If on iteration 70 it finishes and caches a unit of work that saves 65\n"
"  allocations the next time we run, then the subsequent 65 allocation\n"
"  points will go untested.\n"
"\n"
"  Things in this category include lazy parsing and baseline compilation,\n"
"  so it is very easy to accidentally write an oomTest that only tests one\n"
"  or the other of those, and not the functionality you meant to test!\n"
"  To avoid lazy parsing, call the test function once first before passing\n"
"  it to oomTest. The jits can be disabled via the test harness.\n"),

    JS_FN_HELP("stackTest", StackTest, 0, 0,
"stackTest(function, [expectExceptionOnFailure = true])",
"  This function behaves exactly like oomTest with the difference that\n"
"  instead of simulating regular OOM conditions, it simulates the engine\n"
"  running out of stack space (failing recursion check).\n"
"\n"
"  See the WARNING in help('oomTest').\n"),

    JS_FN_HELP("interruptTest", InterruptTest, 0, 0,
"interruptTest(function)",
"  This function simulates interrupts similar to how oomTest simulates OOM conditions."
"\n"
"  See the WARNING in help('oomTest').\n"),

#endif // defined(DEBUG) || defined(JS_OOM_BREAKPOINT)

    JS_FN_HELP("newRope", NewRope, 3, 0,
"newRope(left, right[, options])",
"  Creates a rope with the given left/right strings.\n"
"  Available options:\n"
"    nursery: bool - force the string to be created in/out of the nursery, if possible.\n"),

    JS_FN_HELP("isRope", IsRope, 1, 0,
"isRope(str)",
"  Returns true if the parameter is a rope"),

    JS_FN_HELP("settlePromiseNow", SettlePromiseNow, 1, 0,
"settlePromiseNow(promise)",
"  'Settle' a 'promise' immediately. This just marks the promise as resolved\n"
"  with a value of `undefined` and causes the firing of any onPromiseSettled\n"
"  hooks set on Debugger instances that are observing the given promise's\n"
"  global as a debuggee."),
    JS_FN_HELP("getWaitForAllPromise", GetWaitForAllPromise, 1, 0,
"getWaitForAllPromise(densePromisesArray)",
"  Calls the 'GetWaitForAllPromise' JSAPI function and returns the result\n"
"  Promise."),
JS_FN_HELP("resolvePromise", ResolvePromise, 2, 0,
"resolvePromise(promise, resolution)",
"  Resolve a Promise by calling the JSAPI function JS::ResolvePromise."),
JS_FN_HELP("rejectPromise", RejectPromise, 2, 0,
"rejectPromise(promise, reason)",
"  Reject a Promise by calling the JSAPI function JS::RejectPromise."),

    JS_FN_HELP("makeFinalizeObserver", MakeFinalizeObserver, 0, 0,
"makeFinalizeObserver()",
"  Get a special object whose finalization increases the counter returned\n"
"  by the finalizeCount function."),

    JS_FN_HELP("finalizeCount", FinalizeCount, 0, 0,
"finalizeCount()",
"  Return the current value of the finalization counter that is incremented\n"
"  each time an object returned by the makeFinalizeObserver is finalized."),

    JS_FN_HELP("resetFinalizeCount", ResetFinalizeCount, 0, 0,
"resetFinalizeCount()",
"  Reset the value returned by finalizeCount()."),

    JS_FN_HELP("gcPreserveCode", GCPreserveCode, 0, 0,
"gcPreserveCode()",
"  Preserve JIT code during garbage collections."),

#ifdef JS_GC_ZEAL
    JS_FN_HELP("gczeal", GCZeal, 2, 0,
"gczeal([mode, [frequency]])",
gc::ZealModeHelpText),

    JS_FN_HELP("unsetgczeal", UnsetGCZeal, 2, 0,
"unsetgczeal(mode)",
"  Turn off a single zeal mode set with gczeal() and don't finish any ongoing\n"
"  collection that may be happening."),

    JS_FN_HELP("schedulegc", ScheduleGC, 1, 0,
"schedulegc([num])",
"  If num is given, schedule a GC after num allocations.\n"
"  Returns the number of allocations before the next trigger."),

    JS_FN_HELP("selectforgc", SelectForGC, 0, 0,
"selectforgc(obj1, obj2, ...)",
"  Schedule the given objects to be marked in the next GC slice."),

    JS_FN_HELP("verifyprebarriers", VerifyPreBarriers, 0, 0,
"verifyprebarriers()",
"  Start or end a run of the pre-write barrier verifier."),

    JS_FN_HELP("verifypostbarriers", VerifyPostBarriers, 0, 0,
"verifypostbarriers()",
"  Does nothing (the post-write barrier verifier has been remove)."),

    JS_FN_HELP("currentgc", CurrentGC, 0, 0,
"currentgc()",
"  Report various information about the currently running incremental GC,\n"
"  if one is running."),

    JS_FN_HELP("deterministicgc", DeterministicGC, 1, 0,
"deterministicgc(true|false)",
"  If true, only allow determinstic GCs to run."),

    JS_FN_HELP("dumpGCArenaInfo", DumpGCArenaInfo, 0, 0,
"dumpGCArenaInfo()",
"  Prints information about the different GC things and how they are arranged\n"
"  in arenas.\n"),

    JS_FN_HELP("setMarkStackLimit", SetMarkStackLimit, 1, 0,
"markStackLimit(limit)",
"  Sets a limit on the number of words used for the mark stack. Used to test OOM"
"  handling during marking.\n"),

#endif

    JS_FN_HELP("gcstate", GCState, 0, 0,
"gcstate([obj])",
"  Report the global GC state, or the GC state for the zone containing |obj|."),

    JS_FN_HELP("schedulezone", ScheduleZoneForGC, 1, 0,
"schedulezone([obj | string])",
"  If obj is given, schedule a GC of obj's zone.\n"
"  If string is given, schedule a GC of the string's zone if possible."),

    JS_FN_HELP("startgc", StartGC, 1, 0,
"startgc([n [, 'shrinking']])",
"  Start an incremental GC and run a slice that processes about n objects.\n"
"  If 'shrinking' is passesd as the optional second argument, perform a\n"
"  shrinking GC rather than a normal GC. If no zones have been selected with\n"
"  schedulezone(), a full GC will be performed."),

    JS_FN_HELP("finishgc", FinishGC, 0, 0,
"finishgc()",
"   Finish an in-progress incremental GC, if none is running then do nothing."),

    JS_FN_HELP("gcslice", GCSlice, 1, 0,
"gcslice([n [, options]])",
"  Start or continue an an incremental GC, running a slice that processes\n"
"  about n objects. Takes an optional options object, which may contain the\n"
"  following properties:\n"
"    dontStart: do not start a new incremental GC if one is not already\n"
"               running"),

    JS_FN_HELP("abortgc", AbortGC, 1, 0,
"abortgc()",
"  Abort the current incremental GC."),

    JS_FN_HELP("fullcompartmentchecks", FullCompartmentChecks, 1, 0,
"fullcompartmentchecks(true|false)",
"  If true, check for compartment mismatches before every GC."),

    JS_FN_HELP("nondeterministicGetWeakMapKeys", NondeterministicGetWeakMapKeys, 1, 0,
"nondeterministicGetWeakMapKeys(weakmap)",
"  Return an array of the keys in the given WeakMap."),

    JS_FN_HELP("internalConst", InternalConst, 1, 0,
"internalConst(name)",
"  Query an internal constant for the engine. See InternalConst source for\n"
"  the list of constant names."),

    JS_FN_HELP("isProxy", IsProxy, 1, 0,
"isProxy(obj)",
"  If true, obj is a proxy of some sort"),

    JS_FN_HELP("dumpHeap", DumpHeap, 1, 0,
"dumpHeap([filename])",
"  Dump reachable and unreachable objects to the named file, or to stdout. Objects\n"
"  in the nursery are ignored, so if you wish to include them, consider calling\n"
"  minorgc() first."),

    JS_FN_HELP("terminate", Terminate, 0, 0,
"terminate()",
"  Terminate JavaScript execution, as if we had run out of\n"
"  memory or been terminated by the slow script dialog."),

    JS_FN_HELP("readGeckoProfilingStack", ReadGeckoProfilingStack, 0, 0,
"readGeckoProfilingStack()",
"  Reads the JIT/Wasm stack using ProfilingFrameIterator. Skips non-JIT/Wasm frames."),

    JS_FN_HELP("readGeckoInterpProfilingStack", ReadGeckoInterpProfilingStack, 0, 0,
"readGeckoInterpProfilingStack()",
"  Reads the C++ interpreter profiling stack. Skips JIT/Wasm frames."),

    JS_FN_HELP("enableOsiPointRegisterChecks", EnableOsiPointRegisterChecks, 0, 0,
"enableOsiPointRegisterChecks()",
"  Emit extra code to verify live regs at the start of a VM call are not\n"
"  modified before its OsiPoint."),

    JS_FN_HELP("displayName", DisplayName, 1, 0,
"displayName(fn)",
"  Gets the display name for a function, which can possibly be a guessed or\n"
"  inferred name based on where the function was defined. This can be\n"
"  different from the 'name' property on the function."),

    JS_FN_HELP("isAsmJSCompilationAvailable", IsAsmJSCompilationAvailable, 0, 0,
"isAsmJSCompilationAvailable",
"  Returns whether asm.js compilation is currently available or whether it is disabled\n"
"  (e.g., by the debugger)."),

    JS_FN_HELP("getJitCompilerOptions", GetJitCompilerOptions, 0, 0,
"getJitCompilerOptions()",
"  Return an object describing some of the JIT compiler options.\n"),

    JS_FN_HELP("isAsmJSModule", IsAsmJSModule, 1, 0,
"isAsmJSModule(fn)",
"  Returns whether the given value is a function containing \"use asm\" that has been\n"
"  validated according to the asm.js spec."),

    JS_FN_HELP("isAsmJSFunction", IsAsmJSFunction, 1, 0,
"isAsmJSFunction(fn)",
"  Returns whether the given value is a nested function in an asm.js module that has been\n"
"  both compile- and link-time validated."),

    JS_FN_HELP("isAvxPresent", IsAvxPresent, 0, 0,
"isAvxPresent([minVersion])",
"  Returns whether AVX is present and enabled. If minVersion specified,\n"
"  use 1 - to check if AVX is enabled (default), 2 - if AVX2 is enabled."),

    JS_FN_HELP("wasmIsSupported", WasmIsSupported, 0, 0,
"wasmIsSupported()",
"  Returns a boolean indicating whether WebAssembly is supported on the current device."),

    JS_FN_HELP("wasmIsSupportedByHardware", WasmIsSupportedByHardware, 0, 0,
"wasmIsSupportedByHardware()",
"  Returns a boolean indicating whether WebAssembly is supported on the current hardware (regardless of whether we've enabled support)."),

    JS_FN_HELP("wasmDebuggingEnabled", WasmDebuggingEnabled, 0, 0,
"wasmDebuggingEnabled()",
"  Returns a boolean indicating whether WebAssembly debugging is supported on the current device;\n"
"  returns false also if WebAssembly is not supported"),

    JS_FN_HELP("wasmStreamingEnabled", WasmStreamingEnabled, 0, 0,
"wasmStreamingEnabled()",
"  Returns a boolean indicating whether WebAssembly caching is supported by the runtime."),

    JS_FN_HELP("wasmCachingEnabled", WasmCachingEnabled, 0, 0,
"wasmCachingEnabled()",
"  Returns a boolean indicating whether WebAssembly caching is supported by the runtime."),

    JS_FN_HELP("wasmHugeMemorySupported", WasmHugeMemorySupported, 0, 0,
"wasmHugeMemorySupported()",
"  Returns a boolean indicating whether WebAssembly supports using a large"
"  virtual memory reservation in order to elide bounds checks on this platform."),

    JS_FN_HELP("wasmMaxMemoryPages", WasmMaxMemoryPages, 1, 0,
"wasmMaxMemoryPages(indexType)",
"  Returns an int with the maximum number of pages that can be allocated to a memory."
"  This is an implementation artifact that does depend on the index type, the hardware,"
"  the operating system, the build configuration, and flags.  The result is constant for"
"  a given combination of those; there is no guarantee that that size allocation will"
"  always succeed, only that it can succeed in principle.  The indexType is a string,"
"  'i32' or 'i64'."),

#define WASM_FEATURE(NAME, ...) \
    JS_FN_HELP("wasm" #NAME "Enabled", Wasm##NAME##Enabled, 0, 0, \
"wasm" #NAME "Enabled()", \
"  Returns a boolean indicating whether the WebAssembly " #NAME " proposal is enabled."),
JS_FOR_WASM_FEATURES(WASM_FEATURE)
#undef WASM_FEATURE

    JS_FN_HELP("wasmThreadsEnabled", WasmThreadsEnabled, 0, 0,
"wasmThreadsEnabled()",
"  Returns a boolean indicating whether the WebAssembly threads proposal is\n"
"  supported on the current device."),

    JS_FN_HELP("wasmSimdEnabled", WasmSimdEnabled, 0, 0,
"wasmSimdEnabled()",
"  Returns a boolean indicating whether WebAssembly SIMD proposal is\n"
"  supported by the current device."),

#if defined(ENABLE_WASM_SIMD) && defined(DEBUG)
    JS_FN_HELP("wasmSimdAnalysis", WasmSimdAnalysis, 1, 0,
"wasmSimdAnalysis(...)",
"  Unstable API for white-box testing.\n"),
#endif

    JS_FN_HELP("wasmGlobalFromArrayBuffer", WasmGlobalFromArrayBuffer, 2, 0,
"wasmGlobalFromArrayBuffer(type, arrayBuffer)",
"  Create a WebAssembly.Global object from a provided ArrayBuffer. The type\n"
"  must be POD (i32, i64, f32, f64, v128). The buffer must be the same\n"
"  size as the type in bytes.\n"),
    JS_FN_HELP("wasmGlobalExtractLane", WasmGlobalExtractLane, 3, 0,
"wasmGlobalExtractLane(global, laneInterp, laneIndex)",
"  Extract a lane from a WebAssembly.Global object that contains a v128 value\n"
"  and return it as a new WebAssembly.Global object of the appropriate type.\n"
"  The supported laneInterp values are i32x4, i64x2, f32x4, and\n"
"  f64x2.\n"),
    JS_FN_HELP("wasmGlobalsEqual", WasmGlobalsEqual, 2, 0,
"wasmGlobalsEqual(globalA, globalB)",
"  Compares two WebAssembly.Global objects for if their types and values are\n"
"  equal. Mutability is not compared. Floating point values are compared for\n"
"  bitwise equality, not IEEE 754 equality.\n"),
    JS_FN_HELP("wasmGlobalIsNaN", WasmGlobalIsNaN, 2, 0,
"wasmGlobalIsNaN(global, flavor)",
"  Compares a floating point WebAssembly.Global object for if its value is a\n"
"  specific NaN flavor. Valid flavors are `arithmetic_nan` and `canonical_nan`.\n"),
    JS_FN_HELP("wasmGlobalToString", WasmGlobalToString, 1, 0,
"wasmGlobalToString(global)",
"  Returns a debug representation of the contents of a WebAssembly.Global\n"
"  object.\n"),
    JS_FN_HELP("wasmLosslessInvoke", WasmLosslessInvoke, 1, 0,
"wasmLosslessInvoke(wasmFunc, args...)",
"  Invokes the provided WebAssembly function using a modified conversion\n"
"  function that allows providing a param as a WebAssembly.Global and\n"
"  returning a result as a WebAssembly.Global.\n"),

    JS_FN_HELP("wasmCompilersPresent", WasmCompilersPresent, 0, 0,
"wasmCompilersPresent()",
"  Returns a string indicating the present wasm compilers: a comma-separated list\n"
"  of 'baseline', 'ion'.  A compiler is present in the executable if it is compiled\n"
"  in and can generate code for the current architecture."),

    JS_FN_HELP("wasmCompileMode", WasmCompileMode, 0, 0,
"wasmCompileMode()",
"  Returns a string indicating the available wasm compilers: 'baseline', 'ion',\n"
"  'baseline+ion', or 'none'.  A compiler is available if it is present in the\n"
"  executable and not disabled by switches or runtime conditions.  At most one\n"
"  baseline and one optimizing compiler can be available."),

    JS_FN_HELP("wasmBaselineDisabledByFeatures", WasmBaselineDisabledByFeatures, 0, 0,
"wasmBaselineDisabledByFeatures()",
"  If some feature is enabled at compile-time or run-time that prevents baseline\n"
"  from being used then this returns a truthy string describing the features that\n."
"  are disabling it.  Otherwise it returns false."),

    JS_FN_HELP("wasmIonDisabledByFeatures", WasmIonDisabledByFeatures, 0, 0,
"wasmIonDisabledByFeatures()",
"  If some feature is enabled at compile-time or run-time that prevents Ion\n"
"  from being used then this returns a truthy string describing the features that\n."
"  are disabling it.  Otherwise it returns false."),

    JS_FN_HELP("wasmExtractCode", WasmExtractCode, 1, 0,
"wasmExtractCode(module[, tier])",
"  Extracts generated machine code from WebAssembly.Module.  The tier is a string,\n"
"  'stable', 'best', 'baseline', or 'ion'; the default is 'stable'.  If the request\n"
"  cannot be satisfied then null is returned.  If the request is 'ion' then block\n"
"  until background compilation is complete."),

    JS_FN_HELP("wasmDis", WasmDisassemble, 1, 0,
"wasmDis(wasmObject[, options])\n",
"  Disassembles generated machine code from an exported WebAssembly function,\n"
"  or from all the functions defined in the module or instance, exported and not.\n"
"  The `options` is an object with the following optional keys:\n"
"    asString: boolean - if true, return a string rather than printing on stderr,\n"
"          the default is false.\n"
"    tier: string - one of 'stable', 'best', 'baseline', or 'ion'; the default is\n"
"          'stable'.\n"
"    kinds: string - if set, and the wasmObject is a module or instance, a\n"
"           comma-separated list of the following keys, the default is `Function`:\n"
"      Function         - functions defined in the module\n"
"      InterpEntry      - C++-to-wasm stubs\n"
"      JitEntry         - jitted-js-to-wasm stubs\n"
"      ImportInterpExit - wasm-to-C++ stubs\n"
"      ImportJitExit    - wasm-to-jitted-JS stubs\n"
"      all              - all kinds, including obscure ones\n"),

    JS_FN_HELP("wasmDumpIon", WasmDumpIon, 2, 0,
"wasmDumpIon(bytecode, funcIndex, [, contents])\n",
"wasmDumpIon(bytecode, funcIndex, [, contents])"
"  Returns a dump of compiling a function in the specified module with Ion."
"  The `contents` flag controls what is dumped. one of:"
"    `mir` | `unopt-mir`: Unoptimized MIR (the default)"
"    `opt-mir`: Optimized MIR"
"    `lir`: LIR"),

    JS_FN_HELP("wasmHasTier2CompilationCompleted", WasmHasTier2CompilationCompleted, 1, 0,
"wasmHasTier2CompilationCompleted(module)",
"  Returns a boolean indicating whether a given module has finished compiled code for tier2. \n"
"This will return true early if compilation isn't two-tiered. "),

    JS_FN_HELP("wasmLoadedFromCache", WasmLoadedFromCache, 1, 0,
"wasmLoadedFromCache(module)",
"  Returns a boolean indicating whether a given module was deserialized directly from a\n"
"  cache (as opposed to compiled from bytecode)."),

    JS_FN_HELP("wasmBuiltinI8VecMul", WasmBuiltinI8VecMul, 0, 0,
"wasmBuiltinI8VecMul()",
"  Returns a module that implements an i8 vector pairwise multiplication intrinsic."),

#ifdef ENABLE_WASM_GC
    JS_FN_HELP("wasmGcReadField", WasmGcReadField, 2, 0,
"wasmGcReadField(obj, index)",
"  Gets a field of a WebAssembly GC struct or array."),

    JS_FN_HELP("wasmGcArrayLength", WasmGcArrayLength, 1, 0,
"wasmGcArrayLength(arr)",
"  Gets the length of a WebAssembly GC array."),
#endif // ENABLE_WASM_GC

#ifdef ENABLE_WASM_BRANCH_HINTING
    JS_FN_HELP("wasmParsedBranchHints", WasmParsedBranchHints, 1, 0,
"wasmParsedBranchHints(module)",
"  Returns a boolean indicating whether a given module has successfully parsed a\n"
"  custom branch hinting section."),

#endif // ENABLE_WASM_BRANCH_HINTING

    JS_FN_HELP("largeArrayBufferSupported", LargeArrayBufferSupported, 0, 0,
"largeArrayBufferSupported()",
"  Returns true if array buffers larger than 2GB can be allocated."),

    JS_FN_HELP("isLazyFunction", IsLazyFunction, 1, 0,
"isLazyFunction(fun)",
"  True if fun is a lazy JSFunction."),

    JS_FN_HELP("isRelazifiableFunction", IsRelazifiableFunction, 1, 0,
"isRelazifiableFunction(fun)",
"  True if fun is a JSFunction with a relazifiable JSScript."),

    JS_FN_HELP("hasSameBytecodeData", HasSameBytecodeData, 2, 0,
"hasSameBytecodeData(fun1, fun2)",
"  True if fun1 and fun2 share the same copy of bytecode data. This will\n"
"  delazify the function if necessary."),

    JS_FN_HELP("enableShellAllocationMetadataBuilder", EnableShellAllocationMetadataBuilder, 0, 0,
"enableShellAllocationMetadataBuilder()",
"  Use ShellAllocationMetadataBuilder to supply metadata for all newly created objects."),

    JS_FN_HELP("getAllocationMetadata", GetAllocationMetadata, 1, 0,
"getAllocationMetadata(obj)",
"  Get the metadata for an object."),

    JS_INLINABLE_FN_HELP("bailout", testingFunc_bailout, 0, 0, TestBailout,
"bailout()",
"  Force a bailout out of ionmonkey (if running in ionmonkey)."),

    JS_FN_HELP("bailAfter", testingFunc_bailAfter, 1, 0,
"bailAfter(number)",
"  Start a counter to bail once after passing the given amount of possible bailout positions in\n"
"  ionmonkey.\n"),

    JS_FN_HELP("invalidate", testingFunc_invalidate, 0, 0,
"invalidate()",
"  Force an immediate invalidation (if running in Warp)."),

    JS_FN_HELP("inJit", testingFunc_inJit, 0, 0,
"inJit()",
"  Returns true when called within (jit-)compiled code. When jit compilation is disabled this\n"
"  function returns an error string. This function returns false in all other cases.\n"
"  Depending on truthiness, you should continue to wait for compilation to happen or stop execution.\n"),

    JS_FN_HELP("inIon", testingFunc_inIon, 0, 0,
"inIon()",
"  Returns true when called within ion. When ion is disabled or when compilation is abnormally\n"
"  slow to start, this function returns an error string. Otherwise, this function returns false.\n"
"  This behaviour ensures that a falsy value means that we are not in ion, but expect a\n"
"  compilation to occur in the future. Conversely, a truthy value means that we are either in\n"
"  ion or that there is litle or no chance of ion ever compiling the current script."),

    JS_FN_HELP("assertJitStackInvariants", TestingFunc_assertJitStackInvariants, 0, 0,
"assertJitStackInvariants()",
"  Iterates the Jit stack and check that stack invariants hold."),

    JS_FN_HELP("setIonCheckGraphCoherency", SetIonCheckGraphCoherency, 1, 0,
"setIonCheckGraphCoherency(bool)",
"  Set whether Ion should perform graph consistency (DEBUG-only) assertions. These assertions\n"
"  are valuable and should be generally enabled, however they can be very expensive for large\n"
"  (wasm) programs."),

    JS_FN_HELP("serialize", testingFunc_serialize, 1, 0,
"serialize(data, [transferables, [policy]])",
"  Serialize 'data' using JS_WriteStructuredClone. Returns a structured\n"
"  clone buffer object. 'policy' may be an options hash. Valid keys:\n"
"    'SharedArrayBuffer' - either 'allow' or 'deny' (the default)\n"
"      to specify whether SharedArrayBuffers may be serialized.\n"
"    'scope' - SameProcess, DifferentProcess, or\n"
"      DifferentProcessForIndexedDB. Determines how some values will be\n"
"      serialized. Clone buffers may only be deserialized with a compatible\n"
"      scope. NOTE - For DifferentProcess/DifferentProcessForIndexedDB,\n"
"      must also set SharedArrayBuffer:'deny' if data contains any shared memory\n"
"      object."),

    JS_FN_HELP("deserialize", Deserialize, 1, 0,
"deserialize(clonebuffer[, opts])",
"  Deserialize data generated by serialize. 'opts' may be an options hash.\n"
"  Valid keys:\n"
"    'SharedArrayBuffer' - either 'allow' or 'deny' (the default)\n"
"      to specify whether SharedArrayBuffers may be serialized.\n"
"    'scope', which limits the clone buffers that are considered\n"
"  valid. Allowed values: ''SameProcess', 'DifferentProcess',\n"
"  and 'DifferentProcessForIndexedDB'. So for example, a\n"
"  DifferentProcessForIndexedDB clone buffer may be deserialized in any scope, but\n"
"  a SameProcess clone buffer cannot be deserialized in a\n"
"  DifferentProcess scope."),

    JS_FN_HELP("detachArrayBuffer", DetachArrayBuffer, 1, 0,
"detachArrayBuffer(buffer)",
"  Detach the given ArrayBuffer object from its memory, i.e. as if it\n"
"  had been transferred to a WebWorker."),

    JS_FN_HELP("makeSerializable", MakeSerializable, 1, 0,
"makeSerializable(numeric id, [behavior])",
"  Make a custom serializable, transferable object. It will have a single accessor\n"
"  obj.log that will give a history of all operations on all such objects in the\n"
"  current thread as an array [id, action, id, action, ...] where the id\n"
"  is the number passed into this function, and the action is one of:\n"
"     ? - the canTransfer() hook was called.\n"
"     w - the write() hook was called.\n"
"     W - the writeTransfer() hook was called.\n"
"     R - the readTransfer() hook was called.\n"
"     r - the read() hook was called.\n"
"     F - the freeTransfer() hook was called.\n"
"  The `behavior` parameter can be used to force a failure during processing:\n"
"     1 - fail during readTransfer() hook\n"
"     2 - fail during read() hook\n"
"  Set the log to null to clear it."),

    JS_FN_HELP("ensureNonInline", EnsureNonInline, 1, 0,
"ensureNonInline(view or buffer)",
"  Ensure that the memory for the given ArrayBuffer or ArrayBufferView\n"
"  is not inline."),

    JS_FN_HELP("pinArrayBufferOrViewLength", PinArrayBufferOrViewLength, 1, 0,
"pinArrayBufferOrViewLength(view or buffer[, pin])",
"  Prevent or allow (if `pin` is false) changes to the length of the given\n"
"  ArrayBuffer or ArrayBufferView. `pin` defaults to true."),

    JS_FN_HELP("JSONStringify", JSONStringify, 4, 0,
"JSONStringify(value, behavior)",
"  Same as JSON.stringify(value), but allows setting behavior:\n"
"    Normal: the default\n"
"    FastOnly: throw if the fast path bails\n"
"    SlowOnly: skip the fast path entirely\n"
"    Compare: run both the fast and slow paths and compare the result. Crash if\n"
"      they do not match. If the fast path bails, no comparison is done."),

    JS_FN_HELP("helperThreadCount", HelperThreadCount, 0, 0,
"helperThreadCount()",
"  Returns the number of helper threads available for off-thread tasks."),

    JS_FN_HELP("createShapeSnapshot", CreateShapeSnapshot, 1, 0,
"createShapeSnapshot(obj)",
"  Returns an object containing a shape snapshot for use with\n"
"  checkShapeSnapshot.\n"),

    JS_FN_HELP("checkShapeSnapshot", CheckShapeSnapshot, 2, 0,
"checkShapeSnapshot(snapshot, [obj])",
"  Check shape invariants based on the given snapshot and optional object.\n"
"  If there's no object argument, the snapshot's object is used.\n"),

    JS_FN_HELP("enableShapeConsistencyChecks", EnableShapeConsistencyChecks, 0, 0,
"enableShapeConsistencyChecks()",
"  Enable some slow Shape assertions.\n"),

    JS_FN_HELP("reportOutOfMemory", ReportOutOfMemory, 0, 0,
"reportOutOfMemory()",
"  Report OOM, then clear the exception and return undefined. For crash testing."),

    JS_FN_HELP("throwOutOfMemory", ThrowOutOfMemory, 0, 0,
"throwOutOfMemory()",
"  Throw out of memory exception, for OOM handling testing."),

    JS_FN_HELP("reportLargeAllocationFailure", ReportLargeAllocationFailure, 0, 0,
"reportLargeAllocationFailure([bytes])",
"  Call the large allocation failure callback, as though a large malloc call failed,\n"
"  then return undefined. In Gecko, this sends a memory pressure notification, which\n"
"  can free up some memory."),

    JS_FN_HELP("findPath", FindPath, 2, 0,
"findPath(start, target)",
"  Return an array describing one of the shortest paths of GC heap edges from\n"
"  |start| to |target|, or |undefined| if |target| is unreachable from |start|.\n"
"  Each element of the array is either of the form:\n"
"    { node: <object or string>, edge: <string describing edge from node> }\n"
"  if the node is a JavaScript object or value; or of the form:\n"
"    { type: <string describing node>, edge: <string describing edge> }\n"
"  if the node is some internal thing that is not a proper JavaScript value\n"
"  (like a shape or a scope chain element). The destination of the i'th array\n"
"  element's edge is the node of the i+1'th array element; the destination of\n"
"  the last array element is implicitly |target|.\n"),

    JS_FN_HELP("wasmMetadataAnalysis", wasmMetadataAnalysis, 1, 0,
"wasmMetadataAnalysis(wasmObject)",
"  Prints an analysis of the size of metadata on this wasm object.\n"),

    JS_FN_HELP("sharedMemoryEnabled", SharedMemoryEnabled, 0, 0,
"sharedMemoryEnabled()",
"  Return true if SharedArrayBuffer and Atomics are enabled"),

    JS_FN_HELP("sharedArrayRawBufferRefcount", SharedArrayRawBufferRefcount, 0, 0,
"sharedArrayRawBufferRefcount(sab)",
"  Return the reference count of the SharedArrayRawBuffer object held by sab"),

#ifdef NIGHTLY_BUILD
    JS_FN_HELP("objectAddress", ObjectAddress, 1, 0,
"objectAddress(obj)",
"  Return the current address of the object. For debugging only--this\n"
"  address may change during a moving GC."),

    JS_FN_HELP("sharedAddress", SharedAddress, 1, 0,
"sharedAddress(obj)",
"  Return the address of the shared storage of a SharedArrayBuffer."),
#endif

    JS_FN_HELP("hasInvalidatedTeleporting", HasInvalidatedTeleporting, 1, 0,
"hasInvalidatedTeleporting(obj)",
"  Return true if the shape teleporting optimization has been disabled for |obj|."),

    JS_FN_HELP("evalReturningScope", EvalReturningScope, 1, 0,
"evalReturningScope(scriptStr, [global])",
"  Evaluate the script in a new scope and return the scope.\n"
"  If |global| is present, clone the script to |global| before executing."),

    JS_FN_HELP("backtrace", DumpBacktrace, 1, 0,
"backtrace()",
"  Dump out a brief backtrace."),

    JS_FN_HELP("getBacktrace", GetBacktrace, 1, 0,
"getBacktrace([options])",
"  Return the current stack as a string. Takes an optional options object,\n"
"  which may contain any or all of the boolean properties:\n"
"    options.args - show arguments to each function\n"
"    options.locals - show local variables in each frame\n"
"    options.thisprops - show the properties of the 'this' object of each frame\n"),

    JS_FN_HELP("byteSize", ByteSize, 1, 0,
"byteSize(value)",
"  Return the size in bytes occupied by |value|, or |undefined| if value\n"
"  is not allocated in memory.\n"),

    JS_FN_HELP("byteSizeOfScript", ByteSizeOfScript, 1, 0,
"byteSizeOfScript(f)",
"  Return the size in bytes occupied by the function |f|'s JSScript.\n"),

    JS_FN_HELP("setImmutablePrototype", SetImmutablePrototype, 1, 0,
"setImmutablePrototype(obj)",
"  Try to make obj's [[Prototype]] immutable, such that subsequent attempts to\n"
"  change it will fail.  Return true if obj's [[Prototype]] was successfully made\n"
"  immutable (or if it already was immutable), false otherwise.  Throws in case\n"
"  of internal error, or if the operation doesn't even make sense (for example,\n"
"  because the object is a revoked proxy)."),

    JS_FN_HELP("allocationMarker", AllocationMarker, 0, 0,
"allocationMarker([options])",
"  Return a freshly allocated object whose [[Class]] name is\n"
"  \"AllocationMarker\". Such objects are allocated only by calls\n"
"  to this function, never implicitly by the system, making them\n"
"  suitable for use in allocation tooling tests. Takes an optional\n"
"  options object which may contain the following properties:\n"
"    * nursery: bool, whether to allocate the object in the nursery\n"),

    JS_FN_HELP("setGCCallback", SetGCCallback, 1, 0,
"setGCCallback({action:\"...\", options...})",
"  Set the GC callback. action may be:\n"
"    'minorGC' - run a nursery collection\n"
"    'majorGC' - run a major collection, nesting up to a given 'depth'\n"),

#ifdef DEBUG
    JS_FN_HELP("enqueueMark", EnqueueMark, 1, 0,
"enqueueMark(obj|string)",
"  Add an object to the queue of objects to mark at the beginning every GC. (Note\n"
"  that the objects will actually be marked at the beginning of every slice, but\n"
"  after the first slice they will already be marked so nothing will happen.)\n"
"  \n"
"  Instead of an object, a few magic strings may be used:\n"
"    'yield' - cause the current marking slice to end, as if the mark budget were\n"
"      exceeded.\n"
"    'enter-weak-marking-mode' - divide the list into two segments. The items after\n"
"      this string will not be marked until we enter weak marking mode. Note that weak\n"
"      marking mode may be entered zero or multiple times for one GC.\n"
"    'abort-weak-marking-mode' - same as above, but then abort weak marking to fall back\n"
"      on the old iterative marking code path.\n"
"    'drain' - fully drain the mark stack before continuing.\n"
"    'set-color-black' - force everything following in the mark queue to be marked black.\n"
"    'set-color-gray' - continue with the regular GC until gray marking is possible, then force\n"
"       everything following in the mark queue to be marked gray.\n"
"    'unset-color' - stop forcing the mark color."),

    JS_FN_HELP("clearMarkQueue", ClearMarkQueue, 0, 0,
"clearMarkQueue()",
"  Cancel the special marking of all objects enqueue with enqueueMark()."),

    JS_FN_HELP("getMarkQueue", GetMarkQueue, 0, 0,
"getMarkQueue()",
"  Return the current mark queue set up via enqueueMark calls. Note that all\n"
"  returned values will be wrapped into the current compartment, so this loses\n"
"  some fidelity."),
#endif // DEBUG

    JS_FN_HELP("nurseryStringsEnabled", NurseryStringsEnabled, 0, 0,
"nurseryStringsEnabled()",
"  Return whether strings are currently allocated in the nursery for the current\n"
"  global\n"),

    JS_FN_HELP("isNurseryAllocated", IsNurseryAllocated, 1, 0,
"isNurseryAllocated(thing)",
"  Return whether a GC thing is nursery allocated.\n"),

    JS_FN_HELP("numAllocSitesPretenured", NumAllocSitesPretenured, 0, 0,
"numAllocSitesPretenured()",
"  Return the number of allocation sites that were pretenured for the current\n"
"  global\n"),

    JS_FN_HELP("getLcovInfo", GetLcovInfo, 1, 0,
"getLcovInfo(global)",
"  Generate LCOV tracefile for the given compartment.  If no global are provided then\n"
"  the current global is used as the default one.\n"),

#ifdef DEBUG
    JS_FN_HELP("setRNGState", SetRNGState, 2, 0,
"setRNGState(seed0, seed1)",
"  Set this compartment's RNG state.\n"),
#endif

#ifdef AFLFUZZ
    JS_FN_HELP("aflloop", AflLoop, 1, 0,
"aflloop(max_cnt)",
"  Call the __AFL_LOOP() runtime function (see AFL docs)\n"),
#endif

    JS_FN_HELP("monotonicNow", MonotonicNow, 0, 0,
"monotonicNow()",
"  Return a timestamp reflecting the current elapsed system time.\n"
"  This is monotonically increasing.\n"),

    JS_FN_HELP("timeSinceCreation", TimeSinceCreation, 0, 0,
"TimeSinceCreation()",
"  Returns the time in milliseconds since process creation.\n"
"  This uses a clock compatible with the profiler.\n"),

    JS_FN_HELP("isConstructor", IsConstructor, 1, 0,
"isConstructor(value)",
"  Returns whether the value is considered IsConstructor.\n"),

    JS_FN_HELP("getTimeZone", GetTimeZone, 0, 0,
"getTimeZone()",
"  Get the current time zone.\n"),

    JS_FN_HELP("getDefaultLocale", GetDefaultLocale, 0, 0,
"getDefaultLocale()",
"  Get the current default locale.\n"),

    JS_FN_HELP("getCoreCount", GetCoreCount, 0, 0,
"getCoreCount()",
"  Get the number of CPU cores from the platform layer.  Typically this\n"
"  means the number of hyperthreads on systems where that makes sense.\n"),

    JS_FN_HELP("setTimeResolution", SetTimeResolution, 2, 0,
"setTimeResolution(resolution, jitter)",
"  Enables time clamping and jittering. Specify a time resolution in\n"
"  microseconds and whether or not to jitter\n"),

    JS_FN_HELP("scriptedCallerGlobal", ScriptedCallerGlobal, 0, 0,
"scriptedCallerGlobal()",
"  Get the caller's global (or null). See JS::GetScriptedCallerGlobal.\n"),

    JS_FN_HELP("objectGlobal", ObjectGlobal, 1, 0,
"objectGlobal(obj)",
"  Returns the object's global object or null if the object is a wrapper.\n"),

    JS_FN_HELP("isSameCompartment", IsSameCompartment, 2, 0,
"isSameCompartment(obj1, obj2)",
"  Unwraps obj1 and obj2 and returns whether the unwrapped objects are\n"
"  same-compartment.\n"),

    JS_FN_HELP("firstGlobalInCompartment", FirstGlobalInCompartment, 1, 0,
"firstGlobalInCompartment(obj)",
"  Returns the first global in obj's compartment.\n"),

    JS_FN_HELP("assertCorrectRealm", AssertCorrectRealm, 0, 0,
"assertCorrectRealm()",
"  Asserts cx->realm matches callee->realm.\n"),

    JS_FN_HELP("globalLexicals", GlobalLexicals, 0, 0,
"globalLexicals()",
"  Returns an object containing a copy of all global lexical bindings.\n"
"  Example use: let x = 1; assertEq(globalLexicals().x, 1);\n"),

    JS_FN_HELP("baselineCompile", BaselineCompile, 2, 0,
"baselineCompile([fun/code], forceDebugInstrumentation=false)",
"  Baseline-compiles the given JS function or script.\n"
"  Without arguments, baseline-compiles the caller's script; but note\n"
"  that extra boilerplate is needed afterwards to cause the VM to start\n"
"  running the jitcode rather than staying in the interpreter:\n"
"    baselineCompile();  for (var i=0; i<1; i++) {} ...\n"
"  The interpreter will enter the new jitcode at the loop header unless\n"
"  baselineCompile returned a string or threw an error.\n"),

    JS_FN_HELP("encodeAsUtf8InBuffer", EncodeAsUtf8InBuffer, 2, 0,
"encodeAsUtf8InBuffer(str, uint8Array)",
"  Encode as many whole code points from the string str into the provided\n"
"  Uint8Array as will completely fit in it, converting lone surrogates to\n"
"  REPLACEMENT CHARACTER.  Return an array [r, w] where |r| is the\n"
"  number of 16-bit units read and |w| is the number of bytes of UTF-8\n"
"  written."),

   JS_FN_HELP("clearKeptObjects", ClearKeptObjects, 0, 0,
"clearKeptObjects()",
"Perform the ECMAScript ClearKeptObjects operation, clearing the list of\n"
"observed WeakRef targets that are kept alive until the next synchronous\n"
"sequence of ECMAScript execution completes. This is used for testing\n"
"WeakRefs.\n"),

  JS_FN_HELP("numberToDouble", NumberToDouble, 1, 0,
"numberToDouble(number)",
"  Return the input number as double-typed number."),

JS_FN_HELP("getICUOptions", GetICUOptions, 0, 0,
"getICUOptions()",
"  Return an object describing the following ICU options.\n\n"
"    version: a string containing the ICU version number, e.g. '67.1'\n"
"    unicode: a string containing the Unicode version number, e.g. '13.0'\n"
"    locale: the ICU default locale, e.g. 'en_US'\n"
"    tzdata: a string containing the tzdata version number, e.g. '2020a'\n"
"    timezone: the ICU default time zone, e.g. 'America/Los_Angeles'\n"
"    host-timezone: the host time zone, e.g. 'America/Los_Angeles'"),

JS_FN_HELP("getAvailableLocalesOf", GetAvailableLocalesOf, 0, 0,
"getAvailableLocalesOf(name)",
"  Return an array of all available locales for the given Intl constuctor."),

JS_FN_HELP("isSmallFunction", IsSmallFunction, 1, 0,
"isSmallFunction(fun)",
"  Returns true if a scripted function is small enough to be inlinable."),

    JS_FN_HELP("compileToStencil", CompileToStencil, 1, 0,
"compileToStencil(string, [options])",
"  Parses the given string argument as js script, returns the stencil"
"  for it."),

    JS_FN_HELP("evalStencil", EvalStencil, 1, 0,
"evalStencil(stencil, [options])",
"  Instantiates the given stencil, and evaluates the top-level script it"
"  defines."),

    JS_FN_HELP("compileToStencilXDR", CompileToStencilXDR, 1, 0,
"compileToStencilXDR(string, [options])",
"  Parses the given string argument as js script, produces the stencil"
"  for it, XDR-encodes the stencil, and returns an object that contains the"
"  XDR buffer."),

    JS_FN_HELP("evalStencilXDR", EvalStencilXDR, 1, 0,
"evalStencilXDR(stencilXDR, [options])",
"  Reads the given stencil XDR object, and evaluates the top-level script it"
"  defines."),

    JS_FN_HELP("getExceptionInfo", GetExceptionInfo, 1, 0,
"getExceptionInfo(fun)",
"  Calls the given function and returns information about the exception it"
"  throws. Returns null if the function didn't throw an exception."),

    JS_FN_HELP("nukeCCW", NukeCCW, 1, 0,
"nukeCCW(wrapper)",
"  Nuke a CrossCompartmentWrapper, which turns it into a DeadProxyObject."),

  JS_FN_HELP("assertRealmFuseInvariants", AssertRealmFuseInvariants, 0, 0,
  "assertRealmFuseInvariants()",
  " Runs the realm's fuse invariant checks -- these will crash on failure. "
  " Only available in fuzzing or debug builds, so usage should be guarded. "),

  JS_FN_HELP("popAllFusesInRealm", PopAllFusesInRealm, 0, 0,
  "popAllFusesInRealm()",
  " Pops all the fuses in the current realm"),

    JS_FN_HELP("getAllPrefNames", GetAllPrefNames, 0, 0,
"getAllPrefNames()",
"  Returns an array containing the names of all JS prefs."),

    JS_FN_HELP("getPrefValue", GetPrefValue, 1, 0,
"getPrefValue(name)",
"  Return the value of the JS pref with the given name."),

  JS_FS_HELP_END
};
// clang-format on

// clang-format off
static const JSFunctionSpecWithHelp FuzzingUnsafeTestingFunctions[] = {
    JS_FN_HELP("getErrorNotes", GetErrorNotes, 1, 0,
"getErrorNotes(error)",
"  Returns an array of error notes."),

    JS_FN_HELP("setTimeZone", SetTimeZone, 1, 0,
"setTimeZone(tzname)",
"  Set the 'TZ' environment variable to the given time zone and applies the new time zone.\n"
"  The time zone given is validated according to the current environment.\n"
"  An empty string or undefined resets the time zone to its default value."),

JS_FN_HELP("setDefaultLocale", SetDefaultLocale, 1, 0,
"setDefaultLocale(locale)",
"  Set the runtime default locale to the given value.\n"
"  An empty string or undefined resets the runtime locale to its default value.\n"
"  NOTE: The input string is not fully validated, it must be a valid BCP-47 language tag."),

JS_FN_HELP("isInStencilCache", IsInStencilCache, 1, 0,
"isInStencilCache(fun)",
"  True if fun is available in the stencil cache."),

JS_FN_HELP("waitForStencilCache", WaitForStencilCache, 1, 0,
"waitForStencilCache(fun)",
"  Block main thread execution until the function is made available in the cache."),

JS_FN_HELP("getInnerMostEnvironmentObject", GetInnerMostEnvironmentObject, 0, 0,
"getInnerMostEnvironmentObject()",
"  Return the inner-most environment object for current execution."),

JS_FN_HELP("getEnclosingEnvironmentObject", GetEnclosingEnvironmentObject, 1, 0,
"getEnclosingEnvironmentObject(env)",
"  Return the enclosing environment object for given environment object."),

JS_FN_HELP("getEnvironmentObjectType", GetEnvironmentObjectType, 1, 0,
"getEnvironmentObjectType(env)",
"  Return a string represents the type of given environment object."),

    JS_FN_HELP("shortestPaths", ShortestPaths, 3, 0,
"shortestPaths(targets, options)",
"  Return an array of arrays of shortest retaining paths. There is an array of\n"
      "  shortest retaining paths for each object in |targets|. Each element in a path\n"
      "  is of the form |{ predecessor, edge }|. |options| may contain:\n"
      "  \n"
      "    maxNumPaths: The maximum number of paths returned in each of those arrays\n"
      "      (default 3).\n"
      "    start: The object to start all paths from. If not given, then\n"
      "      the starting point will be the set of GC roots."),

    JS_FN_HELP("getFuseState", GetFuseState, 0, 0,
      "getFuseState()",
      "  Return an object describing the calling realm's fuse state, "
      "as well as the state of any runtime fuses."),

#if defined(DEBUG) || defined(JS_JITSPEW)
    JS_FN_HELP("dumpObject", DumpObject, 1, 0,
"dumpObject(obj)",
"  Dump an internal representation of an object."),

    JS_FN_HELP("dumpValue", DumpValue, 1, 0,
"dumpValue(v)",
"  Dump an internal representation of a value."),

    JS_FN_HELP("dumpValueToString", DumpValueToString, 1, 0,
"dumpValue(v)",
"  Return a dump of an internal representation of a value."),

    JS_FN_HELP("dumpStringRepresentation", DumpStringRepresentation, 1, 0,
"dumpStringRepresentation(str)",
"  Print a human-readable description of how the string |str| is represented.\n"),

    JS_FN_HELP("stringRepresentation", GetStringRepresentation, 1, 0,
"stringRepresentation(str)",
"  Return a human-readable description of how the string |str| is represented.\n"),

#endif

    JS_FS_HELP_END
};
// clang-format on

// clang-format off
static const JSFunctionSpecWithHelp PCCountProfilingTestingFunctions[] = {
    JS_FN_HELP("start", PCCountProfiling_Start, 0, 0,
    "start()",
    "  Start PC count profiling."),

    JS_FN_HELP("stop", PCCountProfiling_Stop, 0, 0,
    "stop()",
    "  Stop PC count profiling."),

    JS_FN_HELP("purge", PCCountProfiling_Purge, 0, 0,
    "purge()",
    "  Purge the collected PC count profiling data."),

    JS_FN_HELP("count", PCCountProfiling_ScriptCount, 0, 0,
    "count()",
    "  Return the number of profiled scripts."),

    JS_FN_HELP("summary", PCCountProfiling_ScriptSummary, 1, 0,
    "summary(index)",
    "  Return the PC count profiling summary for the given script index.\n"
    "  The script index must be in the range [0, pc.count())."),

    JS_FN_HELP("contents", PCCountProfiling_ScriptContents, 1, 0,
    "contents(index)",
    "  Return the complete profiling contents for the given script index.\n"
    "  The script index must be in the range [0, pc.count())."),

    JS_FS_HELP_END
};
// clang-format on

// clang-format off
static const JSFunctionSpecWithHelp FdLibMTestingFunctions[] = {
    JS_FN_HELP("pow", FdLibM_Pow, 2, 0,
    "pow(x, y)",
    "  Return x ** y."),

    JS_FS_HELP_END
};
// clang-format on

bool js::InitTestingFunctions() { return disasmBuf.init(); }

bool js::DefineTestingFunctions(JSContext* cx, HandleObject obj,
                                bool fuzzingSafe_, bool disableOOMFunctions_) {
  fuzzingSafe = fuzzingSafe_;
  if (EnvVarIsDefined("MOZ_FUZZING_SAFE")) {
    fuzzingSafe = true;
  }

  disableOOMFunctions = disableOOMFunctions_;

  if (!fuzzingSafe) {
    if (!JS_DefineFunctionsWithHelp(cx, obj, FuzzingUnsafeTestingFunctions)) {
      return false;
    }

    RootedObject pccount(cx, JS_NewPlainObject(cx));
    if (!pccount) {
      return false;
    }

    if (!JS_DefineProperty(cx, obj, "pccount", pccount, 0)) {
      return false;
    }

    if (!JS_DefineFunctionsWithHelp(cx, pccount,
                                    PCCountProfilingTestingFunctions)) {
      return false;
    }
  }

  RootedObject fdlibm(cx, JS_NewPlainObject(cx));
  if (!fdlibm) {
    return false;
  }

  if (!JS_DefineProperty(cx, obj, "fdlibm", fdlibm, 0)) {
    return false;
  }

  if (!JS_DefineFunctionsWithHelp(cx, fdlibm, FdLibMTestingFunctions)) {
    return false;
  }

  return JS_DefineFunctionsWithHelp(cx, obj, TestingFunctions);
}

#ifdef FUZZING_JS_FUZZILLI
uint32_t js::FuzzilliHashDouble(double value) {
  // We shouldn't GC here as this is called directly from IC code.
  AutoUnsafeCallWithABI unsafe;
  uint64_t v = mozilla::BitwiseCast<uint64_t>(value);
  return static_cast<uint32_t>(v) + static_cast<uint32_t>(v >> 32);
}

uint32_t js::FuzzilliHashBigInt(BigInt* bigInt) {
  // We shouldn't GC here as this is called directly from IC code.
  AutoUnsafeCallWithABI unsafe;
  return bigInt->hash();
}

void js::FuzzilliHashObject(JSContext* cx, JSObject* obj) {
  // called from IC and baseline/interpreter
  uint32_t hash;
  FuzzilliHashObjectInl(cx, obj, &hash);

  cx->executionHashInputs += 1;
  cx->executionHash = mozilla::RotateLeft(cx->executionHash + hash, 1);
}

void js::FuzzilliHashObjectInl(JSContext* cx, JSObject* obj, uint32_t* out) {
  *out = 0;
  if (!js::SupportDifferentialTesting()) {
    return;
  }

  RootedValue v(cx);
  v.setObject(*obj);

  JSAutoStructuredCloneBuffer JSCloner(
      JS::StructuredCloneScope::DifferentProcess, nullptr, nullptr);
  if (JSCloner.write(cx, v)) {
    JSStructuredCloneData& data = JSCloner.data();
    data.ForEachDataChunk([&](const char* aData, size_t aSize) {
      uint32_t h = mozilla::HashBytes(aData, aSize);
      h = (h << 1) | 1;
      *out ^= h;
      *out *= h;
      return true;
    });
  } else if (JS_IsExceptionPending(cx)) {
    JS_ClearPendingException(cx);
  }
}
#endif
