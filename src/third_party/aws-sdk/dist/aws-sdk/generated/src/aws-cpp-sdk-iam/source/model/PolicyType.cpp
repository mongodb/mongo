/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PolicyType.h>
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
      namespace PolicyTypeMapper
      {

        static const int INLINE_HASH = HashingUtils::HashString("INLINE");
        static const int MANAGED_HASH = HashingUtils::HashString("MANAGED");


        PolicyType GetPolicyTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == INLINE_HASH)
          {
            return PolicyType::INLINE;
          }
          else if (hashCode == MANAGED_HASH)
          {
            return PolicyType::MANAGED;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<PolicyType>(hashCode);
          }

          return PolicyType::NOT_SET;
        }

        Aws::String GetNameForPolicyType(PolicyType enumValue)
        {
          switch(enumValue)
          {
          case PolicyType::NOT_SET:
            return {};
          case PolicyType::INLINE:
            return "INLINE";
          case PolicyType::MANAGED:
            return "MANAGED";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace PolicyTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
