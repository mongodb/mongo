/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/**
 * @file s2n.h
 * s2n-tls is a C99 implementation of the TLS/SSL protocols that is designed to
 * be simple, small, fast, and with security as a priority. <br> It is released and 
 * licensed under the Apache License 2.0.
 */

#pragma once

#ifndef S2N_API
    /**
     * Marks a function as belonging to the public s2n API.
     */
    #define S2N_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/uio.h>

/**
 *  Function return code
 */
#define S2N_SUCCESS 0
/**
 * Function return code
 */
#define S2N_FAILURE -1

/**
 * Callback return code 
 */
#define S2N_CALLBACK_BLOCKED -2

/**
 * s2n minimum supported TLS record major version 
 */
#define S2N_MINIMUM_SUPPORTED_TLS_RECORD_MAJOR_VERSION 2

/**
 * s2n maximum supported TLS record major version
 */
#define S2N_MAXIMUM_SUPPORTED_TLS_RECORD_MAJOR_VERSION 3

/**
 * s2n SSL 2.0 Version Constant
 */
#define S2N_SSLv2 20

/**
 * s2n SSL 3.0 Version Constant
 */
#define S2N_SSLv3 30

/**
 * s2n TLS 1.0 Version Constant
 */
#define S2N_TLS10 31

/**
 * s2n TLS 1.1 Version Constant
 */
#define S2N_TLS11 32

/**
 * s2n TLS 1.2 Version Constant
 */
#define S2N_TLS12 33

/**
 * s2n TLS 1.3 Version Constant
 */
#define S2N_TLS13 34

/**
 * s2n Unknown TLS Version
 */
#define S2N_UNKNOWN_PROTOCOL_VERSION 0

/** 
 * s2n-tls functions that return 'int' return 0 to indicate success and -1 to indicate
 * failure. 
 *
 * s2n-tls functions that return pointer types return NULL in the case of
 * failure. 
 *
 * When an s2n-tls function returns a failure, s2n_errno will be set to a value
 * corresponding to the error. This error value can be translated into a string
 * explaining the error in English by calling s2n_strerror(s2n_errno, "EN").
 * A string containing human readable error name; can be generated with `s2n_strerror_name`.
 * A string containing internal debug information, including filename and line number, can be generated with `s2n_strerror_debug`.
 * A string containing only the filename and line number can be generated with `s2n_strerror_source`.
 * This string is useful to include when reporting issues to the s2n-tls development team.
 *
 * @warning To avoid possible confusion, s2n_errno should be cleared after processing an error: `s2n_errno = S2N_ERR_T_OK`
 */
S2N_API extern __thread int s2n_errno;

/**
 * This function can be used instead of trying to resolve `s2n_errno` directly
 * in runtimes where thread-local variables may not be easily accessible.
 *
 * @returns The address of the thread-local `s2n_errno` variable
 */
S2N_API extern int *s2n_errno_location(void);

/**
 * Used to help applications determine why an s2n-tls function failed.
 *
 * This enum is optimized for use in C switch statements. Each value in the enum represents
 * an error "category".
 *
 * s2n-tls organizes errors into different "types" to allow applications to handle error
 * values without catching all possibilities. Applications using non-blocking I/O should check
 * the error type to determine if the I/O operation failed because it would block or for some other
 * error. To retrieve the type for a given error use `s2n_error_get_type()`. Applications should
 * perform any error handling logic using these high level types.
 *
 * See the [Error Handling](https://github.com/aws/s2n-tls/blob/main/docs/usage-guide/topics/ch03-error-handling.md) section for how the errors should be interpreted.
 */
typedef enum {
    /** No error */
    S2N_ERR_T_OK = 0,
    /** Underlying I/O operation failed, check system errno */
    S2N_ERR_T_IO,
    /** EOF */
    S2N_ERR_T_CLOSED,
    /** Underlying I/O operation would block */
    S2N_ERR_T_BLOCKED,
    /** Incoming Alert */
    S2N_ERR_T_ALERT,
    /** Failure in some part of the TLS protocol. Ex: CBC verification failure */
    S2N_ERR_T_PROTO,
    /** Error internal to s2n-tls. A precondition could have failed. */
    S2N_ERR_T_INTERNAL,
    /** User input error. Ex: Providing an invalid cipher preference version */
    S2N_ERR_T_USAGE
} s2n_error_type;

/**
 * Gets the category of error from an error.
 * 
 * s2n-tls organizes errors into different "types" to allow applications to do logic on error values without catching all possibilities.
 * Applications using non-blocking I/O should check error type to determine if the I/O operation failed because 
 * it would block or for some other error.
 *
 * @param error The error from s2n. Usually this is `s2n_errno`.
 * @returns An s2n_error_type
 */
S2N_API extern int s2n_error_get_type(int error);

/**
 * An opaque configuration object, used by clients and servers for holding cryptographic certificates, keys and preferences.
 */
struct s2n_config;

/**
 * An opaque connection. Used to track each s2n connection.
 */
struct s2n_connection;

/**
 * Prevents S2N from calling `OPENSSL_init_crypto`/`OPENSSL_cleanup`/`EVP_cleanup` on OpenSSL versions
 * prior to 1.1.x. This allows applications or languages that also init OpenSSL to interoperate
 * with S2N.
 *
 * @warning This function must be called BEFORE s2n_init() to have any effect. It will return an error
 * if s2n is already initialized.
 *
 * @note If you disable this and are using a version of OpenSSL/libcrypto < 1.1.x, you will
 * be responsible for library init and cleanup (specifically `OPENSSL_add_all_algorithms()`
 * or `OPENSSL_init_crypto()`, and EVP_* APIs will not be usable unless the library is initialized.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_crypto_disable_init(void);

/**
 * Prevents S2N from installing an atexit handler, which allows safe shutdown of S2N from within a
 * re-entrant shared library
 *
 * @warning This function must be called BEFORE s2n_init() to have any effect. It will return an error
 * if s2n is already initialized.
 *
 * @note This will cause `s2n_cleanup` to do complete cleanup of s2n-tls when called from the main
 * thread (the thread `s2n_init` was called from).
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_disable_atexit(void);

/**
 * Fetches the OpenSSL version s2n-tls was compiled with. This can be used by applications to validate at runtime
 * that the versions of s2n-tls and Openssl that they have loaded are correct.
 *
 * @returns the version number of OpenSSL that s2n-tls was compiled with 
 */
S2N_API extern unsigned long s2n_get_openssl_version(void);

/**
 * Initializes the s2n-tls library and should be called once in your application, before any other s2n-tls
 * functions are called. Failure to call s2n_init() will result in errors from other s2n-tls functions.
 *
 * @warning This function is not thread safe and should only be called once.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_init(void);

/**
 * Cleans up thread-local resources used by s2n-tls. Does not perform a full library cleanup. To fully
 * clean up the library use s2n_cleanup_final().
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cleanup(void);

/*
 * Performs a complete deinitialization and cleanup of the s2n-tls library.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cleanup_final(void);

typedef enum {
    S2N_FIPS_MODE_DISABLED = 0,
    S2N_FIPS_MODE_ENABLED,
} s2n_fips_mode;

/**
 * Determines whether s2n-tls is operating in FIPS mode.
 *
 * s2n-tls enters FIPS mode on initialization when built with a version of AWS-LC that supports
 * FIPS (https://github.com/aws/aws-lc/blob/main/crypto/fipsmodule/FIPS.md). FIPS mode controls
 * some internal configuration related to FIPS support, like which random number generator is used.
 *
 * FIPS mode does not enforce the use of FIPS-approved cryptography. Applications attempting to use
 * only FIPS-approved cryptography should also ensure that s2n-tls is configured to use a security
 * policy that only supports FIPS-approved cryptography.
 *
 * @param fips_mode Set to the FIPS mode of s2n-tls.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure.
 */
S2N_API extern int s2n_get_fips_mode(s2n_fips_mode *fips_mode);

/**
 * Creates a new s2n_config object. This object can (and should) be associated with many connection
 * objects.
 *
 * The returned config will be initialized with default system certificates in its trust store.
 *
 * The returned config should be freed with `s2n_config_free()` after it's no longer in use by any
 * connection.
 *
 * @returns A new configuration object suitable for configuring connections and associating certs
 * and keys.
 */
S2N_API extern struct s2n_config *s2n_config_new(void);

/**
 * Creates a new s2n_config object with minimal default options.
 *
 * This function has better performance than `s2n_config_new()` because it does not load default
 * system certificates into the trust store by default. To add system certificates to this config,
 * call `s2n_config_load_system_certs()`.
 *
 * The returned config should be freed with `s2n_config_free()` after it's no longer in use by any
 * connection.
 *
 * @returns A new configuration object suitable for configuring connections and associating certs
 * and keys.
 */
S2N_API extern struct s2n_config *s2n_config_new_minimal(void);

/**
 * Frees the memory associated with an `s2n_config` object.
 *
 * @param config The configuration object being freed
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_free(struct s2n_config *config);

/**
 * Frees the DH params associated with an `s2n_config` object.
 *
 * @param config The configuration object with DH params being freed
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_free_dhparams(struct s2n_config *config);

/**
 * Frees the certificate chain and key associated with an `s2n_config` object.
 *
 * @param config The configuration object with DH params being freed
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_free_cert_chain_and_key(struct s2n_config *config);

/**
 * Callback function type used to get the system time.
 *
 * @param void* A pointer to arbitrary data for use within the callback
 * @param uint64_t* A pointer that the callback will set to the time in nanoseconds
 * The function should return 0 on success and -1 on failure.
 */
typedef int (*s2n_clock_time_nanoseconds)(void *, uint64_t *);

/**
 * Cache callback function that allows the caller to retrieve SSL session data
 * from a cache. 
 *
 * The callback function takes six arguments:
 * a pointer to the s2n_connection object, 
 * a pointer to arbitrary data for use within the callback, 
 * a pointer to a key which can be used to retrieve the cached entry, 
 * a 64 bit unsigned integer specifying the size of this key, 
 * a pointer to a memory location where the value should be stored, 
 * and a pointer to a 64 bit unsigned integer specifying the size of this value. 
 *
 * Initially *value_size will be set to the amount of space allocated for the value,
 * the callback should set *value_size to the actual size of the data returned. 
 * If there is insufficient space, -1 should be returned. 
 * If the cache is not ready to provide data for the request, 
 * S2N_CALLBACK_BLOCKED should be returned. 
 *
 * This will cause s2n_negotiate() to return S2N_BLOCKED_ON_APPLICATION_INPUT.
 */
typedef int (*s2n_cache_retrieve_callback)(struct s2n_connection *conn, void *, const void *key, uint64_t key_size, void *value, uint64_t *value_size);

/**
 * Cache callback function that allows the caller to store SSL session data in a
 * cache. 
 *
 * The callback function takes seven arguments:
 * a pointer to the s2n_connection object, 
 * a pointer to arbitrary data for use within the callback, 
 * a 64-bit unsigned integer specifying the number of seconds the session data may be stored for, 
 * a pointer to a key which can be used to retrieve the cached entry,
 * a 64 bit unsigned integer specifying the size of this key, 
 * a pointer to a value which should be stored, 
 * and a 64 bit unsigned integer specified the size of this value.
 */
typedef int (*s2n_cache_store_callback)(struct s2n_connection *conn, void *, uint64_t ttl_in_seconds, const void *key, uint64_t key_size, const void *value, uint64_t value_size);

/**
*  Cache callback function that allows the caller to set a callback function 
*  that will be used to delete SSL session data from a cache. 
*
*  The callback function takes four arguments:
*  a pointer to s2n_connection object,
*  a pointer to arbitrary data for use within the callback, 
*  a pointer to a key which can be used to delete the cached entry, 
*  and a 64 bit unsigned integer specifying the size of this key.
*/
typedef int (*s2n_cache_delete_callback)(struct s2n_connection *conn, void *, const void *key, uint64_t key_size);

/** 
 * Allows the caller to set a callback function that will be used to get the
 * system time. The time returned should be the number of nanoseconds since the
 * Unix epoch (Midnight, January 1st, 1970).
 *
 * s2n-tls uses this clock for timestamps.
 *
 * @param config The configuration object being updated
 * @param clock_fn The wall clock time callback function
 * @param ctx An opaque pointer that the callback will be invoked with
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_wall_clock(struct s2n_config *config, s2n_clock_time_nanoseconds clock_fn, void *ctx);

/** 
 * Allows the caller to set a callback function that will be used to get 
 * monotonic time. The monotonic time is the time since an arbitrary, unspecified
 * point. Unlike wall clock time, it MUST never move backwards.
 *
 * s2n-tls uses this clock for timers.
 *
 * @param config The configuration object being updated
 * @param clock_fn The monotonic time callback function
 * @param ctx An opaque pointer that the callback will be invoked with
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_monotonic_clock(struct s2n_config *config, s2n_clock_time_nanoseconds clock_fn, void *ctx);

/**
 * Translates an s2n_error code to a human readable string explaining the error.
 *
 * @param error The error code to explain. Usually this is s2n_errno
 * @param lang The language to explain the error code. Pass "EN" or NULL for English.
 * @returns The error string
 */
S2N_API extern const char *s2n_strerror(int error, const char *lang);

/**
 * Translates an s2n_error code to a human readable string containing internal debug
 * information, including file name and line number. This function is useful when
 * reporting issues to the s2n-tls development team.
 *
 * @param error The error code to explain. Usually this is s2n_errno
 * @param lang The language to explain the error code. Pass "EN" or NULL for English.
 * @returns The error string
 */
S2N_API extern const char *s2n_strerror_debug(int error, const char *lang);

/**
 * Translates an s2n_error code to a human readable string.
 *
 * @param error The error code to explain. Usually this is s2n_errno
 * @returns The error string
 */
S2N_API extern const char *s2n_strerror_name(int error);

/**
 * Translates an s2n_error code to a filename and line number.
 *
 * @param error The error code to explain. Usually this is s2n_errno.
 * @returns The error string.
 */
S2N_API extern const char *s2n_strerror_source(int error);

/**
 * Opaque stack trace structure.
 */
struct s2n_stacktrace;

/**
 * Checks if s2n stack trace captures are enabled.
 *
 * @returns True if stack traces are enabled. False if they are disabled.
 */
S2N_API extern bool s2n_stack_traces_enabled(void);

/**
 * Configures the s2n stack trace captures option.
 *
 * @param newval Boolean to determine if stack traces should be enabled. True to enable them. False to disable them.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_stack_traces_enabled_set(bool newval);

/**
 * Calculates the s2n stack trace.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_calculate_stacktrace(void);

/**
 * Prints the s2n stack trace to a file. The file descriptor is expected to be
 * open and ready for writing.
 *
 * @param fptr A pointer to the file s2n-tls should write the stack trace to.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_print_stacktrace(FILE *fptr);

/**
 * Clean up the memory used to contain the stack trace.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_free_stacktrace(void);

/**
 * Export the s2n_stacktrace.
 *
 * @param trace A pointer to the s2n_stacktrace to fill.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_get_stacktrace(struct s2n_stacktrace *trace);

/**
 * Allows the caller to set a callback function that will be used to store SSL
 * session data in a cache.
 *
 * @param config The configuration object being updated
 * @param cache_store_callback The cache store callback function.
 * @param data An opaque context pointer that the callback will be invoked with.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_cache_store_callback(struct s2n_config *config, s2n_cache_store_callback cache_store_callback, void *data);

/**
 * Allows the caller to set a callback function that will be used to retrieve SSL
 * session data from a cache.
 *
 * @param config The configuration object being updated
 * @param cache_retrieve_callback The cache retrieve callback function.
 * @param data An opaque context pointer that the callback will be invoked with.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_cache_retrieve_callback(struct s2n_config *config, s2n_cache_retrieve_callback cache_retrieve_callback, void *data);

/**
 * Allows the caller to set a callback function that will be used to delete SSL
 * session data from a cache.
 *
 * @param config The configuration object being updated
 * @param cache_delete_callback The cache delete callback function.
 * @param data An opaque context pointer that the callback will be invoked with.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_cache_delete_callback(struct s2n_config *config, s2n_cache_delete_callback cache_delete_callback, void *data);

/**
 * Called when `s2n_init` is executed.
 */
typedef int (*s2n_mem_init_callback)(void);

/**
 * Will be called when `s2n_cleanup` is executed.
 */
typedef int (*s2n_mem_cleanup_callback)(void);

/**
 * A function that can allocate at least `requested` bytes of memory.
 *
 * It stores the location of that memory in **\*ptr** and the size of the allocated
 * data in **\*allocated**. The function may choose to allocate more memory
 * than was requested. s2n-tls will consider all allocated memory available for
 * use, and will attempt to free all allocated memory when able.
 */
typedef int (*s2n_mem_malloc_callback)(void **ptr, uint32_t requested, uint32_t *allocated);

/**
 * Frees memory allocated by s2n_mem_malloc_callback.
 */
typedef int (*s2n_mem_free_callback)(void *ptr, uint32_t size);

/**
 * Allows the caller to override s2n-tls's internal memory handling functions.
 * 
 * @warning This function must be called before s2n_init().
 * 
 * @param mem_init_callback The s2n_mem_init_callback
 * @param mem_cleanup_callback The s2n_mem_cleanup_callback
 * @param mem_malloc_callback The s2n_mem_malloc_callback
 * @param mem_free_callback The s2n_mem_free_callback
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_mem_set_callbacks(s2n_mem_init_callback mem_init_callback, s2n_mem_cleanup_callback mem_cleanup_callback,
        s2n_mem_malloc_callback mem_malloc_callback, s2n_mem_free_callback mem_free_callback);

/**
 * A callback function that will be called when s2n-tls is initialized.
 */
typedef int (*s2n_rand_init_callback)(void);

/**
 * A callback function that will be called when `s2n_cleanup` is executed.
 */
typedef int (*s2n_rand_cleanup_callback)(void);

/**
 * A callback function that will be used to provide entropy to the s2n-tls
 * random number generators.
 */
typedef int (*s2n_rand_seed_callback)(void *data, uint32_t size);

/**
 * A callback function that will be used to mix in entropy every time the RNG
 * is invoked.
 */
typedef int (*s2n_rand_mix_callback)(void *data, uint32_t size);

/**
 * Allows the caller to override s2n-tls's entropy functions.
 * 
 * @warning This function must be called before s2n_init().
 *
 * @note The overriden random callbacks will not be used when s2n-tls is operating in FIPS mode.
 * 
 * @param rand_init_callback The s2n_rand_init_callback
 * @param rand_cleanup_callback The s2n_rand_cleanup_callback
 * @param rand_seed_callback The s2n_rand_seed_callback
 * @param rand_mix_callback The s2n_rand_mix_callback
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_rand_set_callbacks(s2n_rand_init_callback rand_init_callback, s2n_rand_cleanup_callback rand_cleanup_callback,
        s2n_rand_seed_callback rand_seed_callback, s2n_rand_mix_callback rand_mix_callback);

/**
 * TLS extensions supported by s2n-tls
 */
typedef enum {
    S2N_EXTENSION_SERVER_NAME = 0,
    S2N_EXTENSION_MAX_FRAG_LEN = 1,
    S2N_EXTENSION_OCSP_STAPLING = 5,
    S2N_EXTENSION_SUPPORTED_GROUPS = 10,
    S2N_EXTENSION_EC_POINT_FORMATS = 11,
    S2N_EXTENSION_SIGNATURE_ALGORITHMS = 13,
    S2N_EXTENSION_ALPN = 16,
    S2N_EXTENSION_CERTIFICATE_TRANSPARENCY = 18,
    S2N_EXTENSION_SUPPORTED_VERSIONS = 43,
    S2N_EXTENSION_RENEGOTIATION_INFO = 65281,
} s2n_tls_extension_type;

/**
 * MFL configurations from https://datatracker.ietf.org/doc/html/rfc6066#section-4.
 */
