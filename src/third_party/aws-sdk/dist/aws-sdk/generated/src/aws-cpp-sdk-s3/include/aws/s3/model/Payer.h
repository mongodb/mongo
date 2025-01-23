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
  enum class Payer
  {
    NOT_SET,
    Requester,
    BucketOwner
  };

namespace PayerMapper
{
AWS_S3_API Payer GetPayerForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForPayer(Payer value);
} // namespace PayerMapper
} // namespace Model
} // namespace S3
} // namespace Aws
