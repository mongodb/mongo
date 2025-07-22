/* pb_common.h: Common support functions for pb_encode.c and pb_decode.c.
 * These functions are rarely needed by applications directly.
 */

#ifndef PB_COMMON_H_INCLUDED
#define PB_COMMON_H_INCLUDED

#include "nanopb/pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the field iterator structure to beginning.
 * Returns false if the message type is empty. */
bool pb_field_iter_begin(pb_field_iter_t *iter, const pb_msgdesc_t *desc, void *message);

/* Get a field iterator for extension field. */
bool pb_field_iter_begin_extension(pb_field_iter_t *iter, pb_extension_t *extension);

/* Same as pb_field_iter_begin(), but for const message pointer.
 * Note that the pointers in pb_field_iter_t will be non-const but shouldn't
 * be written to when using these functions. */
bool pb_field_iter_begin_const(pb_field_iter_t *iter, const pb_msgdesc_t *desc, const void *message);
bool pb_field_iter_begin_extension_const(pb_field_iter_t *iter, const pb_extension_t *extension);

/* Advance the iterator to the next field.
 * Returns false when the iterator wraps back to the first field. */
bool pb_field_iter_next(pb_field_iter_t *iter);

/* Advance the iterator until it points at a field with the given tag.
 * Returns false if no such field exists. */
bool pb_field_iter_find(pb_field_iter_t *iter, uint32_t tag);

/* Find a field with type PB_LTYPE_EXTENSION, or return false if not found.
 * There can be only one extension range field per message. */
bool pb_field_iter_find_extension(pb_field_iter_t *iter);

#ifdef PB_VALIDATE_UTF8
/* Validate UTF-8 text string */
bool pb_validate_utf8(const char *s);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

