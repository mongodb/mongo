#ifndef AWS_IO_TLS_CHANNEL_HANDLER_H
#define AWS_IO_TLS_CHANNEL_HANDLER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/byte_buf.h>
#include <aws/common/ref_count.h>
#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_TLS_NEGOTIATED_PROTOCOL_MESSAGE 0x01

struct aws_channel_slot;
struct aws_channel_handler;
struct aws_pkcs11_session;
struct aws_string;

enum aws_tls_versions {
    AWS_IO_SSLv3,
    AWS_IO_TLSv1,
    AWS_IO_TLSv1_1,
    AWS_IO_TLSv1_2,
    AWS_IO_TLSv1_3,
    AWS_IO_TLS_VER_SYS_DEFAULTS = 128,
};

enum aws_tls_cipher_pref {
    AWS_IO_TLS_CIPHER_PREF_SYSTEM_DEFAULT = 0,

    /* Deprecated */ AWS_IO_TLS_CIPHER_PREF_KMS_PQ_TLSv1_0_2019_06 = 1,
    /* Deprecated */ AWS_IO_TLS_CIPHER_PREF_KMS_PQ_SIKE_TLSv1_0_2019_11 = 2,
    /* Deprecated */ AWS_IO_TLS_CIPHER_PREF_KMS_PQ_TLSv1_0_2020_02 = 3,
    /* Deprecated */ AWS_IO_TLS_CIPHER_PREF_KMS_PQ_SIKE_TLSv1_0_2020_02 = 4,
    /* Deprecated */ AWS_IO_TLS_CIPHER_PREF_KMS_PQ_TLSv1_0_2020_07 = 5,
    /* Deprecated */ AWS_IO_TLS_CIPHER_PREF_PQ_TLSv1_0_2021_05 = 6,

    /*
     * This TLS cipher preference list contains post-quantum key exchange algorithms that have been standardized by
     * NIST. PQ algorithms in this preference list will be used in hybrid mode, and always combined with a classical
     * ECDHE key exchange.
     */
    AWS_IO_TLS_CIPHER_PREF_PQ_TLSV1_2_2024_10 = 7,

    AWS_IO_TLS_CIPHER_PREF_END_RANGE = 0xFFFF
};

/**
 * The hash algorithm of a TLS private key operation. Any custom private key operation handlers are expected to perform
 * operations on the input TLS data using the correct hash algorithm or fail the operation.
 */
enum aws_tls_hash_algorithm {
    AWS_TLS_HASH_UNKNOWN,
    AWS_TLS_HASH_SHA1,
    AWS_TLS_HASH_SHA224,
    AWS_TLS_HASH_SHA256,
    AWS_TLS_HASH_SHA384,
    AWS_TLS_HASH_SHA512,
};

/**
 * The signature of a TLS private key operation. Any custom private key operation handlers are expected to perform
 * operations on the input TLS data using the correct signature algorithm or fail the operation.
 */
enum aws_tls_signature_algorithm {
    AWS_TLS_SIGNATURE_UNKNOWN,
    AWS_TLS_SIGNATURE_RSA,
    AWS_TLS_SIGNATURE_ECDSA,
};

/**
 * The TLS private key operation that needs to be performed by a custom private key operation handler when making
 * a connection using mutual TLS.
 */
enum aws_tls_key_operation_type {
    AWS_TLS_KEY_OPERATION_UNKNOWN,
    AWS_TLS_KEY_OPERATION_SIGN,
    AWS_TLS_KEY_OPERATION_DECRYPT,
};

struct aws_tls_ctx {
    struct aws_allocator *alloc;
    void *impl;
    struct aws_ref_count ref_count;
};

/**
 * Invoked upon completion of the TLS handshake. If successful error_code will be AWS_OP_SUCCESS, otherwise
 * the negotiation failed and immediately after this function is invoked, the channel will be shutting down.
 */
typedef void(aws_tls_on_negotiation_result_fn)(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int error_code,
    void *user_data);

/**
 * Only used if the TLS handler is the last handler in the channel. This allows you to read any data that
 * was read and decrypted by the handler. If you have application protocol channel handlers, this function
 * is not necessary and certainly not recommended.
 */
typedef void(aws_tls_on_data_read_fn)(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_byte_buf *buffer,
    void *user_data);

