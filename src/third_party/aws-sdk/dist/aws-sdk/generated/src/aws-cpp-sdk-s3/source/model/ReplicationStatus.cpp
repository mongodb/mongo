/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ReplicationStatus.h>
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
      namespace ReplicationStatusMapper
      {

        static const int COMPLETED_HASH = HashingUtils::HashString("COMPLETED");
        static const int PENDING_HASH = HashingUtils::HashString("PENDING");
        static const int FAILED_HASH = HashingUtils::HashString("FAILED");
        static const int REPLICA_HASH = HashingUtils::HashString("REPLICA");


        ReplicationStatus GetReplicationStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == COMPLETED_HASH)
          {
            return ReplicationStatus::COMPLETED;
          }
          else if (hashCode == PENDING_HASH)
          {
            return ReplicationStatus::PENDING;
          }
          else if (hashCode == FAILED_HASH)
          {
            return ReplicationStatus::FAILED;
          }
          else if (hashCode == REPLICA_HASH)
          {
            return ReplicationStatus::REPLICA;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ReplicationStatus>(hashCode);
          }

          return ReplicationStatus::NOT_SET;
        }

        Aws::String GetNameForReplicationStatus(ReplicationStatus enumValue)
        {
          switch(enumValue)
          {
          case ReplicationStatus::NOT_SET:
            return {};
          case ReplicationStatus::COMPLETED:
            return "COMPLETED";
          case ReplicationStatus::PENDING:
            return "PENDING";
          case ReplicationStatus::FAILED:
            return "FAILED";
          case ReplicationStatus::REPLICA:
            return "REPLICA";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ReplicationStatusMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
