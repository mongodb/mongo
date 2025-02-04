#ifndef AWS_S3_ENDPOINT_RESOLVER_H
#define AWS_S3_ENDPOINT_RESOLVER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/s3.h>
AWS_PUSH_SANE_WARNING_LEVEL

struct aws_endpoints_request_context;
struct aws_endpoints_rule_engine;
AWS_EXTERN_C_BEGIN

/**
 * Creates a new S3 endpoint resolver.
 * Warning: Before using this header, you have to enable it by
 * setting cmake config AWS_ENABLE_S3_ENDPOINT_RESOLVER=ON
 */
AWS_S3_API
struct aws_endpoints_rule_engine *aws_s3_endpoint_resolver_new(struct aws_allocator *allocator);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL
#endif /* AWS_S3_ENDPOINT_RESOLVER_H */
