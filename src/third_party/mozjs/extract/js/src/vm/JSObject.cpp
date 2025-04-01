/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS object implementation.
 */

#include "vm/JSObject-inl.h"

#include "mozilla/MemoryReporting.h"
#include "mozilla/Try.h"

#include <string.h>

#include "jsapi.h"
#include "jsexn.h"
#include "jsfriendapi.h"
#include "jsnum.h"
#include "jstypes.h"

#include "builtin/BigInt.h"
#include "builtin/MapObject.h"
#include "builtin/Object.h"
#include "builtin/String.h"
#include "builtin/Symbol.h"
#include "builtin/WeakSetObject.h"
#include "gc/AllocKind.h"
#include "gc/GC.h"
#include "js/CharacterEncoding.h"
#include "js/friend/DumpFunctions.h"  // js::DumpObject
#include "js/friend/ErrorMessages.h"  // JSErrNum, js::GetErrorMessage, JSMSG_*
#include "js/friend/WindowProxy.h"    // js::IsWindow, js::ToWindowProxyIfWindow
#include "js/MemoryMetrics.h"
#include "js/Prefs.h"               // JS::Prefs
#include "js/Printer.h"             // js::GenericPrinter, js::Fprinter
#include "js/PropertyDescriptor.h"  // JS::FromPropertyDescriptor
#include "js/PropertySpec.h"        // JSPropertySpec
#include "js/Proxy.h"
#include "js/Result.h"
#include "js/UbiNode.h"
#include "js/Wrapper.h"
#include "proxy/DeadObjectProxy.h"
#include "util/Memory.h"
#include "util/Text.h"
#include "util/WindowsWrapper.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayBufferObject.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/Compartment.h"
#include "vm/DateObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSAtomUtils.h"  // Atomize
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSONPrinter.h"  // js::JSONPrinter
#include "vm/JSScript.h"
#include "vm/PromiseObject.h"
#include "vm/ProxyObject.h"
#include "vm/RegExpObject.h"
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/TypedArrayObject.h"
#include "vm/Watchtower.h"
#include "vm/WrapperObject.h"
#ifdef ENABLE_RECORD_TUPLE
#  include "builtin/RecordObject.h"
#  include "builtin/TupleObject.h"
#  include "vm/RecordType.h"
#  include "vm/TupleType.h"
#endif

#include "gc/StableCellHasher-inl.h"
#include "vm/BooleanObject-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSAtomUtils-inl.h"  // AtomToId, PrimitiveValueToId, IndexToId
#include "vm/JSContext-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/NumberObject-inl.h"
#include "vm/ObjectFlags-inl.h"
#include "vm/Realm-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/TypedArrayObject-inl.h"
#include "wasm/WasmGcObject-inl.h"

using namespace js;

using mozilla::Maybe;

void js::ReportNotObject(JSContext* cx, JSErrNum err, int spindex,
                         HandleValue v) {
  MOZ_ASSERT(!v.isObject());
  ReportValueError(cx, err, spindex, v, nullptr);
}

void js::ReportNotObject(JSContext* cx, JSErrNum err, HandleValue v) {
  ReportNotObject(cx, err, JSDVG_SEARCH_STACK, v);
}

void js::ReportNotObject(JSContext* cx, const Value& v) {
  RootedValue value(cx, v);
  ReportNotObject(cx, JSMSG_OBJECT_REQUIRED, value);
}

void js::ReportNotObjectArg(JSContext* cx, const char* nth, const char* fun,
                            HandleValue v) {
  MOZ_ASSERT(!v.isObject());

  UniqueChars bytes;
  if (const char* chars = ValueToSourceForError(cx, v, bytes)) {
    JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr,
                               JSMSG_OBJECT_REQUIRED_ARG, nth, fun, chars);
  }
}

JS_PUBLIC_API const char* JS::InformalValueTypeName(const Value& v) {
  switch (v.type()) {
    case ValueType::Double:
    case ValueType::Int32:
      return "number";
    case ValueType::Boolean:
      return "boolean";
    case ValueType::Undefined:
      return "undefined";
    case ValueType::Null:
      return "null";
    case ValueType::String:
      return "string";
    case ValueType::Symbol:
      return "symbol";
    case ValueType::BigInt:
      return "bigint";
    case ValueType::Object:
#ifdef ENABLE_RECORD_TUPLE
    case ValueType::ExtendedPrimitive:
#endif
      return v.getObjectPayload().getClass()->name;
    case ValueType::Magic:
      return "magic";
    case ValueType::PrivateGCThing:
      break;
  }

  MOZ_CRASH("unexpected type");
}

// ES6 draft rev37 6.2.4.4 FromPropertyDescriptor
JS_PUBLIC_API bool JS::FromPropertyDescriptor(
    JSContext* cx, Handle<Maybe<PropertyDescriptor>> desc_,
    MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(desc_);

  // Step 1.
  if (desc_.isNothing()) {
    vp.setUndefined();
    return true;
  }

  Rooted<PropertyDescriptor> desc(cx, *desc_);
  return FromPropertyDescriptorToObject(cx, desc, vp);
}

bool js::FromPropertyDescriptorToObject(JSContext* cx,
                                        Handle<PropertyDescriptor> desc,
                                        MutableHandleValue vp) {
  // Step 2-3.
  RootedObject obj(cx, NewPlainObject(cx));
  if (!obj) {
    return false;
  }

  const JSAtomState& names = cx->names();

  // Step 4.
  if (desc.hasValue()) {
    if (!DefineDataProperty(cx, obj, names.value, desc.value())) {
      return false;
    }
  }

  // Step 5.
  RootedValue v(cx);
  if (desc.hasWritable()) {
    v.setBoolean(desc.writable());
    if (!DefineDataProperty(cx, obj, names.writable, v)) {
      return false;
    }
  }

  // Step 6.
  if (desc.hasGetter()) {
    if (JSObject* get = desc.getter()) {
      v.setObject(*get);
    } else {
      v.setUndefined();
    }
    if (!DefineDataProperty(cx, obj, names.get, v)) {
      return false;
    }
  }

  // Step 7.
  if (desc.hasSetter()) {
    if (JSObject* set = desc.setter()) {
      v.setObject(*set);
    } else {
      v.setUndefined();
    }
    if (!DefineDataProperty(cx, obj, names.set, v)) {
      return false;
    }
  }

  // Step 8.
  if (desc.hasEnumerable()) {
    v.setBoolean(desc.enumerable());
    if (!DefineDataProperty(cx, obj, names.enumerable, v)) {
      return false;
    }
  }

  // Step 9.
  if (desc.hasConfigurable()) {
    v.setBoolean(desc.configurable());
    if (!DefineDataProperty(cx, obj, names.configurable, v)) {
      return false;
    }
  }

  vp.setObject(*obj);
  return true;
}

bool js::GetFirstArgumentAsObject(JSContext* cx, const CallArgs& args,
                                  const char* method,
                                  MutableHandleObject objp) {
  if (!args.requireAtLeast(cx, method, 1)) {
    return false;
  }

  HandleValue v = args[0];
  if (!v.isObject()) {
    UniqueChars bytes =
        DecompileValueGenerator(cx, JSDVG_SEARCH_STACK, v, nullptr);
    if (!bytes) {
      return false;
    }
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_UNEXPECTED_TYPE, bytes.get(),
                             "not an object");
    return false;
  }

  objp.set(&v.toObject());
  return true;
}

static bool GetPropertyIfPresent(JSContext* cx, HandleObject obj, HandleId id,
                                 MutableHandleValue vp, bool* foundp) {
  if (!HasProperty(cx, obj, id, foundp)) {
    return false;
  }
  if (!*foundp) {
    vp.setUndefined();
    return true;
  }

  return GetProperty(cx, obj, obj, id, vp);
}

bool js::Throw(JSContext* cx, HandleId id, unsigned errorNumber,
               const char* details) {
  MOZ_ASSERT(js_ErrorFormatString[errorNumber].argCount == (details ? 2 : 1));
  MOZ_ASSERT_IF(details, JS::StringIsASCII(details));

  UniqueChars bytes =
      IdToPrintableUTF8(cx, id, IdToPrintableBehavior::IdIsPropertyKey);
  if (!bytes) {
    return false;
  }

  if (details) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber,
                             bytes.get(), details);
  } else {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber,
                             bytes.get());
  }

  return false;
}

/*** PropertyDescriptor operations and DefineProperties *********************/

static Result<> CheckCallable(JSContext* cx, JSObject* obj,
                              const char* fieldName) {
  if (obj && !obj->isCallable()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_GET_SET_FIELD, fieldName);
    return cx->alreadyReportedError();
  }
  return Ok();
}

// 6.2.5.5 ToPropertyDescriptor(Obj)
bool js::ToPropertyDescriptor(JSContext* cx, HandleValue descval,
                              bool checkAccessors,
                              MutableHandle<PropertyDescriptor> desc_) {
  // Step 1.
  RootedObject obj(cx,
                   RequireObject(cx, JSMSG_OBJECT_REQUIRED_PROP_DESC, descval));
  if (!obj) {
    return false;
  }

  // Step 2.
  Rooted<PropertyDescriptor> desc(cx, PropertyDescriptor::Empty());

  RootedId id(cx);
  RootedValue v(cx);

  // Steps 3-4.
  id = NameToId(cx->names().enumerable);
  bool hasEnumerable = false;
  if (!GetPropertyIfPresent(cx, obj, id, &v, &hasEnumerable)) {
    return false;
  }
  if (hasEnumerable) {
    desc.setEnumerable(ToBoolean(v));
  }

  // Steps 5-6.
  id = NameToId(cx->names().configurable);
  bool hasConfigurable = false;
  if (!GetPropertyIfPresent(cx, obj, id, &v, &hasConfigurable)) {
    return false;
  }
  if (hasConfigurable) {
    desc.setConfigurable(ToBoolean(v));
  }

  // Steps 7-8.
  id = NameToId(cx->names().value);
  bool hasValue = false;
  if (!GetPropertyIfPresent(cx, obj, id, &v, &hasValue)) {
    return false;
  }
  if (hasValue) {
    desc.setValue(v);
  }

  // Steps 9-10.
  id = NameToId(cx->names().writable);
  bool hasWritable = false;
  if (!GetPropertyIfPresent(cx, obj, id, &v, &hasWritable)) {
    return false;
  }
  if (hasWritable) {
    desc.setWritable(ToBoolean(v));
  }

  // Steps 11-12.
  id = NameToId(cx->names().get);
  bool hasGet = false;
  if (!GetPropertyIfPresent(cx, obj, id, &v, &hasGet)) {
    return false;
  }
  RootedObject getter(cx);
  if (hasGet) {
    if (v.isObject()) {
      if (checkAccessors) {
        JS_TRY_OR_RETURN_FALSE(cx, CheckCallable(cx, &v.toObject(), "getter"));
      }
      getter = &v.toObject();
    } else if (v.isUndefined()) {
      getter = nullptr;
    } else {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_GET_SET_FIELD, "getter");
      return false;
    }
  }

  // Steps 13-14.
  id = NameToId(cx->names().set);
  bool hasSet = false;
  if (!GetPropertyIfPresent(cx, obj, id, &v, &hasSet)) {
    return false;
  }
  RootedObject setter(cx);
  if (hasSet) {
    if (v.isObject()) {
      if (checkAccessors) {
        JS_TRY_OR_RETURN_FALSE(cx, CheckCallable(cx, &v.toObject(), "setter"));
      }
      setter = &v.toObject();
    } else if (v.isUndefined()) {
      setter = nullptr;
    } else {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_GET_SET_FIELD, "setter");
      return false;
    }
  }

  // Step 15.
  if (hasGet || hasSet) {
    if (hasValue || hasWritable) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_INVALID_DESCRIPTOR);
      return false;
    }

    // We delay setGetter/setSetter after the previous check,
    // because otherwise we would assert.
    if (hasGet) {
      desc.setGetter(getter);
    }
    if (hasSet) {
      desc.setSetter(setter);
    }
  }

  desc.assertValid();
  desc_.set(desc);
  return true;
}

Result<> js::CheckPropertyDescriptorAccessors(JSContext* cx,
                                              Handle<PropertyDescriptor> desc) {
  if (desc.hasGetter()) {
    MOZ_TRY(CheckCallable(cx, desc.getter(), "getter"));
  }

  if (desc.hasSetter()) {
    MOZ_TRY(CheckCallable(cx, desc.setter(), "setter"));
  }

  return Ok();
}

// 6.2.5.6 CompletePropertyDescriptor(Desc)
void js::CompletePropertyDescriptor(MutableHandle<PropertyDescriptor> desc) {
  // Step 1.
  desc.assertValid();

  // Step 2.
  // Let like be the Record { [[Value]]: undefined, [[Writable]]: false,
  //                          [[Get]]: undefined, [[Set]]: undefined,
  //                          [[Enumerable]]: false, [[Configurable]]: false }.

  // Step 3.
  if (desc.isGenericDescriptor() || desc.isDataDescriptor()) {
    // Step 3.a.
    if (!desc.hasValue()) {
      desc.setValue(UndefinedHandleValue);
    }
    // Step 3.b.
    if (!desc.hasWritable()) {
      desc.setWritable(false);
    }
  } else {
    // Step 4.a.
    if (!desc.hasGetter()) {
      desc.setGetter(nullptr);
    }
    // Step 4.b.
    if (!desc.hasSetter()) {
      desc.setSetter(nullptr);
    }
  }

  // Step 5.
  if (!desc.hasEnumerable()) {
    desc.setEnumerable(false);
  }

  // Step 6.
  if (!desc.hasConfigurable()) {
    desc.setConfigurable(false);
  }

  desc.assertComplete();
}

bool js::ReadPropertyDescriptors(
    JSContext* cx, HandleObject props, bool checkAccessors,
    MutableHandleIdVector ids, MutableHandle<PropertyDescriptorVector> descs) {
  if (!GetPropertyKeys(cx, props, JSITER_OWNONLY | JSITER_SYMBOLS, ids)) {
    return false;
  }

  RootedId id(cx);
  for (size_t i = 0, len = ids.length(); i < len; i++) {
    id = ids[i];
    Rooted<PropertyDescriptor> desc(cx);
    RootedValue v(cx);
    if (!GetProperty(cx, props, props, id, &v) ||
        !ToPropertyDescriptor(cx, v, checkAccessors, &desc) ||
        !descs.append(desc)) {
      return false;
    }
  }
  return true;
}

/*** Seal and freeze ********************************************************/

