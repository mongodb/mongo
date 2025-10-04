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
  enum class SessionMode
  {
    NOT_SET,
    ReadOnly,
    ReadWrite
  };

namespace SessionModeMapper
{
AWS_S3_API SessionMode GetSessionModeForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForSessionMode(SessionMode value);
} // namespace SessionModeMapper
} // namespace Model
} // namespace S3
} // namespace Aws