typedef enum {
    S2N_TLS_MAX_FRAG_LEN_512 = 1,
    S2N_TLS_MAX_FRAG_LEN_1024 = 2,
    S2N_TLS_MAX_FRAG_LEN_2048 = 3,
    S2N_TLS_MAX_FRAG_LEN_4096 = 4,
} s2n_max_frag_len;

/**
 * Opaque certificate type.
 */
struct s2n_cert;

/**
 * Opaque certificate chain and key type.
 */
struct s2n_cert_chain_and_key;

/**
 * Opaque key type.
 */
struct s2n_pkey;

/**
 * Opaque public key type.
 */
typedef struct s2n_pkey s2n_cert_public_key;

/**
 * Opaque private key type.
 */
typedef struct s2n_pkey s2n_cert_private_key;

/**
 * Creates a new s2n_cert_chain_and_key object. This object can be associated
 * with many config objects. It is used to represent a certificate and key pair.
 *
 * @returns A new object used to represent a certificate-chain/key pair
 */
S2N_API extern struct s2n_cert_chain_and_key *s2n_cert_chain_and_key_new(void);

/**
 * Associates a certificate chain and private key with an `s2n_cert_chain_and_key` object.
 * 
 * `cert_chain_pem` should be a PEM encoded certificate chain, with the first
 * certificate in the chain being your leaf certificate. `private_key_pem`
 * should be a PEM encoded private key corresponding to the leaf certificate.
 *
 * @note Prefer using s2n_cert_chain_and_key_load_pem_bytes.
 *
 * @param chain_and_key The certificate chain and private key handle
 * @param chain_pem A byte array of a PEM encoded certificate chain.
 * @param private_key_pem A byte array of a PEM encoded key.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cert_chain_and_key_load_pem(struct s2n_cert_chain_and_key *chain_and_key, const char *chain_pem, const char *private_key_pem);

/**
 * Associates a certificate chain and private key with an `s2n_cert_chain_and_key` object.
 * 
 * `cert_chain_pem` should be a PEM encoded certificate chain, with the first
 * certificate in the chain being your leaf certificate. `private_key_pem`
 * should be a PEM encoded private key corresponding to the leaf certificate.
 *
 * @param chain_and_key The certificate chain and private key handle
 * @param chain_pem A byte array of a PEM encoded certificate chain.
 * @param chain_pem_len Size of `chain_pem`
 * @param private_key_pem A byte array of a PEM encoded key.
 * @param private_key_pem_len Size of `private_key_pem`
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cert_chain_and_key_load_pem_bytes(struct s2n_cert_chain_and_key *chain_and_key, uint8_t *chain_pem, uint32_t chain_pem_len, uint8_t *private_key_pem, uint32_t private_key_pem_len);

/**
 * Associates a public certificate chain with a `s2n_cert_chain_and_key` object. It does
 * NOT set a private key, so the connection will need to be configured to
 * [offload private key operations](https://github.com/aws/s2n-tls/blob/main/docs/usage-guide/topics/ch12-private-key-ops.md).
 *
 * @param chain_and_key The certificate chain and private key handle
 * @param chain_pem A byte array of a PEM encoded certificate chain.
 * @param chain_pem_len Size of `chain_pem`
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cert_chain_and_key_load_public_pem_bytes(struct s2n_cert_chain_and_key *chain_and_key, uint8_t *chain_pem, uint32_t chain_pem_len);

/**
 * Frees the memory associated with an `s2n_cert_chain_and_key` object.
 *
 * @param cert_and_key The certificate chain and private key handle
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cert_chain_and_key_free(struct s2n_cert_chain_and_key *cert_and_key);

/**
 * Adds a context to the `s2n_cert_chain_and_key` object.
 *
 * @param cert_and_key The certificate chain and private key handle
 * @param ctx An opaque pointer to user supplied data.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cert_chain_and_key_set_ctx(struct s2n_cert_chain_and_key *cert_and_key, void *ctx);

/**
 * Get the user supplied context from the `s2n_cert_chain_and_key` object.
 *
 * @param cert_and_key The certificate chain and private key handle
 * @returns The user supplied pointer from s2n_cert_chain_and_key_set_ctx()
 */
S2N_API extern void *s2n_cert_chain_and_key_get_ctx(struct s2n_cert_chain_and_key *cert_and_key);

/**
 * Get the private key from the `s2n_cert_chain_and_key` object.
 *
 * @param cert_and_key The certificate chain and private key handle
 * @returns A pointer to the `s2n_cert_private_key`
 */
S2N_API extern s2n_cert_private_key *s2n_cert_chain_and_key_get_private_key(struct s2n_cert_chain_and_key *cert_and_key);

/**
 * Set the raw OCSP stapling data for a certificate chain.
 *
 * @param chain_and_key The certificate chain handle
 * @param data A pointer to the raw OCSP stapling data bytes. The data will be copied.
 * @param length The length of the data bytes.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cert_chain_and_key_set_ocsp_data(struct s2n_cert_chain_and_key *chain_and_key, const uint8_t *data, uint32_t length);

/**
 * Set the signed certificate timestamp (SCT) for a certificate chain.
 * This is used for Certificate Transparency.
 *
 * @param chain_and_key The certificate chain handle
 * @param data A pointer to the SCT data. The data will be copied.
 * @param length The length of the data bytes.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_cert_chain_and_key_set_sct_list(struct s2n_cert_chain_and_key *chain_and_key, const uint8_t *data, uint32_t length);

/**
 * A callback function that is invoked if s2n-tls cannot resolve a conflict between 
 * two certificates with the same domain name. This function is invoked while certificates
 * are added to an `s2n_config`. 
 *
 * Currently, the only builtin resolution for domain name conflicts is certificate type(RSA, 
 * ECDSA, etc). The callback should return a pointer to the `s2n_cert_chain_and_key` that
 * should be used for dns name `name`. 
 *
 * If NULL is returned, the first certificate will be used. Typically an application
 * will use properties like trust and expiry to implement tiebreaking.
 */
typedef struct s2n_cert_chain_and_key *(*s2n_cert_tiebreak_callback)(struct s2n_cert_chain_and_key *cert1, struct s2n_cert_chain_and_key *cert2, uint8_t *name, uint32_t name_len);

/**
 * Sets the `s2n_cert_tiebreak_callback` for resolving domain name conflicts. 
 * If no callback is set, the first certificate added for a domain name will always be preferred.
 *
 * @param config The configuration object being updated
 * @param cert_tiebreak_cb The pointer to the certificate tiebreak function
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_cert_tiebreak_callback(struct s2n_config *config, s2n_cert_tiebreak_callback cert_tiebreak_cb);

/**
 * Associates a certificate chain and private key with an `s2n_config` object.
 * Using this API, only one cert chain of each type (like ECDSA or RSA) may be associated with a config.
 * `cert_chain_pem` should be a PEM encoded certificate chain, with the first certificate 
 * in the chain being your server's certificate. `private_key_pem` should be a
 * PEM encoded private key corresponding to the server certificate.
 *
 * @deprecated Use s2n_config_add_cert_chain_and_key_to_store instead.
 *
 * @param config The configuration object being updated
 * @param cert_chain_pem A byte array of a PEM encoded certificate chain.
 * @param private_key_pem A byte array of a PEM encoded key.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure.
 */
S2N_API extern int s2n_config_add_cert_chain_and_key(struct s2n_config *config, const char *cert_chain_pem, const char *private_key_pem);

/**
 * The preferred method of associating a certificate chain and private key pair with an `s2n_config` object.
 * This method may be called multiple times to support multiple key types (RSA, RSA-PSS, ECDSA) and multiple
 * domains. On the server side, the certificate selected will be based on the incoming SNI value and the
 * client's capabilities (supported ciphers).
 *
 * In the case of no certificate matching the client's SNI extension or if no SNI extension was sent by
 * the client, the certificate from the `first` call to `s2n_config_add_cert_chain_and_key_to_store()`
 * will be selected. Use `s2n_config_set_cert_chain_and_key_defaults()` to set different defaults.
 * 
 * @warning It is not recommended to free or modify the `cert_key_pair` as any subsequent changes will be
 * reflected in the config. 
 *
 * @param config The configuration object being updated
 * @param cert_key_pair The certificate chain and private key handle
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_add_cert_chain_and_key_to_store(struct s2n_config *config, struct s2n_cert_chain_and_key *cert_key_pair);

/**
 * Explicitly sets certificate chain and private key pairs to be used as defaults for each auth
 * method (key type). A "default" certificate is used when there is not an SNI match with any other
 * configured certificate.
 *
 * Only one certificate can be set as the default per auth method (one RSA default, one ECDSA default,
 * etc.). All previous default certificates will be cleared and re-set when this API is called.
 *
 * This API is called for a specific `s2n_config` object. s2n-tls will attempt to automatically choose
 * default certificates for each auth method (key type) based on the order that `s2n_cert_chain_and_key`
 * are added to the `s2n_config` using one of the APIs listed above.
 * `s2n_config_set_cert_chain_and_key_defaults` can be called at any time; s2n-tls will clear defaults
 * and no longer attempt to automatically choose any default certificates.
 *
 * @param config The configuration object being updated
 * @param cert_key_pairs An array of certificate chain and private key handles
 * @param num_cert_key_pairs The amount of handles in cert_key_pairs
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_cert_chain_and_key_defaults(struct s2n_config *config,
        struct s2n_cert_chain_and_key **cert_key_pairs, uint32_t num_cert_key_pairs);

/**
 * Adds to the trust store from a CA file or directory containing trusted certificates.
 *
 * When configs are created with `s2n_config_new()`, the trust store is initialized with default
 * system certificates. To completely override these certificates, call
 * `s2n_config_wipe_trust_store()` before calling this function.
 *
 * @note The trust store will be initialized with the common locations for the host
 * operating system by default.
 *
 * @warning This API uses the PEM parsing implementation from the linked libcrypto. This
 * implementation will typically make a best-effort attempt to parse all of the certificates in the
 * provided file or directory. This permissive approach may silently ignore malformed certificates,
 * leading to possible connection failures if a certificate was expected to exist in the trust
 * store but was skipped while parsing. As such, this API should only be used on PEMs that are
 * known to be well-formed and parsable with the linked libcrypto, such as the system trust store.
 * For all other PEMs, `s2n_config_add_pem_to_trust_store()` should be used instead, which parses
 * more strictly.
 *
 * @param config The configuration object being updated
 * @param ca_pem_filename A string for the file path of the CA PEM file.
 * @param ca_dir A string for the directory of the CA PEM files.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_verification_ca_location(struct s2n_config *config, const char *ca_pem_filename, const char *ca_dir);

/**
 * Adds a PEM to the trust store. This will allocate memory, and load `pem` into the trust store.
 *
 * When configs are created with `s2n_config_new()`, the trust store is initialized with default
 * system certificates. To completely override these certificates, call
 * `s2n_config_wipe_trust_store()` before calling this function.
 *
 * @note This API uses the s2n-tls PEM parsing implementation, which is more strict than typical
 * libcrypto implementations such as OpenSSL. An error is returned if any unexpected data is
 * encountered while parsing `pem`. This allows applications to be made aware of any malformed
 * certificates rather than attempt to negotiate with a partial trust store. However, some PEMs may
 * need to be loaded that are not under control of the application, such as system trust stores. In
 * this case, `s2n_config_set_verification_ca_location()` may be used, which performs more widely
 * compatible and permissive parsing from the linked libcrypto.
 *
 * @param config The configuration object being updated
 * @param pem The string value of the PEM certificate.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_add_pem_to_trust_store(struct s2n_config *config, const char *pem);

/**
 * Clears the trust store of all certificates.
 *
 * When configs are created with `s2n_config_new()`, the trust store is initialized with default
 * system certificates. To completely override these certificates, call this function before
 * functions like `s2n_config_set_verification_ca_location()` or
 * `s2n_config_add_pem_to_trust_store()`.
 *
 * @param config The configuration object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_wipe_trust_store(struct s2n_config *config);

/**
 * Loads default system certificates into the trust store.
 *
 * `s2n_config_new_minimal()` doesn't load default system certificates into the config's trust
 * store by default. If `config` was created with `s2n_config_new_minimal`, this function can be
 * used to load system certificates into the trust store.
 *
 * @note This API will error if called on a config that has already loaded system certificates
 * into its trust store, which includes all configs created with `s2n_config_new()`.
 *
 * @param config The configuration object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_load_system_certs(struct s2n_config *config);

typedef enum {
    S2N_VERIFY_AFTER_SIGN_DISABLED,
    S2N_VERIFY_AFTER_SIGN_ENABLED
} s2n_verify_after_sign;

/**
 * Toggle whether generated signatures are verified before being sent.
 *
 * Although signatures produced by the underlying libcrypto should always be valid,
 * hardware faults, bugs in the signing implementation, or other uncommon factors
 * can cause unexpected mistakes in the final signatures. Because these mistakes
 * can leak information about the private key, applications with low trust in their
 * hardware or libcrypto may want to verify signatures before sending them.
 *
 * However, this feature will significantly impact handshake latency.
 * Additionally, most libcrypto implementations already check for common errors in signatures.
 */
S2N_API extern int s2n_config_set_verify_after_sign(struct s2n_config *config, s2n_verify_after_sign mode);

/** 
 * Set a custom send buffer size.
 *
 * This buffer is used to stage records for sending. By default,
 * enough memory is allocated to hold a single record of the maximum
 * size configured for the connection. With the default fragment size,
 * that is about 8K bytes.
 *
 * Less memory can be allocated for the send buffer, but this will result in
 * smaller, more fragmented records and increased overhead. While the absolute
 * minimum size required is 1034 bytes, at least 2K bytes is recommended for
 * reasonable record sizes.
 *
 * More memory can be allocated for the send buffer. This will result in s2n-tls
 * buffering multiple records before sending them, reducing system write calls.
 * At least 17K bytes is recommended for this use case, or at least 35K bytes
 * if larger fragment sizes are used via `s2n_connection_prefer_throughput()`.
 *
 * @param config The configuration object being updated
 * @param size The desired custom buffer size.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_send_buffer_size(struct s2n_config *config, uint32_t size);

/**
 * Enable or disable receiving of multiple TLS records in a single s2n_recv call
 *
 * By default, s2n-tls returns from s2n_recv() after reading a single TLS record.
 * Enabling receiving of multiple records will instead cause s2n_recv() to attempt
 * to read until the application-provided output buffer is full. This may be more
 * efficient, especially if larger receive buffers are used.
 *
 * @note If this option is enabled with blocking IO, the call to s2n_recv() will
 * not return until either the application-provided output buffer is full or the
 * peer closes the connection. This may lead to unintentionally long waits if the
 * peer does not send enough data.
 *
 * @param config The configuration object being updated
 * @param enabled Set to `true` if multiple record receive is to be enabled; `false` to disable.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_recv_multi_record(struct s2n_config *config, bool enabled);

/**
 * A callback function invoked (usually multiple times) during X.509 validation for each
 * name encountered in the leaf certificate.
 *
 * Return 1 to trust that hostname or 0 to not trust the hostname.
 *
 * If this function returns 1, then the certificate is considered trusted and that portion
 * of the X.509 validation will succeed.
 *
 * If no hostname results in a 1 being returned, the certificate will be untrusted and the
 * validation will terminate immediately.
 *
 * Data is a opaque user context set in s2n_config_set_verify_host_callback() or s2n_connection_set_verify_host_callback().
 */
typedef uint8_t (*s2n_verify_host_fn)(const char *host_name, size_t host_name_len, void *data);

/**
 * Sets the callback to use for verifying that a hostname from an X.509 certificate is trusted.
 *
 * The default behavior is to require that the hostname match the server name set with s2n_set_server_name().
 * This will likely lead to all client certificates being rejected, so the callback will need to be overriden when using
 *  client authentication.
 *
 * This change will be inherited by s2n_connections using this config. If a separate callback for different connections
 * using the same config is desired, see s2n_connection_set_verify_host_callback().
 *
 * @param config The configuration object being updated
 * @param data A user supplied opaque context to pass back to the callback
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_verify_host_callback(struct s2n_config *config, s2n_verify_host_fn, void *data);

/**
 * Toggles whether or not to validate stapled OCSP responses.
 *
 * 1 means OCSP responses will be validated when they are encountered, while 0 means this step will
 * be skipped.
 *
 * The default value is 1 if the underlying libCrypto implementation supports OCSP. 
 *
 * @param config The configuration object being updated
 * @param check_ocsp The desired OCSP response check configuration
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_check_stapled_ocsp_response(struct s2n_config *config, uint8_t check_ocsp);

/**
 * Disables timestamp validation for received certificates.
 *
 * By default, s2n-tls checks the notBefore and notAfter fields on the certificates it receives
 * during the handshake. If the current date is not within the range of these fields for any
 * certificate in the chain of trust, `s2n_negotiate()` will error. This validation is in
 * accordance with RFC 5280, section 6.1.3 a.2:
 * https://datatracker.ietf.org/doc/html/rfc5280#section-6.1.3.
 *
 * This API will disable this timestamp validation, permitting negotiation with peers that send
 * expired certificates, or certificates that are not yet considered valid.
 *
 * @warning Applications calling this API should seriously consider the security implications of
 * disabling this validation. The validity period of a certificate corresponds to the range of time
 * in which the CA is guaranteed to maintain information regarding the certificate's revocation
 * status. As such, it may not be possible to obtain accurate revocation information for
 * certificates with invalid timestamps. Applications disabling this validation MUST implement
 * some external method for limiting certificate lifetime.
 *
 * @param config The associated connection config.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API extern int s2n_config_disable_x509_time_verification(struct s2n_config *config);

/**
 * Turns off all X.509 validation during the negotiation phase of the connection. This should only
 * be used for testing or debugging purposes.
 *
 * @param config The configuration object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_disable_x509_verification(struct s2n_config *config);

/**
 * Sets the maximum allowed depth of a cert chain used for X509 validation. The default value is
 * 7. If this limit is exceeded, validation will fail if s2n_config_disable_x509_verification()
 * has not been called. 0 is an illegal value and will return an error.
 * 1 means only a root certificate will be used.
 *
 * @param config The configuration object being updated
 * @param max_depth The number of allowed certificates in the certificate chain
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_max_cert_chain_depth(struct s2n_config *config, uint16_t max_depth);

/**
 * Associates a set of Diffie-Hellman parameters with an `s2n_config` object.
 * @note `dhparams_pem` should be PEM encoded DH parameters.
 *
 * @param config The configuration object being updated
 * @param dhparams_pem A string containing the PEM encoded DH parameters.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_add_dhparams(struct s2n_config *config, const char *dhparams_pem);

/**
 * Sets the security policy that includes the cipher/kem/signature/ecc preferences and
 * protocol version.
 *
 * See the [USAGE-GUIDE.md](https://github.com/aws/s2n-tls/blob/main/docs/usage-guide) for how to use security policies.
 */
S2N_API extern int s2n_config_set_cipher_preferences(struct s2n_config *config, const char *version);

/**
 * Appends the provided application protocol to the preference list
 *
 * The data provided in `protocol` parameter will be copied into an internal buffer
 *
 * @param config The configuration object being updated
 * @param protocol A pointer to a byte array value
 * @param protocol_len The length of bytes that should be read from `protocol`. Note: this value cannot be 0, otherwise an error will be returned.
 */
S2N_API extern int s2n_config_append_protocol_preference(struct s2n_config *config, const uint8_t *protocol, uint8_t protocol_len);

