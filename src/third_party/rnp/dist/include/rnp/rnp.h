/*-
 * Copyright (c) 2017-2021 Ribose Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <rnp/rnp_export.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Function return type. 0 == SUCCESS, all other values indicate an error.
 */
typedef uint32_t rnp_result_t;

#define RNP_KEY_EXPORT_ARMORED (1U << 0)
#define RNP_KEY_EXPORT_PUBLIC (1U << 1)
#define RNP_KEY_EXPORT_SECRET (1U << 2)
#define RNP_KEY_EXPORT_SUBKEYS (1U << 3)

/* Export base64-encoded autocrypt key instead of binary */
#define RNP_KEY_EXPORT_BASE64 (1U << 9)

#define RNP_KEY_REMOVE_PUBLIC (1U << 0)
#define RNP_KEY_REMOVE_SECRET (1U << 1)
#define RNP_KEY_REMOVE_SUBKEYS (1U << 2)

#define RNP_KEY_UNLOAD_PUBLIC (1U << 0)
#define RNP_KEY_UNLOAD_SECRET (1U << 1)

/**
 * Flags for optional details to include in JSON.
 */
#define RNP_JSON_PUBLIC_MPIS (1U << 0)
#define RNP_JSON_SECRET_MPIS (1U << 1)
#define RNP_JSON_SIGNATURES (1U << 2)
#define RNP_JSON_SIGNATURE_MPIS (1U << 3)

/**
 * Flags to include additional data in packet dumping
 */
#define RNP_JSON_DUMP_MPI (1U << 0)
#define RNP_JSON_DUMP_RAW (1U << 1)
#define RNP_JSON_DUMP_GRIP (1U << 2)

#define RNP_DUMP_MPI (1U << 0)
#define RNP_DUMP_RAW (1U << 1)
#define RNP_DUMP_GRIP (1U << 2)

/**
 * Flags for the key loading/saving functions.
 */
#define RNP_LOAD_SAVE_PUBLIC_KEYS (1U << 0)
#define RNP_LOAD_SAVE_SECRET_KEYS (1U << 1)
#define RNP_LOAD_SAVE_PERMISSIVE (1U << 8)
#define RNP_LOAD_SAVE_SINGLE (1U << 9)
#define RNP_LOAD_SAVE_BASE64 (1U << 10)

/**
 * Flags for the rnp_key_remove_signatures
 */

#define RNP_KEY_SIGNATURE_INVALID (1U << 0)
#define RNP_KEY_SIGNATURE_UNKNOWN_KEY (1U << 1)
#define RNP_KEY_SIGNATURE_NON_SELF_SIG (1U << 2)

#define RNP_KEY_SIGNATURE_KEEP (0U)
#define RNP_KEY_SIGNATURE_REMOVE (1U)

/**
 * Flags for output structure creation.
 */
#define RNP_OUTPUT_FILE_OVERWRITE (1U << 0)
#define RNP_OUTPUT_FILE_RANDOM (1U << 1)

/**
 * Flags for default key selection.
 */
#define RNP_KEY_SUBKEYS_ONLY (1U << 0)
#if defined(RNP_EXPERIMENTAL_PQC)
#define RNP_KEY_PREFER_PQC_ENC_SUBKEY (1U << 1)
#endif

/**
 * User id type
 */
#define RNP_USER_ID (1U)
#define RNP_USER_ATTR (2U)

/**
 * Predefined feature security levels
 */
#define RNP_SECURITY_PROHIBITED (0U)
#define RNP_SECURITY_INSECURE (1U)
#define RNP_SECURITY_DEFAULT (2U)

/**
 * Flags for feature security rules.
 */
#define RNP_SECURITY_OVERRIDE (1U << 0)
#define RNP_SECURITY_VERIFY_KEY (1U << 1)
#define RNP_SECURITY_VERIFY_DATA (1U << 2)
#define RNP_SECURITY_REMOVE_ALL (1U << 16)

/**
 * Encryption flags
 */
#define RNP_ENCRYPT_NOWRAP (1U << 0)

/**
 * Decryption/verification flags
 */
#define RNP_VERIFY_IGNORE_SIGS_ON_DECRYPT (1U << 0)
#define RNP_VERIFY_REQUIRE_ALL_SIGS (1U << 1)
#define RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT (1U << 2)

/**
 * Revocation key flags.
 */
#define RNP_REVOKER_SENSITIVE (1U << 0)

/**
 * Key feature flags.
 */
#define RNP_KEY_FEATURE_MDC (1U << 0)
#define RNP_KEY_FEATURE_AEAD (1U << 1)
#define RNP_KEY_FEATURE_V5 (1U << 2)

/**
 *  Key usage flags.
 */

#define RNP_KEY_USAGE_CERTIFY (1U << 0)
#define RNP_KEY_USAGE_SIGN (1U << 1)
#define RNP_KEY_USAGE_ENCRYPT_COMMS (1U << 2)
#define RNP_KEY_USAGE_ENCRYPT_STORAGE (1U << 3)

/**
 *  Key server preferences flags.
 */

#define RNP_KEY_SERVER_NO_MODIFY (1U << 7)

/**
 *  Signature validation flags.
 */

#define RNP_SIGNATURE_REVALIDATE (1U << 0)

/**
 * Return a constant string describing the result code
 */
RNP_API const char *rnp_result_to_string(rnp_result_t result);

RNP_API const char *rnp_version_string();
RNP_API const char *rnp_version_string_full();

/** return a value representing the version of librnp
 *
 *  This function is only useful for releases. For non-releases,
 *  it will return 0.
 *
 *  The value returned can be used in comparisons by utilizing
 *  rnp_version_for.
 *
 *  @return a value representing the librnp version
 **/
RNP_API uint32_t rnp_version();

/** return a value representing a specific version of librnp
 *
 *  This value can be used in comparisons.
 *
 *  @return a value representing a librnp version
 **/
RNP_API uint32_t rnp_version_for(uint32_t major, uint32_t minor, uint32_t patch);

/** return the librnp major version
 *
 *  @return
 **/
RNP_API uint32_t rnp_version_major(uint32_t version);

/** return the librnp minor version
 *
 *  @return
 **/
RNP_API uint32_t rnp_version_minor(uint32_t version);

/** return the librnp patch version
 *
 *  @return
 **/
RNP_API uint32_t rnp_version_patch(uint32_t version);

/** return a unix timestamp of the last commit, if available
 *
 *  This function is only useful for non-releases. For releases,
 *  it will return 0.
 *
 *  The intended usage is to provide a form of versioning for the main
 *  branch.
 *
 *  @return the unix timestamp of the last commit, or 0 if unavailable
 **/
RNP_API uint64_t rnp_version_commit_timestamp();

#ifndef RNP_NO_DEPRECATED
/** @brief This function is deprecated and should not be used anymore. It would just silently
 *         return RNP_SUCCESS.
 *
 * @param file name of the sourcer file. Use 'all' to enable debug for all code.
 *
 */
RNP_API RNP_DEPRECATED rnp_result_t rnp_enable_debug(const char *file);

/**
 * @brief This function is deprecated and should not be used anymore. It would just silently
 *        return RNP_SUCCESS.
 *
 */
RNP_API RNP_DEPRECATED rnp_result_t rnp_disable_debug();
#endif

/*
 * Opaque structures
 */
typedef struct rnp_ffi_st *                rnp_ffi_t;
typedef struct rnp_key_handle_st *         rnp_key_handle_t;
typedef struct rnp_input_st *              rnp_input_t;
typedef struct rnp_output_st *             rnp_output_t;
typedef struct rnp_op_generate_st *        rnp_op_generate_t;
typedef struct rnp_op_sign_st *            rnp_op_sign_t;
typedef struct rnp_op_sign_signature_st *  rnp_op_sign_signature_t;
typedef struct rnp_op_verify_st *          rnp_op_verify_t;
typedef struct rnp_op_verify_signature_st *rnp_op_verify_signature_t;
typedef struct rnp_op_encrypt_st *         rnp_op_encrypt_t;
typedef struct rnp_identifier_iterator_st *rnp_identifier_iterator_t;
typedef struct rnp_uid_handle_st *         rnp_uid_handle_t;
typedef struct rnp_signature_handle_st *   rnp_signature_handle_t;
typedef struct rnp_sig_subpacket_st *      rnp_sig_subpacket_t;
typedef struct rnp_recipient_handle_st *   rnp_recipient_handle_t;
typedef struct rnp_symenc_handle_st *      rnp_symenc_handle_t;

/* Callbacks */
/**
 * @brief Callback, used to read data from the source.
 *
 * @param app_ctx custom parameter, passed back to the function.
 * @param buf on successful call data should be put here. Cannot be NULL,
 *            and must be capable to store at least len bytes.
 * @param len number of bytes to read.
 * @param read on successful call number of read bytes must be put here.
 * @return true on success (including EOF condition), or false on read error.
 *         EOF case is indicated by zero bytes read on non-zero read call.
 */
typedef bool rnp_input_reader_t(void *app_ctx, void *buf, size_t len, size_t *read);
/**
 * @brief Callback, used to close input stream.
 *
 * @param app_ctx custom parameter, passed back to the function.
 * @return void
 */
typedef void rnp_input_closer_t(void *app_ctx);
/**
 * @brief Callback, used to write data to the output stream.
 *
 * @param app_ctx custom parameter, passed back to the function.
 * @param buf buffer with data, cannot be NULL.
 * @param len number of bytes to write.
 * @return true if call was successful and all data is written, or false otherwise.
 */
typedef bool rnp_output_writer_t(void *app_ctx, const void *buf, size_t len);

/**
 * @brief Callback, used to close output stream.
 *
 * @param app_ctx custom parameter, passed back to the function.
 * @param discard true if the already written data should be deleted.
 * @return void
 */
typedef void rnp_output_closer_t(void *app_ctx, bool discard);

/**
 * Callback used for getting a password.
 *
 * @param ffi
 * @param app_ctx provided by application
 * @param key the key, if any, for which the password is being requested.
 *        Note: this key handle should not be held by the application,
 *        it is destroyed after the callback. It should only be used to
 *        retrieve information like the userids, grip, etc.
 * @param pgp_context a descriptive string on why the password is being
 *        requested, may have one of the following values:
 *         - "add subkey": add subkey to the encrypted secret key
 *         - "add userid": add userid to the encrypted secret key
 *         - "sign": sign data
 *         - "decrypt": decrypt data using the encrypted secret key
 *         - "unlock": temporary unlock secret key (decrypting its fields), so it may be used
 *           later without need to decrypt
 *         - "protect": encrypt secret key fields
 *         - "unprotect": decrypt secret key fields, leaving those in a raw format
 *         - "decrypt (symmetric)": decrypt data, using the password
 *         - "encrypt (symmetric)": encrypt data, using the password
 * @param buf to which the callback should write the returned password, NULL terminated.
 * @param buf_len the size of buf
 * @return true if a password was provided, false otherwise
 */
typedef bool (*rnp_password_cb)(rnp_ffi_t        ffi,
                                void *           app_ctx,
                                rnp_key_handle_t key,
                                const char *     pgp_context,
                                char             buf[],
                                size_t           buf_len);

/** callback used to signal the application that a key is needed
 *
 *  The application should use the appropriate functions (rnp_load_keys() or
 *  rnp_import_keys()) to load the requested key.
 *
 *  This may be called multiple times for the same key. For example, if attempting
 *  to verify a signature, the signer's keyid may be used first to request the key.
 *  If that is not successful, the signer's fingerprint (if available) may be used.
 *
 *  Please note that there is a special case with 'hidden' recipient, with all-zero keyid. In
 *  this case implementation should load all available secret keys for the decryption attempt
 *  (or do nothing, in this case decryption to the hidden recipient would fail).
 *
 *  Situations in which this callback would be used include:
 *   - When decrypting data that includes a public-key encrypted session key,
 *     and the key is not found in the keyrings.
 *   - When attempting to verify a signature, when the signer's key is not found in
 *     the keyrings.
 *
 *  @param ffi
 *  @param app_ctx provided by application in rnp_ffi_set_key_provider()
 *  @param identifier_type the type of identifier ("userid", "keyid", "grip")
 *  @param identifier the identifier for locating the key
 *  @param secret true if a secret key is being requested
 */
typedef void (*rnp_get_key_cb)(rnp_ffi_t   ffi,
                               void *      app_ctx,
                               const char *identifier_type,
                               const char *identifier,
                               bool        secret);

/**
 * @brief callback used to report back signatures from the function
 *        rnp_key_remove_signatures(). This may be used to implement custom signature filtering
 *        code or record information about the signatures which are removed.
 * @param ffi
 * @param app_ctx custom context, provided by application.
 * @param sig signature handle to retrieve information about the signature. Callback must not
 *            call rnp_signature_handle_destroy() on it.
 * @param action action which will be performed on the signature. Currently defined are
 *               RNP_KEY_SIGNATURE_KEEP an RNP_KEY_SIGNATURE_REMOVE.
 *               Callback may overwrite this value.
 *
 */
typedef void (*rnp_key_signatures_cb)(rnp_ffi_t              ffi,
                                      void *                 app_ctx,
                                      rnp_signature_handle_t sig,
                                      uint32_t *             action);

/** create the top-level object used for interacting with the library
 *
 *  @param ffi pointer that will be set to the created ffi object
 *  @param pub_format the format of the public keyring, RNP_KEYSTORE_GPG or other
 *         RNP_KEYSTORE_* constant
 *  @param sec_format the format of the secret keyring, RNP_KEYSTORE_GPG or other
 *         RNP_KEYSTORE_* constant
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_ffi_create(rnp_ffi_t * ffi,
                                    const char *pub_format,
                                    const char *sec_format);

/** destroy the top-level object used for interacting with the library
 *
 *  Note that this invalidates key handles, keyrings, and any other
 *  objects associated with this particular object.
 *
 *  @param ffi the ffi object
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_ffi_destroy(rnp_ffi_t ffi);

RNP_API rnp_result_t rnp_ffi_set_log_fd(rnp_ffi_t ffi, int fd);

/**
 * @brief Set key provider callback. This callback would be called in case when required public
 *        or secret key is not loaded to the keyrings.
 *
 * @param ffi initialized ffi object, cannot be NULL.
 * @param getkeycb callback function. See rnp_get_key_cb documentation for details.
 * @param getkeycb_ctx implementation-specific context, which would be passed to the getkeycb
 *                      on invocation.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_ffi_set_key_provider(rnp_ffi_t      ffi,
                                              rnp_get_key_cb getkeycb,
                                              void *         getkeycb_ctx);
RNP_API rnp_result_t rnp_ffi_set_pass_provider(rnp_ffi_t       ffi,
                                               rnp_password_cb getpasscb,
                                               void *          getpasscb_ctx);

/* Operations on key rings */

/** retrieve the default homedir (example: /home/user/.rnp)
 *
 * @param homedir pointer that will be set to the homedir path.
 *        The caller should free this with rnp_buffer_destroy.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_get_default_homedir(char **homedir);

/** Try to detect the formats and paths of the homedir keyrings.
 * @param homedir the path to the home directory (example: /home/user/.rnp)
 * @param pub_format pointer that will be set to the format of the public keyring.
 *        The caller should free this with rnp_buffer_destroy.
 *        Note: this and below may be set to NULL in case of no known format is found.
 * @param pub_path pointer that will be set to the path to the public keyring.
 *        The caller should free this with rnp_buffer_destroy.
 * @param sec_format pointer that will be set to the format of the secret keyring.
 *        The caller should free this with rnp_buffer_destroy.
 * @param sec_path pointer that will be set to the path to the secret keyring.
 *        The caller should free this with rnp_buffer_destroy.
 * @return RNP_SUCCESS on success (even if no known format was found), or any other value on
 *         error.
 */
RNP_API rnp_result_t rnp_detect_homedir_info(
  const char *homedir, char **pub_format, char **pub_path, char **sec_format, char **sec_path);

/** try to detect the key format of the provided data
 *
 * @param buf the key data, must not be NULL
 * @param buf_len the size of the buffer, must be > 0
 * @param format pointer that will be set to the format of the keyring.
 *        Must not be NULL. The caller should free this with rnp_buffer_destroy.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_detect_key_format(const uint8_t buf[], size_t buf_len, char **format);

/** Get the number of s2k hash iterations, based on calculation time requested.
 *  Number of iterations is used to derive encryption key from password.
 *
 * @param hash hash algorithm to try
 * @param msec number of milliseconds which will be needed to derive key from the password.
 *             Since it depends on CPU speed the calculated value will make sense only for the
 *             system it was calculated for.
 * @param iterations approximate number of iterations to satisfy time complexity.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_calculate_iterations(const char *hash,
                                              size_t      msec,
                                              size_t *    iterations);

/** Check whether rnp supports specific feature (algorithm, elliptic curve, whatever else).
 *
 * @param type string with the feature type. See RNP_FEATURE_* defines for the supported
 * values.
 * @param name value of the feature to check whether it is supported.
 * @param supported will contain true or false depending whether feature is supported or not.
 * @return RNP_SUCCESS on success or any other value on error.
 */
RNP_API rnp_result_t rnp_supports_feature(const char *type, const char *name, bool *supported);

/** Get the JSON with array of supported rnp feature values (algorithms, curves, etc) by type.
 *
 * @param type type of the feature. See RNP_FEATURE_* defines for the supported values.
 * @param result after successful execution will contain the JSON array with supported feature
 *        string values. You must destroy it using the rnp_buffer_destroy() function.\n
 *        Example JSON array output listing available hash algorithms:\n
 *
 *            [
 *              "MD5",
 *              "SHA1",
 *              "RIPEMD160",
 *              "SHA256",
 *              "SHA384",
 *              "SHA512",
 *              "SHA224",
 *              "SHA3-256",
 *              "SHA3-512"
 *            ]
 *
 * @return RNP_SUCCESS on success or any other value on error.
 */
RNP_API rnp_result_t rnp_supported_features(const char *type, char **result);

/**
 * @brief Add new security rule to the FFI. Security rules allows to override default algorithm
 *        security settings by disabling them or marking as insecure. After creation of FFI
 *        object default rules are added, however caller may add more strict rules or
 *        completely overwrite rule table by calling rnp_remove_security_rule().
 *        Note: key signature validation status is cached, so rules should be changed before
 *        keyrings are loaded or keyring should be reloaded after updating rules.
 *
 * @param ffi initialized FFI object.
 * @param type type of the feature, cannot be NULL. Currently only RNP_FEATURE_HASH_ALG is
 *             supported.
 * @param name name of the feature, i.e. SHA1, MD5. The same values are used in
 *             rnp_supports_feature()/rnp_supported_features().
 * @param flags additional flags. Following ones currently supported:
 *              - RNP_SECURITY_OVERRIDE : override all other rules for the specified feature.
 *                May be used to temporarily enable or disable some feature value (e.g., to
 *                enable verification of SHA1 or MD5 signature), and then revert changes via
 *                rnp_remove_security_rule().
 *              - RNP_SECURITY_VERIFY_KEY : limit rule only to the key signature verification.
 *              - RNP_SECURITY_VERIFY_DATA : limit rule only to the data signature
 *                verification.
 *              Note: by default rule applies to all possible usages.
 *
 * @param from timestamp, from when the rule is active. Objects that have creation time (like
 *             signatures) are matched with the closest rules from the past, unless there is
 *             a rule with an override flag. For instance, given a single rule with algorithm
 *             'MD5', level 'insecure' and timestamp '2012-01-01', all signatures made before
 *             2012-01-01 using the MD5 hash algorithm are considered to be at the default
 *             security level (i.e., valid),  whereas all signatures made after 2021-01-01 will
 *             be marked as 'insecure' (i.e., invalid).
 * @param level security level of the rule. Currently the following ones are defined:
 *              - RNP_SECURITY_PROHIBITED : feature (for instance, MD5 algorithm) is completely
 *                disabled, so no processing can be done. In terms of signature check, that
 *                would mean the check will fail right after the hashing begins.
 *                Note: Currently it works in the same way as RNP_SECURITY_INSECURE.
 *              - RNP_SECURITY_INSECURE : feature (for instance, SHA1 algorithm) is marked as
 *                insecure. So even valid signatures, produced later than `from`, will be
 *                marked as invalid.
 *              - RNP_SECURITY_DEFAULT : feature is secure enough. Default value when there are
 *                no other rules for feature.
 *
 * @return RNP_SUCCESS or any other value on error.
 */