/**
 * Invoked when an error occurs in the TLS state machine AFTER the handshake has completed. This function should only
 * be used in conjunction with the rules of aws_tls_on_data_read_fn.
 */
typedef void(aws_tls_on_error_fn)(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int err,
    const char *message,
    void *user_data);

struct aws_tls_connection_options {
    /** semi-colon delimited list of protocols. Example:
     *  h2;http/1.1
     */
    struct aws_string *alpn_list;
    /**
     * Serves two purposes. If SNI is supported (hint... it is),
     * this sets the SNI extension.
     *
     * For X.509 validation this also sets the name that will be used
     * for verifying the subj alt name and common name of the peer's certificate.
     */
    struct aws_string *server_name;
    aws_tls_on_negotiation_result_fn *on_negotiation_result;
    aws_tls_on_data_read_fn *on_data_read;
    aws_tls_on_error_fn *on_error;
    void *user_data;
    struct aws_tls_ctx *ctx;
    bool advertise_alpn_message;
    uint32_t timeout_ms;
};

/**
 * A struct containing all of the data needed for a private key operation when
 * making a mutual TLS connection. This struct contains the data that needs
 * to be operated on, like performing a sign operation or a decrypt operation.
 */
struct aws_tls_key_operation;

struct aws_tls_ctx_options {
    struct aws_allocator *allocator;

    /**
     *  minimum tls version to use. If you just want us to use the
     *  system defaults, you can set: AWS_IO_TLS_VER_SYS_DEFAULTS. This
     *  has the added benefit of automatically picking up new TLS versions
     *  as your OS or distribution adds support.
     */
    enum aws_tls_versions minimum_tls_version;

    /**
     * The Cipher Preference List to use
     */
    enum aws_tls_cipher_pref cipher_pref;

    /**
     * A PEM armored PKCS#7 collection of CAs you want to trust as a string.
     * Only use this if it's a CA not currently installed on your system.
     */
    struct aws_byte_buf ca_file;
    /**
     * Only used on Unix systems using an openssl style trust API.
     * this is typically something like /etc/pki/tls/certs/"
     */
    struct aws_string *ca_path;
    /**
     * Sets ctx wide alpn string. This is most useful for servers.
     * This is a semi-colon delimited list. example:
     * h2;http/1.1
     */
    struct aws_string *alpn_list;
    /**
     * A PEM armored PKCS#7 certificate as a string.
     * It is supported on every operating system.
     */
    struct aws_byte_buf certificate;

#ifdef _WIN32
    /** The path to a system
     * installed certficate/private key pair. Example:
     * CurrentUser\\MY\\<thumprint>
     */
    const char *system_certificate_path;
#endif

    /**
     * A PEM armored PKCS#7 private key as a string.
     *
     * On windows, this field should be NULL only if you are
     * using a system installed certficate.
     */
    struct aws_byte_buf private_key;

#ifdef __APPLE__
    /**
     * Apple Only!
     *
     * On Apple OS you can also use a pkcs#12 for your certificate
     * and private key. This is the contents the certificate.
     */
    struct aws_byte_buf pkcs12;

    /**
     * Password for the pkcs12 data in pkcs12.
     */
    struct aws_byte_buf pkcs12_password;

#    if !defined(AWS_OS_IOS)
    /**
     * On Apple OS you can also use a custom keychain instead of
     * the default keychain of the account.
     */
    struct aws_string *keychain_path;
#    endif

#endif

    /** max tls fragment size. Default is the value of g_aws_channel_max_fragment_size. */
    size_t max_fragment_size;

    /**
     * default is true for clients and false for servers.
     * You should not change this default for clients unless
     * you're testing and don't want to fool around with CA trust stores.
     * Before you release to production, you'll want to turn this back on
     * and add your custom CA to the aws_tls_ctx_options.
     *
     * If you set this in server mode, it enforces client authentication.
     */
    bool verify_peer;

    /**
     * For use when adding BYO_CRYPTO implementations. You can set extra data in here for use with your TLS
     * implementation.
     */
    void *ctx_options_extension;