/**
 * Sets the application protocol preferences on an `s2n_config` object. 
 * `protocols` is a list in order of preference, with most preferred protocol first, and of
 * length `protocol_count`. 
 *
 * When acting as an `S2N_CLIENT` the protocol list is included in the Client Hello message
 * as the ALPN extension. 
 *
 * As an `S2N_SERVER`, the list is used to negotiate a mutual application protocol with the
 * client. After the negotiation for the connection has completed, the agreed upon protocol
 * can be retrieved with s2n_get_application_protocol()
 *
 * @param config The configuration object being updated
 * @param protocols The list of preferred protocols, in order of preference
 * @param protocol_count The size of the protocols list
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_protocol_preferences(struct s2n_config *config, const char *const *protocols, int protocol_count);

/**
 * Enum used to define the type, if any, of certificate status request
 * a connection should make during the handshake. The only supported status request type is
 * OCSP, `S2N_STATUS_REQUEST_OCSP`.
*/
typedef enum {
    S2N_STATUS_REQUEST_NONE = 0,
    S2N_STATUS_REQUEST_OCSP = 1
} s2n_status_request_type;

/**
 * Sets up a connection to request the certificate status of a peer during an SSL handshake. If set
 * to S2N_STATUS_REQUEST_NONE, no status request is made.
 *
 * @note SHA-1 is the only supported hash algorithm for the `certID` field. This is different
 * from the hash algorithm used for the OCSP signature. See 
 * [RFC 6960](https://datatracker.ietf.org/doc/html/rfc6960#section-4.1.1) for more information.
 * While unlikely to be the case, if support for a different hash algorithm is required, the
 * s2n-tls validation can be disabled with `s2n_config_set_check_stapled_ocsp_response()` and the
 * response can be retrieved for manual validation with `s2n_connection_get_ocsp_response()`.
 * 
 * @param config The configuration object being updated
 * @param type The desired request status type
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_status_request_type(struct s2n_config *config, s2n_status_request_type type);

/**
 * Enum to set Certificate Transparency Support level. 
 */
typedef enum {
    S2N_CT_SUPPORT_NONE = 0,
    S2N_CT_SUPPORT_REQUEST = 1
} s2n_ct_support_level;

/**
 * Set the Certificate Transparency Support level.
 *
 * @param config The configuration object being updated
 * @param level The desired Certificate Transparency Support configuration
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_ct_support_level(struct s2n_config *config, s2n_ct_support_level level);

/**
 * Sets whether or not a connection should terminate on receiving a WARNING alert from its peer.
 *
 * `alert_behavior` can take the following values:
 * - `S2N_ALERT_FAIL_ON_WARNINGS` default behavior: s2n-tls will terminate the connection if its peer sends a WARNING alert.
 * - `S2N_ALERT_IGNORE_WARNINGS` - with the exception of `close_notify` s2n-tls will ignore all WARNING alerts and keep communicating with its peer. This setting is ignored in TLS1.3
 *
 * @note TLS1.3 terminates a connection for all alerts except user_canceled.
 * @warning S2N_ALERT_FAIL_ON_WARNINGS is the recommended behavior. Past TLS protocol vulnerabilities have involved downgrading alerts to warnings.
 */
typedef enum {
    S2N_ALERT_FAIL_ON_WARNINGS = 0,
    S2N_ALERT_IGNORE_WARNINGS = 1
} s2n_alert_behavior;

/**
 * Sets the config's alert behavior based on the `s2n_alert_behavior` enum.
 *
 * @param config The configuration object being updated
 * @param alert_behavior The desired alert behavior.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_alert_behavior(struct s2n_config *config, s2n_alert_behavior alert_behavior);

/**
 * Sets the extension data in the `s2n_config` object for the specified extension. 
 * This method will clear any existing data that is set. If the data and length
 * parameters are set to NULL, no new data is set in the `s2n_config` object,
 * effectively clearing existing data.
 *
 * @deprecated Use s2n_cert_chain_and_key_set_ocsp_data and s2n_cert_chain_and_key_set_sct_list instead.
 *
 * @param config The configuration object being updated
 * @param type The extension type
 * @param data Data for the extension
 * @param length Length of the `data` buffer
 */
S2N_API extern int s2n_config_set_extension_data(struct s2n_config *config, s2n_tls_extension_type type, const uint8_t *data, uint32_t length);

/**
 * Allows the caller to set a TLS Maximum Fragment Length extension that will be used
 * to fragment outgoing messages. s2n-tls currently does not reject fragments larger
 * than the configured maximum when in server mode. The TLS negotiated maximum fragment
 * length overrides the preference set by the `s2n_connection_prefer_throughput` and
 * `s2n_connection_prefer_low_latency`.
 *
 * @note Some TLS implementations do not respect their peer's max fragment length extension.
 *
 * @param config The configuration object being updated
 * @param mfl_code The selected MFL size
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_send_max_fragment_length(struct s2n_config *config, s2n_max_frag_len mfl_code);

/**
 * Allows the server to opt-in to accept client's TLS maximum fragment length extension
 * requests. If this API is not called, and client requests the extension, server will ignore
 * the request and continue TLS handshake with default maximum fragment length of 8k bytes
 *
 * @note Some TLS implementations do not respect their peer's max fragment length extension.
 *
 * @param config The configuration object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_accept_max_fragment_length(struct s2n_config *config);

/**
 * Sets the lifetime of the cached session state. The default value is 15 hours.
 *
 * @param config The configuration object being updated
 * @param lifetime_in_secs The desired lifetime of the session state in seconds
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_session_state_lifetime(struct s2n_config *config, uint64_t lifetime_in_secs);

/**
 * Enable or disable session resumption using session ticket.
 *
 * @param config The configuration object being updated
 * @param enabled The configuration object being updated. Set to 1 to enable. Set to 0 to disable.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_session_tickets_onoff(struct s2n_config *config, uint8_t enabled);

/**
 * Enable or disable session caching.
 * 
 * @note Session caching will not be turned on unless all three session cache callbacks are set
 * prior to calling this function.
 *
 * @param config The configuration object being updated
 * @param enabled The configuration object being updated. Set to 1 to enable. Set to 0 to disable.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_session_cache_onoff(struct s2n_config *config, uint8_t enabled);

/**
 * Sets how long a session ticket key will be in a state where it can be used for both encryption
 * and decryption of tickets on the server side.
 *
 * @note The default value is 2 hours.
 * @param config The configuration object being updated
 * @param lifetime_in_secs The desired lifetime of decrypting and encrypting tickets in seconds
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_ticket_encrypt_decrypt_key_lifetime(struct s2n_config *config, uint64_t lifetime_in_secs);

/**
 * Sets how long a session ticket key will be in a state where it can used just for decryption of
 * already assigned tickets on the server side. Once decrypted, the session will resume and the
 * server will issue a new session ticket encrypted using a key in encrypt-decrypt state.
 *
 * @note The default value is 13 hours.
 * @param config The configuration object being updated
 * @param lifetime_in_secs The desired lifetime of decrypting and encrypting tickets in seconds
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_ticket_decrypt_key_lifetime(struct s2n_config *config, uint64_t lifetime_in_secs);

/**
 * Adds session ticket key on the server side. It would be ideal to add new keys after every
 * (encrypt_decrypt_key_lifetime_in_nanos/2) nanos because this will allow for gradual and
 * linear transition of a key from encrypt-decrypt state to decrypt-only state.
 *
 * @param config The configuration object being updated
 * @param name Name of the session ticket key that should be randomly generated to avoid collisions
 * @param name_len Length of session ticket key name
 * @param key Key used to perform encryption/decryption of session ticket
 * @param key_len Length of the session ticket key
 * @param intro_time_in_seconds_from_epoch Time at which the session ticket key is introduced. If this is 0, then intro_time_in_seconds_from_epoch is set to now. 
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_add_ticket_crypto_key(struct s2n_config *config, const uint8_t *name, uint32_t name_len,
        uint8_t *key, uint32_t key_len, uint64_t intro_time_in_seconds_from_epoch);

/**
 * Requires that session tickets are only used when forward secrecy is possible.
 *
 * Restricts session resumption to TLS1.3, as the tickets used in TLS1.2 resumption are
 * not forward secret. Clients should not expect to receive new session tickets and servers
 * will not send new session tickets when TLS1.2 is negotiated and ticket forward secrecy is required.
 * 
 * @note The default behavior is that forward secrecy is not required.
 *
 * @param config The config object being updated
 * @param enabled Indicates if forward secrecy is required or not on tickets
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_require_ticket_forward_secrecy(struct s2n_config *config, bool enabled);

/**
 * Sets user defined context on the `s2n_config` object.
 *
 * @param config The configuration object being updated
 * @param ctx A pointer to the user defined ctx.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_ctx(struct s2n_config *config, void *ctx);

/**
 * Gets the user defined context from the `s2n_config` object.
 * The context is set by calling s2n_config_set_ctx()
 *
 * @param config The configuration object being accessed
 * @param ctx A pointer to the user defined ctx.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_get_ctx(struct s2n_config *config, void **ctx);

/**
 * Used to declare connections as server or client type, respectively.
 */
typedef enum {
    S2N_SERVER,
    S2N_CLIENT
} s2n_mode;

/**
 * Creates a new connection object. Each s2n-tls SSL/TLS connection uses
 * one of these objects. These connection objects can be operated on by up
 * to two threads at a time, one sender and one receiver, but neither sending
 * nor receiving are atomic, so if these objects are being called by multiple
 * sender or receiver threads, you must perform your own locking to ensure 
 * that only one sender or receiver is active at a time. 
 *
 * The `mode` parameters specifies if the caller is a server, or is a client.
 * Connections objects are re-usable across many connections, and should be
 * re-used (to avoid deallocating and allocating memory). You should wipe
 * connections immediately after use.
 *
 * @param mode The desired connection type
 * @returns A s2n_connection handle
 */
S2N_API extern struct s2n_connection *s2n_connection_new(s2n_mode mode);

/**
 * Associates a configuration object with a connection.
 *
 * @param conn The connection object being associated
 * @param config The configuration object being associated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_config(struct s2n_connection *conn, struct s2n_config *config);

/**
 * Sets user defined context in `s2n_connection` object.
 *
 * @param conn The connection object being updated
 * @param ctx A pointer to the user defined context
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_ctx(struct s2n_connection *conn, void *ctx);

/**
 * Gets user defined context from a `s2n_connection` object.
 *
 * @param conn The connection object that contains the desired context
 */
S2N_API extern void *s2n_connection_get_ctx(struct s2n_connection *conn);

/**
 * The callback function takes a s2n-tls connection as input, which receives the ClientHello
 * and the context previously provided in `s2n_config_set_client_hello_cb`. The callback can
 * access any ClientHello information from the connection and use the `s2n_connection_set_config`
 * call to change the config of the connection.
 */
typedef int s2n_client_hello_fn(struct s2n_connection *conn, void *ctx);

/**
 * Client Hello callback modes
 * - `S2N_CLIENT_HELLO_CB_BLOCKING` (default):
 *   - In this mode s2n-tls expects the callback to complete its work and return the appropriate response code before the handshake continues. If any of the connection properties were changed based on the server_name extension the callback must either return a value greater than 0 or invoke `s2n_connection_server_name_extension_used`, otherwise the callback returns 0 to continue the handshake.
 * - `S2N_CLIENT_HELLO_CB_NONBLOCKING`:
 *   - In non-blocking mode, s2n-tls expects the callback to not complete its work. If the callback returns a response code of 0, s2n-tls will return `S2N_FAILURE` with `S2N_ERR_T_BLOCKED` error type and `s2n_blocked_status` set to `S2N_BLOCKED_ON_APPLICATION_INPUT`. The handshake is paused and further calls to `s2n_negotiate` will continue to return the same error until `s2n_client_hello_cb_done` is invoked for the `s2n_connection` to resume the handshake. If any of the connection properties were changed on the basis of the server_name extension then `s2n_connection_server_name_extension_used` must be invoked before marking the callback done.
 */
typedef enum {
    S2N_CLIENT_HELLO_CB_BLOCKING,
    S2N_CLIENT_HELLO_CB_NONBLOCKING
} s2n_client_hello_cb_mode;

/**
 * Allows the caller to set a callback function that will be called after ClientHello was parsed.
 *
 * @param config The configuration object being updated
 * @param client_hello_callback The client hello callback function
 * @param ctx A pointer to a user defined context that the Client Hello callback will be invoked with. 
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_client_hello_cb(struct s2n_config *config, s2n_client_hello_fn client_hello_callback, void *ctx);

/**
 * Sets the callback execution mode.
 *
 * See s2n_client_hello_cb_mode for each mode's behavior.
 *
 * @param config The configuration object being updated
 * @param cb_mode The desired callback mode
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_client_hello_cb_mode(struct s2n_config *config, s2n_client_hello_cb_mode cb_mode);

/**
 * Marks the non-blocking callback as complete. Can be invoked from within the callback when
 * operating in non-blocking mode to continue the handshake.
 *
 * @param conn The connection object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_client_hello_cb_done(struct s2n_connection *conn);

/**
 * Must be invoked if any of the connection properties were changed on the basis of the server_name
 * extension. This must be invoked before marking the Client Hello callback done.
 *
 * @param conn The connection object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_server_name_extension_used(struct s2n_connection *conn);

/**
 * Opaque client hello handle
 */
struct s2n_client_hello;

/**
 * Get the Client Hello from a s2n_connection.
 *
 * Earliest point during the handshake when this structure is available for use is in the
 * client_hello_callback (see s2n_config_set_client_hello_cb()).
 *
 * @param conn The connection object containing the client hello
 * @returns A handle to the s2n_client_hello structure holding the client hello message sent by the client during the handshake. NULL is returned if a Client Hello has not yet been received and parsed.
 */
S2N_API extern struct s2n_client_hello *s2n_connection_get_client_hello(struct s2n_connection *conn);

/**
 * Creates an s2n_client_hello from bytes representing a ClientHello message.
 *
 * The input bytes should include the message header (message type and length),
 * but not the record header.
 *
 * Unlike s2n_connection_get_client_hello, the s2n_client_hello returned by this
 * method is owned by the application and must be freed with s2n_client_hello_free.
 *
 * This method does not support SSLv2 ClientHellos.
 *
 * @param bytes The raw bytes representing the ClientHello.
 * @param size The size of raw_message.
 * @returns A new s2n_client_hello on success, or NULL on failure.
 */
S2N_API extern struct s2n_client_hello *s2n_client_hello_parse_message(const uint8_t *bytes, uint32_t size);

/**
 * Frees an s2n_client_hello structure.
 *
 * This method should be called to free s2n_client_hellos returned by
 * s2n_client_hello_parse_message. It will error if passed an s2n_client_hello
 * returned by s2n_connection_get_client_hello and owned by the connection.
 *
 * @param ch The structure to be freed.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API extern int s2n_client_hello_free(struct s2n_client_hello **ch);

/**
 * Function to determine the size of the raw Client Hello buffer. 
 *
 * Can be used to determine the necessary size of the `out` buffer for 
 * s2n_client_hello_get_raw_message()
 *
 * @param ch The Client Hello handle
 * @returns The size of the ClientHello message received by the server
 */
S2N_API extern ssize_t s2n_client_hello_get_raw_message_length(struct s2n_client_hello *ch);

/**
 * Copies `max_length` bytes of the ClientHello message into the `out` buffer.
 * The ClientHello instrumented using this function will have the Random bytes
 * zero-ed out.
 *
 * Note: SSLv2 ClientHello messages follow a different structure than more modern
 * ClientHello messages. See [RFC5246](https://tools.ietf.org/html/rfc5246#appendix-E.2).
 * In addition, due to how s2n-tls parses SSLv2 ClientHellos, the raw message is
 * missing the first three bytes (the msg_type and version) and instead begins with
 * the cipher_specs. To determine whether a ClientHello is an SSLv2 ClientHello,
 * you will need to use s2n_connection_get_client_hello_version(). To get the
 * protocol version advertised in the SSLv2 ClientHello (which may be higher
 * than SSLv2), you will need to use s2n_connection_get_client_protocol_version().
 *
 * @param ch The Client Hello handle
 * @param out The destination buffer for the raw Client Hello
 * @param max_length The size of out in bytes
 * @returns The number of copied bytes
 */
S2N_API extern ssize_t s2n_client_hello_get_raw_message(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);

/**
 * Function to determine the size of the Client Hello cipher suites.
 * This can be used to allocate the `out` buffer for s2n_client_hello_get_cipher_suites().
 *
 * @param ch The Client Hello handle
 * @returns the number of bytes the cipher_suites takes on the ClientHello message received by the server
 */
S2N_API extern ssize_t s2n_client_hello_get_cipher_suites_length(struct s2n_client_hello *ch);

/**
 * Copies into the `out` buffer `max_length` bytes of the cipher_suites on the ClientHello.
 *
 * Note: SSLv2 ClientHello cipher suites follow a different structure than modern
 * ClientHello messages. See [RFC5246](https://tools.ietf.org/html/rfc5246#appendix-E.2).
 * To determine whether a ClientHello is an SSLv2 ClientHello,
 * you will need to use s2n_connection_get_client_hello_version().
 *
 * @param ch The Client Hello handle
 * @param out The destination buffer for the raw Client Hello cipher suites
 * @param max_length The size of out in bytes
 * @returns The number of copied bytes
 */
S2N_API extern ssize_t s2n_client_hello_get_cipher_suites(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);

/**
 * Function to determine the size of the Client Hello extensions.
 * This can be used to allocate the `out` buffer for s2n_client_hello_get_extensions().
 *
 * @param ch The Client Hello handle
 * @returns the number of bytes the extensions take in the ClientHello message received by the server
 */
S2N_API extern ssize_t s2n_client_hello_get_extensions_length(struct s2n_client_hello *ch);

/**
 * Copies into the `out` buffer `max_length` bytes of the extensions in the ClientHello.
 *
 * @param ch The Client Hello handle
 * @param out The destination buffer for the raw Client Hello extensions
 * @param max_length The size of out in bytes
 * @returns The number of copied bytes
 */
S2N_API extern ssize_t s2n_client_hello_get_extensions(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);

/**
 * Query the ClientHello message received by the server. Use this function to allocate the `out` buffer for
 * other client hello extension functions.
 *
 * @param ch A pointer to the Client Hello
 * @param extension_type Indicates the desired extension
 * @returns The number of bytes the given extension type takes
 */
S2N_API extern ssize_t s2n_client_hello_get_extension_length(struct s2n_client_hello *ch, s2n_tls_extension_type extension_type);

/**
 * Copies into the `out` buffer `max_length` bytes of a given extension type on the ClientHello
 *
 * `ch` is a pointer to the `s2n_client_hello` of the `s2n_connection` which can be obtained using s2n_connection_get_client_hello(). 
 *
 * @param ch A pointer to the Client Hello
 * @param extension_type Indicates the desired extension
 * @param out A pointer to the buffer that s2n will write the client session id to. This buffer MUST be the size of `max_length`
 * @param max_length The size of `out`.
 * @returns The number of copied bytes
 */
S2N_API extern ssize_t s2n_client_hello_get_extension_by_id(struct s2n_client_hello *ch, s2n_tls_extension_type extension_type, uint8_t *out, uint32_t max_length);

/**
 * Used to check if a particular extension exists in the client hello.
 *
 * `ch` is a pointer to the `s2n_client_hello` of the `s2n_connection` which can be obtained using s2n_connection_get_client_hello(). 
 *
 * @param ch A pointer to the client hello object
 * @param extension_iana The iana value of the extension
 * @param exists A pointer that will be set to whether or not the extension exists
 */
S2N_API extern int s2n_client_hello_has_extension(struct s2n_client_hello *ch, uint16_t extension_iana, bool *exists);

