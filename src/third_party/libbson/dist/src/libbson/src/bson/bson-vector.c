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


#include <bson/bson-vector-private.h>

#include <bson/bson.h>

#include <mlib/config.h>


static BSON_INLINE bool
bson_vector_binary_header_impl_init(bson_vector_binary_header_impl_t *header_out,
                                    const uint8_t *binary_data,
                                    uint32_t binary_data_len)
{
   if (binary_data_len >= BSON_VECTOR_HEADER_LEN) {
      memcpy(header_out->bytes, binary_data, BSON_VECTOR_HEADER_LEN);
      return true;
   } else {
      return false;
   }
}

static BSON_INLINE bool
bson_vector_int8_validate(bson_vector_binary_header_impl_t header)
{
   return header.bytes[0] == bson_vector_header_byte_0(BSON_VECTOR_ELEMENT_SIGNED_INT, BSON_VECTOR_ELEMENT_8_BITS) &&
          header.bytes[1] == bson_vector_header_byte_1(0);
}

static BSON_INLINE bool
bson_vector_float32_validate(bson_vector_binary_header_impl_t header, uint32_t binary_data_len)
{
   return (binary_data_len - BSON_VECTOR_HEADER_LEN) % sizeof(float) == 0 &&
          header.bytes[0] == bson_vector_header_byte_0(BSON_VECTOR_ELEMENT_FLOAT, BSON_VECTOR_ELEMENT_32_BITS) &&
          header.bytes[1] == bson_vector_header_byte_1(0);
}

static BSON_INLINE bool
bson_vector_packed_bit_validate(bson_vector_binary_header_impl_t header,
                                const uint8_t *binary_data,
                                uint32_t binary_data_len)
{
   if (header.bytes[0] == bson_vector_header_byte_0(BSON_VECTOR_ELEMENT_UNSIGNED_INT, BSON_VECTOR_ELEMENT_1_BIT)) {
      size_t padding = bson_vector_padding_from_header_byte_1(header.bytes[1]);
      if (header.bytes[1] != bson_vector_header_byte_1(padding)) {
         return false;
      }
      uint32_t vector_data_len = binary_data_len - BSON_VECTOR_HEADER_LEN;
      if (vector_data_len == 0) {
         return padding == 0;
      } else {
         // We need to read the last byte of the binary block to validate that unused bits are zero.
         uint8_t last_data_byte = binary_data[binary_data_len - 1];
         uint8_t mask_of_unused_bits = (uint8_t)((1u << padding) - 1u);
         return (last_data_byte & mask_of_unused_bits) == 0;
      }
   } else {
      return false;
   }
}


bool
bson_vector_int8_view_init(bson_vector_int8_view_t *view_out, uint8_t *binary_data, uint32_t binary_data_len)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(binary_data);
   bson_vector_binary_header_impl_t header;
   if (bson_vector_binary_header_impl_init(&header, binary_data, binary_data_len) &&
       bson_vector_int8_validate(header)) {
      if (view_out) {
         *view_out = (bson_vector_int8_view_t){
            .binary.data = binary_data, .binary.data_len = binary_data_len, .binary.header_copy = header};
      }
      return true;
   } else {
      return false;
   }
}

bool
bson_vector_int8_const_view_init(bson_vector_int8_const_view_t *view_out,
                                 const uint8_t *binary_data,
                                 uint32_t binary_data_len)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(binary_data);
   bson_vector_binary_header_impl_t header;
   if (bson_vector_binary_header_impl_init(&header, binary_data, binary_data_len) &&
       bson_vector_int8_validate(header)) {
      if (view_out) {
         *view_out = (bson_vector_int8_const_view_t){
            .binary.data = binary_data, .binary.data_len = binary_data_len, .binary.header_copy = header};
      }
      return true;
   } else {
      return false;
   }
}

