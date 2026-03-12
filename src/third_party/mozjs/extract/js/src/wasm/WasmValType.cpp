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
  switch (hierarchy()) {
    case wasm::RefTypeHierarchy::Any:
      return wasm::RefType::any();
    case wasm::RefTypeHierarchy::Func:
      return wasm::RefType::func();
    case wasm::RefTypeHierarchy::Extern:
      return wasm::RefType::extern_();
    case wasm::RefTypeHierarchy::Exn:
      return wasm::RefType::exn();
    default:
      MOZ_CRASH("switch is exhaustive");
  }
}

RefType RefType::bottomType() const {
  switch (hierarchy()) {
    case wasm::RefTypeHierarchy::Any:
      return wasm::RefType::none();
    case wasm::RefTypeHierarchy::Func:
      return wasm::RefType::nofunc();
    case wasm::RefTypeHierarchy::Extern:
      return wasm::RefType::noextern();
    case wasm::RefTypeHierarchy::Exn:
      return wasm::RefType::noexn();
    default:
      MOZ_CRASH("switch is exhaustive");
  }
}

static RefType FirstCommonSuperType(RefType a, RefType b,
                                    std::initializer_list<RefType> supers) {
  for (RefType super : supers) {
    if (RefType::isSubTypeOf(a, super) && RefType::isSubTypeOf(b, super)) {
      return super;
    }
  }
  MOZ_CRASH("failed to find common super type");
}

RefType RefType::leastUpperBound(RefType a, RefType b) {
  // Types in different hierarchies have no common bound. Validation should
  // always prevent two such types from being compared.
  MOZ_RELEASE_ASSERT(a.hierarchy() == b.hierarchy());

  // Whether the LUB is nullable can be determined by the nullability of a and
  // b, regardless of their actual types.
  bool nullable = a.isNullable() || b.isNullable();

  // If one type is a subtype of the other, the higher type is the LUB - and we
  // can capture nulls here too, as we know the nullability of the LUB.
  if (RefType::isSubTypeOf(a, b.withIsNullable(nullable))) {
    return b.withIsNullable(nullable);
  }
  if (RefType::isSubTypeOf(b, a.withIsNullable(nullable))) {
    return a.withIsNullable(nullable);
  }

  // Concrete types may share a concrete parent type. We can test b against all
  // of a's parent types to see if this is true.
  if (a.isTypeRef() && b.isTypeRef()) {
    const TypeDef* aSuper = a.typeDef()->superTypeDef();
    while (aSuper) {
      if (TypeDef::isSubTypeOf(b.typeDef(), aSuper)) {
        return RefType(aSuper, nullable);
      }
      aSuper = aSuper->superTypeDef();
    }
  }

  // Because wasm type hierarchies are pretty small and simple, we can
  // essentially brute-force the LUB by simply iterating over all the abstract
  // types bottom-to-top. The first one that is a super type of both a and b is
  // the LUB. We are guaranteed to find a common bound because we have verified
  // that the types have the same hierarchy and we will therefore at least find
  // the hierarchy's top type.
  //
  // We test against the nullable versions of these types, and then apply the
  // true nullability afterward. This is ok -- this finds the *kind* of the LUB
  // (which we now know to be abstract), and applying the correct nullability
  // will not affect this. For example, for the non-nullable types
  // (ref $myStruct) and (ref $myArray), we will find (ref null eq), and then
  // modify it to (ref eq), which is the correct LUB.
  RefType common;
  switch (a.hierarchy()) {
    case RefTypeHierarchy::Any:
      common = FirstCommonSuperType(
          a, b,
          {RefType::none(), RefType::i31(), RefType::struct_(),
           RefType::array(), RefType::eq(), RefType::any()});
      break;
    case RefTypeHierarchy::Func:
      common = FirstCommonSuperType(a, b, {RefType::nofunc(), RefType::func()});
      break;
    case RefTypeHierarchy::Extern:
      common =
          FirstCommonSuperType(a, b, {RefType::noextern(), RefType::extern_()});
      break;
    case RefTypeHierarchy::Exn:
      common = FirstCommonSuperType(a, b, {RefType::noexn(), RefType::exn()});
      break;
    default:
      MOZ_CRASH("unknown type hierarchy");
  }
  return common.withIsNullable(nullable);
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

static bool ToRefType(JSContext* cx, const JSLinearString* typeLinearStr,
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

UniqueChars wasm::ToString(const mozilla::Maybe<ValType>& type,
                           const TypeContext* types) {
  return type ? ToString(type.ref(), types) : JS_smprintf("%s", "void");
}

UniqueChars wasm::ToString(const MaybeRefType& type, const TypeContext* types) {
  return type ? ToString(type.value(), types) : JS_smprintf("%s", "void");
}
