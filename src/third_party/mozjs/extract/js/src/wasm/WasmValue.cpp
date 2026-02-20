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

#include "wasm/WasmValue.h"

#include "jsmath.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Printf.h"
#include "js/Value.h"
#include "vm/BigIntType.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/StringType.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmLog.h"
#include "wasm/WasmTypeDef.h"

#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::wasm;

Val::Val(const LitVal& val) {
  type_ = val.type();
  switch (type_.kind()) {
    case ValType::I32:
      cell_.i32_ = val.i32();
      return;
    case ValType::F32:
      cell_.f32_ = val.f32();
      return;
    case ValType::I64:
      cell_.i64_ = val.i64();
      return;
    case ValType::F64:
      cell_.f64_ = val.f64();
      return;
    case ValType::V128:
      cell_.v128_ = val.v128();
      return;
    case ValType::Ref:
      cell_.ref_ = val.ref();
      return;
  }
  MOZ_CRASH();
}

void Val::initFromRootedLocation(ValType type, const void* loc) {
  MOZ_ASSERT(!type_.isValid());
  type_ = type;
  memset(&cell_, 0, sizeof(Cell));
  memcpy(&cell_, loc, type_.size());
}

void Val::initFromHeapLocation(ValType type, const void* loc) {
  MOZ_ASSERT(!type_.isValid());
  type_ = type;
  memset(&cell_, 0, sizeof(Cell));
  readFromHeapLocation(loc);
}

void Val::writeToRootedLocation(void* loc, bool mustWrite64) const {
  memcpy(loc, &cell_, type_.size());
  if (mustWrite64 && type_.size() == 4) {
    memset((uint8_t*)(loc) + 4, 0, 4);
  }
}

void Val::readFromHeapLocation(const void* loc) {
  memcpy(&cell_, loc, type_.size());
}

void Val::writeToHeapLocation(void* loc) const {
  if (isAnyRef()) {
    *((GCPtr<AnyRef>*)loc) = toAnyRef();
    return;
  }
  memcpy(loc, &cell_, type_.size());
}

bool Val::fromJSValue(JSContext* cx, ValType targetType, HandleValue val,
                      MutableHandleVal rval) {
  rval.get().type_ = targetType;
  // No pre/post barrier needed as rval is rooted
  return ToWebAssemblyValue(cx, val, targetType, &rval.get().cell_,
                            targetType.size() == 8);
}

bool Val::toJSValue(JSContext* cx, MutableHandleValue rval) const {
  return ToJSValue(cx, &cell_, type_, rval);
}

void Val::trace(JSTracer* trc) const {
  if (isAnyRef()) {
    TraceManuallyBarrieredEdge(trc, &toAnyRef(), "anyref");
  }
}

bool CheckFuncRefValue(JSContext* cx, HandleValue v, MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<JSFunction>()) {
      JSFunction* f = &obj.as<JSFunction>();
      if (f->isWasm()) {
        vp.set(AnyRef::fromJSObject(*f));
        return true;
      }
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_FUNCREF_VALUE);
  return false;
}

bool CheckAnyRefValue(JSContext* cx, HandleValue v, MutableHandleAnyRef vp) {
  if (!AnyRef::fromJSValue(cx, v, vp)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return false;
  }
  return true;
}

bool CheckNullFuncRefValue(JSContext* cx, HandleValue v,
                           MutableHandleAnyRef vp) {
  if (!v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_NULL_FUNCREF_VALUE);
    return false;
  }
  vp.set(AnyRef::null());
  return true;
}

bool CheckNullExnRefValue(JSContext* cx, HandleValue v,
                          MutableHandleAnyRef vp) {
  if (!v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_NULL_EXNREF_VALUE);
    return false;
  }

  vp.set(AnyRef::null());
  return true;
}

bool CheckNullExternRefValue(JSContext* cx, HandleValue v,
                             MutableHandleAnyRef vp) {
  if (!v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_NULL_EXTERNREF_VALUE);
    return false;
  }

  vp.set(AnyRef::null());
  return true;
}

bool CheckNullRefValue(JSContext* cx, HandleValue v, MutableHandleAnyRef vp) {
  if (!v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_NULL_ANYREF_VALUE);
    return false;
  }

  vp.set(AnyRef::null());
  return true;
}