/* ES6 draft rev 29 (6 Dec 2014) 7.3.13. */
bool js::SetIntegrityLevel(JSContext* cx, HandleObject obj,
                           IntegrityLevel level) {
  cx->check(obj);

  // Steps 3-5. (Steps 1-2 are redundant assertions.)
  if (!PreventExtensions(cx, obj)) {
    return false;
  }

  // Steps 6-9, loosely interpreted.
  if (obj->is<NativeObject>() && !obj->is<TypedArrayObject>() &&
      !obj->is<MappedArgumentsObject>()) {
    Handle<NativeObject*> nobj = obj.as<NativeObject>();

    // Use a fast path to seal/freeze properties. This has the benefit of
    // creating shared property maps if possible, whereas the slower/generic
    // implementation below ends up converting non-empty objects to dictionary
    // mode.
    if (nobj->shape()->propMapLength() > 0) {
      if (!NativeObject::freezeOrSealProperties(cx, nobj, level)) {
        return false;
      }
    }

    // Ordinarily ArraySetLength handles this, but we're going behind its back
    // right now, so we must do this manually.
    if (level == IntegrityLevel::Frozen && obj->is<ArrayObject>()) {
      obj->as<ArrayObject>().setNonWritableLength(cx);
    }
  } else {
    // Steps 6-7.
    RootedIdVector keys(cx);
    if (!GetPropertyKeys(
            cx, obj, JSITER_HIDDEN | JSITER_OWNONLY | JSITER_SYMBOLS, &keys)) {
      return false;
    }

    RootedId id(cx);
    Rooted<PropertyDescriptor> desc(cx, PropertyDescriptor::Empty());

    // 8.a/9.a. The two different loops are merged here.
    for (size_t i = 0; i < keys.length(); i++) {
      id = keys[i];

      if (level == IntegrityLevel::Sealed) {
        // 8.a.i.
        desc.setConfigurable(false);
      } else {
        // 9.a.i-ii.
        Rooted<Maybe<PropertyDescriptor>> currentDesc(cx);
        if (!GetOwnPropertyDescriptor(cx, obj, id, &currentDesc)) {
          return false;
        }

        // 9.a.iii.
        if (currentDesc.isNothing()) {
          continue;
        }

        // 9.a.iii.1-2
        desc = PropertyDescriptor::Empty();
        if (currentDesc->isAccessorDescriptor()) {
          desc.setConfigurable(false);
        } else {
          desc.setConfigurable(false);
          desc.setWritable(false);
        }
      }

      // 8.a.i-ii. / 9.a.iii.3-4
      if (!DefineProperty(cx, obj, id, desc)) {
        return false;
      }
    }
  }

  // Finally, freeze or seal the dense elements.
  if (obj->is<NativeObject>()) {
    if (!ObjectElements::FreezeOrSeal(cx, obj.as<NativeObject>(), level)) {
      return false;
    }
  }

  return true;
}

static bool ResolveLazyProperties(JSContext* cx, Handle<NativeObject*> obj) {
  const JSClass* clasp = obj->getClass();
  if (JSEnumerateOp enumerate = clasp->getEnumerate()) {
    if (!enumerate(cx, obj)) {
      return false;
    }
  }
  if (clasp->getNewEnumerate() && clasp->getResolve()) {
    RootedIdVector properties(cx);
    if (!clasp->getNewEnumerate()(cx, obj, &properties,
                                  /* enumerableOnly = */ false)) {
      return false;
    }

    RootedId id(cx);
    for (size_t i = 0; i < properties.length(); i++) {
      id = properties[i];
      bool found;
      if (!HasOwnProperty(cx, obj, id, &found)) {
        return false;
      }
    }
  }
  return true;
}

// ES6 draft rev33 (12 Feb 2015) 7.3.15
bool js::TestIntegrityLevel(JSContext* cx, HandleObject obj,
                            IntegrityLevel level, bool* result) {
  // Steps 3-6. (Steps 1-2 are redundant assertions.)
  bool status;
  if (!IsExtensible(cx, obj, &status)) {
    return false;
  }
  if (status) {
    *result = false;
    return true;
  }

  // Fast path for native objects.
  if (obj->is<NativeObject>()) {
    Handle<NativeObject*> nobj = obj.as<NativeObject>();

    // Force lazy properties to be resolved.
    if (!ResolveLazyProperties(cx, nobj)) {
      return false;
    }

    // Typed array elements are configurable, writable properties, so if any
    // elements are present, the typed array can neither be sealed nor frozen.
    if (nobj->is<TypedArrayObject>() &&
        nobj->as<TypedArrayObject>().length().valueOr(0) > 0) {
      *result = false;
      return true;
    }

    bool hasDenseElements = false;
    for (size_t i = 0; i < nobj->getDenseInitializedLength(); i++) {
      if (nobj->containsDenseElement(i)) {
        hasDenseElements = true;
        break;
      }
    }

    if (hasDenseElements) {
      // Unless the sealed flag is set, dense elements are configurable.
      if (!nobj->denseElementsAreSealed()) {
        *result = false;
        return true;
      }

      // Unless the frozen flag is set, dense elements are writable.
      if (level == IntegrityLevel::Frozen && !nobj->denseElementsAreFrozen()) {
        *result = false;
        return true;
      }
    }

    // Steps 7-9.
    for (ShapePropertyIter<NoGC> iter(nobj->shape()); !iter.done(); iter++) {
      // Steps 9.c.i-ii.
      if (iter->configurable() ||
          (level == IntegrityLevel::Frozen && iter->isDataDescriptor() &&
           iter->writable())) {
        // Private fields on objects don't participate in the frozen state, and
        // so should be elided from checking for frozen state.
        if (iter->key().isPrivateName()) {
          continue;
        }

        *result = false;
        return true;
      }
    }
  } else {
    // Steps 7-8.
    RootedIdVector props(cx);
    if (!GetPropertyKeys(
            cx, obj, JSITER_HIDDEN | JSITER_OWNONLY | JSITER_SYMBOLS, &props)) {
      return false;
    }

    // Step 9.
    RootedId id(cx);
    Rooted<Maybe<PropertyDescriptor>> desc(cx);
    for (size_t i = 0, len = props.length(); i < len; i++) {
      id = props[i];

      // Steps 9.a-b.
      if (!GetOwnPropertyDescriptor(cx, obj, id, &desc)) {
        return false;
      }

      // Step 9.c.
      if (desc.isNothing()) {
        continue;
      }

      // Steps 9.c.i-ii.
      if (desc->configurable() ||
          (level == IntegrityLevel::Frozen && desc->isDataDescriptor() &&
           desc->writable())) {
        // Since we don't request JSITER_PRIVATE in GetPropertyKeys above, we
        // should never see a private name here.
        MOZ_ASSERT(!id.isPrivateName());
        *result = false;
        return true;
      }
    }
  }

  // Step 10.
  *result = true;
  return true;
}

/* * */

static MOZ_ALWAYS_INLINE NativeObject* NewObject(
    JSContext* cx, const JSClass* clasp, Handle<TaggedProto> proto,
    gc::AllocKind kind, NewObjectKind newKind, ObjectFlags objFlags) {
  MOZ_ASSERT(clasp->isNativeObject());

  // Some classes have specialized allocation functions and shouldn't end up
  // here.
  MOZ_ASSERT(clasp != &ArrayObject::class_);
  MOZ_ASSERT(clasp != &PlainObject::class_);
  MOZ_ASSERT(!clasp->isJSFunction());

  // Computing nfixed based on the AllocKind isn't right for objects which can
  // store fixed data inline (TypedArrays and ArrayBuffers) so for simplicity
  // and performance reasons we don't support such objects here.
  MOZ_ASSERT(!ClassCanHaveFixedData(clasp));
  size_t nfixed = GetGCKindSlots(kind);

  if (CanChangeToBackgroundAllocKind(kind, clasp)) {
    kind = ForegroundToBackgroundAllocKind(kind);
  }

  Rooted<SharedShape*> shape(
      cx, SharedShape::getInitialShape(cx, clasp, cx->realm(), proto, nfixed,
                                       objFlags));
  if (!shape) {
    return nullptr;
  }

  gc::Heap heap = GetInitialHeap(newKind, clasp);
  NativeObject* obj = NativeObject::create(cx, kind, heap, shape);
  if (!obj) {
    return nullptr;
  }

  probes::CreateObject(cx, obj);
  return obj;
}

NativeObject* js::NewObjectWithGivenTaggedProto(
    JSContext* cx, const JSClass* clasp, Handle<TaggedProto> proto,
    gc::AllocKind allocKind, NewObjectKind newKind, ObjectFlags objFlags) {
  return NewObject(cx, clasp, proto, allocKind, newKind, objFlags);
}

NativeObject* js::NewObjectWithClassProto(JSContext* cx, const JSClass* clasp,
                                          HandleObject protoArg,
                                          gc::AllocKind allocKind,
                                          NewObjectKind newKind,
                                          ObjectFlags objFlags) {
  if (protoArg) {
    return NewObjectWithGivenTaggedProto(cx, clasp, AsTaggedProto(protoArg),
                                         allocKind, newKind, objFlags);
  }

  // Find the appropriate proto for clasp. Built-in classes have a cached
  // proto on cx->global(); all others get %ObjectPrototype%.
  JSProtoKey protoKey = JSCLASS_CACHED_PROTO_KEY(clasp);
  if (protoKey == JSProto_Null) {
    protoKey = JSProto_Object;
  }

  JSObject* proto = GlobalObject::getOrCreatePrototype(cx, protoKey);
  if (!proto) {
    return nullptr;
  }

  Rooted<TaggedProto> taggedProto(cx, TaggedProto(proto));
  return NewObject(cx, clasp, taggedProto, allocKind, newKind, objFlags);
}

bool js::GetPrototypeFromConstructor(JSContext* cx, HandleObject newTarget,
                                     JSProtoKey intrinsicDefaultProto,
                                     MutableHandleObject proto) {
  RootedValue protov(cx);
  if (!GetProperty(cx, newTarget, newTarget, cx->names().prototype, &protov)) {
    return false;
  }
  if (protov.isObject()) {
    proto.set(&protov.toObject());
  } else if (newTarget->is<JSFunction>() &&
             newTarget->as<JSFunction>().realm() == cx->realm()) {
    // Steps 4.a-b fetch the builtin prototype of the current realm, which we
    // represent as nullptr.
    proto.set(nullptr);
  } else if (intrinsicDefaultProto == JSProto_Null) {
    // Bug 1317416. The caller did not pass a reasonable JSProtoKey, so let the
    // caller select a prototype object. Most likely they will choose one from
    // the wrong realm.
    proto.set(nullptr);
  } else {
    // Step 4.a: Let realm be ? GetFunctionRealm(constructor);
    Realm* realm = JS::GetFunctionRealm(cx, newTarget);
    if (!realm) {
      return false;
    }

    // Step 4.b: Set proto to realm's intrinsic object named
    //           intrinsicDefaultProto.
    {
      Maybe<AutoRealm> ar;
      if (cx->realm() != realm) {
        ar.emplace(cx, realm->maybeGlobal());
      }
      proto.set(GlobalObject::getOrCreatePrototype(cx, intrinsicDefaultProto));
    }
    if (!proto) {
      return false;
    }
    if (!cx->compartment()->wrap(cx, proto)) {
      return false;
    }
  }
  return true;
}

/* static */
bool JSObject::nonNativeSetProperty(JSContext* cx, HandleObject obj,
                                    HandleId id, HandleValue v,
                                    HandleValue receiver,
                                    ObjectOpResult& result) {
  return obj->getOpsSetProperty()(cx, obj, id, v, receiver, result);
}

/* static */
bool JSObject::nonNativeSetElement(JSContext* cx, HandleObject obj,
                                   uint32_t index, HandleValue v,
                                   HandleValue receiver,
                                   ObjectOpResult& result) {
  RootedId id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return nonNativeSetProperty(cx, obj, id, v, receiver, result);
}

static bool CopyPropertyFrom(JSContext* cx, HandleId id, HandleObject target,
                             HandleObject obj) {
  // |target| must not be a CCW because we need to enter its realm below and
  // CCWs are not associated with a single realm.
  MOZ_ASSERT(!IsCrossCompartmentWrapper(target));

  // |obj| and |cx| are generally not same-compartment with |target| here.
  cx->check(obj, id);
  Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);

  if (!GetOwnPropertyDescriptor(cx, obj, id, &desc)) {
    return false;
  }
  MOZ_ASSERT(desc.isSome());

  JSAutoRealm ar(cx, target);
  cx->markId(id);
  RootedId wrappedId(cx, id);
  if (!cx->compartment()->wrap(cx, &desc)) {
    return false;
  }

  Rooted<PropertyDescriptor> desc_(cx, *desc);
  return DefineProperty(cx, target, wrappedId, desc_);
}

JS_PUBLIC_API bool JS_CopyOwnPropertiesAndPrivateFields(JSContext* cx,
                                                        HandleObject target,
                                                        HandleObject obj) {
  // Both |obj| and |target| must not be CCWs because we need to enter their
  // realms below and CCWs are not associated with a single realm.
  MOZ_ASSERT(!IsCrossCompartmentWrapper(obj));
  MOZ_ASSERT(!IsCrossCompartmentWrapper(target));

  JSAutoRealm ar(cx, obj);

  RootedIdVector props(cx);
  if (!GetPropertyKeys(
          cx, obj,
          JSITER_PRIVATE | JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS,
          &props)) {
    return false;
  }

  for (size_t i = 0; i < props.length(); ++i) {
    if (!CopyPropertyFrom(cx, props[i], target, obj)) {
      return false;
    }
  }

  return true;
}

static bool InitializePropertiesFromCompatibleNativeObject(
    JSContext* cx, Handle<NativeObject*> dst, Handle<NativeObject*> src) {
  cx->check(src, dst);
  MOZ_ASSERT(src->getClass() == dst->getClass());
  MOZ_ASSERT(dst->shape()->objectFlags().isEmpty());
  MOZ_ASSERT(src->numFixedSlots() == dst->numFixedSlots());
  MOZ_ASSERT(!src->inDictionaryMode());
  MOZ_ASSERT(!dst->inDictionaryMode());

  if (!dst->ensureElements(cx, src->getDenseInitializedLength())) {
    return false;
  }

  uint32_t initialized = src->getDenseInitializedLength();
  for (uint32_t i = 0; i < initialized; ++i) {
    dst->setDenseInitializedLength(i + 1);
    dst->initDenseElement(i, src->getDenseElement(i));
  }

  // If there are no properties to copy, we're done.
  if (!src->sharedShape()->propMap()) {
    return true;
  }

  Rooted<SharedShape*> shape(cx);
  if (src->staticPrototype() == dst->staticPrototype()) {
    shape = src->sharedShape();
  } else {
    // We need to generate a new shape for dst that has dst's proto but all
    // the property information from src.  Note that we asserted above that
    // dst's object flags are empty.
    SharedShape* srcShape = src->sharedShape();
    ObjectFlags objFlags;
    objFlags = CopyPropMapObjectFlags(objFlags, srcShape->objectFlags());
    Rooted<SharedPropMap*> map(cx, srcShape->propMap());
    uint32_t mapLength = srcShape->propMapLength();
    shape = SharedShape::getPropMapShape(cx, dst->shape()->base(),
                                         dst->numFixedSlots(), map, mapLength,
                                         objFlags);
    if (!shape) {
      return false;
    }
  }

  uint32_t oldSpan = dst->sharedShape()->slotSpan();
  uint32_t newSpan = shape->slotSpan();
  if (!dst->setShapeAndAddNewSlots(cx, shape, oldSpan, newSpan)) {
    return false;
  }
  for (size_t i = JSCLASS_RESERVED_SLOTS(src->getClass()); i < newSpan; i++) {
    dst->setSlot(i, src->getSlot(i));
  }

  return true;
}

