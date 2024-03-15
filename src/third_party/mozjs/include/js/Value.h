/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS::Value implementation. */

#ifndef js_Value_h
#define js_Value_h

#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"

#include <limits> /* for std::numeric_limits */
#include <type_traits>

#include "jstypes.h"

#include "js/HeapAPI.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

namespace JS {
class JS_PUBLIC_API Value;
}

// [SMDOC] JS::Value Boxing Formats
//
// JS::Value is a 64-bit value, on all architectures. It is conceptually a
// discriminated union of all the types of values that can be represented in SM:
// - Object Pointers
// - 64 bit IEEE 754 floats
// - 32-bit integer values
// - and quite a few more (see JSValueType)
//
// The ECMAScript standard specifies that ECMAScript numbers are IEEE 64-bit
// floating-point values. A JS::Value can represent any JavaScript number
// value directly, without referring to additional storage, or represent an
// object, string, or other ECMAScript value, and remember which type it is.
//
// This may seem surprising: how can a 64-bit type hold all the 64-bit IEEE
// values, and still distinguish them from objects, strings, and so on,
// which have 64-bit addresses ?
//
// This is possible for two reasons:
//
// - First, ECMAScript implementations aren't required to distinguish all
//   the values the IEEE 64-bit format can represent.
//
//   The IEEE 754 format for floating point numbers specifies that every
//   floating-point value whose 11-bit exponent field is all ones, and whose
//   52-bit fraction field is non-zero, has the value NaN. EMCAScript requires
//   only one NaN value. This means we can use one IEEE NaN to represent
//   ECMAScript's NaN, and use all the other 2^52-2 NaN bitstrings to
//   represent the other ECMAScript values.
//
// - Second, on the 64 bit architectures we suppport, only the
//   lower 48 bits of an address are currently significant. The upper sixteen
//   bits are required to be the sign-extension of bit 48. Furthermore, user
//   code always runs in "positive addresses": those in which bit 48 is zero. So
//   we only actually need 47 bits to store all possible object or string
//   addresses, even on 64-bit platforms.
//
//   Our memory initialization system ensures that all pointers we will store in
//   objects use only 47 bits. See js::gc::MapAlignedPagesRandom.
//
//   The introduction of 5-level page tables, supporting 57-bit virtual
//   addresses, is a potential complication. For now, large addresses are
//   opt-in, and we simply don't use them.
//
// With a 52-bit fraction field, and 47 bits needed for the 'payload', we
// have up to five bits left to store a 'tag' value, to indicate which
// branch of our discriminated union is live. (In practice, one of those
// bits is used up to simplify NaN representation; see micro-optimization 5
// below.)
//
// Thus, we define JS::Value representations in terms of the IEEE 64-bit
// floating-point format:
//
// - Any bitstring that IEEE calls a number or an infinity represents that
//   ECMAScript number.
//
// - Any bitstring that IEEE calls a NaN represents either an ECMAScript NaN
//   or a non-number ECMAScript value, as determined by a tag field stored
//   towards the most significant end of the fraction field (exactly where
//   depends on the address size). If the tag field indicates that this
//   JS::Value is an object, the fraction field's least significant end
//   holds the address of a JSObject; if a string, the address of a
//   JSString; and so on.
//
//   To enforce this invariant, anywhere that may provide a numerical value
//   which may have a non-canonical NaN value (NaN, but not the one we've chosen
//   for ECMAScript) we must convert that to the canonical NaN. See
//   JS::CanonicalizeNaN.
//
// We have two boxing modes defined: NUNBOX32 and PUNBOX64.The first is
// "NaN unboxed boxing" (or Nunboxing), as non-Number payload are stored
// unaltered in the lower bits. The second is "Packed NaN boxing" (or
// punboxing), which is 'logically like nunboxing, but with all the unused bits
// sucked out' [1],  as we rely on unused bits of the payload to pack the
// payload in the lower bits using Nunboxing.
//
// - In NUNBOX32 the tag is stored in the least-significant bits of the high
//   word of the NaN. Since it's used on 32-bit systems, this has the nice
//   property that boxed values are simply stored in the low-word of the 8-byte
//   NaN.
//
// - In PUNBOX64, since we need to store more pointer bits (47, see above), the
//   tag is stored in the 5 most significant bits of the fraction adjacent to
//   the exponent.
//
// Tag values are carefully ordered to support a set of micro-optimizations. In
// particular:
//
// 1. Object is the highest tag, to simplify isPrimitive checks. (See
//    ValueUpperExclPrimitiveTag)
// 2. Numbers (Double and Int32) are the lowest tags, to simplify isNumber
//    checks. (See ValueUpperInclNumberTag)
// 3. Non-GC tags are ordered before GC-tags, to simplify isGCThing checks. (See
//    ValueLowerInclGCThingTag)
// 4. The tags for Object and Null differ by a single flipped bit, to simplify
//    toObjectOrNull. (See ValueObjectOrNullBit)
// 5. In PUNBOX64, the most significant bit of every non-Double tag is always
//    set. This is to simplify isDouble checks. Note that the highest bitstring
//    that corresponds to a non-NaN double is -Infinity:
//      0xfff0_0000_0000_0000
//    But the canonical hardware NaN (produced by, for example, 0/0) is:
//      0x?ff8_0000_0000_0000
//    on all platforms with JIT support*. (The most significant bit is the sign
//    bit; it is 1 on x86, but 0 on ARM.) The most significant bit of the
//    fraction field is set, which corresponds to the most significant of the 5
//    tag bits. Because we only use tags that have the high bit set, any Value
//    represented by a bitstring less than or equal to 0xfff8_..._0000 is a
//    Double. (If we wanted to use all five bits, we could define 0x10 as
//    JSVAL_TYPE_NAN, and mask off the most significant bit of the tag for
//    IsDouble checks. This is not yet necessary, because we still have room
//    left to allocate new tags.)
//
//    * But see JS_NONCANONICAL_HARDWARE_NAN below.
//
// [1]:
// https://wingolog.org/archives/2011/05/18/value-representation-in-javascript-implementations#969f63bbe4eb912778c9da85feb0f5763e7a7862

/* JS::Value can store a full int32_t. */
#define JSVAL_INT_BITS 32
#define JSVAL_INT_MIN ((int32_t)0x80000000)
#define JSVAL_INT_MAX ((int32_t)0x7fffffff)

#if defined(JS_NUNBOX32)
#  define JSVAL_TAG_SHIFT 32
#elif defined(JS_PUNBOX64)
#  define JSVAL_TAG_SHIFT 47
#endif

// Use enums so that printing a JS::Value in the debugger shows nice
// symbolic type tags.

enum JSValueType : uint8_t {
  JSVAL_TYPE_DOUBLE = 0x00,
  JSVAL_TYPE_INT32 = 0x01,
  JSVAL_TYPE_BOOLEAN = 0x02,
  JSVAL_TYPE_UNDEFINED = 0x03,
  JSVAL_TYPE_NULL = 0x04,
  JSVAL_TYPE_MAGIC = 0x05,
  JSVAL_TYPE_STRING = 0x06,
  JSVAL_TYPE_SYMBOL = 0x07,
  JSVAL_TYPE_PRIVATE_GCTHING = 0x08,
  JSVAL_TYPE_BIGINT = 0x09,
#ifdef ENABLE_RECORD_TUPLE
  JSVAL_TYPE_EXTENDED_PRIMITIVE = 0x0b,
#endif
  JSVAL_TYPE_OBJECT = 0x0c,

  // This type never appears in a Value; it's only an out-of-band value.
  JSVAL_TYPE_UNKNOWN = 0x20
};

