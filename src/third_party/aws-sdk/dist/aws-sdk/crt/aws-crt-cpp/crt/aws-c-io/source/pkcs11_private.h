#ifndef AWS_IO_PKCS11_PRIVATE_H
#define AWS_IO_PKCS11_PRIVATE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/tls_channel_handler.h>

#include "pkcs11/v2.40/pkcs11.h"

/**
 * pkcs11_private.h
 * This file declares symbols that are private to aws-c-io but need to be
 * accessed from multiple .c files.
 *
 * NOTE: Not putting this file under `include/private/...` like we usually
 * do with private headers because it breaks aws-crt-swift. Swift was trying
 * to compile each file under include/, but the official PKCS#11 header files
 * are too weird break it.
 */

struct aws_pkcs11_lib;
struct aws_pkcs11_tls_key_handler;
struct aws_string;

AWS_EXTERN_C_BEGIN

/**
 * Return c-string for PKCS#11 CKR_* constant.
 * For use in tests only.
 */
AWS_IO_API
const char *aws_pkcs11_ckr_str(CK_RV rv);

/**
 * Return the raw function list.
 * For use in tests only.
 */
AWS_IO_API
CK_FUNCTION_LIST *aws_pkcs11_lib_get_function_list(struct aws_pkcs11_lib *pkcs11_lib);

/**
 * Find the slot that meets all criteria:
 * - has a token
 * - if match_slot_id is non-null, then slot IDs must match
 * - if match_token_label is non-null, then labels must match
 * The function fails unless it finds exactly one slot meeting all criteria.
 */
AWS_IO_API
int aws_pkcs11_lib_find_slot_with_token(
    struct aws_pkcs11_lib *pkcs11_lib,
    const uint64_t *match_slot_id,
    const struct aws_string *match_token_label,
    CK_SLOT_ID *out_slot_id);

AWS_IO_API
int aws_pkcs11_lib_open_session(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SLOT_ID slot_id,
    CK_SESSION_HANDLE *out_session_handle);

AWS_IO_API
void aws_pkcs11_lib_close_session(struct aws_pkcs11_lib *pkcs11_lib, CK_SESSION_HANDLE session_handle);

AWS_IO_API
int aws_pkcs11_lib_login_user(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    const struct aws_string *optional_user_pin);

/**
 * Find the object that meets all criteria:
 * - is private key
 * - if match_label is non-null, then labels must match
 * The function fails unless it finds exactly one object meeting all criteria.
 */
AWS_IO_API
int aws_pkcs11_lib_find_private_key(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    const struct aws_string *match_label,
    CK_OBJECT_HANDLE *out_key_handle,
    CK_KEY_TYPE *out_key_type);

/**
 * Decrypt the encrypted data.
 * out_data should be passed in uninitialized.
 * If successful, out_data will be initialized and contain the recovered data.
 */
AWS_IO_API
int aws_pkcs11_lib_decrypt(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    CK_OBJECT_HANDLE key_handle,
    CK_KEY_TYPE key_type,
    struct aws_byte_cursor encrypted_data,
    struct aws_allocator *allocator,
    struct aws_byte_buf *out_data);

/**
 * Sign a digest with the private key during TLS negotiation.
 * out_signature should be passed in uninitialized.
 * If successful, out_signature will be initialized and contain the signature.
 */
AWS_IO_API
int aws_pkcs11_lib_sign(
    struct aws_pkcs11_lib *pkcs11_lib,
    CK_SESSION_HANDLE session_handle,
    CK_OBJECT_HANDLE key_handle,
    CK_KEY_TYPE key_type,
    struct aws_byte_cursor digest_data,
    struct aws_allocator *allocator,
    enum aws_tls_hash_algorithm digest_alg,
    enum aws_tls_signature_algorithm signature_alg,
    struct aws_byte_buf *out_signature);

/**
 * Get the DER encoded DigestInfo value to be prefixed to the hash, used for RSA signing
 * See https://tools.ietf.org/html/rfc3447#page-43
 */
AWS_IO_API
int aws_get_prefix_to_rsa_sig(enum aws_tls_hash_algorithm digest_alg, struct aws_byte_cursor *out_prefix);

/**
 * ASN.1 DER encode a big unsigned integer. Note that the source integer may be zero padded. It may also have
 * most significant bit set. The encoded format is canonical and unambiguous - that is, most significant
 * bit is never set.
 */
AWS_IO_API
int aws_pkcs11_asn1_enc_ubigint(struct aws_byte_buf *const buffer, struct aws_byte_cursor bigint);

/**
 * Creates a new PKCS11 TLS operation handler with an associated aws_custom_key_op_handler
 * with a reference count set to 1.
 *
 * The PKCS11 TLS operation handler will automatically be destroyed when the reference count reaches zero
 * on the aws_custom_key_op_handler.
 */
AWS_IO_API
struct aws_custom_key_op_handler *aws_pkcs11_tls_op_handler_new(
    struct aws_allocator *allocator,
    struct aws_pkcs11_lib *pkcs11_lib,
    const struct aws_byte_cursor *user_pin,
    const struct aws_byte_cursor *match_token_label,
    const struct aws_byte_cursor *match_private_key_label,
    const uint64_t *match_slot_id);

AWS_EXTERN_C_END
#endif /* AWS_IO_PKCS11_PRIVATE_H */
