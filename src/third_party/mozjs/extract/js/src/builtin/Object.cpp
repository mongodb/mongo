/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Object.h"
#include "js/Object.h"  // JS::GetBuiltinClass

#include "mozilla/MaybeOneOf.h"
#include "mozilla/Range.h"
#include "mozilla/RangedPtr.h"

#include <algorithm>

#include "builtin/BigInt.h"
#include "builtin/Eval.h"
#include "builtin/SelfHostingDefines.h"
#include "frontend/BytecodeCompiler.h"
#include "jit/InlinableNatives.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/PropertySpec.h"
#include "js/UniquePtr.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/AsyncFunction.h"
#include "vm/BooleanObject.h"
#include "vm/DateObject.h"
#include "vm/EqualityOperations.h"  // js::SameValue
#include "vm/ErrorObject.h"
#include "vm/JSContext.h"
#include "vm/NumberObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/RegExpObject.h"
#include "vm/StringObject.h"
#include "vm/ToSource.h"       // js::ValueToSource
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

#ifdef FUZZING
#  include "builtin/TestingFunctions.h"
#endif

using namespace js;

using js::frontend::IsIdentifier;

using mozilla::Maybe;
using mozilla::Range;
using mozilla::RangedPtr;

bool js::obj_construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject obj(cx, nullptr);
  if (args.isConstructing() &&
      (&args.newTarget().toObject() != &args.callee())) {
    RootedObject newTarget(cx, &args.newTarget().toObject());
    obj = CreateThis(cx, &PlainObject::class_, newTarget);
    if (!obj) {
      return false;
    }
  } else if (args.length() > 0 && !args[0].isNullOrUndefined()) {
    obj = ToObject(cx, args[0]);
    if (!obj) {
      return false;
    }
  } else {
    /* Make an object whether this was called with 'new' or not. */
    if (!NewObjectScriptedCall(cx, &obj)) {
      return false;
    }
  }

  args.rval().setObject(*obj);
  return true;
}

/* ES5 15.2.4.7. */
bool js::obj_propertyIsEnumerable(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  HandleValue idValue = args.get(0);

  // As an optimization, provide a fast path when rooting is not necessary and
  // we can safely retrieve the attributes from the object's shape.

  /* Steps 1-2. */
  jsid id;
  if (args.thisv().isObject() && idValue.isPrimitive() &&
      PrimitiveValueToId<NoGC>(cx, idValue, &id)) {
    JSObject* obj = &args.thisv().toObject();

    /* Step 3. */
    PropertyResult prop;
    if (obj->is<NativeObject>() &&
        NativeLookupOwnProperty<NoGC>(cx, &obj->as<NativeObject>(), id,
                                      &prop)) {
      /* Step 4. */
      if (prop.isNotFound()) {
        args.rval().setBoolean(false);
        return true;
      }

      /* Step 5. */
      JS::PropertyAttributes attrs = GetPropertyAttributes(obj, prop);
      args.rval().setBoolean(attrs.enumerable());
      return true;
    }
  }

  /* Step 1. */
  RootedId idRoot(cx);
  if (!ToPropertyKey(cx, idValue, &idRoot)) {
    return false;
  }

  /* Step 2. */
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  /* Step 3. */
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, obj, idRoot, &desc)) {
    return false;
  }

  /* Step 4. */
  if (desc.isNothing()) {
    args.rval().setBoolean(false);
    return true;
  }

  /* Step 5. */
  args.rval().setBoolean(desc->enumerable());
  return true;
}

static bool obj_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return false;
  }

  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  JSString* str = ObjectToSource(cx, obj);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

template <typename CharT>
static bool Consume(RangedPtr<const CharT>& s, RangedPtr<const CharT> e,
                    const char* chars) {
  MOZ_ASSERT(s <= e);
  size_t len = strlen(chars);
  if (e - s < len) {
    return false;
  }
  if (!EqualChars(s.get(), chars, len)) {
    return false;
  }
  s += len;
  return true;
}

template <typename CharT>
static bool ConsumeUntil(RangedPtr<const CharT>& s, RangedPtr<const CharT> e,
                         char16_t ch) {
  MOZ_ASSERT(s <= e);
  const CharT* result = js_strchr_limit(s.get(), ch, e.get());
  if (!result) {
    return false;
  }
  s += result - s.get();
  MOZ_ASSERT(*s == ch);
  return true;
}

template <typename CharT>
static void ConsumeSpaces(RangedPtr<const CharT>& s, RangedPtr<const CharT> e) {
  while (s < e && *s == ' ') {
    s++;
  }
}

/*
 * Given a function source string, return the offset and length of the part
 * between '(function $name' and ')'.
 */
template <typename CharT>
static bool ArgsAndBodySubstring(Range<const CharT> chars, size_t* outOffset,
                                 size_t* outLen) {
  const RangedPtr<const CharT> start = chars.begin();
  RangedPtr<const CharT> s = start;
  RangedPtr<const CharT> e = chars.end();

  if (s == e) {
    return false;
  }

  // Remove enclosing parentheses.
  if (*s == '(' && *(e - 1) == ')') {
    s++;
    e--;
  }

  // Support the following cases, with spaces between tokens:
  //
  //   -+---------+-+------------+-+-----+-+- [ - <any> - ] - ( -+-
  //    |         | |            | |     | |                     |
  //    +- async -+ +- function -+ +- * -+ +- <any> - ( ---------+
  //                |            |
  //                +- get ------+
  //                |            |
  //                +- set ------+
  //
  // This accepts some invalid syntax, but we don't care, since it's only
  // used by the non-standard toSource, and we're doing a best-effort attempt
  // here.

  (void)Consume(s, e, "async");
  ConsumeSpaces(s, e);
  (void)(Consume(s, e, "function") || Consume(s, e, "get") ||
         Consume(s, e, "set"));
  ConsumeSpaces(s, e);
  (void)Consume(s, e, "*");
  ConsumeSpaces(s, e);

  // Jump over the function's name.
  if (Consume(s, e, "[")) {
    if (!ConsumeUntil(s, e, ']')) {
      return false;
    }
    s++;  // Skip ']'.
    ConsumeSpaces(s, e);
    if (s >= e || *s != '(') {
      return false;
    }
  } else {
    if (!ConsumeUntil(s, e, '(')) {
      return false;
    }
  }

  MOZ_ASSERT(*s == '(');

  *outOffset = s - start;
  *outLen = e - s;
  MOZ_ASSERT(*outOffset + *outLen <= chars.length());
  return true;
}

enum class PropertyKind { Getter, Setter, Method, Normal };

