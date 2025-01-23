/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "stuffer/s2n_stuffer.h"

#include <sys/param.h>

#include "error/s2n_errno.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

S2N_RESULT s2n_stuffer_validate(const struct s2n_stuffer *stuffer)
{
    /**
     * Note that we do not assert any properties on the tainted field,
     * as any boolean value in that field is valid.
     */
    RESULT_ENSURE_REF(stuffer);
    RESULT_GUARD(s2n_blob_validate(&stuffer->blob));
    RESULT_DEBUG_ENSURE(S2N_IMPLIES(stuffer->growable, stuffer->alloced), S2N_ERR_SAFETY);

    /* <= is valid because we can have a fully written/read stuffer */
    RESULT_DEBUG_ENSURE(stuffer->high_water_mark <= stuffer->blob.size, S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(stuffer->write_cursor <= stuffer->high_water_mark, S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(stuffer->read_cursor <= stuffer->write_cursor, S2N_ERR_SAFETY);
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_stuffer_reservation_validate(const struct s2n_stuffer_reservation *reservation)
{
    /**
     * Note that we need two dereferences here to decrease proof complexity
     * for CBMC (see https://github.com/awslabs/s2n/issues/2290). We can roll back
     * this change once CBMC can handle common subexpression elimination.
     */
    RESULT_ENSURE_REF(reservation);
    const struct s2n_stuffer_reservation reserve_obj = *reservation;
    RESULT_GUARD(s2n_stuffer_validate(reserve_obj.stuffer));
    const struct s2n_stuffer stuffer_obj = *(reserve_obj.stuffer);

    /* Verify that write_cursor + length can be represented as a uint32_t without overflow */
    RESULT_ENSURE_LTE(reserve_obj.write_cursor, UINT32_MAX - reserve_obj.length);
    /* The entire reservation must fit between the stuffer read and write cursors */
    RESULT_ENSURE_LTE(reserve_obj.write_cursor + reserve_obj.length, stuffer_obj.write_cursor);
    RESULT_ENSURE_GTE(reserve_obj.write_cursor, stuffer_obj.read_cursor);

    return S2N_RESULT_OK;
}

int s2n_stuffer_init(struct s2n_stuffer *stuffer, struct s2n_blob *in)
{
    POSIX_ENSURE_MUT(stuffer);
    POSIX_PRECONDITION(s2n_blob_validate(in));
    stuffer->blob = *in;
    stuffer->read_cursor = 0;
    stuffer->write_cursor = 0;
    stuffer->high_water_mark = 0;
    stuffer->alloced = 0;
    stuffer->growable = 0;
    stuffer->tainted = 0;
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_init_written(struct s2n_stuffer *stuffer, struct s2n_blob *in)
{
    POSIX_ENSURE_REF(in);
    POSIX_GUARD(s2n_stuffer_init(stuffer, in));
    POSIX_GUARD(s2n_stuffer_skip_write(stuffer, in->size));
    return S2N_SUCCESS;
}

int s2n_stuffer_alloc(struct s2n_stuffer *stuffer, const uint32_t size)
{
    POSIX_ENSURE_REF(stuffer);
    *stuffer = (struct s2n_stuffer){ 0 };
    POSIX_GUARD(s2n_alloc(&stuffer->blob, size));
    POSIX_GUARD(s2n_stuffer_init(stuffer, &stuffer->blob));

    stuffer->alloced = 1;

    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_growable_alloc(struct s2n_stuffer *stuffer, const uint32_t size)
{
    POSIX_GUARD(s2n_stuffer_alloc(stuffer, size));

    stuffer->growable = 1;

    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_free(struct s2n_stuffer *stuffer)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    if (stuffer->alloced) {
        POSIX_GUARD(s2n_free(&stuffer->blob));
    }
    *stuffer = (struct s2n_stuffer){ 0 };
    return S2N_SUCCESS;
}

int s2n_stuffer_free_without_wipe(struct s2n_stuffer *stuffer)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    if (stuffer->alloced) {
        POSIX_GUARD(s2n_free_without_wipe(&stuffer->blob));
    }
    *stuffer = (struct s2n_stuffer){ 0 };
    return S2N_SUCCESS;
}

int s2n_stuffer_resize(struct s2n_stuffer *stuffer, const uint32_t size)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE(!stuffer->tainted, S2N_ERR_RESIZE_TAINTED_STUFFER);
    POSIX_ENSURE(stuffer->growable, S2N_ERR_RESIZE_STATIC_STUFFER);

    if (size == stuffer->blob.size) {
        return S2N_SUCCESS;
    }

    if (size == 0) {
        s2n_stuffer_wipe(stuffer);
        return s2n_free(&stuffer->blob);
    }

    if (size < stuffer->blob.size) {
        POSIX_CHECKED_MEMSET(stuffer->blob.data + size, S2N_WIPE_PATTERN, (stuffer->blob.size - size));
        if (stuffer->read_cursor > size) {
            stuffer->read_cursor = size;
        }
        if (stuffer->write_cursor > size) {
            stuffer->write_cursor = size;
        }
        if (stuffer->high_water_mark > size) {
            stuffer->high_water_mark = size;
        }
        stuffer->blob.size = size;
        POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
        return S2N_SUCCESS;
    }

    POSIX_GUARD(s2n_realloc(&stuffer->blob, size));
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_resize_if_empty(struct s2n_stuffer *stuffer, const uint32_t size)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    if (stuffer->blob.data == NULL) {
        POSIX_ENSURE(!stuffer->tainted, S2N_ERR_RESIZE_TAINTED_STUFFER);
        POSIX_ENSURE(stuffer->growable, S2N_ERR_RESIZE_STATIC_STUFFER);
        POSIX_GUARD(s2n_realloc(&stuffer->blob, size));
    }
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_rewrite(struct s2n_stuffer *stuffer)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    stuffer->write_cursor = 0;
    stuffer->read_cursor = 0;
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_rewind_read(struct s2n_stuffer *stuffer, const uint32_t size)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE(stuffer->read_cursor >= size, S2N_ERR_STUFFER_OUT_OF_DATA);
    stuffer->read_cursor -= size;
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_reread(struct s2n_stuffer *stuffer)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    stuffer->read_cursor = 0;
    return S2N_SUCCESS;
}

int s2n_stuffer_wipe_n(struct s2n_stuffer *stuffer, const uint32_t size)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    uint32_t wipe_size = MIN(size, stuffer->write_cursor);

    stuffer->write_cursor -= wipe_size;
    stuffer->read_cursor = MIN(stuffer->read_cursor, stuffer->write_cursor);
    POSIX_CHECKED_MEMSET(stuffer->blob.data + stuffer->write_cursor, S2N_WIPE_PATTERN, wipe_size);

    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

bool s2n_stuffer_is_consumed(struct s2n_stuffer *stuffer)
{
    return stuffer && (stuffer->read_cursor == stuffer->write_cursor) && !stuffer->tainted;
}

int s2n_stuffer_wipe(struct s2n_stuffer *stuffer)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    if (!s2n_stuffer_is_wiped(stuffer)) {
        POSIX_CHECKED_MEMSET(stuffer->blob.data, S2N_WIPE_PATTERN, stuffer->high_water_mark);
    }

    stuffer->tainted = 0;
    stuffer->write_cursor = 0;
    stuffer->read_cursor = 0;
    stuffer->high_water_mark = 0;
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_skip_read(struct s2n_stuffer *stuffer, uint32_t n)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE(s2n_stuffer_data_available(stuffer) >= n, S2N_ERR_STUFFER_OUT_OF_DATA);

    stuffer->read_cursor += n;
    return S2N_SUCCESS;
}

void *s2n_stuffer_raw_read(struct s2n_stuffer *stuffer, uint32_t data_len)
{
    PTR_GUARD_POSIX(s2n_stuffer_skip_read(stuffer, data_len));

    stuffer->tainted = 1;

    return (stuffer->blob.data) ? (stuffer->blob.data + stuffer->read_cursor - data_len) : NULL;
}

int s2n_stuffer_read(struct s2n_stuffer *stuffer, struct s2n_blob *out)
{
    POSIX_ENSURE_REF(out);

    return s2n_stuffer_read_bytes(stuffer, out->data, out->size);
}

int s2n_stuffer_erase_and_read(struct s2n_stuffer *stuffer, struct s2n_blob *out)
{
    POSIX_GUARD(s2n_stuffer_skip_read(stuffer, out->size));

    void *ptr = (stuffer->blob.data) ? (stuffer->blob.data + stuffer->read_cursor - out->size) : NULL;
    POSIX_ENSURE(S2N_MEM_IS_READABLE(ptr, out->size), S2N_ERR_NULL);

    POSIX_CHECKED_MEMCPY(out->data, ptr, out->size);
    POSIX_CHECKED_MEMSET(ptr, 0, out->size);

    return S2N_SUCCESS;
}

int s2n_stuffer_read_bytes(struct s2n_stuffer *stuffer, uint8_t *data, uint32_t size)
{
    POSIX_ENSURE_REF(data);
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_GUARD(s2n_stuffer_skip_read(stuffer, size));
    POSIX_ENSURE_REF(stuffer->blob.data);
    void *ptr = stuffer->blob.data + stuffer->read_cursor - size;

    POSIX_CHECKED_MEMCPY(data, ptr, size);

    return S2N_SUCCESS;
}

int s2n_stuffer_erase_and_read_bytes(struct s2n_stuffer *stuffer, uint8_t *data, uint32_t size)
{
    POSIX_GUARD(s2n_stuffer_skip_read(stuffer, size));
    POSIX_ENSURE_REF(stuffer->blob.data);
    void *ptr = stuffer->blob.data + stuffer->read_cursor - size;

    POSIX_CHECKED_MEMCPY(data, ptr, size);
    POSIX_CHECKED_MEMSET(ptr, 0, size);

    return S2N_SUCCESS;
}

int s2n_stuffer_skip_write(struct s2n_stuffer *stuffer, const uint32_t n)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_GUARD(s2n_stuffer_reserve_space(stuffer, n));
    stuffer->write_cursor += n;
    stuffer->high_water_mark = MAX(stuffer->write_cursor, stuffer->high_water_mark);
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

void *s2n_stuffer_raw_write(struct s2n_stuffer *stuffer, const uint32_t data_len)
{
    PTR_GUARD_POSIX(s2n_stuffer_skip_write(stuffer, data_len));

    stuffer->tainted = 1;

    return (stuffer->blob.data) ? (stuffer->blob.data + stuffer->write_cursor - data_len) : NULL;
}

int s2n_stuffer_write(struct s2n_stuffer *stuffer, const struct s2n_blob *in)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_PRECONDITION(s2n_blob_validate(in));
    return s2n_stuffer_write_bytes(stuffer, in->data, in->size);
}

int s2n_stuffer_write_bytes(struct s2n_stuffer *stuffer, const uint8_t *data, const uint32_t size)
{
    if (size == 0) {
        return S2N_SUCCESS;
    }

    POSIX_ENSURE(S2N_MEM_IS_READABLE(data, size), S2N_ERR_SAFETY);
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_GUARD(s2n_stuffer_skip_write(stuffer, size));

    void *ptr = stuffer->blob.data + stuffer->write_cursor - size;
    POSIX_ENSURE(S2N_MEM_IS_READABLE(ptr, size), S2N_ERR_NULL);

    if (ptr == data) {
        POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
        return S2N_SUCCESS;
    }

    POSIX_CHECKED_MEMCPY(ptr, data, size);

    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_writev_bytes(struct s2n_stuffer *stuffer, const struct iovec *iov, size_t iov_count, uint32_t offs,
        uint32_t size)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE_REF(iov);
    void *ptr = s2n_stuffer_raw_write(stuffer, size);
    POSIX_ENSURE(S2N_MEM_IS_READABLE(ptr, size), S2N_ERR_NULL);

    size_t size_left = size, to_skip = offs;
    for (size_t i = 0; i < iov_count; i++) {
        if (to_skip >= iov[i].iov_len) {
            to_skip -= iov[i].iov_len;
            continue;
        }
        size_t iov_len_op = iov[i].iov_len - to_skip;
        POSIX_ENSURE_LTE(iov_len_op, UINT32_MAX);
        uint32_t iov_len = (uint32_t) iov_len_op;
        uint32_t iov_size_to_take = MIN(size_left, iov_len);
        POSIX_ENSURE_REF(iov[i].iov_base);
        POSIX_ENSURE_LT(to_skip, iov[i].iov_len);
        POSIX_CHECKED_MEMCPY(ptr, ((uint8_t *) (iov[i].iov_base)) + to_skip, iov_size_to_take);
        size_left -= iov_size_to_take;
        if (size_left == 0) {
            break;
        }
        ptr = (void *) ((uint8_t *) ptr + iov_size_to_take);
        to_skip = 0;
    }

    return S2N_SUCCESS;
}

static int s2n_stuffer_copy_impl(struct s2n_stuffer *from, struct s2n_stuffer *to, const uint32_t len)
{
    POSIX_GUARD(s2n_stuffer_skip_read(from, len));
    POSIX_GUARD(s2n_stuffer_skip_write(to, len));

    uint8_t *from_ptr = (from->blob.data) ? (from->blob.data + from->read_cursor - len) : NULL;
    uint8_t *to_ptr = (to->blob.data) ? (to->blob.data + to->write_cursor - len) : NULL;

    POSIX_CHECKED_MEMCPY(to_ptr, from_ptr, len);

    return S2N_SUCCESS;
}

int s2n_stuffer_reserve_space(struct s2n_stuffer *stuffer, uint32_t n)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    if (s2n_stuffer_space_remaining(stuffer) < n) {
        POSIX_ENSURE(stuffer->growable, S2N_ERR_STUFFER_IS_FULL);
        /* Always grow a stuffer by at least 1k */
        const uint32_t growth = MAX(n - s2n_stuffer_space_remaining(stuffer), S2N_MIN_STUFFER_GROWTH_IN_BYTES);
        uint32_t new_size = 0;
        POSIX_GUARD(s2n_add_overflow(stuffer->blob.size, growth, &new_size));
        POSIX_GUARD(s2n_stuffer_resize(stuffer, new_size));
    }
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

/* Copies "len" bytes from "from" to "to".
 * If the copy cannot succeed (i.e. there are either not enough bytes available, or there is not enough space to write them
 * restore the old value of the stuffer */
int s2n_stuffer_copy(struct s2n_stuffer *from, struct s2n_stuffer *to, const uint32_t len)
{
    const uint32_t orig_read_cursor = from->read_cursor;
    const uint32_t orig_write_cursor = to->write_cursor;

    if (s2n_stuffer_copy_impl(from, to, len) < 0) {
        from->read_cursor = orig_read_cursor;
        to->write_cursor = orig_write_cursor;
        S2N_ERROR_PRESERVE_ERRNO();
    }

    return S2N_SUCCESS;
}

int s2n_stuffer_extract_blob(struct s2n_stuffer *stuffer, struct s2n_blob *out)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE_REF(out);
    POSIX_GUARD(s2n_realloc(out, s2n_stuffer_data_available(stuffer)));

    if (s2n_stuffer_data_available(stuffer) > 0) {
        POSIX_CHECKED_MEMCPY(out->data, stuffer->blob.data + stuffer->read_cursor, s2n_stuffer_data_available(stuffer));
    }

    POSIX_POSTCONDITION(s2n_blob_validate(out));
    return S2N_SUCCESS;
}

int s2n_stuffer_shift(struct s2n_stuffer *stuffer)
{
    POSIX_ENSURE_REF(stuffer);
    struct s2n_stuffer copy = *stuffer;
    POSIX_GUARD(s2n_stuffer_rewrite(&copy));

    uint8_t *data = stuffer->blob.data;
    /* Adding 0 to a NULL value is undefined behavior */
    if (stuffer->read_cursor != 0) {
        data += stuffer->read_cursor;
    }

    uint32_t data_size = s2n_stuffer_data_available(stuffer);
    POSIX_GUARD(s2n_stuffer_write_bytes(&copy, data, data_size));
    *stuffer = copy;
    return S2N_SUCCESS;
}
