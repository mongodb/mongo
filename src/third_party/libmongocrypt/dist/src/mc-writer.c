/*
 * Copyright 2022-present MongoDB, Inc.
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

#include "mongocrypt-private.h"

#include "mc-writer-private.h"

#define CHECK_AND_RETURN(x)                                                                                            \
    if (!(x)) {                                                                                                        \
        return false;                                                                                                  \
    }

#define CHECK_REMAINING_BUFFER_AND_RET(write_size)                                                                     \
    if ((write_size) > writer->len - writer->pos) {                                                                    \
        CLIENT_ERR("%s expected at most %" PRIu64 " bytes, got: %" PRIu64,                                             \
                   writer->parser_name,                                                                                \
                   (writer->len - writer->pos),                                                                        \
                   (uint64_t)(write_size));                                                                            \
        return false;                                                                                                  \
    }

void mc_writer_init(mc_writer_t *writer, uint8_t *ptr, uint64_t len, const char *parser_name) {
    BSON_ASSERT_PARAM(writer);
    BSON_ASSERT_PARAM(ptr);
    BSON_ASSERT_PARAM(parser_name);

    *writer = (mc_writer_t){.pos = 0u, .ptr = ptr, .len = len, .parser_name = parser_name};
}

void mc_writer_init_from_buffer(mc_writer_t *writer, _mongocrypt_buffer_t *buf, const char *parser_name) {
    BSON_ASSERT_PARAM(writer);
    BSON_ASSERT_PARAM(buf);
    BSON_ASSERT_PARAM(parser_name);

    mc_writer_init(writer, buf->data, buf->len, parser_name);
}

mc_writer_t *mc_writer_new(uint8_t *ptr, uint64_t len, const char *parser_name) {
    BSON_ASSERT_PARAM(ptr);
    BSON_ASSERT_PARAM(parser_name);

    mc_writer_t *writer = bson_malloc(sizeof(mc_writer_t));
    mc_writer_init(writer, ptr, len, parser_name);
    return writer;
}

void mc_writer_destroy(mc_writer_t *writer) {
    bson_free(writer);
}

bool mc_writer_write_u8(mc_writer_t *writer, const uint8_t value, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(writer);
    CHECK_REMAINING_BUFFER_AND_RET(sizeof(uint8_t));
    memcpy(writer->ptr + writer->pos, &value, sizeof(uint8_t));

    writer->pos += sizeof(uint8_t);
    return true;
}

bool mc_writer_write_u32(mc_writer_t *writer, const uint32_t value, mongocrypt_status_t *status) {
    CHECK_REMAINING_BUFFER_AND_RET(sizeof(uint32_t));

    uint32_t temp = BSON_UINT32_TO_LE(value);
    memcpy(writer->ptr + writer->pos, &temp, sizeof(uint32_t));
    writer->pos += sizeof(uint32_t);

    return true;
}

bool mc_writer_write_u64(mc_writer_t *writer, const uint64_t value, mongocrypt_status_t *status) {
    CHECK_REMAINING_BUFFER_AND_RET(sizeof(uint64_t));

    uint64_t temp = BSON_UINT64_TO_LE(value);
    memcpy(writer->ptr + writer->pos, &temp, sizeof(uint64_t));
    writer->pos += sizeof(uint64_t);

    return true;
}

bool mc_writer_write_buffer(mc_writer_t *writer,
                            const _mongocrypt_buffer_t *buf,
                            uint64_t length,
                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(writer);
    BSON_ASSERT_PARAM(buf);

    if (length > buf->len) {
        CLIENT_ERR("%s cannot write %" PRIu64 " bytes from buffer with length %" PRIu32,
                   writer->parser_name,
                   length,
                   buf->len);

        return false;
    }

    CHECK_REMAINING_BUFFER_AND_RET(length);

    if (length > SIZE_MAX) {
        CLIENT_ERR("%s failed to copy "
                   "data of length %" PRIu64,
                   writer->parser_name,
                   length);
        return false;
    }

    memcpy(writer->ptr + writer->pos, buf->data, (size_t)length);
    writer->pos += length;

    return true;
}

bool mc_writer_write_uuid_buffer(mc_writer_t *writer, const _mongocrypt_buffer_t *buf, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(writer);
    BSON_ASSERT_PARAM(buf);

    CHECK_AND_RETURN(mc_writer_write_buffer(writer, buf, UUID_LEN, status));
    return true;
}

bool mc_writer_write_prfblock_buffer(mc_writer_t *writer,
                                     const _mongocrypt_buffer_t *buf,
                                     mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(writer);
    BSON_ASSERT_PARAM(buf);

    CHECK_AND_RETURN(mc_writer_write_buffer(writer, buf, PRF_LEN, status));
    return true;
}