JSString* js::ObjectToSource(JSContext* cx, HandleObject obj) {
  /* If outermost, we need parentheses to be an expression, not a block. */
  bool outermost = cx->cycleDetectorVector().empty();

  AutoCycleDetector detector(cx, obj);
  if (!detector.init()) {
    return nullptr;
  }
  if (detector.foundCycle()) {
    return NewStringCopyZ<CanGC>(cx, "{}");
  }

  JSStringBuilder buf(cx);
  if (outermost && !buf.append('(')) {
    return nullptr;
  }
  if (!buf.append('{')) {
    return nullptr;
  }

  RootedIdVector idv(cx);
  if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY | JSITER_SYMBOLS, &idv)) {
    return nullptr;
  }

  bool comma = false;

  auto AddProperty = [cx, &comma, &buf](HandleId id, HandleValue val,
                                        PropertyKind kind) -> bool {
    /* Convert id to a string. */
    RootedString idstr(cx);
    if (id.isSymbol()) {
      RootedValue v(cx, SymbolValue(id.toSymbol()));
      idstr = ValueToSource(cx, v);
      if (!idstr) {
        return false;
      }
    } else {
      RootedValue idv(cx, IdToValue(id));
      idstr = ToString<CanGC>(cx, idv);
      if (!idstr) {
        return false;
      }

      /*
       * If id is a string that's not an identifier, or if it's a
       * negative integer, then it must be quoted.
       */
      if (id.isAtom() ? !IsIdentifier(id.toAtom()) : JSID_TO_INT(id) < 0) {
        UniqueChars quotedId = QuoteString(cx, idstr, '\'');
        if (!quotedId) {
          return false;
        }
        idstr = NewStringCopyZ<CanGC>(cx, quotedId.get());
        if (!idstr) {
          return false;
        }
      }
    }

    RootedString valsource(cx, ValueToSource(cx, val));
    if (!valsource) {
      return false;
    }

    RootedLinearString valstr(cx, valsource->ensureLinear(cx));
    if (!valstr) {
      return false;
    }

    if (comma && !buf.append(", ")) {
      return false;
    }
    comma = true;

    size_t voffset, vlength;

    // Methods and accessors can return exact syntax of source, that fits
    // into property without adding property name or "get"/"set" prefix.
    // Use the exact syntax when the following conditions are met:
    //
    //   * It's a function object
    //     (exclude proxies)
    //   * Function's kind and property's kind are same
    //     (this can be false for dynamically defined properties)
    //   * Function has explicit name
    //     (this can be false for computed property and dynamically defined
    //      properties)
    //   * Function's name and property's name are same
    //     (this can be false for dynamically defined properties)
    if (kind == PropertyKind::Getter || kind == PropertyKind::Setter ||
        kind == PropertyKind::Method) {
      RootedFunction fun(cx);
      if (val.toObject().is<JSFunction>()) {
        fun = &val.toObject().as<JSFunction>();
        // Method's case should be checked on caller.
        if (((fun->isGetter() && kind == PropertyKind::Getter) ||
             (fun->isSetter() && kind == PropertyKind::Setter) ||
             kind == PropertyKind::Method) &&
            fun->explicitName()) {
          bool result;
          if (!EqualStrings(cx, fun->explicitName(), idstr, &result)) {
            return false;
          }

          if (result) {
            if (!buf.append(valstr)) {
              return false;
            }
            return true;
          }
        }
      }

      {
        // When falling back try to generate a better string
        // representation by skipping the prelude, and also removing
        // the enclosing parentheses.
        bool success;
        JS::AutoCheckCannotGC nogc;
        if (valstr->hasLatin1Chars()) {
          success = ArgsAndBodySubstring(valstr->latin1Range(nogc), &voffset,
                                         &vlength);
        } else {
          success = ArgsAndBodySubstring(valstr->twoByteRange(nogc), &voffset,
                                         &vlength);
        }
        if (!success) {
          kind = PropertyKind::Normal;
        }
      }

      if (kind == PropertyKind::Getter) {
        if (!buf.append("get ")) {
          return false;
        }
      } else if (kind == PropertyKind::Setter) {
        if (!buf.append("set ")) {
          return false;
        }
      } else if (kind == PropertyKind::Method && fun) {
        if (fun->isAsync()) {
          if (!buf.append("async ")) {
            return false;
          }
        }

        if (fun->isGenerator()) {
          if (!buf.append('*')) {
            return false;
          }
        }
      }
    }

    bool needsBracket = id.isSymbol();
    if (needsBracket && !buf.append('[')) {
      return false;
    }
    if (!buf.append(idstr)) {
      return false;
    }
    if (needsBracket && !buf.append(']')) {
      return false;
    }

    if (kind == PropertyKind::Getter || kind == PropertyKind::Setter ||
        kind == PropertyKind::Method) {
      if (!buf.appendSubstring(valstr, voffset, vlength)) {
        return false;
      }
    } else {
      if (!buf.append(':')) {
        return false;
      }
      if (!buf.append(valstr)) {
        return false;
      }
    }
    return true;
  };

  RootedId id(cx);
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  RootedValue val(cx);
  for (size_t i = 0; i < idv.length(); ++i) {
    id = idv[i];
    if (!GetOwnPropertyDescriptor(cx, obj, id, &desc)) {
      return nullptr;
    }

    if (desc.isNothing()) {
      continue;
    }

    if (desc->isAccessorDescriptor()) {
      if (desc->hasGetter() && desc->getter()) {
        val.setObject(*desc->getter());
        if (!AddProperty(id, val, PropertyKind::Getter)) {
          return nullptr;
        }
      }
      if (desc->hasSetter() && desc->setter()) {
        val.setObject(*desc->setter());
        if (!AddProperty(id, val, PropertyKind::Setter)) {
          return nullptr;
        }
      }
      continue;
    }

    val.set(desc->value());

    JSFunction* fun;
    if (IsFunctionObject(val, &fun) && fun->isMethod()) {
      if (!AddProperty(id, val, PropertyKind::Method)) {
        return nullptr;
      }
      continue;
    }

    if (!AddProperty(id, val, PropertyKind::Normal)) {
      return nullptr;
    }
  }

  if (!buf.append('}')) {
    return nullptr;
  }
  if (outermost && !buf.append(')')) {
    return nullptr;
  }

  return buf.finishString();
}

static JSString* GetBuiltinTagSlow(JSContext* cx, HandleObject obj) {
  // Step 4.
  bool isArray;
  if (!IsArray(cx, obj, &isArray)) {
    return nullptr;
  }

  // Step 5.
  if (isArray) {
    return cx->names().objectArray;
  }

  // Steps 6-14.
  ESClass cls;
  if (!JS::GetBuiltinClass(cx, obj, &cls)) {
    return nullptr;
  }

  switch (cls) {
    case ESClass::String:
      return cx->names().objectString;
    case ESClass::Arguments:
      return cx->names().objectArguments;
    case ESClass::Error:
      return cx->names().objectError;
    case ESClass::Boolean:
      return cx->names().objectBoolean;
    case ESClass::Number:
      return cx->names().objectNumber;
    case ESClass::Date:
      return cx->names().objectDate;
    case ESClass::RegExp:
      return cx->names().objectRegExp;
    default:
      if (obj->isCallable()) {
        // Non-standard: Prevent <object> from showing up as Function.
        JSObject* unwrapped = CheckedUnwrapDynamic(obj, cx);
        if (!unwrapped || !unwrapped->getClass()->isDOMClass()) {
          return cx->names().objectFunction;
        }
      }
      return cx->names().objectObject;
  }
}

static MOZ_ALWAYS_INLINE JSString* GetBuiltinTagFast(JSObject* obj,
                                                     const JSClass* clasp,
                                                     JSContext* cx) {
  MOZ_ASSERT(clasp == obj->getClass());
  MOZ_ASSERT(!clasp->isProxyObject());

  // Optimize the non-proxy case to bypass GetBuiltinClass.
  if (clasp == &PlainObject::class_) {
    // This case is by far the most common so we handle it first.
    return cx->names().objectObject;
  }

  if (clasp == &ArrayObject::class_) {
    return cx->names().objectArray;
  }

  if (clasp == &JSFunction::class_) {
    return cx->names().objectFunction;
  }

  if (clasp == &StringObject::class_) {
    return cx->names().objectString;
  }

  if (clasp == &NumberObject::class_) {
    return cx->names().objectNumber;
  }

  if (clasp == &BooleanObject::class_) {
    return cx->names().objectBoolean;
  }

  if (clasp == &DateObject::class_) {
    return cx->names().objectDate;
  }

  if (clasp == &RegExpObject::class_) {
    return cx->names().objectRegExp;
  }

  if (obj->is<ArgumentsObject>()) {
    return cx->names().objectArguments;
  }

  if (obj->is<ErrorObject>()) {
    return cx->names().objectError;
  }

  if (obj->isCallable() && !obj->getClass()->isDOMClass()) {
    // Non-standard: Prevent <object> from showing up as Function.
    return cx->names().objectFunction;
  }

  return cx->names().objectObject;
}

// For primitive values we try to avoid allocating the object if we can
// determine that the prototype it would use does not define Symbol.toStringTag.
static JSAtom* MaybeObjectToStringPrimitive(JSContext* cx, const Value& v) {
  JSProtoKey protoKey = js::PrimitiveToProtoKey(cx, v);

  // If prototype doesn't exist yet, just fall through.
  JSObject* proto = cx->global()->maybeGetPrototype(protoKey);
  if (!proto) {
    return nullptr;
  }

  // If determining this may have side-effects, we must instead create the
  // object normally since it is the receiver while looking up
  // Symbol.toStringTag.
  if (MaybeHasInterestingSymbolProperty(
          cx, proto, cx->wellKnownSymbols().toStringTag, nullptr)) {
    return nullptr;
  }

  // Return the direct result.
  switch (protoKey) {
    case JSProto_String:
      return cx->names().objectString;
    case JSProto_Number:
      return cx->names().objectNumber;
    case JSProto_Boolean:
      return cx->names().objectBoolean;
    case JSProto_Symbol:
      return cx->names().objectSymbol;
    case JSProto_BigInt:
      return cx->names().objectBigInt;
    default:
      break;
  }

  return nullptr;
}

