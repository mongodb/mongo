/*
 * Copyright (c) 2014-2020 Pavel Kalvoda <me@pavelkalvoda.com>
 *
 * libcbor is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef LIBCBOR_SERIALIZATION_H
#define LIBCBOR_SERIALIZATION_H

#include "cbor/cbor_export.h"
#include "cbor/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * High level encoding
 * ============================================================================
 */

/** Serialize the given item
 *
 * @param item A data item
 * @param buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result. 0 on failure.
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize(const cbor_item_t *item,
                                                  cbor_mutable_data buffer,
                                                  size_t buffer_size);

/** Compute the length (in bytes) of the item when serialized using
 * `cbor_serialize`.
 *
 * Time complexity is proportional to the number of nested items.
 *
 * @param item A data item
 * @return Length (>= 1) of the item when serialized. 0 if the length overflows
 * `size_t`.
 */
_CBOR_NODISCARD CBOR_EXPORT size_t
cbor_serialized_size(const cbor_item_t *item);

/** Serialize the given item, allocating buffers as needed
 *
 * Since libcbor v0.10, the return value is always the same as `buffer_size` (if
 * provided, see https://github.com/PJK/libcbor/pull/251/). New clients should
 * ignore the return value.
 *
 * \rst
 * .. warning:: It is the caller's responsibility to free the buffer using an
 *  appropriate ``free`` implementation.
 * \endrst
 *
 * @param item A data item
 * @param[out] buffer Buffer containing the result
 * @param[out] buffer_size Size of the \p buffer, or 0 on memory allocation
 * failure.
 * @return Length of the result in bytes
 * @return 0 on memory allocation failure, in which case \p buffer is `NULL`.
 */
CBOR_EXPORT size_t cbor_serialize_alloc(const cbor_item_t *item,
                                        unsigned char **buffer,
                                        size_t *buffer_size);

/** Serialize an uint
 *
 * @param item A uint
 * @param[out] buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result
 * @return 0 if the \p buffer_size doesn't fit the result
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize_uint(const cbor_item_t *item,
                                                       cbor_mutable_data buffer,
                                                       size_t buffer_size);

/** Serialize a negint
 *
 * @param item A negint
 * @param[out] buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result
 * @return 0 if the \p buffer_size doesn't fit the result
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize_negint(
    const cbor_item_t *item, cbor_mutable_data buffer, size_t buffer_size);

/** Serialize a bytestring
 *
 * @param item A bytestring
 * @param[out] buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result
 * @return 0 if the \p buffer_size doesn't fit the result. The \p buffer may
 * still be modified
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize_bytestring(
    const cbor_item_t *item, cbor_mutable_data buffer, size_t buffer_size);

/** Serialize a string
 *
 * @param item A string
 * @param[out] buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result
 * @return 0 if the \p buffer_size doesn't fit the result. The \p buffer may
 * still be modified
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize_string(
    const cbor_item_t *item, cbor_mutable_data buffer, size_t buffer_size);
/** Serialize an array
 *
 * @param item An array
 * @param[out] buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result
 * @return 0 if the \p buffer_size doesn't fit the result. The \p buffer may
 * still be modified
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize_array(
    const cbor_item_t *item, cbor_mutable_data buffer, size_t buffer_size);

/** Serialize a map
 *
 * @param item A map
 * @param[out] buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result
 * @return 0 if the \p buffer_size doesn't fit the result. The \p buffer may
 * still be modified
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize_map(const cbor_item_t *item,
                                                      cbor_mutable_data buffer,
                                                      size_t buffer_size);

/** Serialize a tag
 *
 * @param item A tag
 * @param[out] buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result
 * @return 0 if the \p buffer_size doesn't fit the result. The \p buffer may
 * still be modified
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize_tag(const cbor_item_t *item,
                                                      cbor_mutable_data buffer,
                                                      size_t buffer_size);

/** Serialize a
 *
 * @param item A float or ctrl
 * @param[out] buffer Buffer to serialize to
 * @param buffer_size Size of the \p buffer
 * @return Length of the result
 * @return 0 if the \p buffer_size doesn't fit the result
 */
_CBOR_NODISCARD CBOR_EXPORT size_t cbor_serialize_float_ctrl(
    const cbor_item_t *item, cbor_mutable_data buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif  // LIBCBOR_SERIALIZATION_H
