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
  enum class ReportStateType
  {
    NOT_SET,
    STARTED,
    INPROGRESS,
    COMPLETE
  };

namespace ReportStateTypeMapper
{
AWS_IAM_API ReportStateType GetReportStateTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForReportStateType(ReportStateType value);
} // namespace ReportStateTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
