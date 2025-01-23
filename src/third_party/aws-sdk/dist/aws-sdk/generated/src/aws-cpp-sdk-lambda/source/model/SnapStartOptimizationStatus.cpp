/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/SnapStartOptimizationStatus.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace Lambda
  {
    namespace Model
    {
      namespace SnapStartOptimizationStatusMapper
      {

        static const int On_HASH = HashingUtils::HashString("On");
        static const int Off_HASH = HashingUtils::HashString("Off");


        SnapStartOptimizationStatus GetSnapStartOptimizationStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == On_HASH)
          {
            return SnapStartOptimizationStatus::On;
          }
          else if (hashCode == Off_HASH)
          {
            return SnapStartOptimizationStatus::Off;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<SnapStartOptimizationStatus>(hashCode);
          }

          return SnapStartOptimizationStatus::NOT_SET;
        }

        Aws::String GetNameForSnapStartOptimizationStatus(SnapStartOptimizationStatus enumValue)
        {
          switch(enumValue)
          {
          case SnapStartOptimizationStatus::NOT_SET:
            return {};
          case SnapStartOptimizationStatus::On:
            return "On";
          case SnapStartOptimizationStatus::Off:
            return "Off";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace SnapStartOptimizationStatusMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