namespace JS {
enum class ValueType : uint8_t {
  Double = JSVAL_TYPE_DOUBLE,
  Int32 = JSVAL_TYPE_INT32,
  Boolean = JSVAL_TYPE_BOOLEAN,
  Undefined = JSVAL_TYPE_UNDEFINED,
  Null = JSVAL_TYPE_NULL,
  Magic = JSVAL_TYPE_MAGIC,
  String = JSVAL_TYPE_STRING,
  Symbol = JSVAL_TYPE_SYMBOL,
  PrivateGCThing = JSVAL_TYPE_PRIVATE_GCTHING,
  BigInt = JSVAL_TYPE_BIGINT,
#ifdef ENABLE_RECORD_TUPLE
  ExtendedPrimitive = JSVAL_TYPE_EXTENDED_PRIMITIVE,
#endif
  Object = JSVAL_TYPE_OBJECT,
};
}  // namespace JS

static_assert(sizeof(JSValueType) == 1,
              "compiler typed enum support is apparently buggy");

#if defined(JS_NUNBOX32)

enum JSValueTag : uint32_t {
  JSVAL_TAG_CLEAR = 0xFFFFFF80,
  JSVAL_TAG_INT32 = JSVAL_TAG_CLEAR | JSVAL_TYPE_INT32,
  JSVAL_TAG_UNDEFINED = JSVAL_TAG_CLEAR | JSVAL_TYPE_UNDEFINED,
  JSVAL_TAG_NULL = JSVAL_TAG_CLEAR | JSVAL_TYPE_NULL,
  JSVAL_TAG_BOOLEAN = JSVAL_TAG_CLEAR | JSVAL_TYPE_BOOLEAN,
  JSVAL_TAG_MAGIC = JSVAL_TAG_CLEAR | JSVAL_TYPE_MAGIC,
  JSVAL_TAG_STRING = JSVAL_TAG_CLEAR | JSVAL_TYPE_STRING,
  JSVAL_TAG_SYMBOL = JSVAL_TAG_CLEAR | JSVAL_TYPE_SYMBOL,
  JSVAL_TAG_PRIVATE_GCTHING = JSVAL_TAG_CLEAR | JSVAL_TYPE_PRIVATE_GCTHING,
  JSVAL_TAG_BIGINT = JSVAL_TAG_CLEAR | JSVAL_TYPE_BIGINT,
#  ifdef ENABLE_RECORD_TUPLE
  JSVAL_TAG_EXTENDED_PRIMITIVE =
      JSVAL_TAG_CLEAR | JSVAL_TYPE_EXTENDED_PRIMITIVE,
#  endif
  JSVAL_TAG_OBJECT = JSVAL_TAG_CLEAR | JSVAL_TYPE_OBJECT
};

static_assert(sizeof(JSValueTag) == sizeof(uint32_t),
              "compiler typed enum support is apparently buggy");

#elif defined(JS_PUNBOX64)

enum JSValueTag : uint32_t {
  JSVAL_TAG_MAX_DOUBLE = 0x1FFF0,
  JSVAL_TAG_INT32 = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_INT32,
  JSVAL_TAG_UNDEFINED = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_UNDEFINED,
  JSVAL_TAG_NULL = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_NULL,
  JSVAL_TAG_BOOLEAN = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_BOOLEAN,
  JSVAL_TAG_MAGIC = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_MAGIC,
  JSVAL_TAG_STRING = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_STRING,
  JSVAL_TAG_SYMBOL = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_SYMBOL,
  JSVAL_TAG_PRIVATE_GCTHING = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_PRIVATE_GCTHING,
  JSVAL_TAG_BIGINT = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_BIGINT,
#  ifdef ENABLE_RECORD_TUPLE
  JSVAL_TAG_EXTENDED_PRIMITIVE =
      JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_EXTENDED_PRIMITIVE,
#  endif
  JSVAL_TAG_OBJECT = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_OBJECT
};

static_assert(sizeof(JSValueTag) == sizeof(uint32_t),
              "compiler typed enum support is apparently buggy");

enum JSValueShiftedTag : uint64_t {
  // See Bug 584653 for why we include 0xFFFFFFFF.
  JSVAL_SHIFTED_TAG_MAX_DOUBLE =
      ((uint64_t(JSVAL_TAG_MAX_DOUBLE) << JSVAL_TAG_SHIFT) | 0xFFFFFFFF),
  JSVAL_SHIFTED_TAG_INT32 = (uint64_t(JSVAL_TAG_INT32) << JSVAL_TAG_SHIFT),
  JSVAL_SHIFTED_TAG_UNDEFINED =
      (uint64_t(JSVAL_TAG_UNDEFINED) << JSVAL_TAG_SHIFT),
  JSVAL_SHIFTED_TAG_NULL = (uint64_t(JSVAL_TAG_NULL) << JSVAL_TAG_SHIFT),
  JSVAL_SHIFTED_TAG_BOOLEAN = (uint64_t(JSVAL_TAG_BOOLEAN) << JSVAL_TAG_SHIFT),
  JSVAL_SHIFTED_TAG_MAGIC = (uint64_t(JSVAL_TAG_MAGIC) << JSVAL_TAG_SHIFT),
  JSVAL_SHIFTED_TAG_STRING = (uint64_t(JSVAL_TAG_STRING) << JSVAL_TAG_SHIFT),
  JSVAL_SHIFTED_TAG_SYMBOL = (uint64_t(JSVAL_TAG_SYMBOL) << JSVAL_TAG_SHIFT),
  JSVAL_SHIFTED_TAG_PRIVATE_GCTHING =
      (uint64_t(JSVAL_TAG_PRIVATE_GCTHING) << JSVAL_TAG_SHIFT),
  JSVAL_SHIFTED_TAG_BIGINT = (uint64_t(JSVAL_TAG_BIGINT) << JSVAL_TAG_SHIFT),
#  ifdef ENABLE_RECORD_TUPLE
  JSVAL_SHIFTED_TAG_EXTENDED_PRIMITIVE =
      (uint64_t(JSVAL_TYPE_EXTENDED_PRIMITIVE) << JSVAL_TAG_SHIFT),
#  endif
  JSVAL_SHIFTED_TAG_OBJECT = (uint64_t(JSVAL_TAG_OBJECT) << JSVAL_TAG_SHIFT)
};

static_assert(sizeof(JSValueShiftedTag) == sizeof(uint64_t),
              "compiler typed enum support is apparently buggy");

#endif

