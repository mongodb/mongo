#ifndef AWS_AUTH_TOKEN_PRIVATE_H
#define AWS_AUTH_TOKEN_PRIVATE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/auth.h>
#include <aws/common/date_time.h>

/* structure to represent a parsed sso token */
struct aws_sso_token {
    struct aws_allocator *allocator;

    struct aws_string *access_token;
    struct aws_date_time expiration;
};

AWS_EXTERN_C_BEGIN

/* Construct token path which is ~/.aws/sso/cache/<hex encoded sha1 of input>.json */
AWS_AUTH_API
struct aws_string *aws_construct_sso_token_path(struct aws_allocator *allocator, const struct aws_string *input);

AWS_AUTH_API
void aws_sso_token_destroy(struct aws_sso_token *token);

/* Parse `aws_sso_token` from the give file path */
AWS_AUTH_API
struct aws_sso_token *aws_sso_token_new_from_file(struct aws_allocator *allocator, const struct aws_string *file_path);

/**
 * Creates a set of AWS credentials based on a token with expiration.
 *
 * @param allocator memory allocator to use for all memory allocation
 * @param token token for the credentials
 * @param expiration_timepoint_in_seconds time at which these credentials expire
 * @return a new pair of AWS credentials, or NULL
 */
AWS_AUTH_API
struct aws_credentials *aws_credentials_new_token(
    struct aws_allocator *allocator,
    struct aws_byte_cursor token,
    uint64_t expiration_timepoint_in_seconds);

/**
 * Get the token from a set of AWS credentials
 *
 * @param credentials credentials to get the token from
 * @return a byte cursor to the token or an empty byte cursor if there is no token
 */
AWS_AUTH_API
struct aws_byte_cursor aws_credentials_get_token(const struct aws_credentials *credentials);

AWS_EXTERN_C_END

#endif /* AWS_AUTH_TOKEN_PRIVATE_H */