JS_PUBLIC_API bool JS_InitializePropertiesFromCompatibleNativeObject(
    JSContext* cx, HandleObject dst, HandleObject src) {
  return InitializePropertiesFromCompatibleNativeObject(
      cx, dst.as<NativeObject>(), src.as<NativeObject>());
}

bool js::ObjectMayBeSwapped(const JSObject* obj) {
  const JSClass* clasp = obj->getClass();

  // We want to optimize Window/globals and Gecko doesn't require transplanting
  // them (only the WindowProxy around them). A Window may be a DOMClass, so we
  // explicitly check if this is a global.
  if (clasp->isGlobal()) {
    return false;
  }

  // WindowProxy, Wrapper, DeadProxyObject, DOMProxy, and DOMClass (non-global)
  // types may be swapped. It is hard to detect DOMProxy from shell, so target
  // proxies in general.
  return clasp->isProxyObject() || clasp->isDOMClass();
}

bool NativeObject::prepareForSwap(JSContext* cx,
                                  MutableHandleValueVector slotValuesOut) {
  MOZ_ASSERT(slotValuesOut.empty());

  for (size_t i = 0; i < slotSpan(); i++) {
    if (!slotValuesOut.append(getSlot(i))) {
      return false;
    }
  }

  if (hasDynamicSlots()) {
    ObjectSlots* slotsHeader = getSlotsHeader();
    size_t size = ObjectSlots::allocSize(slotsHeader->capacity());
    RemoveCellMemory(this, size, MemoryUse::ObjectSlots);
    if (!cx->nursery().isInside(slotsHeader)) {
      if (!isTenured()) {
        cx->nursery().removeMallocedBuffer(slotsHeader, size);
      }
      js_free(slotsHeader);
    }
    setEmptyDynamicSlots(0);
  }

  if (hasDynamicElements()) {
    ObjectElements* elements = getElementsHeader();
    void* allocatedElements = getUnshiftedElementsHeader();
    size_t count = elements->numAllocatedElements();
    size_t size = count * sizeof(HeapSlot);

    if (isTenured()) {
      RemoveCellMemory(this, size, MemoryUse::ObjectElements);
    } else if (cx->nursery().isInside(allocatedElements)) {
      // Move nursery allocated elements in case they end up in a tenured
      // object.
      ObjectElements* newElements =
          reinterpret_cast<ObjectElements*>(js_pod_malloc<HeapSlot>(count));
      if (!newElements) {
        return false;
      }

      memmove(newElements, elements, size);
      elements_ = newElements->elements();
    } else {
      cx->nursery().removeMallocedBuffer(allocatedElements, size);
    }
    MOZ_ASSERT(hasDynamicElements());
  }

  return true;
}

