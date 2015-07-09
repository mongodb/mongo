/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/TypedEnumBits.h"

#include <stdint.h>

// A rough feature check for is_literal_type. Not very carefully checked.
// Feel free to amend as needed.
// We leave ANDROID out because it's using stlport which doesn't have std::is_literal_type.
#if __cplusplus >= 201103L && !defined(ANDROID)
#  if defined(__clang__)
     /*
      * Per Clang documentation, "Note that marketing version numbers should not
      * be used to check for language features, as different vendors use different
      * numbering schemes. Instead, use the feature checking macros."
      */
#    ifndef __has_extension
#      define __has_extension __has_feature /* compatibility, for older versions of clang */
#    endif
#    if __has_extension(is_literal) && __has_include(<type_traits>)
#      define MOZ_HAVE_IS_LITERAL
#    endif
#  elif defined(__GNUC__) || defined(_MSC_VER)
#    define MOZ_HAVE_IS_LITERAL
#  endif
#endif

#if defined(MOZ_HAVE_IS_LITERAL) && defined(MOZ_HAVE_CXX11_CONSTEXPR)
#include <type_traits>
template<typename T>
void
RequireLiteralType()
{
  static_assert(std::is_literal_type<T>::value, "Expected a literal type");
}
#else // not MOZ_HAVE_IS_LITERAL
template<typename T>
void
RequireLiteralType()
{
}
#endif

template<typename T>
void
RequireLiteralType(const T&)
{
  RequireLiteralType<T>();
}

enum class AutoEnum {
  A,
  B = -3,
  C
};

enum class CharEnum : char {
  A,
  B = 3,
  C
};

enum class AutoEnumBitField {
  A = 0x10,
  B = 0x20,
  C
};

enum class CharEnumBitField : char {
  A = 0x10,
  B,
  C = 0x40
};

struct Nested
{
  enum class AutoEnum {
    A,
    B,
    C = -1
  };

  enum class CharEnum : char {
    A = 4,
    B,
    C = 1
  };

  enum class AutoEnumBitField {
    A,
    B = 0x20,
    C
  };

  enum class CharEnumBitField : char {
    A = 1,
    B = 1,
    C = 1
  };
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(AutoEnumBitField)
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(CharEnumBitField)
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Nested::AutoEnumBitField)
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Nested::CharEnumBitField)

