/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/crypto/Sha256.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/crypto/Factories.h>

using namespace Aws::Utils::Crypto;

Sha256::Sha256() :
    m_hashImpl(CreateSha256Implementation())
{
}

Sha256::~Sha256()
{
}

HashResult Sha256::Calculate(const Aws::String& str)
{
    return m_hashImpl->Calculate(str);
}

HashResult Sha256::Calculate(Aws::IStream& stream)
{
    return m_hashImpl->Calculate(stream);
}

void Sha256::Update(unsigned char* buffer, size_t bufferSize)
{
    return m_hashImpl->Update(buffer, bufferSize);
}

HashResult Sha256::GetHash()
{
    return m_hashImpl->GetHash();
}