/* static */
bool NativeObject::fixupAfterSwap(JSContext* cx, Handle<NativeObject*> obj,
                                  gc::AllocKind kind,
                                  HandleValueVector slotValues) {
  // This object has just been swapped with some other object, and its shape
  // no longer reflects its allocated size. Correct this information and
  // fill the slots in with the specified values.
  MOZ_ASSERT_IF(!obj->inDictionaryMode(),
                obj->slotSpan() == slotValues.length());

  // Make sure the shape's numFixedSlots() is correct.
  size_t nfixed = gc::GetGCKindSlots(kind);
  if (nfixed != obj->shape()->numFixedSlots()) {
    if (!NativeObject::changeNumFixedSlotsAfterSwap(cx, obj, nfixed)) {
      return false;
    }
    MOZ_ASSERT(obj->shape()->numFixedSlots() == nfixed);
  }

  uint32_t oldDictionarySlotSpan =
      obj->inDictionaryMode() ? slotValues.length() : 0;

  MOZ_ASSERT(!obj->hasUniqueId());
  size_t ndynamic =
      calculateDynamicSlots(nfixed, slotValues.length(), obj->getClass());
  size_t currentSlots = obj->getSlotsHeader()->capacity();
  MOZ_ASSERT(ndynamic >= currentSlots);
  if (ndynamic > currentSlots) {
    if (!obj->growSlots(cx, currentSlots, ndynamic)) {
      return false;
    }
  }

  if (obj->inDictionaryMode()) {
    obj->setDictionaryModeSlotSpan(oldDictionarySlotSpan);
  }

  for (size_t i = 0, len = slotValues.length(); i < len; i++) {
    obj->initSlotUnchecked(i, slotValues[i]);
  }

  if (obj->hasDynamicElements()) {
    ObjectElements* elements = obj->getElementsHeader();
    void* allocatedElements = obj->getUnshiftedElementsHeader();
    MOZ_ASSERT(!cx->nursery().isInside(allocatedElements));
    size_t size = elements->numAllocatedElements() * sizeof(HeapSlot);
    if (obj->isTenured()) {
      AddCellMemory(obj, size, MemoryUse::ObjectElements);
    } else if (!cx->nursery().registerMallocedBuffer(allocatedElements, size)) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] bool ProxyObject::prepareForSwap(
    JSContext* cx, MutableHandleValueVector valuesOut) {
  MOZ_ASSERT(valuesOut.empty());

  // Remove the GCPtr<Value>s we're about to swap from the store buffer, to
  // ensure we don't trace bogus values.
  gc::StoreBuffer& sb = cx->runtime()->gc.storeBuffer();

  // Reserve space for the expando, private slot and the reserved slots.
  if (!valuesOut.reserve(2 + numReservedSlots())) {
    return false;
  }

  js::detail::ProxyValueArray* valArray = data.values();
  sb.unputValue(&valArray->expandoSlot);
  sb.unputValue(&valArray->privateSlot);
  valuesOut.infallibleAppend(valArray->expandoSlot);
  valuesOut.infallibleAppend(valArray->privateSlot);

  for (size_t i = 0; i < numReservedSlots(); i++) {
    sb.unputValue(&valArray->reservedSlots.slots[i]);
    valuesOut.infallibleAppend(valArray->reservedSlots.slots[i]);
  }

  if (isTenured() && !usingInlineValueArray()) {
    size_t count = detail::ProxyValueArray::allocCount(numReservedSlots());
    RemoveCellMemory(this, count * sizeof(Value),
                     MemoryUse::ProxyExternalValueArray);
    js_free(valArray);
    data.reservedSlots = nullptr;
  }

  return true;
}

bool ProxyObject::fixupAfterSwap(JSContext* cx,
                                 const HandleValueVector values) {
  MOZ_ASSERT(getClass()->isProxyObject());

  size_t nreserved = numReservedSlots();

  // |values| contains the expando slot, private slot and the reserved slots.
  MOZ_ASSERT(values.length() == 2 + nreserved);

  // Allocate the external value array in malloc memory, even for nursery
  // proxies.
  size_t count = detail::ProxyValueArray::allocCount(nreserved);
  auto* allocation = js_pod_malloc<JS::Value>(count);
  if (!allocation) {
    return false;
  }

  size_t size = count * sizeof(Value);
  if (isTenured()) {
    AddCellMemory(&asTenured(), size, MemoryUse::ProxyExternalValueArray);
  } else if (!cx->nursery().registerMallocedBuffer(allocation, size)) {
    js_free(allocation);
    return false;
  }

  auto* valArray = reinterpret_cast<js::detail::ProxyValueArray*>(allocation);

  valArray->expandoSlot = values[0];
  valArray->privateSlot = values[1];

  for (size_t i = 0; i < nreserved; i++) {
    valArray->reservedSlots.slots[i] = values[i + 2];
  }

  data.reservedSlots = &valArray->reservedSlots;
  MOZ_ASSERT(!usingInlineValueArray());
  return true;
}

static gc::AllocKind SwappableObjectAllocKind(JSObject* obj) {
  MOZ_ASSERT(ObjectMayBeSwapped(obj));

  if (obj->isTenured()) {
    return obj->asTenured().getAllocKind();
  }

  if (obj->is<NativeObject>()) {
    return obj->as<NativeObject>().allocKindForTenure();
  }

  return obj->as<ProxyObject>().allocKindForTenure();
}

/* Use this method with extreme caution. It trades the guts of two objects. */
void JSObject::swap(JSContext* cx, HandleObject a, HandleObject b,
                    AutoEnterOOMUnsafeRegion& oomUnsafe) {
  // Ensure swap doesn't cause a finalizer to be run at the wrong time.
  MOZ_ASSERT(a->isBackgroundFinalized() == b->isBackgroundFinalized());

  MOZ_ASSERT(a->compartment() == b->compartment());

  // You must have entered the objects' compartment before calling this.
  MOZ_ASSERT(cx->compartment() == a->compartment());

  // Only certain types of objects are allowed to be swapped. This allows the
  // JITs to better optimize objects that can never swap and rules out most
  // builtin objects that have special behaviour.
  MOZ_RELEASE_ASSERT(js::ObjectMayBeSwapped(a));
  MOZ_RELEASE_ASSERT(js::ObjectMayBeSwapped(b));

  // Don't allow a GC which may observe intermediate state or run before we
  // execute all necessary barriers.
  gc::AutoSuppressGC nogc(cx);

  if (!Watchtower::watchObjectSwap(cx, a, b)) {
    oomUnsafe.crash("watchObjectSwap");
  }

  // Ensure we update any embedded nursery pointers in either object.
  gc::StoreBuffer& storeBuffer = cx->runtime()->gc.storeBuffer();
  if (a->isTenured()) {
    storeBuffer.putWholeCell(a);
  }
  if (b->isTenured()) {
    storeBuffer.putWholeCell(b);
  }
  if (a->isTenured() || b->isTenured()) {
    if (a->zone()->wasGCStarted()) {
      storeBuffer.setMayHavePointersToDeadCells();
    }
  }

  unsigned r = NotifyGCPreSwap(a, b);

  ProxyObject* pa = a->is<ProxyObject>() ? &a->as<ProxyObject>() : nullptr;
  ProxyObject* pb = b->is<ProxyObject>() ? &b->as<ProxyObject>() : nullptr;
  bool aIsProxyWithInlineValues = pa && pa->usingInlineValueArray();
  bool bIsProxyWithInlineValues = pb && pb->usingInlineValueArray();

  bool aIsUsedAsPrototype = a->isUsedAsPrototype();
  bool bIsUsedAsPrototype = b->isUsedAsPrototype();

  // Swap element associations.
  Zone* zone = a->zone();

  // Record any associated unique IDs and prepare for swap.
  //
  // Note that unique IDs are NOT swapped but remain associated with the
  // original address.
  uint64_t aid = 0;
  uint64_t bid = 0;
  (void)gc::MaybeGetUniqueId(a, &aid);
  (void)gc::MaybeGetUniqueId(b, &bid);
  NativeObject* na = a->is<NativeObject>() ? &a->as<NativeObject>() : nullptr;
  NativeObject* nb = b->is<NativeObject>() ? &b->as<NativeObject>() : nullptr;
  if ((aid || bid) && (na || nb)) {
    // We can't remove unique IDs from native objects when they are swapped with
    // objects without an ID. Instead ensure they both have IDs so we always
    // have something to overwrite the old ID with.
    if (!gc::GetOrCreateUniqueId(a, &aid) ||
        !gc::GetOrCreateUniqueId(b, &bid)) {
      oomUnsafe.crash("Failed to create unique ID during swap");
    }

    // IDs stored in NativeObjects could shadow those stored in the zone
    // table. Remove any zone table IDs first.
    if (pa && aid) {
      gc::RemoveUniqueId(a);
    }
    if (pb && bid) {
      gc::RemoveUniqueId(b);
    }
  }

  gc::AllocKind ka = SwappableObjectAllocKind(a);
  gc::AllocKind kb = SwappableObjectAllocKind(b);

  size_t sa = gc::Arena::thingSize(ka);
  size_t sb = gc::Arena::thingSize(kb);
  if (sa == sb && a->isTenured() == b->isTenured()) {
    // When both objects are the same size and in the same heap, just do a plain
    // swap of their contents.

    // Swap slot associations.
    zone->swapCellMemory(a, b, MemoryUse::ObjectSlots);

    size_t size = sa;
    char tmp[sizeof(JSObject_Slots16)];
    MOZ_ASSERT(size <= sizeof(tmp));

    js_memcpy(tmp, a, size);
    js_memcpy(a, b, size);
    js_memcpy(b, tmp, size);

    zone->swapCellMemory(a, b, MemoryUse::ObjectElements);
    zone->swapCellMemory(a, b, MemoryUse::ProxyExternalValueArray);

    if (aIsProxyWithInlineValues) {
      b->as<ProxyObject>().setInlineValueArray();
    }
    if (bIsProxyWithInlineValues) {
      a->as<ProxyObject>().setInlineValueArray();
    }
  } else {
    // When the objects have different sizes, they will have different numbers
    // of fixed slots before and after the swap, so the slots for native objects
    // will need to be rearranged. Remember the original values from the
    // objects.
    RootedValueVector avals(cx);
    RootedValueVector bvals(cx);
    if (na && !na->prepareForSwap(cx, &avals)) {
      oomUnsafe.crash("NativeObject::prepareForSwap");
    }
    if (nb && !nb->prepareForSwap(cx, &bvals)) {
      oomUnsafe.crash("NativeObject::prepareForSwap");
    }

    // Do the same for proxy value arrays.
    if (pa && !pa->prepareForSwap(cx, &avals)) {
      oomUnsafe.crash("ProxyObject::prepareForSwap");
    }
    if (pb && !pb->prepareForSwap(cx, &bvals)) {
      oomUnsafe.crash("ProxyObject::prepareForSwap");
    }

    // Swap the main fields of the objects, whether they are native objects or
    // proxies.
    char tmp[sizeof(JSObject_Slots0)];
    js_memcpy(&tmp, a, sizeof tmp);
    js_memcpy(a, b, sizeof tmp);
    js_memcpy(b, &tmp, sizeof tmp);

    if (na &&
        !NativeObject::fixupAfterSwap(cx, b.as<NativeObject>(), kb, avals)) {
      oomUnsafe.crash("NativeObject::fixupAfterSwap");
    }
    if (nb &&
        !NativeObject::fixupAfterSwap(cx, a.as<NativeObject>(), ka, bvals)) {
      oomUnsafe.crash("NativeObject::fixupAfterSwap");
    }

    if (pa && !b->as<ProxyObject>().fixupAfterSwap(cx, avals)) {
      oomUnsafe.crash("ProxyObject::fixupAfterSwap");
    }
    if (pb && !a->as<ProxyObject>().fixupAfterSwap(cx, bvals)) {
      oomUnsafe.crash("ProxyObject::fixupAfterSwap");
    }
  }

  // Restore original unique IDs.
  if ((aid || bid) && (na || nb)) {
    if ((aid && !gc::SetOrUpdateUniqueId(cx, a, aid)) ||
        (bid && !gc::SetOrUpdateUniqueId(cx, b, bid))) {
      oomUnsafe.crash("Failed to set unique ID after swap");
    }
  }
  MOZ_ASSERT_IF(aid, gc::GetUniqueIdInfallible(a) == aid);
  MOZ_ASSERT_IF(bid, gc::GetUniqueIdInfallible(b) == bid);

  // Preserve the IsUsedAsPrototype flag on the objects.
  if (aIsUsedAsPrototype) {
    if (!JSObject::setIsUsedAsPrototype(cx, a)) {
      oomUnsafe.crash("setIsUsedAsPrototype");
    }
  }
  if (bIsUsedAsPrototype) {
    if (!JSObject::setIsUsedAsPrototype(cx, b)) {
      oomUnsafe.crash("setIsUsedAsPrototype");
    }
  }

  /*
   * We need a write barrier here. If |a| was marked and |b| was not, then
   * after the swap, |b|'s guts would never be marked. The write barrier
   * solves this.
   *
   * Normally write barriers happen before the write. However, that's not
   * necessary here because nothing is being destroyed. We're just swapping.
   */
  PreWriteBarrier(zone, a.get(), [](JSTracer* trc, JSObject* obj) {
    obj->traceChildren(trc);
  });
  PreWriteBarrier(zone, b.get(), [](JSTracer* trc, JSObject* obj) {
    obj->traceChildren(trc);
  });

  NotifyGCPostSwap(a, b, r);
}

static NativeObject* DefineConstructorAndPrototype(
    JSContext* cx, HandleObject obj, Handle<JSAtom*> atom,
    HandleObject protoProto, const JSClass* clasp, Native constructor,
    unsigned nargs, const JSPropertySpec* ps, const JSFunctionSpec* fs,
    const JSPropertySpec* static_ps, const JSFunctionSpec* static_fs,
    NativeObject** ctorp) {
  // Create the prototype object.
  Rooted<NativeObject*> proto(
      cx, GlobalObject::createBlankPrototypeInheriting(cx, clasp, protoProto));
  if (!proto) {
    return nullptr;
  }

  Rooted<NativeObject*> ctor(cx);
  if (!constructor) {
    ctor = proto;
  } else {
    ctor = NewNativeConstructor(cx, constructor, nargs, atom);
    if (!ctor) {
      return nullptr;
    }

    if (!LinkConstructorAndPrototype(cx, ctor, proto)) {
      return nullptr;
    }
  }

  if (!DefinePropertiesAndFunctions(cx, proto, ps, fs) ||
      (ctor != proto &&
       !DefinePropertiesAndFunctions(cx, ctor, static_ps, static_fs))) {
    return nullptr;
  }

  RootedId id(cx, AtomToId(atom));
  RootedValue value(cx, ObjectValue(*ctor));
  if (!DefineDataProperty(cx, obj, id, value, 0)) {
    return nullptr;
  }

  if (ctorp) {
    *ctorp = ctor;
  }
  return proto;
}

NativeObject* js::InitClass(JSContext* cx, HandleObject obj,
                            const JSClass* protoClass, HandleObject protoProto_,
                            const char* name, Native constructor,
                            unsigned nargs, const JSPropertySpec* ps,
                            const JSFunctionSpec* fs,
                            const JSPropertySpec* static_ps,
                            const JSFunctionSpec* static_fs,
                            NativeObject** ctorp) {
  Rooted<JSAtom*> atom(cx, Atomize(cx, name, strlen(name)));
  if (!atom) {
    return nullptr;
  }

  /*
   * All instances of the class will inherit properties from the prototype
   * object we are about to create (in DefineConstructorAndPrototype), which
   * in turn will inherit from protoProto.
   *
   * If protoProto is nullptr, default to Object.prototype.
   * If protoClass is nullptr, default to PlainObject.
   */
  RootedObject protoProto(cx, protoProto_);
  if (!protoProto) {
    protoProto = &cx->global()->getObjectPrototype();
  }
  if (!protoClass) {
    protoClass = &PlainObject::class_;
  }

  return DefineConstructorAndPrototype(cx, obj, atom, protoProto, protoClass,
                                       constructor, nargs, ps, fs, static_ps,
                                       static_fs, ctorp);
}

/**
 * Returns the original Object.prototype from the embedding-provided incumbent
 * global.
 *
 * Really, we want the incumbent global itself so we can pass it to other
 * embedding hooks which need it. Specifically, the enqueue promise hook
 * takes an incumbent global so it can set that on the PromiseCallbackJob
 * it creates.
 *
 * The reason for not just returning the global itself is that we'd need to
 * wrap it into the current compartment, and later unwrap it. Unwrapping
 * globals is tricky, though: we might accidentally unwrap through an inner
 * to its outer window and end up with the wrong global. Plain objects don't
 * have this problem, so we use the global's Object.prototype. The code using
 * it - e.g. EnqueuePromiseReactionJob - can then unwrap the object and get
 * its global without fear of unwrapping too far.
 */
bool js::GetObjectFromIncumbentGlobal(JSContext* cx, MutableHandleObject obj) {
  Rooted<GlobalObject*> globalObj(cx, cx->runtime()->getIncumbentGlobal(cx));
  if (!globalObj) {
    obj.set(nullptr);
    return true;
  }

  obj.set(&globalObj->getObjectPrototype());

  // The object might be from a different compartment, so wrap it.
  if (obj && !cx->compartment()->wrap(cx, obj)) {
    return false;
  }

  return true;
}

static bool IsStandardPrototype(JSObject* obj, JSProtoKey key) {
  return obj->nonCCWGlobal().maybeGetPrototype(key) == obj;
}

JSProtoKey JS::IdentifyStandardInstance(JSObject* obj) {
  // Note: The prototype shares its JSClass with instances.
  MOZ_ASSERT(!obj->is<CrossCompartmentWrapperObject>());
  JSProtoKey key = StandardProtoKeyOrNull(obj);
  if (key != JSProto_Null && !IsStandardPrototype(obj, key)) {
    return key;
  }
  return JSProto_Null;
}

JSProtoKey JS::IdentifyStandardPrototype(JSObject* obj) {
  // Note: The prototype shares its JSClass with instances.
  MOZ_ASSERT(!obj->is<CrossCompartmentWrapperObject>());
  JSProtoKey key = StandardProtoKeyOrNull(obj);
  if (key != JSProto_Null && IsStandardPrototype(obj, key)) {
    return key;
  }
  return JSProto_Null;
}

JSProtoKey JS::IdentifyStandardInstanceOrPrototype(JSObject* obj) {
  return StandardProtoKeyOrNull(obj);
}

JSProtoKey JS::IdentifyStandardConstructor(JSObject* obj) {
  // Note that isNativeConstructor does not imply that we are a standard
  // constructor, but the converse is true (at least until we start having
  // self-hosted constructors for standard classes). This lets us avoid a costly
  // loop for many functions (which, depending on the call site, may be the
  // common case).
  if (!obj->is<JSFunction>() ||
      !(obj->as<JSFunction>().flags().isNativeConstructor())) {
    return JSProto_Null;
  }

  static_assert(JSProto_Null == 0,
                "Loop below can start at 1 to skip JSProto_Null");

  GlobalObject& global = obj->as<JSFunction>().global();
  for (size_t k = 1; k < JSProto_LIMIT; ++k) {
    JSProtoKey key = static_cast<JSProtoKey>(k);
    if (global.maybeGetConstructor(key) == obj) {
      return key;
    }
  }

  return JSProto_Null;
}

bool js::LookupProperty(JSContext* cx, HandleObject obj, js::HandleId id,
                        MutableHandleObject objp, PropertyResult* propp) {
  if (LookupPropertyOp op = obj->getOpsLookupProperty()) {
    return op(cx, obj, id, objp, propp);
  }
  return NativeLookupPropertyInline<CanGC>(cx, obj.as<NativeObject>(), id, objp,
                                           propp);
}

bool js::LookupName(JSContext* cx, Handle<PropertyName*> name,
                    HandleObject envChain, MutableHandleObject objp,
                    MutableHandleObject pobjp, PropertyResult* propp) {
  RootedId id(cx, NameToId(name));

  for (RootedObject env(cx, envChain); env; env = env->enclosingEnvironment()) {
    if (!LookupProperty(cx, env, id, pobjp, propp)) {
      return false;
    }
    if (propp->isFound()) {
      objp.set(env);
      return true;
    }
  }

  objp.set(nullptr);
  pobjp.set(nullptr);
  propp->setNotFound();
  return true;
}

bool js::LookupNameNoGC(JSContext* cx, PropertyName* name, JSObject* envChain,
                        JSObject** objp, NativeObject** pobjp,
                        PropertyResult* propp) {
  AutoAssertNoPendingException nogc(cx);

  MOZ_ASSERT(!*objp && !*pobjp && propp->isNotFound());

  for (JSObject* env = envChain; env; env = env->enclosingEnvironment()) {
    if (env->getOpsLookupProperty()) {
      return false;
    }
    if (!NativeLookupPropertyInline<NoGC>(cx, &env->as<NativeObject>(),
                                          NameToId(name), pobjp, propp)) {
      return false;
    }
    if (propp->isFound()) {
      *objp = env;
      return true;
    }
  }

  return true;
}

bool js::LookupNameWithGlobalDefault(JSContext* cx, Handle<PropertyName*> name,
                                     HandleObject envChain,
                                     MutableHandleObject objp) {
  RootedId id(cx, NameToId(name));

  RootedObject pobj(cx);
  PropertyResult prop;

  RootedObject env(cx, envChain);
  for (; !env->is<GlobalObject>(); env = env->enclosingEnvironment()) {
    if (!LookupProperty(cx, env, id, &pobj, &prop)) {
      return false;
    }
    if (prop.isFound()) {
      break;
    }
  }

  objp.set(env);
  return true;
}

bool js::LookupNameUnqualified(JSContext* cx, Handle<PropertyName*> name,
                               HandleObject envChain,
                               MutableHandleObject objp) {
  RootedId id(cx, NameToId(name));

  RootedObject pobj(cx);
  PropertyResult prop;

  RootedObject env(cx, envChain);
  for (; !env->isUnqualifiedVarObj(); env = env->enclosingEnvironment()) {
    if (!LookupProperty(cx, env, id, &pobj, &prop)) {
      return false;
    }
    if (prop.isFound()) {
      break;
    }
  }

  // See note above RuntimeLexicalErrorObject.
  if (pobj == env) {
    bool isTDZ = false;
    if (prop.isFound() && name != cx->names().dot_this_) {
      // Treat Debugger environments specially for TDZ checks, as they
      // look like non-native environments but in fact wrap native
      // environments.
      if (env->is<DebugEnvironmentProxy>()) {
        RootedValue v(cx);
        Rooted<DebugEnvironmentProxy*> envProxy(
            cx, &env->as<DebugEnvironmentProxy>());
        if (!DebugEnvironmentProxy::getMaybeSentinelValue(cx, envProxy, id,
                                                          &v)) {
          return false;
        }
        isTDZ = IsUninitializedLexical(v);
      } else {
        isTDZ = IsUninitializedLexicalSlot(env, prop);
      }
    }

    if (isTDZ) {
      env = RuntimeLexicalErrorObject::create(cx, env,
                                              JSMSG_UNINITIALIZED_LEXICAL);
      if (!env) {
        return false;
      }
    } else if (env->is<LexicalEnvironmentObject>() &&
               !prop.propertyInfo().writable()) {
      // Assigning to a named lambda callee name is a no-op in sloppy mode.
      if (!(env->is<BlockLexicalEnvironmentObject>() &&
            env->as<BlockLexicalEnvironmentObject>().scope().kind() ==
                ScopeKind::NamedLambda)) {
        MOZ_ASSERT(name != cx->names().dot_this_);
        env =
            RuntimeLexicalErrorObject::create(cx, env, JSMSG_BAD_CONST_ASSIGN);
        if (!env) {
          return false;
        }
      }
    }
  }

  objp.set(env);
  return true;
}

bool js::HasOwnProperty(JSContext* cx, HandleObject obj, HandleId id,
                        bool* result) {
  if (obj->is<ProxyObject>()) {
    return Proxy::hasOwn(cx, obj, id, result);
  }

  if (GetOwnPropertyOp op = obj->getOpsGetOwnPropertyDescriptor()) {
    Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
    if (!op(cx, obj, id, &desc)) {
      return false;
    }
    *result = desc.isSome();
    return true;
  }

  PropertyResult prop;
  if (!NativeLookupOwnProperty<CanGC>(cx, obj.as<NativeObject>(), id, &prop)) {
    return false;
  }
  *result = prop.isFound();
  return true;
}

bool js::LookupPropertyPure(JSContext* cx, JSObject* obj, jsid id,
                            NativeObject** objp, PropertyResult* propp) {
  if (obj->getOpsLookupProperty()) {
    return false;
  }
  return NativeLookupPropertyInline<NoGC, LookupResolveMode::CheckMayResolve>(
      cx, &obj->as<NativeObject>(), id, objp, propp);
}

bool js::LookupOwnPropertyPure(JSContext* cx, JSObject* obj, jsid id,
                               PropertyResult* propp) {
  if (obj->getOpsLookupProperty()) {
    return false;
  }
  return NativeLookupOwnPropertyInline<NoGC,
                                       LookupResolveMode::CheckMayResolve>(
      cx, &obj->as<NativeObject>(), id, propp);
}

static inline bool NativeGetPureInline(NativeObject* pobj, jsid id,
                                       PropertyResult prop, Value* vp,
                                       JSContext* cx) {
  if (prop.isDenseElement()) {
    *vp = pobj->getDenseElement(prop.denseElementIndex());
    return true;
  }
  if (prop.isTypedArrayElement()) {
    size_t idx = prop.typedArrayElementIndex();
    return pobj->as<TypedArrayObject>().getElement<NoGC>(cx, idx, vp);
  }

  // Fail if we have a custom getter.
  PropertyInfo propInfo = prop.propertyInfo();
  if (!propInfo.isDataProperty()) {
    return false;
  }

  *vp = pobj->getSlot(propInfo.slot());
  MOZ_ASSERT(!vp->isMagic());
  return true;
}

bool js::GetPropertyPure(JSContext* cx, JSObject* obj, jsid id, Value* vp) {
  NativeObject* pobj;
  PropertyResult prop;
  if (!LookupPropertyPure(cx, obj, id, &pobj, &prop)) {
    return false;
  }

  if (prop.isNotFound()) {
    vp->setUndefined();
    return true;
  }

  return NativeGetPureInline(pobj, id, prop, vp, cx);
}

bool js::GetOwnPropertyPure(JSContext* cx, JSObject* obj, jsid id, Value* vp,
                            bool* found) {
  PropertyResult prop;
  if (!LookupOwnPropertyPure(cx, obj, id, &prop)) {
    return false;
  }

  if (prop.isNotFound()) {
    *found = false;
    vp->setUndefined();
    return true;
  }

  *found = true;
  return obj->is<NativeObject>() &&
         NativeGetPureInline(&obj->as<NativeObject>(), id, prop, vp, cx);
}

static inline bool NativeGetGetterPureInline(NativeObject* holder,
                                             PropertyResult prop,
                                             JSFunction** fp) {
  MOZ_ASSERT(prop.isNativeProperty());

  PropertyInfo propInfo = prop.propertyInfo();
  if (holder->hasGetter(propInfo)) {
    JSObject* getter = holder->getGetter(propInfo);
    if (getter->is<JSFunction>()) {
      *fp = &getter->as<JSFunction>();
      return true;
    }
  }

  *fp = nullptr;
  return true;
}

bool js::GetGetterPure(JSContext* cx, JSObject* obj, jsid id, JSFunction** fp) {
  /* Just like GetPropertyPure, but get getter function, without invoking
   * it. */
  NativeObject* pobj;
  PropertyResult prop;
  if (!LookupPropertyPure(cx, obj, id, &pobj, &prop)) {
    return false;
  }

  if (prop.isNotFound()) {
    *fp = nullptr;
    return true;
  }

  return prop.isNativeProperty() && NativeGetGetterPureInline(pobj, prop, fp);
}

bool js::GetOwnGetterPure(JSContext* cx, JSObject* obj, jsid id,
                          JSFunction** fp) {
  JS::AutoCheckCannotGC nogc;
  PropertyResult prop;
  if (!LookupOwnPropertyPure(cx, obj, id, &prop)) {
    return false;
  }

  if (prop.isNotFound()) {
    *fp = nullptr;
    return true;
  }

  return prop.isNativeProperty() &&
         NativeGetGetterPureInline(&obj->as<NativeObject>(), prop, fp);
}

bool js::GetOwnNativeGetterPure(JSContext* cx, JSObject* obj, jsid id,
                                JSNative* native) {
  JS::AutoCheckCannotGC nogc;
  *native = nullptr;
  PropertyResult prop;
  if (!LookupOwnPropertyPure(cx, obj, id, &prop)) {
    return false;
  }

  if (!prop.isNativeProperty()) {
    return true;
  }

  PropertyInfo propInfo = prop.propertyInfo();

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->hasGetter(propInfo)) {
    return true;
  }

  JSObject* getterObj = nobj->getGetter(propInfo);
  if (!getterObj->is<JSFunction>()) {
    return true;
  }

  JSFunction* getter = &getterObj->as<JSFunction>();
  if (!getter->isNativeFun()) {
    return true;
  }

  *native = getter->native();
  return true;
}