/**
 * Get the the ClientHello session id length in bytes
 * 
 * `ch` is a pointer to the `s2n_client_hello` of the `s2n_connection` which can be obtained using s2n_connection_get_client_hello(). 
 *
 * @param ch A pointer to the Client Hello
 * @param out_length An out pointer. s2n will set it's value to the size of the session_id in bytes.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_client_hello_get_session_id_length(struct s2n_client_hello *ch, uint32_t *out_length);

/**
 * Copies up to `max_length` bytes of the ClientHello session_id into the `out` buffer and stores the number of copied bytes in `out_length`.
 *
 * Retrieve the session id as sent by the client in the ClientHello message. The session id on the `s2n_connection` may change later
 * when the server sends the ServerHello; see `s2n_connection_get_session_id` for how to get the final session id used for future session resumption.
 * 
 * Use s2n_client_hello_get_session_id_length() to get the the ClientHello session id length in bytes. `ch` is a pointer to the `s2n_client_hello`
 * of the `s2n_connection` which can be obtained using s2n_connection_get_client_hello(). 
 *
 * @param ch A pointer to the Client Hello
 * @param out A pointer to the buffer that s2n will write the client session id to. This buffer MUST be the size of `max_length`
 * @param out_length An out pointer. s2n will set it's value to the size of the session_id in bytes.
 * @param max_length The size of `out`.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_client_hello_get_session_id(struct s2n_client_hello *ch, uint8_t *out, uint32_t *out_length, uint32_t max_length);

/**
 * Get the length of the compression methods list sent in the Client Hello.
 *
 * @param ch A pointer to the Client Hello
 * @param out_length An out pointer. Will be set to the length of the compression methods list in bytes.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_client_hello_get_compression_methods_length(struct s2n_client_hello *ch, uint32_t *out_length);

/**
 * Retrieves the list of compression methods sent in the Client Hello.
 *
 * Use `s2n_client_hello_get_compression_methods_length()`
 * to retrieve how much memory should be allocated for the buffer in advance.
 *
 * @note Compression methods were removed in TLS1.3 and therefore the only valid value in this list is the
 * "null" compression method when TLS1.3 is negotiated.
 *
 * @note s2n-tls has never supported compression methods in any TLS version and therefore a
 * compression method will never be negotiated or used.
 * 
 * @param ch A pointer to the Client Hello
 * @param list A pointer to some memory that s2n will write the compression methods to. This memory MUST be the size of `list_length`
 * @param list_length The size of `list`.
 * @param out_length An out pointer. s2n will set its value to the size of the compression methods list in bytes.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_client_hello_get_compression_methods(struct s2n_client_hello *ch, uint8_t *list, uint32_t list_length, uint32_t *out_length);

/**
 * Access the Client Hello protocol version
 *
 * @note This field is a legacy field in TLS1.3 and is no longer used to negotiate the
 * protocol version of the connection. It will be set to TLS1.2 even if TLS1.3 is negotiated.
 * Therefore this method should only be used for logging or fingerprinting.
 *
 * @param ch A pointer to the client hello struct
 * @param out The protocol version in the client hello.
 */
S2N_API extern int s2n_client_hello_get_legacy_protocol_version(struct s2n_client_hello *ch, uint8_t *out);

/**
 * Retrieves the supported groups received from the client in the supported groups extension.
 *
 * IANA values for each of the received supported groups are written to the provided `groups`
 * array, and `groups_count` is set to the number of received supported groups.
 *
 * `groups_count_max` should be set to the maximum capacity of the `groups` array. If
 * `groups_count_max` is less than the number of received supported groups, this function will
 * error. To determine how large `groups` should be in advance, use
 * `s2n_client_hello_get_extension_length()` with the S2N_EXTENSION_SUPPORTED_GROUPS extension
 * type, and divide the value by 2.
 *
 * If no supported groups extension was received from the peer, or the received supported groups
 * extension is malformed, this function will error.
 *
 * @param ch A pointer to the ClientHello. Can be retrieved from a connection via
 * `s2n_connection_get_client_hello()`.
 * @param groups The array to populate with the received supported groups.
 * @param groups_count_max The maximum number of supported groups that can fit in the `groups` array.
 * @param groups_count Returns the number of received supported groups.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure.
 */
S2N_API extern int s2n_client_hello_get_supported_groups(struct s2n_client_hello *ch, uint16_t *groups,
        uint16_t groups_count_max, uint16_t *groups_count);

/**
 * Gets the length of the first server name in a Client Hello.
 *
 * @param ch A pointer to the ClientHello
 * @param length A pointer which will be populated with the length of the server name
 */
S2N_API extern int s2n_client_hello_get_server_name_length(struct s2n_client_hello *ch, uint16_t *length);

/**
 * Gets the first server name in a Client Hello.
 *
 * Use `s2n_client_hello_get_server_name_length()` to get the amount of memory needed for the buffer.
 *
 * @param ch A pointer to the ClientHello
 * @param server_name A pointer to the memory which will be populated with the server name
 * @param length The maximum amount of data that can be written to `server_name`
 * @param out_length A pointer which will be populated with the size of the server name
 */
S2N_API extern int s2n_client_hello_get_server_name(struct s2n_client_hello *ch, uint8_t *server_name, uint16_t length, uint16_t *out_length);

/**
 * Sets the file descriptor for a s2n connection.
 *
 * @warning If the read end of the pipe is closed unexpectedly, writing to the pipe will raise a SIGPIPE signal.
 * **s2n-tls does NOT handle SIGPIPE.** A SIGPIPE signal will cause the process to terminate unless it is handled
 * or ignored by the application.
 * @note This file-descriptor should be active and connected
 * @param conn A pointer to the s2n connection
 * @param fd The new file descriptor
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_fd(struct s2n_connection *conn, int fd);

/**
 * Sets the file descriptor for the read channel of an s2n connection.
 *
 * @warning If the read end of the pipe is closed unexpectedly, writing to the pipe will raise a SIGPIPE signal.
 * **s2n-tls does NOT handle SIGPIPE.** A SIGPIPE signal will cause the process to terminate unless it is handled
 * or ignored by the application.
 * @note This file-descriptor should be active and connected
 * @param conn A pointer to the s2n connection
 * @param readfd The new read file descriptor
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_read_fd(struct s2n_connection *conn, int readfd);

/**
 * Sets the assigned file descriptor for the write channel of an s2n connection.
 *
 * @note This file-descriptor should be active and connected
 * @param conn A pointer to the s2n connection
 * @param writefd The new write file descriptor
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_write_fd(struct s2n_connection *conn, int writefd);

/**
 * Gets the assigned file descriptor for the read channel of an s2n connection.
 *
 * @param conn A pointer to the s2n connection
 * @param readfd pointer to place the used file descriptor.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_get_read_fd(struct s2n_connection *conn, int *readfd);

/**
 * Gets the assigned file descriptor for the write channel of an s2n connection.
 *
 * @param conn A pointer to the s2n connection
 * @param writefd pointer to place the used file descriptor.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_get_write_fd(struct s2n_connection *conn, int *writefd);

/**
 * Indicates to s2n that the connection is using corked IO.
 *
 * @warning This API should only be used when using managed send IO.
 *
 * @param conn The connection object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_use_corked_io(struct s2n_connection *conn);

/**
 * Function pointer for a user provided send callback.
 */
typedef int s2n_recv_fn(void *io_context, uint8_t *buf, uint32_t len);

/**
 * Function pointer for a user provided send callback.
 */
typedef int s2n_send_fn(void *io_context, const uint8_t *buf, uint32_t len);

/**
 * Set a context containing anything needed in the recv callback function (for example,
 * a file descriptor), the buffer holding data to be sent or received, and the length of the buffer. 
 *
 * @note The `io_context` passed to the callbacks may be set separately using `s2n_connection_set_recv_ctx` and `s2n_connection_set_send_ctx`.
 *
 * @param conn The connection object being updated
 * @param ctx A user provided context that the callback will be invoked with
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_recv_ctx(struct s2n_connection *conn, void *ctx);

/**
 * Set a context containing anything needed in the send callback function (for example,
 * a file descriptor), the buffer holding data to be sent or received, and the length of the buffer. 
 *
 * @note The `io_context` passed to the callbacks may be set separately using `s2n_connection_set_recv_ctx` and `s2n_connection_set_send_ctx`.
 *
 * @param conn The connection object being updated
 * @param ctx A user provided context that the callback will be invoked with
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_send_ctx(struct s2n_connection *conn, void *ctx);

/**
 * Configure a connection to use a recv callback to receive data.
 *
 * @note This callback may be blocking or nonblocking.
 * @note The callback may receive less than the requested length. The function should return the number
 * of bytes received, or set errno and return an error code < 0.
 *
 * @param conn The connection object being updated
 * @param recv A recv callback function pointer
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_recv_cb(struct s2n_connection *conn, s2n_recv_fn recv);

/**
 * Configure a connection to use a send callback to send data.
 *
 * @note This callback may be blocking or nonblocking.
 * @note The callback may send less than the requested length. The function should return the
 * number of bytes sent or set errno and return an error code < 0.
 *
 * @param conn The connection object being updated
 * @param send A send callback function pointer
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_send_cb(struct s2n_connection *conn, s2n_send_fn send);

/**
 * Change the behavior of s2n-tls when sending data to prefer high throughput.
 *
 * Connections preferring throughput will use
 * large record sizes that minimize overhead.
 *
 * @param conn The connection object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_prefer_throughput(struct s2n_connection *conn);

/**
 * Change the behavior of s2n-tls when sending data to prefer low latency.
 *
 * Connections preferring low latency will be encrypted
 * using small record sizes that can be decrypted sooner by the recipient. 
 *
 * @param conn The connection object being updated
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_prefer_low_latency(struct s2n_connection *conn);

/**
 * Configure the connection to reduce potentially expensive calls to recv.
 *
 * If this setting is disabled, s2n-tls will call read twice for every TLS record,
 * which can be expensive but ensures that s2n-tls will always attempt to read the
 * exact number of bytes it requires. If this setting is enabled, s2n-tls will
 * instead reduce the number of calls to read by attempting to read as much data
 * as possible in each read call, storing the extra in the existing IO buffers.
 * This may cause it to request more data than will ever actually be available.
 *
 * There is no additional memory cost of enabling this setting. It reuses the
 * existing IO buffers.
 *
 * This setting is disabled by default. Depending on how your application detects
 * data available for reading, buffering reads may break your event loop.
 * In particular, note that:
 *
 * 1. File descriptor reads or calls to your custom s2n_recv_cb may request more
 *    data than is available. Reads must return partial data when available rather
 *    than blocking until all requested data is available.
 *
 * 2. s2n_negotiate may read and buffer application data records.
 *    You must call s2n_recv at least once after negotiation to ensure that you
 *    handle any buffered data.
 *
 * 3. s2n_recv may read and buffer more records than it parses and decrypts.
 *    You must call s2n_recv until it reports S2N_ERR_T_BLOCKED, rather than just
 *    until it reports S2N_SUCCESS.
 *
 * 4. s2n_peek reports available decrypted data. It does not report any data
 *    buffered by this feature. However, s2n_peek_buffered does report data
 *    buffered by this feature.
 *
 * 5. s2n_connection_release_buffers will not release the input buffer if it
 *    contains buffered data.
 *
 * For example: if your event loop uses `poll`, you will receive a POLLIN event
 * for your read file descriptor when new data is available. When you call s2n_recv
 * to read that data, s2n-tls reads one or more TLS records from the file descriptor.
 * If you stop calling s2n_recv before it reports S2N_ERR_T_BLOCKED, some of those
 * records may remain in s2n-tls's read buffer. If you read part of a record,
 * s2n_peek will report the remainder of that record as available. But if you don't
 * read any of a record, it remains encrypted and is not reported by s2n_peek, but
 * is still reported by s2n_peek_buffered. And because the data is buffered in s2n-tls
 * instead of in the file descriptor, another call to `poll` will NOT report any
 * more data available. Your application may hang waiting for more data.
 *
 * @warning This feature cannot be enabled for a connection that will enable kTLS for receiving.
 *
 * @warning This feature may work with blocking IO, if used carefully. Your blocking
 * IO must support partial reads (so MSG_WAITALL cannot be used). You will either
 * need to know exactly how much data your peer is sending, or will need to use
 * `s2n_peek` and `s2n_peek_buffered` rather than relying on S2N_ERR_T_BLOCKED
 * as noted in #3 above.
 *
 * @param conn The connection object being updated
 * @param enabled Set to `true` to enable, `false` to disable.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_recv_buffering(struct s2n_connection *conn, bool enabled);

/**
 * Reports how many bytes of unprocessed TLS records are buffered due to the optimization
 * enabled by `s2n_connection_set_recv_buffering`.
 *
 * `s2n_peek_buffered` is not a replacement for `s2n_peek`.
 * While `s2n_peek` reports application data that is ready for the application
 * to read with no additional processing, `s2n_peek_buffered` reports raw TLS
 * records that still need to be parsed and likely decrypted. Those records may
 * contain application data, but they may also only contain TLS control messages.
 *
 * If an application needs to determine whether there is any data left to handle
 * (for example, before calling `poll` to wait on the read file descriptor) then
 * that application must check both `s2n_peek` and `s2n_peek_buffered`.
 *
 * @param conn A pointer to the s2n_connection object
 * @returns The number of buffered encrypted bytes
 */
S2N_API extern uint32_t s2n_peek_buffered(struct s2n_connection *conn);

/**
 * Configure the connection to free IO buffers when they are not currently in use.
 *
 * This configuration can be used to minimize connection memory footprint size, at the cost
 * of more calls to alloc and free. Some of these costs can be mitigated by configuring s2n-tls
 * to use an allocator that includes thread-local caches or lock-free allocation patterns.
 *
 * @param conn The connection object being update
 * @param enabled Set to `true` if dynamic buffers are enabled; `false` if disabled
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_dynamic_buffers(struct s2n_connection *conn, bool enabled);

/**
 * Changes the behavior of s2n-tls when sending data to initially prefer records
 * small enough to fit in single ethernet frames.
 *
 * When dynamic record sizing is active, the connection sends records small enough
 * to fit in a single standard 1500 byte ethernet frame. Otherwise, the connection
 * chooses record sizes according to the configured maximum fragment length.
 *
 * Dynamic record sizing is active for the first resize_threshold bytes of a connection,
 * and is reactivated whenever timeout_threshold seconds pass without sending data.
 *
 * @param conn The connection object being updated
 * @param resize_threshold The number of bytes to send before changing the record size. Maximum 8MiB.
 * @param timeout_threshold Reset record size back to a single segment after threshold seconds of inactivity
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_dynamic_record_threshold(struct s2n_connection *conn, uint32_t resize_threshold, uint16_t timeout_threshold);

/** 
 * Sets the callback to use for verifying that a hostname from an X.509 certificate is trusted.
 *
 * The default behavior is to require that the hostname match the server name set with s2n_set_server_name(). This will
 * likely lead to all client certificates being rejected, so the callback will need to be overriden when using client authentication.
 *
 * If a single callback for different connections using the same config is desired, see s2n_config_set_verify_host_callback().
 *
 * @param conn A pointer to a s2n_connection object
 * @param host_fn A pointer to a callback function that s2n will invoke in order to verify the hostname of an X.509 certificate
 * @param data Opaque pointer to data that the verify host function will be invoked with
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_verify_host_callback(struct s2n_connection *conn, s2n_verify_host_fn host_fn, void *data);

/**
 * Used to opt-out of s2n-tls's built-in blinding. Blinding is a
 * mitigation against timing side-channels which in some cases can leak information
 * about encrypted data. By default s2n-tls will cause a thread to sleep between 10 and
 * 30 seconds whenever tampering is detected.
 * 
 * Setting the S2N_SELF_SERVICE_BLINDING option with s2n_connection_set_blinding()
 * turns off this behavior. This is useful for applications that are handling many connections
 * in a single thread. In that case, if s2n_recv() or s2n_negotiate() return an error,
 * self-service applications should call s2n_connection_get_delay() and pause
 * activity on the connection  for the specified number of nanoseconds before calling
 * close() or shutdown().
 */
typedef enum {
    S2N_BUILT_IN_BLINDING,
    S2N_SELF_SERVICE_BLINDING
} s2n_blinding;

/**
 * Used to configure s2n-tls to either use built-in blinding (set blinding to S2N_BUILT_IN_BLINDING) or 
 * self-service blinding (set blinding to S2N_SELF_SERVICE_BLINDING).
 *
 * @param conn The connection object being updated
 * @param blinding The desired blinding mode for the connection
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_blinding(struct s2n_connection *conn, s2n_blinding blinding);

/**
 * Query the connection object for the configured blinding delay.
 * @param conn The connection object being updated
 * @returns the number of nanoseconds an application using self-service blinding should pause before calling close() or shutdown().
 */
S2N_API extern uint64_t s2n_connection_get_delay(struct s2n_connection *conn);

/**
 * Configures the maximum blinding delay enforced after errors.
 *
 * Blinding protects your application from timing side channel attacks like Lucky13. While s2n-tls
 * implements other, more specific mitigations for known timing side channels, blinding is important
 * as a defense against currently unknown or unreported timing attacks.
 * 
 * Setting a maximum delay lower than the recommended default (30s) will make timing attacks against
 * your application easier. The lower you set the delay, the fewer requests and less total time an
 * attacker will require to execute an attack. If you must lower the delay for reasons such as client
 * timeouts, then you should choose the highest value practically possible to limit your risk.
 *
 * If you lower the blinding delay, you should also consider implementing monitoring and filtering
 * to detect and reject suspicious traffic that could be gathering timing information from a potential
 * side channel. Timing attacks usually involve repeatedly triggering TLS errors.
 *
 * @warning Do NOT set a lower blinding delay unless you understand the risks and have other
 * mitigations for timing side channels in place.
 *
 * @note This delay needs to be set lower than any timeouts, such as your TCP socket timeout.
 *
 * @param config The config object being updated.
 * @param seconds The maximum number of seconds that s2n-tls will delay for in the event of a
 * sensitive error.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure.
 */
S2N_API extern int s2n_config_set_max_blinding_delay(struct s2n_config *config, uint32_t seconds);

/**
 * Sets the cipher preference override for the s2n_connection. Calling this function is not necessary
 * unless you want to set the cipher preferences on the connection to something different than what is in the s2n_config.
 *
 * @param conn The connection object being updated
 * @param version The human readable string representation of the security policy version.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_cipher_preferences(struct s2n_connection *conn, const char *version);

/**
 * Used to indicate the type of key update that is being requested. For further 
 * information refer to `s2n_connection_request_key_update`.
*/
typedef enum {
    S2N_KEY_UPDATE_NOT_REQUESTED = 0,
    S2N_KEY_UPDATE_REQUESTED
} s2n_peer_key_update;

/**
 * Signals the connection to do a key_update at the next possible opportunity. Note that the resulting key update message
 * will not be sent until `s2n_send` is called.
 * 
 * @param conn The connection object to trigger the key update on.
 * @param peer_request Indicates if a key update should also be requested 
 * of the peer. When set to `S2N_KEY_UPDATE_NOT_REQUESTED`, then only the sending
 * key of `conn` will be updated. If set to `S2N_KEY_UPDATE_REQUESTED`, then 
 * the sending key of conn will be updated AND the peer will be requested to 
 * update their sending key. Note that s2n-tls currently only supports 
 * `peer_request` being set to `S2N_KEY_UPDATE_NOT_REQUESTED` and will return
 *  S2N_FAILURE if any other value is used.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
*/
S2N_API extern int s2n_connection_request_key_update(struct s2n_connection *conn, s2n_peer_key_update peer_request);
/**
 * Appends the provided application protocol to the preference list
 *
 * The data provided in `protocol` parameter will be copied into an internal buffer
 *
 * @param conn The connection object being updated
 * @param protocol A pointer to a slice of bytes
 * @param protocol_len The length of bytes that should be read from `protocol`. Note: this value cannot be 0, otherwise an error will be returned.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_append_protocol_preference(struct s2n_connection *conn, const uint8_t *protocol, uint8_t protocol_len);

/**
 * Sets the protocol preference override for the s2n_connection. Calling this function is not necessary unless you want
 * to set the protocol preferences on the connection to something different than what is in the s2n_config.
 *
 * @param conn The connection object being updated
 * @param protocols A pointer to an array of protocol strings
 * @param protocol_count The number of protocols contained in protocols
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_protocol_preferences(struct s2n_connection *conn, const char *const *protocols, int protocol_count);

/**
 * Sets the server name for the connection.
 *
 * The provided server name will be sent by the client to the server in the
 * server_name ClientHello extension. It may be desirable for clients
 * to provide this information to facilitate secure connections to
 * servers that host multiple 'virtual' servers at a single underlying
 * network address.
 *
 * s2n-tls does not place any restrictions on the provided server name. However,
 * other TLS implementations might. Specifically, the TLS specification for the
 * server_name extension requires that it be an ASCII-encoded DNS name without a
 * trailing dot, and explicitly forbids literal IPv4 or IPv6 addresses.
 *
 * @param conn The connection object being queried
 * @param server_name A pointer to a string containing the desired server name
 * @warning `server_name` must be a NULL terminated string.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_set_server_name(struct s2n_connection *conn, const char *server_name);

/**
 * Query the connection for the selected server name.
 *
 * This can be used by a server to determine which server name the client is using. This function returns the first ServerName entry
 * in the ServerNameList sent by the client. Subsequent entries are not returned.
 *
 * @param conn The connection object being queried
 * @returns The server name associated with a connection, or NULL if none is found. 
 */
