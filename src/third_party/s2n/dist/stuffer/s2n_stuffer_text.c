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

#include <string.h>
#include <sys/param.h>

#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

int s2n_stuffer_peek_char(struct s2n_stuffer *s2n_stuffer, char *c)
{
    int r = s2n_stuffer_read_uint8(s2n_stuffer, (uint8_t *) c);
    if (r == S2N_SUCCESS) {
        s2n_stuffer->read_cursor--;
    }
    POSIX_POSTCONDITION(s2n_stuffer_validate(s2n_stuffer));
    return r;
}

/* Peeks in stuffer to see if expected string is present. */
int s2n_stuffer_peek_check_for_str(struct s2n_stuffer *s2n_stuffer, const char *expected)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(s2n_stuffer));
    uint32_t orig_read_pos = s2n_stuffer->read_cursor;
    int rc = s2n_stuffer_read_expected_str(s2n_stuffer, expected);
    s2n_stuffer->read_cursor = orig_read_pos;
    POSIX_POSTCONDITION(s2n_stuffer_validate(s2n_stuffer));
    return rc;
}

int s2n_stuffer_skip_whitespace(struct s2n_stuffer *s2n_stuffer, uint32_t *skipped)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(s2n_stuffer));
    uint32_t initial_read_cursor = s2n_stuffer->read_cursor;
    while (s2n_stuffer_data_available(s2n_stuffer)) {
        uint8_t c = s2n_stuffer->blob.data[s2n_stuffer->read_cursor];
        /* We don't use isspace, because it changes under locales. */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            s2n_stuffer->read_cursor += 1;
        } else {
            break;
        }
    }
    if (skipped != NULL) {
        *skipped = s2n_stuffer->read_cursor - initial_read_cursor;
    }
    POSIX_POSTCONDITION(s2n_stuffer_validate(s2n_stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_read_expected_str(struct s2n_stuffer *stuffer, const char *expected)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE_REF(expected);
    size_t expected_length = strlen(expected);
    if (expected_length == 0) {
        return S2N_SUCCESS;
    }
    POSIX_ENSURE(s2n_stuffer_data_available(stuffer) >= expected_length, S2N_ERR_STUFFER_OUT_OF_DATA);
    uint8_t *actual = stuffer->blob.data + stuffer->read_cursor;
    POSIX_ENSURE_REF(actual);
    POSIX_ENSURE(!memcmp(actual, expected, expected_length), S2N_ERR_STUFFER_NOT_FOUND);
    stuffer->read_cursor += expected_length;
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

/* Read from stuffer until the target string is found, or until there is no more data. */
int s2n_stuffer_skip_read_until(struct s2n_stuffer *stuffer, const char *target)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE_REF(target);
    const uint32_t len = strlen(target);
    if (len == 0) {
        return S2N_SUCCESS;
    }
    while (s2n_stuffer_data_available(stuffer) >= len) {
        POSIX_GUARD(s2n_stuffer_skip_to_char(stuffer, target[0]));
        POSIX_GUARD(s2n_stuffer_skip_read(stuffer, len));
        uint8_t *actual = stuffer->blob.data + stuffer->read_cursor - len;
        POSIX_ENSURE_REF(actual);

        if (strncmp((char *) actual, target, len) == 0) {
            return S2N_SUCCESS;
        } else {
            /* If string doesn't match, rewind stuffer to 1 byte after last read */
            POSIX_GUARD(s2n_stuffer_rewind_read(stuffer, len - 1));
            continue;
        }
    }
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

/* Skips the stuffer until the first instance of the target character or until there is no more data. */
int s2n_stuffer_skip_to_char(struct s2n_stuffer *stuffer, const char target)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    while (s2n_stuffer_data_available(stuffer) > 0) {
        if (stuffer->blob.data[stuffer->read_cursor] == target) {
            break;
        }
        stuffer->read_cursor += 1;
    }
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

/* Skips an expected character in the stuffer between min and max times */
int s2n_stuffer_skip_expected_char(struct s2n_stuffer *stuffer, const char expected, const uint32_t min,
        const uint32_t max, uint32_t *skipped)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE(min <= max, S2N_ERR_SAFETY);

    uint32_t skip = 0;
    while (stuffer->read_cursor < stuffer->write_cursor && skip < max) {
        if (stuffer->blob.data[stuffer->read_cursor] == expected) {
            stuffer->read_cursor += 1;
            skip += 1;
        } else {
            break;
        }
    }
    POSIX_ENSURE(skip >= min, S2N_ERR_STUFFER_NOT_FOUND);
    if (skipped != NULL) {
        *skipped = skip;
    }
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

/* Read a line of text. Agnostic to LF or CR+LF line endings. */
int s2n_stuffer_read_line(struct s2n_stuffer *stuffer, struct s2n_stuffer *token)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_PRECONDITION(s2n_stuffer_validate(token));
    /* Consume an LF terminated line */
    POSIX_GUARD(s2n_stuffer_read_token(stuffer, token, '\n'));

    /* Snip off the carriage return if it's present */
    if ((s2n_stuffer_data_available(token) > 0) && (token->blob.data[(token->write_cursor - 1)] == '\r')) {
        token->write_cursor--;
    }
    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    POSIX_POSTCONDITION(s2n_stuffer_validate(token));
    return S2N_SUCCESS;
}

int s2n_stuffer_read_token(struct s2n_stuffer *stuffer, struct s2n_stuffer *token, char delim)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_PRECONDITION(s2n_stuffer_validate(token));
    uint32_t token_size = 0;

    while ((stuffer->read_cursor + token_size) < stuffer->write_cursor) {
        if (stuffer->blob.data[stuffer->read_cursor + token_size] == delim) {
            break;
        }
        token_size++;
    }

    POSIX_GUARD(s2n_stuffer_copy(stuffer, token, token_size));

    /* Consume the delimiter too */
    if (stuffer->read_cursor < stuffer->write_cursor) {
        stuffer->read_cursor++;
    }

    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    POSIX_POSTCONDITION(s2n_stuffer_validate(token));
    return S2N_SUCCESS;
}

