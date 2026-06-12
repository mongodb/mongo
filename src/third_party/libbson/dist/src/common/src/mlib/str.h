/**
 * @file mlib/str.h
 * @brief String handling utilities
 * @date 2025-04-30
 *
 * This file provides utilities for handling *sized* strings. That is, strings
 * that carry their size, and do not rely on null termination. These APIs also
 * do a lot more bounds checking than is found in `<string.h>`.
 *
 * @copyright Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef MLIB_STR_H_INCLUDED
#define MLIB_STR_H_INCLUDED

#include <mlib/ckdint.h>
#include <mlib/cmp.h>
#include <mlib/config.h>
#include <mlib/intutil.h>
#include <mlib/loop.h>
#include <mlib/test.h>

#include <stdarg.h> // va_list
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>  // vsnprintf
#include <stdlib.h> // malloc/free
#include <string.h> // memcpy

/**
 * @brief A simple non-owning string-view type.
 *
 * The viewed string can be treated as an array of `char`. It's pointed-to data
 * must not be freed or manipulated.
 *
 * @note The viewed string is NOT guaranteed to be null-terminated. It WILL
 * be null-terminated if: Directly created from a string literal, a C string, or
 * a null-terminated `mstr_view`.
 * @note The viewed string MAY contain nul (zero-value) characters, so using them
 * with C string APIs could truncate unexpectedly.
 * @note The view itself may be "null" if the `data` member of the string view
 * is a null pointer. A zero-initialized `mstr_view` is null.
 */
typedef struct mstr_view {
   /**
    * @brief Pointer to the string data viewed by this object.
    *
    * - This pointer may be null, in which case the string view itself is "null".
    * - If `len > 1`, then this points to a contiguous array of `char` of length
    *   `len`.
    * - If `len == 1`, then this *may* point to a single `char` object.
    * - The pointed-to string might not be a null-terminated C string. Accessing
    *   the `char` value at `data[len]` is undefined behavior.
    */
   const char *data;
   /**
    * @brief The length of the viewed string pointed-to by `data`
    *
    * If `data` points to a single `char` object, then this must be `1`. If
    * `data` is a null pointer, then this value should be zero.
    */
   size_t len;
} mstr_view;

/**
 * @brief Expand to the two printf format arguments required to format an mstr object
 *
 * You should use the format specifier `%.*s' for all mstr strings.
 *
 * This is just a convenience shorthand.
 */
#define MSTR_FMT(S) (int)mstr_view_from(S).len, mstr_view_from(S).data

/**
 * @brief Create an `mstr_view` that views the given array of `char`
 *
 * @param data Pointer to the beginning of the string, or pointer to a single
 * `char`, or a null pointer
 * @param len Length of the new string-view. If `data` points to a single `char`,
 * this must be `0` or `1`. If `data` is a null pointer, this should be `0`.
 *
 * @note This is defined as a macro that expands to a compound literal to prevent
 * proliferation of redundant function calls in debug builds.
 */
#define mstr_view_data(DataPointer, Length) (mlib_init(mstr_view){(DataPointer), (Length)})

#if 1 // See "!! NOTE" below

/**
 * @brief Coerce a string-like object to an `mstr_view` of that string
 *
 * This macro requires that the object have `.data` and `.len` members.
 *
 * @note This macro will double-evaluate its argument.
 */
#define mstr_view_from(X) mstr_view_data((X).data, (X).len)

/**
 * ! NOTE: The disabled snippet below is kept for posterity as a drop-in replacment
 * ! for mstr_view_from with support for _Generic.
 *
 * When we can increase the compiler requirements to support _Generic, the following
 * macro definition alone makes almost every function in this file significantly
 * more concise to use, as it allows us to pass a C string to any API that
 * expects an `mstr_view`, enabling code like this:
 *
 * ```
 * mstr s = get_string();
 * if (mstr_cmp(s, ==, "magicKeyword")) {
 *    Do something...
 * }
 * ```
 *
 * This also allows us to avoid the double-evaluation problem presented by
 * `mstr_view_from` being defined as above.
 *
 * Without _Generic, we require all C strings to be wrapped with `mstr_cstring`,
 * which isn't especially onerous, but it is annoying. Additionally, the below
 * `_Generic` macro can be extended to support more complex string-like types.
 *
 * For reference, support for _Generic requires the following compilers:
 *
 * - MSVC 19.28.0+ (VS 2019, 16.8.1)
 * - GCC 4.9+
 * - Clang 3.0+
 */

#else

/**
 * @brief Coerce an object to an `mstr_view`
 *
 * The object requires a `data` and `len` member
 */
#define mstr_view_from(X) \
   _Generic((X), mstr_view: _mstr_view_trivial_copy, char *: mstr_cstring, const char *: mstr_cstring)((X))
// Just copy an mstr_view by-value
static inline mstr_view
_mstr_view_trivial_copy(mstr_view s)
{
   return s;
}

#endif


/**
 * @brief Create an `mstr_view` referring to the given null-terminated C string
 *
 * @param s Pointer to a C string. The length of the returned string is infered using `strlen`
 *
 * This should not defined as a macro, because defining it as a macro would require
 * double-evaluating for the call to `strlen`.
 */
static inline mstr_view
mstr_cstring(const char *s)
{
   const size_t l = strlen(s);
   return mstr_view_data(s, l);
}

/**
 * @brief Compare two strings lexicographically by each code unit
 *
 * If called with two arguments behaves the same as `strcmp`. If called with
 * three arguments, the center argument should be an infix operator to perform
 * the semantic comparison.
 */
