/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * UTF-8-related functionality, including a type-safe structure representing a
 * UTF-8 code unit.
 */

#ifndef mozilla_Utf8_h
#define mozilla_Utf8_h

#include "mozilla/Casting.h"    // for mozilla::AssertedCast
#include "mozilla/Likely.h"     // for MOZ_UNLIKELY
#include "mozilla/Maybe.h"      // for mozilla::Maybe
#include "mozilla/Span.h"       // for mozilla::Span
#include "mozilla/TextUtils.h"  // for mozilla::IsAscii and via Latin1.h for
                                // encoding_rs_mem.h and MOZ_HAS_JSRUST.
#include "mozilla/Tuple.h"      // for mozilla::Tuple
#include "mozilla/Types.h"      // for MFBT_API

#include <limits>    // for CHAR_BIT / std::numeric_limits
#include <stddef.h>  // for size_t
#include <stdint.h>  // for uint8_t

#if MOZ_HAS_JSRUST()
// Can't include mozilla/Encoding.h here.
extern "C" {
// Declared as uint8_t instead of char to match declaration in another header.
size_t encoding_utf8_valid_up_to(uint8_t const* buffer, size_t buffer_len);
}
#else
namespace mozilla {
namespace detail {
extern MFBT_API bool IsValidUtf8(const void* aCodeUnits, size_t aCount);
};  // namespace detail
};  // namespace mozilla
#endif  // MOZ_HAS_JSRUST

