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
  enum class JSONType
  {
    NOT_SET,
    DOCUMENT,
    LINES
  };

namespace JSONTypeMapper
{
AWS_S3_API JSONType GetJSONTypeForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForJSONType(JSONType value);
} // namespace JSONTypeMapper
} // namespace Model
} // namespace S3
} // namespace Aws
