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

#include <bson/bson-prelude.h>

#ifndef BSON_VECTOR_H
#define BSON_VECTOR_H

#include <bson/bson-endian.h>
#include <bson/bson-types.h>
#include <bson/bson_t.h>
#include <bson/compat.h>
#include <bson/macros.h>

BSON_BEGIN_DECLS


// Length of the required header for BSON_SUBTYPE_VECTOR, in bytes
#define BSON_VECTOR_HEADER_LEN 2

// Forward declaration (typedef bson_array_builder_t in bson.h)
struct _bson_array_builder_t;

/** @brief Error codes for domain BSON_ERROR_VECTOR */
typedef enum {
   BSON_VECTOR_ERROR_ARRAY_ELEMENT_TYPE = 1,
   BSON_VECTOR_ERROR_ARRAY_ELEMENT_VALUE,
   BSON_VECTOR_ERROR_ARRAY_KEY,
   BSON_VECTOR_ERROR_MAX_SIZE,
} bson_vector_error_code_t;


/** @brief Implementation detail. A copy of the BSON_SUBTYPE_VECTOR header, suitable for pass-by-value. */
typedef struct bson_vector_binary_header_impl_t {
   uint8_t bytes[BSON_VECTOR_HEADER_LEN];
} bson_vector_binary_header_impl_t;

/** @brief Implementation detail. A reference to non-owned const BSON Binary data of subtype BSON_SUBTYPE_VECTOR */
typedef struct bson_vector_binary_const_view_impl_t {
   const uint8_t *data;
   uint32_t data_len;
   bson_vector_binary_header_impl_t header_copy;
} bson_vector_binary_const_view_impl_t;

/** @brief Implementation detail. A reference to non-owned BSON Binary data of subtype BSON_SUBTYPE_VECTOR */
typedef struct bson_vector_binary_view_impl_t {
   uint8_t *data;
   uint32_t data_len;
   bson_vector_binary_header_impl_t header_copy;
} bson_vector_binary_view_impl_t;

/** @brief Implementation detail. Obtain a const reference from a non-const reference without re-validating. */
static BSON_INLINE bson_vector_binary_const_view_impl_t
bson_vector_binary_view_impl_as_const(bson_vector_binary_view_impl_t view)
{
   bson_vector_binary_const_view_impl_t result;
   result.data = view.data;
   result.data_len = view.data_len;
   result.header_copy = view.header_copy;
   return result;
}


/** @brief A reference to non-owned BSON Binary data holding a valid Vector of int8 element type */
typedef struct bson_vector_int8_view_t {
   bson_vector_binary_view_impl_t binary;
} bson_vector_int8_view_t;

/** @brief A reference to non-owned const BSON Binary data holding a valid Vector of int8 element type */
typedef struct bson_vector_int8_const_view_t {
   bson_vector_binary_const_view_impl_t binary;
} bson_vector_int8_const_view_t;

/** @brief A reference to non-owned BSON Binary data holding a valid Vector of float32 element type */
typedef struct bson_vector_float32_view_t {
   bson_vector_binary_view_impl_t binary;
} bson_vector_float32_view_t;

/** @brief A reference to non-owned const BSON Binary data holding a valid Vector of float32 element type */
typedef struct bson_vector_float32_const_view_t {
   bson_vector_binary_const_view_impl_t binary;
} bson_vector_float32_const_view_t;

/** @brief A reference to non-owned BSON Binary data holding a valid Vector of packed_bit */
typedef struct bson_vector_packed_bit_view_t {
   bson_vector_binary_view_impl_t binary;
} bson_vector_packed_bit_view_t;

/** @brief A reference to non-owned const BSON Binary data holding a valid Vector of packed_bit */
typedef struct bson_vector_packed_bit_const_view_t {
   bson_vector_binary_const_view_impl_t binary;
} bson_vector_packed_bit_const_view_t;


static BSON_INLINE bson_vector_int8_const_view_t
bson_vector_int8_view_as_const(bson_vector_int8_view_t view)
{
   bson_vector_int8_const_view_t result;
   result.binary = bson_vector_binary_view_impl_as_const(view.binary);
   return result;
}

