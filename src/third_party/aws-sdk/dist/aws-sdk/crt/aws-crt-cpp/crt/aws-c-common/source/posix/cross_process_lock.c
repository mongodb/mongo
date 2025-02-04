/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/cross_process_lock.h>

#include <aws/common/byte_buf.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aws/common/error.h>
#include <aws/common/file.h>
#include <aws/common/logging.h>

struct aws_cross_process_lock {
    struct aws_allocator *allocator;
    int locked_fd;
};

struct aws_cross_process_lock *aws_cross_process_lock_try_acquire(
    struct aws_allocator *allocator,
    struct aws_byte_cursor instance_nonce) {

    /* validate we don't have a directory slash. */
    struct aws_byte_cursor to_find = aws_byte_cursor_from_c_str("/");
    struct aws_byte_cursor found;
    AWS_ZERO_STRUCT(found);
    if (aws_byte_cursor_find_exact(&instance_nonce, &to_find, &found) != AWS_OP_ERR &&
        aws_last_error() != AWS_ERROR_STRING_MATCH_NOT_FOUND) {
        AWS_LOGF_ERROR(
            AWS_LS_COMMON_GENERAL,
            "static: Lock " PRInSTR "creation has illegal character /",
            AWS_BYTE_CURSOR_PRI(instance_nonce));
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    /*
     * The unix standard says /tmp has to be there and be writable. However, while it may be tempting to just use the
     * /tmp/ directory, it often has the sticky bit set which would prevent a subprocess from being able to call open
     * with create on the file. The solution is simple, just write it to a subdirectory inside
     * /tmp and override umask via. chmod of 0777.
     */
    struct aws_byte_cursor path_prefix = aws_byte_cursor_from_c_str("/tmp/aws_crt_cross_process_lock/");
    struct aws_string *path_to_create = aws_string_new_from_cursor(allocator, &path_prefix);

    /* It's probably there already and we don't care if it is. */
    if (!aws_directory_exists(path_to_create)) {
        /* if this call fails just let it fail on open below. */
        aws_directory_create(path_to_create);
        /* bypass umask by setting the perms we actually requested */
        chmod(aws_string_c_str(path_to_create), S_IRWXU | S_IRWXG | S_IRWXO);
    }
    aws_string_destroy(path_to_create);

    struct aws_byte_cursor path_suffix = aws_byte_cursor_from_c_str(".lock");

    struct aws_byte_buf nonce_buf;
    aws_byte_buf_init_copy_from_cursor(&nonce_buf, allocator, path_prefix);
    aws_byte_buf_append_dynamic(&nonce_buf, &instance_nonce);
    aws_byte_buf_append_dynamic(&nonce_buf, &path_suffix);
    aws_byte_buf_append_null_terminator(&nonce_buf);

    struct aws_cross_process_lock *instance_lock = NULL;

    errno = 0;
    int fd = open((const char *)nonce_buf.buffer, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        AWS_LOGF_DEBUG(
            AWS_LS_COMMON_GENERAL,
            "static: Lock file %s failed to open with errno %d",
            (const char *)nonce_buf.buffer,
            errno);

        aws_translate_and_raise_io_error_or(errno, AWS_ERROR_MUTEX_FAILED);

        if (aws_last_error() == AWS_ERROR_NO_PERMISSION) {
            AWS_LOGF_DEBUG(
                AWS_LS_COMMON_GENERAL,
                "static: Lock file %s couldn't be opened due to file ownership permissions. Attempting to open as read "
                "only",
                (const char *)nonce_buf.buffer);

            errno = 0;
            fd = open((const char *)nonce_buf.buffer, O_RDONLY);

            if (fd < 0) {
                AWS_LOGF_ERROR(
                    AWS_LS_COMMON_GENERAL,
                    "static: Lock file %s failed to open with read-only permissions with errno %d",
                    (const char *)nonce_buf.buffer,
                    errno);
                aws_translate_and_raise_io_error_or(errno, AWS_ERROR_MUTEX_FAILED);
                goto cleanup;
            }
        } else {
            AWS_LOGF_ERROR(
                AWS_LS_COMMON_GENERAL,
                "static: Lock file %s failed to open. The lock cannot be acquired.",
                (const char *)nonce_buf.buffer);
            goto cleanup;
        }
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        AWS_LOGF_TRACE(
            AWS_LS_COMMON_GENERAL,
            "static: Lock file %s already acquired by another instance",
            (const char *)nonce_buf.buffer);
        close(fd);
        aws_raise_error(AWS_ERROR_MUTEX_CALLER_NOT_OWNER);
        goto cleanup;
    }

    instance_lock = aws_mem_calloc(allocator, 1, sizeof(struct aws_cross_process_lock));
    instance_lock->locked_fd = fd;
    instance_lock->allocator = allocator;

    AWS_LOGF_TRACE(
        AWS_LS_COMMON_GENERAL,
        "static: Lock file %s acquired by this instance with fd %d",
        (const char *)nonce_buf.buffer,
        fd);

cleanup:
    aws_byte_buf_clean_up(&nonce_buf);

    return instance_lock;
}

void aws_cross_process_lock_release(struct aws_cross_process_lock *instance_lock) {
    if (instance_lock) {
        flock(instance_lock->locked_fd, LOCK_UN);
        close(instance_lock->locked_fd);
        AWS_LOGF_TRACE(AWS_LS_COMMON_GENERAL, "static: Lock file released for fd %d", instance_lock->locked_fd);
        aws_mem_release(instance_lock->allocator, instance_lock);
    }
}