namespace JS {
namespace detail {

#if defined(JS_NUNBOX32)

constexpr JSValueTag ValueTypeToTag(JSValueType type) {
  return static_cast<JSValueTag>(JSVAL_TAG_CLEAR |
                                 std::underlying_type_t<JSValueType>(type));
}

constexpr bool ValueIsDouble(uint64_t bits) {
  return uint32_t(bits >> JSVAL_TAG_SHIFT) <= uint32_t(JSVAL_TAG_CLEAR);
}

constexpr JSValueTag ValueUpperExclPrimitiveTag = JSVAL_TAG_OBJECT;
constexpr JSValueTag ValueUpperInclNumberTag = JSVAL_TAG_INT32;
constexpr JSValueTag ValueLowerInclGCThingTag = JSVAL_TAG_STRING;

#elif defined(JS_PUNBOX64)

constexpr JSValueTag ValueTypeToTag(JSValueType type) {
  return static_cast<JSValueTag>(JSVAL_TAG_MAX_DOUBLE |
                                 std::underlying_type_t<JSValueType>(type));
}

constexpr bool ValueIsDouble(uint64_t bits) {
  return bits <= JSVAL_SHIFTED_TAG_MAX_DOUBLE;
}

constexpr uint64_t ValueTagMask = 0xFFFF'8000'0000'0000;

// This should only be used in toGCThing. See the 'Spectre mitigations' comment.
constexpr uint64_t ValueGCThingPayloadMask = 0x0000'7FFF'FFFF'FFFF;

// Mask used to combine an unbox operation with getting the chunk base.
constexpr uint64_t ValueGCThingPayloadChunkMask =
    ValueGCThingPayloadMask & ~js::gc::ChunkMask;

constexpr uint64_t ValueTypeToShiftedTag(JSValueType type) {
  return static_cast<uint64_t>(ValueTypeToTag(type)) << JSVAL_TAG_SHIFT;
}
#  define JSVAL_TYPE_TO_SHIFTED_TAG(type) \
    (JS::detail::ValueTypeToShiftedTag(type))

constexpr JSValueTag ValueUpperExclPrimitiveTag = JSVAL_TAG_OBJECT;
constexpr JSValueTag ValueUpperInclNumberTag = JSVAL_TAG_INT32;
constexpr JSValueTag ValueLowerInclGCThingTag = JSVAL_TAG_STRING;

constexpr uint64_t ValueUpperExclShiftedPrimitiveTag = JSVAL_SHIFTED_TAG_OBJECT;
constexpr uint64_t ValueUpperExclShiftedNumberTag = JSVAL_SHIFTED_TAG_BOOLEAN;
constexpr uint64_t ValueLowerInclShiftedGCThingTag = JSVAL_SHIFTED_TAG_STRING;

// JSVAL_TYPE_OBJECT and JSVAL_TYPE_NULL differ by one bit. We can use this to
// implement toObjectOrNull more efficiently.
constexpr uint64_t ValueObjectOrNullBit = 0x8ULL << JSVAL_TAG_SHIFT;
static_assert(
    (JSVAL_SHIFTED_TAG_NULL ^ JSVAL_SHIFTED_TAG_OBJECT) == ValueObjectOrNullBit,
    "ValueObjectOrNullBit must be consistent with object and null tags");

constexpr uint64_t IsValidUserModePointer(uint64_t bits) {
  // All 64-bit platforms that we support actually have a 48-bit address space
  // for user-mode pointers, with the top 16 bits all set to zero.
  return (bits & 0xFFFF'0000'0000'0000) == 0;
}

#endif /* JS_PUNBOX64 */

}  // namespace detail
}  // namespace JS

#define JSVAL_TYPE_TO_TAG(type) (JS::detail::ValueTypeToTag(type))

enum JSWhyMagic {
  /** a hole in a native object's elements */
  JS_ELEMENTS_HOLE,

  /** there is not a pending iterator value */
  JS_NO_ITER_VALUE,

  /** exception value thrown when closing a generator */
  JS_GENERATOR_CLOSING,

  /** used in debug builds to catch tracing errors */
  JS_ARG_POISON,

  /** an empty subnode in the AST serializer */
  JS_SERIALIZE_NO_NODE,

  /** magic value passed to natives to indicate construction */
  JS_IS_CONSTRUCTING,

  /** see class js::HashableValue */
  JS_HASH_KEY_EMPTY,

  /** error while running Ion code */
  JS_ION_ERROR,

  /** missing recover instruction result */
  JS_ION_BAILOUT,

  /** optimized out slot */
  JS_OPTIMIZED_OUT,

  /** uninitialized lexical bindings that produce ReferenceError on touch. */
  JS_UNINITIALIZED_LEXICAL,

  /** arguments object can't be created because environment is dead. */
  JS_MISSING_ARGUMENTS,

  /** for local use */
  JS_GENERIC_MAGIC,

  /**
   * When an error object is created without the error cause argument, we set
   * the error's cause slot to this magic value.
   */
  JS_ERROR_WITHOUT_CAUSE,

  JS_WHY_MAGIC_COUNT
};

namespace js {
static inline JS::Value PoisonedObjectValue(uintptr_t poison);
#ifdef ENABLE_RECORD_TUPLE
// Re-defined in vm/RecordTupleBoxShared.h. We cannot include that
// file because it circularly includes this one.
bool IsExtendedPrimitive(const JSObject& obj);
namespace gc {
bool MaybeForwardedIsExtendedPrimitive(const JSObject& obj);
}  // namespace gc
#endif
}  // namespace js

namespace JS {

namespace detail {

// IEEE-754 bit pattern for double-precision positive infinity.
constexpr int InfinitySignBit = 0;
constexpr uint64_t InfinityBits =
    mozilla::InfinityBits<double, detail::InfinitySignBit>::value;

// This is a quiet NaN on IEEE-754[2008] compatible platforms, including X86,
// ARM, SPARC, RISC-V and modern MIPS.
//
// Note: The default sign bit for a hardware synthesized NaN differs between X86
//       and ARM. Both values are considered compatible values on both
//       platforms.
constexpr int CanonicalizedNaNSignBit = 0;
constexpr uint64_t CanonicalizedNaNSignificand = 0x8000000000000;

#if defined(__sparc__)
// Some architectures (not to name names) generate NaNs with bit patterns that
// are incompatible with JS::Value's bit pattern restrictions. Instead we must
// canonicalize all hardware values before storing in JS::Value.
#  define JS_NONCANONICAL_HARDWARE_NAN
#endif

#if defined(__mips__) && !defined(__mips_nan_2008)
// These builds may run on hardware that has differing polarity of the signaling
// NaN bit. While the kernel may handle the trap for us, it is a performance
// issue so instead we compute the NaN to use on startup. The runtime value must
// still meet `ValueIsDouble` requirements which are checked on startup.

// In particular, we expect one of the following values on MIPS:
//  - 0x7FF7FFFFFFFFFFFF    Legacy
//  - 0x7FF8000000000000    IEEE-754[2008]
#  define JS_RUNTIME_CANONICAL_NAN
#endif

#if defined(JS_RUNTIME_CANONICAL_NAN)
extern uint64_t CanonicalizedNaNBits;
#else
constexpr uint64_t CanonicalizedNaNBits =
    mozilla::SpecificNaNBits<double, detail::CanonicalizedNaNSignBit,
                             detail::CanonicalizedNaNSignificand>::value;
#endif
}  // namespace detail

// Return a quiet NaN that is compatible with JS::Value restrictions.
static MOZ_ALWAYS_INLINE double GenericNaN() {
#if !defined(JS_RUNTIME_CANONICAL_NAN)
  static_assert(detail::ValueIsDouble(detail::CanonicalizedNaNBits),
                "Canonical NaN must be compatible with JS::Value");
#endif

  return mozilla::BitwiseCast<double>(detail::CanonicalizedNaNBits);
}

// Return the infinity the engine uses
static MOZ_ALWAYS_INLINE double Infinity() {
  return mozilla::BitwiseCast<double>(detail::InfinityBits);
}

// Convert an arbitrary double to one compatible with JS::Value representation
// by replacing any NaN value with a canonical one.
static MOZ_ALWAYS_INLINE double CanonicalizeNaN(double d) {
  if (MOZ_UNLIKELY(std::isnan(d))) {
    return GenericNaN();
  }
  return d;
}

/**
 * [SMDOC] JS::Value type
 *
 * JS::Value is the interface for a single JavaScript Engine value.  A few
 * general notes on JS::Value:
 *
 * - JS::Value has setX() and isX() members for X in
 *
 *     { Int32, Double, String, Symbol, BigInt, Boolean, Undefined, Null,
 *       Object, Magic }
 *
 *   JS::Value also contains toX() for each of the non-singleton types.
 *
 * - Magic is a singleton type whose payload contains either a JSWhyMagic
 *   "reason" for the magic value or a uint32_t value. By providing JSWhyMagic
 *   values when creating and checking for magic values, it is possible to
 *   assert, at runtime, that only magic values with the expected reason flow
 *   through a particular value. For example, if cx->exception has a magic
 *   value, the reason must be JS_GENERATOR_CLOSING.
 *
 * - The JS::Value operations are preferred.  The JSVAL_* operations remain for
 *   compatibility; they may be removed at some point.  These operations mostly
 *   provide similar functionality.  But there are a few key differences.  One
 *   is that JS::Value gives null a separate type.
 *   Also, to help prevent mistakenly boxing a nullable JSObject* as an object,
 *   Value::setObject takes a JSObject&. (Conversely, Value::toObject returns a
 *   JSObject&.)  A convenience member Value::setObjectOrNull is provided.
 *
 * - Note that JS::Value is 8 bytes on 32 and 64-bit architectures. Thus, on
 *   32-bit user code should avoid copying jsval/JS::Value as much as possible,
 *   preferring to pass by const Value&.
 *
 * Spectre mitigations
 * ===================
 * To mitigate Spectre attacks, we do the following:
 *
 * - On 64-bit platforms, when unboxing a Value, we XOR the bits with the
 *   expected type tag (instead of masking the payload bits). This guarantees
 *   that toString, toObject, toSymbol will return an invalid pointer (because
 *   some high bits will be set) when called on a Value with a different type
 *   tag.
 *
 * - On 32-bit platforms,when unboxing an object/string/symbol Value, we use a
 *   conditional move (not speculated) to zero the payload register if the type
 *   doesn't match.
 */
class alignas(8) Value {
 private:
  uint64_t asBits_;

