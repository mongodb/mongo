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
  enum class SortKeyType
  {
    NOT_SET,
    SERVICE_NAMESPACE_ASCENDING,
    SERVICE_NAMESPACE_DESCENDING,
    LAST_AUTHENTICATED_TIME_ASCENDING,
    LAST_AUTHENTICATED_TIME_DESCENDING
  };

namespace SortKeyTypeMapper
{
AWS_IAM_API SortKeyType GetSortKeyTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForSortKeyType(SortKeyType value);
} // namespace SortKeyTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
