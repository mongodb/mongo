/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSError.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/lambda/LambdaErrors.h>
#include <aws/lambda/model/EFSMountConnectivityException.h>
#include <aws/lambda/model/ResourceNotReadyException.h>
#include <aws/lambda/model/ResourceNotFoundException.h>
#include <aws/lambda/model/ProvisionedConcurrencyConfigNotFoundException.h>
#include <aws/lambda/model/KMSInvalidStateException.h>
#include <aws/lambda/model/RecursiveInvocationException.h>
#include <aws/lambda/model/InvalidParameterValueException.h>
#include <aws/lambda/model/PolicyLengthExceededException.h>
#include <aws/lambda/model/KMSNotFoundException.h>
#include <aws/lambda/model/PreconditionFailedException.h>
#include <aws/lambda/model/CodeVerificationFailedException.h>
#include <aws/lambda/model/SnapStartException.h>
#include <aws/lambda/model/ResourceInUseException.h>
#include <aws/lambda/model/SubnetIPAddressLimitReachedException.h>
#include <aws/lambda/model/SnapStartNotReadyException.h>
#include <aws/lambda/model/InvalidRequestContentException.h>
#include <aws/lambda/model/EC2AccessDeniedException.h>
#include <aws/lambda/model/RequestTooLargeException.h>
#include <aws/lambda/model/InvalidCodeSignatureException.h>
#include <aws/lambda/model/EFSIOException.h>
#include <aws/lambda/model/InvalidSecurityGroupIDException.h>
#include <aws/lambda/model/InvalidSubnetIDException.h>
#include <aws/lambda/model/CodeSigningConfigNotFoundException.h>
#include <aws/lambda/model/EFSMountTimeoutException.h>
#include <aws/lambda/model/InvalidRuntimeException.h>
#include <aws/lambda/model/EC2UnexpectedException.h>
#include <aws/lambda/model/InvalidZipFileException.h>
#include <aws/lambda/model/UnsupportedMediaTypeException.h>
#include <aws/lambda/model/EFSMountFailureException.h>
#include <aws/lambda/model/KMSDisabledException.h>
#include <aws/lambda/model/KMSAccessDeniedException.h>
#include <aws/lambda/model/EC2ThrottledException.h>
#include <aws/lambda/model/ResourceConflictException.h>
#include <aws/lambda/model/ENILimitReachedException.h>
#include <aws/lambda/model/TooManyRequestsException.h>
#include <aws/lambda/model/ServiceException.h>
#include <aws/lambda/model/SnapStartTimeoutException.h>
#include <aws/lambda/model/CodeStorageExceededException.h>

using namespace Aws::Client;
using namespace Aws::Utils;
using namespace Aws::Lambda;
using namespace Aws::Lambda::Model;

namespace Aws
{
namespace Lambda
{
template<> AWS_LAMBDA_API EFSMountConnectivityException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::E_F_S_MOUNT_CONNECTIVITY);
  return EFSMountConnectivityException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API ResourceNotReadyException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::RESOURCE_NOT_READY);
  return ResourceNotReadyException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API ResourceNotFoundException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::RESOURCE_NOT_FOUND);
  return ResourceNotFoundException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API ProvisionedConcurrencyConfigNotFoundException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::PROVISIONED_CONCURRENCY_CONFIG_NOT_FOUND);
  return ProvisionedConcurrencyConfigNotFoundException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API KMSInvalidStateException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::K_M_S_INVALID_STATE);
  return KMSInvalidStateException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API RecursiveInvocationException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::RECURSIVE_INVOCATION);
  return RecursiveInvocationException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API InvalidParameterValueException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::INVALID_PARAMETER_VALUE);
  return InvalidParameterValueException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API PolicyLengthExceededException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::POLICY_LENGTH_EXCEEDED);
  return PolicyLengthExceededException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API KMSNotFoundException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::K_M_S_NOT_FOUND);
  return KMSNotFoundException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API PreconditionFailedException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::PRECONDITION_FAILED);
  return PreconditionFailedException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API CodeVerificationFailedException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::CODE_VERIFICATION_FAILED);
  return CodeVerificationFailedException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API SnapStartException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::SNAP_START);
  return SnapStartException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API ResourceInUseException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::RESOURCE_IN_USE);
  return ResourceInUseException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API SubnetIPAddressLimitReachedException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::SUBNET_I_P_ADDRESS_LIMIT_REACHED);
  return SubnetIPAddressLimitReachedException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API SnapStartNotReadyException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::SNAP_START_NOT_READY);
  return SnapStartNotReadyException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API InvalidRequestContentException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::INVALID_REQUEST_CONTENT);
  return InvalidRequestContentException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API EC2AccessDeniedException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::E_C2_ACCESS_DENIED);
  return EC2AccessDeniedException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API RequestTooLargeException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::REQUEST_TOO_LARGE);
  return RequestTooLargeException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API InvalidCodeSignatureException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::INVALID_CODE_SIGNATURE);
  return InvalidCodeSignatureException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API EFSIOException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::E_F_S_I_O);
  return EFSIOException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API InvalidSecurityGroupIDException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::INVALID_SECURITY_GROUP_I_D);
  return InvalidSecurityGroupIDException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API InvalidSubnetIDException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::INVALID_SUBNET_I_D);
  return InvalidSubnetIDException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API CodeSigningConfigNotFoundException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::CODE_SIGNING_CONFIG_NOT_FOUND);
  return CodeSigningConfigNotFoundException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API EFSMountTimeoutException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::E_F_S_MOUNT_TIMEOUT);
  return EFSMountTimeoutException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API InvalidRuntimeException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::INVALID_RUNTIME);
  return InvalidRuntimeException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API EC2UnexpectedException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::E_C2_UNEXPECTED);
  return EC2UnexpectedException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API InvalidZipFileException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::INVALID_ZIP_FILE);
  return InvalidZipFileException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API UnsupportedMediaTypeException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::UNSUPPORTED_MEDIA_TYPE);
  return UnsupportedMediaTypeException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API EFSMountFailureException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::E_F_S_MOUNT_FAILURE);
  return EFSMountFailureException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API KMSDisabledException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::K_M_S_DISABLED);
  return KMSDisabledException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API KMSAccessDeniedException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::K_M_S_ACCESS_DENIED);
  return KMSAccessDeniedException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API EC2ThrottledException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::E_C2_THROTTLED);
  return EC2ThrottledException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API ResourceConflictException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::RESOURCE_CONFLICT);
  return ResourceConflictException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API ENILimitReachedException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::E_N_I_LIMIT_REACHED);
  return ENILimitReachedException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API TooManyRequestsException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::TOO_MANY_REQUESTS);
  return TooManyRequestsException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API ServiceException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::SERVICE);
  return ServiceException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API SnapStartTimeoutException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::SNAP_START_TIMEOUT);
  return SnapStartTimeoutException(this->GetJsonPayload().View());
}

