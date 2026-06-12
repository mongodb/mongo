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

#include <common-b64-private.h>
#include <common-bits-private.h>
#include <common-string-private.h>
#include <common-utf8-private.h>

#include <mlib/cmp.h>


mcommon_string_t *
mcommon_string_new_with_capacity(const char *str, uint32_t length, uint32_t min_capacity)
{
   BSON_ASSERT_PARAM(str);
   BSON_ASSERT(length < UINT32_MAX && min_capacity < UINT32_MAX);
   uint32_t capacity = BSON_MAX(length, min_capacity);
   uint32_t alloc = capacity + 1u;
   char *buffer = bson_malloc(alloc);
   memcpy(buffer, str, length);
   buffer[length] = '\0';
   return mcommon_string_new_with_buffer(buffer, length, alloc);
}

mcommon_string_t *
mcommon_string_new_with_buffer(char *buffer, uint32_t length, uint32_t alloc)
{
   BSON_ASSERT_PARAM(buffer);
   BSON_ASSERT(length < UINT32_MAX && alloc >= length + 1u);
   BSON_ASSERT(buffer[length] == '\0');
   mcommon_string_t *string = bson_malloc0(sizeof *string);
   string->str = buffer;
   string->len = length;
   string->alloc = alloc;
   return string;
}

void
mcommon_string_destroy(mcommon_string_t *string)
{
   if (string) {
      bson_free(mcommon_string_destroy_with_steal(string));
   }
}

char *
mcommon_string_destroy_with_steal(mcommon_string_t *string)
{
   if (string) {
      char *buffer = string->str;
      BSON_ASSERT(buffer[string->len] == '\0');
      bson_free(string);
      return buffer;
   } else {
      return NULL;
   }
}

void
mcommon_string_grow_to_capacity(mcommon_string_t *string, uint32_t capacity)
{
   BSON_ASSERT_PARAM(string);
   BSON_ASSERT(capacity < UINT32_MAX);
   uint32_t min_alloc_needed = capacity + 1u;
   if (string->alloc < min_alloc_needed) {
      uint32_t alloc = mcommon_next_power_of_two_u32(min_alloc_needed);
      string->str = bson_realloc(string->str, alloc);
      string->alloc = alloc;
   }
}

// Handle cases omitted from the inlined mcommon_string_append_bytes()
bool
mcommon_string_append_bytes_internal(mcommon_string_append_t *append, const char *str, uint32_t len)
{
   mcommon_string_t *string = append->_string;
   BSON_ASSERT(string);
   uint32_t old_len = string->len;
   uint32_t max_len = append->_max_len;
   BSON_ASSERT(max_len < UINT32_MAX);

   uint32_t max_append_len = old_len < max_len ? max_len - old_len : 0;
   uint32_t truncated_append_len = len;
   if (len > max_append_len) {
      // Search for an actual append length, <= the maximum allowed, which preserves UTF-8 validity
      append->_max_len_exceeded = true;
      truncated_append_len = mcommon_utf8_truncate_len(str, max_append_len);
   }

   uint32_t new_len = old_len + truncated_append_len;
   BSON_ASSERT(new_len <= max_len);
   mcommon_string_grow_to_capacity(string, new_len);
   char *buffer = string->str;

   memcpy(buffer + old_len, str, truncated_append_len);
   buffer[new_len] = '\0';
   string->len = new_len;

   return mcommon_string_status_from_append(append);
}

// Variant of mcommon_string_append_bytes() that grows but never truncates
bool
mcommon_string_append_bytes_all_or_none(mcommon_string_append_t *append, const char *str, uint32_t len)
{
   BSON_ASSERT_PARAM(append);
   BSON_ASSERT_PARAM(str);

   if (BSON_UNLIKELY(!mcommon_string_status_from_append(append))) {
      return false;
   }

   mcommon_string_t *string = append->_string;
   BSON_ASSERT(string);
   uint32_t old_len = string->len;
   uint32_t max_len = append->_max_len;
   BSON_ASSERT(max_len < UINT32_MAX);

   uint32_t max_append_len = old_len < max_len ? max_len - old_len : 0;
   if (len > max_append_len) {
      append->_max_len_exceeded = true;
      return false;
   }

   uint32_t new_len = old_len + len;
   BSON_ASSERT(new_len <= max_len);
   mcommon_string_grow_to_capacity(string, new_len);
   char *buffer = string->str;

   memcpy(buffer + old_len, str, len);
   buffer[new_len] = '\0';
   string->len = new_len;

   return mcommon_string_status_from_append(append);
}

