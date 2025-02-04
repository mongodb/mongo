/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/shared_library.h>

#include <aws/io/logging.h>

#include <dlfcn.h>

static const char *s_null = "<NULL>";
static const char *s_unknown_error = "<Unknown>";

int aws_shared_library_init(struct aws_shared_library *library, const char *library_path) {
    AWS_ZERO_STRUCT(*library);

    library->library_handle = dlopen(library_path, RTLD_LAZY);
    if (library->library_handle == NULL) {
        const char *error = dlerror();
        AWS_LOGF_ERROR(
            AWS_LS_IO_SHARED_LIBRARY,
            "id=%p: Failed to load shared library at path \"%s\" with error: %s",
            (void *)library,
            library_path ? library_path : s_null,
            error ? error : s_unknown_error);
        return aws_raise_error(AWS_IO_SHARED_LIBRARY_LOAD_FAILURE);
    }

    return AWS_OP_SUCCESS;
}

void aws_shared_library_clean_up(struct aws_shared_library *library) {
    if (library && library->library_handle) {
        dlclose(library->library_handle);
        library->library_handle = NULL;
    }
}

int aws_shared_library_find_function(
    struct aws_shared_library *library,
    const char *symbol_name,
    aws_generic_function *function_address) {
    if (library == NULL || library->library_handle == NULL) {
        return aws_raise_error(AWS_IO_SHARED_LIBRARY_FIND_SYMBOL_FAILURE);
    }

    /*
     * Suggested work around for (undefined behavior) cast from void * to function pointer
     * in POSIX.1-2003 standard, at least according to dlsym man page code sample.
     */
    *(void **)(function_address) = dlsym(library->library_handle, symbol_name);

    if (*function_address == NULL) {
        const char *error = dlerror();
        AWS_LOGF_ERROR(
            AWS_LS_IO_SHARED_LIBRARY,
            "id=%p: Failed to find shared library symbol \"%s\" with error: %s",
            (void *)library,
            symbol_name ? symbol_name : s_null,
            error ? error : s_unknown_error);
        return aws_raise_error(AWS_IO_SHARED_LIBRARY_FIND_SYMBOL_FAILURE);
    }

    return AWS_OP_SUCCESS;
}
