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
  enum class ObjectStorageClass
  {
    NOT_SET,
    STANDARD,
    REDUCED_REDUNDANCY,
    GLACIER,
    STANDARD_IA,
    ONEZONE_IA,
    INTELLIGENT_TIERING,
    DEEP_ARCHIVE,
    OUTPOSTS,
    GLACIER_IR,
    SNOW,
    EXPRESS_ONEZONE
  };

namespace ObjectStorageClassMapper
{
AWS_S3_API ObjectStorageClass GetObjectStorageClassForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForObjectStorageClass(ObjectStorageClass value);
} // namespace ObjectStorageClassMapper
} // namespace Model
} // namespace S3
} // namespace Aws
