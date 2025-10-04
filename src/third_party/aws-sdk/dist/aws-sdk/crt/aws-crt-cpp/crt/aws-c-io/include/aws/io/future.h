/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_IO_FUTURE_H
#define AWS_IO_FUTURE_H

/*

// THIS IS AN EXPERIMENTAL AND UNSTABLE API
//
// An aws_future is used to deliver the result of an asynchronous function.
//
// When an async function is called, it creates a future and returns it to the caller.
// When the async work is finished, it completes the future by setting an error or result value.
// The caller waits until the future is done, checks for error, and then gets
// the result if everything was OK. Typically, the caller waits by registering
// a callback that the future invokes when it's done.
//
// If result type T has a "destructor" (clean_up(), destroy(), or release() function),
// then the future has set_result_by_move() and get_result_by_move() functions
// that explicitly transfer ownership to and from the future.
// If the future dies, and still "owns" the resource, it calls the destructor.
// If T has no destructor, then the future has set_result() and get_result()
// functions that simply copy T by value.
//
// Macros are used to define a type-safe API for each result type T,
// similar to C++ templates. This makes the API hard to browse, so functions
// are documented in comments below. The result setter/getter functions
// are mildly different based on T's destructor type, and are documented later.

//
// --- API (common to all aws_future<T>) ---
//

// Create a new future, with refcount of 1.
struct aws_future_T *aws_future_T_new(struct aws_allocator *alloc);

// Increment the refcount.
// You can pass NULL (has no effect).
// Returns the same pointer that was passed in.
struct aws_future_T *aws_future_T_acquire(struct aws_future_T *future);

// Decrement the refcount.
// You can pass NULL (has no effect).
// Always returns NULL.
struct aws_future_T *aws_future_T_release(struct aws_future_T *future);

// Set future as done, with an error_code.
// If the future is already done this call is ignored.
void aws_future_T_set_error(struct aws_future_T *future, int error_code);

// Return whether the future is done.
bool aws_future_T_is_done(const struct aws_future_T *future);

// Get the error-code of a completed future.
// If 0 is returned, then the future completed successfully,
// you may now get the result.
//
// WARNING: You MUST NOT call this until the future is done.
int aws_future_T_get_error(const struct aws_future_T *future);

// Register callback to be invoked when the future completes.
//
// If the future is already done, the callback runs synchronously on the calling thread.
// If the future isn't done yet, the callback is registered, and it
// will run synchronously on whatever thread completes the future.
//
// WARNING: You MUST NOT register more than one callback.
void aws_future_T_register_callback(struct aws_future_T *future, aws_future_callback_fn *on_done, void *user_data);

// If the future isn't done yet, then register the completion callback.
//
// Returns true if the callback was registered,
// or false if the future is already done.
//
// Use this when you can't risk the callback running synchronously.
// For example: If you're calling an async function repeatedly,
// and synchronous completion could lead to stack overflow due to recursion.
// Or if you are holding a non-recursive mutex, and the callback also
// needs the mutex, and an immediate callback would deadlock.
//
// WARNING: If a callback is registered, you MUST NOT call this again until
// the callback has been invoked.
bool aws_future_T_register_callback_if_not_done(
    struct aws_future_T *future,
    aws_future_callback_fn *on_done,
    void *user_data);

// Register completion callback to run async on an event-loop thread.
//
// When the future completes, the callback is scheduled to run as an event-loop task.
//
// Use this when you want the callback to run on the event-loop's thread,
// or to ensure the callback runs async even if the future completed synchronously.
//
// WARNING: You MUST NOT register more than one callback.
void aws_future_T_register_event_loop_callback(
    struct aws_future_T *future,
    struct aws_event_loop *event_loop,
    aws_future_callback_fn *on_done,
    void *user_data);

// Register completion callback to run async on an aws_channel's thread.
//
// When the future completes, the callback is scheduled to run as a channel task.
//
// Use this when you want the callback to run on the channel's thread,
// or to ensure the callback runs async even if the future completed synchronously.
//
// WARNING: You MUST NOT register more than one callback.
void aws_future_T_register_channel_callback(
    struct aws_future_T *future,
    struct aws_channel *channel,
    aws_future_callback_fn *on_done,
    void *user_data);

// Wait (up to timeout_ns) for future to complete.
// Returns true if future completes in this time.
// This blocks the current thread, and is probably only useful for tests and sample programs.
bool aws_future_T_wait(struct aws_future_T *future, uint64_t timeout_ns);

//
// --- Defining new aws_future types ---
// TODO UPDATE THESE DOCS
// To define new types of aws_future<T>, add the appropriate macro to the appropriate header.
// The macros are:
//
// AWS_DECLARE_FUTURE_T_BY_VALUE(FUTURE, T)
// For T stored by value, with no destructor.
// Use with types like bool, size_t, etc
//
// AWS_DECLARE_FUTURE_T_BY_VALUE_WITH_CLEAN_UP(FUTURE, T, CLEAN_UP_FN)
// For T stored by value, with destructor like: void aws_T_clean_up(T*)
// Use with types like `struct aws_byte_buf`
//
// AWS_DECLARE_FUTURE_T_POINTER_WITH_DESTROY(FUTURE, T, DESTROY_FN)
// For T stored by pointer, with destructor like: void aws_T_destroy(T*)
// Use with types like `struct aws_string *`
//
// AWS_DECLARE_FUTURE_T_POINTER_WITH_RELEASE(FUTURE, T, RELEASE_FN)
// For T stored by pointer, with destructor like: T* aws_T_release(T*)
// Use with types like `struct aws_http_message *`
// Note: if T's release() function doesn't return a pointer, use _WITH_DESTROY instead of _WITH_RELEASE.
//
// This file declares several common types: aws_future<size_t>, aws_future<void>, etc.
// But new future types should be declared in the header where that type's API is declared.
// For example: AWS_DECLARE_FUTURE_T_POINTER_WITH_RELEASE(aws_future_http_message, struct aws_http_message)
// would go in: aws-c-http/include/aws/http/request_response.h
//
// The APIs generated by these macros are identical except for the "setter" and "getter" functions.

//
// --- Design (if you're curious) ---
//
// This class was developed to give the user more control over how the completion
// callback is invoked. In the past, we passed completion callbacks to the async
// function. But this could lead to issues when an async function "sometimes"
// completed synchronously and "sometimes" completed async. The async function
// would need to stress about how to schedule the callback so it was always async,
// or more typically just invoke it whenever and leave the caller to figure it out.
//
// This class is also an experiment with "templates/generics in C".
// In order to make the class type-safe, we use macros to define a unique
// API for each result type T we need to store in a future.
// If we refer to aws_future<struct aws_byte_buf>, we mean a struct named
// aws_future_byte_buf, which stores an aws_byte_buf by value.
// This could lead to code bloat, but the type-safety seems worth it.
//
// future is defined in aws-c-io, instead of aws-c-common, so it can
// easily integrate with aws_event_loop and aws_channel.
//
// It's legal to call set_error() or set_result() multiple times.
// If the future is already done, it ignores the call.
// If result T has a destructor, the new result is immediately freed instead of saved.
// This design lets us deal with ambiguity where it's not 100% certain whether a handoff occurred.
// For example: if we call from C->Java and an exception is thrown,
// it's not clear whether Java got the handoff. In this case, we can safely
// call set_error(), completing the future if necessary,
// or being ignored if the future was already done.

*/