RNP_API rnp_result_t rnp_add_security_rule(rnp_ffi_t   ffi,
                                           const char *type,
                                           const char *name,
                                           uint32_t    flags,
                                           uint64_t    from,
                                           uint32_t    level);

/**
 * @brief Get security rule applicable for the corresponding feature value and timestamp.
 *        Note: if there is no matching rule, it will fall back to the default security level
 *        with empty flags and `from`.
 *
 * @param ffi initialized FFI object.
 * @param type feature type to search for. Only RNP_FEATURE_HASH_ALG is supported right now.
 * @param name feature name, i.e. SHA1 or so on.
 * @param time timestamp for which feature should be checked.
 * @param flags if non-NULL then rule's flags will be put here. In this case *flags must be
 *              initialized to the desired usage limitation:
 *               - 0 to look up for any usage (this is also assumed if flags parameter is
 *                 NULL).
 *               - RNP_SECURITY_VERIFY_KEY, RNP_SECURITY_VERIFY_DATA and so on to look up for
 *                 the specific usage. Please note that constants cannot be ORed here, only
 *                 single one must be present.
 * @param from if non-NULL then rule's from time will be put here.
 * @param level cannot be NULL. Security level will be stored here.
 * @return RNP_SUCCESS or any other value on error.
 */
RNP_API rnp_result_t rnp_get_security_rule(rnp_ffi_t   ffi,
                                           const char *type,
                                           const char *name,
                                           uint64_t    time,
                                           uint32_t *  flags,
                                           uint64_t *  from,
                                           uint32_t *  level);

/**
 * @brief Remove security rule(s), matching the parameters.
 *        Note: use this with caution, as this may also clear default security rules, so
 *        all affected features would be considered of the default security level.
 *
 * @param ffi populated FFI structure, cannot be NULL.
 * @param type type of the feature. If NULL, then all of the rules will be cleared.
 * @param name name of the feature. If NULL, then all rules of the type will be cleared.
 * @param level security level of the rule.
 * @param flags additional flags, following are defined at the moment:
 *          - RNP_SECURITY_OVERRIDE : rule should match this flag
 *          - RNP_SECURITY_VERIFY_KEY, RNP_SECURITY_VERIFY_DATA : rule should match these flags
 *            (can be ORed together)
 *          - RNP_SECURITY_REMOVE_ALL : remove all rules for type and name.
 * @param from timestamp, for when the rule should be removed. Ignored if
 *             RNP_SECURITY_REMOVE_ALL_FROM is specified.
 * @param removed if non-NULL then number of removed rules will be stored here.
 * @return RNP_SUCCESS on success or any other value on error. Please note that if no rules are
 *         matched, execution will be marked as successful. Use the `removed` parameter to
 *         check for this case.
 */
RNP_API rnp_result_t rnp_remove_security_rule(rnp_ffi_t   ffi,
                                              const char *type,
                                              const char *name,
                                              uint32_t    level,
                                              uint32_t    flags,
                                              uint64_t    from,
                                              size_t *    removed);

/**
 * @brief Request password via configured FFI's callback
 *
 * @param ffi initialized FFI structure
 * @param key key handle for which password is requested. May be NULL.
 * @param context string describing the purpose of password request. See description of
 *                rnp_password_cb for the list of possible values. Also you may use any
 *                custom one as far as your password callback handles it.
 * @param password password will be put here on success. Must be destroyed via
 *                 rnp_buffer_destroy(), also it is good idea to securely clear it via
 *                 rnp_buffer_clear().
 * @return RNP_SUCCESS or other value on error.
 */
RNP_API rnp_result_t rnp_request_password(rnp_ffi_t        ffi,
                                          rnp_key_handle_t key,
                                          const char *     context,
                                          char **          password);

/**
 * @brief Set timestamp, used in all operations instead of system's time. These operations
 *        include key/signature generation (this timestamp will be used as signature/key
 *        creation date), verification of the keys and signatures (this timestamp will be used
 *        as 'current' time).
 *        Please note, that exactly this timestamp will be used during the whole ffi lifetime.
 *
 * @param ffi initialized FFI structure
 * @param time non-zero timestamp to be used. Zero value restores original behaviour and uses
 *             system's time.
 * @return RNP_SUCCESS or other value on error.
 */
RNP_API rnp_result_t rnp_set_timestamp(rnp_ffi_t ffi, uint64_t time);

/** load keys
 *
 * Note that for G10, the input must be a directory (which must already exist).
 *
 * @param ffi
 * @param format the key format of the data (GPG, KBX, G10). Must not be NULL.
 * @param input source to read from.
 * @param flags the flags. See RNP_LOAD_SAVE_*.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_load_keys(rnp_ffi_t   ffi,
                                   const char *format,
                                   rnp_input_t input,
                                   uint32_t    flags);

/** unload public and/or secret keys
 *  Note: After unloading all key handles will become invalid and must be destroyed.
 * @param ffi
 * @param flags choose which keys should be unloaded (pubic, secret or both).
 *              See RNP_KEY_UNLOAD_PUBLIC/RNP_KEY_UNLOAD_SECRET.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_unload_keys(rnp_ffi_t ffi, uint32_t flags);

/** import keys to the keyring and receive JSON list of the new/updated keys.
 *  Note: this will work only with keys in OpenPGP format, use rnp_load_keys for other formats.
 * @param ffi
 * @param input source to read from. Cannot be NULL.
 * @param flags see RNP_LOAD_SAVE_* constants. If RNP_LOAD_SAVE_PERMISSIVE is specified
 *              then import process will skip unrecognized or bad keys/signatures instead of
 *              failing the whole operation.
 *              If flag RNP_LOAD_SAVE_SINGLE is set, then only first key will be loaded (subkey
 *              or primary key with its subkeys). In case RNP_LOAD_SAVE_PERMISSIVE and
 *              erroneous first key on the stream RNP_SUCCESS will be returned, but results
 *              will include an empty array. Also RNP_ERROR_EOF will be returned if the last
 *              key was read.
 *              RNP_LOAD_SAVE_BASE64 should set to allow import of base64-encoded keys (i.e.
 *              autocrypt ones). By default only binary and OpenPGP-armored keys are allowed.
 * @param results if not NULL then after the successful execution will contain JSON with
 *              information about new and updated keys. You must free it using the
 *              rnp_buffer_destroy() function.
 *              JSON output is an object containing array of objects named "keys".
 *              Each array item is an object representing an imported key.
 *              It contains the following members:\n
 * JSON member  | Description
 * -------------|------------
 * "public"     | string, status of a public key, one of "new", "updated", "unchanged", "none"
 * "secret"     | string, status of a secret key, same possible values as for public
 * "fingerprint"| string, hexadecimal fingerprint of the key
 *              Example of JSON output:\n
 *
 *                  {
 *                      "keys":[
 *                       {
 *                          "public":"unchanged",
 *                          "secret":"new",
 *                          "fingerprint":"090bd712a1166be572252c3c9747d2a6b3a63124"
 *                       }
 *                      ]
 *                  }
 *
 * @return RNP_SUCCESS on success
 *         RNP_ERROR_EOF if last key was read (if RNP_LOAD_SAVE_SINGLE was used)
 *         any other value on error.
 */
RNP_API rnp_result_t rnp_import_keys(rnp_ffi_t   ffi,
                                     rnp_input_t input,
                                     uint32_t    flags,
                                     char **     results);

/** import standalone signatures to the keyring and receive JSON list of the updated
 * signatures.
 *
 *  @param ffi
 *  @param input source to read from. Cannot be NULL.
 *  @param flags additional import flags, currently must be 0.
 *  @param results if not NULL then after the successful execution will contain JSON with
 *                 information about the updated signatures. You must free it using the
 *                 rnp_buffer_destroy() function.
 *                 JSON output is an object containing array of objects named "sigs".
 *                 Each array item is an object representing imported signature.
 *                 It contains the following members:\n
 * JSON member         | Description
 * --------------------|------------
 * "public"            |string, signature import status in a public keyring
 * "secret"            |string, signature import status in a secret keyring
 * "signer fingerprint"|string, optional, fingerprint of a signing key
 *                 "public" and "secret" status strings can have any of these string values:
 *                 "new", "unchanged", "unknown key", "none".
 *                 The "signer fingerprint" member could be missing
 *                 if the signer key is not available.\n
 *                 Example JSON output:\n
 *
 *                     {
 *                       "sigs":[
 *                         {
 *                           "public":"new",
 *                           "secret":"unknown key",
 *                           "signer fingerprint":"73edcc9119afc8e2dbbdcde50451409669ffde3c"
 *                         },
 *                         {
 *                           "public":"none",
 *                           "secret":"none",
 *                         }
 *                       ]
 *                     }
 *
 *  @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_import_signatures(rnp_ffi_t   ffi,
                                           rnp_input_t input,
                                           uint32_t    flags,
                                           char **     results);

/** save keys
 *
 * Note that for G10, the output must be a directory (which must already exist).
 *
 * @param ffi
 * @param format the key format of the data (GPG, KBX, G10). Must not be NULL.
 * @param output the output destination to write to.
 * @param flags the flags. See RNP_LOAD_SAVE_*.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_save_keys(rnp_ffi_t    ffi,
                                   const char * format,
                                   rnp_output_t output,
                                   uint32_t     flags);

RNP_API rnp_result_t rnp_get_public_key_count(rnp_ffi_t ffi, size_t *count);
RNP_API rnp_result_t rnp_get_secret_key_count(rnp_ffi_t ffi, size_t *count);

/** Search for the key
 *  Note: only valid userids are checked while searching by userid.
 *
 *  @param ffi
 *  @param identifier_type string with type of the identifier: userid, keyid, fingerprint, grip
 *  @param identifier for userid is the userid string, for other search types - hex string
 *         representation of the value
 *  @param key if key was found then the resulting key handle will be stored here, otherwise it
 *         will contain NULL value. You must free handle after use with rnp_key_handle_destroy.
 *  @return RNP_SUCCESS on success (including case where key is not found), or any other value
 * on error
 */
RNP_API rnp_result_t rnp_locate_key(rnp_ffi_t         ffi,
                                    const char *      identifier_type,
                                    const char *      identifier,
                                    rnp_key_handle_t *key);

RNP_API rnp_result_t rnp_key_handle_destroy(rnp_key_handle_t key);

/** generate a key or pair of keys using a JSON description
 *
 *  Notes:
 *  - When generating a subkey, the  pass provider may be required.
 *
 *  @param ffi
 *  @param json the json data that describes the key generation.
 *         Must not be NULL.
 *         JSON input must be an object containing one or two members:\n
 *
 *         JSON member | Description
 *         ------------|------------
 *         "primary"   | JSON object describing parameters of primary key generation.
 *         "sub"       | optional member, JSON object describing subkey generation parameters.
 *         Both "primary" and "sub" objects can contain the following members,
 *         if not specified otherwise:\n
 *         JSON member  | Description
 *         -------------|------------
 *         "type"       | string, key algorithm, see rnp_op_generate_create()
 *         "length"     | integer, key size in bits, see rnp_op_generate_set_bits()
 *         "curve"      | string, curve name, see rnp_op_generate_set_curve()
 *         "expiration" | integer, see rnp_op_generate_set_expiration()
 *         "usage"      | string or array of strings, see rnp_op_generate_add_usage()
 *         "hash"       | string, hash algorithm, see rnp_op_generate_set_hash()
 *         "userid"     | string, primary key only, user ID, see rnp_op_generate_set_userid()
 *         "preferences"| object, primary key only, user preferences, see description below
 *         "protection" | object, secret key protection settings, see description below
 *         The "preferences" member object can contain the following members:\n
 *         JSON member  | Description
 *         -------------|------------
 *         "hashes"     | array of strings, see rnp_op_generate_add_pref_hash()
 *         "ciphers"    | array of strings, see rnp_op_generate_add_pref_cipher()
 *         "compression"| array of strings, see rnp_op_generate_add_pref_compression()
 *         "key server" | string, see rnp_op_generate_set_pref_keyserver()
 *         The "protection" member object describes the secret key protection settings
 *         and it can contain the following members:\n
 *         JSON member | Description
 *         ------------|------------
 *         "cipher"    | string, protection cipher, see rnp_op_generate_set_protection_cipher()
 *         "hash"      | string, protection hash, see rnp_op_generate_set_protection_hash()
 *         "mode"      | string, protection mode, see rnp_op_generate_set_protection_mode()
 *         "iterations"| integer, see rnp_op_generate_set_protection_iterations()
 *         Example JSON input:\n
 *
 *             {
 *               "primary": {
 *                   "type": "ECDSA",
 *                   "curve": "NIST P-256",
 *                   "userid": "test0",
 *                   "usage": "sign",
 *                   "expiration": 0,
 *                   "hash": "SHA256",
 *                   "preferences" : {
 *                     "hashes": ["SHA512", "SHA256"],
 *                     "ciphers": ["AES256", "AES128"],
 *                     "compression": ["Zlib"],
 *                     "key server": "hkp://pgp.mit.edu"
 *                   },
 *                   "protection" : {
 *                       "cipher": "AES256",
 *                       "hash": "SHA256",
 *                       "mode":  "CBC",
 *                       "iterations": 65536
 *                   }
 *                },
 *                "sub": {
 *                   "type": "RSA",
 *                   "length": 1024
 *                }
 *             }
 *
 *  @param results pointer that will be set to the JSON results.
 *         Must not be NULL. The caller should free this with rnp_buffer_destroy.
 *         Serialized JSON output will contain a JSON object with a mandatory
 *         "primary" member object for the generated primary key and an optional "sub"
 *         member object if the subkey generation was requested. Both of them contain
 *         a single string member "grip" that holds
 *         hexadecimal key grip of a generated key.\n
 *         Example JSON output:\n
 *
 *             {
 *               "primary":{
 *                 "grip":"9F593A6333467A534BE8520CAE2600206BFE3681"
 *               },
 *               "sub":{
 *                 "grip":"ED822D77DDF199707B13D0E1BCA00868314FE47D"
 *               }
 *             }
 *
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_generate_key_json(rnp_ffi_t ffi, const char *json, char **results);

/* Key operations */

/** Shortcut function for rsa key-subkey pair generation. See rnp_generate_key_ex() for the
 *  detailed parameters description.
 */
RNP_API rnp_result_t rnp_generate_key_rsa(rnp_ffi_t         ffi,
                                          uint32_t          bits,
                                          uint32_t          subbits,
                                          const char *      userid,
                                          const char *      password,
                                          rnp_key_handle_t *key);

/** Shortcut function for DSA/ElGamal key-subkey pair generation. See rnp_generate_key_ex() for
 *  the detailed parameters description.
 */
RNP_API rnp_result_t rnp_generate_key_dsa_eg(rnp_ffi_t         ffi,
                                             uint32_t          bits,
                                             uint32_t          subbits,
                                             const char *      userid,
                                             const char *      password,
                                             rnp_key_handle_t *key);

/** Shortcut function for ECDSA/ECDH key-subkey pair generation. See rnp_generate_key_ex() for
 *  the detailed parameters description.
 */
RNP_API rnp_result_t rnp_generate_key_ec(rnp_ffi_t         ffi,
                                         const char *      curve,
                                         const char *      userid,
                                         const char *      password,
                                         rnp_key_handle_t *key);

/** Shortcut function for EdDSA/x25519 key-subkey pair generation. See rnp_generate_key_ex()
 *  for the detailed parameters description.
 */
RNP_API rnp_result_t rnp_generate_key_25519(rnp_ffi_t         ffi,
                                            const char *      userid,
                                            const char *      password,
                                            rnp_key_handle_t *key);

/** Shortcut function for SM2/SM2 key-subkey pair generation. See rnp_generate_key_ex() for
 *  for the detailed parameters description.
 */
RNP_API rnp_result_t rnp_generate_key_sm2(rnp_ffi_t         ffi,
                                          const char *      userid,
                                          const char *      password,
                                          rnp_key_handle_t *key);

/**
 * @brief Shortcut for quick key generation. It is used in other shortcut functions for
 *        key generation (rnp_generate_key_*).
 *
 * @param ffi
 * @param key_alg string with primary key algorithm. Cannot be NULL.
 * @param sub_alg string with subkey algorithm. If NULL then subkey will not be generated.
 * @param key_bits size of key in bits. If zero then default value will be used.
 *             Must be zero for EC-based primary key algorithm (use curve instead).
 * @param sub_bits size of subkey in bits. If zero then default value will be used.
 *              Must be zero for EC-based subkey algorithm (use scurve instead).
 * @param key_curve Curve name. Must be non-NULL only with EC-based primary key algorithm,
 *              otherwise error will be returned.
 * @param sub_curve Subkey curve name. Must be non-NULL only with EC-based subkey algorithm,
 *               otherwise error will be returned.
 * @param userid String with userid. Cannot be NULL.
 * @param password String with password which would be used to protect the key and subkey.
 *                 If NULL then key will be stored in cleartext (unencrypted).
 * @param key if non-NULL, then handle of the primary key will be stored here on success.
 *            Caller must destroy it with rnp_key_handle_destroy() call.
 * @return RNP_SUCCESS or error code instead.
 */
RNP_API rnp_result_t rnp_generate_key_ex(rnp_ffi_t         ffi,
                                         const char *      key_alg,
                                         const char *      sub_alg,
                                         uint32_t          key_bits,
                                         uint32_t          sub_bits,
                                         const char *      key_curve,
                                         const char *      sub_curve,
                                         const char *      userid,
                                         const char *      password,
                                         rnp_key_handle_t *key);