 public:
  constexpr Value() : asBits_(bitsFromTagAndPayload(JSVAL_TAG_UNDEFINED, 0)) {}

 private:
  explicit constexpr Value(uint64_t asBits) : asBits_(asBits) {}

  static uint64_t bitsFromDouble(double d) {
#if defined(JS_NONCANONICAL_HARDWARE_NAN)
    d = CanonicalizeNaN(d);
#endif
    return mozilla::BitwiseCast<uint64_t>(d);
  }

  static_assert(sizeof(JSValueType) == 1,
                "type bits must fit in a single byte");
  static_assert(sizeof(JSValueTag) == 4,
                "32-bit Value's tag_ must have size 4 to complement the "
                "payload union's size 4");
  static_assert(sizeof(JSWhyMagic) <= 4,
                "32-bit Value's JSWhyMagic payload field must not inflate "
                "the payload beyond 4 bytes");

 public:
#if defined(JS_NUNBOX32)
  using PayloadType = uint32_t;
#elif defined(JS_PUNBOX64)
  using PayloadType = uint64_t;
#endif

  static constexpr uint64_t bitsFromTagAndPayload(JSValueTag tag,
                                                  PayloadType payload) {
    return (uint64_t(tag) << JSVAL_TAG_SHIFT) | payload;
  }

  static constexpr Value fromTagAndPayload(JSValueTag tag,
                                           PayloadType payload) {
    return fromRawBits(bitsFromTagAndPayload(tag, payload));
  }

  static constexpr Value fromRawBits(uint64_t asBits) { return Value(asBits); }

  static constexpr Value fromInt32(int32_t i) {
    return fromTagAndPayload(JSVAL_TAG_INT32, uint32_t(i));
  }

  static Value fromDouble(double d) { return fromRawBits(bitsFromDouble(d)); }

  /**
   * Returns false if creating a NumberValue containing the given type would
   * be lossy, true otherwise.
   */
  template <typename T>
  static bool isNumberRepresentable(const T t) {
    return T(double(t)) == t;
  }

  /*** Mutators ***/

  void setNull() {
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_NULL, 0);
    MOZ_ASSERT(isNull());
  }

  void setUndefined() {
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_UNDEFINED, 0);
    MOZ_ASSERT(isUndefined());
  }

  void setInt32(int32_t i) {
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_INT32, uint32_t(i));
    MOZ_ASSERT(toInt32() == i);
  }

  void setDouble(double d) {
    asBits_ = bitsFromDouble(d);
    MOZ_ASSERT(isDouble());
  }

  void setString(JSString* str) {
    MOZ_ASSERT(js::gc::IsCellPointerValid(str));
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_STRING, PayloadType(str));
    MOZ_ASSERT(toString() == str);
  }

  void setSymbol(JS::Symbol* sym) {
    MOZ_ASSERT(js::gc::IsCellPointerValid(sym));
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_SYMBOL, PayloadType(sym));
    MOZ_ASSERT(toSymbol() == sym);
  }

  void setBigInt(JS::BigInt* bi) {
    MOZ_ASSERT(js::gc::IsCellPointerValid(bi));
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_BIGINT, PayloadType(bi));
    MOZ_ASSERT(toBigInt() == bi);
  }

  void setObject(JSObject& obj) {
    MOZ_ASSERT(js::gc::IsCellPointerValid(&obj));
#ifdef ENABLE_RECORD_TUPLE
    MOZ_ASSERT(!js::gc::MaybeForwardedIsExtendedPrimitive(obj));
#endif
    setObjectNoCheck(&obj);
    MOZ_ASSERT(&toObject() == &obj);
  }

#ifdef ENABLE_RECORD_TUPLE
  void setExtendedPrimitive(JSObject& obj) {
    MOZ_ASSERT(js::gc::IsCellPointerValid(&obj));
    MOZ_ASSERT(js::gc::MaybeForwardedIsExtendedPrimitive(obj));
    asBits_ =
        bitsFromTagAndPayload(JSVAL_TAG_EXTENDED_PRIMITIVE, PayloadType(&obj));
    MOZ_ASSERT(&toExtendedPrimitive() == &obj);
  }
