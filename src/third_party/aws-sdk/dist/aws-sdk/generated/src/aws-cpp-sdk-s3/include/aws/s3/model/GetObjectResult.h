/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/stream/ResponseStream.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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

namespace S3
{
namespace Model
{
  class GetObjectResult
  {
  public:
    AWS_S3_API GetObjectResult();
    //We have to define these because Microsoft doesn't auto generate them
    AWS_S3_API GetObjectResult(GetObjectResult&&);
    AWS_S3_API GetObjectResult& operator=(GetObjectResult&&);
    //we delete these because Microsoft doesn't handle move generation correctly
    //and we therefore don't trust them to get it right here either.
    GetObjectResult(const GetObjectResult&) = delete;
    GetObjectResult& operator=(const GetObjectResult&) = delete;


    AWS_S3_API GetObjectResult(Aws::AmazonWebServiceResult<Aws::Utils::Stream::ResponseStream>&& result);
    AWS_S3_API GetObjectResult& operator=(Aws::AmazonWebServiceResult<Aws::Utils::Stream::ResponseStream>&& result);



    ///@{
    /**
     * <p>Object data.</p>
     */
    inline Aws::IOStream& GetBody() const { return m_body.GetUnderlyingStream(); }
    inline void ReplaceBody(Aws::IOStream* body) { m_body = Aws::Utils::Stream::ResponseStream(body); }

    ///@}

