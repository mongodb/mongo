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
#include "vm/JSObject.h"
#include "vm/StringType.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmLog.h"

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

void Val::readFromRootedLocation(const void* loc) {
  memset(&cell_, 0, sizeof(Cell));
  memcpy(&cell_, loc, type_.size());
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
  if (type_.isRefRepr()) {
    // TODO/AnyRef-boxing: With boxed immediates and strings, the write
    // barrier is going to have to be more complicated.
    *((GCPtr<JSObject*>*)loc) = cell_.ref_.asJSObject();
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
  if (isJSObject()) {
    // TODO/AnyRef-boxing: With boxed immediates and strings, the write
    // barrier is going to have to be more complicated.
    ASSERT_ANYREF_IS_JSOBJECT;
    TraceManuallyBarrieredEdge(trc, asJSObjectAddress(), "wasm val");
  }
}

bool wasm::CheckRefType(JSContext* cx, RefType targetType, HandleValue v,
                        MutableHandleFunction fnval,
                        MutableHandleAnyRef refval) {
  if (!targetType.isNullable() && v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_REF_NONNULLABLE_VALUE);
    return false;
  }

  switch (targetType.kind()) {
    case RefType::Func:
      return CheckFuncRefValue(cx, v, fnval);
    case RefType::Extern:
      return BoxAnyRef(cx, v, refval);
    case RefType::Any:
      return CheckAnyRefValue(cx, v, refval);
    case RefType::NoFunc:
      return CheckNullFuncRefValue(cx, v, fnval);
    case RefType::NoExtern:
      return CheckNullExternRefValue(cx, v, refval);
    case RefType::None:
      return CheckNullRefValue(cx, v, refval);
    case RefType::Eq:
      return CheckEqRefValue(cx, v, refval);
    case RefType::Struct:
      return CheckStructRefValue(cx, v, refval);
    case RefType::Array:
      return CheckArrayRefValue(cx, v, refval);
    case RefType::TypeRef:
      return CheckTypeRefValue(cx, targetType.typeDef(), v, refval);
  }

  MOZ_ASSERT(!ValType(targetType).isExposable());
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_VAL_TYPE);
  return false;
}

bool wasm::CheckFuncRefValue(JSContext* cx, HandleValue v,
                             MutableHandleFunction fun) {
  if (v.isNull()) {
    MOZ_ASSERT(!fun);
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<JSFunction>()) {
      JSFunction* f = &obj.as<JSFunction>();
      if (IsWasmExportedFunction(f)) {
        fun.set(f);
        return true;
      }
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_FUNCREF_VALUE);
  return false;
}

bool wasm::CheckAnyRefValue(JSContext* cx, HandleValue v,
                            MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<WasmGcObject>()) {
      vp.set(AnyRef::fromJSObject(&obj.as<WasmGcObject>()));
      return true;
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_ANYREF_VALUE);
  return false;
}

bool wasm::CheckNullFuncRefValue(JSContext* cx, HandleValue v,
                                 MutableHandleFunction fun) {
  if (!v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_NULL_FUNCREF_VALUE);
    return false;
  }
  MOZ_ASSERT(!fun);
  return true;
}

bool wasm::CheckNullExternRefValue(JSContext* cx, HandleValue v,
                                   MutableHandleAnyRef vp) {
  if (!v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_NULL_EXTERNREF_VALUE);
    return false;
  }

  vp.set(AnyRef::null());
  return true;
}

bool wasm::CheckNullRefValue(JSContext* cx, HandleValue v,
                             MutableHandleAnyRef vp) {
  if (!v.isNull()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_NULL_ANYREF_VALUE);
    return false;
  }

  vp.set(AnyRef::null());
  return true;
}

bool wasm::CheckEqRefValue(JSContext* cx, HandleValue v,
                           MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<WasmGcObject>()) {
      vp.set(AnyRef::fromJSObject(&obj.as<WasmGcObject>()));
      return true;
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_EQREF_VALUE);
  return false;
}

bool wasm::CheckStructRefValue(JSContext* cx, HandleValue v,
                               MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<WasmStructObject>()) {
      vp.set(AnyRef::fromJSObject(&obj.as<WasmStructObject>()));
      return true;
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_STRUCTREF_VALUE);
  return false;
}

bool wasm::CheckArrayRefValue(JSContext* cx, HandleValue v,
                              MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<WasmArrayObject>()) {
      vp.set(AnyRef::fromJSObject(&obj.as<WasmArrayObject>()));
      return true;
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_ARRAYREF_VALUE);
  return false;
}

