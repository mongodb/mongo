/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/crypto/CRC32.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/crypto/Factories.h>

using namespace Aws::Utils::Crypto;

CRC32::CRC32() :
    m_hashImpl(CreateCRC32Implementation())
{
}

CRC32::~CRC32()
{
}

HashResult CRC32::Calculate(const Aws::String& str)
{
    return m_hashImpl->Calculate(str);
}

HashResult CRC32::Calculate(Aws::IStream& stream)
{
    return m_hashImpl->Calculate(stream);
}

void CRC32::Update(unsigned char* buffer, size_t bufferSize)
{
    m_hashImpl->Update(buffer, bufferSize);
}

HashResult CRC32::GetHash()
{
    return m_hashImpl->GetHash();
}

CRC32C::CRC32C() :
    m_hashImpl(CreateCRC32CImplementation())
{
}

CRC32C::~CRC32C()
{
}

HashResult CRC32C::Calculate(const Aws::String& str)
{
    return m_hashImpl->Calculate(str);
}

HashResult CRC32C::Calculate(Aws::IStream& stream)
{
    return m_hashImpl->Calculate(stream);
}


void CRC32C::Update(unsigned char* buffer, size_t bufferSize)
{
    m_hashImpl->Update(buffer, bufferSize);
}

HashResult CRC32C::GetHash()
{
    return m_hashImpl->GetHash();
}
