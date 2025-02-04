/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/identity/AwsIdentity.h>

namespace smithy {
    class AwsCredentialIdentityBase : public AwsIdentity {
    public:
        virtual Aws::String accessKeyId() const = 0;
        virtual Aws::String secretAccessKey() const = 0;
        virtual Aws::Crt::Optional<Aws::String> sessionToken() const = 0;
        virtual Aws::Crt::Optional<AwsIdentity::DateTime> expiration() const override = 0 ;
    };
}