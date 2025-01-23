/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/resolver/AwsCredentialIdentityResolver.h>

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>

namespace smithy
{
    class SimpleAwsCredentialIdentityResolver : public AwsCredentialIdentityResolver
    {
    public:
        using SigV4AuthSchemeParameters = DefaultAuthSchemeResolverParameters;

        explicit SimpleAwsCredentialIdentityResolver(const Aws::Auth::AWSCredentials& credentials)
            : m_credentials(credentials)
        {
        }

        SimpleAwsCredentialIdentityResolver(const SimpleAwsCredentialIdentityResolver& other) = delete;
        SimpleAwsCredentialIdentityResolver(SimpleAwsCredentialIdentityResolver&& other) noexcept = default;
        SimpleAwsCredentialIdentityResolver& operator=(const SimpleAwsCredentialIdentityResolver& other) = delete;
        SimpleAwsCredentialIdentityResolver& operator=(SimpleAwsCredentialIdentityResolver&& other) noexcept = default;
        virtual ~SimpleAwsCredentialIdentityResolver() = default;

        ResolveIdentityFutureOutcome getIdentity(const IdentityProperties& identityProperties,
                                                 const AdditionalParameters& additionalParameters) override
        {
            AWS_UNREFERENCED_PARAM(identityProperties);
            AWS_UNREFERENCED_PARAM(additionalParameters);

            auto smithyCreds = Aws::MakeUnique<AwsCredentialIdentity>("DefaultAwsCredentialIdentityResolver",
                                                                     m_credentials.GetAWSAccessKeyId(), m_credentials.GetAWSSecretKey(),
                                                                     m_credentials.GetSessionToken(), m_credentials.GetExpiration());

            return {std::move(smithyCreds)};
        }

    protected:
        Aws::Auth::AWSCredentials m_credentials;
    };
}
