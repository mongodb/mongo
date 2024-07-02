/*
 * Copyright 2022 MongoDB, Inc.
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

#include <bson/bson-prelude.h>


#ifndef BSON_CMP_H
#define BSON_CMP_H


#include <bson/bson-compat.h> /* ssize_t */
#include <bson/bson-macros.h> /* BSON_CONCAT */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>


BSON_BEGIN_DECLS


/* Based on the "Safe Integral Comparisons" proposal merged in C++20:
 * http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0586r2.html
 *
 * Due to lack of type deduction in C, relational comparison functions (e.g.
 * `cmp_less`) are defined in sets of four "functions" according to the
 * signedness of each value argument, e.g.:
 *  - bson_cmp_less_ss (signed-value, signed-value)
 *  - bson_cmp_less_uu (unsigned-value, unsigned-value)
 *  - bson_cmp_less_su (signed-value, unsigned-value)
 *  - bson_cmp_less_us (unsigned-value, signed-value)
 *
 * Similarly, the `in_range` function is defined as a set of two "functions"
 * according to the signedness of the value argument:
 *  - bson_in_range_signed (Type, signed-value)
 *  - bson_in_range_unsigned (Type, unsigned-value)
 *
 * The user must take care to use the correct signedness for the provided
 * argument(s). Enabling compiler warnings for implicit sign conversions is
 * recommended.
 */


#define BSON_CMP_SET(op, ss, uu, su, us)                                              \
   static BSON_INLINE bool BSON_CONCAT3 (bson_cmp_, op, _ss) (int64_t t, int64_t u)   \
   {                                                                                  \
      return (ss);                                                                    \
   }                                                                                  \
                                                                                      \
   static BSON_INLINE bool BSON_CONCAT3 (bson_cmp_, op, _uu) (uint64_t t, uint64_t u) \
   {                                                                                  \
      return (uu);                                                                    \
   }                                                                                  \
                                                                                      \
   static BSON_INLINE bool BSON_CONCAT3 (bson_cmp_, op, _su) (int64_t t, uint64_t u)  \
   {                                                                                  \
      return (su);                                                                    \
   }                                                                                  \
                                                                                      \
   static BSON_INLINE bool BSON_CONCAT3 (bson_cmp_, op, _us) (uint64_t t, int64_t u)  \
   {                                                                                  \
      return (us);                                                                    \
   }

BSON_CMP_SET (equal, t == u, t == u, t < 0 ? false : (uint64_t) (t) == u, u < 0 ? false : t == (uint64_t) (u))

BSON_CMP_SET (not_equal,
              !bson_cmp_equal_ss (t, u),
              !bson_cmp_equal_uu (t, u),
              !bson_cmp_equal_su (t, u),
              !bson_cmp_equal_us (t, u))

BSON_CMP_SET (less, t < u, t < u, t < 0 ? true : (uint64_t) (t) < u, u < 0 ? false : t < (uint64_t) (u))

BSON_CMP_SET (
   greater, bson_cmp_less_ss (u, t), bson_cmp_less_uu (u, t), bson_cmp_less_us (u, t), bson_cmp_less_su (u, t))

BSON_CMP_SET (less_equal,
              !bson_cmp_greater_ss (t, u),
              !bson_cmp_greater_uu (t, u),
              !bson_cmp_greater_su (t, u),
              !bson_cmp_greater_us (t, u))

BSON_CMP_SET (greater_equal,
              !bson_cmp_less_ss (t, u),
              !bson_cmp_less_uu (t, u),
              !bson_cmp_less_su (t, u),
              !bson_cmp_less_us (t, u))

#undef BSON_CMP_SET


/* Return true if the given value is within the range of the corresponding
 * signed type. The suffix must match the signedness of the given value. */
