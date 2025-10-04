/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/crypto/Hash.h>
#include <aws/core/utils/crypto/HashResult.h>
#include <aws/core/utils/Outcome.h>

namespace Aws {
    namespace Utils {
        namespace Crypto {
            /**
             * A hash implementation that stores a pre-calculated hash
             * behind the Hash interface. It is a read through wrapper
             * around the checksum value and no hashing are actually done.
             */
            class AWS_CORE_API PrecalculatedHash : public Hash {
            public:
                explicit PrecalculatedHash(const Aws::String &hash);
                ~PrecalculatedHash() override;
                HashResult Calculate(const Aws::String &str) override;
                HashResult Calculate(Aws::IStream &stream) override;
                void Update(unsigned char *string, size_t bufferSize) override;
                HashResult GetHash() override;

            private:
                Aws::String m_hashString;
                HashResult m_decodedHashString;

            };
        }
    }
}
