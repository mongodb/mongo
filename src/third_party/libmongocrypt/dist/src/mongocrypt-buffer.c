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

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-endian-private.h"
#include "mongocrypt-util-private.h"
#include <bson/bson.h>

#define INT32_LEN 4
#define TYPE_LEN 1
#define NULL_BYTE_LEN 1
#define NULL_BYTE_VAL 0x00

/* if a buffer is not owned, copy the data and make it owned. */
static void _make_owned(_mongocrypt_buffer_t *buf) {
    uint8_t *tmp;

    BSON_ASSERT_PARAM(buf);
    if (buf->owned) {
        return;
    }
    tmp = buf->data;
    if (buf->len > 0) {
        buf->data = bson_malloc(buf->len);
        BSON_ASSERT(buf->data);
        memcpy(buf->data, tmp, buf->len);
    } else {
        buf->data = NULL;
    }

    buf->owned = true;
}

/* TODO CDRIVER-2990 have buffer operations require initialized buffer to
 * prevent leaky code. */
void _mongocrypt_buffer_init(_mongocrypt_buffer_t *buf) {
    BSON_ASSERT_PARAM(buf);

    memset(buf, 0, sizeof(*buf));
}

void _mongocrypt_buffer_resize(_mongocrypt_buffer_t *buf, uint32_t len) {
    BSON_ASSERT_PARAM(buf);

    /* Currently this just wipes whatever was in data before,
       but a fancier implementation could copy over up to 'len'
       bytes from the old buffer to the new one. */
    if (buf->owned) {
        buf->data = bson_realloc(buf->data, len);
        buf->len = len;
        return;
    }

    buf->data = bson_malloc(len);
    BSON_ASSERT(buf->data);

    buf->len = len;
    buf->owned = true;
}

void _mongocrypt_buffer_init_size(_mongocrypt_buffer_t *buf, uint32_t len) {
    BSON_ASSERT_PARAM(buf);

    _mongocrypt_buffer_init(buf);
    _mongocrypt_buffer_resize(buf, len);
}

void _mongocrypt_buffer_steal(_mongocrypt_buffer_t *buf, _mongocrypt_buffer_t *src) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(src);

    if (!src->owned) {
        _mongocrypt_buffer_copy_to(src, buf);
        _mongocrypt_buffer_init(src);
        return;
    }

    buf->data = src->data;
    buf->len = src->len;
    buf->owned = true;
    _mongocrypt_buffer_init(src);
}

bool _mongocrypt_buffer_from_binary_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(iter);

    if (!BSON_ITER_HOLDS_BINARY(iter)) {
        return false;
    }
    _mongocrypt_buffer_init(buf);
    bson_iter_binary(iter, &buf->subtype, &buf->len, (const uint8_t **)&buf->data);
    buf->owned = false;
    return true;
}

bool _mongocrypt_buffer_copy_from_binary_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(iter);

    if (!_mongocrypt_buffer_from_binary_iter(buf, iter)) {
        return false;
    }

    _make_owned(buf);
    return true;
}

bool _mongocrypt_buffer_from_document_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(iter);

    if (!BSON_ITER_HOLDS_DOCUMENT(iter)) {
        return false;
    }
    _mongocrypt_buffer_init(buf);
    bson_iter_document(iter, &buf->len, (const uint8_t **)&buf->data);
    buf->owned = false;
    return true;
}

bool _mongocrypt_buffer_copy_from_document_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(iter);

    if (!_mongocrypt_buffer_from_document_iter(buf, iter)) {
        return false;
    }
    _make_owned(buf);
    return true;
}

void _mongocrypt_buffer_steal_from_bson(_mongocrypt_buffer_t *buf, bson_t *bson) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(bson);

    _mongocrypt_buffer_init(buf);
    buf->data = bson_destroy_with_steal(bson, true, &buf->len);
    buf->owned = true;
}

void _mongocrypt_buffer_from_bson(_mongocrypt_buffer_t *buf, const bson_t *bson) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(bson);

    _mongocrypt_buffer_init(buf);
    buf->data = (uint8_t *)bson_get_data(bson);
    buf->len = bson->len;
    buf->owned = false;
}

bool _mongocrypt_buffer_to_bson(const _mongocrypt_buffer_t *buf, bson_t *bson) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(bson);

    return bson_init_static(bson, buf->data, buf->len);
}

