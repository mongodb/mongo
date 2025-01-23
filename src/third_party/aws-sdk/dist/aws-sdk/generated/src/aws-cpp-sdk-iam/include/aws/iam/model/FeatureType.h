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
  enum class FeatureType
  {
    NOT_SET,
    RootCredentialsManagement,
    RootSessions
  };

namespace FeatureTypeMapper
{
AWS_IAM_API FeatureType GetFeatureTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForFeatureType(FeatureType value);
} // namespace FeatureTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
