/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Queuing strategies. */

#include "builtin/streams/QueueingStrategies.h"

#include "builtin/streams/ClassSpecMacro.h"  // JS_STREAMS_CLASS_SPEC
#include "js/CallArgs.h"                     // JS::CallArgs{,FromVp}
#include "js/Class.h"        // JS::ObjectOpResult, JS_NULL_CLASS_OPS
#include "js/Conversions.h"  // JS::ToNumber
#include "js/PropertySpec.h"  // JS{Property,Function}Spec, JS_FN, JS_FS_END, JS_PS_END
#include "js/ProtoKey.h"          // JSProto_{ByteLength,Count}QueuingStrategy
#include "js/RootingAPI.h"        // JS::{Handle,Rooted}
#include "vm/JSObject.h"          // js::GetPrototypeFromBuiltinConstructor
#include "vm/ObjectOperations.h"  // js::{Define,Get}Property
#include "vm/Runtime.h"           // JSAtomState
#include "vm/StringType.h"        // js::NameToId, PropertyName

#include "vm/Compartment-inl.h"   // js::UnwrapAndTypeCheckThis
#include "vm/JSObject-inl.h"      // js::NewObjectWithClassProto
#include "vm/NativeObject-inl.h"  // js::ThrowIfNotConstructing

using js::ByteLengthQueuingStrategy;
using js::CountQueuingStrategy;
using js::PropertyName;
using js::UnwrapAndTypeCheckThis;

using JS::CallArgs;
using JS::CallArgsFromVp;
using JS::Handle;
using JS::ObjectOpResult;
using JS::Rooted;
using JS::ToNumber;
using JS::ToObject;
using JS::Value;

/*** 6.1. Queuing strategies ************************************************/

// Streams spec, 6.1.2.2. new ByteLengthQueuingStrategy({ highWaterMark })
bool js::ByteLengthQueuingStrategy::constructor(JSContext* cx, unsigned argc,
                                                Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "ByteLengthQueuingStrategy")) {
    return false;
  }

  // Implicit in the spec: Create the new strategy object.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(
          cx, args, JSProto_ByteLengthQueuingStrategy, &proto)) {
    return false;
  }
  Rooted<ByteLengthQueuingStrategy*> strategy(
      cx, NewObjectWithClassProto<ByteLengthQueuingStrategy>(cx, proto));
  if (!strategy) {
    return false;
  }

  // Implicit in the spec: Argument destructuring.
  RootedObject argObj(cx, ToObject(cx, args.get(0)));
  if (!argObj) {
    return false;
  }

  // https://heycam.github.io/webidl/#es-dictionary
  // 3.2.17. Dictionary types
  // Step 4.1.2: Let esMemberValue be an ECMAScript value,
  //             depending on Type(esDict): ? Get(esDict, key)
  RootedValue highWaterMarkV(cx);
  if (!GetProperty(cx, argObj, argObj, cx->names().highWaterMark,
                   &highWaterMarkV)) {
    return false;
  }
  // Step 4.1.5: Otherwise, if esMemberValue is undefined and
  //             member is required, then throw a TypeError.
  if (highWaterMarkV.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_STREAM_MISSING_HIGHWATERMARK);
    return false;
  }
  // Step 4.1.3: If esMemberValue is not undefined, then:
  //  Let idlMemberValue be the result of converting esMemberValue to
  //  an IDL value whose type is the type member is declared to be of.
  double highWaterMark;
  if (!ToNumber(cx, highWaterMarkV, &highWaterMark)) {
    return false;
  }

  // Step 1: Set this.[[highWaterMark]] to init["highWaterMark"].
  strategy->setHighWaterMark(highWaterMark);

  args.rval().setObject(*strategy);
  return true;
}

static bool ByteLengthQueuingStrategy_highWaterMark(JSContext* cx,
                                                    unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<ByteLengthQueuingStrategy*> unwrappedStrategy(
      cx, UnwrapAndTypeCheckThis<ByteLengthQueuingStrategy>(
              cx, args, "get highWaterMark"));
  if (!unwrappedStrategy) {
    return false;
  }

  // Step 1: Return this.[[highWaterMark]].
  args.rval().setDouble(unwrappedStrategy->highWaterMark());
  return true;
}

