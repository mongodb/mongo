/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/Value.h"

#include "mozilla/Assertions.h"

#include <inttypes.h>

#include "gc/Cell.h"         // js::gc::Cell
#include "js/Conversions.h"  // JS::NumberToString, JS::MaximumNumberToStringLength
#include "js/Printer.h"      // js::GenericPrinter, js::Fprinter
#include "vm/BigIntType.h"   // JS::BigInt
#include "vm/JSObject.h"     // JSObject
#include "vm/JSONPrinter.h"  // js::JSONPrinter
#include "vm/StringType.h"   // JSString
#include "vm/SymbolType.h"   // JS::Symbol

static const JS::Value JSVAL_NULL =
    JS::Value::fromTagAndPayload(JSVAL_TAG_NULL, 0);
static const JS::Value JSVAL_FALSE =
    JS::Value::fromTagAndPayload(JSVAL_TAG_BOOLEAN, false);
static const JS::Value JSVAL_TRUE =
    JS::Value::fromTagAndPayload(JSVAL_TAG_BOOLEAN, true);
static const JS::Value JSVAL_VOID =
    JS::Value::fromTagAndPayload(JSVAL_TAG_UNDEFINED, 0);
static const mozilla::Maybe<JS::Value> JSVAL_NOTHING;

namespace JS {

const HandleValue NullHandleValue =
    HandleValue::fromMarkedLocation(&JSVAL_NULL);
const HandleValue UndefinedHandleValue =
    HandleValue::fromMarkedLocation(&JSVAL_VOID);
const HandleValue TrueHandleValue =
    HandleValue::fromMarkedLocation(&JSVAL_TRUE);
const HandleValue FalseHandleValue =
    HandleValue::fromMarkedLocation(&JSVAL_FALSE);
const Handle<mozilla::Maybe<Value>> NothingHandleValue =
    Handle<mozilla::Maybe<Value>>::fromMarkedLocation(&JSVAL_NOTHING);

#ifdef DEBUG
void JS::Value::assertTraceKindMatches(js::gc::Cell* cell) const {
  MOZ_ASSERT(traceKind() == cell->getTraceKind());
}
#endif

}  // namespace JS

void js::ReportBadValueTypeAndCrash(const JS::Value& value) {
  MOZ_CRASH_UNSAFE_PRINTF("JS::Value has illegal type: 0x%" PRIx64,
                          value.asRawBits());
}

#if defined(DEBUG) || defined(JS_JITSPEW)
void JS::Value::dump() const {
  js::Fprinter out(stderr);
  dump(out);
}

void JS::Value::dump(js::GenericPrinter& out) const {
  js::JSONPrinter json(out);
  dump(json);
  out.put("\n");
}

void JS::Value::dump(js::JSONPrinter& json) const {
  json.beginObject();
  dumpFields(json);
  json.endObject();
}

template <typename KnownF, typename UnknownF>
void WhyMagicToString(JSWhyMagic why, KnownF known, UnknownF unknown) {
  switch (why) {
    case JS_ELEMENTS_HOLE:
      known("JS_ELEMENTS_HOLE");
      break;
    case JS_NO_ITER_VALUE:
      known("JS_NO_ITER_VALUE");
      break;
    case JS_GENERATOR_CLOSING:
      known("JS_GENERATOR_CLOSING");
      break;
    case JS_ARG_POISON:
      known("JS_ARG_POISON");
      break;
    case JS_SERIALIZE_NO_NODE:
      known("JS_SERIALIZE_NO_NODE");
      break;
    case JS_IS_CONSTRUCTING:
      known("JS_IS_CONSTRUCTING");
      break;
    case JS_HASH_KEY_EMPTY:
      known("JS_HASH_KEY_EMPTY");
      break;
    case JS_ION_ERROR:
      known("JS_ION_ERROR");
      break;
    case JS_ION_BAILOUT:
      known("JS_ION_BAILOUT");
      break;
    case JS_OPTIMIZED_OUT:
      known("JS_OPTIMIZED_OUT");
      break;
    case JS_UNINITIALIZED_LEXICAL:
      known("JS_UNINITIALIZED_LEXICAL");
      break;
    case JS_MISSING_ARGUMENTS:
      known("JS_MISSING_ARGUMENTS");
      break;
    case JS_GENERIC_MAGIC:
      known("JS_GENERIC_MAGIC");
      break;
    case JS_ERROR_WITHOUT_CAUSE:
      known("JS_ERROR_WITHOUT_CAUSE");
      break;
    default:
      unknown(int(why));
      break;
  }
}