S2N_API extern const char *s2n_get_server_name(struct s2n_connection *conn);

/**
 * Query the connection for the selected application protocol.
 *
 * @param conn The connection object being queried
 * @returns The negotiated application protocol for a `s2n_connection`.  In the event of no protocol being negotiated, NULL is returned.
 */
S2N_API extern const char *s2n_get_application_protocol(struct s2n_connection *conn);

/**
 * Query the connection for a buffer containing the OCSP response.
 * 
 * @param conn The connection object being queried
 * @param length A pointer that is set to the certificate transparency response buffer's size
 * @returns A pointer to the OCSP response sent by a server during the handshake.  If no status response is received, NULL is returned.
 */
S2N_API extern const uint8_t *s2n_connection_get_ocsp_response(struct s2n_connection *conn, uint32_t *length);

/**
 * Query the connection for a buffer containing the Certificate Transparency response.
 *
 * @param conn The connection object being queried
 * @param length A pointer that is set to the certificate transparency response buffer's size
 * @returns A pointer to the certificate transparency response buffer.
 */
S2N_API extern const uint8_t *s2n_connection_get_sct_list(struct s2n_connection *conn, uint32_t *length);

/**
 * Used in non-blocking mode to indicate in which direction s2n-tls became blocked on I/O before it 
 * returned control to the caller. This allows an application to avoid retrying s2n-tls operations 
 * until I/O is possible in that direction.
 */
typedef enum {
    S2N_NOT_BLOCKED = 0,
    S2N_BLOCKED_ON_READ,
    S2N_BLOCKED_ON_WRITE,
    S2N_BLOCKED_ON_APPLICATION_INPUT,
    S2N_BLOCKED_ON_EARLY_DATA,
} s2n_blocked_status;

/**
 * Performs the initial "handshake" phase of a TLS connection and must be called before any s2n_recv() or s2n_send() calls.
 *
 * @note When using client authentication with TLS1.3, s2n_negotiate() will report a successful
 * handshake to clients before the server validates the client certificate. If the server then
 * rejects the client certificate, the client may later receive an alert while calling s2n_recv,
 * potentially after already having sent application data with s2n_send.
 *
 * See the following example for guidance on calling `s2n_negotiate()`:
 * https://github.com/aws/s2n-tls/blob/main/docs/examples/s2n_negotiate.c
 *
 * @param conn A pointer to the s2n_connection object
 * @param blocked A pointer which will be set to the blocked status if an `S2N_ERR_T_BLOCKED` error is returned.
 * @returns S2N_SUCCESS if the handshake completed. S2N_FAILURE if the handshake encountered an error or is blocked.
 */
S2N_API extern int s2n_negotiate(struct s2n_connection *conn, s2n_blocked_status *blocked);

/**
 * Writes and encrypts `size` of `buf` data to the associated connection. s2n_send() will return the number of bytes 
 * written, and may indicate a partial write. 
 *
 * @note Partial writes are possible not just for non-blocking I/O, but also for connections aborted while active. 
 * @note Unlike OpenSSL, repeated calls to s2n_send() should not duplicate the original parameters, but should 
 * update `buf` and `size` per the indication of size written.
 *
 * See the following example for guidance on calling `s2n_send()`:
 * https://github.com/aws/s2n-tls/blob/main/docs/examples/s2n_send.c
 *
 * @param conn A pointer to the s2n_connection object
 * @param buf A pointer to a buffer that s2n will write data from
 * @param size The size of buf
 * @param blocked A pointer which will be set to the blocked status if an `S2N_ERR_T_BLOCKED` error is returned.
 * @returns The number of bytes written on success, which may indicate a partial write. S2N_FAILURE on failure.
 */
S2N_API extern ssize_t s2n_send(struct s2n_connection *conn, const void *buf, ssize_t size, s2n_blocked_status *blocked);

/**
 * Works in the same way as s2n_sendv_with_offset() but with the `offs` parameter implicitly assumed to be 0.
 * Therefore in the partial write case, the caller would have to make sure that the `bufs` and `count` fields are modified in a way that takes
 * the partial writes into account.
 *
 * @param conn A pointer to the s2n_connection object
 * @param bufs A pointer to a vector of buffers that s2n will write data from.
 * @param count The number of buffers in `bufs`
 * @param blocked A pointer which will be set to the blocked status if an `S2N_ERR_T_BLOCKED` error is returned.
 * @returns The number of bytes written on success, which may indicate a partial write. S2N_FAILURE on failure.
 */
S2N_API extern ssize_t s2n_sendv(struct s2n_connection *conn, const struct iovec *bufs, ssize_t count, s2n_blocked_status *blocked);

/**
 * Works in the same way as s2n_send() except that it accepts vectorized buffers. Will return the number of bytes written, and may indicate a partial write. Partial writes are possible not just for non-blocking I/O, but also for connections aborted while active. 
 *
 * @note Partial writes are possible not just for non-blocking I/O, but also for connections aborted while active.
 *
 * @note Unlike OpenSSL, repeated calls to s2n_sendv_with_offset() should not duplicate the original parameters, but should update `bufs` and `count` per the indication of size written.
 *
 * See the following example for guidance on calling `s2n_sendv_with_offset()`:
 * https://github.com/aws/s2n-tls/blob/main/docs/examples/s2n_send.c
 *
 * @param conn A pointer to the s2n_connection object
 * @param bufs A pointer to a vector of buffers that s2n will write data from.
 * @param count The number of buffers in `bufs`
 * @param offs The write cursor offset. This should be updated as data is written. See the example code.
 * @param blocked A pointer which will be set to the blocked status if an `S2N_ERR_T_BLOCKED` error is returned.
 * @returns The number of bytes written on success, which may indicate a partial write. S2N_FAILURE on failure.
 */
S2N_API extern ssize_t s2n_sendv_with_offset(struct s2n_connection *conn, const struct iovec *bufs, ssize_t count, ssize_t offs, s2n_blocked_status *blocked);

/**
 * Decrypts and reads **size* to `buf` data from the associated
 * connection. 
 * 
 * @note Unlike OpenSSL, repeated calls to `s2n_recv` should not duplicate the original parameters, but should update `buf` and `size` per the indication of size read.
 *
 * See the following example for guidance on calling `s2n_recv()`:
 * https://github.com/aws/s2n-tls/blob/main/docs/examples/s2n_recv.c
 *
 * @param conn A pointer to the s2n_connection object
 * @param buf A pointer to a buffer that s2n will place read data into.
 * @param size Size of `buf`
 * @param blocked A pointer which will be set to the blocked status if an `S2N_ERR_T_BLOCKED` error is returned.
 * @returns The number of bytes read on success. 0 if the connection was shutdown by the peer. S2N_FAILURE on failure.
 */
S2N_API extern ssize_t s2n_recv(struct s2n_connection *conn, void *buf, ssize_t size, s2n_blocked_status *blocked);

/**
 * Allows users of s2n-tls to peek inside the data buffer of an s2n-tls connection to see if there more data to be read without actually reading it. 
 *
 * This is useful when using select() on the underlying s2n-tls file descriptor with a message based application layer protocol. As a single call 
 * to s2n_recv may read all data off the underlying file descriptor, select() will be unable to tell you there if there is more application data 
 * ready for processing already loaded into the s2n-tls buffer. 
 *
 * @note can then be used to determine if s2n_recv() needs to be called before more data comes in on the raw fd
 * @param conn A pointer to the s2n_connection object
 * @returns The number of bytes that can be read from the connection
 */
S2N_API extern uint32_t s2n_peek(struct s2n_connection *conn);

/** 
 * Wipes and releases buffers and memory allocated during the TLS handshake.
 *
 * @note This function should be called after the handshake is successfully negotiated and logging or recording of handshake data is complete.
 *
 * @param conn A pointer to the s2n_connection object
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_free_handshake(struct s2n_connection *conn);

/** 
 * Wipes and free the `in` and `out` buffers associated with a connection.
 *
 * @note This function may be called when a connection is
 * in keep-alive or idle state to reduce memory overhead of long lived connections.
 *
 * @param conn A pointer to the s2n_connection object
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_release_buffers(struct s2n_connection *conn);

/** 
 * Wipes an existing connection and allows it to be reused. Erases all data associated with a connection including
 * pending reads. 
 *
 * @note This function should be called after all I/O is completed and s2n_shutdown has been called.
 * @note Reusing the same connection handle(s) is more performant than repeatedly calling s2n_connection_new() and s2n_connection_free().
 *
 * @param conn A pointer to the s2n_connection object
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_wipe(struct s2n_connection *conn);

/** 
 * Frees the memory associated with an s2n_connection
 * handle. The handle is considered invalid after `s2n_connection_free` is used.
 * s2n_connection_wipe() does not need to be called prior to this function. `s2n_connection_free` performs its own wipe
 * of sensitive data.
 *
 * @param conn A pointer to the s2n_connection object
 * @returns 0 on success. -1 on failure
 */
S2N_API extern int s2n_connection_free(struct s2n_connection *conn);

/**
 * Attempts a closure at the TLS layer. Does not close the underlying transport. This call may block in either direction.
 *
 * Unlike other TLS implementations, `s2n_shutdown` attempts a graceful shutdown by default. It will not return with success unless a close_notify alert is successfully
 * sent and received. As a result, `s2n_shutdown` may fail when interacting with a non-conformant TLS implementation.
 *
 * Once `s2n_shutdown` is complete:
 * * The s2n_connection handle cannot be used for reading for writing.
 * * The underlying transport can be closed. Most likely via `shutdown()` or `close()`.
 * * The s2n_connection handle can be freed via s2n_connection_free() or reused via s2n_connection_wipe()
 *
 * @param conn A pointer to the s2n_connection object
 * @param blocked A pointer which will be set to the blocked status if an `S2N_ERR_T_BLOCKED` error is returned.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_shutdown(struct s2n_connection *conn, s2n_blocked_status *blocked);

/**
 * Attempts to close the write side of the TLS connection.
 *
 * TLS1.3 supports closing the write side of a TLS connection while leaving the read
 * side unaffected. This feature is usually referred to as "half-close". We send
 * a close_notify alert, but do not wait for the peer to respond.
 *
 * Like `s2n_shutdown()`, this method does not affect the underlying transport.
 *
 * `s2n_shutdown_send()` may still be called for earlier TLS versions, but most
 * TLS implementations will react by immediately discarding any pending writes and
 * closing the connection.
 *
 * Once `s2n_shutdown_send()` is complete:
 * * The s2n_connection handle CANNOT be used for writing.
 * * The s2n_connection handle CAN be used for reading.
 * * The write side of the underlying transport can be closed. Most likely via `shutdown()`.
 *
 * The application should still call `s2n_shutdown()` or wait for `s2n_recv()` to
 * return 0 to indicate end-of-data before cleaning up the connection or closing
 * the read side of the underlying transport.
 *
 * @param conn A pointer to the s2n_connection object
 * @param blocked A pointer which will be set to the blocked status if an `S2N_ERR_T_BLOCKED` error is returned.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_shutdown_send(struct s2n_connection *conn, s2n_blocked_status *blocked);

/**
 * Used to declare what type of client certificate authentication to use.
 *
 * A s2n_connection will enforce client certificate authentication (mTLS) differently based on
 * the `s2n_cert_auth_type` and `s2n_mode` (client/server) of the connection, as described below.
 *
 * Server behavior:
 * - None (default): Will not request client authentication.
 * - Optional: Request the client's certificate and validate it. If no certificate is received then
 *     no validation is performed.
 * - Required: Request the client's certificate and validate it. Abort the handshake if a client
 *     certificate is not received.
 *
 * Client behavior:
 * - None: Abort the handshake if the server requests client authentication.
 * - Optional (default): Sends the client certificate if the server requests client
 *     authentication. No certificate is sent if the application hasn't provided a certificate.
 * - Required: Send the client certificate. Abort the handshake if the server doesn't request
 *     client authentication or if the application hasn't provided a certificate.
 */
typedef enum {
    S2N_CERT_AUTH_NONE,
    S2N_CERT_AUTH_REQUIRED,
    S2N_CERT_AUTH_OPTIONAL
} s2n_cert_auth_type;

/**
 * Gets Client Certificate authentication method the s2n_config object is using.
 *
 * @param config A pointer to a s2n_config object
 * @param client_auth_type A pointer to a client auth policy. This will be updated to the s2n_config value.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_get_client_auth_type(struct s2n_config *config, s2n_cert_auth_type *client_auth_type);

/**
 * Sets whether or not a Client Certificate should be required to complete the TLS Connection. 
 *
 * If this is set to `S2N_CERT_AUTH_OPTIONAL` the server will request a client certificate but allow the client to not provide one.
 * Rejecting a client certificate when using `S2N_CERT_AUTH_OPTIONAL` will terminate the handshake.
 *
 * @param config A pointer to a s2n_config object
 * @param client_auth_type The client auth policy for the connection
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_client_auth_type(struct s2n_config *config, s2n_cert_auth_type client_auth_type);

/**
 * Gets Client Certificate authentication method the s2n_connection object is using.
 *
 * @param conn A pointer to the s2n_connection object
 * @param client_auth_type A pointer to a client auth policy. This will be updated to the s2n_connection value.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_get_client_auth_type(struct s2n_connection *conn, s2n_cert_auth_type *client_auth_type);

/**
 * Sets whether or not a Client Certificate should be required to complete the TLS Connection. 
 *
 * If this is set to `S2N_CERT_AUTH_OPTIONAL` the server will request a client certificate but allow the client to not provide one.
 * Rejecting a client certificate when using `S2N_CERT_AUTH_OPTIONAL` will terminate the handshake.
 *
 * @param conn A pointer to the s2n_connection object
 * @param client_auth_type The client auth policy for the connection
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_client_auth_type(struct s2n_connection *conn, s2n_cert_auth_type client_auth_type);

/**
 * Gets the raw certificate chain received from the client.
 *
 * The retrieved certificate chain has the format described by the TLS 1.2 RFC:
 * https://datatracker.ietf.org/doc/html/rfc5246#section-7.4.2. Each certificate is a DER-encoded ASN.1 X.509,
 * prepended by a 3 byte network-endian length value. Note that this format is used regardless of the connection's
 * protocol version.
 *
 * @warning The buffer pointed to by `cert_chain_out` shares its lifetime with the s2n_connection object.
 *
 * @param conn A pointer to the s2n_connection object
 * @param cert_chain_out A pointer that's set to the client certificate chain.
 * @param cert_chain_len A pointer that's set to the size of the `cert_chain_out` buffer.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_get_client_cert_chain(struct s2n_connection *conn, uint8_t **der_cert_chain_out, uint32_t *cert_chain_len);

/**
 * Sets the initial number of session tickets to send after a >=TLS1.3 handshake. The default value is one ticket.
 *
 * @param config A pointer to the config object.
 * @param num The number of session tickets that will be sent.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_config_set_initial_ticket_count(struct s2n_config *config, uint8_t num);

/**
 * Increases the number of session tickets to send after a >=TLS1.3 handshake.
 *
 * @param conn A pointer to the connection object.
 * @param num The number of additional session tickets to send.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_add_new_tickets_to_send(struct s2n_connection *conn, uint8_t num);

/**
 * Returns the number of session tickets issued by the server.
 *
 * In TLS1.3, this number can be up to the limit configured by s2n_config_set_initial_ticket_count
 * and s2n_connection_add_new_tickets_to_send. In earlier versions of TLS, this number will be either 0 or 1.
 *
 * This method only works for server connections.
 *
 * @param conn A pointer to the connection object.
 * @param num The number of additional session tickets sent.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_get_tickets_sent(struct s2n_connection *conn, uint16_t *num);

/**
 * Sets the keying material lifetime for >=TLS1.3 session tickets so that one session doesn't get re-used ad infinitum.
 * The default value is one week.
 *
 * @param conn A pointer to the connection object.
 * @param lifetime_in_secs Lifetime of keying material in seconds.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API extern int s2n_connection_set_server_keying_material_lifetime(struct s2n_connection *conn, uint32_t lifetime_in_secs);

struct s2n_session_ticket;

/**
 * Callback function for receiving a session ticket.
 *
 * This function will be called each time a session ticket is received, which may be multiple times for TLS1.3.
 *
 * # Safety
 *
 * `ctx` is a void pointer and the caller is responsible for ensuring it is cast to the correct type.
 * `ticket` is valid only within the scope of this callback.
 *
 * @param conn A pointer to the connection object.
 * @param ctx Context for the session ticket callback function.
 * @param ticket Pointer to the received session ticket object.
 */
typedef int (*s2n_session_ticket_fn)(struct s2n_connection *conn, void *ctx, struct s2n_session_ticket *ticket);

/**
 * Sets a session ticket callback to be called when a client receives a new session ticket.
 *
 * # Safety
 *
 * `callback` MUST cast `ctx` into the same type of pointer that was originally created.
 * `ctx` MUST be valid for the lifetime of the config, or until a different context is set.
 *
 * @param config A pointer to the config object.
 * @param callback The function that should be called when the callback is triggered.
 * @param ctx The context to be passed when the callback is called.
 */
S2N_API extern int s2n_config_set_session_ticket_cb(struct s2n_config *config, s2n_session_ticket_fn callback, void *ctx);

/**
 * Gets the length of the session ticket from a session ticket object.
 *
 * @param ticket Pointer to the session ticket object.
 * @param data_len Pointer to be set to the length of the session ticket on success.
 */
S2N_API extern int s2n_session_ticket_get_data_len(struct s2n_session_ticket *ticket, size_t *data_len);

/**
 * Gets the session ticket data from a session ticket object.
 *
 * # Safety
 * The entire session ticket will be copied into `data` on success. Therefore, `data` MUST have enough
 * memory to store the session ticket data.
 *
 * @param ticket Pointer to the session ticket object.
 * @param max_data_len Maximum length of data that can be written to the 'data' pointer.
 * @param data Pointer to where the session ticket data will be stored.
 */
S2N_API extern int s2n_session_ticket_get_data(struct s2n_session_ticket *ticket, size_t max_data_len, uint8_t *data);

/**
 * Gets the lifetime in seconds of the session ticket from a session ticket object.
 *
 * @param ticket Pointer to the session ticket object.
 * @param session_lifetime Pointer to a variable where the lifetime of the session ticket will be stored.
 */
S2N_API extern int s2n_session_ticket_get_lifetime(struct s2n_session_ticket *ticket, uint32_t *session_lifetime);