#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_channel;
struct aws_event_loop;
struct aws_future_impl;

/** Completion callback for aws_future<T> */
typedef void(aws_future_callback_fn)(void *user_data);

typedef void(aws_future_impl_result_clean_up_fn)(void *result_addr);
typedef void(aws_future_impl_result_destroy_fn)(void *result);
typedef void *(aws_future_impl_result_release_fn)(void *result);

AWS_EXTERN_C_BEGIN

AWS_IO_API
struct aws_future_impl *aws_future_impl_new_by_value(struct aws_allocator *alloc, size_t sizeof_result);

AWS_IO_API
struct aws_future_impl *aws_future_impl_new_by_value_with_clean_up(
    struct aws_allocator *alloc,
    size_t sizeof_result,
    aws_future_impl_result_clean_up_fn *result_clean_up);

AWS_IO_API
struct aws_future_impl *aws_future_impl_new_pointer(struct aws_allocator *alloc);

AWS_IO_API
struct aws_future_impl *aws_future_impl_new_pointer_with_destroy(
    struct aws_allocator *alloc,
    aws_future_impl_result_destroy_fn *result_destroy);

AWS_IO_API
struct aws_future_impl *aws_future_impl_new_pointer_with_release(
    struct aws_allocator *alloc,
    aws_future_impl_result_release_fn *result_release);

