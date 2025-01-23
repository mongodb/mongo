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
            enum class ContentCryptoScheme
            {
                CBC,
                CTR,
                GCM,
                NONE
            };

            namespace ContentCryptoSchemeMapper
            {
                AWS_CORE_API ContentCryptoScheme GetContentCryptoSchemeForName(const Aws::String& name);

                AWS_CORE_API Aws::String GetNameForContentCryptoScheme(ContentCryptoScheme enumValue);
            }
        } //namespace Crypto

    }//namespace Utils
}//namespace Aws