template<> AWS_LAMBDA_API CodeStorageExceededException LambdaError::GetModeledError()
{
  assert(this->GetErrorType() == LambdaErrors::CODE_STORAGE_EXCEEDED);
  return CodeStorageExceededException(this->GetJsonPayload().View());
}

namespace LambdaErrorMapper
{

static const int E_F_S_MOUNT_CONNECTIVITY_HASH = HashingUtils::HashString("EFSMountConnectivityException");
static const int RESOURCE_NOT_READY_HASH = HashingUtils::HashString("ResourceNotReadyException");
static const int PROVISIONED_CONCURRENCY_CONFIG_NOT_FOUND_HASH = HashingUtils::HashString("ProvisionedConcurrencyConfigNotFoundException");
static const int K_M_S_INVALID_STATE_HASH = HashingUtils::HashString("KMSInvalidStateException");
static const int RECURSIVE_INVOCATION_HASH = HashingUtils::HashString("RecursiveInvocationException");
static const int POLICY_LENGTH_EXCEEDED_HASH = HashingUtils::HashString("PolicyLengthExceededException");
static const int K_M_S_NOT_FOUND_HASH = HashingUtils::HashString("KMSNotFoundException");
static const int PRECONDITION_FAILED_HASH = HashingUtils::HashString("PreconditionFailedException");
static const int CODE_VERIFICATION_FAILED_HASH = HashingUtils::HashString("CodeVerificationFailedException");
static const int SNAP_START_HASH = HashingUtils::HashString("SnapStartException");
static const int RESOURCE_IN_USE_HASH = HashingUtils::HashString("ResourceInUseException");
static const int SUBNET_I_P_ADDRESS_LIMIT_REACHED_HASH = HashingUtils::HashString("SubnetIPAddressLimitReachedException");
static const int SNAP_START_NOT_READY_HASH = HashingUtils::HashString("SnapStartNotReadyException");
static const int INVALID_REQUEST_CONTENT_HASH = HashingUtils::HashString("InvalidRequestContentException");
static const int E_C2_ACCESS_DENIED_HASH = HashingUtils::HashString("EC2AccessDeniedException");
static const int REQUEST_TOO_LARGE_HASH = HashingUtils::HashString("RequestTooLargeException");
static const int INVALID_CODE_SIGNATURE_HASH = HashingUtils::HashString("InvalidCodeSignatureException");
static const int E_F_S_I_O_HASH = HashingUtils::HashString("EFSIOException");
static const int INVALID_SECURITY_GROUP_I_D_HASH = HashingUtils::HashString("InvalidSecurityGroupIDException");
static const int INVALID_SUBNET_I_D_HASH = HashingUtils::HashString("InvalidSubnetIDException");
static const int CODE_SIGNING_CONFIG_NOT_FOUND_HASH = HashingUtils::HashString("CodeSigningConfigNotFoundException");
static const int E_F_S_MOUNT_TIMEOUT_HASH = HashingUtils::HashString("EFSMountTimeoutException");
static const int INVALID_RUNTIME_HASH = HashingUtils::HashString("InvalidRuntimeException");
static const int E_C2_UNEXPECTED_HASH = HashingUtils::HashString("EC2UnexpectedException");
static const int INVALID_ZIP_FILE_HASH = HashingUtils::HashString("InvalidZipFileException");
static const int UNSUPPORTED_MEDIA_TYPE_HASH = HashingUtils::HashString("UnsupportedMediaTypeException");
static const int E_F_S_MOUNT_FAILURE_HASH = HashingUtils::HashString("EFSMountFailureException");
static const int K_M_S_DISABLED_HASH = HashingUtils::HashString("KMSDisabledException");
static const int K_M_S_ACCESS_DENIED_HASH = HashingUtils::HashString("KMSAccessDeniedException");
static const int E_C2_THROTTLED_HASH = HashingUtils::HashString("EC2ThrottledException");
static const int RESOURCE_CONFLICT_HASH = HashingUtils::HashString("ResourceConflictException");
static const int E_N_I_LIMIT_REACHED_HASH = HashingUtils::HashString("ENILimitReachedException");
static const int TOO_MANY_REQUESTS_HASH = HashingUtils::HashString("TooManyRequestsException");
static const int SERVICE_HASH = HashingUtils::HashString("ServiceException");
static const int SNAP_START_TIMEOUT_HASH = HashingUtils::HashString("SnapStartTimeoutException");
static const int CODE_STORAGE_EXCEEDED_HASH = HashingUtils::HashString("CodeStorageExceededException");


AWSError<CoreErrors> GetErrorForName(const char* errorName)
{
  int hashCode = HashingUtils::HashString(errorName);

  if (hashCode == E_F_S_MOUNT_CONNECTIVITY_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::E_F_S_MOUNT_CONNECTIVITY), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == RESOURCE_NOT_READY_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::RESOURCE_NOT_READY), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == PROVISIONED_CONCURRENCY_CONFIG_NOT_FOUND_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::PROVISIONED_CONCURRENCY_CONFIG_NOT_FOUND), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == K_M_S_INVALID_STATE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::K_M_S_INVALID_STATE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == RECURSIVE_INVOCATION_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::RECURSIVE_INVOCATION), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == POLICY_LENGTH_EXCEEDED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::POLICY_LENGTH_EXCEEDED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == K_M_S_NOT_FOUND_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::K_M_S_NOT_FOUND), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == PRECONDITION_FAILED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::PRECONDITION_FAILED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == CODE_VERIFICATION_FAILED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::CODE_VERIFICATION_FAILED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == SNAP_START_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::SNAP_START), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == RESOURCE_IN_USE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::RESOURCE_IN_USE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == SUBNET_I_P_ADDRESS_LIMIT_REACHED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::SUBNET_I_P_ADDRESS_LIMIT_REACHED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == SNAP_START_NOT_READY_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::SNAP_START_NOT_READY), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == INVALID_REQUEST_CONTENT_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::INVALID_REQUEST_CONTENT), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == E_C2_ACCESS_DENIED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::E_C2_ACCESS_DENIED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == REQUEST_TOO_LARGE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::REQUEST_TOO_LARGE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == INVALID_CODE_SIGNATURE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::INVALID_CODE_SIGNATURE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == E_F_S_I_O_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::E_F_S_I_O), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == INVALID_SECURITY_GROUP_I_D_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::INVALID_SECURITY_GROUP_I_D), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == INVALID_SUBNET_I_D_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::INVALID_SUBNET_I_D), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == CODE_SIGNING_CONFIG_NOT_FOUND_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::CODE_SIGNING_CONFIG_NOT_FOUND), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == E_F_S_MOUNT_TIMEOUT_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::E_F_S_MOUNT_TIMEOUT), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == INVALID_RUNTIME_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::INVALID_RUNTIME), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == E_C2_UNEXPECTED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::E_C2_UNEXPECTED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == INVALID_ZIP_FILE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::INVALID_ZIP_FILE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == UNSUPPORTED_MEDIA_TYPE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::UNSUPPORTED_MEDIA_TYPE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == E_F_S_MOUNT_FAILURE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::E_F_S_MOUNT_FAILURE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == K_M_S_DISABLED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::K_M_S_DISABLED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == K_M_S_ACCESS_DENIED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::K_M_S_ACCESS_DENIED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == E_C2_THROTTLED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::E_C2_THROTTLED), RetryableType::RETRYABLE);
  }
  else if (hashCode == RESOURCE_CONFLICT_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::RESOURCE_CONFLICT), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == E_N_I_LIMIT_REACHED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::E_N_I_LIMIT_REACHED), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == TOO_MANY_REQUESTS_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::TOO_MANY_REQUESTS), RetryableType::RETRYABLE);
  }
  else if (hashCode == SERVICE_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::SERVICE), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == SNAP_START_TIMEOUT_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::SNAP_START_TIMEOUT), RetryableType::NOT_RETRYABLE);
  }
  else if (hashCode == CODE_STORAGE_EXCEEDED_HASH)
  {
    return AWSError<CoreErrors>(static_cast<CoreErrors>(LambdaErrors::CODE_STORAGE_EXCEEDED), RetryableType::NOT_RETRYABLE);
  }
  return AWSError<CoreErrors>(CoreErrors::UNKNOWN, false);
}

} // namespace LambdaErrorMapper
} // namespace Lambda
} // namespace Aws
