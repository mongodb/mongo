/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/auth/bearer-token-provider/AWSBearerTokenProviderBase.h>
#include <aws/core/auth/bearer-token-provider/SSOBearerTokenProvider.h>
#include <smithy/identity/identity/AwsBearerTokenIdentity.h>
#include <smithy/identity/resolver/AwsIdentityResolverBase.h>

namespace smithy
{

class AwsBearerTokenIdentityResolver
    : public IdentityResolverBase<AwsBearerTokenIdentityBase>
{
  public:
    static const char BEARER_TOKEN_PROVIDER_CHAIN_LOG_TAG[];

    using IdentityT = AwsBearerTokenIdentity;
    virtual ~AwsBearerTokenIdentityResolver() = default;

    AwsBearerTokenIdentityResolver() = default;

    AwsBearerTokenIdentityResolver(
        const Aws::Vector<
            std::shared_ptr<Aws::Auth::AWSBearerTokenProviderBase>>
            &providerChain)
        : m_providerChainLegacy{providerChain}
    {
    }

    ResolveIdentityFutureOutcome
    getIdentity(const IdentityProperties &identityProperties,
                const AdditionalParameters &additionalParameters) override
    {
        AWS_UNREFERENCED_PARAM(identityProperties);
        AWS_UNREFERENCED_PARAM(additionalParameters);
        for (auto &bearerTokenProvider : m_providerChainLegacy)
        {
            if (!bearerTokenProvider)
            {
                AWS_LOGSTREAM_FATAL(
                    BEARER_TOKEN_PROVIDER_CHAIN_LOG_TAG,
                    "Unexpected nullptr in "
                    "DefaultBearerTokenProviderChain::m_providerChain");
                return Aws::Client::AWSError<Aws::Client::CoreErrors>(
                    Aws::Client::CoreErrors::INVALID_PARAMETER_VALUE, "",
                    "Unexpected nullptr in "
                    "BearerTokenProviderChain::m_providerChain",
                    false);
            }
            auto bearerToken = bearerTokenProvider->GetAWSBearerToken();
            if (!bearerToken.IsExpiredOrEmpty())
            {
                auto outcomePtr = Aws::MakeUnique<AwsBearerTokenIdentity>(
                    BEARER_TOKEN_PROVIDER_CHAIN_LOG_TAG);
                outcomePtr->token() = bearerToken.GetToken();
                outcomePtr->expiration() = bearerToken.GetExpiration();
                return ResolveIdentityFutureOutcome(std::move(outcomePtr));
            }
        }

        return Aws::Client::AWSError<Aws::Client::CoreErrors>(
            Aws::Client::CoreErrors::NOT_INITIALIZED, "",
            "No bearer token provider in chain found", false);
    }

    void AddBearerTokenProvider(
        std::shared_ptr<Aws::Auth::AWSBearerTokenProviderBase> provider)
    {
        m_providerChainLegacy.emplace_back(std::move(provider));
    }

  protected:
    Aws::Vector<std::shared_ptr<Aws::Auth::AWSBearerTokenProviderBase>>
        m_providerChainLegacy;
};

class DefaultAwsBearerTokenIdentityResolver
    : public AwsBearerTokenIdentityResolver
{
  public:
    using IdentityT = AwsBearerTokenIdentity;
    virtual ~DefaultAwsBearerTokenIdentityResolver() = default;

    DefaultAwsBearerTokenIdentityResolver()
        : AwsBearerTokenIdentityResolver(
              {Aws::MakeShared<Aws::Auth::SSOBearerTokenProvider>(
                  "SSOBearerTokenProvider")}){};
};
const char
    AwsBearerTokenIdentityResolver::BEARER_TOKEN_PROVIDER_CHAIN_LOG_TAG[] =
        "BearerTokenProvider";

} // namespace smithy