/** Create key generation context for the primary key.
 *  To generate a subkey use function rnp_op_generate_subkey_create() instead.
 *  Note: pass provider is required if generated key needs protection.
 *
 * @param op pointer to opaque key generation context.
 * @param ffi
 * @param alg key algorithm as string. Must be able to sign. Currently the following algorithms
 *            are supported (case-insensitive) : 'rsa', 'dsa', 'ecdsa', 'eddsa', 'sm2'.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_create(rnp_op_generate_t *op,
                                            rnp_ffi_t          ffi,
                                            const char *       alg);

/** Create key generation context for the subkey.
 *  Note: you need to have primary key before calling this function. It can be loaded from
 * keyring or generated via the function rnp_op_generate_create(). Also pass provider is needed
 * if primary key is encrypted (protected and locked).
 *
 * @param op pointer to opaque key generation context.
 * @param ffi
 * @param primary primary key handle, must have secret part.
 * @param alg key algorithm as string. Currently the following algorithms are supported
 *            (case-insensitive) : 'rsa', 'dsa', 'elgamal', 'ecdsa', 'eddsa', 'ecdh', 'sm2'.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_subkey_create(rnp_op_generate_t *op,
                                                   rnp_ffi_t          ffi,
                                                   rnp_key_handle_t   primary,
                                                   const char *       alg);

/** Set bits of the generated key or subkey.
 *  Note: this is applicable only to rsa, dsa and el-gamal keys.
 *
 * @param op pointer to opaque key generation context.
 * @param bits number of bits
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_bits(rnp_op_generate_t op, uint32_t bits);

/** Set hash algorithm used in self signature or subkey binding signature.
 *
 * @param op pointer to opaque key generation context.
 * @param hash string with hash algorithm name. Following hash algorithms are supported:
 *             "MD5", "SHA1", "RIPEMD160", "SHA256", "SHA384", "SHA512", "SHA224", "SM3"
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_hash(rnp_op_generate_t op, const char *hash);

/** Set size of q parameter for DSA key.
 *  Note: appropriate default value will be set, depending on key bits. However you may
 *        override it if needed.
 * @param op pointer to opaque key generation context.
 * @param qbits number of bits
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_dsa_qbits(rnp_op_generate_t op, uint32_t qbits);

/** Set the curve used for ECC key
 *  Note: this is only applicable for ECDSA, ECDH and SM2 keys.
 * @param op pointer to opaque key generation context.
 * @param curve string with curve name. Following curve names may be used:
 *              "NIST P-256", "NIST P-384", "NIST P-521", "Curve25519" (ECDH only),
 *              "brainpoolP256r1", "brainpoolP384r1", "brainpoolP512r1", "secp256k1",
 *              "SM2 P-256" (SM2 only)
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_curve(rnp_op_generate_t op, const char *curve);

/** Set password, used to encrypt secret key data. If this method is not called then
 *  key will be generated without protection (unencrypted).
 *
 * @param op pointer to opaque key generation context.
 * @param password string with password, could not be NULL. Will be copied internally so may
 *                 be safely freed after the call.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_protection_password(rnp_op_generate_t op,
                                                             const char *      password);

/**
 * @brief Enable or disable password requesting via ffi's password provider. This password
 *        then will be used for key encryption.
 *        Note: this will be ignored if password was set via
 *        rnp_op_generate_set_protection_password().
 *
 * @param op pointer to opaque key generation context.
 * @param request true to enable password requesting or false otherwise. Default value is false
 *                (i.e. key will be generated unencrypted).
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_request_password(rnp_op_generate_t op, bool request);

/** Set cipher used to encrypt secret key data. If not called then default one will be used.
 *
 * @param op pointer to opaque key generation context.
 * @param cipher string with cipher name. Following ciphers are supported:
 *               "Idea", "Tripledes", "Cast5", "Blowfish", "AES128", "AES192", "AES256",
 *               "Twofish", "Camellia128", "Camellia192", "Camellia256", "SM4".
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_protection_cipher(rnp_op_generate_t op,
                                                           const char *      cipher);

/** Set hash algorithm, used to derive key from password for secret key data encryption.
 *  If not called then default one will be used.
 *
 * @param op pointer to opaque key generation context.
 * @param hash string with hash algorithm, see rnp_op_generate_set_hash() for the whole list.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_protection_hash(rnp_op_generate_t op,
                                                         const char *      hash);

/** Set encryption mode, used for secret key data encryption.
 *  Note: currently this makes sense only for G10 key format
 *
 * @param op pointer to opaque key generation context.
 * @param mode string with mode name: "CFB", "CBC", "OCB"
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_protection_mode(rnp_op_generate_t op,
                                                         const char *      mode);

/** Set number of iterations used to derive key from password for secret key encryption.
 *  If not called then default one will be used.
 *
 * @param op pointer to opaque key generation context.
 * @param iterations number of iterations
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_protection_iterations(rnp_op_generate_t op,
                                                               uint32_t          iterations);

/** Add key usage flag to the key or subkey.
 *  Note: use it only if you need to override defaults, which depend on primary key or subkey,
 *        and public key algorithm.
 *
 * @param op pointer to opaque key generation context.
 * @param usage string, representing key usage. Following values are supported: "sign",
 *              "certify", "encrypt", "authenticate".
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_add_usage(rnp_op_generate_t op, const char *usage);

/** Reset key usage flags, so default ones will be used during key/subkey generation
 *
 * @param op pointer to opaque key generation context.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_clear_usage(rnp_op_generate_t op);

/** Set the userid which will represent the generate key.
 *  Note: Makes sense only for primary key generation.
 *
 * @param op pointer to opaque key generation context.
 * @param userid NULL-terminated string with userid.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_userid(rnp_op_generate_t op, const char *userid);

/** Set the key or subkey expiration time.
 *
 * @param op pointer to opaque key generation context.
 * @param expiration expiration time in seconds. 0 value means that key doesn't expire.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_expiration(rnp_op_generate_t op, uint32_t expiration);

/** Add preferred hash to user preferences.
 *  Note: the first added hash algorithm has the highest priority, then the second and so on.
 *        Applicable only for the primary key generation.
 *
 * @param op pointer to opaque key generation context.
 * @param hash string, representing the hash algorithm. See the rnp_op_generate_set_hash()
 *             function description for the list of possible values.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_add_pref_hash(rnp_op_generate_t op, const char *hash);

/** Clear the preferred hash algorithms list, so default ones will be used.
 *
 * @param op pointer to opaque key generation context.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_clear_pref_hashes(rnp_op_generate_t op);

/** Add preferred compression algorithm to user preferences.
 *  Note: the first added algorithm has the highest priority, then the second and so on.
 *        Applicable only for the primary key generation.
 *
 * @param op pointer to opaque key generation context.
 * @param compression string, representing the compression algorithm. Possible values are:
 *                    "zip", "zlib", "bzip2"
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_add_pref_compression(rnp_op_generate_t op,
                                                          const char *      compression);

/** Clear the preferred compression algorithms list, so default ones will be used.
 *
 * @param op pointer to opaque key generation context.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_clear_pref_compression(rnp_op_generate_t op);

/** Add preferred encryption algorithm to user preferences.
 *  Note: the first added algorithm has the highest priority, then the second and so on.
 *        Applicable only for the primary key generation.
 *
 * @param op pointer to opaque key generation context.
 * @param cipher string, representing the encryption algorithm.
 *               See the rnp_op_generate_set_protection_cipher() function description for
 *               the list of possible values.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_add_pref_cipher(rnp_op_generate_t op, const char *cipher);

/** Clear the preferred encryption algorithms list, so default ones will be used.
 *
 * @param op pointer to opaque key generation context.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_clear_pref_ciphers(rnp_op_generate_t op);

/** Set the preferred key server. Applicable only for the primary key.
 *
 * @param op pointer to opaque key generation context.
 * @param keyserver NULL-terminated string with key server's URL, or NULL to delete it from
 *                  user preferences.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_pref_keyserver(rnp_op_generate_t op,
                                                        const char *      keyserver);

#if defined(RNP_EXPERIMENTAL_CRYPTO_REFRESH)
/** Set the generated key version to v6.
 *  NOTE: This is an experimental feature and this function can be replaced (or removed) at any
 *        time.
 *
 * @param op pointer to opaque key generation context.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_v6_key(rnp_op_generate_t op);
#endif

#if defined(RNP_EXPERIMENTAL_PQC)
/** Set the SPHINCS+ parameter set
 *  NOTE: This is an experimental feature and this function can be replaced (or removed) at any
 *        time.
 *
 * @param op pointer to opaque key generation context.
 * @param param string, representing the SHPINCS+ parameter set.
 *               Possible Values:
 *                  128s, 128f, 192s, 192f, 256s, 256f
 *               All parameter sets refer to the simple variant and the hash function is given
 * by the algorithm id.
 *
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_set_sphincsplus_param(rnp_op_generate_t op,
                                                           const char *      param);
#endif

/** Execute the prepared key or subkey generation operation.
 *  Note: if you set protection algorithm, then you need to specify ffi password provider to
 *        be able to request password for key encryption.
 *
 * @param op pointer to opaque key generation context.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_execute(rnp_op_generate_t op);

/** Get the generated key's handle. Should be called only after successful execution of
 *  rnp_op_generate_execute().
 *
 * @param op pointer to opaque key generation context.
 * @param handle pointer to key handle will be stored here.
 *            You must free handle after use with rnp_key_handle_destroy.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_get_key(rnp_op_generate_t op, rnp_key_handle_t *handle);

/** Free resources associated with key generation operation.
 *
 *  @param op opaque key generation context. Must be successfully initialized with one of the
 *         rnp_op_generate_*_create functions.
 *  @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_generate_destroy(rnp_op_generate_t op);

/** export a key
 *
 *  @param key the key to export
 *  @param output the stream to write to
 *  @param flags see RNP_KEY_EXPORT_*.
 *  @return RNP_SUCCESS on success, or any other value on error
 **/
RNP_API rnp_result_t rnp_key_export(rnp_key_handle_t key, rnp_output_t output, uint32_t flags);

/**
 * @brief Export minimal key for autocrypt feature (just 5 packets: key, uid, signature,
 *        encryption subkey, signature)
 *
 * @param key primary key handle, cannot be NULL.
 * @param subkey subkey to export. May be NULL to pick the first suitable.
 * @param uid userid to export. May be NULL if key has only one uid.
 * @param output the stream to write to
 * @param flags additional flags. Currently only RNP_KEY_EXPORT_BASE64 is supported. Enabling
 *              it would export key base64-encoded instead of binary.
 * @return RNP_SUCCESS on success, or any other value if failed.
 */
RNP_API rnp_result_t rnp_key_export_autocrypt(rnp_key_handle_t key,
                                              rnp_key_handle_t subkey,
                                              const char *     uid,
                                              rnp_output_t     output,
                                              uint32_t         flags);

