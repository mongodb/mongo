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
  enum class BucketCannedACL
  {
    NOT_SET,
    private_,
    public_read,
    public_read_write,
    authenticated_read
  };

namespace BucketCannedACLMapper
{
AWS_S3_API BucketCannedACL GetBucketCannedACLForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForBucketCannedACL(BucketCannedACL value);
} // namespace BucketCannedACLMapper
} // namespace Model
} // namespace S3
} // namespace Aws
