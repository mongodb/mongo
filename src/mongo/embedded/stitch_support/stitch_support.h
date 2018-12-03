
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#ifndef HEADERUUID_645147E3_14CB_4DFC_8F05_16CE4D190C18_DEFINED
#define HEADERUUID_645147E3_14CB_4DFC_8F05_16CE4D190C18_DEFINED

#include <stddef.h>
#include <stdint.h>

#pragma push_macro("MONGO_API_CALL")
#undef MONGO_API_CALL

#pragma push_macro("MONGO_API_IMPORT")
#undef MONGO_API_IMPORT

#pragma push_macro("MONGO_API_EXPORT")
#undef MONGO_API_EXPORT

#pragma push_macro("STITCH_SUPPORT_API")
#undef STITCH_SUPPORT_API

#if defined(_WIN32)
#define MONGO_API_CALL __cdecl
#define MONGO_API_IMPORT __declspec(dllimport)
#define MONGO_API_EXPORT __declspec(dllexport)
#else
#define MONGO_API_CALL
#define MONGO_API_IMPORT __attribute__((visibility("default")))
#define MONGO_API_EXPORT __attribute__((used, visibility("default")))
#endif

#if defined(STITCH_SUPPORT_STATIC)
#define STITCH_SUPPORT_API
#else
#if defined(STITCH_SUPPORT_COMPILING)
#define STITCH_SUPPORT_API MONGO_API_EXPORT
#else
#define STITCH_SUPPORT_API MONGO_API_IMPORT
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * An object which describes the details of the failure of an operation.
 *
 * The Stitch Support Library uses allocated objects of this type to report the details of any
 * failure, when an operation cannot be completed. Several `stitch_support_v1_status` functions are
 * provided which permit observing the details of these failures. Further a construction function
 * and a destruction function for these objects are also provided.
 *
 * The use of status objects from multiple threads is not thread safe unless all of the threads
 * accessing a single status object are passing that object as a const-qualified (const
 * stitch_support_v1_status *) pointer. If a single thread is passing a status object to a function
 * taking it by non-const-qualified (stitch_support_v1_status*) pointer, then no other thread may
 * access the status object.
 *
 * The `status` parameter is optional for all `stitch_support_v1_` functions that can take a status
 * pointer. The caller may pass NULL instead of a valid `status` object, in which case the function
 * will execute normally but will not provide any detailed error information in the caes of a
 * failure.
 *
 * All `stitch_support_v1_status` functions can be used before the Stitch Support library is
 * initialized. This facilitates detailed error reporting from all library functions.
 */
typedef struct stitch_support_v1_status stitch_support_v1_status;

/**
 * Allocate and construct an API-return-status buffer object.
 *
 * Returns NULL when construction of a stitch_support_v1_status object fails.
 *
 * This function may be called before stitch_support_v1_lib_init().
 */
STITCH_SUPPORT_API stitch_support_v1_status* MONGO_API_CALL stitch_support_v1_status_create(void);

/**
 * Destroys a valid status object.
 *
 * The status object must not be NULL and must be a valid stitch_support_v1_status object.
 *
 * This function is not thread safe, and it must not execute concurrently with any other function
 * that accesses the status object being destroyed. It is, however, safe to destroy distinct status
 * objects on distinct threads.
 *
 * This function does not report failures.
 *
 * This function may be called before `stitch_support_v1_lib_init`.
 *
 * This function causes all storage associated with the specified status object to be released,
 * including the storage referenced by functions that returned observable storage buffers from this
 * status, such as strings.
 */
STITCH_SUPPORT_API void MONGO_API_CALL
stitch_support_v1_status_destroy(stitch_support_v1_status* status);

/**
 * The error codes reported by `stitch_support_v1` functions will be given the symbolic names as
 * mapped by this enum.
 *
 * When a `stitch_support_v1` function fails (and it has been documented to report errors) it will
 * report that error in the form of an `int` status code. That status code will always be returned
 * as the type `int`; however, the values in this enum can be used to classify the failure.
 */
typedef enum {
    STITCH_SUPPORT_V1_ERROR_IN_REPORTING_ERROR = -2,
    STITCH_SUPPORT_V1_ERROR_UNKNOWN = -1,

    STITCH_SUPPORT_V1_SUCCESS = 0,

    STITCH_SUPPORT_V1_ERROR_ENOMEM = 1,
    STITCH_SUPPORT_V1_ERROR_EXCEPTION = 2,
    STITCH_SUPPORT_V1_ERROR_LIBRARY_ALREADY_INITIALIZED = 3,
    STITCH_SUPPORT_V1_ERROR_LIBRARY_NOT_INITIALIZED = 4,
    STITCH_SUPPORT_V1_ERROR_INVALID_LIB_HANDLE = 5,
} stitch_support_v1_error;