bool js::HasOwnDataPropertyPure(JSContext* cx, JSObject* obj, jsid id,
                                bool* result) {
  PropertyResult prop;
  if (!LookupOwnPropertyPure(cx, obj, id, &prop)) {
    return false;
  }

  *result = prop.isNativeProperty() && prop.propertyInfo().isDataProperty();
  return true;
}

bool js::GetPrototypeIfOrdinary(JSContext* cx, HandleObject obj,
                                bool* isOrdinary, MutableHandleObject protop) {
  if (obj->is<js::ProxyObject>()) {
    return js::Proxy::getPrototypeIfOrdinary(cx, obj, isOrdinary, protop);
  }

  *isOrdinary = true;
  protop.set(obj->staticPrototype());
  return true;
}

/*** ES6 standard internal methods ******************************************/

bool js::SetPrototype(JSContext* cx, HandleObject obj, HandleObject proto,
                      JS::ObjectOpResult& result) {
  // The proxy trap subsystem fully handles prototype-setting for proxies
  // with dynamic [[Prototype]]s.
  if (obj->hasDynamicPrototype()) {
    MOZ_ASSERT(obj->is<ProxyObject>());
    return Proxy::setPrototype(cx, obj, proto, result);
  }

  /*
   * ES6 9.1.2 step 3-4 if |obj.[[Prototype]]| has SameValue as |proto| return
   * true. Since the values in question are objects, we can just compare
   * pointers.
   */
  if (proto == obj->staticPrototype()) {
    return result.succeed();
  }

  /* Disallow mutation of immutable [[Prototype]]s. */
  if (obj->staticPrototypeIsImmutable()) {
    return result.fail(JSMSG_CANT_SET_PROTO);
  }

  /*
   * Disallow mutating the [[Prototype]] on WebAssembly GC objects.
   */
  if (obj->is<WasmGcObject>()) {
    return result.fail(JSMSG_CANT_SET_PROTO);
  }

  /* ES6 9.1.2 step 5 forbids changing [[Prototype]] if not [[Extensible]]. */
  bool extensible;
  if (!IsExtensible(cx, obj, &extensible)) {
    return false;
  }
  if (!extensible) {
    return result.fail(JSMSG_CANT_SET_PROTO);
  }

  /*
   * ES6 9.1.2 step 6 forbids generating cyclical prototype chains. But we
   * have to do this comparison on the observable WindowProxy, not on the
   * possibly-Window object we're setting the proto on.
   */
  RootedObject objMaybeWindowProxy(cx, ToWindowProxyIfWindow(obj));
  RootedObject obj2(cx, proto);
  while (obj2) {
    MOZ_ASSERT(!IsWindow(obj2));
    if (obj2 == objMaybeWindowProxy) {
      return result.fail(JSMSG_CANT_SET_PROTO_CYCLE);
    }

    bool isOrdinary;
    if (!GetPrototypeIfOrdinary(cx, obj2, &isOrdinary, &obj2)) {
      return false;
    }
    if (!isOrdinary) {
      break;
    }
  }

  Rooted<TaggedProto> taggedProto(cx, TaggedProto(proto));
  if (!JSObject::setProtoUnchecked(cx, obj, taggedProto)) {
    return false;
  }

  return result.succeed();
}

bool js::SetPrototype(JSContext* cx, HandleObject obj, HandleObject proto) {
  ObjectOpResult result;
  return SetPrototype(cx, obj, proto, result) && result.checkStrict(cx, obj);
}

bool js::PreventExtensions(JSContext* cx, HandleObject obj,
                           ObjectOpResult& result) {
  if (obj->is<ProxyObject>()) {
    return js::Proxy::preventExtensions(cx, obj, result);
  }

  if (obj->is<WasmGcObject>()) {
    return result.failCantPreventExtensions();
  }

  if (!obj->nonProxyIsExtensible()) {
    // If the following assertion fails, there's somewhere else a missing
    // call to shrinkCapacityToInitializedLength() which needs to be found
    // and fixed.
    MOZ_ASSERT_IF(obj->is<NativeObject>(),
                  obj->as<NativeObject>().getDenseInitializedLength() ==
                      obj->as<NativeObject>().getDenseCapacity());

    return result.succeed();
  }

  if (obj->is<NativeObject>()) {
    // Force lazy properties to be resolved.
    Handle<NativeObject*> nobj = obj.as<NativeObject>();
    if (!ResolveLazyProperties(cx, nobj)) {
      return false;
    }

    // Prepare the elements. We have to do this before we mark the object
    // non-extensible; that's fine because these changes are not observable.
    ObjectElements::PrepareForPreventExtensions(cx, nobj);
  }

  // Finally, set the NotExtensible flag on the Shape and ObjectElements.
  if (!JSObject::setFlag(cx, obj, ObjectFlag::NotExtensible)) {
    return false;
  }
  if (obj->is<NativeObject>()) {
    ObjectElements::PreventExtensions(&obj->as<NativeObject>());
  }

  return result.succeed();
}

bool js::PreventExtensions(JSContext* cx, HandleObject obj) {
  ObjectOpResult result;
  return PreventExtensions(cx, obj, result) && result.checkStrict(cx, obj);
}

bool js::GetOwnPropertyDescriptor(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<Maybe<PropertyDescriptor>> desc) {
  if (GetOwnPropertyOp op = obj->getOpsGetOwnPropertyDescriptor()) {
    bool ok = op(cx, obj, id, desc);
    if (ok && desc.isSome()) {
      desc->assertComplete();
    }
    return ok;
  }

  return NativeGetOwnPropertyDescriptor(cx, obj.as<NativeObject>(), id, desc);
}

bool js::DefineProperty(JSContext* cx, HandleObject obj, HandleId id,
                        Handle<PropertyDescriptor> desc) {
  ObjectOpResult result;
  return DefineProperty(cx, obj, id, desc, result) &&
         result.checkStrict(cx, obj, id);
}

bool js::DefineProperty(JSContext* cx, HandleObject obj, HandleId id,
                        Handle<PropertyDescriptor> desc,
                        ObjectOpResult& result) {
  desc.assertValid();
  if (DefinePropertyOp op = obj->getOpsDefineProperty()) {
    return op(cx, obj, id, desc, result);
  }
  return NativeDefineProperty(cx, obj.as<NativeObject>(), id, desc, result);
}

bool js::DefineAccessorProperty(JSContext* cx, HandleObject obj, HandleId id,
                                HandleObject getter, HandleObject setter,
                                unsigned attrs, ObjectOpResult& result) {
  Rooted<PropertyDescriptor> desc(
      cx, PropertyDescriptor::Accessor(
              getter ? mozilla::Some(getter) : mozilla::Nothing(),
              setter ? mozilla::Some(setter) : mozilla::Nothing(), attrs));

  if (DefinePropertyOp op = obj->getOpsDefineProperty()) {
    return op(cx, obj, id, desc, result);
  }
  return NativeDefineProperty(cx, obj.as<NativeObject>(), id, desc, result);
}

bool js::DefineDataProperty(JSContext* cx, HandleObject obj, HandleId id,
                            HandleValue value, unsigned attrs,
                            ObjectOpResult& result) {
  Rooted<PropertyDescriptor> desc(cx, PropertyDescriptor::Data(value, attrs));
  if (DefinePropertyOp op = obj->getOpsDefineProperty()) {
    return op(cx, obj, id, desc, result);
  }
  return NativeDefineProperty(cx, obj.as<NativeObject>(), id, desc, result);
}

bool js::DefineAccessorProperty(JSContext* cx, HandleObject obj, HandleId id,
                                HandleObject getter, HandleObject setter,
                                unsigned attrs) {
  ObjectOpResult result;
  if (!DefineAccessorProperty(cx, obj, id, getter, setter, attrs, result)) {
    return false;
  }
  if (!result) {
    result.reportError(cx, obj, id);
    return false;
  }
  return true;
}

bool js::DefineDataProperty(JSContext* cx, HandleObject obj, HandleId id,
                            HandleValue value, unsigned attrs) {
  ObjectOpResult result;
  if (!DefineDataProperty(cx, obj, id, value, attrs, result)) {
    return false;
  }
  if (!result) {
    result.reportError(cx, obj, id);
    return false;
  }
  return true;
}

bool js::DefineDataProperty(JSContext* cx, HandleObject obj, PropertyName* name,
                            HandleValue value, unsigned attrs) {
  RootedId id(cx, NameToId(name));
  return DefineDataProperty(cx, obj, id, value, attrs);
}

bool js::DefineDataElement(JSContext* cx, HandleObject obj, uint32_t index,
                           HandleValue value, unsigned attrs) {
  RootedId id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return DefineDataProperty(cx, obj, id, value, attrs);
}

/*** SpiderMonkey nonstandard internal methods ******************************/

// Mark an object as having an immutable prototype
//
// NOTE: This does not correspond to the SetImmutablePrototype ECMAScript
//       method.
bool js::SetImmutablePrototype(JSContext* cx, HandleObject obj,
                               bool* succeeded) {
  if (obj->hasDynamicPrototype()) {
    return Proxy::setImmutablePrototype(cx, obj, succeeded);
  }

  if (!JSObject::setFlag(cx, obj, ObjectFlag::ImmutablePrototype)) {
    return false;
  }
  *succeeded = true;
  return true;
}

bool js::GetPropertyDescriptor(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<mozilla::Maybe<PropertyDescriptor>> desc,
    MutableHandleObject holder) {
  RootedObject pobj(cx);
  for (pobj = obj; pobj;) {
    if (!GetOwnPropertyDescriptor(cx, pobj, id, desc)) {
      return false;
    }

    if (desc.isSome()) {
      holder.set(pobj);
      return true;
    }

    if (!GetPrototype(cx, pobj, &pobj)) {
      return false;
    }
  }

  MOZ_ASSERT(desc.isNothing());
  holder.set(nullptr);
  return true;
}

/* * */

extern bool PropertySpecNameToId(JSContext* cx, JSPropertySpec::Name name,
                                 MutableHandleId id);

// If a property or method is part of an experimental feature that can be
// disabled at run-time by a preference, we keep it in the JSFunctionSpec /
// JSPropertySpec list, but omit the definition if the preference is off.
JS_PUBLIC_API bool js::ShouldIgnorePropertyDefinition(JSContext* cx,
                                                      JSProtoKey key, jsid id) {
  if (!cx->realm()->creationOptions().getToSourceEnabled() &&
      (id == NameToId(cx->names().toSource) ||
       id == NameToId(cx->names().uneval))) {
    return true;
  }

  if (key == JSProto_FinalizationRegistry &&
      JS::GetWeakRefsEnabled() ==
          JS::WeakRefSpecifier::EnabledWithoutCleanupSome &&
      id == NameToId(cx->names().cleanupSome)) {
    return true;
  }

  // It's gently surprising that this is JSProto_Function, but the trick
  // to realize is that this is a -constructor function-, not a function
  // on the prototype; and the proto of the constructor is JSProto_Function.
  if (key == JSProto_Function && !JS::Prefs::array_grouping() &&
      (id == NameToId(cx->names().groupBy))) {
    return true;
  }

  if (key == JSProto_Set && !JS::Prefs::experimental_new_set_methods() &&
      (id == NameToId(cx->names().union_) ||
       id == NameToId(cx->names().difference) ||
       id == NameToId(cx->names().intersection) ||
       id == NameToId(cx->names().isSubsetOf) ||
       id == NameToId(cx->names().isSupersetOf) ||
       id == NameToId(cx->names().isDisjointFrom) ||
       id == NameToId(cx->names().symmetricDifference))) {
    return true;
  }

  if (key == JSProto_ArrayBuffer &&
      !JS::Prefs::experimental_arraybuffer_resizable() &&
      (id == NameToId(cx->names().maxByteLength) ||
       id == NameToId(cx->names().resizable) ||
       id == NameToId(cx->names().resize))) {
    return true;
  }

  if (key == JSProto_SharedArrayBuffer &&
      !JS::Prefs::experimental_sharedarraybuffer_growable() &&
      (id == NameToId(cx->names().maxByteLength) ||
       id == NameToId(cx->names().growable) ||
       id == NameToId(cx->names().grow))) {
    return true;
  }

  if (key == JSProto_ArrayBuffer && !JS::Prefs::arraybuffer_transfer() &&
      (id == NameToId(cx->names().transfer) ||
       id == NameToId(cx->names().transferToFixedLength) ||
       id == NameToId(cx->names().detached))) {
    return true;
  }

#ifdef NIGHTLY_BUILD
  if (key == JSProto_Uint8Array &&
      !JS::Prefs::experimental_uint8array_base64() &&
      (id == NameToId(cx->names().setFromBase64) ||
       id == NameToId(cx->names().setFromHex) ||
       id == NameToId(cx->names().toBase64) ||
       id == NameToId(cx->names().toHex))) {
    return true;
  }

  // It's gently surprising that this is JSProto_Function, but the trick
  // to realize is that this is a -constructor function-, not a function
  // on the prototype; and the proto of the constructor is JSProto_Function.
  if (key == JSProto_Function && !JS::Prefs::experimental_uint8array_base64() &&
      (id == NameToId(cx->names().fromBase64) ||
       id == NameToId(cx->names().fromHex))) {
    return true;
  }
#endif

#ifdef ENABLE_JSON_PARSE_WITH_SOURCE
  if (key == JSProto_JSON &&
      !JS::Prefs::experimental_json_parse_with_source() &&
      (id == NameToId(cx->names().isRawJSON) ||
       id == NameToId(cx->names().rawJSON))) {
    return true;
  }
#endif

#ifdef NIGHTLY_BUILD
  if (key == JSProto_Math && !JS::Prefs::experimental_float16array() &&
      (id == NameToId(cx->names().f16round))) {
    return true;
  }
  if (key == JSProto_DataView && !JS::Prefs::experimental_float16array() &&
      (id == NameToId(cx->names().getFloat16) ||
       id == NameToId(cx->names().setFloat16))) {
    return true;
  }
#endif

  return false;
}

