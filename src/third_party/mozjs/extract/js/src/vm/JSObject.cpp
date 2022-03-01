/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS object implementation.
 */

#include "vm/JSObject-inl.h"

#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TemplateLib.h"

#include <algorithm>
#include <string.h>

#include "jsapi.h"
#include "jsexn.h"
#include "jsfriendapi.h"
#include "jsnum.h"
#include "jstypes.h"

#include "builtin/Array.h"
#include "builtin/BigInt.h"
#include "builtin/Eval.h"
#include "builtin/MapObject.h"
#include "builtin/Object.h"
#include "builtin/String.h"
#include "builtin/Symbol.h"
#include "builtin/WeakSetObject.h"
#include "ds/IdValuePair.h"  // js::IdValuePair
#include "frontend/BytecodeCompiler.h"
#include "gc/Policy.h"
#include "jit/BaselineJIT.h"
#include "js/CharacterEncoding.h"
#include "js/friend/DumpFunctions.h"  // js::DumpObject
#include "js/friend/ErrorMessages.h"  // JSErrNum, js::GetErrorMessage, JSMSG_*
#include "js/friend/WindowProxy.h"    // js::IsWindow, js::ToWindowProxyIfWindow
#include "js/MemoryMetrics.h"
#include "js/PropertyDescriptor.h"  // JS::FromPropertyDescriptor
#include "js/PropertySpec.h"        // JSPropertySpec
#include "js/Proxy.h"
#include "js/Result.h"
#include "js/UbiNode.h"
#include "js/UniquePtr.h"
#include "js/Wrapper.h"
#include "proxy/DeadObjectProxy.h"
#include "util/Memory.h"
#include "util/Text.h"
#include "util/Windows.h"
#include "vm/ArgumentsObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/DateObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/ProxyObject.h"
#include "vm/RegExpStaticsObject.h"
#include "vm/Shape.h"
#include "vm/TypedArrayObject.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "builtin/Boolean-inl.h"
#include "gc/Marking-inl.h"
#include "vm/ArrayObject-inl.h"
#include "vm/BooleanObject-inl.h"
#include "vm/Caches-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSFunction-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/NumberObject-inl.h"
#include "vm/ObjectFlags-inl.h"
#include "vm/PlainObject-inl.h"  // js::CopyInitializerObject
#include "vm/Realm-inl.h"
#include "vm/Shape-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/TypedArrayObject-inl.h"
#include "wasm/TypedObject-inl.h"

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
      return v.toObject().getClass()->name;
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
  RootedObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
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

