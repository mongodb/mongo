/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/ContentCryptoMaterial.h>
#include <aws/core/utils/crypto/Cipher.h>

using namespace Aws::Utils::Crypto;

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            ContentCryptoMaterial::ContentCryptoMaterial() :
                m_cryptoTagLength(0), m_keyWrapAlgorithm(KeyWrapAlgorithm::NONE), m_contentCryptoScheme(ContentCryptoScheme::NONE)
            {
            }

            ContentCryptoMaterial::ContentCryptoMaterial(ContentCryptoScheme contentCryptoScheme) :
                m_contentEncryptionKey(SymmetricCipher::GenerateKey()), m_cryptoTagLength(0), m_keyWrapAlgorithm(KeyWrapAlgorithm::NONE), m_contentCryptoScheme(contentCryptoScheme)
            {

            }

            ContentCryptoMaterial::ContentCryptoMaterial(const Aws::Utils::CryptoBuffer & cek, ContentCryptoScheme contentCryptoScheme) :
                m_contentEncryptionKey(cek), m_cryptoTagLength(0), m_keyWrapAlgorithm(KeyWrapAlgorithm::NONE), m_contentCryptoScheme(contentCryptoScheme)
            {

            }
        }
    }
}
