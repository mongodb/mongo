/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/AccessAdvisorUsageGranularityType.h>
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
      namespace AccessAdvisorUsageGranularityTypeMapper
      {

        static const int SERVICE_LEVEL_HASH = HashingUtils::HashString("SERVICE_LEVEL");
        static const int ACTION_LEVEL_HASH = HashingUtils::HashString("ACTION_LEVEL");


        AccessAdvisorUsageGranularityType GetAccessAdvisorUsageGranularityTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == SERVICE_LEVEL_HASH)
          {
            return AccessAdvisorUsageGranularityType::SERVICE_LEVEL;
          }
          else if (hashCode == ACTION_LEVEL_HASH)
          {
            return AccessAdvisorUsageGranularityType::ACTION_LEVEL;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<AccessAdvisorUsageGranularityType>(hashCode);
          }

          return AccessAdvisorUsageGranularityType::NOT_SET;
        }

        Aws::String GetNameForAccessAdvisorUsageGranularityType(AccessAdvisorUsageGranularityType enumValue)
        {
          switch(enumValue)
          {
          case AccessAdvisorUsageGranularityType::NOT_SET:
            return {};
          case AccessAdvisorUsageGranularityType::SERVICE_LEVEL:
            return "SERVICE_LEVEL";
          case AccessAdvisorUsageGranularityType::ACTION_LEVEL:
            return "ACTION_LEVEL";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace AccessAdvisorUsageGranularityTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