    /**
     * Set if using custom private key operations.
     * See aws_custom_key_op_handler for more details
     *
     * Note: Custom key operations (and PKCS#11 integration) hasn't been tested with TLS 1.3, so don't use
     * cipher preferences that allow TLS 1.3. If this is set, we will always use non TLS 1.3 preferences.
     */
    struct aws_custom_key_op_handler *custom_key_op_handler;
};

struct aws_tls_negotiated_protocol_message {
    struct aws_byte_buf protocol;
};

typedef struct aws_channel_handler *(
    *aws_tls_on_protocol_negotiated)(struct aws_channel_slot *new_slot, struct aws_byte_buf *protocol, void *user_data);

/**
 * An enum for the current state of tls negotiation within a tls channel handler
 */
enum aws_tls_negotiation_status {
    AWS_TLS_NEGOTIATION_STATUS_NONE,
    AWS_TLS_NEGOTIATION_STATUS_ONGOING,
    AWS_TLS_NEGOTIATION_STATUS_SUCCESS,
    AWS_TLS_NEGOTIATION_STATUS_FAILURE
};

#ifdef BYO_CRYPTO
/**
 * Callback for creating a TLS handler. If you're using this you're using BYO_CRYPTO. This function should return
 * a fully implemented aws_channel_handler instance for TLS. Note: the aws_tls_options passed to your
 * aws_tls_handler_new_fn contains multiple callbacks. Namely: aws_tls_on_negotiation_result_fn. You are responsible for
 * invoking this function when TLs session negotiation has completed.
 */
typedef struct aws_channel_handler *(aws_tls_handler_new_fn)(struct aws_allocator *allocator,
                                                             struct aws_tls_connection_options *options,
                                                             struct aws_channel_slot *slot,
                                                             void *user_data);

/**
 * Invoked when it's time to start TLS negotiation. Note: the aws_tls_options passed to your aws_tls_handler_new_fn
 * contains multiple callbacks. Namely: aws_tls_on_negotiation_result_fn. You are responsible for invoking this function
 * when TLS session negotiation has completed.
 */
typedef int(aws_tls_client_handler_start_negotiation_fn)(struct aws_channel_handler *handler, void *user_data);

struct aws_tls_byo_crypto_setup_options {
    aws_tls_handler_new_fn *new_handler_fn;
    /* ignored for server implementations, required for clients. */
    aws_tls_client_handler_start_negotiation_fn *start_negotiation_fn;
    void *user_data;
};

#endif /* BYO_CRYPTO */

AWS_EXTERN_C_BEGIN

/******************************** tls options init stuff ***********************/
/**
 * Initializes options with default client options
 */
AWS_IO_API void aws_tls_ctx_options_init_default_client(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator);
/**
 * Cleans up resources allocated by init_* functions
 */
AWS_IO_API void aws_tls_ctx_options_clean_up(struct aws_tls_ctx_options *options);

/**
 * Initializes options for use with mutual tls in client mode.
 * cert_path and pkey_path are paths to files on disk. cert_path
 * and pkey_path are treated as PKCS#7 PEM armored. They are loaded
 * from disk and stored in buffers internally.
 *
 * NOTE: This is unsupported on iOS.
 */
AWS_IO_API int aws_tls_ctx_options_init_client_mtls_from_path(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    const char *cert_path,
    const char *pkey_path);

/**
 * Initializes options for use with mutual tls in client mode.
 * cert and pkey are copied. cert and pkey are treated as PKCS#7 PEM
 * armored.
 *
 * NOTE: This is unsupported on iOS.
 */
AWS_IO_API int aws_tls_ctx_options_init_client_mtls(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *cert,
    const struct aws_byte_cursor *pkey);

/**
 * vtable for aws_custom_key_op_handler.
 */
struct aws_custom_key_op_handler_vtable {
    /**
     * Called when the a TLS handshake has an operation it needs the custom key operation handler to perform.
     * NOTE: You must call aws_tls_key_operation_complete() or aws_tls_key_operation_complete_with_error()
     * otherwise the TLS handshake will stall the TLS connection indefinitely and leak memory.
     */
    void (*on_key_operation)(struct aws_custom_key_op_handler *key_op_handler, struct aws_tls_key_operation *operation);
};

/**
 * The custom key operation that is used when performing a mutual TLS handshake. This can
 * be extended to provide custom private key operations, like PKCS11 or similar.
 */
