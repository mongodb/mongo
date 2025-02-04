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
  enum class StateReasonCode
  {
    NOT_SET,
    Idle,
    Creating,
    Restoring,
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

namespace StateReasonCodeMapper
{
AWS_LAMBDA_API StateReasonCode GetStateReasonCodeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForStateReasonCode(StateReasonCode value);
} // namespace StateReasonCodeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
