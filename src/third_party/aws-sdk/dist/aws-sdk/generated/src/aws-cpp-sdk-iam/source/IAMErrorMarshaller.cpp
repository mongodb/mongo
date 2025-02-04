/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/iam/IAMErrorMarshaller.h>
#include <aws/iam/IAMErrors.h>

using namespace Aws::Client;
using namespace Aws::IAM;

AWSError<CoreErrors> IAMErrorMarshaller::FindErrorByName(const char* errorName) const
{
  AWSError<CoreErrors> error = IAMErrorMapper::GetErrorForName(errorName);
  if(error.GetErrorType() != CoreErrors::UNKNOWN)
  {
    return error;
  }

  return AWSErrorMarshaller::FindErrorByName(errorName);
}