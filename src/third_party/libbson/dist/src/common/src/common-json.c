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


#include <common-json-private.h>
#include <common-string-private.h>
#include <common-utf8-private.h>

#include <bson/bson.h>

#include <string.h>


typedef struct {
   mcommon_string_append_t *append;
   unsigned max_depth;
   bson_json_mode_t mode;
   bool has_keys;
   bool not_first_item;
   bool is_corrupt;
} mcommon_json_append_visit_t;


static bool
mcommon_json_append_visit_utf8(
   const bson_iter_t *iter, const char *key, size_t v_utf8_len, const char *v_utf8, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (!mlib_in_range(uint32_t, v_utf8_len)) {
      mcommon_string_append_overflow(state->append);
      return true;
   }
   return !mcommon_json_append_value_utf8(state->append, v_utf8, (uint32_t)v_utf8_len, true);
}

static bool
mcommon_json_append_visit_int32(const bson_iter_t *iter, const char *key, int32_t v_int32, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_int32(state->append, v_int32, state->mode);
}

static bool
mcommon_json_append_visit_int64(const bson_iter_t *iter, const char *key, int64_t v_int64, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_int64(state->append, v_int64, state->mode);
}

static bool
mcommon_json_append_visit_decimal128(const bson_iter_t *iter,
                                     const char *key,
                                     const bson_decimal128_t *value,
                                     void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_decimal128(state->append, value);
}

static bool
mcommon_json_append_visit_double(const bson_iter_t *iter, const char *key, double v_double, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_double(state->append, v_double, state->mode);
}

static bool
mcommon_json_append_visit_undefined(const bson_iter_t *iter, const char *key, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_undefined(state->append);
}

static bool
mcommon_json_append_visit_null(const bson_iter_t *iter, const char *key, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_null(state->append);
}

static bool
mcommon_json_append_visit_oid(const bson_iter_t *iter, const char *key, const bson_oid_t *oid, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_oid(state->append, oid);
}

static bool
mcommon_json_append_visit_binary(const bson_iter_t *iter,
                                 const char *key,
                                 bson_subtype_t v_subtype,
                                 size_t v_binary_len,
                                 const uint8_t *v_binary,
                                 void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (!mlib_in_range(uint32_t, v_binary_len)) {
      mcommon_string_append_overflow(state->append);
      return true;
   }
   return !mcommon_json_append_value_binary(state->append, v_subtype, v_binary, (uint32_t)v_binary_len, state->mode);
}

static bool
mcommon_json_append_visit_bool(const bson_iter_t *iter, const char *key, bool v_bool, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_bool(state->append, v_bool);
}

static bool
mcommon_json_append_visit_date_time(const bson_iter_t *iter, const char *key, int64_t msec_since_epoch, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_date_time(state->append, msec_since_epoch, state->mode);
}

static bool
mcommon_json_append_visit_regex(
   const bson_iter_t *iter, const char *key, const char *v_regex, const char *v_options, void *data)
{
   mcommon_json_append_visit_t *state = data;
   size_t v_regex_len = strlen(v_regex);
   size_t v_options_len = strlen(v_options);
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (!mlib_in_range(uint32_t, v_regex_len)) {
      mcommon_string_append_overflow(state->append);
      return true;
   }
   return !mcommon_json_append_value_regex(
      state->append, v_regex, (uint32_t)v_regex_len, v_options, v_options_len, state->mode);
}

static bool
mcommon_json_append_visit_timestamp(
   const bson_iter_t *iter, const char *key, uint32_t v_timestamp, uint32_t v_increment, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_timestamp(state->append, v_timestamp, v_increment);
}

static bool
mcommon_json_append_visit_dbpointer(const bson_iter_t *iter,
                                    const char *key,
                                    size_t v_collection_len,
                                    const char *v_collection,
                                    const bson_oid_t *v_oid,
                                    void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (!mlib_in_range(uint32_t, v_collection_len)) {
      mcommon_string_append_overflow(state->append);
      return true;
   }
   return !mcommon_json_append_value_dbpointer(
      state->append, v_collection, (uint32_t)v_collection_len, v_oid, state->mode);
}