struct aws_custom_key_op_handler {
    /**
     * A void* intended to be populated with a reference to whatever class is extending this class. For example,
     * if you have extended aws_custom_key_op_handler with a custom struct, you would put a pointer to this struct
     * to *impl so you can retrieve it back in the vtable functions.
     */
    void *impl;

    /**
     * A vtable containing all of the functions the aws_custom_key_op_handler implements. Is intended to be extended.
     * NOTE: Use "aws_custom_key_op_handler_<func>" to access vtable functions.
     */
    const struct aws_custom_key_op_handler_vtable *vtable;

    /**
     * A reference count for handling memory usage.
     * Use aws_custom_key_op_handler_acquire and aws_custom_key_op_handler_release to increase/decrease count.
     */
    struct aws_ref_count ref_count;
};

/**
 * Increases the reference count for the passed-in aws_custom_key_op_handler and returns it.
 */
AWS_IO_API struct aws_custom_key_op_handler *aws_custom_key_op_handler_acquire(
    struct aws_custom_key_op_handler *key_op_handler);

/**
 * Decreases the reference count for the passed-in aws_custom_key_op_handler and returns NULL.
 */
AWS_IO_API struct aws_custom_key_op_handler *aws_custom_key_op_handler_release(
    struct aws_custom_key_op_handler *key_op_handler);

/**
 * Calls the on_key_operation vtable function. See aws_custom_key_op_handler_vtable for function details.
 */
AWS_IO_API void aws_custom_key_op_handler_perform_operation(
    struct aws_custom_key_op_handler *key_op_handler,
    struct aws_tls_key_operation *operation);

/**
 * Initializes options for use with mutual TLS in client mode,
 * where private key operations are handled by custom code.
 *
 * Note: cert_file_contents will be copied into a new buffer after this
 * function is called, so you do not need to keep that data alive
 * after calling this function.
 *
 * @param options               aws_tls_ctx_options to be initialized.
 * @param allocator             Allocator to use.
 * @param custom                Options for custom key operations.
 * @param cert_file_contents    The contents of a certificate file.
 */
AWS_IO_API int aws_tls_ctx_options_init_client_mtls_with_custom_key_operations(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    struct aws_custom_key_op_handler *custom,
    const struct aws_byte_cursor *cert_file_contents);

/**
 * This struct exists as a graceful way to pass many arguments when
 * calling init-with-pkcs11 functions on aws_tls_ctx_options (this also makes
 * it easy to introduce optional arguments in the future).
 * Instances of this struct should only exist briefly on the stack.
 *
 * Instructions for binding this to high-level languages:
 * - Python: The members of this struct should be the keyword args to the init-with-pkcs11 functions.
 * - JavaScript: This should be an options map passed to init-with-pkcs11 functions.
 * - Java: This should be an options class passed to init-with-pkcs11 functions.
 * - C++: Same as Java
 *
 * Notes on integer types:
 * PKCS#11 uses `unsigned long` for IDs, handles, etc but we expose them as `uint64_t` in public APIs.
 * We do this because sizeof(long) is inconsistent across platform/arch/language
 * (ex: always 64bit in Java, always 32bit in C on Windows, matches CPU in C on Linux and Apple).
 * By using uint64_t in our public API, we can keep the careful bounds-checking all in one
 * place, instead of expecting each high-level language binding to get it just right.
 */
struct aws_tls_ctx_pkcs11_options {
    /**
     * The PKCS#11 library to use.
     * This field is required.
     */
    struct aws_pkcs11_lib *pkcs11_lib;

    /**
     * User PIN, for logging into the PKCS#11 token (UTF-8).
     * Zero out to log into a token with a "protected authentication path".
     */
    struct aws_byte_cursor user_pin;

    /**
     * ID of slot containing PKCS#11 token.
     * If set to NULL, the token will be chosen based on other criteria
     * (such as token label).
     */
    const uint64_t *slot_id;

    /**
     * Label of PKCS#11 token to use.
     * If zeroed out, the token will be chosen based on other criteria
     * (such as slot ID).
     */
    struct aws_byte_cursor token_label;

    /**
     * Label of private key object on PKCS#11 token (UTF-8).
     * If zeroed out, the private key will be chosen based on other criteria
     * (such as being the only available private key on the token).
     */
    struct aws_byte_cursor private_key_object_label;

