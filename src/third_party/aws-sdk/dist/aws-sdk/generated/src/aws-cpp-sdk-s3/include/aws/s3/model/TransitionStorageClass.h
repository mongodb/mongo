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
  enum class TransitionStorageClass
  {
    NOT_SET,
    GLACIER,
    STANDARD_IA,
    ONEZONE_IA,
    INTELLIGENT_TIERING,
    DEEP_ARCHIVE,
    GLACIER_IR
  };

namespace TransitionStorageClassMapper
{
AWS_S3_API TransitionStorageClass GetTransitionStorageClassForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForTransitionStorageClass(TransitionStorageClass value);
} // namespace TransitionStorageClassMapper
} // namespace Model
} // namespace S3
} // namespace Aws
