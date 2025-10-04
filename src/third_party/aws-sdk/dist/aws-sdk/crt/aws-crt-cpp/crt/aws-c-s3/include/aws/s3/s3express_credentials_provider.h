#ifndef AWS_S3EXPRESS_CREDENTIALS_PROVIDER_H
#define AWS_S3EXPRESS_CREDENTIALS_PROVIDER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>
#include <aws/common/ref_count.h>
#include <aws/s3/s3.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_s3_client;
struct aws_s3express_credentials_provider;

struct aws_credentials_properties_s3express {
    /**
     * Required.
     * The host address of the s3 bucket for the request.
     */
    struct aws_byte_cursor host;
    /**
     * Optional.
     * The region of the bucket.
     * If empty, the region of the S3 client will be used.
     */
    struct aws_byte_cursor region;
};

struct aws_s3express_credentials_provider_vtable {
    /**
     * Implementation for S3 Express provider to get S3 Express credentials
     */
    int (*get_credentials)(
        struct aws_s3express_credentials_provider *provider,
        const struct aws_credentials *original_credentials,
        const struct aws_credentials_properties_s3express *properties,
        aws_on_get_credentials_callback_fn callback,
        void *user_data);

    /**
     * Implementation to destroy the provider.
     */
    void (*destroy)(struct aws_s3express_credentials_provider *provider);
};

struct aws_s3express_credentials_provider {
    struct aws_s3express_credentials_provider_vtable *vtable;
    struct aws_allocator *allocator;
    /* Optional callback for shutdown complete of the provider */
    aws_simple_completion_callback *shutdown_complete_callback;
    void *shutdown_user_data;
    void *impl;
    struct aws_ref_count ref_count;
};

AWS_EXTERN_C_BEGIN

AWS_S3_API
struct aws_s3express_credentials_provider *aws_s3express_credentials_provider_release(
    struct aws_s3express_credentials_provider *provider);

/**
 * To initialize the provider with basic vtable and refcount. And hook up the refcount with vtable functions.
 *
 * @param provider
 * @param allocator
 * @param vtable
 * @param impl Optional, the impl for the provider
 * @return AWS_S3_API
 */
AWS_S3_API
void aws_s3express_credentials_provider_init_base(
    struct aws_s3express_credentials_provider *provider,
    struct aws_allocator *allocator,
    struct aws_s3express_credentials_provider_vtable *vtable,
    void *impl);

/**
 * Async function for retrieving specific credentials based on properties.
 *
 * @param provider aws_s3express_credentials_provider provider to source from
 * @param original_credentials The credentials used to derive the credentials for S3 Express.
 * @param properties Specific properties for credentials being fetched.
 * @param user_data user data to pass to the completion callback
 *
 * callback will only be invoked if-and-only-if the return value was AWS_OP_SUCCESS.
 *
 */
AWS_S3_API int aws_s3express_credentials_provider_get_credentials(
    struct aws_s3express_credentials_provider *provider,
    const struct aws_credentials *original_credentials,
    const struct aws_credentials_properties_s3express *properties,
    aws_on_get_credentials_callback_fn callback,
    void *user_data);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_S3EXPRESS_CREDENTIALS_PROVIDER_H */