// ES6 19.1.3.6
bool js::obj_toString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject obj(cx);

  if (args.thisv().isPrimitive()) {
    // Step 1.
    if (args.thisv().isUndefined()) {
      args.rval().setString(cx->names().objectUndefined);
      return true;
    }

    // Step 2.
    if (args.thisv().isNull()) {
      args.rval().setString(cx->names().objectNull);
      return true;
    }

    // Try fast-path for primitives. This is unusual but we encounter code like
    // this in the wild.
    JSAtom* result = MaybeObjectToStringPrimitive(cx, args.thisv());
    if (result) {
      args.rval().setString(result);
      return true;
    }

    // Step 3.
    obj = ToObject(cx, args.thisv());
    if (!obj) {
      return false;
    }
  } else {
    obj = &args.thisv().toObject();
  }

  // When |obj| is a non-proxy object, compute |builtinTag| only when needed.
  RootedString builtinTag(cx);
  const JSClass* clasp = obj->getClass();
  if (MOZ_UNLIKELY(clasp->isProxyObject())) {
    builtinTag = GetBuiltinTagSlow(cx, obj);
    if (!builtinTag) {
      return false;
    }
  }

  // Step 15.
  RootedValue tag(cx);
  if (!GetInterestingSymbolProperty(cx, obj, cx->wellKnownSymbols().toStringTag,
                                    &tag)) {
    return false;
  }

  // Step 16.
  if (!tag.isString()) {
    if (!builtinTag) {
      builtinTag = GetBuiltinTagFast(obj, clasp, cx);
#ifdef DEBUG
      // Assert this fast path is correct and matches BuiltinTagSlow.
      JSString* builtinTagSlow = GetBuiltinTagSlow(cx, obj);
      if (!builtinTagSlow) {
        return false;
      }
      MOZ_ASSERT(builtinTagSlow == builtinTag);
#endif
    }

    args.rval().setString(builtinTag);
    return true;
  }

  // Step 17.
  StringBuffer sb(cx);
  if (!sb.append("[object ") || !sb.append(tag.toString()) || !sb.append(']')) {
    return false;
  }

  JSString* str = sb.finishAtom();
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

JSString* js::ObjectClassToString(JSContext* cx, JSObject* obj) {
  AutoUnsafeCallWithABI unsafe;

  if (MaybeHasInterestingSymbolProperty(cx, obj,
                                        cx->wellKnownSymbols().toStringTag)) {
    return nullptr;
  }
  return GetBuiltinTagFast(obj, obj->getClass(), cx);
}

static bool obj_setPrototypeOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!args.requireAtLeast(cx, "Object.setPrototypeOf", 2)) {
    return false;
  }

  /* Step 1-2. */
  if (args[0].isNullOrUndefined()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
        args[0].isNull() ? "null" : "undefined", "object");
    return false;
  }

  /* Step 3. */
  if (!args[1].isObjectOrNull()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "Object.setPrototypeOf",
                              "an object or null",
                              InformalValueTypeName(args[1]));
    return false;
  }

  /* Step 4. */
  if (!args[0].isObject()) {
    args.rval().set(args[0]);
    return true;
  }

  /* Step 5-7. */
  RootedObject obj(cx, &args[0].toObject());
  RootedObject newProto(cx, args[1].toObjectOrNull());
  if (!SetPrototype(cx, obj, newProto)) {
    return false;
  }

  /* Step 8. */
  args.rval().set(args[0]);
  return true;
}

static bool PropertyIsEnumerable(JSContext* cx, HandleObject obj, HandleId id,
                                 bool* enumerable) {
  PropertyResult prop;
  if (obj->is<NativeObject>() &&
      NativeLookupOwnProperty<NoGC>(cx, &obj->as<NativeObject>(), id, &prop)) {
    if (prop.isNotFound()) {
      *enumerable = false;
      return true;
    }

    JS::PropertyAttributes attrs = GetPropertyAttributes(obj, prop);
    *enumerable = attrs.enumerable();
    return true;
  }

  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, obj, id, &desc)) {
    return false;
  }

  *enumerable = desc.isSome() && desc->enumerable();
  return true;
}

// Returns true if properties not named "__proto__" can be added to |obj|
// with a fast path that doesn't check any properties on the prototype chain.
static bool CanAddNewPropertyExcludingProtoFast(PlainObject* obj) {
  // The object must be an extensible, non-prototype object. Prototypes require
  // extra shadowing checks for shape teleporting.
  if (!obj->isExtensible() || obj->isUsedAsPrototype()) {
    return false;
  }

  // Ensure the object has no non-writable properties or getters/setters.
  // For now only support PlainObjects so that we don't have to worry about
  // resolve hooks and other JSClass hooks.
  while (true) {
    if (obj->hasNonWritableOrAccessorPropExclProto()) {
      return false;
    }

    JSObject* proto = obj->staticPrototype();
    if (!proto) {
      return true;
    }
    if (!proto->is<PlainObject>()) {
      return false;
    }
    obj = &proto->as<PlainObject>();
  }
}

[[nodiscard]] static bool TryAssignPlain(JSContext* cx, HandleObject to,
                                         HandleObject from, bool* optimized) {
  // Object.assign is used with PlainObjects most of the time. This is a fast
  // path to optimize that case. This lets us avoid checks that are only
  // relevant for other JSClasses.

  MOZ_ASSERT(*optimized == false);

  if (!from->is<PlainObject>() || !to->is<PlainObject>()) {
    return true;
  }

  // Don't use the fast path if |from| may have extra indexed properties.
  HandlePlainObject fromPlain = from.as<PlainObject>();
  if (fromPlain->getDenseInitializedLength() > 0 || fromPlain->isIndexed()) {
    return true;
  }
  MOZ_ASSERT(!fromPlain->getClass()->getNewEnumerate());
  MOZ_ASSERT(!fromPlain->getClass()->getEnumerate());

  // Empty |from| objects are common, so check for this first.
  if (fromPlain->empty()) {
    *optimized = true;
    return true;
  }

  HandlePlainObject toPlain = to.as<PlainObject>();
  if (!CanAddNewPropertyExcludingProtoFast(toPlain)) {
    return true;
  }

  // Get a list of all enumerable |from| properties.

  Rooted<PropertyInfoWithKeyVector> props(cx, PropertyInfoWithKeyVector(cx));

#ifdef DEBUG
  RootedShape fromShape(cx, fromPlain->shape());
#endif

  bool hasPropsWithNonDefaultAttrs = false;
  for (ShapePropertyIter<NoGC> iter(fromPlain->shape()); !iter.done(); iter++) {
    // Symbol properties need to be assigned last. For now fall back to the
    // slow path if we see a symbol property.
    jsid id = iter->key();
    if (MOZ_UNLIKELY(id.isSymbol())) {
      return true;
    }
    // __proto__ is not supported by CanAddNewPropertyExcludingProtoFast.
    if (MOZ_UNLIKELY(id.isAtom(cx->names().proto))) {
      return true;
    }
    if (MOZ_UNLIKELY(!iter->isDataProperty())) {
      return true;
    }
    if (iter->flags() != PropertyFlags::defaultDataPropFlags) {
      hasPropsWithNonDefaultAttrs = true;
    }
    if (!iter->enumerable()) {
      continue;
    }
    if (MOZ_UNLIKELY(!props.append(*iter))) {
      return false;
    }
  }

  *optimized = true;

  bool toWasEmpty = toPlain->empty();

  // If the |to| object has no properties and the |from| object only has plain
  // enumerable/writable/configurable data properties, try to use its shape.
  if (toWasEmpty && !hasPropsWithNonDefaultAttrs &&
      toPlain->canReuseShapeForNewProperties(fromPlain->shape())) {
    Shape* newShape = fromPlain->shape();
    if (!toPlain->setShapeAndUpdateSlots(cx, newShape)) {
      return false;
    }
    for (size_t i = props.length(); i > 0; i--) {
      size_t slot = props[i - 1].slot();
      toPlain->initSlot(slot, fromPlain->getSlot(slot));
    }
    return true;
  }

  RootedValue propValue(cx);
  RootedId nextKey(cx);

  for (size_t i = props.length(); i > 0; i--) {
    // Assert |from| still has the same properties.
    MOZ_ASSERT(fromPlain->shape() == fromShape);

    PropertyInfoWithKey fromProp = props[i - 1];
    MOZ_ASSERT(fromProp.isDataProperty());
    MOZ_ASSERT(fromProp.enumerable());

    nextKey = fromProp.key();
    propValue = fromPlain->getSlot(fromProp.slot());

    Maybe<PropertyInfo> toProp;
    if (toWasEmpty) {
      MOZ_ASSERT(!toPlain->containsPure(nextKey));
      MOZ_ASSERT(toProp.isNothing());
    } else {
      toProp = toPlain->lookup(cx, nextKey);
    }

    if (toProp.isSome()) {
      MOZ_ASSERT(toProp->isDataProperty());
      MOZ_ASSERT(toProp->writable());
      toPlain->setSlot(toProp->slot(), propValue);
    } else {
      if (!AddDataPropertyNonPrototype(cx, toPlain, nextKey, propValue)) {
        return false;
      }
    }
  }

  return true;
}