/**
 * @brief Generate and export primary key revocation signature.
 *        Note: to revoke a key you'll need to import this signature into the keystore or use
 *        rnp_key_revoke() function.
 * @param key primary key to be revoked. Must have secret key, otherwise keyrings will be
 *            searched for the authorized to issue revocation signature secret key. If secret
 *            key is locked then password will be asked via password provider.
 * @param output signature contents will be saved here.
 * @param flags must be RNP_KEY_EXPORT_ARMORED or 0.
 * @param hash hash algorithm used to calculate signature. Pass NULL for default algorithm
 *             selection.
 * @param code reason for revocation code. Possible values: 'no', 'superseded', 'compromised',
 *             'retired'. May be NULL - then 'no' value will be used.
 * @param reason textual representation of the reason for revocation. May be NULL or empty
 *               string.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_key_export_revocation(rnp_key_handle_t key,
                                               rnp_output_t     output,
                                               uint32_t         flags,
                                               const char *     hash,
                                               const char *     code,
                                               const char *     reason);

/**
 * @brief revoke a key or subkey by generating and adding revocation signature.
 * @param key key or subkey to be revoked. For primary key must have secret key, otherwise
 *            keyrings will be searched for the authorized to issue revocation signatures
 *            secret key. For subkey keyrings must have primary secret key.
 *            If secret key is locked then password will be asked via password provider.
 * @param flags currently must be 0.
 * @param hash hash algorithm used to calculate signature. Pass NULL for default algorithm
 *             selection.
 * @param code reason for revocation code. Possible values: 'no', 'superseded', 'compromised',
 *             'retired'. May be NULL - then 'no' value will be used.
 * @param reason textual representation of the reason for revocation. May be NULL or empty
 *               string.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_key_revoke(rnp_key_handle_t key,
                                    uint32_t         flags,
                                    const char *     hash,
                                    const char *     code,
                                    const char *     reason);

/**
 * @brief Check whether Curve25519 secret key's bits are correctly set, i.e. 3 least
 *        significant bits are zero and key is exactly 255 bits in size. See RFC 7748, section
 *        5 for the details. RNP interpreted RFC requirements in the way that Curve25519 secret
 *        key is random 32-byte string, which bits are correctly tweaked afterwards within
 *        secret key operation. However, for compatibility reasons, it would be more correct to
 *        store/transfer secret key with bits already tweaked.
 *
 *        Note: this operation requires unlocked secret key, so make sure to call
 *        rnp_key_lock() afterwards.
 *
 * @param key key handle, cannot be NULL. Must be ECDH Curve25519 unlocked secret key.
 * @param result true will be stored here if secret key's low/high bits are not correctly set.
 *               In this case you may need to call `rnp_key_25519_bits_tweak()` on it to set
 *               bits to correct values so exported secret key will be compatible with
 *               implementations which do not tweak these bits automatically.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_25519_bits_tweaked(rnp_key_handle_t key, bool *result);

/**
 * @brief Make sure Curve25519 secret key's least significant and most significant bits are
 *        correctly set, see rnp_key_25519_bits_tweaked() documentation for the details.
 *        Note: this operation requires unprotected secret key since it would modify secret
 *        key's data, so make sure to call rnp_key_protect() afterwards.
 *
 * @param key key handle, cannot be NULL. Must be ECDH Curve25519 unprotected secret key.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_25519_bits_tweak(rnp_key_handle_t key);

/** remove a key from keyring(s)
 *  Note: you need to call rnp_save_keys() to write updated keyring(s) out.
 *        Other handles of the same key should not be used after this call.
 * @param key pointer to the key handle.
 * @param flags see RNP_KEY_REMOVE_* constants. Flag RNP_KEY_REMOVE_SUBKEYS will work only for
 *              primary key, and remove all of its subkeys as well.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_remove(rnp_key_handle_t key, uint32_t flags);

/**
 * @brief Remove unneeded signatures from the key, it's userids and subkeys if any.
 *        May be called on subkey handle as well.
 *        Note: you'll need to call rnp_save_keys() to write updated keyring(s) out.
 *        Any signature handles related to this key, it's uids or subkeys should not be used
 *        after this call.
 *
 * @param key key handle, cannot be NULL.
 * @param flags flags, controlling which signatures to remove. Signature will be removed if it
 *              matches at least one of these flags.
 *              Currently following signature matching flags are defined:
 *              - RNP_KEY_SIGNATURE_INVALID : signature is invalid and was never valid. Note,
 *                  that this will not remove invalid signature if there is no signer's public
 *                  key in the keyring.
 *              - RNP_KEY_SIGNATURE_UNKNOWN_KEY : signature is made by the key which is not
 *                  known/available.
 *              - RNP_KEY_SIGNATURE_NON_SELF_SIG : signature is not a self-signature (i.e. made
 *                  by the key itself or corresponding primary key).
 *
 *             Note: if RNP_KEY_SIGNATURE_NON_SELF_SIG is not specified then function will
 *             attempt to validate all the signatures, and look up for the signer's public key
 *             via keyring/key provider.
 *
 * @param sigcb callback, used to record information about the removed signatures, or further
 *              filter out the signatures. May be NULL.
 * @param app_ctx context information, passed to sigcb. May be NULL.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_remove_signatures(rnp_key_handle_t      key,
                                               uint32_t              flags,
                                               rnp_key_signatures_cb sigcb,
                                               void *                app_ctx);

/**
 * @brief Guess contents of the OpenPGP data stream.
 *        Note: This call just peeks data from the stream, so stream is still usable for
 *              the further processing.
 * @param input stream with data. Must be opened and cannot be NULL.
 * @param contents string with guessed data format will be stored here.
 *                 Possible values: 'message', 'public key', 'secret key', 'signature',
 *                 'unknown'. May be used as type in rnp_enarmor() function. Must be
 *                 deallocated with rnp_buffer_destroy() call.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_guess_contents(rnp_input_t input, char **contents);

/** Add ASCII Armor
 *
 *  @param input stream to read data from
 *  @param output stream to write armored data to
 *  @param type the type of armor to add ("message", "public key",
 *         "secret key", "signature", "cleartext"). Use NULL to try
 *         to guess the type.
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_enarmor(rnp_input_t input, rnp_output_t output, const char *type);

/** Remove ASCII Armor
 *
 *  @param input stream to read armored data from
 *  @param output stream to write dearmored data to
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_dearmor(rnp_input_t input, rnp_output_t output);

/** Get key's primary user id.
 *  Note: userid considered as primary if it has marked as primary in self-certification, and
 *        is valid (i.e. both certification and key are valid, not expired and not revoked). If
 *        there is no userid marked as primary then the first valid userid handle will be
 *        returned.
 * @param key key handle.
 * @param uid pointer to the string with primary user id will be stored here.
 *            You must free it using the rnp_buffer_destroy().
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_primary_uid(rnp_key_handle_t key, char **uid);

/** Get number of the key's user ids.
 *
 * @param key key handle.
 * @param count number of user ids will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_uid_count(rnp_key_handle_t key, size_t *count);

/** Get key's user id by its index.
 *
 * @param key key handle.
 * @param idx zero-based index of the userid.
 * @param uid pointer to the string with user id will be stored here.
 *            You must free it using the rnp_buffer_destroy().
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_uid_at(rnp_key_handle_t key, size_t idx, char **uid);

/** Get key's user id handle by its index.
 *  Note: user id handle may become invalid once corresponding user id or key is removed.
 *
 * @param key key handle
 * @param idx zero-based index of the userid.
 * @param uid user id handle will be stored here on success. You must destroy it
 *            using the rnp_uid_handle_destroy().
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_uid_handle_at(rnp_key_handle_t  key,
                                               size_t            idx,
                                               rnp_uid_handle_t *uid);

/** Get userid's type. Currently two possible values are defined:
 *  - RNP_USER_ID - string representation of user's name and email.
 *  - RNP_USER_ATTR - binary photo of the user

 * @param uid uid handle, cannot be NULL.
 * @param type on success userid type will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_get_type(rnp_uid_handle_t uid, uint32_t *type);

/** Get userid's data. Representation of data depends on userid type (see rnp_uid_get_type()
 * function)
 *
 * @param uid uid handle, cannot be NULL.
 * @param data cannot be NULL. On success pointer to the allocated buffer with data will be
 * stored here. Must be deallocated by caller via rnp_buffer_destroy().
 * @param size cannot be NULL. On success size of the data will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_get_data(rnp_uid_handle_t uid, void **data, size_t *size);

/** Check whether uid is marked as primary.
 *
 * @param uid uid handle, cannot be NULL
 * @param primary cannot be NULL. On success true or false will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_is_primary(rnp_uid_handle_t uid, bool *primary);

/** Get userid validity status. Userid is considered as valid if key itself is valid, and
 *  userid has at least one valid, non-expired self-certification.
 *  Note: - userid still may be valid even if a primary key is invalid - expired, revoked, etc.
 *        - up to the RNP version 0.15.1 uid was not considered as valid if it's latest
 *          self-signature has key expiration in the past.
 *
 * @param uid user id handle.
 * @param valid validity status will be stored here on success.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_is_valid(rnp_uid_handle_t uid, bool *valid);

/** Get number of key's signatures.
 *  Note: this will not count user id certifications and subkey(s) signatures if any.
 *        I.e. it will return only number of direct-key and key revocation signatures for the
 *        primary key, and number of subkey bindings/revocation signatures for the subkey.
 *        Use rnp_uid_get_signature_count() or call this function on subkey's handle.
 *
 * @param key key handle
 * @param count number of key's signatures will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_signature_count(rnp_key_handle_t key, size_t *count);

/** Get key's signature, based on its index.
 *  Note: see the rnp_key_get_signature_count() description for the details.
 *
 * @param key key handle
 * @param idx zero-based signature index.
 * @param sig signature handle will be stored here on success. You must free it after use with
 *            the rnp_signature_handle_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_signature_at(rnp_key_handle_t        key,
                                              size_t                  idx,
                                              rnp_signature_handle_t *sig);

/**
 * @brief Create new direct-key signature over the target, issued by signer. It may be
 *        customized via the rnp_signature_set_* calls, and finalized via the
 *        rnp_key_signature_sign() call.
 *
 * @param signer signing key, must be secret, and must exist in the keyring up to the
 *               rnp_key_signature_sign() call. Cannot be NULL.
 * @param target target key for which signature should be made. May be NULL, then signature
 *               over the signer (self-signature) will be made.
 *
 * @param sig on success signature handle will be stored here. It is initialized with current
 *            creation time, default hash algorithm and version. Cannot be NULL.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_direct_signature_create(rnp_key_handle_t        signer,
                                                     rnp_key_handle_t        target,
                                                     rnp_signature_handle_t *sig);

/**
 * @brief Create new certification signature, issued by the signer. This could be
 *        self-certification (if uid belongs to the signer key) or certification of the other
 *        key. This signature could be customized by rnp_signature_set_* calls and finalized
 *        via the rnp_key_signature_sign() call.
 *
 * @param signer signing key, must be secret, and must exist in the keyring up to the
 *               rnp_key_signature_sign() call. Cannot be NULL.
 * @param uid user id which should be certified, i.e. bound to the key with signature.
 *            Cannot be NULL.
 * @param type certification type. May be one of the RNP_CERTIFICATION_* values, or NULL
 *             for the default one. Default would be POSITIVE for self-certification or GENERIC
 *             for the certification of another key.
 *             Note: it is common to use RNP_CERTIFICATION_POSITIVE for self-certifications,
 *             and RNP_CERTIFICATION_GENERIC while certifying other keys. However it's up to
 *             the caller to pick the type according to OpenPGP specification.
 * @param sig on success signature handle will be stored here. It is initialized with current
 *            creation time, default hash algorithm and version. Cannot be NULL.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_certification_create(rnp_key_handle_t        signer,
                                                  rnp_uid_handle_t        uid,
                                                  const char *            type,
                                                  rnp_signature_handle_t *sig);

/**
 * @brief Create new key or subkey revocation signature. It may be
 *        customized via the rnp_signature_set_* calls, and finalized via the
 *        rnp_key_signature_sign() call.
 *
 * @param signer revoker's key, must be secret, and must exist in the keyring up to the
 *               rnp_key_signature_sign() call. Cannot be NULL.
 * @param target target key for which signature should be made. May be NULL, then signer will
 *               revoke itself.
 *
 * @param sig on success signature handle will be stored here. It is initialized with current
 *            creation time, default hash algorithm and version. Cannot be NULL.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_revocation_signature_create(rnp_key_handle_t        signer,
                                                         rnp_key_handle_t        target,
                                                         rnp_signature_handle_t *sig);

/**
 * @brief Set hash algorithm, used during signing.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param hash hash algorithm name, i.e. "SHA256" or others.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_hash(rnp_signature_handle_t sig, const char *hash);

/**
 * @brief Set the signature creation time. While it is set by default to the current time,
 *        caller may override it in case of need.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param creation timestamp with the creation time.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_creation(rnp_signature_handle_t sig,
                                                    uint32_t               ctime);

/**
 * @brief Set the key usage flags, i.e. whether it is usable for signing, encryption, whatever
 *        else.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param flags key flags, which directly maps to the ones described in the OpenPGP
 *              specification. See the RNP_KEY_USAGE_* constants.
 *              Note: RNP will not check whether flags are applicable to the key itself (i.e.
 *              signing flag for encryption-only key), so it's up to the caller to check this.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_key_flags(rnp_signature_handle_t sig,
                                                     uint32_t               flags);

/**
 * @brief Set the key expiration time. Makes sense only for self-certification or direct-key
 *        signatures.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param expiry number of seconds since key creation when it is considered as valid. Zero
 *               value means that key never expires.
 *               I.e. if you want key to last for 1 year from now (given that signature
 *               creation time is set to now), you should calculate the following:
 *               expiry = now() - rnp_key_get_creation() + 365*24*60*60
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_key_expiration(rnp_signature_handle_t sig,
                                                          uint32_t               expiry);

/**
 * @brief Set the key features. Makes sense only for self-signature.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param features or'ed together feature flags (RNP_FEATURE_*). For the list of currently
 *                 supported flags please see the description of rnp_signature_get_features().
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_features(rnp_signature_handle_t sig,
                                                    uint32_t               features);

/**
 * @brief Add preferred symmetric algorithm to the signature. Should be subsequently called for
 *        each algorithm, making first ones of higher priority.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param alg symmetric algorithm name, cannot be NULL. See
 *            rnp_op_generate_set_protection_cipher() for the list of possible values.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_add_preferred_alg(rnp_signature_handle_t sig,
                                                         const char *           alg);

/**
 * @brief Add preferred hash algorithm to the signature. Should be subsequently called for each
 *        algorithm, making first ones of higher priority.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param alg hash algorithm name, cannot be NULL. See rnp_op_generate_set_hash() for the list
 *            of possible values.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_add_preferred_hash(rnp_signature_handle_t sig,
                                                          const char *           hash);

/**
 * @brief Add preferred compression algorithm to the signature. Should be subsequently called
 *        for each algorithm, making first ones of higher priority.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param alg compression algorithm name, cannot be NULL. See
 *            rnp_op_generate_add_pref_compression() for the list of possible values.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_add_preferred_zalg(rnp_signature_handle_t sig,
                                                          const char *           zalg);

/**
 * @brief Set whether corresponding user id should be considered as primary. Makes sense only
 *        for self-certification.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param primary true for primary or false for not.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_primary_uid(rnp_signature_handle_t sig,
                                                       bool                   primary);

/**
 * @brief Set the key server url which is applicable for this key.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param keyserver key server url. If NULL or empty string then key server field in the
 * signature will be removed.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_key_server(rnp_signature_handle_t sig,
                                                      const char *           keyserver);

/**
 * @brief Set the key server preferences flags.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param flags or'ed together preferences flags. Currently only single flag is supported -
 *              RNP_KEY_SERVER_NO_MODIFY.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_key_server_prefs(rnp_signature_handle_t sig,
                                                            uint32_t               flags);

/**
 * @brief Set revocation reason and code for the revocation signature.
 *        See `rnp_key_revoke()` for the details.
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param code revocation reason code. Could be NULL, then default one will be set.
 * @param reason human-readable reason for revocation. Could be NULL or empty string.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_revocation_reason(rnp_signature_handle_t sig,
                                                             const char *           code,
                                                             const char *           reason);

/**
 * @brief Add designated revoker subpacket to the signature. See RFC 4880, section 5.2.3.15.
 *        Only single revoker could be set - subsequent calls would overwrite the previous one.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param revoker revoker's key.
 * @param flags additional flags. The following flag is currently supported:
 *              RNP_REVOKER_SENSITIVE: information about the revocation key should be
 *                considered as sensitive. See RFC for the details.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_revoker(rnp_signature_handle_t sig,
                                                   rnp_key_handle_t       revoker,
                                                   uint32_t               flags);

/**
 * @brief Set the signature trust level and amount. See OpenPGP specification for the details
 *        on their interpretation ('Trust Signature' signature subpacket). Makes sense only for
 *        other key's certification.
 *
 * @param sig editable key signature handle, i.e. created with rnp_key_*_signature_create().
 * @param level trust level
 * @param amount trust amount
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_set_trust_level(rnp_signature_handle_t sig,
                                                       uint8_t                level,
                                                       uint8_t                amount);

/**
 * @brief Finalize populating and sign signature, created with one of the
 *        rnp_key_*_signature_create functions, and add it to the corresponding key.
 *
 * @param sig signature handle.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_signature_sign(rnp_signature_handle_t sig);

/**
 * @brief Get number of the designated revokers for the key. Designated revoker is a key, which
 * is allowed to revoke this key.
 *
 * @param key key handle, cannot be NULL.
 * @param count number of designated revokers will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_revoker_count(rnp_key_handle_t key, size_t *count);

/**
 * @brief Get the fingerprint of designated revoker's key, based on it's index.
 *
 * @param key key handle, cannot be NULL.
 * @param idx zero-based index.
 * @param revoker on success hex-encoded revoker's key fingerprint will be stored here. Must be
 * later freed via rnp_buffer_destroy().
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_revoker_at(rnp_key_handle_t key, size_t idx, char **revoker);

/**
 * @brief Get key's revocation signature handle, if any.
 *
 * @param key key handle
 * @param sig signature handle or NULL will be stored here on success. NULL will be stored in
 *            case when there is no valid revocation signature.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_revocation_signature(rnp_key_handle_t        key,
                                                      rnp_signature_handle_t *sig);

/** Get the number of user id's signatures.
 *
 * @param uid user id handle.
 * @param count number of uid's signatures will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_get_signature_count(rnp_uid_handle_t uid, size_t *count);

/** Get user id's signature, based on its index.
 *
 * @param uid uid handle.
 * @param idx zero-based signature index.
 * @param sig signature handle will be stored here on success. You must free it after use with
 *            the rnp_signature_handle_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_get_signature_at(rnp_uid_handle_t        uid,
                                              size_t                  idx,
                                              rnp_signature_handle_t *sig);

/**
 * @brief Get signature's type.
 *
 * @param sig signature handle.
 * @param type on success string with signature type will be saved here. Cannot be NULL.
 *             You must free it using the rnp_buffer_destroy().
 *             Currently defined values are:
 *             - 'binary' : signature of a binary document
 *             - 'text' : signature of a canonical text document
 *             - 'standalone' : standalone signature
 *             - 'certification (generic)` : generic certification of a user id
 *             - 'certification (persona)' : persona certification of a user id
 *             - 'certification (casual)' : casual certification of a user id
 *             - 'certification (positive)' : positive certification of a user id
 *             - 'subkey binding' : subkey binding signature
 *             - 'primary key binding' : primary key binding signature
 *             - 'direct' : direct-key signature
 *             - 'key revocation' : primary key revocation signature
 *             - 'subkey revocation' : subkey revocation signature
 *             - 'certification revocation' : certification revocation signature
 *             - 'timestamp' : timestamp signature
 *             - 'third-party' : third party confirmation signature
 *             - 'unknown: 0..255' : unknown signature with its type specified as number
 *
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_type(rnp_signature_handle_t sig, char **type);

/** Get signature's algorithm.
 *
 * @param sig signature handle.
 * @param alg on success string with algorithm name will be saved here. Cannot be NULL.
*            You must free it using the rnp_buffer_destroy().

 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_alg(rnp_signature_handle_t sig, char **alg);

/** Get signature's hash algorithm.
 *
 * @param sig signature handle.
 * @param alg on success string with algorithm name will be saved here. Cannot be NULL.
 *            You must free it using the rnp_buffer_destroy().
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_hash_alg(rnp_signature_handle_t sig, char **alg);

/** Get the signature creation time as number of seconds since Jan, 1 1970 UTC
 *
 * @param sig signature handle.
 * @param create on success result will be stored here. Cannot be NULL.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_creation(rnp_signature_handle_t sig, uint32_t *create);

/**
 * @brief Get number of the signature subpackets.
 *
 * @param sig signature handle, cannot be NULL.
 * @param count on success number of the subpackets will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_subpacket_count(rnp_signature_handle_t sig, size_t *count);

/**
 * @brief Get signature subpacket at the specified position.
 *
 * @param sig signature handle, cannot be NULL.
 * @param idx index of the subpacket (see rnp_signature_subpacket_count for the total amount)
 * @param subpkt on success handle to the subpacket object will be stored here. Must be later
 *               destroyed via the rnp_signature_subpacket_destroy() call.
 * @return RNP_SUCCESS on success, RNP_ERROR_NOT_FOUND if index is out of bounds, or any other
 *         error code if failed.
 */
RNP_API rnp_result_t rnp_signature_subpacket_at(rnp_signature_handle_t sig,
                                                size_t                 idx,
                                                rnp_sig_subpacket_t *  subpkt);

/**
 * @brief Find the signature subpacket matching criteria.
 *
 * @param sig signature handle, cannot be NULL.
 * @param type type of the subpacket as per OpenPGP specification.
 * @param hashed if true, then subpacket will be looked only in hashed area. If false - then in
 *               both, hashed and unhashed areas.
 * @param skip number of matching subpackets to skip, allowing to iterate over the subpackets
 *             of the same type.
 * @param subpkt on success handle to the subpacket will be stored here. Must be destroyed via
 *               the rnp_signature_subpacket_destroy() call.
 * @return RNP_SUCCESS if subpacket found, or RNP_ERROR_NOT_FOUND otherwise. Any other value
 *         would mean that search failed.
 */
RNP_API rnp_result_t rnp_signature_subpacket_find(rnp_signature_handle_t sig,
                                                  uint8_t                type,
                                                  bool                   hashed,
                                                  size_t                 skip,
                                                  rnp_sig_subpacket_t *  subpkt);

/**
 * @brief Get the subpacket info.
 *
 * @param subpkt signature subpacket handle, cannot be NULL.
 * @param type type of the subpacket as per OpenPGP specification will be stored here.
 * @param hashed whether subpackets is stored in hased or unhashed area.
 * @param critical whether subpacket has critical bit set.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_subpacket_info(rnp_sig_subpacket_t subpkt,
                                                  uint8_t *           type,
                                                  bool *              hashed,
                                                  bool *              critical);

/**
 * @brief Get signature subpacket raw data.
 *
 * @param subpkt signature subpacket handle, cannot be NULL.
 * @param data pointer to raw data will be stored here. Must be deallocated via the
 * rnp_buffer_destroy() call.
 * @param size size of the data will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_subpacket_data(rnp_sig_subpacket_t subpkt,
                                                  uint8_t **          data,
                                                  size_t *            size);

/**
 * @brief Destroy the subpacket object.
 *
 * @param subpkt initialized signature subpacket handle, cannot be NULL.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_subpacket_destroy(rnp_sig_subpacket_t subpkt);

/** Get the signature expiration time as number of seconds after creation time
 *
 * @param sig signature handle.
 * @param expires on success result will be stored here. Cannot be NULL.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_expiration(rnp_signature_handle_t sig,
                                                  uint32_t *             expires);

/**
 * @brief Get the key features if any as per RFC 4880 and later. Do not confuse with key flags.
 *
 * @param sig signature handle, cannot be NULL.
 * @param features on success result will be stored here as or'ed together flag bits.
 *                 If corresponding value is not available then 0 will be stored.
 *                 Currently known feature bit flags are (consult RFC for more details):
 *                 RNP_KEY_FEATURE_MDC  - support for MDC packets (see RFC 4880)
 *                 RNP_KEY_FEATURE_AEAD - support for OCB encrypted packet and v5 SKESK (please
 *                   see LibrePGP standard)
 *                 RNP_KEY_FEATURE_V5   - version 5 public-key format and corresponding
 *                   fingerprint
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_features(rnp_signature_handle_t sig,
                                                uint32_t *             features);

/**
 * @brief Get number of the preferred symmetric algorithms, listed in the signature. Applies to
 *        the self-signature (self-certification or direct-key signature).
 *
 * @param sig signature handle, cannot be NULL.
 * @param count on success nunmber of available algorithms will be stored here. It may be 0 if
 *              no such information is available within the signature.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_preferred_alg_count(rnp_signature_handle_t sig,
                                                           size_t *               count);

/**
 * @brief Get preferred symmetric algorithm from the preferences, specified in the signature.
 *
 * @param sig signature handle, cannot be NULL.
 * @param idx index in the list. Number of available items could be obtained via the
 *            rnp_signature_get_preferred_alg_count() call.
 * @param alg on success algorithm name will be stored here. Caller must deallocate it using
 *            the rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_preferred_alg(rnp_signature_handle_t sig,
                                                     size_t                 idx,
                                                     char **                alg);

/**
 * @brief Get number of the preferred hash algorithms, listed in the signature. Applies to the
 *        self-signature (self-certification or direct-key signature).
 *
 * @param sig signature handle, cannot be NULL.
 * @param count on success nunmber of available algorithms will be stored here. It may be 0 if
 *              no such information is available within the signature.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_preferred_hash_count(rnp_signature_handle_t sig,
                                                            size_t *               count);

/**
 * @brief Get preferred hash algorithm from the preferences, specified in the signature.
 *
 * @param sig signature handle, cannot be NULL.
 * @param idx index in the list. Number of available items could be obtained via the
 *            rnp_signature_get_preferred_hash_count() call.
 * @param alg on success algorithm name will be stored here. Caller must deallocate it using
 *            the rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_preferred_hash(rnp_signature_handle_t sig,
                                                      size_t                 idx,
                                                      char **                alg);

/**
 * @brief Get number of the preferred compression algorithms, listed in the signature. Applies
 * to the self-signature (self-certification or direct-key signature).
 *
 * @param sig signature handle, cannot be NULL.
 * @param count on success nunmber of available algorithms will be stored here. It may be 0 if
 *              no such information is available within the signature.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_preferred_zalg_count(rnp_signature_handle_t sig,
                                                            size_t *               count);

/**
 * @brief Get preferred compression algorithm from the preferences, specified in the signature.
 *
 * @param sig signature handle, cannot be NULL.
 * @param idx index in the list. Number of available items could be obtained via the
 *            rnp_signature_get_preferred_zalg_count() call.
 * @param alg on success algorithm name will be stored here. Caller must deallocate it using
 *            the rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_preferred_zalg(rnp_signature_handle_t sig,
                                                      size_t                 idx,
                                                      char **                alg);

/**
 * @brief Get key usage flags from the signature, if any. Those are mapped directly to the
 *        values described in the OpenPGP specification.
 *
 * @param sig signature handle, cannot be NULL.
 * @param flags on success result will be stored here as or'ed together flag bits.
 *              If corresponding value is not available then 0 will be stored.
 *              These flags would correspond to string values which are passed to the
 *              rnp_op_generate_add_usage(). See the RNP_KEY_USAGE_* constants for possible
 *              values.
 *
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_key_flags(rnp_signature_handle_t sig, uint32_t *flags);

/**
 * @brief Get the key expiration time from the signature.
 *
 * @param sig signature handle, cannot be NULL.
 * @param expiry on success result will be stored here. It is number of seconds since key
 *               creation (not the signature creation) when this key is considered to be valid.
 *               Zero value means that key is valid forever.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_key_expiration(rnp_signature_handle_t sig,
                                                      uint32_t *             expiry);

/**
 * @brief Check whether signature indicates that corresponding user id should be considered as
 *        primary.
 *
 * @param sig signature handle, cannot be NULL.
 * @param primary on success result will be stored here. True for primary and false otherwise.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_primary_uid(rnp_signature_handle_t sig, bool *primary);

/**
 * @brief Get the key server associated with this key, if any.
 *
 * @param sig signature handle, cannot be NULL.
 * @param keyserver on success key server string, stored in the signature, will be stored here.
 *                  If it isn't present in the signature, an empty value will be stored. In
 *                  both cases, the buffer must be deallocated via the rnp_buffer_destroy()
 *                  call.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_key_server(rnp_signature_handle_t sig,
                                                  char **                keyserver);

/**
 * @brief Get the key server preferences flags, if any.
 *
 * @param sig signature handle, cannot be NULL.
 * @param flags on success flags will be stored here. Currently only one flag is supported:
 *              RNP_KEY_SERVER_NO_MODIFY
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_key_server_prefs(rnp_signature_handle_t sig,
                                                        uint32_t *             flags);

/** Get signer's key id from the signature.
 *  Note: if key id is not available from the signature then NULL value will
 *        be stored to result.
 * @param sig signature handle
 * @param result hex-encoded key id will be stored here. Cannot be NULL. You must free it
 *               later on using the rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_keyid(rnp_signature_handle_t sig, char **result);

/** Get signer's key fingerprint from the signature.
 *  Note: if key fingerprint is not available from the signature then NULL value will
 *        be stored to result.
 * @param sig signature handle
 * @param result hex-encoded key fp will be stored here. Cannot be NULL. You must free it
 *               later on using the rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_key_fprint(rnp_signature_handle_t sig, char **result);

/** Get signing key handle, if available.
 *  Note: if signing key is not available then NULL will be stored in key.
 * @param sig signature handle
 * @param key on success and key availability will contain signing key's handle. You must
 *            destroy it using the rnp_key_handle_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_signer(rnp_signature_handle_t sig,
                                              rnp_key_handle_t *     key);

/**
 * @brief Get fingerprint of the designated revocation key, if it is available. See
 *        section 5.2.3.15 of the RFC 4880 for the details.
 *
 * @param sig signature handle, cannot be NULL.
 * @param revoker on success hex-encoded revocation key fingerprint will be stored here, if
 *                available. Otherwise empty string will be stored. Must be freed via
 *                rnp_buffer_destroy().
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_revoker(rnp_signature_handle_t sig, char **revoker);

/**
 * @brief Get revocation reason data, if it is available in the signature.
 *
 * @param sig signature handle, cannot be NULL.
 * @param code string with revocation code will be stored here, if not NULL. See description of
 *             function rnp_key_revoke() for possible values. If information is not available,
 *             empty string will be stored here.
 * @param reason revocation reason will be stored here, if available. Otherwise empty string
 *               will be stored here. May be NULL if this information is not needed.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_revocation_reason(rnp_signature_handle_t sig,
                                                         char **                code,
                                                         char **                reason);

/**
 * @brief Get the signature trust level and amount. See OpenPGP specification for the details
 *        on their interpretation ('Trust Signature' signature subpacket).
 *
 * @param sig signature handle, cannot be NULL.
 * @param level trust level will be stored here if non-NULL. If corresponding value is not
 *              available then 0 will be stored.
 * @param amount trust amount will be stored here if non-NULL. If corresponding value is not
 *               available then 0 will be stored.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_get_trust_level(rnp_signature_handle_t sig,
                                                   uint8_t *              level,
                                                   uint8_t *              amount);

/**
 * @brief Get signature validity, revalidating it if didn't before.
 *
 * @param sig key/userid/document signature handle
 * @param flags validation flags. Currently supported only single flag:
 *              RNP_SIGNATURE_REVALIDATE - force revalidation of the signature even if it was
 *                  validated previously. Makes sense only for key signatures.
 *
 * @return Following error codes represents the validation status. For more detailed
 *         information why signature is invalid it is recommended to use
 *         rnp_signature_error_count()/rnp_signature_error_at() functions.
 *
 *         RNP_SUCCESS : operation succeeds and signature is valid
 *         RNP_ERROR_KEY_NOT_FOUND : signer's key not found
 *         RNP_ERROR_VERIFICATION_FAILED: verification failed, so validity cannot be checked
 *         RNP_ERROR_SIGNATURE_EXPIRED: signature is valid but expired
 *         RNP_ERROR_SIGNATURE_INVALID: signature is invalid (corrupted, malformed, was issued
 *             by invalid key, whatever else.)
 *
 *         Please also note that other error codes may be returned because of wrong
 *         function call (included, but not limited to):
 *         RNP_ERROR_NULL_POINTER: sig as well as some of its fields are NULL
 *         RNP_ERROR_BAD_PARAMETERS: invalid parameter value (unsupported flag, etc).
 */
