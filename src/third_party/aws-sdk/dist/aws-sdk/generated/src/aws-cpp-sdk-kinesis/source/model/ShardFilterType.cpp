/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/ShardFilterType.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace Kinesis
  {
    namespace Model
    {
      namespace ShardFilterTypeMapper
      {

        static const int AFTER_SHARD_ID_HASH = HashingUtils::HashString("AFTER_SHARD_ID");
        static const int AT_TRIM_HORIZON_HASH = HashingUtils::HashString("AT_TRIM_HORIZON");
        static const int FROM_TRIM_HORIZON_HASH = HashingUtils::HashString("FROM_TRIM_HORIZON");
        static const int AT_LATEST_HASH = HashingUtils::HashString("AT_LATEST");
        static const int AT_TIMESTAMP_HASH = HashingUtils::HashString("AT_TIMESTAMP");
        static const int FROM_TIMESTAMP_HASH = HashingUtils::HashString("FROM_TIMESTAMP");


        ShardFilterType GetShardFilterTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == AFTER_SHARD_ID_HASH)
          {
            return ShardFilterType::AFTER_SHARD_ID;
          }
          else if (hashCode == AT_TRIM_HORIZON_HASH)
          {
            return ShardFilterType::AT_TRIM_HORIZON;
          }
          else if (hashCode == FROM_TRIM_HORIZON_HASH)
          {
            return ShardFilterType::FROM_TRIM_HORIZON;
          }
          else if (hashCode == AT_LATEST_HASH)
          {
            return ShardFilterType::AT_LATEST;
          }
          else if (hashCode == AT_TIMESTAMP_HASH)
          {
            return ShardFilterType::AT_TIMESTAMP;
          }
          else if (hashCode == FROM_TIMESTAMP_HASH)
          {
            return ShardFilterType::FROM_TIMESTAMP;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ShardFilterType>(hashCode);
          }

          return ShardFilterType::NOT_SET;
        }

        Aws::String GetNameForShardFilterType(ShardFilterType enumValue)
        {
          switch(enumValue)
          {
          case ShardFilterType::NOT_SET:
            return {};
          case ShardFilterType::AFTER_SHARD_ID:
            return "AFTER_SHARD_ID";
          case ShardFilterType::AT_TRIM_HORIZON:
            return "AT_TRIM_HORIZON";
          case ShardFilterType::FROM_TRIM_HORIZON:
            return "FROM_TRIM_HORIZON";
          case ShardFilterType::AT_LATEST:
            return "AT_LATEST";
          case ShardFilterType::AT_TIMESTAMP:
            return "AT_TIMESTAMP";
          case ShardFilterType::FROM_TIMESTAMP:
            return "FROM_TIMESTAMP";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ShardFilterTypeMapper
    } // namespace Model
  } // namespace Kinesis
} // namespace Aws
