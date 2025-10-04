/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/crt/CRTHMAC.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {

            HashResult CRTSha256Hmac::Calculate(const Aws::Utils::ByteBuffer &toSign, const Aws::Utils::ByteBuffer &secret)
            {
                auto toSignCur = Crt::ByteCursorFromArray(toSign.GetUnderlyingData(), toSign.GetLength());
                auto secretCur = Crt::ByteCursorFromArray(secret.GetUnderlyingData(), secret.GetLength());

                ByteBuffer resultBuf(Crt::Crypto::SHA256_HMAC_DIGEST_SIZE);
                Crt::ByteBuf outBuf = Crt::ByteBufFromEmptyArray(resultBuf.GetUnderlyingData(), resultBuf.GetSize());

                if (Crt::Crypto::ComputeSHA256HMAC(secretCur, toSignCur,outBuf))
                {
                    resultBuf.SetLength(outBuf.len);
                    return resultBuf;
                }

                return false;
            }
        }
    }
}
