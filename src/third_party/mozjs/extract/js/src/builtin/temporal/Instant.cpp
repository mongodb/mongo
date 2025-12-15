/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/Instant.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/Span.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <stddef.h>
#include <stdint.h>
#include <utility>

#include "jsnum.h"
#include "jspubtd.h"
#include "NamespaceImports.h"

#include "builtin/intl/DateTimeFormat.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Int128.h"
#include "builtin/temporal/Int96.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ToString.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/BigIntType.h"
#include "vm/BytecodeUtil.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

static inline bool IsInstant(Handle<Value> v) {
  return v.isObject() && v.toObject().is<InstantObject>();
}

/**
 * Check if the absolute value is less-or-equal to the given limit.
 */
template <const auto& digits>
static bool AbsoluteValueIsLessOrEqual(const BigInt* bigInt) {
  size_t length = bigInt->digitLength();

  // Fewer digits than the limit, so definitely in range.
  if (length < std::size(digits)) {
    return true;
  }

  // More digits than the limit, so definitely out of range.
  if (length > std::size(digits)) {
    return false;
  }

  // Compare each digit when the input has the same number of digits.
  size_t index = std::size(digits);
  for (auto digit : digits) {
    auto d = bigInt->digit(--index);
    if (d < digit) {
      return true;
    }
    if (d > digit) {
      return false;
    }
  }
  return true;
}