namespace mozilla {

union Utf8Unit;

static_assert(CHAR_BIT == 8,
              "Utf8Unit won't work so well with non-octet chars");

/**
 * A code unit within a UTF-8 encoded string.  (A code unit is the smallest
 * unit within the Unicode encoding of a string.  For UTF-8 this is an 8-bit
 * number; for UTF-16 it would be a 16-bit number.)
 *
 * This is *not* the same as a single code point: in UTF-8, non-ASCII code
 * points are constituted by multiple code units.
 */
union Utf8Unit {
 private:
  // Utf8Unit is a union wrapping a raw |char|.  The C++ object model and C++
  // requirements as to how objects may be accessed with respect to their actual
  // types (almost?) uniquely compel this choice.
  //
  // Our requirements for a UTF-8 code unit representation are:
  //
  //   1. It must be "compatible" with C++ character/string literals that use
  //      the UTF-8 encoding.  Given a properly encoded C++ literal, you should
  //      be able to use |Utf8Unit| and friends to access it; given |Utf8Unit|
  //      and friends (particularly UnicodeData), you should be able to access
  //      C++ character types for their contents.
  //   2. |Utf8Unit| and friends must convert to/from |char| and |char*| only by
  //      explicit operation.
  //   3. |Utf8Unit| must participate in overload resolution and template type
  //      equivalence (that is, given |template<class> class X|, when |X<T>| and
  //      |X<U>| are the same type) distinctly from the C++ character types.
  //
  // And a few nice-to-haves (at least for the moment):
  //
  //   4. The representation should use unsigned numbers, to avoid undefined
  //      behavior that can arise with signed types, and because Unicode code
  //      points and code units are unsigned.
  //   5. |Utf8Unit| and friends should be convertible to/from |unsigned char|
  //      and |unsigned char*|, for APIs that (because of #4 above) use those
  //      types as the "natural" choice for UTF-8 data.
  //
  // #1 requires that |Utf8Unit| "incorporate" a C++ character type: one of
  // |{,{un,}signed} char|.[0]  |uint8_t| won't work because it might not be a
  // C++ character type.
  //
  // #2 and #3 mean that |Utf8Unit| can't *be* such a type (or a typedef to one:
  // typedefs don't generate *new* types, just type aliases).  This requires a
  // compound type.
  //
  // The ultimate representation (and character type in it) is constrained by
  // C++14 [basic.lval]p10 that defines how objects may be accessed, with
  // respect to the dynamic type in memory and the actual type used to access
  // them.  It reads:
  //
  //     If a program attempts to access the stored value of an object
  //     through a glvalue of other than one of the following types the
  //     behavior is undefined:
  //
  //       1. the dynamic type of the object,
  //       2. a cv-qualified version of the dynamic type of the object,
  //       ...other types irrelevant here...
  //       3. an aggregate or union type that includes one of the
  //          aforementioned types among its elements or non-static data
  //          members (including, recursively, an element or non-static
  //          data member of a subaggregate or contained union),
  //       ...more irrelevant types...
  //       4. a char or unsigned char type.
  //
  // Accessing (wrapped) UTF-8 data as |char|/|unsigned char| is allowed no
  // matter the representation by #4.  (Briefly set aside what values are seen.)
  // (And #2 allows |const| on either the dynamic type or the accessing type.)
  // (|signed char| is really only useful for small signed numbers, not
  // characters, so we ignore it.)
  //
  // If we interpret contents as |char|/|unsigned char| contrary to the actual
  // type stored there, what happens?  C++14 [basic.fundamental]p1 requires
  // character types be identically aligned/sized; C++14 [basic.fundamental]p3
  // requires |signed char| and |unsigned char| have the same value
  // representation.  C++ doesn't require identical bitwise representation, tho.
  // Practically we could assume it, but this verges on C++ spec bits best not
  // *relied* on for correctness, if possible.
  //
  // So we don't expose |Utf8Unit|'s contents as |unsigned char*|: only |char|
  // and |char*|.  Instead we safely expose |unsigned char| by fully-defined
  // *integral conversion* (C++14 [conv.integral]p2).  Integral conversion from
  // |unsigned char| → |char| has only implementation-defined behavior.  It'd be
  // better not to depend on that, but given twos-complement won, it should be
  // okay.  (Also |unsigned char*| is awkward enough to work with for strings
  // that it probably doesn't appear in string manipulation much anyway, only in
  // places that should really use |Utf8Unit| directly.)
  //
  // The opposite direction -- interpreting |char| or |char*| data through
  // |Utf8Unit| -- isn't tricky as long as |Utf8Unit| contains a |char| as
  // decided above, using #3.  An "aggregate or union" will work that contains a
  // |char|.  Oddly, an aggregate won't work: C++14 [dcl.init.aggr]p1 says
  // aggregates must have "no private or protected non-static data members", and
  // we want to keep the inner |char| hidden.  So a |struct| is out, and only
  // |union| remains.
  //
  // (Enums are not "an aggregate or union type", so [maybe surprisingly] we
  // can't make |Utf8Unit| an enum class with |char| underlying type, because we
  // are given no license to treat |char| memory as such an |enum|'s memory.)
  //
  // Therefore |Utf8Unit| is a union type with a |char| non-static data member.
  // This satisfies all our requirements.  It also supports the nice-to-haves of
  // creating a |Utf8Unit| from an |unsigned char|, and being convertible to
  // |unsigned char|.  It doesn't satisfy the nice-to-haves of using an
  // |unsigned char| internally, nor of letting us wrap an existing
  // |unsigned char| or pointer to one.  We probably *could* do these, if we
  // were willing to rely harder on implementation-defined behaviors, but for
  // now we privilege C++'s main character type over some conceptual purity.
  //
  // 0. There's a proposal for a UTF-8 character type distinct from the existing
  //    C++ narrow character types:
  //
  //      http://open-std.org/JTC1/SC22/WG21/docs/papers/2016/p0482r0.html
  //
  //    but it hasn't been standardized (and might never be), and none of the
  //    compilers we really care about have implemented it.  Maybe someday we
  //    can change our implementation to it without too much trouble, if we're
  //    lucky...
  char mValue = '\0';

 public:
  Utf8Unit() = default;

  explicit constexpr Utf8Unit(char aUnit) : mValue(aUnit) {}

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811

  explicit constexpr Utf8Unit(char8_t aUnit)
      : Utf8Unit(static_cast<char>(aUnit)) {}

#endif

  explicit constexpr Utf8Unit(unsigned char aUnit)
      : mValue(static_cast<char>(aUnit)) {
    // Per the above comment, the prior cast is integral conversion with
    // implementation-defined semantics, and we regretfully but unavoidably
    // assume the conversion does what we want it to.
  }

  constexpr bool operator==(const Utf8Unit& aOther) const {
    return mValue == aOther.mValue;
  }

  constexpr bool operator!=(const Utf8Unit& aOther) const {
    return !(*this == aOther);
  }

  /** Convert a UTF-8 code unit to a raw char. */
  constexpr char toChar() const {
    // Only a |char| is ever permitted to be written into this location, so this
    // is both permissible and returns the desired value.
    return mValue;
  }

