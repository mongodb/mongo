/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 */

// clang-format off

#ifndef MONGO_CRYPT_SUPPORT_H
#define MONGO_CRYPT_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#pragma push_macro("MONGO_API_CALL")
#undef MONGO_API_CALL

#pragma push_macro("MONGO_API_IMPORT")
#undef MONGO_API_IMPORT

#pragma push_macro("MONGO_API_EXPORT")
#undef MONGO_API_EXPORT

#pragma push_macro("MONGO_CRYPT_SUPPORT_API")
#undef MONGO_CRYPT_SUPPORT_API

#if defined(_WIN32)
#define MONGO_API_CALL __cdecl
#define MONGO_API_IMPORT __declspec(dllimport)
#define MONGO_API_EXPORT __declspec(dllexport)
#else
#define MONGO_API_CALL
#define MONGO_API_IMPORT __attribute__((visibility("default")))
#define MONGO_API_EXPORT __attribute__((used, visibility("default")))
#endif

#if defined(MONGO_CRYPT_SUPPORT_STATIC)
#define MONGO_CRYPT_API
#else
#if defined(MONGO_CRYPT_SUPPORT_COMPILING)
#define MONGO_CRYPT_API MONGO_API_EXPORT
#else
#define MONGO_CRYPT_API MONGO_API_IMPORT
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * An object which describes the details of the failure of an operation.
 *
 * The Mongo Crypt Shared Library uses allocated objects of this type to report the details of any
 * failure, when an operation cannot be completed. Several `mongo_crypt_v1_status` functions are
 * provided which permit observing the details of these failures. Further a construction function
 * and a destruction function for these objects are also provided.
 *
 * The use of status objects from multiple threads is not thread safe unless all of the threads
 * accessing a single status object are passing that object as a const-qualified (const
 * mongo_crypt_v1_status *) pointer. If a single thread is passing a status object to a function
 * taking it by non-const-qualified (mongo_crypt_v1_status*) pointer, then no other thread may
 * access the status object.
 *
 * The `status` parameter is optional for all `mongo_crypt_v1_` functions that can take a status
 * pointer. The caller may pass NULL instead of a valid `status` object, in which case the function
 * will execute normally but will not provide any detailed error information in the caes of a
 * failure.
 *
 * All `mongo_crypt_v1_status` functions can be used before the Mongo Crypt Shared library is
 * initialized. This facilitates detailed error reporting from all library functions.
 */
typedef struct mongo_crypt_v1_status mongo_crypt_v1_status;

/**
 * Allocate and construct an API-return-status buffer object.
 *
 * Returns NULL when construction of a mongo_crypt_v1_status object fails.
 *
 * This function may be called before mongo_crypt_v1_lib_create().
 */
MONGO_CRYPT_API mongo_crypt_v1_status* MONGO_API_CALL mongo_crypt_v1_status_create(void);

/**
 * Destroys a valid status object.
 *
 * The status object must be a valid mongo_crypt_v1_status object or NULL.
 *
 * This function is not thread safe, and it must not execute concurrently with any other function
 * that accesses the status object being destroyed. It is, however, safe to destroy distinct status
 * objects on distinct threads.
 *
 * This function does not report failures.
 *
 * This function may be called before `mongo_crypt_v1_lib_create()`.
 *
 * This function causes all storage associated with the specified status object to be released,
 * including the storage referenced by functions that returned observable storage buffers from this
 * status, such as strings.
 */
MONGO_CRYPT_API void MONGO_API_CALL mongo_crypt_v1_status_destroy(mongo_crypt_v1_status* status);

/**
 * The error codes reported by `mongo_crypt_v1` functions will be given the symbolic names as
 * mapped by this enum.
 *
 * When a `mongo_crypt_v1` function fails (and it has been documented to report errors) it will
 * report that error in the form of an `int` status code. That status code will always be returned
 * as the type `int`; however, the values in this enum can be used to classify the failure.
 */
typedef enum {
    MONGO_CRYPT_V1_ERROR_IN_REPORTING_ERROR = -2,
    MONGO_CRYPT_V1_ERROR_UNKNOWN = -1,

    MONGO_CRYPT_V1_SUCCESS = 0,

    MONGO_CRYPT_V1_ERROR_ENOMEM = 1,
    MONGO_CRYPT_V1_ERROR_EXCEPTION = 2,
    MONGO_CRYPT_V1_ERROR_LIBRARY_ALREADY_INITIALIZED = 3,
    MONGO_CRYPT_V1_ERROR_LIBRARY_NOT_INITIALIZED = 4,
    MONGO_CRYPT_V1_ERROR_INVALID_LIB_HANDLE = 5,
    MONGO_CRYPT_V1_ERROR_REENTRANCY_NOT_ALLOWED = 6,
} mongo_crypt_v1_error;

