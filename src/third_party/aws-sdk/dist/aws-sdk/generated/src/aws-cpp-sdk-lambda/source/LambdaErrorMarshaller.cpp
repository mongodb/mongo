/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/lambda/LambdaErrorMarshaller.h>
#include <aws/lambda/LambdaErrors.h>

using namespace Aws::Client;
using namespace Aws::Lambda;

AWSError<CoreErrors> LambdaErrorMarshaller::FindErrorByName(const char* errorName) const
{
  AWSError<CoreErrors> error = LambdaErrorMapper::GetErrorForName(errorName);
  if(error.GetErrorType() != CoreErrors::UNKNOWN)
  {
    return error;
  }

  return AWSErrorMarshaller::FindErrorByName(errorName);
}