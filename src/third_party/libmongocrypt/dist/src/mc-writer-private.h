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

#ifndef MONGOCRYPT_WRITER_PRIVATE_H
#define MONGOCRYPT_WRITER_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include <stdint.h>
#include <stdlib.h>

/**
 * A non-owning forward-only cursor api to write to a buffer.
 *
 * Tracks length of buffer and current position of buffer. parser_name is
 * typically __FUNCTION__ to provide useful error messages automatically.
 *
 * All numbers are written as little endian.
 */

struct _mc_writer_t {
    uint8_t *ptr;
    uint64_t pos;
    uint64_t len;

    const char *parser_name;
};

typedef struct _mc_writer_t mc_writer_t;

void mc_writer_init(mc_writer_t *writer, uint8_t *ptr, uint64_t len, const char *parser_name);

void mc_writer_init_from_buffer(mc_writer_t *writer, _mongocrypt_buffer_t *buf, const char *parser_name);

mc_writer_t *mc_writer_new(uint8_t *ptr, uint64_t len, const char *parser_name);

void mc_writer_destroy(mc_writer_t *writer);

bool mc_writer_write_u8(mc_writer_t *writer, const uint8_t value, mongocrypt_status_t *status);

bool mc_writer_write_u32(mc_writer_t *writer, const uint32_t value, mongocrypt_status_t *status);

bool mc_writer_write_u64(mc_writer_t *writer, const uint64_t value, mongocrypt_status_t *status);

bool mc_writer_write_buffer(mc_writer_t *writer,
                            const _mongocrypt_buffer_t *buf,
                            uint64_t length,
                            mongocrypt_status_t *status);

bool mc_writer_write_uuid_buffer(mc_writer_t *writer, const _mongocrypt_buffer_t *buf, mongocrypt_status_t *status);

bool mc_writer_write_prfblock_buffer(mc_writer_t *writer, const _mongocrypt_buffer_t *buf, mongocrypt_status_t *status);

#endif /* MONGOCRYPT_READER_PRIVATE_H */