static inline enum mlib_cmp_result
mstr_cmp(mstr_view a, mstr_view b)
{
   size_t l = a.len;
   if (b.len < l) {
      l = b.len;
   }
   // Use `memcmp`, not `strncmp`: We want to respect nul characters
   int r = memcmp(a.data, b.data, l);
   if (r) {
      // Not equal: Compare with zero to normalize to the cmp_result value
      return mlib_cmp(r, 0);
   }
   // Same prefixes, the ordering is now based on their length (longer string > shorter string)
   return mlib_cmp(a.len, b.len);
}

#define mstr_cmp(...) MLIB_ARGC_PICK(_mstr_cmp, __VA_ARGS__)
#define _mstr_cmp_argc_2(A, B) mstr_cmp(mstr_view_from(A), mstr_view_from(B))
#define _mstr_cmp_argc_3(A, Op, B) (_mstr_cmp_argc_2(A, B) Op 0)

/**
 * @brief If the given codepoint is a Basic Latin (ASCII) uppercase character,
 * return the lowercase character. Other codepoint values are returned unchanged.
 *
 * This is safer than `tolower`, because it doesn't respect locale and has no
 * undefined behavior.
 */
static inline int32_t
mlib_latin_tolower(int32_t a)
{
   if (a >= 0x41 /* "A" */ && a <= 0x5a /* "Z" */) {
      a += 0x20; // Adjust from "A" -> "a"
   }
   return a;
}

/**
 * @brief Compare two individual codepoint values, with case-insensitivity in
 * the Basic Latin range.
 */
static inline enum mlib_cmp_result
mlib_latin_charcasecmp(int32_t a, int32_t b)
{
   return mlib_cmp(mlib_latin_tolower(a), mlib_latin_tolower(b));
}

/**
 * @brief Compare two strings lexicographically, case-insensitive in the Basic
 * Latin range.
 *
 * If called with two arguments, behaves the same as `strcasecmp`. If called with
 * three arguments, the center argument should be an infix operator to perform
 * the semantic comparison.
 */
static inline enum mlib_cmp_result
mstr_latin_casecmp(mstr_view a, mstr_view b)
{
   size_t l = a.len;
   if (b.len < l) {
      l = b.len;
   }
   mlib_foreach_urange (i, l) {
      // We don't need to do any UTF-8 decoding, because our case insensitivity
      // only activates for 1-byte encoded codepoints, and all other valid UTF-8
      // sequences will collate equivalently with byte-wise comparison to a UTF-32
      // encoding.
      enum mlib_cmp_result r = mlib_latin_charcasecmp(a.data[i], b.data[i]);
      if (r) {
         // Not equivalent at this code unit. Return this as the overall string ordering.
         return r;
      }
   }
   // Same prefixes, the ordering is now based on their length (longer string > shorter string)
   return mlib_cmp(a.len, b.len);
}

#define mstr_latin_casecmp(...) MLIB_ARGC_PICK(_mstr_latin_casecmp, __VA_ARGS__)
#define _mstr_latin_casecmp_argc_2(A, B) mstr_latin_casecmp(mstr_view_from(A), mstr_view_from(B))
#define _mstr_latin_casecmp_argc_3(A, Op, B) (_mstr_latin_casecmp_argc_2(A, B) Op 0)

/**
 * @brief Adjust a possibly negative index position to wrap around for a string
 *
 * @param s The string to be respected for index wrapping
 * @param pos The maybe-negative index to be adjusted
 * @param clamp_to_length If `true` and given a non-negative value, if that
 * value is greater than the string length, this function will return the string
 * length instead.
 * @return size_t The new zero-based non-negative index
 *
 * If `pos` is negative, then it represents indexing from the end of the string,
 * where `-1` refers to the last character in the string, `-2` the penultimate,
 * etc. If the absolute value is greater than the length of the string, the
 * program will be terminated.
 */
static inline size_t
_mstr_adjust_index(mstr_view s, mlib_upsized_integer pos, bool clamp_to_length)
{
   if (clamp_to_length && (mlib_cmp)(pos, mlib_upsize_integer(s.len), 0) == mlib_greater) {
      // We want to clamp to the length, and the given value is greater than the string length.
      return s.len;
   }
   if (pos.is_signed && pos.bits.as_signed < 0) {
      // This will add the negative value to the length of the string. If such
      // an operation would result a negative value, this will terminate the
      // program.
      return mlib_assert_add(size_t, s.len, pos.bits.as_signed);
   }
   // No special behavior, just assert that the given position is in-bounds for the string
   mlib_check(
      pos.bits.as_unsigned <= s.len, because, "the string position index must not be larger than the string length");
   return pos.bits.as_unsigned;
}

/**
 * @brief Obtain the code unit at the given zero-based index, with negative index wrapping.
 *
 * This function asserts that the index is in-bounds for the given string.
 *
 * @param s The string to be inspected.
 * @param pos The index to access. Zero is the first code unit, and -1 is the last.
 * @return char The code unit at position `pos`.
 */
static inline char
mstr_at(mstr_view s, mlib_upsized_integer pos_)
{
   size_t pos = _mstr_adjust_index(s, pos_, false);
   return s.data[pos];
}

#define mstr_at(S, Pos) (mstr_at)(mstr_view_from(S), mlib_upsize_integer(Pos))

