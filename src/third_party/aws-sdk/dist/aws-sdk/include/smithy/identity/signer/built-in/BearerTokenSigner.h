/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <smithy/identity/identity/AwsBearerTokenIdentityBase.h>
#include <smithy/identity/signer/AwsSignerBase.h>

#include <aws/core/auth/signer/AWSAuthSignerHelper.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/crt/http/HttpConnection.h>
#include <aws/crt/http/HttpRequestResponse.h>

namespace smithy
{
static const char AUTHORIZATION_HEADER[] = "authorization";

class BearerTokenSigner : public AwsSignerBase<AwsBearerTokenIdentityBase>
{

  public:
    static const char LOGGING_TAG[];

    using BearerTokenAuthSchemeParameters =
        smithy::DefaultAuthSchemeResolverParameters;
    explicit BearerTokenSigner(const Aws::String &serviceName,
                               const Aws::String &region)
        : m_serviceName(serviceName), m_region(region)
    {
    }

    SigningFutureOutcome
    sign(std::shared_ptr<HttpRequest> httpRequest,
         const smithy::AwsBearerTokenIdentityBase &identity,
         SigningProperties properties) override
    {
        AWS_UNREFERENCED_PARAM(properties);

        if (Aws::Http::Scheme::HTTPS != httpRequest->GetUri().GetScheme())
        {
            // Clients MUST always use TLS (https) or equivalent transport
            // security when making requests with bearer tokens.
            // https://datatracker.ietf.org/doc/html/rfc6750
            AWS_LOGSTREAM_ERROR(
                LOGGING_TAG,
                "HTTPS scheme must be used with a bearer token authorization");
            return SigningError(
                Aws::Client::CoreErrors::INVALID_PARAMETER_VALUE, "",
                "Failed to sign the request with bearer", false);
        }

        httpRequest->SetHeaderValue(AUTHORIZATION_HEADER,
                                    "Bearer " + identity.token());

        return SigningFutureOutcome(std::move(httpRequest));
    }

    virtual ~BearerTokenSigner(){};

  protected:
    Aws::String m_serviceName;
    Aws::String m_region;
};

const char BearerTokenSigner::LOGGING_TAG[] = "BearerTokenSigner";
} // namespace smithy