// Streams spec 6.1.2.3.1. size ( chunk )
static bool ByteLengthQueuingStrategy_size(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: Return ? GetV(chunk, "byteLength").
  return GetProperty(cx, args.get(0), cx->names().byteLength, args.rval());
}

static const JSPropertySpec ByteLengthQueuingStrategy_properties[] = {
    JS_PSG("highWaterMark", ByteLengthQueuingStrategy_highWaterMark,
           JSPROP_ENUMERATE),
    JS_STRING_SYM_PS(toStringTag, "ByteLengthQueuingStrategy", JSPROP_READONLY),
    JS_PS_END};

static const JSFunctionSpec ByteLengthQueuingStrategy_methods[] = {
    JS_FN("size", ByteLengthQueuingStrategy_size, 1, 0), JS_FS_END};

JS_STREAMS_CLASS_SPEC(ByteLengthQueuingStrategy, 1, SlotCount, 0, 0,
                      JS_NULL_CLASS_OPS);

// Streams spec, 6.1.3.2. new CountQueuingStrategy({ highWaterMark })
bool js::CountQueuingStrategy::constructor(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "CountQueuingStrategy")) {
    return false;
  }

  // Implicit in the spec: Create the new strategy object.
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(
          cx, args, JSProto_CountQueuingStrategy, &proto)) {
    return false;
  }
  Rooted<CountQueuingStrategy*> strategy(
      cx, NewObjectWithClassProto<CountQueuingStrategy>(cx, proto));
  if (!strategy) {
    return false;
  }

  // Implicit in the spec: Argument destructuring.
  RootedObject argObj(cx, ToObject(cx, args.get(0)));
  if (!argObj) {
    return false;
  }
  // https://heycam.github.io/webidl/#es-dictionary
  // 3.2.17. Dictionary types
  // Step 4.1.2: Let esMemberValue be an ECMAScript value,
  //             depending on Type(esDict): ? Get(esDict, key)
  RootedValue highWaterMarkV(cx);
  if (!GetProperty(cx, argObj, argObj, cx->names().highWaterMark,
                   &highWaterMarkV)) {
    return false;
  }
  // Step 4.1.5: Otherwise, if esMemberValue is undefined and
  //             member is required, then throw a TypeError.
  if (highWaterMarkV.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_STREAM_MISSING_HIGHWATERMARK);
    return false;
  }
  // Step 4.1.3: If esMemberValue is not undefined, then:
  //  Let idlMemberValue be the result of converting esMemberValue to
  //  an IDL value whose type is the type member is declared to be of.
  double highWaterMark;
  if (!ToNumber(cx, highWaterMarkV, &highWaterMark)) {
    return false;
  }

  // Step 1: Set this.[[highWaterMark]] to init["highWaterMark"].
  strategy->setHighWaterMark(highWaterMark);

  args.rval().setObject(*strategy);
  return true;
}

static bool CountQueuingStrategy_highWaterMark(JSContext* cx, unsigned argc,
                                               Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<CountQueuingStrategy*> unwrappedStrategy(
      cx, UnwrapAndTypeCheckThis<CountQueuingStrategy>(cx, args,
                                                       "get highWaterMark"));
  if (!unwrappedStrategy) {
    return false;
  }

  // Step 1: Return this.[[highWaterMark]].
  args.rval().setDouble(unwrappedStrategy->highWaterMark());
  return true;
}

// Streams spec 6.1.3.3.1. size ( chunk )
static bool CountQueuingStrategy_size(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1: Return 1.
  args.rval().setInt32(1);
  return true;
}

static const JSPropertySpec CountQueuingStrategy_properties[] = {
    JS_PSG("highWaterMark", CountQueuingStrategy_highWaterMark,
           JSPROP_ENUMERATE),
    JS_STRING_SYM_PS(toStringTag, "CountQueuingStrategy", JSPROP_READONLY),
    JS_PS_END};

static const JSFunctionSpec CountQueuingStrategy_methods[] = {
    JS_FN("size", CountQueuingStrategy_size, 0, 0), JS_FS_END};

JS_STREAMS_CLASS_SPEC(CountQueuingStrategy, 1, SlotCount, 0, 0,
                      JS_NULL_CLASS_OPS);
