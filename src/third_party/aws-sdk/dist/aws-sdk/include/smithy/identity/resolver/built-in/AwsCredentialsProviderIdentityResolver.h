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
    class AwsCredentialsProviderIdentityResolver : public AwsCredentialIdentityResolver
    {
    public:
        using SigV4AuthSchemeParameters = DefaultAuthSchemeResolverParameters;

        explicit AwsCredentialsProviderIdentityResolver(const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentialsProvider)
            : m_credentialsProvider(credentialsProvider)
        {
        }

        AwsCredentialsProviderIdentityResolver(const AwsCredentialsProviderIdentityResolver& other) = delete;
        AwsCredentialsProviderIdentityResolver(AwsCredentialsProviderIdentityResolver&& other) noexcept = default;
        AwsCredentialsProviderIdentityResolver& operator=(const AwsCredentialsProviderIdentityResolver& other) = delete;
        AwsCredentialsProviderIdentityResolver& operator=(AwsCredentialsProviderIdentityResolver&& other) noexcept = default;
        ~AwsCredentialsProviderIdentityResolver() override = default;

        ResolveIdentityFutureOutcome getIdentity(const IdentityProperties& identityProperties,
                                                 const AdditionalParameters& additionalParameters) override
        {
            AWS_UNREFERENCED_PARAM(identityProperties);
            AWS_UNREFERENCED_PARAM(additionalParameters);

            const auto fetchedCreds = m_credentialsProvider->GetAWSCredentials();

            auto smithyCreds = Aws::MakeUnique<AwsCredentialIdentity>("DefaultAwsCredentialIdentityResolver",
                                                                     fetchedCreds.GetAWSAccessKeyId(), fetchedCreds.GetAWSSecretKey(),
                                                                     fetchedCreds.GetSessionToken(), fetchedCreds.GetExpiration());

            return {std::move(smithyCreds)};
        }

    protected:
        std::shared_ptr<Aws::Auth::AWSCredentialsProvider> m_credentialsProvider;
    };
}
