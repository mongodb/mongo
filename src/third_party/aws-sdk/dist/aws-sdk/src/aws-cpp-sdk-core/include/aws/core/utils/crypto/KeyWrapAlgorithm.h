/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            enum class KeyWrapAlgorithm
            {
                KMS, // Deprecated
                KMS_CONTEXT,
                AES_KEY_WRAP, // Deprecated
                AES_GCM,
                NONE
            };

            namespace KeyWrapAlgorithmMapper
            {
                AWS_CORE_API KeyWrapAlgorithm GetKeyWrapAlgorithmForName(const Aws::String& name);

                AWS_CORE_API Aws::String GetNameForKeyWrapAlgorithm(KeyWrapAlgorithm enumValue);
            }
        } //namespace Crypto

    }//namespace Utils
}//namespace Aws