bool CheckEqRefValue(JSContext* cx, HandleValue v, MutableHandleAnyRef vp) {
  if (!AnyRef::fromJSValue(cx, v, vp)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return false;
  }

  if (vp.isNull() || vp.isI31() ||
      (vp.isJSObject() && vp.toJSObject().is<WasmGcObject>())) {
    return true;
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_EQREF_VALUE);
  return false;
}

bool CheckI31RefValue(JSContext* cx, HandleValue v, MutableHandleAnyRef vp) {
  if (!AnyRef::fromJSValue(cx, v, vp)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return false;
  }

  if (vp.isNull() || vp.isI31()) {
    return true;
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_I31REF_VALUE);
  return false;
}

bool CheckStructRefValue(JSContext* cx, HandleValue v, MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<WasmStructObject>()) {
      vp.set(AnyRef::fromJSObject(obj.as<WasmStructObject>()));
      return true;
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_STRUCTREF_VALUE);
  return false;
}

bool CheckArrayRefValue(JSContext* cx, HandleValue v, MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<WasmArrayObject>()) {
      vp.set(AnyRef::fromJSObject(obj.as<WasmArrayObject>()));
      return true;
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_ARRAYREF_VALUE);
  return false;
}

bool CheckTypeRefValue(JSContext* cx, const TypeDef* typeDef, HandleValue v,
                       MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<WasmGcObject>() &&
        obj.as<WasmGcObject>().isRuntimeSubtypeOf(typeDef)) {
      vp.set(AnyRef::fromJSObject(obj.as<WasmGcObject>()));
      return true;
    }
    if (obj.is<JSFunction>() && obj.as<JSFunction>().isWasm()) {
      JSFunction& funcObj = obj.as<JSFunction>();
      if (TypeDef::isSubTypeOf(funcObj.wasmTypeDef(), typeDef)) {
        vp.set(AnyRef::fromJSObject(funcObj));
        return true;
      }
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_TYPEREF_VALUE);
  return false;
}

bool wasm::CheckRefType(JSContext* cx, RefType targetType, HandleValue v,
                        MutableHandleAnyRef vp) {
  if (!targetType.isNullable() && v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_REF_NONNULLABLE_VALUE);
    return false;
  }

  switch (targetType.kind()) {
    case RefType::Func:
      return CheckFuncRefValue(cx, v, vp);
    case RefType::Extern:
      return AnyRef::fromJSValue(cx, v, vp);
    case RefType::Exn:
      // Break to the non-exposable case
      break;
    case RefType::Any:
      return CheckAnyRefValue(cx, v, vp);
    case RefType::NoFunc:
      return CheckNullFuncRefValue(cx, v, vp);
    case RefType::NoExn:
      return CheckNullExnRefValue(cx, v, vp);
    case RefType::NoExtern:
      return CheckNullExternRefValue(cx, v, vp);
    case RefType::None:
      return CheckNullRefValue(cx, v, vp);
    case RefType::Eq:
      return CheckEqRefValue(cx, v, vp);
    case RefType::I31:
      return CheckI31RefValue(cx, v, vp);
    case RefType::Struct:
      return CheckStructRefValue(cx, v, vp);
    case RefType::Array:
      return CheckArrayRefValue(cx, v, vp);
    case RefType::TypeRef:
      return CheckTypeRefValue(cx, targetType.typeDef(), v, vp);
  }

  MOZ_ASSERT(!ValType(targetType).isExposable());
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_VAL_TYPE);
  return false;
}

bool wasm::CheckRefType(JSContext* cx, RefType targetType, HandleValue v) {
  RootedAnyRef any(cx, AnyRef::null());
  return CheckRefType(cx, targetType, v, &any);
}

class wasm::NoDebug {
 public:
  template <typename T>
  static void print(T v) {}
};

class wasm::DebugCodegenVal {
  template <typename T>
  static void print(const char* fmt, T v) {
    DebugCodegen(DebugChannel::Function, fmt, v);
  }

 public:
  static void print(int32_t v) { print(" i32(%d)", v); }
  static void print(int64_t v) { print(" i64(%" PRId64 ")", v); }
  static void print(float v) { print(" f32(%f)", v); }
  static void print(double v) { print(" f64(%lf)", v); }
  static void print(void* v) { print(" ptr(%p)", v); }
};

template bool wasm::ToJSValue<NoDebug>(JSContext* cx, const void* src,
                                       StorageType type, MutableHandleValue dst,
                                       CoercionLevel level);