bool
bson_vector_float32_view_init(bson_vector_float32_view_t *view_out, uint8_t *binary_data, uint32_t binary_data_len)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(binary_data);
   bson_vector_binary_header_impl_t header;
   if (bson_vector_binary_header_impl_init(&header, binary_data, binary_data_len) &&
       bson_vector_float32_validate(header, binary_data_len)) {
      if (view_out) {
         *view_out = (bson_vector_float32_view_t){
            .binary.data = binary_data, .binary.data_len = binary_data_len, .binary.header_copy = header};
      }
      return true;
   } else {
      return false;
   }
}

bool
bson_vector_float32_const_view_init(bson_vector_float32_const_view_t *view_out,
                                    const uint8_t *binary_data,
                                    uint32_t binary_data_len)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(binary_data);
   bson_vector_binary_header_impl_t header;
   if (bson_vector_binary_header_impl_init(&header, binary_data, binary_data_len) &&
       bson_vector_float32_validate(header, binary_data_len)) {
      if (view_out) {
         *view_out = (bson_vector_float32_const_view_t){
            .binary.data = binary_data, .binary.data_len = binary_data_len, .binary.header_copy = header};
      }
      return true;
   } else {
      return false;
   }
}

bool
bson_vector_packed_bit_view_init(bson_vector_packed_bit_view_t *view_out,
                                 uint8_t *binary_data,
                                 uint32_t binary_data_len)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(binary_data);
   bson_vector_binary_header_impl_t header;
   if (bson_vector_binary_header_impl_init(&header, binary_data, binary_data_len) &&
       bson_vector_packed_bit_validate(header, binary_data, binary_data_len)) {
      if (view_out) {
         *view_out = (bson_vector_packed_bit_view_t){
            .binary.data = binary_data, .binary.data_len = binary_data_len, .binary.header_copy = header};
      }
      return true;
   } else {
      return false;
   }
}

bool
bson_vector_packed_bit_const_view_init(bson_vector_packed_bit_const_view_t *view_out,
                                       const uint8_t *binary_data,
                                       uint32_t binary_data_len)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(binary_data);
   bson_vector_binary_header_impl_t header;
   if (bson_vector_binary_header_impl_init(&header, binary_data, binary_data_len) &&
       bson_vector_packed_bit_validate(header, binary_data, binary_data_len)) {
      if (view_out) {
         *view_out = (bson_vector_packed_bit_const_view_t){
            .binary.data = binary_data, .binary.data_len = binary_data_len, .binary.header_copy = header};
      }
      return true;
   } else {
      return false;
   }
}


bool
bson_vector_int8_view_from_iter(bson_vector_int8_view_t *view_out, bson_iter_t *iter)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(iter);
   if (BSON_ITER_HOLDS_BINARY(iter)) {
      uint32_t binary_len;
      uint8_t *binary;
      bson_iter_overwrite_binary(iter, BSON_SUBTYPE_VECTOR, &binary_len, &binary);
      return binary && bson_vector_int8_view_init(view_out, binary, binary_len);
   } else {
      return false;
   }
}

bool
bson_vector_int8_const_view_from_iter(bson_vector_int8_const_view_t *view_out, const bson_iter_t *iter)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(iter);
   if (BSON_ITER_HOLDS_BINARY(iter)) {
      bson_subtype_t subtype;
      uint32_t binary_len;
      const uint8_t *binary;
      bson_iter_binary(iter, &subtype, &binary_len, &binary);
      return binary && subtype == BSON_SUBTYPE_VECTOR && bson_vector_int8_const_view_init(view_out, binary, binary_len);
   } else {
      return false;
   }
}

bool
bson_vector_float32_view_from_iter(bson_vector_float32_view_t *view_out, bson_iter_t *iter)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(iter);
   if (BSON_ITER_HOLDS_BINARY(iter)) {
      uint32_t binary_len;
      uint8_t *binary;
      bson_iter_overwrite_binary(iter, BSON_SUBTYPE_VECTOR, &binary_len, &binary);
      return binary && bson_vector_float32_view_init(view_out, binary, binary_len);
   } else {
      return false;
   }
}