AWS_IO_API
struct aws_future_impl *aws_future_impl_release(struct aws_future_impl *promise);

AWS_IO_API
struct aws_future_impl *aws_future_impl_acquire(struct aws_future_impl *promise);

AWS_IO_API
void aws_future_impl_set_error(struct aws_future_impl *promise, int error_code);

AWS_IO_API
void aws_future_impl_set_result_by_move(struct aws_future_impl *promise, void *src_address);

AWS_IO_API
bool aws_future_impl_is_done(const struct aws_future_impl *future);

AWS_IO_API
void aws_future_impl_register_callback(
    struct aws_future_impl *future,
    aws_future_callback_fn *on_done,
    void *user_data);

AWS_IO_API
bool aws_future_impl_register_callback_if_not_done(
    struct aws_future_impl *future,
    aws_future_callback_fn *on_done,
    void *user_data);

AWS_IO_API
void aws_future_impl_register_event_loop_callback(
    struct aws_future_impl *future,
    struct aws_event_loop *event_loop,
    aws_future_callback_fn *on_done,
    void *user_data);

AWS_IO_API
void aws_future_impl_register_channel_callback(
    struct aws_future_impl *future,
    struct aws_channel *channel,
    aws_future_callback_fn *on_done,
    void *user_data);

AWS_IO_API
bool aws_future_impl_wait(const struct aws_future_impl *future, uint64_t timeout_ns);

AWS_IO_API
int aws_future_impl_get_error(const struct aws_future_impl *future);

AWS_IO_API
void *aws_future_impl_get_result_address(const struct aws_future_impl *future);

AWS_IO_API
void aws_future_impl_get_result_by_move(struct aws_future_impl *future, void *dst_address);

/* Common beginning to all aws_future<T> declarations */
#define AWS_FUTURE_T_DECLARATION_BEGIN(FUTURE, API) struct FUTURE;

/* Common beginning to all aws_future<T> implementations */
#define AWS_FUTURE_T_IMPLEMENTATION_BEGIN(FUTURE)

/* Common end to all aws_future<T> declarations */
#define AWS_FUTURE_T_DECLARATION_END(FUTURE, API)                                                                      \
    API struct FUTURE *FUTURE##_acquire(struct FUTURE *future);                                                        \
    API struct FUTURE *FUTURE##_release(struct FUTURE *future);                                                        \
    API void FUTURE##_set_error(struct FUTURE *future, int error_code);                                                \
    API bool FUTURE##_is_done(const struct FUTURE *future);                                                            \
    API int FUTURE##_get_error(const struct FUTURE *future);                                                           \
    API void FUTURE##_register_callback(struct FUTURE *future, aws_future_callback_fn *on_done, void *user_data);      \
    API bool FUTURE##_register_callback_if_not_done(                                                                   \
        struct FUTURE *future, aws_future_callback_fn *on_done, void *user_data);                                      \
    API void FUTURE##_register_event_loop_callback(                                                                    \
        struct FUTURE *future, struct aws_event_loop *event_loop, aws_future_callback_fn *on_done, void *user_data);   \
    API void FUTURE##_register_channel_callback(                                                                       \
        struct FUTURE *future, struct aws_channel *channel, aws_future_callback_fn *on_done, void *user_data);         \
    API bool FUTURE##_wait(struct FUTURE *future, uint64_t timeout_ns);

