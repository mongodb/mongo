/*
 * Copyright (c) 2014-2020 Pavel Kalvoda <me@pavelkalvoda.com>
 *
 * libcbor is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef LIBCBOR_ARRAYS_H
#define LIBCBOR_ARRAYS_H

#include "cbor/cbor_export.h"
#include "cbor/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Get the number of members
 *
 * @param item An array
 * @return The number of members
 */
_CBOR_NODISCARD
CBOR_EXPORT size_t cbor_array_size(const cbor_item_t* item);

/** Get the size of the allocated storage
 *
 * @param item An array
 * @return The size of the allocated storage (number of items)
 */
_CBOR_NODISCARD
CBOR_EXPORT size_t cbor_array_allocated(const cbor_item_t* item);

/** Get item by index
 *
 * @param item An array
 * @param index The index (zero-based)
 * @return Reference to the item, or `NULL` in case of boundary violation.
 *
 * Increases the reference count of the underlying item. The returned reference
 * must be released using #cbor_decref.
 */
_CBOR_NODISCARD
CBOR_EXPORT cbor_item_t* cbor_array_get(const cbor_item_t* item, size_t index);

/** Set item by index
 *
 * If the index is out of bounds, the array is not modified and false is
 * returned. Creating arrays with holes is not possible.
 *
 * @param item An array
 * @param value The item to assign
 * @param index The index (zero-based)
 * @return `true` on success, `false` on allocation failure.
 */
_CBOR_NODISCARD
CBOR_EXPORT bool cbor_array_set(cbor_item_t* item, size_t index,
                                cbor_item_t* value);

/** Replace item at an index
 *
 * The reference to the item being replaced will be released using #cbor_decref.
 *
 * @param item An array
 * @param value The item to assign. Its reference count will be increased by
 * one.
 * @param index The index (zero-based)
 * @return true on success, false on allocation failure.
 */
_CBOR_NODISCARD
CBOR_EXPORT bool cbor_array_replace(cbor_item_t* item, size_t index,
                                    cbor_item_t* value);

/** Is the array definite?
 *
 * @param item An array
 * @return Is the array definite?
 */
_CBOR_NODISCARD
CBOR_EXPORT bool cbor_array_is_definite(const cbor_item_t* item);

/** Is the array indefinite?
 *
 * @param item An array
 * @return Is the array indefinite?
 */
_CBOR_NODISCARD
CBOR_EXPORT bool cbor_array_is_indefinite(const cbor_item_t* item);

/** Get the array contents
 *
 * The items may be reordered and modified as long as references remain
 * consistent.
 *
 * @param item An array item
 * @return An array of #cbor_item_t pointers of size #cbor_array_size.
 */
_CBOR_NODISCARD
CBOR_EXPORT cbor_item_t** cbor_array_handle(const cbor_item_t* item);

/** Create new definite array
 *
 * @param size Number of slots to preallocate
 * @return Reference to the new array item. The item's reference count is
 * initialized to one.
 * @return `NULL` if memory allocation fails
 */
_CBOR_NODISCARD
CBOR_EXPORT cbor_item_t* cbor_new_definite_array(size_t size);

/** Create new indefinite array
 *
 * @return Reference to the new array item. The item's reference count is
 * initialized to one.
 * @return `NULL` if memory allocation fails
 */
_CBOR_NODISCARD
CBOR_EXPORT cbor_item_t* cbor_new_indefinite_array(void);

/** Append to the end
 *
 * For indefinite items, storage may be reallocated. For definite items, only
 * the preallocated capacity is available.
 *
 * @param array An array
 * @param pushee The item to push. Its reference count will be increased by
 * one.
 * @return `true` on success, `false` on failure
 */
_CBOR_NODISCARD
CBOR_EXPORT bool cbor_array_push(cbor_item_t* array, cbor_item_t* pushee);

#ifdef __cplusplus
}
#endif

#endif  // LIBCBOR_ARRAYS_H