static BSON_INLINE bson_vector_float32_const_view_t
bson_vector_float32_view_as_const(bson_vector_float32_view_t view)
{
   bson_vector_float32_const_view_t result;
   result.binary = bson_vector_binary_view_impl_as_const(view.binary);
   return result;
}

static BSON_INLINE bson_vector_packed_bit_const_view_t
bson_vector_packed_bit_view_as_const(bson_vector_packed_bit_view_t view)
{
   bson_vector_packed_bit_const_view_t result;
   result.binary = bson_vector_binary_view_impl_as_const(view.binary);
   return result;
}


BSON_EXPORT(bool)
bson_vector_int8_view_init(bson_vector_int8_view_t *view_out, uint8_t *binary_data, uint32_t binary_data_len);

BSON_EXPORT(bool)
bson_vector_int8_const_view_init(bson_vector_int8_const_view_t *view_out,
                                 const uint8_t *binary_data,
                                 uint32_t binary_data_len);

BSON_EXPORT(bool)
bson_vector_float32_view_init(bson_vector_float32_view_t *view_out, uint8_t *binary_data, uint32_t binary_data_len);


BSON_EXPORT(bool)
bson_vector_float32_const_view_init(bson_vector_float32_const_view_t *view_out,
                                    const uint8_t *binary_data,
                                    uint32_t binary_data_len);

BSON_EXPORT(bool)
bson_vector_packed_bit_view_init(bson_vector_packed_bit_view_t *view_out,
                                 uint8_t *binary_data,
                                 uint32_t binary_data_len);

BSON_EXPORT(bool)
bson_vector_packed_bit_const_view_init(bson_vector_packed_bit_const_view_t *view_out,
                                       const uint8_t *binary_data,
                                       uint32_t binary_data_len);


BSON_EXPORT(bool)
bson_vector_int8_view_from_iter(bson_vector_int8_view_t *view_out, bson_iter_t *iter);

BSON_EXPORT(bool)
bson_vector_int8_const_view_from_iter(bson_vector_int8_const_view_t *view_out, const bson_iter_t *iter);

BSON_EXPORT(bool)
bson_vector_float32_view_from_iter(bson_vector_float32_view_t *view_out, bson_iter_t *iter);

BSON_EXPORT(bool)
bson_vector_float32_const_view_from_iter(bson_vector_float32_const_view_t *view_out, const bson_iter_t *iter);

BSON_EXPORT(bool)
bson_vector_packed_bit_view_from_iter(bson_vector_packed_bit_view_t *view_out, bson_iter_t *iter);

BSON_EXPORT(bool)
bson_vector_packed_bit_const_view_from_iter(bson_vector_packed_bit_const_view_t *view_out, const bson_iter_t *iter);


BSON_EXPORT(bool)
bson_array_builder_append_vector_int8_elements(struct _bson_array_builder_t *builder,
                                               bson_vector_int8_const_view_t view);

BSON_EXPORT(bool)
bson_array_builder_append_vector_float32_elements(struct _bson_array_builder_t *builder,
                                                  bson_vector_float32_const_view_t view);

BSON_EXPORT(bool)
bson_array_builder_append_vector_packed_bit_elements(struct _bson_array_builder_t *builder,
                                                     bson_vector_packed_bit_const_view_t view);

BSON_EXPORT(bool)
bson_array_builder_append_vector_elements(struct _bson_array_builder_t *builder, const bson_iter_t *iter);


BSON_EXPORT(bool)
bson_append_vector_int8_uninit(
   bson_t *bson, const char *key, int key_length, size_t element_count, bson_vector_int8_view_t *view_out);

#define BSON_APPEND_VECTOR_INT8_UNINIT(b, key, count, view) \
   bson_append_vector_int8_uninit(b, key, (int)strlen(key), count, view)

BSON_EXPORT(bool)
bson_append_vector_float32_uninit(
   bson_t *bson, const char *key, int key_length, size_t element_count, bson_vector_float32_view_t *view_out);

#define BSON_APPEND_VECTOR_FLOAT32_UNINIT(b, key, count, view) \
   bson_append_vector_float32_uninit(b, key, (int)strlen(key), count, view)

