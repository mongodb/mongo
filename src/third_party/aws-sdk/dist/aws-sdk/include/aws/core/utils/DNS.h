/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
    namespace Utils
    {
        AWS_CORE_API bool IsValidDnsLabel(const Aws::String& label);

        AWS_CORE_API bool IsValidHost(const Aws::String& host);
    }
}
