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
  enum class PolicyEvaluationDecisionType
  {
    NOT_SET,
    allowed,
    explicitDeny,
    implicitDeny
  };

namespace PolicyEvaluationDecisionTypeMapper
{
AWS_IAM_API PolicyEvaluationDecisionType GetPolicyEvaluationDecisionTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForPolicyEvaluationDecisionType(PolicyEvaluationDecisionType value);
} // namespace PolicyEvaluationDecisionTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
