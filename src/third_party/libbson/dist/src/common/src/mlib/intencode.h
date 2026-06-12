/**
 * @file mlib/intencode.h
 * @brief Integer encoding functions
 * @date 2025-01-31
 *
 * @copyright Copyright (c) 2025
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
#pragma once

#include <mlib/config.h>
#include <mlib/loop.h>
#include <mlib/str.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Decode an unsigned 32-bit little-endian integer from a memory buffer
 */
static inline uint32_t
mlib_read_u32le(const void *buf)
{
   uint32_t ret = 0;
   if (mlib_is_little_endian()) {
      // Optimize: The platform uses a LE encoding already
      memcpy(&ret, buf, sizeof ret);
   } else {
      // Portable decode of an LE integer
      const uint8_t *cptr = (const uint8_t *)buf;
      mlib_foreach_urange (i, sizeof ret) {
         ret <<= 8;
         ret |= cptr[(sizeof ret) - i - 1];
      }
   }
   return ret;
}

/**
 * @brief Decode an signed 32-bit little-endian integer from a memory buffer
 */
static inline int32_t
mlib_read_i32le(const void *buf)
{
   const uint32_t u = mlib_read_u32le(buf);
   int32_t r;
   memcpy(&r, &u, sizeof r);
   return r;
}

/**
 * @brief Decode an unsigned 64-bit little-endian integer from a memory buffer
 */
static inline uint64_t
mlib_read_u64le(const void *buf)
{
   uint64_t ret = 0;
   if (mlib_is_little_endian()) {
      // Optimize: The platform uses a LE encoding already
      memcpy(&ret, buf, sizeof ret);
   } else {
      // Portable decode of an LE integer
      const uint8_t *cptr = (const uint8_t *)buf;
      mlib_foreach_urange (i, sizeof ret) {
         ret <<= 8;
         ret |= cptr[(sizeof ret) - i - 1];
      }
   }
   return ret;
}

/**
 * @brief Decode an signed 64-bit little-endian integer from a memory buffer
 */
static inline int64_t
mlib_read_i64le(const void *buf)
{
   const uint64_t u = mlib_read_u64le(buf);
   int64_t r;
   memcpy(&r, &u, sizeof r);
   return r;
}

/**
 * @brief Write an unsigned 32-bit little-endian integer into a destination
 *
 * @return void* The address after the written value
 */
static inline void *
mlib_write_u32le(void *out, const uint32_t value)
{
   uint8_t *o = (uint8_t *)out;
   if (mlib_is_little_endian()) {
      memcpy(o, &value, sizeof value);
      return o + sizeof value;
   }
   mlib_foreach_urange (i, sizeof value) {
      *o++ = (value >> (8u * i)) & 0xffu;
   }
   return o;
}

/**
 * @brief Write a signed 32-bit little-endian integer into a destination
 *
 * @return void* The address after the written value
 */
static inline void *
mlib_write_i32le(void *out, int32_t value)
{
   return mlib_write_u32le(out, (uint32_t)value);
}

/**
 * @brief Write an unsigned 64-bit little-endian integer into a destination
 *
 * @return void* The address after the written value
 */
static inline void *
mlib_write_u64le(void *out, const uint64_t value)
{
   uint8_t *o = (uint8_t *)out;
   if (mlib_is_little_endian()) {
      memcpy(o, &value, sizeof value);
      return o + sizeof value;
   }
   mlib_foreach_urange (i, sizeof value) {
      *o++ = (value >> (8u * i)) & 0xffu;
   }
   return o;
}

/**
 * @brief Write an signed 64-bit little-endian integer into a destination
 *
 * @return void* The address after the written value
 */
static inline void *
mlib_write_i64le(void *out, int64_t value)
{
   return mlib_write_u64le(out, (uint64_t)value);
}

/**
 * @brief Write a little-endian 64-bit floating point (double) to the given
 * memory location
 *
 * @return void* The address after the written value.
 */
static inline void *
mlib_write_f64le(void *out, double d)
{
   mlib_static_assert(sizeof(double) == sizeof(uint64_t));
   uint64_t bits;
   memcpy(&bits, &d, sizeof d);
   return mlib_write_u64le(out, bits);
}

/**
 * @brief Decode a 64-bit natural number
 *
 * @param in The input string to be decoded. Does not support a sign or base prefix!
 * @param base The base to be decoded. Must not be zero!
 * @param out Pointer that receives the decoded value
 * @return int A result code for the operation.
 *
 * See `mlib_i64_parse` for more details.
 */
