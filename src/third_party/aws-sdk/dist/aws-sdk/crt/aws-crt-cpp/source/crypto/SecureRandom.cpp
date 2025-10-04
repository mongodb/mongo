/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/crypto/SecureRandom.h>

#include <aws/common/device_random.h>

namespace Aws
{
    namespace Crt
    {
        namespace Crypto
        {
            bool GenerateRandomBytes(ByteBuf &output, size_t lengthToGenerate)
            {
                return aws_device_random_buffer_append(&output, lengthToGenerate) == AWS_OP_SUCCESS;
            }
        } // namespace Crypto
    } // namespace Crt
} // namespace Aws
