/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace IAM
{
namespace Model
{
  enum class GlobalEndpointTokenVersion
  {
    NOT_SET,
    v1Token,
    v2Token
  };

namespace GlobalEndpointTokenVersionMapper
{
AWS_IAM_API GlobalEndpointTokenVersion GetGlobalEndpointTokenVersionForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForGlobalEndpointTokenVersion(GlobalEndpointTokenVersion value);
} // namespace GlobalEndpointTokenVersionMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