BSON_EXPORT(bool)
bson_append_vector_packed_bit_uninit(
   bson_t *bson, const char *key, int key_length, size_t element_count, bson_vector_packed_bit_view_t *view_out);

#define BSON_APPEND_VECTOR_PACKED_BIT_UNINIT(b, key, count, view) \
   bson_append_vector_packed_bit_uninit(b, key, (int)strlen(key), count, view)


BSON_EXPORT(bool)
bson_append_vector_int8_from_array(
   bson_t *bson, const char *key, int key_length, const bson_iter_t *iter, bson_error_t *error);

#define BSON_APPEND_VECTOR_INT8_FROM_ARRAY(b, key, iter, err) \
   bson_append_vector_int8_from_array(b, key, (int)strlen(key), iter, err)

BSON_EXPORT(bool)
bson_append_vector_float32_from_array(
   bson_t *bson, const char *key, int key_length, const bson_iter_t *iter, bson_error_t *error);

#define BSON_APPEND_VECTOR_FLOAT32_FROM_ARRAY(b, key, iter, err) \
   bson_append_vector_float32_from_array(b, key, (int)strlen(key), iter, err)

BSON_EXPORT(bool)
bson_append_vector_packed_bit_from_array(
   bson_t *bson, const char *key, int key_length, const bson_iter_t *iter, bson_error_t *error);

#define BSON_APPEND_VECTOR_PACKED_BIT_FROM_ARRAY(b, key, iter, err) \
   bson_append_vector_packed_bit_from_array(b, key, (int)strlen(key), iter, err)


BSON_EXPORT(bool)
bson_append_array_from_vector_int8(bson_t *bson, const char *key, int key_length, bson_vector_int8_const_view_t view);

#define BSON_APPEND_ARRAY_FROM_VECTOR_INT8(b, key, view) \
   bson_append_array_from_vector_int8(b, key, (int)strlen(key), view)

BSON_EXPORT(bool)
bson_append_array_from_vector_float32(bson_t *bson,
                                      const char *key,
                                      int key_length,
                                      bson_vector_float32_const_view_t view);

#define BSON_APPEND_ARRAY_FROM_VECTOR_FLOAT32(b, key, view) \
   bson_append_array_from_vector_float32(b, key, (int)strlen(key), view)

BSON_EXPORT(bool)
bson_append_array_from_vector_packed_bit(bson_t *bson,
                                         const char *key,
                                         int key_length,
                                         bson_vector_packed_bit_const_view_t view);

#define BSON_APPEND_ARRAY_FROM_VECTOR_PACKED_BIT(b, key, view) \
   bson_append_array_from_vector_packed_bit(b, key, (int)strlen(key), view)


static BSON_INLINE const int8_t *
bson_vector_int8_const_view_pointer(bson_vector_int8_const_view_t view)
{
   BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
   return (const int8_t *)(view.binary.data + BSON_VECTOR_HEADER_LEN);
   BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
}

static BSON_INLINE int8_t *
bson_vector_int8_view_pointer(bson_vector_int8_view_t view)
{
   BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
   return (int8_t *)(view.binary.data + BSON_VECTOR_HEADER_LEN);
   BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
}


static BSON_INLINE uint32_t
bson_vector_int8_binary_data_length(size_t element_count)
{
   const size_t max_representable = (size_t)UINT32_MAX - (size_t)BSON_VECTOR_HEADER_LEN;
   return element_count > max_representable ? 0u : (uint32_t)element_count + (uint32_t)BSON_VECTOR_HEADER_LEN;
}

static BSON_INLINE uint32_t
bson_vector_float32_binary_data_length(size_t element_count)
{
   const size_t max_representable = ((size_t)UINT32_MAX - (size_t)BSON_VECTOR_HEADER_LEN) / sizeof(float);
   return element_count > max_representable
             ? 0u
             : (uint32_t)element_count * sizeof(float) + (uint32_t)BSON_VECTOR_HEADER_LEN;
}

