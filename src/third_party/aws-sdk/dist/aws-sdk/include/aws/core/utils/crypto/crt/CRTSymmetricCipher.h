/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/Cipher.h>
#include <aws/crt/crypto/SymmetricCipher.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto {
            class AWS_CORE_API CRTSymmetricCipher : public SymmetricCipher {
            public:
                CRTSymmetricCipher(const CRTSymmetricCipher &) = delete;

                CRTSymmetricCipher &operator=(const CRTSymmetricCipher &) = delete;

                CRTSymmetricCipher(CRTSymmetricCipher &&) = default;

                CRTSymmetricCipher &operator=(CRTSymmetricCipher &&) = default;

                ~CRTSymmetricCipher() override = default;

                CryptoBuffer EncryptBuffer(const CryptoBuffer &unEncryptedData) override;

                CryptoBuffer FinalizeEncryption() override;

                CryptoBuffer DecryptBuffer(const CryptoBuffer &encryptedData) override;

                CryptoBuffer FinalizeDecryption() override;

                void Reset() override;

                bool Good() const override;

                // @private but we need make_shared support.
                CRTSymmetricCipher(Crt::Crypto::SymmetricCipher &&);
            private:
                Crt::Crypto::SymmetricCipher m_cipher;
                mutable CryptoBuffer m_lastFetchedIv;
                mutable CryptoBuffer m_lastFetchedTag;
            };
        }
    }
}
