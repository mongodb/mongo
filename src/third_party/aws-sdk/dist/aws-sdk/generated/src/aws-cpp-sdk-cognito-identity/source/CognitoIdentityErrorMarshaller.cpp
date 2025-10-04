/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/cognito-identity/CognitoIdentityErrorMarshaller.h>
#include <aws/cognito-identity/CognitoIdentityErrors.h>

using namespace Aws::Client;
using namespace Aws::CognitoIdentity;

AWSError<CoreErrors> CognitoIdentityErrorMarshaller::FindErrorByName(const char* errorName) const
{
  AWSError<CoreErrors> error = CognitoIdentityErrorMapper::GetErrorForName(errorName);
  if(error.GetErrorType() != CoreErrors::UNKNOWN)
  {
    return error;
  }

  return AWSErrorMarshaller::FindErrorByName(errorName);
}