/* Common end to all aws_future<T> implementations */
#define AWS_FUTURE_T_IMPLEMENTATION_END(FUTURE)                                                                        \
    struct FUTURE *FUTURE##_acquire(struct FUTURE *future) {                                                           \
        return (struct FUTURE *)aws_future_impl_acquire((struct aws_future_impl *)future);                             \
    }                                                                                                                  \
                                                                                                                       \
    struct FUTURE *FUTURE##_release(struct FUTURE *future) {                                                           \
        return (struct FUTURE *)aws_future_impl_release((struct aws_future_impl *)future);                             \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_set_error(struct FUTURE *future, int error_code) {                                                   \
        aws_future_impl_set_error((struct aws_future_impl *)future, error_code);                                       \
    }                                                                                                                  \
                                                                                                                       \
    bool FUTURE##_is_done(const struct FUTURE *future) {                                                               \
        return aws_future_impl_is_done((const struct aws_future_impl *)future);                                        \
    }                                                                                                                  \
                                                                                                                       \
    int FUTURE##_get_error(const struct FUTURE *future) {                                                              \
        return aws_future_impl_get_error((const struct aws_future_impl *)future);                                      \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_register_callback(struct FUTURE *future, aws_future_callback_fn *on_done, void *user_data) {         \
        aws_future_impl_register_callback((struct aws_future_impl *)future, on_done, user_data);                       \
    }                                                                                                                  \
                                                                                                                       \
    bool FUTURE##_register_callback_if_not_done(                                                                       \
        struct FUTURE *future, aws_future_callback_fn *on_done, void *user_data) {                                     \
                                                                                                                       \
        return aws_future_impl_register_callback_if_not_done((struct aws_future_impl *)future, on_done, user_data);    \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_register_event_loop_callback(                                                                        \
        struct FUTURE *future, struct aws_event_loop *event_loop, aws_future_callback_fn *on_done, void *user_data) {  \
                                                                                                                       \
        aws_future_impl_register_event_loop_callback(                                                                  \
            (struct aws_future_impl *)future, event_loop, on_done, user_data);                                         \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_register_channel_callback(                                                                           \
        struct FUTURE *future, struct aws_channel *channel, aws_future_callback_fn *on_done, void *user_data) {        \
                                                                                                                       \
        aws_future_impl_register_channel_callback((struct aws_future_impl *)future, channel, on_done, user_data);      \
    }                                                                                                                  \
                                                                                                                       \
    bool FUTURE##_wait(struct FUTURE *future, uint64_t timeout_ns) {                                                   \
        return aws_future_impl_wait((struct aws_future_impl *)future, timeout_ns);                                     \
    }

/**
 * Declare a future that holds a simple T by value, that needs no destructor.
 * Use with types like bool, size_t, etc.
 *
 * See top of future.h for most API docs.
 * The result setters and getters are:

// Set the result.
//
// If the future is already done this call is ignored.
void aws_future_T_set_result(const struct aws_future_T *future, T result);

// Get the result of a completed future.
//
// WARNING: You MUST NOT call this until the future is done.
// WARNING: You MUST NOT call this unless get_error() returned 0.
T aws_future_T_get_result(const struct aws_future_T *future);

*/
#define AWS_FUTURE_T_BY_VALUE_DECLARATION(FUTURE, T, API)                                                              \
    AWS_FUTURE_T_DECLARATION_BEGIN(FUTURE, API)                                                                        \
    API struct FUTURE *FUTURE##_new(struct aws_allocator *alloc);                                                      \
    API void FUTURE##_set_result(struct FUTURE *future, T result);                                                     \
    API T FUTURE##_get_result(const struct FUTURE *future);                                                            \
    AWS_FUTURE_T_DECLARATION_END(FUTURE, API)

#define AWS_FUTURE_T_BY_VALUE_IMPLEMENTATION(FUTURE, T)                                                                \
    AWS_FUTURE_T_IMPLEMENTATION_BEGIN(FUTURE)                                                                          \
    struct FUTURE *FUTURE##_new(struct aws_allocator *alloc) {                                                         \
        return (struct FUTURE *)aws_future_impl_new_by_value(alloc, sizeof(T));                                        \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_set_result(struct FUTURE *future, T result) {                                                        \
        aws_future_impl_set_result_by_move((struct aws_future_impl *)future, &result);                                 \
    }                                                                                                                  \
                                                                                                                       \
    T FUTURE##_get_result(const struct FUTURE *future) {                                                               \
        return *(T *)aws_future_impl_get_result_address((const struct aws_future_impl *)future);                       \
    }                                                                                                                  \
    AWS_FUTURE_T_IMPLEMENTATION_END(FUTURE)

