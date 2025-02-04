/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace S3
{
namespace Model
{
  enum class MFADeleteStatus
  {
    NOT_SET,
    Enabled,
    Disabled
  };

namespace MFADeleteStatusMapper
{
AWS_S3_API MFADeleteStatus GetMFADeleteStatusForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForMFADeleteStatus(MFADeleteStatus value);
} // namespace MFADeleteStatusMapper
} // namespace Model
} // namespace S3
} // namespace Aws