static bool DefineFunctionFromSpec(JSContext* cx, HandleObject obj,
                                   const JSFunctionSpec* fs) {
  RootedId id(cx);
  if (!PropertySpecNameToId(cx, fs->name, &id)) {
    return false;
  }

  if (ShouldIgnorePropertyDefinition(cx, StandardProtoKeyOrNull(obj), id)) {
    return true;
  }

  JSFunction* fun = NewFunctionFromSpec(cx, fs, id);
  if (!fun) {
    return false;
  }

  RootedValue funVal(cx, ObjectValue(*fun));
  return DefineDataProperty(cx, obj, id, funVal, fs->flags & ~JSFUN_FLAGS_MASK);
}

bool js::DefineFunctions(JSContext* cx, HandleObject obj,
                         const JSFunctionSpec* fs) {
  for (; fs->name; fs++) {
    if (!DefineFunctionFromSpec(cx, obj, fs)) {
      return false;
    }
  }
  return true;
}

/*** ToPrimitive ************************************************************/

/*
 * Gets |obj[id]|.  If that value's not callable, returns true and stores an
 * object value in *vp.  If it's callable, calls it with no arguments and |obj|
 * as |this|, returning the result in *vp.
 *
 * This is a mini-abstraction for ES6 draft rev 36 (2015 Mar 17),
 * 7.1.1, second algorithm (OrdinaryToPrimitive), steps 5.a-c.
 */
static bool MaybeCallMethod(JSContext* cx, HandleObject obj, HandleId id,
                            MutableHandleValue vp) {
  if (!GetProperty(cx, obj, obj, id, vp)) {
    return false;
  }
  if (!IsCallable(vp)) {
    vp.setObject(*obj);
    return true;
  }

  return js::Call(cx, vp, obj, vp);
}

static bool ReportCantConvert(JSContext* cx, unsigned errorNumber,
                              HandleObject obj, JSType hint) {
  const JSClass* clasp = obj->getClass();

  // Avoid recursive death when decompiling in ReportValueError.
  RootedString str(cx);
  if (hint == JSTYPE_STRING) {
    str = JS_AtomizeString(cx, clasp->name);
    if (!str) {
      return false;
    }
  } else {
    str = nullptr;
  }

  RootedValue val(cx, ObjectValue(*obj));
  ReportValueError(cx, errorNumber, JSDVG_SEARCH_STACK, val, str,
                   hint == JSTYPE_UNDEFINED ? "primitive type"
                   : hint == JSTYPE_STRING  ? "string"
                                            : "number");
  return false;
}

bool JS::OrdinaryToPrimitive(JSContext* cx, HandleObject obj, JSType hint,
                             MutableHandleValue vp) {
  MOZ_ASSERT(hint == JSTYPE_NUMBER || hint == JSTYPE_STRING ||
             hint == JSTYPE_UNDEFINED);

  Rooted<jsid> id(cx);

  const JSClass* clasp = obj->getClass();
  if (hint == JSTYPE_STRING) {
    id = NameToId(cx->names().toString);

    bool calledToString = false;
    if (clasp == &StringObject::class_) {
      // Optimize (new String(...)).toString().
      StringObject* nobj = &obj->as<StringObject>();
      if (HasNativeMethodPure(nobj, cx->names().toString, str_toString, cx)) {
        vp.setString(nobj->unbox());
        return true;
      }
    } else if (clasp == &PlainObject::class_) {
      JSFunction* fun;
      if (GetPropertyPure(cx, obj, id, vp.address()) &&
          IsFunctionObject(vp, &fun)) {
        // Common case: we have a toString function. Try to short-circuit if
        // it's Object.prototype.toString and there's no @@toStringTag.
        if (fun->maybeNative() == obj_toString &&
            !MaybeHasInterestingSymbolProperty(
                cx, obj, cx->wellKnownSymbols().toStringTag)) {
          vp.setString(cx->names().object_Object_);
          return true;
        }
        if (!js::Call(cx, vp, obj, vp)) {
          return false;
        }
        calledToString = true;
      }
    }

    if (!calledToString) {
      if (!MaybeCallMethod(cx, obj, id, vp)) {
        return false;
      }
    }
    if (vp.isPrimitive()) {
      return true;
    }

    id = NameToId(cx->names().valueOf);
    if (!MaybeCallMethod(cx, obj, id, vp)) {
      return false;
    }
    if (vp.isPrimitive()) {
      return true;
    }
  } else {
    id = NameToId(cx->names().valueOf);

    if (clasp == &StringObject::class_) {
      // Optimize new String(...).valueOf().
      StringObject* nobj = &obj->as<StringObject>();
      if (HasNativeMethodPure(nobj, cx->names().valueOf, str_toString, cx)) {
        vp.setString(nobj->unbox());
        return true;
      }
    } else if (clasp == &NumberObject::class_) {
      // Optimize new Number(...).valueOf().
      NumberObject* nobj = &obj->as<NumberObject>();
      if (HasNativeMethodPure(nobj, cx->names().valueOf, num_valueOf, cx)) {
        vp.setNumber(nobj->unbox());
        return true;
      }
    }

    if (!MaybeCallMethod(cx, obj, id, vp)) {
      return false;
    }
    if (vp.isPrimitive()) {
      return true;
    }

    id = NameToId(cx->names().toString);
    if (!MaybeCallMethod(cx, obj, id, vp)) {
      return false;
    }
    if (vp.isPrimitive()) {
      return true;
    }
  }

  return ReportCantConvert(cx, JSMSG_CANT_CONVERT_TO, obj, hint);
}

bool js::ToPrimitiveSlow(JSContext* cx, JSType preferredType,
                         MutableHandleValue vp) {
  // Step numbers refer to the first algorithm listed in ES6 draft rev 36
  // (2015 Mar 17) 7.1.1 ToPrimitive.
  MOZ_ASSERT(preferredType == JSTYPE_UNDEFINED ||
             preferredType == JSTYPE_STRING || preferredType == JSTYPE_NUMBER);
  RootedObject obj(cx, &vp.toObject());

  // Steps 4-5.
  RootedValue method(cx);
  if (!GetInterestingSymbolProperty(cx, obj, cx->wellKnownSymbols().toPrimitive,
                                    &method)) {
    return false;
  }

  // Step 6.
  if (!method.isNullOrUndefined()) {
    // Step 6 of GetMethod. js::Call() below would do this check and throw a
    // TypeError anyway, but this produces a better error message.
    if (!IsCallable(method)) {
      return ReportCantConvert(cx, JSMSG_TOPRIMITIVE_NOT_CALLABLE, obj,
                               preferredType);
    }

    // Steps 1-3, 6.a-b.
    RootedValue arg0(
        cx,
        StringValue(preferredType == JSTYPE_STRING   ? cx->names().string
                    : preferredType == JSTYPE_NUMBER ? cx->names().number
                                                     : cx->names().default_));

    if (!js::Call(cx, method, vp, arg0, vp)) {
      return false;
    }

    // Steps 6.c-d.
    if (vp.isObject()) {
      return ReportCantConvert(cx, JSMSG_TOPRIMITIVE_RETURNED_OBJECT, obj,
                               preferredType);
    }
    return true;
  }

  return OrdinaryToPrimitive(cx, obj, preferredType, vp);
}

/* ES6 draft rev 28 (2014 Oct 14) 7.1.14 */
bool js::ToPropertyKeySlow(JSContext* cx, HandleValue argument,
                           MutableHandleId result) {
  MOZ_ASSERT(argument.isObject());

  // Steps 1-2.
  RootedValue key(cx, argument);
  if (!ToPrimitiveSlow(cx, JSTYPE_STRING, &key)) {
    return false;
  }

  // Steps 3-4.
  return PrimitiveValueToId<CanGC>(cx, key, result);
}

/* * */

bool js::IsPrototypeOf(JSContext* cx, HandleObject protoObj, JSObject* obj,
                       bool* result) {
  RootedObject obj2(cx, obj);
  for (;;) {
    // The [[Prototype]] chain might be cyclic.
    if (!CheckForInterrupt(cx)) {
      return false;
    }
    if (!GetPrototype(cx, obj2, &obj2)) {
      return false;
    }
    if (!obj2) {
      *result = false;
      return true;
    }
    if (obj2 == protoObj) {
      *result = true;
      return true;
    }
  }
}

JSObject* js::PrimitiveToObject(JSContext* cx, const Value& v) {
  MOZ_ASSERT(v.isPrimitive());

  switch (v.type()) {
    case ValueType::String: {
      Rooted<JSString*> str(cx, v.toString());
      return StringObject::create(cx, str);
    }
    case ValueType::Double:
    case ValueType::Int32:
      return NumberObject::create(cx, v.toNumber());
    case ValueType::Boolean:
      return BooleanObject::create(cx, v.toBoolean());
    case ValueType::Symbol: {
      RootedSymbol symbol(cx, v.toSymbol());
      return SymbolObject::create(cx, symbol);
    }
    case ValueType::BigInt: {
      RootedBigInt bigInt(cx, v.toBigInt());
      return BigIntObject::create(cx, bigInt);
    }
#ifdef ENABLE_RECORD_TUPLE
    case ValueType::ExtendedPrimitive: {
      JSObject& obj = v.toExtendedPrimitive();

      if (obj.is<RecordType>()) {
        Rooted<RecordType*> rec(cx, &obj.as<RecordType>());
        return RecordObject::create(cx, rec);
      }
      if (obj.is<TupleType>()) {
        Rooted<TupleType*> tuple(cx, &obj.as<TupleType>());
        return TupleObject::create(cx, tuple);
      }

      MOZ_CRASH("Unexpected ExtendedPrimitive type.");
    }
#endif
    case ValueType::Undefined:
    case ValueType::Null:
    case ValueType::Magic:
    case ValueType::PrivateGCThing:
    case ValueType::Object:
      break;
  }

  MOZ_CRASH("unexpected type");
}

// Like PrimitiveToObject, but returns the JSProtoKey of the prototype that
// would be used without actually creating the object.
JSProtoKey js::PrimitiveToProtoKey(JSContext* cx, const Value& v) {
  MOZ_ASSERT(v.isPrimitive());

  switch (v.type()) {
    case ValueType::String:
      return JSProto_String;
    case ValueType::Double:
    case ValueType::Int32:
      return JSProto_Number;
    case ValueType::Boolean:
      return JSProto_Boolean;
    case ValueType::Symbol:
      return JSProto_Symbol;
    case ValueType::BigInt:
      return JSProto_BigInt;
#ifdef ENABLE_RECORD_TUPLE
    case ValueType::ExtendedPrimitive:
      if (v.toExtendedPrimitive().is<TupleType>()) {
        return JSProto_Tuple;
      }
      if (v.toExtendedPrimitive().is<RecordType>()) {
        return JSProto_Null;
      }
      MOZ_CRASH("Unsupported ExtendedPrimitive");
#endif
    case ValueType::Undefined:
    case ValueType::Null:
    case ValueType::Magic:
    case ValueType::PrivateGCThing:
    case ValueType::Object:
      break;
  }

  MOZ_CRASH("unexpected type");
}

/*
 * Invokes the ES5 ToObject algorithm on vp, returning the result. If vp might
 * already be an object, use ToObject. reportScanStack controls how null and
 * undefined errors are reported.
 *
 * Callers must handle the already-object case.
 */
JSObject* js::ToObjectSlow(JSContext* cx, JS::HandleValue val,
                           bool reportScanStack) {
  MOZ_ASSERT(!val.isMagic());
  MOZ_ASSERT(!val.isObject());

  if (val.isNullOrUndefined()) {
    ReportIsNullOrUndefinedForPropertyAccess(
        cx, val, reportScanStack ? JSDVG_SEARCH_STACK : JSDVG_IGNORE_STACK);
    return nullptr;
  }

  return PrimitiveToObject(cx, val);
}

JSObject* js::ToObjectSlowForPropertyAccess(JSContext* cx, JS::HandleValue val,
                                            int valIndex, HandleId key) {
  MOZ_ASSERT(!val.isMagic());
  MOZ_ASSERT(!val.isObject());

  if (val.isNullOrUndefined()) {
    ReportIsNullOrUndefinedForPropertyAccess(cx, val, valIndex, key);
    return nullptr;
  }

  return PrimitiveToObject(cx, val);
}

JSObject* js::ToObjectSlowForPropertyAccess(JSContext* cx, JS::HandleValue val,
                                            int valIndex,
                                            Handle<PropertyName*> key) {
  MOZ_ASSERT(!val.isMagic());
  MOZ_ASSERT(!val.isObject());

  if (val.isNullOrUndefined()) {
    RootedId keyId(cx, NameToId(key));
    ReportIsNullOrUndefinedForPropertyAccess(cx, val, valIndex, keyId);
    return nullptr;
  }

  return PrimitiveToObject(cx, val);
}

JSObject* js::ToObjectSlowForPropertyAccess(JSContext* cx, JS::HandleValue val,
                                            int valIndex,
                                            HandleValue keyValue) {
  MOZ_ASSERT(!val.isMagic());
  MOZ_ASSERT(!val.isObject());

  if (val.isNullOrUndefined()) {
    RootedId key(cx);
    if (keyValue.isPrimitive()) {
      if (!PrimitiveValueToId<CanGC>(cx, keyValue, &key)) {
        return nullptr;
      }
      ReportIsNullOrUndefinedForPropertyAccess(cx, val, valIndex, key);
    } else {
      ReportIsNullOrUndefinedForPropertyAccess(cx, val, valIndex);
    }
    return nullptr;
  }

  return PrimitiveToObject(cx, val);
}

JSObject* js::GetThisObject(JSObject* obj) {
  // Use the WindowProxy if the global is a Window, as Window must never be
  // exposed to script.
  if (obj->is<GlobalObject>()) {
    return ToWindowProxyIfWindow(obj);
  }

  // We should not expose any environments except NSVOs to script. The NSVO is
  // pretending to be the global object in this case.
  MOZ_ASSERT(obj->is<NonSyntacticVariablesObject>() ||
             !obj->is<EnvironmentObject>());

  return obj;
}

JSObject* js::GetThisObjectOfLexical(JSObject* env) {
  return env->as<ExtensibleLexicalEnvironmentObject>().thisObject();
}

JSObject* js::GetThisObjectOfWith(JSObject* env) {
  MOZ_ASSERT(env->is<WithEnvironmentObject>());
  return GetThisObject(env->as<WithEnvironmentObject>().withThis());
}

class GetObjectSlotNameFunctor : public JS::TracingContext::Functor {
  JSObject* obj;

