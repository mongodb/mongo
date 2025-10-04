/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/client/AWSError.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/lambda/Lambda_EXPORTS.h>

namespace Aws
{
namespace Lambda
{
enum class LambdaErrors
{
  //From Core//
  //////////////////////////////////////////////////////////////////////////////////////////
  INCOMPLETE_SIGNATURE = 0,
  INTERNAL_FAILURE = 1,
  INVALID_ACTION = 2,
  INVALID_CLIENT_TOKEN_ID = 3,
  INVALID_PARAMETER_COMBINATION = 4,
  INVALID_QUERY_PARAMETER = 5,
  INVALID_PARAMETER_VALUE = 6,
  MISSING_ACTION = 7, // SDK should never allow
  MISSING_AUTHENTICATION_TOKEN = 8, // SDK should never allow
  MISSING_PARAMETER = 9, // SDK should never allow
  OPT_IN_REQUIRED = 10,
  REQUEST_EXPIRED = 11,
  SERVICE_UNAVAILABLE = 12,
  THROTTLING = 13,
  VALIDATION = 14,
  ACCESS_DENIED = 15,
  RESOURCE_NOT_FOUND = 16,
  UNRECOGNIZED_CLIENT = 17,
  MALFORMED_QUERY_STRING = 18,
  SLOW_DOWN = 19,
  REQUEST_TIME_TOO_SKEWED = 20,
  INVALID_SIGNATURE = 21,
  SIGNATURE_DOES_NOT_MATCH = 22,
  INVALID_ACCESS_KEY_ID = 23,
  REQUEST_TIMEOUT = 24,
  NETWORK_CONNECTION = 99,

  UNKNOWN = 100,
  ///////////////////////////////////////////////////////////////////////////////////////////

  CODE_SIGNING_CONFIG_NOT_FOUND= static_cast<int>(Aws::Client::CoreErrors::SERVICE_EXTENSION_START_RANGE) + 1,
  CODE_STORAGE_EXCEEDED,
  CODE_VERIFICATION_FAILED,
  E_C2_ACCESS_DENIED,
  E_C2_THROTTLED,
  E_C2_UNEXPECTED,
  E_F_S_I_O,
  E_F_S_MOUNT_CONNECTIVITY,
  E_F_S_MOUNT_FAILURE,
  E_F_S_MOUNT_TIMEOUT,
  E_N_I_LIMIT_REACHED,
  INVALID_CODE_SIGNATURE,
  INVALID_REQUEST_CONTENT,
  INVALID_RUNTIME,
  INVALID_SECURITY_GROUP_I_D,
  INVALID_SUBNET_I_D,
  INVALID_ZIP_FILE,
  K_M_S_ACCESS_DENIED,
  K_M_S_DISABLED,
  K_M_S_INVALID_STATE,
  K_M_S_NOT_FOUND,
  POLICY_LENGTH_EXCEEDED,
  PRECONDITION_FAILED,
  PROVISIONED_CONCURRENCY_CONFIG_NOT_FOUND,
  RECURSIVE_INVOCATION,
  REQUEST_TOO_LARGE,
  RESOURCE_CONFLICT,
  RESOURCE_IN_USE,
  RESOURCE_NOT_READY,
  SERVICE,
  SNAP_START,
  SNAP_START_NOT_READY,
  SNAP_START_TIMEOUT,
  SUBNET_I_P_ADDRESS_LIMIT_REACHED,
  TOO_MANY_REQUESTS,
  UNSUPPORTED_MEDIA_TYPE
};

class AWS_LAMBDA_API LambdaError : public Aws::Client::AWSError<LambdaErrors>
{
public:
  LambdaError() {}
  LambdaError(const Aws::Client::AWSError<Aws::Client::CoreErrors>& rhs) : Aws::Client::AWSError<LambdaErrors>(rhs) {}
  LambdaError(Aws::Client::AWSError<Aws::Client::CoreErrors>&& rhs) : Aws::Client::AWSError<LambdaErrors>(rhs) {}
  LambdaError(const Aws::Client::AWSError<LambdaErrors>& rhs) : Aws::Client::AWSError<LambdaErrors>(rhs) {}
  LambdaError(Aws::Client::AWSError<LambdaErrors>&& rhs) : Aws::Client::AWSError<LambdaErrors>(rhs) {}

  template <typename T>
  T GetModeledError();
};

namespace LambdaErrorMapper
{
  AWS_LAMBDA_API Aws::Client::AWSError<Aws::Client::CoreErrors> GetErrorForName(const char* errorName);
}

} // namespace Lambda
} // namespace Aws
