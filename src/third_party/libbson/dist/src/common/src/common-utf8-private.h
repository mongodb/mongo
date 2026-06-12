/*
 * Copyright 2009-present MongoDB, Inc.
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

#include <common-prelude.h>

#ifndef MONGO_C_DRIVER_COMMON_UTF8_PRIVATE_H
#define MONGO_C_DRIVER_COMMON_UTF8_PRIVATE_H

#include <bson/bson.h>


/*
 *--------------------------------------------------------------------------
 *
 * mcommon_utf8_get_sequence --
 *
 *       Determine the sequence length of the first UTF-8 character in
 *       @utf8. The sequence length is stored in @seq_length and the mask
 *       for the first character is stored in @first_mask.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       @seq_length is set.
 *       @first_mask is set.
 *
 *--------------------------------------------------------------------------
 */

static BSON_INLINE void
mcommon_utf8_get_sequence(const char *utf8,    /* IN */
                          uint8_t *seq_length, /* OUT */
                          uint8_t *first_mask) /* OUT */
{
   unsigned char c = *(const unsigned char *)utf8;
   uint8_t m;
   uint8_t n;

   /*
    * See the following[1] for a description of what the given multi-byte
    * sequences will be based on the bits set of the first byte. We also need
    * to mask the first byte based on that.  All subsequent bytes are masked
    * against 0x3F.
    *
    * [1] http://www.joelonsoftware.com/articles/Unicode.html
    */

   if ((c & 0x80) == 0) {
      n = 1;
      m = 0x7F;
   } else if ((c & 0xE0) == 0xC0) {
      n = 2;
      m = 0x1F;
   } else if ((c & 0xF0) == 0xE0) {
      n = 3;
      m = 0x0F;
   } else if ((c & 0xF8) == 0xF0) {
      n = 4;
      m = 0x07;
   } else {
      n = 0;
      m = 0;
   }

   *seq_length = n;
   *first_mask = m;
}


/*
 *--------------------------------------------------------------------------
 *
 * mcommon_utf8_from_unichar --
 *
 *       Converts the unichar to a sequence of utf8 bytes and stores those
 *       in @utf8. The number of bytes in the sequence are stored in @len.
 *
 * Parameters:
 *       @unichar: A bson_unichar_t.
 *       @utf8: A location for the multi-byte sequence.
 *       @len: A location for number of bytes stored in @utf8.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       @utf8 is set.
 *       @len is set.
 *
 *--------------------------------------------------------------------------
 */

static BSON_INLINE void
mcommon_utf8_from_unichar(bson_unichar_t unichar,                     /* IN */
                          char utf8[BSON_ENSURE_ARRAY_PARAM_SIZE(6)], /* OUT */
                          uint32_t *len)                              /* OUT */
{
   BSON_ASSERT_PARAM(len);

   if (unichar <= 0x7F) {
      utf8[0] = unichar;
      *len = 1;
   } else if (unichar <= 0x7FF) {
      *len = 2;
      utf8[0] = 0xC0 | ((unichar >> 6) & 0x3F);
      utf8[1] = 0x80 | ((unichar) & 0x3F);
   } else if (unichar <= 0xFFFF) {
      *len = 3;
      utf8[0] = 0xE0 | ((unichar >> 12) & 0xF);
      utf8[1] = 0x80 | ((unichar >> 6) & 0x3F);
      utf8[2] = 0x80 | ((unichar) & 0x3F);
   } else if (unichar <= 0x1FFFFF) {
      *len = 4;
      utf8[0] = 0xF0 | ((unichar >> 18) & 0x7);
      utf8[1] = 0x80 | ((unichar >> 12) & 0x3F);
      utf8[2] = 0x80 | ((unichar >> 6) & 0x3F);
      utf8[3] = 0x80 | ((unichar) & 0x3F);
   } else {
      *len = 0;
   }
}


/*
 * @brief Calculate a truncation length that preserves UTF-8 validity
 * @param str String data, at least 'len' bytes long.
 * @returns A new length <= 'len'
 *
 * When 'str' is a valid UTF-8 string with length >= 'len' bytes,
 * this calculates a new length, less than or equal to 'len', which
 * guarantees that the string will be truncated in-between code points.
 */

static BSON_INLINE uint32_t
mcommon_utf8_truncate_len(const char *str, uint32_t len)
{
   uint32_t resulting_len = len;
   while (resulting_len > 0) {
      if (BSON_LIKELY((uint8_t)str[resulting_len - 1u] <= 0x7f)) {
         // Single-byte sequence, always a fine place to stop
         return resulting_len;
      }

      // Search for the last byte that could begin a UTF-8 sequence
      uint32_t seq_begin_at = resulting_len - 1u;
      while (((uint8_t)str[seq_begin_at] & 0xc0) == 0x80) {
         if (seq_begin_at > 0) {
            seq_begin_at--;
         } else {
            return 0;
         }
      }

      uint8_t seq_length, first_mask_unused;
      mcommon_utf8_get_sequence(str + seq_begin_at, &seq_length, &first_mask_unused);
      if (seq_begin_at + seq_length == resulting_len) {
         // Sequence is complete, we can truncate here.
         return resulting_len;
      }

      // Sequence was truncated or invalid; resume search prior to it's beginning.
      resulting_len = seq_begin_at;
   }
   return 0;
}


#endif /* MONGO_C_DRIVER_COMMON_UTF8_PRIVATE_H */