/**
 * De-serializes the session state and updates the connection accordingly.
 *
 * If this method fails, the connection should not be affected: calling s2n_negotiate
 * with the connection should simply result in a full handshake.
 *
 * @param conn A pointer to the s2n_connection object
 * @param session A pointer to a buffer of size `length`
 * @param length The size of the `session` buffer
 *
 * @returns The number of copied bytes 
 */
S2N_API extern int s2n_connection_set_session(struct s2n_connection *conn, const uint8_t *session, size_t length);

/**
 * Serializes the session state from connection and copies into the `session` buffer and returns the number of copied bytes
 *
 * @note This function is not recommended for > TLS 1.2 because in TLS1.3
 * servers can send multiple session tickets and this function will only
 * return the most recently received ticket.
 *
 * @param conn A pointer to the s2n_connection object
 * @param session A pointer to a buffer of size `max_length`
 * @param max_length The size of the `session` buffer
 *
 * @returns The number of copied bytes 
 */
S2N_API extern int s2n_connection_get_session(struct s2n_connection *conn, uint8_t *session, size_t max_length);

/**
 * Retrieves a hint from the server indicating how long this ticket's lifetime is.
 * 
 * @note This function is not recommended for > TLS 1.2 because in TLS1.3
 * servers can send multiple session tickets and this function will only
 * return the most recently received ticket lifetime hint.
 *
 * @param conn A pointer to the s2n_connection object
 *
 * @returns The session ticket lifetime hint in seconds from the server or -1 when session ticket was not used for resumption.
 */
S2N_API extern int s2n_connection_get_session_ticket_lifetime_hint(struct s2n_connection *conn);

/**
 * Use this to query the serialized session state size before copying it into a buffer.
 *
 * @param conn A pointer to the s2n_connection object
 *
 * @returns number of bytes needed to store serialized session state
 */
S2N_API extern int s2n_connection_get_session_length(struct s2n_connection *conn);

/**
 * Gets the latest session id's length from the connection.
 *
 * Use this to query the session id size before copying it into a buffer.
 *
 * @param conn A pointer to the s2n_connection object
 *
 * @returns The latest session id length from the connection. Session id length will be 0 for TLS versions >= TLS1.3 as stateful session resumption has not yet been implemented in TLS1.3.
 */
S2N_API extern int s2n_connection_get_session_id_length(struct s2n_connection *conn);

/**
* Gets the latest session id from the connection, copies it into the `session_id` buffer, and returns the number of copied bytes. 
*
* The session id may change between s2n receiving the ClientHello and sending the ServerHello, but this function will always describe the latest session id. 
*
* See s2n_client_hello_get_session_id() to get the session id as it was sent by the client in the ClientHello message.
 *
 * @param conn A pointer to the s2n_connection object
 * @param session_id A pointer to a buffer of size `max_length`
 * @param max_length The size of the `session_id` buffer
 *
 * @returns The number of copied bytes.
 */
S2N_API extern int s2n_connection_get_session_id(struct s2n_connection *conn, uint8_t *session_id, size_t max_length);

/**
 * Check if the connection was resumed from an earlier handshake.
 *
 * @param conn A pointer to the s2n_connection object
 *
 * @returns returns 1 if the handshake was abbreviated, otherwise returns 0
 */
S2N_API extern int s2n_connection_is_session_resumed(struct s2n_connection *conn);

/**
 * Check if the connection is OCSP stapled.
 *
 * @param conn A pointer to the s2n_connection object
 *
 * @returns 1 if OCSP response was sent (if connection is in S2N_SERVER mode) or received (if connection is in S2N_CLIENT mode) during handshake, otherwise it returns 0.
 */
S2N_API extern int s2n_connection_is_ocsp_stapled(struct s2n_connection *conn);

/** 
 * TLS Signature Algorithms - RFC 5246 7.4.1.4.1 
 * https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-16 
 */
typedef enum {
    S2N_TLS_SIGNATURE_ANONYMOUS = 0,
    S2N_TLS_SIGNATURE_RSA = 1,
    S2N_TLS_SIGNATURE_ECDSA = 3,

    /* Use Private Range for RSA PSS since it's not defined there */
    S2N_TLS_SIGNATURE_RSA_PSS_RSAE = 224,
    S2N_TLS_SIGNATURE_RSA_PSS_PSS
} s2n_tls_signature_algorithm;

/** TLS Hash Algorithms - https://tools.ietf.org/html/rfc5246#section-7.4.1.4.1 
 * https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-18 
 */
typedef enum {
    S2N_TLS_HASH_NONE = 0,
    S2N_TLS_HASH_MD5 = 1,
    S2N_TLS_HASH_SHA1 = 2,
    S2N_TLS_HASH_SHA224 = 3,
    S2N_TLS_HASH_SHA256 = 4,
    S2N_TLS_HASH_SHA384 = 5,
    S2N_TLS_HASH_SHA512 = 6,

    /* Use Private Range for MD5_SHA1 */
    S2N_TLS_HASH_MD5_SHA1 = 224
} s2n_tls_hash_algorithm;

/**
 * Get the connection's selected signature algorithm.
 *
 * @param conn A pointer to the s2n_connection object
 * @param chosen_alg A pointer to a s2n_tls_signature_algorithm object. This is an output parameter.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE if bad parameters are received. 
 */
S2N_API extern int s2n_connection_get_selected_signature_algorithm(struct s2n_connection *conn, s2n_tls_signature_algorithm *chosen_alg);

/**
 * Get the connection's selected digest algorithm.
 *
 * @param conn A pointer to the s2n_connection object
 * @param chosen_alg A pointer to a s2n_tls_hash_algorithm object. This is an output parameter.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE if bad parameters are received. 
 */
S2N_API extern int s2n_connection_get_selected_digest_algorithm(struct s2n_connection *conn, s2n_tls_hash_algorithm *chosen_alg);

/**
 * Get the client certificate's signature algorithm.
 *
 * @param conn A pointer to the s2n_connection object
 * @param chosen_alg A pointer to a s2n_tls_signature_algorithm object. This is an output parameter.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE if bad parameters are received. 
 */
S2N_API extern int s2n_connection_get_selected_client_cert_signature_algorithm(struct s2n_connection *conn, s2n_tls_signature_algorithm *chosen_alg);

/**
 * Get the client certificate's digest algorithm.
 *
 * @param conn A pointer to the s2n_connection object
 * @param chosen_alg A pointer to a s2n_tls_hash_algorithm object. This is an output parameter.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE if bad parameters are received. 
 */
S2N_API extern int s2n_connection_get_selected_client_cert_digest_algorithm(struct s2n_connection *conn, s2n_tls_hash_algorithm *chosen_alg);

/**
 * Get the certificate used during the TLS handshake
 *
 * - If `conn` is a server connection, the certificate selected will depend on the
 *   ServerName sent by the client and supported ciphers.
 * - If `conn` is a client connection, the certificate sent in response to a CertificateRequest
 *   message is returned. Currently s2n-tls supports loading only one certificate in client mode. Note that
 *   not all TLS endpoints will request a certificate.
 *
 * @param conn A pointer to the s2n_connection object
 *
 * @returns NULL if the certificate selection phase of the handshake has not completed or if a certificate was not requested by the peer
 */
S2N_API extern struct s2n_cert_chain_and_key *s2n_connection_get_selected_cert(struct s2n_connection *conn);

/**
 * @param chain_and_key A pointer to the s2n_cert_chain_and_key object being read.
 * @param cert_length This return value represents the length of the s2n certificate chain `chain_and_key`.
 * @returns the length of the s2n certificate chain `chain_and_key`.
 */
S2N_API extern int s2n_cert_chain_get_length(const struct s2n_cert_chain_and_key *chain_and_key, uint32_t *cert_length);

/**
 * Returns the certificate `out_cert` present at the index `cert_idx` of the certificate chain `chain_and_key`.
 * 
 * Note that the index of the leaf certificate is zero. If the certificate chain `chain_and_key` is NULL or the
 * certificate index value is not in the acceptable range for the input certificate chain, an error is returned.
 * 
 * # Safety 
 *
 * There is no memory allocation required for `out_cert` buffer prior to calling the `s2n_cert_chain_get_cert` API.
 * The `out_cert` will contain the pointer to the s2n_cert initialized within the input s2n_cert_chain_and_key `chain_and_key`.
 * The pointer to the output s2n certificate `out_cert` is valid until `chain_and_key` is freed up. 
 * If a caller wishes to persist the `out_cert` beyond the lifetime of `chain_and_key`, the contents would need to be
 * copied prior to freeing `chain_and_key`.
 *
 * @param chain_and_key A pointer to the s2n_cert_chain_and_key object being read.
 * @param out_cert A pointer to the output s2n_cert `out_cert` present at the index `cert_idx` of the certificate chain `chain_and_key`.
 * @param cert_idx The certificate index for the requested certificate within the s2n certificate chain.
 */
S2N_API extern int s2n_cert_chain_get_cert(const struct s2n_cert_chain_and_key *chain_and_key, struct s2n_cert **out_cert, const uint32_t cert_idx);

/**
 * Returns the s2n certificate in DER format along with its length.
 * 
 * The API gets the s2n certificate `cert` in DER format. The certificate is returned in the `out_cert_der` buffer.
 * Here, `cert_len` represents the length of the certificate.
 * 
 * A caller can use certificate parsing tools such as the ones provided by OpenSSL to parse the DER encoded certificate chain returned.
 *
 * # Safety
 * 
 * The memory for the `out_cert_der` buffer is allocated and owned by s2n-tls. 
 * Since the size of the certificate can potentially be very large, a pointer to internal connection data is returned instead of 
 * copying the contents into a caller-provided buffer.
 * 
 * The pointer to the output buffer `out_cert_der` is valid only while the connection exists.
 * The `s2n_connection_free` API frees the memory associated with the out_cert_der buffer and after the `s2n_connection_wipe` API is
 * called the memory pointed by out_cert_der is invalid.
 * 
 * If a caller wishes to persist the `out_cert_der` beyond the lifetime of the connection, the contents would need to be
 * copied prior to the connection termination.
 * 
 * @param cert A pointer to the s2n_cert object being read.
 * @param out_cert_der A pointer to the output buffer which will hold the s2n certificate `cert` in DER format.
 * @param cert_length This return value represents the length of the certificate.
 */
S2N_API extern int s2n_cert_get_der(const struct s2n_cert *cert, const uint8_t **out_cert_der, uint32_t *cert_length);

/**
 * Returns the validated peer certificate chain as a `s2n_cert_chain_and_key` opaque object.
 * 
 * The `s2n_cert_chain_and_key` parameter must be allocated by the caller using the `s2n_cert_chain_and_key_new` API
 * prior to this function call and must be empty. To free the memory associated with the `s2n_cert_chain_and_key` object use the 
 * `s2n_cert_chain_and_key_free` API.
 * 
 * @param conn A pointer to the s2n_connection object being read.
 * @param cert_chain The returned validated peer certificate chain `cert_chain` retrieved from the s2n connection.
 */
S2N_API extern int s2n_connection_get_peer_cert_chain(const struct s2n_connection *conn, struct s2n_cert_chain_and_key *cert_chain);

/**
 * Returns the length of the DER encoded extension value of the ASN.1 X.509 certificate extension.
 * 
 * @param cert A pointer to the s2n_cert object being read.
 * @param oid A null-terminated cstring that contains the OID of the X.509 certificate extension to be read.
 * @param ext_value_len This return value contains the length of DER encoded extension value of the ASN.1 X.509 certificate extension.
 */
S2N_API extern int s2n_cert_get_x509_extension_value_length(struct s2n_cert *cert, const uint8_t *oid, uint32_t *ext_value_len);

/**
 * Returns the DER encoding of an ASN.1 X.509 certificate extension value, it's length and a boolean critical.
 * 
 * @param cert A pointer to the s2n_cert object being read.
 * @param oid A null-terminated cstring that contains the OID of the X.509 certificate extension to be read.
 * @param ext_value A pointer to the output buffer which will hold the DER encoding of an ASN.1 X.509 certificate extension value returned. 
 * @param ext_value_len  This value is both an input and output parameter and represents the length of the output buffer `ext_value`. 
 * When used as an input parameter, the caller must use this parameter to convey the maximum length of `ext_value`. 
 * When used as an output parameter, `ext_value_len` holds the actual length of the DER encoding of the ASN.1 X.509 certificate extension value returned. 
 * @param critical This return value contains the boolean value for `critical`.
 */
S2N_API extern int s2n_cert_get_x509_extension_value(struct s2n_cert *cert, const uint8_t *oid, uint8_t *ext_value, uint32_t *ext_value_len, bool *critical);

/**
 * Returns the UTF8 String length of the ASN.1 X.509 certificate extension data. 
 * 
 * @param extension_data A pointer to the DER encoded ASN.1 X.509 certificate extension value being read.
 * @param extension_len represents the length of the input buffer `extension_data`.
 * @param utf8_str_len This return value contains the UTF8 String length of the ASN.1 X.509 certificate extension data.
 */
S2N_API extern int s2n_cert_get_utf8_string_from_extension_data_length(const uint8_t *extension_data, uint32_t extension_len, uint32_t *utf8_str_len);

/**
 * Returns the UTF8 String representation of the DER encoded ASN.1 X.509 certificate extension data.
 * 
 * @param extension_data A pointer to the DER encoded ASN.1 X.509 certificate extension value being read.
 * @param extension_len represents the length of the input buffer `extension_data`.
 * @param out_data A pointer to the output buffer which will hold the UTF8 String representation of the DER encoded ASN.1 X.509 
 * certificate extension data returned. 
 * @param out_len This value is both an input and output parameter and represents the length of the output buffer `out_data`.
 * When used as an input parameter, the caller must use this parameter to convey the maximum length of `out_data`. 
 * When used as an output parameter, `out_len` holds the actual length of UTF8 String returned.
 */
S2N_API extern int s2n_cert_get_utf8_string_from_extension_data(const uint8_t *extension_data, uint32_t extension_len, uint8_t *out_data, uint32_t *out_len);

/** 
 * Pre-shared key (PSK) Hash Algorithm - RFC 8446 Section-2.2
 */
typedef enum {
    S2N_PSK_HMAC_SHA256,
    S2N_PSK_HMAC_SHA384,
} s2n_psk_hmac;

/**
 * Opaque pre shared key handle
 */
struct s2n_psk;

/**
 * Creates a new s2n external pre-shared key (PSK) object with `S2N_PSK_HMAC_SHA256` as the default 
 * PSK hash algorithm. An external PSK is a key established outside of TLS using a secure mutually agreed upon mechanism.
 * 
 * Use `s2n_psk_free` to free the memory allocated to the s2n external PSK object created by this API. 
 *
 * @returns struct s2n_psk* Returns a pointer to the newly created external PSK object.
 */
S2N_API struct s2n_psk *s2n_external_psk_new(void);

/**
 * Frees the memory associated with the external PSK object.
 *
 * @param psk Pointer to the PSK object to be freed.
 */
S2N_API int s2n_psk_free(struct s2n_psk **psk);

/**
 * Sets the identity for a given external PSK object.
 * The identity is a unique identifier for the pre-shared secret.
 * It is a non-secret value represented by raw bytes.
 *
 * # Safety 
 *
 * The identity is transmitted over the network unencrypted and is a non-secret value.
 * Do not include confidential information in the identity.
 * 
 * Note that the identity is copied into s2n-tls memory and the caller is responsible for 
 * freeing the memory associated with the identity input. 
 *
 * @param psk A pointer to a PSK object to be updated with the identity.
 * @param identity The identity in raw bytes format to be copied.
 * @param identity_size The length of the PSK identity being set.
 */
S2N_API int s2n_psk_set_identity(struct s2n_psk *psk, const uint8_t *identity, uint16_t identity_size);

/**
 * Sets the out-of-band/externally provisioned secret for a given external PSK object.
 *
 * # Safety
 *
 * Note that the secret is copied into s2n-tls memory and the caller is responsible for 
 * freeing the memory associated with the `secret` input. 
 *
 * Deriving a shared secret from a password or other low-entropy source
 * is not secure and is subject to dictionary attacks.
 * See https://tools.ietf.org/rfc/rfc8446#section-2.2 for more information.
 *
 * @param psk A pointer to a PSK object to be updated with the secret.
 * @param secret The secret in raw bytes format to be copied.
 * @param secret_size The length of the pre-shared secret being set.
 */
S2N_API int s2n_psk_set_secret(struct s2n_psk *psk, const uint8_t *secret, uint16_t secret_size);

/**
 * Sets the hash algorithm for a given external PSK object. The supported PSK hash 
 * algorithms are as listed in the enum `s2n_psk_hmac` above.
 * 
 * @param psk A pointer to the external PSK object to be updated with the PSK hash algorithm.
 * @param hmac The PSK hash algorithm being set.  
 */
S2N_API int s2n_psk_set_hmac(struct s2n_psk *psk, s2n_psk_hmac hmac);

/**
 * Appends a PSK object to the list of PSKs supported by the s2n connection. 
 * If a PSK with a duplicate identity is found, an error is returned and the PSK is not added to the list.
 * Note that a copy of `psk` is stored on the connection. The user is still responsible for freeing the 
 * memory associated with `psk`.
 *
 * @param conn A pointer to the s2n_connection object that contains the list of PSKs supported.
 * @param psk A pointer to the `s2n_psk` object to be appended to the list of PSKs on the s2n connection.
 */
S2N_API int s2n_connection_append_psk(struct s2n_connection *conn, struct s2n_psk *psk);

/**
 * The list of PSK modes supported by s2n-tls for TLS versions >= TLS1.3.
 * Currently s2n-tls supports two modes - `S2N_PSK_MODE_RESUMPTION`, which represents the PSKs established 
 * using the previous connection via session resumption, and `S2N_PSK_MODE_EXTERNAL`, which represents PSKs 
 * established out-of-band/externally using a secure mutually agreed upon mechanism.
 */
typedef enum {
    S2N_PSK_MODE_RESUMPTION,
    S2N_PSK_MODE_EXTERNAL
} s2n_psk_mode;

/**
 * Sets the PSK mode on the s2n config object. 
 * The supported PSK modes are listed in the enum `s2n_psk_mode` above. 
 * 
 * @param config A pointer to the s2n_config object being updated.
 * @param mode The PSK mode to be set.
 */
S2N_API int s2n_config_set_psk_mode(struct s2n_config *config, s2n_psk_mode mode);

/**
 * Sets the PSK mode on the s2n connection object.
 * The supported PSK modes are listed in the enum `s2n_psk_mode` above. 
 * This API overrides the PSK mode set on config for this connection.
 *
 * @param conn A pointer to the s2n_connection object being updated.
 * @param mode The PSK mode to be set.
 */
S2N_API int s2n_connection_set_psk_mode(struct s2n_connection *conn, s2n_psk_mode mode);

/**
 * Gets the negotiated PSK identity length from the s2n connection object. The negotiated PSK 
 * refers to the chosen PSK by the server to be used for the connection. 
 * 
 * This API can be used to determine if the negotiated PSK exists. If negotiated PSK exists a 
 * call to this API returns a value greater than zero. If the negotiated PSK does not exist, the 
 * value `0` is returned.
 * 
 * @param conn A pointer to the s2n_connection object that successfully negotiated a PSK connection.
 * @param identity_length The length of the negotiated PSK identity. 
 */
S2N_API int s2n_connection_get_negotiated_psk_identity_length(struct s2n_connection *conn, uint16_t *identity_length);

/**
 * Gets the negotiated PSK identity from the s2n connection object. 
 * If the negotiated PSK does not exist, the PSK identity will not be obtained and no error will be returned. 
 * Prior to this API call, use `s2n_connection_get_negotiated_psk_identity_length` to determine if a 
 * negotiated PSK exists or not. 
 *
 * # Safety
 *
 * The negotiated PSK identity will be copied into the identity buffer on success.
 * Therefore, the identity buffer must have enough memory to fit the identity length.
 * 
 * @param conn A pointer to the s2n_connection object.
 * @param identity The negotiated PSK identity obtained from the s2n_connection object. 
 * @param max_identity_length The maximum length for the PSK identity. If the negotiated psk_identity length is 
 * greater than this `max_identity_length` value an error will be returned.
 */