 public:
  explicit GetObjectSlotNameFunctor(JSObject* ctx) : obj(ctx) {}
  virtual void operator()(JS::TracingContext* trc, char* buf,
                          size_t bufsize) override;
};

void GetObjectSlotNameFunctor::operator()(JS::TracingContext* tcx, char* buf,
                                          size_t bufsize) {
  MOZ_ASSERT(tcx->index() != JS::TracingContext::InvalidIndex);

  uint32_t slot = uint32_t(tcx->index());

  Maybe<PropertyKey> key;
  if (obj->is<NativeObject>()) {
    NativeShape* shape = obj->as<NativeObject>().shape();
    for (ShapePropertyIter<NoGC> iter(shape); !iter.done(); iter++) {
      if (iter->hasSlot() && iter->slot() == slot) {
        key.emplace(iter->key());
        break;
      }
    }
  }

  if (key.isNothing()) {
    do {
      const char* slotname = nullptr;
      const char* pattern = nullptr;
      if (obj->is<GlobalObject>()) {
        pattern = "CLASS_OBJECT(%s)";
        if (false) {
          ;
        }
#define TEST_SLOT_MATCHES_PROTOTYPE(name, clasp)       \
  else if ((JSProto_##name) == slot){slotname = #name; \
  }
        JS_FOR_EACH_PROTOTYPE(TEST_SLOT_MATCHES_PROTOTYPE)
#undef TEST_SLOT_MATCHES_PROTOTYPE
      } else {
        pattern = "%s";
        if (obj->is<EnvironmentObject>()) {
          if (slot == EnvironmentObject::enclosingEnvironmentSlot()) {
            slotname = "enclosing_environment";
          } else if (obj->is<CallObject>()) {
            if (slot == CallObject::calleeSlot()) {
              slotname = "callee_slot";
            }
          } else if (obj->is<WithEnvironmentObject>()) {
            if (slot == WithEnvironmentObject::objectSlot()) {
              slotname = "with_object";
            } else if (slot == WithEnvironmentObject::thisSlot()) {
              slotname = "with_this";
            }
          }
        }
      }

      if (slotname) {
        snprintf(buf, bufsize, pattern, slotname);
      } else {
        snprintf(buf, bufsize, "**UNKNOWN SLOT %" PRIu32 "**", slot);
      }
    } while (false);
  } else {
    if (key->isInt()) {
      snprintf(buf, bufsize, "%" PRId32, key->toInt());
    } else if (key->isAtom()) {
      PutEscapedString(buf, bufsize, key->toAtom(), 0);
    } else if (key->isSymbol()) {
      snprintf(buf, bufsize, "**SYMBOL KEY**");
    } else {
      snprintf(buf, bufsize, "**FINALIZED ATOM KEY**");
    }
  }
}

/*** Debugging routines *****************************************************/

#if defined(DEBUG) || defined(JS_JITSPEW)

/*
 * Routines to print out values during debugging.  These are JS_PUBLIC_API to
 * help the debugger find them and to support temporarily hacking js::Dump*
 * calls into other code.
 */

namespace js {

// We don't want jsfriendapi.h to depend on GenericPrinter,
// so these functions are declared directly in the cpp.

JS_PUBLIC_API void DumpValue(const JS::Value& val, js::GenericPrinter& out);

JS_PUBLIC_API void DumpId(jsid id, js::GenericPrinter& out);

JS_PUBLIC_API void DumpInterpreterFrame(JSContext* cx, js::GenericPrinter& out,
                                        InterpreterFrame* start = nullptr);

}  // namespace js

JS_PUBLIC_API void js::DumpValue(const Value& val, js::GenericPrinter& out) {
  val.dump(out);
}

JS_PUBLIC_API void js::DumpId(jsid id, js::GenericPrinter& out) {
  out.printf("jsid %p = ", (void*)id.asRawBits());
  id.dump(out);
}

bool JSObject::hasSameRealmAs(JSContext* cx) const {
  return nonCCWRealm() == cx->realm();
}

bool JSObject::uninlinedIsProxyObject() const { return is<ProxyObject>(); }

bool JSObject::uninlinedNonProxyIsExtensible() const {
  return nonProxyIsExtensible();
}

void JSObject::dump() const {
  js::Fprinter out(stderr);
  dump(out);
}

void JSObject::dump(js::GenericPrinter& out) const {
  js::JSONPrinter json(out);
  dump(json);
  out.put("\n");
}

void JSObject::dump(js::JSONPrinter& json) const {
  json.beginObject();
  dumpFields(json);
  json.endObject();
}

#  define FOR_EACH_CLASS(M)  \
    M(ArrayBufferViewObject) \
    M(ArrayBufferObject)     \
    M(JSFunction)            \
    M(PromiseObject)         \
    M(RegExpObject)

static void DumpOwnFields(const JSObject* obj, js::JSONPrinter& json) {
#  define CALL(CLASS)                       \
    if (obj->is<CLASS>()) {                 \
      obj->as<CLASS>().dumpOwnFields(json); \
      return;                               \
    }
  FOR_EACH_CLASS(CALL)
#  undef CALL
}

static void DumpOwnStringContent(const JSObject* obj, js::GenericPrinter& out) {
#  define CALL(CLASS)                             \
    if (obj->is<CLASS>()) {                       \
      out.put(" ");                               \
      obj->as<CLASS>().dumpOwnStringContent(out); \
      return;                                     \
    }
  FOR_EACH_CLASS(CALL)
#  undef CALL
}

#  undef FOR_EACH_CLASS

void JSObject::dumpFields(js::JSONPrinter& json) const {
  json.formatProperty("address", "(JSObject*)0x%p", this);

  if (IsCrossCompartmentWrapper(this)) {
    json.formatProperty("compartment", "(JS::Compartment*)0x%p", compartment());
  } else {
    JSObject* globalObj = &nonCCWGlobal();
    js::GenericPrinter& out = json.beginStringProperty("nonCCWGlobal");
    globalObj->dumpStringContent(out);
    json.endStringProperty();
  }

  const JSClass* clasp = getClass();
  json.formatProperty("clasp", "<%s @ (JSClass*)0x%p>", clasp->name, clasp);

  js::GenericPrinter& out = json.beginStringProperty("shape");
  shape()->dumpStringContent(out);
  json.endStringProperty();

  json.beginObjectProperty("shape.base");
  shape()->base()->dumpFields(json);
  json.endObject();

  if (IsProxy(this)) {
    const js::BaseProxyHandler* handler = GetProxyHandler(this);
    if (IsDeadProxyObject(this)) {
      json.formatProperty("handler", "(js::DeadObjectProxy*)0x%p", handler);
    } else if (IsCrossCompartmentWrapper(this)) {
      json.formatProperty("handler", "(js::CrossCompartmentWrapper*)0x%p",
                          handler);
    } else {
      json.formatProperty("handler", "(js::BaseProxyHandler*)0x%p", handler);
    }

    Value priv = GetProxyPrivate(this);
    if (!priv.isUndefined()) {
      js::GenericPrinter& out = json.beginStringProperty("private");
      priv.dumpStringContent(out);
      json.endStringProperty();
    }

    Value expando = GetProxyExpando(this);
    if (!expando.isNull()) {
      js::GenericPrinter& out = json.beginStringProperty("expando");
      expando.dumpStringContent(out);
      json.endStringProperty();
    }

    if (is<DebugEnvironmentProxy>()) {
      json.boolProperty("isQualifiedVarObj", isQualifiedVarObj());
      json.boolProperty("isUnqualifiedVarObj", isUnqualifiedVarObj());
    }
  }

  DumpOwnFields(this, json);

  if (is<NativeObject>()) {
    const auto* nobj = &as<NativeObject>();

    js::GenericPrinter& out = json.beginStringProperty("elementsHeader");
    nobj->getElementsHeader()->dumpStringContent(out);
    json.endStringProperty();

    uint32_t reserved = JSCLASS_RESERVED_SLOTS(clasp);
    if (reserved) {
      char name[256];
      json.beginObjectProperty("reservedSlots");
      for (uint32_t i = 0; i < reserved; i++) {
        SprintfLiteral(name, "%u", i);
        js::GenericPrinter& out = json.beginStringProperty(name);
        nobj->getSlot(i).dumpStringContent(out);
        json.endStringProperty();
      }
      json.endObject();
    }

    json.beginObjectProperty("properties");
    if (PropMap* map = nobj->shape()->propMap()) {
      Vector<PropMap*, 8, SystemAllocPolicy> maps;
      while (true) {
        if (!maps.append(map)) {
          json.property("error", "*oom in JSObject::dumpFields*");
          break;
        }
        if (!map->hasPrevious()) {
          break;
        }
        map = map->asLinked()->previous();
      }

      for (size_t i = maps.length(); i > 0; i--) {
        size_t index = i - 1;
        PropMap* map = maps[index];
        uint32_t len = (index == 0) ? shape()->asNative().propMapLength()
                                    : PropMap::Capacity;
        for (uint32_t j = 0; j < len; j++) {
          if (!map->hasKey(j)) {
            MOZ_ASSERT(map->isDictionary());
            continue;
          }

          JS::UniqueChars propChars = map->getPropertyNameAt(j);
          if (!propChars) {
            json.property("error", "*oom in PropMap::getPropertyNameAt*");
            continue;
          }

          js::GenericPrinter& out = json.beginStringProperty(propChars.get());

          PropertyInfoWithKey prop = map->getPropertyInfoWithKey(j);
          if (prop.isDataProperty()) {
            nobj->getSlot(prop.slot()).dumpStringContent(out);
            out.put(" ");
          } else if (prop.isAccessorProperty()) {
            out.printf("getter=0x%p, setter=0x%p", nobj->getGetter(prop),
                       nobj->getSetter(prop));
            out.put(" ");
          }

          out.put("(");
          map->dumpDescriptorStringContentAt(out, j);
          out.put(")");

          json.endStringProperty();
        }
      }
    }
    json.endObject();

    uint32_t slots = nobj->getDenseInitializedLength();
    if (slots) {
      char name[64];
      json.beginObjectProperty("elements");
      for (uint32_t i = 0; i < slots; i++) {
        SprintfLiteral(name, "%u", i);
        js::GenericPrinter& out = json.beginStringProperty(name);
        nobj->getDenseElement(i).dumpStringContent(out);
        json.endStringProperty();
      }
      json.endObject();
    }
  }
}

void JSObject::dumpStringContent(js::GenericPrinter& out) const {
  out.printf("<%s", getClass()->name);

  DumpOwnStringContent(this, out);

  out.printf(" @ (JSObject*)0x%p>", this);
}

static void MaybeDumpScope(Scope* scope, js::GenericPrinter& out) {
  if (scope) {
    out.printf("  scope: %s\n", ScopeKindString(scope->kind()));
    for (BindingIter bi(scope); bi; bi++) {
      out.put("    ");
      StringValue(bi.name()).dump(out);
    }
  }
}

static void MaybeDumpValue(const char* name, const Value& v,
                           js::GenericPrinter& out) {
  if (!v.isNull()) {
    out.printf("  %s: ", name);
    v.dump(out);
  }
}

JS_PUBLIC_API void js::DumpInterpreterFrame(JSContext* cx,
                                            js::GenericPrinter& out,
                                            InterpreterFrame* start) {
  /* This should only called during live debugging. */
  ScriptFrameIter i(cx);
  if (!start) {
    if (i.done()) {
      out.printf("no stack for cx = %p\n", (void*)cx);
      return;
    }
  } else {
    while (!i.done() && !i.isJSJit() && i.interpFrame() != start) {
      ++i;
    }

    if (i.done()) {
      out.printf("fp = %p not found in cx = %p\n", (void*)start, (void*)cx);
      return;
    }
  }

  for (; !i.done(); ++i) {
    if (i.isJSJit()) {
      out.put("JIT frame\n");
    } else {
      out.printf("InterpreterFrame at %p\n", (void*)i.interpFrame());
    }

    if (i.isFunctionFrame()) {
      out.put("callee fun: ");
      RootedValue v(cx);
      JSObject* fun = i.callee(cx);
      v.setObject(*fun);
      v.get().dump(out);
    } else {
      out.put("global or eval frame, no callee\n");
    }

    out.printf("file %s line %u\n", i.script()->filename(),
               i.script()->lineno());

    if (jsbytecode* pc = i.pc()) {
      out.printf("  pc = %p\n", pc);
      out.printf("  current op: %s\n", CodeName(JSOp(*pc)));
      MaybeDumpScope(i.script()->lookupScope(pc), out);
    }
    if (i.isFunctionFrame()) {
      MaybeDumpValue("this", i.thisArgument(cx), out);
    }
    if (!i.isJSJit()) {
      out.put("  rval: ");
      i.interpFrame()->returnValue().get().dump(out);
    }

    out.put("  flags:");
    if (i.isConstructing()) {
      out.put(" constructing");
    }
    if (!i.isJSJit() && i.interpFrame()->isDebuggerEvalFrame()) {
      out.put(" debugger eval");
    }
    if (i.isEvalFrame()) {
      out.put(" eval");
    }
    out.putChar('\n');

    out.printf("  envChain: (JSObject*) %p\n", (void*)i.environmentChain(cx));

    out.putChar('\n');
  }
}

#endif /* defined(DEBUG) || defined(JS_JITSPEW) */

JS_PUBLIC_API void js::DumpBacktrace(JSContext* cx, FILE* fp) {
  Fprinter out(fp);
  js::DumpBacktrace(cx, out);
}

JS_PUBLIC_API void js::DumpBacktrace(JSContext* cx, js::GenericPrinter& out) {
  size_t depth = 0;
  for (AllFramesIter i(cx); !i.done(); ++i, ++depth) {
    const char* filename;
    unsigned line;
    if (i.hasScript()) {
      filename = JS_GetScriptFilename(i.script());
      line = PCToLineNumber(i.script(), i.pc());
    } else {
      filename = i.filename();
      line = i.computeLine();
    }
    char frameType = i.isInterp()     ? 'i'
                     : i.isBaseline() ? 'b'
                     : i.isIon()      ? 'I'
                     : i.isWasm()     ? 'W'
                                      : '?';

    out.printf("#%zu %14p %c   %s:%u", depth, i.rawFramePtr(), frameType,
               filename, line);

    if (i.hasScript()) {
      out.printf(" (%p @ %zu)\n", i.script(), i.script()->pcToOffset(i.pc()));
    } else {
      out.printf(" (%p)\n", i.pc());
    }
  }
}

JS_PUBLIC_API void js::DumpBacktrace(JSContext* cx) {
  DumpBacktrace(cx, stdout);
}

/* * */

bool JSObject::isBackgroundFinalized() const {
  if (isTenured()) {
    return js::gc::IsBackgroundFinalized(asTenured().getAllocKind());
  }

  js::Nursery& nursery = runtimeFromMainThread()->gc.nursery();
  return js::gc::IsBackgroundFinalized(allocKindForTenure(nursery));
}

js::gc::AllocKind JSObject::allocKindForTenure(
    const js::Nursery& nursery) const {
  using namespace js::gc;

  MOZ_ASSERT(IsInsideNursery(this));

  if (is<NativeObject>()) {
    if (canHaveFixedElements()) {
      const NativeObject& nobj = as<NativeObject>();
      MOZ_ASSERT(nobj.numFixedSlots() == 0);

      /* Use minimal size object if we are just going to copy the pointer. */
      if (!nursery.isInside(nobj.getUnshiftedElementsHeader())) {
        return gc::AllocKind::OBJECT0_BACKGROUND;
      }

      size_t nelements = nobj.getDenseCapacity();
      return ForegroundToBackgroundAllocKind(GetGCArrayKind(nelements));
    }

    if (is<JSFunction>()) {
      return as<JSFunction>().getAllocKind();
    }

    if (is<FixedLengthTypedArrayObject>()) {
      return as<FixedLengthTypedArrayObject>().allocKindForTenure();
    }

    return as<NativeObject>().allocKindForTenure();
  }

  // Handle all non-native objects.

  // Proxies that are CrossCompartmentWrappers may be nursery allocated.
  if (is<ProxyObject>()) {
    return as<ProxyObject>().allocKindForTenure();
  }

  // WasmStructObjects have a variable-length tail which contains the first
  // few data fields, so make sure we copy it all over to the new object.
  if (is<WasmStructObject>()) {
    // Figure out the size of this object, from the object's TypeDef.
    const wasm::TypeDef* typeDef = &as<WasmStructObject>().typeDef();
    return WasmStructObject::allocKindForTypeDef(typeDef);
  }

  // WasmArrayObjects sometimes have a variable-length tail which contains the
  // data for small arrays. Make sure we copy it all over to the new object.
  MOZ_ASSERT(is<WasmArrayObject>());
  gc::AllocKind allocKind = as<WasmArrayObject>().allocKind();
  return allocKind;
}

void JSObject::addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                      JS::ClassInfo* info,
                                      JS::RuntimeSizes* runtimeSizes) {
  if (is<NativeObject>() && as<NativeObject>().hasDynamicSlots()) {
    info->objectsMallocHeapSlots +=
        mallocSizeOf(as<NativeObject>().getSlotsHeader());
  }

  if (is<NativeObject>() && as<NativeObject>().hasDynamicElements()) {
    void* allocatedElements = as<NativeObject>().getUnshiftedElementsHeader();
    info->objectsMallocHeapElementsNormal += mallocSizeOf(allocatedElements);
  }

  // Other things may be measured in the future if DMD indicates it is
  // worthwhile.
  if (is<JSFunction>() || is<PlainObject>() || is<ArrayObject>() ||
      is<CallObject>() || is<RegExpObject>() || is<ProxyObject>()) {
    // Do nothing.  But this function is hot, and we win by getting the
    // common cases out of the way early.  Some stats on the most common
    // classes, as measured during a vanilla browser session:
    // - (53.7%, 53.7%): Function
    // - (18.0%, 71.7%): Object
    // - (16.9%, 88.6%): Array
    // - ( 3.9%, 92.5%): Call
    // - ( 2.8%, 95.3%): RegExp
    // - ( 1.0%, 96.4%): Proxy

    // Note that any JSClass that is special cased below likely needs to
    // specify the JSCLASS_DELAY_METADATA_BUILDER flag, or else we will
    // probably crash if the object metadata callback attempts to get the
    // size of the new object (which Debugger code does) before private
    // slots are initialized.
  } else if (is<ArgumentsObject>()) {
    info->objectsMallocHeapMisc +=
        as<ArgumentsObject>().sizeOfMisc(mallocSizeOf);
  } else if (is<MapObject>()) {
    info->objectsMallocHeapMisc += as<MapObject>().sizeOfData(mallocSizeOf);
  } else if (is<SetObject>()) {
    info->objectsMallocHeapMisc += as<SetObject>().sizeOfData(mallocSizeOf);
  } else if (is<PropertyIteratorObject>()) {
    info->objectsMallocHeapMisc +=
        as<PropertyIteratorObject>().sizeOfMisc(mallocSizeOf);
  } else if (is<ArrayBufferObject>()) {
    ArrayBufferObject::addSizeOfExcludingThis(this, mallocSizeOf, info,
                                              runtimeSizes);
  } else if (is<SharedArrayBufferObject>()) {
    SharedArrayBufferObject::addSizeOfExcludingThis(this, mallocSizeOf, info,
                                                    runtimeSizes);
  } else if (is<GlobalObject>()) {
    as<GlobalObject>().addSizeOfData(mallocSizeOf, info);
  } else if (is<WeakCollectionObject>()) {
    info->objectsMallocHeapMisc +=
        as<WeakCollectionObject>().sizeOfExcludingThis(mallocSizeOf);
  }
#ifdef JS_HAS_CTYPES
  else {
    // This must be the last case.
    info->objectsMallocHeapMisc += ctypes::SizeOfDataIfCDataObject(
        mallocSizeOf, const_cast<JSObject*>(this));
  }
#endif
}

