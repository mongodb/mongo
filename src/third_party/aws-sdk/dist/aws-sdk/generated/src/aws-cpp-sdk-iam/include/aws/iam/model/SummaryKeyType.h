/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace IAM
{
namespace Model
{
  enum class SummaryKeyType
  {
    NOT_SET,
    Users,
    UsersQuota,
    Groups,
    GroupsQuota,
    ServerCertificates,
    ServerCertificatesQuota,
    UserPolicySizeQuota,
    GroupPolicySizeQuota,
    GroupsPerUserQuota,
    SigningCertificatesPerUserQuota,
    AccessKeysPerUserQuota,
    MFADevices,
    MFADevicesInUse,
    AccountMFAEnabled,
    AccountAccessKeysPresent,
    AccountPasswordPresent,
    AccountSigningCertificatesPresent,
    AttachedPoliciesPerGroupQuota,
    AttachedPoliciesPerRoleQuota,
    AttachedPoliciesPerUserQuota,
    Policies,
    PoliciesQuota,
    PolicySizeQuota,
    PolicyVersionsInUse,
    PolicyVersionsInUseQuota,
    VersionsPerPolicyQuota,
    GlobalEndpointTokenVersion
  };

namespace SummaryKeyTypeMapper
{
AWS_IAM_API SummaryKeyType GetSummaryKeyTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForSummaryKeyType(SummaryKeyType value);
} // namespace SummaryKeyTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
