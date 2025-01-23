/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ReplicaModificationsStatus.h>
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
      namespace ReplicaModificationsStatusMapper
      {

        static const int Enabled_HASH = HashingUtils::HashString("Enabled");
        static const int Disabled_HASH = HashingUtils::HashString("Disabled");


        ReplicaModificationsStatus GetReplicaModificationsStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Enabled_HASH)
          {
            return ReplicaModificationsStatus::Enabled;
          }
          else if (hashCode == Disabled_HASH)
          {
            return ReplicaModificationsStatus::Disabled;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ReplicaModificationsStatus>(hashCode);
          }

          return ReplicaModificationsStatus::NOT_SET;
        }

        Aws::String GetNameForReplicaModificationsStatus(ReplicaModificationsStatus enumValue)
        {
          switch(enumValue)
          {
          case ReplicaModificationsStatus::NOT_SET:
            return {};
          case ReplicaModificationsStatus::Enabled:
            return "Enabled";
          case ReplicaModificationsStatus::Disabled:
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

      } // namespace ReplicaModificationsStatusMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