void JS::Value::dumpFields(js::JSONPrinter& json) const {
  if (isDouble()) {
    json.property("type", "double");
    double d = toDouble();

    if (mozilla::IsNegativeZero(d)) {
      // Negative zero needs special handling.
      json.property("value", "-0");
    } else {
      // Infinity, -Infinity, NaN are handled by JS::NumberToString.
      char buf[JS::MaximumNumberToStringLength];
      JS::NumberToString(d, buf);
      json.property("value", buf);
    }

    json.formatProperty("private", "0x%p", toPrivateUnchecked());
  } else {
    auto tag = toTag();
    switch (tag) {
      case JSVAL_TAG_INT32:
        json.property("type", "int32");
        json.property("value", toInt32());
        break;
      case JSVAL_TAG_UNDEFINED:
        json.property("type", "undefined");
        break;
      case JSVAL_TAG_NULL:
        json.property("type", "null");
        break;
      case JSVAL_TAG_BOOLEAN:
        json.property("type", "boolean");
        json.boolProperty("value", toBoolean());
        break;
      case JSVAL_TAG_MAGIC:
        json.property("type", "magic");
        WhyMagicToString(
            whyMagic(), [&](const char* name) { json.property("value", name); },
            [&](int value) {
              json.formatProperty("value", "Unknown(%d)", value);
            });
        break;
      case JSVAL_TAG_STRING:
        json.property("type", "string");
        toString()->dumpFields(json);
        break;
      case JSVAL_TAG_SYMBOL:
        json.property("type", "symbol");
        toSymbol()->dumpFields(json);
        break;
      case JSVAL_TAG_PRIVATE_GCTHING:
        json.property("type", "private GCThing");
        json.formatProperty("address", "0x%p", toGCThing());
        break;
      case JSVAL_TAG_BIGINT:
        json.property("type", "bigint");
        toBigInt()->dumpFields(json);
        break;
#  ifdef ENABLE_RECORD_TUPLE
      case JSVAL_TAG_EXTENDED_PRIMITIVE: {
        json.property("type", "extended primitive");
        JSObject* obj = &toExtendedPrimitive();
        json.property("class", obj->getClass()->name);
        json.formatProperty("address", "(JSObject*)0x%p", obj);
        break;
      }
#  endif
      case JSVAL_TAG_OBJECT:
        json.property("type", "object");
        toObject().dumpFields(json);
        break;
      default:
        json.formatProperty("type", "unknown tag(%08x)", tag);
        break;
    }
  }
}

void JS::Value::dumpStringContent(js::GenericPrinter& out) const {
  if (isDouble()) {
    double d = toDouble();

    if (mozilla::IsNegativeZero(d)) {
      // Negative zero needs special handling.
      out.put("-0");
    } else {
      // Infinity, -Infinity, NaN are handled by JS::NumberToString.
      char buf[JS::MaximumNumberToStringLength];
      JS::NumberToString(d, buf);
      out.put(buf);
    }

    out.printf(" / <private @ 0x%p>", toPrivateUnchecked());
  } else {
    auto tag = toTag();
    switch (tag) {
      case JSVAL_TAG_INT32:
        out.printf("%d", toInt32());
        break;
      case JSVAL_TAG_UNDEFINED:
        out.put("undefined");
        break;
      case JSVAL_TAG_NULL:
        out.put("null");
        break;
      case JSVAL_TAG_BOOLEAN:
        if (toBoolean()) {
          out.put("true");
        } else {
          out.put("false");
        }
        break;
      case JSVAL_TAG_MAGIC:
        out.put("<magic ");
        WhyMagicToString(
            whyMagic(), [&](const char* name) { out.put(name); },
            [&](int value) { out.printf("Unknown(%d)", value); });
        out.put(">");
        break;
      case JSVAL_TAG_STRING:
        toString()->dumpStringContent(out);
        break;
      case JSVAL_TAG_SYMBOL:
        toSymbol()->dumpStringContent(out);
        break;
      case JSVAL_TAG_PRIVATE_GCTHING:
        out.printf("<private GCThing @ 0x%p>", toGCThing());
        break;
      case JSVAL_TAG_BIGINT:
        toBigInt()->dumpStringContent(out);
        break;
#  ifdef ENABLE_RECORD_TUPLE
      case JSVAL_TAG_EXTENDED_PRIMITIVE:
        toExtendedPrimitive().dumpStringContent(out);
        break;
#  endif
      case JSVAL_TAG_OBJECT:
        toObject().dumpStringContent(out);
        break;
      default:
        out.printf("Unknown(%08x)", tag);
        break;
    }
  }
}
#endif