bool
bson_vector_float32_const_view_from_iter(bson_vector_float32_const_view_t *view_out, const bson_iter_t *iter)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(iter);
   if (BSON_ITER_HOLDS_BINARY(iter)) {
      bson_subtype_t subtype;
      uint32_t binary_len;
      const uint8_t *binary;
      bson_iter_binary(iter, &subtype, &binary_len, &binary);
      return binary && subtype == BSON_SUBTYPE_VECTOR &&
             bson_vector_float32_const_view_init(view_out, binary, binary_len);
   } else {
      return false;
   }
}

bool
bson_vector_packed_bit_view_from_iter(bson_vector_packed_bit_view_t *view_out, bson_iter_t *iter)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(iter);
   if (BSON_ITER_HOLDS_BINARY(iter)) {
      uint32_t binary_len;
      uint8_t *binary;
      bson_iter_overwrite_binary(iter, BSON_SUBTYPE_VECTOR, &binary_len, &binary);
      return binary && bson_vector_packed_bit_view_init(view_out, binary, binary_len);
   } else {
      return false;
   }
}

bool
bson_vector_packed_bit_const_view_from_iter(bson_vector_packed_bit_const_view_t *view_out, const bson_iter_t *iter)
{
   BSON_OPTIONAL_PARAM(view_out);
   BSON_ASSERT_PARAM(iter);
   if (BSON_ITER_HOLDS_BINARY(iter)) {
      bson_subtype_t subtype;
      uint32_t binary_len;
      const uint8_t *binary;
      bson_iter_binary(iter, &subtype, &binary_len, &binary);
      return binary && subtype == BSON_SUBTYPE_VECTOR &&
             bson_vector_packed_bit_const_view_init(view_out, binary, binary_len);
   } else {
      return false;
   }
}


bool
bson_append_vector_int8_uninit(
   bson_t *bson, const char *key, int key_length, size_t element_count, bson_vector_int8_view_t *view_out)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(view_out);

   uint32_t length = bson_vector_int8_binary_data_length(element_count);
   if (length < BSON_VECTOR_HEADER_LEN) {
      return false;
   }
   uint8_t *binary;
   if (bson_append_binary_uninit(bson, key, key_length, BSON_SUBTYPE_VECTOR, &binary, length)) {
      bson_vector_binary_header_impl_t header = {
         .bytes[0] = bson_vector_header_byte_0(BSON_VECTOR_ELEMENT_SIGNED_INT, BSON_VECTOR_ELEMENT_8_BITS),
         .bytes[1] = bson_vector_header_byte_1(0)};
      memcpy(binary, header.bytes, BSON_VECTOR_HEADER_LEN);
      *view_out =
         (bson_vector_int8_view_t){.binary.data = binary, .binary.data_len = length, .binary.header_copy = header};
      return true;
   } else {
      return false;
   }
}

bool
bson_append_vector_float32_uninit(
   bson_t *bson, const char *key, int key_length, size_t element_count, bson_vector_float32_view_t *view_out)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(view_out);

   uint32_t length = bson_vector_float32_binary_data_length(element_count);
   if (length < BSON_VECTOR_HEADER_LEN) {
      return false;
   }
   uint8_t *binary;
   if (bson_append_binary_uninit(bson, key, key_length, BSON_SUBTYPE_VECTOR, &binary, length)) {
      bson_vector_binary_header_impl_t header = {
         .bytes[0] = bson_vector_header_byte_0(BSON_VECTOR_ELEMENT_FLOAT, BSON_VECTOR_ELEMENT_32_BITS),
         .bytes[1] = bson_vector_header_byte_1(0)};
      memcpy(binary, header.bytes, BSON_VECTOR_HEADER_LEN);
      *view_out =
         (bson_vector_float32_view_t){.binary.data = binary, .binary.data_len = length, .binary.header_copy = header};
      return true;
   } else {
      return false;
   }
}

