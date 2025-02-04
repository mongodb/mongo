#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/symmetric_cipher.h>
#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>

struct aws_symmetric_cipher;

namespace Aws
{
    namespace Crt
    {
        namespace Crypto
        {
            static const size_t AES_256_CIPHER_BLOCK_SIZE = 16u;
            static const size_t AES_256_KEY_SIZE_BYTES = 32u;

            enum class SymmetricCipherState
            {
                Ready = AWS_SYMMETRIC_CIPHER_READY,
                Finalized = AWS_SYMMETRIC_CIPHER_FINALIZED,
                Error = AWS_SYMMETRIC_CIPHER_ERROR,
            };

            class AWS_CRT_CPP_API SymmetricCipher final
            {
              public:
                SymmetricCipher(const SymmetricCipher &) = delete;
                SymmetricCipher &operator=(const SymmetricCipher &) = delete;
                SymmetricCipher(SymmetricCipher &&) noexcept = default;
                SymmetricCipher &operator=(SymmetricCipher &&) noexcept = default;

                /**
                 * Creates an AES 256 CBC mode cipher using a provided key and iv.
                 * Key must be 32 bytes. If key or iv are not provided, they will be generated.
                 */
                static SymmetricCipher CreateAES_256_CBC_Cipher(
                    const Optional<ByteCursor> &key = Optional<ByteCursor>(),
                    const Optional<ByteCursor> &iv = Optional<ByteCursor>(),
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Creates an AES 256 CTR mode cipher using a provided key and iv.
                 * If key and iv are not provided, they will be generated.
                 */
                static SymmetricCipher CreateAES_256_CTR_Cipher(
                    const Optional<ByteCursor> &key = Optional<ByteCursor>(),
                    const Optional<ByteCursor> &iv = Optional<ByteCursor>(),
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Creates an AES 256 GCM mode cipher using a provided key, iv, tag, and aad if provided.
                 * Key and iv will be generated if not provided.
                 * AAD values are not generated.
                 * Provide AAD if you need to provide additional auth info.
                 */
                static SymmetricCipher CreateAES_256_GCM_Cipher(
                    const Optional<ByteCursor> &key = Optional<ByteCursor>(),
                    const Optional<ByteCursor> &iv = Optional<ByteCursor>(),
                    const Optional<ByteCursor> &aad = Optional<ByteCursor>(),
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Creates an AES 256 Keywrap mode cipher using key if provided.
                 * If a key is not provided, one will be generated.
                 */
                static SymmetricCipher CreateAES_256_KeyWrap_Cipher(
                    const Optional<ByteCursor> &key = Optional<ByteCursor>(),
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Returns true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept;

                /**
                 * Returns current state of the cipher instance. ready to be used, finalized, or in a error state.
                 * If the cipher is in a finalized or error state it may not be used anymore
                 **/
                SymmetricCipherState GetState() const noexcept;

                /**
                 * Returns the value of the last aws error encountered by operations on this instance.
                 */
                inline int LastError() const noexcept { return m_lastError; }

                /**
                 * Encrypts the value in toEncrypt and stores any immediate results in out. Out can be dynamically
                 * re-sized if out is a dynamic byte buf. Otherwise, make sure the size of out is at least 2 blocks
                 * larger than the input to allow for padding.
                 *
                 * Returns true on success. Call
                 * LastError() for the reason this call failed.
                 */
                bool Encrypt(const ByteCursor &toEncrypt, ByteBuf &out) noexcept;

                /**
                 * Encrypts any remaining data on the cipher and stores the output in out. Out can be dynamically
                 * re-sized if out is a dynamic byte buf. Otherwise, make sure the size of out is at least 2 blocks
                 * for CBC, CTR, and GCM modes and 40 bytes for KeyWrap.
                 *
                 * Returns true on success. Call
                 * LastError() for the reason this call failed.
                 */
                bool FinalizeEncryption(ByteBuf &out) noexcept;

                /**
                 * Decrypts the value in toEncrypt and stores any immediate results in out. Out can be dynamically
                 * re-sized if out is a dynamic byte buf. Otherwise, make sure the size of out is at least 1 block
                 * larger than the input to allow for padding. Returns true on success. Call LastError() for the reason
                 * this call failed.
                 */
                bool Decrypt(const ByteCursor &toDecrypt, ByteBuf &out) noexcept;

                /**
                 * Decrypts any remaining data on the cipher and stores the output in out. Out can be dynamically
                 * re-sized if out is a dynamic byte buf. Otherwise, make sure the size of out is at least 2 blocks
                 * for CBC, CTR, GCM, and Keywrap modes.
                 *
                 * Returns true on success. Call
                 * LastError() for the reason this call failed.
                 */
                bool FinalizeDecryption(ByteBuf &out) noexcept;

                /**
                 * Reset to cipher to new state.
                 */
                bool Reset() noexcept;

                /**
                 * Returns the key used for this cipher. This key is not copied from the cipher so do not mutate this
                 * data. Copy if you need to pass it around anywhere.
                 */
                ByteCursor GetKey() const noexcept;

                /**
                 * Returns the initialization vector used for this cipher.
                 * This IV is not copied from the cipher so do not mutate this
                 * data. Copy if you need to pass it around anywhere.
                 */
                ByteCursor GetIV() const noexcept;

                /**
                 * Returns the encryption tag generated during encryption operations for this cipher in GCM mode.
                 * This tag is not copied from the cipher so do not mutate this
                 * data. Copy if you need to pass it around anywhere.
                 */
                ByteCursor GetTag() const noexcept;

                /**
                 * Sets the tag used during decryption operations for this cipher in GCM mode.
                 * No-op outside of GCM mode. In GCM mode, encrypt operation overrides the value of the tag.
                 */
                void SetTag(ByteCursor tag) const noexcept;

              private:
                SymmetricCipher(aws_symmetric_cipher *cipher) noexcept;
                ScopedResource<struct aws_symmetric_cipher> m_cipher;
                int m_lastError;
            };
        } // namespace Crypto
    } // namespace Crt
} // namespace Aws