/**
 * Declares a future that holds T by value, with destructor like: void aws_T_clean_up(T*)
 * Use with types like aws_byte_buf.
 *
 * See top of future.h for most API docs.
 * The result setters and getters are:

// Set the result, transferring ownership.
//
// The memory at `value_address` is memcpy'd into the future,
// and then zeroed out to help prevent accidental reuse.
// It is safe to call this multiple times. If the future is already done,
// the new result is destroyed instead of saved.
void aws_future_T_set_result_by_move(struct aws_future_T *future, T *value_address);

// Get the result, transferring ownership.
//
// WARNING: You MUST NOT call this until the future is done.
// WARNING: You MUST NOT call this unless get_error() returned 0.
// WARNING: You MUST NOT call this multiple times.
T aws_future_T_get_result_by_move(struct aws_future_T *future);

// Get the result, without transferring ownership.
//
// WARNING: You MUST NOT call this until the future is done.
// WARNING: You MUST NOT call this unless get_error() returned 0.
// WARNING: You MUST NOT call this multiple times.
T* aws_future_T_peek_result(const struct aws_future_T *future);

 */
#define AWS_FUTURE_T_BY_VALUE_WITH_CLEAN_UP_DECLARATION(FUTURE, T, API)                                                \
    AWS_FUTURE_T_DECLARATION_BEGIN(FUTURE, API)                                                                        \
    API struct FUTURE *FUTURE##_new(struct aws_allocator *alloc);                                                      \
    API void FUTURE##_set_result_by_move(struct FUTURE *future, T *value_address);                                     \
    API T *FUTURE##_peek_result(const struct FUTURE *future);                                                          \
    API T FUTURE##_get_result_by_move(struct FUTURE *future);                                                          \
    AWS_FUTURE_T_DECLARATION_END(FUTURE, API)

#define AWS_FUTURE_T_BY_VALUE_WITH_CLEAN_UP_IMPLEMENTATION(FUTURE, T, CLEAN_UP_FN)                                     \
    AWS_FUTURE_T_IMPLEMENTATION_BEGIN(FUTURE)                                                                          \
                                                                                                                       \
    struct FUTURE *FUTURE##_new(struct aws_allocator *alloc) {                                                         \
        void (*clean_up_fn)(T *) = CLEAN_UP_FN; /* check clean_up() function signature */                              \
        return (struct FUTURE *)aws_future_impl_new_by_value_with_clean_up(                                            \
            alloc, sizeof(T), (aws_future_impl_result_clean_up_fn)clean_up_fn);                                        \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_set_result_by_move(struct FUTURE *future, T *value_address) {                                        \
        aws_future_impl_set_result_by_move((struct aws_future_impl *)future, value_address);                           \
    }                                                                                                                  \
                                                                                                                       \
    T *FUTURE##_peek_result(const struct FUTURE *future) {                                                             \
        return aws_future_impl_get_result_address((const struct aws_future_impl *)future);                             \
    }                                                                                                                  \
                                                                                                                       \
    T FUTURE##_get_result_by_move(struct FUTURE *future) {                                                             \
        T value;                                                                                                       \
        aws_future_impl_get_result_by_move((struct aws_future_impl *)future, &value);                                  \
        return value;                                                                                                  \
    }                                                                                                                  \
                                                                                                                       \
    AWS_FUTURE_T_IMPLEMENTATION_END(FUTURE)

/**
 * Declares a future that holds T*, with no destructor.
 */
#define AWS_FUTURE_T_POINTER_DECLARATION(FUTURE, T, API)                                                               \
    AWS_FUTURE_T_DECLARATION_BEGIN(FUTURE, API)                                                                        \
    API struct FUTURE *FUTURE##_new(struct aws_allocator *alloc);                                                      \
    API void FUTURE##_set_result(struct FUTURE *future, T *result);                                                    \
    API T *FUTURE##_get_result(const struct FUTURE *future);                                                           \
    AWS_FUTURE_T_DECLARATION_END(FUTURE, API)

