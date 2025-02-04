#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>

namespace Aws
{
    namespace Crt
    {
        namespace Crypto
        {
            bool AWS_CRT_CPP_API GenerateRandomBytes(ByteBuf &output, size_t lengthToGenerate);
        }
    } // namespace Crt
} // namespace Aws
