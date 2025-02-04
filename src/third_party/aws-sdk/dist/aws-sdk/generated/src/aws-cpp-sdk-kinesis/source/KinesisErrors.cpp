/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/kinesis/KinesisErrors.h>

using namespace Aws::Client;
using namespace Aws::Utils;
using namespace Aws::Kinesis;

namespace Aws
{
namespace Kinesis
{
namespace KinesisErrorMapper
{

static const int K_M_S_OPT_IN_REQUIRED_HASH = HashingUtils::HashString("KMSOptInRequired");
static const int K_M_S_DISABLED_HASH = HashingUtils::HashString("KMSDisabledException");
static const int K_M_S_ACCESS_DENIED_HASH = HashingUtils::HashString("KMSAccessDeniedException");
static const int K_M_S_NOT_FOUND_HASH = HashingUtils::HashString("KMSNotFoundException");
static const int K_M_S_INVALID_STATE_HASH = HashingUtils::HashString("KMSInvalidStateException");
static const int EXPIRED_ITERATOR_HASH = HashingUtils::HashString("ExpiredIteratorException");
static const int LIMIT_EXCEEDED_HASH = HashingUtils::HashString("LimitExceededException");
static const int K_M_S_THROTTLING_HASH = HashingUtils::HashString("KMSThrottlingException");
static const int RESOURCE_IN_USE_HASH = HashingUtils::HashString("ResourceInUseException");
static const int PROVISIONED_THROUGHPUT_EXCEEDED_HASH = HashingUtils::HashString("ProvisionedThroughputExceededException");
static const int INVALID_ARGUMENT_HASH = HashingUtils::HashString("InvalidArgumentException");
static const int EXPIRED_NEXT_TOKEN_HASH = HashingUtils::HashString("ExpiredNextTokenException");


AWSError<CoreErrors> GetErrorForName(const char* errorName)
{
  int hashCode = HashingUtils::HashString(errorName);

  if (hashCode == K_M_S_OPT_IN_REQUIRED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::K_M_S_OPT_IN_REQUIRED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == K_M_S_DISABLED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::K_M_S_DISABLED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == K_M_S_ACCESS_DENIED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::K_M_S_ACCESS_DENIED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == K_M_S_NOT_FOUND_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::K_M_S_NOT_FOUND), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == K_M_S_INVALID_STATE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::K_M_S_INVALID_STATE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == EXPIRED_ITERATOR_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::EXPIRED_ITERATOR), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == LIMIT_EXCEEDED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::LIMIT_EXCEEDED), RetryableType::RETRYABLE);
  }
  else if (hashCode == K_M_S_THROTTLING_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::K_M_S_THROTTLING), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == RESOURCE_IN_USE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::RESOURCE_IN_USE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == PROVISIONED_THROUGHPUT_EXCEEDED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::PROVISIONED_THROUGHPUT_EXCEEDED), RetryableType::RETRYABLE);
  }
  else if (hashCode == INVALID_ARGUMENT_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::INVALID_ARGUMENT), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == EXPIRED_NEXT_TOKEN_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(KinesisErrors::EXPIRED_NEXT_TOKEN), RetryableType::NOT_RETRYABLE);
  }
  return AWSError<CoreErrors>(CoreErrors::UNKNOWN, false);
}

} // namespace KinesisErrorMapper
} // namespace Kinesis
} // namespace Aws
