/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/ArchiveStatus.h>
#include <aws/core/utils/DateTime.h>
#include <aws/s3/model/ServerSideEncryption.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/s3/model/StorageClass.h>
#include <aws/s3/model/RequestCharged.h>
#include <aws/s3/model/ReplicationStatus.h>
#include <aws/s3/model/ObjectLockMode.h>
#include <aws/s3/model/ObjectLockLegalHoldStatus.h>
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
  class HeadObjectResult
  {
  public:
    AWS_S3_API HeadObjectResult();
    AWS_S3_API HeadObjectResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API HeadObjectResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Specifies whether the object retrieved was (true) or was not (false) a Delete
     * Marker. If false, this response header does not appear in the response.</p>
     *  <p>This functionality is not supported for directory buckets.</p> 
     */
    inline bool GetDeleteMarker() const{ return m_deleteMarker; }
    inline void SetDeleteMarker(bool value) { m_deleteMarker = value; }
    inline HeadObjectResult& WithDeleteMarker(bool value) { SetDeleteMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates that a range of bytes was specified.</p>
     */
    inline const Aws::String& GetAcceptRanges() const{ return m_acceptRanges; }
    inline void SetAcceptRanges(const Aws::String& value) { m_acceptRanges = value; }
    inline void SetAcceptRanges(Aws::String&& value) { m_acceptRanges = std::move(value); }
    inline void SetAcceptRanges(const char* value) { m_acceptRanges.assign(value); }
    inline HeadObjectResult& WithAcceptRanges(const Aws::String& value) { SetAcceptRanges(value); return *this;}
    inline HeadObjectResult& WithAcceptRanges(Aws::String&& value) { SetAcceptRanges(std::move(value)); return *this;}
    inline HeadObjectResult& WithAcceptRanges(const char* value) { SetAcceptRanges(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If the object expiration is configured (see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutBucketLifecycleConfiguration.html">
     * <code>PutBucketLifecycleConfiguration</code> </a>), the response includes this
     * header. It includes the <code>expiry-date</code> and <code>rule-id</code>
     * key-value pairs providing object expiration information. The value of the
     * <code>rule-id</code> is URL-encoded.</p>  <p>Object expiration information
     * is not returned in directory buckets and this header returns the value
     * "<code>NotImplemented</code>" in all responses for directory buckets.</p>
     * 
     */
    inline const Aws::String& GetExpiration() const{ return m_expiration; }
    inline void SetExpiration(const Aws::String& value) { m_expiration = value; }
    inline void SetExpiration(Aws::String&& value) { m_expiration = std::move(value); }
    inline void SetExpiration(const char* value) { m_expiration.assign(value); }
    inline HeadObjectResult& WithExpiration(const Aws::String& value) { SetExpiration(value); return *this;}
    inline HeadObjectResult& WithExpiration(Aws::String&& value) { SetExpiration(std::move(value)); return *this;}
    inline HeadObjectResult& WithExpiration(const char* value) { SetExpiration(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If the object is an archived object (an object whose storage class is
     * GLACIER), the response includes this header if either the archive restoration is
     * in progress (see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_RestoreObject.html">RestoreObject</a>
     * or an archive copy is already restored.</p> <p> If an archive copy is already
     * restored, the header value indicates when Amazon S3 is scheduled to delete the
     * object copy. For example:</p> <p> <code>x-amz-restore: ongoing-request="false",
     * expiry-date="Fri, 21 Dec 2012 00:00:00 GMT"</code> </p> <p>If the object
     * restoration is in progress, the header returns the value
     * <code>ongoing-request="true"</code>.</p> <p>For more information about archiving
     * objects, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lifecycle-mgmt.html#lifecycle-transition-general-considerations">Transitioning
     * Objects: General Considerations</a>.</p>  <p>This functionality is not
     * supported for directory buckets. Only the S3 Express One Zone storage class is
     * supported by directory buckets to store objects.</p> 
     */
    inline const Aws::String& GetRestore() const{ return m_restore; }
    inline void SetRestore(const Aws::String& value) { m_restore = value; }
    inline void SetRestore(Aws::String&& value) { m_restore = std::move(value); }
    inline void SetRestore(const char* value) { m_restore.assign(value); }
    inline HeadObjectResult& WithRestore(const Aws::String& value) { SetRestore(value); return *this;}
    inline HeadObjectResult& WithRestore(Aws::String&& value) { SetRestore(std::move(value)); return *this;}
    inline HeadObjectResult& WithRestore(const char* value) { SetRestore(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The archive state of the head object.</p>  <p>This functionality is not
     * supported for directory buckets.</p> 
     */
    inline const ArchiveStatus& GetArchiveStatus() const{ return m_archiveStatus; }
    inline void SetArchiveStatus(const ArchiveStatus& value) { m_archiveStatus = value; }
    inline void SetArchiveStatus(ArchiveStatus&& value) { m_archiveStatus = std::move(value); }
    inline HeadObjectResult& WithArchiveStatus(const ArchiveStatus& value) { SetArchiveStatus(value); return *this;}
    inline HeadObjectResult& WithArchiveStatus(ArchiveStatus&& value) { SetArchiveStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Date and time when the object was last modified.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModified = std::move(value); }
    inline HeadObjectResult& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline HeadObjectResult& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Size of the body in bytes.</p>
     */
    inline long long GetContentLength() const{ return m_contentLength; }
    inline void SetContentLength(long long value) { m_contentLength = value; }
    inline HeadObjectResult& WithContentLength(long long value) { SetContentLength(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 32-bit CRC-32 checksum of the object. This will only be
     * present if it was uploaded with the object. When you use an API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32() const{ return m_checksumCRC32; }
    inline void SetChecksumCRC32(const Aws::String& value) { m_checksumCRC32 = value; }
    inline void SetChecksumCRC32(Aws::String&& value) { m_checksumCRC32 = std::move(value); }
    inline void SetChecksumCRC32(const char* value) { m_checksumCRC32.assign(value); }
    inline HeadObjectResult& WithChecksumCRC32(const Aws::String& value) { SetChecksumCRC32(value); return *this;}
    inline HeadObjectResult& WithChecksumCRC32(Aws::String&& value) { SetChecksumCRC32(std::move(value)); return *this;}
    inline HeadObjectResult& WithChecksumCRC32(const char* value) { SetChecksumCRC32(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 32-bit CRC-32C checksum of the object. This will only be
     * present if it was uploaded with the object. When you use an API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32C() const{ return m_checksumCRC32C; }
    inline void SetChecksumCRC32C(const Aws::String& value) { m_checksumCRC32C = value; }
    inline void SetChecksumCRC32C(Aws::String&& value) { m_checksumCRC32C = std::move(value); }
    inline void SetChecksumCRC32C(const char* value) { m_checksumCRC32C.assign(value); }
    inline HeadObjectResult& WithChecksumCRC32C(const Aws::String& value) { SetChecksumCRC32C(value); return *this;}
    inline HeadObjectResult& WithChecksumCRC32C(Aws::String&& value) { SetChecksumCRC32C(std::move(value)); return *this;}
    inline HeadObjectResult& WithChecksumCRC32C(const char* value) { SetChecksumCRC32C(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 160-bit SHA-1 digest of the object. This will only be
     * present if it was uploaded with the object. When you use the API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA1() const{ return m_checksumSHA1; }
    inline void SetChecksumSHA1(const Aws::String& value) { m_checksumSHA1 = value; }
    inline void SetChecksumSHA1(Aws::String&& value) { m_checksumSHA1 = std::move(value); }
    inline void SetChecksumSHA1(const char* value) { m_checksumSHA1.assign(value); }
    inline HeadObjectResult& WithChecksumSHA1(const Aws::String& value) { SetChecksumSHA1(value); return *this;}
    inline HeadObjectResult& WithChecksumSHA1(Aws::String&& value) { SetChecksumSHA1(std::move(value)); return *this;}
    inline HeadObjectResult& WithChecksumSHA1(const char* value) { SetChecksumSHA1(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 256-bit SHA-256 digest of the object. This will only be
     * present if it was uploaded with the object. When you use an API operation on an
     * object that was uploaded using multipart uploads, this value may not be a direct
     * checksum value of the full object. Instead, it's a calculation based on the
     * checksum values of each individual part. For more information about how
     * checksums are calculated with multipart uploads, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html#large-object-checksums">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA256() const{ return m_checksumSHA256; }
    inline void SetChecksumSHA256(const Aws::String& value) { m_checksumSHA256 = value; }
    inline void SetChecksumSHA256(Aws::String&& value) { m_checksumSHA256 = std::move(value); }
    inline void SetChecksumSHA256(const char* value) { m_checksumSHA256.assign(value); }
    inline HeadObjectResult& WithChecksumSHA256(const Aws::String& value) { SetChecksumSHA256(value); return *this;}
    inline HeadObjectResult& WithChecksumSHA256(Aws::String&& value) { SetChecksumSHA256(std::move(value)); return *this;}
    inline HeadObjectResult& WithChecksumSHA256(const char* value) { SetChecksumSHA256(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An entity tag (ETag) is an opaque identifier assigned by a web server to a
     * specific version of a resource found at a URL.</p>
     */
    inline const Aws::String& GetETag() const{ return m_eTag; }
    inline void SetETag(const Aws::String& value) { m_eTag = value; }
    inline void SetETag(Aws::String&& value) { m_eTag = std::move(value); }
    inline void SetETag(const char* value) { m_eTag.assign(value); }
    inline HeadObjectResult& WithETag(const Aws::String& value) { SetETag(value); return *this;}
    inline HeadObjectResult& WithETag(Aws::String&& value) { SetETag(std::move(value)); return *this;}
    inline HeadObjectResult& WithETag(const char* value) { SetETag(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This is set to the number of metadata entries not returned in
     * <code>x-amz-meta</code> headers. This can happen if you create metadata using an
     * API like SOAP that supports more flexible metadata than the REST API. For
     * example, using SOAP, you can create metadata whose values are not legal HTTP
     * headers.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline int GetMissingMeta() const{ return m_missingMeta; }
    inline void SetMissingMeta(int value) { m_missingMeta = value; }
    inline HeadObjectResult& WithMissingMeta(int value) { SetMissingMeta(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Version ID of the object.</p>  <p>This functionality is not supported
     * for directory buckets.</p> 
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline void SetVersionId(const Aws::String& value) { m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionId.assign(value); }
    inline HeadObjectResult& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline HeadObjectResult& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline HeadObjectResult& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies caching behavior along the request/reply chain.</p>
     */
    inline const Aws::String& GetCacheControl() const{ return m_cacheControl; }
    inline void SetCacheControl(const Aws::String& value) { m_cacheControl = value; }
    inline void SetCacheControl(Aws::String&& value) { m_cacheControl = std::move(value); }
    inline void SetCacheControl(const char* value) { m_cacheControl.assign(value); }
    inline HeadObjectResult& WithCacheControl(const Aws::String& value) { SetCacheControl(value); return *this;}
    inline HeadObjectResult& WithCacheControl(Aws::String&& value) { SetCacheControl(std::move(value)); return *this;}
    inline HeadObjectResult& WithCacheControl(const char* value) { SetCacheControl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies presentational information for the object.</p>
     */
    inline const Aws::String& GetContentDisposition() const{ return m_contentDisposition; }
    inline void SetContentDisposition(const Aws::String& value) { m_contentDisposition = value; }
    inline void SetContentDisposition(Aws::String&& value) { m_contentDisposition = std::move(value); }
    inline void SetContentDisposition(const char* value) { m_contentDisposition.assign(value); }
    inline HeadObjectResult& WithContentDisposition(const Aws::String& value) { SetContentDisposition(value); return *this;}
    inline HeadObjectResult& WithContentDisposition(Aws::String&& value) { SetContentDisposition(std::move(value)); return *this;}
    inline HeadObjectResult& WithContentDisposition(const char* value) { SetContentDisposition(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates what content encodings have been applied to the object and thus
     * what decoding mechanisms must be applied to obtain the media-type referenced by
     * the Content-Type header field.</p>
     */
    inline const Aws::String& GetContentEncoding() const{ return m_contentEncoding; }
    inline void SetContentEncoding(const Aws::String& value) { m_contentEncoding = value; }
    inline void SetContentEncoding(Aws::String&& value) { m_contentEncoding = std::move(value); }
    inline void SetContentEncoding(const char* value) { m_contentEncoding.assign(value); }
    inline HeadObjectResult& WithContentEncoding(const Aws::String& value) { SetContentEncoding(value); return *this;}
    inline HeadObjectResult& WithContentEncoding(Aws::String&& value) { SetContentEncoding(std::move(value)); return *this;}
    inline HeadObjectResult& WithContentEncoding(const char* value) { SetContentEncoding(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The language the content is in.</p>
     */
    inline const Aws::String& GetContentLanguage() const{ return m_contentLanguage; }
    inline void SetContentLanguage(const Aws::String& value) { m_contentLanguage = value; }
    inline void SetContentLanguage(Aws::String&& value) { m_contentLanguage = std::move(value); }
    inline void SetContentLanguage(const char* value) { m_contentLanguage.assign(value); }
    inline HeadObjectResult& WithContentLanguage(const Aws::String& value) { SetContentLanguage(value); return *this;}
    inline HeadObjectResult& WithContentLanguage(Aws::String&& value) { SetContentLanguage(std::move(value)); return *this;}
    inline HeadObjectResult& WithContentLanguage(const char* value) { SetContentLanguage(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A standard MIME type describing the format of the object data.</p>
     */
    inline const Aws::String& GetContentType() const{ return m_contentType; }
    inline void SetContentType(const Aws::String& value) { m_contentType = value; }
    inline void SetContentType(Aws::String&& value) { m_contentType = std::move(value); }
    inline void SetContentType(const char* value) { m_contentType.assign(value); }
    inline HeadObjectResult& WithContentType(const Aws::String& value) { SetContentType(value); return *this;}
    inline HeadObjectResult& WithContentType(Aws::String&& value) { SetContentType(std::move(value)); return *this;}
    inline HeadObjectResult& WithContentType(const char* value) { SetContentType(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time at which the object is no longer cacheable.</p>
     */
    inline const Aws::Utils::DateTime& GetExpires() const{ return m_expires; }
    inline void SetExpires(const Aws::Utils::DateTime& value) { m_expires = value; }
    inline void SetExpires(Aws::Utils::DateTime&& value) { m_expires = std::move(value); }
    inline HeadObjectResult& WithExpires(const Aws::Utils::DateTime& value) { SetExpires(value); return *this;}
    inline HeadObjectResult& WithExpires(Aws::Utils::DateTime&& value) { SetExpires(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If the bucket is configured as a website, redirects requests for this object
     * to another object in the same bucket or to an external URL. Amazon S3 stores the
     * value of this header in the object metadata.</p>  <p>This functionality is
     * not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetWebsiteRedirectLocation() const{ return m_websiteRedirectLocation; }
    inline void SetWebsiteRedirectLocation(const Aws::String& value) { m_websiteRedirectLocation = value; }
    inline void SetWebsiteRedirectLocation(Aws::String&& value) { m_websiteRedirectLocation = std::move(value); }
    inline void SetWebsiteRedirectLocation(const char* value) { m_websiteRedirectLocation.assign(value); }
    inline HeadObjectResult& WithWebsiteRedirectLocation(const Aws::String& value) { SetWebsiteRedirectLocation(value); return *this;}
    inline HeadObjectResult& WithWebsiteRedirectLocation(Aws::String&& value) { SetWebsiteRedirectLocation(std::move(value)); return *this;}
    inline HeadObjectResult& WithWebsiteRedirectLocation(const char* value) { SetWebsiteRedirectLocation(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The server-side encryption algorithm used when you store this object in
     * Amazon S3 (for example, <code>AES256</code>, <code>aws:kms</code>,
     * <code>aws:kms:dsse</code>).</p>
     */
    inline const ServerSideEncryption& GetServerSideEncryption() const{ return m_serverSideEncryption; }
    inline void SetServerSideEncryption(const ServerSideEncryption& value) { m_serverSideEncryption = value; }
    inline void SetServerSideEncryption(ServerSideEncryption&& value) { m_serverSideEncryption = std::move(value); }
    inline HeadObjectResult& WithServerSideEncryption(const ServerSideEncryption& value) { SetServerSideEncryption(value); return *this;}
    inline HeadObjectResult& WithServerSideEncryption(ServerSideEncryption&& value) { SetServerSideEncryption(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A map of metadata to store with the object in S3.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetMetadata() const{ return m_metadata; }
    inline void SetMetadata(const Aws::Map<Aws::String, Aws::String>& value) { m_metadata = value; }
    inline void SetMetadata(Aws::Map<Aws::String, Aws::String>&& value) { m_metadata = std::move(value); }
    inline HeadObjectResult& WithMetadata(const Aws::Map<Aws::String, Aws::String>& value) { SetMetadata(value); return *this;}
    inline HeadObjectResult& WithMetadata(Aws::Map<Aws::String, Aws::String>&& value) { SetMetadata(std::move(value)); return *this;}
    inline HeadObjectResult& AddMetadata(const Aws::String& key, const Aws::String& value) { m_metadata.emplace(key, value); return *this; }
    inline HeadObjectResult& AddMetadata(Aws::String&& key, const Aws::String& value) { m_metadata.emplace(std::move(key), value); return *this; }
    inline HeadObjectResult& AddMetadata(const Aws::String& key, Aws::String&& value) { m_metadata.emplace(key, std::move(value)); return *this; }
    inline HeadObjectResult& AddMetadata(Aws::String&& key, Aws::String&& value) { m_metadata.emplace(std::move(key), std::move(value)); return *this; }
    inline HeadObjectResult& AddMetadata(const char* key, Aws::String&& value) { m_metadata.emplace(key, std::move(value)); return *this; }
    inline HeadObjectResult& AddMetadata(Aws::String&& key, const char* value) { m_metadata.emplace(std::move(key), value); return *this; }
    inline HeadObjectResult& AddMetadata(const char* key, const char* value) { m_metadata.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>If server-side encryption with a customer-provided encryption key was
     * requested, the response will include this header to confirm the encryption
     * algorithm that's used.</p>  <p>This functionality is not supported for
     * directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerAlgorithm() const{ return m_sSECustomerAlgorithm; }
    inline void SetSSECustomerAlgorithm(const Aws::String& value) { m_sSECustomerAlgorithm = value; }
    inline void SetSSECustomerAlgorithm(Aws::String&& value) { m_sSECustomerAlgorithm = std::move(value); }
    inline void SetSSECustomerAlgorithm(const char* value) { m_sSECustomerAlgorithm.assign(value); }
    inline HeadObjectResult& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline HeadObjectResult& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline HeadObjectResult& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If server-side encryption with a customer-provided encryption key was
     * requested, the response will include this header to provide the round-trip
     * message integrity verification of the customer-provided encryption key.</p>
     *  <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerKeyMD5() const{ return m_sSECustomerKeyMD5; }
    inline void SetSSECustomerKeyMD5(const Aws::String& value) { m_sSECustomerKeyMD5 = value; }
    inline void SetSSECustomerKeyMD5(Aws::String&& value) { m_sSECustomerKeyMD5 = std::move(value); }
    inline void SetSSECustomerKeyMD5(const char* value) { m_sSECustomerKeyMD5.assign(value); }
    inline HeadObjectResult& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline HeadObjectResult& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline HeadObjectResult& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, indicates the ID of the KMS key that was used for object
     * encryption.</p>
     */
    inline const Aws::String& GetSSEKMSKeyId() const{ return m_sSEKMSKeyId; }
    inline void SetSSEKMSKeyId(const Aws::String& value) { m_sSEKMSKeyId = value; }
    inline void SetSSEKMSKeyId(Aws::String&& value) { m_sSEKMSKeyId = std::move(value); }
    inline void SetSSEKMSKeyId(const char* value) { m_sSEKMSKeyId.assign(value); }
    inline HeadObjectResult& WithSSEKMSKeyId(const Aws::String& value) { SetSSEKMSKeyId(value); return *this;}
    inline HeadObjectResult& WithSSEKMSKeyId(Aws::String&& value) { SetSSEKMSKeyId(std::move(value)); return *this;}
    inline HeadObjectResult& WithSSEKMSKeyId(const char* value) { SetSSEKMSKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether the object uses an S3 Bucket Key for server-side encryption
     * with Key Management Service (KMS) keys (SSE-KMS).</p>
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabled = value; }
    inline HeadObjectResult& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Provides storage class information of the object. Amazon S3 returns this
     * header for all objects except for S3 Standard storage class objects.</p> <p>For
     * more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html">Storage
     * Classes</a>.</p>  <p> <b>Directory buckets </b> - Only the S3 Express One
     * Zone storage class is supported by directory buckets to store objects.</p>
     * 
     */
    inline const StorageClass& GetStorageClass() const{ return m_storageClass; }
    inline void SetStorageClass(const StorageClass& value) { m_storageClass = value; }
    inline void SetStorageClass(StorageClass&& value) { m_storageClass = std::move(value); }
    inline HeadObjectResult& WithStorageClass(const StorageClass& value) { SetStorageClass(value); return *this;}
    inline HeadObjectResult& WithStorageClass(StorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline HeadObjectResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline HeadObjectResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Amazon S3 can return this header if your request involves a bucket that is
     * either a source or a destination in a replication rule.</p> <p>In replication,
     * you have a source bucket on which you configure replication and destination
     * bucket or buckets where Amazon S3 stores object replicas. When you request an
     * object (<code>GetObject</code>) or object metadata (<code>HeadObject</code>)
     * from these buckets, Amazon S3 will return the
     * <code>x-amz-replication-status</code> header in the response as follows:</p>
     * <ul> <li> <p> <b>If requesting an object from the source bucket</b>, Amazon S3
     * will return the <code>x-amz-replication-status</code> header if the object in
     * your request is eligible for replication.</p> <p> For example, suppose that in
     * your replication configuration, you specify object prefix <code>TaxDocs</code>
     * requesting Amazon S3 to replicate objects with key prefix <code>TaxDocs</code>.
     * Any objects you upload with this key name prefix, for example
     * <code>TaxDocs/document1.pdf</code>, are eligible for replication. For any object
     * request with this key name prefix, Amazon S3 will return the
     * <code>x-amz-replication-status</code> header with value PENDING, COMPLETED or
     * FAILED indicating object replication status.</p> </li> <li> <p> <b>If requesting
     * an object from a destination bucket</b>, Amazon S3 will return the
     * <code>x-amz-replication-status</code> header with value REPLICA if the object in
     * your request is a replica that Amazon S3 created and there is no replica
     * modification replication in progress.</p> </li> <li> <p> <b>When replicating
     * objects to multiple destination buckets</b>, the
     * <code>x-amz-replication-status</code> header acts differently. The header of the
     * source object will only return a value of COMPLETED when replication is
     * successful to all destinations. The header will remain at value PENDING until
     * replication has completed for all destinations. If one or more destinations
     * fails replication the header will return FAILED. </p> </li> </ul> <p>For more
     * information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html">Replication</a>.</p>
     *  <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const ReplicationStatus& GetReplicationStatus() const{ return m_replicationStatus; }
    inline void SetReplicationStatus(const ReplicationStatus& value) { m_replicationStatus = value; }
    inline void SetReplicationStatus(ReplicationStatus&& value) { m_replicationStatus = std::move(value); }
    inline HeadObjectResult& WithReplicationStatus(const ReplicationStatus& value) { SetReplicationStatus(value); return *this;}
    inline HeadObjectResult& WithReplicationStatus(ReplicationStatus&& value) { SetReplicationStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The count of parts this object has. This value is only returned if you
     * specify <code>partNumber</code> in your request and the object was uploaded as a
     * multipart upload.</p>
     */
    inline int GetPartsCount() const{ return m_partsCount; }
    inline void SetPartsCount(int value) { m_partsCount = value; }
    inline HeadObjectResult& WithPartsCount(int value) { SetPartsCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Object Lock mode, if any, that's in effect for this object. This header
     * is only returned if the requester has the <code>s3:GetObjectRetention</code>
     * permission. For more information about S3 Object Lock, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lock.html">Object
     * Lock</a>. </p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const ObjectLockMode& GetObjectLockMode() const{ return m_objectLockMode; }
    inline void SetObjectLockMode(const ObjectLockMode& value) { m_objectLockMode = value; }
    inline void SetObjectLockMode(ObjectLockMode&& value) { m_objectLockMode = std::move(value); }
    inline HeadObjectResult& WithObjectLockMode(const ObjectLockMode& value) { SetObjectLockMode(value); return *this;}
    inline HeadObjectResult& WithObjectLockMode(ObjectLockMode&& value) { SetObjectLockMode(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time when the Object Lock retention period expires. This header
     * is only returned if the requester has the <code>s3:GetObjectRetention</code>
     * permission.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const Aws::Utils::DateTime& GetObjectLockRetainUntilDate() const{ return m_objectLockRetainUntilDate; }
    inline void SetObjectLockRetainUntilDate(const Aws::Utils::DateTime& value) { m_objectLockRetainUntilDate = value; }
    inline void SetObjectLockRetainUntilDate(Aws::Utils::DateTime&& value) { m_objectLockRetainUntilDate = std::move(value); }
    inline HeadObjectResult& WithObjectLockRetainUntilDate(const Aws::Utils::DateTime& value) { SetObjectLockRetainUntilDate(value); return *this;}
    inline HeadObjectResult& WithObjectLockRetainUntilDate(Aws::Utils::DateTime&& value) { SetObjectLockRetainUntilDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether a legal hold is in effect for this object. This header is
     * only returned if the requester has the <code>s3:GetObjectLegalHold</code>
     * permission. This header is not returned if the specified version of this object
     * has never had a legal hold applied. For more information about S3 Object Lock,
     * see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/object-lock.html">Object
     * Lock</a>.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const ObjectLockLegalHoldStatus& GetObjectLockLegalHoldStatus() const{ return m_objectLockLegalHoldStatus; }
    inline void SetObjectLockLegalHoldStatus(const ObjectLockLegalHoldStatus& value) { m_objectLockLegalHoldStatus = value; }
    inline void SetObjectLockLegalHoldStatus(ObjectLockLegalHoldStatus&& value) { m_objectLockLegalHoldStatus = std::move(value); }
    inline HeadObjectResult& WithObjectLockLegalHoldStatus(const ObjectLockLegalHoldStatus& value) { SetObjectLockLegalHoldStatus(value); return *this;}
    inline HeadObjectResult& WithObjectLockLegalHoldStatus(ObjectLockLegalHoldStatus&& value) { SetObjectLockLegalHoldStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time at which the object is no longer cacheable.</p>
     */
    inline const Aws::String& GetExpiresString() const{ return m_expiresString; }
    inline void SetExpiresString(const Aws::String& value) { m_expiresString = value; }
    inline void SetExpiresString(Aws::String&& value) { m_expiresString = std::move(value); }
    inline void SetExpiresString(const char* value) { m_expiresString.assign(value); }
    inline HeadObjectResult& WithExpiresString(const Aws::String& value) { SetExpiresString(value); return *this;}
    inline HeadObjectResult& WithExpiresString(Aws::String&& value) { SetExpiresString(std::move(value)); return *this;}
    inline HeadObjectResult& WithExpiresString(const char* value) { SetExpiresString(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline HeadObjectResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline HeadObjectResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline HeadObjectResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    bool m_deleteMarker;

    Aws::String m_acceptRanges;

    Aws::String m_expiration;

    Aws::String m_restore;

    ArchiveStatus m_archiveStatus;

    Aws::Utils::DateTime m_lastModified;

    long long m_contentLength;

    Aws::String m_checksumCRC32;

    Aws::String m_checksumCRC32C;

    Aws::String m_checksumSHA1;

    Aws::String m_checksumSHA256;

    Aws::String m_eTag;

    int m_missingMeta;

    Aws::String m_versionId;

    Aws::String m_cacheControl;

    Aws::String m_contentDisposition;

    Aws::String m_contentEncoding;

    Aws::String m_contentLanguage;

    Aws::String m_contentType;

    Aws::Utils::DateTime m_expires;

    Aws::String m_websiteRedirectLocation;

    ServerSideEncryption m_serverSideEncryption;

    Aws::Map<Aws::String, Aws::String> m_metadata;

    Aws::String m_sSECustomerAlgorithm;

    Aws::String m_sSECustomerKeyMD5;

    Aws::String m_sSEKMSKeyId;

    bool m_bucketKeyEnabled;

    StorageClass m_storageClass;

    RequestCharged m_requestCharged;

    ReplicationStatus m_replicationStatus;

    int m_partsCount;

    ObjectLockMode m_objectLockMode;

    Aws::Utils::DateTime m_objectLockRetainUntilDate;

    ObjectLockLegalHoldStatus m_objectLockLegalHoldStatus;

    Aws::String m_expiresString;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
