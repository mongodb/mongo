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


#include <bson/bson-utf8.h>

#include <common-json-private.h>
#include <common-macros-private.h>
#include <common-string-private.h>
#include <common-utf8-private.h>

#include <bson/memory.h>

#include <mlib/cmp.h>

#include <string.h>


/*
 *--------------------------------------------------------------------------
 *
 * bson_utf8_validate --
 *
 *       Validates that @utf8 is a valid UTF-8 string. Note that we only
 *       support UTF-8 characters which have sequence length less than or equal
 *       to 4 bytes (RFC 3629).
 *
 *       If @allow_null is true, then \0 is allowed within @utf8_len bytes
 *       of @utf8.  Generally, this is bad practice since the main point of
 *       UTF-8 strings is that they can be used with strlen() and friends.
 *       However, some languages such as Python can send UTF-8 encoded
 *       strings with NUL's in them.
 *
 *       Note that the two-byte sequence "C0 80" is also interpreted as an
 *       internal NUL, for historical reasons. This sequence is considered
 *       invalid according to RFC3629.
 *
 * Parameters:
 *       @utf8: A UTF-8 encoded string.
 *       @utf8_len: The length of @utf8 in bytes.
 *       @allow_null: If the single "00" byte or two-byte sequence "C0 80" are allowed internally within @utf8.
 *
 * Returns:
 *       true if @utf8 is valid UTF-8. otherwise false.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

bool
bson_utf8_validate(const char *utf8, /* IN */
                   size_t utf8_len,  /* IN */
                   bool allow_null)  /* IN */
{
   bson_unichar_t c;
   uint8_t first_mask;
   uint8_t seq_length;
   size_t i;
   size_t j;

   BSON_ASSERT(utf8);

   for (i = 0; i < utf8_len; i += seq_length) {
      mcommon_utf8_get_sequence(&utf8[i], &seq_length, &first_mask);

      /*
       * Ensure we have a valid multi-byte sequence length.
       */
      if (!seq_length) {
         return false;
      }

      /*
       * Ensure we have enough bytes left.
       */
      if ((utf8_len - i) < seq_length) {
         return false;
      }

      /*
       * Also calculate the next char as a unichar so we can
       * check code ranges for non-shortest form.
       */
      c = utf8[i] & first_mask;

      /*
       * Check the high-bits for each additional sequence byte.
       */
      for (j = i + 1; j < (i + seq_length); j++) {
         c = (c << 6) | (utf8[j] & 0x3F);
         if ((utf8[j] & 0xC0) != 0x80) {
            return false;
         }
      }

      /*
       * Check for NULL bytes afterwards.
       *
       * Hint: if you want to optimize this function, starting here to do
       * this in the same pass as the data above would probably be a good
       * idea. You would add a branch into the inner loop, but save possibly
       * on cache-line bouncing on larger strings. Just a thought.
       */
      if (!allow_null) {
         for (j = 0; j < seq_length; j++) {
            if (((i + j) > utf8_len) || !utf8[i + j]) {
               return false;
            }
         }
      }

      /*
       * Code point won't fit in utf-16, not allowed.
       */
      if (c > 0x0010FFFF) {
         return false;
      }

      /*
       * Byte is in reserved range for UTF-16 high-marks
       * for surrogate pairs.
       */
      if ((c & 0xFFFFF800) == 0xD800) {
         return false;
      }

      /*
       * Check non-shortest form unicode.
       */
      switch (seq_length) {
      case 1:
         if (c <= 0x007F) {
            continue;
         }
         return false;

      case 2:
         if ((c >= 0x0080) && (c <= 0x07FF)) {
            continue;
         } else if (c == 0) {
            /* Two-byte representation for NULL. */
            if (!allow_null) {
               return false;
            }
            continue;
         }
         return false;

      case 3:
         if (((c >= 0x0800) && (c <= 0x0FFF)) || ((c >= 0x1000) && (c <= 0xFFFF))) {
            continue;
         }
         return false;

      case 4:
         if (((c >= 0x10000) && (c <= 0x3FFFF)) || ((c >= 0x40000) && (c <= 0xFFFFF)) ||
             ((c >= 0x100000) && (c <= 0x10FFFF))) {
            continue;
         }
         return false;

      default:
         return false;
      }
   }

   return true;
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_utf8_escape_for_json --
 *
 *       Allocates a new string matching @utf8 except that special
 *       characters in JSON will be escaped. The resulting string is also
 *       UTF-8 encoded.
 *
 *       Both " and \ characters will be escaped. Additionally, if a NUL
 *       byte is found before @utf8_len bytes, it will be converted to the
 *       two byte UTF-8 sequence.
 *
 *       The two-byte sequence "C0 80" is also interpreted as an internal NUL,
 *       for historical reasons. This sequence is considered invalid according
 *       to RFC3629.
 *
 * Parameters:
 *       @utf8: A UTF-8 encoded string.
 *       @utf8_len: The length of @utf8 in bytes or -1 if NUL terminated.
 *
 * Returns:
 *       A newly allocated string that should be freed with bson_free().
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

char *
bson_utf8_escape_for_json(const char *utf8, /* IN */
                          ssize_t utf8_len) /* IN */
{
   uint32_t len32;
   bool allow_nul;

   if (utf8_len < 0) {
      size_t sizet_len = strlen(utf8);
      if (sizet_len < UINT32_MAX) {
         len32 = (uint32_t)sizet_len;
         allow_nul = false;
      } else {
         return NULL;
      }
   } else {
      if (mlib_in_range(uint32_t, utf8_len) && (uint32_t)utf8_len < UINT32_MAX) {
         len32 = utf8_len;
         allow_nul = true;
      } else {
         return NULL;
      }
   }

   /* The new private implementation of mcommon_json_append_escaped() avoids
    * parsing UTF-8 sequences at all in most cases. It preserves the validity
    * of valid sequences, but it will not catch most UTF-8 errors. For compatibility
    * at the expense of performance, we emulate the old behavior in this wrapper.
    */
   if (!bson_utf8_validate(utf8, (size_t)len32, allow_nul)) {
      return NULL;
   }

   mcommon_string_append_t append;
   mcommon_string_new_with_capacity_as_append(&append, len32);
   if (mcommon_json_append_escaped(&append, utf8, len32, allow_nul)) {
      return mcommon_string_from_append_destroy_with_steal(&append);
   } else {
      mcommon_string_from_append_destroy(&append);
      return NULL;
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_utf8_get_char --
 *
 *       Fetches the next UTF-8 character from the UTF-8 sequence.
 *
 * Parameters:
 *       @utf8: A string containing validated UTF-8.
 *
 * Returns:
 *       A 32-bit bson_unichar_t reprsenting the multi-byte sequence.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

bson_unichar_t
bson_utf8_get_char(const char *utf8) /* IN */
{
   bson_unichar_t c;
   uint8_t mask;
   uint8_t num;
   int i;

   BSON_ASSERT(utf8);

   mcommon_utf8_get_sequence(utf8, &num, &mask);
   c = (*utf8) & mask;

   for (i = 1; i < num; i++) {
      c = (c << 6) | (utf8[i] & 0x3F);
   }

   return c;
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_utf8_next_char --
 *
 *       Returns an incremented pointer to the beginning of the next
 *       multi-byte sequence in @utf8.
 *
 * Parameters:
 *       @utf8: A string containing validated UTF-8.
 *
 * Returns:
 *       An incremented pointer in @utf8.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

const char *
bson_utf8_next_char(const char *utf8) /* IN */
{
   uint8_t mask;
   uint8_t num;

   BSON_ASSERT(utf8);

   mcommon_utf8_get_sequence(utf8, &num, &mask);

   return utf8 + num;
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_utf8_from_unichar --
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

void
bson_utf8_from_unichar(bson_unichar_t unichar,                     /* IN */
                       char utf8[BSON_ENSURE_ARRAY_PARAM_SIZE(6)], /* OUT */
                       uint32_t *len)                              /* OUT */
{
   // Inlined implementation from common-utf8-private
   mcommon_utf8_from_unichar(unichar, utf8, len);
}
