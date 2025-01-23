/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/CryptoStream.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            static const char* CLASS_TAG = "Aws::Utils::Crypto::SymmetricCryptoStream";

            SymmetricCryptoStream::SymmetricCryptoStream(Aws::IStream& src, CipherMode mode, SymmetricCipher& cipher, size_t bufSize) :
                Aws::IOStream(m_cryptoBuf = Aws::New<SymmetricCryptoBufSrc>(CLASS_TAG, src, cipher, mode, bufSize)), m_hasOwnership(true)
            {
            }

            SymmetricCryptoStream::SymmetricCryptoStream(Aws::OStream& sink, CipherMode mode, SymmetricCipher& cipher, size_t bufSize, int16_t blockOffset) :
                    Aws::IOStream(m_cryptoBuf = Aws::New<SymmetricCryptoBufSink>(CLASS_TAG, sink, cipher, mode, bufSize, blockOffset)), m_hasOwnership(true)
            {
            }

            SymmetricCryptoStream::SymmetricCryptoStream(Aws::Utils::Crypto::SymmetricCryptoBufSrc& bufSrc) :
                    Aws::IOStream(&bufSrc), m_cryptoBuf(&bufSrc), m_hasOwnership(false)
            {
            }

            SymmetricCryptoStream::SymmetricCryptoStream(Aws::Utils::Crypto::SymmetricCryptoBufSink& bufSink) :
                    Aws::IOStream(&bufSink), m_cryptoBuf(&bufSink), m_hasOwnership(false)
            {
            }

            SymmetricCryptoStream::~SymmetricCryptoStream()
            {
                Finalize();

                if(m_hasOwnership && m_cryptoBuf)
                {
                    Aws::Delete(m_cryptoBuf);
                }
            }

            void SymmetricCryptoStream::Finalize()
            {
                assert(m_cryptoBuf);
                m_cryptoBuf->Finalize();
            }
        }
    }
}