static bool TryAssignNative(JSContext* cx, HandleObject to, HandleObject from,
                            bool* optimized) {
  MOZ_ASSERT(*optimized == false);

  if (!from->is<NativeObject>() || !to->is<NativeObject>()) {
    return true;
  }

  // Don't use the fast path if |from| may have extra indexed or lazy
  // properties.
  NativeObject* fromNative = &from->as<NativeObject>();
  if (fromNative->getDenseInitializedLength() > 0 || fromNative->isIndexed() ||
      fromNative->is<TypedArrayObject>() ||
      fromNative->getClass()->getNewEnumerate() ||
      fromNative->getClass()->getEnumerate()) {
    return true;
  }

  // Get a list of |from| properties. As long as from->shape() == fromShape
  // we can use this to speed up both the enumerability check and the GetProp.

  Rooted<PropertyInfoWithKeyVector> props(cx, PropertyInfoWithKeyVector(cx));

  RootedShape fromShape(cx, fromNative->shape());
  for (ShapePropertyIter<NoGC> iter(fromShape); !iter.done(); iter++) {
    // Symbol properties need to be assigned last. For now fall back to the
    // slow path if we see a symbol property.
    if (MOZ_UNLIKELY(iter->key().isSymbol())) {
      return true;
    }
    if (MOZ_UNLIKELY(!props.append(*iter))) {
      return false;
    }
  }

  *optimized = true;

  RootedValue propValue(cx);
  RootedId nextKey(cx);
  RootedValue toReceiver(cx, ObjectValue(*to));

  for (size_t i = props.length(); i > 0; i--) {
    PropertyInfoWithKey prop = props[i - 1];
    nextKey = prop.key();

    // If |from| still has the same shape, it must still be a NativeObject with
    // the properties in |props|.
    if (MOZ_LIKELY(from->shape() == fromShape && prop.isDataProperty())) {
      if (!prop.enumerable()) {
        continue;
      }
      propValue = from->as<NativeObject>().getSlot(prop.slot());
    } else {
      // |from| changed shape or the property is not a data property, so
      // we have to do the slower enumerability check and GetProp.
      bool enumerable;
      if (!PropertyIsEnumerable(cx, from, nextKey, &enumerable)) {
        return false;
      }
      if (!enumerable) {
        continue;
      }
      if (!GetProperty(cx, from, from, nextKey, &propValue)) {
        return false;
      }
    }

    ObjectOpResult result;
    if (MOZ_UNLIKELY(
            !SetProperty(cx, to, nextKey, propValue, toReceiver, result))) {
      return false;
    }
    if (MOZ_UNLIKELY(!result.checkStrict(cx, to, nextKey))) {
      return false;
    }
  }

  return true;
}

static bool AssignSlow(JSContext* cx, HandleObject to, HandleObject from) {
  // Step 4.b.ii.
  RootedIdVector keys(cx);
  if (!GetPropertyKeys(
          cx, from, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &keys)) {
    return false;
  }

  // Step 4.c.
  RootedId nextKey(cx);
  RootedValue propValue(cx);
  for (size_t i = 0, len = keys.length(); i < len; i++) {
    nextKey = keys[i];

    // Step 4.c.i.
    bool enumerable;
    if (MOZ_UNLIKELY(!PropertyIsEnumerable(cx, from, nextKey, &enumerable))) {
      return false;
    }
    if (!enumerable) {
      continue;
    }

    // Step 4.c.ii.1.
    if (MOZ_UNLIKELY(!GetProperty(cx, from, from, nextKey, &propValue))) {
      return false;
    }

    // Step 4.c.ii.2.
    if (MOZ_UNLIKELY(!SetProperty(cx, to, nextKey, propValue))) {
      return false;
    }
  }

  return true;
}

JS_PUBLIC_API bool JS_AssignObject(JSContext* cx, JS::HandleObject target,
                                   JS::HandleObject src) {
  bool optimized = false;

  if (!TryAssignPlain(cx, target, src, &optimized)) {
    return false;
  }
  if (optimized) {
    return true;
  }

  if (!TryAssignNative(cx, target, src, &optimized)) {
    return false;
  }
  if (optimized) {
    return true;
  }

  return AssignSlow(cx, target, src);
}

// ES2018 draft rev 48ad2688d8f964da3ea8c11163ef20eb126fb8a4
// 19.1.2.1 Object.assign(target, ...sources)
static bool obj_assign(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject to(cx, ToObject(cx, args.get(0)));
  if (!to) {
    return false;
  }

  // Note: step 2 is implicit. If there are 0 arguments, ToObject throws. If
  // there's 1 argument, the loop below is a no-op.

  // Step 4.
  RootedObject from(cx);
  for (size_t i = 1; i < args.length(); i++) {
    // Step 4.a.
    if (args[i].isNullOrUndefined()) {
      continue;
    }

    // Step 4.b.i.
    from = ToObject(cx, args[i]);
    if (!from) {
      return false;
    }

    // Steps 4.b.ii, 4.c.
    if (!JS_AssignObject(cx, to, from)) {
      return false;
    }
  }

  // Step 5.
  args.rval().setObject(*to);
  return true;
}

/* ES5 15.2.4.6. */
bool js::obj_isPrototypeOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  if (args.length() < 1 || !args[0].isObject()) {
    args.rval().setBoolean(false);
    return true;
  }

  /* Step 2. */
  RootedObject obj(cx, ToObject(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  /* Step 3. */
  bool isPrototype;
  if (!IsPrototypeOf(cx, obj, &args[0].toObject(), &isPrototype)) {
    return false;
  }
  args.rval().setBoolean(isPrototype);
  return true;
}

PlainObject* js::ObjectCreateImpl(JSContext* cx, HandleObject proto,
                                  NewObjectKind newKind) {
  // Give the new object a small number of fixed slots, like we do for empty
  // object literals ({}).
  gc::AllocKind allocKind = GuessObjectGCKind(0);
  return NewObjectWithGivenProtoAndKinds<PlainObject>(cx, proto, allocKind,
                                                      newKind);
}

PlainObject* js::ObjectCreateWithTemplate(JSContext* cx,
                                          HandlePlainObject templateObj) {
  RootedObject proto(cx, templateObj->staticPrototype());
  return ObjectCreateImpl(cx, proto, GenericObject);
}

// ES 2017 draft 19.1.2.3.1
static bool ObjectDefineProperties(JSContext* cx, HandleObject obj,
                                   HandleValue properties,
                                   bool* failedOnWindowProxy) {
  // Step 1. implicit
  // Step 2.
  RootedObject props(cx, ToObject(cx, properties));
  if (!props) {
    return false;
  }

  // Step 3.
  RootedIdVector keys(cx);
  if (!GetPropertyKeys(
          cx, props, JSITER_OWNONLY | JSITER_SYMBOLS | JSITER_HIDDEN, &keys)) {
    return false;
  }

  RootedId nextKey(cx);
  Rooted<Maybe<PropertyDescriptor>> keyDesc(cx);
  Rooted<PropertyDescriptor> desc(cx);
  RootedValue descObj(cx);

  // Step 4.
  Rooted<PropertyDescriptorVector> descriptors(cx,
                                               PropertyDescriptorVector(cx));
  RootedIdVector descriptorKeys(cx);

  // Step 5.
  for (size_t i = 0, len = keys.length(); i < len; i++) {
    nextKey = keys[i];

    // Step 5.a.
    if (!GetOwnPropertyDescriptor(cx, props, nextKey, &keyDesc)) {
      return false;
    }

    // Step 5.b.
    if (keyDesc.isSome() && keyDesc->enumerable()) {
      if (!GetProperty(cx, props, props, nextKey, &descObj) ||
          !ToPropertyDescriptor(cx, descObj, true, &desc) ||
          !descriptors.append(desc) || !descriptorKeys.append(nextKey)) {
        return false;
      }
    }
  }

  // Step 6.
  *failedOnWindowProxy = false;
  for (size_t i = 0, len = descriptors.length(); i < len; i++) {
    ObjectOpResult result;
    if (!DefineProperty(cx, obj, descriptorKeys[i], descriptors[i], result)) {
      return false;
    }

    if (!result.ok()) {
      if (result.failureCode() == JSMSG_CANT_DEFINE_WINDOW_NC) {
        *failedOnWindowProxy = true;
      } else if (!result.checkStrict(cx, obj, descriptorKeys[i])) {
        return false;
      }
    }
  }

  return true;
}

// ES6 draft rev34 (2015/02/20) 19.1.2.2 Object.create(O [, Properties])
bool js::obj_create(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!args.requireAtLeast(cx, "Object.create", 1)) {
    return false;
  }

  if (!args[0].isObjectOrNull()) {
    UniqueChars bytes =
        DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, args[0], nullptr);
    if (!bytes) {
      return false;
    }

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_UNEXPECTED_TYPE, bytes.get(),
                             "not an object or null");
    return false;
  }

  // Step 2.
  RootedObject proto(cx, args[0].toObjectOrNull());
  RootedPlainObject obj(cx, ObjectCreateImpl(cx, proto));
  if (!obj) {
    return false;
  }

  // Step 3.
  if (args.hasDefined(1)) {
    // we can't ever end up with failures to define on a WindowProxy
    // here, because "obj" is never a WindowProxy.
    bool failedOnWindowProxy = false;
    if (!ObjectDefineProperties(cx, obj, args[1], &failedOnWindowProxy)) {
      return false;
    }
    MOZ_ASSERT(!failedOnWindowProxy, "How did we get a WindowProxy here?");
  }

  // Step 4.
  args.rval().setObject(*obj);
  return true;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 6.2.4.4 FromPropertyDescriptor ( Desc )