RNP_API rnp_result_t rnp_signature_is_valid(rnp_signature_handle_t sig, uint32_t flags);

/**
 * @brief Get number of signature validation errors. This would allow to check in details why
 * signature verification failed.
 *
 * @param sig signature handle. Cannot be NULL.
 * @param count on success number of verification errors would be stored here
 * @return RNP_SUCCESS if operation succeeded,
 *         RNP_ERROR_VERIFICATION_FAILED if signature was not validated,
 *         RNP_ERROR_NULL_POINTER if any of the parameters is NULL.
 */
RNP_API rnp_result_t rnp_signature_error_count(rnp_signature_handle_t sig, size_t *count);

/**
 * @brief Get error code at the specified position.
 *
 * @param sig signature handle, cannot be NULL.
 * @param idx zero-based index of the error. Must be less then count obtained via the
 *            rnp_signature_error_count() call.
 * @param error on success error code will be stored here. Cannot be NULL.
 *        Following error codes are currently defined (but new ones could be added):
 *
 *        RNP_ERROR_SIG_ERROR : some general signature validation error
 *        RNP_ERROR_SIG_PARSE_ERROR : failed to parse signature
 *        RNP_ERROR_SIG_SIGNER_UNTRUSTED : key which produced signature is not trusted
 *        RNP_ERROR_SIG_PUB_ALG_MISMATCH : key and signature algorithms do not match
 *        RNP_ERROR_SIG_WEAK_HASH : too weak hash algorithm (i.e. MD5 or SHA1)
 *        RNP_ERROR_SIG_HASH_ALG_MISMATCH : used hash algorithm is not allowed by signature
 *                                          algorithm
 *        RNP_ERROR_SIG_LBITS_MISMATCH : left 16 bits of hash, stored in signature, do not
 *                                       match hash value
 *        RNP_ERROR_SIG_FROM_FUTURE : signature with timestamp from the future
 *        RNP_ERROR_SIG_EXPIRED : signature is expired
 *        RNP_ERROR_SIG_OLDER_KEY : signature is older than the key
 *        RNP_ERROR_SIG_EXPIRED_KEY : key was expired at signature creation time
 *        RNP_ERROR_SIG_FP_MISMATCH : key fingerprint doesn't match fingerprint from the
 *                                    signature
 *        RNP_ERROR_SIG_UNKNOWN_NOTATION : unknown critical notation
 *        RNP_ERROR_SIG_NOT_DOCUMENT : non-document signature used to sign data
 *        RNP_ERROR_SIG_NO_SIGNER_ID : unknown signer's key id/fingerprint
 *        RNP_ERROR_SIG_NO_SIGNER_KEY : signer's key not found
 *        RNP_ERROR_SIG_NO_HASH_CTX : no corresponding hash context
 *        RNP_ERROR_SIG_WRONG_KEY_SIG : non-key signature used on key
 *        RNP_ERROR_SIG_UID_MISSING : missing uid for certification
 *        RNP_ERROR_SIG_WRONG_BINDING : wrong subkey binding
 *        RNP_ERROR_SIG_WRONG_DIRECT : wrong direct-key signature
 *        RNP_ERROR_SIG_WRONG_REV : wrong revocation
 *        RNP_ERROR_SIG_UNSUPPORTED : unsupported key signature type
 *        RNP_ERROR_SIG_NO_PRIMARY_BINDING : subkey binding without primary key binding
 *        RNP_ERROR_SIG_BINDING_PARSE : failed to parse primary key binding signature
 *        RNP_ERROR_SIG_WRONG_BIND_TYPE : wrong primary key binding type
 *        RNP_ERROR_SIG_INVALID_BINDING : invalid primary key binding
 *        RNP_ERROR_SIG_UNUSABLE_KEY : key is not usable for verification, i.e. wrong key flags
 *
 * @return RNP_SUCCESS on success or some other value in case of error.
 */
RNP_API rnp_result_t rnp_signature_error_at(rnp_signature_handle_t sig,
                                            size_t                 idx,
                                            rnp_result_t *         error);

/** Dump signature packet to JSON, obtaining the whole information about it.
 *
 * @param sig sigmature handle, cannot be NULL
 * @param flags include additional fields in JSON (see RNP_JSON_DUMP_MPI and other
 *              RNP_JSON_DUMP_* flags)
 * @param result resulting JSON string will be stored here. You must free it using the
 *               rnp_buffer_destroy() function. See rnp_dump_packets_to_json() for
 *               detailed JSON format description.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_signature_packet_to_json(rnp_signature_handle_t sig,
                                                  uint32_t               flags,
                                                  char **                json);

/**
 * @brief Remove a signature.
 *
 * @param key key handle, cannot be NULL.
 * @param sig signature handle, cannot be NULL. Must be obtained via the key handle or one of
 *            its userids. You still need to call rnp_signature_handle_destroy afterwards to
 *            destroy handle itself. All other handles of the same signature, if any, should
 *            not be used after the call is made.
 * @return RNP_SUCCESS if signature was successfully deleted, or any other value on error.
 */
RNP_API rnp_result_t rnp_signature_remove(rnp_key_handle_t key, rnp_signature_handle_t sig);

/**
 * @brief Export a signature.
 *
 * @param sig signature handle, cannot be NULL.
 * @param output destination of the data stream.
 * @param flags must be RNP_KEY_EXPORT_ARMORED or 0.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_export(rnp_signature_handle_t sig,
                                          rnp_output_t           output,
                                          uint32_t               flags);

/** Free signature handle.
 *
 * @param sig signature handle.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_signature_handle_destroy(rnp_signature_handle_t sig);

/** Check whether user id is revoked.
 *
 * @param uid user id handle, should not be NULL.
 * @param result boolean result will be stored here on success. Cannot be NULL.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_is_revoked(rnp_uid_handle_t uid, bool *result);

/** Retrieve uid revocation signature, if any.
 *
 * @param uid user id handle, should not be NULL.
 * @param sig on success signature handle or NULL will be stored here. NULL will be stored in
 *            case when uid is not revoked.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_get_revocation_signature(rnp_uid_handle_t        uid,
                                                      rnp_signature_handle_t *sig);

/**
 * @brief Remove userid with all of its signatures from the key
 *
 * @param key key handle, cannot be NULL and must own the uid.
 * @param uid uid handle, cannot be NULL. Still must be destroyed afterwards via the
 *            rnp_uid_handle_destroy(). All other handles pointing to the same uid will
 *            become invalid and should not be used.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_uid_remove(rnp_key_handle_t key, rnp_uid_handle_t uid);

/** Destroy previously allocated user id handle.
 *
 * @param uid user id handle.
 * @return RNP_SUCCESS or error code
 */
RNP_API rnp_result_t rnp_uid_handle_destroy(rnp_uid_handle_t uid);

/**
 * @brief Get key's version as integer.
 *
 * @param key key handle, should not be NULL
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_get_version(rnp_key_handle_t handle, uint32_t *version);

/** Get number of the key's subkeys.
 *
 * @param key key handle.
 * @param count number of subkeys will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_subkey_count(rnp_key_handle_t key, size_t *count);

/** Get the handle of one of the key's subkeys, using its index in the list.
 *
 * @param key handle of the primary key.
 * @param idx zero-based index of the subkey.
 * @param subkey on success handle for the subkey will be stored here. You must free it
 *               using the rnp_key_handle_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_subkey_at(rnp_key_handle_t  key,
                                           size_t            idx,
                                           rnp_key_handle_t *subkey);

/** Get default key for specified usage. Accepts primary key
 *  and returns one of its subkeys suitable for desired usage.
 *  May return the same primary key if it is suitable for requested
 *  usage and flag RNP_KEY_SUBKEYS_ONLY is not set.
 *
 *  @param primary_key handle of the primary key.
 *  @param usage desired key usage i.e. "sign", "certify", etc,
 *               see rnp_op_generate_add_usage() function description for all possible values.
 *  @param flags possible values:  RNP_KEY_SUBKEYS_ONLY - select only subkeys,
 *               otherwise if flags is 0, primary key can be returned if
 *               it is suitable for specified usage.
 *               Note: If RNP_EXPERIMENTAL_PQC is set, then the flag
 *               RNP_KEY_PREFER_PQC_ENC_SUBKEY can be used to prefer PQC-encryption subkeys
 *               over non-PQC-encryption subkeys
 *  @param default_key on success resulting key handle will be stored here, otherwise it
 *                     will contain NULL value. You must free this handle after use with
 *                     rnp_key_handle_destroy().
 *  @return RNP_SUCCESS on success, RNP_ERROR_KEY_NOT_FOUND if no key with desired usage
 *          was found or any other error code.
 */
RNP_API rnp_result_t rnp_key_get_default_key(rnp_key_handle_t  primary_key,
                                             const char *      usage,
                                             uint32_t          flags,
                                             rnp_key_handle_t *default_key);

/** Get the key's algorithm.
 *
 * @param key key handle
 * @param alg string with algorithm name will be stored here. You must free it using the
 *            rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_alg(rnp_key_handle_t key, char **alg);

#if defined(RNP_EXPERIMENTAL_PQC)
/** Get a SPHINCS+ key's parameter string
 *
 * @param key key handle
 * @param alg string with parameter name will be stored here. You must free it using the
 *            rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 * NOTE: This is an experimental feature and this function can be replaced (or removed) at any
 * time.
 */
RNP_API rnp_result_t rnp_key_sphincsplus_get_param(rnp_key_handle_t handle, char **param);
#endif

/** Get number of bits in the key. For EC-based keys it will return size of the curve.
 *
 * @param key key handle
 * @param bits number of bits will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_bits(rnp_key_handle_t key, uint32_t *bits);

/** Get the number of bits in q parameter of the DSA key. Makes sense only for DSA keys.
 *
 * @param key key handle
 * @param qbits number of bits will be stored here.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_dsa_qbits(rnp_key_handle_t key, uint32_t *qbits);

/** Get the curve of EC-based key.
 *
 * @param key key handle
 * @param curve string with name of the curve will be stored here. You must free it using the
 *              rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_get_curve(rnp_key_handle_t key, char **curve);

/** Add a new user identifier to a key
 *
 *  @param ffi
 *  @param key the key to add - must be a secret key
 *  @param uid the UID to add
 *  @param hash name of the hash function to use for the uid binding
 *         signature (eg "SHA256"). If NULL, default hash algorithm
 *         will be used.
 *  @param expiration time when this user id expires
 *  @param key_flags usage flags, see section 5.2.3.21 of RFC 4880
 *         or just provide zero to indicate no special handling.
 *  @param primary indicates if this is the primary UID
 *  @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_add_uid(rnp_key_handle_t key,
                                     const char *     uid,
                                     const char *     hash,
                                     uint32_t         expiration,
                                     uint8_t          key_flags,
                                     bool             primary);

/* The following output hex encoded strings */

/**
 * @brief Get key's fingerprint as hex-encoded string.
 *
 * @param key key handle, should not be NULL
 * @param fprint pointer to the NULL-terminated string with hex-encoded fingerprint will be
 *        stored here. You must free it later using rnp_buffer_destroy function.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_get_fprint(rnp_key_handle_t key, char **fprint);

/**
 * @brief Get key's id as hex-encoded string
 *
 * @param key key handle, should not be NULL
 * @param keyid pointer to the NULL-terminated string with hex-encoded key id will be
 *        stored here. You must free it later using rnp_buffer_destroy function.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_get_keyid(rnp_key_handle_t key, char **keyid);

/**
 * @brief Get key's grip as hex-encoded string
 *
 * @param key key handle, should not be NULL
 * @param grip pointer to the NULL-terminated string with hex-encoded key grip will be
 *        stored here. You must free it later using rnp_buffer_destroy function.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_get_grip(rnp_key_handle_t key, char **grip);

/**
 * @brief Get primary's key grip for the subkey, if available.
 *
 * @param key key handle, should not be NULL
 * @param grip pointer to the NULL-terminated string with hex-encoded key grip or NULL will be
 *        stored here, depending whether primary key is available or not.
 *        You must free it later using rnp_buffer_destroy function.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_get_primary_grip(rnp_key_handle_t key, char **grip);

/**
 * @brief Get primary's key fingerprint for the subkey, if available.
 *
 * @param key subkey handle, should not be NULL
 * @param grip pointer to the NULL-terminated string with hex-encoded key fingerprint or NULL
 *             will be stored here, depending whether primary key is available or not. You must
 *             free it later using rnp_buffer_destroy function.
 * @return RNP_SUCCESS on success, RNP_BAD_PARAMETERS if not a subkey, or other error code
 *         on failure.
 */
RNP_API rnp_result_t rnp_key_get_primary_fprint(rnp_key_handle_t key, char **fprint);

/**
 * @brief Check whether certain usage type is allowed for the key.
 *
 * @param key key handle, should not be NULL
 * @param usage string describing the key usage. For the list of allowed values see the
 *              rnp_op_generate_add_usage() function description.
 * @param result function result will be stored here. Could not be NULL.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_allows_usage(rnp_key_handle_t key,
                                          const char *     usage,
                                          bool *           result);

/**
 * @brief Get the key's creation time.
 *
 * @param key key handle, should not be NULL.
 * @param result creation time will be stored here. Cannot be NULL.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_get_creation(rnp_key_handle_t key, uint32_t *result);

/**
 * @brief Get the key's expiration time in seconds.
 *        Note: 0 means that the key doesn't expire.
 *
 * @param key key handle, should not be NULL
 * @param result expiration time will be stored here. Could not be NULL.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_get_expiration(rnp_key_handle_t key, uint32_t *result);

/**
 * @brief Set the key's expiration time in seconds.
 *        Note: this will require re-signing, which requires availability of the secret key (or
 *        secret primary key for the subkey). If the secret key is locked then may ask for
 *        key's password via FFI callback.
 *
 * @param key key's handle.
 * @param expiry expiration time in seconds (or 0 if key doesn't expire). Please note that it
 *               is calculated from the key creation time, not from the current time.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_set_expiration(rnp_key_handle_t key, uint32_t expiry);

/**
 * @brief Check whether public key is valid. This includes checks of the self-signatures,
 *        expiration times, revocations and so on.
 *        Note: it doesn't take in account secret key, if it is available.
 *
 * @param key key's handle.
 * @param result on success true or false will be stored here. Cannot be NULL.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_is_valid(rnp_key_handle_t key, bool *result);

/**
 * @brief Get the timestamp till which key can be considered as valid.
 *        Note: this will take into account not only key's expiration, but revocations as well.
 *        For the subkey primary key's validity time will be also checked.
 *        While in OpenPGP key creation and expiration times are 32-bit, their sum may overflow
 *        32 bits, so rnp_key_valid_till64 function should be used.
 *        In case of 32 bit overflow result will be set to the UINT32_MAX - 1.
 * @param key key's handle.
 * @param result on success timestamp will be stored here. If key doesn't expire then maximum
 *               value (UINT32_MAX or UINT64_MAX) will be stored here. If key was never valid
 *               then zero value will be stored here.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_valid_till(rnp_key_handle_t key, uint32_t *result);
RNP_API rnp_result_t rnp_key_valid_till64(rnp_key_handle_t key, uint64_t *result);

/**
 * @brief Check whether key is revoked.
 *
 * @param key key handle, should not be NULL
 * @param result on success result will be stored here. Could not be NULL.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_is_revoked(rnp_key_handle_t key, bool *result);

/**
 * @brief Get textual description of the key's revocation reason (if any)
 *
 * @param key key handle, should not be NULL
 * @param result on success pointer to the NULL-terminated string will be stored here.
 *               You must free it later using rnp_buffer_destroy() function.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_get_revocation_reason(rnp_key_handle_t key, char **result);

/**
 * @brief Check whether revoked key was superseded by other key.
 *
 * @param key key handle, should not be NULL
 * @param result on success result will be stored here. Could not be NULL.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_is_superseded(rnp_key_handle_t key, bool *result);

/**
 * @brief Check whether revoked key's material was compromised.
 *
 * @param key key handle, should not be NULL
 * @param result on success result will be stored here. Could not be NULL.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_is_compromised(rnp_key_handle_t key, bool *result);

/**
 * @brief Check whether revoked key was retired.
 *
 * @param key key handle, should not be NULL
 * @param result on success result will be stored here. Could not be NULL.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_is_retired(rnp_key_handle_t key, bool *result);

/**
 * @brief Check whether key is expired.
 *        Note: while expired key cannot be used to generate new signatures or encrypt to, it
 *        still could be used to check older signatures/decrypt previously encrypted data.
 *
 * @param key key handle, should not be NULL.
 * @param result on success result will be stored here. True means that key is expired and is
 *               not usable and false otherwise.
 * @return RNP_SUCCESS or error code on failure.
 */
RNP_API rnp_result_t rnp_key_is_expired(rnp_key_handle_t key, bool *result);