#define BSON_IN_RANGE_SET_SIGNED(Type, min, max)                                             \
   static BSON_INLINE bool BSON_CONCAT3 (bson_in_range, _##Type, _signed) (int64_t value)    \
   {                                                                                         \
      return bson_cmp_greater_equal_ss (value, min) && bson_cmp_less_equal_ss (value, max);  \
   }                                                                                         \
                                                                                             \
   static BSON_INLINE bool BSON_CONCAT3 (bson_in_range, _##Type, _unsigned) (uint64_t value) \
   {                                                                                         \
      return bson_cmp_greater_equal_us (value, min) && bson_cmp_less_equal_us (value, max);  \
   }

/* Return true if the given value is within the range of the corresponding
 * unsigned type. The suffix must match the signedness of the given value. */
#define BSON_IN_RANGE_SET_UNSIGNED(Type, max)                                                \
   static BSON_INLINE bool BSON_CONCAT3 (bson_in_range, _##Type, _signed) (int64_t value)    \
   {                                                                                         \
      return bson_cmp_greater_equal_su (value, 0u) && bson_cmp_less_equal_su (value, max);   \
   }                                                                                         \
                                                                                             \
   static BSON_INLINE bool BSON_CONCAT3 (bson_in_range, _##Type, _unsigned) (uint64_t value) \
   {                                                                                         \
      return bson_cmp_less_equal_uu (value, max);                                            \
   }

BSON_IN_RANGE_SET_SIGNED (signed_char, SCHAR_MIN, SCHAR_MAX)
BSON_IN_RANGE_SET_SIGNED (short, SHRT_MIN, SHRT_MAX)
BSON_IN_RANGE_SET_SIGNED (int, INT_MIN, INT_MAX)
BSON_IN_RANGE_SET_SIGNED (long, LONG_MIN, LONG_MAX)
BSON_IN_RANGE_SET_SIGNED (long_long, LLONG_MIN, LLONG_MAX)

BSON_IN_RANGE_SET_UNSIGNED (unsigned_char, UCHAR_MAX)
BSON_IN_RANGE_SET_UNSIGNED (unsigned_short, USHRT_MAX)
BSON_IN_RANGE_SET_UNSIGNED (unsigned_int, UINT_MAX)
BSON_IN_RANGE_SET_UNSIGNED (unsigned_long, ULONG_MAX)
BSON_IN_RANGE_SET_UNSIGNED (unsigned_long_long, ULLONG_MAX)

BSON_IN_RANGE_SET_SIGNED (int8_t, INT8_MIN, INT8_MAX)
BSON_IN_RANGE_SET_SIGNED (int16_t, INT16_MIN, INT16_MAX)
BSON_IN_RANGE_SET_SIGNED (int32_t, INT32_MIN, INT32_MAX)
BSON_IN_RANGE_SET_SIGNED (int64_t, INT64_MIN, INT64_MAX)

BSON_IN_RANGE_SET_UNSIGNED (uint8_t, UINT8_MAX)
BSON_IN_RANGE_SET_UNSIGNED (uint16_t, UINT16_MAX)
BSON_IN_RANGE_SET_UNSIGNED (uint32_t, UINT32_MAX)
BSON_IN_RANGE_SET_UNSIGNED (uint64_t, UINT64_MAX)

BSON_IN_RANGE_SET_SIGNED (ssize_t, SSIZE_MIN, SSIZE_MAX)
BSON_IN_RANGE_SET_UNSIGNED (size_t, SIZE_MAX)

#undef BSON_IN_RANGE_SET_SIGNED
#undef BSON_IN_RANGE_SET_UNSIGNED


/* Return true if the value with *signed* type is in the representable range of
 * Type and false otherwise. */
#define bson_in_range_signed(Type, value) BSON_CONCAT3 (bson_in_range, _##Type, _signed) (value)

/* Return true if the value with *unsigned* type is in the representable range
 * of Type and false otherwise. */
#define bson_in_range_unsigned(Type, value) BSON_CONCAT3 (bson_in_range, _##Type, _unsigned) (value)


BSON_END_DECLS


#endif /* BSON_CMP_H */