static bool FromPropertyDescriptorToArray(
    JSContext* cx, Handle<Maybe<PropertyDescriptor>> desc,
    MutableHandleValue vp) {
  // Step 1.
  if (desc.isNothing()) {
    vp.setUndefined();
    return true;
  }

  // Steps 2-11.
  // Retrieve all property descriptor fields and place them into the result
  // array. The actual return object is created in self-hosted code for
  // performance reasons.

  int32_t attrsAndKind = 0;
  if (desc->enumerable()) {
    attrsAndKind |= ATTR_ENUMERABLE;
  }
  if (desc->configurable()) {
    attrsAndKind |= ATTR_CONFIGURABLE;
  }
  if (!desc->isAccessorDescriptor()) {
    if (desc->writable()) {
      attrsAndKind |= ATTR_WRITABLE;
    }
    attrsAndKind |= DATA_DESCRIPTOR_KIND;
  } else {
    attrsAndKind |= ACCESSOR_DESCRIPTOR_KIND;
  }

  RootedArrayObject result(cx);
  if (!desc->isAccessorDescriptor()) {
    result = NewDenseFullyAllocatedArray(cx, 2);
    if (!result) {
      return false;
    }
    result->setDenseInitializedLength(2);

    result->initDenseElement(PROP_DESC_ATTRS_AND_KIND_INDEX,
                             Int32Value(attrsAndKind));
    result->initDenseElement(PROP_DESC_VALUE_INDEX, desc->value());
  } else {
    result = NewDenseFullyAllocatedArray(cx, 3);
    if (!result) {
      return false;
    }
    result->setDenseInitializedLength(3);

    result->initDenseElement(PROP_DESC_ATTRS_AND_KIND_INDEX,
                             Int32Value(attrsAndKind));

    if (JSObject* get = desc->getter()) {
      result->initDenseElement(PROP_DESC_GETTER_INDEX, ObjectValue(*get));
    } else {
      result->initDenseElement(PROP_DESC_GETTER_INDEX, UndefinedValue());
    }

    if (JSObject* set = desc->setter()) {
      result->initDenseElement(PROP_DESC_SETTER_INDEX, ObjectValue(*set));
    } else {
      result->initDenseElement(PROP_DESC_SETTER_INDEX, UndefinedValue());
    }
  }

  vp.setObject(*result);
  return true;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 19.1.2.6 Object.getOwnPropertyDescriptor ( O, P )
bool js::GetOwnPropertyDescriptorToArray(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args[0]));
  if (!obj) {
    return false;
  }

  // Step 2.
  RootedId id(cx);
  if (!ToPropertyKey(cx, args[1], &id)) {
    return false;
  }

  // Step 3.
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  if (!GetOwnPropertyDescriptor(cx, obj, id, &desc)) {
    return false;
  }

  // Step 4.
  return FromPropertyDescriptorToArray(cx, desc, args.rval());
}

static bool NewValuePair(JSContext* cx, HandleValue val1, HandleValue val2,
                         MutableHandleValue rval) {
  ArrayObject* array = NewDenseFullyAllocatedArray(cx, 2);
  if (!array) {
    return false;
  }

  array->setDenseInitializedLength(2);
  array->initDenseElement(0, val1);
  array->initDenseElement(1, val2);

  rval.setObject(*array);
  return true;
}

enum class EnumerableOwnPropertiesKind { Keys, Values, KeysAndValues, Names };

static bool HasEnumerableStringNonDataProperties(NativeObject* obj) {
  // We also check for enumerability and symbol properties, so uninteresting
  // non-data properties like |array.length| don't let us fall into the slow
  // path.
  for (ShapePropertyIter<NoGC> iter(obj->shape()); !iter.done(); iter++) {
    if (!iter->isDataProperty() && iter->enumerable() &&
        !iter->key().isSymbol()) {
      return true;
    }
  }
  return false;
}