static const char js_getter_str[] = "getter";
static const char js_setter_str[] = "setter";

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
        JS_TRY_OR_RETURN_FALSE(cx,
                               CheckCallable(cx, &v.toObject(), js_getter_str));
      }
      getter = &v.toObject();
    } else if (v.isUndefined()) {
      getter = nullptr;
    } else {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_GET_SET_FIELD, js_getter_str);
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
        JS_TRY_OR_RETURN_FALSE(cx,
                               CheckCallable(cx, &v.toObject(), js_setter_str));
      }
      setter = &v.toObject();
    } else if (v.isUndefined()) {
      setter = nullptr;
    } else {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_GET_SET_FIELD, js_setter_str);
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
    MOZ_TRY(CheckCallable(cx, desc.getter(), js_getter_str));
  }

  if (desc.hasSetter()) {
    MOZ_TRY(CheckCallable(cx, desc.setter(), js_setter_str));
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
    HandleNativeObject nobj = obj.as<NativeObject>();

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

static bool ResolveLazyProperties(JSContext* cx, HandleNativeObject obj) {
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
    HandleNativeObject nobj = obj.as<NativeObject>();

    // Force lazy properties to be resolved.
    if (!ResolveLazyProperties(cx, nobj)) {
      return false;
    }

    // Typed array elements are configurable, writable properties, so if any
    // elements are present, the typed array can neither be sealed nor frozen.
    if (nobj->is<TypedArrayObject>() &&
        nobj->as<TypedArrayObject>().length() > 0) {
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

static inline JSObject* NewObject(JSContext* cx, Handle<TaggedProto> proto,
                                  const JSClass* clasp, gc::AllocKind kind,
                                  NewObjectKind newKind,
                                  ObjectFlags objectFlags = {}) {
  MOZ_ASSERT(clasp != &ArrayObject::class_);
  MOZ_ASSERT_IF(clasp == &JSFunction::class_,
                kind == gc::AllocKind::FUNCTION ||
                    kind == gc::AllocKind::FUNCTION_EXTENDED);

  // For objects which can have fixed data following the object, only use
  // enough fixed slots to cover the number of reserved slots in the object,
  // regardless of the allocation kind specified.
  size_t nfixed = ClassCanHaveFixedData(clasp)
                      ? GetGCKindSlots(gc::GetGCObjectKind(clasp), clasp)
                      : GetGCKindSlots(kind, clasp);

  RootedShape shape(
      cx, SharedShape::getInitialShape(cx, clasp, cx->realm(), proto, nfixed,
                                       objectFlags));
  if (!shape) {
    return nullptr;
  }

  gc::InitialHeap heap = GetInitialHeap(newKind, clasp);

  JSObject* obj;
  if (clasp->isJSFunction()) {
    JS_TRY_VAR_OR_RETURN_NULL(cx, obj,
                              JSFunction::create(cx, kind, heap, shape));
  } else if (MOZ_LIKELY(clasp->isNativeObject())) {
    JS_TRY_VAR_OR_RETURN_NULL(cx, obj,
                              NativeObject::create(cx, kind, heap, shape));
  } else {
    MOZ_ASSERT(IsTypedObjectClass(clasp));
    JS_TRY_VAR_OR_RETURN_NULL(cx, obj,
                              TypedObject::create(cx, kind, heap, shape));
  }

  probes::CreateObject(cx, obj);
  return obj;
}

void NewObjectCache::fillProto(EntryIndex entry, const JSClass* clasp,
                               js::TaggedProto proto, gc::AllocKind kind,
                               NativeObject* obj) {
  MOZ_ASSERT_IF(proto.isObject(), !proto.toObject()->is<GlobalObject>());
  MOZ_ASSERT(obj->taggedProto() == proto);
  return fill(entry, clasp, proto.raw(), kind, obj);
}

bool js::NewObjectWithTaggedProtoIsCachable(JSContext* cx,
                                            Handle<TaggedProto> proto,
                                            NewObjectKind newKind,
                                            const JSClass* clasp) {
  return !cx->isHelperThreadContext() && proto.isObject() &&
         newKind == GenericObject && clasp->isNativeObject() &&
         !proto.toObject()->is<GlobalObject>();
}

JSObject* js::NewObjectWithGivenTaggedProto(JSContext* cx, const JSClass* clasp,
                                            Handle<TaggedProto> proto,
                                            gc::AllocKind allocKind,
                                            NewObjectKind newKind,
                                            ObjectFlags objectFlags) {
  if (CanChangeToBackgroundAllocKind(allocKind, clasp)) {
    allocKind = ForegroundToBackgroundAllocKind(allocKind);
  }

  bool isCachable =
      NewObjectWithTaggedProtoIsCachable(cx, proto, newKind, clasp);
  if (isCachable) {
    NewObjectCache& cache = cx->caches().newObjectCache;
    NewObjectCache::EntryIndex entry = -1;
    if (cache.lookupProto(clasp, proto.toObject(), allocKind, &entry)) {
      JSObject* obj =
          cache.newObjectFromHit(cx, entry, GetInitialHeap(newKind, clasp));
      if (obj) {
        return obj;
      }
    }
  }

  RootedObject obj(
      cx, NewObject(cx, proto, clasp, allocKind, newKind, objectFlags));
  if (!obj) {
    return nullptr;
  }

  if (isCachable && !obj->as<NativeObject>().hasDynamicSlots()) {
    NewObjectCache& cache = cx->caches().newObjectCache;
    NewObjectCache::EntryIndex entry = -1;
    cache.lookupProto(clasp, proto.toObject(), allocKind, &entry);
    cache.fillProto(entry, clasp, proto, allocKind, &obj->as<NativeObject>());
  }

  return obj;
}

static bool NewObjectIsCachable(JSContext* cx, NewObjectKind newKind,
                                const JSClass* clasp) {
  return !cx->isHelperThreadContext() && newKind == GenericObject &&
         clasp->isNativeObject();
}

JSObject* js::NewObjectWithClassProto(JSContext* cx, const JSClass* clasp,
                                      HandleObject protoArg,
                                      gc::AllocKind allocKind,
                                      NewObjectKind newKind) {
  if (protoArg) {
    return NewObjectWithGivenTaggedProto(cx, clasp, AsTaggedProto(protoArg),
                                         allocKind, newKind);
  }

  if (CanChangeToBackgroundAllocKind(allocKind, clasp)) {
    allocKind = ForegroundToBackgroundAllocKind(allocKind);
  }

  Handle<GlobalObject*> global = cx->global();

  bool isCachable = NewObjectIsCachable(cx, newKind, clasp);
  if (isCachable) {
    NewObjectCache& cache = cx->caches().newObjectCache;
    NewObjectCache::EntryIndex entry = -1;
    if (cache.lookupGlobal(clasp, global, allocKind, &entry)) {
      gc::InitialHeap heap = GetInitialHeap(newKind, clasp);
      JSObject* obj = cache.newObjectFromHit(cx, entry, heap);
      if (obj) {
        return obj;
      }
    }
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
  JSObject* obj = NewObject(cx, taggedProto, clasp, allocKind, newKind);
  if (!obj) {
    return nullptr;
  }

  if (isCachable && !obj->as<NativeObject>().hasDynamicSlots()) {
    NewObjectCache& cache = cx->caches().newObjectCache;
    NewObjectCache::EntryIndex entry = -1;
    cache.lookupGlobal(clasp, global, allocKind, &entry);
    cache.fillGlobal(entry, clasp, global, allocKind, &obj->as<NativeObject>());
  }

  return obj;
}

bool js::NewObjectScriptedCall(JSContext* cx, MutableHandleObject pobj) {
  gc::AllocKind allocKind = NewObjectGCKind();
  NewObjectKind newKind = GenericObject;

  JSObject* obj = NewBuiltinClassInstance<PlainObject>(cx, allocKind, newKind);
  if (!obj) {
    return false;
  }

  pobj.set(obj);
  return true;
}

JSObject* js::CreateThis(JSContext* cx, const JSClass* newclasp,
                         HandleObject callee) {
  RootedObject proto(cx);
  if (!GetPrototypeFromConstructor(
          cx, callee, JSCLASS_CACHED_PROTO_KEY(newclasp), &proto)) {
    return nullptr;
  }
  gc::AllocKind kind = NewObjectGCKind();
  return NewObjectWithClassProto(cx, newclasp, proto, kind);
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

static bool GetScriptArrayObjectElements(
    HandleArrayObject arr, MutableHandle<GCVector<Value>> values) {
  MOZ_ASSERT(!arr->isIndexed());

  size_t length = arr->length();
  if (!values.appendN(MagicValue(JS_ELEMENTS_HOLE), length)) {
    return false;
  }

  size_t initlen = arr->getDenseInitializedLength();
  for (size_t i = 0; i < initlen; i++) {
    values[i].set(arr->getDenseElement(i));
  }

  return true;
}

static bool GetScriptPlainObjectProperties(
    HandleObject obj, MutableHandle<IdValueVector> properties) {
  MOZ_ASSERT(obj->is<PlainObject>());
  PlainObject* nobj = &obj->as<PlainObject>();

  if (!properties.appendN(IdValuePair(), nobj->slotSpan())) {
    return false;
  }

  for (ShapePropertyIter<NoGC> iter(nobj->shape()); !iter.done(); iter++) {
    MOZ_ASSERT(iter->isDataDescriptor());
    uint32_t slot = iter->slot();
    properties[slot].get().id = iter->key();
    properties[slot].get().value = nobj->getSlot(slot);
  }

  for (size_t i = 0; i < nobj->getDenseInitializedLength(); i++) {
    Value v = nobj->getDenseElement(i);
    if (!v.isMagic(JS_ELEMENTS_HOLE) &&
        !properties.emplaceBack(INT_TO_JSID(i), v)) {
      return false;
    }
  }

  return true;
}

static bool DeepCloneValue(JSContext* cx, Value* vp) {
  if (vp->isObject()) {
    RootedObject obj(cx, &vp->toObject());
    obj = DeepCloneObjectLiteral(cx, obj);
    if (!obj) {
      return false;
    }
    vp->setObject(*obj);
  } else {
    cx->markAtomValue(*vp);
  }
  return true;
}

JSObject* js::DeepCloneObjectLiteral(JSContext* cx, HandleObject obj) {
  /* NB: Keep this in sync with XDRObjectLiteral. */
  MOZ_ASSERT(obj->is<PlainObject>() || obj->is<ArrayObject>());

  if (obj->is<ArrayObject>()) {
    Rooted<GCVector<Value>> values(cx, GCVector<Value>(cx));
    if (!GetScriptArrayObjectElements(obj.as<ArrayObject>(), &values)) {
      return nullptr;
    }

    // Deep clone any elements.
    for (uint32_t i = 0; i < values.length(); ++i) {
      if (!DeepCloneValue(cx, values[i].address())) {
        return nullptr;
      }
    }

    return NewDenseCopiedArray(cx, values.length(), values.begin(),
                               /* proto = */ nullptr, TenuredObject);
  }

  Rooted<IdValueVector> properties(cx, IdValueVector(cx));
  if (!GetScriptPlainObjectProperties(obj, &properties)) {
    return nullptr;
  }

  for (size_t i = 0; i < properties.length(); i++) {
    cx->markId(properties[i].get().id);
    if (!DeepCloneValue(cx, &properties[i].get().value)) {
      return nullptr;
    }
  }

  return NewPlainObjectWithProperties(cx, properties.begin(),
                                      properties.length(), TenuredObject);
}

static bool InitializePropertiesFromCompatibleNativeObject(
    JSContext* cx, HandleNativeObject dst, HandleNativeObject src) {
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
  if (!src->shape()->sharedPropMap()) {
    return true;
  }

  MOZ_ASSERT(!src->hasPrivate());
  RootedShape shape(cx);
  if (src->staticPrototype() == dst->staticPrototype()) {
    shape = src->shape();
  } else {
    // We need to generate a new shape for dst that has dst's proto but all
    // the property information from src.  Note that we asserted above that
    // dst's object flags are empty.
    Shape* srcShape = src->shape();
    ObjectFlags objFlags;
    objFlags = CopyPropMapObjectFlags(objFlags, srcShape->objectFlags());
    Rooted<SharedPropMap*> map(cx, srcShape->sharedPropMap());
    uint32_t mapLength = srcShape->propMapLength();
    shape = SharedShape::getPropMapShape(cx, dst->shape()->base(),
                                         dst->numFixedSlots(), map, mapLength,
                                         objFlags);
    if (!shape) {
      return false;
    }
  }

  size_t span = shape->slotSpan();
  if (!dst->setShapeAndUpdateSlots(cx, shape)) {
    return false;
  }
  for (size_t i = JSCLASS_RESERVED_SLOTS(src->getClass()); i < span; i++) {
    dst->setSlot(i, src->getSlot(i));
  }

  return true;
}

JS_PUBLIC_API bool JS_InitializePropertiesFromCompatibleNativeObject(
    JSContext* cx, HandleObject dst, HandleObject src) {
  return InitializePropertiesFromCompatibleNativeObject(
      cx, dst.as<NativeObject>(), src.as<NativeObject>());
}

template <XDRMode mode>
XDRResult js::XDRObjectLiteral(XDRState<mode>* xdr, MutableHandleObject obj) {
  /* NB: Keep this in sync with DeepCloneObjectLiteral. */

  JSContext* cx = xdr->cx();
  cx->check(obj);

  // Distinguish between objects and array classes.
  uint32_t isArray = 0;
  {
    if (mode == XDR_ENCODE) {
      MOZ_ASSERT(obj->is<PlainObject>() || obj->is<ArrayObject>());
      isArray = obj->is<ArrayObject>() ? 1 : 0;
    }

    MOZ_TRY(xdr->codeUint32(&isArray));
  }

  RootedValue tmpValue(cx), tmpIdValue(cx);
  RootedId tmpId(cx);

  if (isArray) {
    Rooted<GCVector<Value>> values(cx, GCVector<Value>(cx));
    if (mode == XDR_ENCODE) {
      RootedArrayObject arr(cx, &obj->as<ArrayObject>());
      if (!GetScriptArrayObjectElements(arr, &values)) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }

    uint32_t initialized;
    if (mode == XDR_ENCODE) {
      initialized = values.length();
    }
    MOZ_TRY(xdr->codeUint32(&initialized));
    if (mode == XDR_DECODE &&
        !values.appendN(MagicValue(JS_ELEMENTS_HOLE), initialized)) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }

    // Recursively copy dense elements.
    for (unsigned i = 0; i < initialized; i++) {
      MOZ_TRY(XDRScriptConst(xdr, values[i]));
    }

    if (mode == XDR_DECODE) {
      obj.set(NewDenseCopiedArray(cx, values.length(), values.begin(),
                                  /* proto = */ nullptr, TenuredObject));
      if (!obj) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }

    return Ok();
  }

  // Code the properties in the object.
  Rooted<IdValueVector> properties(cx, IdValueVector(cx));
  if (mode == XDR_ENCODE && !GetScriptPlainObjectProperties(obj, &properties)) {
    return xdr->fail(JS::TranscodeResult::Throw);
  }

  uint32_t nproperties = properties.length();
  MOZ_TRY(xdr->codeUint32(&nproperties));

  if (mode == XDR_DECODE && !properties.appendN(IdValuePair(), nproperties)) {
    return xdr->fail(JS::TranscodeResult::Throw);
  }

  for (size_t i = 0; i < nproperties; i++) {
    if (mode == XDR_ENCODE) {
      tmpIdValue = IdToValue(properties[i].get().id);
      tmpValue = properties[i].get().value;
    }

    MOZ_TRY(XDRScriptConst(xdr, &tmpIdValue));
    MOZ_TRY(XDRScriptConst(xdr, &tmpValue));

    if (mode == XDR_DECODE) {
      if (!PrimitiveValueToId<CanGC>(cx, tmpIdValue, &tmpId)) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
      properties[i].get().id = tmpId;
      properties[i].get().value = tmpValue;
    }
  }

  if (mode == XDR_DECODE) {
    obj.set(NewPlainObjectWithProperties(cx, properties.begin(),
                                         properties.length(), TenuredObject));
    if (!obj) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  return Ok();
}

template XDRResult js::XDRObjectLiteral(XDRState<XDR_ENCODE>* xdr,
                                        MutableHandleObject obj);

template XDRResult js::XDRObjectLiteral(XDRState<XDR_DECODE>* xdr,
                                        MutableHandleObject obj);

/* static */
bool NativeObject::fillInAfterSwap(JSContext* cx, HandleNativeObject obj,
                                   NativeObject* old, HandleValueVector values,
                                   void* priv) {
  // This object has just been swapped with some other object, and its shape
  // no longer reflects its allocated size. Correct this information and
  // fill the slots in with the specified values.
  MOZ_ASSERT(obj->slotSpan() == values.length());
  MOZ_ASSERT(!IsInsideNursery(obj));

  // Make sure the shape's numFixedSlots() is correct.
  size_t nfixed =
      gc::GetGCKindSlots(obj->asTenured().getAllocKind(), obj->getClass());
  if (nfixed != obj->shape()->numFixedSlots()) {
    if (!NativeObject::changeNumFixedSlotsAfterSwap(cx, obj, nfixed)) {
      return false;
    }
    MOZ_ASSERT(obj->shape()->numFixedSlots() == nfixed);
  }

  if (obj->hasPrivate()) {
    obj->setPrivate(priv);
  } else {
    MOZ_ASSERT(!priv);
  }

  uint32_t oldDictionarySlotSpan =
      obj->inDictionaryMode() ? obj->dictionaryModeSlotSpan() : 0;

  Zone* zone = obj->zone();
  if (obj->hasDynamicSlots()) {
    ObjectSlots* slotsHeader = obj->getSlotsHeader();
    size_t size = ObjectSlots::allocSize(slotsHeader->capacity());
    zone->removeCellMemory(old, size, MemoryUse::ObjectSlots);
    js_free(slotsHeader);
    obj->setEmptyDynamicSlots(0);
  }

  size_t ndynamic =
      calculateDynamicSlots(nfixed, values.length(), obj->getClass());
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

  obj->initSlots(values.begin(), values.length());

  return true;
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

[[nodiscard]] static bool CopyProxyValuesBeforeSwap(
    JSContext* cx, ProxyObject* proxy, MutableHandleValueVector values) {
  MOZ_ASSERT(values.empty());

  // Remove the GCPtrValues we're about to swap from the store buffer, to
  // ensure we don't trace bogus values.
  gc::StoreBuffer& sb = cx->runtime()->gc.storeBuffer();

  // Reserve space for the expando, private slot and the reserved slots.
  if (!values.reserve(2 + proxy->numReservedSlots())) {
    return false;
  }

  js::detail::ProxyValueArray* valArray =
      js::detail::GetProxyDataLayout(proxy)->values();
  sb.unputValue(&valArray->expandoSlot);
  sb.unputValue(&valArray->privateSlot);
  values.infallibleAppend(valArray->expandoSlot);
  values.infallibleAppend(valArray->privateSlot);

  for (size_t i = 0; i < proxy->numReservedSlots(); i++) {
    sb.unputValue(&valArray->reservedSlots.slots[i]);
    values.infallibleAppend(valArray->reservedSlots.slots[i]);
  }

  return true;
}

bool ProxyObject::initExternalValueArrayAfterSwap(
    JSContext* cx, const HandleValueVector values) {
  MOZ_ASSERT(getClass()->isProxyObject());

  size_t nreserved = numReservedSlots();

  // |values| contains the expando slot, private slot and the reserved slots.
  MOZ_ASSERT(values.length() == 2 + nreserved);

  size_t nbytes = js::detail::ProxyValueArray::sizeOf(nreserved);

  auto* valArray = reinterpret_cast<js::detail::ProxyValueArray*>(
      cx->zone()->pod_malloc<uint8_t>(nbytes));
  if (!valArray) {
    return false;
  }

  valArray->expandoSlot = values[0];
  valArray->privateSlot = values[1];

  for (size_t i = 0; i < nreserved; i++) {
    valArray->reservedSlots.slots[i] = values[i + 2];
  }

  // Note: we allocate external slots iff the proxy had an inline
  // ProxyValueArray, so at this point reservedSlots points into the
  // old object and we don't have to free anything.
  data.reservedSlots = &valArray->reservedSlots;
  return true;
}

/* Use this method with extreme caution. It trades the guts of two objects. */
void JSObject::swap(JSContext* cx, HandleObject a, HandleObject b,
                    AutoEnterOOMUnsafeRegion& oomUnsafe) {
  // Ensure swap doesn't cause a finalizer to not be run.
  MOZ_ASSERT(IsBackgroundFinalized(a->asTenured().getAllocKind()) ==
             IsBackgroundFinalized(b->asTenured().getAllocKind()));
  MOZ_ASSERT(a->compartment() == b->compartment());

  // You must have entered the objects' compartment before calling this.
  MOZ_ASSERT(cx->compartment() == a->compartment());

  // Only certain types of objects are allowed to be swapped. This allows the
  // JITs to better optimize objects that can never swap.
  MOZ_RELEASE_ASSERT(js::ObjectMayBeSwapped(a));
  MOZ_RELEASE_ASSERT(js::ObjectMayBeSwapped(b));

  /*
   * Neither object may be in the nursery, but ensure we update any embedded
   * nursery pointers in either object.
   */
  MOZ_ASSERT(!IsInsideNursery(a) && !IsInsideNursery(b));
  gc::StoreBuffer& storeBuffer = cx->runtime()->gc.storeBuffer();
  storeBuffer.putWholeCell(a);
  storeBuffer.putWholeCell(b);
  if (a->zone()->wasGCStarted() || b->zone()->wasGCStarted()) {
    storeBuffer.setMayHavePointersToDeadCells();
  }

  unsigned r = NotifyGCPreSwap(a, b);

  // Do the fundamental swapping of the contents of two objects.
  MOZ_ASSERT(a->compartment() == b->compartment());
  MOZ_ASSERT(a->is<JSFunction>() == b->is<JSFunction>());

  // Don't try to swap functions with different sizes.
  MOZ_ASSERT_IF(a->is<JSFunction>(),
                a->tenuredSizeOfThis() == b->tenuredSizeOfThis());

  // Watch for oddball objects that have special organizational issues and
  // can't be swapped.
  MOZ_ASSERT(!a->is<RegExpObject>() && !b->is<RegExpObject>());
  MOZ_ASSERT(!a->is<ArrayObject>() && !b->is<ArrayObject>());
  MOZ_ASSERT(!a->is<ArrayBufferObject>() && !b->is<ArrayBufferObject>());
  MOZ_ASSERT(!a->is<TypedArrayObject>() && !b->is<TypedArrayObject>());
  MOZ_ASSERT(!a->is<TypedObject>() && !b->is<TypedObject>());

  // Don't swap objects that may currently be participating in shape
  // teleporting optimizations.
  //
  // See: ReshapeForProtoMutation, ReshapeForShadowedProp
  MOZ_ASSERT_IF(a->is<NativeObject>() && a->isUsedAsPrototype(),
                a->taggedProto() == TaggedProto());
  MOZ_ASSERT_IF(b->is<NativeObject>() && b->isUsedAsPrototype(),
                b->taggedProto() == TaggedProto());

  bool aIsProxyWithInlineValues =
      a->is<ProxyObject>() && a->as<ProxyObject>().usingInlineValueArray();
  bool bIsProxyWithInlineValues =
      b->is<ProxyObject>() && b->as<ProxyObject>().usingInlineValueArray();

  bool aIsUsedAsPrototype = a->isUsedAsPrototype();
  bool bIsUsedAsPrototype = b->isUsedAsPrototype();

  // Swap element associations.
  Zone* zone = a->zone();
  zone->swapCellMemory(a, b, MemoryUse::ObjectElements);

  if (a->tenuredSizeOfThis() == b->tenuredSizeOfThis()) {
    // When both objects are the same size, just do a plain swap of their
    // contents.

    // Swap slot associations.
    zone->swapCellMemory(a, b, MemoryUse::ObjectSlots);

    size_t size = a->tenuredSizeOfThis();

    char tmp[mozilla::tl::Max<sizeof(JSFunction),
                              sizeof(JSObject_Slots16)>::value];
    MOZ_ASSERT(size <= sizeof(tmp));

    js_memcpy(tmp, a, size);
    js_memcpy(a, b, size);
    js_memcpy(b, tmp, size);

    if (aIsProxyWithInlineValues) {
      b->as<ProxyObject>().setInlineValueArray();
    }
    if (bIsProxyWithInlineValues) {
      a->as<ProxyObject>().setInlineValueArray();
    }
  } else {
    // Avoid GC in here to avoid confusing the tracing code with our
    // intermediate state.
    gc::AutoSuppressGC suppress(cx);

    // When the objects have different sizes, they will have different
    // numbers of fixed slots before and after the swap, so the slots for
    // native objects will need to be rearranged.
    NativeObject* na = a->is<NativeObject>() ? &a->as<NativeObject>() : nullptr;
    NativeObject* nb = b->is<NativeObject>() ? &b->as<NativeObject>() : nullptr;

    // Remember the original values from the objects.
    RootedValueVector avals(cx);
    void* apriv = nullptr;
    if (na) {
      apriv = na->hasPrivate() ? na->getPrivate() : nullptr;
      for (size_t i = 0; i < na->slotSpan(); i++) {
        if (!avals.append(na->getSlot(i))) {
          oomUnsafe.crash("JSObject::swap");
        }
      }
    }
    RootedValueVector bvals(cx);
    void* bpriv = nullptr;
    if (nb) {
      bpriv = nb->hasPrivate() ? nb->getPrivate() : nullptr;
      for (size_t i = 0; i < nb->slotSpan(); i++) {
        if (!bvals.append(nb->getSlot(i))) {
          oomUnsafe.crash("JSObject::swap");
        }
      }
    }

    // Do the same for proxies storing ProxyValueArray inline.
    ProxyObject* proxyA =
        a->is<ProxyObject>() ? &a->as<ProxyObject>() : nullptr;
    ProxyObject* proxyB =
        b->is<ProxyObject>() ? &b->as<ProxyObject>() : nullptr;

    if (aIsProxyWithInlineValues) {
      if (!CopyProxyValuesBeforeSwap(cx, proxyA, &avals)) {
        oomUnsafe.crash("CopyProxyValuesBeforeSwap");
      }
    }
    if (bIsProxyWithInlineValues) {
      if (!CopyProxyValuesBeforeSwap(cx, proxyB, &bvals)) {
        oomUnsafe.crash("CopyProxyValuesBeforeSwap");
      }
    }

    // Swap the main fields of the objects, whether they are native objects or
    // proxies.
    char tmp[sizeof(JSObject_Slots0)];
    js_memcpy(&tmp, a, sizeof tmp);
    js_memcpy(a, b, sizeof tmp);
    js_memcpy(b, &tmp, sizeof tmp);

    if (na) {
      if (!NativeObject::fillInAfterSwap(cx, b.as<NativeObject>(), na, avals,
                                         apriv)) {
        oomUnsafe.crash("fillInAfterSwap");
      }
    }
    if (nb) {
      if (!NativeObject::fillInAfterSwap(cx, a.as<NativeObject>(), nb, bvals,
                                         bpriv)) {
        oomUnsafe.crash("fillInAfterSwap");
      }
    }
    if (aIsProxyWithInlineValues) {
      if (!b->as<ProxyObject>().initExternalValueArrayAfterSwap(cx, avals)) {
        oomUnsafe.crash("initExternalValueArray");
      }
    }
    if (bIsProxyWithInlineValues) {
      if (!a->as<ProxyObject>().initExternalValueArrayAfterSwap(cx, bvals)) {
        oomUnsafe.crash("initExternalValueArray");
      }
    }
  }

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
    JSContext* cx, HandleObject obj, HandleAtom atom, HandleObject protoProto,
    const JSClass* clasp, Native constructor, unsigned nargs,
    const JSPropertySpec* ps, const JSFunctionSpec* fs,
    const JSPropertySpec* static_ps, const JSFunctionSpec* static_fs,
    NativeObject** ctorp) {
  // Create the prototype object.
  RootedNativeObject proto(
      cx, GlobalObject::createBlankPrototypeInheriting(cx, clasp, protoProto));
  if (!proto) {
    return nullptr;
  }

  RootedNativeObject ctor(cx);
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
                            HandleObject protoProto_, const JSClass* clasp,
                            Native constructor, unsigned nargs,
                            const JSPropertySpec* ps, const JSFunctionSpec* fs,
                            const JSPropertySpec* static_ps,
                            const JSFunctionSpec* static_fs,
                            NativeObject** ctorp) {
  RootedAtom atom(cx, Atomize(cx, clasp->name, strlen(clasp->name)));
  if (!atom) {
    return nullptr;
  }

  /*
   * All instances of the class will inherit properties from the prototype
   * object we are about to create (in DefineConstructorAndPrototype), which
   * in turn will inherit from protoProto.
   *
   * If protoProto is null, default to Object.prototype.
   */
  RootedObject protoProto(cx, protoProto_);
  if (!protoProto) {
    protoProto = GlobalObject::getOrCreateObjectPrototype(cx, cx->global());
    if (!protoProto) {
      return nullptr;
    }
  }

  return DefineConstructorAndPrototype(cx, obj, atom, protoProto, clasp,
                                       constructor, nargs, ps, fs, static_ps,
                                       static_fs, ctorp);
}

static bool ReshapeForProtoMutation(JSContext* cx, HandleObject obj) {
  // To avoid the JIT guarding on each prototype in chain to detect prototype
  // mutation, we can instead reshape the rest of the proto chain such that a
  // guard on any of them is sufficient. To avoid excessive reshaping and
  // invalidation, we apply heuristics to decide when to apply this and when
  // to require a guard.
  //
  // There are two cases:
  //
  // (1) The object is not marked IsUsedAsPrototype. This is the common case.
  //     Because shape implies proto, we rely on the caller changing the
  //     object's shape. The JIT guards on this object's shape or prototype so
  //     there's nothing we have to do here for objects on the proto chain.
  //
  // (2) The object is marked IsUsedAsPrototype. This implies the object may be
  //     participating in shape teleporting. To invalidate JIT ICs depending on
  //     the proto chain being unchanged, set the UncacheableProto shape flag
  //     for this object and objects on its proto chain.
  //
  //     This flag disables future shape teleporting attempts, so next time this
  //     happens the loop below will be a no-op.
  //
  // NOTE: We only handle NativeObjects and don't propagate reshapes through
  //       any non-native objects on the chain.
  //
  // See Also:
  //  - GeneratePrototypeGuards
  //  - GeneratePrototypeHoleGuards

  if (!obj->isUsedAsPrototype()) {
    return true;
  }

  RootedObject pobj(cx, obj);

  while (pobj && pobj->is<NativeObject>()) {
    if (!pobj->hasUncacheableProto()) {
      if (!JSObject::setUncacheableProto(cx, pobj)) {
        return false;
      }
    }
    pobj = pobj->staticPrototype();
  }

  return true;
}

static bool SetProto(JSContext* cx, HandleObject obj,
                     Handle<js::TaggedProto> proto) {
  // Update prototype shapes if needed to invalidate JIT code that is affected
  // by a prototype mutation.
  if (!ReshapeForProtoMutation(cx, obj)) {
    return false;
  }

  if (proto.isObject()) {
    RootedObject protoObj(cx, proto.toObject());
    if (!JSObject::setIsUsedAsPrototype(cx, protoObj)) {
      return false;
    }
  }

  return JSObject::setProtoUnchecked(cx, obj, proto);
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

  {
    AutoRealm ar(cx, globalObj);
    obj.set(GlobalObject::getOrCreateObjectPrototype(cx, globalObj));
    if (!obj) {
      return false;
    }
  }

  // The object might be from a different compartment, so wrap it.
  if (obj && !cx->compartment()->wrap(cx, obj)) {
    return false;
  }

  return true;
}

static bool IsStandardPrototype(JSObject* obj, JSProtoKey key) {
  Value v = obj->nonCCWGlobal().getPrototype(key);
  return v.isObject() && obj == &v.toObject();
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

  GlobalObject& global = obj->as<JSFunction>().global();
  for (size_t k = 0; k < JSProto_LIMIT; ++k) {
    JSProtoKey key = static_cast<JSProtoKey>(k);
    if (global.getConstructor(key) == ObjectValue(*obj)) {
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

bool js::LookupName(JSContext* cx, HandlePropertyName name,
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

bool js::LookupNameWithGlobalDefault(JSContext* cx, HandlePropertyName name,
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

bool js::LookupNameUnqualified(JSContext* cx, HandlePropertyName name,
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
    if (prop.isFound() && name != cx->names().dotThis) {
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
        MOZ_ASSERT(name != cx->names().dotThis);
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
   * Disallow mutating the [[Prototype]] on Typed Objects, per the spec.
   */
  if (obj->is<TypedObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_SET_PROTO_OF,
                              "incompatible TypedObject");
    return false;
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
  if (!SetProto(cx, obj, taggedProto)) {
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
    HandleNativeObject nobj = obj.as<NativeObject>();
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
    MOZ_ASSERT(!cx->isHelperThreadContext());
    return op(cx, obj, id, desc, result);
  }
  return NativeDefineProperty(cx, obj.as<NativeObject>(), id, desc, result);
}

bool js::DefineDataProperty(JSContext* cx, HandleObject obj, HandleId id,
                            HandleValue value, unsigned attrs,
                            ObjectOpResult& result) {
  Rooted<PropertyDescriptor> desc(cx, PropertyDescriptor::Data(value, attrs));
  if (DefinePropertyOp op = obj->getOpsDefineProperty()) {
    MOZ_ASSERT(!cx->isHelperThreadContext());
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
    MOZ_ASSERT(!cx->isHelperThreadContext());
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
    MOZ_ASSERT(!cx->isHelperThreadContext());
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
    MOZ_ASSERT(!cx->isHelperThreadContext());
    return Proxy::setImmutablePrototype(cx, obj, succeeded);
  }

  // If this is a global object, resolve the Object class first to ensure the
  // global's prototype is set to Object.prototype before we mark the global as
  // having an immutable prototype.
  if (obj->is<GlobalObject>()) {
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    if (!GlobalObject::ensureConstructor(cx, global, JSProto_Object)) {
      return false;
    }
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
                                 MutableHandleId id,
                                 js::PinningBehavior pin = js::DoNotPinAtom);

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
      cx->realm()->creationOptions().getWeakRefsEnabled() ==
          JS::WeakRefSpecifier::EnabledWithoutCleanupSome &&
      id == NameToId(cx->names().cleanupSome)) {
    return true;
  }

  return false;
}

static bool DefineFunctionFromSpec(JSContext* cx, HandleObject obj,
                                   const JSFunctionSpec* fs, unsigned flags,
                                   DefineAsIntrinsic intrinsic) {
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

  if (intrinsic == AsIntrinsic) {
    fun->setIsIntrinsic();
  }

  RootedValue funVal(cx, ObjectValue(*fun));
  return DefineDataProperty(cx, obj, id, funVal, flags & ~JSFUN_FLAGS_MASK);
}

bool js::DefineFunctions(JSContext* cx, HandleObject obj,
                         const JSFunctionSpec* fs,
                         DefineAsIntrinsic intrinsic) {
  for (; fs->name; fs++) {
    if (!DefineFunctionFromSpec(cx, obj, fs, fs->flags, intrinsic)) {
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
    str = JS_AtomizeAndPinString(cx, clasp->name);
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

    /* Optimize (new String(...)).toString(). */
    if (clasp == &StringObject::class_) {
      StringObject* nobj = &obj->as<StringObject>();
      if (HasNativeMethodPure(nobj, cx->names().toString, str_toString, cx)) {
        vp.setString(nobj->unbox());
        return true;
      }
    }

    if (!MaybeCallMethod(cx, obj, id, vp)) {
      return false;
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

    /* Optimize new String(...).valueOf(). */
    if (clasp == &StringObject::class_) {
      StringObject* nobj = &obj->as<StringObject>();
      if (HasNativeMethodPure(nobj, cx->names().valueOf, str_toString, cx)) {
        vp.setString(nobj->unbox());
        return true;
      }
    }

    /* Optimize new Number(...).valueOf(). */
    if (clasp == &NumberObject::class_) {
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
                                            HandlePropertyName key) {
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
    for (ShapePropertyIter<NoGC> iter(obj->shape()); !iter.done(); iter++) {
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
#define TEST_SLOT_MATCHES_PROTOTYPE(name, clasp) \
  else if ((JSProto_##name) == slot) {           \
    slotname = js_##name##_str;                  \
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
 * Routines to print out values during debugging.  These are FRIEND_API to help
 * the debugger find them and to support temporarily hacking js::Dump* calls
 * into other code.
 */

static void dumpValue(const Value& v, js::GenericPrinter& out) {
  switch (v.type()) {
    case ValueType::Null:
      out.put("null");
      break;
    case ValueType::Undefined:
      out.put("undefined");
      break;
    case ValueType::Int32:
      out.printf("%d", v.toInt32());
      break;
    case ValueType::Double:
      out.printf("%g", v.toDouble());
      break;
    case ValueType::String:
      v.toString()->dumpNoNewline(out);
      break;
    case ValueType::Symbol:
      v.toSymbol()->dump(out);
      break;
    case ValueType::BigInt:
      v.toBigInt()->dump(out);
      break;
    case ValueType::Object:
      if (v.toObject().is<JSFunction>()) {
        JSFunction* fun = &v.toObject().as<JSFunction>();
        if (fun->displayAtom()) {
          out.put("<function ");
          EscapedStringPrinter(out, fun->displayAtom(), 0);
        } else {
          out.put("<unnamed function");
        }
        if (fun->hasBaseScript()) {
          BaseScript* script = fun->baseScript();
          out.printf(" (%s:%u)", script->filename() ? script->filename() : "",
                     script->lineno());
        }
        out.printf(" at %p>", (void*)fun);
      } else {
        JSObject* obj = &v.toObject();
        const JSClass* clasp = obj->getClass();
        out.printf("<%s%s at %p>", clasp->name,
                   (clasp == &PlainObject::class_) ? "" : " object",
                   (void*)obj);
      }
      break;
    case ValueType::Boolean:
      if (v.toBoolean()) {
        out.put("true");
      } else {
        out.put("false");
      }
      break;
    case ValueType::Magic:
      out.put("<magic");
      switch (v.whyMagic()) {
        case JS_ELEMENTS_HOLE:
          out.put(" elements hole");
          break;
        case JS_NO_ITER_VALUE:
          out.put(" no iter value");
          break;
        case JS_GENERATOR_CLOSING:
          out.put(" generator closing");
          break;
        case JS_OPTIMIZED_OUT:
          out.put(" optimized out");
          break;
        default:
          out.put(" ?!");
          break;
      }
      out.putChar('>');
      break;
    case ValueType::PrivateGCThing:
      out.printf("<PrivateGCThing %p>", v.toGCThing());
      break;
  }
}

namespace js {

// We don't want jsfriendapi.h to depend on GenericPrinter,
// so these functions are declared directly in the cpp.

JS_PUBLIC_API void DumpValue(const JS::Value& val, js::GenericPrinter& out);

JS_PUBLIC_API void DumpId(jsid id, js::GenericPrinter& out);

JS_PUBLIC_API void DumpInterpreterFrame(JSContext* cx, js::GenericPrinter& out,
                                        InterpreterFrame* start = nullptr);

}  // namespace js

JS_PUBLIC_API void js::DumpValue(const Value& val, js::GenericPrinter& out) {
  dumpValue(val, out);
  out.putChar('\n');
}

JS_PUBLIC_API void js::DumpId(jsid id, js::GenericPrinter& out) {
  out.printf("jsid %p = ", (void*)JSID_BITS(id));
  dumpValue(IdToValue(id), out);
  out.putChar('\n');
}

static void DumpProperty(const NativeObject* obj, PropMap* map, uint32_t index,
                         js::GenericPrinter& out) {
  PropertyInfoWithKey prop = map->getPropertyInfoWithKey(index);
  jsid id = prop.key();
  if (id.isAtom()) {
    id.toAtom()->dumpCharsNoNewline(out);
  } else if (id.isInt()) {
    out.printf("%d", id.toInt());
  } else if (id.isSymbol()) {
    id.toSymbol()->dump(out);
  } else {
    out.printf("id %p", reinterpret_cast<void*>(JSID_BITS(id)));
  }

  if (prop.isDataProperty()) {
    out.printf(": ");
    dumpValue(obj->getSlot(prop.slot()), out);
  } else if (prop.isAccessorProperty()) {
    out.printf(": getter %p setter %p", obj->getGetter(prop),
               obj->getSetter(prop));
  }

  out.printf(" (map %p/%u", map, index);

  if (prop.enumerable()) {
    out.put(" enumerable");
  }
  if (prop.configurable()) {
    out.put(" configurable");
  }
  if (prop.isDataDescriptor() && prop.writable()) {
    out.put(" writable");
  }

  if (prop.isCustomDataProperty()) {
    out.printf(" <custom-data-prop>");
  }

  if (prop.hasSlot()) {
    out.printf(" slot %u", prop.slot());
  }

  out.printf(")\n");
}

bool JSObject::hasSameRealmAs(JSContext* cx) const {
  return nonCCWRealm() == cx->realm();
}

bool JSObject::uninlinedIsProxyObject() const { return is<ProxyObject>(); }

bool JSObject::uninlinedNonProxyIsExtensible() const {
  return nonProxyIsExtensible();
}

void JSObject::dump(js::GenericPrinter& out) const {
  const JSObject* obj = this;
  out.printf("object %p\n", obj);

  if (IsCrossCompartmentWrapper(this)) {
    out.printf("  compartment %p\n", compartment());
  } else {
    JSObject* globalObj = &nonCCWGlobal();
    out.printf("  global %p [%s]\n", globalObj, globalObj->getClass()->name);
  }

  const JSClass* clasp = obj->getClass();
  out.printf("  class %p %s\n", clasp, clasp->name);

  if (IsProxy(obj)) {
    auto* handler = GetProxyHandler(obj);
    out.printf("    handler %p", handler);
    if (IsDeadProxyObject(obj)) {
      out.printf(" (DeadObjectProxy)");
    } else if (IsCrossCompartmentWrapper(obj)) {
      out.printf(" (CCW)");
    }
    out.putChar('\n');

    Value priv = GetProxyPrivate(obj);
    if (!priv.isUndefined()) {
      out.printf("    private ");
      dumpValue(priv, out);
      out.putChar('\n');
    }

    Value expando = GetProxyExpando(obj);
    if (!expando.isNull()) {
      out.printf("    expando ");
      dumpValue(expando, out);
      out.putChar('\n');
    }
  }

  const Shape* shape = obj->shape();
  out.printf("  shape %p\n", shape);

  out.put("  flags:");
  if (obj->isUsedAsPrototype()) {
    out.put(" used_as_prototype");
  }
  if (!obj->is<ProxyObject>() && !obj->nonProxyIsExtensible()) {
    out.put(" not_extensible");
  }
  if (obj->maybeHasInterestingSymbolProperty()) {
    out.put(" maybe_has_interesting_symbol");
  }
  if (obj->isBoundFunction()) {
    out.put(" bound_function");
  }
  if (obj->isQualifiedVarObj()) {
    out.put(" varobj");
  }
  if (obj->isUnqualifiedVarObj()) {
    out.put(" unqualified_varobj");
  }
  if (obj->hasUncacheableProto()) {
    out.put(" has_uncacheable_proto");
  }
  if (obj->hasStaticPrototype() && obj->staticPrototypeIsImmutable()) {
    out.put(" immutable_prototype");
  }

  const NativeObject* nobj =
      obj->is<NativeObject>() ? &obj->as<NativeObject>() : nullptr;
  if (nobj) {
    if (nobj->inDictionaryMode()) {
      out.put(" inDictionaryMode");
    }
    if (nobj->hadGetterSetterChange()) {
      out.put(" had_getter_setter_change");
    }
    if (nobj->isIndexed()) {
      out.put(" indexed");
    }
    if (nobj->is<PlainObject>() &&
        nobj->as<PlainObject>().hasNonWritableOrAccessorPropExclProto()) {
      out.put(" has_non_writable_or_accessor_prop_excl_proto");
    }
    if (!nobj->denseElementsArePacked()) {
      out.put(" non_packed_elements");
    }
    if (nobj->getElementsHeader()->isNotExtensible()) {
      out.put(" not_extensible");
    }
    if (nobj->getElementsHeader()->isSealed()) {
      out.put(" sealed_elements");
    }
    if (nobj->getElementsHeader()->isFrozen()) {
      out.put(" frozen_elements");
    }
    if (nobj->getElementsHeader()->maybeInIteration()) {
      out.put(" elements_maybe_in_iteration");
    }
  } else {
    out.put(" not_native");
  }
  out.putChar('\n');

  out.put("  proto ");
  TaggedProto proto = obj->taggedProto();
  if (proto.isDynamic()) {
    out.put("<dynamic>");
  } else {
    dumpValue(ObjectOrNullValue(proto.toObjectOrNull()), out);
  }
  out.putChar('\n');

  if (nobj) {
    if (clasp->flags & JSCLASS_HAS_PRIVATE) {
      out.printf("  private %p\n", nobj->getPrivate());
    }

    uint32_t reserved = JSCLASS_RESERVED_SLOTS(clasp);
    if (reserved) {
      out.printf("  reserved slots:\n");
      for (uint32_t i = 0; i < reserved; i++) {
        out.printf("    %3u ", i);
        out.put(": ");
        dumpValue(nobj->getSlot(i), out);
        out.putChar('\n');
      }
    }

    out.put("  properties:\n");

    if (PropMap* map = nobj->shape()->propMap()) {
      Vector<PropMap*, 8, SystemAllocPolicy> maps;
      while (true) {
        if (!maps.append(map)) {
          out.printf("(OOM while appending maps)\n");
          break;
        }
        if (!map->hasPrevious()) {
          break;
        }
        map = map->asLinked()->previous();
      }

      for (size_t i = maps.length(); i > 0; i--) {
        size_t index = i - 1;
        uint32_t len =
            (index == 0) ? nobj->shape()->propMapLength() : PropMap::Capacity;
        for (uint32_t j = 0; j < len; j++) {
          PropMap* map = maps[index];
          if (!map->hasKey(j)) {
            MOZ_ASSERT(map->isDictionary());
            continue;
          }
          out.printf("    ");
          DumpProperty(nobj, map, j, out);
        }
      }
    }

    uint32_t slots = nobj->getDenseInitializedLength();
    if (slots) {
      out.put("  elements:\n");
      for (uint32_t i = 0; i < slots; i++) {
        out.printf("    %3u: ", i);
        dumpValue(nobj->getDenseElement(i), out);
        out.putChar('\n');
      }
    }
  }
}

// For debuggers.
void JSObject::dump() const {
  Fprinter out(stderr);
  dump(out);
}

static void MaybeDumpScope(Scope* scope, js::GenericPrinter& out) {
  if (scope) {
    out.printf("  scope: %s\n", ScopeKindString(scope->kind()));
    for (BindingIter bi(scope); bi; bi++) {
      out.put("    ");
      dumpValue(StringValue(bi.name()), out);
      out.putChar('\n');
    }
  }
}

static void MaybeDumpValue(const char* name, const Value& v,
                           js::GenericPrinter& out) {
  if (!v.isNull()) {
    out.printf("  %s: ", name);
    dumpValue(v, out);
    out.putChar('\n');
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
      dumpValue(v, out);
    } else {
      out.put("global or eval frame, no callee");
    }
    out.putChar('\n');

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
      dumpValue(i.interpFrame()->returnValue(), out);
      out.putChar('\n');
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

namespace js {

// We don't want jsfriendapi.h to depend on GenericPrinter,
// so these functions are declared directly in the cpp.

JS_PUBLIC_API void DumpBacktrace(JSContext* cx, js::GenericPrinter& out);

}  // namespace js

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

js::gc::AllocKind JSObject::allocKindForTenure(
    const js::Nursery& nursery) const {
  using namespace js::gc;

  MOZ_ASSERT(IsInsideNursery(this));

  if (is<ArrayObject>()) {
    const ArrayObject& aobj = as<ArrayObject>();
    MOZ_ASSERT(aobj.numFixedSlots() == 0);

    /* Use minimal size object if we are just going to copy the pointer. */
    if (!nursery.isInside(aobj.getElementsHeader())) {
      return gc::AllocKind::OBJECT0_BACKGROUND;
    }

    size_t nelements = aobj.getDenseCapacity();
    return ForegroundToBackgroundAllocKind(GetGCArrayKind(nelements));
  }

  if (is<JSFunction>()) {
    return as<JSFunction>().getAllocKind();
  }

  /*
   * Typed arrays in the nursery may have a lazily allocated buffer, make
   * sure there is room for the array's fixed data when moving the array.
   */
  if (is<TypedArrayObject>() && !as<TypedArrayObject>().hasBuffer()) {
    gc::AllocKind allocKind;
    if (as<TypedArrayObject>().hasInlineElements()) {
      size_t nbytes = as<TypedArrayObject>().byteLength();
      allocKind = TypedArrayObject::AllocKindForLazyBuffer(nbytes);
    } else {
      allocKind = GetGCObjectKind(getClass());
    }
    return ForegroundToBackgroundAllocKind(allocKind);
  }

  // Proxies that are CrossCompartmentWrappers may be nursery allocated.
  if (is<ProxyObject>()) {
    return as<ProxyObject>().allocKindForTenure();
  }

  // Inlined typed objects are followed by their data, so make sure we copy
  // it all over to the new object.
  if (is<InlineTypedObject>()) {
    // Figure out the size of this object, from the prototype's RttValue.
    // The objects we are traversing here are all tenured, so we don't need
    // to check forwarding pointers.
    RttValue& descr = as<InlineTypedObject>().rttValue();
    MOZ_ASSERT(!IsInsideNursery(&descr));
    return InlineTypedObject::allocKindForRttValue(&descr);
  }

  if (is<OutlineTypedObject>()) {
    return OutlineTypedObject::allocKind();
  }

  // All nursery allocatable non-native objects are handled above.
  return as<NativeObject>().allocKindForTenure();
}

void JSObject::addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                      JS::ClassInfo* info) {
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
  } else if (is<RegExpStaticsObject>()) {
    info->objectsMallocHeapMisc +=
        as<RegExpStaticsObject>().sizeOfData(mallocSizeOf);
  } else if (is<PropertyIteratorObject>()) {
    info->objectsMallocHeapMisc +=
        as<PropertyIteratorObject>().sizeOfMisc(mallocSizeOf);
  } else if (is<ArrayBufferObject>()) {
    ArrayBufferObject::addSizeOfExcludingThis(this, mallocSizeOf, info);
  } else if (is<SharedArrayBufferObject>()) {
    SharedArrayBufferObject::addSizeOfExcludingThis(this, mallocSizeOf, info);
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
  obj.addSizeOfExcludingThis(mallocSizeOf, &info);
  return obj.tenuredSizeOfThis() + info.sizeOfAllThings();
}

const char16_t JS::ubi::Concrete<JSObject>::concreteTypeName[] = u"JSObject";

void JSObject::traceChildren(JSTracer* trc) {
  TraceCellHeaderEdge(trc, this, "shape");

  const JSClass* clasp = getClass();
  if (clasp->isNativeObject()) {
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
  if (clasp->hasTrace()) {
    clasp->doTrace(trc, this);
  }

  if (trc->isMarkingTracer()) {
    GCMarker::fromTracer(trc)->markImplicitEdges(this);
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
    jsid speciesId = SYMBOL_TO_JSID(cx->wellKnownSymbols().species);
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
  RootedId speciesId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().species));
  if (!GetProperty(cx, ctorObj, ctor, speciesId, &s)) {
    return nullptr;
  }

  // Step 6.
  if (s.isNullOrUndefined()) {
    return defaultCtor;
  }

  // Step 7.
  if (IsConstructor(s)) {
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
  } else {
    vp.setUndefined();
  }

  return true;
}

#ifdef DEBUG
/* static */
void JSObject::debugCheckNewObject(Shape* shape, js::gc::AllocKind allocKind,
                                   js::gc::InitialHeap heap) {
  const JSClass* clasp = shape->getObjectClass();
  MOZ_ASSERT(clasp != &ArrayObject::class_);

  if (!ClassCanHaveFixedData(clasp)) {
    MOZ_ASSERT(shape);
    MOZ_ASSERT(gc::GetGCKindSlots(allocKind, clasp) == shape->numFixedSlots());
  }

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
                heap == gc::TenuredHeap ||
                    CanNurseryAllocateFinalizedClass(clasp) ||
                    clasp->isProxyObject());

  MOZ_ASSERT(!shape->realm()->hasObjectPendingMetadata());

  // Non-native classes manage their own data and slots, so numFixedSlots is
  // always 0. Note that proxy classes can have reserved slots but they're not
  // included in numFixedSlots.
  if (!clasp->isNativeObject()) {
    MOZ_ASSERT_IF(!clasp->isProxyObject(), JSCLASS_RESERVED_SLOTS(clasp) == 0);
    MOZ_ASSERT(!clasp->hasPrivate());
    MOZ_ASSERT_IF(shape, shape->numFixedSlots() == 0);
  }
}
#endif
