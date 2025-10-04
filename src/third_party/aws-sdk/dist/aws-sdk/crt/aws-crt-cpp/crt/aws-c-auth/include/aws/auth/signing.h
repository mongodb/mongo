#ifndef AWS_AUTH_SIGNER_H
#define AWS_AUTH_SIGNER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/auth.h>

#include <aws/auth/signing_config.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_ecc_key_pair;
struct aws_signable;
struct aws_signing_result;

/**
 * Gets called by the signing function when the signing is complete.
 *
 * Note that result will be destroyed after this function returns, so either copy it,
 * or do all necessary adjustments inside the callback.
 *
 * When performing event or chunk signing, you will need to copy out the signature value in order
 * to correctly configure the signable that wraps the event or chunk you want signed next.  The signature is
 * found in the "signature" property on the signing result.  This value must be added as the
 * "previous-signature" property on the next signable.
 */
typedef void(aws_signing_complete_fn)(struct aws_signing_result *result, int error_code, void *userdata);

AWS_EXTERN_C_BEGIN

/*
 * Takes a signable object and a configuration struct and computes the changes to the signable necessary
 * for compliance with the signer's signing algorithm.
 *
 * This signing function currently supports only the sigv4 algorithm.
 *
 * When using this signing function to sign AWS http requests:
 *
 *   (1) Do not add the following headers to requests before signing:
 *      x-amz-content-sha256,
 *      X-Amz-Date,
 *      Authorization
 *
 *   (2) Do not add the following query params to requests before signing:
 *      X-Amz-Signature,
 *      X-Amz-Date,
 *      X-Amz-Credential,
 *      X-Amz-Algorithm,
 *      X-Amz-SignedHeaders
 *
 *   The signing result will tell exactly what header and/or query params to add to the request
 *   to become a fully-signed AWS http request.
 *
 *
 * When using this signing function to sign chunks:
 *
 *   (1) Use aws_signable_new_chunk() to create the signable object representing the chunk
 *
 *   The signing result will include the chunk's signature as the "signature" property.
 *
 *
 */

/**
 * (Asynchronous) entry point to sign something (a request, a chunk, an event) with an AWS signing process.
 * Depending on the configuration, the signing process may or may not complete synchronously.
 *
 * @param allocator memory allocator to use throughout the signing process
 * @param signable the thing to be signed.  See signable.h for common constructors for signables that
 * wrap different types.
 * @param base_config pointer to a signing configuration, currently this must be of type aws_signing_config_aws
 * @param on_complete completion callback to be invoked when signing has finished
 * @param user_data opaque user data that will be passed to the completion callback
 *
 * @return AWS_OP_SUCCESS if the signing attempt was *initiated* successfully, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_sign_request_aws(
    struct aws_allocator *allocator,
    const struct aws_signable *signable,
    const struct aws_signing_config_base *base_config,
    aws_signing_complete_fn *on_complete,
    void *userdata);

/**
 * Test-only API used for cross-library signing verification tests
 *
 * Verifies:
 *  (1) The canonical request generated during sigv4a signing of the request matches what is passed in
 *  (2) The signature passed in is a valid ECDSA signature of the hashed string-to-sign derived from the
 *  canonical request
 *
 *  @param allocator memory allocator to use throughout the signing verification process
 *  @param signable the thing to be signed.  See signable.h for common constructors for signables that
 * wrap different types.
 *  @param base_config pointer to a signing configuration, currently this must be of type aws_signing_config_aws
 *  @param expected_canonical_request_cursor expected result when building the canonical request
 *  @param signature_cursor the actual signature computed from a previous signing of the signable
 *  @param ecc_key_pub_x the x coordinate of the public part of the ecc key to verify the signature
 *  @param ecc_key_pub_y the y coordinate of the public part of the ecc key to verify the signature
 *
 *  @return AWS_OP_SUCCESS if the signing attempt was *initiated* successfully, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_verify_sigv4a_signing(
    struct aws_allocator *allocator,
    const struct aws_signable *signable,
    const struct aws_signing_config_base *base_config,
    struct aws_byte_cursor expected_canonical_request_cursor,
    struct aws_byte_cursor signature_cursor,
    struct aws_byte_cursor ecc_key_pub_x,
    struct aws_byte_cursor ecc_key_pub_y);

/**
 * Another helper function to check a computed sigv4a signature.
 */
AWS_AUTH_API
int aws_validate_v4a_authorization_value(
    struct aws_allocator *allocator,
    struct aws_ecc_key_pair *ecc_key,
    struct aws_byte_cursor string_to_sign_cursor,
    struct aws_byte_cursor signature_value_cursor);

/**
 * Removes any padding added to the end of a sigv4a signature.  Signature must be hex-encoded.
 * @param signature signature to remove padding from
 * @return cursor that ranges over only the valid hex encoding of the sigv4a signature
 */
AWS_AUTH_API
struct aws_byte_cursor aws_trim_padded_sigv4a_signature(struct aws_byte_cursor signature);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_AUTH_SIGNER_H */
