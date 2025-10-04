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
  enum class InventoryOptionalField
  {
    NOT_SET,
    Size,
    LastModifiedDate,
    StorageClass,
    ETag,
    IsMultipartUploaded,
    ReplicationStatus,
    EncryptionStatus,
    ObjectLockRetainUntilDate,
    ObjectLockMode,
    ObjectLockLegalHoldStatus,
    IntelligentTieringAccessTier,
    BucketKeyStatus,
    ChecksumAlgorithm,
    ObjectAccessControlList,
    ObjectOwner
  };

namespace InventoryOptionalFieldMapper
{
AWS_S3_API InventoryOptionalField GetInventoryOptionalFieldForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForInventoryOptionalField(InventoryOptionalField value);
} // namespace InventoryOptionalFieldMapper
} // namespace Model
} // namespace S3
} // namespace Aws