bool wasm::CheckTypeRefValue(JSContext* cx, const TypeDef* typeDef,
                             HandleValue v, MutableHandleAnyRef vp) {
  if (v.isNull()) {
    vp.set(AnyRef::null());
    return true;
  }

  if (v.isObject()) {
    JSObject& obj = v.toObject();
    if (obj.is<WasmGcObject>() &&
        obj.as<WasmGcObject>().isRuntimeSubtypeOf(typeDef)) {
      vp.set(AnyRef::fromJSObject(&obj.as<WasmGcObject>()));
      return true;
    }
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_TYPEREF_VALUE);
  return false;
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

template bool wasm::ToWebAssemblyValue<NoDebug>(JSContext* cx, HandleValue val,
                                                FieldType type, void* loc,
                                                bool mustWrite64,
                                                CoercionLevel level);
template bool wasm::ToWebAssemblyValue<DebugCodegenVal>(
    JSContext* cx, HandleValue val, FieldType type, void* loc, bool mustWrite64,
    CoercionLevel level);
template bool wasm::ToJSValue<NoDebug>(JSContext* cx, const void* src,
                                       FieldType type, MutableHandleValue dst,
                                       CoercionLevel level);
template bool wasm::ToJSValue<DebugCodegenVal>(JSContext* cx, const void* src,
                                               FieldType type,
                                               MutableHandleValue dst,
                                               CoercionLevel level);
template bool wasm::ToJSValueMayGC<NoDebug>(FieldType type);
template bool wasm::ToJSValueMayGC<DebugCodegenVal>(FieldType type);

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
bool ToWebAssemblyValue_i8(JSContext* cx, HandleValue val, int8_t* loc) {
  bool ok = ToInt8(cx, val, loc);
  Debug::print(*loc);
  return ok;
}

template <typename Debug = NoDebug>
bool ToWebAssemblyValue_i16(JSContext* cx, HandleValue val, int16_t* loc) {
  bool ok = ToInt16(cx, val, loc);
  Debug::print(*loc);
  return ok;
}

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
  if (!BoxAnyRef(cx, val, &result)) {
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
  RootedFunction fun(cx);
  if (!CheckFuncRefValue(cx, val, &fun)) {
    return false;
  }
  loc[0] = fun;
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
  RootedFunction fun(cx);
  if (!CheckNullFuncRefValue(cx, val, &fun)) {
    return false;
  }
  loc[0] = fun;
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
  return ToWebAssemblyValue_anyref(cx, val, loc, mustWrite64);
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
bool wasm::ToWebAssemblyValue(JSContext* cx, HandleValue val, FieldType type,
                              void* loc, bool mustWrite64,
                              CoercionLevel level) {
  if (level == CoercionLevel::Lossless &&
      ToWebAssemblyValue_lossless(cx, val, type.valType(), (void*)loc,
                                  mustWrite64)) {
    return true;
  }

  switch (type.kind()) {
    case FieldType::I8:
      return ToWebAssemblyValue_i8<Debug>(cx, val, (int8_t*)loc);
    case FieldType::I16:
      return ToWebAssemblyValue_i16<Debug>(cx, val, (int16_t*)loc);
    case FieldType::I32:
      return ToWebAssemblyValue_i32<Debug>(cx, val, (int32_t*)loc, mustWrite64);
    case FieldType::I64:
      return ToWebAssemblyValue_i64<Debug>(cx, val, (int64_t*)loc, mustWrite64);
    case FieldType::F32:
      return ToWebAssemblyValue_f32<Debug>(cx, val, (float*)loc, mustWrite64);
    case FieldType::F64:
      return ToWebAssemblyValue_f64<Debug>(cx, val, (double*)loc, mustWrite64);
    case FieldType::V128:
      break;
    case FieldType::Ref:
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
      if (!type.isNullable() && val.isNull()) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_BAD_REF_NONNULLABLE_VALUE);
        return false;
      }
#else
      MOZ_ASSERT(type.isNullable());
#endif
      switch (type.refTypeKind()) {
        case RefType::Func:
          return ToWebAssemblyValue_funcref<Debug>(cx, val, (void**)loc,
                                                   mustWrite64);
        case RefType::Extern:
          return ToWebAssemblyValue_externref<Debug>(cx, val, (void**)loc,
                                                     mustWrite64);
        case RefType::Any:
          return ToWebAssemblyValue_anyref<Debug>(cx, val, (void**)loc,
                                                  mustWrite64);
        case RefType::NoFunc:
          return ToWebAssemblyValue_nullfuncref<Debug>(cx, val, (void**)loc,
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

template <typename Debug>
bool wasm::ToWebAssemblyValue(JSContext* cx, HandleValue val, ValType type,
                              void* loc, bool mustWrite64,
                              CoercionLevel level) {
  return wasm::ToWebAssemblyValue(cx, val, FieldType(type.packed()), loc,
                                  mustWrite64, level);
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
  dst.set(UnboxAnyRef(AnyRef::fromCompiledCode(src)));
  Debug::print(src);
  return true;
}

template <typename Debug = NoDebug>
bool ToJSValue_anyref(JSContext* cx, void* src, MutableHandleValue dst) {
  dst.set(UnboxAnyRef(AnyRef::fromCompiledCode(src)));
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
  dst.set(ObjectValue(*srcGlobal.get()));
  return true;
}

template <typename Debug>
bool wasm::ToJSValue(JSContext* cx, const void* src, FieldType type,
                     MutableHandleValue dst, CoercionLevel level) {
  if (level == CoercionLevel::Lossless) {
    MOZ_ASSERT(type.isValType());
    return ToJSValue_lossless(cx, src, dst, type.valType());
  }

  switch (type.kind()) {
    case FieldType::I8:
      return ToJSValue_i8<Debug>(cx, *reinterpret_cast<const int8_t*>(src),
                                 dst);
    case FieldType::I16:
      return ToJSValue_i16<Debug>(cx, *reinterpret_cast<const int16_t*>(src),
                                  dst);
    case FieldType::I32:
      return ToJSValue_i32<Debug>(cx, *reinterpret_cast<const int32_t*>(src),
                                  dst);
    case FieldType::I64:
      return ToJSValue_i64<Debug>(cx, *reinterpret_cast<const int64_t*>(src),
                                  dst);
    case FieldType::F32:
      return ToJSValue_f32<Debug>(cx, *reinterpret_cast<const float*>(src),
                                  dst);
    case FieldType::F64:
      return ToJSValue_f64<Debug>(cx, *reinterpret_cast<const double*>(src),
                                  dst);
    case FieldType::V128:
      break;
    case FieldType::Ref:
      switch (type.refType().hierarchy()) {
        case RefTypeHierarchy::Func:
          return ToJSValue_funcref<Debug>(
              cx, *reinterpret_cast<void* const*>(src), dst);
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
bool wasm::ToJSValueMayGC(FieldType type) {
  return type.kind() == FieldType::I64;
}

template <typename Debug>
bool wasm::ToJSValue(JSContext* cx, const void* src, ValType type,
                     MutableHandleValue dst, CoercionLevel level) {
  return wasm::ToJSValue(cx, src, FieldType(type.packed()), dst, level);
}

template <typename Debug>
bool wasm::ToJSValueMayGC(ValType type) {
  return wasm::ToJSValueMayGC(FieldType(type.packed()));
}

void AnyRef::trace(JSTracer* trc) {
  if (value_) {
    TraceManuallyBarrieredEdge(trc, &value_, "wasm anyref referent");
  }
}

const JSClass WasmValueBox::class_ = {
    "WasmValueBox", JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS)};

WasmValueBox* WasmValueBox::create(JSContext* cx, HandleValue val) {
  WasmValueBox* obj = NewObjectWithGivenProto<WasmValueBox>(cx, nullptr);
  if (!obj) {
    return nullptr;
  }
  obj->setFixedSlot(VALUE_SLOT, val);
  return obj;
}

bool wasm::BoxAnyRef(JSContext* cx, HandleValue val,
                     MutableHandleAnyRef result) {
  if (val.isNull()) {
    result.set(AnyRef::null());
    return true;
  }

  if (val.isObject()) {
    JSObject* obj = &val.toObject();
    MOZ_ASSERT(!obj->is<WasmValueBox>());
    MOZ_ASSERT(obj->compartment() == cx->compartment());
    result.set(AnyRef::fromJSObject(obj));
    return true;
  }

  WasmValueBox* box = WasmValueBox::create(cx, val);
  if (!box) return false;
  result.set(AnyRef::fromJSObject(box));
  return true;
}

JSObject* wasm::BoxBoxableValue(JSContext* cx, HandleValue val) {
  MOZ_ASSERT(!val.isNull() && !val.isObject());
  return WasmValueBox::create(cx, val);
}

Value wasm::UnboxAnyRef(AnyRef val) {
  // If UnboxAnyRef needs to allocate then we need a more complicated API, and
  // we need to root the value in the callers, see comments in callExport().
  JSObject* obj = val.asJSObject();
  Value result;
  if (obj == nullptr) {
    result.setNull();
  } else if (obj->is<WasmValueBox>()) {
    result = obj->as<WasmValueBox>().value();
  } else {
    result.setObjectOrNull(obj);
  }
  return result;
}

/* static */
wasm::FuncRef wasm::FuncRef::fromAnyRefUnchecked(AnyRef p) {
#ifdef DEBUG
  Value v = UnboxAnyRef(p);
  if (v.isNull()) {
    return FuncRef(nullptr);
  }
  if (v.toObject().is<JSFunction>()) {
    return FuncRef(&v.toObject().as<JSFunction>());
  }
  MOZ_CRASH("Bad value");
#else
  return FuncRef(&p.asJSObject()->as<JSFunction>());
#endif
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
