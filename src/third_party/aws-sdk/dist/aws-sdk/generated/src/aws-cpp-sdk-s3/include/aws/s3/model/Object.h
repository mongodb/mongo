/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/ObjectStorageClass.h>
#include <aws/s3/model/Owner.h>
#include <aws/s3/model/RestoreStatus.h>
#include <aws/s3/model/ChecksumAlgorithm.h>
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
   * <p>An object consists of data and its descriptive metadata.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Object">AWS API
   * Reference</a></p>
   */
  class Object
  {
  public:
    AWS_S3_API Object();
    AWS_S3_API Object(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Object& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The name that you assign to an object. You use the object key to retrieve the
     * object.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline Object& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline Object& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline Object& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Creation date of the object.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline bool LastModifiedHasBeenSet() const { return m_lastModifiedHasBeenSet; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModifiedHasBeenSet = true; m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModifiedHasBeenSet = true; m_lastModified = std::move(value); }
    inline Object& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline Object& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The entity tag is a hash of the object. The ETag reflects changes only to the
     * contents of an object, not its metadata. The ETag may or may not be an MD5
     * digest of the object data. Whether or not it is depends on how the object was
     * created and how it is encrypted as described below:</p> <ul> <li> <p>Objects
     * created by the PUT Object, POST Object, or Copy operation, or through the Amazon
     * Web Services Management Console, and are encrypted by SSE-S3 or plaintext, have
     * ETags that are an MD5 digest of their object data.</p> </li> <li> <p>Objects
     * created by the PUT Object, POST Object, or Copy operation, or through the Amazon
     * Web Services Management Console, and are encrypted by SSE-C or SSE-KMS, have
     * ETags that are not an MD5 digest of their object data.</p> </li> <li> <p>If an
     * object is created by either the Multipart Upload or Part Copy operation, the
     * ETag is not an MD5 digest, regardless of the method of encryption. If an object
     * is larger than 16 MB, the Amazon Web Services Management Console will upload or
     * copy that object as a Multipart Upload, and therefore the ETag will not be an
     * MD5 digest.</p> </li> </ul>  <p> <b>Directory buckets</b> - MD5 is not
     * supported by directory buckets.</p> 
     */
    inline const Aws::String& GetETag() const{ return m_eTag; }
    inline bool ETagHasBeenSet() const { return m_eTagHasBeenSet; }
    inline void SetETag(const Aws::String& value) { m_eTagHasBeenSet = true; m_eTag = value; }
    inline void SetETag(Aws::String&& value) { m_eTagHasBeenSet = true; m_eTag = std::move(value); }
    inline void SetETag(const char* value) { m_eTagHasBeenSet = true; m_eTag.assign(value); }
    inline Object& WithETag(const Aws::String& value) { SetETag(value); return *this;}
    inline Object& WithETag(Aws::String&& value) { SetETag(std::move(value)); return *this;}
    inline Object& WithETag(const char* value) { SetETag(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The algorithm that was used to create a checksum of the object.</p>
     */
    inline const Aws::Vector<ChecksumAlgorithm>& GetChecksumAlgorithm() const{ return m_checksumAlgorithm; }
    inline bool ChecksumAlgorithmHasBeenSet() const { return m_checksumAlgorithmHasBeenSet; }
    inline void SetChecksumAlgorithm(const Aws::Vector<ChecksumAlgorithm>& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = value; }
    inline void SetChecksumAlgorithm(Aws::Vector<ChecksumAlgorithm>&& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = std::move(value); }
    inline Object& WithChecksumAlgorithm(const Aws::Vector<ChecksumAlgorithm>& value) { SetChecksumAlgorithm(value); return *this;}
    inline Object& WithChecksumAlgorithm(Aws::Vector<ChecksumAlgorithm>&& value) { SetChecksumAlgorithm(std::move(value)); return *this;}
    inline Object& AddChecksumAlgorithm(const ChecksumAlgorithm& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm.push_back(value); return *this; }
    inline Object& AddChecksumAlgorithm(ChecksumAlgorithm&& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Size in bytes of the object</p>
     */
    inline long long GetSize() const{ return m_size; }
    inline bool SizeHasBeenSet() const { return m_sizeHasBeenSet; }
    inline void SetSize(long long value) { m_sizeHasBeenSet = true; m_size = value; }
    inline Object& WithSize(long long value) { SetSize(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The class of storage used to store the object.</p>  <p> <b>Directory
     * buckets</b> - Only the S3 Express One Zone storage class is supported by
     * directory buckets to store objects.</p> 
     */
    inline const ObjectStorageClass& GetStorageClass() const{ return m_storageClass; }
    inline bool StorageClassHasBeenSet() const { return m_storageClassHasBeenSet; }
    inline void SetStorageClass(const ObjectStorageClass& value) { m_storageClassHasBeenSet = true; m_storageClass = value; }
    inline void SetStorageClass(ObjectStorageClass&& value) { m_storageClassHasBeenSet = true; m_storageClass = std::move(value); }
    inline Object& WithStorageClass(const ObjectStorageClass& value) { SetStorageClass(value); return *this;}
    inline Object& WithStorageClass(ObjectStorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The owner of the object</p>  <p> <b>Directory buckets</b> - The bucket
     * owner is returned as the object owner.</p> 
     */
    inline const Owner& GetOwner() const{ return m_owner; }
    inline bool OwnerHasBeenSet() const { return m_ownerHasBeenSet; }
    inline void SetOwner(const Owner& value) { m_ownerHasBeenSet = true; m_owner = value; }
    inline void SetOwner(Owner&& value) { m_ownerHasBeenSet = true; m_owner = std::move(value); }
    inline Object& WithOwner(const Owner& value) { SetOwner(value); return *this;}
    inline Object& WithOwner(Owner&& value) { SetOwner(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the restoration status of an object. Objects in certain storage
     * classes must be restored before they can be retrieved. For more information
     * about these storage classes and how to work with archived objects, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/archived-objects.html">
     * Working with archived objects</a> in the <i>Amazon S3 User Guide</i>.</p> 
     * <p>This functionality is not supported for directory buckets. Only the S3
     * Express One Zone storage class is supported by directory buckets to store
     * objects.</p> 
     */
    inline const RestoreStatus& GetRestoreStatus() const{ return m_restoreStatus; }
    inline bool RestoreStatusHasBeenSet() const { return m_restoreStatusHasBeenSet; }
    inline void SetRestoreStatus(const RestoreStatus& value) { m_restoreStatusHasBeenSet = true; m_restoreStatus = value; }
    inline void SetRestoreStatus(RestoreStatus&& value) { m_restoreStatusHasBeenSet = true; m_restoreStatus = std::move(value); }
    inline Object& WithRestoreStatus(const RestoreStatus& value) { SetRestoreStatus(value); return *this;}
    inline Object& WithRestoreStatus(RestoreStatus&& value) { SetRestoreStatus(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    Aws::Utils::DateTime m_lastModified;
    bool m_lastModifiedHasBeenSet = false;

    Aws::String m_eTag;
    bool m_eTagHasBeenSet = false;

    Aws::Vector<ChecksumAlgorithm> m_checksumAlgorithm;
    bool m_checksumAlgorithmHasBeenSet = false;

    long long m_size;
    bool m_sizeHasBeenSet = false;

    ObjectStorageClass m_storageClass;
    bool m_storageClassHasBeenSet = false;

    Owner m_owner;
    bool m_ownerHasBeenSet = false;

    RestoreStatus m_restoreStatus;
    bool m_restoreStatusHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
