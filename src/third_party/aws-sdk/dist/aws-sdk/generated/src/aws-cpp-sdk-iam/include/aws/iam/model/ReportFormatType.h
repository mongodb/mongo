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
  enum class ReportFormatType
  {
    NOT_SET,
    text_csv
  };

namespace ReportFormatTypeMapper
{
AWS_IAM_API ReportFormatType GetReportFormatTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForReportFormatType(ReportFormatType value);
} // namespace ReportFormatTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