static BSON_INLINE uint32_t
bson_vector_packed_bit_binary_data_length(size_t element_count)
{
   const size_t max_representable =
      (size_t)BSON_MIN((uint64_t)SIZE_MAX, ((uint64_t)UINT32_MAX - (uint64_t)BSON_VECTOR_HEADER_LEN) * 8u);
   return element_count > max_representable
             ? 0u
             : (uint32_t)(((uint64_t)element_count + 7u) / 8u) + (uint32_t)BSON_VECTOR_HEADER_LEN;
}


static BSON_INLINE size_t
bson_vector_int8_const_view_length(bson_vector_int8_const_view_t view)
{
   return view.binary.data_len - (uint32_t)BSON_VECTOR_HEADER_LEN;
}

static BSON_INLINE size_t
bson_vector_int8_view_length(bson_vector_int8_view_t view)
{
   return bson_vector_int8_const_view_length(bson_vector_int8_view_as_const(view));
}

static BSON_INLINE size_t
bson_vector_float32_const_view_length(bson_vector_float32_const_view_t view)
{
   return (view.binary.data_len - (uint32_t)BSON_VECTOR_HEADER_LEN) / (uint32_t)sizeof(float);
}

static BSON_INLINE size_t
bson_vector_float32_view_length(bson_vector_float32_view_t view)
{
   return bson_vector_float32_const_view_length(bson_vector_float32_view_as_const(view));
}

static BSON_INLINE size_t
bson_vector_packed_bit_const_view_length_bytes(bson_vector_packed_bit_const_view_t view)
{
   return view.binary.data_len - (uint32_t)BSON_VECTOR_HEADER_LEN;
}

static BSON_INLINE size_t
bson_vector_packed_bit_view_length_bytes(bson_vector_packed_bit_view_t view)
{
   return bson_vector_packed_bit_const_view_length_bytes(bson_vector_packed_bit_view_as_const(view));
}

// Implementation detail, not part of documented API.
static BSON_INLINE size_t
bson_vector_padding_from_header_byte_1(uint8_t byte_1)
{
   return byte_1 & 7;
}

static BSON_INLINE size_t
bson_vector_packed_bit_const_view_padding(bson_vector_packed_bit_const_view_t view)
{
   BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
   return bson_vector_padding_from_header_byte_1(view.binary.header_copy.bytes[1]);
   BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
}

static BSON_INLINE size_t
bson_vector_packed_bit_view_padding(bson_vector_packed_bit_view_t view)
{
   return bson_vector_packed_bit_const_view_padding(bson_vector_packed_bit_view_as_const(view));
}

static BSON_INLINE size_t
bson_vector_packed_bit_const_view_length(bson_vector_packed_bit_const_view_t view)
{
   return bson_vector_packed_bit_const_view_length_bytes(view) * 8u - bson_vector_packed_bit_const_view_padding(view);
}

static BSON_INLINE size_t
bson_vector_packed_bit_view_length(bson_vector_packed_bit_view_t view)
{
   return bson_vector_packed_bit_const_view_length(bson_vector_packed_bit_view_as_const(view));
}


static BSON_INLINE bool
bson_vector_int8_const_view_read(bson_vector_int8_const_view_t view,
                                 int8_t *BSON_RESTRICT values_out,
                                 size_t element_count,
                                 size_t vector_offset_elements)
{
   size_t length = bson_vector_int8_const_view_length(view);
   if (BSON_LIKELY(vector_offset_elements <= length && element_count <= length - vector_offset_elements)) {
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
      memcpy(values_out, bson_vector_int8_const_view_pointer(view) + vector_offset_elements, element_count);
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
      return true;
   } else {
      return false;
   }
}

static BSON_INLINE bool
bson_vector_int8_view_read(bson_vector_int8_view_t view,
                           int8_t *BSON_RESTRICT values_out,
                           size_t element_count,
                           uint32_t vector_offset_elements)
{
   return bson_vector_int8_const_view_read(
      bson_vector_int8_view_as_const(view), values_out, element_count, vector_offset_elements);
}

