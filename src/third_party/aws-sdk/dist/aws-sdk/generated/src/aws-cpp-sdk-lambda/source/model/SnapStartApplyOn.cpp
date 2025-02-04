/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/SnapStartApplyOn.h>
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
      namespace SnapStartApplyOnMapper
      {

        static const int PublishedVersions_HASH = HashingUtils::HashString("PublishedVersions");
        static const int None_HASH = HashingUtils::HashString("None");


        SnapStartApplyOn GetSnapStartApplyOnForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == PublishedVersions_HASH)
          {
            return SnapStartApplyOn::PublishedVersions;
          }
          else if (hashCode == None_HASH)
          {
            return SnapStartApplyOn::None;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<SnapStartApplyOn>(hashCode);
          }

          return SnapStartApplyOn::NOT_SET;
        }

        Aws::String GetNameForSnapStartApplyOn(SnapStartApplyOn enumValue)
        {
          switch(enumValue)
          {
          case SnapStartApplyOn::NOT_SET:
            return {};
          case SnapStartApplyOn::PublishedVersions:
            return "PublishedVersions";
          case SnapStartApplyOn::None:
            return "None";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace SnapStartApplyOnMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
