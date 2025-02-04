/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/IntelligentTieringAccessTier.h>
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
      namespace IntelligentTieringAccessTierMapper
      {

        static const int ARCHIVE_ACCESS_HASH = HashingUtils::HashString("ARCHIVE_ACCESS");
        static const int DEEP_ARCHIVE_ACCESS_HASH = HashingUtils::HashString("DEEP_ARCHIVE_ACCESS");


        IntelligentTieringAccessTier GetIntelligentTieringAccessTierForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == ARCHIVE_ACCESS_HASH)
          {
            return IntelligentTieringAccessTier::ARCHIVE_ACCESS;
          }
          else if (hashCode == DEEP_ARCHIVE_ACCESS_HASH)
          {
            return IntelligentTieringAccessTier::DEEP_ARCHIVE_ACCESS;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<IntelligentTieringAccessTier>(hashCode);
          }

          return IntelligentTieringAccessTier::NOT_SET;
        }

        Aws::String GetNameForIntelligentTieringAccessTier(IntelligentTieringAccessTier enumValue)
        {
          switch(enumValue)
          {
          case IntelligentTieringAccessTier::NOT_SET:
            return {};
          case IntelligentTieringAccessTier::ARCHIVE_ACCESS:
            return "ARCHIVE_ACCESS";
          case IntelligentTieringAccessTier::DEEP_ARCHIVE_ACCESS:
            return "DEEP_ARCHIVE_ACCESS";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace IntelligentTieringAccessTierMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
