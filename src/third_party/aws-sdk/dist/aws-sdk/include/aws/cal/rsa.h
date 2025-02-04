#ifndef AWS_CAL_RSA_H
#define AWS_CAL_RSA_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/cal.h>
#include <aws/common/byte_buf.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_rsa_key_pair;

enum aws_rsa_encryption_algorithm {
    AWS_CAL_RSA_ENCRYPTION_PKCS1_5,
    AWS_CAL_RSA_ENCRYPTION_OAEP_SHA256,
    AWS_CAL_RSA_ENCRYPTION_OAEP_SHA512,
};

enum aws_rsa_signature_algorithm {
    AWS_CAL_RSA_SIGNATURE_PKCS1_5_SHA256,
    AWS_CAL_RSA_SIGNATURE_PKCS1_5_SHA1,
    AWS_CAL_RSA_SIGNATURE_PSS_SHA256,
};

/*
 * Note: prefer using standard key sizes - 1024, 2048, 4096.
 * Other key sizes will work, but which key sizes are supported may vary by
 * platform. Typically, multiples of 64 should work on all platforms.
 */
enum {
    AWS_CAL_RSA_MIN_SUPPORTED_KEY_SIZE_IN_BITS = 1024,
    AWS_CAL_RSA_MAX_SUPPORTED_KEY_SIZE_IN_BITS = 4096,
};

AWS_EXTERN_C_BEGIN

/**
 * Creates an RSA public key from RSAPublicKey as defined in rfc 8017 (aka PKCS1).
 * Returns a new instance of aws_rsa_key_pair if the key was successfully built.
 * Otherwise returns NULL.
 */
AWS_CAL_API struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_public_key_pkcs1(
    struct aws_allocator *allocator,
    struct aws_byte_cursor key);

/**
 * Creates an RSA private key from RSAPrivateKey as defined in rfc 8017 (aka PKCS1).
 * Returns a new instance of aws_rsa_key_pair if the key was successfully built.
 * Otherwise returns NULL.
 */
AWS_CAL_API struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_private_key_pkcs1(
    struct aws_allocator *allocator,
    struct aws_byte_cursor key);

/**
 * Adds one to an RSA key pair's ref count.
 * Returns key_pair pointer.
 */
AWS_CAL_API struct aws_rsa_key_pair *aws_rsa_key_pair_acquire(struct aws_rsa_key_pair *key_pair);

/**
 * Subtracts one from an RSA key pair's ref count. If ref count reaches zero, the key pair is destroyed.
 * Always returns NULL.
 */
AWS_CAL_API struct aws_rsa_key_pair *aws_rsa_key_pair_release(struct aws_rsa_key_pair *key_pair);

/**
 * Max plaintext size that can be encrypted by the key (i.e. max data size
 * supported by the key - bytes needed for padding).
 */
AWS_CAL_API size_t aws_rsa_key_pair_max_encrypt_plaintext_size(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_encryption_algorithm algorithm);

/*
 * Uses the key_pair's private key to encrypt the plaintext. The output will be
 * in out. out must be large enough to to hold the ciphertext. Check
 * aws_rsa_key_pair_block_length() for output upper bound.
 */
AWS_CAL_API int aws_rsa_key_pair_encrypt(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_encryption_algorithm algorithm,
    struct aws_byte_cursor plaintext,
    struct aws_byte_buf *out);

/*
 * Uses the key_pair's private key to decrypt the ciphertext. The output will be
 * in out. out must be large enough to to hold the ciphertext. Check
 * aws_rsa_key_pair_block_length() for output upper bound.
 */
AWS_CAL_API int aws_rsa_key_pair_decrypt(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_encryption_algorithm algorithm,
    struct aws_byte_cursor ciphertext,
    struct aws_byte_buf *out);

/*
 * Max size for a block supported by a given key pair.
 */
AWS_CAL_API size_t aws_rsa_key_pair_block_length(const struct aws_rsa_key_pair *key_pair);

/**
 * Uses the key_pair's private key to sign message. The output will be in out. out must be large enough
 * to hold the signature. Check aws_rsa_key_pair_signature_length() for the appropriate size.
 *
 * It is the callers job to make sure message is the appropriate cryptographic digest for this operation. It's usually
 * something like a SHA256.
 */
AWS_CAL_API int aws_rsa_key_pair_sign_message(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_signature_algorithm algorithm,
    struct aws_byte_cursor digest,
    struct aws_byte_buf *out);

/**
 * Uses the key_pair's public key to verify signature of message.
 *
 * It is the callers job to make sure message is the appropriate cryptographic digest for this operation. It's usually
 * something like a SHA256.
 *
 * returns AWS_OP_SUCCESS if the signature is valid.
 * raises AWS_ERROR_CAL_SIGNATURE_VALIDATION_FAILED if signature validation failed
 */
AWS_CAL_API int aws_rsa_key_pair_verify_signature(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_signature_algorithm algorithm,
    struct aws_byte_cursor digest,
    struct aws_byte_cursor signature);

/*
 * Max size for a signature supported by a given key pair.
 */
AWS_CAL_API size_t aws_rsa_key_pair_signature_length(const struct aws_rsa_key_pair *key_pair);

enum aws_rsa_key_export_format {
    AWS_CAL_RSA_KEY_EXPORT_PKCS1,
};

/*
 * Get public key for the key pair.
 * Inits out to a copy of key.
 * Any encoding on top of that (ex. b64) is left up to user.
 * Note: this function is currently not supported on windows for generated keys.
 */
AWS_CAL_API int aws_rsa_key_pair_get_public_key(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_key_export_format format,
    struct aws_byte_buf *out);

/*
 * Get private key for the key pair.
 * Inits out to a copy of key.
 * Any encoding on top of that (ex. b64) is left up to user.
 * Note: this function is currently not supported on Windows for generated keys.
 */
AWS_CAL_API int aws_rsa_key_pair_get_private_key(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_key_export_format format,
    struct aws_byte_buf *out);

AWS_EXTERN_C_END

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_CAL_RSA_H */