static bool
mcommon_json_append_visit_minkey(const bson_iter_t *iter, const char *key, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_minkey(state->append);
}

static bool
mcommon_json_append_visit_maxkey(const bson_iter_t *iter, const char *key, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_json_append_value_maxkey(state->append);
}

static bool
mcommon_json_append_visit_before(const bson_iter_t *iter, const char *key, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);

   if (!mcommon_string_status_from_append(state->append)) {
      return true;
   }

   if (state->not_first_item) {
      if (!mcommon_json_append_separator(state->append)) {
         return true;
      }
   } else {
      state->not_first_item = true;
   }

   if (state->has_keys) {
      size_t key_len = strlen(key);
      if (!mlib_in_range(uint32_t, key_len)) {
         mcommon_string_append_overflow(state->append);
         return true;
      }
      if (!mcommon_json_append_key(state->append, key, (uint32_t)key_len)) {
         return true;
      }
   }

   return false;
}

static bool
mcommon_json_append_visit_after(const bson_iter_t *iter, const char *key, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   return !mcommon_string_status_from_append(state->append);
}

static void
mcommon_json_append_visit_corrupt(const bson_iter_t *iter, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   state->is_corrupt = true;
}

static bool
mcommon_json_append_visit_code(
   const bson_iter_t *iter, const char *key, size_t v_code_len, const char *v_code, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (!mlib_in_range(uint32_t, v_code_len)) {
      mcommon_string_append_overflow(state->append);
      return true;
   }
   return !mcommon_json_append_value_code(state->append, v_code, (uint32_t)v_code_len);
}

static bool
mcommon_json_append_visit_symbol(
   const bson_iter_t *iter, const char *key, size_t v_symbol_len, const char *v_symbol, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (!mlib_in_range(uint32_t, v_symbol_len)) {
      mcommon_string_append_overflow(state->append);
      return true;
   }
   return !mcommon_json_append_value_symbol(state->append, v_symbol, (uint32_t)v_symbol_len, state->mode);
}

static bool
mcommon_json_append_visit_codewscope(
   const bson_iter_t *iter, const char *key, size_t v_code_len, const char *v_code, const bson_t *v_scope, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (!mlib_in_range(uint32_t, v_code_len)) {
      mcommon_string_append_overflow(state->append);
      return true;
   }
   if (mcommon_json_append_value_codewscope(
          state->append, v_code, (uint32_t)v_code_len, v_scope, state->mode, state->max_depth)) {
      return !mcommon_string_status_from_append(state->append);
   } else {
      state->is_corrupt = true;
      return true;
   }
}

static bool
mcommon_json_append_visit_document(const bson_iter_t *iter, const char *key, const bson_t *v_document, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (mcommon_json_append_bson_document(state->append, v_document, state->mode, state->max_depth)) {
      return !mcommon_string_status_from_append(state->append);
   } else {
      state->is_corrupt = true;
      return true;
   }
}

static bool
mcommon_json_append_visit_array(const bson_iter_t *iter, const char *key, const bson_t *v_array, void *data)
{
   mcommon_json_append_visit_t *state = data;
   BSON_UNUSED(iter);
   BSON_UNUSED(key);
   if (mcommon_json_append_bson_array(state->append, v_array, state->mode, state->max_depth)) {
      return !mcommon_string_status_from_append(state->append);
   } else {
      state->is_corrupt = true;
      return true;
   }
}