static BSON_INLINE bool
bson_vector_int8_view_write(bson_vector_int8_view_t view,
                            const int8_t *BSON_RESTRICT values,
                            size_t element_count,
                            size_t vector_offset_elements)
{
   size_t length = bson_vector_int8_view_length(view);
   if (BSON_LIKELY(vector_offset_elements <= length && element_count <= length - vector_offset_elements)) {
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
      memcpy(bson_vector_int8_view_pointer(view) + vector_offset_elements, values, element_count);
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
      return true;
   } else {
      return false;
   }
}


BSON_STATIC_ASSERT2(float_is_float32, sizeof(float) == 4);

static BSON_INLINE bool
bson_vector_float32_const_view_read(bson_vector_float32_const_view_t view,
                                    float *BSON_RESTRICT values_out,
                                    size_t element_count,
                                    size_t vector_offset_elements)
{
   size_t length = bson_vector_float32_const_view_length(view);
   if (BSON_LIKELY(vector_offset_elements <= length && element_count <= length - vector_offset_elements)) {
      size_t byte_offset = BSON_VECTOR_HEADER_LEN + vector_offset_elements * 4;
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
#if BSON_BYTE_ORDER == BSON_LITTLE_ENDIAN
      memcpy(values_out, view.binary.data + byte_offset, element_count * 4);
#else
      size_t i;
      for (i = 0; i < element_count; i++) {
         float aligned_tmp;
         memcpy(&aligned_tmp, view.binary.data + byte_offset + i * 4, 4);
         values_out[i] = BSON_FLOAT_FROM_LE(aligned_tmp);
      }
#endif
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
      return true;
   } else {
      return false;
   }
}

static BSON_INLINE bool
bson_vector_float32_view_read(bson_vector_float32_view_t view,
                              float *BSON_RESTRICT values_out,
                              size_t element_count,
                              size_t vector_offset_elements)
{
   return bson_vector_float32_const_view_read(
      bson_vector_float32_view_as_const(view), values_out, element_count, vector_offset_elements);
}

static BSON_INLINE bool
bson_vector_float32_view_write(bson_vector_float32_view_t view,
                               const float *BSON_RESTRICT values,
                               size_t element_count,
                               size_t vector_offset_elements)
{
   size_t length = bson_vector_float32_view_length(view);
   if (BSON_LIKELY(vector_offset_elements <= length && element_count <= length - vector_offset_elements)) {
      size_t byte_offset = BSON_VECTOR_HEADER_LEN + vector_offset_elements * 4;
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
#if BSON_BYTE_ORDER == BSON_LITTLE_ENDIAN
      memcpy(view.binary.data + byte_offset, values, element_count * 4);
#else
      size_t i;
      for (i = 0; i < element_count; i++) {
         float aligned_tmp = BSON_FLOAT_TO_LE(values[i]);
         memcpy(view.binary.data + byte_offset + i * 4, &aligned_tmp, 4);
      }
#endif
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
      return true;
   } else {
      return false;
   }
}


static BSON_INLINE bool
bson_vector_packed_bit_const_view_read_packed(bson_vector_packed_bit_const_view_t view,
                                              uint8_t *BSON_RESTRICT packed_values_out,
                                              size_t byte_count,
                                              size_t vector_offset_bytes)
{
   size_t length_bytes = bson_vector_packed_bit_const_view_length_bytes(view);
   if (BSON_LIKELY(vector_offset_bytes <= length_bytes && byte_count <= length_bytes - vector_offset_bytes)) {
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
      memcpy(packed_values_out, view.binary.data + BSON_VECTOR_HEADER_LEN + vector_offset_bytes, byte_count);
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
      return true;
   } else {
      return false;
   }
}

static BSON_INLINE bool
bson_vector_packed_bit_view_read_packed(bson_vector_packed_bit_view_t view,
                                        uint8_t *BSON_RESTRICT packed_values_out,
                                        size_t byte_count,
                                        size_t vector_offset_bytes)
{
   return bson_vector_packed_bit_const_view_read_packed(
      bson_vector_packed_bit_view_as_const(view), packed_values_out, byte_count, vector_offset_bytes);
}