    /**
     * Certificate's file path on disk (UTF-8).
     * The certificate must be PEM formatted and UTF-8 encoded.
     * Zero out if passing in certificate by some other means (such as file contents).
     */
    struct aws_byte_cursor cert_file_path;

    /**
     * Certificate's file contents (UTF-8).
     * The certificate must be PEM formatted and UTF-8 encoded.
     * Zero out if passing in certificate by some other means (such as file path).
     */
    struct aws_byte_cursor cert_file_contents;
};

/**
 * Initializes options for use with mutual TLS in client mode,
 * where a PKCS#11 library provides access to the private key.
 *
 * NOTE: This only works on Unix devices.
 *
 * @param options           aws_tls_ctx_options to be initialized.
 * @param allocator         Allocator to use.
 * @param pkcs11_options    Options for using PKCS#11 (contents are copied)
 */
AWS_IO_API int aws_tls_ctx_options_init_client_mtls_with_pkcs11(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    const struct aws_tls_ctx_pkcs11_options *pkcs11_options);

/**
 * @Deprecated
 *
 * Sets a custom keychain path for storing the cert and pkey with mutual tls in client mode.
 *
 * NOTE: This only works on MacOS.
 */
AWS_IO_API int aws_tls_ctx_options_set_keychain_path(
    struct aws_tls_ctx_options *options,
    struct aws_byte_cursor *keychain_path_cursor);

/**
 * Initializes options for use with in server mode.
 * cert_path and pkey_path are paths to files on disk. cert_path
 * and pkey_path are treated as PKCS#7 PEM armored. They are loaded
 * from disk and stored in buffers internally.
 */
AWS_IO_API int aws_tls_ctx_options_init_default_server_from_path(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    const char *cert_path,
    const char *pkey_path);

/**
 * Initializes options for use with in server mode.
 * cert and pkey are copied. cert and pkey are treated as PKCS#7 PEM
 * armored.
 */
AWS_IO_API int aws_tls_ctx_options_init_default_server(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    struct aws_byte_cursor *cert,
    struct aws_byte_cursor *pkey);

/**
 * Initializes options for use with mutual tls in client mode.
 * cert_reg_path is the path to a system
 * installed certficate/private key pair. Example:
 * CurrentUser\\MY\\<thumprint>
 *
 * NOTE: This only works on Windows.
 */
AWS_IO_API int aws_tls_ctx_options_init_client_mtls_from_system_path(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    const char *cert_reg_path);

/**
 * Initializes options for use with server mode.
 * cert_reg_path is the path to a system
 * installed certficate/private key pair. Example:
 * CurrentUser\\MY\\<thumprint>
 *
 * NOTE: This only works on Windows.
 */
AWS_IO_API int aws_tls_ctx_options_init_default_server_from_system_path(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    const char *cert_reg_path);

/**
 * Initializes options for use with mutual tls in client mode.
 * pkcs12_path is a path to a file on disk containing a pkcs#12 file. The file is loaded
 * into an internal buffer. pkcs_pwd is the corresponding password for the pkcs#12 file; it is copied.
 *
 * NOTE: This only works on Apple devices.
 */
AWS_IO_API int aws_tls_ctx_options_init_client_mtls_pkcs12_from_path(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    const char *pkcs12_path,
    const struct aws_byte_cursor *pkcs_pwd);

/**
 * Initializes options for use with mutual tls in client mode.
 * pkcs12 is a buffer containing a pkcs#12 certificate and private key; it is copied.
 * pkcs_pwd is the corresponding password for the pkcs#12 buffer; it is copied.
 *
 * NOTE: This only works on Apple devices.
 */
AWS_IO_API int aws_tls_ctx_options_init_client_mtls_pkcs12(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    struct aws_byte_cursor *pkcs12,
    struct aws_byte_cursor *pkcs_pwd);

/**
 * Initializes options for use in server mode.
 * pkcs12_path is a path to a file on disk containing a pkcs#12 file. The file is loaded
 * into an internal buffer. pkcs_pwd is the corresponding password for the pkcs#12 file; it is copied.
 *
 * NOTE: This only works on Apple devices.
 */