bool
mcommon_string_append_unichar_internal(mcommon_string_append_t *append, bson_unichar_t unichar)
{
   mcommon_string_t *string = append->_string;
   uint32_t old_len = string->len;
   uint32_t max_len = append->_max_len;
   BSON_ASSERT(max_len < UINT32_MAX);

   char max_utf8_sequence[6];
   uint32_t max_append_len = old_len < max_len ? max_len - old_len : 0;

   // Usually we can write the UTF-8 sequence directly
   if (BSON_LIKELY(max_append_len >= sizeof max_utf8_sequence)) {
      uint32_t actual_sequence_len;
      mcommon_string_grow_to_capacity(string, old_len + sizeof max_utf8_sequence);
      char *buffer = string->str;
      mcommon_utf8_from_unichar(unichar, buffer + old_len, &actual_sequence_len);
      BSON_ASSERT(actual_sequence_len <= sizeof max_utf8_sequence);
      BSON_ASSERT(append->_max_len_exceeded == false);
      uint32_t new_len = old_len + actual_sequence_len;
      buffer[new_len] = '\0';
      string->len = new_len;
      return true;
   }

   // If we are near max_len, avoid growing the buffer beyond it.
   uint32_t actual_sequence_len;
   mcommon_utf8_from_unichar(unichar, max_utf8_sequence, &actual_sequence_len);
   return mcommon_string_append_bytes_internal(append, max_utf8_sequence, actual_sequence_len);
}

bool
mcommon_string_append_base64_encode(mcommon_string_append_t *append, const uint8_t *bytes, uint32_t len)
{
   BSON_ASSERT_PARAM(append);
   BSON_ASSERT_PARAM(bytes);

   if (BSON_UNLIKELY(!mcommon_string_status_from_append(append))) {
      return false;
   }

   mcommon_string_t *string = append->_string;
   uint32_t old_len = string->len;
   uint32_t max_len = append->_max_len;
   BSON_ASSERT(max_len < UINT32_MAX);
   uint32_t max_append_len = old_len < max_len ? max_len - old_len : 0;

   // Note that mcommon_b64_ntop_calculate_target_size includes room for NUL.
   // mcommon_b64_ntop includes NUL in the input (buffer size) but not in the return value (string length).
   size_t encoded_target_len = mcommon_b64_ntop_calculate_target_size((size_t)len) - 1;

   if (encoded_target_len <= (size_t)max_append_len) {
      // No truncation needed. Grow the buffer and encode directly.
      mcommon_string_grow_to_capacity(string, old_len + encoded_target_len);
      const int tgt = mcommon_b64_ntop(bytes, (size_t)len, string->str + old_len, encoded_target_len + 1);
      BSON_ASSERT(mlib_cmp(encoded_target_len, ==, tgt));
      BSON_ASSERT(mlib_in_range(uint32_t, encoded_target_len));
      string->len = old_len + (uint32_t)encoded_target_len;
      return true;
   } else if (max_append_len == 0) {
      // Truncation to a zero-length append
      mcommon_string_append_overflow(append);
      return false;
   } else {
      /* We expect to append at least one byte, and truncate.
       * Encoding only produces single-byte UTF-8 sequences, so the result always has exactly the maximum length.
       *
       * mcommon_b64_ntop() can't truncate without failing. To do this without allocating a full size temporary buffer
       * or rewriting mcommon_b64_ntop, we can partition the write into three parts: a 'direct' portion made from entire
       * non-truncated units of 3 bytes in and 4 characters out, a truncated 'remainder', and an ignored portion.
       * Remainders longer than 3 bytes in / 4 bytes out are never necessary, and further portions of the input data
       * will not be used.
       */
      mcommon_string_grow_to_capacity(string, max_len);
      char *buffer = string->str;

      uint32_t remainder_truncated_len = max_append_len % 4;
      uint32_t direct_encoded_len = max_append_len - remainder_truncated_len;
      uint32_t direct_input_len = mcommon_b64_pton_calculate_target_size((size_t)direct_encoded_len);
      BSON_ASSERT(direct_input_len % 3 == 0);
      BSON_ASSERT(direct_input_len < len);
      const int tgt = mcommon_b64_ntop(bytes, (size_t)direct_input_len, string->str + old_len, direct_encoded_len + 1);
      BSON_ASSERT(mlib_cmp(direct_encoded_len, ==, tgt));

      char remainder_buffer[5];
      uint32_t remainder_input_len = BSON_MIN(3, len - direct_input_len);
      BSON_ASSERT(remainder_input_len > 0);
      uint32_t remainder_encoded_len = mcommon_b64_ntop_calculate_target_size((size_t)remainder_input_len) - 1;
      BSON_ASSERT(remainder_encoded_len > remainder_truncated_len);
      const int t2 = mcommon_b64_ntop(
         bytes + direct_input_len, (size_t)remainder_input_len, remainder_buffer, sizeof remainder_buffer);
      BSON_ASSERT(mlib_cmp(remainder_encoded_len, ==, t2));
      memcpy(buffer + old_len + direct_encoded_len, remainder_buffer, remainder_encoded_len);

      BSON_ASSERT(old_len + direct_encoded_len + remainder_truncated_len == max_len);
      buffer[max_len] = '\0';
      string->len = max_len;
      mcommon_string_append_overflow(append);
      return false;
   }
}

