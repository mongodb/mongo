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
  enum class AssignmentStatusType
  {
    NOT_SET,
    Assigned,
    Unassigned,
    Any
  };

namespace AssignmentStatusTypeMapper
{
AWS_IAM_API AssignmentStatusType GetAssignmentStatusTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForAssignmentStatusType(AssignmentStatusType value);
} // namespace AssignmentStatusTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