#endif

 private:
  void setObjectNoCheck(JSObject* obj) {
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_OBJECT, PayloadType(obj));
  }

  friend inline Value js::PoisonedObjectValue(uintptr_t poison);

 public:
  void setBoolean(bool b) {
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_BOOLEAN, uint32_t(b));
    MOZ_ASSERT(toBoolean() == b);
  }

  void setMagic(JSWhyMagic why) {
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_MAGIC, uint32_t(why));
    MOZ_ASSERT(whyMagic() == why);
  }

  void setMagicUint32(uint32_t payload) {
    MOZ_ASSERT(payload >= JS_WHY_MAGIC_COUNT,
               "This should only be used for non-standard magic values");
    asBits_ = bitsFromTagAndPayload(JSVAL_TAG_MAGIC, payload);
    MOZ_ASSERT(magicUint32() == payload);
  }

  void setNumber(float f) {
    int32_t i;
    if (mozilla::NumberIsInt32(f, &i)) {
      setInt32(i);
      return;
    }

    setDouble(double(f));
  }

  void setNumber(double d) {
    int32_t i;
    if (mozilla::NumberIsInt32(d, &i)) {
      setInt32(i);
      return;
    }

    setDouble(d);
  }

  template <typename T>
  void setNumber(const T t) {
    static_assert(std::is_integral<T>::value, "must be integral type");
    MOZ_ASSERT(isNumberRepresentable(t), "value creation would be lossy");

    if constexpr (std::numeric_limits<T>::is_signed) {
      if constexpr (sizeof(t) <= sizeof(int32_t)) {
        setInt32(int32_t(t));
      } else {
        if (JSVAL_INT_MIN <= t && t <= JSVAL_INT_MAX) {
          setInt32(int32_t(t));
        } else {
          setDouble(double(t));
        }
      }
    } else {
      if constexpr (sizeof(t) <= sizeof(uint16_t)) {
        setInt32(int32_t(t));
      } else {
        if (t <= JSVAL_INT_MAX) {
          setInt32(int32_t(t));
        } else {
          setDouble(double(t));
        }
      }
    }
  }

  void setObjectOrNull(JSObject* arg) {
    if (arg) {
      setObject(*arg);
    } else {
      setNull();
    }
  }

  void swap(Value& rhs) {
    uint64_t tmp = rhs.asBits_;
    rhs.asBits_ = asBits_;
    asBits_ = tmp;
  }

 private:
  JSValueTag toTag() const { return JSValueTag(asBits_ >> JSVAL_TAG_SHIFT); }

  template <typename T, JSValueTag Tag>
  T* unboxGCPointer() const {
    MOZ_ASSERT((asBits_ & js::gc::CellAlignMask) == 0,
               "GC pointer is not aligned. Is this memory corruption?");
#if defined(JS_NUNBOX32)
    uintptr_t payload = uint32_t(asBits_);
    return reinterpret_cast<T*>(payload);
#elif defined(JS_PUNBOX64)
    // Note: the 'Spectre mitigations' comment at the top of this class
    // explains why we use XOR here.
    constexpr uint64_t shiftedTag = uint64_t(Tag) << JSVAL_TAG_SHIFT;
    return reinterpret_cast<T*>(uintptr_t(asBits_ ^ shiftedTag));
#endif
  }

 public:
  /*** JIT-only interfaces to interact with and create raw Values ***/
#if defined(JS_NUNBOX32)
  PayloadType toNunboxPayload() const { return uint32_t(asBits_); }

  JSValueTag toNunboxTag() const { return toTag(); }
#elif defined(JS_PUNBOX64)
  const void* bitsAsPunboxPointer() const {
    return reinterpret_cast<void*>(asBits_);
  }
#endif

  /*** Value type queries ***/

  /*
   * N.B. GCC, in some but not all cases, chooses to emit signed comparison
   * of JSValueTag even though its underlying type has been forced to be
   * uint32_t.  Thus, all comparisons should explicitly cast operands to
   * uint32_t.
   */

  bool isUndefined() const {
#if defined(JS_NUNBOX32)
    return toTag() == JSVAL_TAG_UNDEFINED;
#elif defined(JS_PUNBOX64)
    return asBits_ == JSVAL_SHIFTED_TAG_UNDEFINED;
#endif
  }

  bool isNull() const {
#if defined(JS_NUNBOX32)
    return toTag() == JSVAL_TAG_NULL;
#elif defined(JS_PUNBOX64)
    return asBits_ == JSVAL_SHIFTED_TAG_NULL;
#endif
  }

  bool isNullOrUndefined() const { return isNull() || isUndefined(); }

  bool isInt32() const { return toTag() == JSVAL_TAG_INT32; }

  bool isInt32(int32_t i32) const {
    return asBits_ == bitsFromTagAndPayload(JSVAL_TAG_INT32, uint32_t(i32));
  }

  bool isDouble() const { return detail::ValueIsDouble(asBits_); }

  bool isNumber() const {
#if defined(JS_NUNBOX32)
    MOZ_ASSERT(toTag() != JSVAL_TAG_CLEAR);
    return uint32_t(toTag()) <= uint32_t(detail::ValueUpperInclNumberTag);
#elif defined(JS_PUNBOX64)
    return asBits_ < detail::ValueUpperExclShiftedNumberTag;
#endif
  }

  bool isString() const { return toTag() == JSVAL_TAG_STRING; }

  bool isSymbol() const { return toTag() == JSVAL_TAG_SYMBOL; }

  bool isBigInt() const { return toTag() == JSVAL_TAG_BIGINT; }

  bool isObject() const {
#if defined(JS_NUNBOX32)
    return toTag() == JSVAL_TAG_OBJECT;
#elif defined(JS_PUNBOX64)
    MOZ_ASSERT((asBits_ >> JSVAL_TAG_SHIFT) <= JSVAL_TAG_OBJECT);
    return asBits_ >= JSVAL_SHIFTED_TAG_OBJECT;
#endif
  }

#ifdef ENABLE_RECORD_TUPLE
  bool isExtendedPrimitive() const {
    return toTag() == JSVAL_TAG_EXTENDED_PRIMITIVE;
  }
#endif

  bool hasObjectPayload() const {
    return isObject() || IF_RECORD_TUPLE(isExtendedPrimitive(), false);
  }

  bool isPrimitive() const {
#if defined(JS_NUNBOX32)
    return uint32_t(toTag()) < uint32_t(detail::ValueUpperExclPrimitiveTag);
#elif defined(JS_PUNBOX64)
    return asBits_ < detail::ValueUpperExclShiftedPrimitiveTag;
#endif
  }

  bool isObjectOrNull() const { return isObject() || isNull(); }

  bool isNumeric() const { return isNumber() || isBigInt(); }

  bool isGCThing() const {
#if defined(JS_NUNBOX32)
    /* gcc sometimes generates signed < without explicit casts. */
    return uint32_t(toTag()) >= uint32_t(detail::ValueLowerInclGCThingTag);
#elif defined(JS_PUNBOX64)
    return asBits_ >= detail::ValueLowerInclShiftedGCThingTag;
#endif
  }

  bool isBoolean() const { return toTag() == JSVAL_TAG_BOOLEAN; }

  bool isTrue() const {
    return asBits_ == bitsFromTagAndPayload(JSVAL_TAG_BOOLEAN, uint32_t(true));
  }

  bool isFalse() const {
    return asBits_ == bitsFromTagAndPayload(JSVAL_TAG_BOOLEAN, uint32_t(false));
  }

  bool isMagic() const { return toTag() == JSVAL_TAG_MAGIC; }

  bool isMagic(JSWhyMagic why) const {
    if (!isMagic()) {
      return false;
    }
    MOZ_RELEASE_ASSERT(whyMagic() == why);
    return true;
  }

  JS::TraceKind traceKind() const {
    MOZ_ASSERT(isGCThing());
    static_assert((JSVAL_TAG_STRING & 0x03) == size_t(JS::TraceKind::String),
                  "Value type tags must correspond with JS::TraceKinds.");
    static_assert((JSVAL_TAG_SYMBOL & 0x03) == size_t(JS::TraceKind::Symbol),
                  "Value type tags must correspond with JS::TraceKinds.");
    static_assert((JSVAL_TAG_OBJECT & 0x03) == size_t(JS::TraceKind::Object),
                  "Value type tags must correspond with JS::TraceKinds.");
    static_assert((JSVAL_TAG_BIGINT & 0x03) == size_t(JS::TraceKind::BigInt),
                  "Value type tags must correspond with JS::TraceKinds.");
    if (MOZ_UNLIKELY(isPrivateGCThing())) {
      return JS::GCThingTraceKind(toGCThing());
    }
#ifdef ENABLE_RECORD_TUPLE
    if (isExtendedPrimitive()) {
      return JS::TraceKind::Object;
    }
#endif
    return JS::TraceKind(toTag() & 0x03);
  }

  JSWhyMagic whyMagic() const {
    MOZ_ASSERT(magicUint32() < JS_WHY_MAGIC_COUNT);
    return static_cast<JSWhyMagic>(magicUint32());
  }

  uint32_t magicUint32() const {
    MOZ_ASSERT(isMagic());
    return uint32_t(asBits_);
  }

  /*** Comparison ***/

  bool operator==(const Value& rhs) const { return asBits_ == rhs.asBits_; }

  bool operator!=(const Value& rhs) const { return asBits_ != rhs.asBits_; }

  friend inline bool SameType(const Value& lhs, const Value& rhs);

  /*** Extract the value's typed payload ***/

  int32_t toInt32() const {
    MOZ_ASSERT(isInt32());
    return int32_t(asBits_);
  }

  double toDouble() const {
    MOZ_ASSERT(isDouble());
    return mozilla::BitwiseCast<double>(asBits_);
  }

  double toNumber() const {
    MOZ_ASSERT(isNumber());
    return isDouble() ? toDouble() : double(toInt32());
  }

  JSString* toString() const {
    MOZ_ASSERT(isString());
    return unboxGCPointer<JSString, JSVAL_TAG_STRING>();
  }

  JS::Symbol* toSymbol() const {
    MOZ_ASSERT(isSymbol());
    return unboxGCPointer<JS::Symbol, JSVAL_TAG_SYMBOL>();
  }

  JS::BigInt* toBigInt() const {
    MOZ_ASSERT(isBigInt());
    return unboxGCPointer<JS::BigInt, JSVAL_TAG_BIGINT>();
  }

  JSObject& toObject() const {
    MOZ_ASSERT(isObject());
#if defined(JS_PUNBOX64)
    MOZ_ASSERT((asBits_ & detail::ValueGCThingPayloadMask) != 0);
#endif
    return *unboxGCPointer<JSObject, JSVAL_TAG_OBJECT>();
  }

  JSObject* toObjectOrNull() const {
    MOZ_ASSERT(isObjectOrNull());
#if defined(JS_NUNBOX32)
    return reinterpret_cast<JSObject*>(uintptr_t(asBits_));
#elif defined(JS_PUNBOX64)
    // Note: the 'Spectre mitigations' comment at the top of this class
    // explains why we use XOR here and in other to* methods.
    uint64_t ptrBits =
        (asBits_ ^ JSVAL_SHIFTED_TAG_OBJECT) & ~detail::ValueObjectOrNullBit;
    MOZ_ASSERT((ptrBits & 0x7) == 0);
    return reinterpret_cast<JSObject*>(ptrBits);
#endif
  }

