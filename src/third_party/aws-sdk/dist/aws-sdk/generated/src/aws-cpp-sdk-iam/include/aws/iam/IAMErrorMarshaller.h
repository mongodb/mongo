/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/client/AWSErrorMarshaller.h>

namespace Aws
{
namespace Client
{

class AWS_IAM_API IAMErrorMarshaller : public Aws::Client::XmlErrorMarshaller
{
public:
  Aws::Client::AWSError<Aws::Client::CoreErrors> FindErrorByName(const char* exceptionName) const override;
};

} // namespace Client
} // namespace Aws
