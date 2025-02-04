#ifndef AWS_CAL_ECC_H
#define AWS_CAL_ECC_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/exports.h>

#include <aws/common/atomics.h>
#include <aws/common/byte_buf.h>
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_ecc_curve_name {
    AWS_CAL_ECDSA_P256,
    AWS_CAL_ECDSA_P384,
};

struct aws_ecc_key_pair;

typedef void aws_ecc_key_pair_destroy_fn(struct aws_ecc_key_pair *key_pair);
typedef int aws_ecc_key_pair_sign_message_fn(
    const struct aws_ecc_key_pair *key_pair,
    const struct aws_byte_cursor *message,
    struct aws_byte_buf *signature_output);
typedef int aws_ecc_key_pair_derive_public_key_fn(struct aws_ecc_key_pair *key_pair);
typedef int aws_ecc_key_pair_verify_signature_fn(
    const struct aws_ecc_key_pair *signer,
    const struct aws_byte_cursor *message,
    const struct aws_byte_cursor *signature);
typedef size_t aws_ecc_key_pair_signature_length_fn(const struct aws_ecc_key_pair *signer);

struct aws_ecc_key_pair_vtable {
    aws_ecc_key_pair_destroy_fn *destroy;
    aws_ecc_key_pair_derive_public_key_fn *derive_pub_key;
    aws_ecc_key_pair_sign_message_fn *sign_message;
    aws_ecc_key_pair_verify_signature_fn *verify_signature;
    aws_ecc_key_pair_signature_length_fn *signature_length;
};

struct aws_ecc_key_pair {
    struct aws_allocator *allocator;
    struct aws_atomic_var ref_count;
    enum aws_ecc_curve_name curve_name;
    struct aws_byte_buf key_buf;
    struct aws_byte_buf pub_x;
    struct aws_byte_buf pub_y;
    struct aws_byte_buf priv_d;
    struct aws_ecc_key_pair_vtable *vtable;
    void *impl;
};

AWS_EXTERN_C_BEGIN

/**
 * Adds one to an ecc key pair's ref count.
 */
AWS_CAL_API void aws_ecc_key_pair_acquire(struct aws_ecc_key_pair *key_pair);

/**
 * Subtracts one from an ecc key pair's ref count.  If ref count reaches zero, the key pair is destroyed.
 */
AWS_CAL_API void aws_ecc_key_pair_release(struct aws_ecc_key_pair *key_pair);

/**
 * Creates an Elliptic Curve private key that can be used for signing.
 * Returns a new instance of aws_ecc_key_pair if the key was successfully built.
 * Otherwise returns NULL. Note: priv_key::len must match the appropriate length
 * for the selected curve_name.
 */
AWS_CAL_API struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_private_key(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *priv_key);

#if !defined(AWS_OS_IOS)
/**
 * Creates an Elliptic Curve public/private key pair that can be used for signing and verifying.
 * Returns a new instance of aws_ecc_key_pair if the key was successfully built.
 * Otherwise returns NULL.
 * Note: On Apple platforms this function is only supported on MacOS. This is
 * due to usage of SecItemExport, which is only available on MacOS 10.7+
 * (yes, MacOS only and no other Apple platforms). There are alternatives for
 * ios and other platforms, but they are ugly to use. Hence for now it only
 * supports this call on MacOS.
 */
AWS_CAL_API struct aws_ecc_key_pair *aws_ecc_key_pair_new_generate_random(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name);
#endif /* !AWS_OS_IOS */

/**
 * Creates an Elliptic Curve public key that can be used for verifying.
 * Returns a new instance of aws_ecc_key_pair if the key was successfully built.
 * Otherwise returns NULL. Note: public_key_x::len and public_key_y::len must
 * match the appropriate length for the selected curve_name.
 */
AWS_CAL_API struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_public_key(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    const struct aws_byte_cursor *public_key_x,
    const struct aws_byte_cursor *public_key_y);

/**
 * Creates an Elliptic Curve public/private key pair from a DER encoded key pair.
 * Returns a new instance of aws_ecc_key_pair if the key was successfully built.
 * Otherwise returns NULL. Whether or not signing or verification can be perform depends
 * on if encoded_keys is a public/private pair or a public key.
 */
AWS_CAL_API struct aws_ecc_key_pair *aws_ecc_key_pair_new_from_asn1(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *encoded_keys);

/**
 * Creates an Elliptic curve public key from x and y coordinates encoded as hex strings
 * Returns a new instance of aws_ecc_key_pair if the key was successfully built.
 * Otherwise returns NULL.
 */
AWS_CAL_API struct aws_ecc_key_pair *aws_ecc_key_new_from_hex_coordinates(
    struct aws_allocator *allocator,
    enum aws_ecc_curve_name curve_name,
    struct aws_byte_cursor pub_x_hex_cursor,
    struct aws_byte_cursor pub_y_hex_cursor);

/**
 * Derives a public key from the private key if supported by this operating system (not supported on OSX).
 * key_pair::pub_x and key_pair::pub_y will be set with the raw key buffers.
 */
AWS_CAL_API int aws_ecc_key_pair_derive_public_key(struct aws_ecc_key_pair *key_pair);

/**
 * Get the curve name from the oid. OID here is the payload of the DER encoded ASN.1 part (doesn't include
 * type specifier or length. On success, the value of curve_name will be set.
 */
AWS_CAL_API int aws_ecc_curve_name_from_oid(struct aws_byte_cursor *oid, enum aws_ecc_curve_name *curve_name);

/**
 * Get the DER encoded OID from the curve_name. The OID in this case will not contain the type or the length specifier.
 */
AWS_CAL_API int aws_ecc_oid_from_curve_name(enum aws_ecc_curve_name curve_name, struct aws_byte_cursor *oid);

/**
 * Uses the key_pair's private key to sign message. The output will be in signature. Signature must be large enough
 * to hold the signature. Check aws_ecc_key_pair_signature_length() for the appropriate size. Signature will be DER
 * encoded.
 *
 * It is the callers job to make sure message is the appropriate cryptographic digest for this operation. It's usually
 * something like a SHA256.
 */
AWS_CAL_API int aws_ecc_key_pair_sign_message(
    const struct aws_ecc_key_pair *key_pair,
    const struct aws_byte_cursor *message,
    struct aws_byte_buf *signature);

/**
 * Uses the key_pair's public key to verify signature of message. Signature should be DER
 * encoded.
 *
 * It is the callers job to make sure message is the appropriate cryptographic digest for this operation. It's usually
 * something like a SHA256.
 *
 * returns AWS_OP_SUCCESS if the signature is valid.
 */
AWS_CAL_API int aws_ecc_key_pair_verify_signature(
    const struct aws_ecc_key_pair *key_pair,
    const struct aws_byte_cursor *message,
    const struct aws_byte_cursor *signature);
AWS_CAL_API size_t aws_ecc_key_pair_signature_length(const struct aws_ecc_key_pair *key_pair);

AWS_CAL_API void aws_ecc_key_pair_get_public_key(
    const struct aws_ecc_key_pair *key_pair,
    struct aws_byte_cursor *pub_x,
    struct aws_byte_cursor *pub_y);

AWS_CAL_API void aws_ecc_key_pair_get_private_key(
    const struct aws_ecc_key_pair *key_pair,
    struct aws_byte_cursor *private_d);

AWS_CAL_API size_t aws_ecc_key_coordinate_byte_size_from_curve_name(enum aws_ecc_curve_name curve_name);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_CAL_ECC_H */
