/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Symbol.h"
#include "js/Symbol.h"

#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/SymbolType.h"

#include "vm/JSObject-inl.h"

using namespace js;

const JSClass SymbolObject::class_ = {
    "Symbol",
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Symbol),
    JS_NULL_CLASS_OPS, &SymbolObject::classSpec_};

// This uses PlainObject::class_ because: "The Symbol prototype object is an
// ordinary object. It is not a Symbol instance and does not have a
// [[SymbolData]] internal slot." (ES6 rev 24, 19.4.3)
const JSClass& SymbolObject::protoClass_ = PlainObject::class_;

SymbolObject* SymbolObject::create(JSContext* cx, JS::HandleSymbol symbol) {
  SymbolObject* obj = NewBuiltinClassInstance<SymbolObject>(cx);
  if (!obj) {
    return nullptr;
  }
  obj->setPrimitiveValue(symbol);
  return obj;
}

const JSPropertySpec SymbolObject::properties[] = {
    JS_PSG("description", descriptionGetter, 0),
    JS_STRING_SYM_PS(toStringTag, "Symbol", JSPROP_READONLY), JS_PS_END};

const JSFunctionSpec SymbolObject::methods[] = {
    JS_FN(js_toString_str, toString, 0, 0),
    JS_FN(js_valueOf_str, valueOf, 0, 0),
    JS_SYM_FN(toPrimitive, toPrimitive, 1, JSPROP_READONLY), JS_FS_END};

const JSFunctionSpec SymbolObject::staticMethods[] = {
    JS_FN("for", for_, 1, 0), JS_FN("keyFor", keyFor, 1, 0), JS_FS_END};

static bool SymbolClassFinish(JSContext* cx, HandleObject ctor,
                              HandleObject proto) {
  Handle<NativeObject*> nativeCtor = ctor.as<NativeObject>();

  // Define the well-known symbol properties, such as Symbol.iterator.
  ImmutableTenuredPtr<PropertyName*>* names =
      cx->names().wellKnownSymbolNames();
  RootedValue value(cx);
  unsigned attrs = JSPROP_READONLY | JSPROP_PERMANENT;
  WellKnownSymbols* wks = cx->runtime()->wellKnownSymbols;
  for (size_t i = 0; i < JS::WellKnownSymbolLimit; i++) {
    value.setSymbol(wks->get(i));
    if (!NativeDefineDataProperty(cx, nativeCtor, names[i], value, attrs)) {
      return false;
    }
  }
  return true;
}

const ClassSpec SymbolObject::classSpec_ = {
    GenericCreateConstructor<SymbolObject::construct, 0,
                             gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<SymbolObject>,
    staticMethods,
    nullptr,
    methods,
    properties,
    SymbolClassFinish};

// ES2020 draft rev ecb4178012d6b4d9abc13fcbd45f5c6394b832ce
// 19.4.1.1 Symbol ( [ description ] )
bool SymbolObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (args.isConstructing()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_CONSTRUCTOR, "Symbol");
    return false;
  }

  // Steps 2-3.
  RootedString desc(cx);
  if (!args.get(0).isUndefined()) {
    desc = ToString(cx, args.get(0));
    if (!desc) {
      return false;
    }
  }

  // Step 4.
  JS::Symbol* symbol = JS::Symbol::new_(cx, JS::SymbolCode::UniqueSymbol, desc);
  if (!symbol) {
    return false;
  }
  args.rval().setSymbol(symbol);
  return true;
}

// ES2020 draft rev ecb4178012d6b4d9abc13fcbd45f5c6394b832ce
// 19.4.2.2 Symbol.for ( key )
bool SymbolObject::for_(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  RootedString stringKey(cx, ToString(cx, args.get(0)));
  if (!stringKey) {
    return false;
  }

  // Steps 2-6.
  JS::Symbol* symbol = JS::Symbol::for_(cx, stringKey);
  if (!symbol) {
    return false;
  }
  args.rval().setSymbol(symbol);
  return true;
}

// ES2020 draft rev ecb4178012d6b4d9abc13fcbd45f5c6394b832ce
// 19.4.2.6 Symbol.keyFor ( sym )
bool SymbolObject::keyFor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  HandleValue arg = args.get(0);
  if (!arg.isSymbol()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, arg,
                     nullptr, "not a symbol");
    return false;
  }

  // Step 2.
  if (arg.toSymbol()->code() == JS::SymbolCode::InSymbolRegistry) {
#ifdef DEBUG
    RootedString desc(cx, arg.toSymbol()->description());
    MOZ_ASSERT(JS::Symbol::for_(cx, desc) == arg.toSymbol());
#endif
    args.rval().setString(arg.toSymbol()->description());
    return true;
  }

  // Step 3: omitted.
  // Step 4.
  args.rval().setUndefined();
  return true;
}

static MOZ_ALWAYS_INLINE bool IsSymbol(HandleValue v) {
  return v.isSymbol() || (v.isObject() && v.toObject().is<SymbolObject>());
}

// ES2020 draft rev ecb4178012d6b4d9abc13fcbd45f5c6394b832ce
// 19.4.3 Properties of the Symbol Prototype Object, thisSymbolValue.
static MOZ_ALWAYS_INLINE JS::Symbol* ThisSymbolValue(HandleValue val) {
  // Step 3, the error case, is handled by CallNonGenericMethod.
  MOZ_ASSERT(IsSymbol(val));

  // Step 1.
  if (val.isSymbol()) {
    return val.toSymbol();
  }

  // Step 2.
  return val.toObject().as<SymbolObject>().unbox();
}

// ES2020 draft rev ecb4178012d6b4d9abc13fcbd45f5c6394b832ce
// 19.4.3.3 Symbol.prototype.toString ( )
bool SymbolObject::toString_impl(JSContext* cx, const CallArgs& args) {
  // Step 1.
  JS::Symbol* sym = ThisSymbolValue(args.thisv());

  // Step 2.
  return SymbolDescriptiveString(cx, sym, args.rval());
}

bool SymbolObject::toString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsSymbol, toString_impl>(cx, args);
}

// ES2020 draft rev ecb4178012d6b4d9abc13fcbd45f5c6394b832ce
// 19.4.3.4 Symbol.prototype.valueOf ( )
bool SymbolObject::valueOf_impl(JSContext* cx, const CallArgs& args) {
  // Step 1.
  args.rval().setSymbol(ThisSymbolValue(args.thisv()));
  return true;
}

bool SymbolObject::valueOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsSymbol, valueOf_impl>(cx, args);
}

// ES2020 draft rev ecb4178012d6b4d9abc13fcbd45f5c6394b832ce
// 19.4.3.5 Symbol.prototype [ @@toPrimitive ] ( hint )
bool SymbolObject::toPrimitive(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // The specification gives exactly the same algorithm for @@toPrimitive as
  // for valueOf, so reuse the valueOf implementation.
  return CallNonGenericMethod<IsSymbol, valueOf_impl>(cx, args);
}

// ES2020 draft rev ecb4178012d6b4d9abc13fcbd45f5c6394b832ce
// 19.4.3.2 get Symbol.prototype.description
bool SymbolObject::descriptionGetter_impl(JSContext* cx, const CallArgs& args) {
  // Steps 1-2.
  JS::Symbol* sym = ThisSymbolValue(args.thisv());

  // Step 3.
  // Return the symbol's description if present, otherwise return undefined.
  if (JSString* str = sym->description()) {
    args.rval().setString(str);
  } else {
    args.rval().setUndefined();
  }
  return true;
}

bool SymbolObject::descriptionGetter(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsSymbol, descriptionGetter_impl>(cx, args);
}
