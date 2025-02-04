/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PolicyScopeType.h>
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
      namespace PolicyScopeTypeMapper
      {

        static const int All_HASH = HashingUtils::HashString("All");
        static const int AWS_HASH = HashingUtils::HashString("AWS");
        static const int Local_HASH = HashingUtils::HashString("Local");


        PolicyScopeType GetPolicyScopeTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == All_HASH)
          {
            return PolicyScopeType::All;
          }
          else if (hashCode == AWS_HASH)
          {
            return PolicyScopeType::AWS;
          }
          else if (hashCode == Local_HASH)
          {
            return PolicyScopeType::Local;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<PolicyScopeType>(hashCode);
          }

          return PolicyScopeType::NOT_SET;
        }

        Aws::String GetNameForPolicyScopeType(PolicyScopeType enumValue)
        {
          switch(enumValue)
          {
          case PolicyScopeType::NOT_SET:
            return {};
          case PolicyScopeType::All:
            return "All";
          case PolicyScopeType::AWS:
            return "AWS";
          case PolicyScopeType::Local:
            return "Local";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace PolicyScopeTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
