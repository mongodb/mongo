/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/file.h>
#include <aws/common/log_writer.h>

#include <aws/common/string.h>

#include <errno.h>
#include <stdio.h>

/*
 * Basic log writer implementations - stdout, stderr, arbitrary file
 */

struct aws_file_writer;

struct aws_file_writer {
    FILE *log_file;
    bool close_file_on_cleanup;
};

static int s_aws_file_writer_write(struct aws_log_writer *writer, const struct aws_string *output) {
    struct aws_file_writer *impl = (struct aws_file_writer *)writer->impl;

    size_t length = output->len;
    if (fwrite(output->bytes, 1, length, impl->log_file) < length) {
        int errno_value = ferror(impl->log_file) ? errno : 0; /* Always cache errno before potential side-effect */
        return aws_translate_and_raise_io_error_or(errno_value, AWS_ERROR_FILE_WRITE_FAILURE);
    }

    return AWS_OP_SUCCESS;
}

static void s_aws_file_writer_clean_up(struct aws_log_writer *writer) {
    struct aws_file_writer *impl = (struct aws_file_writer *)writer->impl;

    if (impl->close_file_on_cleanup) {
        fclose(impl->log_file);
    }

    aws_mem_release(writer->allocator, impl);
}

static struct aws_log_writer_vtable s_aws_file_writer_vtable = {
    .write = s_aws_file_writer_write,
    .clean_up = s_aws_file_writer_clean_up,
};

/*
 * Shared internal init implementation
 */
static int s_aws_file_writer_init_internal(
    struct aws_log_writer *writer,
    struct aws_allocator *allocator,
    const char *file_name_to_open,
    FILE *currently_open_file) {

    /* One or the other should be set */
    if (!((file_name_to_open != NULL) ^ (currently_open_file != NULL))) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /* Allocate and initialize the file writer */
    struct aws_file_writer *impl = aws_mem_calloc(allocator, 1, sizeof(struct aws_file_writer));
    if (impl == NULL) {
        return AWS_OP_ERR;
    }

    impl->log_file = NULL;
    impl->close_file_on_cleanup = false;

    /* Open file if name passed in */
    if (file_name_to_open != NULL) {
        impl->log_file = aws_fopen(file_name_to_open, "a+");
        if (impl->log_file == NULL) {
            aws_mem_release(allocator, impl);
            return AWS_OP_ERR;
        }
        impl->close_file_on_cleanup = true;
    } else {
        impl->log_file = currently_open_file;
    }

    writer->vtable = &s_aws_file_writer_vtable;
    writer->allocator = allocator;
    writer->impl = impl;

    return AWS_OP_SUCCESS;
}

/*
 * Public initialization interface
 */
int aws_log_writer_init_stdout(struct aws_log_writer *writer, struct aws_allocator *allocator) {
    return s_aws_file_writer_init_internal(writer, allocator, NULL, stdout);
}

int aws_log_writer_init_stderr(struct aws_log_writer *writer, struct aws_allocator *allocator) {
    return s_aws_file_writer_init_internal(writer, allocator, NULL, stderr);
}

int aws_log_writer_init_file(
    struct aws_log_writer *writer,
    struct aws_allocator *allocator,
    struct aws_log_writer_file_options *options) {
    return s_aws_file_writer_init_internal(writer, allocator, options->filename, options->file);
}

void aws_log_writer_clean_up(struct aws_log_writer *writer) {
    AWS_ASSERT(writer->vtable->clean_up);
    (writer->vtable->clean_up)(writer);
}
