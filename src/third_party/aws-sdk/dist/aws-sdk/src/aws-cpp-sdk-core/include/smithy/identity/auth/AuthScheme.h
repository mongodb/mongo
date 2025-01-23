/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/resolver/AwsIdentityResolverBase.h>
#include <smithy/identity/signer/AwsSignerBase.h>

namespace smithy {
    template<typename IDENTITY_T>
    class AuthScheme
    {
    public:
        using IdentityT = IDENTITY_T;

        template<std::size_t N>
        AuthScheme(char const (&iSchemeId)[N])
        {
            memcpy(schemeId, iSchemeId, N);
        }

        char schemeId[32];

        virtual ~AuthScheme() = default;

        virtual std::shared_ptr<IdentityResolverBase<IdentityT>> identityResolver() = 0;

        virtual std::shared_ptr<AwsSignerBase<IdentityT>> signer() = 0;
    };
}