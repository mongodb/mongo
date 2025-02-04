/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/MD5.h>
#include <aws/core/utils/crypto/Sha1.h>
#include <aws/core/utils/crypto/Sha256.h>
#include <aws/core/utils/crypto/Sha256HMAC.h>

#include <aws/crt/crypto/HMAC.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            class AWS_CORE_API CRTSha256Hmac : public HMAC
            {
            public:
                HashResult Calculate(const Aws::Utils::ByteBuffer& toSign, const Aws::Utils::ByteBuffer& secret) override;
            };
        }
    }
}
