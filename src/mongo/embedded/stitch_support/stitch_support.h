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
    STITCH_SUPPORT_V1_ERROR_REENTRANCY_NOT_ALLOWED = 6,
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
 * stitch_support_v1_lib_init() has not been called previously.
 */
STITCH_SUPPORT_API int MONGO_API_CALL
stitch_support_v1_fini(stitch_support_v1_lib* const lib, stitch_support_v1_status* const status);

/**
 * A collator object represents a parsed collation. A single collator can be used by multiple
 * matcher, projection, and update objects.
 *
 * It is the client's responsibility to call stitch_support_v1_collator_destroy() to free up
 * resources used by the collator. Once a collator is destroyed, it is not safe to call any
 * functions on matcher, projection, and update objects that reference the collator, except for
 * their destroy functions.
 */
typedef struct stitch_support_v1_collator stitch_support_v1_collator;

/**
 * Creates a stitch_support_v1_collator object, which stores a parsed collation.
 *
 * This function will fail if the collationBSON is invalid. On failure, it returns NULL and
 * populates the 'status' object if it is not NULL.
 */
STITCH_SUPPORT_API stitch_support_v1_collator* MONGO_API_CALL
stitch_support_v1_collator_create(stitch_support_v1_lib* lib,
                                  const uint8_t* collationBSON,
                                  stitch_support_v1_status* const status);

/**
 * Destroys a valid stitch_support_v1_collator object.
 *
 * This function is not thread safe, and it must not execute concurrently with any other function
 * that accesses the collation object being destroyed including those that access a matcher,
 * projection or update object which reference the collation object.
 *
 * This function does not report failures.
 */
STITCH_SUPPORT_API void MONGO_API_CALL
stitch_support_v1_collator_destroy(stitch_support_v1_collator* collator);

/**
 * A matcher object is used to determine if a BSON document matches a predicate.
 *
 * A matcher can optionally use a collator. The client is responsible for ensuring that a matcher's
 * collator continues to exist for the lifetime of the matcher and for ultimately destroying both
 * the collator and the matcher. Multiple matcher, projection, and update objects can share the same
 * collation object.
 */
typedef struct stitch_support_v1_matcher stitch_support_v1_matcher;

/**
 * Creates a stitch_support_v1_matcher object, which represents a predicate to match against. The
 * predicate itself is represented as a BSON object, which is passed in the 'filterBSON' argument.
 *
 * This function will fail if the predicate is invalid, returning NULL and populating 'status' with
 * information about the error.
 *
 * The 'collator' argument, a pointer to a stitch_support_v1_collator, will cause the matcher to use
 * the given collator if provided but the pointer can be NULL to cause the matcher to use no
 * collator. The newly created matcher does _not_ take ownership of its 'collator' object.
 */
STITCH_SUPPORT_API stitch_support_v1_matcher* MONGO_API_CALL
stitch_support_v1_matcher_create(stitch_support_v1_lib* lib,
                                 const uint8_t* filterBSON,
                                 stitch_support_v1_collator* collator,
                                 stitch_support_v1_status* status);

/**
 * Destroys a valid stitch_support_v1_matcher object.
 *
 * This function does not destroy the collator associated with the destroyed matcher. When
 * destroying a matcher and its associated collator together, it is safe to destroy them in either
 * order. Although a matcher is no longer valid once its associated collator has been destroyed, it
 * is still safe to call this destroy function on the matcher.
 *
 * This function is not thread safe, and it must not execute concurrently with any other function
 * that accesses the matcher object being destroyed.
 *
 * This function does not report failures.
 */
STITCH_SUPPORT_API void MONGO_API_CALL
stitch_support_v1_matcher_destroy(stitch_support_v1_matcher* const matcher);

/**
 * Check if the 'documentBSON' input matches the predicate represented by the 'matcher' object.
 *
 * The 'matcher' and 'documentBSON' parameters must point to initialized objects of their respective
 * types, and 'isMatch' must be non-NULL.
 *
 * When the check is successful, this function returns STITCH_SUPPORT_V1_SUCCESS, sets 'isMatch' to
 * indicate whether the document matched.
 */
STITCH_SUPPORT_API int MONGO_API_CALL
stitch_support_v1_check_match(stitch_support_v1_matcher* matcher,
                              const uint8_t* documentBSON,
                              bool* isMatch,
                              stitch_support_v1_status* status);

/**
 * A projection object is used to apply a projection to a BSON document which may include or exclude
 * particular fields (e.g.: {_id: 0, a: 1}) or produce new columns through an expression made of
 * projection operators.
 *
 * A projection requires a matcher if it uses the positional ($) operator. This matcher is used to
 * determine which array element matches the specified position. The matcher is optional and unused
 * when there is no positional operator.
 *
 * A projection can optionally use a collator for evaluating $elemMatch operators. Projection
 * objects without collators will use the default collation for $elemMatch, even if their matcher
 * has a collator object. Multiple matcher, projection, and update objects can share the same
 * collation object.
 */
typedef struct stitch_support_v1_projection stitch_support_v1_projection;