bool
mcommon_json_append_bson_values(
   mcommon_string_append_t *append, const bson_t *bson, bson_json_mode_t mode, bool has_keys, unsigned max_depth)
{
   mcommon_json_append_visit_t state = {.append = append, .max_depth = max_depth, .mode = mode, .has_keys = has_keys};
   bson_iter_t iter;
   if (!bson_iter_init(&iter, bson)) {
      return false;
   }
   static const bson_visitor_t visitors = {
      mcommon_json_append_visit_before,     mcommon_json_append_visit_after,     mcommon_json_append_visit_corrupt,
      mcommon_json_append_visit_double,     mcommon_json_append_visit_utf8,      mcommon_json_append_visit_document,
      mcommon_json_append_visit_array,      mcommon_json_append_visit_binary,    mcommon_json_append_visit_undefined,
      mcommon_json_append_visit_oid,        mcommon_json_append_visit_bool,      mcommon_json_append_visit_date_time,
      mcommon_json_append_visit_null,       mcommon_json_append_visit_regex,     mcommon_json_append_visit_dbpointer,
      mcommon_json_append_visit_code,       mcommon_json_append_visit_symbol,    mcommon_json_append_visit_codewscope,
      mcommon_json_append_visit_int32,      mcommon_json_append_visit_timestamp, mcommon_json_append_visit_int64,
      mcommon_json_append_visit_maxkey,     mcommon_json_append_visit_minkey,    NULL, /* visit_unsupported_type */
      mcommon_json_append_visit_decimal128,
   };
   /* Note that early exit from bson_iter_visit_all does not affect our success, which is based only on BSON validity.
    * BSON errors will set is_corrupt if they prevent full traversal, but non-fatal parse errors (like invalid UTF-8)
    * may let bson_iter_visit_all() succeed while leaving an error status in iter.err_off. */
   (void)bson_iter_visit_all(&iter, &visitors, &state);
   return iter.err_off == 0 && !state.is_corrupt;
}

static BSON_INLINE bool
mcommon_json_append_bson_container(mcommon_string_append_t *append,
                                   const bson_t *bson,
                                   bson_json_mode_t mode,
                                   unsigned max_depth,
                                   bool has_keys,
                                   const char *empty,
                                   const char *begin_non_empty,
                                   const char *end_non_empty,
                                   const char *omitted)
{
   // Note that the return value here is bson validity, not append status.
   if (bson_empty(bson)) {
      (void)mcommon_string_append(append, empty);
      return true;
   } else if (max_depth == 0) {
      (void)mcommon_string_append(append, omitted);
      return true;
   } else {
      (void)mcommon_string_append(append, begin_non_empty);
      bool result = mcommon_json_append_bson_values(append, bson, mode, has_keys, max_depth - 1u);
      (void)mcommon_string_append(append, end_non_empty);
      return result;
   }
}

bool
mcommon_json_append_bson_document(mcommon_string_append_t *append,
                                  const bson_t *bson,
                                  bson_json_mode_t mode,
                                  unsigned max_depth)
{
   return mcommon_json_append_bson_container(append, bson, mode, max_depth, true, "{ }", "{ ", " }", "{ ... }");
}

bool
mcommon_json_append_bson_array(mcommon_string_append_t *append,
                               const bson_t *bson,
                               bson_json_mode_t mode,
                               unsigned max_depth)
{
   return mcommon_json_append_bson_container(append, bson, mode, max_depth, false, "[ ]", "[ ", " ]", "[ ... ]");
}

/**
 * @brief Like mcommon_string_append_printf (append, "\\u%04x", c) but intended to be more optimizable.
 */
static BSON_INLINE bool
mcommon_json_append_hex_char(mcommon_string_append_t *append, uint16_t c)
{
   static const char digit_table[] = "0123456789abcdef";
   char hex_char[6];
   hex_char[0] = '\\';
   hex_char[1] = 'u';
   hex_char[2] = digit_table[0xf & (c >> 12)];
   hex_char[3] = digit_table[0xf & (c >> 8)];
   hex_char[4] = digit_table[0xf & (c >> 4)];
   hex_char[5] = digit_table[0xf & c];
   return mcommon_string_append_bytes(append, hex_char, 6);
}

/**
 * @brief Test whether a byte may require special processing in mcommon_json_append_escaped.
 * @returns true for bytes in the range 0x00 - 0x1F, '\\', '\"', and 0xC0.
 */
static BSON_INLINE bool
mcommon_json_append_escaped_considers_byte_as_special(uint8_t byte)
{
   static const uint64_t table[4] = {
      0x00000004ffffffffull, // 0x00-0x1F (control), 0x22 (")
      0x0000000010000000ull, // 0x5C (')
      0x0000000000000000ull, // none
      0x0000000000000001ull, // 0xC0 (Possible two-byte NUL)
   };
   return 0 != (table[byte >> 6] & (1ull << (byte & 0x3f)));
}

