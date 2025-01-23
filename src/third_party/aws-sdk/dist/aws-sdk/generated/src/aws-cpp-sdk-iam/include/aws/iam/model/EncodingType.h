/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace IAM
{
namespace Model
{
  enum class EncodingType
  {
    NOT_SET,
    SSH,
    PEM
  };

namespace EncodingTypeMapper
{
AWS_IAM_API EncodingType GetEncodingTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForEncodingType(EncodingType value);
} // namespace EncodingTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