bool _mongocrypt_buffer_append(const _mongocrypt_buffer_t *buf, bson_t *bson, const char *key, int key_len) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(key);

    return bson_append_binary(bson, key, key_len, buf->subtype, buf->data, buf->len);
}

void _mongocrypt_buffer_from_binary(_mongocrypt_buffer_t *buf, const mongocrypt_binary_t *binary) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(binary);

    _mongocrypt_buffer_init(buf);
    buf->data = binary->data;
    buf->len = binary->len;
    buf->owned = false;
}

void _mongocrypt_buffer_copy_from_binary(_mongocrypt_buffer_t *buf, const struct _mongocrypt_binary_t *binary) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(binary);

    _mongocrypt_buffer_from_binary(buf, binary);
    _make_owned(buf);
}

void _mongocrypt_buffer_to_binary(const _mongocrypt_buffer_t *buf, mongocrypt_binary_t *binary) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(binary);

    binary->data = buf->data;
    binary->len = buf->len;
}

void _mongocrypt_buffer_copy_to(const _mongocrypt_buffer_t *src, _mongocrypt_buffer_t *dst) {
    if (src == dst) {
        return;
    }

    BSON_ASSERT_PARAM(src);
    BSON_ASSERT_PARAM(dst);

    _mongocrypt_buffer_cleanup(dst);
    if (src->len == 0) {
        return;
    }

    dst->data = bson_malloc((size_t)src->len);
    BSON_ASSERT(dst->data);

    memcpy(dst->data, src->data, src->len);
    dst->len = src->len;
    dst->subtype = src->subtype;
    dst->owned = true;
}

void _mongocrypt_buffer_set_to(const _mongocrypt_buffer_t *src, _mongocrypt_buffer_t *dst) {
    if (src == dst) {
        return;
    }

    BSON_ASSERT_PARAM(src);
    BSON_ASSERT_PARAM(dst);

    dst->data = src->data;
    dst->len = src->len;
    dst->subtype = src->subtype;
    dst->owned = false;
}

int _mongocrypt_buffer_cmp(const _mongocrypt_buffer_t *a, const _mongocrypt_buffer_t *b) {
    BSON_ASSERT_PARAM(a);
    BSON_ASSERT_PARAM(b);

    if (a->len != b->len) {
        return a->len > b->len ? 1 : -1;
    }
    if (0 == a->len) {
        return 0;
    }
    return memcmp(a->data, b->data, a->len);
}

void _mongocrypt_buffer_cleanup(_mongocrypt_buffer_t *buf) {
    if (buf && buf->owned) {
        bson_free(buf->data);
    }
}

bool _mongocrypt_buffer_empty(const _mongocrypt_buffer_t *buf) {
    BSON_ASSERT_PARAM(buf);

    return buf->data == NULL;
}

bool _mongocrypt_buffer_to_bson_value(_mongocrypt_buffer_t *plaintext, uint8_t type, bson_value_t *out) {
    bool ret = false;
    bson_iter_t iter;
    bson_t wrapper;
    uint32_t data_len;
    uint32_t le_data_len;
    uint8_t *data;
    uint8_t data_prefix;

    BSON_ASSERT_PARAM(plaintext);
    BSON_ASSERT_PARAM(out);

    data_prefix = INT32_LEN      /* adds document size */
                + TYPE_LEN       /* element type */
                + NULL_BYTE_LEN; /* and doc's null byte terminator */

    BSON_ASSERT(plaintext->len <= UINT32_MAX - data_prefix - NULL_BYTE_LEN);

    data_len = (plaintext->len + data_prefix + NULL_BYTE_LEN);
    le_data_len = BSON_UINT32_TO_LE(data_len);

    data = bson_malloc0(data_len);
    BSON_ASSERT(data);

    memcpy(data + data_prefix, plaintext->data, plaintext->len);
    memcpy(data, &le_data_len, INT32_LEN);
    memcpy(data + INT32_LEN, &type, TYPE_LEN);
    data[data_len - 1] = NULL_BYTE_VAL;

    if (!bson_init_static(&wrapper, data, data_len)) {
        goto fail;
    }

    if (!bson_validate(&wrapper, BSON_VALIDATE_NONE, NULL)) {
        goto fail;
    }

    if (!bson_iter_init_find(&iter, &wrapper, "")) {
        goto fail;
    }
    bson_value_copy(bson_iter_value(&iter), out);

    /* Due to an open libbson bug (CDRIVER-3340), give an empty
     * binary payload a real address. TODO: remove this after
     * CDRIVER-3340 is fixed. */
    if (out->value_type == BSON_TYPE_BINARY && 0 == out->value.v_binary.data_len) {
        out->value.v_binary.data = bson_malloc(1); /* Freed in bson_value_destroy */
    }

    ret = true;
fail:
    bson_free(data);
    return ret;
}