/**
 * @brief Measure the number of consecutive non-special bytes.
 */
static BSON_INLINE uint32_t
mcommon_json_append_escaped_count_non_special_bytes(const char *str, uint32_t len)
{
   uint32_t result = 0;
   // Good candidate for architecture-specific optimizations.
   // SSE4 strcspn is nearly what we want, but our table of special bytes would be too large (34 > 16)
   while (len) {
      if (mcommon_json_append_escaped_considers_byte_as_special((uint8_t)*str)) {
         break;
      }
      result++;
      str++;
      len--;
   }
   return result;
}

bool
mcommon_json_append_escaped(mcommon_string_append_t *append, const char *str, uint32_t len, bool allow_nul)
{
   BSON_ASSERT_PARAM(append);
   BSON_ASSERT_PARAM(str);

   // Repeatedly handle runs of zero or more non-special bytes punctuated by a potentially-special sequence.
   uint32_t non_special_len = mcommon_json_append_escaped_count_non_special_bytes(str, len);
   while (len) {
      if (!mcommon_string_append_bytes(append, str, non_special_len)) {
         return false;
      }
      str += non_special_len;
      len -= non_special_len;
      if (len) {
         char c = *str;
         switch (c) {
         case '"':
            if (!mcommon_string_append(append, "\\\"")) {
               return false;
            }
            break;
         case '\\':
            if (!mcommon_string_append(append, "\\\\")) {
               return false;
            }
            break;
         case '\b':
            if (!mcommon_string_append(append, "\\b")) {
               return false;
            }
            break;
         case '\f':
            if (!mcommon_string_append(append, "\\f")) {
               return false;
            }
            break;
         case '\n':
            if (!mcommon_string_append(append, "\\n")) {
               return false;
            }
            break;
         case '\r':
            if (!mcommon_string_append(append, "\\r")) {
               return false;
            }
            break;
         case '\t':
            if (!mcommon_string_append(append, "\\t")) {
               return false;
            }
            break;
         case '\0':
            if (!allow_nul || !mcommon_json_append_hex_char(append, 0)) {
               return false;
            }
            break;
         case '\xc0': // Could be a 2-byte NUL, or could begin another non-special run
            if (len >= 2 && str[1] == '\x80') {
               if (!allow_nul || !mcommon_json_append_hex_char(append, 0)) {
                  return false;
               }
               str++;
               len--;
            } else {
               // Wasn't "C0 80". Begin a non-special run with the "C0" byte, which is usually special.
               non_special_len = mcommon_json_append_escaped_count_non_special_bytes(str + 1, len - 1) + 1;
               continue;
            }
            break;
         default:
            BSON_ASSERT(c > 0x00 && c < 0x20);
            if (!mcommon_json_append_hex_char(append, c)) {
               return false;
            }
            break;
         }
         str++;
         len--;
         non_special_len = mcommon_json_append_escaped_count_non_special_bytes(str, len);
      }
   }
   return mcommon_string_status_from_append(append);
}

bool
mcommon_iso8601_string_append(mcommon_string_append_t *append, int64_t msec_since_epoch)
{
   time_t t;
   int64_t msec_part;
   char buf[64];

   msec_part = msec_since_epoch % 1000;
   t = (time_t)(msec_since_epoch / 1000);

#ifdef BSON_HAVE_GMTIME_R
   {
      struct tm posix_date;
      gmtime_r(&t, &posix_date);
      strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &posix_date);
   }
#elif defined(_MSC_VER)
   {
      /* Windows gmtime_s is thread-safe */
      struct tm time_buf;
      gmtime_s(&time_buf, &t);
      strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &time_buf);
   }
#else
   strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", gmtime(&t));
#endif

   if (msec_part) {
      return mcommon_string_append_printf(append, "%s.%03" PRId64 "Z", buf, msec_part);
   } else {
      return mcommon_string_append_printf(append, "%sZ", buf);
   }
}

