/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Event.h>
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
      namespace EventMapper
      {

        static const int s3_ReducedRedundancyLostObject_HASH = HashingUtils::HashString("s3:ReducedRedundancyLostObject");
        static const int s3_ObjectCreated_HASH = HashingUtils::HashString("s3:ObjectCreated:*");
        static const int s3_ObjectCreated_Put_HASH = HashingUtils::HashString("s3:ObjectCreated:Put");
        static const int s3_ObjectCreated_Post_HASH = HashingUtils::HashString("s3:ObjectCreated:Post");
        static const int s3_ObjectCreated_Copy_HASH = HashingUtils::HashString("s3:ObjectCreated:Copy");
        static const int s3_ObjectCreated_CompleteMultipartUpload_HASH = HashingUtils::HashString("s3:ObjectCreated:CompleteMultipartUpload");
        static const int s3_ObjectRemoved_HASH = HashingUtils::HashString("s3:ObjectRemoved:*");
        static const int s3_ObjectRemoved_Delete_HASH = HashingUtils::HashString("s3:ObjectRemoved:Delete");
        static const int s3_ObjectRemoved_DeleteMarkerCreated_HASH = HashingUtils::HashString("s3:ObjectRemoved:DeleteMarkerCreated");
        static const int s3_ObjectRestore_HASH = HashingUtils::HashString("s3:ObjectRestore:*");
        static const int s3_ObjectRestore_Post_HASH = HashingUtils::HashString("s3:ObjectRestore:Post");
        static const int s3_ObjectRestore_Completed_HASH = HashingUtils::HashString("s3:ObjectRestore:Completed");
        static const int s3_Replication_HASH = HashingUtils::HashString("s3:Replication:*");
        static const int s3_Replication_OperationFailedReplication_HASH = HashingUtils::HashString("s3:Replication:OperationFailedReplication");
        static const int s3_Replication_OperationNotTracked_HASH = HashingUtils::HashString("s3:Replication:OperationNotTracked");
        static const int s3_Replication_OperationMissedThreshold_HASH = HashingUtils::HashString("s3:Replication:OperationMissedThreshold");
        static const int s3_Replication_OperationReplicatedAfterThreshold_HASH = HashingUtils::HashString("s3:Replication:OperationReplicatedAfterThreshold");
        static const int s3_ObjectRestore_Delete_HASH = HashingUtils::HashString("s3:ObjectRestore:Delete");
        static const int s3_LifecycleTransition_HASH = HashingUtils::HashString("s3:LifecycleTransition");
        static const int s3_IntelligentTiering_HASH = HashingUtils::HashString("s3:IntelligentTiering");
        static const int s3_ObjectAcl_Put_HASH = HashingUtils::HashString("s3:ObjectAcl:Put");
        static const int s3_LifecycleExpiration_HASH = HashingUtils::HashString("s3:LifecycleExpiration:*");
        static const int s3_LifecycleExpiration_Delete_HASH = HashingUtils::HashString("s3:LifecycleExpiration:Delete");
        static const int s3_LifecycleExpiration_DeleteMarkerCreated_HASH = HashingUtils::HashString("s3:LifecycleExpiration:DeleteMarkerCreated");
        static const int s3_ObjectTagging_HASH = HashingUtils::HashString("s3:ObjectTagging:*");
        static const int s3_ObjectTagging_Put_HASH = HashingUtils::HashString("s3:ObjectTagging:Put");
        static const int s3_ObjectTagging_Delete_HASH = HashingUtils::HashString("s3:ObjectTagging:Delete");


        Event GetEventForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == s3_ReducedRedundancyLostObject_HASH)
          {
            return Event::s3_ReducedRedundancyLostObject;
          }
          else if (hashCode == s3_ObjectCreated_HASH)
          {
            return Event::s3_ObjectCreated;
          }
          else if (hashCode == s3_ObjectCreated_Put_HASH)
          {
            return Event::s3_ObjectCreated_Put;
          }
          else if (hashCode == s3_ObjectCreated_Post_HASH)
          {
            return Event::s3_ObjectCreated_Post;
          }
          else if (hashCode == s3_ObjectCreated_Copy_HASH)
          {
            return Event::s3_ObjectCreated_Copy;
          }
          else if (hashCode == s3_ObjectCreated_CompleteMultipartUpload_HASH)
          {
            return Event::s3_ObjectCreated_CompleteMultipartUpload;
          }
          else if (hashCode == s3_ObjectRemoved_HASH)
          {
            return Event::s3_ObjectRemoved;
          }
          else if (hashCode == s3_ObjectRemoved_Delete_HASH)
          {
            return Event::s3_ObjectRemoved_Delete;
          }
          else if (hashCode == s3_ObjectRemoved_DeleteMarkerCreated_HASH)
          {
            return Event::s3_ObjectRemoved_DeleteMarkerCreated;
          }
          else if (hashCode == s3_ObjectRestore_HASH)
          {
            return Event::s3_ObjectRestore;
          }
          else if (hashCode == s3_ObjectRestore_Post_HASH)
          {
            return Event::s3_ObjectRestore_Post;
          }
          else if (hashCode == s3_ObjectRestore_Completed_HASH)
          {
            return Event::s3_ObjectRestore_Completed;
          }
          else if (hashCode == s3_Replication_HASH)
          {
            return Event::s3_Replication;
          }
          else if (hashCode == s3_Replication_OperationFailedReplication_HASH)
          {
            return Event::s3_Replication_OperationFailedReplication;
          }
          else if (hashCode == s3_Replication_OperationNotTracked_HASH)
          {
            return Event::s3_Replication_OperationNotTracked;
          }
          else if (hashCode == s3_Replication_OperationMissedThreshold_HASH)
          {
            return Event::s3_Replication_OperationMissedThreshold;
          }
          else if (hashCode == s3_Replication_OperationReplicatedAfterThreshold_HASH)
          {
            return Event::s3_Replication_OperationReplicatedAfterThreshold;
          }
          else if (hashCode == s3_ObjectRestore_Delete_HASH)
          {
            return Event::s3_ObjectRestore_Delete;
          }
          else if (hashCode == s3_LifecycleTransition_HASH)
          {
            return Event::s3_LifecycleTransition;
          }
          else if (hashCode == s3_IntelligentTiering_HASH)
          {
            return Event::s3_IntelligentTiering;
          }
          else if (hashCode == s3_ObjectAcl_Put_HASH)
          {
            return Event::s3_ObjectAcl_Put;
          }
          else if (hashCode == s3_LifecycleExpiration_HASH)
          {
            return Event::s3_LifecycleExpiration;
          }
          else if (hashCode == s3_LifecycleExpiration_Delete_HASH)
          {
            return Event::s3_LifecycleExpiration_Delete;
          }
          else if (hashCode == s3_LifecycleExpiration_DeleteMarkerCreated_HASH)
          {
            return Event::s3_LifecycleExpiration_DeleteMarkerCreated;
          }
          else if (hashCode == s3_ObjectTagging_HASH)
          {
            return Event::s3_ObjectTagging;
          }
          else if (hashCode == s3_ObjectTagging_Put_HASH)
          {
            return Event::s3_ObjectTagging_Put;
          }
          else if (hashCode == s3_ObjectTagging_Delete_HASH)
          {
            return Event::s3_ObjectTagging_Delete;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<Event>(hashCode);
          }

          return Event::NOT_SET;
        }

        Aws::String GetNameForEvent(Event enumValue)
        {
          switch(enumValue)
          {
          case Event::NOT_SET:
            return {};
          case Event::s3_ReducedRedundancyLostObject:
            return "s3:ReducedRedundancyLostObject";
          case Event::s3_ObjectCreated:
            return "s3:ObjectCreated:*";
          case Event::s3_ObjectCreated_Put:
            return "s3:ObjectCreated:Put";
          case Event::s3_ObjectCreated_Post:
            return "s3:ObjectCreated:Post";
          case Event::s3_ObjectCreated_Copy:
            return "s3:ObjectCreated:Copy";
          case Event::s3_ObjectCreated_CompleteMultipartUpload:
            return "s3:ObjectCreated:CompleteMultipartUpload";
          case Event::s3_ObjectRemoved:
            return "s3:ObjectRemoved:*";
          case Event::s3_ObjectRemoved_Delete:
            return "s3:ObjectRemoved:Delete";
          case Event::s3_ObjectRemoved_DeleteMarkerCreated:
            return "s3:ObjectRemoved:DeleteMarkerCreated";
          case Event::s3_ObjectRestore:
            return "s3:ObjectRestore:*";
          case Event::s3_ObjectRestore_Post:
            return "s3:ObjectRestore:Post";
          case Event::s3_ObjectRestore_Completed:
            return "s3:ObjectRestore:Completed";
          case Event::s3_Replication:
            return "s3:Replication:*";
          case Event::s3_Replication_OperationFailedReplication:
            return "s3:Replication:OperationFailedReplication";
          case Event::s3_Replication_OperationNotTracked:
            return "s3:Replication:OperationNotTracked";
          case Event::s3_Replication_OperationMissedThreshold:
            return "s3:Replication:OperationMissedThreshold";
          case Event::s3_Replication_OperationReplicatedAfterThreshold:
            return "s3:Replication:OperationReplicatedAfterThreshold";
          case Event::s3_ObjectRestore_Delete:
            return "s3:ObjectRestore:Delete";
          case Event::s3_LifecycleTransition:
            return "s3:LifecycleTransition";
          case Event::s3_IntelligentTiering:
            return "s3:IntelligentTiering";
          case Event::s3_ObjectAcl_Put:
            return "s3:ObjectAcl:Put";
          case Event::s3_LifecycleExpiration:
            return "s3:LifecycleExpiration:*";
          case Event::s3_LifecycleExpiration_Delete:
            return "s3:LifecycleExpiration:Delete";
          case Event::s3_LifecycleExpiration_DeleteMarkerCreated:
            return "s3:LifecycleExpiration:DeleteMarkerCreated";
          case Event::s3_ObjectTagging:
            return "s3:ObjectTagging:*";
          case Event::s3_ObjectTagging_Put:
            return "s3:ObjectTagging:Put";
          case Event::s3_ObjectTagging_Delete:
            return "s3:ObjectTagging:Delete";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace EventMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