S2N_API int s2n_connection_get_negotiated_psk_identity(struct s2n_connection *conn, uint8_t *identity, uint16_t max_identity_length);

struct s2n_offered_psk;

/**
 * Creates a new s2n offered PSK object. 
 * An offered PSK object represents a single PSK sent by the client.
 * 
 * # Safety
 * 
 * Use `s2n_offered_psk_free` to free the memory allocated to the s2n offered PSK object created by this API. 
 *
 * @returns struct s2n_offered_psk* Returns a pointer to the newly created offered PSK object.
 */
S2N_API struct s2n_offered_psk *s2n_offered_psk_new(void);

/**
 * Frees the memory associated with the `s2n_offered_psk` object.
 *
 * @param psk A pointer to the `s2n_offered_psk` object to be freed.
 */
S2N_API int s2n_offered_psk_free(struct s2n_offered_psk **psk);

/**
 * Gets the PSK identity and PSK identity length for a given offered PSK object. 
 * 
 * @param psk A pointer to the offered PSK object being read.
 * @param identity The PSK identity being obtained.
 * @param size The length of the PSK identity being obtained.
 */
S2N_API int s2n_offered_psk_get_identity(struct s2n_offered_psk *psk, uint8_t **identity, uint16_t *size);

struct s2n_offered_psk_list;

/**
 * Checks whether the offered PSK list has an offered psk object next in line in the list.
 * An offered PSK list contains all the PSKs offered by the client for the server to select.
 * 
 * # Safety 
 * 
 * This API returns a pointer to the s2n-tls internal memory with limited lifetime. 
 * After the completion of `s2n_psk_selection_callback` this pointer is invalid.
 *
 * @param psk_list A pointer to the offered PSK list being read.
 * @returns bool A boolean value representing whether an offered psk object is present next in line in the offered PSK list.
 */
S2N_API bool s2n_offered_psk_list_has_next(struct s2n_offered_psk_list *psk_list);

/**
 * Obtains the next offered PSK object from the list of offered PSKs. Use `s2n_offered_psk_list_has_next` 
 * prior to this API call to ensure we have not reached the end of the list.
 * 
 * @param psk_list A pointer to the offered PSK list being read.
 * @param psk A pointer to the next offered PSK object being obtained.
 */
S2N_API int s2n_offered_psk_list_next(struct s2n_offered_psk_list *psk_list, struct s2n_offered_psk *psk);

/**
 * Returns the offered PSK list to its original read state.
 *
 * When `s2n_offered_psk_list_reread` is called, `s2n_offered_psk_list_next` will return the first PSK 
 * in the offered PSK list.
 *
 * @param psk_list A pointer to the offered PSK list being reread.
 */
S2N_API int s2n_offered_psk_list_reread(struct s2n_offered_psk_list *psk_list);

/**
 * Chooses a PSK from the offered PSK list to be used for the connection.  
 * This API matches the PSK identity received from the client against the server's known PSK identities 
 * list, in order to choose the PSK to be used for the connection. If the PSK identity sent from the client 
 * is NULL, no PSK is chosen for the connection. If the client offered PSK identity has no matching PSK identity 
 * with the server, an error will be returned. Use this API along with the `s2n_psk_selection_callback` callback 
 * to select a PSK identity.
 * 
 * @param psk_list A pointer to the server's known PSK list used to compare for a matching PSK with the client.
 * @param psk A pointer to the client's PSK object used to compare with the server's known PSK identities.
 */
S2N_API int s2n_offered_psk_list_choose_psk(struct s2n_offered_psk_list *psk_list, struct s2n_offered_psk *psk);

/**
 * Callback function to select a PSK from a list of offered PSKs.
 * Use this callback to implement custom PSK selection logic. The s2n-tls default PSK selection logic 
 * chooses the first matching PSK from the list of offered PSKs sent by the client.
 * 
 * # Safety
 *
 * `context` is a void pointer and the caller is responsible for ensuring it is cast to the correct type.
 * After the completion of this callback, the pointer to `psk_list` is invalid.
 *
 * @param conn A pointer to the s2n_connection object.
 * @param context A pointer to a context for the caller to pass state to the callback, if needed.
 * @param psk_list A pointer to the offered PSK list being read.
 */
typedef int (*s2n_psk_selection_callback)(struct s2n_connection *conn, void *context,
        struct s2n_offered_psk_list *psk_list);

/**
 * Sets the callback to select the matching PSK. 
 * If this callback is not set s2n-tls uses a default PSK selection logic that selects the first matching 
 * server PSK.
 * 
 * @param config A pointer to the s2n_config object.
 * @param cb The function that should be called when the callback is triggered.
 * @param context A pointer to a context for the caller to pass state to the callback, if needed.
 */
S2N_API int s2n_config_set_psk_selection_callback(struct s2n_config *config, s2n_psk_selection_callback cb, void *context);

/**
 * Get the number of bytes the connection has received.
 *
 * @param conn A pointer to the connection
 * @returns return the number of bytes received by s2n-tls "on the wire"
 */
S2N_API extern uint64_t s2n_connection_get_wire_bytes_in(struct s2n_connection *conn);

/**
 * Get the number of bytes the connection has transmitted out.
 *
 * @param conn A pointer to the connection
 * @returns return the number of bytes transmitted out by s2n-tls "on the wire"
 */
S2N_API extern uint64_t s2n_connection_get_wire_bytes_out(struct s2n_connection *conn);

/**
 * Access the protocol version supported by the client.
 *
 * @note The return value corresponds to the macros defined as S2N_SSLv2,
 * S2N_SSLv3, S2N_TLS10, S2N_TLS11, S2N_TLS12, and S2N_TLS13.
 *
 * @param conn A pointer to the connection
 * @returns returns the highest protocol version supported by the client
 */
S2N_API extern int s2n_connection_get_client_protocol_version(struct s2n_connection *conn);

/**
 * Access the protocol version supported by the server.
 *
 * @note The return value corresponds to the macros defined as S2N_SSLv2,
 * S2N_SSLv3, S2N_TLS10, S2N_TLS11, S2N_TLS12, and S2N_TLS13.
 *
 * @param conn A pointer to the connection
 * @returns Returns the highest protocol version supported by the server
 */
S2N_API extern int s2n_connection_get_server_protocol_version(struct s2n_connection *conn);

/**
 * Access the protocol version selected for the connection.
 *
 * @note The return value corresponds to the macros defined as S2N_SSLv2,
 * S2N_SSLv3, S2N_TLS10, S2N_TLS11, S2N_TLS12, and S2N_TLS13.
 *
 * @param conn A pointer to the connection
 * @returns The protocol version actually negotiated by the handshake
 */
S2N_API extern int s2n_connection_get_actual_protocol_version(struct s2n_connection *conn);

/**
 * Access the client hello protocol version for the connection.
 *
 * @note The return value corresponds to the macros defined as S2N_SSLv2,
 * S2N_SSLv3, S2N_TLS10, S2N_TLS11, S2N_TLS12, and S2N_TLS13.
 * 
 * @param conn A pointer to the connection
 * @returns The protocol version used to send the initial client hello message. 
 */
S2N_API extern int s2n_connection_get_client_hello_version(struct s2n_connection *conn);

/**
 * Access the protocol version from the header of the first record that contained the ClientHello message.
 * 
 * @note This field has been deprecated and should not be confused with the client hello
 * version. It is often set very low, usually to TLS1.0 for compatibility reasons,
 * and should never be set higher than TLS1.2. Therefore this method should only be used
 * for logging or fingerprinting.
 *
 * @param conn A pointer to the client hello struct
 * @param out The protocol version in the record header containing the Client Hello.
 */
S2N_API extern int s2n_client_hello_get_legacy_record_version(struct s2n_client_hello *ch, uint8_t *out);

/**
 * Check if Client Auth was used for a connection.
 *
 * @param conn A pointer to the connection
 * @returns 1 if the handshake completed and Client Auth was negotiated during then
 * handshake.
 */
S2N_API extern int s2n_connection_client_cert_used(struct s2n_connection *conn);

/**
 * A function that provides a human readable string of the cipher suite that was chosen
 * for a connection.
 *
 * @warning The string "TLS_NULL_WITH_NULL_NULL" is returned before the TLS handshake has been performed.
 * This does not mean that the ciphersuite "TLS_NULL_WITH_NULL_NULL" will be used by the connection,
 * it is merely being used as a placeholder.
 *
 * @note This function is only accurate after the TLS handshake.
 *
 * @param conn A pointer to the connection
 * @returns A string indicating the cipher suite negotiated by s2n in OpenSSL format.
 */
S2N_API extern const char *s2n_connection_get_cipher(struct s2n_connection *conn);

/** 
 * A metric to determine whether or not the server found a certificate that matched
 * the client's SNI extension.
 *
 * S2N_SNI_NONE: Client did not send the SNI extension.
 * S2N_SNI_EXACT_MATCH: Server had a certificate that matched the client's SNI extension.
 * S2N_SNI_WILDCARD_MATCH: Server had a certificate with a domain name containing a wildcard character
 * that could be matched to the client's SNI extension.
 * S2N_SNI_NO_MATCH: Server did not have a certificate that could be matched to the client's
 * SNI extension.
 */
typedef enum {
    S2N_SNI_NONE = 1,
    S2N_SNI_EXACT_MATCH,
    S2N_SNI_WILDCARD_MATCH,
    S2N_SNI_NO_MATCH,
} s2n_cert_sni_match;

/**
 * A function that provides insight into whether or not the server was able to send a certificate that
 * partially or completely matched the client's SNI extension.
 * 
 * @note This function can be used as a metric in a failed connection as long as the failure
 * occurs after certificate selection.
 *
 * @param conn A pointer to the connection
 * @param cert_match An enum indicating whether or not the server found a certificate
 * that matched the client's SNI extension.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure.
 */
S2N_API extern int s2n_connection_get_certificate_match(struct s2n_connection *conn, s2n_cert_sni_match *match_status);

/**
 * Provides access to the TLS master secret.
 *
 * This is a dangerous method and should not be used unless absolutely necessary.
 * Mishandling the master secret can compromise both the current connection
 * and any past or future connections that use the same master secret due to
 * session resumption.
 *
 * This method is only supported for older TLS versions, and will report an S2N_ERR_INVALID_STATE
 * usage error if called for a TLS1.3 connection. TLS1.3 includes a new key schedule
 * that derives independent secrets from the master secret for specific purposes,
 * such as separate traffic, session ticket, and exporter secrets. Using the master
 * secret directly circumvents that security feature, reducing the security of
 * the protocol.
 *
 * If you need cryptographic material tied to the current TLS session, consider
 * `s2n_connection_tls_exporter` instead. Although s2n_connection_tls_exporter
 * currently only supports TLS1.3, there is also an RFC that describes exporters
 * for older TLS versions: https://datatracker.ietf.org/doc/html/rfc5705
 * Using the master secret as-is or defining your own exporter is dangerous.
 *
 * @param conn A pointer to the connection.
 * @param secret_bytes Memory to copy the master secret into. The secret
 * is always 48 bytes long.
 * @param max_size The size of the memory available at `secret_bytes`. Must be
 * at least 48 bytes.
 * @returns S2N_SUCCESS on success, S2N_FAILURE otherwise. `secret_bytes`
 * will be set on success.
 */
S2N_API extern int s2n_connection_get_master_secret(const struct s2n_connection *conn,
        uint8_t *secret_bytes, size_t max_size);

/**
 * Provides access to the TLS-Exporter functionality.
 *
 * See https://datatracker.ietf.org/doc/html/rfc5705 and https://www.rfc-editor.org/rfc/rfc8446.
 *
 * @note This is currently only available with TLS 1.3 connections which have finished a handshake.
 *
 * @param conn A pointer to the connection
 * @returns A POSIX error signal. If an error was returned, the value contained in `output` should be considered invalid.
 */
S2N_API extern int s2n_connection_tls_exporter(struct s2n_connection *conn,
        const uint8_t *label, uint32_t label_length, const uint8_t *context, uint32_t context_length,
        uint8_t *output, uint32_t output_length);

/**
 * Returns the IANA value for the connection's negotiated cipher suite.
 *
 * The value is returned in the form of `first,second`, in order to closely match
 * the values defined in the [IANA Registry](https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#table-tls-parameters-4).
 * For example if the connection's negotiated cipher suite is `TLS_AES_128_GCM_SHA256`,
 * which is registered as `0x13,0x01`, then `first = 0x13` and `second = 0x01`.
 *
 * This method will only succeed after the cipher suite has been negotiated with the peer.
 *
 * @param conn A pointer to the connection being read
 * @param first A pointer to a single byte, which will be updated with the first byte in the registered IANA value.
 * @param second A pointer to a single byte, which will be updated with the second byte in the registered IANA value.
 * @returns A POSIX error signal. If an error was returned, the values contained in `first` and `second` should be considered invalid.
 */
S2N_API extern int s2n_connection_get_cipher_iana_value(struct s2n_connection *conn, uint8_t *first, uint8_t *second);

/**
 * Function to check if the cipher used by current connection is supported by the current
 * cipher preferences.
 * @param conn A pointer to the s2n connection
 * @param version A string representing the security policy to check against.
 * @returns 1 if the connection satisfies the cipher suite. 0 if the connection does not satisfy the cipher suite. -1 if there is an error.
 */
S2N_API extern int s2n_connection_is_valid_for_cipher_preferences(struct s2n_connection *conn, const char *version);

/**
 * Function to get the human readable elliptic curve name for the connection.
 *
 * @param conn A pointer to the s2n connection
 * @returns A string indicating the elliptic curve used during ECDHE key exchange. The string "NONE" is returned if no curve was used.
 */
S2N_API extern const char *s2n_connection_get_curve(struct s2n_connection *conn);

/**
 * Function to get the human readable KEM name for the connection.
 *
 * @param conn A pointer to the s2n connection
 * @returns A human readable string for the KEM group. If there is no KEM configured returns "NONE"
 */
S2N_API extern const char *s2n_connection_get_kem_name(struct s2n_connection *conn);

/**
 * Function to get the human readable KEM group name for the connection.
 *
 * @param conn A pointer to the s2n connection
 * @returns A human readable string for the KEM group. If the connection is < TLS1.3 or there is no KEM group configured returns "NONE"
 */
S2N_API extern const char *s2n_connection_get_kem_group_name(struct s2n_connection *conn);

/**
 * Function to get the alert that caused a connection to close. s2n-tls considers all
 * TLS alerts fatal and shuts down a connection whenever one is received.
 *
 * @param conn A pointer to the s2n connection
 * @returns The TLS alert code that caused a connection to be shut down
 */
S2N_API extern int s2n_connection_get_alert(struct s2n_connection *conn);

/**
 * Function to return the last TLS handshake type that was processed. The returned format is a human readable string.
 *
 * @param conn A pointer to the s2n connection
 * @returns A human-readable handshake type name, e.g. "NEGOTIATED|FULL_HANDSHAKE|PERFECT_FORWARD_SECRECY"
 */
S2N_API extern const char *s2n_connection_get_handshake_type_name(struct s2n_connection *conn);

/**
 * Function to return the last TLS message that was processed. The returned format is a human readable string.
 * @param conn A pointer to the s2n connection
 * @returns The last message name in the TLS state machine, e.g. "SERVER_HELLO", "APPLICATION_DATA". 
 */
S2N_API extern const char *s2n_connection_get_last_message_name(struct s2n_connection *conn);

/**
 * Opaque async private key operation handle
 */
struct s2n_async_pkey_op;

/**
 * Sets whether or not a connection should enforce strict signature validation during the
 * `s2n_async_pkey_op_apply` call.
 *
 * `mode` can take the following values:
 * - `S2N_ASYNC_PKEY_VALIDATION_FAST` - default behavior: s2n-tls will perform only the minimum validation required for safe use of the asyn pkey operation.
 * - `S2N_ASYNC_PKEY_VALIDATION_STRICT` - in addition to the previous checks, s2n-tls will also ensure that the signature created as a result of the async private key sign operation matches the public key on the connection.
 */
typedef enum {
    S2N_ASYNC_PKEY_VALIDATION_FAST,
    S2N_ASYNC_PKEY_VALIDATION_STRICT
} s2n_async_pkey_validation_mode;

/**
 * The type of private key operation
 */
typedef enum {
    S2N_ASYNC_DECRYPT,
    S2N_ASYNC_SIGN
} s2n_async_pkey_op_type;

/**
 * Callback function for handling private key operations
 *
 * Invoked every time an operation requiring the private key is encountered
 * during the handshake.
 *
 * # Safety
 * * `op` is owned by the application and MUST be freed.
 *
 * @param conn Connection which triggered the callback
 * @param op An opaque object representing the private key operation
 */
typedef int (*s2n_async_pkey_fn)(struct s2n_connection *conn, struct s2n_async_pkey_op *op);

/**
 * Sets up the callback to invoke when private key operations occur.
 *
 * @param config Config to set the callback
 * @param fn The function that should be called for each private key operation
 */
S2N_API extern int s2n_config_set_async_pkey_callback(struct s2n_config *config, s2n_async_pkey_fn fn);

/**
 * Performs a private key operation using the given private key.
 *
 * # Safety
 * * Can only be called once. Any subsequent calls will produce a `S2N_ERR_T_USAGE` error.
 * * Safe to call from inside s2n_async_pkey_fn
 * * Safe to call from a different thread, as long as no other thread is operating on `op`.
 *
 * @param op An opaque object representing the private key operation
 * @param key The private key used for the operation. It can be extracted from
 * `conn` through the `s2n_connection_get_selected_cert` and `s2n_cert_chain_and_key_get_private_key` calls
 */
S2N_API extern int s2n_async_pkey_op_perform(struct s2n_async_pkey_op *op, s2n_cert_private_key *key);

/**
 * Finalizes a private key operation and unblocks the connection.
 *
 * # Safety
 * * `conn` must match the connection that originally triggered the callback.
 * * Must be called after the operation is performed.
 * * Can only be called once. Any subsequent calls will produce a `S2N_ERR_T_USAGE` error.
 * * Safe to call from inside s2n_async_pkey_fn
 * * Safe to call from a different thread, as long as no other thread is operating on `op`.
 *
 * @param op An opaque object representing the private key operation
 * @param conn The connection associated with the operation that should be unblocked
 */
S2N_API extern int s2n_async_pkey_op_apply(struct s2n_async_pkey_op *op, struct s2n_connection *conn);

/**
 * Frees the opaque structure representing a private key operation.
 *
 * # Safety
 * * MUST be called for every operation passed to s2n_async_pkey_fn
 * * Safe to call before or after the connection that created the operation is freed
 *
 * @param op An opaque object representing the private key operation
 */
S2N_API extern int s2n_async_pkey_op_free(struct s2n_async_pkey_op *op);

/**
 * Configures whether or not s2n-tls will perform potentially expensive validation of
 * the results of a private key operation.
 *
 * @param config Config to set the validation mode for
 * @param mode What level of validation to perform
 */
S2N_API extern int s2n_config_set_async_pkey_validation_mode(struct s2n_config *config, s2n_async_pkey_validation_mode mode);

/**
 * Returns the type of the private key operation.
 *
 * @param op An opaque object representing the private key operation
 * @param type A pointer to be set to the type
 */
S2N_API extern int s2n_async_pkey_op_get_op_type(struct s2n_async_pkey_op *op, s2n_async_pkey_op_type *type);

/**
 * Returns the size of the input to the private key operation.
 *
 * @param op An opaque object representing the private key operation
 * @param data_len A pointer to be set to the size
 */
