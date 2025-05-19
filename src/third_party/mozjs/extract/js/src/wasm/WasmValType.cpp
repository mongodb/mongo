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

#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Printf.h"
#include "js/Value.h"

#include "vm/JSAtomUtils.h"  // Atomize
#include "vm/JSObject.h"
#include "vm/StringType.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmJS.h"

#include "vm/JSAtomUtils-inl.h"  // AtomToId
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::wasm;

RefType RefType::topType() const {
  switch (kind()) {
    case RefType::Any:
    case RefType::Eq:
    case RefType::I31:
    case RefType::Array:
    case RefType::Struct:
    case RefType::None:
      return RefType::any();
    case RefType::Func:
    case RefType::NoFunc:
      return RefType::func();
    case RefType::Extern:
    case RefType::NoExtern:
      return RefType::extern_();
    case RefType::Exn:
    case RefType::NoExn:
      return RefType::exn();
    case RefType::TypeRef:
      switch (typeDef()->kind()) {
        case TypeDefKind::Array:
        case TypeDefKind::Struct:
          return RefType::any();
        case TypeDefKind::Func:
          return RefType::func();
        case TypeDefKind::None:
          MOZ_CRASH("should not see TypeDefKind::None at this point");
      }
  }
  MOZ_CRASH("switch is exhaustive");
}

TypeDefKind RefType::typeDefKind() const {
  switch (kind()) {
    case RefType::Struct:
      return TypeDefKind::Struct;
    case RefType::Array:
      return TypeDefKind::Array;
    case RefType::Func:
      return TypeDefKind::Func;
    default:
      return TypeDefKind::None;
  }
  MOZ_CRASH("switch is exhaustive");
}

static bool ToRefType(JSContext* cx, JSLinearString* typeLinearStr,
                      RefType* out) {
  if (StringEqualsLiteral(typeLinearStr, "anyfunc") ||
      StringEqualsLiteral(typeLinearStr, "funcref")) {
    // The JS API uses "anyfunc" uniformly as the external name of funcref.  We
    // also allow "funcref" for compatibility with code we've already shipped.
    *out = RefType::func();
    return true;
  }
  if (StringEqualsLiteral(typeLinearStr, "externref")) {
    *out = RefType::extern_();
    return true;
  }
  if (ExnRefAvailable(cx)) {
    if (StringEqualsLiteral(typeLinearStr, "exnref")) {
      *out = RefType::exn();
      return true;
    }
  }
#ifdef ENABLE_WASM_GC
  if (GcAvailable(cx)) {
    if (StringEqualsLiteral(typeLinearStr, "anyref")) {
      *out = RefType::any();
      return true;
    }
    if (StringEqualsLiteral(typeLinearStr, "eqref")) {
      *out = RefType::eq();
      return true;
    }
    if (StringEqualsLiteral(typeLinearStr, "i31ref")) {
      *out = RefType::i31();
      return true;
    }
    if (StringEqualsLiteral(typeLinearStr, "structref")) {
      *out = RefType::struct_();
      return true;
    }
    if (StringEqualsLiteral(typeLinearStr, "arrayref")) {
      *out = RefType::array();
      return true;
    }
    if (StringEqualsLiteral(typeLinearStr, "nullfuncref")) {
      *out = RefType::nofunc();
      return true;
    }
    if (StringEqualsLiteral(typeLinearStr, "nullexternref")) {
      *out = RefType::noextern();
      return true;
    }
    if (StringEqualsLiteral(typeLinearStr, "nullexnref")) {
      *out = RefType::noexn();
      return true;
    }
    if (StringEqualsLiteral(typeLinearStr, "nullref")) {
      *out = RefType::none();
      return true;
    }
  }
#endif

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_STRING_VAL_TYPE);
  return false;
}

enum class RefTypeResult {
  Failure,
  Parsed,
  Unparsed,
};

bool wasm::ToValType(JSContext* cx, HandleValue v, ValType* out) {
  RootedString typeStr(cx, ToString(cx, v));
  if (!typeStr) {
    return false;
  }

  Rooted<JSLinearString*> typeLinearStr(cx, typeStr->ensureLinear(cx));
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
  } else {
    RefType rt;
    if (ToRefType(cx, typeLinearStr, &rt)) {
      *out = ValType(rt);
    } else {
      // ToRefType will report an error when it fails, just return false
      return false;
    }
  }

  return true;
}