#ifdef ENABLE_RECORD_TUPLE
  JSObject& toExtendedPrimitive() const {
    MOZ_ASSERT(isExtendedPrimitive());
#  if defined(JS_PUNBOX64)
    MOZ_ASSERT((asBits_ & detail::ValueGCThingPayloadMask) != 0);
#  endif
    return *unboxGCPointer<JSObject, JSVAL_TAG_EXTENDED_PRIMITIVE>();
  }
#endif

  JSObject& getObjectPayload() const {
#ifdef ENABLE_RECORD_TUPLE
    return isExtendedPrimitive() ? toExtendedPrimitive() : toObject();
#else
    return toObject();
#endif
  }

  js::gc::Cell* toGCThing() const {
    MOZ_ASSERT(isGCThing());
#if defined(JS_NUNBOX32)
    return reinterpret_cast<js::gc::Cell*>(uintptr_t(asBits_));
#elif defined(JS_PUNBOX64)
    uint64_t ptrBits = asBits_ & detail::ValueGCThingPayloadMask;
    MOZ_ASSERT((ptrBits & 0x7) == 0);
    return reinterpret_cast<js::gc::Cell*>(ptrBits);
#endif
  }

  GCCellPtr toGCCellPtr() const { return GCCellPtr(toGCThing(), traceKind()); }

  bool toBoolean() const {
    MOZ_ASSERT(isBoolean());
#if defined(JS_NUNBOX32)
    return bool(toNunboxPayload());
#elif defined(JS_PUNBOX64)
    return bool(asBits_ & 0x1);
#endif
  }

  constexpr uint64_t asRawBits() const { return asBits_; }

  JSValueType extractNonDoubleType() const {
    uint32_t type = toTag() & 0xF;
    MOZ_ASSERT(type > JSVAL_TYPE_DOUBLE);
    return JSValueType(type);
  }

  JS::ValueType type() const {
    if (isDouble()) {
      return JS::ValueType::Double;
    }

    JSValueType type = extractNonDoubleType();
    MOZ_ASSERT(type <= JSVAL_TYPE_OBJECT);
    return JS::ValueType(type);
  }

  /*
   * Private API
   *
   * Private setters/getters allow the caller to read/write arbitrary
   * word-size pointers or uint32s.  After storing to a value with
   * setPrivateX, it is the caller's responsibility to only read using
   * toPrivateX. Private values are given a type which ensures they
   * aren't marked by the GC.
   */

  void setPrivate(void* ptr) {
#if defined(JS_PUNBOX64)
    MOZ_ASSERT(detail::IsValidUserModePointer(uintptr_t(ptr)));
#endif
    asBits_ = uintptr_t(ptr);
    MOZ_ASSERT(isDouble());
  }

  void* toPrivate() const {
    MOZ_ASSERT(isDouble());
#if defined(JS_PUNBOX64)
    MOZ_ASSERT(detail::IsValidUserModePointer(asBits_));
#endif
    return reinterpret_cast<void*>(uintptr_t(asBits_));
  }

  void setPrivateUint32(uint32_t ui) {
    MOZ_ASSERT(uint32_t(int32_t(ui)) == ui);
    setInt32(int32_t(ui));
  }

  uint32_t toPrivateUint32() const { return uint32_t(toInt32()); }

  /*
   * Private GC Thing API
   *
   * Non-JSObject, JSString, and JS::Symbol cells may be put into the 64-bit
   * payload as private GC things. Such Values are considered isGCThing(), and
   * as such, automatically marked. Their traceKind() is gotten via their
   * cells.
   */

  void setPrivateGCThing(js::gc::Cell* cell) {
    MOZ_ASSERT(JS::GCThingTraceKind(cell) != JS::TraceKind::String,
               "Private GC thing Values must not be strings. Make a "
               "StringValue instead.");
    MOZ_ASSERT(JS::GCThingTraceKind(cell) != JS::TraceKind::Symbol,
               "Private GC thing Values must not be symbols. Make a "
               "SymbolValue instead.");
    MOZ_ASSERT(JS::GCThingTraceKind(cell) != JS::TraceKind::BigInt,
               "Private GC thing Values must not be BigInts. Make a "
               "BigIntValue instead.");
    MOZ_ASSERT(JS::GCThingTraceKind(cell) != JS::TraceKind::Object,
               "Private GC thing Values must not be objects. Make an "
               "ObjectValue instead.");

    MOZ_ASSERT(js::gc::IsCellPointerValid(cell));
#if defined(JS_PUNBOX64)
    // VisualStudio cannot contain parenthesized C++ style cast and shift
    // inside decltype in template parameter:
    //   AssertionConditionType<decltype((uintptr_t(x) >> 1))>
    // It throws syntax error.
    MOZ_ASSERT((((uintptr_t)cell) >> JSVAL_TAG_SHIFT) == 0);
#endif
    asBits_ =
        bitsFromTagAndPayload(JSVAL_TAG_PRIVATE_GCTHING, PayloadType(cell));
  }

  bool isPrivateGCThing() const { return toTag() == JSVAL_TAG_PRIVATE_GCTHING; }
} JS_HAZ_GC_POINTER MOZ_NON_PARAM;

static_assert(sizeof(Value) == 8,
              "Value size must leave three tag bits, be a binary power, and "
              "is ubiquitously depended upon everywhere");