  /** Convert a UTF-8 code unit to a raw unsigned char. */
  constexpr unsigned char toUnsignedChar() const {
    // Per the above comment, this is well-defined integral conversion.
    return static_cast<unsigned char>(mValue);
  }

  /** Convert a UTF-8 code unit to a uint8_t. */
  constexpr uint8_t toUint8() const {
    // Per the above comment, this is well-defined integral conversion.
    return static_cast<uint8_t>(mValue);
  }

  // We currently don't expose |&mValue|.  |UnicodeData| sort of does, but
  // that's a somewhat separate concern, justified in different comments in
  // that other code.
};

/**
 * Reinterpret the address of a UTF-8 code unit as |const unsigned char*|.
 *
 * Assuming proper backing has been set up, the resulting |const unsigned char*|
 * may validly be dereferenced.
 *
 * No access is provided to mutate this underlying memory as |unsigned char|.
 * Presently memory inside |Utf8Unit| is *only* stored as |char|, and we are
 * loath to offer a way to write non-|char| data until absolutely necessary.
 */
inline const unsigned char* Utf8AsUnsignedChars(const Utf8Unit* aUnits) {
  static_assert(sizeof(Utf8Unit) == sizeof(unsigned char),
                "sizes must match to permissibly reinterpret_cast<>");
  static_assert(alignof(Utf8Unit) == alignof(unsigned char),
                "alignment must match to permissibly reinterpret_cast<>");

  // The static_asserts above only enable the reinterpret_cast<> to occur.
  //
  // Dereferencing the resulting pointer is a separate question.  Any object's
  // memory may be interpreted as |unsigned char| per C++11 [basic.lval]p10, but
  // this doesn't guarantee what values will be observed.  If |char| is
  // implemented to act like |unsigned char|, we're good to go: memory for the
  // |char| in |Utf8Unit| acts as we need.  But if |char| is implemented to act
  // like |signed char|, dereferencing produces the right value only if the
  // |char| types all use two's-complement representation.  Every modern
  // compiler does this, and there's a C++ proposal to standardize it.
  // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0907r0.html   So
  // *technically* this is implementation-defined -- but everyone does it and
  // this behavior is being standardized.
  return reinterpret_cast<const unsigned char*>(aUnits);
}

/** Returns true iff |aUnit| is an ASCII value. */
constexpr bool IsAscii(Utf8Unit aUnit) {
  return IsAscii(aUnit.toUnsignedChar());
}

/**
 * Return true if the given span of memory consists of a valid UTF-8
 * string and false otherwise.
 *
 * The string *may* contain U+0000 NULL code points.
 */
namespace detail {
inline bool IsUtf8(const uint8_t* ptr, size_t length) {
#if MOZ_HAS_JSRUST()
  // For short strings, the function call is a pessimization, and the SIMD
  // code won't have a chance to kick in anyway.
  if (length < 16) {
    for (size_t i = 0; i < length; i++) {
      if (ptr[i] >= 0x80U) {
        ptr += i;
        length -= i;
        goto end;
      }
    }
    return true;
  }
end:
  return length == encoding_utf8_valid_up_to(ptr, length);
#else
  return detail::IsValidUtf8(ptr, length);
#endif
}
}  // namespace detail

inline bool IsUtf8(mozilla::Span<const char> aString) {
  return detail::IsUtf8(reinterpret_cast<const uint8_t*>(aString.Elements()),
                        aString.Length());
}

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811

inline bool IsUtf8(mozilla::Span<const char8_t> aString) {
  return detail::IsUtf8(reinterpret_cast<const uint8_t*>(aString.Elements()),
                        aString.Length());
}

#endif

#if MOZ_HAS_JSRUST()

// See Latin1.h for conversions between Latin1 and UTF-8.

/**
 * Returns the index of the start of the first malformed byte
 * sequence or the length of the string if there are none.
 */
inline size_t Utf8ValidUpTo(mozilla::Span<const char> aString) {
  return encoding_utf8_valid_up_to(
      reinterpret_cast<const uint8_t*>(aString.Elements()), aString.Length());
}

/**
 * Converts potentially-invalid UTF-16 to UTF-8 replacing lone surrogates
 * with the REPLACEMENT CHARACTER.
 *
 * The length of aDest must be at least the length of aSource times three.
 *
 * Returns the number of code units written.
 */
inline size_t ConvertUtf16toUtf8(mozilla::Span<const char16_t> aSource,
                                 mozilla::Span<char> aDest) {
  return encoding_mem_convert_utf16_to_utf8(
      aSource.Elements(), aSource.Length(), aDest.Elements(), aDest.Length());
}

/**
 * Converts potentially-invalid UTF-8 to UTF-16 replacing malformed byte
 * sequences with the REPLACEMENT CHARACTER with potentially insufficient
 * output space.
 *
 * Returns the number of code units read and the number of bytes written.
 *
 * If the output isn't large enough, not all input is consumed.
 *
 * The conversion is guaranteed to be complete if the length of aDest is
 * at least the length of aSource times three.
 *
 * The output is always valid UTF-8 ending on scalar value boundary
 * even in the case of partial conversion.
 *
 * The semantics of this function match the semantics of
 * TextEncoder.encodeInto.
 * https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto
 */
inline mozilla::Tuple<size_t, size_t> ConvertUtf16toUtf8Partial(
    mozilla::Span<const char16_t> aSource, mozilla::Span<char> aDest) {
  size_t srcLen = aSource.Length();
  size_t dstLen = aDest.Length();
  encoding_mem_convert_utf16_to_utf8_partial(aSource.Elements(), &srcLen,
                                             aDest.Elements(), &dstLen);
  return mozilla::MakeTuple(srcLen, dstLen);
}

/**
 * Converts potentially-invalid UTF-8 to UTF-16 replacing malformed byte
 * sequences with the REPLACEMENT CHARACTER.
 *
 * Returns the number of code units written.
 *
 * The length of aDest must be at least one greater than the length of aSource
 * even though the last slot isn't written to.
 *
 * If you know that the input is valid for sure, use
 * UnsafeConvertValidUtf8toUtf16() instead.
 */
inline size_t ConvertUtf8toUtf16(mozilla::Span<const char> aSource,
                                 mozilla::Span<char16_t> aDest) {
  return encoding_mem_convert_utf8_to_utf16(
      aSource.Elements(), aSource.Length(), aDest.Elements(), aDest.Length());
}

/**
 * Converts known-valid UTF-8 to UTF-16. If the input might be invalid,
 * use ConvertUtf8toUtf16() or ConvertUtf8toUtf16WithoutReplacement() instead.
 *
 * Returns the number of code units written.
 *
 * The length of aDest must be at least the length of aSource.
 */
inline size_t UnsafeConvertValidUtf8toUtf16(mozilla::Span<const char> aSource,
                                            mozilla::Span<char16_t> aDest) {
  return encoding_mem_convert_utf8_to_utf16(
      aSource.Elements(), aSource.Length(), aDest.Elements(), aDest.Length());
}

/**
 * Converts potentially-invalid UTF-8 to valid UTF-16 signaling on error.
 *
 * Returns the number of code units written or `mozilla::Nothing` if the
 * input was invalid.
 *
 * The length of the destination buffer must be at least the length of the
 * source buffer.
 *
 * When the input was invalid, some output may have been written.
 *
 * If you know that the input is valid for sure, use
 * UnsafeConvertValidUtf8toUtf16() instead.
 */
inline mozilla::Maybe<size_t> ConvertUtf8toUtf16WithoutReplacement(
    mozilla::Span<const char> aSource, mozilla::Span<char16_t> aDest) {
  size_t written = encoding_mem_convert_utf8_to_utf16_without_replacement(
      aSource.Elements(), aSource.Length(), aDest.Elements(), aDest.Length());
  if (MOZ_UNLIKELY(written == std::numeric_limits<size_t>::max())) {
    return mozilla::Nothing();
  }
  return mozilla::Some(written);
}

#else  // The code below is implemented based on the equivalent specification in
       // `encoding_rs`.

// See Latin1.h for conversions between Latin1 and UTF-8.

/**
 * Returns the index of the start of the first malformed byte
 * sequence or the length of the string if there are none.
 */
inline size_t Utf8ValidUpTo(mozilla::Span<const char> aString) {
  return Utf8ValidUpToIndex(aString);
}

#  define SINGLE_BYTE_REPLACEMENT_CHAR "?"
#  define DOUBLE_BYTE_REPLACEMENT_CHAR "\u00BF"
#  define TRIPLE_BYTE_REPLACEMENT_CHAR "\uFFFD"

/**
 * Converts potentially-invalid UTF-16 to UTF-8 replacing malformed byte
 * sequences with the REPLACEMENT CHARACTER with potentially insufficient
 * output space.
 *
 * Returns the number of code units read and the number of bytes written.
 *
 * If the output isn't large enough, not all input is consumed.
 *
 * The conversion is guaranteed to be complete if the length of aDest is
 * at least the length of aSource times three.
 *
 * The output is always valid UTF-8 ending on scalar value boundary
 * even in the case of partial conversion.
 *
 * The semantics of this function match the semantics of
 * TextEncoder.encodeInto.
 * https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto
 */
mozilla::Tuple<size_t, size_t> ConvertUtf16toUtf8Partial(
    mozilla::Span<const char16_t> aSource, mozilla::Span<char> aDest);

/**
 * Converts potentially-invalid UTF-16 to UTF-8 replacing lone surrogates
 * with the REPLACEMENT CHARACTER.
 *
 * The length of aDest must be at least the length of aSource times three.
 *
 * Returns the number of code units written.
 */
size_t ConvertUtf16toUtf8(mozilla::Span<const char16_t> aSource,
                          mozilla::Span<char> aDest);

/**
 * Converts potentially-invalid UTF-8 to UTF-16 replacing malformed byte
 * sequences with the REPLACEMENT CHARACTER.
 *
 * Returns the number of code units written.
 *
 * The length of aDest must be at least one greater than the length of aSource
 * even though the last slot isn't written to.
 *
 * If you know that the input is valid for sure, use
 * UnsafeConvertValidUtf8toUtf16() instead.
 */
size_t ConvertUtf8toUtf16(mozilla::Span<const char> aSource,
                          mozilla::Span<char16_t> aDest);

/**
 * Converts known-valid UTF-8 to UTF-16. If the input might be invalid,
 * use ConvertUtf8toUtf16() or ConvertUtf8toUtf16WithoutReplacement() instead.
 *
 * Returns the number of code units written.
 *
 * The length of aDest must be at least the length of aSource.
 */
size_t UnsafeConvertValidUtf8toUtf16(mozilla::Span<const char> aSource,
                                     mozilla::Span<char16_t> aDest);

/**
 * Converts potentially-invalid UTF-8 to valid UTF-16 signaling on error.
 *
 * Returns the number of code units written or `mozilla::Nothing` if the
 * input was invalid.
 *
 * The length of the destination buffer must be at least the length of the
 * source buffer.
 *
 * When the input was invalid, some output may have been written.
 *
 * If you know that the input is valid for sure, use
 * UnsafeConvertValidUtf8toUtf16() instead.
 */
inline mozilla::Maybe<size_t> ConvertUtf8toUtf16WithoutReplacement(
    mozilla::Span<const char> aSource, mozilla::Span<char16_t> aDest) {
  size_t utf8ValidUpToResult = Utf8ValidUpToIndex(aSource);
  if (MOZ_UNLIKELY(utf8ValidUpToResult != aSource.Length())) {
    return mozilla::Nothing();
  }
  return mozilla::Some(UnsafeConvertValidUtf8toUtf16(aSource, aDest));
}

#endif  // MOZ_HAS_JSRUST

/**
 * Returns true iff |aUnit| is a UTF-8 trailing code unit matching the pattern
 * 0b10xx'xxxx.
 */
inline bool IsTrailingUnit(Utf8Unit aUnit) {
  return (aUnit.toUint8() & 0b1100'0000) == 0b1000'0000;
}

/**
 * Given |aLeadUnit| that is a non-ASCII code unit, a pointer to an |Iter aIter|
 * that (initially) itself points one unit past |aLeadUnit|, and
 * |const EndIter& aEnd| that denotes the end of the UTF-8 data when compared
 * against |*aIter| using |aEnd - *aIter|:
 *
 * If |aLeadUnit| and subsequent code units computed using |*aIter| (up to
 * |aEnd|) encode a valid code point -- not exceeding Unicode's range, not a
 * surrogate, in shortest form -- then return Some(that code point) and advance
 * |*aIter| past those code units.
 *
 * Otherwise decrement |*aIter| (so that it points at |aLeadUnit|) and return
 * Nothing().
 *
 * |Iter| and |EndIter| are generalized concepts most easily understood as if
 * they were |const char*|, |const unsigned char*|, or |const Utf8Unit*|:
 * iterators that when dereferenced can be used to construct a |Utf8Unit| and
 * that can be compared and modified in certain limited ways.  (Carefully note
 * that this function mutates |*aIter|.)  |Iter| and |EndIter| are template
 * parameters to support more-complicated adaptor iterators.
 *
 * The template parameters after |Iter| allow users to implement custom handling
 * for various forms of invalid UTF-8.  A version of this function that defaults
 * all such handling to no-ops is defined below this function.  To learn how to
 * define your own custom handling, consult the implementation of that function,
 * which documents exactly how custom handler functors are invoked.
 *
 * This function is MOZ_ALWAYS_INLINE: if you don't need that, use the version
 * of this function without the "Inline" suffix on the name.
 */
template <typename Iter, typename EndIter, class OnBadLeadUnit,
          class OnNotEnoughUnits, class OnBadTrailingUnit, class OnBadCodePoint,
          class OnNotShortestForm>
MOZ_ALWAYS_INLINE Maybe<char32_t> DecodeOneUtf8CodePointInline(
    const Utf8Unit aLeadUnit, Iter* aIter, const EndIter& aEnd,
    OnBadLeadUnit aOnBadLeadUnit, OnNotEnoughUnits aOnNotEnoughUnits,
    OnBadTrailingUnit aOnBadTrailingUnit, OnBadCodePoint aOnBadCodePoint,
    OnNotShortestForm aOnNotShortestForm) {
  MOZ_ASSERT(Utf8Unit((*aIter)[-1]) == aLeadUnit);

  char32_t n = aLeadUnit.toUint8();
  MOZ_ASSERT(!IsAscii(n));

  // |aLeadUnit| determines the number of trailing code units in the code point
  // and the bits of |aLeadUnit| that contribute to the code point's value.
  uint8_t remaining;
  uint32_t min;
  if ((n & 0b1110'0000) == 0b1100'0000) {
    remaining = 1;
    min = 0x80;
    n &= 0b0001'1111;
  } else if ((n & 0b1111'0000) == 0b1110'0000) {
    remaining = 2;
    min = 0x800;
    n &= 0b0000'1111;
  } else if ((n & 0b1111'1000) == 0b1111'0000) {
    remaining = 3;
    min = 0x10000;
    n &= 0b0000'0111;
  } else {
    *aIter -= 1;
    aOnBadLeadUnit();
    return Nothing();
  }

  // If the code point would require more code units than remain, the encoding
  // is invalid.
  auto actual = aEnd - *aIter;
  if (MOZ_UNLIKELY(actual < remaining)) {
    *aIter -= 1;
    aOnNotEnoughUnits(AssertedCast<uint8_t>(actual + 1), remaining + 1);
    return Nothing();
  }

  for (uint8_t i = 0; i < remaining; i++) {
    const Utf8Unit unit(*(*aIter)++);

    // Every non-leading code unit in properly encoded UTF-8 has its high
    // bit set and the next-highest bit unset.
    if (MOZ_UNLIKELY(!IsTrailingUnit(unit))) {
      uint8_t unitsObserved = i + 1 + 1;
      *aIter -= unitsObserved;
      aOnBadTrailingUnit(unitsObserved);
      return Nothing();
    }

    // The code point being encoded is the concatenation of all the
    // unconstrained bits.
    n = (n << 6) | (unit.toUint8() & 0b0011'1111);
  }

  // UTF-16 surrogates and values outside the Unicode range are invalid.
  if (MOZ_UNLIKELY(n > 0x10FFFF || (0xD800 <= n && n <= 0xDFFF))) {
    uint8_t unitsObserved = remaining + 1;
    *aIter -= unitsObserved;
    aOnBadCodePoint(n, unitsObserved);
    return Nothing();
  }

  // Overlong code points are also invalid.
  if (MOZ_UNLIKELY(n < min)) {
    uint8_t unitsObserved = remaining + 1;
    *aIter -= unitsObserved;
    aOnNotShortestForm(n, unitsObserved);
    return Nothing();
  }

  return Some(n);
}

/**
 * Identical to the above function, but not forced to be instantiated inline --
 * the compiler is permitted to common up separate invocations if it chooses.
 */
template <typename Iter, typename EndIter, class OnBadLeadUnit,
          class OnNotEnoughUnits, class OnBadTrailingUnit, class OnBadCodePoint,
          class OnNotShortestForm>
inline Maybe<char32_t> DecodeOneUtf8CodePoint(
    const Utf8Unit aLeadUnit, Iter* aIter, const EndIter& aEnd,
    OnBadLeadUnit aOnBadLeadUnit, OnNotEnoughUnits aOnNotEnoughUnits,
    OnBadTrailingUnit aOnBadTrailingUnit, OnBadCodePoint aOnBadCodePoint,
    OnNotShortestForm aOnNotShortestForm) {
  return DecodeOneUtf8CodePointInline(aLeadUnit, aIter, aEnd, aOnBadLeadUnit,
                                      aOnNotEnoughUnits, aOnBadTrailingUnit,
                                      aOnBadCodePoint, aOnNotShortestForm);
}

/**
 * Like the always-inlined function above, but with no-op behavior from all
 * trailing if-invalid notifier functors.
 *
 * This function is MOZ_ALWAYS_INLINE: if you don't need that, use the version
 * of this function without the "Inline" suffix on the name.
 */
template <typename Iter, typename EndIter>
MOZ_ALWAYS_INLINE Maybe<char32_t> DecodeOneUtf8CodePointInline(
    const Utf8Unit aLeadUnit, Iter* aIter, const EndIter& aEnd) {
  // aOnBadLeadUnit is called when |aLeadUnit| itself is an invalid lead unit in
  // a multi-unit code point.  It is passed no arguments: the caller already has
  // |aLeadUnit| on hand, so no need to provide it again.
  auto onBadLeadUnit = []() {};

  // aOnNotEnoughUnits is called when |aLeadUnit| properly indicates a code
  // point length, but there aren't enough units from |*aIter| to |aEnd| to
  // satisfy that length.  It is passed the number of code units actually
  // available (according to |aEnd - *aIter|) and the number of code units that
  // |aLeadUnit| indicates are needed.  Both numbers include the contribution
  // of |aLeadUnit| itself: so |aUnitsAvailable <= 3|, |aUnitsNeeded <= 4|, and
  // |aUnitsAvailable < aUnitsNeeded|.  As above, it also is not passed the lead
  // code unit.
  auto onNotEnoughUnits = [](uint8_t aUnitsAvailable, uint8_t aUnitsNeeded) {};

  // aOnBadTrailingUnit is called when one of the trailing code units implied by
  // |aLeadUnit| doesn't match the 0b10xx'xxxx bit pattern that all UTF-8
  // trailing code units must satisfy.  It is passed the total count of units
  // observed (including |aLeadUnit|).  The bad trailing code unit will
  // conceptually be at |(*aIter)[aUnitsObserved - 1]| if this functor is
  // called, and so |aUnitsObserved <= 4|.
  auto onBadTrailingUnit = [](uint8_t aUnitsObserved) {};

  // aOnBadCodePoint is called when a structurally-correct code point encoding
  // is found, but the *value* that is encoded is not a valid code point: either
  // because it exceeded the U+10FFFF Unicode maximum code point, or because it
  // was a UTF-16 surrogate.  It is passed the non-code point value and the
  // number of code units used to encode it.
  auto onBadCodePoint = [](char32_t aBadCodePoint, uint8_t aUnitsObserved) {};

  // aOnNotShortestForm is called when structurally-correct encoding is found,
  // but the encoded value should have been encoded in fewer code units (e.g.
  // mis-encoding U+0000 as 0b1100'0000 0b1000'0000 in two code units instead of
  // as 0b0000'0000).  It is passed the mis-encoded code point (which will be
  // valid and not a surrogate) and the count of code units that mis-encoded it.
  auto onNotShortestForm = [](char32_t aBadCodePoint, uint8_t aUnitsObserved) {
  };

  return DecodeOneUtf8CodePointInline(aLeadUnit, aIter, aEnd, onBadLeadUnit,
                                      onNotEnoughUnits, onBadTrailingUnit,
                                      onBadCodePoint, onNotShortestForm);
}

/**
 * Identical to the above function, but not forced to be instantiated inline --
 * the compiler/linker are allowed to common up separate invocations.
 */
template <typename Iter, typename EndIter>
inline Maybe<char32_t> DecodeOneUtf8CodePoint(const Utf8Unit aLeadUnit,
                                              Iter* aIter,
                                              const EndIter& aEnd) {
  return DecodeOneUtf8CodePointInline(aLeadUnit, aIter, aEnd);
}

}  // namespace mozilla

#endif /* mozilla_Utf8_h */
