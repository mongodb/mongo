/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Environment
{
    /**
    * shim for getenv
    */
    AWS_CORE_API Aws::String GetEnv(const char* name);

} // namespace Environment
} // namespace Aws

