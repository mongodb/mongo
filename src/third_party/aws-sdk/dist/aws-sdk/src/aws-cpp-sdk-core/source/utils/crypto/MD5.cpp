/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/crypto/MD5.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/crypto/Factories.h>

using namespace Aws::Utils::Crypto;


MD5::MD5() :
    m_hashImpl(CreateMD5Implementation())
{
}

MD5::~MD5()
{
}

HashResult MD5::Calculate(const Aws::String& str)
{
    return m_hashImpl->Calculate(str);
}

HashResult MD5::Calculate(Aws::IStream& stream)
{
    return m_hashImpl->Calculate(stream);
}

void MD5::Update(unsigned char* buffer, size_t bufferSize)
{
    return m_hashImpl->Update(buffer, bufferSize);
}

HashResult MD5::GetHash()
{
    return m_hashImpl->GetHash();
}