static constexpr auto NanosecondsMaxInstant() {
  static_assert(BigInt::DigitBits == 64 || BigInt::DigitBits == 32);

  // ±8.64 × 10^21 is the nanoseconds from epoch limit.
  // 8.64 × 10^21 is 86_40000_00000_00000_00000 or 0x1d4_60162f51_6f000000.
  // Return the BigInt digits of that number for fast BigInt comparisons.
  if constexpr (BigInt::DigitBits == 64) {
    return std::array{
        BigInt::Digit(0x1d4),
        BigInt::Digit(0x6016'2f51'6f00'0000),
    };
  } else {
    return std::array{
        BigInt::Digit(0x1d4),
        BigInt::Digit(0x6016'2f51),
        BigInt::Digit(0x6f00'0000),
    };
  }
}

// Can't be defined in IsValidEpochNanoseconds when compiling with GCC 8.
static constexpr auto EpochLimitBigIntDigits = NanosecondsMaxInstant();

/**
 * IsValidEpochNanoseconds ( epochNanoseconds )
 */
bool js::temporal::IsValidEpochNanoseconds(const BigInt* epochNanoseconds) {
  // Steps 1-2.
  return AbsoluteValueIsLessOrEqual<EpochLimitBigIntDigits>(epochNanoseconds);
}

static bool IsValidEpochMilliseconds(double epochMilliseconds) {
  MOZ_ASSERT(IsInteger(epochMilliseconds));

  constexpr int64_t MillisecondsMaxInstant =
      EpochNanoseconds::max().toMilliseconds();
  return std::abs(epochMilliseconds) <= double(MillisecondsMaxInstant);
}

/**
 * IsValidEpochNanoseconds ( epochNanoseconds )
 */
bool js::temporal::IsValidEpochNanoseconds(
    const EpochNanoseconds& epochNanoseconds) {
  MOZ_ASSERT(0 <= epochNanoseconds.nanoseconds &&
             epochNanoseconds.nanoseconds <= 999'999'999);

  // Steps 1-2.
  return EpochNanoseconds::min() <= epochNanoseconds &&
         epochNanoseconds <= EpochNanoseconds::max();
}

#ifdef DEBUG
/**
 * Validates a nanoseconds amount is at most as large as the difference
 * between two valid epoch nanoseconds.
 */
bool js::temporal::IsValidEpochDuration(const EpochDuration& duration) {
  MOZ_ASSERT(0 <= duration.nanoseconds && duration.nanoseconds <= 999'999'999);

  // Steps 1-2.
  return EpochDuration::min() <= duration && duration <= EpochDuration::max();
}
#endif

/**
 * Return the BigInt as a 96-bit integer. The BigInt digits must not consist of
 * more than 96-bits.
 */
static Int96 ToInt96(const BigInt* ns) {
  static_assert(BigInt::DigitBits == 64 || BigInt::DigitBits == 32);

  auto digits = ns->digits();
  if constexpr (BigInt::DigitBits == 64) {
    BigInt::Digit x = 0, y = 0;
    switch (digits.size()) {
      case 2:
        y = digits[1];
        [[fallthrough]];
      case 1:
        x = digits[0];
        [[fallthrough]];
      case 0:
        break;
      default:
        MOZ_ASSERT_UNREACHABLE("unexpected digit length");
    }
    return Int96{
        Int96::Digits{Int96::Digit(x), Int96::Digit(x >> 32), Int96::Digit(y)},
        ns->isNegative()};
  } else {
    BigInt::Digit x = 0, y = 0, z = 0;
    switch (digits.size()) {
      case 3:
        z = digits[2];
        [[fallthrough]];
      case 2:
        y = digits[1];
        [[fallthrough]];
      case 1:
        x = digits[0];
        [[fallthrough]];
      case 0:
        break;
      default:
        MOZ_ASSERT_UNREACHABLE("unexpected digit length");
    }
    return Int96{
        Int96::Digits{Int96::Digit(x), Int96::Digit(y), Int96::Digit(z)},
        ns->isNegative()};
  }
}

EpochNanoseconds js::temporal::ToEpochNanoseconds(
    const BigInt* epochNanoseconds) {
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  auto [seconds, nanos] =
      ToInt96(epochNanoseconds) / ToNanoseconds(TemporalUnit::Second);
  return {{seconds, nanos}};
}

static BigInt* CreateBigInt(JSContext* cx,
                            const std::array<uint32_t, 3>& digits,
                            bool negative) {
  static_assert(BigInt::DigitBits == 64 || BigInt::DigitBits == 32);

  if constexpr (BigInt::DigitBits == 64) {
    uint64_t x = (uint64_t(digits[1]) << 32) | digits[0];
    uint64_t y = digits[2];

    size_t length = y ? 2 : x ? 1 : 0;
    auto* result = BigInt::createUninitialized(cx, length, negative);
    if (!result) {
      return nullptr;
    }
    if (y) {
      result->setDigit(1, y);
    }
    if (x) {
      result->setDigit(0, x);
    }
    return result;
  } else {
    size_t length = digits[2] ? 3 : digits[1] ? 2 : digits[0] ? 1 : 0;
    auto* result = BigInt::createUninitialized(cx, length, negative);
    if (!result) {
      return nullptr;
    }
    while (length--) {
      result->setDigit(length, digits[length]);
    }
    return result;
  }
}

static auto ToBigIntDigits(uint64_t seconds, uint32_t nanoseconds) {
  // Multiplies two uint32_t values and returns the lower 32-bits. The higher
  // 32-bits are stored in |high|.
  auto digitMul = [](uint32_t a, uint32_t b, uint32_t* high) {
    uint64_t result = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
    *high = result >> 32;
    return static_cast<uint32_t>(result);
  };

  // Adds two uint32_t values and returns the result. Overflow is added to the
  // out-param |carry|.
  auto digitAdd = [](uint32_t a, uint32_t b, uint32_t* carry) {
    uint32_t result = a + b;
    *carry += static_cast<uint32_t>(result < a);
    return result;
  };

  constexpr uint32_t secToNanos = ToNanoseconds(TemporalUnit::Second);

  // uint32_t digits stored in the same order as BigInt digits, i.e. the least
  // significant digit is stored at index zero.
  std::array<uint32_t, 2> multiplicand = {uint32_t(seconds),
                                          uint32_t(seconds >> 32)};
  std::array<uint32_t, 3> accumulator = {nanoseconds, 0, 0};

  // This code follows the implementation of |BigInt::multiplyAccumulate()|.

  uint32_t carry = 0;
  {
    uint32_t high = 0;
    uint32_t low = digitMul(secToNanos, multiplicand[0], &high);

    uint32_t newCarry = 0;
    accumulator[0] = digitAdd(accumulator[0], low, &newCarry);
    accumulator[1] = digitAdd(high, newCarry, &carry);
  }
  {
    uint32_t high = 0;
    uint32_t low = digitMul(secToNanos, multiplicand[1], &high);

    uint32_t newCarry = 0;
    accumulator[1] = digitAdd(accumulator[1], low, &carry);
    accumulator[2] = digitAdd(high, carry, &newCarry);
    MOZ_ASSERT(newCarry == 0);
  }

  return accumulator;
}

BigInt* js::temporal::ToBigInt(JSContext* cx,
                               const EpochNanoseconds& epochNanoseconds) {
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  auto [seconds, nanoseconds] = epochNanoseconds.abs();
  auto digits = ToBigIntDigits(uint64_t(seconds), uint32_t(nanoseconds));
  return CreateBigInt(cx, digits, epochNanoseconds.seconds < 0);
}

/**
 * GetUTCEpochNanoseconds ( isoDateTime )
 */
EpochNanoseconds js::temporal::GetUTCEpochNanoseconds(
    const ISODateTime& isoDateTime) {
  MOZ_ASSERT(ISODateTimeWithinLimits(isoDateTime));

  const auto& [date, time] = isoDateTime;

  // Steps 1-4.
  int64_t ms = MakeDate(isoDateTime);

  // Propagate the input range to the compiler.
  int32_t nanos =
      std::clamp(time.microsecond * 1'000 + time.nanosecond, 0, 999'999);

  // Step 5.
  //
  // The returned epoch nanoseconds value can exceed ±8.64 × 10^21, because it
  // includes the local time zone offset.
  return EpochNanoseconds::fromMilliseconds(ms) + EpochDuration{{0, nanos}};
}

/**
 * CompareEpochNanoseconds ( epochNanosecondsOne, epochNanosecondsTwo )
 */
static int32_t CompareEpochNanoseconds(
    const EpochNanoseconds& epochNanosecondsOne,
    const EpochNanoseconds& epochNanosecondsTwo) {
  // Step 1.
  if (epochNanosecondsOne > epochNanosecondsTwo) {
    return 1;
  }

  // Step 2.
  if (epochNanosecondsOne < epochNanosecondsTwo) {
    return -1;
  }

  // Step 3.
  return 0;
}

/**
 * CreateTemporalInstant ( epochNanoseconds [ , newTarget ] )
 */
InstantObject* js::temporal::CreateTemporalInstant(
    JSContext* cx, const EpochNanoseconds& epochNanoseconds) {
  // Step 1.
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<InstantObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  object->setFixedSlot(InstantObject::SECONDS_SLOT,
                       NumberValue(epochNanoseconds.seconds));
  object->setFixedSlot(InstantObject::NANOSECONDS_SLOT,
                       Int32Value(epochNanoseconds.nanoseconds));

  // Step 5.
  return object;
}

/**
 * CreateTemporalInstant ( epochNanoseconds [ , newTarget ] )
 */
static InstantObject* CreateTemporalInstant(JSContext* cx, const CallArgs& args,
                                            Handle<BigInt*> epochNanoseconds) {
  // Step 1.
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  // Steps 2-3.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Instant, &proto)) {
    return nullptr;
  }

  auto* object = NewObjectWithClassProto<InstantObject>(cx, proto);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  auto epochNs = ToEpochNanoseconds(epochNanoseconds);
  object->setFixedSlot(InstantObject::SECONDS_SLOT,
                       NumberValue(epochNs.seconds));
  object->setFixedSlot(InstantObject::NANOSECONDS_SLOT,
                       Int32Value(epochNs.nanoseconds));

  // Step 5.
  return object;
}

/**
 * ToTemporalInstant ( item )
 */
static bool ToTemporalInstant(JSContext* cx, Handle<Value> item,
                              EpochNanoseconds* result) {
  // Step 1.
  Rooted<Value> primitiveValue(cx, item);
  if (item.isObject()) {
    JSObject* itemObj = &item.toObject();

    // Step 1.a.
    if (auto* instant = itemObj->maybeUnwrapIf<InstantObject>()) {
      *result = instant->epochNanoseconds();
      return true;
    }
    if (auto* zonedDateTime = itemObj->maybeUnwrapIf<ZonedDateTimeObject>()) {
      *result = zonedDateTime->epochNanoseconds();
      return true;
    }

    // Steps 1.b-c.
    if (!ToPrimitive(cx, JSTYPE_STRING, &primitiveValue)) {
      return false;
    }
  }

  // Step 2.
  if (!primitiveValue.isString()) {
    // The value is always on the stack, so JSDVG_SEARCH_STACK can be used for
    // better error reporting.
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK,
                     primitiveValue, nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> string(cx, primitiveValue.toString());

  // Steps 3-7.
  ISODateTime dateTime;
  int64_t offset;
  if (!ParseTemporalInstantString(cx, string, &dateTime, &offset)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offset) < ToNanoseconds(TemporalUnit::Day));

  // Steps 8-9.
  //
  // Modified to call ISODateTimeWithinLimits instead of BalanceISODateTime.
  if (!ISODateTimeWithinLimits(dateTime)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 10.
  auto epochNanoseconds =
      GetUTCEpochNanoseconds(dateTime) - EpochDuration::fromNanoseconds(offset);

  // Step 11.
  if (!IsValidEpochNanoseconds(epochNanoseconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 12.
  *result = epochNanoseconds;
  return true;
}

/**
 * AddInstant ( epochNanoseconds, hours, minutes, seconds, milliseconds,
 * microseconds, nanoseconds )
 */
bool js::temporal::AddInstant(JSContext* cx,
                              const EpochNanoseconds& epochNanoseconds,
                              const TimeDuration& duration,
                              EpochNanoseconds* result) {
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));
  MOZ_ASSERT(IsValidTimeDuration(duration));

  // Step 1. (Inlined AddTimeDurationToEpochNanoseconds)
  auto r = epochNanoseconds + duration.to<EpochDuration>();

  // Step 2.
  if (!IsValidEpochNanoseconds(r)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 3.
  *result = r;
  return true;
}

/**
 * DifferenceInstant ( ns1, ns2, roundingIncrement, smallestUnit, roundingMode )
 */
TimeDuration js::temporal::DifferenceInstant(
    const EpochNanoseconds& ns1, const EpochNanoseconds& ns2,
    Increment roundingIncrement, TemporalUnit smallestUnit,
    TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidEpochNanoseconds(ns1));
  MOZ_ASSERT(IsValidEpochNanoseconds(ns2));
  MOZ_ASSERT(smallestUnit > TemporalUnit::Day);
  MOZ_ASSERT(roundingIncrement <=
             MaximumTemporalDurationRoundingIncrement(smallestUnit));

  // Step 1.
  auto diff = TimeDurationFromEpochNanosecondsDifference(ns2, ns1);
  MOZ_ASSERT(IsValidEpochDuration(diff.to<EpochDuration>()));

  // Steps 2-3.
  return RoundTimeDuration(diff, roundingIncrement, smallestUnit, roundingMode);
}

/**
 * RoundNumberToIncrementAsIfPositive ( x, increment, roundingMode )
 */
static EpochNanoseconds RoundNumberToIncrementAsIfPositive(
    const EpochNanoseconds& x, int64_t increment,
    TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidEpochNanoseconds(x));
  MOZ_ASSERT(increment > 0);
  MOZ_ASSERT(increment <= ToNanoseconds(TemporalUnit::Day));

  // This operation is equivalent to adjusting the rounding mode through
  // |ToPositiveRoundingMode| and then calling |RoundNumberToIncrement|.
  auto rounded = RoundNumberToIncrement(x.toNanoseconds(), Int128{increment},
                                        ToPositiveRoundingMode(roundingMode));
  return EpochNanoseconds::fromNanoseconds(rounded);
}

/**
 * RoundTemporalInstant ( ns, increment, unit, roundingMode )
 */
EpochNanoseconds js::temporal::RoundTemporalInstant(
    const EpochNanoseconds& ns, Increment increment, TemporalUnit unit,
    TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidEpochNanoseconds(ns));
  MOZ_ASSERT(increment >= Increment::min());
  MOZ_ASSERT(uint64_t(increment.value()) <= ToNanoseconds(TemporalUnit::Day));
  MOZ_ASSERT(unit > TemporalUnit::Day);

  // Step 1.
  int64_t unitLength = ToNanoseconds(unit);

  // Step 2.
  int64_t incrementNs = increment.value() * unitLength;
  MOZ_ASSERT(incrementNs <= ToNanoseconds(TemporalUnit::Day),
             "incrementNs doesn't overflow epoch nanoseconds resolution");

  // Step 3.
  return RoundNumberToIncrementAsIfPositive(ns, incrementNs, roundingMode);
}

/**
 * DifferenceTemporalInstant ( operation, instant, other, options )
 */
static bool DifferenceTemporalInstant(JSContext* cx,
                                      TemporalDifference operation,
                                      const CallArgs& args) {
  auto epochNs = args.thisv().toObject().as<InstantObject>().epochNanoseconds();

  // Step 1.
  EpochNanoseconds other;
  if (!ToTemporalInstant(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 2-3.
  DifferenceSettings settings;
  if (args.hasDefined(1)) {
    // Step 2.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 3.
    if (!GetDifferenceSettings(cx, operation, options, TemporalUnitGroup::Time,
                               TemporalUnit::Nanosecond, TemporalUnit::Second,
                               &settings)) {
      return false;
    }
  } else {
    // Steps 2-3.
    settings = {
        TemporalUnit::Nanosecond,
        TemporalUnit::Second,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Steps 4.
  auto timeDuration =
      DifferenceInstant(epochNs, other, settings.roundingIncrement,
                        settings.smallestUnit, settings.roundingMode);

  // Step 5.
  Duration duration;
  if (!TemporalDurationFromInternal(cx, timeDuration, settings.largestUnit,
                                    &duration)) {
    return false;
  }

  // Step 6.
  if (operation == TemporalDifference::Since) {
    duration = duration.negate();
  }

  // Step 7.
  auto* obj = CreateTemporalDuration(cx, duration);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * AddDurationToInstant ( operation, instant, temporalDurationLike )
 */
static bool AddDurationToInstant(JSContext* cx, TemporalAddDuration operation,
                                 const CallArgs& args) {
  auto* instant = &args.thisv().toObject().as<InstantObject>();
  auto epochNanoseconds = instant->epochNanoseconds();

  // Step 1.
  Duration duration;
  if (!ToTemporalDuration(cx, args.get(0), &duration)) {
    return false;
  }

  // Step 2.
  if (operation == TemporalAddDuration::Subtract) {
    duration = duration.negate();
  }

  // Steps 3-4. (Inlined DefaultTemporalLargestUnit and TemporalUnitCategory.)
  if (duration.years != 0 || duration.months != 0 || duration.weeks != 0 ||
      duration.days != 0) {
    const char* part = duration.years != 0    ? "years"
                       : duration.months != 0 ? "months"
                       : duration.weeks != 0  ? "weeks"
                                              : "days";
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_BAD_DURATION, part);
    return false;
  }

  // Step 5. (Inlined ToInternalDurationRecordWith24HourDays.)
  auto timeDuration = TimeDurationFromComponents(duration);

  // Step 6.
  EpochNanoseconds ns;
  if (!AddInstant(cx, epochNanoseconds, timeDuration, &ns)) {
    return false;
  }

  // Step 7.
  auto* result = CreateTemporalInstant(cx, ns);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant ( epochNanoseconds )
 */
static bool InstantConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.Instant")) {
    return false;
  }

  // Step 2.
  Rooted<BigInt*> epochNanoseconds(cx, js::ToBigInt(cx, args.get(0)));
  if (!epochNanoseconds) {
    return false;
  }

  // Step 3.
  if (!IsValidEpochNanoseconds(epochNanoseconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 4.
  auto* result = CreateTemporalInstant(cx, args, epochNanoseconds);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.from ( item )
 */
static bool Instant_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  EpochNanoseconds epochNs;
  if (!ToTemporalInstant(cx, args.get(0), &epochNs)) {
    return false;
  }

  auto* result = CreateTemporalInstant(cx, epochNs);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.fromEpochMilliseconds ( epochMilliseconds )
 */
static bool Instant_fromEpochMilliseconds(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  double epochMilliseconds;
  if (!JS::ToNumber(cx, args.get(0), &epochMilliseconds)) {
    return false;
  }

  // Step 2.
  //
  // NumberToBigInt throws a RangeError for non-integral numbers.
  if (!IsInteger(epochMilliseconds)) {
    ToCStringBuf cbuf;
    const char* str = NumberToCString(&cbuf, epochMilliseconds);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_NONINTEGER, str);
    return false;
  }

  // Step 3. (Not applicable)

  // Step 4.
  if (!IsValidEpochMilliseconds(epochMilliseconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 5.
  int64_t milliseconds = mozilla::AssertedCast<int64_t>(epochMilliseconds);
  auto* result = CreateTemporalInstant(
      cx, EpochNanoseconds::fromMilliseconds(milliseconds));
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.fromEpochNanoseconds ( epochNanoseconds )
 */
static bool Instant_fromEpochNanoseconds(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<BigInt*> epochNanoseconds(cx, js::ToBigInt(cx, args.get(0)));
  if (!epochNanoseconds) {
    return false;
  }

  // Step 2.
  if (!IsValidEpochNanoseconds(epochNanoseconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 3.
  auto* result =
      CreateTemporalInstant(cx, ToEpochNanoseconds(epochNanoseconds));
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.compare ( one, two )
 */
static bool Instant_compare(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  EpochNanoseconds one;
  if (!ToTemporalInstant(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  EpochNanoseconds two;
  if (!ToTemporalInstant(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  args.rval().setInt32(CompareEpochNanoseconds(one, two));
  return true;
}

/**
 * get Temporal.Instant.prototype.epochMilliseconds
 */
static bool Instant_epochMilliseconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto epochNs = args.thisv().toObject().as<InstantObject>().epochNanoseconds();

  // Step 4-5.
  args.rval().setNumber(epochNs.floorToMilliseconds());
  return true;
}

/**
 * get Temporal.Instant.prototype.epochMilliseconds
 */
static bool Instant_epochMilliseconds(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_epochMilliseconds>(cx, args);
}

/**
 * get Temporal.Instant.prototype.epochNanoseconds
 */
static bool Instant_epochNanoseconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto epochNs = args.thisv().toObject().as<InstantObject>().epochNanoseconds();
  auto* nanoseconds = ToBigInt(cx, epochNs);
  if (!nanoseconds) {
    return false;
  }

  args.rval().setBigInt(nanoseconds);
  return true;
}

/**
 * get Temporal.Instant.prototype.epochNanoseconds
 */
static bool Instant_epochNanoseconds(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_epochNanoseconds>(cx, args);
}

/**
 * Temporal.Instant.prototype.add ( temporalDurationLike )
 */
static bool Instant_add(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToInstant(cx, TemporalAddDuration::Add, args);
}

/**
 * Temporal.Instant.prototype.add ( temporalDurationLike )
 */
static bool Instant_add(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_add>(cx, args);
}

/**
 * Temporal.Instant.prototype.subtract ( temporalDurationLike )
 */
static bool Instant_subtract(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return AddDurationToInstant(cx, TemporalAddDuration::Subtract, args);
}

/**
 * Temporal.Instant.prototype.subtract ( temporalDurationLike )
 */
static bool Instant_subtract(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_subtract>(cx, args);
}

/**
 * Temporal.Instant.prototype.until ( other [ , options ] )
 */
static bool Instant_until(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalInstant(cx, TemporalDifference::Until, args);
}

/**
 * Temporal.Instant.prototype.until ( other [ , options ] )
 */
static bool Instant_until(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_until>(cx, args);
}

/**
 * Temporal.Instant.prototype.since ( other [ , options ] )
 */
static bool Instant_since(JSContext* cx, const CallArgs& args) {
  // Step 3.
  return DifferenceTemporalInstant(cx, TemporalDifference::Since, args);
}

/**
 * Temporal.Instant.prototype.since ( other [ , options ] )
 */
static bool Instant_since(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_since>(cx, args);
}

/**
 * Temporal.Instant.prototype.round ( roundTo )
 */
static bool Instant_round(JSContext* cx, const CallArgs& args) {
  auto epochNs = args.thisv().toObject().as<InstantObject>().epochNanoseconds();

  // Steps 3-16.
  auto smallestUnit = TemporalUnit::Auto;
  auto roundingMode = TemporalRoundingMode::HalfExpand;
  auto roundingIncrement = Increment{1};
  if (args.get(0).isString()) {
    // Steps 4 and 6-8. (Not applicable in our implementation.)

    // Step 9.
    Rooted<JSString*> paramString(cx, args[0].toString());
    if (!GetTemporalUnitValuedOption(cx, paramString,
                                     TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }

    // Steps 10-16. (Not applicable in our implementation.)
  } else {
    // Steps 3 and 5.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "roundTo", "round", args.get(0)));
    if (!options) {
      return false;
    }

    // Steps 6-7.
    if (!GetRoundingIncrementOption(cx, options, &roundingIncrement)) {
      return false;
    }

    // Step 8.
    if (!GetRoundingModeOption(cx, options, &roundingMode)) {
      return false;
    }

    // Step 9.
    if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }
    if (smallestUnit == TemporalUnit::Auto) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_MISSING_OPTION, "smallestUnit");
      return false;
    }

    // Steps 10-15.
    int64_t maximum = UnitsPerDay(smallestUnit);

    // Step 16.
    if (!ValidateTemporalRoundingIncrement(cx, roundingIncrement, maximum,
                                           true)) {
      return false;
    }
  }

  // Step 17.
  auto roundedNs = RoundTemporalInstant(epochNs, roundingIncrement,
                                        smallestUnit, roundingMode);

  // Step 18.
  auto* result = CreateTemporalInstant(cx, roundedNs);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.prototype.round ( options )
 */
static bool Instant_round(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_round>(cx, args);
}

/**
 * Temporal.Instant.prototype.equals ( other )
 */
static bool Instant_equals(JSContext* cx, const CallArgs& args) {
  auto epochNs = args.thisv().toObject().as<InstantObject>().epochNanoseconds();

  // Step 3.
  EpochNanoseconds other;
  if (!ToTemporalInstant(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-5.
  args.rval().setBoolean(epochNs == other);
  return true;
}

/**
 * Temporal.Instant.prototype.equals ( other )
 */
static bool Instant_equals(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_equals>(cx, args);
}

/**
 * Temporal.Instant.prototype.toString ( [ options ] )
 */
static bool Instant_toString(JSContext* cx, const CallArgs& args) {
  auto epochNs = args.thisv().toObject().as<InstantObject>().epochNanoseconds();

  Rooted<TimeZoneValue> timeZone(cx);
  auto roundingMode = TemporalRoundingMode::Trunc;
  SecondsStringPrecision precision = {Precision::Auto(),
                                      TemporalUnit::Nanosecond, Increment{1}};
  if (args.hasDefined(0)) {
    // Step 3.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "toString", args[0]));
    if (!options) {
      return false;
    }

    // Steps 4-5.
    auto digits = Precision::Auto();
    if (!GetTemporalFractionalSecondDigitsOption(cx, options, &digits)) {
      return false;
    }

    // Step 6.
    if (!GetRoundingModeOption(cx, options, &roundingMode)) {
      return false;
    }

    // Step 7.
    auto smallestUnit = TemporalUnit::Auto;
    if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::SmallestUnit,
                                     TemporalUnitGroup::Time, &smallestUnit)) {
      return false;
    }

    // Step 8.
    if (smallestUnit == TemporalUnit::Hour) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_UNIT_OPTION, "hour",
                                "smallestUnit");
      return false;
    }

    // Step 9.
    Rooted<Value> value(cx);
    if (!GetProperty(cx, options, options, cx->names().timeZone, &value)) {
      return false;
    }

    // Step 10.
    if (!value.isUndefined()) {
      if (!ToTemporalTimeZone(cx, value, &timeZone)) {
        return false;
      }
    }

    // Step 11.
    precision = ToSecondsStringPrecision(smallestUnit, digits);
  }

  // Steps 12-13.
  auto roundedNs = RoundTemporalInstant(epochNs, precision.increment,
                                        precision.unit, roundingMode);

  // Step 14.
  JSString* str =
      TemporalInstantToString(cx, roundedNs, timeZone, precision.precision);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.Instant.prototype.toString ( [ options ] )
 */
static bool Instant_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_toString>(cx, args);
}

/**
 * Temporal.Instant.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool Instant_toLocaleString(JSContext* cx, const CallArgs& args) {
  // Steps 3-4.
  Handle<PropertyName*> required = cx->names().any;
  Handle<PropertyName*> defaults = cx->names().all;
  return TemporalObjectToLocaleString(cx, args, required, defaults);
}

/**
 * Temporal.Instant.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool Instant_toLocaleString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_toLocaleString>(cx, args);
}

/**
 * Temporal.Instant.prototype.toJSON ( )
 */
static bool Instant_toJSON(JSContext* cx, const CallArgs& args) {
  auto epochNs = args.thisv().toObject().as<InstantObject>().epochNanoseconds();

  // Step 3.
  Rooted<TimeZoneValue> timeZone(cx);
  JSString* str =
      TemporalInstantToString(cx, epochNs, timeZone, Precision::Auto());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.Instant.prototype.toJSON ( )
 */
static bool Instant_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_toJSON>(cx, args);
}

/**
 * Temporal.Instant.prototype.valueOf ( )
 */
static bool Instant_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "Instant", "primitive type");
  return false;
}

/**
 * Temporal.Instant.prototype.toZonedDateTimeISO ( item )
 */
static bool Instant_toZonedDateTimeISO(JSContext* cx, const CallArgs& args) {
  auto epochNs = args.thisv().toObject().as<InstantObject>().epochNanoseconds();

  // Step 3.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  // Step 4.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  auto* result = CreateTemporalZonedDateTime(cx, epochNs, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.prototype.toZonedDateTimeISO ( item )
 */
static bool Instant_toZonedDateTimeISO(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_toZonedDateTimeISO>(cx, args);
}

const JSClass InstantObject::class_ = {
    "Temporal.Instant",
    JSCLASS_HAS_RESERVED_SLOTS(InstantObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Instant),
    JS_NULL_CLASS_OPS,
    &InstantObject::classSpec_,
};

const JSClass& InstantObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec Instant_methods[] = {
    JS_FN("from", Instant_from, 1, 0),
    JS_FN("fromEpochMilliseconds", Instant_fromEpochMilliseconds, 1, 0),
    JS_FN("fromEpochNanoseconds", Instant_fromEpochNanoseconds, 1, 0),
    JS_FN("compare", Instant_compare, 2, 0),
    JS_FS_END,
};

static const JSFunctionSpec Instant_prototype_methods[] = {
    JS_FN("add", Instant_add, 1, 0),
    JS_FN("subtract", Instant_subtract, 1, 0),
    JS_FN("until", Instant_until, 1, 0),
    JS_FN("since", Instant_since, 1, 0),
    JS_FN("round", Instant_round, 1, 0),
    JS_FN("equals", Instant_equals, 1, 0),
    JS_FN("toString", Instant_toString, 0, 0),
    JS_FN("toLocaleString", Instant_toLocaleString, 0, 0),
    JS_FN("toJSON", Instant_toJSON, 0, 0),
    JS_FN("valueOf", Instant_valueOf, 0, 0),
    JS_FN("toZonedDateTimeISO", Instant_toZonedDateTimeISO, 1, 0),
    JS_FS_END,
};

static const JSPropertySpec Instant_prototype_properties[] = {
    JS_PSG("epochMilliseconds", Instant_epochMilliseconds, 0),
    JS_PSG("epochNanoseconds", Instant_epochNanoseconds, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.Instant", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec InstantObject::classSpec_ = {
    GenericCreateConstructor<InstantConstructor, 1, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<InstantObject>,
    Instant_methods,
    nullptr,
    Instant_prototype_methods,
    Instant_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};
