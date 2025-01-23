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
  enum class MFADelete
  {
    NOT_SET,
    Enabled,
    Disabled
  };

namespace MFADeleteMapper
{
AWS_S3_API MFADelete GetMFADeleteForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForMFADelete(MFADelete value);
} // namespace MFADeleteMapper
} // namespace Model
} // namespace S3
} // namespace Aws
