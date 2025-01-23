/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/s3/model/StorageClass.h>
#include <aws/s3/model/Owner.h>
#include <aws/s3/model/Initiator.h>
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
   * <p>Container for the <code>MultipartUpload</code> for the Amazon S3
   * object.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/MultipartUpload">AWS
   * API Reference</a></p>
   */
  class MultipartUpload
  {
  public:
    AWS_S3_API MultipartUpload();
    AWS_S3_API MultipartUpload(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API MultipartUpload& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Upload ID that identifies the multipart upload.</p>
     */
    inline const Aws::String& GetUploadId() const{ return m_uploadId; }
    inline bool UploadIdHasBeenSet() const { return m_uploadIdHasBeenSet; }
    inline void SetUploadId(const Aws::String& value) { m_uploadIdHasBeenSet = true; m_uploadId = value; }
    inline void SetUploadId(Aws::String&& value) { m_uploadIdHasBeenSet = true; m_uploadId = std::move(value); }
    inline void SetUploadId(const char* value) { m_uploadIdHasBeenSet = true; m_uploadId.assign(value); }
    inline MultipartUpload& WithUploadId(const Aws::String& value) { SetUploadId(value); return *this;}
    inline MultipartUpload& WithUploadId(Aws::String&& value) { SetUploadId(std::move(value)); return *this;}
    inline MultipartUpload& WithUploadId(const char* value) { SetUploadId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Key of the object for which the multipart upload was initiated.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline MultipartUpload& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline MultipartUpload& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline MultipartUpload& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Date and time at which the multipart upload was initiated.</p>
     */
    inline const Aws::Utils::DateTime& GetInitiated() const{ return m_initiated; }
    inline bool InitiatedHasBeenSet() const { return m_initiatedHasBeenSet; }
    inline void SetInitiated(const Aws::Utils::DateTime& value) { m_initiatedHasBeenSet = true; m_initiated = value; }
    inline void SetInitiated(Aws::Utils::DateTime&& value) { m_initiatedHasBeenSet = true; m_initiated = std::move(value); }
    inline MultipartUpload& WithInitiated(const Aws::Utils::DateTime& value) { SetInitiated(value); return *this;}
    inline MultipartUpload& WithInitiated(Aws::Utils::DateTime&& value) { SetInitiated(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The class of storage used to store the object.</p>  <p> <b>Directory
     * buckets</b> - Only the S3 Express One Zone storage class is supported by
     * directory buckets to store objects.</p> 
     */
    inline const StorageClass& GetStorageClass() const{ return m_storageClass; }
    inline bool StorageClassHasBeenSet() const { return m_storageClassHasBeenSet; }
    inline void SetStorageClass(const StorageClass& value) { m_storageClassHasBeenSet = true; m_storageClass = value; }
    inline void SetStorageClass(StorageClass&& value) { m_storageClassHasBeenSet = true; m_storageClass = std::move(value); }
    inline MultipartUpload& WithStorageClass(const StorageClass& value) { SetStorageClass(value); return *this;}
    inline MultipartUpload& WithStorageClass(StorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the owner of the object that is part of the multipart upload. </p>
     *  <p> <b>Directory buckets</b> - The bucket owner is returned as the object
     * owner for all the objects.</p> 
     */
    inline const Owner& GetOwner() const{ return m_owner; }
    inline bool OwnerHasBeenSet() const { return m_ownerHasBeenSet; }
    inline void SetOwner(const Owner& value) { m_ownerHasBeenSet = true; m_owner = value; }
    inline void SetOwner(Owner&& value) { m_ownerHasBeenSet = true; m_owner = std::move(value); }
    inline MultipartUpload& WithOwner(const Owner& value) { SetOwner(value); return *this;}
    inline MultipartUpload& WithOwner(Owner&& value) { SetOwner(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Identifies who initiated the multipart upload.</p>
     */
    inline const Initiator& GetInitiator() const{ return m_initiator; }
    inline bool InitiatorHasBeenSet() const { return m_initiatorHasBeenSet; }
    inline void SetInitiator(const Initiator& value) { m_initiatorHasBeenSet = true; m_initiator = value; }
    inline void SetInitiator(Initiator&& value) { m_initiatorHasBeenSet = true; m_initiator = std::move(value); }
    inline MultipartUpload& WithInitiator(const Initiator& value) { SetInitiator(value); return *this;}
    inline MultipartUpload& WithInitiator(Initiator&& value) { SetInitiator(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The algorithm that was used to create a checksum of the object.</p>
     */
    inline const ChecksumAlgorithm& GetChecksumAlgorithm() const{ return m_checksumAlgorithm; }
    inline bool ChecksumAlgorithmHasBeenSet() const { return m_checksumAlgorithmHasBeenSet; }
    inline void SetChecksumAlgorithm(const ChecksumAlgorithm& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = value; }
    inline void SetChecksumAlgorithm(ChecksumAlgorithm&& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = std::move(value); }
    inline MultipartUpload& WithChecksumAlgorithm(const ChecksumAlgorithm& value) { SetChecksumAlgorithm(value); return *this;}
    inline MultipartUpload& WithChecksumAlgorithm(ChecksumAlgorithm&& value) { SetChecksumAlgorithm(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_uploadId;
    bool m_uploadIdHasBeenSet = false;

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    Aws::Utils::DateTime m_initiated;
    bool m_initiatedHasBeenSet = false;

    StorageClass m_storageClass;
    bool m_storageClassHasBeenSet = false;

    Owner m_owner;
    bool m_ownerHasBeenSet = false;

    Initiator m_initiator;
    bool m_initiatorHasBeenSet = false;

    ChecksumAlgorithm m_checksumAlgorithm;
    bool m_checksumAlgorithmHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