size_t JSObject::sizeOfIncludingThisInNursery() const {
  // This function doesn't concern itself yet with typed objects (bug 1133593).

  MOZ_ASSERT(!isTenured());

  const Nursery& nursery = runtimeFromMainThread()->gc.nursery();
  size_t size = gc::Arena::thingSize(allocKindForTenure(nursery));

  if (is<NativeObject>()) {
    const NativeObject& native = as<NativeObject>();

    size += native.numDynamicSlots() * sizeof(Value);

    if (native.hasDynamicElements()) {
      js::ObjectElements& elements = *native.getElementsHeader();
      size += (elements.capacity + elements.numShiftedElements()) *
              sizeof(HeapSlot);
    }

    if (is<ArgumentsObject>()) {
      size += as<ArgumentsObject>().sizeOfData();
    }
  }

  return size;
}

JS::ubi::Node::Size JS::ubi::Concrete<JSObject>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  JSObject& obj = get();

  if (!obj.isTenured()) {
    return obj.sizeOfIncludingThisInNursery();
  }

  JS::ClassInfo info;
  obj.addSizeOfExcludingThis(mallocSizeOf, &info, nullptr);
  return obj.tenuredSizeOfThis() + info.sizeOfAllThings();
}

const char16_t JS::ubi::Concrete<JSObject>::concreteTypeName[] = u"JSObject";

void JSObject::traceChildren(JSTracer* trc) {
  TraceCellHeaderEdge(trc, this, "shape");

  Shape* objShape = shape();
  if (objShape->isNative()) {
    NativeObject* nobj = &as<NativeObject>();

    {
      GetObjectSlotNameFunctor func(nobj);
      JS::AutoTracingDetails ctx(trc, func);
      JS::AutoTracingIndex index(trc);
      // Tracing can mutate the target but cannot change the slot count,
      // but the compiler has no way of knowing this.
      const uint32_t nslots = nobj->slotSpan();
      for (uint32_t i = 0; i < nslots; ++i) {
        TraceEdge(trc, &nobj->getSlotRef(i), "object slot");
        ++index;
      }
      MOZ_ASSERT(nslots == nobj->slotSpan());
    }

    TraceRange(trc, nobj->getDenseInitializedLength(),
               static_cast<HeapSlot*>(nobj->getDenseElements()),
               "objectElements");
  }

  // Call the trace hook at the end so that during a moving GC the trace hook
  // will see updated fields and slots.
  const JSClass* clasp = objShape->getObjectClass();
  if (clasp->hasTrace()) {
    clasp->doTrace(trc, this);
  }
}

// ES 2016 7.3.20.
[[nodiscard]] JSObject* js::SpeciesConstructor(
    JSContext* cx, HandleObject obj, HandleObject defaultCtor,
    bool (*isDefaultSpecies)(JSContext*, JSFunction*)) {
  // Step 1 (implicit).

  // Fast-path for steps 2 - 8. Applies if all of the following conditions
  // are met:
  // - obj.constructor can be retrieved without side-effects.
  // - obj.constructor[[@@species]] can be retrieved without side-effects.
  // - obj.constructor[[@@species]] is the builtin's original @@species
  //   getter.
  RootedValue ctor(cx);
  bool ctorGetSucceeded = GetPropertyPure(
      cx, obj, NameToId(cx->names().constructor), ctor.address());
  if (ctorGetSucceeded && ctor.isObject() && &ctor.toObject() == defaultCtor) {
    jsid speciesId = PropertyKey::Symbol(cx->wellKnownSymbols().species);
    JSFunction* getter;
    if (GetGetterPure(cx, defaultCtor, speciesId, &getter) && getter &&
        isDefaultSpecies(cx, getter)) {
      return defaultCtor;
    }
  }

  // Step 2.
  if (!ctorGetSucceeded &&
      !GetProperty(cx, obj, obj, cx->names().constructor, &ctor)) {
    return nullptr;
  }

  // Step 3.
  if (ctor.isUndefined()) {
    return defaultCtor;
  }

  // Step 4.
  if (!ctor.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED,
                              "object's 'constructor' property");
    return nullptr;
  }

  // Step 5.
  RootedObject ctorObj(cx, &ctor.toObject());
  RootedValue s(cx);
  RootedId speciesId(cx, PropertyKey::Symbol(cx->wellKnownSymbols().species));
  if (!GetProperty(cx, ctorObj, ctor, speciesId, &s)) {
    return nullptr;
  }

  // Step 6.
  if (s.isNullOrUndefined()) {
    return defaultCtor;
  }

  // Step 7.
  if (IsConstructor(s)) {
    if (&s.toObject() != ctorObj) {
      ReportUsageCounter(cx, defaultCtor,
                         SUBCLASSING_DETERMINE_THROUGH_CONSTRUCTOR,
                         SUBCLASSING_TYPE_III);
    }
    return &s.toObject();
  }

  // Step 8.
  JS_ReportErrorNumberASCII(
      cx, GetErrorMessage, nullptr, JSMSG_NOT_CONSTRUCTOR,
      "[Symbol.species] property of object's constructor");
  return nullptr;
}

[[nodiscard]] JSObject* js::SpeciesConstructor(
    JSContext* cx, HandleObject obj, JSProtoKey ctorKey,
    bool (*isDefaultSpecies)(JSContext*, JSFunction*)) {
  RootedObject defaultCtor(cx,
                           GlobalObject::getOrCreateConstructor(cx, ctorKey));
  if (!defaultCtor) {
    return nullptr;
  }
  return SpeciesConstructor(cx, obj, defaultCtor, isDefaultSpecies);
}

bool js::Unbox(JSContext* cx, HandleObject obj, MutableHandleValue vp) {
  if (MOZ_UNLIKELY(obj->is<ProxyObject>())) {
    return Proxy::boxedValue_unbox(cx, obj, vp);
  }

  if (obj->is<BooleanObject>()) {
    vp.setBoolean(obj->as<BooleanObject>().unbox());
  } else if (obj->is<NumberObject>()) {
    vp.setNumber(obj->as<NumberObject>().unbox());
  } else if (obj->is<StringObject>()) {
    vp.setString(obj->as<StringObject>().unbox());
  } else if (obj->is<DateObject>()) {
    vp.set(obj->as<DateObject>().UTCTime());
  } else if (obj->is<SymbolObject>()) {
    vp.setSymbol(obj->as<SymbolObject>().unbox());
  } else if (obj->is<BigIntObject>()) {
    vp.setBigInt(obj->as<BigIntObject>().unbox());
#ifdef ENABLE_RECORD_TUPLE
  } else if (obj->is<RecordObject>()) {
    vp.setExtendedPrimitive(*obj->as<RecordObject>().unbox());
  } else if (obj->is<TupleObject>()) {
    vp.setExtendedPrimitive(obj->as<TupleObject>().unbox());
#endif
  } else {
    vp.setUndefined();
  }

  return true;
}

#ifdef DEBUG
void js::AssertJSClassInvariants(const JSClass* clasp) {
  MOZ_ASSERT(JS::StringIsASCII(clasp->name));

  // Native objects shouldn't use the property operation hooks in ObjectOps.
  // Doing so could violate JIT invariants.
  //
  // Environment objects unfortunately use these hooks, but environment objects
  // are not exposed directly to script so they're generally less of an issue.
  if (clasp->isNativeObject() && clasp != &WithEnvironmentObject::class_ &&
      clasp != &ModuleEnvironmentObject::class_ &&
      clasp != &RuntimeLexicalErrorObject::class_) {
    MOZ_ASSERT(!clasp->getOpsLookupProperty());
    MOZ_ASSERT_IF(clasp != &MappedArgumentsObject::class_,
                  !clasp->getOpsDefineProperty());
    MOZ_ASSERT(!clasp->getOpsHasProperty());
    MOZ_ASSERT(!clasp->getOpsGetProperty());
    MOZ_ASSERT(!clasp->getOpsSetProperty());
    MOZ_ASSERT(!clasp->getOpsGetOwnPropertyDescriptor());
    MOZ_ASSERT(!clasp->getOpsDeleteProperty());
  }
}

/* static */
void JSObject::debugCheckNewObject(Shape* shape, js::gc::AllocKind allocKind,
                                   js::gc::Heap heap) {
  const JSClass* clasp = shape->getObjectClass();

  if (!ClassCanHaveFixedData(clasp)) {
    NativeShape* nshape = &shape->asNative();
    if (clasp == &ArrayObject::class_) {
      // Arrays can store the ObjectElements header inline.
      MOZ_ASSERT(nshape->numFixedSlots() == 0);
    } else {
      MOZ_ASSERT(gc::GetGCKindSlots(allocKind) == nshape->numFixedSlots());
    }
  }

  // Assert background finalization is used when possible.
  MOZ_ASSERT(!CanChangeToBackgroundAllocKind(allocKind, clasp));

  // Classes with a finalizer must specify whether instances will be finalized
  // on the main thread or in the background, except proxies whose behaviour
  // depends on the target object.
  static const uint32_t FinalizeMask =
      JSCLASS_FOREGROUND_FINALIZE | JSCLASS_BACKGROUND_FINALIZE;
  uint32_t flags = clasp->flags;
  uint32_t finalizeFlags = flags & FinalizeMask;
  if (clasp->hasFinalize() && !clasp->isProxyObject()) {
    MOZ_ASSERT(finalizeFlags == JSCLASS_FOREGROUND_FINALIZE ||
               finalizeFlags == JSCLASS_BACKGROUND_FINALIZE);
    MOZ_ASSERT((finalizeFlags == JSCLASS_BACKGROUND_FINALIZE) ==
               IsBackgroundFinalized(allocKind));
  } else {
    MOZ_ASSERT(finalizeFlags == 0);
  }

  MOZ_ASSERT_IF(clasp->hasFinalize(),
                heap == gc::Heap::Tenured ||
                    CanNurseryAllocateFinalizedClass(clasp) ||
                    clasp->isProxyObject());

  MOZ_ASSERT(!shape->isDictionary());
  MOZ_ASSERT(!shape->realm()->hasObjectPendingMetadata());

  // Non-native classes manage their own data and slots, so numFixedSlots is
  // always 0. Note that proxy classes can have reserved slots but they're not
  // included in numFixedSlots.
  if (!clasp->isNativeObject()) {
    MOZ_ASSERT_IF(!clasp->isProxyObject(), JSCLASS_RESERVED_SLOTS(clasp) == 0);
  }
}
#endif
