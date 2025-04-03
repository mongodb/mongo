/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/BuiltinObjectKind.h"

#include "jspubtd.h"

#include "frontend/ParserAtom.h"
#include "vm/GlobalObject.h"

using namespace js;

static JSProtoKey ToProtoKey(BuiltinObjectKind kind) {
  switch (kind) {
    case BuiltinObjectKind::Array:
      return JSProto_Array;
    case BuiltinObjectKind::ArrayBuffer:
      return JSProto_ArrayBuffer;
    case BuiltinObjectKind::Int32Array:
      return JSProto_Int32Array;
    case BuiltinObjectKind::Iterator:
      return JSProto_Iterator;
    case BuiltinObjectKind::Map:
      return JSProto_Map;
    case BuiltinObjectKind::Promise:
      return JSProto_Promise;
    case BuiltinObjectKind::RegExp:
      return JSProto_RegExp;
    case BuiltinObjectKind::Set:
      return JSProto_Set;
    case BuiltinObjectKind::SharedArrayBuffer:
      return JSProto_SharedArrayBuffer;
    case BuiltinObjectKind::Symbol:
      return JSProto_Symbol;

    case BuiltinObjectKind::FunctionPrototype:
      return JSProto_Function;
    case BuiltinObjectKind::ObjectPrototype:
      return JSProto_Object;
    case BuiltinObjectKind::RegExpPrototype:
      return JSProto_RegExp;
    case BuiltinObjectKind::StringPrototype:
      return JSProto_String;

    case BuiltinObjectKind::DateTimeFormatPrototype:
      return JSProto_DateTimeFormat;
    case BuiltinObjectKind::NumberFormatPrototype:
      return JSProto_NumberFormat;

    case BuiltinObjectKind::None:
      break;
  }
  MOZ_CRASH("Unexpected builtin object kind");
}

static bool IsPrototype(BuiltinObjectKind kind) {
  switch (kind) {
    case BuiltinObjectKind::Array:
    case BuiltinObjectKind::ArrayBuffer:
    case BuiltinObjectKind::Int32Array:
    case BuiltinObjectKind::Iterator:
    case BuiltinObjectKind::Map:
    case BuiltinObjectKind::Promise:
    case BuiltinObjectKind::RegExp:
    case BuiltinObjectKind::Set:
    case BuiltinObjectKind::SharedArrayBuffer:
    case BuiltinObjectKind::Symbol:
      return false;

    case BuiltinObjectKind::FunctionPrototype:
    case BuiltinObjectKind::ObjectPrototype:
    case BuiltinObjectKind::RegExpPrototype:
    case BuiltinObjectKind::StringPrototype:
      return true;

    case BuiltinObjectKind::DateTimeFormatPrototype:
    case BuiltinObjectKind::NumberFormatPrototype:
      return true;

    case BuiltinObjectKind::None:
      break;
  }
  MOZ_CRASH("Unexpected builtin object kind");
}

BuiltinObjectKind js::BuiltinConstructorForName(
    frontend::TaggedParserAtomIndex name) {
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Array()) {
    return BuiltinObjectKind::Array;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::ArrayBuffer()) {
    return BuiltinObjectKind::ArrayBuffer;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Int32Array()) {
    return BuiltinObjectKind::Int32Array;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Iterator()) {
    return BuiltinObjectKind::Iterator;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Map()) {
    return BuiltinObjectKind::Map;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Promise()) {
    return BuiltinObjectKind::Promise;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::RegExp()) {
    return BuiltinObjectKind::RegExp;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Set()) {
    return BuiltinObjectKind::Set;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::SharedArrayBuffer()) {
    return BuiltinObjectKind::SharedArrayBuffer;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Symbol()) {
    return BuiltinObjectKind::Symbol;
  }
  return BuiltinObjectKind::None;
}

BuiltinObjectKind js::BuiltinPrototypeForName(
    frontend::TaggedParserAtomIndex name) {
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Function()) {
    return BuiltinObjectKind::FunctionPrototype;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::Object()) {
    return BuiltinObjectKind::ObjectPrototype;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::RegExp()) {
    return BuiltinObjectKind::RegExpPrototype;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::String()) {
    return BuiltinObjectKind::StringPrototype;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::DateTimeFormat()) {
    return BuiltinObjectKind::DateTimeFormatPrototype;
  }
  if (name == frontend::TaggedParserAtomIndex::WellKnown::NumberFormat()) {
    return BuiltinObjectKind::NumberFormatPrototype;
  }
  return BuiltinObjectKind::None;
}

JSObject* js::MaybeGetBuiltinObject(GlobalObject* global,
                                    BuiltinObjectKind kind) {
  JSProtoKey key = ToProtoKey(kind);
  if (IsPrototype(kind)) {
    return global->maybeGetPrototype(key);
  }
  return global->maybeGetConstructor(key);
}

JSObject* js::GetOrCreateBuiltinObject(JSContext* cx, BuiltinObjectKind kind) {
  JSProtoKey key = ToProtoKey(kind);
  if (IsPrototype(kind)) {
    return GlobalObject::getOrCreatePrototype(cx, key);
  }
  return GlobalObject::getOrCreateConstructor(cx, key);
}

const char* js::BuiltinObjectName(BuiltinObjectKind kind) {
  switch (kind) {
    case BuiltinObjectKind::Array:
      return "Array";
    case BuiltinObjectKind::ArrayBuffer:
      return "ArrayBuffer";
    case BuiltinObjectKind::Int32Array:
      return "Int32Array";
    case BuiltinObjectKind::Iterator:
      return "Iterator";
    case BuiltinObjectKind::Map:
      return "Map";
    case BuiltinObjectKind::Promise:
      return "Promise";
    case BuiltinObjectKind::RegExp:
      return "RegExp";
    case BuiltinObjectKind::SharedArrayBuffer:
      return "SharedArrayBuffer";
    case BuiltinObjectKind::Set:
      return "Set";
    case BuiltinObjectKind::Symbol:
      return "Symbol";

    case BuiltinObjectKind::FunctionPrototype:
      return "Function.prototype";
    case BuiltinObjectKind::ObjectPrototype:
      return "Object.prototype";
    case BuiltinObjectKind::RegExpPrototype:
      return "RegExp.prototype";
    case BuiltinObjectKind::StringPrototype:
      return "String.prototype";

    case BuiltinObjectKind::DateTimeFormatPrototype:
      return "DateTimeFormat.prototype";
    case BuiltinObjectKind::NumberFormatPrototype:
      return "NumberFormat.prototype";

    case BuiltinObjectKind::None:
      break;
  }
  MOZ_CRASH("Unexpected builtin object kind");
}
