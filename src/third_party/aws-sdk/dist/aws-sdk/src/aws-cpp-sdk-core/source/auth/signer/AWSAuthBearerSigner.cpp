/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/auth/signer/AWSAuthBearerSigner.h>
#include <aws/core/auth/bearer-token-provider/AWSBearerTokenProviderBase.h>

#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/http/HttpRequest.h>

namespace Aws
{
    namespace Auth
    {
        const char BEARER_SIGNER[] = "Bearer";
    }

    namespace Client
    {
        static const char LOGGING_TAG[] = "AWSAuthBearerSigner";
        static const char AUTHORIZATION_HEADER[] = "authorization";

        bool AWSAuthBearerSigner::SignRequest(Aws::Http::HttpRequest& ioRequest) const
        {
            if(Aws::Http::Scheme::HTTPS != ioRequest.GetUri().GetScheme())
            {
                // Clients MUST always use TLS (https) or equivalent transport security
                // when making requests with bearer tokens.
                // https://datatracker.ietf.org/doc/html/rfc6750
                AWS_LOGSTREAM_ERROR(LOGGING_TAG, "HTTPS scheme must be used with a bearer token authorization");
                return false;
            }
            if(!m_bearerTokenProvider)
            {
                AWS_LOGSTREAM_FATAL(LOGGING_TAG, "Unexpected nullptr AWSAuthBearerSigner::m_bearerTokenProvider");
                return false;
            }
            const Aws::Auth::AWSBearerToken& token = m_bearerTokenProvider->GetAWSBearerToken();
            if(token.IsExpiredOrEmpty())
            {
                AWS_LOGSTREAM_ERROR(LOGGING_TAG, "Invalid bearer token to use: expired or empty");
                return false;
            }

            ioRequest.SetHeaderValue(AUTHORIZATION_HEADER, "Bearer " + token.GetToken());
            return true;
        }
    }
}
