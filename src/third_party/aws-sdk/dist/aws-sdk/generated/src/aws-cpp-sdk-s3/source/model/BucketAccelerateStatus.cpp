/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/BucketAccelerateStatus.h>
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
      namespace BucketAccelerateStatusMapper
      {

        static const int Enabled_HASH = HashingUtils::HashString("Enabled");
        static const int Suspended_HASH = HashingUtils::HashString("Suspended");


        BucketAccelerateStatus GetBucketAccelerateStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Enabled_HASH)
          {
            return BucketAccelerateStatus::Enabled;
          }
          else if (hashCode == Suspended_HASH)
          {
            return BucketAccelerateStatus::Suspended;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<BucketAccelerateStatus>(hashCode);
          }

          return BucketAccelerateStatus::NOT_SET;
        }

        Aws::String GetNameForBucketAccelerateStatus(BucketAccelerateStatus enumValue)
        {
          switch(enumValue)
          {
          case BucketAccelerateStatus::NOT_SET:
            return {};
          case BucketAccelerateStatus::Enabled:
            return "Enabled";
          case BucketAccelerateStatus::Suspended:
            return "Suspended";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace BucketAccelerateStatusMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
