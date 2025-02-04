/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

  /*
  * Interface for Sha256 encryptor and hmac
  */
#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/crypto/Hash.h>
#include <aws/core/utils/Outcome.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            /**
             * Md5 hash implementation
             */
            class AWS_CORE_API MD5 : public Hash
            {
            public:
                /**
                 * Initializes platform crypto libs for md5
                 */
                MD5();
                virtual ~MD5();

                /**
                * Calculates an MD5 hash
                */
                virtual HashResult Calculate(const Aws::String& str) override;

                /**
                * Calculates a Hash digest on a stream (the entire stream is read)
                */
                virtual HashResult Calculate(Aws::IStream& stream) override;

                /**
                 * Updates a Hash digest
                 */
                virtual void Update(unsigned char* buffer, size_t bufferSize) override;

                /**
                 * Get the result in the current value
                 */
                virtual HashResult GetHash() override;

            private:
                std::shared_ptr<Hash> m_hashImpl;
            };

        } // namespace Crypto
    } // namespace Utils
} // namespace Aws

