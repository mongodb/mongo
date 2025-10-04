/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

  /*
  * Interface for Sha256 encryptor and hmac
  */
#pragma once

#ifdef __APPLE__

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif // __clang__

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif // __GNUC__

#endif // __APPLE__

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/crypto/HMAC.h>
#include <aws/core/utils/memory/AWSMemory.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            /**
             * Sha256 HMAC implementation
             */
            class AWS_CORE_API Sha256HMAC : public HMAC
            {
            public:
                /**
                 * initializes platform specific libs.
                 */
                Sha256HMAC();
                virtual ~Sha256HMAC();

                /**
                * Calculates a SHA256 HMAC digest (not hex encoded)
                */
                virtual HashResult Calculate(const Aws::Utils::ByteBuffer& toSign, const Aws::Utils::ByteBuffer& secret) override;

            private:

                std::shared_ptr< HMAC > m_hmacImpl;
            };

        } // namespace Sha256
    } // namespace Utils
} // namespace Aws