/**
 * @brief Create a new `mstr_view` that views a substring within another string
 *
 * @param s The original string view to be inspected
 * @param pos The number of `char` to skip in `s`, or a negative value to
 * pos from the end of the string.
 * @param len The length of the new string view (optional, default SIZE_MAX)
 *
 * The length of the string view is clamped to the characters available in `s`,
 * so passing a too-large value for `len` is well-defined. Passing a too-large
 * value for `pos` will abort the program.
 *
 * Callable as:
 *
 * - `mstr_substr(s, pos)`
 * - `mstr_substr(s, pos, len)`
 */
static inline mstr_view
mstr_substr(mstr_view s, mlib_upsized_integer pos_, size_t len)
{
   const size_t pos = _mstr_adjust_index(s, pos_, false);
   // Number of characters in the string after we remove the prefix
   const size_t remain = s.len - pos;
   // Clamp the new length to the size that is actually available.
   if (len > remain) {
      len = remain;
   }
   return mstr_view_data(s.data + pos, len);
}

#define mstr_substr(...) MLIB_ARGC_PICK(_mstr_substr, __VA_ARGS__)
#define _mstr_substr_argc_2(Str, Start) _mstr_substr_argc_3(Str, Start, SIZE_MAX)
#define _mstr_substr_argc_3(Str, Start, Stop) mstr_substr(mstr_view_from(Str), mlib_upsize_integer(Start), Stop)

/**
 * @brief Obtain a slice of the given string view, where the two arguments are zero-based indices into the string
 *
 * @param s The string to be sliced
 * @param start The zero-based index of the new string start
 * @param end The zero-based index of the first character to exclude from the new string
 *
 * @note Unlike `substr`, the second argument is required, and must specify the index at which the
 * string will end, rather than the length of the string.
 */
static inline mstr_view
mstr_slice(const mstr_view s, const mlib_upsized_integer start_, const mlib_upsized_integer end_)
{
   const size_t start_pos = _mstr_adjust_index(s, start_, false);
   const size_t end_pos = _mstr_adjust_index(s, end_, true);
   mlib_check(end_pos >= start_pos, because, "Slice positions must end after the start position");
   const size_t sz = (size_t)(end_pos - start_pos);
   return mstr_substr(s, start_pos, sz);
}
#define mstr_slice(S, StartPos, EndPos) \
   mstr_slice(mstr_view_from(S), mlib_upsize_integer((StartPos)), mlib_upsize_integer((EndPos)))

/**
 * @brief Find the first occurrence of `needle` within `hay`, returning the zero-based index
 * if found, and `SIZE_MAX` if it is not found.
 *
 * @param hay The string which is being scanned
 * @param needle The substring that we are searching to find
 * @param pos The start position of the search (optional, default zero)
 * @param len The number of characters to search in `hay` (optional, default SIZE_MAX)
 * @return size_t If found, the zero-based index of the first occurrence within
 *    the string. If not found, returns `SIZE_MAX`.
 *
 * The `len` is clamped to the available string length.
 *
 * Callable as:
 *
 * - `mstr_find(hay, needle)`
 * - `mstr_find(hay, needle, pos)`
 * - `mstr_find(hay, needle, pos, len)`
 */
static inline size_t
mstr_find(mstr_view hay, mstr_view const needle, mlib_upsized_integer const pos_, size_t const len)
{
   const size_t pos = _mstr_adjust_index(hay, pos_, false);
   // Trim the hay according to our search window:
   hay = mstr_substr(hay, pos, len);

   // Larger needle can never exist within the smaller string:
   if (hay.len < needle.len) {
      return SIZE_MAX;
   }

   // Set the index at which we can stop searching early. This will never
   // overflow, because we guard against hay.len > needle.len
   size_t stop_idx = hay.len - needle.len;
   // Use "<=", because we do want to include the final search position
   for (size_t offset = 0; offset <= stop_idx; ++offset) {
      if (memcmp(hay.data + offset, needle.data, needle.len) == 0) {
         // Return the found position. Adjust by the start pos since we may
         // have trimmed the search window
         return offset + pos;
      }
   }

   // Nothing was found. Return SIZE_MAX to indicate the not-found
   return SIZE_MAX;
}

#define mstr_find(...) MLIB_ARGC_PICK(_mstr_find, __VA_ARGS__)
#define _mstr_find_argc_2(Hay, Needle) _mstr_find_argc_3(Hay, Needle, 0)
#define _mstr_find_argc_3(Hay, Needle, Start) _mstr_find_argc_4(Hay, Needle, Start, SIZE_MAX)
#define _mstr_find_argc_4(Hay, Needle, Start, Stop) \
   mstr_find(mstr_view_from(Hay), mstr_view_from(Needle), mlib_upsize_integer(Start), Stop)

/**
 * @brief Find the zero-based index of the first `char` in `hay` that also occurs in `needles`
 *
 * This is different from `find()` because it considers each char in `needles` as an individual
 * one-character string to be search for in `hay`.
 *
 * @param hay The string to be searched
 * @param needles A string containing a set of characters which are searched for in `hay`
 * @param pos The index at which to begin searching (optional, default is zero)
 * @param len The number of characters in `hay` to consider before stopping (optional, default is SIZE_MAX)
 * @return size_t If a needle is found, returns the zero-based index of that first needle.
 * Otherwise, returns SIZE_MAX.
 *
 * Callable as:
 *
 * - `mstr_find_first_of(hay, needles)`
 * - `mstr_find_first_of(hay, needles, pos)`
 * - `mstr_find_first_of(hay, needles, pos, len)`
 */
