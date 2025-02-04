/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace OSVersionInfo
{
    /**
    * computing the version string for the current running operating system.
    */
    AWS_CORE_API Aws::String ComputeOSVersionString();

    /**
    * Get architecture of the current running operating system.
    */
    AWS_CORE_API Aws::String ComputeOSVersionArch();

    /**
    * runs a (shell) command string and returns the output; not needed on windows currently
    */
    AWS_CORE_API Aws::String GetSysCommandOutput(const char* command);

} // namespace OSVersionInfo
} // namespace Aws

