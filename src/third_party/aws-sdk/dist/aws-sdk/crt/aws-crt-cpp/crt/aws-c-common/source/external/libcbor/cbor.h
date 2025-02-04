/*
 * Copyright (c) 2014-2020 Pavel Kalvoda <me@pavelkalvoda.com>
 *
 * libcbor is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef LIBCBOR_H_
#define LIBCBOR_H_

#include "cbor/common.h"
#include "cbor/data.h"

#include "cbor/arrays.h"
#include "cbor/bytestrings.h"
#include "cbor/floats_ctrls.h"
#include "cbor/ints.h"
#include "cbor/maps.h"
#include "cbor/strings.h"
#include "cbor/tags.h"

#include "cbor/callbacks.h"
#include "cbor/cbor_export.h"
#include "cbor/encoding.h"
#include "cbor/serialization.h"
#include "cbor/streaming.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * High level decoding
 * ============================================================================
 */

/** Loads data item from a buffer
 *
 * @param source The buffer
 * @param source_size
 * @param[out] result Result indicator. #CBOR_ERR_NONE on success
 * @return Decoded CBOR item. The item's reference count is initialized to one.
 * @return `NULL` on failure. In that case, \p result contains the location and
 * description of the error.
 */
_CBOR_NODISCARD CBOR_EXPORT cbor_item_t* cbor_load(
    cbor_data source, size_t source_size, struct cbor_load_result* result);

/** Take a deep copy of an item
 *
 * All items this item points to (array and map members, string chunks, tagged
 * items) will be copied recursively using #cbor_copy. The new item doesn't
 * alias or point to any items from the original \p item. All the reference
 * counts in the new structure are set to one.
 *
 * @param item item to copy
 * @return Reference to the new item. The item's reference count is initialized
 * to one.
 * @return `NULL` if memory allocation fails
 */
_CBOR_NODISCARD CBOR_EXPORT cbor_item_t* cbor_copy(cbor_item_t* item);

#if CBOR_PRETTY_PRINTER
#include <stdio.h>

CBOR_EXPORT void cbor_describe(cbor_item_t* item, FILE* out);
#endif

#ifdef __cplusplus
}
#endif

#endif  // LIBCBOR_H_
