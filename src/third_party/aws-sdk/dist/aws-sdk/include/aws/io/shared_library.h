#ifndef AWS_COMMON_SHARED_LIBRARY_H
#define AWS_COMMON_SHARED_LIBRARY_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_byte_cursor;

/*
 * A simple platform wrapper for dynamically loading and examining shared libraries
 */
struct aws_shared_library {
    void *library_handle;
};

typedef void (*aws_generic_function)(void);

AWS_EXTERN_C_BEGIN

/*
 * Initializes a dynamically-loaded shared library from its file path location
 */
AWS_IO_API
int aws_shared_library_init(struct aws_shared_library *library, const char *library_path);

/*
 * Closes a dynamically-loaded shared library
 */
AWS_IO_API
void aws_shared_library_clean_up(struct aws_shared_library *library);

/*
 * Finds a function symbol within a shared library.  function_address may be
 * safely cast into any other function type as appropriate.
 */
AWS_IO_API
int aws_shared_library_find_function(
    struct aws_shared_library *library,
    const char *symbol_name,
    aws_generic_function *function_address);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_SHARED_LIBRARY_H */
