/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Lambda
{
namespace Model
{
  enum class SnapStartApplyOn
  {
    NOT_SET,
    PublishedVersions,
    None
  };

namespace SnapStartApplyOnMapper
{
AWS_LAMBDA_API SnapStartApplyOn GetSnapStartApplyOnForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForSnapStartApplyOn(SnapStartApplyOn value);
} // namespace SnapStartApplyOnMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