AWS_IO_API int aws_tls_ctx_options_init_server_pkcs12_from_path(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    const char *pkcs12_path,
    struct aws_byte_cursor *pkcs_password);

/**
 * Initializes options for use in server mode.
 * pkcs12 is a buffer containing a pkcs#12 certificate and private key; it is copied.
 * pkcs_pwd is the corresponding password for the pkcs#12 buffer; it is copied.
 *
 * NOTE: This only works on Apple devices.
 */
AWS_IO_API int aws_tls_ctx_options_init_server_pkcs12(
    struct aws_tls_ctx_options *options,
    struct aws_allocator *allocator,
    struct aws_byte_cursor *pkcs12,
    struct aws_byte_cursor *pkcs_password);

/**
 * Sets alpn list in the form <protocol1;protocol2;...>. A maximum of 4 protocols are supported.
 * alpn_list is copied.
 */
AWS_IO_API int aws_tls_ctx_options_set_alpn_list(struct aws_tls_ctx_options *options, const char *alpn_list);

/**
 * Enables or disables x.509 validation. Disable this only for testing. To enable mutual TLS in server mode,
 * set verify_peer to true.
 */
AWS_IO_API void aws_tls_ctx_options_set_verify_peer(struct aws_tls_ctx_options *options, bool verify_peer);

/**
 * Sets preferred TLS Cipher List
 */
AWS_IO_API void aws_tls_ctx_options_set_tls_cipher_preference(
    struct aws_tls_ctx_options *options,
    enum aws_tls_cipher_pref cipher_pref);

/**
 * Sets the minimum TLS version to allow.
 */
AWS_IO_API void aws_tls_ctx_options_set_minimum_tls_version(
    struct aws_tls_ctx_options *options,
    enum aws_tls_versions minimum_tls_version);

/**
 * Override the default trust store. ca_file is a buffer containing a PEM armored chain of trusted CA certificates.
 * ca_file is copied.
 */
AWS_IO_API int aws_tls_ctx_options_override_default_trust_store(
    struct aws_tls_ctx_options *options,
    const struct aws_byte_cursor *ca_file);

/**
 * Override the default trust store. ca_path is a path to a directory on disk containing trusted certificates. This is
 * only supported on Unix systems (otherwise this parameter is ignored). ca_file is a path to a file on disk containing
 * trusted certificates. ca_file is loaded from disk and stored in an internal buffer.
 */
AWS_IO_API int aws_tls_ctx_options_override_default_trust_store_from_path(
    struct aws_tls_ctx_options *options,
    const char *ca_path,
    const char *ca_file);

/**
 * When implementing BYO_CRYPTO, if you need extra data to pass to your tls implementation, set it here. The lifetime of
 * extension_data must outlive the options object and be cleaned up after options is cleaned up.
 */
AWS_IO_API void aws_tls_ctx_options_set_extension_data(struct aws_tls_ctx_options *options, void *extension_data);

/**
 * Initializes default connection options from an instance ot aws_tls_ctx.
 */
AWS_IO_API void aws_tls_connection_options_init_from_ctx(
    struct aws_tls_connection_options *conn_options,
    struct aws_tls_ctx *ctx);

/**
 * Cleans up resources in aws_tls_connection_options. This can be called immediately after initializing
 * a tls handler, or if using the bootstrap api, immediately after asking for a channel.
 */
AWS_IO_API void aws_tls_connection_options_clean_up(struct aws_tls_connection_options *connection_options);

/**
 * Cleans up 'to' and copies 'from' to 'to'.
 * 'to' must be initialized.
 */
AWS_IO_API int aws_tls_connection_options_copy(
    struct aws_tls_connection_options *to,
    const struct aws_tls_connection_options *from);

/**
 * Sets callbacks for use with a tls connection.
 */
AWS_IO_API void aws_tls_connection_options_set_callbacks(
    struct aws_tls_connection_options *conn_options,
    aws_tls_on_negotiation_result_fn *on_negotiation_result,
    aws_tls_on_data_read_fn *on_data_read,
    aws_tls_on_error_fn *on_error,
    void *user_data);

/**
 * Sets server name to use for the SNI extension (supported everywhere), as well as x.509 validation. If you don't
 * set this, your x.509 validation will likely fail.
 */