bool wasm::ToRefType(JSContext* cx, HandleValue v, RefType* out) {
  RootedString typeStr(cx, ToString(cx, v));
  if (!typeStr) {
    return false;
  }

  Rooted<JSLinearString*> typeLinearStr(cx, typeStr->ensureLinear(cx));
  if (!typeLinearStr) {
    return false;
  }

  return ToRefType(cx, typeLinearStr, out);
}

UniqueChars wasm::ToString(RefType type, const TypeContext* types) {
  // Try to emit a shorthand version first
  if (type.isNullable() && !type.isTypeRef()) {
    const char* literal = nullptr;
    switch (type.kind()) {
      case RefType::Func:
        literal = "funcref";
        break;
      case RefType::Extern:
        literal = "externref";
        break;
      case RefType::Exn:
        literal = "exnref";
        break;
      case RefType::Any:
        literal = "anyref";
        break;
      case RefType::NoFunc:
        literal = "nullfuncref";
        break;
      case RefType::NoExn:
        literal = "nullexnref";
        break;
      case RefType::NoExtern:
        literal = "nullexternref";
        break;
      case RefType::None:
        literal = "nullref";
        break;
      case RefType::Eq:
        literal = "eqref";
        break;
      case RefType::I31:
        literal = "i31ref";
        break;
      case RefType::Struct:
        literal = "structref";
        break;
      case RefType::Array:
        literal = "arrayref";
        break;
      case RefType::TypeRef: {
        MOZ_CRASH("type ref should not be possible here");
      }
    }
    return DuplicateString(literal);
  }

  // Emit the full reference type with heap type
  const char* heapType = nullptr;
  switch (type.kind()) {
    case RefType::Func:
      heapType = "func";
      break;
    case RefType::Extern:
      heapType = "extern";
      break;
    case RefType::Exn:
      heapType = "exn";
      break;
    case RefType::Any:
      heapType = "any";
      break;
    case RefType::NoFunc:
      heapType = "nofunc";
      break;
    case RefType::NoExn:
      heapType = "noexn";
      break;
    case RefType::NoExtern:
      heapType = "noextern";
      break;
    case RefType::None:
      heapType = "none";
      break;
    case RefType::Eq:
      heapType = "eq";
      break;
    case RefType::I31:
      heapType = "i31";
      break;
    case RefType::Struct:
      heapType = "struct";
      break;
    case RefType::Array:
      heapType = "array";
      break;
    case RefType::TypeRef: {
      if (types) {
        uint32_t typeIndex = types->indexOf(*type.typeDef());
        return JS_smprintf("(ref %s%d)", type.isNullable() ? "null " : "",
                           typeIndex);
      }
      return JS_smprintf("(ref %s?)", type.isNullable() ? "null " : "");
    }
  }
  return JS_smprintf("(ref %s%s)", type.isNullable() ? "null " : "", heapType);
}

UniqueChars wasm::ToString(ValType type, const TypeContext* types) {
  return ToString(type.storageType(), types);
}

UniqueChars wasm::ToString(StorageType type, const TypeContext* types) {
  const char* literal = nullptr;
  switch (type.kind()) {
    case StorageType::I8:
      literal = "i8";
      break;
    case StorageType::I16:
      literal = "i16";
      break;
    case StorageType::I32:
      literal = "i32";
      break;
    case StorageType::I64:
      literal = "i64";
      break;
    case StorageType::V128:
      literal = "v128";
      break;
    case StorageType::F32:
      literal = "f32";
      break;
    case StorageType::F64:
      literal = "f64";
      break;
    case StorageType::Ref:
      return ToString(type.refType(), types);
  }
  return DuplicateString(literal);
}

UniqueChars wasm::ToString(const Maybe<ValType>& type,
                           const TypeContext* types) {
  return type ? ToString(type.ref(), types) : JS_smprintf("%s", "void");
}
