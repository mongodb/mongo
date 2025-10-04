/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Lambda
{
namespace Model
{
  enum class LastUpdateStatus
  {
    NOT_SET,
    Successful,
    Failed,
    InProgress
  };

namespace LastUpdateStatusMapper
{
AWS_LAMBDA_API LastUpdateStatus GetLastUpdateStatusForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForLastUpdateStatus(LastUpdateStatus value);
} // namespace LastUpdateStatusMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
