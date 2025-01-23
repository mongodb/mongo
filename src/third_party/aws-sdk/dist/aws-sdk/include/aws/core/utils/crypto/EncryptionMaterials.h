/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/crypto/CryptoBuf.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/crypto/ContentCryptoMaterial.h>
#include <aws/core/NoResult.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            enum class CryptoErrors
            {
                ENCRYPT_CONTENT_ENCRYPTION_KEY_FAILED,
                DECRYPT_CONTENT_ENCRYPTION_KEY_FAILED,
                GENERATE_CONTENT_ENCRYPTION_KEY_FAILED
            };

            typedef Outcome<Aws::NoResult, Aws::Client::AWSError<CryptoErrors>> CryptoOutcome;

            class AWS_CORE_API EncryptionMaterials
            {
            public:
                virtual ~EncryptionMaterials();

                /*
                * Override this method to control how to encrypt the content encryption key (CEK). This occurs in place.
                */
                virtual CryptoOutcome EncryptCEK(ContentCryptoMaterial& contentCryptoMaterial) = 0;

                /*
                * Override this method to control how to decrypt the content encryption key (CEK). This occurs in place.
                */
                virtual CryptoOutcome DecryptCEK(ContentCryptoMaterial& contentCryptoMaterial) = 0;
            };
        }//namespace Crypto
    }//namespace Utils
}//namespace Aws
