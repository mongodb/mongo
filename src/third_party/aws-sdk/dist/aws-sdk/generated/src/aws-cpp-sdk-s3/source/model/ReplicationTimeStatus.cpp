/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ReplicationTimeStatus.h>
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
      namespace ReplicationTimeStatusMapper
      {

        static const int Enabled_HASH = HashingUtils::HashString("Enabled");
        static const int Disabled_HASH = HashingUtils::HashString("Disabled");


        ReplicationTimeStatus GetReplicationTimeStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Enabled_HASH)
          {
            return ReplicationTimeStatus::Enabled;
          }
          else if (hashCode == Disabled_HASH)
          {
            return ReplicationTimeStatus::Disabled;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ReplicationTimeStatus>(hashCode);
          }

          return ReplicationTimeStatus::NOT_SET;
        }

        Aws::String GetNameForReplicationTimeStatus(ReplicationTimeStatus enumValue)
        {
          switch(enumValue)
          {
          case ReplicationTimeStatus::NOT_SET:
            return {};
          case ReplicationTimeStatus::Enabled:
            return "Enabled";
          case ReplicationTimeStatus::Disabled:
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

      } // namespace ReplicationTimeStatusMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
