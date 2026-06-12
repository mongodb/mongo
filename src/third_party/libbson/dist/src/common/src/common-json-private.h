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

#ifndef MONGO_C_DRIVER_COMMON_JSON_PRIVATE_H
#define MONGO_C_DRIVER_COMMON_JSON_PRIVATE_H

#include <common-string-private.h>

#define mcommon_iso8601_string_append COMMON_NAME(iso8601_string_append)
#define mcommon_json_append_escaped COMMON_NAME(json_append_escaped)
#define mcommon_json_append_value_double COMMON_NAME(json_append_value_double)
#define mcommon_json_append_value_decimal128 COMMON_NAME(json_append_value_decimal128)
#define mcommon_json_append_value_oid COMMON_NAME(json_append_value_oid)
#define mcommon_json_append_value_binary COMMON_NAME(json_append_value_binary)
#define mcommon_json_append_value_date_time COMMON_NAME(json_append_value_date_time)
#define mcommon_json_append_value_timestamp COMMON_NAME(json_append_value_timestamp)
#define mcommon_json_append_value_regex COMMON_NAME(json_append_value_regex)
#define mcommon_json_append_value_dbpointer COMMON_NAME(json_append_value_dbpointer)
#define mcommon_json_append_value_code COMMON_NAME(json_append_value_code)
#define mcommon_json_append_value_codewscope COMMON_NAME(json_append_value_codewscope)
#define mcommon_json_append_value_symbol COMMON_NAME(json_append_value_symbol)
#define mcommon_json_append_bson_values COMMON_NAME(json_append_bson_values)
#define mcommon_json_append_bson_document COMMON_NAME(json_append_bson_document)
#define mcommon_json_append_bson_array COMMON_NAME(json_append_bson_array)

// Needed by libbson and common-json
#ifndef BSON_MAX_RECURSION
#define BSON_MAX_RECURSION 200
#endif

// Needed by libbson and common-json
#define BSON_REGEX_OPTIONS_SORTED "ilmsux"

/**
 * @brief Append an ISO 8601 formatted date, given 64-bit milliseconds since the epoch
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param msec_since_epoch Milliseconds since Jan 1 1970 UTC
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_iso8601_string_append(mcommon_string_append_t *append, int64_t msec_since_epoch);

/**
 * @brief Append a UTF-8 string with all special characters escaped
 *
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param str UTF-8 string to escape and append
 * @param len Length of 'str' in bytes
 * @param allow_nul true if internal "00" bytes or "C0 80" sequences should be encoded as "\u0000", false to treat
 * them as invalid data
 * @returns true on success, false if this 'append' has exceeded its max length or if we encountered invalid UTF-8 or
 * disallowed NUL bytes in 'str'
 *
 * The string may include internal NUL characters. It does not need to be NUL terminated.
 * The two-byte sequence "C0 80" is also interpreted as an internal NUL, for historical reasons. This sequence is
 * considered invalid according to RFC3629.
 */
bool
mcommon_json_append_escaped(mcommon_string_append_t *append, const char *str, uint32_t len, bool allow_nul);

/**
 * @brief Append a comma separator string to appear between values
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @returns true on success, false if this 'append' has exceeded its max length
 */
static BSON_INLINE bool
mcommon_json_append_separator(mcommon_string_append_t *append)
{
   return mcommon_string_append(append, ", ");
}

/**
 * @brief Append a quoted and escaped key and key-value separator
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param str UTF-8 string to escape and append
 * @param len Length of 'str' in bytes
 * @returns true on success, false if this 'append' has exceeded its max length or if we encountered invalid UTF-8 or
 * disallowed NUL bytes in 'str'
 *
 * See mcommon_json_append_escaped. NUL values in keys are never allowed.
 */
static BSON_INLINE bool
mcommon_json_append_key(mcommon_string_append_t *append, const char *str, uint32_t len)
{
   return mcommon_string_append(append, "\"") && mcommon_json_append_escaped(append, str, len, false) &&
          mcommon_string_append(append, "\" : ");
}

/**
 * @brief Append a quoted and escaped string
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param str UTF-8 string to escape and append
 * @param len Length of 'str' in bytes
 * @param allow_nul true if internal "00" bytes or "C0 80" sequences should be encoded as "\u0000", false to treat them
 * as invalid data
 * @returns true on success, false if this 'append' has exceeded its max length or if we encountered invalid UTF-8 or
 * disallowed NUL bytes in 'str'
 *
 * See mcommon_json_append_escaped.
 */
