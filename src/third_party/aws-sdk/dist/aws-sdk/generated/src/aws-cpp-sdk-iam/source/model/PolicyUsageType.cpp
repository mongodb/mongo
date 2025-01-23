/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PolicyUsageType.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace IAM
  {
    namespace Model
    {
      namespace PolicyUsageTypeMapper
      {

        static const int PermissionsPolicy_HASH = HashingUtils::HashString("PermissionsPolicy");
        static const int PermissionsBoundary_HASH = HashingUtils::HashString("PermissionsBoundary");


        PolicyUsageType GetPolicyUsageTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == PermissionsPolicy_HASH)
          {
            return PolicyUsageType::PermissionsPolicy;
          }
          else if (hashCode == PermissionsBoundary_HASH)
          {
            return PolicyUsageType::PermissionsBoundary;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<PolicyUsageType>(hashCode);
          }

          return PolicyUsageType::NOT_SET;
        }

        Aws::String GetNameForPolicyUsageType(PolicyUsageType enumValue)
        {
          switch(enumValue)
          {
          case PolicyUsageType::NOT_SET:
            return {};
          case PolicyUsageType::PermissionsPolicy:
            return "PermissionsPolicy";
          case PolicyUsageType::PermissionsBoundary:
            return "PermissionsBoundary";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace PolicyUsageTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