/** check if a key is currently locked
 *
 *  @param key
 *  @param result pointer to hold the result. This will be set to true if
 *         the key is currently locked, or false otherwise. Must not be NULL.
 *  @return RNP_SUCCESS on success, or any other value on error
 **/
RNP_API rnp_result_t rnp_key_is_locked(rnp_key_handle_t key, bool *result);

/**
 * @brief Get type of protection, used for secret key data.
 *
 * @param key key handle, cannot be NULL and should have secret part (see function
 *            rnp_key_have_secret()).
 * @param type on success protection type will be stored here. Cannot be NULL.
 *             Must be freed by caller via rnp_buffer_destroy() call.
 *             Currently defined values are:
 *             - "None" : secret key data is stored in plaintext.
 *             - "Encrypted" : secret key data is encrypted, using just CRC as integrity
 *                 protection.
 *             - "Encrypted-Hashed" : secret key data is encrypted, using the SHA1 hash as
 *                 an integrity protection.
 *             - "GPG-None" : secret key data is not available at all (this would happen if
 *                 secret key is exported from GnuPG via --export-secret-subkeys)
 *             - "GPG-Smartcard" : secret key data is stored on smartcard by GnuPG, so is not
 *                 available
 *             - "Unknown" : key protection type is unknown, so secret key data is not
 *                 available
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_key_get_protection_type(rnp_key_handle_t key, char **type);

/**
 * @brief Get mode in which secret key data is encrypted.
 *
 * @param key key handle, cannot be NULL and should have secret part (see function
 *            rnp_key_have_secret()).
 * @param mode on success secret key protection mode name will be stored here. Cannot be NULL.
 *             Must be freed by caller via rnp_buffer_destroy() call.
 *             Currently defined values are:
 *             - "None" : secret key data is not encrypted at all
 *             - "Unknown" : it is not known how secret key data is encrypted, so there is no
 *                 way to unlock/unprotect the key.
 *             - "CFB" : secret key data is encrypted in CFB mode, using the password
 *             - "CBC" : secret key data is encrypted in CBC mode, using the password
 *                       (only for G10 keys)
 *             - "OCB" : secret key data is encrypted in OCB mode, using the password
 *                       (only for G10 keys)
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_key_get_protection_mode(rnp_key_handle_t key, char **mode);

/**
 * @brief Get cipher, used to encrypt secret key data.
 *        Note: this call will return an error if secret key data is not available or secret
 *        key is not encrypted.
 *
 * @param key key handle, cannot be NULL and should have secret part.
 * @param cipher on success cipher name will be stored here. See
 *               rnp_op_generate_set_protection_cipher for possible values. Cannot be NULL.
 *               Must be freed by caller via rnp_buffer_destroy() call.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_key_get_protection_cipher(rnp_key_handle_t key, char **cipher);

/**
 * @brief Get hash, used to derive secret key data encrypting key from the password.
 *        Note: this call will return an error if secret key data is not available or secret
 *        key is not encrypted.
 * @param key key handle, cannot be NULL and should have secret part.
 * @param hash on success hash name will be stored here. See rnp_op_generate_set_hash() for the
 *             whole list of possible values. Cannot be NULL.
 *             Must be freed by caller via rnp_buffer_destroy() call.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_key_get_protection_hash(rnp_key_handle_t key, char **hash);

/**
 * @brief Get number of iterations used to derive encrypting key from password, using the hash
 *        function.
 *        Note: this call will return an error if secret key data is not available or secret
 *        key is not encrypted.
 *
 * @param key key handle, cannot be NULL and should have secret part.
 * @param iterations on success number of iterations will be stored here. Cannot be NULL.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_key_get_protection_iterations(rnp_key_handle_t key,
                                                       size_t *         iterations);

/** lock the key
 *
 *  A locked key does not have the secret key material immediately
 *  available for use. A locked and protected (aka encrypted) key
 *  is safely encrypted in memory and requires a password for
 *  performing any operations involving the secret key material.
 *
 *  Generally lock/unlock are not useful for unencrypted (not protected) keys.
 *
 *  @param key
 *  @return RNP_SUCCESS on success, or any other value on error
 **/
RNP_API rnp_result_t rnp_key_lock(rnp_key_handle_t key);

/** unlock the key
 *
 *  An unlocked key has unencrypted secret key material available for use
 *  without a password.
 *
 *  Generally lock/unlock are not useful for unencrypted (not protected) keys.
 *
 *  @param key
 *  @param password the password to unlock the key. If NULL, the password
 *         provider will be used.
 *  @return RNP_SUCCESS on success, or any other value on error
 **/
RNP_API rnp_result_t rnp_key_unlock(rnp_key_handle_t key, const char *password);

/** check if a key is currently protected
 *
 *  A protected key is one that is encrypted and can be safely held in memory
 *  and locked/unlocked as needed.
 *
 *  @param key
 *  @param result pointer to hold the result. This will be set to true if
 *         the key is currently protected, or false otherwise. Must not be NULL.
 *  @return RNP_SUCCESS on success, or any other value on error
 **/
RNP_API rnp_result_t rnp_key_is_protected(rnp_key_handle_t key, bool *result);

/** protect the key
 *
 *  This can be used to set a new password on a key or to protect an unprotected
 *  key.
 *
 *  Note that the only required parameter is "password".
 *
 *  @param key
 *  @param password the new password to encrypt/re-encrypt the key with.
 *         Must not be NULL.
 *  @param cipher the cipher (AES256, etc) used to encrypt the key. May be NULL,
 *         in which case a default will be used.
 *  @param cipher_mode the cipher mode (CFB, CBC, OCB). This parameter is not
 *         well supported currently and is mostly relevant for G10.
 *         May be NULL.
 *  @param hash the hash algorithm (SHA512, etc) used for the String-to-Key key
 *         derivation. May be NULL, in which case a default will be used.
 *  @param iterations the number of iterations used for the String-to-Key key
 *         derivation. Use 0 to select a reasonable default.
 *  @return RNP_SUCCESS on success, or any other value on error
 **/
RNP_API rnp_result_t rnp_key_protect(rnp_key_handle_t handle,
                                     const char *     password,
                                     const char *     cipher,
                                     const char *     cipher_mode,
                                     const char *     hash,
                                     size_t           iterations);

/** unprotect the key
 *
 *  This removes the encryption from the key.
 *
 *  @param key
 *  @param password the password to unlock the key. If NULL, the password
 *         provider will be used.
 *  @return RNP_SUCCESS on success, or any other value on error
 **/
RNP_API rnp_result_t rnp_key_unprotect(rnp_key_handle_t key, const char *password);

/**
 * @brief Check whether key is primary key.
 *
 * @param key key handle, cannot be NULL.
 * @param result true or false will be stored here on success.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_key_is_primary(rnp_key_handle_t key, bool *result);

/**
 * @brief Check whether key is subkey.
 *
 * @param key key handle, cannot be NULL.
 * @param result true or false will be stored here on success.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_key_is_sub(rnp_key_handle_t key, bool *result);

/**
 * @brief Check whether key has secret part.
 *
 * @param key key handle, cannot be NULL.
 * @param result true will be stored here on success, or false otherwise.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_key_have_secret(rnp_key_handle_t key, bool *result);

/**
 * @brief Check whether key has public part. Generally all keys would have public part.
 *
 * @param key key handle, cannot be NULL.
 * @param result true will be stored here on success, or false otherwise.
 * @return RNP_SUCCESS on success, or any other value on error.
 */
RNP_API rnp_result_t rnp_key_have_public(rnp_key_handle_t key, bool *result);

/** Get the information about key packets in JSON string.
 *  Note: this will not work for G10 keys.
 *
 * @param key key's handle, cannot be NULL
 * @param secret dump secret key instead of public
 * @param flags include additional fields in JSON (see RNP_JSON_DUMP_MPI and other
 *              RNP_JSON_DUMP_* flags)
 * @param result resulting JSON string will be stored here. You must free it using the
 *               rnp_buffer_destroy() function. See rnp_dump_packets_to_json()
 *               for detailed JSON format description.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_key_packets_to_json(rnp_key_handle_t key,
                                             bool             secret,
                                             uint32_t         flags,
                                             char **          result);

/** Dump OpenPGP packets stream information to the JSON string.
 * @param input source with OpenPGP data
 * @param flags include additional fields in JSON (see RNP_JSON_DUMP_MPI and other
 *              RNP_JSON_DUMP_* flags)
 * @result resulting JSON string will be stored here. You must free it using the
 *         rnp_buffer_destroy() function.\n
 *         JSON output is an array of JSON objects, each array item
 *         represents an OpenPGP packet. Packet objects have common
 *         member object named "header" and packet-specific members.
 *         The "header" object has the following members:\n
 * JSON member     | Description
 * ----------------|------------
 *  "offset"       | integer, byte offset from the beginning of the binary stream
 *  "tag"          | integer, packet tag numeric value
 *  "tag.str"      | string, packet type string
 *  "raw"          | string, hexadecimal raw value of the packet header
 *  "length"       | integer, packet length in bytes
 *  "partial"      | boolean, true if the header is a partial body length header
 *  "indeterminate"| boolean, true if the packet is of indeterminate length
 *         Example "header" object:\n
 *
 *             "header":{
 *               "offset":63727,
 *               "tag":2,
 *               "tag.str":"Signature",
 *               "raw":"c2c07c",
 *               "length":316,
 *               "partial":false,
 *               "indeterminate":false
 *             }
 *
 *         You can see examples of complete JSON dumps by running the `rnp`
 *         program with `--list-packets --json` command line options.
 *
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_dump_packets_to_json(rnp_input_t input,
                                              uint32_t    flags,
                                              char **     result);

/** Dump OpenPGP packets stream information to output in humand-readable format.
 * @param input source with OpenPGP data
 * @param output text, describing packet sequence, will be written here
 * @param flags see RNP_DUMP_MPI and other RNP_DUMP_* constants.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_dump_packets_to_output(rnp_input_t  input,
                                                rnp_output_t output,
                                                uint32_t     flags);

/* Signing operations */

/** @brief Create signing operation context. This method should be used for embedded
 *         signatures of binary data. For detached and cleartext signing corresponding
 *         function should be used.
 *  @param op pointer to opaque signing context
 *  @param ffi
 *  @param input stream with data to be signed. Could not be NULL.
 *  @param output stream to write results to. Could not be NULL.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_create(rnp_op_sign_t *op,
                                        rnp_ffi_t      ffi,
                                        rnp_input_t    input,
                                        rnp_output_t   output);

/** @brief Create cleartext signing operation context. Input should be text data. Output will
 *         contain source data with additional headers and armored signature.
 *  @param op pointer to opaque signing context
 *  @param ffi
 *  @param input stream with data to be signed. Could not be NULL.
 *  @param output stream to write results to. Could not be NULL.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_cleartext_create(rnp_op_sign_t *op,
                                                  rnp_ffi_t      ffi,
                                                  rnp_input_t    input,
                                                  rnp_output_t   output);

/** @brief Create detached signing operation context. Output will contain only signature of the
 *         source data.
 *  @param op pointer to opaque signing context
 *  @param ffi
 *  @param input stream with data to be signed. Could not be NULL.
 *  @param signature stream to write results to. Could not be NULL.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_detached_create(rnp_op_sign_t *op,
                                                 rnp_ffi_t      ffi,
                                                 rnp_input_t    input,
                                                 rnp_output_t   signature);

/** @brief Add information about the signature so it could be calculated later in execute
 *         function call. Multiple signatures could be added.
 *  @param op opaque signing context. Must be successfully initialized with one of the
 *         rnp_op_sign_*_create functions.
 *  @param key handle of the private key. Private key should be capable for signing.
 *  @param sig pointer to opaque structure holding the signature information. May be NULL.
 *         You should not free it as it will be destroyed together with signing context.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_add_signature(rnp_op_sign_t            op,
                                               rnp_key_handle_t         key,
                                               rnp_op_sign_signature_t *sig);

/** @brief Set hash algorithm used during signature calculation instead of default one, or one
 *         set by rnp_op_encrypt_set_hash/rnp_op_sign_set_hash
 *  @param sig opaque signature context, returned via rnp_op_sign_add_signature
 *  @param hash hash algorithm to be used
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_signature_set_hash(rnp_op_sign_signature_t sig,
                                                    const char *            hash);

/** @brief Set signature creation time. By default current time is used or value set by
 *         rnp_op_encrypt_set_creation_time/rnp_op_sign_set_creation_time
 *  @param sig opaque signature context, returned via rnp_op_sign_add_signature
 *  @param create creation time in seconds since Jan, 1 1970 UTC
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_signature_set_creation_time(rnp_op_sign_signature_t sig,
                                                             uint32_t                create);

/** @brief Set signature expiration time. By default is set to never expire or to value set by
 *         rnp_op_encrypt_set_expiration_time/rnp_op_sign_set_expiration_time
 *  @param sig opaque signature context, returned via rnp_op_sign_add_signature
 *  @param expire expiration time in seconds since the creation time. 0 value is used to mark
 *         signature as non-expiring (default value)
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_signature_set_expiration_time(rnp_op_sign_signature_t sig,
                                                               uint32_t expires);

/** @brief Set data compression parameters. Makes sense only for embedded signatures.
 *  @param op opaque signing context. Must be initialized with rnp_op_sign_create function
 *  @param compression compression algorithm (zlib, zip, bzip2)
 *  @param level compression level, 0-9. 0 disables compression.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_set_compression(rnp_op_sign_t op,
                                                 const char *  compression,
                                                 int           level);

/** @brief Enabled or disable armored (textual) output. Doesn't make sense for cleartext sign.
 *  @param op opaque signing context. Must be initialized with rnp_op_sign_create or
 *         rnp_op_sign_detached_create function.
 *  @param armored true if armoring should be used (it is disabled by default)
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_set_armor(rnp_op_sign_t op, bool armored);

/** @brief Set hash algorithm used during signature calculation. This will set hash function
 *         for all signature. To change it for a single signature use
 *         rnp_op_sign_signature_set_hash function.
 *  @param op opaque signing context. Must be successfully initialized with one of the
 *         rnp_op_sign_*_create functions.
 *  @param hash hash algorithm to be used
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_set_hash(rnp_op_sign_t op, const char *hash);

/** @brief Set signature creation time. By default current time is used.
 *  @param op opaque signing context. Must be successfully initialized with one of the
 *         rnp_op_sign_*_create functions.
 *  @param create creation time in seconds since Jan, 1 1970 UTC. 32 bit unsigned integer
 *                datatype is used here instead of 64 bit (like modern timestamps do) because
 *                in OpenPGP messages times are stored as 32-bit unsigned integers.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_set_creation_time(rnp_op_sign_t op, uint32_t create);

/** @brief Set signature expiration time.
 *  @param op opaque signing context. Must be successfully initialized with one of the
 *         rnp_op_sign_*_create functions.
 *  @param expire expiration time in seconds since the creation time. 0 value is used to mark
 *         signature as non-expiring (default value)
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_set_expiration_time(rnp_op_sign_t op, uint32_t expire);

/** @brief Set input's file name. Makes sense only for embedded signature.
 *  @param op opaque signing context. Must be initialized with rnp_op_sign_create function
 *  @param filename source data file name. Special value _CONSOLE may be used to mark message
 *         as 'for your eyes only', i.e. it should not be stored anywhere but only displayed
 *         to the receiver. Default is the empty string.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_set_file_name(rnp_op_sign_t op, const char *filename);

/** @brief Set input's file modification date. Makes sense only for embedded signature.
 *  @param op opaque signing context. Must be initialized with rnp_op_sign_create function
 *  @param mtime modification time in seconds since Jan, 1 1970 UTC. 32 bit unsigned integer
 *               datatype is used here instead of 64 bit (like modern timestamps do) because
 *               in OpenPGP messages times are stored as 32-bit unsigned integers.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_sign_set_file_mtime(rnp_op_sign_t op, uint32_t mtime);

/** @brief Execute previously initialized signing operation.
 *  @param op opaque signing context. Must be successfully initialized with one of the
 *         rnp_op_sign_*_create functions. At least one signing key should be added.
 *  @return RNP_SUCCESS or error code if failed. On success output stream, passed in the create
 *          function call, will be populated with signed data
 */
RNP_API rnp_result_t rnp_op_sign_execute(rnp_op_sign_t op);

/** @brief Free resources associated with signing operation.
 *  @param op opaque signing context. Must be successfully initialized with one of the
 *         rnp_op_sign_*_create functions.
 *  @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_sign_destroy(rnp_op_sign_t op);

/* Verification */

/** @brief Create verification operation context. This method should be used for embedded
 *         signatures, cleartext signed data and encrypted (and possibly signed) data.
 *         For the detached signature verification the function rnp_op_verify_detached_create()
 *         should be used.
 *  @param op pointer to opaque verification context. When no longer needed must be destroyed
 *            via the rnp_op_verify_destroy() call.
 *  @param ffi
 *  @param input stream with signed data. Could not be NULL.
 *  @param output stream to write results to. Could not be NULL, but may be null output stream
 *         if verified data should be discarded.
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_verify_create(rnp_op_verify_t *op,
                                          rnp_ffi_t        ffi,
                                          rnp_input_t      input,
                                          rnp_output_t     output);

/** @brief Create verification operation context for detached signature.
 *  @param op pointer to opaque verification context. When no longer needed must be destroyed
 *            via the rnp_op_verify_destroy() call.
 *  @param ffi
 *  @param input stream with raw data. Could not be NULL.
 *  @param signature stream with detached signature data
 *  @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_verify_detached_create(rnp_op_verify_t *op,
                                                   rnp_ffi_t        ffi,
                                                   rnp_input_t      input,
                                                   rnp_input_t      signature);

/**
 * @brief Set additional flags which control data verification/decryption process.
 *
 * @param op pointer to opaque verification context.
 * @param flags verification flags. OR-ed combination of RNP_VERIFY_* values.
 *              Following flags are supported:
 *              RNP_VERIFY_IGNORE_SIGS_ON_DECRYPT - ignore invalid signatures for the encrypted
 *                and signed data. If this flag is set then rnp_op_verify_execute() call will
 *                succeed and output data even if all signatures are invalid or issued by the
 *                unknown key(s).
 *              RNP_VERIFY_REQUIRE_ALL_SIGS - require that all signatures (if any) must be
 *                valid for successful run of rnp_op_verify_execute().
 *              RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT - allow hidden recipient during the
 *                decryption.
 *
 *              Note: all flags are set at once, if some flag is not present in the subsequent
 *              call then it will be unset.
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_verify_set_flags(rnp_op_verify_t op, uint32_t flags);

/** @brief Execute previously initialized verification operation.
 *  @param op opaque verification context. Must be successfully initialized.
 *  @return RNP_SUCCESS if data was processed successfully and output may be used. By default
 *          this means at least one valid signature for the signed data, or successfully
 *          decrypted data if no signatures are present.
 *          This behaviour may be overridden via rnp_op_verify_set_flags() call.
 *
 *          To check number of signatures and their verification status use functions
 *          rnp_op_verify_get_signature_count() and rnp_op_verify_get_signature_at().
 *          To check data encryption status use function rnp_op_verify_get_protection_info().
 */
RNP_API rnp_result_t rnp_op_verify_execute(rnp_op_verify_t op);

/** @brief Get number of the signatures for verified data.
 *  @param op opaque verification context. Must be initialized and have execute() called on it.
 *  @param count result will be stored here on success.
 *  @return RNP_SUCCESS if call succeeded.
 */
RNP_API rnp_result_t rnp_op_verify_get_signature_count(rnp_op_verify_t op, size_t *count);

