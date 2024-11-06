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

#include "kms_kmip_reader_writer_private.h"

#include "kms_endian_private.h"
#include "kms_request_str.h"

#include <stdint.h>

#define MAX_KMIP_WRITER_POSITIONS 10

/* KMIP encodes signed integers with two's complement.
 * Parsing functions read Integer / LongInteger as int32_t / int64_t by
 * reinterpreting byte representation.
 * Ensure that platform represents integers in two's complement.
 * See: https://stackoverflow.com/a/64843863/774658 */
#if (-1 & 3) != 3
#error Error: Twos complement integer representation is required.
#endif

struct _kmip_writer_t {
   kms_request_str_t *buffer;

   size_t positions[MAX_KMIP_WRITER_POSITIONS];
   size_t cur_pos;
};

kmip_writer_t *
kmip_writer_new (void)
{
   kmip_writer_t *writer = calloc (1, sizeof (kmip_writer_t));
   writer->buffer = kms_request_str_new ();
   return writer;
}

void
kmip_writer_destroy (kmip_writer_t *writer)
{
   kms_request_str_destroy (writer->buffer);
   free (writer);
}

void
kmip_writer_write_u8 (kmip_writer_t *writer, uint8_t value)
{
   char *c = (char *) &value;

   kms_request_str_append_chars (writer->buffer, c, 1);
}

void
kmip_writer_write_u16 (kmip_writer_t *writer, uint16_t value)
{
   uint16_t v = KMS_UINT16_TO_BE (value);
   char *c = (char *) &v;

   kms_request_str_append_chars (writer->buffer, c, 2);
}

void
kmip_writer_write_u32 (kmip_writer_t *writer, uint32_t value)
{
   uint32_t v = KMS_UINT32_TO_BE (value);
   char *c = (char *) &v;

   kms_request_str_append_chars (writer->buffer, c, 4);
}

void
kmip_writer_write_u64 (kmip_writer_t *writer, uint64_t value)
{
   uint64_t v = KMS_UINT64_TO_BE (value);
   char *c = (char *) &v;

   kms_request_str_append_chars (writer->buffer, c, 8);
}

void
kmip_writer_write_tag_enum (kmip_writer_t *writer, kmip_tag_type_t tag)
{
   /* The 0x42 prefix is for tags built into the protocol. */
   /* The 0x54 prefix is for extension tags. */
   kmip_writer_write_u8 (writer, 0x42);
   kmip_writer_write_u16 (writer, (uint16_t) tag);
}

static size_t
compute_padded_length (size_t len)
{
   if (len % 8 == 0) {
      return len;
   }

   size_t padding = 8 - (len % 8);
   return len + padding;
}

void
kmip_writer_write_string (kmip_writer_t *writer, kmip_tag_type_t tag, const char *str, size_t len)
{
   kmip_writer_write_tag_enum (writer, tag);
   kmip_writer_write_u8 (writer, KMIP_ITEM_TYPE_TextString);
   kmip_writer_write_u32 (writer, (uint32_t) len);

   size_t i;
   for (i = 0; i < len; i++) {
      kmip_writer_write_u8 (writer, (uint8_t) str[i]);
   }

   size_t padded_length = compute_padded_length (len);
   for (i = 0; i < padded_length - len; i++) {
      kmip_writer_write_u8 (writer, 0);
   }
}

void
kmip_writer_write_bytes (kmip_writer_t *writer, kmip_tag_type_t tag, const char *str, size_t len)
{
   kmip_writer_write_tag_enum (writer, tag);

   kmip_writer_write_u8 (writer, KMIP_ITEM_TYPE_ByteString);
   kmip_writer_write_u32 (writer, (uint32_t) len);

   size_t i;
   for (i = 0; i < len; i++) {
      kmip_writer_write_u8 (writer, (uint8_t) str[i]);
   }

   size_t padded_length = compute_padded_length (len);
   for (i = 0; i < padded_length - len; i++) {
      kmip_writer_write_u8 (writer, 0);
   }
}

