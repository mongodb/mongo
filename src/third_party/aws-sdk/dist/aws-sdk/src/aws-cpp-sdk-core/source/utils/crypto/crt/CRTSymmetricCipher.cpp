/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/crt/CRTSymmetricCipher.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            CRTSymmetricCipher::CRTSymmetricCipher(Crt::Crypto::SymmetricCipher &&toMove) : SymmetricCipher(), m_cipher(std::move(toMove))
            {
                if (m_cipher)
                {
                    auto ivCur = m_cipher.GetIV();
                    m_initializationVector = CryptoBuffer(ivCur.ptr, ivCur.len);

                    auto keyCur = m_cipher.GetKey();
                    m_key = CryptoBuffer(keyCur.ptr, keyCur.len);

                    auto tagCur = m_cipher.GetTag();

                    if (tagCur.len)
                    {
                        m_tag = CryptoBuffer(tagCur.ptr, tagCur.len);
                    }
                }
            }

            CryptoBuffer CRTSymmetricCipher::EncryptBuffer(const CryptoBuffer &unEncryptedData)
            {
                auto resultBuffer = Crt::ByteBufInit(get_aws_allocator(), Crt::Crypto::AES_256_CIPHER_BLOCK_SIZE);
                Crt::ByteCursor toEncrypt = Crt::ByteCursorFromArray(unEncryptedData.GetUnderlyingData(), unEncryptedData.GetLength());

                if (m_cipher.Encrypt(toEncrypt, resultBuffer))
                {
                    return {std::move(resultBuffer)};
                }
                Crt::ByteBufDelete(resultBuffer);
                return {0};
            }

            CryptoBuffer CRTSymmetricCipher::FinalizeEncryption()
            {
                auto resultBuffer = Crt::ByteBufInit(get_aws_allocator(), Crt::Crypto::AES_256_CIPHER_BLOCK_SIZE);

                if (m_cipher.FinalizeEncryption(resultBuffer))
                {
                    auto tagCur = m_cipher.GetTag();
                    m_tag = CryptoBuffer(tagCur.ptr, tagCur.len);
                    return {std::move(resultBuffer)};
                }
                Crt::ByteBufDelete(resultBuffer);
                return {0};
            }

            CryptoBuffer CRTSymmetricCipher::DecryptBuffer(const CryptoBuffer &encryptedData)
            {
                auto resultBuffer = Crt::ByteBufInit(get_aws_allocator(), Crt::Crypto::AES_256_CIPHER_BLOCK_SIZE);
                Crt::ByteCursor toDecrypt = encryptedData.GetUnderlyingData() != nullptr ?
                    Crt::ByteCursorFromArray(encryptedData.GetUnderlyingData(), encryptedData.GetLength()):
                    Crt::ByteCursorFromCString("");

                if (m_cipher.Decrypt(toDecrypt, resultBuffer))
                {
                    return {std::move(resultBuffer)};
                }
                Crt::ByteBufDelete(resultBuffer);
                return (0);
            }

            CryptoBuffer CRTSymmetricCipher::FinalizeDecryption()
            {;
                auto resultBuffer = Crt::ByteBufInit(get_aws_allocator(), Crt::Crypto::AES_256_CIPHER_BLOCK_SIZE);

                if (m_cipher.FinalizeDecryption(resultBuffer))
                {
                    return {std::move(resultBuffer)};
                }
                Crt::ByteBufDelete(resultBuffer);
                return {0};
            }

            void CRTSymmetricCipher::Reset()
            {
                m_lastFetchedTag = GetTag();
                m_cipher.Reset();
                m_cipher.SetTag(Crt::ByteCursorFromArray(m_lastFetchedTag.GetUnderlyingData(), m_lastFetchedTag.GetLength()));
            }

            bool CRTSymmetricCipher::Good() const
            {
                return m_cipher.GetState() == Crt::Crypto::SymmetricCipherState::Finalized || m_cipher.operator bool();
            }
        }
    }
}
