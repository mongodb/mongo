/*
 * Copyright 2018-present MongoDB, Inc.
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

#ifndef MONGOCRYPT_BUFFER_H
#define MONGOCRYPT_BUFFER_H

#include "mongocrypt-binary-private.h"
#include "mongocrypt-compat.h"
#include <bson/bson.h>

#define UUID_LEN 16
#define PRF_LEN 32

struct _mongocrypt_binary_t;

/* An internal struct to make working with binary values more convenient.
 * - a non-owning buffer can be constructed from a bson_iter_t.
 * - a non-owning buffer can become an owned buffer by copying.
 * - a buffer can be appended as a BSON binary in a bson_t.
 */
typedef struct __mongocrypt_buffer_t {
    uint8_t *data;
    uint32_t len;
    bool owned;
    bson_subtype_t subtype;
    mongocrypt_binary_t bin;
} _mongocrypt_buffer_t;

void _mongocrypt_buffer_init(_mongocrypt_buffer_t *buf);

void _mongocrypt_buffer_resize(_mongocrypt_buffer_t *buf, uint32_t len);

void _mongocrypt_buffer_init_size(_mongocrypt_buffer_t *buf, uint32_t len);

void _mongocrypt_buffer_steal(_mongocrypt_buffer_t *buf, _mongocrypt_buffer_t *src);

/* @iter is iterated to a BSON binary value. */
bool _mongocrypt_buffer_copy_from_binary_iter(_mongocrypt_buffer_t *buf,
                                              bson_iter_t *iter) MONGOCRYPT_WARN_UNUSED_RESULT;

/* @iter is iterated to a BSON binary value. */
bool _mongocrypt_buffer_from_binary_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) MONGOCRYPT_WARN_UNUSED_RESULT;

/* @iter is iterated to a BSON document value. */
bool _mongocrypt_buffer_from_document_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) MONGOCRYPT_WARN_UNUSED_RESULT;

/* @iter is iterated to a BSON document value. */
bool _mongocrypt_buffer_copy_from_document_iter(_mongocrypt_buffer_t *buf,
                                                bson_iter_t *iter) MONGOCRYPT_WARN_UNUSED_RESULT;

void _mongocrypt_buffer_steal_from_bson(_mongocrypt_buffer_t *buf, bson_t *bson);

void _mongocrypt_buffer_from_bson(_mongocrypt_buffer_t *buf, const bson_t *bson);

bool _mongocrypt_buffer_to_bson(const _mongocrypt_buffer_t *buf, bson_t *bson) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_buffer_append(const _mongocrypt_buffer_t *buf, bson_t *bson, const char *key, int key_len)
    MONGOCRYPT_WARN_UNUSED_RESULT;

void _mongocrypt_buffer_from_binary(_mongocrypt_buffer_t *buf, const struct _mongocrypt_binary_t *binary);

void _mongocrypt_buffer_copy_from_binary(_mongocrypt_buffer_t *buf, const struct _mongocrypt_binary_t *binary);

void _mongocrypt_buffer_to_binary(const _mongocrypt_buffer_t *buf, struct _mongocrypt_binary_t *binary);

void _mongocrypt_buffer_copy_to(const _mongocrypt_buffer_t *src, _mongocrypt_buffer_t *dst);

void _mongocrypt_buffer_set_to(const _mongocrypt_buffer_t *src, _mongocrypt_buffer_t *dst);

int _mongocrypt_buffer_cmp(const _mongocrypt_buffer_t *a, const _mongocrypt_buffer_t *b);

void _mongocrypt_buffer_cleanup(_mongocrypt_buffer_t *buf);

bool _mongocrypt_buffer_empty(const _mongocrypt_buffer_t *buf);

bool _mongocrypt_buffer_to_bson_value(_mongocrypt_buffer_t *plaintext,
                                      uint8_t type,
                                      bson_value_t *out) MONGOCRYPT_WARN_UNUSED_RESULT;

void _mongocrypt_buffer_from_iter(_mongocrypt_buffer_t *plaintext, bson_iter_t *iter);

bool _mongocrypt_buffer_from_uuid_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_buffer_copy_from_uuid_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_buffer_is_uuid(_mongocrypt_buffer_t *buf) MONGOCRYPT_WARN_UNUSED_RESULT;

void _mongocrypt_buffer_copy_from_hex(_mongocrypt_buffer_t *buf, const char *hex);

int _mongocrypt_buffer_cmp_hex(_mongocrypt_buffer_t *buf, const char *hex);

char *_mongocrypt_buffer_to_hex(_mongocrypt_buffer_t *buf) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_buffer_concat(_mongocrypt_buffer_t *dst, const _mongocrypt_buffer_t *srcs, uint32_t num_srcs);

struct _mongocrypt_binary_t *_mongocrypt_buffer_as_binary(_mongocrypt_buffer_t *buf);

/* _mongocrypt_buffer_copy_from_data_and_size initializes @buf and copies @len
 * bytes from @data.
 * - Returns false on error.
 * - Caller must call _mongocrypt_buffer_cleanup. */
bool _mongocrypt_buffer_copy_from_data_and_size(_mongocrypt_buffer_t *buf,
                                                const uint8_t *data,
                                                size_t len) MONGOCRYPT_WARN_UNUSED_RESULT;

/* _mongocrypt_buffer_steal_from_data_and_size initializes @buf from @data and
 * @len and takes ownership of @data.
 * - Returns false on error.
 * - @buf does not take ownership of @str on error.
 * - Caller must call _mongocrypt_buffer_cleanup. */
bool _mongocrypt_buffer_steal_from_data_and_size(_mongocrypt_buffer_t *buf,
                                                 uint8_t *data,
                                                 size_t len) MONGOCRYPT_WARN_UNUSED_RESULT;

/* _mongocrypt_buffer_steal_from_string initializes @buf from @str and takes
 * ownership of @str.
 * @buf retains a pointer to @str.
 * @str must be NULL terminated.
 * - Returns false on error.
 * - @buf does not take ownership of @str on error.
 * - Caller must call _mongocrypt_buffer_cleanup. */
bool _mongocrypt_buffer_steal_from_string(_mongocrypt_buffer_t *buf, char *str) MONGOCRYPT_WARN_UNUSED_RESULT;

/* _mongocrypt_buffer_from_string initializes @buf from @str.
 * @buf retains a pointer to @str.
 * @str must outlive @buf.
 * @str must be NULL terminated.
 * - Returns false on error.
 * - Caller must call _mongocrypt_buffer_cleanup. */
bool _mongocrypt_buffer_from_string(_mongocrypt_buffer_t *buf, const char *str) MONGOCRYPT_WARN_UNUSED_RESULT;

/* _mongocrypt_buffer_copy_from_uint64_le initializes @buf from the
 * little-endian byte representation of @value. Caller must call
 * _mongocrypt_buffer_cleanup.
 * @value is expected to be in machine's native endianness.
 */
void _mongocrypt_buffer_copy_from_uint64_le(_mongocrypt_buffer_t *buf, uint64_t value);

/* _mongocrypt_buffer_from_subrange initializes @out as a non-owning buffer to a
 * range of data from @in specified by @offset and @len. Returns false on error.
 */
bool _mongocrypt_buffer_from_subrange(_mongocrypt_buffer_t *out,
                                      const _mongocrypt_buffer_t *in,
                                      uint32_t offset,
                                      uint32_t len) MONGOCRYPT_WARN_UNUSED_RESULT;

#endif /* MONGOCRYPT_BUFFER_H */