/**
 * Gets an error code from a `mongo_crypt_v1_status` object.
 *
 * When a `mongo_crypt_v1` function fails (and it has been documented to report errors) it will
 * report its error in the form of an `int` status code which is stored into a supplied
 * `mongo_crypt_v1_status` object, if provided. Some of these functions may also report extra
 * information, which will be reported by other observer functions. Every `mongo_crypt_v1`
 * function which reports errors will always update the `Error` code stored in a
 * `mongo_crypt_v1_status` object, even upon success.
 *
 * This function does not report its own failures.
 */
MONGO_CRYPT_API int MONGO_API_CALL
mongo_crypt_v1_status_get_error(const mongo_crypt_v1_status* status);

/**
 * Gets a descriptive error message from a `mongo_crypt_v1_status` object.
 *
 * For failures where the error is MONGO_CRYPT_V1_ERROR_EXCEPTION, this returns a string
 * representation of the internal C++ exception.
 *
 * The function to which the specified status object was passed must not have returned
 * MONGO_CRYPT_V1_SUCCESS as its error code.
 *
 * The storage for the returned string is associated with the specified status object, and therefore
 * it will be deallocated when the status is destroyed using mongo_crypt_v1_status_destroy().
 *
 * This function does not report its own failures.
 */
MONGO_CRYPT_API const char* MONGO_API_CALL
mongo_crypt_v1_status_get_explanation(const mongo_crypt_v1_status* status);

/**
 * Gets a status code from a `mongo_crypt_v1_status` object.
 *
 * Returns a numeric status code associated with the status parameter which indicates a sub-category
 * of failure.
 *
 * For any status object that does not have MONGO_CRYPT_V1_ERROR_EXCEPTION as its error, the
 * value of this code is unspecified.
 *
 * This function does not report its own failures.
 */
MONGO_CRYPT_API int MONGO_API_CALL
mongo_crypt_v1_status_get_code(const mongo_crypt_v1_status* status);

/**
 * An object which describes the runtime state of the Mongo Crypt Shared Library.
 *
 * The Mongo Crypt Shared Library uses allocated objects of this type to indicate the present state
 * of the library. Some operations which the library provides need access to this object. Further a
 * construction function and a destruction function for these objects are also provided. No more
 * than a single object instance of this type may exist at any given time.
 *
 * The use of `mongo_crypt_v1_lib` objects from multiple threads is not thread safe unless all of
 * the threads accessing a single `mongo_crypt_v1_lib` object are not destroying this object.  If
 * a single thread is passing a `mongo_crypt_v1_lib` to its destruction function, then no other
 * thread may access the `mongo_crypt_v1_lib` object.
 */
typedef struct mongo_crypt_v1_lib mongo_crypt_v1_lib;

/**
 * Creates a mongo_crypt_v1_lib object, which stores context for the Mongo Crypt Shared library. A
 * process should only ever have one mongo_crypt_v1_lib instance.
 *
 * On failure, returns NULL and populates the 'status' object if it is not NULL.
 */
MONGO_CRYPT_API mongo_crypt_v1_lib* MONGO_API_CALL
mongo_crypt_v1_lib_create(mongo_crypt_v1_status* status);

/**
 * Tears down the state of this library. Existing mongo_crypt_v1_status objects remain valid.
 * Existing mongo_crypt_v1_query_analyzer objects created from this library MUST BE destroyed
 * before destroying the library object.
 *
 * The 'lib' parameter must be a valid mongo_crypt_v1_lib instance and must not be NULL.
 *
 * The 'status' parameter may be NULL, but must be a valid mongo_crypt_v1_status instance if it
 * is not NULL.
 *
 * Returns MONGO_CRYPT_V1_SUCCESS on success.
 *
 * Returns MONGO_CRYPT_V1_ERROR_LIBRARY_NOT_INITIALIZED and modifies 'status' if
 * mongo_crypt_v1_lib_create() has not been called previously.
 */
MONGO_CRYPT_API int MONGO_API_CALL mongo_crypt_v1_lib_destroy(mongo_crypt_v1_lib* lib,
                                                              mongo_crypt_v1_status* status);