template <EnumerableOwnPropertiesKind kind>
static bool TryEnumerableOwnPropertiesNative(JSContext* cx, HandleObject obj,
                                             MutableHandleValue rval,
                                             bool* optimized) {
  *optimized = false;

  // Use the fast path if |obj| has neither extra indexed properties nor a
  // newEnumerate hook. String objects need to be special-cased, because
  // they're only marked as indexed after their enumerate hook ran. And
  // because their enumerate hook is slowish, it's more performant to
  // exclude them directly instead of executing the hook first.
  if (!obj->is<NativeObject>() || obj->as<NativeObject>().isIndexed() ||
      obj->getClass()->getNewEnumerate() || obj->is<StringObject>()) {
    return true;
  }

  HandleNativeObject nobj = obj.as<NativeObject>();

  // Resolve lazy properties on |nobj|.
  if (JSEnumerateOp enumerate = nobj->getClass()->getEnumerate()) {
    if (!enumerate(cx, nobj)) {
      return false;
    }

    // Ensure no extra indexed properties were added through enumerate().
    if (nobj->isIndexed()) {
      return true;
    }
  }

  *optimized = true;

  RootedValueVector properties(cx);
  RootedValue key(cx);
  RootedValue value(cx);

  // We have ensured |nobj| contains no extra indexed properties, so the
  // only indexed properties we need to handle here are dense and typed
  // array elements.

  for (uint32_t i = 0, len = nobj->getDenseInitializedLength(); i < len; i++) {
    value.set(nobj->getDenseElement(i));
    if (value.isMagic(JS_ELEMENTS_HOLE)) {
      continue;
    }

    JSString* str;
    if (kind != EnumerableOwnPropertiesKind::Values) {
      static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT <= JSID_INT_MAX,
                    "dense elements don't exceed JSID_INT_MAX");
      str = Int32ToString<CanGC>(cx, i);
      if (!str) {
        return false;
      }
    }

    if (kind == EnumerableOwnPropertiesKind::Keys ||
        kind == EnumerableOwnPropertiesKind::Names) {
      value.setString(str);
    } else if (kind == EnumerableOwnPropertiesKind::KeysAndValues) {
      key.setString(str);
      if (!NewValuePair(cx, key, value, &value)) {
        return false;
      }
    }

    if (!properties.append(value)) {
      return false;
    }
  }

  if (obj->is<TypedArrayObject>()) {
    Handle<TypedArrayObject*> tobj = obj.as<TypedArrayObject>();
    size_t len = tobj->length();

    // Fail early if the typed array contains too many elements for a
    // dense array, because we likely OOM anyway when trying to allocate
    // more than 2GB for the properties vector. This also means we don't
    // need to handle indices greater than MAX_INT32 in the loop below.
    if (len > NativeObject::MAX_DENSE_ELEMENTS_COUNT) {
      ReportOutOfMemory(cx);
      return false;
    }

    MOZ_ASSERT(properties.empty(), "typed arrays cannot have dense elements");
    if (!properties.resize(len)) {
      return false;
    }

    for (uint32_t i = 0; i < len; i++) {
      JSString* str;
      if (kind != EnumerableOwnPropertiesKind::Values) {
        static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT <= JSID_INT_MAX,
                      "dense elements don't exceed JSID_INT_MAX");
        str = Int32ToString<CanGC>(cx, i);
        if (!str) {
          return false;
        }
      }

      if (kind == EnumerableOwnPropertiesKind::Keys ||
          kind == EnumerableOwnPropertiesKind::Names) {
        value.setString(str);
      } else if (kind == EnumerableOwnPropertiesKind::Values) {
        if (!tobj->getElement<CanGC>(cx, i, &value)) {
          return false;
        }
      } else {
        key.setString(str);
        if (!tobj->getElement<CanGC>(cx, i, &value)) {
          return false;
        }
        if (!NewValuePair(cx, key, value, &value)) {
          return false;
        }
      }

      properties[i].set(value);
    }
  }

  // Up to this point no side-effects through accessor properties are
  // possible which could have replaced |obj| with a non-native object.
  MOZ_ASSERT(obj->is<NativeObject>());

  if (kind == EnumerableOwnPropertiesKind::Keys ||
      kind == EnumerableOwnPropertiesKind::Names ||
      !HasEnumerableStringNonDataProperties(nobj)) {
    // If |kind == Values| or |kind == KeysAndValues|:
    // All enumerable properties with string property keys are data
    // properties. This allows us to collect the property values while
    // iterating over the shape hierarchy without worrying over accessors
    // modifying any state.

    size_t elements = properties.length();
    constexpr bool onlyEnumerable = kind != EnumerableOwnPropertiesKind::Names;
    constexpr AllowGC allowGC =
        kind != EnumerableOwnPropertiesKind::KeysAndValues ? AllowGC::NoGC
                                                           : AllowGC::CanGC;
    mozilla::MaybeOneOf<ShapePropertyIter<NoGC>, ShapePropertyIter<CanGC>> m;
    if (allowGC == AllowGC::NoGC) {
      m.construct<ShapePropertyIter<NoGC>>(nobj->shape());
    } else {
      m.construct<ShapePropertyIter<CanGC>>(cx, nobj->shape());
    }
    for (ShapePropertyIter<allowGC>& iter = m.ref<ShapePropertyIter<allowGC>>();
         !iter.done(); iter++) {
      jsid id = iter->key();
      if ((onlyEnumerable && !iter->enumerable()) || id.isSymbol()) {
        continue;
      }
      MOZ_ASSERT(!JSID_IS_INT(id), "Unexpected indexed property");
      MOZ_ASSERT_IF(kind == EnumerableOwnPropertiesKind::Values ||
                        kind == EnumerableOwnPropertiesKind::KeysAndValues,
                    iter->isDataProperty());

      if (kind == EnumerableOwnPropertiesKind::Keys ||
          kind == EnumerableOwnPropertiesKind::Names) {
        value.setString(JSID_TO_STRING(id));
      } else if (kind == EnumerableOwnPropertiesKind::Values) {
        value.set(nobj->getSlot(iter->slot()));
      } else {
        key.setString(JSID_TO_STRING(id));
        value.set(nobj->getSlot(iter->slot()));
        if (!NewValuePair(cx, key, value, &value)) {
          return false;
        }
      }

      if (!properties.append(value)) {
        return false;
      }
    }

    // The (non-indexed) properties were visited in reverse iteration order,
    // call std::reverse() to ensure they appear in iteration order.
    std::reverse(properties.begin() + elements, properties.end());
  } else {
    MOZ_ASSERT(kind == EnumerableOwnPropertiesKind::Values ||
               kind == EnumerableOwnPropertiesKind::KeysAndValues);

    // Get a list of all |obj| properties. As long as obj->shape()
    // is equal to |objShape|, we can use this to speed up both the
    // enumerability check and GetProperty.
    Rooted<PropertyInfoWithKeyVector> props(cx, PropertyInfoWithKeyVector(cx));

    // Collect all non-symbol properties.
    RootedShape objShape(cx, nobj->shape());
    for (ShapePropertyIter<NoGC> iter(objShape); !iter.done(); iter++) {
      if (iter->key().isSymbol()) {
        continue;
      }
      MOZ_ASSERT(!JSID_IS_INT(iter->key()), "Unexpected indexed property");

      if (!props.append(*iter)) {
        return false;
      }
    }

    RootedId id(cx);
    for (size_t i = props.length(); i > 0; i--) {
      PropertyInfoWithKey prop = props[i - 1];
      id = prop.key();

      // If |obj| still has the same shape, it must still be a NativeObject with
      // the properties in |props|.
      if (obj->shape() == objShape && prop.isDataProperty()) {
        if (!prop.enumerable()) {
          continue;
        }
        value = obj->as<NativeObject>().getSlot(prop.slot());
      } else {
        // |obj| changed shape or the property is not a data property,
        // so we have to do the slower enumerability check and
        // GetProperty.
        bool enumerable;
        if (!PropertyIsEnumerable(cx, obj, id, &enumerable)) {
          return false;
        }
        if (!enumerable) {
          continue;
        }
        if (!GetProperty(cx, obj, obj, id, &value)) {
          return false;
        }
      }

      if (kind == EnumerableOwnPropertiesKind::KeysAndValues) {
        key.setString(JSID_TO_STRING(id));
        if (!NewValuePair(cx, key, value, &value)) {
          return false;
        }
      }

      if (!properties.append(value)) {
        return false;
      }
    }
  }

  JSObject* array =
      NewDenseCopiedArray(cx, properties.length(), properties.begin());
  if (!array) {
    return false;
  }

  rval.setObject(*array);
  return true;
}

// ES2018 draft rev c164be80f7ea91de5526b33d54e5c9321ed03d3f
// 7.3.21 EnumerableOwnProperties ( O, kind )
template <EnumerableOwnPropertiesKind kind>
static bool EnumerableOwnProperties(JSContext* cx, const JS::CallArgs& args) {
  static_assert(kind == EnumerableOwnPropertiesKind::Values ||
                    kind == EnumerableOwnPropertiesKind::KeysAndValues,
                "Only implemented for Object.keys and Object.entries");

  // Step 1. (Step 1 of Object.{keys,values,entries}, really.)
  RootedObject obj(cx, ToObject(cx, args.get(0)));
  if (!obj) {
    return false;
  }

  bool optimized;
  if (!TryEnumerableOwnPropertiesNative<kind>(cx, obj, args.rval(),
                                              &optimized)) {
    return false;
  }
  if (optimized) {
    return true;
  }

  // Typed arrays are always handled in the fast path.
  MOZ_ASSERT(!obj->is<TypedArrayObject>());

  // Step 2.
  RootedIdVector ids(cx);
  if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY | JSITER_HIDDEN, &ids)) {
    return false;
  }

  // Step 3.
  RootedValueVector properties(cx);
  size_t len = ids.length();
  if (!properties.resize(len)) {
    return false;
  }

  RootedId id(cx);
  RootedValue key(cx);
  RootedValue value(cx);
  RootedShape shape(cx);
  Rooted<Maybe<PropertyDescriptor>> desc(cx);
  // Step 4.
  size_t out = 0;
  for (size_t i = 0; i < len; i++) {
    id = ids[i];

    // Step 4.a. (Symbols were filtered out in step 2.)
    MOZ_ASSERT(!id.isSymbol());

    if (kind != EnumerableOwnPropertiesKind::Values) {
      if (!IdToStringOrSymbol(cx, id, &key)) {
        return false;
      }
    }

    // Step 4.a.i.
    if (obj->is<NativeObject>()) {
      HandleNativeObject nobj = obj.as<NativeObject>();
      if (JSID_IS_INT(id) && nobj->containsDenseElement(JSID_TO_INT(id))) {
        value.set(nobj->getDenseElement(JSID_TO_INT(id)));
      } else {
        Maybe<PropertyInfo> prop = nobj->lookup(cx, id);
        if (prop.isNothing() || !prop->enumerable()) {
          continue;
        }
        if (prop->isDataProperty()) {
          value = nobj->getSlot(prop->slot());
        } else if (!GetProperty(cx, obj, obj, id, &value)) {
          return false;
        }
      }
    } else {
      if (!GetOwnPropertyDescriptor(cx, obj, id, &desc)) {
        return false;
      }

      // Step 4.a.ii. (inverted.)
      if (desc.isNothing() || !desc->enumerable()) {
        continue;
      }

      // Step 4.a.ii.1.
      // (Omitted because Object.keys doesn't use this implementation.)

      // Step 4.a.ii.2.a.
      if (!GetProperty(cx, obj, obj, id, &value)) {
        return false;
      }
    }

    // Steps 4.a.ii.2.b-c.
    if (kind == EnumerableOwnPropertiesKind::Values) {
      properties[out++].set(value);
    } else if (!NewValuePair(cx, key, value, properties[out++])) {
      return false;
    }
  }

  // Step 5.
  // (Implemented in step 2.)

  // Step 3 of Object.{keys,values,entries}
  JSObject* aobj = NewDenseCopiedArray(cx, out, properties.begin());
  if (!aobj) {
    return false;
  }

  args.rval().setObject(*aobj);
  return true;
}

