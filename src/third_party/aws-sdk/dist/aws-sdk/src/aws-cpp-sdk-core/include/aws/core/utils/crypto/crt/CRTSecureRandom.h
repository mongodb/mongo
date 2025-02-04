/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
* SPDX-License-Identifier: Apache-2.0.
*/
#pragma once
#include <aws/core/utils/crypto/SecureRandom.h>
#include <aws/core/Core_EXPORTS.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            class CRTSecureRandomBytes : public SecureRandomBytes
            {
            public:
                CRTSecureRandomBytes() = default;
                ~CRTSecureRandomBytes() override = default;

                /**
                 * fill in buffer of size bufferSize with random bytes
                 */
                void GetBytes(unsigned char *buffer, std::size_t bufferSize) override;
            };
        }
    }
}