void
kmip_writer_write_integer (kmip_writer_t *writer, kmip_tag_type_t tag, int32_t value)
{
   kmip_writer_write_tag_enum (writer, tag);
   kmip_writer_write_u8 (writer, KMIP_ITEM_TYPE_Integer);
   kmip_writer_write_u32 (writer, 4);
   KMS_ASSERT (value >= 0);
   kmip_writer_write_u32 (writer, (uint32_t) value);
   kmip_writer_write_u32 (writer, 0);
}

void
kmip_writer_write_long_integer (kmip_writer_t *writer, kmip_tag_type_t tag, int64_t value)
{
   kmip_writer_write_tag_enum (writer, tag);
   kmip_writer_write_u8 (writer, KMIP_ITEM_TYPE_LongInteger);
   kmip_writer_write_u32 (writer, 8);
   KMS_ASSERT (value >= 0);
   kmip_writer_write_u64 (writer, (uint64_t) value);
}

void
kmip_writer_write_enumeration (kmip_writer_t *writer, kmip_tag_type_t tag, int32_t value)
{
   kmip_writer_write_tag_enum (writer, tag);
   kmip_writer_write_u8 (writer, KMIP_ITEM_TYPE_Enumeration);
   kmip_writer_write_u32 (writer, 4);
   KMS_ASSERT (value >= 0);
   kmip_writer_write_u32 (writer, (uint32_t) value);
   kmip_writer_write_u32 (writer, 0);
}

void kmip_writer_write_bool (kmip_writer_t *writer, kmip_tag_type_t tag, bool value)
{
   kmip_writer_write_tag_enum (writer, tag);
   kmip_writer_write_u8 (writer, KMIP_ITEM_TYPE_Boolean);
   kmip_writer_write_u32 (writer, 8);
   kmip_writer_write_u64(writer, (uint64_t) value);
}

void
kmip_writer_write_datetime (kmip_writer_t *writer, kmip_tag_type_t tag, int64_t value)
{
   kmip_writer_write_tag_enum (writer, tag);
   kmip_writer_write_u8 (writer, KMIP_ITEM_TYPE_DateTime);
   kmip_writer_write_u32 (writer, 8);
   KMS_ASSERT (value >= 0);
   kmip_writer_write_u64 (writer, (uint64_t) value);
}

void
kmip_writer_begin_struct (kmip_writer_t *writer, kmip_tag_type_t tag)
{
   kmip_writer_write_tag_enum (writer, tag);
   kmip_writer_write_u8 (writer, KMIP_ITEM_TYPE_Structure);

   size_t pos = writer->buffer->len;

   kmip_writer_write_u32 (writer, 0);
   KMS_ASSERT(writer->cur_pos < MAX_KMIP_WRITER_POSITIONS);
   writer->cur_pos++;
   writer->positions[writer->cur_pos] = pos;
}

void
kmip_writer_close_struct (kmip_writer_t *writer)
{
   size_t current_pos = writer->buffer->len;
   KMS_ASSERT(writer->cur_pos > 0);
   size_t start_pos = writer->positions[writer->cur_pos];
   writer->cur_pos--;
   /* offset by 4 */
   uint32_t len = (uint32_t) (current_pos - start_pos - 4);

   uint32_t v = KMS_UINT32_TO_BE (len);
   char *c = (char *) &v;
   memcpy (writer->buffer->str + start_pos, c, 4);
}

const uint8_t *
kmip_writer_get_buffer (kmip_writer_t *writer, size_t* len) {
   *len = writer->buffer->len;
   return (const uint8_t*) writer->buffer->str;
}

struct _kmip_reader_t {
   uint8_t *ptr;
   size_t pos;
   size_t len;
};

