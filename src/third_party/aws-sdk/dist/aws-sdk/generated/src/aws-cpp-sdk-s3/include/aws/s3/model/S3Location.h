/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/Encryption.h>
#include <aws/s3/model/ObjectCannedACL.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/Tagging.h>
#include <aws/s3/model/StorageClass.h>
#include <aws/s3/model/Grant.h>
#include <aws/s3/model/MetadataEntry.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{

  /**
   * <p>Describes an Amazon S3 location that will receive the results of the restore
   * request.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/S3Location">AWS API
   * Reference</a></p>
   */
  class S3Location
  {
  public:
    AWS_S3_API S3Location();
    AWS_S3_API S3Location(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API S3Location& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The name of the bucket where the restore results will be placed.</p>
     */
    inline const Aws::String& GetBucketName() const{ return m_bucketName; }
    inline bool BucketNameHasBeenSet() const { return m_bucketNameHasBeenSet; }
    inline void SetBucketName(const Aws::String& value) { m_bucketNameHasBeenSet = true; m_bucketName = value; }
    inline void SetBucketName(Aws::String&& value) { m_bucketNameHasBeenSet = true; m_bucketName = std::move(value); }
    inline void SetBucketName(const char* value) { m_bucketNameHasBeenSet = true; m_bucketName.assign(value); }
    inline S3Location& WithBucketName(const Aws::String& value) { SetBucketName(value); return *this;}
    inline S3Location& WithBucketName(Aws::String&& value) { SetBucketName(std::move(value)); return *this;}
    inline S3Location& WithBucketName(const char* value) { SetBucketName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The prefix that is prepended to the restore results for this request.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline S3Location& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline S3Location& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline S3Location& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    
    inline const Encryption& GetEncryption() const{ return m_encryption; }
    inline bool EncryptionHasBeenSet() const { return m_encryptionHasBeenSet; }
    inline void SetEncryption(const Encryption& value) { m_encryptionHasBeenSet = true; m_encryption = value; }
    inline void SetEncryption(Encryption&& value) { m_encryptionHasBeenSet = true; m_encryption = std::move(value); }
    inline S3Location& WithEncryption(const Encryption& value) { SetEncryption(value); return *this;}
    inline S3Location& WithEncryption(Encryption&& value) { SetEncryption(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The canned ACL to apply to the restore results.</p>
     */
    inline const ObjectCannedACL& GetCannedACL() const{ return m_cannedACL; }
    inline bool CannedACLHasBeenSet() const { return m_cannedACLHasBeenSet; }
    inline void SetCannedACL(const ObjectCannedACL& value) { m_cannedACLHasBeenSet = true; m_cannedACL = value; }
    inline void SetCannedACL(ObjectCannedACL&& value) { m_cannedACLHasBeenSet = true; m_cannedACL = std::move(value); }
    inline S3Location& WithCannedACL(const ObjectCannedACL& value) { SetCannedACL(value); return *this;}
    inline S3Location& WithCannedACL(ObjectCannedACL&& value) { SetCannedACL(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of grants that control access to the staged results.</p>
     */
    inline const Aws::Vector<Grant>& GetAccessControlList() const{ return m_accessControlList; }
    inline bool AccessControlListHasBeenSet() const { return m_accessControlListHasBeenSet; }
    inline void SetAccessControlList(const Aws::Vector<Grant>& value) { m_accessControlListHasBeenSet = true; m_accessControlList = value; }
    inline void SetAccessControlList(Aws::Vector<Grant>&& value) { m_accessControlListHasBeenSet = true; m_accessControlList = std::move(value); }
    inline S3Location& WithAccessControlList(const Aws::Vector<Grant>& value) { SetAccessControlList(value); return *this;}
    inline S3Location& WithAccessControlList(Aws::Vector<Grant>&& value) { SetAccessControlList(std::move(value)); return *this;}
    inline S3Location& AddAccessControlList(const Grant& value) { m_accessControlListHasBeenSet = true; m_accessControlList.push_back(value); return *this; }
    inline S3Location& AddAccessControlList(Grant&& value) { m_accessControlListHasBeenSet = true; m_accessControlList.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The tag-set that is applied to the restore results.</p>
     */
    inline const Tagging& GetTagging() const{ return m_tagging; }
    inline bool TaggingHasBeenSet() const { return m_taggingHasBeenSet; }
    inline void SetTagging(const Tagging& value) { m_taggingHasBeenSet = true; m_tagging = value; }
    inline void SetTagging(Tagging&& value) { m_taggingHasBeenSet = true; m_tagging = std::move(value); }
    inline S3Location& WithTagging(const Tagging& value) { SetTagging(value); return *this;}
    inline S3Location& WithTagging(Tagging&& value) { SetTagging(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of metadata to store with the restore results in S3.</p>
     */
    inline const Aws::Vector<MetadataEntry>& GetUserMetadata() const{ return m_userMetadata; }
    inline bool UserMetadataHasBeenSet() const { return m_userMetadataHasBeenSet; }
    inline void SetUserMetadata(const Aws::Vector<MetadataEntry>& value) { m_userMetadataHasBeenSet = true; m_userMetadata = value; }
    inline void SetUserMetadata(Aws::Vector<MetadataEntry>&& value) { m_userMetadataHasBeenSet = true; m_userMetadata = std::move(value); }
    inline S3Location& WithUserMetadata(const Aws::Vector<MetadataEntry>& value) { SetUserMetadata(value); return *this;}
    inline S3Location& WithUserMetadata(Aws::Vector<MetadataEntry>&& value) { SetUserMetadata(std::move(value)); return *this;}
    inline S3Location& AddUserMetadata(const MetadataEntry& value) { m_userMetadataHasBeenSet = true; m_userMetadata.push_back(value); return *this; }
    inline S3Location& AddUserMetadata(MetadataEntry&& value) { m_userMetadataHasBeenSet = true; m_userMetadata.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The class of storage used to store the restore results.</p>
     */
    inline const StorageClass& GetStorageClass() const{ return m_storageClass; }
    inline bool StorageClassHasBeenSet() const { return m_storageClassHasBeenSet; }
    inline void SetStorageClass(const StorageClass& value) { m_storageClassHasBeenSet = true; m_storageClass = value; }
    inline void SetStorageClass(StorageClass&& value) { m_storageClassHasBeenSet = true; m_storageClass = std::move(value); }
    inline S3Location& WithStorageClass(const StorageClass& value) { SetStorageClass(value); return *this;}
    inline S3Location& WithStorageClass(StorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_bucketName;
    bool m_bucketNameHasBeenSet = false;

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Encryption m_encryption;
    bool m_encryptionHasBeenSet = false;

    ObjectCannedACL m_cannedACL;
    bool m_cannedACLHasBeenSet = false;

    Aws::Vector<Grant> m_accessControlList;
    bool m_accessControlListHasBeenSet = false;

    Tagging m_tagging;
    bool m_taggingHasBeenSet = false;

    Aws::Vector<MetadataEntry> m_userMetadata;
    bool m_userMetadataHasBeenSet = false;

    StorageClass m_storageClass;
    bool m_storageClassHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
