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
  enum class FullDocument
  {
    NOT_SET,
    UpdateLookup,
    Default
  };

namespace FullDocumentMapper
{
AWS_LAMBDA_API FullDocument GetFullDocumentForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForFullDocument(FullDocument value);
} // namespace FullDocumentMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
