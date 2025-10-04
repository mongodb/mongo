#ifndef AWS_AUTH_AWS_PROFILE_H
#define AWS_AUTH_AWS_PROFILE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/auth.h>

#include <aws/sdkutils/aws_profile.h>

AWS_EXTERN_C_BEGIN

/**
 * Returns a set of credentials associated with a profile, based on the properties within the profile
 */
AWS_AUTH_API
struct aws_credentials *aws_credentials_new_from_profile(
    struct aws_allocator *allocator,
    const struct aws_profile *profile);

AWS_EXTERN_C_END

#endif /* AWS_AUTH_AWS_PROFILE_H */