int s2n_stuffer_alloc_ro_from_string(struct s2n_stuffer *stuffer, const char *str)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE_REF(str);
    uint32_t length = strlen(str);
    POSIX_GUARD(s2n_stuffer_alloc(stuffer, length + 1));
    return s2n_stuffer_write_bytes(stuffer, (const uint8_t *) str, length);
}

int s2n_stuffer_init_ro_from_string(struct s2n_stuffer *stuffer, uint8_t *data, uint32_t length)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE_REF(data);

    struct s2n_blob data_blob = { 0 };
    POSIX_GUARD(s2n_blob_init(&data_blob, data, length));

    POSIX_GUARD(s2n_stuffer_init(stuffer, &data_blob));
    POSIX_GUARD(s2n_stuffer_skip_write(stuffer, length));

    return S2N_SUCCESS;
}

/* If we call va_start or va_copy there MUST be a matching call to va_end,
 * so we should use DEFER_CLEANUP with our va_lists.
 * Unfortunately, some environments implement va_list in ways that don't
 * act as expected when passed by reference. For example, because va_end is
 * a macro it may expect va_list to be an array (maybe to call sizeof),
 * but passing va_list by reference will cause it to decay to a pointer instead.
 * To avoid any surprises, just wrap the va_list in our own struct.
 */
struct s2n_va_list {
    va_list va_list;
};

static void s2n_va_list_cleanup(struct s2n_va_list *list)
{
    if (list) {
        va_end(list->va_list);
    }
}

int s2n_stuffer_vprintf(struct s2n_stuffer *stuffer, const char *format, va_list vargs_in)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));
    POSIX_ENSURE_REF(format);

    /* vsnprintf consumes the va_list, so copy it first */
    DEFER_CLEANUP(struct s2n_va_list vargs_1 = { 0 }, s2n_va_list_cleanup);
    va_copy(vargs_1.va_list, vargs_in);

    /* The first call to vsnprintf calculates the size of the formatted string.
     * str_len does not include the one byte vsnprintf requires for a trailing '\0',
     * so we need one more byte.
     */
    int str_len = vsnprintf(NULL, 0, format, vargs_1.va_list);
    POSIX_ENSURE_GTE(str_len, 0);
    POSIX_ENSURE_LT(str_len, INT_MAX);
    int mem_size = str_len + 1;

    /* 'tainted' indicates that pointers to the contents of the stuffer exist,
     * so resizing / reallocated the stuffer will invalidate those pointers.
     * However, we do not resize the stuffer in this method after creating `str`
     * and `str` does not live beyond this method, so ignore `str` for the
     * purposes of tracking 'tainted'.
     */
    bool previously_tainted = stuffer->tainted;
    char *str = s2n_stuffer_raw_write(stuffer, mem_size);
    stuffer->tainted = previously_tainted;
    POSIX_GUARD_PTR(str);

    /* vsnprintf again consumes the va_list, so copy it first */
    DEFER_CLEANUP(struct s2n_va_list vargs_2 = { 0 }, s2n_va_list_cleanup);
    va_copy(vargs_2.va_list, vargs_in);

    /* This time, vsnprintf actually writes the formatted string */
    int written = vsnprintf(str, mem_size, format, vargs_2.va_list);
    if (written != str_len) {
        /* If the write fails, undo our raw write */
        POSIX_GUARD(s2n_stuffer_wipe_n(stuffer, mem_size));
        POSIX_BAIL(S2N_ERR_SAFETY);
    }

    /* We don't actually use c-strings, so erase the final '\0' */
    POSIX_GUARD(s2n_stuffer_wipe_n(stuffer, 1));

    POSIX_POSTCONDITION(s2n_stuffer_validate(stuffer));
    return S2N_SUCCESS;
}

int s2n_stuffer_printf(struct s2n_stuffer *stuffer, const char *format, ...)
{
    DEFER_CLEANUP(struct s2n_va_list vargs = { 0 }, s2n_va_list_cleanup);
    va_start(vargs.va_list, format);
    POSIX_GUARD(s2n_stuffer_vprintf(stuffer, format, vargs.va_list));
    return S2N_SUCCESS;
}
