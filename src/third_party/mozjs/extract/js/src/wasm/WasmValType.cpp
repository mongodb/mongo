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

#include "wasm/WasmValType.h"

#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Printf.h"
#include "js/Value.h"
#include "vm/StringType.h"
#include "wasm/WasmJS.h"

using namespace js;
using namespace js::wasm;

bool wasm::ToValType(JSContext* cx, HandleValue v, ValType* out) {
  RootedString typeStr(cx, ToString(cx, v));
  if (!typeStr) {
    return false;
  }

  RootedLinearString typeLinearStr(cx, typeStr->ensureLinear(cx));
  if (!typeLinearStr) {
    return false;
  }

  if (StringEqualsLiteral(typeLinearStr, "i32")) {
    *out = ValType::I32;
  } else if (StringEqualsLiteral(typeLinearStr, "i64")) {
    *out = ValType::I64;
  } else if (StringEqualsLiteral(typeLinearStr, "f32")) {
    *out = ValType::F32;
  } else if (StringEqualsLiteral(typeLinearStr, "f64")) {
    *out = ValType::F64;
#ifdef ENABLE_WASM_SIMD
  } else if (SimdAvailable(cx) && StringEqualsLiteral(typeLinearStr, "v128")) {
    *out = ValType::V128;
#endif
  } else if (StringEqualsLiteral(typeLinearStr, "funcref")) {
    *out = RefType::func();
  } else if (StringEqualsLiteral(typeLinearStr, "externref")) {
    *out = RefType::extern_();
#ifdef ENABLE_WASM_GC
  } else if (GcAvailable(cx) && StringEqualsLiteral(typeLinearStr, "eqref")) {
    *out = RefType::eq();
#endif
  } else {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_STRING_VAL_TYPE);
    return false;
  }

  return true;
}

UniqueChars wasm::ToString(ValType type) {
  const char* literal = nullptr;
  switch (type.kind()) {
    case ValType::I32:
      literal = "i32";
      break;
    case ValType::I64:
      literal = "i64";
      break;
    case ValType::V128:
      literal = "v128";
      break;
    case ValType::F32:
      literal = "f32";
      break;
    case ValType::F64:
      literal = "f64";
      break;
    case ValType::Ref:
      if (type.isNullable() && !type.isTypeIndex()) {
        switch (type.refTypeKind()) {
          case RefType::Func:
            literal = "funcref";
            break;
          case RefType::Extern:
            literal = "externref";
            break;
          case RefType::Eq:
            literal = "eqref";
            break;
          case RefType::TypeIndex:
            MOZ_ASSERT_UNREACHABLE();
        }
      } else {
        const char* heapType = nullptr;
        switch (type.refTypeKind()) {
          case RefType::Func:
            heapType = "func";
            break;
          case RefType::Extern:
            heapType = "extern";
            break;
          case RefType::Eq:
            heapType = "eq";
            break;
          case RefType::TypeIndex:
            return JS_smprintf("(ref %s%d)", type.isNullable() ? "null " : "",
                               type.refType().typeIndex());
        }
        return JS_smprintf("(ref %s%s)", type.isNullable() ? "null " : "",
                           heapType);
      }
      break;
    case ValType::Rtt:
      return JS_smprintf("(rtt %d %d)", type.rttDepth(), type.typeIndex());
  }
  return JS_smprintf("%s", literal);
}

UniqueChars wasm::ToString(const Maybe<ValType>& type) {
  return type ? ToString(type.ref()) : JS_smprintf("%s", "void");
}
