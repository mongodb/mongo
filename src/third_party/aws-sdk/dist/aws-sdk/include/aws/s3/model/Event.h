/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace S3
{
namespace Model
{
  enum class Event
  {
    NOT_SET,
    s3_ReducedRedundancyLostObject,
    s3_ObjectCreated,
    s3_ObjectCreated_Put,
    s3_ObjectCreated_Post,
    s3_ObjectCreated_Copy,
    s3_ObjectCreated_CompleteMultipartUpload,
    s3_ObjectRemoved,
    s3_ObjectRemoved_Delete,
    s3_ObjectRemoved_DeleteMarkerCreated,
    s3_ObjectRestore,
    s3_ObjectRestore_Post,
    s3_ObjectRestore_Completed,
    s3_Replication,
    s3_Replication_OperationFailedReplication,
    s3_Replication_OperationNotTracked,
    s3_Replication_OperationMissedThreshold,
    s3_Replication_OperationReplicatedAfterThreshold,
    s3_ObjectRestore_Delete,
    s3_LifecycleTransition,
    s3_IntelligentTiering,
    s3_ObjectAcl_Put,
    s3_LifecycleExpiration,
    s3_LifecycleExpiration_Delete,
    s3_LifecycleExpiration_DeleteMarkerCreated,
    s3_ObjectTagging,
    s3_ObjectTagging_Put,
    s3_ObjectTagging_Delete
  };

namespace EventMapper
{
AWS_S3_API Event GetEventForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForEvent(Event value);
} // namespace EventMapper
} // namespace Model
} // namespace S3
} // namespace Aws