static inline size_t
mstr_find_first_of(mstr_view hay, mstr_view const needles, mlib_upsized_integer const pos_, size_t const len)
{
   const size_t pos = _mstr_adjust_index(hay, pos_, false);
   // Trim to fit the search window
   hay = mstr_substr(hay, pos, len);
   // We search by incrementing an index
   mlib_foreach_urange (idx, hay.len) {
      // Grab a substring of the single char at the current search index
      mstr_view one = mstr_substr(hay, idx, 1);
      // Test if the single char occurs anywhere in the needle set
      if (mstr_find(needles, one) != SIZE_MAX) {
         // We found the first index in `hay` where one of the needles occurs. Adjust
         // by `pos` since we may have trimmed
         return idx + pos;
      }
   }
   return SIZE_MAX;
}

#define mstr_find_first_of(...) MLIB_ARGC_PICK(_mstr_find_first_of, __VA_ARGS__)
#define _mstr_find_first_of_argc_2(Hay, Needle) _mstr_find_first_of_argc_3(Hay, Needle, 0)
#define _mstr_find_first_of_argc_3(Hay, Needle, Pos) _mstr_find_first_of_argc_4(Hay, Needle, Pos, SIZE_MAX)
#define _mstr_find_first_of_argc_4(Hay, Needle, Pos, Len) mstr_find_first_of(Hay, Needle, mlib_upsize_integer(Pos), Len)

/**
 * @brief Test whether the given codepoint is a Basic Latin whitespace character
 *
 * This function does not depend on the locale and has no undefined behavior, unlike <ctype.h> functions
 *
 * @param c The codepoint to be tested
 */
static inline bool
mlib_is_latin_whitespace(int32_t c)
{
   switch (c) {
   case 0x09: // horizontal tab
   case 0x0a: // line feed
   case 0x0d: // carriage return
   case 0x20: // space
      return true;

   default:
      return false;
   }
}

/**
 * @brief Trim leading latin (ASCII) whitespace from the given string
 *
 * @param s The string to be inspected
 * @return mstr_view A substring view of `s` that excludes any leading whitespace
 */
static inline mstr_view
mstr_trim_left(mstr_view s)
{
   // Testing arbitrary code units for whitespace is safe as only 1-byte-encoded
   // codepoints can land within the Basic Latin range:
   while (s.len && mlib_is_latin_whitespace(mstr_at(s, 0))) {
      s = mstr_substr(s, 1);
   }
   return s;
}
#define mstr_trim_left(S) (mstr_trim_left)(mstr_view_from(S))

/**
 * @brief Trim trailing latin (ASCII) whitespace from the given string
 *
 * @param s The string to be insepcted
 * @return mstr_view A substring view of `s` that excludes any trailing whitespace.
 */
static inline mstr_view
mstr_trim_right(mstr_view s)
{
   while (s.len && mlib_is_latin_whitespace(mstr_at(s, -1))) {
      s = mstr_slice(s, 0, -1);
   }
   return s;
}
#define mstr_trim_right(S) (mstr_trim_right)(mstr_view_from(S))

/**
 * @brief Trim leading and trailing latin (ASCII) whitespace from the string
 *
 * @param s The string to be inspected
 * @return mstr_view A substring of `s` that excludes leading and trailing whitespace.
 */
static inline mstr_view
mstr_trim(mstr_view s)
{
   s = mstr_trim_left(s);
   s = mstr_trim_right(s);
   return s;
}
#define mstr_trim(S) (mstr_trim)(mstr_view_from(S))

/**
 * @brief Split a single string view into two strings at the given position
 *
 * @param s The string to be split
 * @param pos The position at which the prefix string is ended
 * @param drop [optional] The number of characters to drop between the prefix and suffix
 * @param prefix [out] Updated to point to the part of the string before the split
 * @param suffix [out] Updated to point to the part of the string after the split
 *
 * `pos` and `drop` are clamped to the size of the input string.
 *
 * Callable as:
 *
 * - `mstr_split_at(s, pos,       prefix, suffix)`
 * - `mstr_split_at(s, pos, drop, prefix, suffix)`
 *
 * If either `prefix` or `suffix` is a null pointer, then they will be ignored
 */
static inline void
mstr_split_at(mstr_view s, mlib_upsized_integer pos_, size_t drop, mstr_view *prefix, mstr_view *suffix)
{
   const size_t pos = _mstr_adjust_index(s, pos_, true /* clamp to the string size */);
   // Save the prefix string
   if (prefix) {
      *prefix = mstr_substr(s, 0, pos);
   }
   // Save the suffix string
   if (suffix) {
      // The number of characters that remain after the prefix is removed
      const size_t remain = s.len - pos;
      // Clamp the number of chars to drop to not overrun the input string
      if (remain < drop) {
         drop = remain;
      }
      // The start position of the new string
      const size_t next_start = pos + drop;
      *suffix = mstr_substr(s, next_start, SIZE_MAX);
   }
}

#define mstr_split_at(...) MLIB_ARGC_PICK(_mstr_split_at, __VA_ARGS__)
#define _mstr_split_at_argc_4(Str, Pos, Prefix, Suffix) _mstr_split_at_argc_5(Str, Pos, 0, Prefix, Suffix)
#define _mstr_split_at_argc_5(Str, Pos, Drop, Prefix, Suffix) \
   mstr_split_at(mstr_view_from(Str), mlib_upsize_integer(Pos), Drop, Prefix, Suffix)

