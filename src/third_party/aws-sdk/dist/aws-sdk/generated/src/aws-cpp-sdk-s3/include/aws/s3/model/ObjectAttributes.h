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
  enum class ObjectAttributes
  {
    NOT_SET,
    ETag,
    Checksum,
    ObjectParts,
    StorageClass,
    ObjectSize
  };

namespace ObjectAttributesMapper
{
AWS_S3_API ObjectAttributes GetObjectAttributesForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForObjectAttributes(ObjectAttributes value);
} // namespace ObjectAttributesMapper
} // namespace Model
} // namespace S3
} // namespace Aws