static MOZ_ALWAYS_INLINE void ExposeValueToActiveJS(const Value& v) {
#ifdef DEBUG
  Value tmp = v;
  MOZ_ASSERT(!js::gc::EdgeNeedsSweepUnbarrieredSlow(&tmp));
#endif
  if (v.isGCThing()) {
    js::gc::ExposeGCThingToActiveJS(v.toGCCellPtr());
  }
}

/************************************************************************/

static inline MOZ_MAY_CALL_AFTER_MUST_RETURN Value NullValue() {
  Value v;
  v.setNull();
  return v;
}

static constexpr Value UndefinedValue() { return Value(); }

static constexpr Value Int32Value(int32_t i32) { return Value::fromInt32(i32); }

static inline Value DoubleValue(double dbl) {
  Value v;
  v.setDouble(dbl);
  return v;
}

static inline Value CanonicalizedDoubleValue(double d) {
  return Value::fromDouble(CanonicalizeNaN(d));
}

static inline Value NaNValue() {
  return Value::fromRawBits(detail::CanonicalizedNaNBits);
}

static inline Value InfinityValue() {
  return Value::fromRawBits(detail::InfinityBits);
}

static inline Value Float32Value(float f) {
  Value v;
  v.setDouble(f);
  return v;
}

static inline Value StringValue(JSString* str) {
  Value v;
  v.setString(str);
  return v;
}

static inline Value SymbolValue(JS::Symbol* sym) {
  Value v;
  v.setSymbol(sym);
  return v;
}

static inline Value BigIntValue(JS::BigInt* bi) {
  Value v;
  v.setBigInt(bi);
  return v;
}

static inline Value BooleanValue(bool boo) {
  Value v;
  v.setBoolean(boo);
  return v;
}

static inline Value TrueValue() {
  Value v;
  v.setBoolean(true);
  return v;
}

static inline Value FalseValue() {
  Value v;
  v.setBoolean(false);
  return v;
}

static inline Value ObjectValue(JSObject& obj) {
  Value v;
  v.setObject(obj);
  return v;
}

#ifdef ENABLE_RECORD_TUPLE
static inline Value ExtendedPrimitiveValue(JSObject& obj) {
  Value v;
  v.setExtendedPrimitive(obj);
  return v;
}
#endif

static inline Value MagicValue(JSWhyMagic why) {
  Value v;
  v.setMagic(why);
  return v;
}

static inline Value MagicValueUint32(uint32_t payload) {
  Value v;
  v.setMagicUint32(payload);
  return v;
}

static constexpr Value NumberValue(uint32_t i) {
  return i <= JSVAL_INT_MAX ? Int32Value(int32_t(i))
                            : Value::fromDouble(double(i));
}

template <typename T>
static inline Value NumberValue(const T t) {
  Value v;
  v.setNumber(t);
  return v;
}

static inline Value ObjectOrNullValue(JSObject* obj) {
  Value v;
  v.setObjectOrNull(obj);
  return v;
}

static inline Value PrivateValue(void* ptr) {
  Value v;
  v.setPrivate(ptr);
  return v;
}

static inline Value PrivateValue(uintptr_t ptr) {
  return PrivateValue(reinterpret_cast<void*>(ptr));
}

static inline Value PrivateUint32Value(uint32_t ui) {
  Value v;
  v.setPrivateUint32(ui);
  return v;
}

static inline Value PrivateGCThingValue(js::gc::Cell* cell) {
  Value v;
  v.setPrivateGCThing(cell);
  return v;
}

inline bool SameType(const Value& lhs, const Value& rhs) {
#if defined(JS_NUNBOX32)
  JSValueTag ltag = lhs.toTag(), rtag = rhs.toTag();
  return ltag == rtag || (ltag < JSVAL_TAG_CLEAR && rtag < JSVAL_TAG_CLEAR);
#elif defined(JS_PUNBOX64)
  return (lhs.isDouble() && rhs.isDouble()) ||
         (((lhs.asBits_ ^ rhs.asBits_) & 0xFFFF800000000000ULL) == 0);
#endif
}

}  // namespace JS

/************************************************************************/

namespace JS {
JS_PUBLIC_API void HeapValuePostWriteBarrier(Value* valuep, const Value& prev,
                                             const Value& next);
JS_PUBLIC_API void HeapValueWriteBarriers(Value* valuep, const Value& prev,
                                          const Value& next);

template <>
struct GCPolicy<JS::Value> {
  static void trace(JSTracer* trc, Value* v, const char* name) {
    // This should only be called as part of root marking since that's the only
    // time we should trace unbarriered GC thing pointers. This will assert if
    // called at other times.
    TraceRoot(trc, v, name);
  }
  static bool isTenured(const Value& thing) {
    return !thing.isGCThing() || !IsInsideNursery(thing.toGCThing());
  }
  static bool isValid(const Value& value) {
    return !value.isGCThing() || js::gc::IsCellPointerValid(value.toGCThing());
  }
};

}  // namespace JS

namespace js {

template <>
struct BarrierMethods<JS::Value> {
  static gc::Cell* asGCThingOrNull(const JS::Value& v) {
    return v.isGCThing() ? v.toGCThing() : nullptr;
  }
  static void postWriteBarrier(JS::Value* v, const JS::Value& prev,
                               const JS::Value& next) {
    JS::HeapValuePostWriteBarrier(v, prev, next);
  }
  static void exposeToJS(const JS::Value& v) { JS::ExposeValueToActiveJS(v); }
  static void readBarrier(const JS::Value& v) {
    if (v.isGCThing()) {
      js::gc::IncrementalReadBarrier(v.toGCCellPtr());
    }
  }
};

template <class Wrapper>
class MutableValueOperations;

/**
 * A class designed for CRTP use in implementing the non-mutating parts of the
 * Value interface in Value-like classes.  Wrapper must be a class inheriting
 * ValueOperations<Wrapper> with a visible get() method returning a const
 * reference to the Value abstracted by Wrapper.
 */
template <class Wrapper>
class WrappedPtrOperations<JS::Value, Wrapper> {
  const JS::Value& value() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  bool isUndefined() const { return value().isUndefined(); }
  bool isNull() const { return value().isNull(); }
  bool isBoolean() const { return value().isBoolean(); }
  bool isTrue() const { return value().isTrue(); }
  bool isFalse() const { return value().isFalse(); }
  bool isNumber() const { return value().isNumber(); }
  bool isInt32() const { return value().isInt32(); }
  bool isInt32(int32_t i32) const { return value().isInt32(i32); }
  bool isDouble() const { return value().isDouble(); }
  bool isString() const { return value().isString(); }
  bool isSymbol() const { return value().isSymbol(); }
  bool isBigInt() const { return value().isBigInt(); }
  bool isObject() const { return value().isObject(); }
#ifdef ENABLE_RECORD_TUPLE
  bool isExtendedPrimitive() const { return value().isExtendedPrimitive(); }
#endif
  bool hasObjectPayload() const { return value().hasObjectPayload(); }
  bool isMagic() const { return value().isMagic(); }
  bool isMagic(JSWhyMagic why) const { return value().isMagic(why); }
  bool isGCThing() const { return value().isGCThing(); }
  bool isPrivateGCThing() const { return value().isPrivateGCThing(); }
  bool isPrimitive() const { return value().isPrimitive(); }

  bool isNullOrUndefined() const { return value().isNullOrUndefined(); }
  bool isObjectOrNull() const { return value().isObjectOrNull(); }
  bool isNumeric() const { return value().isNumeric(); }