/**
 * @brief Split a string in two around the first occurrence of some infix string.
 *
 * @param s The string to be split in twain
 * @param infix The infix string to be searched for
 * @param prefix The part of the string that precedes the infix (nullable)
 * @param suffix The part of the string that follows the infix (nullable)
 * @return true If the infix was found
 * @return false Otherwise
 *
 * @note If `infix` does not occur in `s`, then `*prefix` will be set equal to `s`,
 * and `*suffix` will be made an empty string, as if the infix occurred at the end
 * of the string.
 */
static inline bool
mstr_split_around(mstr_view s, mstr_view infix, mstr_view *prefix, mstr_view *suffix)
{
   // Find the position of the infix. If it is not found, returns SIZE_MAX
   const size_t pos = mstr_find(s, infix);
   // Split at the infix, dropping as many characters as are in the infix. If
   // the `pos` is SIZE_MAX, then this call will clamp to the end of the string.
   mstr_split_at(s, pos, infix.len, prefix, suffix);
   // Return `true` if we found the infix, indicated by a not-SIZE_MAX `pos`
   return pos != SIZE_MAX;
}

#define mstr_split_around(Str, Infix, PrefixPtr, SuffixPtr) \
   mstr_split_around(mstr_view_from((Str)), mstr_view_from((Infix)), (PrefixPtr), (SuffixPtr))

/**
 * @brief Test whether the given string starts with the given prefix
 *
 * @param str The string to be tested
 * @param prefix The prefix to be searched for
 * @return true if-and-only-if `str` starts with `prefix`
 * @return false Otherwise
 */
static inline bool
mstr_starts_with(mstr_view str, mstr_view prefix)
{
   // Trim to match the length of the prefix we want
   str = mstr_substr(str, 0, prefix.len);
   // Check if the trimmed string is the same as the prefix
   return mstr_cmp(str, ==, prefix);
}
#define mstr_starts_with(Str, Prefix) mstr_starts_with(mstr_view_from(Str), mstr_view_from(Prefix))

/**
 * @brief Test whether a substring occurs at any point within the given string
 *
 * @param str The string to be inspected
 * @param needle The substring to be searched for
 * @return true If-and-only-if `str` contains `needle` at any position
 * @return false Otherise
 */
static inline bool
mstr_contains(mstr_view str, mstr_view needle)
{
   return mstr_find(str, needle) != SIZE_MAX;
}
#define mstr_contains(Str, Needle) mstr_contains(mstr_view_from(Str), mstr_view_from(Needle))

/**
 * @brief Test whether a given string contains any of the characters in some other string
 *
 * @param str The string to be inspected
 * @param needle A string to be treated as a set of one-byte characters to search for
 * @return true If-and-only-if `str` contains `needle` at any position
 * @return false Otherise
 *
 * @note This function does not currently support multi-byte codepoints
 */
static inline bool
mstr_contains_any_of(mstr_view str, mstr_view needle)
{
   return mstr_find_first_of(str, needle) != SIZE_MAX;
}
#define mstr_contains_any_of(Str, Needle) mstr_contains_any_of(mstr_view_from(Str), mstr_view_from(Needle))


/**
 * @brief A simple mutable string type, with a guaranteed null terminator.
 *
 * This type is a trivially relocatable aggregate type that contains a pointer `data`
 * and a size `len`. If not null, the pointer `data` points to an array of mutable
 * `char` of length `len + 1`, where the character at `data[len]` is always zero,
 * and must not be modified.
 *
 * @note The string MAY contain nul (zero-value) characters, so using them with
 * C string APIs could truncate unexpectedly.
 * @note The string itself may be "null" if the `data` member of the string is
 * a null pointer. A zero-initialized `mstr` is null. The null string is distinct
 * from the empty string, which has a non-null `.data` that points to an empty
 * C string.
 */
typedef struct mstr {
   /**
    * @brief Pointer to the first char in the string, or NULL if
    * the string is null.
    *
    * The pointed-to character array has a length of `len + 1`, where
    * the character at `data[len]` is always null.
    *
    * @warning Attempting to overwrite the null character at `data[len]`
    * will result in undefined behavior!
    *
    * @note An empty string is not equivalent to a null string! An empty string
    * will still point to an array of length 1, where the only char is the null
    * terminator.
    */
   char *data;
   /**
    * @brief The number of characters in the array pointed-to by `data`
    * that precede the null terminator.
    */
   size_t len;
} mstr;


/**
 * @brief Resize an existing or null `mstr`, without initializing any of the
 * added content other than the null terminator. This operation is potentially
 * UNSAFE, because it gives uninitialized memory to the caller.
 *
 * @param str Pointer to a valid `mstr`, or a null `mstr`.
 * @param new_len The new length of the string.
 * @return true If the operation succeeds
 * @return false Otherwise
 *
 * If `str` is a null string, this function will initialize a new `mstr` object
 * on-the-fly.
 *
 * If the operation increases the length of the string (or initializes a new string),
 * then the new `char` in `str.data[str.len : new_len] will contain uninitialized
 * values. The char at `str.data[new_len]` WILL be set to zero, to ensure there
 * is a null terminator. The caller should always initialize the new string
 * content to ensure that the string has a specified value.
 */
