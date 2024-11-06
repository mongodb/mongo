/*
 * Copyright 2021-present MongoDB, Inc.
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

#ifndef KMS_KMIP_READER_WRITER_PRIVATE_H
#define KMS_KMIP_READER_WRITER_PRIVATE_H

#include "kms_kmip_item_type_private.h"
#include "kms_kmip_tag_type_private.h"
#include "kms_message_private.h"

#include <stdint.h>

typedef struct _kmip_writer_t kmip_writer_t;

kmip_writer_t * kmip_writer_new (void);

void kmip_writer_destroy (kmip_writer_t *writer);

void
kmip_writer_write_u8 (kmip_writer_t *writer, uint8_t value);

void
kmip_writer_write_u16 (kmip_writer_t *writer, uint16_t value);

void
kmip_writer_write_u32 (kmip_writer_t *writer, uint32_t value);

void
kmip_writer_write_u64 (kmip_writer_t *writer, uint64_t value);

void
kmip_writer_write_tag_enum (kmip_writer_t *writer, kmip_tag_type_t tag);

void
kmip_writer_write_string (kmip_writer_t *writer, kmip_tag_type_t tag, const char *str, size_t len);

void
kmip_writer_write_bytes (kmip_writer_t *writer, kmip_tag_type_t tag, const char *str, size_t len);

void
kmip_writer_write_integer (kmip_writer_t *writer, kmip_tag_type_t tag, int32_t value);

void
kmip_writer_write_long_integer (kmip_writer_t *writer, kmip_tag_type_t tag, int64_t value);

void
kmip_writer_write_enumeration (kmip_writer_t *writer, kmip_tag_type_t tag, int32_t value);

void
kmip_writer_write_bool (kmip_writer_t *writer, kmip_tag_type_t tag, bool value);

void
kmip_writer_write_datetime (kmip_writer_t *writer, kmip_tag_type_t tag, int64_t value);

void
kmip_writer_begin_struct (kmip_writer_t *writer, kmip_tag_type_t tag);

void
kmip_writer_close_struct (kmip_writer_t *writer);

const uint8_t *
kmip_writer_get_buffer (kmip_writer_t *writer, size_t* len);

typedef struct _kmip_reader_t kmip_reader_t;

kmip_reader_t *
kmip_reader_new (uint8_t *ptr, size_t len);

void
kmip_reader_destroy (kmip_reader_t *reader);

bool
kmip_reader_in_place (kmip_reader_t *reader,
                      size_t pos,
                      size_t len,
                      kmip_reader_t *out_reader);

bool
kmip_reader_has_data (kmip_reader_t *reader);

bool
kmip_reader_read_u8 (kmip_reader_t *reader, uint8_t *value);

bool
kmip_reader_read_u16 (kmip_reader_t *reader, uint16_t *value);

bool
kmip_reader_read_u32 (kmip_reader_t *reader, uint32_t *value);

bool
kmip_reader_read_u64 (kmip_reader_t *reader, uint64_t *value);

bool
kmip_reader_read_tag (kmip_reader_t *reader, kmip_tag_type_t *tag);

bool
kmip_reader_read_length (kmip_reader_t *reader, uint32_t *length);

bool
kmip_reader_read_type (kmip_reader_t *reader, kmip_item_type_t *type);

bool
kmip_reader_read_enumeration (kmip_reader_t *reader, uint32_t *enum_value);

bool
kmip_reader_read_bool (kmip_reader_t *reader, bool *value);

bool
kmip_reader_read_integer (kmip_reader_t *reader, int32_t *value);

bool
kmip_reader_read_long_integer (kmip_reader_t *reader, int64_t *value);

bool
kmip_reader_read_bytes (kmip_reader_t *reader, uint8_t **ptr, size_t length);

bool
kmip_reader_read_string (kmip_reader_t *reader, uint8_t **ptr, size_t length);

/* kmip_reader_find does not descend structures.
 * To find and descend into a structure use kmip_reader_find_and_recurse. */
bool
kmip_reader_find (kmip_reader_t *reader,
                  kmip_tag_type_t search_tag,
                  kmip_item_type_t type,
                  size_t *pos,
                  size_t *length);

bool
kmip_reader_find_and_recurse (kmip_reader_t *reader, kmip_tag_type_t tag);

bool
kmip_reader_find_and_read_enum (kmip_reader_t *reader,
                                kmip_tag_type_t tag,
                                uint32_t *value);

bool
kmip_reader_find_and_read_bytes (kmip_reader_t *reader,
                                 kmip_tag_type_t tag,
                                 uint8_t **out_ptr,
                                 size_t *out_len);

#endif /* KMS_KMIP_READER_WRITER_PRIVATE_H */