// ES2018 draft rev c164be80f7ea91de5526b33d54e5c9321ed03d3f
// 19.1.2.16 Object.keys ( O )
static bool obj_keys(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedObject obj(cx, ToObject(cx, args.get(0)));
  if (!obj) {
    return false;
  }

  bool optimized;
  static constexpr EnumerableOwnPropertiesKind kind =
      EnumerableOwnPropertiesKind::Keys;
  if (!TryEnumerableOwnPropertiesNative<kind>(cx, obj, args.rval(),
                                              &optimized)) {
    return false;
  }
  if (optimized) {
    return true;
  }

  // Steps 2-3.
  return GetOwnPropertyKeys(cx, obj, JSITER_OWNONLY, args.rval());
}

// ES2018 draft rev c164be80f7ea91de5526b33d54e5c9321ed03d3f
// 19.1.2.21 Object.values ( O )
static bool obj_values(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-3.
  return EnumerableOwnProperties<EnumerableOwnPropertiesKind::Values>(cx, args);
}

// ES2018 draft rev c164be80f7ea91de5526b33d54e5c9321ed03d3f
// 19.1.2.5 Object.entries ( O )
static bool obj_entries(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-3.
  return EnumerableOwnProperties<EnumerableOwnPropertiesKind::KeysAndValues>(
      cx, args);
}

/* ES6 draft 15.2.3.16 */
bool js::obj_is(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  bool same;
  if (!SameValue(cx, args.get(0), args.get(1), &same)) {
    return false;
  }

  args.rval().setBoolean(same);
  return true;
}

bool js::IdToStringOrSymbol(JSContext* cx, HandleId id,
                            MutableHandleValue result) {
  if (id.isInt()) {
    JSString* str = Int32ToString<CanGC>(cx, id.toInt());
    if (!str) {
      return false;
    }
    result.setString(str);
  } else if (id.isAtom()) {
    result.setString(id.toAtom());
  } else {
    result.setSymbol(id.toSymbol());
  }
  return true;
}

// ES2018 draft rev c164be80f7ea91de5526b33d54e5c9321ed03d3f
// 19.1.2.10.1 Runtime Semantics: GetOwnPropertyKeys ( O, Type )
bool js::GetOwnPropertyKeys(JSContext* cx, HandleObject obj, unsigned flags,
                            MutableHandleValue rval) {
  // Step 1 (Performed in caller).

  // Steps 2-4.
  RootedIdVector keys(cx);
  if (!GetPropertyKeys(cx, obj, flags, &keys)) {
    return false;
  }

  // Step 5 (Inlined CreateArrayFromList).
  RootedArrayObject array(cx, NewDenseFullyAllocatedArray(cx, keys.length()));
  if (!array) {
    return false;
  }

  array->ensureDenseInitializedLength(0, keys.length());

  RootedValue val(cx);
  for (size_t i = 0, len = keys.length(); i < len; i++) {
    MOZ_ASSERT_IF(keys[i].isSymbol(), flags & JSITER_SYMBOLS);
    MOZ_ASSERT_IF(!keys[i].isSymbol(), !(flags & JSITER_SYMBOLSONLY));
    if (!IdToStringOrSymbol(cx, keys[i], &val)) {
      return false;
    }
    array->initDenseElement(i, val);
  }

  rval.setObject(*array);
  return true;
}

// ES2018 draft rev c164be80f7ea91de5526b33d54e5c9321ed03d3f
// 19.1.2.9 Object.getOwnPropertyNames ( O )
static bool obj_getOwnPropertyNames(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject obj(cx, ToObject(cx, args.get(0)));
  if (!obj) {
    return false;
  }

  bool optimized;
  static constexpr EnumerableOwnPropertiesKind kind =
      EnumerableOwnPropertiesKind::Names;
  if (!TryEnumerableOwnPropertiesNative<kind>(cx, obj, args.rval(),
                                              &optimized)) {
    return false;
  }
  if (optimized) {
    return true;
  }

  return GetOwnPropertyKeys(cx, obj, JSITER_OWNONLY | JSITER_HIDDEN,
                            args.rval());
}

// ES2018 draft rev c164be80f7ea91de5526b33d54e5c9321ed03d3f
// 19.1.2.10 Object.getOwnPropertySymbols ( O )
static bool obj_getOwnPropertySymbols(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject obj(cx, ToObject(cx, args.get(0)));
  if (!obj) {
    return false;
  }

  return GetOwnPropertyKeys(
      cx, obj,
      JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS | JSITER_SYMBOLSONLY,
      args.rval());
}

/* ES5 15.2.3.7: Object.defineProperties(O, Properties) */
static bool obj_defineProperties(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Step 1. */
  RootedObject obj(cx);
  if (!GetFirstArgumentAsObject(cx, args, "Object.defineProperties", &obj)) {
    return false;
  }

  /* Step 2. */
  if (!args.requireAtLeast(cx, "Object.defineProperties", 2)) {
    return false;
  }

  /* Steps 3-6. */
  bool failedOnWindowProxy = false;
  if (!ObjectDefineProperties(cx, obj, args[1], &failedOnWindowProxy)) {
    return false;
  }

  /* Step 7, but modified to deal with WindowProxy mess */
  if (failedOnWindowProxy) {
    args.rval().setNull();
  } else {
    args.rval().setObject(*obj);
  }
  return true;
}

// ES6 20141014 draft 19.1.2.15 Object.preventExtensions(O)
static bool obj_preventExtensions(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().set(args.get(0));

  // Step 1.
  if (!args.get(0).isObject()) {
    return true;
  }

  // Steps 2-5.
  RootedObject obj(cx, &args.get(0).toObject());
  return PreventExtensions(cx, obj);
}

// ES6 draft rev27 (2014/08/24) 19.1.2.5 Object.freeze(O)
static bool obj_freeze(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().set(args.get(0));

  // Step 1.
  if (!args.get(0).isObject()) {
    return true;
  }

  // Steps 2-5.
  RootedObject obj(cx, &args.get(0).toObject());
  return SetIntegrityLevel(cx, obj, IntegrityLevel::Frozen);
}

// ES6 draft rev27 (2014/08/24) 19.1.2.12 Object.isFrozen(O)
static bool obj_isFrozen(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  bool frozen = true;

  // Step 2.
  if (args.get(0).isObject()) {
    RootedObject obj(cx, &args.get(0).toObject());
    if (!TestIntegrityLevel(cx, obj, IntegrityLevel::Frozen, &frozen)) {
      return false;
    }
  }
  args.rval().setBoolean(frozen);
  return true;
}

// ES6 draft rev27 (2014/08/24) 19.1.2.17 Object.seal(O)
static bool obj_seal(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().set(args.get(0));

  // Step 1.
  if (!args.get(0).isObject()) {
    return true;
  }

  // Steps 2-5.
  RootedObject obj(cx, &args.get(0).toObject());
  return SetIntegrityLevel(cx, obj, IntegrityLevel::Sealed);
}