/** @brief Get single signature information based on its index.
 *  @param op opaque verification context. Must be initialized and have execute() called on it.
 *  @param sig opaque signature context data will be stored here on success. It is not needed
 *             to deallocate this structure manually, it will be destroyed together with op in
 *             rnp_op_verify_destroy() call.
 *  @return RNP_SUCCESS if call succeeded.
 */
RNP_API rnp_result_t rnp_op_verify_get_signature_at(rnp_op_verify_t            op,
                                                    size_t                     idx,
                                                    rnp_op_verify_signature_t *sig);

/** @brief Get embedded in OpenPGP data file name and modification time. Makes sense only for
 *         embedded signature verification.
 *  @param op opaque verification context. Must be initialized and have execute() called on it.
 *  @param filename pointer to the filename. On success caller is responsible for freeing it
 *                  via the rnp_buffer_destroy function call. May be NULL if this information
 *                  is not needed.
 *  @param mtime file modification time will be stored here on success. May be NULL.
 *  @return RNP_SUCCESS if call succeeded.
 */
RNP_API rnp_result_t rnp_op_verify_get_file_info(rnp_op_verify_t op,
                                                 char **         filename,
                                                 uint32_t *      mtime);

/**
 * @brief Get format of the data stored in the message, if available.
 *
 * @param op opaque verification context. Must be initialized and have execute() called on it.
 * @param format character describing format would be stored here, see RFC 4880 section 5.9 and
 *               further standard extensions for possible values. If information is not
 *               available then '\0' value will be stored here. Cannot be NULL.
 * @return RNP_SUCCESS if call succeeded.
 */
RNP_API rnp_result_t rnp_op_verify_get_format(rnp_op_verify_t op, char *format);

/**
 * @brief Get data protection (encryption) mode, used in processed message.
 *
 * @param op opaque verification context. Must be initialized and have execute() called on it.
 * @param mode on success string with mode will be stored here. Caller is responsible for
 *             freeing it using the rnp_buffer_destroy() call. May be NULL if information is
 *             not needed. Currently defined values are as following:
 *             - none : message was not protected/encrypted
 *             - cfb : message was encrypted in CFB mode without the MDC
 *             - cfb-mdc : message was encrypted in CFB mode and protected with MDC
 *             - aead-ocb : message was encrypted in AEAD-OCB mode
 *             - aead-eax : message was encrypted in AEAD-EAX mode
 * @param cipher symmetric cipher, used for data encryption. May be NULL if information is not
 *               needed. Must be freed by rnp_buffer_destroy() call.
 * @param valid true if message integrity protection was used (i.e. MDC or AEAD), and it was
 *              validated successfully. Otherwise (even for raw cfb mode) will be false. May be
 *              NULL if information is not needed.
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_verify_get_protection_info(rnp_op_verify_t op,
                                                       char **         mode,
                                                       char **         cipher,
                                                       bool *          valid);

/**
 * @brief Get number of public keys (recipients) to whom message was encrypted to.
 *
 * @param op opaque verification context. Must be initialized and have execute() called on it.
 * @param count on success number of keys will be stored here. Cannot be NULL.
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_verify_get_recipient_count(rnp_op_verify_t op, size_t *count);

/**
 * @brief Get the recipient's handle, used to decrypt message.
 *
 * @param op opaque verification context. Must be initialized and have execute() called on it.
 * @param recipient pointer to the opaque handle context. Cannot be NULL. If recipient's key
 *                  was used to decrypt a message then handle will be stored here, otherwise
 *                  it will be set to NULL.
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_verify_get_used_recipient(rnp_op_verify_t         op,
                                                      rnp_recipient_handle_t *recipient);

/**
 * @brief Get the recipient's handle by index.
 *
 * @param op opaque verification context. Must be initialized and have execute() called on it.
 * @param idx zero-based index in array.
 * @param recipient pointer to the opaque handle context. Cannot be NULL. On success handle
 *                  will be stored here.
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_verify_get_recipient_at(rnp_op_verify_t         op,
                                                    size_t                  idx,
                                                    rnp_recipient_handle_t *recipient);

/**
 * @brief Get recipient's keyid.
 *
 * @param recipient recipient's handle, obtained via rnp_op_verify_get_used_recipient() or
 *                  rnp_op_verify_get_recipient_at() function call. Cannot be NULL.
 * @param keyid on success pointer to NULL-terminated string with hex-encoded keyid will be
 *              stored here. Cannot be NULL. Must be freed using the rnp_buffer_destroy().
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_recipient_get_keyid(rnp_recipient_handle_t recipient, char **keyid);

/**
 * @brief Get recipient's key algorithm.
 *
 * @param recipient recipient's handle, obtained via rnp_op_verify_get_used_recipient() or
 *                  rnp_op_verify_get_recipient_at() function call. Cannot be NULL.
 * @param alg on success pointer to NULL-terminated string with algorithm will be stored here.
 *            Cannot be NULL. Must be freed using the rnp_buffer_destroy().
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_recipient_get_alg(rnp_recipient_handle_t recipient, char **alg);

/**
 * @brief Get number of symenc entries (i.e. passwords), to which message was encrypted.
 *
 * @param op opaque verification context. Must be initialized and have execute() called on it.
 * @param count on success number of keys will be stored here. Cannot be NULL.
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_verify_get_symenc_count(rnp_op_verify_t op, size_t *count);

/**
 * @brief Get the symenc handle, used to decrypt a message.
 *
 * @param op opaque verification context. Must be initialized and have execute() called on it.
 * @param symenc pointer to the opaque symenc context. Cannot be NULL. If password was used to
 *               decrypt a message then handle will be stored here, otherwise it will be set to
 *               NULL.
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_verify_get_used_symenc(rnp_op_verify_t      op,
                                                   rnp_symenc_handle_t *symenc);

/**
 * @brief Get the symenc handle by index.
 *
 * @param op opaque verification context. Must be initialized and have execute() called on it.
 * @param idx zero-based index in array.
 * @param symenc pointer to the opaque handle context. Cannot be NULL. On success handle
 *               will be stored here.
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_verify_get_symenc_at(rnp_op_verify_t      op,
                                                 size_t               idx,
                                                 rnp_symenc_handle_t *symenc);

/**
 * @brief Get the symmetric cipher, used to encrypt data encryption key.
 *        Note: if message is encrypted with only one passphrase and without public keys, then
 *        key, derived from password, may be used to encrypt the whole message.
 * @param symenc opaque handle, cannot be NULL.
 * @param cipher NULL-terminated string with cipher's name will be stored here. Cannot be NULL.
 *               Must be freed using the rnp_buffer_destroy().
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_symenc_get_cipher(rnp_symenc_handle_t symenc, char **cipher);

/**
 * @brief Get AEAD algorithm if it was used to encrypt data encryption key.
 *
 * @param symenc opaque handle, cannot be NULL.
 * @param alg NULL-terminated string with AEAD algorithm name will be stored here. If AEAD was
 *            not used then it will contain string 'None'. Must be freed using the
 *            rnp_buffer_destroy().
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_symenc_get_aead_alg(rnp_symenc_handle_t symenc, char **alg);

/**
 * @brief Get hash algorithm, used to derive key from the passphrase.
 *
 * @param symenc opaque handle, cannot be NULL.
 * @param alg NULL-terminated string with hash algorithm name will be stored here. Cannot be
 *            NULL. Must be freed using the rnp_buffer_destroy().
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_symenc_get_hash_alg(rnp_symenc_handle_t symenc, char **alg);

/**
 * @brief Get string-to-key type, used to derive password.
 *
 * @param symenc opaque handle, cannot be NULL.
 * @param type NULL-terminated string with s2k type will be stored here. Currently following
 *             types are available: 'Simple', 'Salted', 'Iterated and salted'. Please note that
 *             first two are considered weak and should not be used. Must be freed using the
 *             rnp_buffer_destroy().
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_symenc_get_s2k_type(rnp_symenc_handle_t symenc, char **type);

/**
 * @brief Get number of iterations in iterated-and-salted S2K, if it was used.
 *
 * @param symenc opaque handle, cannot be NULL.
 * @param iterations on success number of iterations will be stored here. Cannot be NULL.
 *                   If non-iterated s2k was used then will be set to 0.
 * @return RNP_SUCCESS if call succeeded, or error code otherwise.
 */
RNP_API rnp_result_t rnp_symenc_get_s2k_iterations(rnp_symenc_handle_t symenc,
                                                   uint32_t *          iterations);

/** @brief Free resources allocated in verification context.
 *  @param op opaque verification context. Must be initialized.
 *  @return RNP_SUCCESS if call succeeded.
 */
RNP_API rnp_result_t rnp_op_verify_destroy(rnp_op_verify_t op);

/** @brief Get signature verification status. To get more detailed signature information
 *         function rnp_op_verify_signature_get_handle() should be used.
 *  @param sig opaque signature context obtained via rnp_op_verify_get_signature_at call.
 *  @return signature verification status:
 *          RNP_SUCCESS : signature is valid
 *          RNP_ERROR_SIGNATURE_EXPIRED : signature is valid but expired
 *          RNP_ERROR_KEY_NOT_FOUND : public key to verify signature was not available
 *          RNP_ERROR_SIGNATURE_INVALID : data or signature was modified
 *          RNP_ERROR_SIGNATURE_UNKNOWN : signature has unknown format
 */
RNP_API rnp_result_t rnp_op_verify_signature_get_status(rnp_op_verify_signature_t sig);

/** Get the signature handle from the verified signature. This would allow to query extended
 * information on the signature.
 *
 * @param sig verified signature context, cannot be NULL.
 * @param handle signature handle will be stored here on success. You must free it after use
 *        with the rnp_signature_handle_destroy() function.
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_verify_signature_get_handle(rnp_op_verify_signature_t sig,
                                                        rnp_signature_handle_t *  handle);

/** @brief Get hash function used to calculate signature
 *  @param sig opaque signature context obtained via rnp_op_verify_get_signature_at call.
 *  @param hash pointer to string with hash algorithm name will be put here on success.
 *              Caller is responsible for freeing it with rnp_buffer_destroy
 *  @return RNP_SUCCESS or error code otherwise
 */
RNP_API rnp_result_t rnp_op_verify_signature_get_hash(rnp_op_verify_signature_t sig,
                                                      char **                   hash);

/** @brief Get key used for signing
 *  @param sig opaque signature context obtained via rnp_op_verify_get_signature_at call.
 *  @param key pointer to opaque key handle structure.
 *  @return RNP_SUCCESS or error code otherwise
 */
RNP_API rnp_result_t rnp_op_verify_signature_get_key(rnp_op_verify_signature_t sig,
                                                     rnp_key_handle_t *        key);

/** @brief Get signature creation and expiration times
 *  @param sig opaque signature context obtained via rnp_op_verify_get_signature_at call.
 *  @param create signature creation time will be put here. It is number of seconds since
 *                Jan, 1 1970 UTC. May be NULL if called doesn't need this data.
 *  @param expires signature expiration time will be stored here. It is number of seconds since
 *                 the creation time or 0 if signature never expires. May be NULL.
 *  @return RNP_SUCCESS or error code otherwise
 */
RNP_API rnp_result_t rnp_op_verify_signature_get_times(rnp_op_verify_signature_t sig,
                                                       uint32_t *                create,
                                                       uint32_t *                expires);

/**
 * @brief Free buffer allocated by a function in this header.
 *
 * @param ptr previously allocated buffer. May be NULL, then nothing is done.
 */
RNP_API void rnp_buffer_destroy(void *ptr);

/**
 * @brief Securely clear buffer contents.
 *
 * @param ptr pointer to the buffer contents, may be NULL.
 * @param size number of bytes in buffer.
 */
RNP_API void rnp_buffer_clear(void *ptr, size_t size);

/**
 * @brief Initialize input struct to read from a path
 *
 * @param input pointer to the input opaque structure
 * @param path path of the file to read from
 * @return RNP_SUCCESS if operation succeeded and input struct is ready to read, or error code
 *         otherwise
 */
RNP_API rnp_result_t rnp_input_from_path(rnp_input_t *input, const char *path);

/**
 * @brief Initialize input struct to read from the stdin
 *
 * @param input pointer to the input opaque structure
 * @return RNP_SUCCESS if operation succeeded and input struct is ready to read, or error code
 *         otherwise
 */
RNP_API rnp_result_t rnp_input_from_stdin(rnp_input_t *input);

/**
 * @brief Initialize input struct to read from memory
 *
 * @param input pointer to the input opaque structure
 * @param buf memory buffer. Could not be NULL.
 * @param buf_len number of bytes available to read from buf, cannot be zero.
 * @param do_copy if true then the buffer will be copied internally. If
 *        false then the application should ensure that the buffer
 *        is valid and not modified during the lifetime of this object.
 * @return RNP_SUCCESS if operation succeeded or error code otherwise
 */
RNP_API rnp_result_t rnp_input_from_memory(rnp_input_t * input,
                                           const uint8_t buf[],
                                           size_t        buf_len,
                                           bool          do_copy);

/**
 * @brief Initialize input struct to read via callbacks
 *
 * @param input pointer to the input opaque structure
 * @param reader callback used for reading
 * @param closer callback used to close the stream
 * @param app_ctx context to pass as parameter to reader and closer
 * @return RNP_SUCCESS if operation succeeded or error code otherwise
 */
RNP_API rnp_result_t rnp_input_from_callback(rnp_input_t *       input,
                                             rnp_input_reader_t *reader,
                                             rnp_input_closer_t *closer,
                                             void *              app_ctx);

/**
 * @brief Close previously opened input and free all corresponding resources
 *
 * @param input previously opened input structure
 * @return RNP_SUCCESS if operation succeeded or error code otherwise
 */
RNP_API rnp_result_t rnp_input_destroy(rnp_input_t input);

/**
 * @brief Initialize output structure to write to a path. If path is a file
 * that already exists then it will be overwritten.
 *
 * @param output pointer to the opaque output structure.
 * @param path path to the file.
 * @return RNP_SUCCESS if file was opened successfully and ready for writing or error code
 *         otherwise.
 */
RNP_API rnp_result_t rnp_output_to_path(rnp_output_t *output, const char *path);

/**
 * @brief Initialize structure to write to a file.
 *        Note: it doesn't allow output to directory like rnp_output_to_path does, but
 *        allows additional options to be specified.
 *        When RNP_OUTPUT_FILE_RANDOM flag is included then you may want to call
 *        rnp_output_finish() to make sure that final rename succeeded.
 * @param output pointer to the opaque output structure. After use you must free it using the
 *               rnp_output_destroy() function.
 * @param path path to the file.
 * @param flags additional flags, see RNP_OUTPUT_* flags.
 * @return RNP_SUCCESS if file was opened successfully and ready for writing or error code
 *         otherwise.
 */
RNP_API rnp_result_t rnp_output_to_file(rnp_output_t *output,
                                        const char *  path,
                                        uint32_t      flags);

/**
 * @brief Initialize structure to write to the stdout.
 *
 * @param output pointer to the opaque output structure. After use you must free it using the
 *               rnp_output_destroy() function.
 * @return RNP_SUCCESS if output was initialized successfully or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_to_stdout(rnp_output_t *output);

/**
 * @brief Initialize output structure to write to the memory.
 *
 * @param output pointer to the opaque output structure.
 * @param max_alloc maximum amount of memory to allocate. 0 value means unlimited.
 * @return RNP_SUCCESS if operation succeeded or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_to_memory(rnp_output_t *output, size_t max_alloc);

/**
 * @brief Output data to armored stream (and then output to other destination), allowing
 *        streamed output.
 *
 * @param base initialized output structure, where armored data will be written to.
 * @param output pointer to the opaque output structure. You must free it later using the
 *               rnp_output_destroy() function.
 * @param type type of the armored stream. See rnp_enarmor() for possible values.
 * @return RNP_SUCCESS if operation succeeded or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_to_armor(rnp_output_t  base,
                                         rnp_output_t *output,
                                         const char *  type);

/**
 * @brief Get the pointer to the buffer of output, initialized by rnp_output_to_memory
 *
 * @param output output structure, initialized by rnp_output_to_memory and populated with data
 * @param buf pointer to the buffer will be stored here, could not be NULL
 * @param len number of bytes in buffer will be stored here, could not be NULL
 * @param do_copy if true then a newly-allocated buffer will be returned and the application
 *        will be responsible for freeing it with rnp_buffer_destroy. If false
 *        then the internal buffer is returned and the application must not modify the
 *        buffer or access it after this object is destroyed.
 * @return RNP_SUCCESS if operation succeeded or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_memory_get_buf(rnp_output_t output,
                                               uint8_t **   buf,
                                               size_t *     len,
                                               bool         do_copy);

/**
 * @brief Initialize output structure to write to callbacks.
 *
 * @param output pointer to the opaque output structure.
 * @param writer write callback.
 * @param closer close callback.
 * @param app_ctx context parameter which will be passed to writer and closer.
 * @return RNP_SUCCESS if operation succeeded or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_to_callback(rnp_output_t *       output,
                                            rnp_output_writer_t *writer,
                                            rnp_output_closer_t *closer,
                                            void *               app_ctx);

/**
 * @brief Initialize output structure which will discard all data
 *
 * @param output pointer to the opaque output structure.
 * @return RNP_SUCCESS if operation succeeded or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_to_null(rnp_output_t *output);

/**
 * @brief write some data to the output structure.
 *
 * @param output pointer to the initialized opaque output structure.
 * @param data pointer to data which should be written.
 * @param size number of bytes to write.
 * @param written on success will contain the number of bytes written. May be NULL.
 * @return rnp_result_t RNP_SUCCESS if operation succeeded or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_write(rnp_output_t output,
                                      const void * data,
                                      size_t       size,
                                      size_t *     written);

/**
 * @brief Finish writing to the output.
 *        Note: on most output types you'll need just to call rnp_output_destroy().
 *        However, for file output with RNP_OUTPUT_FILE_RANDOM flag, you need to call this
 *        to make sure that rename from random to required name succeeded.
 *
 * @param output pointer to the opaque output structure.
 * @return RNP_SUCCESS if operation succeeded or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_finish(rnp_output_t output);

/**
 * @brief Close previously opened output and free all associated data.
 *
 * @param output previously opened output structure.
 * @return RNP_SUCCESS if operation succeeds or error code otherwise.
 */
RNP_API rnp_result_t rnp_output_destroy(rnp_output_t output);

/* encrypt */
RNP_API rnp_result_t rnp_op_encrypt_create(rnp_op_encrypt_t *op,
                                           rnp_ffi_t         ffi,
                                           rnp_input_t       input,
                                           rnp_output_t      output);

/**
 * @brief Add recipient's public key to encrypting context.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param key public key, used for encryption. Key is not checked for
 *        validity or expiration.
 * @return RNP_SUCCESS if operation succeeds or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_encrypt_add_recipient(rnp_op_encrypt_t op, rnp_key_handle_t key);

#if defined(RNP_EXPERIMENTAL_CRYPTO_REFRESH)
/**
 * @brief Enables the creation of PKESK v6 (instead of v3) which results in the use of SEIPDv2.
 *        The actually created version depends on the capabilities of the list of recipients.
 *        NOTE: This is an experimental feature and this function can be replaced (or removed)
 *        at any time.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @return RNP_SUCCESS or errorcode if failed.
 */
RNP_API rnp_result_t rnp_op_encrypt_enable_pkesk_v6(rnp_op_encrypt_t op);
#endif

#if defined(RNP_EXPERIMENTAL_PQC)
/**
 * @brief Prefer using PQC subkeys over non-PQC subkeys when encrypting.
 *        NOTE: This is an experimental feature and this function can be replaced (or removed)
 *        at any time.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @return RNP_SUCCESS or errorcode if failed.
 */
RNP_API rnp_result_t rnp_op_encrypt_prefer_pqc_enc_subkey(rnp_op_encrypt_t op);
#endif

