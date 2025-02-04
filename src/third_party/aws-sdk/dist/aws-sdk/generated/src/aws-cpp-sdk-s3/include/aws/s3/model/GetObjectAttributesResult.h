/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/RequestCharged.h>
#include <aws/s3/model/Checksum.h>
#include <aws/s3/model/GetObjectAttributesParts.h>
#include <aws/s3/model/StorageClass.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Xml
{
  class XmlDocument;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{
  class GetObjectAttributesResult
  {
  public:
    AWS_S3_API GetObjectAttributesResult();
    AWS_S3_API GetObjectAttributesResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetObjectAttributesResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Specifies whether the object retrieved was (<code>true</code>) or was not
     * (<code>false</code>) a delete marker. If <code>false</code>, this response
     * header does not appear in the response.</p>  <p>This functionality is not
     * supported for directory buckets.</p> 
     */
    inline bool GetDeleteMarker() const{ return m_deleteMarker; }
    inline void SetDeleteMarker(bool value) { m_deleteMarker = value; }
    inline GetObjectAttributesResult& WithDeleteMarker(bool value) { SetDeleteMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Date and time when the object was last modified.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModified = std::move(value); }
    inline GetObjectAttributesResult& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline GetObjectAttributesResult& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The version ID of the object.</p>  <p>This functionality is not
     * supported for directory buckets.</p> 
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline void SetVersionId(const Aws::String& value) { m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionId.assign(value); }
    inline GetObjectAttributesResult& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline GetObjectAttributesResult& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline GetObjectAttributesResult& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline GetObjectAttributesResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline GetObjectAttributesResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>An ETag is an opaque identifier assigned by a web server to a specific
     * version of a resource found at a URL.</p>
     */
    inline const Aws::String& GetETag() const{ return m_eTag; }
    inline void SetETag(const Aws::String& value) { m_eTag = value; }
    inline void SetETag(Aws::String&& value) { m_eTag = std::move(value); }
    inline void SetETag(const char* value) { m_eTag.assign(value); }
    inline GetObjectAttributesResult& WithETag(const Aws::String& value) { SetETag(value); return *this;}
    inline GetObjectAttributesResult& WithETag(Aws::String&& value) { SetETag(std::move(value)); return *this;}
    inline GetObjectAttributesResult& WithETag(const char* value) { SetETag(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The checksum or digest of the object.</p>
     */
    inline const Checksum& GetChecksum() const{ return m_checksum; }
    inline void SetChecksum(const Checksum& value) { m_checksum = value; }
    inline void SetChecksum(Checksum&& value) { m_checksum = std::move(value); }
    inline GetObjectAttributesResult& WithChecksum(const Checksum& value) { SetChecksum(value); return *this;}
    inline GetObjectAttributesResult& WithChecksum(Checksum&& value) { SetChecksum(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A collection of parts associated with a multipart upload.</p>
     */
    inline const GetObjectAttributesParts& GetObjectParts() const{ return m_objectParts; }
    inline void SetObjectParts(const GetObjectAttributesParts& value) { m_objectParts = value; }
    inline void SetObjectParts(GetObjectAttributesParts&& value) { m_objectParts = std::move(value); }
    inline GetObjectAttributesResult& WithObjectParts(const GetObjectAttributesParts& value) { SetObjectParts(value); return *this;}
    inline GetObjectAttributesResult& WithObjectParts(GetObjectAttributesParts&& value) { SetObjectParts(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Provides the storage class information of the object. Amazon S3 returns this
     * header for all objects except for S3 Standard storage class objects.</p> <p>For
     * more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html">Storage
     * Classes</a>.</p>  <p> <b>Directory buckets</b> - Only the S3 Express One
     * Zone storage class is supported by directory buckets to store objects.</p>
     * 
     */
    inline const StorageClass& GetStorageClass() const{ return m_storageClass; }
    inline void SetStorageClass(const StorageClass& value) { m_storageClass = value; }
    inline void SetStorageClass(StorageClass&& value) { m_storageClass = std::move(value); }
    inline GetObjectAttributesResult& WithStorageClass(const StorageClass& value) { SetStorageClass(value); return *this;}
    inline GetObjectAttributesResult& WithStorageClass(StorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The size of the object in bytes.</p>
     */
    inline long long GetObjectSize() const{ return m_objectSize; }
    inline void SetObjectSize(long long value) { m_objectSize = value; }
    inline GetObjectAttributesResult& WithObjectSize(long long value) { SetObjectSize(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetObjectAttributesResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetObjectAttributesResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetObjectAttributesResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    bool m_deleteMarker;

    Aws::Utils::DateTime m_lastModified;

    Aws::String m_versionId;

    RequestCharged m_requestCharged;

    Aws::String m_eTag;

    Checksum m_checksum;

    GetObjectAttributesParts m_objectParts;

    StorageClass m_storageClass;

    long long m_objectSize;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