/**
 * Returns a product version in 64-bit integer in four 16-bit words, from high to low:
 *
 * - Major version
 * - Minor version
 * - Revision
 * - Reserved
 *
 * For example, version 6.2.1 would be encoded as: 0x0006000200010000
 *
 */
MONGO_CRYPT_API uint64_t MONGO_API_CALL mongo_crypt_v1_get_version(void);

/**
 * Returns a product version as a text string. The string should not be modified or released
 *
 * Examples:
 *    Dev Build/patch: mongo_crypt_v1-rhel80-6.2.1-alpha4-284-g8662e7d
 *    Release build: mongo_crypt_v1-rhel80-6.2.1
 */
MONGO_CRYPT_API const char* MONGO_API_CALL mongo_crypt_v1_get_version_str(void);


/**
 * A single query analyzer can be used across repeated calls to mongo_crypt_v1_analyze_query.
 *
 * It is not safe to simultaneously invoke mongo_crypt_v1_query_analyzer_create on the same
 * query analyzer from multiple threads
 *
 * It is the client's responsibility to call mongo_crypt_v1_query_analyzer_destroy() to free up
 * resources used by the query analyzer. Once a query analyzer is destroyed, it is not safe to
 * call mongo_crypt_v1_analyze_query.
 */
typedef struct mongo_crypt_v1_query_analyzer mongo_crypt_v1_query_analyzer;

/**
 * Creates a mongo_crypt_v1_query_analyzer object, which stores a parsed collation.
 *
 * The 'lib' parameter must be a valid mongo_crypt_v1_lib instance and must not be NULL.
 *
 * On failure, it returns NULL and populates the 'status' object if it is not NULL.
 */
MONGO_CRYPT_API mongo_crypt_v1_query_analyzer* MONGO_API_CALL
mongo_crypt_v1_query_analyzer_create(mongo_crypt_v1_lib* lib, mongo_crypt_v1_status* status);

/**
 * Destroys a valid mongo_crypt_v1_query_analyzer object.
 *
 * This function is not thread safe, and it must not execute concurrently with any other function
 * that accesses the collation object being destroyed including those that access a matcher,
 * projection or update object which references the collation object.
 *
 * This function does not report failures.
 */
MONGO_CRYPT_API void MONGO_API_CALL
mongo_crypt_v1_query_analyzer_destroy(mongo_crypt_v1_query_analyzer* analyzer);


/**
 * Analyzes the 'documentBSON' input document which includes a JSON schema for namespace 'ns_str'
 * and returns a new BSON document that replaces fields that need encryption with BinData
 * (subtype 6, first byte 0) placeholders.
 *
 * The input document must be a valid OP_MSG document supports aggregate, count, delete, distinct,
 *  explain, find, findAndModify, insert and update.
 * In addition, the input document must include two additional top-level fields:
 * - jsonSchema : document - contains a valid JSON schema
 * - isRemoteSchema : bool - true if schema comes from MongoD as opposed to client-side
 *
 * The 'analyzer' and 'documentBSON' parameters must point to initialized objects of their
 * respective types.
 *
 * When the analysis is successful, this function returns non-null value which points to a valid
 * BSON document and updates bson_len with the length of the BSON document
 */
MONGO_CRYPT_API uint8_t* MONGO_API_CALL
mongo_crypt_v1_analyze_query(mongo_crypt_v1_query_analyzer* analyzer,
                             const uint8_t* documentBSON,
                             const char* ns_str,
                             uint32_t ns_len,
                             uint32_t* bson_len,
                             mongo_crypt_v1_status* status);

/**
 * Free the memory of a BSON buffer returned by mongo_crypt_v1_analyze_query(). This function can be
 * safely called on a NULL pointer.
 *
 * This function can be called at any time to deallocate a BSON buffer and will not invalidate any
 * library object.
 */
MONGO_CRYPT_API void MONGO_API_CALL mongo_crypt_v1_bson_free(uint8_t* bson);

#ifdef __cplusplus
}  // extern "C"
#endif

#undef MONGO_CRYPT_SUPPORT_API
#pragma pop_macro("MONGO_CRYPT_SUPPORT_API")

#undef MONGO_API_EXPORT
#pragma push_macro("MONGO_API_EXPORT")

#undef MONGO_API_IMPORT
#pragma push_macro("MONGO_API_IMPORT")

#undef MONGO_API_CALL
#pragma pop_macro("MONGO_API_CALL")

#endif  // MONGO_CRYPT_SUPPORT_H
