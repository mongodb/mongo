/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/StringUtils.h>

#include <aws/common/hash_table.h>

namespace Aws
{
    namespace Crt
    {
        size_t HashString(const char *str) noexcept
        {
            return (size_t)aws_hash_c_string(str);
        }
    } // namespace Crt
} // namespace Aws