S2N_API extern int s2n_async_pkey_op_get_input_size(struct s2n_async_pkey_op *op, uint32_t *data_len);

/**
 * Returns the input to the private key operation.
 *
 * When signing, the input is the digest to sign.
 * When decrypting, the input is the data to decrypt.
 *
 * # Safety
 * * `data` must be sufficiently large to contain the input.
 *   `s2n_async_pkey_op_get_input_size` can be called to determine how much memory is required.
 * * s2n-tls does not take ownership of `data`.
 *   The application still owns the memory and must free it if necessary.
 *
 * @param op An opaque object representing the private key operation
 * @param data A pointer to a buffer to copy the input into
 * @param data_len The maximum size of the `data` buffer
 */
S2N_API extern int s2n_async_pkey_op_get_input(struct s2n_async_pkey_op *op, uint8_t *data, uint32_t data_len);

/**
 * Sets the output of the private key operation.
 *
 * # Safety
 * * s2n-tls does not take ownership of `data`.
 *   The application still owns the memory and must free it if necessary.
 *
 * @param op An opaque object representing the private key operation
 * @param data A pointer to a buffer containing the output
 * @param data_len The size of the `data` buffer
 */
S2N_API extern int s2n_async_pkey_op_set_output(struct s2n_async_pkey_op *op, const uint8_t *data, uint32_t data_len);

/**
 * Callback function for handling key log events
 *
 * THIS SHOULD BE USED FOR DEBUGGING PURPOSES ONLY!
 *
 * Each log line is formatted with the
 * [NSS Key Log Format](https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/Key_Log_Format)
 * without a newline.
 *
 * # Safety
 *
 * * `ctx` MUST be cast into the same type of pointer that was originally created
 * * `logline` bytes MUST be copied or discarded before this function returns
 *
 * @param ctx Context for the callback
 * @param conn Connection for which the log line is being emitted
 * @param logline Pointer to the log line data
 * @param len Length of the log line data
 */
typedef int (*s2n_key_log_fn)(void *ctx, struct s2n_connection *conn, uint8_t *logline, size_t len);

/**
 * Sets a key logging callback on the provided config
 *
 * THIS SHOULD BE USED FOR DEBUGGING PURPOSES ONLY!
 *
 * Setting this function enables configurations to emit secrets in the
 * [NSS Key Log Format](https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/Key_Log_Format)
 *
 * # Safety
 *
 * * `callback` MUST cast `ctx` into the same type of pointer that was originally created
 * * `ctx` MUST live for at least as long as it is set on the config
 *
 * @param config Config to set the callback
 * @param callback The function that should be called for each secret log entry
 * @param ctx The context to be passed when the callback is called
 */
S2N_API extern int s2n_config_set_key_log_cb(struct s2n_config *config, s2n_key_log_fn callback, void *ctx);

/** 
 * s2n_config_enable_cert_req_dss_legacy_compat adds a dss cert type in the server certificate request when being called.
 * It only sends the dss cert type in the cert request but does not succeed the handshake if a dss cert is received.
 * Please DO NOT call this api unless you know you actually need legacy DSS certificate type compatibility
 * @param config Config to enable legacy DSS certificates for
 */
S2N_API extern int s2n_config_enable_cert_req_dss_legacy_compat(struct s2n_config *config);

/**
 * Sets the maximum bytes of early data the server will accept.
 *
 * The default maximum is 0. If the maximum is 0, the server rejects all early data requests.
 * The config maximum can be overridden by the connection maximum or the maximum on an external pre-shared key.
 *
 * @param config A pointer to the config
 * @param max_early_data_size The maximum early data that the server will accept
 * @returns A POSIX error signal. If successful, the maximum early data size was updated.
 */
S2N_API int s2n_config_set_server_max_early_data_size(struct s2n_config *config, uint32_t max_early_data_size);

/**
 * Sets the maximum bytes of early data the server will accept.
 *
 * The default maximum is 0. If the maximum is 0, the server rejects all early data requests.
 * The connection maximum can be overridden by the maximum on an external pre-shared key.
 *
 * @param conn A pointer to the connection
 * @param max_early_data_size The maximum early data the server will accept
 * @returns A POSIX error signal. If successful, the maximum early data size was updated.
 */
S2N_API int s2n_connection_set_server_max_early_data_size(struct s2n_connection *conn, uint32_t max_early_data_size);

/**
 * Sets the user context associated with early data on a server.
 *
 * This context is passed to the `s2n_early_data_cb` callback to help decide whether to accept or reject early data.
 *
 * Unlike most contexts, the early data context is a byte buffer instead of a void pointer.
 * This is because we need to serialize the context into session tickets.
 *
 * This API is intended for use with session resumption, and will not affect pre-shared keys.
 *
 * @param conn A pointer to the connection
 * @param context A pointer to the user context data. This data will be copied.
 * @param context_size The size of the data to read from the `context` pointer.
 * @returns A POSIX error signal. If successful, the context was updated.
 */
S2N_API int s2n_connection_set_server_early_data_context(struct s2n_connection *conn, const uint8_t *context, uint16_t context_size);

/**
 * Configures a particular pre-shared key to allow early data.
 *
 * `max_early_data_size` must be set to the maximum early data accepted by the server.
 *
 * In order to use early data, the cipher suite set on the pre-shared key must match the cipher suite
 * ultimately negotiated by the TLS handshake. Additionally, the cipher suite must have the same
 * hmac algorithm as the pre-shared key.
 *
 * @param psk A pointer to the pre-shared key, created with `s2n_external_psk_new`.
 * @param max_early_data_size The maximum early data that can be sent or received using this key.
 * @param cipher_suite_first_byte The first byte in the registered IANA value of the associated cipher suite.
 * @param cipher_suite_second_byte The second byte in the registered IANA value of the associated cipher suite.
 * @returns A POSIX error signal. If successful, `psk` was updated.
 */
S2N_API int s2n_psk_configure_early_data(struct s2n_psk *psk, uint32_t max_early_data_size,
        uint8_t cipher_suite_first_byte, uint8_t cipher_suite_second_byte);

/**
 * Sets the optional `application_protocol` associated with the given pre-shared key.
 *
 * In order to use early data, the `application_protocol` set on the pre-shared key must match
 * the `application_protocol` ultimately negotiated by the TLS handshake.
 *
 * @param psk A pointer to the pre-shared key, created with `s2n_external_psk_new`.
 * @param application_protocol A pointer to the associated application protocol data. This data will be copied.
 * @param size The size of the data to read from the `application_protocol` pointer.
 * @returns A POSIX error signal. If successful, the application protocol was set.
 */
S2N_API int s2n_psk_set_application_protocol(struct s2n_psk *psk, const uint8_t *application_protocol, uint8_t size);

/**
 * Sets the optional user early data context associated with the given pre-shared key.
 *
 * The early data context is passed to the `s2n_early_data_cb` callback to help decide whether
 * to accept or reject early data.
 *
 * @param psk A pointer to the pre-shared key, created with `s2n_external_psk_new`.
 * @param context A pointer to the associated user context data. This data will be copied.
 * @param size The size of the data to read from the `context` pointer.
 * @returns A POSIX error signal. If successful, the context was set.
 */
S2N_API int s2n_psk_set_early_data_context(struct s2n_psk *psk, const uint8_t *context, uint16_t size);

/** 
 * The status of early data on a connection.
 *
 * S2N_EARLY_DATA_STATUS_OK: Early data is in progress.
 * S2N_EARLY_DATA_STATUS_NOT_REQUESTED: The client did not request early data, so none was sent or received.
 * S2N_EARLY_DATA_STATUS_REJECTED: The client requested early data, but the server rejected the request.
 *                                 Early data may have been sent, but was not received.
 * S2N_EARLY_DATA_STATUS_END: All early data was successfully sent and received.
 */
typedef enum {
    S2N_EARLY_DATA_STATUS_OK,
    S2N_EARLY_DATA_STATUS_NOT_REQUESTED,
    S2N_EARLY_DATA_STATUS_REJECTED,
    S2N_EARLY_DATA_STATUS_END,
} s2n_early_data_status_t;

/**
 * Reports the current state of early data for a connection.
 *
 * See `s2n_early_data_status_t` for all possible states.
 *
 * @param conn A pointer to the connection
 * @param status A pointer which will be set to the current early data status
 * @returns A POSIX error signal.
 */
S2N_API int s2n_connection_get_early_data_status(struct s2n_connection *conn, s2n_early_data_status_t *status);

/**
 * Reports the remaining size of the early data allowed by a connection.
 *
 * If early data was rejected or not requested, the remaining early data size is 0.
 * Otherwise, the remaining early data size is the maximum early data allowed by the connection,
 * minus the early data sent or received so far.
 *
 * @param conn A pointer to the connection
 * @param allowed_early_data_size A pointer which will be set to the remaining early data currently allowed by `conn`
 * @returns A POSIX error signal.
 */
S2N_API int s2n_connection_get_remaining_early_data_size(struct s2n_connection *conn, uint32_t *allowed_early_data_size);

/**
 * Reports the maximum size of the early data allowed by a connection.
 *
 * This is the maximum amount of early data that can ever be sent and received for a connection.
 * It is not affected by the actual status of the early data, so can be non-zero even if early data
 * is rejected or not requested.
 *
 * @param conn A pointer to the connection
 * @param max_early_data_size A pointer which will be set to the maximum early data allowed by `conn`
 * @returns A POSIX error signal.
 */
S2N_API int s2n_connection_get_max_early_data_size(struct s2n_connection *conn, uint32_t *max_early_data_size);

/**
 * Called by the client to begin negotiation and send early data.
 *
 * See https://github.com/aws/s2n-tls/blob/main/docs/usage-guide/topics/ch14-early-data.md
 * for usage and examples. DO NOT USE unless you have considered the security issues and
 * implemented mitigation for anti-replay attacks.
 *
 * @param conn A pointer to the connection
 * @param data A pointer to the early data to be sent
 * @param data_len The size of the early data to send
 * @param data_sent A pointer which will be set to the size of the early data sent
 * @param blocked A pointer which will be set to the blocked status, as in `s2n_negotiate`.
 * @returns A POSIX error signal. The error should be handled as in `s2n_negotiate`.
 */
S2N_API int s2n_send_early_data(struct s2n_connection *conn, const uint8_t *data, ssize_t data_len,
        ssize_t *data_sent, s2n_blocked_status *blocked);

/**
 * Called by the server to begin negotiation and accept any early data the client sends.
 *
 * See https://github.com/aws/s2n-tls/blob/main/docs/usage-guide/topics/ch14-early-data.md
 * for usage and examples. DO NOT USE unless you have considered the security issues and
 * implemented mitigation for anti-replay attacks.
 *
 * @param conn A pointer to the connection
 * @param data A pointer to a buffer to store the early data received
 * @param max_data_len The size of the early data buffer
 * @param data_received A pointer which will be set to the size of the early data received
 * @param blocked A pointer which will be set to the blocked status, as in `s2n_negotiate`.
 * @returns A POSIX error signal. The error should be handled as in `s2n_negotiate`.
 */
S2N_API int s2n_recv_early_data(struct s2n_connection *conn, uint8_t *data, ssize_t max_data_len,
        ssize_t *data_received, s2n_blocked_status *blocked);

struct s2n_offered_early_data;

/**
 * A callback which can be implemented to accept or reject early data.
 *
 * This callback is triggered only after the server has determined early data is otherwise acceptable according
 * to the TLS early data specification. Implementations therefore only need to cover application-specific checks,
 * not the standard TLS early data validation.
 *
 * This callback can be synchronous or asynchronous. For asynchronous behavior, return success without
 * calling `s2n_offered_early_data_reject` or `s2n_offered_early_data_accept`. `early_data` will
 * still be a valid reference, and the connection will block until `s2n_offered_early_data_reject` or
 * `s2n_offered_early_data_accept` is called.
 *
 * @param conn A pointer to the connection
 * @param early_data A pointer which can be used to access information about the proposed early data
 *                   and then accept or reject it.
 * @returns A POSIX error signal. If unsuccessful, the connection will be closed with an error.
 */
typedef int (*s2n_early_data_cb)(struct s2n_connection *conn, struct s2n_offered_early_data *early_data);

/**
 * Set a callback to accept or reject early data.
 *
 * @param config A pointer to the connection config
 * @param cb A pointer to the implementation of the callback.
 * @returns A POSIX error signal. If successful, the callback was set.
 */
S2N_API int s2n_config_set_early_data_cb(struct s2n_config *config, s2n_early_data_cb cb);

/**
 * Get the length of the early data context set by the user.
 *
 * @param early_data A pointer to the early data information
 * @param context_len The length of the user context
 * @returns A POSIX error signal.
 */
S2N_API int s2n_offered_early_data_get_context_length(struct s2n_offered_early_data *early_data, uint16_t *context_len);

/**
 * Get the early data context set by the user.
 *
 * @param early_data A pointer to the early data information
 * @param context A byte buffer to copy the user context into
 * @param max_len The size of `context`. Must be >= to the result of `s2n_offered_early_data_get_context_length`.
 * @returns A POSIX error signal.
 */
S2N_API int s2n_offered_early_data_get_context(struct s2n_offered_early_data *early_data, uint8_t *context, uint16_t max_len);

/**
 * Reject early data offered by the client.
 *
 * @param early_data A pointer to the early data information
 * @returns A POSIX error signal. If success, the client's early data will be rejected.
 */
S2N_API int s2n_offered_early_data_reject(struct s2n_offered_early_data *early_data);

/**
 * Accept early data offered by the client.
 *
 * @param early_data A pointer to the early data information
 * @returns A POSIX error signal. If success, the client's early data will be accepted.
 */
S2N_API int s2n_offered_early_data_accept(struct s2n_offered_early_data *early_data);

/**
 * Retrieves the list of supported groups configured by the security policy associated with `config`.
 *
 * The retrieved list of groups will contain all of the supported groups for a security policy that are compatible
 * with the build of s2n-tls. For instance, PQ kem groups that are not supported by the linked libcrypto will not
 * be written. Otherwise, all of the supported groups configured for the security policy will be written. This API
 * can be used with the s2n_client_hello_get_supported_groups() API as a means of comparing compatibility between
 * a client and server.
 *
 * IANA values for each of the supported groups are written to the provided `groups` array, and `groups_count` is
 * set to the number of written supported groups.
 *
 * `groups_count_max` should be set to the maximum capacity of the `groups` array. If `groups_count_max` is less
 * than the number of supported groups configured by the security policy, this function will error.
 *
 * Note that this API retrieves only the groups from a security policy that are available to negotiate via the
 * supported groups extension, and does not return TLS 1.2 PQ kem groups that are negotiated in the supported PQ
 * kem parameters extension.
 *
 * @param config A pointer to the s2n_config object from which the supported groups will be retrieved.
 * @param groups The array to populate with the supported groups.
 * @param groups_count_max The maximum number of supported groups that can fit in the `groups` array.
 * @param groups_count Set to the number of supported groups written to `groups`.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure.
 */
S2N_API int s2n_config_get_supported_groups(struct s2n_config *config, uint16_t *groups, uint16_t groups_count_max,
        uint16_t *groups_count);

/* Indicates which serialized connection version will be provided. The default value is
 * S2N_SERIALIZED_CONN_NONE, which indicates the feature is off.
 */
typedef enum {
    S2N_SERIALIZED_CONN_NONE = 0,
    S2N_SERIALIZED_CONN_V1 = 1
} s2n_serialization_version;

/**
 * Set what version to use when serializing connections
 *
 * A version is required to serialize connections. Versioning ensures that all features negotiated
 * during the handshake will be available wherever the connection is deserialized. Applications may
 * need to update this version to pick up new features, since versioning may disable newer TLS
 * features to ensure compatibility.
 *
 * @param config A pointer to the config object.
 * @param version The requested version.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on error.
 */
S2N_API int s2n_config_set_serialization_version(struct s2n_config *config, s2n_serialization_version version);

/**
 * Retrieves the length of the serialized connection from `s2n_connection_serialize()`. Should be
 * used to allocate enough memory for the serialized connection buffer.
 *
 * @note The size of the serialized connection changes based on parameters negotiated in the TLS
 * handshake. Do not expect the size to always remain the same.
 *
 * @param conn A pointer to the connection object.
 * @param length Output parameter where the length will be written.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on error.
 */
S2N_API int s2n_connection_serialization_length(struct s2n_connection *conn, uint32_t *length);

/**
 * Serializes the s2n_connection into the provided buffer.
 *
 * This API takes an established s2n-tls connection object and "serializes" it
 * into a transferable object to be sent off-box or to another process. This transferable object can
 * then be "deserialized" using the `s2n_connection_deserialize` method to instantiate an s2n-tls
 * connection object that can talk to the original peer with the same encryption keys.
 *
 * @warning This feature is dangerous because it provides cryptographic material from a TLS session
 * in plaintext. Users MUST both encrypt and MAC the contents of the outputted material to provide
 * secrecy and integrity if this material is transported off-box. DO NOT store or send this material off-box
 * without encryption.
 *
 * @note You MUST have used `s2n_config_set_serialization_version()` to set a version on the
 * s2n_config object associated with this connection before this connection began its TLS handshake.
 * @note Call `s2n_connection_serialization_length` to retrieve the amount of memory needed for the
 * buffer parameter.
 * @note This API will error if the handshake is not yet complete.
 *
 * @param conn A pointer to the connection object.
 * @param buffer A pointer to the buffer where the serialized connection will be written.
 * @param buffer_length Maximum amount of data that can be written to the buffer param.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on error.
 */
S2N_API int s2n_connection_serialize(struct s2n_connection *conn, uint8_t *buffer, uint32_t buffer_length);

/**
 * Deserializes the provided buffer into the `s2n_connection` parameter.
 *
 * @warning s2n-tls DOES NOT check the integrity of the provided buffer. s2n-tls may successfully 
 * deserialize a corrupted buffer which WILL cause a connection failure when attempting to resume
 * sending/receiving encrypted data. To avoid this, it is recommended to MAC and encrypt the serialized 
 * connection before sending it off-box and deserializing it.
 *
 * @warning Only a minimal amount of information about the original TLS connection is serialized.
 * Therefore, after deserialization, the connection will behave like a new `s2n_connection` from the 
 * `s2n_connection_new()` call, except that it can read/write encrypted data from a peer. Any desired
 * config-level or connection-level configuration will need to be re-applied to the deserialized connection.
 * For this same reason none of the connection getters will return useful information about the 
 * original connection after deserialization. Any information about the original connection needs to
 * be retrieved before serialization.
 *
 * @param conn A pointer to the connection object. Should be a new s2n_connection object.
 * @param buffer A pointer to the buffer where the serialized connection will be read from.
 * @param buffer_length Maximum amount of data that can be read from the buffer parameter.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on error.
 */
S2N_API int s2n_connection_deserialize(struct s2n_connection *conn, uint8_t *buffer, uint32_t buffer_length);

/* Load all acceptable certificate authorities from the currently configured trust store.
 *
 * The loaded certificate authorities will be advertised during the handshake.
 * This can help your peer select a certificate if they have multiple certificate
 * chains available.
 *
 * For now, s2n-tls only supports advertising certificate authorities to support
 * client auth, so only servers will send the list of certificate authorities.
 *
 * To avoid configuration mistakes, certificate authorities cannot be loaded from
 * a trust store that includes the default system certificates. That means that
 * s2n_config_new_minimal or s2n_config_wipe_trust_store should be used.
 *
 * s2n-tls currently limits the total certificate authorities size to 10k bytes.
 * This method will fail if the certificate authorities retrieved from the trust
 * store exceed that limit.
 *
 * @param config A pointer to the s2n_config object.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure.
 */
S2N_API int s2n_config_set_cert_authorities_from_trust_store(struct s2n_config *config);

#ifdef __cplusplus
}
#endif
