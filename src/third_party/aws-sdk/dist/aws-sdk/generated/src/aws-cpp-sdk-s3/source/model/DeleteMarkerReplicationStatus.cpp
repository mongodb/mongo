/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/DeleteMarkerReplicationStatus.h>
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
      namespace DeleteMarkerReplicationStatusMapper
      {

        static const int Enabled_HASH = HashingUtils::HashString("Enabled");
        static const int Disabled_HASH = HashingUtils::HashString("Disabled");


        DeleteMarkerReplicationStatus GetDeleteMarkerReplicationStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Enabled_HASH)
          {
            return DeleteMarkerReplicationStatus::Enabled;
          }
          else if (hashCode == Disabled_HASH)
          {
            return DeleteMarkerReplicationStatus::Disabled;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<DeleteMarkerReplicationStatus>(hashCode);
          }

          return DeleteMarkerReplicationStatus::NOT_SET;
        }

        Aws::String GetNameForDeleteMarkerReplicationStatus(DeleteMarkerReplicationStatus enumValue)
        {
          switch(enumValue)
          {
          case DeleteMarkerReplicationStatus::NOT_SET:
            return {};
          case DeleteMarkerReplicationStatus::Enabled:
            return "Enabled";
          case DeleteMarkerReplicationStatus::Disabled:
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

      } // namespace DeleteMarkerReplicationStatusMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
