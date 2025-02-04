/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/cognito-identity/CognitoIdentityErrors.h>

using namespace Aws::Client;
using namespace Aws::Utils;
using namespace Aws::CognitoIdentity;

namespace Aws
{
namespace CognitoIdentity
{
namespace CognitoIdentityErrorMapper
{

static const int INTERNAL_ERROR_HASH = HashingUtils::HashString("InternalErrorException");
static const int EXTERNAL_SERVICE_HASH = HashingUtils::HashString("ExternalServiceException");
static const int INVALID_PARAMETER_HASH = HashingUtils::HashString("InvalidParameterException");
static const int NOT_AUTHORIZED_HASH = HashingUtils::HashString("NotAuthorizedException");
static const int RESOURCE_CONFLICT_HASH = HashingUtils::HashString("ResourceConflictException");
static const int LIMIT_EXCEEDED_HASH = HashingUtils::HashString("LimitExceededException");
static const int TOO_MANY_REQUESTS_HASH = HashingUtils::HashString("TooManyRequestsException");
static const int CONCURRENT_MODIFICATION_HASH = HashingUtils::HashString("ConcurrentModificationException");
static const int INVALID_IDENTITY_POOL_CONFIGURATION_HASH = HashingUtils::HashString("InvalidIdentityPoolConfigurationException");
static const int DEVELOPER_USER_ALREADY_REGISTERED_HASH = HashingUtils::HashString("DeveloperUserAlreadyRegisteredException");


AWSError<CoreErrors> GetErrorForName(const char* errorName)
{
  int hashCode = HashingUtils::HashString(errorName);

  if (hashCode == INTERNAL_ERROR_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::INTERNAL_ERROR), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == EXTERNAL_SERVICE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::EXTERNAL_SERVICE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == INVALID_PARAMETER_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::INVALID_PARAMETER), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == NOT_AUTHORIZED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::NOT_AUTHORIZED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == RESOURCE_CONFLICT_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::RESOURCE_CONFLICT), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == LIMIT_EXCEEDED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::LIMIT_EXCEEDED), RetryableType::RETRYABLE);
  }
  else if (hashCode == TOO_MANY_REQUESTS_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::TOO_MANY_REQUESTS), RetryableType::RETRYABLE);
  }
  else if (hashCode == CONCURRENT_MODIFICATION_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::CONCURRENT_MODIFICATION), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == INVALID_IDENTITY_POOL_CONFIGURATION_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::INVALID_IDENTITY_POOL_CONFIGURATION), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == DEVELOPER_USER_ALREADY_REGISTERED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(CognitoIdentityErrors::DEVELOPER_USER_ALREADY_REGISTERED), RetryableType::NOT_RETRYABLE);
  }
  return AWSError<CoreErrors>(CoreErrors::UNKNOWN, false);
}

} // namespace CognitoIdentityErrorMapper
} // namespace CognitoIdentity
} // namespace Aws