AWS_IO_API int aws_tls_connection_options_set_server_name(
    struct aws_tls_connection_options *conn_options,
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *server_name);

/**
 * Sets alpn list in the form <protocol1;protocol2;...>. A maximum of 4 protocols are supported.
 * alpn_list is copied. This value is already inherited from aws_tls_ctx, but the aws_tls_ctx is expensive,
 * and should be used across as many connections as possible. If you want to set this per connection, set it here.
 */
AWS_IO_API int aws_tls_connection_options_set_alpn_list(
    struct aws_tls_connection_options *conn_options,
    struct aws_allocator *allocator,
    const char *alpn_list);

/********************************* TLS context and state management *********************************/

/**
 * Returns true if alpn is available in the underlying tls implementation.
 * This function should always be called before setting an alpn list.
 */
AWS_IO_API bool aws_tls_is_alpn_available(void);

/**
 * Returns true if this Cipher Preference is available in the underlying TLS implementation.
 * This function should always be called before setting a Cipher Preference
 */
AWS_IO_API bool aws_tls_is_cipher_pref_supported(enum aws_tls_cipher_pref cipher_pref);

/**
 * Creates a new tls channel handler in client mode. Options will be copied.
 * You must call aws_tls_client_handler_start_negotiation and wait on the
 * aws_tls_on_negotiation_result_fn callback before the handler can begin processing
 * application data.
 */
AWS_IO_API struct aws_channel_handler *aws_tls_client_handler_new(
    struct aws_allocator *allocator,
    struct aws_tls_connection_options *options,
    struct aws_channel_slot *slot);

/**
 * Creates a new tls channel handler in server mode. Options will be copied.
 * You must wait on the aws_tls_on_negotiation_result_fn callback before the handler can begin processing
 * application data.
 */
AWS_IO_API struct aws_channel_handler *aws_tls_server_handler_new(
    struct aws_allocator *allocator,
    struct aws_tls_connection_options *options,
    struct aws_channel_slot *slot);

#ifdef BYO_CRYPTO
/**
 * If using BYO_CRYPTO, you need to call this function prior to creating any client channels in the application.
 */
AWS_IO_API void aws_tls_byo_crypto_set_client_setup_options(const struct aws_tls_byo_crypto_setup_options *options);
/**
 * If using BYO_CRYPTO, you need to call this function prior to creating any server channels in the application.
 */
AWS_IO_API void aws_tls_byo_crypto_set_server_setup_options(const struct aws_tls_byo_crypto_setup_options *options);

#endif /* BYO_CRYPTO */

/**
 * Creates a channel handler, for client or server mode, that handles alpn. This isn't necessarily required
 * since you can always call aws_tls_handler_protocol in the aws_tls_on_negotiation_result_fn callback, but
 * this makes channel bootstrap easier to handle.
 */
AWS_IO_API struct aws_channel_handler *aws_tls_alpn_handler_new(
    struct aws_allocator *allocator,
    aws_tls_on_protocol_negotiated on_protocol_negotiated,
    void *user_data);

/**
 * Kicks off the negotiation process. This function must be called when in client mode to initiate the
 * TLS handshake. Once the handshake has completed the aws_tls_on_negotiation_result_fn will be invoked.
 */
AWS_IO_API int aws_tls_client_handler_start_negotiation(struct aws_channel_handler *handler);

#ifndef BYO_CRYPTO
/**
 * Creates a new server ctx. This ctx can be used for the lifetime of the application assuming you want the same
 * options for every incoming connection. Options will be copied.
 */
AWS_IO_API struct aws_tls_ctx *aws_tls_server_ctx_new(
    struct aws_allocator *alloc,
    const struct aws_tls_ctx_options *options);

/**
 * Creates a new client ctx. This ctx can be used for the lifetime of the application assuming you want the same
 * options for every outgoing connection. Options will be copied.
 */
AWS_IO_API struct aws_tls_ctx *aws_tls_client_ctx_new(
    struct aws_allocator *alloc,
    const struct aws_tls_ctx_options *options);
#endif /* BYO_CRYPTO */

/**
 * Increments the reference count on the tls context, allowing the caller to take a reference to it.
 *
 * Returns the same tls context passed in.
 */
AWS_IO_API struct aws_tls_ctx *aws_tls_ctx_acquire(struct aws_tls_ctx *ctx);