static inline bool
mstr_resize_for_overwrite(mstr *const str, const size_t new_len)
{
   // We need to allocate one additional char to hold the null terminator
   size_t alloc_size = new_len;
   if (mlib_unlikely(mlib_add(&alloc_size, 1) || alloc_size > PTRDIFF_MAX)) {
      // Allocation size is too large
      return false;
   }
   // Try to (re)allocate the region
   char *data = (char *)realloc(str->data, alloc_size);
   if (!data) {
      // Failed to (re)allocate
      return false;
   }
   // Note: We do not initialize any of the data in the newly allocated region.
   // We only set the null terminator. It is up to the caller to do the rest of
   // the init.
   data[new_len] = '\0';
   // Update the final object
   str->data = data;
   str->len = new_len;
   // Success
   return true;
}

/**
 * @brief Given an existing `mstr`, resize it to hold `new_len` chars
 *
 * @param str Pointer to a string object to update, or a null `mstr`
 * @param new_len The new length of the string, not including the implicit null terminator
 * @return true If the operation succeeds
 * @return false Otherwise
 *
 * @note If the operation fails, then `*str` is not modified.
 */
static inline bool
mstr_resize(mstr *str, size_t new_len)
{
   const size_t old_len = str->len;
   if (!mstr_resize_for_overwrite(str, new_len)) {
      // Failed to allocate new storage for the string
      return false;
   }
   // Check how many chars we added/removed
   const ptrdiff_t len_diff = mlib_assert_sub(ptrdiff_t, new_len, str->len);
   if (len_diff > 0) {
      // We added new chars. Zero-init all the new chars
      memset(str->data + old_len, 0, (size_t)len_diff);
   }
   // Success
   return true;
}

/**
 * @brief Create a new `mstr` of the given length
 *
 * @param new_len The length of the new string, in characters, not including the null terminator
 * @return mstr A new string. The string's `data` member is NULL in case of failure
 *
 * The character array allocated for the string will always be `new_len + 1` `char` in length,
 * where the char at the index `new_len` is a null terminator. This means that a string of
 * length zero will allocate a single character to store the null terminator.
 *
 * All characters in the new string are initialize to zero. If you want uninitialized
 * string content, use `mstr_resize_for_overwrite`.
 */
static inline mstr
mstr_new(size_t new_len)
{
   mstr ret = {NULL, 0};
   // We can rely on `resize` to handle the null state properly.
   mstr_resize(&ret, new_len);
   return ret;
}

/**
 * @brief Free the resources associated with an mstr object.
 *
 * @param s Pointer to an `mstr` object. If pointer or the pointed-to-object is null,
 * this function is a no-op.
 *
 * After this call, the pointed-to `s` will be a null `mstr`
 */
static inline void
mstr_destroy(mstr *s)
{
   if (s) {
      free(s->data);
      s->len = 0;
      s->data = NULL;
   }
}

/**
 * @brief Obtain a null mstr string object.
 *
 * @return mstr A null string, with a null data pointer and zero size
 */
static inline mstr
mstr_null(void)
{
   return mlib_init(mstr){0};
}

/**
 * @internal
 * @brief Test whether the given string-view is a view within the given owning string
 */
static inline bool
_mstr_overlaps(mstr const *str, mstr_view sv)
{
   // Note: Pointer-comparison between objects is unspecified, but is guaranteed
   // to returns `true` if there is overlap. We're okay with false-positive overlaps.
   // Additionally, POSIX and Win32 both offer stronger guarantees about pointer
   // comparison, which we can rely on here.
   return str->data               //
          && str->data <= sv.data //
          && sv.data <= str->data + str->len;
}

/**
 * @brief Replace the content of the given string, attempting to reuse the buffer
 *
 * @param inout Pointer to a valid or null `mstr` to be replaced
 * @param s The new string contents
 * @return true If the operation succeeded
 * @return false Otherwise
 *
 * If the operation fails, `*inout` is not modified
 */
static inline bool
mstr_assign(mstr *inout, mstr_view s)
{
   // Check for self-assignment
   if (_mstr_overlaps(inout, s)) {
      // We are overwriting a string with a (sub)string of its own content.
      // Move the substring to the front of the string (may be a no-op if `s`
      // points to the beginning of the string)
      memmove(inout->data, s.data, s.len);
      // Resize to truncate. This will always shrink the string, because a valid
      // string-view into `inout` cannot be longer than `inout` itself. Thus, it
      // also cannot fail.
      mstr_resize_for_overwrite(inout, s.len);
      return true;
   }
   if (!mstr_resize_for_overwrite(inout, s.len)) {
      return false;
   }
   memcpy(inout->data, s.data, s.len);
   return true;
}

#define mstr_assign(InOut, S) mstr_assign((InOut), mstr_view_from((S)))

/**
 * @brief Create a mutable copy of the given string.
 *
 * @param sv The string to be copied
 * @return mstr A new valid string, or a null string in case of allocation failure.
 */
static inline mstr
mstr_copy(mstr_view sv)
{
   mstr ret = {NULL, 0};
   mstr_assign(&ret, sv);
   return ret;
}

#define mstr_copy(S) mstr_copy(mstr_view_from((S)))
#define mstr_copy_cstring(S) mstr_copy(mstr_cstring((S)))

/**
 * @brief Concatenate two strings into a new mutable string
 *
 * @param a The left-hand string to be concatenated
 * @param b The right-hand string to be concatenated
 * @return mstr A new valid string composed by concatenating `a` with `b`, or
 * a null string in case of allocation failure.
 */
