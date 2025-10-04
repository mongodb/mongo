/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/IntelligentTieringStatus.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace S3
  {
    namespace Model
    {
      namespace IntelligentTieringStatusMapper
      {

        static const int Enabled_HASH = HashingUtils::HashString("Enabled");
        static const int Disabled_HASH = HashingUtils::HashString("Disabled");


        IntelligentTieringStatus GetIntelligentTieringStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Enabled_HASH)
          {
            return IntelligentTieringStatus::Enabled;
          }
          else if (hashCode == Disabled_HASH)
          {
            return IntelligentTieringStatus::Disabled;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<IntelligentTieringStatus>(hashCode);
          }

          return IntelligentTieringStatus::NOT_SET;
        }

        Aws::String GetNameForIntelligentTieringStatus(IntelligentTieringStatus enumValue)
        {
          switch(enumValue)
          {
          case IntelligentTieringStatus::NOT_SET:
            return {};
          case IntelligentTieringStatus::Enabled:
            return "Enabled";
          case IntelligentTieringStatus::Disabled:
            return "Disabled";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace IntelligentTieringStatusMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
