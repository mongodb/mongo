/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/auth/bearer-token-provider/DefaultBearerTokenProviderChain.h>
#include <aws/core/auth/AWSBearerToken.h>
#include <aws/core/auth/bearer-token-provider/SSOBearerTokenProvider.h>
#include <aws/core/utils/logging/LogMacros.h>


static const char SSO_DEFAULT_BEARER_TOKEN_PROVIDER_CHAIN_LOG_TAG[] = "SSOBearerTokenProvider";

Aws::Auth::AWSBearerToken Aws::Auth::DefaultBearerTokenProviderChain::GetAWSBearerToken()
{
    for (auto&& bearerTokenProvider : m_providerChain)
    {
        if(!bearerTokenProvider) {
            AWS_LOGSTREAM_FATAL(SSO_DEFAULT_BEARER_TOKEN_PROVIDER_CHAIN_LOG_TAG,
                                "Unexpected nullptr in DefaultBearerTokenProviderChain::m_providerChain");
            break;
        }
        AWSBearerToken bearerToken = bearerTokenProvider->GetAWSBearerToken();
        if(!bearerToken.IsExpiredOrEmpty())
        {
            return bearerToken;
        }
    }
    return AWSBearerToken("", Aws::Utils::DateTime(0.0));
}

Aws::Auth::DefaultBearerTokenProviderChain::DefaultBearerTokenProviderChain()
{
    AddProvider(Aws::MakeShared<Aws::Auth::SSOBearerTokenProvider>(SSO_DEFAULT_BEARER_TOKEN_PROVIDER_CHAIN_LOG_TAG));
}