/**
 * Gets an error code from a `stitch_support_v1_status` object.
 *
 * When a `stitch_support_v1` function fails (and it has been documented to report errors) it will
 * report its error in the form of an `int` status code which is stored into a supplied
 * `stitch_support_v1_status` object, if provided. Some of these functions may also report extra
 * information, which will be reported by other observer functions. Every `stitch_support_v1`
 * function which reports errors will always update the `Error` code stored in a
 * `stitch_support_v1_status` object, even upon success.
 *
 * This function does not report its own failures.
 */
STITCH_SUPPORT_API int MONGO_API_CALL
stitch_support_v1_status_get_error(const stitch_support_v1_status* status);

/**
 * Gets a descriptive error message from a `stitch_support_v1_status` object.
 *
 * For failures where the error is STITCH_SUPPORT_V1_ERROR_EXCEPTION, this returns a string
 * representation of the internal C++ exception.
 *
 * The function to which the specified status object was passed must not have returned
 * STITCH_SUPPORT_V1_SUCCESS as its error code.
 *
 * The storage for the returned string is associated with the specified status object, and therefore
 * it will be deallocated when the status is destroyed using stitch_support_v1_destroy_status().
 *
 * This function does not report its own failures.
 */
STITCH_SUPPORT_API const char* MONGO_API_CALL
stitch_support_v1_status_get_explanation(const stitch_support_v1_status* status);

/**
 * Gets a status code from a `stitch_support_v1_status` object.
 *
 * Returns a numeric status code associated with the status parameter which indicates a sub-category
 * of failure.
 *
 * For any status object that does not have STITCH_SUPPORT_V1_ERROR_EXCEPTION as its error, the
 * value of this code is unspecified.
 *
 * This function does not report its own failures.
 */
STITCH_SUPPORT_API int MONGO_API_CALL
stitch_support_v1_status_get_code(const stitch_support_v1_status* status);

/**
 * An object which describes the runtime state of the Stitch Support Library.
 *
 * The Stitch Support Library uses allocated objects of this type to indicate the present state of
 * the library. Some operations which the library provides need access to this object. Further a
 * construction function and a destruction function for these objects are also provided. No more
 * than a single object instance of this type may exist at any given time.
 *
 * The use of `stitch_support_v1_lib` objects from multiple threads is not thread safe unless all of
 * the threads accessing a single `stitch_support_v1_lib` object are not destroying this object.  If
 * a single thread is passing a `stitch_support_v1_lib` to its destruction function, then no other
 * thread may access the `stitch_support_v1_lib` object.
 */
typedef struct stitch_support_v1_lib stitch_support_v1_lib;

/**
 * Creates a stitch_support_v1_lib object, which stores context for the Stitch Support library. A
 * process should only ever have one stitch_support_v1_lib instance.
 *
 * On failure, returns NULL and populates the 'status' object if it is not NULL.
 */
STITCH_SUPPORT_API stitch_support_v1_lib* MONGO_API_CALL
stitch_support_v1_init(stitch_support_v1_status* status);

/**
 * Tears down the state of this library. Existing stitch_support_v1_status objects remain valid.
 *
 * The 'lib' parameter must be a valid stitch_support_v1_lib instance and must not be NULL.
 *
 * The 'status' parameter may be NULL, but must be a valid stitch_support_v1_status instance if it
 * is not NULL.
 *
 * Returns STITCH_SUPPORT_V1_SUCCESS on success.
 *
 * Returns STITCH_SUPPORT_V1_ERROR_LIBRARY_NOT_INITIALIZED and modifies 'status' if
 * mongo_embedded_v1_lib_init() has not been called previously.
 */
STITCH_SUPPORT_API int MONGO_API_CALL
stitch_support_v1_fini(stitch_support_v1_lib* const lib, stitch_support_v1_status* const status);

#ifdef __cplusplus
}  // extern "C"
#endif

#undef STITCH_SUPPORT_API
#pragma pop_macro("STITCH_SUPPORT_API")

#undef MONGO_API_EXPORT
#pragma push_macro("MONGO_API_EXPORT")

#undef MONGO_API_IMPORT
#pragma push_macro("MONGO_API_IMPORT")

#undef MONGO_API_CALL
#pragma pop_macro("MONGO_API_CALL")

#endif  // HEADERUUID_645147E3_14CB_4DFC_8F05_16CE4D190C18_DEFINED