template bool wasm::ToJSValue<DebugCodegenVal>(JSContext* cx, const void* src,
                                               StorageType type,
                                               MutableHandleValue dst,
                                               CoercionLevel level);
template bool wasm::ToJSValueMayGC<NoDebug>(StorageType type);
template bool wasm::ToJSValueMayGC<DebugCodegenVal>(StorageType type);

template bool wasm::ToWebAssemblyValue<NoDebug>(JSContext* cx, HandleValue val,
                                                ValType type, void* loc,
                                                bool mustWrite64,
                                                CoercionLevel level);
template bool wasm::ToWebAssemblyValue<DebugCodegenVal>(JSContext* cx,
                                                        HandleValue val,
                                                        ValType type, void* loc,
                                                        bool mustWrite64,
                                                        CoercionLevel level);
template bool wasm::ToJSValue<NoDebug>(JSContext* cx, const void* src,
                                       ValType type, MutableHandleValue dst,
                                       CoercionLevel level);
template bool wasm::ToJSValue<DebugCodegenVal>(JSContext* cx, const void* src,
                                               ValType type,
                                               MutableHandleValue dst,
                                               CoercionLevel level);
template bool wasm::ToJSValueMayGC<NoDebug>(ValType type);
template bool wasm::ToJSValueMayGC<DebugCodegenVal>(ValType type);

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_i32(JSContext* cx, HandleValue val, int32_t* loc,
                            bool mustWrite64) {
  bool ok = ToInt32(cx, val, loc);
  if (ok && mustWrite64) {
#if defined(JS_CODEGEN_MIPS64)
    loc[1] = loc[0] >> 31;
#else
    loc[1] = 0;
#endif
  }
  Debug::print(*loc);
  return ok;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_i64(JSContext* cx, HandleValue val, int64_t* loc,
                            bool mustWrite64) {
  MOZ_ASSERT(mustWrite64);
  JS_TRY_VAR_OR_RETURN_FALSE(cx, *loc, ToBigInt64(cx, val));
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_f32(JSContext* cx, HandleValue val, float* loc,
                            bool mustWrite64) {
  bool ok = RoundFloat32(cx, val, loc);
  if (ok && mustWrite64) {
    loc[1] = 0.0;
  }
  Debug::print(*loc);
  return ok;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_f64(JSContext* cx, HandleValue val, double* loc,
                            bool mustWrite64) {
  MOZ_ASSERT(mustWrite64);
  bool ok = ToNumber(cx, val, loc);
  Debug::print(*loc);
  return ok;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_externref(JSContext* cx, HandleValue val, void** loc,
                                  bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!AnyRef::fromJSValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_nullexnref(JSContext* cx, HandleValue val, void** loc,
                                   bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckNullExnRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_nullexternref(JSContext* cx, HandleValue val,
                                      void** loc, bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckNullExternRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_funcref(JSContext* cx, HandleValue val, void** loc,
                                bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckFuncRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_nullfuncref(JSContext* cx, HandleValue val, void** loc,
                                    bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckNullFuncRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_anyref(JSContext* cx, HandleValue val, void** loc,
                               bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckAnyRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_nullref(JSContext* cx, HandleValue val, void** loc,
                                bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckNullRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_eqref(JSContext* cx, HandleValue val, void** loc,
                              bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckEqRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_i31ref(JSContext* cx, HandleValue val, void** loc,
                               bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckI31RefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_structref(JSContext* cx, HandleValue val, void** loc,
                                  bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckStructRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_arrayref(JSContext* cx, HandleValue val, void** loc,
                                 bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckArrayRefValue(cx, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_typeref(JSContext* cx, const TypeDef* typeDef,
                                HandleValue val, void** loc, bool mustWrite64) {
  RootedAnyRef result(cx, AnyRef::null());
  if (!CheckTypeRefValue(cx, typeDef, val, &result)) {
    return false;
  }
  loc[0] = result.get().forCompiledCode();
#ifndef JS_64BIT
  if (mustWrite64) {
    loc[1] = nullptr;
  }
#endif
  Debug::print(*loc);
  return true;
}

bool ToWebAssemblyValue_lossless(JSContext* cx, HandleValue val, ValType type,
                                 void* loc, bool mustWrite64) {
  if (!val.isObject() || !val.toObject().is<WasmGlobalObject>()) {
    return false;
  }
  Rooted<WasmGlobalObject*> srcVal(cx, &val.toObject().as<WasmGlobalObject>());

  if (srcVal->type() != type) {
    return false;
  }

  srcVal->val().get().writeToRootedLocation(loc, mustWrite64);
  return true;
}

template <typename Debug>
bool wasm::ToWebAssemblyValue(JSContext* cx, HandleValue val, ValType type,
                              void* loc, bool mustWrite64,
                              CoercionLevel level) {
  if (level == CoercionLevel::Lossless &&
      ToWebAssemblyValue_lossless(cx, val, type.valType(), (void*)loc,
                                  mustWrite64)) {
    return true;
  }

  switch (type.kind()) {
    case ValType::I32:
      return ToWebAssemblyValue_i32<Debug>(cx, val, (int32_t*)loc, mustWrite64);
    case ValType::I64:
      return ToWebAssemblyValue_i64<Debug>(cx, val, (int64_t*)loc, mustWrite64);
    case ValType::F32:
      return ToWebAssemblyValue_f32<Debug>(cx, val, (float*)loc, mustWrite64);
    case ValType::F64:
      return ToWebAssemblyValue_f64<Debug>(cx, val, (double*)loc, mustWrite64);
    case ValType::V128:
      break;
    case ValType::Ref:
      if (!type.isNullable() && val.isNull()) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_BAD_REF_NONNULLABLE_VALUE);
        return false;
      }
      switch (type.refTypeKind()) {
        case RefType::Func:
          return ToWebAssemblyValue_funcref<Debug>(cx, val, (void**)loc,
                                                   mustWrite64);
        case RefType::Extern:
          return ToWebAssemblyValue_externref<Debug>(cx, val, (void**)loc,
                                                     mustWrite64);
        case RefType::Exn:
          // Break to the non-exposable case
          break;
        case RefType::Any:
          return ToWebAssemblyValue_anyref<Debug>(cx, val, (void**)loc,
                                                  mustWrite64);
        case RefType::NoFunc:
          return ToWebAssemblyValue_nullfuncref<Debug>(cx, val, (void**)loc,
                                                       mustWrite64);
        case RefType::NoExn:
          return ToWebAssemblyValue_nullexnref<Debug>(cx, val, (void**)loc,
                                                      mustWrite64);
        case RefType::NoExtern:
          return ToWebAssemblyValue_nullexternref<Debug>(cx, val, (void**)loc,
                                                         mustWrite64);
        case RefType::None:
          return ToWebAssemblyValue_nullref<Debug>(cx, val, (void**)loc,
                                                   mustWrite64);
        case RefType::Eq:
          return ToWebAssemblyValue_eqref<Debug>(cx, val, (void**)loc,
                                                 mustWrite64);
        case RefType::I31:
          return ToWebAssemblyValue_i31ref<Debug>(cx, val, (void**)loc,
                                                  mustWrite64);
        case RefType::Struct:
          return ToWebAssemblyValue_structref<Debug>(cx, val, (void**)loc,
                                                     mustWrite64);
        case RefType::Array:
          return ToWebAssemblyValue_arrayref<Debug>(cx, val, (void**)loc,
                                                    mustWrite64);
        case RefType::TypeRef:
          return ToWebAssemblyValue_typeref<Debug>(cx, type.typeDef(), val,
                                                   (void**)loc, mustWrite64);
      }
  }

  MOZ_ASSERT(!type.isExposable());
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_VAL_TYPE);
  return false;
}

template <typename Debug = NoDebug>
bool ToJSValue_i8(JSContext* cx, int8_t src, MutableHandleValue dst) {
  dst.set(Int32Value(src));
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_i16(JSContext* cx, int16_t src, MutableHandleValue dst) {
  dst.set(Int32Value(src));
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_i32(JSContext* cx, int32_t src, MutableHandleValue dst) {
  dst.set(Int32Value(src));
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_i64(JSContext* cx, int64_t src, MutableHandleValue dst) {
  // If bi is manipulated other than test & storing, it would need
  // to be rooted here.
  BigInt* bi = BigInt::createFromInt64(cx, src);
  if (!bi) {
    return false;
  }
  dst.set(BigIntValue(bi));
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_f32(JSContext* cx, float src, MutableHandleValue dst) {
  dst.set(JS::CanonicalizedDoubleValue(src));
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_f64(JSContext* cx, double src, MutableHandleValue dst) {
  dst.set(JS::CanonicalizedDoubleValue(src));
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_funcref(JSContext* cx, void* src, MutableHandleValue dst) {
  dst.set(UnboxFuncRef(FuncRef::fromCompiledCode(src)));
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_externref(JSContext* cx, void* src, MutableHandleValue dst) {
  dst.set(AnyRef::fromCompiledCode(src).toJSValue());
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_anyref(JSContext* cx, void* src, MutableHandleValue dst) {
  dst.set(AnyRef::fromCompiledCode(src).toJSValue());
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_lossless(JSContext* cx, const void* src, MutableHandleValue dst,
                        ValType type) {
  RootedVal srcVal(cx);
  srcVal.get().initFromRootedLocation(type, src);
  RootedObject prototype(
      cx, GlobalObject::getOrCreatePrototype(cx, JSProto_WasmGlobal));
  Rooted<WasmGlobalObject*> srcGlobal(
      cx, WasmGlobalObject::create(cx, srcVal, false, prototype));
  if (!srcGlobal) {
    return false;
  }
  dst.set(ObjectValue(*srcGlobal.get()));
  return true;
}

template <typename Debug>
bool wasm::ToJSValue(JSContext* cx, const void* src, StorageType type,
                     MutableHandleValue dst, CoercionLevel level) {
  if (level == CoercionLevel::Lossless) {
    MOZ_ASSERT(type.isValType());
    return ToJSValue_lossless(cx, src, dst, type.valType());
  }

  switch (type.kind()) {
    case StorageType::I8:
      return ToJSValue_i8<Debug>(cx, *reinterpret_cast<const int8_t*>(src),
                                 dst);
    case StorageType::I16:
      return ToJSValue_i16<Debug>(cx, *reinterpret_cast<const int16_t*>(src),
                                  dst);
    case StorageType::I32:
      return ToJSValue_i32<Debug>(cx, *reinterpret_cast<const int32_t*>(src),
                                  dst);
    case StorageType::I64:
      return ToJSValue_i64<Debug>(cx, *reinterpret_cast<const int64_t*>(src),
                                  dst);
    case StorageType::F32:
      return ToJSValue_f32<Debug>(cx, *reinterpret_cast<const float*>(src),
                                  dst);
    case StorageType::F64:
      return ToJSValue_f64<Debug>(cx, *reinterpret_cast<const double*>(src),
                                  dst);
    case StorageType::V128:
      break;
    case StorageType::Ref:
      switch (type.refType().hierarchy()) {
        case RefTypeHierarchy::Func:
          return ToJSValue_funcref<Debug>(
              cx, *reinterpret_cast<void* const*>(src), dst);
        case RefTypeHierarchy::Exn:
          // Break to the non-exposable case
          break;
        case RefTypeHierarchy::Extern:
          return ToJSValue_externref<Debug>(
              cx, *reinterpret_cast<void* const*>(src), dst);
        case RefTypeHierarchy::Any:
          return ToJSValue_anyref<Debug>(
              cx, *reinterpret_cast<void* const*>(src), dst);
          break;
      }
  }
  MOZ_ASSERT(!type.isExposable());
  Debug::print(nullptr);
  dst.setUndefined();
  return true;
}

template <typename Debug>
bool wasm::ToJSValueMayGC(StorageType type) {
  return type.kind() == StorageType::I64;
}

template <typename Debug>
bool wasm::ToJSValue(JSContext* cx, const void* src, ValType type,
                     MutableHandleValue dst, CoercionLevel level) {
  return wasm::ToJSValue(cx, src, StorageType(type.packed()), dst, level);
}

template <typename Debug>
bool wasm::ToJSValueMayGC(ValType type) {
  return wasm::ToJSValueMayGC(StorageType(type.packed()));
}

/* static */
wasm::FuncRef wasm::FuncRef::fromAnyRefUnchecked(AnyRef p) {
  if (p.isNull()) {
    return FuncRef::null();
  }

  MOZ_ASSERT(p.isJSObject() && p.toJSObject().is<JSFunction>());
  return FuncRef(&p.toJSObject().as<JSFunction>());
}

void wasm::FuncRef::trace(JSTracer* trc) const {
  if (value_) {
    TraceManuallyBarrieredEdge(trc, &value_, "wasm funcref referent");
  }
}

Value wasm::UnboxFuncRef(FuncRef val) {
  JSFunction* fn = val.asJSFunction();
  Value result;
  MOZ_ASSERT_IF(fn, fn->is<JSFunction>());
  result.setObjectOrNull(fn);
  return result;
}
