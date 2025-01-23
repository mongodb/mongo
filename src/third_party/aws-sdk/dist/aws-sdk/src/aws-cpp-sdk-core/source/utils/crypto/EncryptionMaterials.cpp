/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/crypto/EncryptionMaterials.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            //this is here to force the linker to behave correctly since this is an interface that will need to cross the dll 
            //boundary.
            EncryptionMaterials::~EncryptionMaterials()
            {}
        }
    }
}