static BSON_INLINE bool
bson_vector_packed_bit_view_write_packed(bson_vector_packed_bit_view_t view,
                                         const uint8_t *BSON_RESTRICT packed_values,
                                         size_t byte_count,
                                         size_t vector_offset_bytes)
{
   size_t length_bytes = bson_vector_packed_bit_view_length_bytes(view);
   if (BSON_LIKELY(vector_offset_bytes <= length_bytes && byte_count <= length_bytes - vector_offset_bytes)) {
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
      if (byte_count == length_bytes - vector_offset_bytes && byte_count >= 1u) {
         // This write touches the last byte in the vector:
         // special-case that byte so we can ensure unused bits remain set to zero.
         size_t other_bytes = byte_count - 1u;
         memcpy(view.binary.data + BSON_VECTOR_HEADER_LEN + vector_offset_bytes, packed_values, other_bytes);
         view.binary.data[BSON_VECTOR_HEADER_LEN + vector_offset_bytes + other_bytes] =
            (UINT8_C(0xFF) << bson_vector_packed_bit_view_padding(view)) & packed_values[other_bytes];
      } else {
         memcpy(view.binary.data + BSON_VECTOR_HEADER_LEN + vector_offset_bytes, packed_values, byte_count);
      }
      BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
      return true;
   } else {
      return false;
   }
}


static BSON_INLINE bool
bson_vector_packed_bit_const_view_unpack_bool(bson_vector_packed_bit_const_view_t view,
                                              bool *BSON_RESTRICT unpacked_values_out,
                                              size_t element_count,
                                              size_t vector_offset_elements)
{
   size_t length = bson_vector_packed_bit_const_view_length(view);
   if (BSON_LIKELY(vector_offset_elements <= length && element_count <= length - vector_offset_elements)) {
      size_t i;
      for (i = 0; i < element_count; i++) {
         size_t element_index = vector_offset_elements + i;
         BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
         uint8_t packed_byte = view.binary.data[BSON_VECTOR_HEADER_LEN + (element_index >> 3)];
         unpacked_values_out[i] = 0 != (packed_byte & ((uint8_t)0x80 >> (element_index & 7)));
         BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
      }
      return true;
   } else {
      return false;
   }
}

static BSON_INLINE bool
bson_vector_packed_bit_view_unpack_bool(bson_vector_packed_bit_view_t view,
                                        bool *BSON_RESTRICT unpacked_values_out,
                                        size_t element_count,
                                        size_t vector_offset_elements)
{
   return bson_vector_packed_bit_const_view_unpack_bool(
      bson_vector_packed_bit_view_as_const(view), unpacked_values_out, element_count, vector_offset_elements);
}

static BSON_INLINE bool
bson_vector_packed_bit_view_pack_bool(bson_vector_packed_bit_view_t view,
                                      const bool *BSON_RESTRICT unpacked_values,
                                      size_t element_count,
                                      size_t vector_offset_elements)
{
   size_t length = bson_vector_packed_bit_view_length(view);
   if (BSON_LIKELY(vector_offset_elements <= length && element_count <= length - vector_offset_elements)) {
      while (element_count > 0) {
         BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_BEGIN
         uint8_t *BSON_RESTRICT packed_byte = BSON_VECTOR_HEADER_LEN + (vector_offset_elements >> 3) + view.binary.data;
         if (element_count >= 8 && (vector_offset_elements & 7) == 0) {
            uint8_t complete_byte = 0;
            unsigned i;
            for (i = 0; i < 8; i++) {
               complete_byte |= unpacked_values[i] ? ((uint8_t)0x80 >> i) : 0;
            }
            *packed_byte = complete_byte;
            unpacked_values += 8;
            vector_offset_elements += 8;
            element_count -= 8;
         } else {
            uint8_t mask = (uint8_t)0x80 >> (vector_offset_elements & 7);
            *packed_byte = (*packed_byte & ~mask) | (*unpacked_values ? mask : 0);
            unpacked_values++;
            vector_offset_elements++;
            element_count--;
         }
         BSON_DISABLE_UNSAFE_BUFFER_USAGE_WARNING_END
      }
      return true;
   } else {
      return false;
   }
}


BSON_END_DECLS

#endif /* BSON_VECTOR_H */
