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

#include "vm/JSAtom.h"
#include "vm/JSObject.h"
#include "vm/StringType.h"
#include "wasm/WasmJS.h"

#include "vm/JSAtom-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::wasm;

RefType RefType::topType() const {
  switch (kind()) {
    case RefType::Any:
    case RefType::Eq:
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

static RefTypeResult MaybeToRefType(JSContext* cx, HandleObject obj,
                                    RefType* out) {
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
  if (!wasm::FunctionReferencesAvailable(cx)) {
    return RefTypeResult::Unparsed;
  }

  JSAtom* refAtom = Atomize(cx, "ref", strlen("ref"));
  if (!refAtom) {
    return RefTypeResult::Failure;
  }
  RootedId refId(cx, AtomToId(refAtom));

  RootedValue refVal(cx);
  if (!GetProperty(cx, obj, obj, refId, &refVal)) {
    return RefTypeResult::Failure;
  }

  RootedString typeStr(cx, ToString(cx, refVal));
  if (!typeStr) {
    return RefTypeResult::Failure;
  }

  Rooted<JSLinearString*> typeLinearStr(cx, typeStr->ensureLinear(cx));
  if (!typeLinearStr) {
    return RefTypeResult::Failure;
  }

  if (StringEqualsLiteral(typeLinearStr, "func")) {
    *out = RefType::func();
  } else if (StringEqualsLiteral(typeLinearStr, "extern")) {
    *out = RefType::extern_();
#  ifdef ENABLE_WASM_GC
  } else if (GcAvailable(cx) && StringEqualsLiteral(typeLinearStr, "any")) {
    *out = RefType::any();
  } else if (GcAvailable(cx) && StringEqualsLiteral(typeLinearStr, "eq")) {
    *out = RefType::eq();
  } else if (GcAvailable(cx) && StringEqualsLiteral(typeLinearStr, "struct")) {
    *out = RefType::struct_();
  } else if (GcAvailable(cx) && StringEqualsLiteral(typeLinearStr, "array")) {
    *out = RefType::array();
#  endif
  } else {
    return RefTypeResult::Unparsed;
  }

  JSAtom* nullableAtom = Atomize(cx, "nullable", strlen("nullable"));
  if (!nullableAtom) {
    return RefTypeResult::Failure;
  }
  RootedId nullableId(cx, AtomToId(nullableAtom));
  RootedValue nullableVal(cx);
  if (!GetProperty(cx, obj, obj, nullableId, &nullableVal)) {
    return RefTypeResult::Failure;
  }

  bool nullable = ToBoolean(nullableVal);
  if (!nullable) {
    *out = out->asNonNullable();
  }
  MOZ_ASSERT(out->isNullable() == nullable);
  return RefTypeResult::Parsed;
#else
  return RefTypeResult::Unparsed;
#endif
}

bool wasm::ToValType(JSContext* cx, HandleValue v, ValType* out) {
  if (v.isObject()) {
    RootedObject obj(cx, &v.toObject());
    RefType refType;
    switch (MaybeToRefType(cx, obj, &refType)) {
      case RefTypeResult::Failure:
        return false;
      case RefTypeResult::Parsed:
        *out = ValType(refType);
        return true;
      case RefTypeResult::Unparsed:
        break;
    }
  }

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
  if (v.isObject()) {
    RootedObject obj(cx, &v.toObject());
    switch (MaybeToRefType(cx, obj, out)) {
      case RefTypeResult::Failure:
        return false;
      case RefTypeResult::Parsed:
        return true;
      case RefTypeResult::Unparsed:
        break;
    }
  }

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
      case RefType::Any:
        literal = "anyref";
        break;
      case RefType::NoFunc:
        literal = "nullfuncref";
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
    case RefType::Any:
      heapType = "any";
      break;
    case RefType::NoFunc:
      heapType = "nofunc";
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
  return ToString(type.fieldType(), types);
}

UniqueChars wasm::ToString(FieldType type, const TypeContext* types) {
  const char* literal = nullptr;
  switch (type.kind()) {
    case FieldType::I8:
      literal = "i8";
      break;
    case FieldType::I16:
      literal = "i16";
      break;
    case FieldType::I32:
      literal = "i32";
      break;
    case FieldType::I64:
      literal = "i64";
      break;
    case FieldType::V128:
      literal = "v128";
      break;
    case FieldType::F32:
      literal = "f32";
      break;
    case FieldType::F64:
      literal = "f64";
      break;
    case FieldType::Ref:
      return ToString(type.refType(), types);
  }
  return DuplicateString(literal);
}

UniqueChars wasm::ToString(const Maybe<ValType>& type,
                           const TypeContext* types) {
  return type ? ToString(type.ref(), types) : JS_smprintf("%s", "void");
}
