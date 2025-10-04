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
  enum class TransitionDefaultMinimumObjectSize
  {
    NOT_SET,
    varies_by_storage_class,
    all_storage_classes_128K
  };

namespace TransitionDefaultMinimumObjectSizeMapper
{
AWS_S3_API TransitionDefaultMinimumObjectSize GetTransitionDefaultMinimumObjectSizeForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForTransitionDefaultMinimumObjectSize(TransitionDefaultMinimumObjectSize value);
} // namespace TransitionDefaultMinimumObjectSizeMapper
} // namespace Model
} // namespace S3
} // namespace Aws
