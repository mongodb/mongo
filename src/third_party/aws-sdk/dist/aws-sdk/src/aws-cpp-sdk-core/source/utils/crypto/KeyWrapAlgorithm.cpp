/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/KeyWrapAlgorithm.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>
#include <aws/core/Globals.h>

using namespace Aws::Utils;

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            namespace KeyWrapAlgorithmMapper
            {
                static const int keyWrapAlgorithm_KMS_HASH = HashingUtils::HashString("kms");
                static const int keyWrapAlgorithm_KMS_CONTEXT_HASH = HashingUtils::HashString("kms+context");
                static const int keyWrapAlgorithm_KeyWrap_HASH = HashingUtils::HashString("AESWrap");
                static const int keyWrapAlgorithm_AES_GCM_HASH = HashingUtils::HashString("AES/GCM");

                KeyWrapAlgorithm GetKeyWrapAlgorithmForName(const Aws::String& name)
                {
                    int hashcode = HashingUtils::HashString(name.c_str());
                    if (hashcode == keyWrapAlgorithm_KMS_HASH)
                    {
                        return KeyWrapAlgorithm::KMS;
                    }
                    else if (hashcode == keyWrapAlgorithm_KMS_CONTEXT_HASH) 
                    {
                        return KeyWrapAlgorithm::KMS_CONTEXT;
                    }
                    else if (hashcode == keyWrapAlgorithm_KeyWrap_HASH)
                    {
                        return KeyWrapAlgorithm::AES_KEY_WRAP;
                    } 
                    else if (hashcode == keyWrapAlgorithm_AES_GCM_HASH)
                    {
                        return KeyWrapAlgorithm::AES_GCM;
                    }
                    assert(0);
                    return KeyWrapAlgorithm::NONE;
                }

                Aws::String GetNameForKeyWrapAlgorithm(KeyWrapAlgorithm enumValue)
                {
                    switch (enumValue)
                    {
                    case KeyWrapAlgorithm::KMS:
                        return "kms";
                    case KeyWrapAlgorithm::KMS_CONTEXT:
                        return "kms+context";
                    case KeyWrapAlgorithm::AES_KEY_WRAP:
                        return "AESWrap";
                    case KeyWrapAlgorithm::AES_GCM:
                        return "AES/GCM";
                    default:
                        assert(0);
                    }
                    return "";
                }
            }//namespace KeyWrapAlgorithmMapper
        }//namespace Crypto
    }//namespace Utils
}//namespace Aws
