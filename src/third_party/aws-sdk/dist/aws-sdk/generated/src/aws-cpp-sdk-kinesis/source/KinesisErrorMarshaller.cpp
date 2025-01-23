/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/kinesis/KinesisErrorMarshaller.h>
#include <aws/kinesis/KinesisErrors.h>

using namespace Aws::Client;
using namespace Aws::Kinesis;

AWSError<CoreErrors> KinesisErrorMarshaller::FindErrorByName(const char* errorName) const
{
  AWSError<CoreErrors> error = KinesisErrorMapper::GetErrorForName(errorName);
  if(error.GetErrorType() != CoreErrors::UNKNOWN)
  {
    return error;
  }

  return AWSErrorMarshaller::FindErrorByName(errorName);
}