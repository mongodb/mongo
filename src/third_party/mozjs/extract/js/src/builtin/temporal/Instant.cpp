/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/Instant.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
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

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Int96.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ToString.h"
#include "builtin/temporal/Wrapped.h"
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
#include "vm/StringType.h"

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
  // Steps 1-3.
  return AbsoluteValueIsLessOrEqual<EpochLimitBigIntDigits>(epochNanoseconds);
}

static bool IsValidEpochMicroseconds(const BigInt* epochMicroseconds) {
  int64_t i;
  if (!BigInt::isInt64(epochMicroseconds, &i)) {
    return false;
  }

  constexpr int64_t MicrosecondsMaxInstant = Instant::max().toMicroseconds();
  return -MicrosecondsMaxInstant <= i && i <= MicrosecondsMaxInstant;
}

static bool IsValidEpochMilliseconds(double epochMilliseconds) {
  MOZ_ASSERT(IsInteger(epochMilliseconds));

  constexpr int64_t MillisecondsMaxInstant = Instant::max().toMilliseconds();
  return std::abs(epochMilliseconds) <= double(MillisecondsMaxInstant);
}

static bool IsValidEpochSeconds(double epochSeconds) {
  MOZ_ASSERT(IsInteger(epochSeconds));

  constexpr int64_t SecondsMaxInstant = Instant::max().toSeconds();
  return std::abs(epochSeconds) <= double(SecondsMaxInstant);
}

/**
 * IsValidEpochNanoseconds ( epochNanoseconds )
 */
bool js::temporal::IsValidEpochInstant(const Instant& instant) {
  MOZ_ASSERT(0 <= instant.nanoseconds && instant.nanoseconds <= 999'999'999);

  // Steps 1-3.
  return Instant::min() <= instant && instant <= Instant::max();
}

#ifdef DEBUG
/**
 * Validates a nanoseconds amount is at most as large as the difference
 * between two valid nanoseconds from the epoch instants.
 */
