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

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_io.h"
#include "utils/s2n_safety.h"

int s2n_stuffer_recv_from_fd(struct s2n_stuffer *stuffer, const int rfd, const uint32_t len, uint32_t *bytes_written)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));

    /* Make sure we have enough space to write */
    POSIX_GUARD(s2n_stuffer_skip_write(stuffer, len));

    /* "undo" the skip write */
    stuffer->write_cursor -= len;

    ssize_t r = 0;
    POSIX_ENSURE(stuffer->blob.data, S2N_ERR_READ);
    S2N_IO_RETRY_EINTR(r, read(rfd, stuffer->blob.data + stuffer->write_cursor, len));
    POSIX_ENSURE(r >= 0, S2N_ERR_READ);

    /* Record just how many bytes we have written */
    POSIX_ENSURE((size_t) r <= UINT32_MAX, S2N_ERR_INTEGER_OVERFLOW);
    POSIX_GUARD(s2n_stuffer_skip_write(stuffer, (uint32_t) r));
    if (bytes_written != NULL) {
        *bytes_written = r;
    }
    return S2N_SUCCESS;
}

int s2n_stuffer_send_to_fd(struct s2n_stuffer *stuffer, const int wfd, const uint32_t len, uint32_t *bytes_sent)
{
    POSIX_PRECONDITION(s2n_stuffer_validate(stuffer));

    /* Make sure we even have the data */
    POSIX_GUARD(s2n_stuffer_skip_read(stuffer, len));

    /* "undo" the skip read */
    stuffer->read_cursor -= len;

    ssize_t w = 0;
    POSIX_ENSURE(stuffer->blob.data, S2N_ERR_WRITE);
    S2N_IO_RETRY_EINTR(w, write(wfd, stuffer->blob.data + stuffer->read_cursor, len));
    POSIX_ENSURE(w >= 0, S2N_ERR_WRITE);

    POSIX_ENSURE((size_t) w <= UINT32_MAX - stuffer->read_cursor, S2N_ERR_INTEGER_OVERFLOW);
    stuffer->read_cursor += w;
    if (bytes_sent != NULL) {
        *bytes_sent = w;
    }
    return S2N_SUCCESS;
}