static inline int
mlib_nat64_parse(mstr_view in, unsigned base, uint64_t *out)
{
   if (in.len == 0) {
      // Empty string is not valid
      return EINVAL;
   }

   // Accummulate into this value:
   uint64_t value = 0;
   // Whether any operation in the parse overflowed the integer value
   bool did_overflow = false;
   // Loop until we have consumed the full string, or encounter an invalid digit
   while (in.len) {
      // Shift place value for another digit
      did_overflow = mlib_mul(&value, base) || did_overflow;
      // Case-fold for alpha digits
      int32_t digit = mlib_latin_tolower(in.data[0]);
      unsigned digit_value = 0;
      // Only standard digits
      if (digit >= '0' && digit <= '9') {
         // Normal digit
         digit_value = (unsigned)(digit - '0');
      } else if (digit >= 'a' && digit <= 'z') {
         // Letter digits
         digit_value = (unsigned)(digit - 'a') + 10;
      } else {
         // Not a valid alnum digit
         return EINVAL;
      }
      if (digit_value >= base) {
         // The digit value is out-of-range for our chosen base
         return EINVAL;
      }
      // Accumulate the new digit value
      did_overflow = mlib_add(&value, digit_value) || did_overflow;
      // Jump to the next digit in the string
      in = mstr_substr(in, 1);
   }

   if (did_overflow) {
      return ERANGE;
   }

   (void)(out && (*out = value));
   return 0;
}

/**
 * @brief Parse a string as a 64-bit signed integer
 *
 * @param in The string of digits to be parsed.
 * @param base Optional: The base to use for parsing. Use "0" to infer the base.
 * @param out Optional storage for an int64 value to be updated with the result
 * @return int Returns an errno value for the parse
 *
 * - A value of `0` indicates that the parse was successful.
 * - A value of `EINVAL` indicates that the input string is not a valid
 *   representation of an integer.
 * - A value of `ERANGE` indicates that the input string is a valid integer,
 *   but the actual encoded value cannot be represented in an `int64_t`
 * - If the parse fails (returns non-zero), then the value at `*out` will remain
 *   unmodified.
 *
 * This differs from `strtoll` in that it requires that the entire string be
 * parsed as a valid integer. If parsing stops early, then the result will indicate
 * an error of EINVAL.
 */
static inline int
mlib_i64_parse(mstr_view in, unsigned base, int64_t *out)
{
   if (in.len == 0) {
      // Empty string is not a valid integer
      return EINVAL;
   }
   // Parse the possible sign prefix
   int sign = 1;
   // Check for a "+"
   if (in.data[0] == '+') {
      // Just a plus. Drop it and do nothing with it.
      in = mstr_substr(in, 1);
   }
   // Check for a negative prefix
   else if (in.data[0] == '-') {
      // Negative sign. We'll negate the value later.
      in = mstr_substr(in, 1);
      sign = -1;
   }

   // Infer the base value, if we have one
   if (base == 0) {
      if (in.len && in.data[0] == '0') {
         if (in.len > 1) {
            if (mlib_latin_tolower(in.data[1]) == 'x') {
               // Hexadecimal
               base = 16;
               in = mstr_substr(in, 2);
            } else if (mlib_latin_tolower(in.data[1]) == 'o') {
               // Octal
               base = 8;
               in = mstr_substr(in, 2);
            } else if (mlib_latin_tolower(in.data[1]) == 'b') {
               // Binary
               base = 2;
               in = mstr_substr(in, 2);
            }
         }
         if (base == 0) {
            // Other: Octal with a single "0" prefix. Don't trim this, because
            // it may be a literal "0"
            base = 8;
         }
      } else {
         // No '0' prefix. Treat it as decimal
         base = 10;
      }
   }

   // Try to parse the natural number now that we have removed all prefixes and
   // have a non-zero base.
   uint64_t nat;
   int rc = mlib_nat64_parse(in, base, &nat);
   if (rc) {
      return rc;
   }

   // Try to narrow from the u64 to i64 and apply the sign. This must be done as
   // one operation because of the pathological case of parsing INT64_MIN
   int64_t i64 = 0;
   if (mlib_mul(&i64, nat, sign)) {
      return ERANGE;
   }

   (void)(out && (*out = i64));
   return 0;
}

#define mlib_i64_parse(...) MLIB_ARGC_PICK(_mlib_i64_parse, __VA_ARGS__)
#define _mlib_i64_parse_argc_2(S, Ptr) _mlib_i64_parse_argc_3((S), 0, (Ptr))
#define _mlib_i64_parse_argc_3(S, Base, Ptr) mlib_i64_parse(mstr_view_from((S)), Base, Ptr)

/**
 * @brief Parse a 32-bit integer from a string.
 *
 * See `mlib_i64_parse` for more details.
 */
static inline int
mlib_i32_parse(mstr_view in, unsigned base, int32_t *out)
{
   int64_t tmp;
   int ec = mlib_i64_parse(in, base, &tmp);
   if (ec) {
      // Failed to parse the int64 value.
      return ec;
   }
   // Attempt to narrow to a 32-bit value
   int32_t i32 = 0;
   if (mlib_narrow(&i32, tmp)) {
      // Value is out-of-range
      return ERANGE;
   }
   // Success
   (void)(out && (*out = i32));
   return 0;
}

#define mlib_i32_parse(...) MLIB_ARGC_PICK(_mlib_i32_parse, __VA_ARGS__)
#define _mlib_i32_parse_argc_2(S, Ptr) _mlib_i32_parse_argc_3((S), 0, (Ptr))
#define _mlib_i32_parse_argc_3(S, Base, Ptr) mlib_i32_parse(mstr_view_from((S)), Base, Ptr)
