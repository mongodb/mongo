/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
* SPDX-License-Identifier: Apache-2.0.
*/
#include <aws/core/utils/crypto/crt/CRTSecureRandom.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/crt/crypto/SecureRandom.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            static const char SECURE_RANDOM_BYTES_LOG_TAG[] = "CRTSecureRandomBytes";

            void CRTSecureRandomBytes::GetBytes(unsigned char *buffer, std::size_t bufferSize)
            {
                auto outputBuf = Crt::ByteBufFromEmptyArray(buffer, bufferSize);
                if (!Crt::Crypto::GenerateRandomBytes(outputBuf, bufferSize))
                {
                    AWS_LOGSTREAM_ERROR(SECURE_RANDOM_BYTES_LOG_TAG, "CRT Generate Random Bytes Failed")
                    AWS_UNREACHABLE();
                }
            }
        }
    }
}
