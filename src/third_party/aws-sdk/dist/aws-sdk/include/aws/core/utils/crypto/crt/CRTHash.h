/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/MD5.h>
#include <aws/core/utils/crypto/Sha1.h>
#include <aws/core/utils/crypto/Sha256.h>
#include <aws/core/utils/crypto/Sha256HMAC.h>

#include <aws/crt/crypto/Hash.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            class AWS_CORE_API CRTHash : public Hash
            {
            public:
                ~CRTHash() override = default;
                CRTHash(const CRTHash &) = delete;
                CRTHash &operator=(const CRTHash &) = delete;
                CRTHash(CRTHash &&) = default;
                CRTHash &operator=(CRTHash &&) = default;
                explicit CRTHash(Crt::Crypto::Hash &&);

                HashResult Calculate(const String &str) override;

                HashResult Calculate(IStream &stream) override;

                void Update(unsigned char *string, size_t bufferSize) override;

                HashResult GetHash() override;
            private:
                Crt::Crypto::Hash m_hash;
            };
        }
    }
}