static BSON_INLINE bool
mcommon_json_append_value_utf8(mcommon_string_append_t *append, const char *str, uint32_t len, bool allow_nul)
{
   return mcommon_string_append(append, "\"") && mcommon_json_append_escaped(append, str, len, allow_nul) &&
          mcommon_string_append(append, "\"");
}

/**
 * @brief Append an int32_t value, serialized according to a bson_json_mode_t
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param value Integer value
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t.
 * @returns true on success, false if this 'append' has exceeded its max length
 */
static BSON_INLINE bool
mcommon_json_append_value_int32(mcommon_string_append_t *append, int32_t value, bson_json_mode_t mode)
{
   return mode == BSON_JSON_MODE_CANONICAL
             ? mcommon_string_append_printf(append, "{ \"$numberInt\" : \"%" PRId32 "\" }", value)
             : mcommon_string_append_printf(append, "%" PRId32, value);
}

/**
 * @brief Append an int64_t value, serialized according to a bson_json_mode_t
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param value Integer value
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t.
 * @returns true on success, false if this 'append' has exceeded its max length
 */
static BSON_INLINE bool
mcommon_json_append_value_int64(mcommon_string_append_t *append, int64_t value, bson_json_mode_t mode)
{
   return mode == BSON_JSON_MODE_CANONICAL
             ? mcommon_string_append_printf(append, "{ \"$numberLong\" : \"%" PRId64 "\" }", value)
             : mcommon_string_append_printf(append, "%" PRId64, value);
}

/**
 * @brief Append a JSON compatible bool value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param bool Boolean value
 * @returns true on success, false if this 'append' has exceeded its max length
 */
static BSON_INLINE bool
mcommon_json_append_value_bool(mcommon_string_append_t *append, bool value)
{
   return mcommon_string_append(append, value ? "true" : "false");
}

/**
 * @brief Append an $undefined value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @returns true on success, false if this 'append' has exceeded its max length
 */
static BSON_INLINE bool
mcommon_json_append_value_undefined(mcommon_string_append_t *append)
{
   return mcommon_string_append(append, "{ \"$undefined\" : true }");
}

/**
 * @brief Append a null value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @returns true on success, false if this 'append' has exceeded its max length
 */
static BSON_INLINE bool
mcommon_json_append_value_null(mcommon_string_append_t *append)
{
   return mcommon_string_append(append, "null");
}

/**
 * @brief Append a $minKey value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @returns true on success, false if this 'append' has exceeded its max length
 */
static BSON_INLINE bool
mcommon_json_append_value_minkey(mcommon_string_append_t *append)
{
   return mcommon_string_append(append, "{ \"$minKey\" : 1 }");
}

/**
 * @brief Append a $maxKey value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @returns true on success, false if this 'append' has exceeded its max length
 */
static BSON_INLINE bool
mcommon_json_append_value_maxkey(mcommon_string_append_t *append)
{
   return mcommon_string_append(append, "{ \"$maxKey\" : 1 }");
}