/**
 * Creates a stitch_support_v1_projection object, which is used to apply a projection to a BSON
 * document. The projection specification is also represented as a BSON document, which is passed in
 * the 'specBSON' argument. The syntax used for projection is the same as a MongoDB "find" command
 * (i.e., not an aggregation $project stage).
 *
 * This function will fail if the projection specification is invalid, returning NULL and populating
 * 'status' with information about the error.
 *
 * If the projection specification includes a positional ($) operator, then the caller must pass a
 * mongo_embedded_v1_matcher. The 'matcher' argument is unnecessary if the specification has no
 * positional operator and it can be NULL.
 *
 * The 'collator' argument, a pointer to a stitch_support_v1_collator, will cause the projection to
 * use the given collator if provided, The pointer can be NULL to cause the projection to use no
 * collator.
 *
 * The newly created projection object does _not_ take ownership of its 'matcher' or 'collator'
 * objects. The client is responsible for ensuring that the matcher and collator continue to exist
 * for the lifetime of the projection and for ultimately destroying all three of the projection,
 * matcher and collator.
 */
STITCH_SUPPORT_API stitch_support_v1_projection* MONGO_API_CALL
stitch_support_v1_projection_create(stitch_support_v1_lib* lib,
                                    const uint8_t* specBSON,
                                    stitch_support_v1_matcher* matcher,
                                    stitch_support_v1_collator* collator,
                                    stitch_support_v1_status* status);

/**
 * Destroys a valid stitch_support_v1_projection object.
 *
 * This function does not destroy the collator or matcher associated with the destroyed projection.
 * When destroying a projection and its associated collator and/or matcher together, it is safe to
 * destroy them in any order. Although a projection is no longer valid once its associated collator
 * or matcher has been destroyed, it is still safe to call this destroy function on the projection.
 *
 * This function is not thread safe, and it must not execute concurrently with any other function
 * that accesses the matcher object being destroyed.
 *
 * This function does not report failures.
 */
STITCH_SUPPORT_API void MONGO_API_CALL
stitch_support_v1_projection_destroy(stitch_support_v1_projection* const projection);

/**
 * Apply a projection to an input document, writing the resulting BSON to a newly allocated 'output'
 * buffer. Returns a pointer to the output buffer on success or NULL on error. 'status' is populated
 * in the error case. The caller is responsible for destroying the result buffer with
 * stitch_support_v1_bson_free().
 *
 * If the projection includes a positional ($) operator, the caller should verify before applying it
 * that the associated matcher matches the input document. A non-matching input document will
 * trigger an assertion failure.
 */
STITCH_SUPPORT_API uint8_t* MONGO_API_CALL
stitch_support_v1_projection_apply(stitch_support_v1_projection* const projection,
                                   const uint8_t* documentBSON,
                                   stitch_support_v1_status* status);

/**
 * Returns true iff applying this projection requires a matcher that matches the input document,
 * as is the case when the projection includes the positional ($) operator.
 *
 * When this function returns true, it is illegal to call stitch_support_v1_projection_apply()
 * unless the projection has a matcher (passed in during stitch_support_v1_projection_create) and
 * the matcher matches the input document.
 */
STITCH_SUPPORT_API bool MONGO_API_CALL
stitch_support_v1_projection_requires_match(stitch_support_v1_projection* const projection);

/**
 * An update details object stores the list of paths modified by a call to
 * stitch_support_v1_update_apply().
 */
typedef struct stitch_support_v1_update_details stitch_support_v1_update_details;

/**
 * Create an "update details" object to pass to stitch_support_v1_update_apply(), which will
 * populate the update details with a list of paths modified by the update.
 *
 * Clients can reuse the same update details object for multiple calls to
 * stitch_support_v1_update_apply().
 */
STITCH_SUPPORT_API stitch_support_v1_update_details* MONGO_API_CALL
stitch_support_v1_update_details_create(void);

/**
 * Destroys an update details object.
 *
 * This function is not thread safe, and it must not execute concurrently with any other function
 * that accesses the update details object being destroyed.
 *
 * This function does not report failures.
 */
STITCH_SUPPORT_API void MONGO_API_CALL
stitch_support_v1_update_details_destroy(stitch_support_v1_update_details* update_details);

/**
 * The number of modified paths in an update details object. Always call this function to ensure an
 * index is in bounds before calling stitch_support_v1_update_details_path().
 */
STITCH_SUPPORT_API size_t MONGO_API_CALL stitch_support_v1_update_details_num_modified_paths(
    stitch_support_v1_update_details* update_details);

/**
 * Return a dotted-path string from the given index of the modified paths in the update details
 * object.
 */
STITCH_SUPPORT_API const char* MONGO_API_CALL stitch_support_v1_update_details_path(
    stitch_support_v1_update_details* update_details, size_t path_index);

/**
 * An update object used to apply an update to a BSON document, which may modify particular
 * fields (e.g.: {$set: {a: 1}}) or replace the entire document with a new one.
 */
typedef struct stitch_support_v1_update stitch_support_v1_update;

