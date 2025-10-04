/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/crypto/Sha256HMAC.h>
#include <aws/core/utils/crypto/Factories.h>
#include <aws/core/utils/Outcome.h>

namespace Aws
{
namespace Utils
{
namespace Crypto
{

Sha256HMAC::Sha256HMAC() : 
    m_hmacImpl(CreateSha256HMACImplementation())
{
}

Sha256HMAC::~Sha256HMAC()
{
}

HashResult Sha256HMAC::Calculate(const Aws::Utils::ByteBuffer& toSign, const Aws::Utils::ByteBuffer& secret)
{
    return m_hmacImpl->Calculate(toSign, secret);
}

} // namespace Crypto
} // namespace Utils
} // namespace Aws