bool
bson_append_vector_packed_bit_uninit(
   bson_t *bson, const char *key, int key_length, size_t element_count, bson_vector_packed_bit_view_t *view_out)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(view_out);

   uint32_t length = bson_vector_packed_bit_binary_data_length(element_count);
   if (length < BSON_VECTOR_HEADER_LEN) {
      return false;
   }
   uint8_t *binary;
   if (bson_append_binary_uninit(bson, key, key_length, BSON_SUBTYPE_VECTOR, &binary, length)) {
      mlib_diagnostic_push();
      mlib_msvc_warning(disable : 4146);
      size_t padding = (size_t)7 & -element_count;
      mlib_diagnostic_pop();

      bson_vector_binary_header_impl_t header = {
         .bytes[0] = bson_vector_header_byte_0(BSON_VECTOR_ELEMENT_UNSIGNED_INT, BSON_VECTOR_ELEMENT_1_BIT),
         .bytes[1] = bson_vector_header_byte_1(padding)};
      memcpy(binary, header.bytes, BSON_VECTOR_HEADER_LEN);
      if (element_count > 0 && padding > 0) {
         // We must explicitly zero bits in the final byte that aren't part of any element.
         // No reason to read-modify-write here, it's better to write the whole byte.
         binary[length - 1u] = 0u;
      }
      *view_out = (bson_vector_packed_bit_view_t){
         .binary.data = binary, .binary.data_len = length, .binary.header_copy = header};
      return true;
   } else {
      return false;
   }
}

static bool
bson_vector_from_array_expect_key(const bson_iter_t *iter, uint32_t numeric_key, bson_error_t *error)
{
   char buffer[16];
   const char *key;
   bson_uint32_to_string(numeric_key, &key, buffer, sizeof buffer);
   if (0 == strcmp(key, bson_iter_key(iter))) {
      return true;
   } else {
      bson_set_error(error,
                     BSON_ERROR_VECTOR,
                     BSON_VECTOR_ERROR_ARRAY_KEY,
                     "expected BSON array key '%s', found key '%s'",
                     key,
                     bson_iter_key(iter));
      return false;
   }
}

static void
bson_vector_set_error_max_size(bson_error_t *error)
{
   bson_set_error(error, BSON_ERROR_VECTOR, BSON_VECTOR_ERROR_MAX_SIZE, "maximum BSON document size would be exceeded");
}

bool
bson_append_vector_int8_from_array(
   bson_t *bson, const char *key, int key_length, const bson_iter_t *iter, bson_error_t *error)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(key);
   BSON_ASSERT_PARAM(iter);

   uint32_t element_count = 0;
   {
      bson_iter_t validation_iter = *iter;
      while (bson_iter_next(&validation_iter)) {
         if (!bson_vector_from_array_expect_key(&validation_iter, element_count, error)) {
            return false;
         }
         if (!BSON_ITER_HOLDS_INT(&validation_iter)) {
            bson_set_error(error,
                           BSON_ERROR_VECTOR,
                           BSON_VECTOR_ERROR_ARRAY_ELEMENT_TYPE,
                           "expected int32 or int64 in BSON array key '%s', found item type 0x%02X",
                           bson_iter_key(&validation_iter),
                           (unsigned)bson_iter_type(&validation_iter));
            return false;
         }
         int64_t element_as_int64 = bson_iter_as_int64(&validation_iter);
         if (element_as_int64 < INT8_MIN || element_as_int64 > INT8_MAX) {
            bson_set_error(error,
                           BSON_ERROR_VECTOR,
                           BSON_VECTOR_ERROR_ARRAY_ELEMENT_VALUE,
                           "BSON array key '%s' value %" PRId64 " is out of range for vector of int8",
                           bson_iter_key(&validation_iter),
                           element_as_int64);
            return false;
         }
         element_count++;
      }
   }

   bson_vector_int8_view_t view;
   if (!bson_append_vector_int8_uninit(bson, key, key_length, element_count, &view)) {
      bson_vector_set_error_max_size(error);
      return false;
   }
   bson_iter_t copy_iter = *iter;
   for (uint32_t i = 0; i < element_count; i++) {
      BSON_ASSERT(bson_iter_next(&copy_iter));
      int8_t element = (int8_t)bson_iter_as_int64(&copy_iter);
      BSON_ASSERT(bson_vector_int8_view_write(view, &element, 1, i));
   }
   return true;
}