/**
 * @brief Append a double-precision floating point value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param value Double-precision floating point value
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t.
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_double(mcommon_string_append_t *append, double value, bson_json_mode_t mode);

/**
 * @brief Append a decimal128 value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param value decimal128 value to copy
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_decimal128(mcommon_string_append_t *append, const bson_decimal128_t *value);

/**
 * @brief Append the $oid JSON serialization of an ObjectId value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param value bson_oid_t value to copy
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_oid(mcommon_string_append_t *append, const bson_oid_t *value);

/**
 * @brief Append the JSON serialization of a BSON binary value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param subtype Subtype code, identifying the format within the base64-encoded binary block
 * @param bytes Bytes to be base64 encoded
 * @param byte_count Number of bytes
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_binary(mcommon_string_append_t *append,
                                 bson_subtype_t subtype,
                                 const uint8_t *bytes,
                                 uint32_t byte_count,
                                 bson_json_mode_t mode);

/**
 * @brief Append the JSON serialization of a BSON date and time
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param msec_since_epoch Milliseconds since Jan 1 1970
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_date_time(mcommon_string_append_t *append, int64_t msec_since_epoch, bson_json_mode_t mode);

/**
 * @brief Append the JSON serialization of a BSON timestamp value
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param timestamp 32-bit timestamp value
 * @param increment 32-bit increment value
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_timestamp(mcommon_string_append_t *append, uint32_t timestamp, uint32_t increment);

/**
 * @brief Append the JSON serialization of a BSON regular expression
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param pattern Regular expression pattern, as a UTF-8 string
 * @param pattern_len Length of pattern string, in bytes
 * @param options Regular expression options, as a UTF-8 string
 * @param options_len Length of the options string, in bytes
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_regex(mcommon_string_append_t *append,
                                const char *pattern,
                                uint32_t pattern_len,
                                const char *options,
                                size_t options_len,
                                bson_json_mode_t mode);

/**
 * @brief Append the JSON serialization of a BSON legacy DBPointer
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param collection Collection name, as a UTF-8 string
 * @param collection_len Length of collection name string, in bytes
 * @param oid Optional ObjectId reference, or NULL
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_dbpointer(mcommon_string_append_t *append,
                                    const char *collection,
                                    uint32_t collection_len,
                                    const bson_oid_t *oid,
                                    bson_json_mode_t mode);

/**
 * @brief Append the JSON serialization of a BSON legacy code object
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param code Code string, in UTF-8
 * @param code_len Length of code string, in bytes
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_code(mcommon_string_append_t *append, const char *code, uint32_t code_len);

/**
 * @brief Append the JSON serialization of a BSON legacy code-with-scope object
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param code Code string, in UTF-8
 * @param code_len Length of code string, in bytes
 * @param scope Scope as a bson_t document
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t
 * @param max_depth Maximum allowed number of document/array nesting levels below this one
 * @returns true if the input bson was valid, even if we reached max length. false on invalid BSON.
 */
bool
mcommon_json_append_value_codewscope(mcommon_string_append_t *append,
                                     const char *code,
                                     uint32_t code_len,
                                     const bson_t *scope,
                                     bson_json_mode_t mode,
                                     unsigned max_depth);

/**
 * @brief Append the JSON serialization of a BSON legacy symbol object
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param symbol Symbol string, in UTF-8
 * @param symbol_len Length of symbol string, in bytes
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t.
 * @returns true on success, false if this 'append' has exceeded its max length
 */
bool
mcommon_json_append_value_symbol(mcommon_string_append_t *append,
                                 const char *symbol,
                                 uint32_t symbol_len,
                                 bson_json_mode_t mode);

/**
 * @brief Append all JSON-serialized values from a bson_t
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param bson bson_t document or array
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t
 * @param has_keys true if this is a document, false if this is an array
 * @param max_depth Maximum allowed number of document/array nesting levels below this one
 * @returns true if the input bson was valid, even if we reached max length. false on invalid BSON.
 *
 * This generates keys, values, and separators but does not enclose the result in {} or [].
 * Note that the return value reflects the status of BSON decoding, not string appending.
 * The append status can be read using mcommon_string_status_from_append() if needed.
 * If encoding was stopped early due to the max depth limit or max length, invalid input may go unnoticed.
 */
bool
mcommon_json_append_bson_values(
   mcommon_string_append_t *append, const bson_t *bson, bson_json_mode_t mode, bool has_keys, unsigned max_depth);

/**
 * @brief Append a BSON document serialized as a JSON document
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param bson bson_t document
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t
 * @param max_depth Maximum allowed number of document/array nesting levels *including* this one. If zero, appends "{
 * ... }".
 * @returns true if the input bson was valid, even if we reached max length. false on invalid BSON.
 */
bool
mcommon_json_append_bson_document(mcommon_string_append_t *append,
                                  const bson_t *bson,
                                  bson_json_mode_t mode,
                                  unsigned max_depth);

/**
 * @brief Append a BSON document serialized as a JSON array
 * @param append A bounded string append, initialized with mcommon_string_set_append()
 * @param bson bson_t to interpret as an array
 * @param mode One of the JSON serialization modes, as a bson_json_mode_t
 * @param max_depth Maximum allowed number of document/array nesting levels *including* this one. If zero, appends "[
 * ... ]".
 * @returns true if the input bson was valid, even if we reached max length. false on invalid BSON.
 */
bool
mcommon_json_append_bson_array(mcommon_string_append_t *append,
                               const bson_t *bson,
                               bson_json_mode_t mode,
                               unsigned max_depth);

#endif /* MONGO_C_DRIVER_COMMON_JSON_PRIVATE_H */
