/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/crypto/PrecalculatedHash.h>
#include <aws/crt/Types.h>

using namespace Aws::Utils::Crypto;
using namespace Aws::Crt;

PrecalculatedHash::PrecalculatedHash(const Aws::String &hash) : m_hashString(hash)
{
    const auto decoded = Base64Decode(hash.c_str());
    ByteBuffer buf{decoded.data(), decoded.size()};
    m_decodedHashString = std::move(buf);
}

PrecalculatedHash::~PrecalculatedHash() = default;

HashResult PrecalculatedHash::Calculate(const Aws::String& str)
{
    AWS_UNREFERENCED_PARAM(str);
    return m_decodedHashString;
}

HashResult PrecalculatedHash::Calculate(Aws::IStream& stream)
{
    AWS_UNREFERENCED_PARAM(stream);
    return m_decodedHashString;
}

void PrecalculatedHash::Update(unsigned char* string, size_t bufferSize)
{
    AWS_UNREFERENCED_PARAM(string);
    AWS_UNREFERENCED_PARAM(bufferSize);
}

HashResult PrecalculatedHash::GetHash()
{
    return m_decodedHashString;
}
