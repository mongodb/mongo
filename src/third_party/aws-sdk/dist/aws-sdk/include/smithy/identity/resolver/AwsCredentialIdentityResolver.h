/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/resolver/AwsIdentityResolverBase.h>

#include <smithy/identity/identity/AwsCredentialIdentity.h>

namespace smithy {
    class AwsCredentialIdentityResolver : public IdentityResolverBase<AwsCredentialIdentityBase> {
    public:
        using IdentityT = AwsCredentialIdentity;
        virtual ~AwsCredentialIdentityResolver() = default;

        ResolveIdentityFutureOutcome getIdentity(const IdentityProperties& identityProperties, const AdditionalParameters& additionalParameters) override = 0;
    };
}