void _mongocrypt_buffer_from_iter(_mongocrypt_buffer_t *plaintext, bson_iter_t *iter) {
    bson_t wrapper = BSON_INITIALIZER;
    int32_t offset = INT32_LEN      /* skips document size */
                   + TYPE_LEN       /* element type */
                   + NULL_BYTE_LEN; /* and the key's null byte terminator */

    uint8_t *wrapper_data;

    BSON_ASSERT_PARAM(plaintext);
    BSON_ASSERT_PARAM(iter);

    /* It is not straightforward to transform a bson_value_t to a string of
     * bytes. As a workaround, we wrap the value in a bson document with an empty
     * key, then use the raw buffer from inside the new bson_t, skipping the
     * length and type header information and the key name. */
    bson_append_iter(&wrapper, "", 0, iter);
    wrapper_data = ((uint8_t *)bson_get_data(&wrapper));
    BSON_ASSERT(wrapper.len >= (uint32_t)offset + NULL_BYTE_LEN);
    plaintext->len = wrapper.len - (uint32_t)offset - NULL_BYTE_LEN; /* the final null byte */
    plaintext->data = bson_malloc(plaintext->len);
    BSON_ASSERT(plaintext->data);

    plaintext->owned = true;
    memcpy(plaintext->data, wrapper_data + offset, plaintext->len);

    bson_destroy(&wrapper);
}

bool _mongocrypt_buffer_from_uuid_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) {
    const uint8_t *data;
    bson_subtype_t subtype;
    uint32_t len;

    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(iter);

    if (!BSON_ITER_HOLDS_BINARY(iter)) {
        return false;
    }
    bson_iter_binary(iter, &subtype, &len, &data);
    if (subtype != BSON_SUBTYPE_UUID) {
        return false;
    }
    if (len != UUID_LEN) {
        return false;
    }
    _mongocrypt_buffer_init(buf);
    buf->data = (uint8_t *)data;
    buf->len = len;
    buf->subtype = subtype;
    buf->owned = false;
    return true;
}

bool _mongocrypt_buffer_copy_from_uuid_iter(_mongocrypt_buffer_t *buf, bson_iter_t *iter) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(iter);

    if (!_mongocrypt_buffer_from_uuid_iter(buf, iter)) {
        return false;
    }
    _make_owned(buf);
    return true;
}

bool _mongocrypt_buffer_is_uuid(_mongocrypt_buffer_t *buf) {
    BSON_ASSERT_PARAM(buf);

    return buf->len == UUID_LEN && buf->subtype == BSON_SUBTYPE_UUID;
}

void _mongocrypt_buffer_copy_from_hex(_mongocrypt_buffer_t *buf, const char *hex) {
    uint32_t i;
    size_t hex_len;

    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(hex);

    hex_len = strlen(hex);
    if (hex_len == 0) {
        _mongocrypt_buffer_init(buf);
        return;
    }

    BSON_ASSERT(hex_len / 2u <= UINT32_MAX);
    buf->len = (uint32_t)(hex_len / 2u);
    buf->data = bson_malloc(buf->len);
    BSON_ASSERT(buf->data);

    buf->owned = true;
    for (i = 0; i < buf->len; i++) {
        uint32_t tmp;
        BSON_ASSERT(i <= UINT32_MAX / 2);
        BSON_ASSERT(sscanf(hex + (2 * i), "%02x", &tmp));
        *(buf->data + i) = (uint8_t)tmp;
    }
}

int _mongocrypt_buffer_cmp_hex(_mongocrypt_buffer_t *buf, const char *hex) {
    _mongocrypt_buffer_t tmp;
    int res;

    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(hex);

    _mongocrypt_buffer_copy_from_hex(&tmp, hex);
    res = _mongocrypt_buffer_cmp(buf, &tmp);
    _mongocrypt_buffer_cleanup(&tmp);
    return res;
}