  bool toBoolean() const { return value().toBoolean(); }
  double toNumber() const { return value().toNumber(); }
  int32_t toInt32() const { return value().toInt32(); }
  double toDouble() const { return value().toDouble(); }
  JSString* toString() const { return value().toString(); }
  JS::Symbol* toSymbol() const { return value().toSymbol(); }
  JS::BigInt* toBigInt() const { return value().toBigInt(); }
  JSObject& toObject() const { return value().toObject(); }
  JSObject* toObjectOrNull() const { return value().toObjectOrNull(); }
#ifdef ENABLE_RECORD_TUPLE
  JSObject& toExtendedPrimitive() const {
    return value().toExtendedPrimitive();
  }
#endif
  JSObject& getObjectPayload() const { return value().getObjectPayload(); }
  JS::GCCellPtr toGCCellPtr() const { return value().toGCCellPtr(); }
  gc::Cell* toGCThing() const { return value().toGCThing(); }
  JS::TraceKind traceKind() const { return value().traceKind(); }
  void* toPrivate() const { return value().toPrivate(); }
  uint32_t toPrivateUint32() const { return value().toPrivateUint32(); }

  uint64_t asRawBits() const { return value().asRawBits(); }
  JSValueType extractNonDoubleType() const {
    return value().extractNonDoubleType();
  }
  JS::ValueType type() const { return value().type(); }

  JSWhyMagic whyMagic() const { return value().whyMagic(); }
  uint32_t magicUint32() const { return value().magicUint32(); }
};

/**
 * A class designed for CRTP use in implementing all the mutating parts of the
 * Value interface in Value-like classes.  Wrapper must be a class inheriting
 * MutableWrappedPtrOperations<Wrapper> with visible get() methods returning
 * const and non-const references to the Value abstracted by Wrapper.
 */
template <class Wrapper>
class MutableWrappedPtrOperations<JS::Value, Wrapper>
    : public WrappedPtrOperations<JS::Value, Wrapper> {
 protected:
  void set(const JS::Value& v) {
    // Call Wrapper::set to trigger any barriers.
    static_cast<Wrapper*>(this)->set(v);
  }

 public:
  void setNull() { set(JS::NullValue()); }
  void setUndefined() { set(JS::UndefinedValue()); }
  void setInt32(int32_t i) { set(JS::Int32Value(i)); }
  void setDouble(double d) { set(JS::DoubleValue(d)); }
  void setNaN() { set(JS::NaNValue()); }
  void setInfinity() { set(JS::InfinityValue()); }
  void setBoolean(bool b) { set(JS::BooleanValue(b)); }
  void setMagic(JSWhyMagic why) { set(JS::MagicValue(why)); }
  template <typename T>
  void setNumber(T t) {
    set(JS::NumberValue(t));
  }
  void setString(JSString* str) { set(JS::StringValue(str)); }
  void setSymbol(JS::Symbol* sym) { set(JS::SymbolValue(sym)); }
  void setBigInt(JS::BigInt* bi) { set(JS::BigIntValue(bi)); }
  void setObject(JSObject& obj) { set(JS::ObjectValue(obj)); }
  void setObjectOrNull(JSObject* arg) { set(JS::ObjectOrNullValue(arg)); }
#ifdef ENABLE_RECORD_TUPLE
  void setExtendedPrimitive(JSObject& obj) {
    return set(JS::ExtendedPrimitiveValue(obj));
  }
#endif
  void setPrivate(void* ptr) { set(JS::PrivateValue(ptr)); }
  void setPrivateUint32(uint32_t ui) { set(JS::PrivateUint32Value(ui)); }
  void setPrivateGCThing(js::gc::Cell* cell) {
    set(JS::PrivateGCThingValue(cell));
  }
};

/*
 * Augment the generic Heap<T> interface when T = Value with
 * type-querying, value-extracting, and mutating operations.
 */
template <typename Wrapper>
class HeapOperations<JS::Value, Wrapper>
    : public MutableWrappedPtrOperations<JS::Value, Wrapper> {};

MOZ_HAVE_NORETURN MOZ_COLD MOZ_NEVER_INLINE void ReportBadValueTypeAndCrash(
    const JS::Value& val);

// If the Value is a GC pointer type, call |f| with the pointer cast to that
// type and return the result wrapped in a Maybe, otherwise return None().
template <typename F>
auto MapGCThingTyped(const JS::Value& val, F&& f) {
  switch (val.type()) {
    case JS::ValueType::String: {
      JSString* str = val.toString();
      MOZ_ASSERT(gc::IsCellPointerValid(str));
      return mozilla::Some(f(str));
    }
#ifdef ENABLE_RECORD_TUPLE
    case JS::ValueType::ExtendedPrimitive:
#endif
    case JS::ValueType::Object: {
      JSObject* obj = &val.getObjectPayload();
      MOZ_ASSERT(gc::IsCellPointerValid(obj));
      return mozilla::Some(f(obj));
    }
    case JS::ValueType::Symbol: {
      JS::Symbol* sym = val.toSymbol();
      MOZ_ASSERT(gc::IsCellPointerValid(sym));
      return mozilla::Some(f(sym));
    }
    case JS::ValueType::BigInt: {
      JS::BigInt* bi = val.toBigInt();
      MOZ_ASSERT(gc::IsCellPointerValid(bi));
      return mozilla::Some(f(bi));
    }
    case JS::ValueType::PrivateGCThing: {
      MOZ_ASSERT(gc::IsCellPointerValid(val.toGCThing()));
      return mozilla::Some(MapGCThingTyped(val.toGCCellPtr(), std::move(f)));
    }
    case JS::ValueType::Double:
    case JS::ValueType::Int32:
    case JS::ValueType::Boolean:
    case JS::ValueType::Undefined:
    case JS::ValueType::Null:
    case JS::ValueType::Magic: {
      MOZ_ASSERT(!val.isGCThing());
      using ReturnType = decltype(f(static_cast<JSObject*>(nullptr)));
      return mozilla::Maybe<ReturnType>();
    }
  }

  ReportBadValueTypeAndCrash(val);
}

// If the Value is a GC pointer type, call |f| with the pointer cast to that
// type. Return whether this happened.
template <typename F>
bool ApplyGCThingTyped(const JS::Value& val, F&& f) {
  return MapGCThingTyped(val,
                         [&f](auto t) {
                           f(t);
                           return true;
                         })
      .isSome();
}

static inline JS::Value PoisonedObjectValue(uintptr_t poison) {
  JS::Value v;
  v.setObjectNoCheck(reinterpret_cast<JSObject*>(poison));
  return v;
}

}  // namespace js

#ifdef DEBUG
namespace JS {

MOZ_ALWAYS_INLINE void AssertValueIsNotGray(const Value& value) {
  if (value.isGCThing()) {
    AssertCellIsNotGray(value.toGCThing());
  }
}

MOZ_ALWAYS_INLINE void AssertValueIsNotGray(const Heap<Value>& value) {
  AssertValueIsNotGray(value.unbarrieredGet());
}

}  // namespace JS
#endif

/************************************************************************/

namespace JS {

extern JS_PUBLIC_DATA const HandleValue NullHandleValue;
extern JS_PUBLIC_DATA const HandleValue UndefinedHandleValue;
extern JS_PUBLIC_DATA const HandleValue TrueHandleValue;
extern JS_PUBLIC_DATA const HandleValue FalseHandleValue;
extern JS_PUBLIC_DATA const Handle<mozilla::Maybe<Value>> NothingHandleValue;

}  // namespace JS

#endif /* js_Value_h */