bool
bson_append_vector_float32_from_array(
   bson_t *bson, const char *key, int key_length, const bson_iter_t *iter, bson_error_t *error)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(key);
   BSON_ASSERT_PARAM(iter);

   uint32_t element_count = 0;
   {
      bson_iter_t validation_iter = *iter;
      while (bson_iter_next(&validation_iter)) {
         if (!bson_vector_from_array_expect_key(&validation_iter, element_count, error)) {
            return false;
         }
         if (!BSON_ITER_HOLDS_DOUBLE(&validation_iter)) {
            bson_set_error(error,
                           BSON_ERROR_VECTOR,
                           BSON_VECTOR_ERROR_ARRAY_ELEMENT_TYPE,
                           "expected 'double' number type in BSON array key '%s', found item type 0x%02X",
                           bson_iter_key(&validation_iter),
                           (unsigned)bson_iter_type(&validation_iter));
            return false;
         }
         element_count++;
      }
   }

   bson_vector_float32_view_t view;
   if (!bson_append_vector_float32_uninit(bson, key, key_length, element_count, &view)) {
      bson_vector_set_error_max_size(error);
      return false;
   }
   bson_iter_t copy_iter = *iter;
   for (uint32_t i = 0; i < element_count; i++) {
      BSON_ASSERT(bson_iter_next(&copy_iter));
      float element = (float)bson_iter_double(&copy_iter);
      BSON_ASSERT(bson_vector_float32_view_write(view, &element, 1, i));
   }
   return true;
}

bool
bson_append_vector_packed_bit_from_array(
   bson_t *bson, const char *key, int key_length, const bson_iter_t *iter, bson_error_t *error)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(key);
   BSON_ASSERT_PARAM(iter);

   uint32_t element_count = 0;
   {
      bson_iter_t validation_iter = *iter;
      while (bson_iter_next(&validation_iter)) {
         if (!bson_vector_from_array_expect_key(&validation_iter, element_count, error)) {
            return false;
         }
         if (!BSON_ITER_HOLDS_INT(&validation_iter) && !BSON_ITER_HOLDS_BOOL(&validation_iter)) {
            bson_set_error(error,
                           BSON_ERROR_VECTOR,
                           BSON_VECTOR_ERROR_ARRAY_ELEMENT_TYPE,
                           "expected int32, int64, or bool in BSON array key '%s', found item type 0x%02X",
                           bson_iter_key(&validation_iter),
                           (unsigned)bson_iter_type(&validation_iter));
            return false;
         }
         int64_t element_as_int64 = bson_iter_as_int64(&validation_iter);
         if (element_as_int64 < 0 || element_as_int64 > 1) {
            bson_set_error(error,
                           BSON_ERROR_VECTOR,
                           BSON_VECTOR_ERROR_ARRAY_ELEMENT_VALUE,
                           "BSON array key '%s' value %" PRId64 " is out of range for vector of packed_bit",
                           bson_iter_key(&validation_iter),
                           element_as_int64);
            return false;
         }
         element_count++;
      }
   }

   bson_vector_packed_bit_view_t view;
   if (!bson_append_vector_packed_bit_uninit(bson, key, key_length, element_count, &view)) {
      bson_vector_set_error_max_size(error);
      return false;
   }
   bson_iter_t copy_iter = *iter;
   for (uint32_t i = 0; i < element_count; i++) {
      BSON_ASSERT(bson_iter_next(&copy_iter));
      bool element_as_bool = (bool)bson_iter_as_int64(&copy_iter);
      BSON_ASSERT(bson_vector_packed_bit_view_pack_bool(view, &element_as_bool, 1, i));
   }
   return true;
}


bool
bson_array_builder_append_vector_int8_elements(bson_array_builder_t *builder, bson_vector_int8_const_view_t view)
{
   BSON_ASSERT_PARAM(builder);
   size_t length = bson_vector_int8_const_view_length(view);
   for (size_t i = 0; i < length; i++) {
      // Note, the zero initializer is only needed due to a false positive -Wmaybe-uninitialized warning in uncommon
      // configurations where the compiler does not have visibility into memcpy().
      int8_t element = 0;
      BSON_ASSERT(bson_vector_int8_const_view_read(view, &element, 1, i));
      if (!bson_array_builder_append_int32(builder, (int32_t)element)) {
         return false;
      }
   }
   return true;
}