static inline mstr
mstr_concat(mstr_view a, mstr_view b)
{
   mstr ret = {NULL, 0};
   size_t cat_len = 0;
   if (mlib_unlikely(mlib_add(&cat_len, a.len, b.len))) {
      // Size would overflow. No go.
      return ret;
   }
   // Prepare the new string
   if (!mstr_resize_for_overwrite(&ret, cat_len)) {
      // Failed to allocate. The ret string is still null, and we can just return it
      return ret;
   }
   // Copy in the characters from `a`
   char *out = ret.data;
   memcpy(out, a.data, a.len);
   // Copy in the characters from `b`
   out += a.len;
   memcpy(out, b.data, b.len);
   // Success
   return ret;
}

#define mstr_concat(A, B) mstr_concat(mstr_view_from((A)), mstr_view_from((B)))

/**
 * @brief Delete and/or insert characters into a string
 *
 * @param str The string object to be updated
 * @param splice_pos The position at which to do the splice
 * @param n_delete The number of characters to delete at `splice_pos`
 * @param insert A string to be inserted at `split_pos` after chars are deleted
 * @return true If the operation succeeds
 * @return false Otherwise
 *
 * If `n_delete` is zero, then no characters are deleted. If `insert` is empty
 * or null, then no characters are inserted.
 */
static inline bool
mstr_splice(mstr *str, size_t splice_pos, size_t n_delete, mstr_view insert)
{
   // Guard against self-insertion:
   if (insert.data && _mstr_overlaps(str, insert)) {
      // The insertion string exists within the current string. We cannot modify it in-place.
      // Duplicate the insertion string to remain pristine while we splice:
      mstr insert_dup = mstr_copy(insert);
      if (!insert_dup.data) {
         // Failed to dup the insert string. Failure to splice
         return false;
      }
      // Do the splice, now using the copy of the insertion string
      const bool ok = mstr_splice(str, splice_pos, n_delete, mstr_view_from(insert_dup));
      // We're done with the dup
      mstr_destroy(&insert_dup);
      // Return the sub-result
      return ok;
   }
   mlib_check(splice_pos <= str->len);
   // How many chars is it possible to delete from `splice_pos`?
   size_t n_chars_avail_to_delete = str->len - splice_pos;
   // Clamp to the number of chars available for deletion:
   if (n_delete > n_chars_avail_to_delete) {
      n_delete = n_chars_avail_to_delete;
   }
   // Compute the new string length
   size_t new_len = str->len;
   // This should never fail, because we should never try to delete more chars than we have
   mlib_check(!mlib_sub(&new_len, n_delete));
   // Check if appending would make too big of a string
   if (mlib_unlikely(mlib_add(&new_len, insert.len))) {
      // New string will be too long
      return false;
   }
   char *mut = str->data;
   // We either resize first or resize last, depending on where we are shifting chars
   if (new_len > str->len) {
      // Do the resize first
      if (!mstr_resize_for_overwrite(str, new_len)) {
         // Failed to allocate
         return false;
      }
      mut = str->data;
   }
   // Move to the splice position
   mut += splice_pos;
   // Shift the existing string parts around for the deletion operation
   const size_t tail_len = n_chars_avail_to_delete - n_delete;
   // Adjust to the begining of the string part that we want to keep
   char *copy_from = mut + n_delete;
   char *copy_to = mut + insert.len;
   memmove(copy_to, copy_from, tail_len);
   if (new_len < str->len) {
      // We didn't resize first, so resize now. We are shrinking the string, so this
      // will never fail, and does not create any uninitialized memory:
      mlib_check(mstr_resize_for_overwrite(str, new_len));
      mut = str->data + splice_pos;
   }
   // Insert the new data if the insertion string is non-null
   if (insert.data) {
      memcpy(mut, insert.data, insert.len);
   }
   return true;
}

/**
 * @brief Append a string to the end of some other string.
 *
 * @param str The string to be modified
 * @param suffix The suffix string to be appended onto `*str`
 * @return true If the operation was successful
 * @return false Otherwise
 *
 * If case of failure, `*str` is not modified.
 */
static inline bool
mstr_append(mstr *str, mstr_view suffix)
{
   return mstr_splice(str, str->len, 0, suffix);
}

#define mstr_append(Into, Suffix) mstr_append((Into), mstr_view_from((Suffix)))

/**
 * @brief Append a single character to the given string object
 *
 * @param str The string object to be updated
 * @param c The single character that will be inserted at the end
 * @return true If the operation succeeded
 * @return false Otherwise
 *
 * In case of failure, the string is not modified.
 */
static inline bool
mstr_append_char(mstr *str, char c)
{
   mstr_view one = mstr_view_data(&c, 1);
   return mstr_append(str, one);
}

/**
 * @brief Replace every occurrence of `needle` in `str` with `sub`
 *
 * @param str The string object to be updated
 * @param needle The non-empty needle string to be searched for.s
 * @param sub The string to be inserted in place of each `needle`
 * @return true If the operation succeeds
 * @return false Otherwise
 *
 * @note If the `needle` string is empty, then the substitution string will
 * be inserted around and between every byte in the string:
 *
 *    replace("foo", "", "|") -> "|f|o|o|"
 *
 * @note The operation is guaranteed to never fail if the `sub` string is not
 * longer than the `needle` string AND the needle and sub strings do not overlap
 *
 * @note If the operation fails, the content of `str` is an unspecified but valid
 * string.
 */
