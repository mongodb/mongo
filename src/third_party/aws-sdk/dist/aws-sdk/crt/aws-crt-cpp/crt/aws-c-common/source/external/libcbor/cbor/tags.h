/*
 * Copyright (c) 2014-2020 Pavel Kalvoda <me@pavelkalvoda.com>
 *
 * libcbor is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef LIBCBOR_TAGS_H
#define LIBCBOR_TAGS_H

#include "cbor/cbor_export.h"
#include "cbor/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Tag manipulation
 * ============================================================================
 */

/** Create a new tag
 *
 * @param value The tag value. Please consult the tag repository
 * @return Reference to the new tag item. The item's reference count is
 * initialized to one.
 * @return `NULL` if memory allocation fails
 */
_CBOR_NODISCARD CBOR_EXPORT cbor_item_t *cbor_new_tag(uint64_t value);

/** Get the tagged item
 *
 * @param item A tag
 * @return Reference to the tagged item
 *
 * Increases the reference count of the underlying item. The returned reference
 * must be released using #cbor_decref.
 */
_CBOR_NODISCARD CBOR_EXPORT cbor_item_t *cbor_tag_item(const cbor_item_t *item);

/** Get tag value
 *
 * @param item A tag
 * @return The tag value. Please consult the tag repository
 */
_CBOR_NODISCARD CBOR_EXPORT uint64_t cbor_tag_value(const cbor_item_t *item);

/** Set the tagged item
 *
 * @param item A tag
 * @param tagged_item The item to tag. Its reference count will be increased
 * by one.
 */
CBOR_EXPORT void cbor_tag_set_item(cbor_item_t *item, cbor_item_t *tagged_item);

/** Build a new tag
 *
 * @param item The item to tag. Its reference count will be increased by
 * one.
 * @param value Tag value
 * @return Reference to the new tag item. The item's reference count is
 * initialized to one.
 * @return `NULL` if memory allocation fails
 */
_CBOR_NODISCARD CBOR_EXPORT cbor_item_t *cbor_build_tag(uint64_t value,
                                                        cbor_item_t *item);

#ifdef __cplusplus
}
#endif

#endif  // LIBCBOR_TAGS_H