kmip_reader_t *
kmip_reader_new (uint8_t *ptr, size_t len)
{
   kmip_reader_t *reader = calloc (1, sizeof (kmip_reader_t));
   reader->ptr = ptr;
   reader->len = len;
   return reader;
}

void
kmip_reader_destroy (kmip_reader_t *reader)
{
   free (reader);
}

bool
kmip_reader_in_place (kmip_reader_t *reader,
                      size_t pos,
                      size_t len,
                      kmip_reader_t *out_reader)
{
   /* Everything should be padding to 8 byte boundaries. */
   len = compute_padded_length (len);
   if ((pos + len) > reader->len) {
      return false;
   }

   memset (out_reader, 0, sizeof (kmip_reader_t));
   out_reader->ptr = reader->ptr + reader->pos;
   out_reader->len = len;
   return true;
}

bool
kmip_reader_has_data (kmip_reader_t *reader)
{
   return reader->pos < reader->len;
}

#define CHECK_REMAINING_BUFFER_AND_RET(read_size)   \
   if ((reader->pos + (read_size)) > reader->len) { \
      return false;                                 \
   }

bool
kmip_reader_read_u8 (kmip_reader_t *reader, uint8_t *value)
{
   CHECK_REMAINING_BUFFER_AND_RET (sizeof (uint8_t));

   *value = *(reader->ptr + reader->pos);
   reader->pos += sizeof (uint8_t);

   return true;
}

bool
kmip_reader_read_u16 (kmip_reader_t *reader, uint16_t *value)
{
   CHECK_REMAINING_BUFFER_AND_RET (sizeof (uint16_t));

   uint16_t temp;
   memcpy (&temp, reader->ptr + reader->pos, sizeof (uint16_t));
   *value = KMS_UINT16_FROM_BE (temp);
   reader->pos += sizeof (uint16_t);

   return true;
}

bool
kmip_reader_read_u32 (kmip_reader_t *reader, uint32_t *value)
{
   CHECK_REMAINING_BUFFER_AND_RET (sizeof (uint32_t));

   uint32_t temp;
   memcpy (&temp, reader->ptr + reader->pos, sizeof (uint32_t));
   *value = KMS_UINT32_FROM_BE (temp);
   reader->pos += sizeof (uint32_t);

   return true;
}

bool
kmip_reader_read_u64 (kmip_reader_t *reader, uint64_t *value)
{
   CHECK_REMAINING_BUFFER_AND_RET (sizeof (uint64_t));

   uint64_t temp;
   memcpy (&temp, reader->ptr + reader->pos, sizeof (uint64_t));
   *value = KMS_UINT64_FROM_BE (temp);
   reader->pos += sizeof (uint64_t);

   return true;
}

bool
kmip_reader_read_bytes (kmip_reader_t *reader, uint8_t **ptr, size_t length)
{
   size_t advance_length = compute_padded_length (length);
   CHECK_REMAINING_BUFFER_AND_RET (advance_length);

   *ptr = reader->ptr + reader->pos;
   reader->pos += advance_length;

   return true;
}

#define CHECK_AND_RET(x) \
   if (!(x)) {           \
      return false;      \
   }

bool
kmip_reader_read_tag (kmip_reader_t *reader, kmip_tag_type_t *tag)
{
   uint8_t tag_first;

   CHECK_AND_RET (kmip_reader_read_u8 (reader, &tag_first));

   if (tag_first != 0x42) {
      return false;
   }

   uint16_t tag_second;
   CHECK_AND_RET (kmip_reader_read_u16 (reader, &tag_second));

   *tag = (kmip_tag_type_t) (0x420000 + tag_second);
   return true;
}

bool
kmip_reader_read_length (kmip_reader_t *reader, uint32_t *length)
{
   return kmip_reader_read_u32 (reader, length);
}

bool
kmip_reader_read_type (kmip_reader_t *reader, kmip_item_type_t *type)
{
   uint8_t u8;
   CHECK_AND_RET (kmip_reader_read_u8 (reader, &u8));
   *type = (kmip_item_type_t) u8;
   return true;
}