static inline bool
mstr_replace(mstr *str, mstr_view needle, mstr_view sub)
{
   bool okay = true;
   // We may dup the needle/sub if they overlap the output string
   mstr needle_dup = mstr_null();
   mstr sub_dup = mstr_null();
   // Check if the needle is a substring of the target:
   if (_mstr_overlaps(str, needle)) {
      // Copy the needle string
      needle_dup = mstr_copy(needle);
      // Detect allocation failure:
      okay = !!needle_dup.data;
      // Update the needle to point to the duplicate:
      needle = mstr_view_from(needle_dup);
   }
   // Do the same with the sub string:
   if (okay && _mstr_overlaps(str, sub)) {
      sub_dup = mstr_copy(sub);
      okay = !!needle_dup.data;
      sub = mstr_view_from(sub_dup);
   }
   // Scan forward, starting from the first position:
   size_t off = 0;
   while (okay && off <= str->len) {
      // Find the next occurrence, starting from the scan offset
      off = mstr_find(*str, needle, off);
      if (off == SIZE_MAX) {
         // No more occurrences.
         break;
      }
      // Replace the needle string with the new value
      if (!mstr_splice(str, off, needle.len, sub)) {
         okay = false;
      }
      // Advance over the length of the replacement string, so we don't try to
      // infinitely replace content if the replacement itself contains the needle
      // string
      if (mlib_unlikely(mlib_add(&off, sub.len))) {
         // Integer overflow while advancing the offset. No good.
         okay = false;
      }
      // Note: To support empty needles, advance one more space to avoid infinite
      // repititions in-place.
      // TODO: To do this "properly", this should instead advance over a full UTF-8-encoded
      // codepoint. For now, just do a single byte.
      if (!needle.len && mlib_unlikely(mlib_add(&off, 1))) {
         // Advancing the extra distance failed
         okay = false;
      }
   }
   // Destroy the needle/sub strings, which we may have duplicated if they overlapped
   // the target. If not, then these are a no-op.
   mstr_destroy(&needle_dup);
   mstr_destroy(&sub_dup);
   return okay;
}

/**
 * @brief Like `mstr_sprintf`, but accepts a `va_list` directly.
 */
mlib_printf_attribute(1, 0) static inline mstr mstr_vsprintf(const char *format, va_list args)
{
   size_t format_strlen = strlen(format);
   size_t sz = format_strlen;
   if (mlib_unlikely(mlib_mul(&sz, 2))) {
      // Overflow on multiply. Oof
      sz = format_strlen;
   }

   mstr ret = mstr_null();
   while (1) {
      // Resize to make room for the formatted text
      if (!mstr_resize(&ret, sz)) {
         // Allocation failure
         break;
      }

      // Calc the size with the null terminator
      size_t len_with_null = ret.len;
      if (mlib_unlikely(mlib_add(&len_with_null, 1))) {
         // Unlikely: Overflow
         break;
      }

      // Do the formatting
      va_list dup_args;
      va_copy(dup_args, args);

      // clang complains that the format string is not a string literal;
      // this is fine since we're writing a vsnprintf wrapper
      // the format string will be checked at call sites to mstr_vsprintf()
      // because we mark it using mlib_printf_attribute
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
      int n_chars = vsnprintf(ret.data, len_with_null, format, dup_args);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic pop
#endif
      va_end(dup_args);

      // On error, returns a negative value
      if (n_chars < 0) {
         break;
      }

      if ((size_t)n_chars <= ret.len) {
         // Success. Truncate to the number of chars actually written:
         mstr_resize(&ret, (size_t)n_chars);
         // Return the successfully formatted string:
         return ret;
      }

      // Need more room. Resize and try again:
      sz = (size_t)n_chars;
      continue;
   }

   // Only reached if the operation failed
   mstr_destroy(&ret);
   return ret;
}

/**
 * @brief Format a string according to `printf` rules
 *
 * @param f The format string to be used.
 * @param ... The formatting arguments to interpolate into the string
 * @return mstr A new mstr upon success, or a null mstr upon failure.
 */
mlib_printf_attribute(1, 2) static inline mstr mstr_sprintf(const char *f, ...)
{
   va_list args;
   va_start(args, f);
   mstr ret = mstr_vsprintf(f, args);
   va_end(args);
   return ret;
}

/**
 * @brief Like `mstr_sprintf_append`, but accepts the va_list directly.
 */
mlib_printf_attribute(2, 0) static inline bool mstr_vsprintf_append(mstr *string, const char *format, va_list args)
{
   mlib_check(string != NULL, because, "Output string parameter is required");
   mstr suffix = mstr_vsprintf(format, args);
   bool ok = mstr_append(string, suffix);
   mstr_destroy(&suffix);
   return ok;
}

/**
 * @brief Append content to a string using `printf()` style formatting.
 *
 * @param string Pointer to a valid or null string object which will be modified
 * @param format A printf-style format string to append onto `string`
 * @param ... The interpolation arguments for `format`
 *
 * @retval true If-and-only-if the string is successfully modified
 * @retval false If there was an error during formatting. The content of `string`
 * is unspecified.
 *
 * This function maintains the existing content of `string` and only inserts
 * additional characters at the end of the string.
 */
mlib_printf_attribute(2, 3) static inline bool mstr_sprintf_append(mstr *string, const char *format, ...)
{
   va_list args;
   va_start(args, format);
   const bool okay = mstr_vsprintf_append(string, format, args);
   va_end(args);
   return okay;
}


#endif // MLIB_STR_H_INCLUDED
