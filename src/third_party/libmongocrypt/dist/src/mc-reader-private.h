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

#ifndef MONGOCRYPT_READER_PRIVATE_H
#define MONGOCRYPT_READER_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include <stdint.h>
#include <stdlib.h>

/**
 * A non-owning forward-only cursor api to read a buffer.
 *
 * Tracks length of buffer and current position of buffer. parser_name is
 * typically __FUNCTION__ to provide useful error messages automatically.
 *
 * All numbers are read as little endian.
 *
 * Functions return false on error and set mongocrypt_status_t to an error.
 *
 * Example:
 *
 *  const _mongocrypt_buffer_t *buf;
 *  mongocrypt_status_t *status
 *
 *  mc_reader_t reader;
 *  mc_reader_init_from_buffer (&reader, buf, __FUNCTION__);
 *
 *  _mongocrypt_buffer_t in;
 *  if (!mc_reader_read_uuid_buffer (&reader, &in, status)) {
 *     abort ();
 *  }
 *
 *  uint8_t value;
 *  if (!mc_reader_read_u8 (&reader, value, status)) {
 *     abort ();
 *  }
 */
struct _mc_reader_t {
    const uint8_t *ptr;
    uint64_t pos;
    uint64_t len;

    const char *parser_name;
};

typedef struct _mc_reader_t mc_reader_t;

void mc_reader_init(mc_reader_t *reader, const uint8_t *ptr, uint64_t len, const char *parser_name);

void mc_reader_init_from_buffer(mc_reader_t *reader, const _mongocrypt_buffer_t *buf, const char *parser_name);

mc_reader_t *mc_reader_new(const uint8_t *ptr, uint64_t len, const char *parser_name);

void mc_reader_destroy(mc_reader_t *reader);

bool mc_reader_has_data(const mc_reader_t *reader);

uint64_t mc_reader_get_remaining_length(const mc_reader_t *reader);

uint64_t mc_reader_get_consumed_length(const mc_reader_t *reader);

bool mc_reader_read_u8(mc_reader_t *reader, uint8_t *value, mongocrypt_status_t *status);

bool mc_reader_read_u32(mc_reader_t *reader, uint32_t *value, mongocrypt_status_t *status);

bool mc_reader_read_u64(mc_reader_t *reader, uint64_t *value, mongocrypt_status_t *status);

bool mc_reader_read_bytes(mc_reader_t *reader, const uint8_t **ptr, uint64_t length, mongocrypt_status_t *status);

bool mc_reader_read_buffer(mc_reader_t *reader,
                           _mongocrypt_buffer_t *buf,
                           uint64_t length,
                           mongocrypt_status_t *status);

bool mc_reader_read_uuid_buffer(mc_reader_t *reader, _mongocrypt_buffer_t *buf, mongocrypt_status_t *status);

bool mc_reader_read_prfblock_buffer(mc_reader_t *reader, _mongocrypt_buffer_t *buf, mongocrypt_status_t *status);

bool mc_reader_read_buffer_to_end(mc_reader_t *reader, _mongocrypt_buffer_t *buf, mongocrypt_status_t *status);

#endif /* MONGOCRYPT_READER_PRIVATE_H */
