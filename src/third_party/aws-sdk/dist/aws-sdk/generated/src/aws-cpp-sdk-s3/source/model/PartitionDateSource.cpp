/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/PartitionDateSource.h>
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
      namespace PartitionDateSourceMapper
      {

        static const int EventTime_HASH = HashingUtils::HashString("EventTime");
        static const int DeliveryTime_HASH = HashingUtils::HashString("DeliveryTime");


        PartitionDateSource GetPartitionDateSourceForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == EventTime_HASH)
          {
            return PartitionDateSource::EventTime;
          }
          else if (hashCode == DeliveryTime_HASH)
          {
            return PartitionDateSource::DeliveryTime;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<PartitionDateSource>(hashCode);
          }

          return PartitionDateSource::NOT_SET;
        }

        Aws::String GetNameForPartitionDateSource(PartitionDateSource enumValue)
        {
          switch(enumValue)
          {
          case PartitionDateSource::NOT_SET:
            return {};
          case PartitionDateSource::EventTime:
            return "EventTime";
          case PartitionDateSource::DeliveryTime:
            return "DeliveryTime";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace PartitionDateSourceMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