bool js::temporal::IsValidInstantSpan(const InstantSpan& span) {
  MOZ_ASSERT(0 <= span.nanoseconds && span.nanoseconds <= 999'999'999);

  // Steps 1-3.
  return InstantSpan::min() <= span && span <= InstantSpan::max();
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

Instant js::temporal::ToInstant(const BigInt* epochNanoseconds) {
  MOZ_ASSERT(IsValidEpochNanoseconds(epochNanoseconds));

  auto [seconds, nanos] =
      ToInt96(epochNanoseconds) / ToNanoseconds(TemporalUnit::Second);
  return {seconds, nanos};
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

template <typename T>
static BigInt* ToBigInt(JSContext* cx,
                        const SecondsAndNanoseconds<T>& secondsAndNanoseconds) {
  uint64_t seconds = std::abs(secondsAndNanoseconds.seconds);
  uint32_t nanoseconds = secondsAndNanoseconds.nanoseconds;

  // Negative nanoseconds are represented as the difference to 1'000'000'000.
  // Convert these back to their absolute value and adjust the seconds part
  // accordingly.
  //
  // For example the nanoseconds from the epoch value |-1n| is represented as
  // the instant {seconds: -1, nanoseconds: 999'999'999}.
  if (secondsAndNanoseconds.seconds < 0 && nanoseconds != 0) {
    nanoseconds = ToNanoseconds(TemporalUnit::Second) - nanoseconds;
    seconds -= 1;
  }

  auto digits = ToBigIntDigits(seconds, nanoseconds);
  return CreateBigInt(cx, digits, secondsAndNanoseconds.seconds < 0);
}

BigInt* js::temporal::ToEpochNanoseconds(JSContext* cx,
                                         const Instant& instant) {
  MOZ_ASSERT(IsValidEpochInstant(instant));
  return ::ToBigInt(cx, instant);
}

/**
 * GetUTCEpochNanoseconds ( year, month, day, hour, minute, second, millisecond,
 * microsecond, nanosecond [ , offsetNanoseconds ] )
 */
Instant js::temporal::GetUTCEpochNanoseconds(const PlainDateTime& dateTime) {
  const auto& [date, time] = dateTime;

  // Step 1.
  MOZ_ASSERT(IsValidISODateTime(dateTime));

  // Additionally ensure the date-time value can be represented as an Instant.
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  // Steps 2-5.
  int64_t ms = MakeDate(dateTime);

  // Propagate the input range to the compiler.
  int32_t nanos =
      std::clamp(time.microsecond * 1'000 + time.nanosecond, 0, 999'999);

  // Steps 6-8.
  return Instant::fromMilliseconds(ms) + InstantSpan{0, nanos};
}

/**
 * GetUTCEpochNanoseconds ( year, month, day, hour, minute, second, millisecond,
 * microsecond, nanosecond [ , offsetNanoseconds ] )
 */
Instant js::temporal::GetUTCEpochNanoseconds(
    const PlainDateTime& dateTime, const InstantSpan& offsetNanoseconds) {
  MOZ_ASSERT(offsetNanoseconds.abs() <
             InstantSpan::fromNanoseconds(ToNanoseconds(TemporalUnit::Day)));

  // Steps 1-6.
  auto epochNanoseconds = GetUTCEpochNanoseconds(dateTime);

  // Steps 7-9.
  return epochNanoseconds - offsetNanoseconds;
}

/**
 * CompareEpochNanoseconds ( epochNanosecondsOne, epochNanosecondsTwo )
 */
static int32_t CompareEpochNanoseconds(const Instant& epochNanosecondsOne,
                                       const Instant& epochNanosecondsTwo) {
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
InstantObject* js::temporal::CreateTemporalInstant(JSContext* cx,
                                                   const Instant& instant) {
  // Step 1.
  MOZ_ASSERT(IsValidEpochInstant(instant));

  // Steps 2-3.
  auto* object = NewBuiltinClassInstance<InstantObject>(cx);
  if (!object) {
    return nullptr;
  }

  // Step 4.
  object->setFixedSlot(InstantObject::SECONDS_SLOT,
                       NumberValue(instant.seconds));
  object->setFixedSlot(InstantObject::NANOSECONDS_SLOT,
                       Int32Value(instant.nanoseconds));

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
  auto instant = ToInstant(epochNanoseconds);
  object->setFixedSlot(InstantObject::SECONDS_SLOT,
                       NumberValue(instant.seconds));
  object->setFixedSlot(InstantObject::NANOSECONDS_SLOT,
                       Int32Value(instant.nanoseconds));

  // Step 5.
  return object;
}

/**
 * ToTemporalInstant ( item )
 */
Wrapped<InstantObject*> js::temporal::ToTemporalInstant(JSContext* cx,
                                                        Handle<Value> item) {
  // Step 1.
  if (item.isObject()) {
    JSObject* itemObj = &item.toObject();

    // Step 1.a.
    if (itemObj->canUnwrapAs<InstantObject>()) {
      return itemObj;
    }
  }

  // Steps 1.b-d and 3-7
  Instant epochNanoseconds;
  if (!ToTemporalInstant(cx, item, &epochNanoseconds)) {
    return nullptr;
  }

  // Step 8.
  return CreateTemporalInstant(cx, epochNanoseconds);
}

/**
 * ToTemporalInstant ( item )
 */
bool js::temporal::ToTemporalInstant(JSContext* cx, Handle<Value> item,
                                     Instant* result) {
  // Step 1.
  Rooted<Value> primitiveValue(cx, item);
  if (item.isObject()) {
    JSObject* itemObj = &item.toObject();

    // Step 1.a.
    if (auto* instant = itemObj->maybeUnwrapIf<InstantObject>()) {
      *result = ToInstant(instant);
      return true;
    }

    // Step 1.b.
    if (auto* zonedDateTime = itemObj->maybeUnwrapIf<ZonedDateTimeObject>()) {
      *result = ToInstant(zonedDateTime);
      return true;
    }

    // Steps 1.c-d.
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

  // Steps 3-4.
  PlainDateTime dateTime;
  int64_t offset;
  if (!ParseTemporalInstantString(cx, string, &dateTime, &offset)) {
    return false;
  }
  MOZ_ASSERT(std::abs(offset) < ToNanoseconds(TemporalUnit::Day));

  // Steps 5-6. (Reordered)
  if (!ISODateTimeWithinLimits(dateTime)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 4.
  auto epochNanoseconds =
      GetUTCEpochNanoseconds(dateTime, InstantSpan::fromNanoseconds(offset));

  // Step 7.
  if (!IsValidEpochInstant(epochNanoseconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 8.
  *result = epochNanoseconds;
  return true;
}

/**
 * AddInstant ( epochNanoseconds, hours, minutes, seconds, milliseconds,
 * microseconds, nanoseconds )
 */
bool js::temporal::AddInstant(JSContext* cx, const Instant& instant,
                              const NormalizedTimeDuration& duration,
                              Instant* result) {
  MOZ_ASSERT(IsValidEpochInstant(instant));
  MOZ_ASSERT(IsValidNormalizedTimeDuration(duration));

  // Step 1. (Inlined AddNormalizedTimeDurationToEpochNanoseconds)
  auto r = instant + duration.to<InstantSpan>();

  // Step 2.
  if (!IsValidEpochInstant(r)) {
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
NormalizedTimeDuration js::temporal::DifferenceInstant(
    const Instant& ns1, const Instant& ns2, Increment roundingIncrement,
    TemporalUnit smallestUnit, TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidEpochInstant(ns1));
  MOZ_ASSERT(IsValidEpochInstant(ns2));
  MOZ_ASSERT(smallestUnit > TemporalUnit::Day);
  MOZ_ASSERT(roundingIncrement <=
             MaximumTemporalDurationRoundingIncrement(smallestUnit));

  // Step 1.
  auto diff = NormalizedTimeDurationFromEpochNanosecondsDifference(ns2, ns1);
  MOZ_ASSERT(IsValidInstantSpan(diff.to<InstantSpan>()));

  // Step 2.
  if (smallestUnit == TemporalUnit::Nanosecond &&
      roundingIncrement == Increment{1}) {
    return diff;
  }

  // Steps 3-4.
  return RoundDuration(diff, roundingIncrement, smallestUnit, roundingMode);
}

/**
 * RoundNumberToIncrementAsIfPositive ( x, increment, roundingMode )
 */
static Instant RoundNumberToIncrementAsIfPositive(
    const Instant& x, int64_t increment, TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidEpochInstant(x));
  MOZ_ASSERT(increment > 0);
  MOZ_ASSERT(increment <= ToNanoseconds(TemporalUnit::Day));

  // This operation is equivalent to adjusting the rounding mode through
  // |ToPositiveRoundingMode| and then calling |RoundNumberToIncrement|.
  auto rounded = RoundNumberToIncrement(x.toNanoseconds(), Int128{increment},
                                        ToPositiveRoundingMode(roundingMode));
  return Instant::fromNanoseconds(rounded);
}

/**
 * RoundTemporalInstant ( ns, increment, unit, roundingMode )
 */
Instant js::temporal::RoundTemporalInstant(const Instant& ns,
                                           Increment increment,
                                           TemporalUnit unit,
                                           TemporalRoundingMode roundingMode) {
  MOZ_ASSERT(IsValidEpochInstant(ns));
  MOZ_ASSERT(increment >= Increment::min());
  MOZ_ASSERT(uint64_t(increment.value()) <= ToNanoseconds(TemporalUnit::Day));

  // Step 1.
  MOZ_ASSERT(unit > TemporalUnit::Day);

  // Step 2.
  int64_t unitLength = ToNanoseconds(unit);

  // Step 3.
  int64_t incrementNs = increment.value() * unitLength;
  MOZ_ASSERT(incrementNs <= ToNanoseconds(TemporalUnit::Day),
             "incrementNs doesn't overflow instant resolution");

  // Step 4.
  return RoundNumberToIncrementAsIfPositive(ns, incrementNs, roundingMode);
}

/**
 * DifferenceTemporalInstant ( operation, instant, other, options )
 */
static bool DifferenceTemporalInstant(JSContext* cx,
                                      TemporalDifference operation,
                                      const CallArgs& args) {
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Instant other;
  if (!ToTemporalInstant(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 3-4.
  DifferenceSettings settings;
  if (args.hasDefined(1)) {
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", ToName(operation), args[1]));
    if (!options) {
      return false;
    }

    // Step 3.
    Rooted<PlainObject*> resolvedOptions(cx,
                                         SnapshotOwnProperties(cx, options));
    if (!resolvedOptions) {
      return false;
    }

    // Step 4.
    if (!GetDifferenceSettings(
            cx, operation, resolvedOptions, TemporalUnitGroup::Time,
            TemporalUnit::Nanosecond, TemporalUnit::Second, &settings)) {
      return false;
    }
  } else {
    // Steps 3-4.
    settings = {
        TemporalUnit::Nanosecond,
        TemporalUnit::Second,
        TemporalRoundingMode::Trunc,
        Increment{1},
    };
  }

  // Step 5.
  auto difference =
      DifferenceInstant(instant, other, settings.roundingIncrement,
                        settings.smallestUnit, settings.roundingMode);

  // Step 6.
  TimeDuration balanced;
  if (!BalanceTimeDuration(cx, difference, settings.largestUnit, &balanced)) {
    return false;
  }

  // Step 7.
  auto duration = balanced.toDuration();
  if (operation == TemporalDifference::Since) {
    duration = duration.negate();
  }

  auto* obj = CreateTemporalDuration(cx, duration);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

enum class InstantDuration { Add, Subtract };

/**
 * AddDurationToOrSubtractDurationFromInstant ( operation, instant,
 * temporalDurationLike )
 */
static bool AddDurationToOrSubtractDurationFromInstant(
    JSContext* cx, InstantDuration operation, const CallArgs& args) {
  auto* instant = &args.thisv().toObject().as<InstantObject>();
  auto epochNanoseconds = ToInstant(instant);

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Duration duration;
  if (!ToTemporalDurationRecord(cx, args.get(0), &duration)) {
    return false;
  }

  // Steps 3-6.
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

  // Step 7.
  if (operation == InstantDuration::Subtract) {
    duration = duration.negate();
  }
  auto timeDuration = NormalizeTimeDuration(duration);

  // Step 8.
  Instant ns;
  if (!AddInstant(cx, epochNanoseconds, timeDuration, &ns)) {
    return false;
  }

  // Step 9.
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

  // Steps 1-2.
  Instant epochInstant;
  if (!ToTemporalInstant(cx, args.get(0), &epochInstant)) {
    return false;
  }

  auto* result = CreateTemporalInstant(cx, epochInstant);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.fromEpochSeconds ( epochSeconds )
 */
static bool Instant_fromEpochSeconds(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  double epochSeconds;
  if (!JS::ToNumber(cx, args.get(0), &epochSeconds)) {
    return false;
  }

  // Step 2.
  //
  // NumberToBigInt throws a RangeError for non-integral numbers.
  if (!IsInteger(epochSeconds)) {
    ToCStringBuf cbuf;
    const char* str = NumberToCString(&cbuf, epochSeconds);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_NONINTEGER, str);
    return false;
  }

  // Step 3. (Not applicable)

  // Step 4.
  if (!IsValidEpochSeconds(epochSeconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  // Step 5.
  int64_t seconds = mozilla::AssertedCast<int64_t>(epochSeconds);
  auto* result = CreateTemporalInstant(cx, Instant::fromSeconds(seconds));
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
  auto* result =
      CreateTemporalInstant(cx, Instant::fromMilliseconds(milliseconds));
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.fromEpochMicroseconds ( epochMicroseconds )
 */
static bool Instant_fromEpochMicroseconds(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<BigInt*> epochMicroseconds(cx, js::ToBigInt(cx, args.get(0)));
  if (!epochMicroseconds) {
    return false;
  }

  // Step 2. (Not applicable)

  // Step 3.
  if (!IsValidEpochMicroseconds(epochMicroseconds)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_INSTANT_INVALID);
    return false;
  }

  int64_t i;
  MOZ_ALWAYS_TRUE(BigInt::isInt64(epochMicroseconds, &i));

  // Step 4.
  auto* result = CreateTemporalInstant(cx, Instant::fromMicroseconds(i));
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
  auto* result = CreateTemporalInstant(cx, ToInstant(epochNanoseconds));
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
  Instant one;
  if (!ToTemporalInstant(cx, args.get(0), &one)) {
    return false;
  }

  // Step 2.
  Instant two;
  if (!ToTemporalInstant(cx, args.get(1), &two)) {
    return false;
  }

  // Step 3.
  args.rval().setInt32(CompareEpochNanoseconds(one, two));
  return true;
}

/**
 * get Temporal.Instant.prototype.epochSeconds
 */
static bool Instant_epochSeconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

  // Steps 4-5.
  args.rval().setNumber(instant.seconds);
  return true;
}

/**
 * get Temporal.Instant.prototype.epochSeconds
 */
static bool Instant_epochSeconds(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_epochSeconds>(cx, args);
}

/**
 * get Temporal.Instant.prototype.epochMilliseconds
 */
static bool Instant_epochMilliseconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

  // Step 4-5.
  args.rval().setNumber(instant.floorToMilliseconds());
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
 * get Temporal.Instant.prototype.epochMicroseconds
 */
static bool Instant_epochMicroseconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

  // Step 4.
  auto* microseconds =
      BigInt::createFromInt64(cx, instant.floorToMicroseconds());
  if (!microseconds) {
    return false;
  }

  // Step 5.
  args.rval().setBigInt(microseconds);
  return true;
}

/**
 * get Temporal.Instant.prototype.epochMicroseconds
 */
static bool Instant_epochMicroseconds(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_epochMicroseconds>(cx, args);
}

/**
 * get Temporal.Instant.prototype.epochNanoseconds
 */
static bool Instant_epochNanoseconds(JSContext* cx, const CallArgs& args) {
  // Step 3.
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());
  auto* nanoseconds = ToEpochNanoseconds(cx, instant);
  if (!nanoseconds) {
    return false;
  }

  // Step 4.
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
  return AddDurationToOrSubtractDurationFromInstant(cx, InstantDuration::Add,
                                                    args);
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
  return AddDurationToOrSubtractDurationFromInstant(
      cx, InstantDuration::Subtract, args);
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
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

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
  auto roundedNs = RoundTemporalInstant(instant, roundingIncrement,
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
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

  // Step 3.
  Instant other;
  if (!ToTemporalInstant(cx, args.get(0), &other)) {
    return false;
  }

  // Steps 4-5.
  args.rval().setBoolean(instant == other);
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
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

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

  // Step 12.
  auto ns = RoundTemporalInstant(instant, precision.increment, precision.unit,
                                 roundingMode);

  // Step 13.
  Rooted<InstantObject*> roundedInstant(cx, CreateTemporalInstant(cx, ns));
  if (!roundedInstant) {
    return false;
  }

  // Step 14.
  JSString* str = TemporalInstantToString(cx, roundedInstant, timeZone,
                                          precision.precision);
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
  Rooted<InstantObject*> instant(cx,
                                 &args.thisv().toObject().as<InstantObject>());

  // Step 3.
  Rooted<TimeZoneValue> timeZone(cx);
  JSString* str =
      TemporalInstantToString(cx, instant, timeZone, Precision::Auto());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
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
  Rooted<InstantObject*> instant(cx,
                                 &args.thisv().toObject().as<InstantObject>());

  // Step 3.
  Rooted<TimeZoneValue> timeZone(cx);
  JSString* str =
      TemporalInstantToString(cx, instant, timeZone, Precision::Auto());
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
 * Temporal.Instant.prototype.toZonedDateTime ( item )
 */
static bool Instant_toZonedDateTime(JSContext* cx, const CallArgs& args) {
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

  // Step 3.
  Rooted<JSObject*> item(
      cx, RequireObjectArg(cx, "item", "toZonedDateTime", args.get(0)));
  if (!item) {
    return false;
  }

  // Step 4.
  Rooted<Value> calendarLike(cx);
  if (!GetProperty(cx, item, item, cx->names().calendar, &calendarLike)) {
    return false;
  }

  // Step 5.
  if (calendarLike.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_MISSING_PROPERTY, "calendar");
    return false;
  }

  // Step 6.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendar(cx, calendarLike, &calendar)) {
    return false;
  }

  // Step 7.
  Rooted<Value> timeZoneLike(cx);
  if (!GetProperty(cx, item, item, cx->names().timeZone, &timeZoneLike)) {
    return false;
  }

  // Step 8.
  if (timeZoneLike.isUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_MISSING_PROPERTY, "timeZone");
    return false;
  }

  // Step 9.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, timeZoneLike, &timeZone)) {
    return false;
  }

  // Step 10.
  auto* result = CreateTemporalZonedDateTime(cx, instant, timeZone, calendar);
  if (!result) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

/**
 * Temporal.Instant.prototype.toZonedDateTime ( item )
 */
static bool Instant_toZonedDateTime(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsInstant, Instant_toZonedDateTime>(cx, args);
}

/**
 * Temporal.Instant.prototype.toZonedDateTimeISO ( item )
 */
static bool Instant_toZonedDateTimeISO(JSContext* cx, const CallArgs& args) {
  auto instant = ToInstant(&args.thisv().toObject().as<InstantObject>());

  // Step 3.
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ToTemporalTimeZone(cx, args.get(0), &timeZone)) {
    return false;
  }

  // Step 4.
  Rooted<CalendarValue> calendar(cx, CalendarValue(CalendarId::ISO8601));
  auto* result = CreateTemporalZonedDateTime(cx, instant, timeZone, calendar);
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
    JS_FN("fromEpochSeconds", Instant_fromEpochSeconds, 1, 0),
    JS_FN("fromEpochMilliseconds", Instant_fromEpochMilliseconds, 1, 0),
    JS_FN("fromEpochMicroseconds", Instant_fromEpochMicroseconds, 1, 0),
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
    JS_FN("toZonedDateTime", Instant_toZonedDateTime, 1, 0),
    JS_FN("toZonedDateTimeISO", Instant_toZonedDateTimeISO, 1, 0),
    JS_FS_END,
};

static const JSPropertySpec Instant_prototype_properties[] = {
    JS_PSG("epochSeconds", Instant_epochSeconds, 0),
    JS_PSG("epochMilliseconds", Instant_epochMilliseconds, 0),
    JS_PSG("epochMicroseconds", Instant_epochMicroseconds, 0),
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
