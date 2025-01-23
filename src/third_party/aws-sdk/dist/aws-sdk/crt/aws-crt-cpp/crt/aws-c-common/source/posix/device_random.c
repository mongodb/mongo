/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/device_random.h>

#include <aws/common/byte_buf.h>
#include <aws/common/thread.h>

#include <fcntl.h>
#include <unistd.h>

static int s_rand_fd = -1;
static aws_thread_once s_rand_init = AWS_THREAD_ONCE_STATIC_INIT;

#ifdef O_CLOEXEC
#    define OPEN_FLAGS (O_RDONLY | O_CLOEXEC)
#else
#    define OPEN_FLAGS (O_RDONLY)
#endif
static void s_init_rand(void *user_data) {
    (void)user_data;
    s_rand_fd = open("/dev/urandom", OPEN_FLAGS);

    if (s_rand_fd == -1) {
        s_rand_fd = open("/dev/urandom", O_RDONLY);

        if (s_rand_fd == -1) {
            abort();
        }
    }

    if (-1 == fcntl(s_rand_fd, F_SETFD, FD_CLOEXEC)) {
        abort();
    }
}

int aws_device_random_buffer_append(struct aws_byte_buf *output, size_t n) {
    AWS_PRECONDITION(aws_byte_buf_is_valid(output));

    aws_thread_call_once(&s_rand_init, s_init_rand, NULL);

    size_t space_available = output->capacity - output->len;
    if (space_available < n) {
        AWS_POSTCONDITION(aws_byte_buf_is_valid(output));
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    size_t original_len = output->len;

    /* read() can fail if N is too large (e.g. x64 macos fails if N > INT32_MAX),
     * so work in reasonably sized chunks. */
    while (n > 0) {
        size_t capped_n = aws_min_size(
            n, 1024 * 1024 * 1024 * 1 /* 1GiB */); /* NOLINT(bugprone-implicit-widening-of-multiplication-result) */

        ssize_t amount_read = read(s_rand_fd, output->buffer + output->len, capped_n);

        if (amount_read <= 0) {
            output->len = original_len;
            AWS_POSTCONDITION(aws_byte_buf_is_valid(output));
            return aws_raise_error(AWS_ERROR_RANDOM_GEN_FAILED);
        }

        output->len += amount_read;
        n -= amount_read;
    }

    AWS_POSTCONDITION(aws_byte_buf_is_valid(output));
    return AWS_OP_SUCCESS;
}

int aws_device_random_buffer(struct aws_byte_buf *output) {
    return aws_device_random_buffer_append(output, output->capacity - output->len);
}
