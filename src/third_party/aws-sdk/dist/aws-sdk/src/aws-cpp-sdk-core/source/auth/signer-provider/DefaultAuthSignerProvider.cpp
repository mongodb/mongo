/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
 
#include <aws/core/auth/signer-provider/DefaultAuthSignerProvider.h>

#include <aws/core/auth/signer/AWSAuthEventStreamV4Signer.h>
#include <aws/core/auth/signer/AWSNullSigner.h>


#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>

const char CLASS_TAG[] = "AuthSignerProvider";

using namespace Aws::Auth;

DefaultAuthSignerProvider::DefaultAuthSignerProvider(const std::shared_ptr<AWSCredentialsProvider>& credentialsProvider,
        const Aws::String& serviceName,
        const Aws::String& region,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy signingPolicy,
        bool urlEscapePath):
    m_credentialsProvider(credentialsProvider)
{
    m_signers.emplace_back(Aws::MakeShared<Aws::Client::AWSAuthV4Signer>(CLASS_TAG, credentialsProvider, serviceName.c_str(), region, signingPolicy, urlEscapePath, AWSSigningAlgorithm::SIGV4));
    m_signers.emplace_back(Aws::MakeShared<Aws::Client::AWSAuthV4Signer>(CLASS_TAG, credentialsProvider, serviceName.c_str(), region, signingPolicy, urlEscapePath, AWSSigningAlgorithm::ASYMMETRIC_SIGV4));
    m_signers.emplace_back(Aws::MakeShared<Aws::Client::AWSAuthEventStreamV4Signer>(CLASS_TAG, credentialsProvider, serviceName.c_str(), region));
    m_signers.emplace_back(Aws::MakeShared<Aws::Client::AWSNullSigner>(CLASS_TAG));
}

DefaultAuthSignerProvider::DefaultAuthSignerProvider(const std::shared_ptr<Aws::Client::AWSAuthSigner>& signer)
{
    m_signers.emplace_back(Aws::MakeShared<Aws::Client::AWSNullSigner>(CLASS_TAG));
    if(signer)
    {
        m_signers.emplace_back(signer);
    }
}

std::shared_ptr<Aws::Client::AWSAuthSigner> DefaultAuthSignerProvider::GetSigner(const Aws::String& signerName) const
{
    for(const auto& signer : m_signers)
    {
        if(signer->GetName() == signerName)
        {
            return signer;
        }
    }
    AWS_LOGSTREAM_ERROR(CLASS_TAG, "Request's signer: '" << signerName << "' is not found in the signer's map.");
    assert(false);
    return nullptr;
}

void DefaultAuthSignerProvider::AddSigner(std::shared_ptr<Aws::Client::AWSAuthSigner>& signer)
{
    assert(signer);
    m_signers.emplace_back(signer);
}
