/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Api.h>
#include <aws/crt/crypto/SymmetricCipher.h>

namespace Aws
{
    namespace Crt
    {
        namespace Crypto
        {
            SymmetricCipher::SymmetricCipher(aws_symmetric_cipher *cipher) noexcept
                : m_cipher(cipher, aws_symmetric_cipher_destroy), m_lastError(0)
            {
                if (cipher == nullptr)
                {
                    m_lastError = Crt::LastError();
                }
            }

            SymmetricCipher::operator bool() const noexcept
            {
                return m_cipher != nullptr ? aws_symmetric_cipher_is_good(m_cipher.get()) : false;
            }

            SymmetricCipherState SymmetricCipher::GetState() const noexcept
            {
                if (m_cipher == nullptr)
                {
                    return SymmetricCipherState::Error;
                }
                return static_cast<SymmetricCipherState>(aws_symmetric_cipher_get_state(m_cipher.get()));
            }

            bool SymmetricCipher::Encrypt(const ByteCursor &toEncrypt, ByteBuf &out) noexcept
            {
                if (!*this)
                {
                    m_lastError = AWS_ERROR_INVALID_STATE;
                    return false;
                }

                if (aws_symmetric_cipher_encrypt(m_cipher.get(), toEncrypt, &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = Aws::Crt::LastError();
                    return false;
                }

                return true;
            }

            bool SymmetricCipher::FinalizeEncryption(ByteBuf &out) noexcept
            {
                if (!*this)
                {
                    m_lastError = AWS_ERROR_INVALID_STATE;
                    return false;
                }

                if (aws_symmetric_cipher_finalize_encryption(m_cipher.get(), &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = Aws::Crt::LastError();
                    return false;
                }

                return true;
            }

            bool SymmetricCipher::Decrypt(const ByteCursor &toDecrypt, ByteBuf &out) noexcept
            {
                if (!*this)
                {
                    m_lastError = AWS_ERROR_INVALID_STATE;
                    return false;
                }

                if (aws_symmetric_cipher_decrypt(m_cipher.get(), toDecrypt, &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = Aws::Crt::LastError();
                    return false;
                }

                return true;
            }

            bool SymmetricCipher::FinalizeDecryption(ByteBuf &out) noexcept
            {
                if (!*this)
                {
                    m_lastError = AWS_ERROR_INVALID_STATE;
                    return false;
                }

                if (aws_symmetric_cipher_finalize_decryption(m_cipher.get(), &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = Aws::Crt::LastError();
                    return false;
                }

                return true;
            }

            bool SymmetricCipher::Reset() noexcept
            {
                if (m_cipher.get() == nullptr)
                {
                    m_lastError = AWS_ERROR_INVALID_STATE;
                    return false;
                }

                if (aws_symmetric_cipher_reset(m_cipher.get()) != AWS_OP_SUCCESS)
                {
                    m_lastError = Aws::Crt::LastError();
                    return false;
                }

                m_lastError = 0;

                return true;
            }

            ByteCursor SymmetricCipher::GetKey() const noexcept
            {
                return aws_symmetric_cipher_get_key(m_cipher.get());
            }

            ByteCursor SymmetricCipher::GetIV() const noexcept
            {
                return aws_symmetric_cipher_get_initialization_vector(m_cipher.get());
            }

            ByteCursor SymmetricCipher::GetTag() const noexcept
            {
                return aws_symmetric_cipher_get_tag(m_cipher.get());
            }

            void SymmetricCipher::SetTag(ByteCursor tag) const noexcept
            {
                return aws_symmetric_cipher_set_tag(m_cipher.get(), tag);
            }

            SymmetricCipher SymmetricCipher::CreateAES_256_CBC_Cipher(
                const Optional<ByteCursor> &key,
                const Optional<ByteCursor> &iv,
                Allocator *allocator) noexcept
            {
                return {aws_aes_cbc_256_new(
                    allocator, key.has_value() ? &key.value() : nullptr, iv.has_value() ? &iv.value() : nullptr)};
            }

            SymmetricCipher SymmetricCipher::CreateAES_256_CTR_Cipher(
                const Optional<ByteCursor> &key,
                const Optional<ByteCursor> &iv,
                Allocator *allocator) noexcept
            {
                return {aws_aes_ctr_256_new(
                    allocator, key.has_value() ? &key.value() : nullptr, iv.has_value() ? &iv.value() : nullptr)};
            }

            SymmetricCipher SymmetricCipher::CreateAES_256_GCM_Cipher(
                const Optional<ByteCursor> &key,
                const Optional<ByteCursor> &iv,
                const Optional<ByteCursor> &aad,
                Allocator *allocator) noexcept
            {
                return {aws_aes_gcm_256_new(
                    allocator,
                    key.has_value() ? &key.value() : nullptr,
                    iv.has_value() ? &iv.value() : nullptr,
                    aad.has_value() ? &aad.value() : nullptr)};
            }

            SymmetricCipher SymmetricCipher::CreateAES_256_KeyWrap_Cipher(
                const Optional<ByteCursor> &key,
                Allocator *allocator) noexcept
            {
                return {aws_aes_keywrap_256_new(allocator, key.has_value() ? &key.value() : nullptr)};
            }
        } // namespace Crypto
    } // namespace Crt
} // namespace Aws
