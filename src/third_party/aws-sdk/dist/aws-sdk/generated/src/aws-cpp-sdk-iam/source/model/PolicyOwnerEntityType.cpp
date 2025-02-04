/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PolicyOwnerEntityType.h>
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
      namespace PolicyOwnerEntityTypeMapper
      {

        static const int USER_HASH = HashingUtils::HashString("USER");
        static const int ROLE_HASH = HashingUtils::HashString("ROLE");
        static const int GROUP_HASH = HashingUtils::HashString("GROUP");


        PolicyOwnerEntityType GetPolicyOwnerEntityTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == USER_HASH)
          {
            return PolicyOwnerEntityType::USER;
          }
          else if (hashCode == ROLE_HASH)
          {
            return PolicyOwnerEntityType::ROLE;
          }
          else if (hashCode == GROUP_HASH)
          {
            return PolicyOwnerEntityType::GROUP;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<PolicyOwnerEntityType>(hashCode);
          }

          return PolicyOwnerEntityType::NOT_SET;
        }

        Aws::String GetNameForPolicyOwnerEntityType(PolicyOwnerEntityType enumValue)
        {
          switch(enumValue)
          {
          case PolicyOwnerEntityType::NOT_SET:
            return {};
          case PolicyOwnerEntityType::USER:
            return "USER";
          case PolicyOwnerEntityType::ROLE:
            return "ROLE";
          case PolicyOwnerEntityType::GROUP:
            return "GROUP";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace PolicyOwnerEntityTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
