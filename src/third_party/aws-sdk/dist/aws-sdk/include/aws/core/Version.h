/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Version
{
    AWS_CORE_API const char* GetVersionString();
    AWS_CORE_API unsigned GetVersionMajor();
    AWS_CORE_API unsigned GetVersionMinor();
    AWS_CORE_API unsigned GetVersionPatch();
    AWS_CORE_API const char* GetCompilerVersionString();
    AWS_CORE_API const char* GetCPPStandard();
} //namespace Version
} //namespace Aws