char *_mongocrypt_buffer_to_hex(_mongocrypt_buffer_t *buf) {
    BSON_ASSERT_PARAM(buf);
    /* since buf->len is a uint32_t, even doubling it won't bring it anywhere
     * near to SIZE_MAX */

    char *hex = bson_malloc0(buf->len * 2 + 1);
    BSON_ASSERT(hex);

    char *out = hex;

    for (uint32_t i = 0; i < buf->len; i++, out += 2) {
        sprintf(out, "%02X", buf->data[i]);
    }
    return hex;
}

bool _mongocrypt_buffer_concat(_mongocrypt_buffer_t *dst, const _mongocrypt_buffer_t *srcs, uint32_t num_srcs) {
    uint32_t total = 0;
    uint32_t offset;
    uint32_t i;

    BSON_ASSERT_PARAM(dst);
    BSON_ASSERT_PARAM(srcs);

    for (i = 0; i < num_srcs; i++) {
        uint32_t old_total = total;

        total += srcs[i].len;
        /* If the previous operation overflowed, then total will have a smaller
         * value than previously. */
        if (total < old_total) {
            return false;
        }
    }

    _mongocrypt_buffer_init(dst);
    _mongocrypt_buffer_resize(dst, total);
    offset = 0;
    for (i = 0; i < num_srcs; i++) {
        if (srcs[i].len) {
            memcpy(dst->data + offset, srcs[i].data, srcs[i].len);
        }
        offset += srcs[i].len;
    }
    return true;
}

struct _mongocrypt_binary_t *_mongocrypt_buffer_as_binary(_mongocrypt_buffer_t *buf) {
    BSON_ASSERT_PARAM(buf);

    buf->bin.data = buf->data;
    buf->bin.len = buf->len;
    return &buf->bin;
}

bool _mongocrypt_buffer_copy_from_data_and_size(_mongocrypt_buffer_t *buf, const uint8_t *data, size_t len) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(data);

    _mongocrypt_buffer_init(buf);

    if (!size_to_uint32(len, &buf->len)) {
        return false;
    }

    if ((buf->data = bson_malloc(len))) {
        memcpy(buf->data, data, len);
        buf->owned = true;
    }

    return true;
}

bool _mongocrypt_buffer_steal_from_data_and_size(_mongocrypt_buffer_t *buf, uint8_t *data, size_t len) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(data);

    _mongocrypt_buffer_init(buf);
    if (!size_to_uint32(len, &buf->len)) {
        return false;
    }
    buf->data = data;
    buf->owned = true;
    return true;
}

bool _mongocrypt_buffer_steal_from_string(_mongocrypt_buffer_t *buf, char *str) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(str);

    _mongocrypt_buffer_init(buf);
    if (!size_to_uint32(strlen(str), &buf->len)) {
        return false;
    }
    buf->data = (uint8_t *)str;
    buf->owned = true;
    return true;
}

bool _mongocrypt_buffer_from_string(_mongocrypt_buffer_t *buf, const char *str) {
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(str);

    _mongocrypt_buffer_init(buf);
    if (!size_to_uint32(strlen(str), &buf->len)) {
        return false;
    }
    buf->data = (uint8_t *)str;
    buf->owned = false;
    return true;
}

void _mongocrypt_buffer_copy_from_uint64_le(_mongocrypt_buffer_t *buf, uint64_t value) {
    uint64_t value_le = MONGOCRYPT_UINT64_TO_LE(value);

    BSON_ASSERT_PARAM(buf);

    _mongocrypt_buffer_init(buf);
    _mongocrypt_buffer_resize(buf, sizeof(value));
    memcpy(buf->data, &value_le, buf->len);
}

bool _mongocrypt_buffer_from_subrange(_mongocrypt_buffer_t *out,
                                      const _mongocrypt_buffer_t *in,
                                      uint32_t offset,
                                      uint32_t len) {
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(in);

    _mongocrypt_buffer_init(out);
    BSON_ASSERT(offset <= UINT32_MAX - len);
    if (offset + len > in->len) {
        return false;
    }
    out->data = in->data + offset;
    out->len = len;
    return true;
}