#define MAKE_STANDARD_BITFIELD_FOR_TYPE(IntType)                   \
  enum class BitFieldFor_##IntType : IntType {                     \
    A = 1,                                                         \
    B = 2,                                                         \
    C = 4,                                                         \
  };                                                               \
  MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(BitFieldFor_##IntType)

MAKE_STANDARD_BITFIELD_FOR_TYPE(int8_t)
MAKE_STANDARD_BITFIELD_FOR_TYPE(uint8_t)
MAKE_STANDARD_BITFIELD_FOR_TYPE(int16_t)
MAKE_STANDARD_BITFIELD_FOR_TYPE(uint16_t)
MAKE_STANDARD_BITFIELD_FOR_TYPE(int32_t)
MAKE_STANDARD_BITFIELD_FOR_TYPE(uint32_t)
MAKE_STANDARD_BITFIELD_FOR_TYPE(int64_t)
MAKE_STANDARD_BITFIELD_FOR_TYPE(uint64_t)
MAKE_STANDARD_BITFIELD_FOR_TYPE(char)
typedef signed char signed_char;
MAKE_STANDARD_BITFIELD_FOR_TYPE(signed_char)
typedef unsigned char unsigned_char;
MAKE_STANDARD_BITFIELD_FOR_TYPE(unsigned_char)
MAKE_STANDARD_BITFIELD_FOR_TYPE(short)
typedef unsigned short unsigned_short;
MAKE_STANDARD_BITFIELD_FOR_TYPE(unsigned_short)
MAKE_STANDARD_BITFIELD_FOR_TYPE(int)
typedef unsigned int unsigned_int;
MAKE_STANDARD_BITFIELD_FOR_TYPE(unsigned_int)
MAKE_STANDARD_BITFIELD_FOR_TYPE(long)
typedef unsigned long unsigned_long;
MAKE_STANDARD_BITFIELD_FOR_TYPE(unsigned_long)
typedef long long long_long;
MAKE_STANDARD_BITFIELD_FOR_TYPE(long_long)
typedef unsigned long long unsigned_long_long;
MAKE_STANDARD_BITFIELD_FOR_TYPE(unsigned_long_long)

#undef MAKE_STANDARD_BITFIELD_FOR_TYPE

template<typename T>
void
TestNonConvertibilityForOneType()
{
  using mozilla::IsConvertible;

#if defined(MOZ_HAVE_EXPLICIT_CONVERSION)
  static_assert(!IsConvertible<T, bool>::value, "should not be convertible");
  static_assert(!IsConvertible<T, int>::value, "should not be convertible");
  static_assert(!IsConvertible<T, uint64_t>::value, "should not be convertible");
#endif

  static_assert(!IsConvertible<bool, T>::value, "should not be convertible");
  static_assert(!IsConvertible<int, T>::value, "should not be convertible");
  static_assert(!IsConvertible<uint64_t, T>::value, "should not be convertible");
}

template<typename TypedEnum>
void
TestTypedEnumBasics()
{
  const TypedEnum a = TypedEnum::A;
  int unused = int(a);
  (void) unused;
  RequireLiteralType(TypedEnum::A);
  RequireLiteralType(a);
  TestNonConvertibilityForOneType<TypedEnum>();
}

// Op wraps a bitwise binary operator, passed as a char template parameter,
// and applies it to its arguments (aT1, aT2). For example,
//
//   Op<'|'>(aT1, aT2)
//
// is the same as
//
//   aT1 | aT2.
//
template<char o, typename T1, typename T2>
auto Op(const T1& aT1, const T2& aT2)
  -> decltype(aT1 | aT2) // See the static_assert's below --- the return type
                         // depends solely on the operands type, not on the
                         // choice of operation.
{
  using mozilla::IsSame;
  static_assert(IsSame<decltype(aT1 | aT2), decltype(aT1 & aT2)>::value,
                "binary ops should have the same result type");
  static_assert(IsSame<decltype(aT1 | aT2), decltype(aT1 ^ aT2)>::value,
                "binary ops should have the same result type");

  static_assert(o == '|' ||
                o == '&' ||
                o == '^', "unexpected operator character");

  return o == '|' ? aT1 | aT2
       : o == '&' ? aT1 & aT2
                  : aT1 ^ aT2;
}

// OpAssign wraps a bitwise binary operator, passed as a char template
// parameter, and applies the corresponding compound-assignment operator to its
// arguments (aT1, aT2). For example,
//
//   OpAssign<'|'>(aT1, aT2)
//
// is the same as
//
//   aT1 |= aT2.
//
template<char o, typename T1, typename T2>
T1& OpAssign(T1& aT1, const T2& aT2)
{
  static_assert(o == '|' ||
                o == '&' ||
                o == '^', "unexpected operator character");

  switch (o) {
    case '|': return aT1 |= aT2;
    case '&': return aT1 &= aT2;
    case '^': return aT1 ^= aT2;
    default: MOZ_CRASH();
  }
}

// Tests a single binary bitwise operator, using a single set of three operands.
// The operations tested are:
//
//   result = aT1 Op aT2;
//   result Op= aT3;
//
// Where Op is the operator specified by the char template parameter 'o' and
// can be any of '|', '&', '^'.
//
// Note that the operands aT1, aT2, aT3 are intentionally passed with free
// types (separate template parameters for each) because their type may
// actually be different from TypedEnum:
//
//   1) Their type could be CastableTypedEnumResult<TypedEnum> if they are
//      the result of a bitwise operation themselves;
//   2) In the non-c++11 legacy path, the type of enum values is also
//      different from TypedEnum.
//
template<typename TypedEnum, char o, typename T1, typename T2, typename T3>
void TestBinOp(const T1& aT1, const T2& aT2, const T3& aT3)
{
  typedef typename mozilla::detail::UnsignedIntegerTypeForEnum<TypedEnum>::Type
          UnsignedIntegerType;

  // Part 1:
  // Test the bitwise binary operator i.e.
  //   result = aT1 Op aT2;
  auto result = Op<o>(aT1, aT2);

  typedef decltype(result) ResultType;

  RequireLiteralType<ResultType>();
  TestNonConvertibilityForOneType<ResultType>();

  UnsignedIntegerType unsignedIntegerResult =
    Op<o>(UnsignedIntegerType(aT1), UnsignedIntegerType(aT2));

  MOZ_RELEASE_ASSERT(unsignedIntegerResult == UnsignedIntegerType(result));
  MOZ_RELEASE_ASSERT(TypedEnum(unsignedIntegerResult) == TypedEnum(result));
  MOZ_RELEASE_ASSERT((!unsignedIntegerResult) == (!result));
  MOZ_RELEASE_ASSERT((!!unsignedIntegerResult) == (!!result));
  MOZ_RELEASE_ASSERT(bool(unsignedIntegerResult) == bool(result));

  // Part 2:
  // Test the compound-assignment operator, i.e.
  //   result Op= aT3;
  TypedEnum newResult = result;
  OpAssign<o>(newResult, aT3);
  UnsignedIntegerType unsignedIntegerNewResult = unsignedIntegerResult;
  OpAssign<o>(unsignedIntegerNewResult, UnsignedIntegerType(aT3));
  MOZ_RELEASE_ASSERT(TypedEnum(unsignedIntegerNewResult) == newResult);

  // Part 3:
  // Test additional boolean operators that we unfortunately had to add to
  // CastableTypedEnumResult at some point to please some compiler,
  // even though bool convertibility should have been enough.
  MOZ_RELEASE_ASSERT(result == TypedEnum(result));
  MOZ_RELEASE_ASSERT(!(result != TypedEnum(result)));
  MOZ_RELEASE_ASSERT((result && true) == bool(result));
  MOZ_RELEASE_ASSERT((result && false) == false);
  MOZ_RELEASE_ASSERT((true && result) == bool(result));
  MOZ_RELEASE_ASSERT((false && result && false) == false);
  MOZ_RELEASE_ASSERT((result || false) == bool(result));
  MOZ_RELEASE_ASSERT((result || true) == true);
  MOZ_RELEASE_ASSERT((false || result) == bool(result));
  MOZ_RELEASE_ASSERT((true || result) == true);
}

// Similar to TestBinOp but testing the unary ~ operator.
template<typename TypedEnum, typename T>
void TestTilde(const T& aT)
{
  typedef typename mozilla::detail::UnsignedIntegerTypeForEnum<TypedEnum>::Type
          UnsignedIntegerType;

  auto result = ~aT;

  typedef decltype(result) ResultType;

  RequireLiteralType<ResultType>();
  TestNonConvertibilityForOneType<ResultType>();

  UnsignedIntegerType unsignedIntegerResult = ~(UnsignedIntegerType(aT));

  MOZ_RELEASE_ASSERT(unsignedIntegerResult == UnsignedIntegerType(result));
  MOZ_RELEASE_ASSERT(TypedEnum(unsignedIntegerResult) == TypedEnum(result));
  MOZ_RELEASE_ASSERT((!unsignedIntegerResult) == (!result));
  MOZ_RELEASE_ASSERT((!!unsignedIntegerResult) == (!!result));
  MOZ_RELEASE_ASSERT(bool(unsignedIntegerResult) == bool(result));
}

// Helper dispatching a given triple of operands to all operator-specific
// testing functions.
template<typename TypedEnum, typename T1, typename T2, typename T3>
void TestAllOpsForGivenOperands(const T1& aT1, const T2& aT2, const T3& aT3)
{
  TestBinOp<TypedEnum, '|'>(aT1, aT2, aT3);
  TestBinOp<TypedEnum, '&'>(aT1, aT2, aT3);
  TestBinOp<TypedEnum, '^'>(aT1, aT2, aT3);
  TestTilde<TypedEnum>(aT1);
}

// Helper building various triples of operands using a given operator,
// and testing all operators with them.
template<typename TypedEnum, char o>
void TestAllOpsForOperandsBuiltUsingGivenOp()
{
  // The type of enum values like TypedEnum::A may be different from
  // TypedEnum. That is the case in the legacy non-C++11 path. We want to
  // ensure good test coverage even when these two types are distinct.
  // To that effect, we have both 'auto' typed variables, preserving the
  // original type of enum values, and 'plain' typed variables, that
  // are plain TypedEnum's.

  const TypedEnum a_plain = TypedEnum::A;
  const TypedEnum b_plain = TypedEnum::B;
  const TypedEnum c_plain = TypedEnum::C;

  auto a_auto = TypedEnum::A;
  auto b_auto = TypedEnum::B;
  auto c_auto = TypedEnum::C;

  auto ab_plain = Op<o>(a_plain, b_plain);
  auto bc_plain = Op<o>(b_plain, c_plain);
  auto ab_auto = Op<o>(a_auto, b_auto);
  auto bc_auto = Op<o>(b_auto, c_auto);

  // On each row below, we pass a triple of operands. Keep in mind that this
  // is going to be received as (aT1, aT2, aT3) and the actual tests performed
  // will be of the form
  //
  //   result = aT1 Op aT2;
  //   result Op= aT3;
  //
  // For this reason, we carefully ensure that the values of (aT1, aT2)
  // systematically cover all types of such pairs; to limit complexity,
  // we are not so careful with aT3, and we just try to pass aT3's
  // that may lead to nontrivial bitwise operations.
  TestAllOpsForGivenOperands<TypedEnum>(a_plain,  b_plain,  c_plain);
  TestAllOpsForGivenOperands<TypedEnum>(a_plain,  bc_plain, b_auto);
  TestAllOpsForGivenOperands<TypedEnum>(ab_plain, c_plain,  a_plain);
  TestAllOpsForGivenOperands<TypedEnum>(ab_plain, bc_plain, a_auto);

  TestAllOpsForGivenOperands<TypedEnum>(a_plain,  b_auto,   c_plain);
  TestAllOpsForGivenOperands<TypedEnum>(a_plain,  bc_auto,  b_auto);
  TestAllOpsForGivenOperands<TypedEnum>(ab_plain, c_auto,   a_plain);
  TestAllOpsForGivenOperands<TypedEnum>(ab_plain, bc_auto,  a_auto);

  TestAllOpsForGivenOperands<TypedEnum>(a_auto,   b_plain,  c_plain);
  TestAllOpsForGivenOperands<TypedEnum>(a_auto,   bc_plain, b_auto);
  TestAllOpsForGivenOperands<TypedEnum>(ab_auto,  c_plain,  a_plain);
  TestAllOpsForGivenOperands<TypedEnum>(ab_auto,  bc_plain, a_auto);

  TestAllOpsForGivenOperands<TypedEnum>(a_auto,   b_auto,   c_plain);
  TestAllOpsForGivenOperands<TypedEnum>(a_auto,   bc_auto,  b_auto);
  TestAllOpsForGivenOperands<TypedEnum>(ab_auto,  c_auto,   a_plain);
  TestAllOpsForGivenOperands<TypedEnum>(ab_auto,  bc_auto,  a_auto);
}

// Tests all bitwise operations on a given TypedEnum bitfield.
template<typename TypedEnum>
void
TestTypedEnumBitField()
{
  TestTypedEnumBasics<TypedEnum>();

  TestAllOpsForOperandsBuiltUsingGivenOp<TypedEnum, '|'>();
  TestAllOpsForOperandsBuiltUsingGivenOp<TypedEnum, '&'>();
  TestAllOpsForOperandsBuiltUsingGivenOp<TypedEnum, '^'>();
}

// Checks that enum bitwise expressions have the same non-convertibility
// properties as c++11 enum classes do, i.e. not implicitly convertible to
// anything (though *explicitly* convertible).
void TestNoConversionsBetweenUnrelatedTypes()
{
  using mozilla::IsConvertible;

  // Two typed enum classes having the same underlying integer type, to ensure
  // that we would catch bugs accidentally allowing conversions in that case.
  typedef CharEnumBitField T1;
  typedef Nested::CharEnumBitField T2;

  static_assert(!IsConvertible<T1, T2>::value,
                "should not be convertible");
  static_assert(!IsConvertible<T1, decltype(T2::A)>::value,
                "should not be convertible");
  static_assert(!IsConvertible<T1, decltype(T2::A | T2::B)>::value,
                "should not be convertible");

  static_assert(!IsConvertible<decltype(T1::A), T2>::value,
                "should not be convertible");
  static_assert(!IsConvertible<decltype(T1::A), decltype(T2::A)>::value,
                "should not be convertible");
  static_assert(!IsConvertible<decltype(T1::A), decltype(T2::A | T2::B)>::value,
                "should not be convertible");

  // The following are #ifdef MOZ_HAVE_EXPLICIT_CONVERSION because without
  // support for explicit conversion operators, we can't easily have these bad
  // conversions completely removed. They still do fail to compile in practice,
  // but not in a way that we can static_assert on.
#ifdef MOZ_HAVE_EXPLICIT_CONVERSION
  static_assert(!IsConvertible<decltype(T1::A | T1::B), T2>::value,
                "should not be convertible");
  static_assert(!IsConvertible<decltype(T1::A | T1::B), decltype(T2::A)>::value,
                "should not be convertible");
  static_assert(!IsConvertible<decltype(T1::A | T1::B), decltype(T2::A | T2::B)>::value,
                "should not be convertible");
#endif
}

enum class Int8EnumWithHighBits : int8_t {
  A = 0x20,
  B = 0x40
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Int8EnumWithHighBits)

enum class Uint8EnumWithHighBits : uint8_t {
  A = 0x40,
  B = 0x80
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Uint8EnumWithHighBits)

enum class Int16EnumWithHighBits : int16_t {
  A = 0x2000,
  B = 0x4000
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Int16EnumWithHighBits)

enum class Uint16EnumWithHighBits : uint16_t {
  A = 0x4000,
  B = 0x8000
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Uint16EnumWithHighBits)

enum class Int32EnumWithHighBits : int32_t {
  A = 0x20000000,
  B = 0x40000000
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Int32EnumWithHighBits)

enum class Uint32EnumWithHighBits : uint32_t {
  A = 0x40000000u,
  B = 0x80000000u
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Uint32EnumWithHighBits)

enum class Int64EnumWithHighBits : int64_t {
  A = 0x2000000000000000ll,
  B = 0x4000000000000000ll
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Int64EnumWithHighBits)

enum class Uint64EnumWithHighBits : uint64_t {
  A = 0x4000000000000000ull,
  B = 0x8000000000000000ull
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Uint64EnumWithHighBits)

// Checks that we don't accidentally truncate high bits by coercing to the wrong
// integer type internally when implementing bitwise ops.
template<typename EnumType, typename IntType>
void TestIsNotTruncated()
{
  EnumType a = EnumType::A;
  EnumType b = EnumType::B;
  MOZ_RELEASE_ASSERT(IntType(a));
  MOZ_RELEASE_ASSERT(IntType(b));
  MOZ_RELEASE_ASSERT(a | EnumType::B);
  MOZ_RELEASE_ASSERT(a | b);
  MOZ_RELEASE_ASSERT(EnumType::A | EnumType::B);
  EnumType c = EnumType::A | EnumType::B;
  MOZ_RELEASE_ASSERT(IntType(c));
  MOZ_RELEASE_ASSERT(c & c);
  MOZ_RELEASE_ASSERT(c | c);
  MOZ_RELEASE_ASSERT(c == (EnumType::A | EnumType::B));
  MOZ_RELEASE_ASSERT(a != (EnumType::A | EnumType::B));
  MOZ_RELEASE_ASSERT(b != (EnumType::A | EnumType::B));
  MOZ_RELEASE_ASSERT(c & EnumType::A);
  MOZ_RELEASE_ASSERT(c & EnumType::B);
  EnumType d = EnumType::A;
  d |= EnumType::B;
  MOZ_RELEASE_ASSERT(d == c);
}

int
main()
{
  TestTypedEnumBasics<AutoEnum>();
  TestTypedEnumBasics<CharEnum>();
  TestTypedEnumBasics<Nested::AutoEnum>();
  TestTypedEnumBasics<Nested::CharEnum>();

  TestTypedEnumBitField<AutoEnumBitField>();
  TestTypedEnumBitField<CharEnumBitField>();
  TestTypedEnumBitField<Nested::AutoEnumBitField>();
  TestTypedEnumBitField<Nested::CharEnumBitField>();

  TestTypedEnumBitField<BitFieldFor_uint8_t>();
  TestTypedEnumBitField<BitFieldFor_int8_t>();
  TestTypedEnumBitField<BitFieldFor_uint16_t>();
  TestTypedEnumBitField<BitFieldFor_int16_t>();
  TestTypedEnumBitField<BitFieldFor_uint32_t>();
  TestTypedEnumBitField<BitFieldFor_int32_t>();
  TestTypedEnumBitField<BitFieldFor_uint64_t>();
  TestTypedEnumBitField<BitFieldFor_int64_t>();
  TestTypedEnumBitField<BitFieldFor_char>();
  TestTypedEnumBitField<BitFieldFor_signed_char>();
  TestTypedEnumBitField<BitFieldFor_unsigned_char>();
  TestTypedEnumBitField<BitFieldFor_short>();
  TestTypedEnumBitField<BitFieldFor_unsigned_short>();
  TestTypedEnumBitField<BitFieldFor_int>();
  TestTypedEnumBitField<BitFieldFor_unsigned_int>();
  TestTypedEnumBitField<BitFieldFor_long>();
  TestTypedEnumBitField<BitFieldFor_unsigned_long>();
  TestTypedEnumBitField<BitFieldFor_long_long>();
  TestTypedEnumBitField<BitFieldFor_unsigned_long_long>();

  TestNoConversionsBetweenUnrelatedTypes();

  TestIsNotTruncated<Int8EnumWithHighBits, int8_t>();
  TestIsNotTruncated<Int16EnumWithHighBits, int16_t>();
  TestIsNotTruncated<Int32EnumWithHighBits, int32_t>();
  TestIsNotTruncated<Int64EnumWithHighBits, int64_t>();
  TestIsNotTruncated<Uint8EnumWithHighBits, uint8_t>();
  TestIsNotTruncated<Uint16EnumWithHighBits, uint16_t>();
  TestIsNotTruncated<Uint32EnumWithHighBits, uint32_t>();
  TestIsNotTruncated<Uint64EnumWithHighBits, uint64_t>();

  return 0;
}