bool
mcommon_json_append_value_double(mcommon_string_append_t *append, double value, bson_json_mode_t mode)
{
   /* Determine if legacy (i.e. unwrapped) output should be used. Relaxed mode
    * will use this for nan and inf values, which we check manually since old
    * platforms may not have isinf or isnan. */
   bool legacy =
      mode == BSON_JSON_MODE_LEGACY || (mode == BSON_JSON_MODE_RELAXED && !(value != value || value * 0 != 0));

   if (!legacy) {
      mcommon_string_append(append, "{ \"$numberDouble\" : \"");
   }

   if (!legacy && value != value) {
      mcommon_string_append(append, "NaN");
   } else if (!legacy && value * 0 != 0) {
      if (value > 0) {
         mcommon_string_append(append, "Infinity");
      } else {
         mcommon_string_append(append, "-Infinity");
      }
   } else {
      const mcommon_string_t *string = mcommon_string_from_append(append);
      uint32_t start_len = string->len;
      if (mcommon_string_append_printf(append, "%.20g", value)) {
         /* ensure trailing ".0" to distinguish "3" from "3.0" */
         if (strspn(&string->str[start_len], "0123456789-") == string->len - start_len) {
            mcommon_string_append(append, ".0");
         }
      }
   }

   if (!legacy) {
      mcommon_string_append(append, "\" }");
   }

   return mcommon_string_status_from_append(append);
}

bool
mcommon_json_append_value_decimal128(mcommon_string_append_t *append, const bson_decimal128_t *value)
{
   char decimal128_string[BSON_DECIMAL128_STRING];
   bson_decimal128_to_string(value, decimal128_string);

   return mcommon_string_append(append, "{ \"$numberDecimal\" : \"") &&
          mcommon_string_append(append, decimal128_string) && mcommon_string_append(append, "\" }");
}

bool
mcommon_json_append_value_oid(mcommon_string_append_t *append, const bson_oid_t *value)
{
   return mcommon_string_append(append, "{ \"$oid\" : \"") && mcommon_string_append_oid_as_hex(append, value) &&
          mcommon_string_append(append, "\" }");
}

bool
mcommon_json_append_value_binary(mcommon_string_append_t *append,
                                 bson_subtype_t subtype,
                                 const uint8_t *bytes,
                                 uint32_t byte_count,
                                 bson_json_mode_t mode)
{
   if (mode == BSON_JSON_MODE_CANONICAL || mode == BSON_JSON_MODE_RELAXED) {
      return mcommon_string_append(append, "{ \"$binary\" : { \"base64\" : \"") &&
             mcommon_string_append_base64_encode(append, bytes, byte_count) &&
             mcommon_string_append_printf(append, "\", \"subType\" : \"%02x\" } }", (unsigned int)subtype);
   } else {
      return mcommon_string_append(append, "{ \"$binary\" : \"") &&
             mcommon_string_append_base64_encode(append, bytes, byte_count) &&
             mcommon_string_append_printf(append, "\", \"$type\" : \"%02x\" }", (unsigned int)subtype);
   }
}

bool
mcommon_json_append_value_date_time(mcommon_string_append_t *append, int64_t msec_since_epoch, bson_json_mode_t mode)
{
   const int64_t y10k = 253402300800000; // 10000-01-01T00:00:00Z in milliseconds since the epoch.

   if (mode == BSON_JSON_MODE_CANONICAL ||
       (mode == BSON_JSON_MODE_RELAXED && (msec_since_epoch < 0 || msec_since_epoch >= y10k))) {
      return mcommon_string_append_printf(
         append, "{ \"$date\" : { \"$numberLong\" : \"%" PRId64 "\" } }", msec_since_epoch);
   } else if (mode == BSON_JSON_MODE_RELAXED) {
      return mcommon_string_append(append, "{ \"$date\" : \"") &&
             mcommon_iso8601_string_append(append, msec_since_epoch) && mcommon_string_append(append, "\" }");
   } else {
      return mcommon_string_append_printf(append, "{ \"$date\" : %" PRId64 " }", msec_since_epoch);
   }
}

