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
  enum class ObjectOwnership
  {
    NOT_SET,
    BucketOwnerPreferred,
    ObjectWriter,
    BucketOwnerEnforced
  };

namespace ObjectOwnershipMapper
{
AWS_S3_API ObjectOwnership GetObjectOwnershipForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForObjectOwnership(ObjectOwnership value);
} // namespace ObjectOwnershipMapper
} // namespace Model
} // namespace S3
} // namespace Aws
