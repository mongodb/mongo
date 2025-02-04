/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/s3/S3ExpressSignerProvider.h>
#include <aws/s3/S3ExpressSigner.h>

static const char *CLASS_TAG = "S3ExpressSignerProvider";

Aws::Auth::S3ExpressSignerProvider::S3ExpressSignerProvider(
    const std::shared_ptr<AWSCredentialsProvider> &credentialsProvider,
    const std::shared_ptr<S3::S3ExpressIdentityProvider> &S3ExpressIdentityProvider,
    const Aws::String &serviceName,
    const Aws::String &region,
    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy signingPolicy,
    bool urlEscapePath) :
    DefaultAuthSignerProvider(credentialsProvider,
        serviceName,
        region,
        signingPolicy,
        urlEscapePath) {
    m_signers.emplace_back(Aws::MakeShared<Aws::S3::S3ExpressSigner>(CLASS_TAG,
        S3ExpressIdentityProvider,
        credentialsProvider,
        serviceName.c_str(),
        region,
        signingPolicy,
        urlEscapePath,
        AWSSigningAlgorithm::SIGV4));
}