bool
mcommon_json_append_value_timestamp(mcommon_string_append_t *append, uint32_t timestamp, uint32_t increment)
{
   BSON_ASSERT_PARAM(append);
   return mcommon_string_append_printf(append, "{ \"$timestamp\" : { \"t\" : %u, \"i\" : %u } }", timestamp, increment);
}

bool
mcommon_json_append_value_regex(mcommon_string_append_t *append,
                                const char *pattern,
                                uint32_t pattern_len,
                                const char *options,
                                size_t options_len,
                                bson_json_mode_t mode)
{
   if (mode == BSON_JSON_MODE_CANONICAL || mode == BSON_JSON_MODE_RELAXED) {
      return mcommon_string_append(append, "{ \"$regularExpression\" : { \"pattern\" : \"") &&
             mcommon_json_append_escaped(append, pattern, pattern_len, false) &&
             mcommon_string_append(append, "\", \"options\" : \"") &&
             mcommon_string_append_selected_chars(append, BSON_REGEX_OPTIONS_SORTED, options, options_len) &&
             mcommon_string_append(append, "\" } }");
   } else {
      return mcommon_string_append(append, "{ \"$regex\" : \"") &&
             mcommon_json_append_escaped(append, pattern, pattern_len, false) &&
             mcommon_string_append(append, "\", \"$options\" : \"") &&
             mcommon_string_append_selected_chars(append, BSON_REGEX_OPTIONS_SORTED, options, options_len) &&
             mcommon_string_append(append, "\" }");
   }
}

bool
mcommon_json_append_value_dbpointer(mcommon_string_append_t *append,
                                    const char *collection,
                                    uint32_t collection_len,
                                    const bson_oid_t *oid,
                                    bson_json_mode_t mode)
{
   if (mode == BSON_JSON_MODE_CANONICAL || mode == BSON_JSON_MODE_RELAXED) {
      return mcommon_string_append(append, "{ \"$dbPointer\" : { \"$ref\" : \"") &&
             mcommon_json_append_escaped(append, collection, collection_len, false) &&
             mcommon_string_append(append, "\"") &&
             (!oid || (mcommon_string_append(append, ", \"$id\" : ") && mcommon_json_append_value_oid(append, oid))) &&
             mcommon_string_append(append, " } }");
   } else {
      return mcommon_string_append(append, "{ \"$ref\" : \"") &&
             mcommon_json_append_escaped(append, collection, collection_len, false) &&
             mcommon_string_append(append, "\"") &&
             (!oid ||
              (mcommon_string_append(append, ", \"$id\" : \"") && mcommon_string_append_oid_as_hex(append, oid))) &&
             mcommon_string_append(append, "\" }");
   }
}

bool
mcommon_json_append_value_code(mcommon_string_append_t *append, const char *code, uint32_t code_len)
{
   return mcommon_string_append(append, "{ \"$code\" : \"") &&
          mcommon_json_append_escaped(append, code, code_len, true) && mcommon_string_append(append, "\" }");
}

bool
mcommon_json_append_value_codewscope(mcommon_string_append_t *append,
                                     const char *code,
                                     uint32_t code_len,
                                     const bson_t *scope,
                                     bson_json_mode_t mode,
                                     unsigned max_depth)
{
   // Note that the return value here is bson validity, not append status.
   (void)mcommon_string_append(append, "{ \"$code\" : \"");
   (void)mcommon_json_append_escaped(append, code, code_len, true);
   (void)mcommon_string_append(append, "\", \"$scope\" : ");
   bool result = mcommon_json_append_bson_document(append, scope, mode, max_depth);
   (void)mcommon_string_append(append, " }");
   return result;
}

bool
mcommon_json_append_value_symbol(mcommon_string_append_t *append,
                                 const char *symbol,
                                 uint32_t symbol_len,
                                 bson_json_mode_t mode)
{
   if (mode == BSON_JSON_MODE_CANONICAL || mode == BSON_JSON_MODE_RELAXED) {
      return mcommon_string_append(append, "{ \"$symbol\" : \"") &&
             mcommon_json_append_escaped(append, symbol, symbol_len, true) && mcommon_string_append(append, "\" }");
   } else {
      return mcommon_json_append_value_utf8(append, symbol, symbol_len, true);
   }
}
