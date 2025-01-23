/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Lambda
{
namespace Model
{
  enum class LastUpdateStatusReasonCode
  {
    NOT_SET,
    EniLimitExceeded,
    InsufficientRolePermissions,
    InvalidConfiguration,
    InternalError,
    SubnetOutOfIPAddresses,
    InvalidSubnet,
    InvalidSecurityGroup,
    ImageDeleted,
    ImageAccessDenied,
    InvalidImage,
    KMSKeyAccessDenied,
    KMSKeyNotFound,
    InvalidStateKMSKey,
    DisabledKMSKey,
    EFSIOError,
    EFSMountConnectivityError,
    EFSMountFailure,
    EFSMountTimeout,
    InvalidRuntime,
    InvalidZipFileException,
    FunctionError
  };

namespace LastUpdateStatusReasonCodeMapper
{
AWS_LAMBDA_API LastUpdateStatusReasonCode GetLastUpdateStatusReasonCodeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForLastUpdateStatusReasonCode(LastUpdateStatusReasonCode value);
} // namespace LastUpdateStatusReasonCodeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
