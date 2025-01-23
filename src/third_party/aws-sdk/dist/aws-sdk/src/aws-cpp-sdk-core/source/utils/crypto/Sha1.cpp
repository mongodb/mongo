/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/crypto/Sha1.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/crypto/Factories.h>

using namespace Aws::Utils::Crypto;

Sha1::Sha1() :
    m_hashImpl(CreateSha1Implementation())
{
}

Sha1::~Sha1()
{
}

HashResult Sha1::Calculate(const Aws::String& str)
{
    return m_hashImpl->Calculate(str);
}

HashResult Sha1::Calculate(Aws::IStream& stream)
{
    return m_hashImpl->Calculate(stream);
}

void Sha1::Update(unsigned char* buffer, size_t bufferSize)
{
    return m_hashImpl->Update(buffer, bufferSize);
}

HashResult Sha1::GetHash()
{
    return m_hashImpl->GetHash();
}