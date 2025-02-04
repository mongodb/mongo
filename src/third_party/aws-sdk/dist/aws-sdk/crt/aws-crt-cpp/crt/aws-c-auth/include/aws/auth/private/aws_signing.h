#ifndef AWS_AUTH_SIGNING_SIGV4_H
#define AWS_AUTH_SIGNING_SIGV4_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/auth.h>
#include <aws/auth/signing.h>
#include <aws/auth/signing_result.h>

#include <aws/common/byte_buf.h>
#include <aws/common/hash_table.h>

struct aws_ecc_key_pair;
struct aws_signable;
struct aws_signing_config_aws;
struct aws_signing_result;

/*
 * Private signing API
 *
 * Technically this could be folded directly into signing.c but it's useful to be able
 * to call the individual stages of the signing process for testing.
 */

/*
 * A structure that contains all the state related to signing a request for AWS.  We pass
 * this around rather than a million parameters.
 */
struct aws_signing_state_aws {
    struct aws_allocator *allocator;

    const struct aws_signable *signable;
    aws_signing_complete_fn *on_complete;
    void *userdata;

    struct aws_signing_config_aws config;
    struct aws_byte_buf config_string_buffer;

    struct aws_signing_result result;
    int error_code;

    /* persistent, constructed values that are either/or
     *  (1) consumed by later stages of the signing process,
     *  (2) used in multiple places
     */
    struct aws_byte_buf canonical_request;
    struct aws_byte_buf string_to_sign;
    struct aws_byte_buf signed_headers;
    struct aws_byte_buf canonical_header_block;
    struct aws_byte_buf payload_hash;
    struct aws_byte_buf credential_scope;
    struct aws_byte_buf access_credential_scope;
    struct aws_byte_buf date;
    struct aws_byte_buf signature;
    /* The "payload" to be used in the string-to-sign.
     * For a normal HTTP request, this is the hashed canonical-request.
     * But for other types of signing (i.e chunk, event) it's something else. */
    struct aws_byte_buf string_to_sign_payload;

    /* temp buf for writing out strings */
    struct aws_byte_buf scratch_buf;

    char expiration_array[32]; /* serialization of the pre-signing expiration duration value */
};

AWS_EXTERN_C_BEGIN

AWS_AUTH_API
struct aws_signing_state_aws *aws_signing_state_new(
    struct aws_allocator *allocator,
    const struct aws_signing_config_aws *config,
    const struct aws_signable *signable,
    aws_signing_complete_fn *on_complete,
    void *userdata);

AWS_AUTH_API
void aws_signing_state_destroy(struct aws_signing_state_aws *state);

/*
 * A set of functions that together performs the AWS signing process based
 * on the algorithm and signature type requested in the shared config.
 *
 * These must be called (presumably by the signer) in sequential order:
 *
 *   (1) aws_signing_build_canonical_request
 *   (2) aws_signing_build_string_to_sign
 *   (3) aws_signing_build_authorization_value
 */

AWS_AUTH_API
int aws_signing_build_canonical_request(struct aws_signing_state_aws *state);

AWS_AUTH_API
int aws_signing_build_string_to_sign(struct aws_signing_state_aws *state);

AWS_AUTH_API
int aws_signing_build_authorization_value(struct aws_signing_state_aws *state);

/*
 * Named constants particular to the sigv4 signing algorithm.  Can be moved to a public header
 * as needed.
 */
AWS_AUTH_API extern const struct aws_string *g_aws_signing_content_header_name;
AWS_AUTH_API extern const struct aws_string *g_aws_signing_algorithm_query_param_name;
AWS_AUTH_API extern const struct aws_string *g_aws_signing_credential_query_param_name;
AWS_AUTH_API extern const struct aws_string *g_aws_signing_date_name;
AWS_AUTH_API extern const struct aws_string *g_aws_signing_signed_headers_query_param_name;
AWS_AUTH_API extern const struct aws_string *g_aws_signing_security_token_name;
AWS_AUTH_API extern const struct aws_string *g_aws_signing_s3session_token_name;
AWS_AUTH_API extern const struct aws_string *g_signature_type_sigv4a_http_request;

/**
 * Initializes the internal table of headers that should not be signed
 */
AWS_AUTH_API
int aws_signing_init_signing_tables(struct aws_allocator *allocator);

/**
 * Cleans up the internal table of headers that should not be signed
 */
AWS_AUTH_API
void aws_signing_clean_up_signing_tables(void);

AWS_EXTERN_C_END

#endif /* AWS_AUTH_SIGNING_SIGV4_H */
