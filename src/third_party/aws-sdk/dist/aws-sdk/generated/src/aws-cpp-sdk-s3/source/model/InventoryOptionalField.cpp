/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/InventoryOptionalField.h>
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
      namespace InventoryOptionalFieldMapper
      {

        static const int Size_HASH = HashingUtils::HashString("Size");
        static const int LastModifiedDate_HASH = HashingUtils::HashString("LastModifiedDate");
        static const int StorageClass_HASH = HashingUtils::HashString("StorageClass");
        static const int ETag_HASH = HashingUtils::HashString("ETag");
        static const int IsMultipartUploaded_HASH = HashingUtils::HashString("IsMultipartUploaded");
        static const int ReplicationStatus_HASH = HashingUtils::HashString("ReplicationStatus");
        static const int EncryptionStatus_HASH = HashingUtils::HashString("EncryptionStatus");
        static const int ObjectLockRetainUntilDate_HASH = HashingUtils::HashString("ObjectLockRetainUntilDate");
        static const int ObjectLockMode_HASH = HashingUtils::HashString("ObjectLockMode");
        static const int ObjectLockLegalHoldStatus_HASH = HashingUtils::HashString("ObjectLockLegalHoldStatus");
        static const int IntelligentTieringAccessTier_HASH = HashingUtils::HashString("IntelligentTieringAccessTier");
        static const int BucketKeyStatus_HASH = HashingUtils::HashString("BucketKeyStatus");
        static const int ChecksumAlgorithm_HASH = HashingUtils::HashString("ChecksumAlgorithm");
        static const int ObjectAccessControlList_HASH = HashingUtils::HashString("ObjectAccessControlList");
        static const int ObjectOwner_HASH = HashingUtils::HashString("ObjectOwner");


        InventoryOptionalField GetInventoryOptionalFieldForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Size_HASH)
          {
            return InventoryOptionalField::Size;
          }
          else if (hashCode == LastModifiedDate_HASH)
          {
            return InventoryOptionalField::LastModifiedDate;
          }
          else if (hashCode == StorageClass_HASH)
          {
            return InventoryOptionalField::StorageClass;
          }
          else if (hashCode == ETag_HASH)
          {
            return InventoryOptionalField::ETag;
          }
          else if (hashCode == IsMultipartUploaded_HASH)
          {
            return InventoryOptionalField::IsMultipartUploaded;
          }
          else if (hashCode == ReplicationStatus_HASH)
          {
            return InventoryOptionalField::ReplicationStatus;
          }
          else if (hashCode == EncryptionStatus_HASH)
          {
            return InventoryOptionalField::EncryptionStatus;
          }
          else if (hashCode == ObjectLockRetainUntilDate_HASH)
          {
            return InventoryOptionalField::ObjectLockRetainUntilDate;
          }
          else if (hashCode == ObjectLockMode_HASH)
          {
            return InventoryOptionalField::ObjectLockMode;
          }
          else if (hashCode == ObjectLockLegalHoldStatus_HASH)
          {
            return InventoryOptionalField::ObjectLockLegalHoldStatus;
          }
          else if (hashCode == IntelligentTieringAccessTier_HASH)
          {
            return InventoryOptionalField::IntelligentTieringAccessTier;
          }
          else if (hashCode == BucketKeyStatus_HASH)
          {
            return InventoryOptionalField::BucketKeyStatus;
          }
          else if (hashCode == ChecksumAlgorithm_HASH)
          {
            return InventoryOptionalField::ChecksumAlgorithm;
          }
          else if (hashCode == ObjectAccessControlList_HASH)
          {
            return InventoryOptionalField::ObjectAccessControlList;
          }
          else if (hashCode == ObjectOwner_HASH)
          {
            return InventoryOptionalField::ObjectOwner;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<InventoryOptionalField>(hashCode);
          }

          return InventoryOptionalField::NOT_SET;
        }

        Aws::String GetNameForInventoryOptionalField(InventoryOptionalField enumValue)
        {
          switch(enumValue)
          {
          case InventoryOptionalField::NOT_SET:
            return {};
          case InventoryOptionalField::Size:
            return "Size";
          case InventoryOptionalField::LastModifiedDate:
            return "LastModifiedDate";
          case InventoryOptionalField::StorageClass:
            return "StorageClass";
          case InventoryOptionalField::ETag:
            return "ETag";
          case InventoryOptionalField::IsMultipartUploaded:
            return "IsMultipartUploaded";
          case InventoryOptionalField::ReplicationStatus:
            return "ReplicationStatus";
          case InventoryOptionalField::EncryptionStatus:
            return "EncryptionStatus";
          case InventoryOptionalField::ObjectLockRetainUntilDate:
            return "ObjectLockRetainUntilDate";
          case InventoryOptionalField::ObjectLockMode:
            return "ObjectLockMode";
          case InventoryOptionalField::ObjectLockLegalHoldStatus:
            return "ObjectLockLegalHoldStatus";
          case InventoryOptionalField::IntelligentTieringAccessTier:
            return "IntelligentTieringAccessTier";
          case InventoryOptionalField::BucketKeyStatus:
            return "BucketKeyStatus";
          case InventoryOptionalField::ChecksumAlgorithm:
            return "ChecksumAlgorithm";
          case InventoryOptionalField::ObjectAccessControlList:
            return "ObjectAccessControlList";
          case InventoryOptionalField::ObjectOwner:
            return "ObjectOwner";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace InventoryOptionalFieldMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