#define AWS_FUTURE_T_POINTER_IMPLEMENTATION(FUTURE, T)                                                                 \
    AWS_FUTURE_T_IMPLEMENTATION_BEGIN(FUTURE)                                                                          \
                                                                                                                       \
    struct FUTURE *FUTURE##_new(struct aws_allocator *alloc) {                                                         \
        return (struct FUTURE *)aws_future_impl_new_pointer(alloc);                                                    \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_set_result(struct FUTURE *future, T *result) {                                                       \
        aws_future_impl_set_result_by_move((struct aws_future_impl *)future, &result);                                 \
    }                                                                                                                  \
                                                                                                                       \
    T *FUTURE##_get_result(const struct FUTURE *future) {                                                              \
        return *(T **)aws_future_impl_get_result_address((const struct aws_future_impl *)future);                      \
    }                                                                                                                  \
                                                                                                                       \
    AWS_FUTURE_T_IMPLEMENTATION_END(FUTURE)

/**
 * Declares a future that holds T*, with destructor like: void aws_T_destroy(T*)
 * Use with types like aws_string.
 *
 * See top of future.h for most API docs.
 * The result setters and getters are:

// Set the result, transferring ownership.
//
// The value at `pointer_address` is copied into the future,
// and then set NULL to prevent accidental reuse.
// If the future is already done, this new result is destroyed instead of saved.
void aws_future_T_set_result_by_move(struct aws_future_T *future, T **pointer_address);

// Get the result, transferring ownership.
//
// WARNING: You MUST NOT call this until the future is done.
// WARNING: You MUST NOT call this unless get_error() returned 0.
// WARNING: You MUST NOT call this multiple times.
T* aws_future_T_get_result_by_move(struct aws_future_T *future);

// Get the result, without transferring ownership.
//
// WARNING: You MUST NOT call this until the future is done.
// WARNING: You MUST NOT call this unless get_error() returned 0.
// WARNING: You MUST NOT call this multiple times.
T* aws_future_T_peek_result(const struct aws_future_T *future);

 */
#define AWS_FUTURE_T_POINTER_WITH_DESTROY_DECLARATION(FUTURE, T, API)                                                  \
    AWS_FUTURE_T_DECLARATION_BEGIN(FUTURE, API)                                                                        \
    API struct FUTURE *FUTURE##_new(struct aws_allocator *alloc);                                                      \
    API void FUTURE##_set_result_by_move(struct FUTURE *future, T **pointer_address);                                  \
    API T *FUTURE##_get_result_by_move(struct FUTURE *future);                                                         \
    API T *FUTURE##_peek_result(const struct FUTURE *future);                                                          \
    AWS_FUTURE_T_DECLARATION_END(FUTURE, API)

#define AWS_FUTURE_T_POINTER_WITH_DESTROY_IMPLEMENTATION(FUTURE, T, DESTROY_FN)                                        \
    AWS_FUTURE_T_IMPLEMENTATION_BEGIN(FUTURE)                                                                          \
                                                                                                                       \
    struct FUTURE *FUTURE##_new(struct aws_allocator *alloc) {                                                         \
        void (*destroy_fn)(T *) = DESTROY_FN; /* check destroy() function signature */                                 \
        return (struct FUTURE *)aws_future_impl_new_pointer_with_destroy(                                              \
            alloc, (aws_future_impl_result_destroy_fn *)destroy_fn);                                                   \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_set_result_by_move(struct FUTURE *future, T **pointer_address) {                                     \
        aws_future_impl_set_result_by_move((struct aws_future_impl *)future, pointer_address);                         \
    }                                                                                                                  \
                                                                                                                       \
    T *FUTURE##_get_result_by_move(struct FUTURE *future) {                                                            \
        T *pointer;                                                                                                    \
        aws_future_impl_get_result_by_move((struct aws_future_impl *)future, &pointer);                                \
        return pointer;                                                                                                \
    }                                                                                                                  \
                                                                                                                       \
    T *FUTURE##_peek_result(const struct FUTURE *future) {                                                             \
        return *(T **)aws_future_impl_get_result_address((const struct aws_future_impl *)future);                      \
    }                                                                                                                  \
                                                                                                                       \
    AWS_FUTURE_T_IMPLEMENTATION_END(FUTURE)