    ///@{
    /**
     * <p>Indicates whether the object retrieved was (true) or was not (false) a Delete
     * Marker. If false, this response header does not appear in the response.</p>
     *  <ul> <li> <p>If the current version of the object is a delete marker,
     * Amazon S3 behaves as if the object was deleted and includes
     * <code>x-amz-delete-marker: true</code> in the response.</p> </li> <li> <p>If the
     * specified version in the request is a delete marker, the response returns a
     * <code>405 Method Not Allowed</code> error and the <code>Last-Modified:
     * timestamp</code> response header.</p> </li> </ul> 
     */
    inline bool GetDeleteMarker() const{ return m_deleteMarker; }
    inline void SetDeleteMarker(bool value) { m_deleteMarker = value; }
    inline GetObjectResult& WithDeleteMarker(bool value) { SetDeleteMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates that a range of bytes was specified in the request.</p>
     */
    inline const Aws::String& GetAcceptRanges() const{ return m_acceptRanges; }
    inline void SetAcceptRanges(const Aws::String& value) { m_acceptRanges = value; }
    inline void SetAcceptRanges(Aws::String&& value) { m_acceptRanges = std::move(value); }
    inline void SetAcceptRanges(const char* value) { m_acceptRanges.assign(value); }
    inline GetObjectResult& WithAcceptRanges(const Aws::String& value) { SetAcceptRanges(value); return *this;}
    inline GetObjectResult& WithAcceptRanges(Aws::String&& value) { SetAcceptRanges(std::move(value)); return *this;}
    inline GetObjectResult& WithAcceptRanges(const char* value) { SetAcceptRanges(value); return *this;}
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
    inline GetObjectResult& WithExpiration(const Aws::String& value) { SetExpiration(value); return *this;}
    inline GetObjectResult& WithExpiration(Aws::String&& value) { SetExpiration(std::move(value)); return *this;}
    inline GetObjectResult& WithExpiration(const char* value) { SetExpiration(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Provides information about object restoration action and expiration time of
     * the restored object copy.</p>  <p>This functionality is not supported for
     * directory buckets. Only the S3 Express One Zone storage class is supported by
     * directory buckets to store objects.</p> 
     */
    inline const Aws::String& GetRestore() const{ return m_restore; }
    inline void SetRestore(const Aws::String& value) { m_restore = value; }
    inline void SetRestore(Aws::String&& value) { m_restore = std::move(value); }
    inline void SetRestore(const char* value) { m_restore.assign(value); }
    inline GetObjectResult& WithRestore(const Aws::String& value) { SetRestore(value); return *this;}
    inline GetObjectResult& WithRestore(Aws::String&& value) { SetRestore(std::move(value)); return *this;}
    inline GetObjectResult& WithRestore(const char* value) { SetRestore(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Date and time when the object was last modified.</p> <p> <b>General purpose
     * buckets </b> - When you specify a <code>versionId</code> of the object in your
     * request, if the specified version in the request is a delete marker, the
     * response returns a <code>405 Method Not Allowed</code> error and the
     * <code>Last-Modified: timestamp</code> response header.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModified = std::move(value); }
    inline GetObjectResult& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline GetObjectResult& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Size of the body in bytes.</p>
     */
    inline long long GetContentLength() const{ return m_contentLength; }
    inline void SetContentLength(long long value) { m_contentLength = value; }
    inline GetObjectResult& WithContentLength(long long value) { SetContentLength(value); return *this;}
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
    inline GetObjectResult& WithETag(const Aws::String& value) { SetETag(value); return *this;}
    inline GetObjectResult& WithETag(Aws::String&& value) { SetETag(std::move(value)); return *this;}
    inline GetObjectResult& WithETag(const char* value) { SetETag(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 32-bit CRC-32 checksum of the object. This will only be
     * present if it was uploaded with the object. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32() const{ return m_checksumCRC32; }
    inline void SetChecksumCRC32(const Aws::String& value) { m_checksumCRC32 = value; }
    inline void SetChecksumCRC32(Aws::String&& value) { m_checksumCRC32 = std::move(value); }
    inline void SetChecksumCRC32(const char* value) { m_checksumCRC32.assign(value); }
    inline GetObjectResult& WithChecksumCRC32(const Aws::String& value) { SetChecksumCRC32(value); return *this;}
    inline GetObjectResult& WithChecksumCRC32(Aws::String&& value) { SetChecksumCRC32(std::move(value)); return *this;}
    inline GetObjectResult& WithChecksumCRC32(const char* value) { SetChecksumCRC32(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 32-bit CRC-32C checksum of the object. This will only be
     * present if it was uploaded with the object. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32C() const{ return m_checksumCRC32C; }
    inline void SetChecksumCRC32C(const Aws::String& value) { m_checksumCRC32C = value; }
    inline void SetChecksumCRC32C(Aws::String&& value) { m_checksumCRC32C = std::move(value); }
    inline void SetChecksumCRC32C(const char* value) { m_checksumCRC32C.assign(value); }
    inline GetObjectResult& WithChecksumCRC32C(const Aws::String& value) { SetChecksumCRC32C(value); return *this;}
    inline GetObjectResult& WithChecksumCRC32C(Aws::String&& value) { SetChecksumCRC32C(std::move(value)); return *this;}
    inline GetObjectResult& WithChecksumCRC32C(const char* value) { SetChecksumCRC32C(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 160-bit SHA-1 digest of the object. This will only be
     * present if it was uploaded with the object. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA1() const{ return m_checksumSHA1; }
    inline void SetChecksumSHA1(const Aws::String& value) { m_checksumSHA1 = value; }
    inline void SetChecksumSHA1(Aws::String&& value) { m_checksumSHA1 = std::move(value); }
    inline void SetChecksumSHA1(const char* value) { m_checksumSHA1.assign(value); }
    inline GetObjectResult& WithChecksumSHA1(const Aws::String& value) { SetChecksumSHA1(value); return *this;}
    inline GetObjectResult& WithChecksumSHA1(Aws::String&& value) { SetChecksumSHA1(std::move(value)); return *this;}
    inline GetObjectResult& WithChecksumSHA1(const char* value) { SetChecksumSHA1(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded, 256-bit SHA-256 digest of the object. This will only be
     * present if it was uploaded with the object. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">
     * Checking object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA256() const{ return m_checksumSHA256; }
    inline void SetChecksumSHA256(const Aws::String& value) { m_checksumSHA256 = value; }
    inline void SetChecksumSHA256(Aws::String&& value) { m_checksumSHA256 = std::move(value); }
    inline void SetChecksumSHA256(const char* value) { m_checksumSHA256.assign(value); }
    inline GetObjectResult& WithChecksumSHA256(const Aws::String& value) { SetChecksumSHA256(value); return *this;}
    inline GetObjectResult& WithChecksumSHA256(Aws::String&& value) { SetChecksumSHA256(std::move(value)); return *this;}
    inline GetObjectResult& WithChecksumSHA256(const char* value) { SetChecksumSHA256(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This is set to the number of metadata entries not returned in the headers
     * that are prefixed with <code>x-amz-meta-</code>. This can happen if you create
     * metadata using an API like SOAP that supports more flexible metadata than the
     * REST API. For example, using SOAP, you can create metadata whose values are not
     * legal HTTP headers.</p>  <p>This functionality is not supported for
     * directory buckets.</p> 
     */
    inline int GetMissingMeta() const{ return m_missingMeta; }
    inline void SetMissingMeta(int value) { m_missingMeta = value; }
    inline GetObjectResult& WithMissingMeta(int value) { SetMissingMeta(value); return *this;}
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
    inline GetObjectResult& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline GetObjectResult& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline GetObjectResult& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies caching behavior along the request/reply chain.</p>
     */
    inline const Aws::String& GetCacheControl() const{ return m_cacheControl; }
    inline void SetCacheControl(const Aws::String& value) { m_cacheControl = value; }
    inline void SetCacheControl(Aws::String&& value) { m_cacheControl = std::move(value); }
    inline void SetCacheControl(const char* value) { m_cacheControl.assign(value); }
    inline GetObjectResult& WithCacheControl(const Aws::String& value) { SetCacheControl(value); return *this;}
    inline GetObjectResult& WithCacheControl(Aws::String&& value) { SetCacheControl(std::move(value)); return *this;}
    inline GetObjectResult& WithCacheControl(const char* value) { SetCacheControl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies presentational information for the object.</p>
     */
    inline const Aws::String& GetContentDisposition() const{ return m_contentDisposition; }
    inline void SetContentDisposition(const Aws::String& value) { m_contentDisposition = value; }
    inline void SetContentDisposition(Aws::String&& value) { m_contentDisposition = std::move(value); }
    inline void SetContentDisposition(const char* value) { m_contentDisposition.assign(value); }
    inline GetObjectResult& WithContentDisposition(const Aws::String& value) { SetContentDisposition(value); return *this;}
    inline GetObjectResult& WithContentDisposition(Aws::String&& value) { SetContentDisposition(std::move(value)); return *this;}
    inline GetObjectResult& WithContentDisposition(const char* value) { SetContentDisposition(value); return *this;}
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
    inline GetObjectResult& WithContentEncoding(const Aws::String& value) { SetContentEncoding(value); return *this;}
    inline GetObjectResult& WithContentEncoding(Aws::String&& value) { SetContentEncoding(std::move(value)); return *this;}
    inline GetObjectResult& WithContentEncoding(const char* value) { SetContentEncoding(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The language the content is in.</p>
     */
    inline const Aws::String& GetContentLanguage() const{ return m_contentLanguage; }
    inline void SetContentLanguage(const Aws::String& value) { m_contentLanguage = value; }
    inline void SetContentLanguage(Aws::String&& value) { m_contentLanguage = std::move(value); }
    inline void SetContentLanguage(const char* value) { m_contentLanguage.assign(value); }
    inline GetObjectResult& WithContentLanguage(const Aws::String& value) { SetContentLanguage(value); return *this;}
    inline GetObjectResult& WithContentLanguage(Aws::String&& value) { SetContentLanguage(std::move(value)); return *this;}
    inline GetObjectResult& WithContentLanguage(const char* value) { SetContentLanguage(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The portion of the object returned in the response.</p>
     */
    inline const Aws::String& GetContentRange() const{ return m_contentRange; }
    inline void SetContentRange(const Aws::String& value) { m_contentRange = value; }
    inline void SetContentRange(Aws::String&& value) { m_contentRange = std::move(value); }
    inline void SetContentRange(const char* value) { m_contentRange.assign(value); }
    inline GetObjectResult& WithContentRange(const Aws::String& value) { SetContentRange(value); return *this;}
    inline GetObjectResult& WithContentRange(Aws::String&& value) { SetContentRange(std::move(value)); return *this;}
    inline GetObjectResult& WithContentRange(const char* value) { SetContentRange(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A standard MIME type describing the format of the object data.</p>
     */
    inline const Aws::String& GetContentType() const{ return m_contentType; }
    inline void SetContentType(const Aws::String& value) { m_contentType = value; }
    inline void SetContentType(Aws::String&& value) { m_contentType = std::move(value); }
    inline void SetContentType(const char* value) { m_contentType.assign(value); }
    inline GetObjectResult& WithContentType(const Aws::String& value) { SetContentType(value); return *this;}
    inline GetObjectResult& WithContentType(Aws::String&& value) { SetContentType(std::move(value)); return *this;}
    inline GetObjectResult& WithContentType(const char* value) { SetContentType(value); return *this;}
    ///@}

    ///@{
    /**
     * Deprecated: Please use ExpiresString instead. 
     * <p>The date and time at which the object is no longer cacheable.</p>
     */
    inline const Aws::Utils::DateTime& GetExpires() const{ return m_expires; }
    inline void SetExpires(const Aws::Utils::DateTime& value) { m_expires = value; }
    inline void SetExpires(Aws::Utils::DateTime&& value) { m_expires = std::move(value); }
    inline GetObjectResult& WithExpires(const Aws::Utils::DateTime& value) { SetExpires(value); return *this;}
    inline GetObjectResult& WithExpires(Aws::Utils::DateTime&& value) { SetExpires(std::move(value)); return *this;}
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
    inline GetObjectResult& WithWebsiteRedirectLocation(const Aws::String& value) { SetWebsiteRedirectLocation(value); return *this;}
    inline GetObjectResult& WithWebsiteRedirectLocation(Aws::String&& value) { SetWebsiteRedirectLocation(std::move(value)); return *this;}
    inline GetObjectResult& WithWebsiteRedirectLocation(const char* value) { SetWebsiteRedirectLocation(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The server-side encryption algorithm used when you store this object in
     * Amazon S3.</p>
     */
    inline const ServerSideEncryption& GetServerSideEncryption() const{ return m_serverSideEncryption; }
    inline void SetServerSideEncryption(const ServerSideEncryption& value) { m_serverSideEncryption = value; }
    inline void SetServerSideEncryption(ServerSideEncryption&& value) { m_serverSideEncryption = std::move(value); }
    inline GetObjectResult& WithServerSideEncryption(const ServerSideEncryption& value) { SetServerSideEncryption(value); return *this;}
    inline GetObjectResult& WithServerSideEncryption(ServerSideEncryption&& value) { SetServerSideEncryption(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A map of metadata to store with the object in S3.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetMetadata() const{ return m_metadata; }
    inline void SetMetadata(const Aws::Map<Aws::String, Aws::String>& value) { m_metadata = value; }
    inline void SetMetadata(Aws::Map<Aws::String, Aws::String>&& value) { m_metadata = std::move(value); }
    inline GetObjectResult& WithMetadata(const Aws::Map<Aws::String, Aws::String>& value) { SetMetadata(value); return *this;}
    inline GetObjectResult& WithMetadata(Aws::Map<Aws::String, Aws::String>&& value) { SetMetadata(std::move(value)); return *this;}
    inline GetObjectResult& AddMetadata(const Aws::String& key, const Aws::String& value) { m_metadata.emplace(key, value); return *this; }
    inline GetObjectResult& AddMetadata(Aws::String&& key, const Aws::String& value) { m_metadata.emplace(std::move(key), value); return *this; }
    inline GetObjectResult& AddMetadata(const Aws::String& key, Aws::String&& value) { m_metadata.emplace(key, std::move(value)); return *this; }
    inline GetObjectResult& AddMetadata(Aws::String&& key, Aws::String&& value) { m_metadata.emplace(std::move(key), std::move(value)); return *this; }
    inline GetObjectResult& AddMetadata(const char* key, Aws::String&& value) { m_metadata.emplace(key, std::move(value)); return *this; }
    inline GetObjectResult& AddMetadata(Aws::String&& key, const char* value) { m_metadata.emplace(std::move(key), value); return *this; }
    inline GetObjectResult& AddMetadata(const char* key, const char* value) { m_metadata.emplace(key, value); return *this; }
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
    inline GetObjectResult& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline GetObjectResult& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline GetObjectResult& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
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
    inline GetObjectResult& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline GetObjectResult& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline GetObjectResult& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
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
    inline GetObjectResult& WithSSEKMSKeyId(const Aws::String& value) { SetSSEKMSKeyId(value); return *this;}
    inline GetObjectResult& WithSSEKMSKeyId(Aws::String&& value) { SetSSEKMSKeyId(std::move(value)); return *this;}
    inline GetObjectResult& WithSSEKMSKeyId(const char* value) { SetSSEKMSKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether the object uses an S3 Bucket Key for server-side encryption
     * with Key Management Service (KMS) keys (SSE-KMS).</p>
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabled = value; }
    inline GetObjectResult& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Provides storage class information of the object. Amazon S3 returns this
     * header for all objects except for S3 Standard storage class objects.</p> 
     * <p> <b>Directory buckets </b> - Only the S3 Express One Zone storage class is
     * supported by directory buckets to store objects.</p> 
     */
    inline const StorageClass& GetStorageClass() const{ return m_storageClass; }
    inline void SetStorageClass(const StorageClass& value) { m_storageClass = value; }
    inline void SetStorageClass(StorageClass&& value) { m_storageClass = std::move(value); }
    inline GetObjectResult& WithStorageClass(const StorageClass& value) { SetStorageClass(value); return *this;}
    inline GetObjectResult& WithStorageClass(StorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestCharged = std::move(value); }
    inline GetObjectResult& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline GetObjectResult& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Amazon S3 can return this if your request involves a bucket that is either a
     * source or destination in a replication rule.</p>  <p>This functionality is
     * not supported for directory buckets.</p> 
     */
    inline const ReplicationStatus& GetReplicationStatus() const{ return m_replicationStatus; }
    inline void SetReplicationStatus(const ReplicationStatus& value) { m_replicationStatus = value; }
    inline void SetReplicationStatus(ReplicationStatus&& value) { m_replicationStatus = std::move(value); }
    inline GetObjectResult& WithReplicationStatus(const ReplicationStatus& value) { SetReplicationStatus(value); return *this;}
    inline GetObjectResult& WithReplicationStatus(ReplicationStatus&& value) { SetReplicationStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The count of parts this object has. This value is only returned if you
     * specify <code>partNumber</code> in your request and the object was uploaded as a
     * multipart upload.</p>
     */
    inline int GetPartsCount() const{ return m_partsCount; }
    inline void SetPartsCount(int value) { m_partsCount = value; }
    inline GetObjectResult& WithPartsCount(int value) { SetPartsCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of tags, if any, on the object, when you have the relevant
     * permission to read object tags.</p> <p>You can use <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObjectTagging.html">GetObjectTagging</a>
     * to retrieve the tag set associated with an object.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline int GetTagCount() const{ return m_tagCount; }
    inline void SetTagCount(int value) { m_tagCount = value; }
    inline GetObjectResult& WithTagCount(int value) { SetTagCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Object Lock mode that's currently in place for this object.</p> 
     * <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const ObjectLockMode& GetObjectLockMode() const{ return m_objectLockMode; }
    inline void SetObjectLockMode(const ObjectLockMode& value) { m_objectLockMode = value; }
    inline void SetObjectLockMode(ObjectLockMode&& value) { m_objectLockMode = std::move(value); }
    inline GetObjectResult& WithObjectLockMode(const ObjectLockMode& value) { SetObjectLockMode(value); return *this;}
    inline GetObjectResult& WithObjectLockMode(ObjectLockMode&& value) { SetObjectLockMode(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time when this object's Object Lock will expire.</p> 
     * <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::Utils::DateTime& GetObjectLockRetainUntilDate() const{ return m_objectLockRetainUntilDate; }
    inline void SetObjectLockRetainUntilDate(const Aws::Utils::DateTime& value) { m_objectLockRetainUntilDate = value; }
    inline void SetObjectLockRetainUntilDate(Aws::Utils::DateTime&& value) { m_objectLockRetainUntilDate = std::move(value); }
    inline GetObjectResult& WithObjectLockRetainUntilDate(const Aws::Utils::DateTime& value) { SetObjectLockRetainUntilDate(value); return *this;}
    inline GetObjectResult& WithObjectLockRetainUntilDate(Aws::Utils::DateTime&& value) { SetObjectLockRetainUntilDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether this object has an active legal hold. This field is only
     * returned if you have permission to view an object's legal hold status. </p>
     *  <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const ObjectLockLegalHoldStatus& GetObjectLockLegalHoldStatus() const{ return m_objectLockLegalHoldStatus; }
    inline void SetObjectLockLegalHoldStatus(const ObjectLockLegalHoldStatus& value) { m_objectLockLegalHoldStatus = value; }
    inline void SetObjectLockLegalHoldStatus(ObjectLockLegalHoldStatus&& value) { m_objectLockLegalHoldStatus = std::move(value); }
    inline GetObjectResult& WithObjectLockLegalHoldStatus(const ObjectLockLegalHoldStatus& value) { SetObjectLockLegalHoldStatus(value); return *this;}
    inline GetObjectResult& WithObjectLockLegalHoldStatus(ObjectLockLegalHoldStatus&& value) { SetObjectLockLegalHoldStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetId2() const{ return m_id2; }
    inline void SetId2(const Aws::String& value) { m_id2 = value; }
    inline void SetId2(Aws::String&& value) { m_id2 = std::move(value); }
    inline void SetId2(const char* value) { m_id2.assign(value); }
    inline GetObjectResult& WithId2(const Aws::String& value) { SetId2(value); return *this;}
    inline GetObjectResult& WithId2(Aws::String&& value) { SetId2(std::move(value)); return *this;}
    inline GetObjectResult& WithId2(const char* value) { SetId2(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetObjectResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetObjectResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetObjectResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time at which the object is no longer cacheable.</p>
     */
    inline const Aws::String& GetExpiresString() const{ return m_expiresString; }
    inline void SetExpiresString(const Aws::String& value) { m_expiresString = value; }
    inline void SetExpiresString(Aws::String&& value) { m_expiresString = std::move(value); }
    inline void SetExpiresString(const char* value) { m_expiresString.assign(value); }
    inline GetObjectResult& WithExpiresString(const Aws::String& value) { SetExpiresString(value); return *this;}
    inline GetObjectResult& WithExpiresString(Aws::String&& value) { SetExpiresString(std::move(value)); return *this;}
    inline GetObjectResult& WithExpiresString(const char* value) { SetExpiresString(value); return *this;}
    ///@}
  private:

    Aws::Utils::Stream::ResponseStream m_body;

    bool m_deleteMarker;

    Aws::String m_acceptRanges;

    Aws::String m_expiration;

    Aws::String m_restore;

    Aws::Utils::DateTime m_lastModified;

    long long m_contentLength;

    Aws::String m_eTag;

    Aws::String m_checksumCRC32;

    Aws::String m_checksumCRC32C;

    Aws::String m_checksumSHA1;

    Aws::String m_checksumSHA256;

    int m_missingMeta;

    Aws::String m_versionId;

    Aws::String m_cacheControl;

    Aws::String m_contentDisposition;

    Aws::String m_contentEncoding;

    Aws::String m_contentLanguage;

    Aws::String m_contentRange;

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

    int m_tagCount;

    ObjectLockMode m_objectLockMode;

    Aws::Utils::DateTime m_objectLockRetainUntilDate;

    ObjectLockLegalHoldStatus m_objectLockLegalHoldStatus;

    Aws::String m_id2;

    Aws::String m_requestId;

    Aws::String m_expiresString;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
