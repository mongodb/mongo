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
  enum class AnalyticsS3ExportFileFormat
  {
    NOT_SET,
    CSV
  };

namespace AnalyticsS3ExportFileFormatMapper
{
AWS_S3_API AnalyticsS3ExportFileFormat GetAnalyticsS3ExportFileFormatForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForAnalyticsS3ExportFileFormat(AnalyticsS3ExportFileFormat value);
} // namespace AnalyticsS3ExportFileFormatMapper
} // namespace Model
} // namespace S3
} // namespace Aws
