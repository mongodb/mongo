#ifndef AWS_IO_PKCS11_H
#define AWS_IO_PKCS11_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_allocator;

/**
 * Handle to a loaded PKCS#11 library.
 */
struct aws_pkcs11_lib;

/**
 * Controls how aws_pkcs11_lib calls C_Initialize() and C_Finalize() on the PKCS#11 library.
 */
enum aws_pkcs11_lib_behavior {
    /**
     * Default behavior that accommodates most use cases.
     * C_Initialize() is called on creation, and "already-initialized" errors are ignored.
     * C_Finalize() is never called, just in case another part of your
     * application is still using the PKCS#11 library.
     */
    AWS_PKCS11_LIB_DEFAULT_BEHAVIOR,

    /**
     * Skip calling C_Initialize() and C_Finalize().
     * Use this if your application has already initialized the PKCS#11 library,
     * and you do not want C_Initialize() called again.
     */
    AWS_PKCS11_LIB_OMIT_INITIALIZE,

    /**
     * C_Initialize() is called on creation and C_Finalize() is called on cleanup.
     * If C_Initialize() reports that's it's already initialized, this is treated as an error.
     * Use this if you need perfect cleanup (ex: running valgrind with --leak-check).
     */
    AWS_PKCS11_LIB_STRICT_INITIALIZE_FINALIZE,
};

/* The enum above was misspelled, and later got fixed (pcks11 -> pkcs11).
 * This macro maintain backwards compatibility with the old spelling */
#define aws_pcks11_lib_behavior aws_pkcs11_lib_behavior

/**
 * Options for aws_pkcs11_lib_new()
 */
struct aws_pkcs11_lib_options {
    /**
     * Name of PKCS#11 library file to load (UTF-8).
     * Zero out if your application is compiled with PKCS#11 symbols linked in.
     */
    struct aws_byte_cursor filename;

    /**
     * Behavior for calling C_Initialize() and C_Finalize() on the PKCS#11 library.
     */
    enum aws_pkcs11_lib_behavior initialize_finalize_behavior;
};

AWS_EXTERN_C_BEGIN

/**
 * Load and initialize a PKCS#11 library.
 * See `aws_pkcs11_lib_options` for options.
 *
 * If successful a valid pointer is returned. You must call aws_pkcs11_lib_release() when you are done with it.
 * If unsuccessful, NULL is returned and an error is set.
 */
AWS_IO_API
struct aws_pkcs11_lib *aws_pkcs11_lib_new(
    struct aws_allocator *allocator,
    const struct aws_pkcs11_lib_options *options);

/**
 * Acquire a reference to a PKCS#11 library, preventing it from being cleaned up.
 * You must call aws_pkcs11_lib_release() when you are done with it.
 * This function returns whatever was passed in. It cannot fail.
 */
AWS_IO_API
struct aws_pkcs11_lib *aws_pkcs11_lib_acquire(struct aws_pkcs11_lib *pkcs11_lib);

/**
 * Release a reference to the PKCS#11 library.
 * When the last reference is released, the library is cleaned up.
 */
AWS_IO_API
void aws_pkcs11_lib_release(struct aws_pkcs11_lib *pkcs11_lib);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_PKCS11_H */