bool
mcommon_string_append_oid_as_hex(mcommon_string_append_t *append, const bson_oid_t *value)
{
   BSON_ASSERT_PARAM(append);
   BSON_ASSERT_PARAM(value);

   char oid_str[25];
   bson_oid_to_string(value, oid_str);
   return mcommon_string_append(append, oid_str);
}

bool
mcommon_string_append_selected_chars(mcommon_string_append_t *append,
                                     const char *tmplt,
                                     const char *selector,
                                     size_t selector_len)
{
   BSON_ASSERT_PARAM(append);
   BSON_ASSERT_PARAM(tmplt);
   BSON_ASSERT_PARAM(selector);

   for (uint8_t template_char; (template_char = (uint8_t)*tmplt); tmplt++) {
      BSON_ASSERT(template_char <= 0x7f);
      if (memchr(selector, template_char, selector_len) && !mcommon_string_append_unichar(append, template_char)) {
         return false;
      }
   }
   return mcommon_string_status_from_append(append);
}

bool
mcommon_string_append_printf(mcommon_string_append_t *append, const char *format, ...)
{
   BSON_ASSERT_PARAM(append);
   BSON_ASSERT_PARAM(format);

   va_list args;
   va_start(args, format);
   bool ret = mcommon_string_append_vprintf(append, format, args);
   va_end(args);
   return ret;
}

bool
mcommon_string_append_vprintf(mcommon_string_append_t *append, const char *format, va_list args)
{
   BSON_ASSERT_PARAM(append);
   BSON_ASSERT_PARAM(format);

   if (BSON_UNLIKELY(!mcommon_string_status_from_append(append))) {
      return false;
   }

   mcommon_string_t *string = append->_string;
   uint32_t old_len = string->len;
   uint32_t max_len = append->_max_len;
   BSON_ASSERT(max_len < UINT32_MAX);
   uint32_t max_append_len = old_len < max_len ? max_len - old_len : 0;

   // Initial minimum buffer length; increases on retry.
   uint32_t min_format_buffer_capacity = 16;

   while (true) {
      // Allocate room for a format buffer at the end of the string.
      // It will be at least this round's min_format_buffer_capacity, but if we happen to have extra space allocated we
      // do want that to be available to vsnprintf().

      min_format_buffer_capacity = BSON_MIN(min_format_buffer_capacity, max_append_len);
      mcommon_string_grow_to_capacity(string, old_len + min_format_buffer_capacity);
      uint32_t alloc = string->alloc;
      BSON_ASSERT(alloc > 0 && alloc - 1u >= old_len);
      char *format_buffer = string->str + old_len;
      uint32_t actual_format_buffer_capacity = BSON_MIN(alloc - 1u - old_len, max_append_len);
      BSON_ASSERT(actual_format_buffer_capacity >= min_format_buffer_capacity);
      BSON_ASSERT(actual_format_buffer_capacity < UINT32_MAX);
      uint32_t format_buffer_alloc = actual_format_buffer_capacity + 1u;

      va_list args_copy;
      va_copy(args_copy, args);
      int format_result = bson_vsnprintf(format_buffer, format_buffer_alloc, format, args_copy);
      va_end(args_copy);

      if (format_result > -1 && mlib_in_range(uint32_t, format_result) &&
          (uint32_t)format_result <= actual_format_buffer_capacity) {
         // Successful result, no truncation.
         format_buffer[format_result] = '\0';
         string->len = old_len + (uint32_t)format_result;
         BSON_ASSERT(string->len <= append->_max_len);
         BSON_ASSERT(append->_max_len_exceeded == false);
         return true;
      }

      if (actual_format_buffer_capacity == max_append_len) {
         // No more space to grow into, this must be the final result.

         if (format_result > -1 && mlib_in_range(uint32_t, format_result) && (uint32_t)format_result < UINT32_MAX) {
            // We have truncated output from vsnprintf. Clean it up by removing
            // any partial UTF-8 sequences that might be left on the end.
            uint32_t truncated_append_len = mcommon_utf8_truncate_len(
               format_buffer, BSON_MIN(actual_format_buffer_capacity, (uint32_t)format_result));
            BSON_ASSERT(truncated_append_len <= actual_format_buffer_capacity);
            format_buffer[truncated_append_len] = '\0';
            string->len = old_len + truncated_append_len;
            append->_max_len_exceeded = true;
            return false;
         }

         // Error from vsnprintf; This operation fails, but we do not set max_len_exceeded.
         return false;
      }

      // Choose a larger format_buffer_len and try again. Length will be clamped to max_append_len above.
      if (format_result > -1 && mlib_in_range(uint32_t, format_result) && (uint32_t)format_result < UINT32_MAX) {
         min_format_buffer_capacity = (uint32_t)format_result + 1u;
      } else if (min_format_buffer_capacity < UINT32_MAX / 2) {
         min_format_buffer_capacity *= 2;
      } else {
         min_format_buffer_capacity = UINT32_MAX - 1u;
      }
   }
}
