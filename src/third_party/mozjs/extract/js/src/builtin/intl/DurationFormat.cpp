/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.DurationFormat implementation. */

#include "builtin/intl/DurationFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/intl/DateTimeFormat.h"
#include "mozilla/Span.h"

#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/temporal/Duration.h"
#include "gc/AllocKind.h"
#include "gc/GCContext.h"
#include "js/CallArgs.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"
#include "vm/SelfHosting.h"
#include "vm/WellKnownAtom.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

const JSClass DurationFormatObject::class_ = {
    "Intl.DurationFormat",
    JSCLASS_HAS_RESERVED_SLOTS(DurationFormatObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_DurationFormat),
    JS_NULL_CLASS_OPS,
    &DurationFormatObject::classSpec_,
};

const JSClass& DurationFormatObject::protoClass_ = PlainObject::class_;

static bool durationFormat_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().DurationFormat);
  return true;
}

static const JSFunctionSpec durationFormat_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf",
                      "Intl_DurationFormat_supportedLocalesOf", 1, 0),
    JS_FS_END,
};

static const JSFunctionSpec durationFormat_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_DurationFormat_resolvedOptions",
                      0, 0),
    JS_SELF_HOSTED_FN("format", "Intl_DurationFormat_format", 1, 0),
    JS_SELF_HOSTED_FN("formatToParts", "Intl_DurationFormat_formatToParts", 1,
                      0),
    JS_FN("toSource", durationFormat_toSource, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec durationFormat_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Intl.DurationFormat", JSPROP_READONLY),
    JS_PS_END,
};

static bool DurationFormat(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec DurationFormatObject::classSpec_ = {
    GenericCreateConstructor<DurationFormat, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<DurationFormatObject>,
    durationFormat_static_methods,
    nullptr,
    durationFormat_methods,
    durationFormat_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

/**
 * Intl.DurationFormat ( [ locales [ , options ] ] )
 */
static bool DurationFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Intl.DurationFormat")) {
    return false;
  }

  // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_DurationFormat,
                                          &proto)) {
    return false;
  }

  Rooted<DurationFormatObject*> durationFormat(
      cx, NewObjectWithClassProto<DurationFormatObject>(cx, proto));
  if (!durationFormat) {
    return false;
  }

  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Steps 3-28.
  if (!intl::InitializeObject(cx, durationFormat,
                              cx->names().InitializeDurationFormat, locales,
                              options)) {
    return false;
  }

  args.rval().setObject(*durationFormat);
  return true;
}

bool js::intl_GetTimeSeparator(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isString());
  MOZ_ASSERT(args[1].isString());

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  UniqueChars numberingSystem = EncodeAscii(cx, args[1].toString());
  if (!numberingSystem) {
    return false;
  }

  intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> separator(cx);
  auto result = mozilla::intl::DateTimeFormat::GetTimeSeparator(
      mozilla::MakeStringSpan(locale.get()),
      mozilla::MakeStringSpan(numberingSystem.get()), separator);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  JSString* str = separator.toString(cx);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

bool js::TemporalDurationToLocaleString(JSContext* cx,
                                        const JS::CallArgs& args) {
  MOZ_ASSERT(args.thisv().isObject());
  MOZ_ASSERT(args.thisv().toObject().is<temporal::DurationObject>());

  Rooted<DurationFormatObject*> durationFormat(
      cx, NewBuiltinClassInstance<DurationFormatObject>(cx));
  if (!durationFormat) {
    return false;
  }

  if (!intl::InitializeObject(cx, durationFormat,
                              cx->names().InitializeDurationFormat, args.get(0),
                              args.get(1))) {
    return false;
  }

  Rooted<Value> thisv(cx, ObjectValue(*durationFormat));

  FixedInvokeArgs<1> invokeArgs(cx);
  invokeArgs[0].set(args.thisv());

  return CallSelfHostedFunction(cx, cx->names().Intl_DurationFormat_format,
                                thisv, invokeArgs, args.rval());
}