/**
 * Decrements a tls context's ref count.  When the ref count drops to zero, the object will be destroyed.
 */
AWS_IO_API void aws_tls_ctx_release(struct aws_tls_ctx *ctx);

/**
 * Returns a byte buffer by copy of the negotiated protocols. If there is no agreed upon protocol, len will be 0 and
 * buffer will be NULL.
 */
AWS_IO_API struct aws_byte_buf aws_tls_handler_protocol(struct aws_channel_handler *handler);

/**
 * Client mode only. This is the server name that was used for SNI and host name validation.
 */
AWS_IO_API struct aws_byte_buf aws_tls_handler_server_name(struct aws_channel_handler *handler);

/**************************** TLS KEY OPERATION *******************************/

/* Note: Currently this assumes the user knows what key is being used for key/cert pairs
         but s2n supports multiple cert/key pairs. This functionality is not used in the
         CRT currently, but in the future, we may need to implement this */

/**
 * Complete a successful TLS private key operation by providing its output.
 * The output is copied into the TLS connection.
 * The operation is freed by this call.
 *
 * You MUST call this or aws_tls_key_operation_complete_with_error().
 * Failure to do so will stall the TLS connection indefinitely and leak memory.
 */
AWS_IO_API
void aws_tls_key_operation_complete(struct aws_tls_key_operation *operation, struct aws_byte_cursor output);

/**
 * Complete an failed TLS private key operation.
 * The TLS connection will fail.
 * The operation is freed by this call.
 *
 * You MUST call this or aws_tls_key_operation_complete().
 * Failure to do so will stall the TLS connection indefinitely and leak memory.
 */
AWS_IO_API
void aws_tls_key_operation_complete_with_error(struct aws_tls_key_operation *operation, int error_code);

/**
 * Returns the input data that needs to be operated on by the custom key operation.
 */
AWS_IO_API
struct aws_byte_cursor aws_tls_key_operation_get_input(const struct aws_tls_key_operation *operation);

/**
 * Returns the type of operation that needs to be performed by the custom key operation.
 * If the implementation cannot perform the operation,
 * use aws_tls_key_operation_complete_with_error() to preventing stalling the TLS connection.
 */
AWS_IO_API
enum aws_tls_key_operation_type aws_tls_key_operation_get_type(const struct aws_tls_key_operation *operation);

/**
 * Returns the algorithm the operation is expected to be operated with.
 * If the implementation does not support the signature algorithm,
 * use aws_tls_key_operation_complete_with_error() to preventing stalling the TLS connection.
 */
AWS_IO_API
enum aws_tls_signature_algorithm aws_tls_key_operation_get_signature_algorithm(
    const struct aws_tls_key_operation *operation);

/**
 * Returns the algorithm the operation digest is signed with.
 * If the implementation does not support the digest algorithm,
 * use aws_tls_key_operation_complete_with_error() to preventing stalling the TLS connection.
 */
AWS_IO_API
enum aws_tls_hash_algorithm aws_tls_key_operation_get_digest_algorithm(const struct aws_tls_key_operation *operation);

/********************************* Misc TLS related *********************************/

/*
 * Injects a tls handler/slot into a channel and begins tls negotiation.
 * If desired, ALPN must be handled separately
 *
 * right_of_slot must be an existing slot in a channel
 */
AWS_IO_API int aws_channel_setup_client_tls(
    struct aws_channel_slot *right_of_slot,
    struct aws_tls_connection_options *tls_options);

/**
 * Given enum, return string like: AWS_TLS_HASH_SHA256 -> "SHA256"
 */
AWS_IO_API
const char *aws_tls_hash_algorithm_str(enum aws_tls_hash_algorithm hash);

/**
 * Given enum, return string like: AWS_TLS_SIGNATURE_RSA -> "RSA"
 */
AWS_IO_API
const char *aws_tls_signature_algorithm_str(enum aws_tls_signature_algorithm signature);

/**
 * Given enum, return string like: AWS_TLS_SIGNATURE_RSA -> "RSA"
 */
AWS_IO_API
const char *aws_tls_key_operation_type_str(enum aws_tls_key_operation_type operation_type);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_TLS_CHANNEL_HANDLER_H */
