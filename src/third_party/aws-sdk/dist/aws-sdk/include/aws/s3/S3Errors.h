/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/client/AWSError.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/s3/S3_EXPORTS.h>

namespace Aws
{
namespace S3
{
enum class S3Errors
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

  BUCKET_ALREADY_EXISTS= static_cast<int>(Aws::Client::CoreErrors::SERVICE_EXTENSION_START_RANGE) + 1,
  BUCKET_ALREADY_OWNED_BY_YOU,
  ENCRYPTION_TYPE_MISMATCH,
  INVALID_OBJECT_STATE,
  INVALID_REQUEST,
  INVALID_WRITE_OFFSET,
  NO_SUCH_BUCKET,
  NO_SUCH_KEY,
  NO_SUCH_UPLOAD,
  OBJECT_ALREADY_IN_ACTIVE_TIER,
  OBJECT_NOT_IN_ACTIVE_TIER,
  TOO_MANY_PARTS
};

class AWS_S3_API S3Error : public Aws::Client::AWSError<S3Errors>
{
public:
  S3Error() {}
  S3Error(const Aws::Client::AWSError<Aws::Client::CoreErrors>& rhs) : Aws::Client::AWSError<S3Errors>(rhs) {}
  S3Error(Aws::Client::AWSError<Aws::Client::CoreErrors>&& rhs) : Aws::Client::AWSError<S3Errors>(rhs) {}
  S3Error(const Aws::Client::AWSError<S3Errors>& rhs) : Aws::Client::AWSError<S3Errors>(rhs) {}
  S3Error(Aws::Client::AWSError<S3Errors>&& rhs) : Aws::Client::AWSError<S3Errors>(rhs) {}

  template <typename T>
  T GetModeledError();
};

namespace S3ErrorMapper
{
  AWS_S3_API Aws::Client::AWSError<Aws::Client::CoreErrors> GetErrorForName(const char* errorName);
}

} // namespace S3
} // namespace Aws