// ES6 draft rev27 (2014/08/24) 19.1.2.13 Object.isSealed(O)
static bool obj_isSealed(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  bool sealed = true;

  // Step 2.
  if (args.get(0).isObject()) {
    RootedObject obj(cx, &args.get(0).toObject());
    if (!TestIntegrityLevel(cx, obj, IntegrityLevel::Sealed, &sealed)) {
      return false;
    }
  }
  args.rval().setBoolean(sealed);
  return true;
}

bool js::obj_setProto(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  HandleValue thisv = args.thisv();
  if (thisv.isNullOrUndefined()) {
    ReportIncompatible(cx, args);
    return false;
  }
  if (thisv.isPrimitive()) {
    // Mutating a boxed primitive's [[Prototype]] has no side effects.
    args.rval().setUndefined();
    return true;
  }

  /* Do nothing if __proto__ isn't being set to an object or null. */
  if (!args[0].isObjectOrNull()) {
    args.rval().setUndefined();
    return true;
  }

  Rooted<JSObject*> obj(cx, &args.thisv().toObject());
  Rooted<JSObject*> newProto(cx, args[0].toObjectOrNull());
  if (!SetPrototype(cx, obj, newProto)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static const JSFunctionSpec object_methods[] = {
    JS_FN(js_toSource_str, obj_toSource, 0, 0),
    JS_INLINABLE_FN(js_toString_str, obj_toString, 0, 0, ObjectToString),
    JS_SELF_HOSTED_FN(js_toLocaleString_str, "Object_toLocaleString", 0, 0),
    JS_SELF_HOSTED_FN(js_valueOf_str, "Object_valueOf", 0, 0),
    JS_SELF_HOSTED_FN(js_hasOwnProperty_str, "Object_hasOwnProperty", 1, 0),
    JS_INLINABLE_FN(js_isPrototypeOf_str, obj_isPrototypeOf, 1, 0,
                    ObjectIsPrototypeOf),
    JS_FN(js_propertyIsEnumerable_str, obj_propertyIsEnumerable, 1, 0),
    JS_SELF_HOSTED_FN(js_defineGetter_str, "ObjectDefineGetter", 2, 0),
    JS_SELF_HOSTED_FN(js_defineSetter_str, "ObjectDefineSetter", 2, 0),
    JS_SELF_HOSTED_FN(js_lookupGetter_str, "ObjectLookupGetter", 1, 0),
    JS_SELF_HOSTED_FN(js_lookupSetter_str, "ObjectLookupSetter", 1, 0),
    JS_FS_END};

static const JSPropertySpec object_properties[] = {
    JS_SELF_HOSTED_GETSET("__proto__", "$ObjectProtoGetter",
                          "$ObjectProtoSetter", 0),
    JS_PS_END};

static const JSFunctionSpec object_static_methods[] = {
    JS_FN("assign", obj_assign, 2, 0),
    JS_SELF_HOSTED_FN("getPrototypeOf", "ObjectGetPrototypeOf", 1, 0),
    JS_FN("setPrototypeOf", obj_setPrototypeOf, 2, 0),
    JS_SELF_HOSTED_FN("getOwnPropertyDescriptor",
                      "ObjectGetOwnPropertyDescriptor", 2, 0),
    JS_SELF_HOSTED_FN("getOwnPropertyDescriptors",
                      "ObjectGetOwnPropertyDescriptors", 1, 0),
    JS_FN("keys", obj_keys, 1, 0),
    JS_FN("values", obj_values, 1, 0),
    JS_FN("entries", obj_entries, 1, 0),
    JS_INLINABLE_FN("is", obj_is, 2, 0, ObjectIs),
    JS_SELF_HOSTED_FN("defineProperty", "ObjectDefineProperty", 3, 0),
    JS_FN("defineProperties", obj_defineProperties, 2, 0),
    JS_INLINABLE_FN("create", obj_create, 2, 0, ObjectCreate),
    JS_FN("getOwnPropertyNames", obj_getOwnPropertyNames, 1, 0),
    JS_FN("getOwnPropertySymbols", obj_getOwnPropertySymbols, 1, 0),
    JS_SELF_HOSTED_FN("isExtensible", "ObjectIsExtensible", 1, 0),
    JS_FN("preventExtensions", obj_preventExtensions, 1, 0),
    JS_FN("freeze", obj_freeze, 1, 0),
    JS_FN("isFrozen", obj_isFrozen, 1, 0),
    JS_FN("seal", obj_seal, 1, 0),
    JS_FN("isSealed", obj_isSealed, 1, 0),
    JS_SELF_HOSTED_FN("fromEntries", "ObjectFromEntries", 1, 0),
/* Proposal */
#ifdef NIGHTLY_BUILD
    JS_SELF_HOSTED_FN("hasOwn", "ObjectHasOwn", 2, 0),
#endif
    JS_FS_END};

static JSObject* CreateObjectConstructor(JSContext* cx, JSProtoKey key) {
  Rooted<GlobalObject*> self(cx, cx->global());
  if (!GlobalObject::ensureConstructor(cx, self, JSProto_Function)) {
    return nullptr;
  }

  /* Create the Object function now that we have a [[Prototype]] for it. */
  JSFunction* fun = NewNativeConstructor(
      cx, obj_construct, 1, HandlePropertyName(cx->names().Object),
      gc::AllocKind::FUNCTION, TenuredObject);
  if (!fun) {
    return nullptr;
  }

  fun->setJitInfo(&jit::JitInfo_Object);
  return fun;
}

static JSObject* CreateObjectPrototype(JSContext* cx, JSProtoKey key) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  MOZ_ASSERT(cx->global()->is<NativeObject>());

  /*
   * Create |Object.prototype| first, mirroring CreateBlankProto but for the
   * prototype of the created object.
   */
  RootedPlainObject objectProto(
      cx, NewTenuredObjectWithGivenProto<PlainObject>(cx, nullptr));
  if (!objectProto) {
    return nullptr;
  }

  bool succeeded;
  if (!SetImmutablePrototype(cx, objectProto, &succeeded)) {
    return nullptr;
  }
  MOZ_ASSERT(succeeded,
             "should have been able to make a fresh Object.prototype's "
             "[[Prototype]] immutable");

  return objectProto;
}

static bool FinishObjectClassInit(JSContext* cx, JS::HandleObject ctor,
                                  JS::HandleObject proto) {
  Rooted<GlobalObject*> global(cx, cx->global());

  /* ES5 15.1.2.1. */
  RootedId evalId(cx, NameToId(cx->names().eval));
  JSObject* evalobj =
      DefineFunction(cx, global, evalId, IndirectEval, 1, JSPROP_RESOLVING);
  if (!evalobj) {
    return false;
  }
  global->setOriginalEval(evalobj);

#ifdef FUZZING
  if (cx->options().fuzzing()) {
    if (!DefineTestingFunctions(cx, global, /* fuzzingSafe = */ true,
                                /* disableOOMFunctions = */ false)) {
      return false;
    }
  }
#endif

  Rooted<NativeObject*> holder(cx,
                               GlobalObject::getIntrinsicsHolder(cx, global));
  if (!holder) {
    return false;
  }

  /*
   * The global object should have |Object.prototype| as its [[Prototype]].
   * Eventually we'd like to have standard classes be there from the start,
   * and thus we would know we were always setting what had previously been a
   * null [[Prototype]], but right now some code assumes it can set the
   * [[Prototype]] before standard classes have been initialized.  For now,
   * only set the [[Prototype]] if it hasn't already been set.
   */
  if (global->staticPrototype() == nullptr) {
    MOZ_ASSERT(!global->staticPrototypeIsImmutable());
    if (!SetPrototype(cx, global, proto)) {
      return false;
    }
  }
  return true;
}

static const ClassSpec PlainObjectClassSpec = {
    CreateObjectConstructor, CreateObjectPrototype,
    object_static_methods,   nullptr,
    object_methods,          object_properties,
    FinishObjectClassInit};

const JSClass PlainObject::class_ = {js_Object_str,
                                     JSCLASS_HAS_CACHED_PROTO(JSProto_Object),
                                     JS_NULL_CLASS_OPS, &PlainObjectClassSpec};

const JSClass* const js::ObjectClassPtr = &PlainObject::class_;