/**
 * Creates a stitch_support_v1_update object by parsing the update expression provided in the
 * 'updateBSON' argument.
 *
 * The optional 'arrayFiltersBSON' argument must be a BSON array where each element of the array
 * represents an array filter with the field name storing the placeholder name and the value storing
 * the assoicated match expression. This argument is permitted to be NULL in the case where there
 * are no array filters. If 'updateBSON' contains a placeholder ($[*]) that does not have a
 * corresponding 'arrayFiltersBSON' entry this function will fail to parse.
 *
 * This function will fail if either 'updateBSON' or 'arrayFiltersBSON fails to parse, returning
 * NULL and populating 'status' with information about the error.
 *
 * A 'matcher' argument can be provided which is required if the positional '$' operator appears
 * inside the 'update' and optional and unused otherwise.
 *
 * The caller can optionally provide a collator, which is used when evaluating arrayFilters match
 * expressions. The 'collator' parameter must match the collator in 'matcher'. A mismatch will raise
 * an invariant violation if 'matcher' is non-NULL. Multiple matcher, projection, and update objects
 * can share the same collation object.
 *
 * The newly created update object does _not_ take ownership of its 'matcher' or 'collator'
 * objects. The client is responsible for ensuring that the matcher and collator continue to exist
 * for the lifetime of the update and for ultimately destroying all three of the update, matcher,
 * and collator.
 */
STITCH_SUPPORT_API stitch_support_v1_update* MONGO_API_CALL
stitch_support_v1_update_create(stitch_support_v1_lib* lib,
                                const uint8_t* updateBSON,
                                const uint8_t* arrayFiltersBSON,
                                stitch_support_v1_matcher* matcher,
                                stitch_support_v1_collator* collator,
                                stitch_support_v1_status* status);

/**
 * Destroys a valid stitch_support_v1_update object.
 *
 * This function does not destroy the collator associated with the destroyed update. When
 * destroying an update and its associated collator together, it is safe to destroy them in either
 * order. Although an update is no longer valid once its associated collator has been destroyed, it
 * is still safe to call this destroy function on the update.
 *
 * This function is not thread safe, and it must not execute concurrently with any other function
 * that accesses the update object being destroyed.
 *
 * This function does not report failures.
 */
STITCH_SUPPORT_API void MONGO_API_CALL
stitch_support_v1_update_destroy(stitch_support_v1_update* const update);

/**
 * Apply an update to an input document writing the resulting BSON to a newly allocated output
 * buffer. Returns a pointer to the output buffer on success or NULL on error. 'status' is populated
 * in the error case. The caller is responsible for destroying the result buffer with
 * stitch_support_v1_bson_free().
 *
 * If the update includes a positional ($) operator, the caller should verify before applying it
 * that the associated matcher matches the input document. A non-matching input document will
 * trigger an assertion failure.
 */
STITCH_SUPPORT_API uint8_t* MONGO_API_CALL
stitch_support_v1_update_apply(stitch_support_v1_update* const update,
                               const uint8_t* documentBSON,
                               stitch_support_v1_update_details* update_details,
                               stitch_support_v1_status* status);

/**
 * Return the document that would result from an {upsert: true} update operation that must perform
 * an upsert. There is no "input document," like in stitch_support_v1_apply(), because upserts occur
 * when no input document is found by the match predicate.
 *
 * In the case of an update consisting of operators, the resulting document is based on the match
 * predicate in the associated matcher and on the update document. This case expects an update
 * object that has an associated matcher but will synthesize an empty matcher if none was given.
 *
 * In the case of an update consisting of a full document, the resulting document will be a copy of
 * the given document. This is consistent with the server behavior for updates without operators. In
 * this case a matcher is not required and if present, is ignored.
 *
 * This operation returns a pointer to the output buffer on success or, on error, returns NULL and
 * populates the 'status' object. The caller is responsible for destroying the result buffer with
 * stitch_support_v1_bson_free().
 *
 * Note that, although a database upsert operations guarantees that the inserted document will have
 * an '_id' field, this function does not populate an '_id' field if one is not in the match or
 * update document.
 */
STITCH_SUPPORT_API uint8_t* MONGO_API_CALL stitch_support_v1_update_upsert(
    stitch_support_v1_update* const update, stitch_support_v1_status* status);

/**
 * Returns true iff applying this update requires a matcher that matches the input document, as is
 * the case when the update includes the positional ($) operator.
 *
 * When this function returns true, it is illegal to call stitch_support_v1_update_apply() unless
 * the update has a matcher (passed in during stitch_support_v1_update_create) and the matcher
 * matches the input document.
 */
STITCH_SUPPORT_API bool MONGO_API_CALL
stitch_support_v1_update_requires_match(stitch_support_v1_update* const update);

/**
 * Free the memory of a BSON buffer returned by stitch_support_v1_projection_apply() or
 * stitch_support_v1_update_apply(). This function can be safely called on a NULL pointer.
 *
 * This function can be called at any time to deallocate a BSON buffer and will not invalidate any
 * library object.
 */
STITCH_SUPPORT_API void MONGO_API_CALL stitch_support_v1_bson_free(uint8_t* bson);

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
