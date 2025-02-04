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
  enum class EntityType
  {
    NOT_SET,
    User,
    Role,
    Group,
    LocalManagedPolicy,
    AWSManagedPolicy
  };

namespace EntityTypeMapper
{
AWS_IAM_API EntityType GetEntityTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForEntityType(EntityType value);
} // namespace EntityTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
