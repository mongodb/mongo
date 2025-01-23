/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/utils/UUID.h>
#include <aws/s3/S3ExpressSigner.h>

#include <utility>

using namespace Aws::S3;
using namespace Aws::Client;
using namespace Aws::Config;
using namespace Aws::Environment;
using namespace Aws::Utils;

static const char *S3_EXPRESS_SIGNER_NAME = "S3ExpressSigner";
static const char *S3_EXPRESS_HEADER = "x-amz-s3session-token";
static const char *S3_EXPRESS_QUERY_PARAM = "X-Amz-S3session-Token";
static const char *S3_EXPRESS_SERVICE_NAME = "s3express";

S3ExpressSigner::S3ExpressSigner(
    std::shared_ptr<S3ExpressIdentityProvider> S3ExpressIdentityProvider,
    const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> &credentialsProvider,
    const Aws::String &serviceName,
    const Aws::String &region,
    AWSAuthV4Signer::PayloadSigningPolicy signingPolicy,
    bool urlEscapePath,
    Aws::Auth::AWSSigningAlgorithm signingAlgorithm) :
    AWSAuthV4Signer(credentialsProvider,
        serviceName.c_str(),
        region,
        signingPolicy,
        urlEscapePath,
        signingAlgorithm),
    m_S3ExpressIdentityProvider(std::move(S3ExpressIdentityProvider)) {
}

const char *S3ExpressSigner::GetName() const {
    return S3_EXPRESS_SIGNER_NAME;
}

bool S3ExpressSigner::SignRequest(Aws::Http::HttpRequest &request,
    const char *region,
    const char *serviceName,
    bool signBody
) const {
    const auto requestId = Aws::GetWithDefault(request.GetServiceSpecificParameters()->parameterMap,
        Aws::String("dedupeId"),
        Aws::String(UUID::RandomUUID()));
    if(hasRequestId(requestId)) {
        // We were already processing this request and are attempting to sign
        // it again in infinite recursion
        AWS_LOG_ERROR(S3_EXPRESS_SIGNER_NAME, "Refusing to sign request more than once")
        return false;
    }
    putRequestId(requestId);
    auto identity = m_S3ExpressIdentityProvider->GetS3ExpressIdentity(request.GetServiceSpecificParameters());
    request.SetHeaderValue(S3_EXPRESS_HEADER, identity.getSessionToken());
    auto isSigned = AWSAuthV4Signer::SignRequest(request, region, serviceName, signBody);
    deleteRequestId(requestId);
    return isSigned;
}

bool S3ExpressSigner::PresignRequest(Aws::Http::HttpRequest &request,
    const char *region,
    const char *serviceName,
    long long int expirationInSeconds
) const {
    const auto requestId = Aws::GetWithDefault(request.GetServiceSpecificParameters()->parameterMap,
        Aws::String("dedupeId"),
        Aws::String(UUID::RandomUUID()));
    if(hasRequestId(requestId)) {
        // We were already processing this request and are attempting to sign
        // it again in infinite recursion
        AWS_LOG_ERROR(S3_EXPRESS_SIGNER_NAME, "Refusing to sign request more than once")
        return false;
    }
    putRequestId(requestId);
    auto identity = m_S3ExpressIdentityProvider->GetS3ExpressIdentity(request.GetServiceSpecificParameters());
    request.AddQueryStringParameter(S3_EXPRESS_QUERY_PARAM, identity.getSessionToken());
    auto isSigned = AWSAuthV4Signer::PresignRequest(request, region, serviceName, expirationInSeconds);
    deleteRequestId(requestId);
    return isSigned;
}

bool S3ExpressSigner::ServiceRequireUnsignedPayload(const Aws::String &serviceName) const {
    return S3_EXPRESS_SERVICE_NAME == serviceName || AWSAuthV4Signer::ServiceRequireUnsignedPayload(serviceName);
}

Aws::Auth::AWSCredentials S3ExpressSigner::GetCredentials(const std::shared_ptr<Aws::Http::ServiceSpecificParameters> &serviceSpecificParameters) const {
    auto identity = m_S3ExpressIdentityProvider->GetS3ExpressIdentity(serviceSpecificParameters);
    return {identity.getAccessKeyId(), identity.getSecretKeyId()};
}
