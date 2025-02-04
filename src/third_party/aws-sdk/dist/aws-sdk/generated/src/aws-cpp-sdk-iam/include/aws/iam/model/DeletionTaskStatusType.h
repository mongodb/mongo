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
  enum class DeletionTaskStatusType
  {
    NOT_SET,
    SUCCEEDED,
    IN_PROGRESS,
    FAILED,
    NOT_STARTED
  };

namespace DeletionTaskStatusTypeMapper
{
AWS_IAM_API DeletionTaskStatusType GetDeletionTaskStatusTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForDeletionTaskStatusType(DeletionTaskStatusType value);
} // namespace DeletionTaskStatusTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
