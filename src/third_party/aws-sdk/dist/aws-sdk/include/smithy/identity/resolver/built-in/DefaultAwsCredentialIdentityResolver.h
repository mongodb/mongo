/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <smithy/identity/resolver/AwsCredentialIdentityResolver.h>

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>

namespace smithy {
    constexpr char ALLOC_ID[] = "DefaultAwsCredentialIdentityResolver";
    /**
     * A smithy SigV4 AWS Credentials resolver wrapper on top of legacy SDK Credentials provider
     * TODO: refactor into own signer using smithy design
     */
    class DefaultAwsCredentialIdentityResolver : public AwsCredentialIdentityResolver {
    protected:

        mutable std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> legacyChain_sp;

    public:
        using SigV4AuthSchemeParameters = DefaultAuthSchemeResolverParameters;

        DefaultAwsCredentialIdentityResolver(): legacyChain_sp{Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>(ALLOC_ID)}{

        };

        DefaultAwsCredentialIdentityResolver(const DefaultAwsCredentialIdentityResolver& other) = delete;
        DefaultAwsCredentialIdentityResolver(DefaultAwsCredentialIdentityResolver&& other) noexcept = default;
        DefaultAwsCredentialIdentityResolver& operator=(const DefaultAwsCredentialIdentityResolver& other) = delete;
        DefaultAwsCredentialIdentityResolver& operator=(DefaultAwsCredentialIdentityResolver&& other) noexcept = default;
        virtual ~DefaultAwsCredentialIdentityResolver() = default;

        DefaultAwsCredentialIdentityResolver(std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> providerChain): legacyChain_sp{providerChain}
        {
            assert(legacyChain_sp);
        };

        ResolveIdentityFutureOutcome getIdentity(const IdentityProperties& identityProperties, const AdditionalParameters& additionalParameters) override
        {
            AWS_UNREFERENCED_PARAM(identityProperties);
            AWS_UNREFERENCED_PARAM(additionalParameters);

            auto legacyCreds = legacyChain_sp->GetAWSCredentials();

            auto smithyCreds = Aws::MakeUnique<AwsCredentialIdentity>("DefaultAwsCredentialIdentityResolver",
                                                                     legacyCreds.GetAWSAccessKeyId(), 
                                                                     legacyCreds.GetAWSSecretKey(),
                                                                     legacyCreds.GetSessionToken().empty()? Aws::Crt::Optional<Aws::String>() : legacyCreds.GetSessionToken(),
                                                                     legacyCreds.GetExpiration());

            return ResolveIdentityFutureOutcome(std::move(smithyCreds));
        }
    };
}