/**
 * Declares a future that holds T*, with destructor like: T* aws_T_release(T*)
 * Use with types like aws_http_message
 *
 * See top of future.h for most API docs.
 * The result setters and getters are:

// Set the result, transferring ownership.
//
// The value at `pointer_address` is copied into the future,
// and then set NULL to prevent accidental reuse.
// If the future is already done, this new result is destroyed instead of saved.
void aws_future_T_set_result_by_move(struct aws_future_T *future, T **pointer_address);

// Get the result, transferring ownership.
//
// WARNING: You MUST NOT call this until the future is done.
// WARNING: You MUST NOT call this unless get_error() returned 0.
// WARNING: You MUST NOT call this multiple times.
T* aws_future_T_get_result_by_move(struct aws_future_T *future);

// Get the result, without transferring ownership.
//
// WARNING: You MUST NOT call this until the future is done.
// WARNING: You MUST NOT call this unless get_error() returned 0.
// WARNING: You MUST NOT call this multiple times.
T* aws_future_T_peek_result(const struct aws_future_T *future);

 */
#define AWS_FUTURE_T_POINTER_WITH_RELEASE_DECLARATION(FUTURE, T, API)                                                  \
    AWS_FUTURE_T_DECLARATION_BEGIN(FUTURE, API)                                                                        \
    API struct FUTURE *FUTURE##_new(struct aws_allocator *alloc);                                                      \
    API void FUTURE##_set_result_by_move(struct FUTURE *future, T **pointer_address);                                  \
    API T *FUTURE##_get_result_by_move(struct FUTURE *future);                                                         \
    API T *FUTURE##_peek_result(const struct FUTURE *future);                                                          \
    AWS_FUTURE_T_DECLARATION_END(FUTURE, API)

#define AWS_FUTURE_T_POINTER_WITH_RELEASE_IMPLEMENTATION(FUTURE, T, RELEASE_FN)                                        \
    AWS_FUTURE_T_IMPLEMENTATION_BEGIN(FUTURE)                                                                          \
                                                                                                                       \
    struct FUTURE *FUTURE##_new(struct aws_allocator *alloc) {                                                         \
        T *(*release_fn)(T *) = RELEASE_FN; /* check release() function signature */                                   \
        return (struct FUTURE *)aws_future_impl_new_pointer_with_release(                                              \
            alloc, (aws_future_impl_result_release_fn *)release_fn);                                                   \
    }                                                                                                                  \
                                                                                                                       \
    void FUTURE##_set_result_by_move(struct FUTURE *future, T **pointer_address) {                                     \
        aws_future_impl_set_result_by_move((struct aws_future_impl *)future, pointer_address);                         \
    }                                                                                                                  \
                                                                                                                       \
    T *FUTURE##_get_result_by_move(struct FUTURE *future) {                                                            \
        T *pointer;                                                                                                    \
        aws_future_impl_get_result_by_move((struct aws_future_impl *)future, &pointer);                                \
        return pointer;                                                                                                \
    }                                                                                                                  \
                                                                                                                       \
    T *FUTURE##_peek_result(const struct FUTURE *future) {                                                             \
        return *(T **)aws_future_impl_get_result_address((const struct aws_future_impl *)future);                      \
    }                                                                                                                  \
                                                                                                                       \
    AWS_FUTURE_T_IMPLEMENTATION_END(FUTURE)

/**
 * aws_future<size_t>
 */
AWS_FUTURE_T_BY_VALUE_DECLARATION(aws_future_size, size_t, AWS_IO_API)

/**
 * aws_future<bool>
 */
AWS_FUTURE_T_BY_VALUE_DECLARATION(aws_future_bool, bool, AWS_IO_API)

/**
 * aws_future<void>
 */
AWS_FUTURE_T_DECLARATION_BEGIN(aws_future_void, AWS_IO_API)

AWS_IO_API struct aws_future_void *aws_future_void_new(struct aws_allocator *alloc);

AWS_IO_API void aws_future_void_set_result(struct aws_future_void *future);

AWS_FUTURE_T_DECLARATION_END(aws_future_void, AWS_IO_API)

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_FUTURE_H */