bool
kmip_reader_read_enumeration (kmip_reader_t *reader, uint32_t *enum_value)
{
   CHECK_AND_RET (kmip_reader_read_u32 (reader, enum_value));

   /* Skip 4 bytes because enums are padded. */
   uint32_t ignored;

   return kmip_reader_read_u32 (reader, &ignored);
}

bool
kmip_reader_read_bool (kmip_reader_t *reader, bool *value)
{
   uint64_t u64;
   CHECK_AND_RET (kmip_reader_read_u64 (reader, &u64));
   *value = (bool) u64;
   return true;
}

bool
kmip_reader_read_integer (kmip_reader_t *reader, int32_t *value)
{
   CHECK_AND_RET (kmip_reader_read_u32 (reader, (uint32_t*) value));

   /* Skip 4 bytes because integers are padded. */
   uint32_t ignored;

   return kmip_reader_read_u32 (reader, &ignored);
}

bool
kmip_reader_read_long_integer (kmip_reader_t *reader, int64_t *value)
{
   return kmip_reader_read_u64 (reader, (uint64_t*) value);
}

bool
kmip_reader_read_string (kmip_reader_t *reader, uint8_t **ptr, size_t length)
{
   return kmip_reader_read_bytes (reader, ptr, length);
}

bool
kmip_reader_find (kmip_reader_t *reader,
                  kmip_tag_type_t search_tag,
                  kmip_item_type_t type,
                  size_t *pos,
                  size_t *length)
{
   reader->pos = 0;

   while (kmip_reader_has_data (reader)) {
      kmip_tag_type_t read_tag;
      CHECK_AND_RET (kmip_reader_read_tag (reader, &read_tag));

      kmip_item_type_t read_type;
      CHECK_AND_RET (kmip_reader_read_type (reader, &read_type));

      uint32_t read_length;
      CHECK_AND_RET (kmip_reader_read_length (reader, &read_length));


      if (read_tag == search_tag && read_type == type) {
         *pos = reader->pos;
         *length = read_length;
         return true;
      }

      size_t advance_length = read_length;
      advance_length = compute_padded_length (advance_length);

      CHECK_REMAINING_BUFFER_AND_RET (advance_length);

      /* Skip to the next type. */
      reader->pos += advance_length;
   }

   return false;
}

bool
kmip_reader_find_and_recurse (kmip_reader_t *reader, kmip_tag_type_t tag)
{
   size_t pos;
   size_t length;

   if (!kmip_reader_find (reader, tag, KMIP_ITEM_TYPE_Structure, &pos, &length)) {
      return false;
   }

   reader->pos = 0;
   reader->ptr = reader->ptr + pos;
   reader->len = length;
   return true;
}

bool
kmip_reader_find_and_read_enum (kmip_reader_t *reader,
                                kmip_tag_type_t tag,
                                uint32_t *value)
{
   size_t pos;
   size_t length;

   if (!kmip_reader_find (reader, tag, KMIP_ITEM_TYPE_Enumeration, &pos, &length)) {
      return false;
   }

   kmip_reader_t temp_reader;
   if (!kmip_reader_in_place (reader, pos, length, &temp_reader)) {
      return false;
   }

   return kmip_reader_read_enumeration (&temp_reader, value);
}

bool
kmip_reader_find_and_read_bytes (kmip_reader_t *reader,
                                 kmip_tag_type_t tag,
                                 uint8_t **out_ptr,
                                 size_t *out_len)
{
   size_t pos;

   if (!kmip_reader_find (reader, tag, KMIP_ITEM_TYPE_ByteString, &pos, out_len)) {
      return false;
   }

   kmip_reader_t temp_reader;
   if (!kmip_reader_in_place (reader, pos, *out_len, &temp_reader)) {
      return false;
   }

   return kmip_reader_read_bytes (&temp_reader, out_ptr, *out_len);
}