/**
 * @brief Add signature to encrypting context, so data will be encrypted and signed.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param key private key, used for signing.
 * @param sig pointer to the newly added signature will be stored here. May be NULL.
 * @return RNP_SUCCESS if signature was added or error code otherwise.
 */
RNP_API rnp_result_t rnp_op_encrypt_add_signature(rnp_op_encrypt_t         op,
                                                  rnp_key_handle_t         key,
                                                  rnp_op_sign_signature_t *sig);

/**
 * @brief Set hash function used for signature calculation. Makes sense if encrypt-and-sign is
 *        used. To set hash function for each signature separately use
 *        rnp_op_sign_signature_set_hash.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param hash hash algorithm to be used as NULL-terminated string. Following values are
 *        supported: "MD5", "SHA1", "RIPEMD160", "SHA256", "SHA384", "SHA512", "SHA224", "SM3".
 *        However, some signature types may require specific hash function or hash function
 *        output length.
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_encrypt_set_hash(rnp_op_encrypt_t op, const char *hash);

/**
 * @brief Set signature creation time. By default current time is used.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param create creation time in seconds since Jan, 1 1970 UTC
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_encrypt_set_creation_time(rnp_op_encrypt_t op, uint32_t create);

/**
 * @brief Set signature expiration time. By default signatures do not expire.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param expire expiration time in seconds since the creation time. 0 value is used to mark
 *        signature as non-expiring
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_encrypt_set_expiration_time(rnp_op_encrypt_t op, uint32_t expire);

/**
 * @brief Add password which is used to encrypt data. Multiple passwords can be added.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param password NULL-terminated password string, or NULL if password should be requested
 *                 via password provider.
 * @param s2k_hash hash algorithm, used in key-from-password derivation. Pass NULL for default
 *        value. See rnp_op_encrypt_set_hash for possible values.
 * @param iterations number of iterations, used in key derivation function.
 *        According to RFC 4880, chapter 3.7.1.3, only 256 distinct values within the range
 *        [1024..0x3e00000] can be encoded. Thus, the number will be increased to the closest
 *        encodable value. In case it exceeds the maximum encodable value, it will be decreased
 *        to the maximum encodable value.
 *        If 0 is passed, an optimal number (greater or equal to 1024) will be calculated based
 *        on performance measurement.
 * @param s2k_cipher symmetric cipher, used for key encryption. Pass NULL for default value.
 * See rnp_op_encrypt_set_cipher for possible values.
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_encrypt_add_password(rnp_op_encrypt_t op,
                                                 const char *     password,
                                                 const char *     s2k_hash,
                                                 size_t           iterations,
                                                 const char *     s2k_cipher);

/**
 * @brief Set whether output should be ASCII-armored, or binary.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param armored true for armored, false for binary
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_encrypt_set_armor(rnp_op_encrypt_t op, bool armored);

/**
 * @brief set the encryption algorithm
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param cipher NULL-terminated string with cipher's name. One of the "IDEA", "TRIPLEDES",
 *        "CAST5", "BLOWFISH", "AES128", "AES192", "AES256", "TWOFISH", "CAMELLIA128",
 *        "CAMELLIA192", "CAMELLIA256", "SM4".
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_encrypt_set_cipher(rnp_op_encrypt_t op, const char *cipher);

/**
 * @brief set AEAD mode algorithm or disable AEAD usage. By default it is disabled.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param alg NULL-terminated AEAD algorithm name. Use "None" to disable AEAD, or "OCB"
 *            to use AEAD-OCB authenticated encryption.
 *            Note: there is "EAX" mode which is deprecated and should not be used.
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_encrypt_set_aead(rnp_op_encrypt_t op, const char *alg);

/**
 * @brief set chunk length for AEAD mode via number of chunk size bits (refer to the OpenPGP
 *        specification for the details).
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param bits number of bits, currently it must be from 0 to 16.
 * @return RNP_SUCCESS or error code if failed
 */
RNP_API rnp_result_t rnp_op_encrypt_set_aead_bits(rnp_op_encrypt_t op, int bits);

/**
 * @brief set the compression algorithm and level for the inner raw data
 *
 * @param op opaque encrypted context. Must be allocated and initialized
 * @param compression compression algorithm name. Can be one of the "Uncompressed", "ZIP",
 *        "ZLIB", "BZip2". Please note that ZIP is not PkWare's ZIP file format but just a
 *        DEFLATE compressed data (RFC 1951).
 * @param level 0 - 9, where 0 is no compression and 9 is maximum compression level.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_op_encrypt_set_compression(rnp_op_encrypt_t op,
                                                    const char *     compression,
                                                    int              level);

/**
 * @brief Set additional encryption flags.
 *
 * @param op opaque encrypting context. Must be allocated and initialized.
 * @param flags encryption flags. ORed combination of RNP_ENCRYPT_* values.
 *              Following flags are supported:
 *              RNP_ENCRYPT_NOWRAP - do not wrap the data in a literal data packet. This
 *              would allow to encrypt already signed data.
 *
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_op_encrypt_set_flags(rnp_op_encrypt_t op, uint32_t flags);

/**
 * @brief set the internally stored file name for the data being encrypted
 *
 * @param op opaque encrypted context. Must be allocated and initialized
 * @param filename file name as NULL-terminated string. May be empty string. Value "_CONSOLE"
 *                 may have specific processing (see RFC 4880 for the details), depending on
 *                 implementation.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_op_encrypt_set_file_name(rnp_op_encrypt_t op, const char *filename);

/**
 * @brief set the internally stored file modification date for the data being encrypted
 *
 * @param op opaque encrypted context. Must be allocated and initialized
 * @param mtime time in seconds since Jan, 1 1970. 32 bit unsigned integer datatype is used
 *              here instead of 64 bit (like modern timestamps do) because in OpenPGP messages
 *              times are stored as 32-bit unsigned integers.
 * @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_op_encrypt_set_file_mtime(rnp_op_encrypt_t op, uint32_t mtime);

RNP_API rnp_result_t rnp_op_encrypt_execute(rnp_op_encrypt_t op);
RNP_API rnp_result_t rnp_op_encrypt_destroy(rnp_op_encrypt_t op);

/**
 * @brief Decrypt encrypted data in input and write it to the output on success.
 *        If data is additionally signed then signatures are ignored.
 *        For more control over the decryption process see functions rnp_op_verify_create() and
 *        rnp_op_verify_execute(), which allows to verify signatures as well as decrypt data
 *        and retrieve encryption-related information.
 *
 * @param ffi initialized FFI object. Cannot be NULL.
 * @param input source with encrypted data. Cannot be NULL.
 * @param output on success decrypted data will be written here. Cannot be NULL.
 * @return RNP_SUCCESS if data was successfully decrypted and written to the output, or any
 *         other value on error.
 */
RNP_API rnp_result_t rnp_decrypt(rnp_ffi_t ffi, rnp_input_t input, rnp_output_t output);

/**
 *  @brief retrieve the raw data for a public key
 *
 *  This will always be PGP packets and will never include ASCII armor.
 *
 *  @param handle the key handle
 *  @param buf
 *  @param buf_len
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_get_public_key_data(rnp_key_handle_t handle,
                                             uint8_t **       buf,
                                             size_t *         buf_len);

/**
 *  @brief retrieve the raw data for a secret key
 *
 *  If this is a G10 key, this will be the s-expr data. Otherwise, it will
 *  be PGP packets.
 *
 *  Note that this result will never include ASCII armor.
 *
 *  @param handle the key handle
 *  @param buf
 *  @param buf_len
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_get_secret_key_data(rnp_key_handle_t handle,
                                             uint8_t **       buf,
                                             size_t *         buf_len);

/** output key information to JSON structure and serialize it to the string
 *
 * @param handle the key handle, could not be NULL
 * @param flags controls which key data is printed, see RNP_JSON_* constants.
 * @param result pointer to the resulting string will be stored here on success. You must
 *               release it afterwards via rnp_buffer_destroy() function call.\n
 *               JSON output will be a JSON object that contains the following members:\n
 * JSON member     | Description
 * ----------------|------------
 *  "type"         | string, key algorithm, see rnp_op_generate_create()
 *  "length"       | integer, key size in bits, see rnp_op_generate_set_bits()
 *  "curve"        | string, curve name, see rnp_op_generate_set_curve()
 *  "keyid"        | string, hexadecimal PGP key id
 *  "fingerprint"  | string, hexadecimal PGP key fingerprint
 *  "grip"         | string, hexadecimal PGP key grip
 *  "revoked"      | boolean, true if key is revoked
 *  "creation time"| integer, creation time in seconds since Jan, 1 1970 UTC
 *  "expiration"   | integer, see rnp_op_generate_set_expiration()
 *  "usage"        | array of strings, see rnp_op_generate_add_usage()
 *  "subkey grips" | array of strings, hexadecimal PGP key grips of subkeys
 *  "public key"   | object, describes public key, see description below
 *  "secret key"   | object, describes secret key, see description below
 *  "userids"      | array of strings, user ID-s
 *  "signatures"   | array of objects, each object represents a signature
 *               "public key" object can contain the following members:
 * JSON member     | Description
 * ----------------|------------
 *  "present"      | boolean, true of public key is present
 *  "mpis"         | object, contains MPI-s of the key
 *               "secret key" object can contain the following members:
 * JSON member     | Description
 * ----------------|------------
 *  "present"      | boolean, true of secret key is present
 *  "mpis"         | object, contains MPI-s of the key, can be null
 *  "locked"       | boolean, true if the key is locked
 *  "protected"    | boolean, true if the secret key is protected
 *               The "signatures" member is present only if the flag RNP_JSON_SIGNATURES
 *               is set. Each signature object can contain the following members:
 * JSON member     | Description
 * ----------------|------------
 *  "userid"       | integer, index of user id, primary key only
 *  "trust"        | object, trust level, see description below
 *  "usage"        | array of strings, see rnp_op_generate_add_usage()
 *  "preferences"  | object, see description of "preferences" in rnp_generate_key_json()
 *  "version"      | integer, version
 *  "type"         | string, signature type (textual)
 *  "key type"     | string, key algorithm used for signature
 *  "hash"         | string, hash algorithm used for signature
 *  "creation time"| integer, creation time in seconds since Jan, 1 1970 UTC
 *  "expiration"   | integer, see rnp_op_generate_set_expiration()
 *  "signer"       | object, describes signing key
 *  "mpis"         | object, MPI-s of the signature
 *               The "trust" object member in signature object contains two members:
 * JSON member     | Description
 * ----------------|------------
 *  "level"        | integer, trust level
 *  "amount"       | integer, trust amount. See OpenPGP RFC for details.
 *               The format of the "mpis" object in the "signatures", "public key" and
 *               "secret key" members may vary and depends on the key algorithm.
 *               But generally they contain hexadecimal strings representing
 *               MPI-s (multi-precision integers) of the key or signature.\n
 *               "mpis" objects are present if the flags argument contains
 *               RNP_JSON_SIGNATURE_MPIS,RNP_JSON_PUBLIC_MPIS and RNP_JSON_SECRET_MPIS
 *               flag respectively.\n
 *               Example of the JSON output string:\n
 *
 *                   {
 *                    "type":"ECDSA",
 *                    "length":256,
 *                    "curve":"NIST P-256",
 *                    "keyid":"014F7B24CD14F2A5",
 *                    "fingerprint":"9034431D2F803D20F9840833014F7B24CD14F2A5",
 *                    "grip":"B5331B92954B51C72904B97527EC85BEC4FF3154",
 *                    "revoked":false,
 *                    "creation time":1683104807,
 *                    "expiration":0,
 *                    "usage":[
 *                      "sign"
 *                    ],
 *                    "subkey grips":[
 *                      "E50D9738D779A587425248D3483DB8E1805B0174"
 *                    ],
 *                    "public key":{
 *                      "present":true
 *                    },
 *                    "secret key":{
 *                      "present":true,
 *                      "locked":true,
 *                      "protected":true
 *                    },
 *                    "userids":[
 *                      "test0"
 *                    ]
 *                  }
 *
 *
 * @return RNP_SUCCESS or error code if failed.
 */
RNP_API rnp_result_t rnp_key_to_json(rnp_key_handle_t handle, uint32_t flags, char **result);

/** create an identifier iterator
 *
 *  @param ffi
 *  @param it pointer that will be set to the created iterator
 *  @param identifier_type the type of identifier ("userid", "keyid", "grip", "fingerprint")
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_identifier_iterator_create(rnp_ffi_t                  ffi,
                                                    rnp_identifier_iterator_t *it,
                                                    const char *identifier_type);

/** retrieve the next item from an iterator
 *
 *  @param it the iterator
 *  @param identifier pointer that will be set to the identifier value.
 *         Must not be NULL. This buffer should not be freed by the application.
 *         It will be modified by subsequent calls to this function, and its
 *         life is tied to the iterator.
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_identifier_iterator_next(rnp_identifier_iterator_t it,
                                                  const char **             identifier);

/** destroy an identifier iterator
 *
 *  @param it the iterator object
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_identifier_iterator_destroy(rnp_identifier_iterator_t it);

/** Read from input and write to output
 *
 *  @param input stream to read data from
 *  @param output stream to write data to
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_output_pipe(rnp_input_t input, rnp_output_t output);

/** Set line length for armored output
 *
 *  @param output stream to configure
 *  @param llen line length in characters [16..76]
 *  @return RNP_SUCCESS on success, or any other value on error
 */
RNP_API rnp_result_t rnp_output_armor_set_line_length(rnp_output_t output, size_t llen);

/**
 * @brief Return cryptographic backend library name.
 *
 * @return Backend name string. Currently supported
 * backends are "Botan" and "OpenSSL".
 */
RNP_API const char *rnp_backend_string();

/**
 * @brief Return cryptographic backend library version.
 *
 * @return Version string.
 */
RNP_API const char *rnp_backend_version();

#if defined(__cplusplus)
}

#endif

/**
 * Feature strings.
 */
#ifndef RNP_FEATURE_SYMM_ALG

#define RNP_FEATURE_SYMM_ALG "symmetric algorithm"
#define RNP_FEATURE_AEAD_ALG "aead algorithm"
#define RNP_FEATURE_PROT_MODE "protection mode"
#define RNP_FEATURE_PK_ALG "public key algorithm"
#define RNP_FEATURE_HASH_ALG "hash algorithm"
#define RNP_FEATURE_COMP_ALG "compression algorithm"
#define RNP_FEATURE_CURVE "elliptic curve"

#endif

/**
 * Certification signature type strings.
 */
#define RNP_CERTIFICATION_GENERIC "generic"
#define RNP_CERTIFICATION_PERSONA "persona"
#define RNP_CERTIFICATION_CASUAL "casual"
#define RNP_CERTIFICATION_POSITIVE "positive"

/** Algorithm Strings
 */
#ifndef RNP_ALGNAME_PLAINTEXT

#define RNP_ALGNAME_PLAINTEXT "PLAINTEXT"
#define RNP_ALGNAME_RSA "RSA"
#define RNP_ALGNAME_ELGAMAL "ELGAMAL"
#define RNP_ALGNAME_DSA "DSA"
#define RNP_ALGNAME_ECDH "ECDH"
#define RNP_ALGNAME_ECDSA "ECDSA"
#define RNP_ALGNAME_EDDSA "EDDSA"
#if defined(RNP_EXPERIMENTAL_CRYPTO_REFRESH) || defined(RNP_EXPERIMENTAL_PQC)
#define RNP_ALGNAME_ED25519 "ED25519"
#define RNP_ALGNAME_X25519 "X25519"
#endif
#if defined(RNP_EXPERIMENTAL_PQC)
#define RNP_ALGNAME_KYBER768_X25519 "ML-KEM-768+X25519"
#define RNP_ALGNAME_KYBER1024_X448 "ML-KEM-1024+X448"
#define RNP_ALGNAME_KYBER768_P256 "ML-KEM-768+ECDH-P256"
#define RNP_ALGNAME_KYBER1024_P384 "ML-KEM-1024+ECDH-P384"
#define RNP_ALGNAME_KYBER768_BP256 "ML-KEM-768+ECDH-BP256"
#define RNP_ALGNAME_KYBER1024_BP384 "ML-KEM-1024+ECDH-BP384"
#define RNP_ALGNAME_DILITHIUM3_ED25519 "ML-DSA-65+ED25519"
#define RNP_ALGNAME_DILITHIUM5_ED448 "ML-DSA-87+ED448"
#define RNP_ALGNAME_DILITHIUM3_P256 "ML-DSA-65+ECDSA-P256"
#define RNP_ALGNAME_DILITHIUM5_P384 "ML-DSA-87+ECDSA-P384"
#define RNP_ALGNAME_DILITHIUM3_BP256 "ML-DSA-65+ECDSA-BP256"
#define RNP_ALGNAME_DILITHIUM5_BP384 "ML-DSA-87+ECDSA-BP384"
#define RNP_ALGNAME_SPHINCSPLUS_SHA2 "SLH-DSA-SHA2"
#define RNP_ALGNAME_SPHINCSPLUS_SHAKE "SLH-DSA-SHAKE"
#endif
#define RNP_ALGNAME_IDEA "IDEA"
#define RNP_ALGNAME_TRIPLEDES "TRIPLEDES"
#define RNP_ALGNAME_CAST5 "CAST5"
#define RNP_ALGNAME_BLOWFISH "BLOWFISH"
#define RNP_ALGNAME_TWOFISH "TWOFISH"
#define RNP_ALGNAME_AES_128 "AES128"
#define RNP_ALGNAME_AES_192 "AES192"
#define RNP_ALGNAME_AES_256 "AES256"
#define RNP_ALGNAME_CAMELLIA_128 "CAMELLIA128"
#define RNP_ALGNAME_CAMELLIA_192 "CAMELLIA192"
#define RNP_ALGNAME_CAMELLIA_256 "CAMELLIA256"
#define RNP_ALGNAME_SM2 "SM2"
#define RNP_ALGNAME_SM3 "SM3"
#define RNP_ALGNAME_SM4 "SM4"
#define RNP_ALGNAME_MD5 "MD5"
#define RNP_ALGNAME_SHA1 "SHA1"
#define RNP_ALGNAME_SHA256 "SHA256"
#define RNP_ALGNAME_SHA384 "SHA384"
#define RNP_ALGNAME_SHA512 "SHA512"
#define RNP_ALGNAME_SHA224 "SHA224"
#define RNP_ALGNAME_SHA3_256 "SHA3-256"
#define RNP_ALGNAME_SHA3_512 "SHA3-512"
#define RNP_ALGNAME_RIPEMD160 "RIPEMD160"
#define RNP_ALGNAME_CRC24 "CRC24"
#define RNP_ALGNAME_ZLIB "ZLib"
#define RNP_ALGNAME_BZIP2 "BZip2"
#define RNP_ALGNAME_ZIP "ZIP"

/* SHA1 is not considered secured anymore and SHOULD NOT be used to create messages (as per
 * Appendix C of RFC 4880-bis-02). SHA2 MUST be implemented.
 * Let's preempt this by specifying SHA256 - gpg interoperates just fine with SHA256 - agc,
 * 20090522
 */
#define DEFAULT_HASH_ALG RNP_ALGNAME_SHA256

/* Default symmetric algorithm */
#define DEFAULT_SYMM_ALG RNP_ALGNAME_AES_256

/* Keystore format: GPG, KBX (pub), G10 (sec), GPG21 ( KBX for pub, G10 for sec) */
#define RNP_KEYSTORE_GPG ("GPG")
#define RNP_KEYSTORE_KBX ("KBX")
#define RNP_KEYSTORE_G10 ("G10")
#define RNP_KEYSTORE_GPG21 ("GPG21")

#endif