bool
bson_array_builder_append_vector_float32_elements(bson_array_builder_t *builder, bson_vector_float32_const_view_t view)
{
   BSON_ASSERT_PARAM(builder);
   size_t length = bson_vector_float32_const_view_length(view);
   for (size_t i = 0; i < length; i++) {
      float element;
      BSON_ASSERT(bson_vector_float32_const_view_read(view, &element, 1, i));
      if (!bson_array_builder_append_double(builder, (double)element)) {
         return false;
      }
   }
   return true;
}

bool
bson_array_builder_append_vector_packed_bit_elements(bson_array_builder_t *builder,
                                                     bson_vector_packed_bit_const_view_t view)
{
   BSON_ASSERT_PARAM(builder);
   size_t length = bson_vector_packed_bit_const_view_length(view);
   for (size_t i = 0; i < length; i++) {
      bool element;
      BSON_ASSERT(bson_vector_packed_bit_const_view_unpack_bool(view, &element, 1, i));
      if (!bson_array_builder_append_int32(builder, element ? 1 : 0)) {
         return false;
      }
   }
   return true;
}


bool
bson_array_builder_append_vector_elements(bson_array_builder_t *builder, const bson_iter_t *iter)
{
   BSON_ASSERT_PARAM(builder);
   BSON_ASSERT_PARAM(iter);
   {
      bson_vector_int8_const_view_t view;
      if (bson_vector_int8_const_view_from_iter(&view, iter)) {
         return bson_array_builder_append_vector_int8_elements(builder, view);
      }
   }
   {
      bson_vector_float32_const_view_t view;
      if (bson_vector_float32_const_view_from_iter(&view, iter)) {
         return bson_array_builder_append_vector_float32_elements(builder, view);
      }
   }
   {
      bson_vector_packed_bit_const_view_t view;
      if (bson_vector_packed_bit_const_view_from_iter(&view, iter)) {
         return bson_array_builder_append_vector_packed_bit_elements(builder, view);
      }
   }
   return false;
}


bool
bson_append_array_from_vector_int8(bson_t *bson, const char *key, int key_length, bson_vector_int8_const_view_t view)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(key);
   bson_array_builder_t *child;
   if (bson_append_array_builder_begin(bson, key, key_length, &child)) {
      bool ok = bson_array_builder_append_vector_int8_elements(child, view);
      return bson_append_array_builder_end(bson, child) && ok;
   } else {
      return false;
   }
}

bool
bson_append_array_from_vector_float32(bson_t *bson,
                                      const char *key,
                                      int key_length,
                                      bson_vector_float32_const_view_t view)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(key);
   bson_array_builder_t *child;
   if (bson_append_array_builder_begin(bson, key, key_length, &child)) {
      bool ok = bson_array_builder_append_vector_float32_elements(child, view);
      return bson_append_array_builder_end(bson, child) && ok;
   } else {
      return false;
   }
}

bool
bson_append_array_from_vector_packed_bit(bson_t *bson,
                                         const char *key,
                                         int key_length,
                                         bson_vector_packed_bit_const_view_t view)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(key);
   bson_array_builder_t *child;
   if (bson_append_array_builder_begin(bson, key, key_length, &child)) {
      bool ok = bson_array_builder_append_vector_packed_bit_elements(child, view);
      return bson_append_array_builder_end(bson, child) && ok;
   } else {
      return false;
   }
}


bool
bson_append_array_from_vector(bson_t *bson, const char *key, int key_length, const bson_iter_t *iter)
{
   BSON_ASSERT_PARAM(bson);
   BSON_ASSERT_PARAM(key);
   BSON_ASSERT_PARAM(iter);
   bson_array_builder_t *child;
   if (bson_append_array_builder_begin(bson, key, key_length, &child)) {
      bool ok = bson_array_builder_append_vector_elements(child, iter);
      return bson_append_array_builder_end(bson, child) && ok;
   } else {
      return false;
   }
}
