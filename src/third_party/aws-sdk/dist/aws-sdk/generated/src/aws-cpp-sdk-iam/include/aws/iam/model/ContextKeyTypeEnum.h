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
  enum class ContextKeyTypeEnum
  {
    NOT_SET,
    string,
    stringList,
    numeric,
    numericList,
    boolean,
    booleanList,
    ip,
    ipList,
    binary,
    binaryList,
    date,
    dateList
  };

namespace ContextKeyTypeEnumMapper
{
AWS_IAM_API ContextKeyTypeEnum GetContextKeyTypeEnumForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForContextKeyTypeEnum(ContextKeyTypeEnum value);
} // namespace ContextKeyTypeEnumMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
