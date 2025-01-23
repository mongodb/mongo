/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/s3/model/ObjectLockMode.h>
#include <aws/s3/model/ObjectLockLegalHoldStatus.h>
#include <aws/s3/model/ReplicationStatus.h>
#include <aws/s3/model/RequestCharged.h>
#include <aws/s3/model/ServerSideEncryption.h>
#include <aws/s3/model/StorageClass.h>
#include <utility>

namespace Aws
{
namespace Http
{
    class URI;
} //namespace Http
namespace S3
{
namespace Model
{

  /**
   */
  class WriteGetObjectResponseRequest : public StreamingS3Request
  {
  public:
    AWS_S3_API WriteGetObjectResponseRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "WriteGetObjectResponse"; }

    AWS_S3_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;

    AWS_S3_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    AWS_S3_API bool HasEmbeddedError(IOStream &body, const Http::HeaderValueCollection &header) const override;
    AWS_S3_API bool SignBody() const override { return false; }

    AWS_S3_API bool IsChunked() const override { return true; }

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_S3_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>Route prefix to the HTTP URL generated.</p>
     */
    inline const Aws::String& GetRequestRoute() const{ return m_requestRoute; }
    inline bool RequestRouteHasBeenSet() const { return m_requestRouteHasBeenSet; }
    inline void SetRequestRoute(const Aws::String& value) { m_requestRouteHasBeenSet = true; m_requestRoute = value; }
    inline void SetRequestRoute(Aws::String&& value) { m_requestRouteHasBeenSet = true; m_requestRoute = std::move(value); }
    inline void SetRequestRoute(const char* value) { m_requestRouteHasBeenSet = true; m_requestRoute.assign(value); }
    inline WriteGetObjectResponseRequest& WithRequestRoute(const Aws::String& value) { SetRequestRoute(value); return *this;}
    inline WriteGetObjectResponseRequest& WithRequestRoute(Aws::String&& value) { SetRequestRoute(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithRequestRoute(const char* value) { SetRequestRoute(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A single use encrypted token that maps <code>WriteGetObjectResponse</code> to
     * the end user <code>GetObject</code> request.</p>
     */
    inline const Aws::String& GetRequestToken() const{ return m_requestToken; }
    inline bool RequestTokenHasBeenSet() const { return m_requestTokenHasBeenSet; }
    inline void SetRequestToken(const Aws::String& value) { m_requestTokenHasBeenSet = true; m_requestToken = value; }
    inline void SetRequestToken(Aws::String&& value) { m_requestTokenHasBeenSet = true; m_requestToken = std::move(value); }
    inline void SetRequestToken(const char* value) { m_requestTokenHasBeenSet = true; m_requestToken.assign(value); }
    inline WriteGetObjectResponseRequest& WithRequestToken(const Aws::String& value) { SetRequestToken(value); return *this;}
    inline WriteGetObjectResponseRequest& WithRequestToken(Aws::String&& value) { SetRequestToken(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithRequestToken(const char* value) { SetRequestToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The integer status code for an HTTP response of a corresponding
     * <code>GetObject</code> request. The following is a list of status codes.</p>
     * <ul> <li> <p> <code>200 - OK</code> </p> </li> <li> <p> <code>206 - Partial
     * Content</code> </p> </li> <li> <p> <code>304 - Not Modified</code> </p> </li>
     * <li> <p> <code>400 - Bad Request</code> </p> </li> <li> <p> <code>401 -
     * Unauthorized</code> </p> </li> <li> <p> <code>403 - Forbidden</code> </p> </li>
     * <li> <p> <code>404 - Not Found</code> </p> </li> <li> <p> <code>405 - Method Not
     * Allowed</code> </p> </li> <li> <p> <code>409 - Conflict</code> </p> </li> <li>
     * <p> <code>411 - Length Required</code> </p> </li> <li> <p> <code>412 -
     * Precondition Failed</code> </p> </li> <li> <p> <code>416 - Range Not
     * Satisfiable</code> </p> </li> <li> <p> <code>500 - Internal Server Error</code>
     * </p> </li> <li> <p> <code>503 - Service Unavailable</code> </p> </li> </ul>
     */
    inline int GetStatusCode() const{ return m_statusCode; }
    inline bool StatusCodeHasBeenSet() const { return m_statusCodeHasBeenSet; }
    inline void SetStatusCode(int value) { m_statusCodeHasBeenSet = true; m_statusCode = value; }
    inline WriteGetObjectResponseRequest& WithStatusCode(int value) { SetStatusCode(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A string that uniquely identifies an error condition. Returned in the
     * &lt;Code&gt; tag of the error XML response for a corresponding
     * <code>GetObject</code> call. Cannot be used with a successful
     * <code>StatusCode</code> header or when the transformed object is provided in the
     * body. All error codes from S3 are sentence-cased. The regular expression (regex)
     * value is <code>"^[A-Z][a-zA-Z]+$"</code>.</p>
     */
    inline const Aws::String& GetErrorCode() const{ return m_errorCode; }
    inline bool ErrorCodeHasBeenSet() const { return m_errorCodeHasBeenSet; }
    inline void SetErrorCode(const Aws::String& value) { m_errorCodeHasBeenSet = true; m_errorCode = value; }
    inline void SetErrorCode(Aws::String&& value) { m_errorCodeHasBeenSet = true; m_errorCode = std::move(value); }
    inline void SetErrorCode(const char* value) { m_errorCodeHasBeenSet = true; m_errorCode.assign(value); }
    inline WriteGetObjectResponseRequest& WithErrorCode(const Aws::String& value) { SetErrorCode(value); return *this;}
    inline WriteGetObjectResponseRequest& WithErrorCode(Aws::String&& value) { SetErrorCode(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithErrorCode(const char* value) { SetErrorCode(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Contains a generic description of the error condition. Returned in the
     * &lt;Message&gt; tag of the error XML response for a corresponding
     * <code>GetObject</code> call. Cannot be used with a successful
     * <code>StatusCode</code> header or when the transformed object is provided in
     * body.</p>
     */
    inline const Aws::String& GetErrorMessage() const{ return m_errorMessage; }
    inline bool ErrorMessageHasBeenSet() const { return m_errorMessageHasBeenSet; }
    inline void SetErrorMessage(const Aws::String& value) { m_errorMessageHasBeenSet = true; m_errorMessage = value; }
    inline void SetErrorMessage(Aws::String&& value) { m_errorMessageHasBeenSet = true; m_errorMessage = std::move(value); }
    inline void SetErrorMessage(const char* value) { m_errorMessageHasBeenSet = true; m_errorMessage.assign(value); }
    inline WriteGetObjectResponseRequest& WithErrorMessage(const Aws::String& value) { SetErrorMessage(value); return *this;}
    inline WriteGetObjectResponseRequest& WithErrorMessage(Aws::String&& value) { SetErrorMessage(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithErrorMessage(const char* value) { SetErrorMessage(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates that a range of bytes was specified.</p>
     */
    inline const Aws::String& GetAcceptRanges() const{ return m_acceptRanges; }
    inline bool AcceptRangesHasBeenSet() const { return m_acceptRangesHasBeenSet; }
    inline void SetAcceptRanges(const Aws::String& value) { m_acceptRangesHasBeenSet = true; m_acceptRanges = value; }
    inline void SetAcceptRanges(Aws::String&& value) { m_acceptRangesHasBeenSet = true; m_acceptRanges = std::move(value); }
    inline void SetAcceptRanges(const char* value) { m_acceptRangesHasBeenSet = true; m_acceptRanges.assign(value); }
    inline WriteGetObjectResponseRequest& WithAcceptRanges(const Aws::String& value) { SetAcceptRanges(value); return *this;}
    inline WriteGetObjectResponseRequest& WithAcceptRanges(Aws::String&& value) { SetAcceptRanges(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithAcceptRanges(const char* value) { SetAcceptRanges(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies caching behavior along the request/reply chain.</p>
     */
    inline const Aws::String& GetCacheControl() const{ return m_cacheControl; }
    inline bool CacheControlHasBeenSet() const { return m_cacheControlHasBeenSet; }
    inline void SetCacheControl(const Aws::String& value) { m_cacheControlHasBeenSet = true; m_cacheControl = value; }
    inline void SetCacheControl(Aws::String&& value) { m_cacheControlHasBeenSet = true; m_cacheControl = std::move(value); }
    inline void SetCacheControl(const char* value) { m_cacheControlHasBeenSet = true; m_cacheControl.assign(value); }
    inline WriteGetObjectResponseRequest& WithCacheControl(const Aws::String& value) { SetCacheControl(value); return *this;}
    inline WriteGetObjectResponseRequest& WithCacheControl(Aws::String&& value) { SetCacheControl(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithCacheControl(const char* value) { SetCacheControl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies presentational information for the object.</p>
     */
    inline const Aws::String& GetContentDisposition() const{ return m_contentDisposition; }
    inline bool ContentDispositionHasBeenSet() const { return m_contentDispositionHasBeenSet; }
    inline void SetContentDisposition(const Aws::String& value) { m_contentDispositionHasBeenSet = true; m_contentDisposition = value; }
    inline void SetContentDisposition(Aws::String&& value) { m_contentDispositionHasBeenSet = true; m_contentDisposition = std::move(value); }
    inline void SetContentDisposition(const char* value) { m_contentDispositionHasBeenSet = true; m_contentDisposition.assign(value); }
    inline WriteGetObjectResponseRequest& WithContentDisposition(const Aws::String& value) { SetContentDisposition(value); return *this;}
    inline WriteGetObjectResponseRequest& WithContentDisposition(Aws::String&& value) { SetContentDisposition(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithContentDisposition(const char* value) { SetContentDisposition(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies what content encodings have been applied to the object and thus
     * what decoding mechanisms must be applied to obtain the media-type referenced by
     * the Content-Type header field.</p>
     */
    inline const Aws::String& GetContentEncoding() const{ return m_contentEncoding; }
    inline bool ContentEncodingHasBeenSet() const { return m_contentEncodingHasBeenSet; }
    inline void SetContentEncoding(const Aws::String& value) { m_contentEncodingHasBeenSet = true; m_contentEncoding = value; }
    inline void SetContentEncoding(Aws::String&& value) { m_contentEncodingHasBeenSet = true; m_contentEncoding = std::move(value); }
    inline void SetContentEncoding(const char* value) { m_contentEncodingHasBeenSet = true; m_contentEncoding.assign(value); }
    inline WriteGetObjectResponseRequest& WithContentEncoding(const Aws::String& value) { SetContentEncoding(value); return *this;}
    inline WriteGetObjectResponseRequest& WithContentEncoding(Aws::String&& value) { SetContentEncoding(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithContentEncoding(const char* value) { SetContentEncoding(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The language the content is in.</p>
     */
    inline const Aws::String& GetContentLanguage() const{ return m_contentLanguage; }
    inline bool ContentLanguageHasBeenSet() const { return m_contentLanguageHasBeenSet; }
    inline void SetContentLanguage(const Aws::String& value) { m_contentLanguageHasBeenSet = true; m_contentLanguage = value; }
    inline void SetContentLanguage(Aws::String&& value) { m_contentLanguageHasBeenSet = true; m_contentLanguage = std::move(value); }
    inline void SetContentLanguage(const char* value) { m_contentLanguageHasBeenSet = true; m_contentLanguage.assign(value); }
    inline WriteGetObjectResponseRequest& WithContentLanguage(const Aws::String& value) { SetContentLanguage(value); return *this;}
    inline WriteGetObjectResponseRequest& WithContentLanguage(Aws::String&& value) { SetContentLanguage(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithContentLanguage(const char* value) { SetContentLanguage(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The size of the content body in bytes.</p>
     */
    inline long long GetContentLength() const{ return m_contentLength; }
    inline bool ContentLengthHasBeenSet() const { return m_contentLengthHasBeenSet; }
    inline void SetContentLength(long long value) { m_contentLengthHasBeenSet = true; m_contentLength = value; }
    inline WriteGetObjectResponseRequest& WithContentLength(long long value) { SetContentLength(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The portion of the object returned in the response.</p>
     */
    inline const Aws::String& GetContentRange() const{ return m_contentRange; }
    inline bool ContentRangeHasBeenSet() const { return m_contentRangeHasBeenSet; }
    inline void SetContentRange(const Aws::String& value) { m_contentRangeHasBeenSet = true; m_contentRange = value; }
    inline void SetContentRange(Aws::String&& value) { m_contentRangeHasBeenSet = true; m_contentRange = std::move(value); }
    inline void SetContentRange(const char* value) { m_contentRangeHasBeenSet = true; m_contentRange.assign(value); }
    inline WriteGetObjectResponseRequest& WithContentRange(const Aws::String& value) { SetContentRange(value); return *this;}
    inline WriteGetObjectResponseRequest& WithContentRange(Aws::String&& value) { SetContentRange(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithContentRange(const char* value) { SetContentRange(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This specifies the
     * base64-encoded, 32-bit CRC-32 checksum of the object returned by the Object
     * Lambda function. This may not match the checksum for the object stored in Amazon
     * S3. Amazon S3 will perform validation of the checksum values only when the
     * original <code>GetObject</code> request required checksum validation. For more
     * information about checksums, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p> <p>Only one
     * checksum header can be specified at a time. If you supply multiple checksum
     * headers, this request will fail.</p> <p/>
     */
    inline const Aws::String& GetChecksumCRC32() const{ return m_checksumCRC32; }
    inline bool ChecksumCRC32HasBeenSet() const { return m_checksumCRC32HasBeenSet; }
    inline void SetChecksumCRC32(const Aws::String& value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32 = value; }
    inline void SetChecksumCRC32(Aws::String&& value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32 = std::move(value); }
    inline void SetChecksumCRC32(const char* value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32.assign(value); }
    inline WriteGetObjectResponseRequest& WithChecksumCRC32(const Aws::String& value) { SetChecksumCRC32(value); return *this;}
    inline WriteGetObjectResponseRequest& WithChecksumCRC32(Aws::String&& value) { SetChecksumCRC32(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithChecksumCRC32(const char* value) { SetChecksumCRC32(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This specifies the
     * base64-encoded, 32-bit CRC-32C checksum of the object returned by the Object
     * Lambda function. This may not match the checksum for the object stored in Amazon
     * S3. Amazon S3 will perform validation of the checksum values only when the
     * original <code>GetObject</code> request required checksum validation. For more
     * information about checksums, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p> <p>Only one
     * checksum header can be specified at a time. If you supply multiple checksum
     * headers, this request will fail.</p>
     */
    inline const Aws::String& GetChecksumCRC32C() const{ return m_checksumCRC32C; }
    inline bool ChecksumCRC32CHasBeenSet() const { return m_checksumCRC32CHasBeenSet; }
    inline void SetChecksumCRC32C(const Aws::String& value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C = value; }
    inline void SetChecksumCRC32C(Aws::String&& value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C = std::move(value); }
    inline void SetChecksumCRC32C(const char* value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C.assign(value); }
    inline WriteGetObjectResponseRequest& WithChecksumCRC32C(const Aws::String& value) { SetChecksumCRC32C(value); return *this;}
    inline WriteGetObjectResponseRequest& WithChecksumCRC32C(Aws::String&& value) { SetChecksumCRC32C(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithChecksumCRC32C(const char* value) { SetChecksumCRC32C(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This specifies the
     * base64-encoded, 160-bit SHA-1 digest of the object returned by the Object Lambda
     * function. This may not match the checksum for the object stored in Amazon S3.
     * Amazon S3 will perform validation of the checksum values only when the original
     * <code>GetObject</code> request required checksum validation. For more
     * information about checksums, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p> <p>Only one
     * checksum header can be specified at a time. If you supply multiple checksum
     * headers, this request will fail.</p>
     */
    inline const Aws::String& GetChecksumSHA1() const{ return m_checksumSHA1; }
    inline bool ChecksumSHA1HasBeenSet() const { return m_checksumSHA1HasBeenSet; }
    inline void SetChecksumSHA1(const Aws::String& value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1 = value; }
    inline void SetChecksumSHA1(Aws::String&& value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1 = std::move(value); }
    inline void SetChecksumSHA1(const char* value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1.assign(value); }
    inline WriteGetObjectResponseRequest& WithChecksumSHA1(const Aws::String& value) { SetChecksumSHA1(value); return *this;}
    inline WriteGetObjectResponseRequest& WithChecksumSHA1(Aws::String&& value) { SetChecksumSHA1(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithChecksumSHA1(const char* value) { SetChecksumSHA1(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This specifies the
     * base64-encoded, 256-bit SHA-256 digest of the object returned by the Object
     * Lambda function. This may not match the checksum for the object stored in Amazon
     * S3. Amazon S3 will perform validation of the checksum values only when the
     * original <code>GetObject</code> request required checksum validation. For more
     * information about checksums, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p> <p>Only one
     * checksum header can be specified at a time. If you supply multiple checksum
     * headers, this request will fail.</p>
     */
    inline const Aws::String& GetChecksumSHA256() const{ return m_checksumSHA256; }
    inline bool ChecksumSHA256HasBeenSet() const { return m_checksumSHA256HasBeenSet; }
    inline void SetChecksumSHA256(const Aws::String& value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256 = value; }
    inline void SetChecksumSHA256(Aws::String&& value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256 = std::move(value); }
    inline void SetChecksumSHA256(const char* value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256.assign(value); }
    inline WriteGetObjectResponseRequest& WithChecksumSHA256(const Aws::String& value) { SetChecksumSHA256(value); return *this;}
    inline WriteGetObjectResponseRequest& WithChecksumSHA256(Aws::String&& value) { SetChecksumSHA256(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithChecksumSHA256(const char* value) { SetChecksumSHA256(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether an object stored in Amazon S3 is (<code>true</code>) or is
     * not (<code>false</code>) a delete marker. </p>
     */
    inline bool GetDeleteMarker() const{ return m_deleteMarker; }
    inline bool DeleteMarkerHasBeenSet() const { return m_deleteMarkerHasBeenSet; }
    inline void SetDeleteMarker(bool value) { m_deleteMarkerHasBeenSet = true; m_deleteMarker = value; }
    inline WriteGetObjectResponseRequest& WithDeleteMarker(bool value) { SetDeleteMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An opaque identifier assigned by a web server to a specific version of a
     * resource found at a URL. </p>
     */
    inline const Aws::String& GetETag() const{ return m_eTag; }
    inline bool ETagHasBeenSet() const { return m_eTagHasBeenSet; }
    inline void SetETag(const Aws::String& value) { m_eTagHasBeenSet = true; m_eTag = value; }
    inline void SetETag(Aws::String&& value) { m_eTagHasBeenSet = true; m_eTag = std::move(value); }
    inline void SetETag(const char* value) { m_eTagHasBeenSet = true; m_eTag.assign(value); }
    inline WriteGetObjectResponseRequest& WithETag(const Aws::String& value) { SetETag(value); return *this;}
    inline WriteGetObjectResponseRequest& WithETag(Aws::String&& value) { SetETag(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithETag(const char* value) { SetETag(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time at which the object is no longer cacheable.</p>
     */
    inline const Aws::Utils::DateTime& GetExpires() const{ return m_expires; }
    inline bool ExpiresHasBeenSet() const { return m_expiresHasBeenSet; }
    inline void SetExpires(const Aws::Utils::DateTime& value) { m_expiresHasBeenSet = true; m_expires = value; }
    inline void SetExpires(Aws::Utils::DateTime&& value) { m_expiresHasBeenSet = true; m_expires = std::move(value); }
    inline WriteGetObjectResponseRequest& WithExpires(const Aws::Utils::DateTime& value) { SetExpires(value); return *this;}
    inline WriteGetObjectResponseRequest& WithExpires(Aws::Utils::DateTime&& value) { SetExpires(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If the object expiration is configured (see PUT Bucket lifecycle), the
     * response includes this header. It includes the <code>expiry-date</code> and
     * <code>rule-id</code> key-value pairs that provide the object expiration
     * information. The value of the <code>rule-id</code> is URL-encoded. </p>
     */
    inline const Aws::String& GetExpiration() const{ return m_expiration; }
    inline bool ExpirationHasBeenSet() const { return m_expirationHasBeenSet; }
    inline void SetExpiration(const Aws::String& value) { m_expirationHasBeenSet = true; m_expiration = value; }
    inline void SetExpiration(Aws::String&& value) { m_expirationHasBeenSet = true; m_expiration = std::move(value); }
    inline void SetExpiration(const char* value) { m_expirationHasBeenSet = true; m_expiration.assign(value); }
    inline WriteGetObjectResponseRequest& WithExpiration(const Aws::String& value) { SetExpiration(value); return *this;}
    inline WriteGetObjectResponseRequest& WithExpiration(Aws::String&& value) { SetExpiration(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithExpiration(const char* value) { SetExpiration(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time that the object was last modified.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline bool LastModifiedHasBeenSet() const { return m_lastModifiedHasBeenSet; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModifiedHasBeenSet = true; m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModifiedHasBeenSet = true; m_lastModified = std::move(value); }
    inline WriteGetObjectResponseRequest& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline WriteGetObjectResponseRequest& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Set to the number of metadata entries not returned in <code>x-amz-meta</code>
     * headers. This can happen if you create metadata using an API like SOAP that
     * supports more flexible metadata than the REST API. For example, using SOAP, you
     * can create metadata whose values are not legal HTTP headers.</p>
     */
    inline int GetMissingMeta() const{ return m_missingMeta; }
    inline bool MissingMetaHasBeenSet() const { return m_missingMetaHasBeenSet; }
    inline void SetMissingMeta(int value) { m_missingMetaHasBeenSet = true; m_missingMeta = value; }
    inline WriteGetObjectResponseRequest& WithMissingMeta(int value) { SetMissingMeta(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A map of metadata to store with the object in S3.</p>
     */
    inline const Aws::Map<Aws::String, Aws::String>& GetMetadata() const{ return m_metadata; }
    inline bool MetadataHasBeenSet() const { return m_metadataHasBeenSet; }
    inline void SetMetadata(const Aws::Map<Aws::String, Aws::String>& value) { m_metadataHasBeenSet = true; m_metadata = value; }
    inline void SetMetadata(Aws::Map<Aws::String, Aws::String>&& value) { m_metadataHasBeenSet = true; m_metadata = std::move(value); }
    inline WriteGetObjectResponseRequest& WithMetadata(const Aws::Map<Aws::String, Aws::String>& value) { SetMetadata(value); return *this;}
    inline WriteGetObjectResponseRequest& WithMetadata(Aws::Map<Aws::String, Aws::String>&& value) { SetMetadata(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& AddMetadata(const Aws::String& key, const Aws::String& value) { m_metadataHasBeenSet = true; m_metadata.emplace(key, value); return *this; }
    inline WriteGetObjectResponseRequest& AddMetadata(Aws::String&& key, const Aws::String& value) { m_metadataHasBeenSet = true; m_metadata.emplace(std::move(key), value); return *this; }
    inline WriteGetObjectResponseRequest& AddMetadata(const Aws::String& key, Aws::String&& value) { m_metadataHasBeenSet = true; m_metadata.emplace(key, std::move(value)); return *this; }
    inline WriteGetObjectResponseRequest& AddMetadata(Aws::String&& key, Aws::String&& value) { m_metadataHasBeenSet = true; m_metadata.emplace(std::move(key), std::move(value)); return *this; }
    inline WriteGetObjectResponseRequest& AddMetadata(const char* key, Aws::String&& value) { m_metadataHasBeenSet = true; m_metadata.emplace(key, std::move(value)); return *this; }
    inline WriteGetObjectResponseRequest& AddMetadata(Aws::String&& key, const char* value) { m_metadataHasBeenSet = true; m_metadata.emplace(std::move(key), value); return *this; }
    inline WriteGetObjectResponseRequest& AddMetadata(const char* key, const char* value) { m_metadataHasBeenSet = true; m_metadata.emplace(key, value); return *this; }
    ///@}

    ///@{
    /**
     * <p>Indicates whether an object stored in Amazon S3 has Object Lock enabled. For
     * more information about S3 Object Lock, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-lock.html">Object
     * Lock</a>.</p>
     */
    inline const ObjectLockMode& GetObjectLockMode() const{ return m_objectLockMode; }
    inline bool ObjectLockModeHasBeenSet() const { return m_objectLockModeHasBeenSet; }
    inline void SetObjectLockMode(const ObjectLockMode& value) { m_objectLockModeHasBeenSet = true; m_objectLockMode = value; }
    inline void SetObjectLockMode(ObjectLockMode&& value) { m_objectLockModeHasBeenSet = true; m_objectLockMode = std::move(value); }
    inline WriteGetObjectResponseRequest& WithObjectLockMode(const ObjectLockMode& value) { SetObjectLockMode(value); return *this;}
    inline WriteGetObjectResponseRequest& WithObjectLockMode(ObjectLockMode&& value) { SetObjectLockMode(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether an object stored in Amazon S3 has an active legal hold.</p>
     */
    inline const ObjectLockLegalHoldStatus& GetObjectLockLegalHoldStatus() const{ return m_objectLockLegalHoldStatus; }
    inline bool ObjectLockLegalHoldStatusHasBeenSet() const { return m_objectLockLegalHoldStatusHasBeenSet; }
    inline void SetObjectLockLegalHoldStatus(const ObjectLockLegalHoldStatus& value) { m_objectLockLegalHoldStatusHasBeenSet = true; m_objectLockLegalHoldStatus = value; }
    inline void SetObjectLockLegalHoldStatus(ObjectLockLegalHoldStatus&& value) { m_objectLockLegalHoldStatusHasBeenSet = true; m_objectLockLegalHoldStatus = std::move(value); }
    inline WriteGetObjectResponseRequest& WithObjectLockLegalHoldStatus(const ObjectLockLegalHoldStatus& value) { SetObjectLockLegalHoldStatus(value); return *this;}
    inline WriteGetObjectResponseRequest& WithObjectLockLegalHoldStatus(ObjectLockLegalHoldStatus&& value) { SetObjectLockLegalHoldStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date and time when Object Lock is configured to expire.</p>
     */
    inline const Aws::Utils::DateTime& GetObjectLockRetainUntilDate() const{ return m_objectLockRetainUntilDate; }
    inline bool ObjectLockRetainUntilDateHasBeenSet() const { return m_objectLockRetainUntilDateHasBeenSet; }
    inline void SetObjectLockRetainUntilDate(const Aws::Utils::DateTime& value) { m_objectLockRetainUntilDateHasBeenSet = true; m_objectLockRetainUntilDate = value; }
    inline void SetObjectLockRetainUntilDate(Aws::Utils::DateTime&& value) { m_objectLockRetainUntilDateHasBeenSet = true; m_objectLockRetainUntilDate = std::move(value); }
    inline WriteGetObjectResponseRequest& WithObjectLockRetainUntilDate(const Aws::Utils::DateTime& value) { SetObjectLockRetainUntilDate(value); return *this;}
    inline WriteGetObjectResponseRequest& WithObjectLockRetainUntilDate(Aws::Utils::DateTime&& value) { SetObjectLockRetainUntilDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The count of parts this object has.</p>
     */
    inline int GetPartsCount() const{ return m_partsCount; }
    inline bool PartsCountHasBeenSet() const { return m_partsCountHasBeenSet; }
    inline void SetPartsCount(int value) { m_partsCountHasBeenSet = true; m_partsCount = value; }
    inline WriteGetObjectResponseRequest& WithPartsCount(int value) { SetPartsCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates if request involves bucket that is either a source or destination
     * in a Replication rule. For more information about S3 Replication, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/replication.html">Replication</a>.</p>
     */
    inline const ReplicationStatus& GetReplicationStatus() const{ return m_replicationStatus; }
    inline bool ReplicationStatusHasBeenSet() const { return m_replicationStatusHasBeenSet; }
    inline void SetReplicationStatus(const ReplicationStatus& value) { m_replicationStatusHasBeenSet = true; m_replicationStatus = value; }
    inline void SetReplicationStatus(ReplicationStatus&& value) { m_replicationStatusHasBeenSet = true; m_replicationStatus = std::move(value); }
    inline WriteGetObjectResponseRequest& WithReplicationStatus(const ReplicationStatus& value) { SetReplicationStatus(value); return *this;}
    inline WriteGetObjectResponseRequest& WithReplicationStatus(ReplicationStatus&& value) { SetReplicationStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const RequestCharged& GetRequestCharged() const{ return m_requestCharged; }
    inline bool RequestChargedHasBeenSet() const { return m_requestChargedHasBeenSet; }
    inline void SetRequestCharged(const RequestCharged& value) { m_requestChargedHasBeenSet = true; m_requestCharged = value; }
    inline void SetRequestCharged(RequestCharged&& value) { m_requestChargedHasBeenSet = true; m_requestCharged = std::move(value); }
    inline WriteGetObjectResponseRequest& WithRequestCharged(const RequestCharged& value) { SetRequestCharged(value); return *this;}
    inline WriteGetObjectResponseRequest& WithRequestCharged(RequestCharged&& value) { SetRequestCharged(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Provides information about object restoration operation and expiration time
     * of the restored object copy.</p>
     */
    inline const Aws::String& GetRestore() const{ return m_restore; }
    inline bool RestoreHasBeenSet() const { return m_restoreHasBeenSet; }
    inline void SetRestore(const Aws::String& value) { m_restoreHasBeenSet = true; m_restore = value; }
    inline void SetRestore(Aws::String&& value) { m_restoreHasBeenSet = true; m_restore = std::move(value); }
    inline void SetRestore(const char* value) { m_restoreHasBeenSet = true; m_restore.assign(value); }
    inline WriteGetObjectResponseRequest& WithRestore(const Aws::String& value) { SetRestore(value); return *this;}
    inline WriteGetObjectResponseRequest& WithRestore(Aws::String&& value) { SetRestore(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithRestore(const char* value) { SetRestore(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The server-side encryption algorithm used when storing requested object in
     * Amazon S3 (for example, AES256, <code>aws:kms</code>).</p>
     */
    inline const ServerSideEncryption& GetServerSideEncryption() const{ return m_serverSideEncryption; }
    inline bool ServerSideEncryptionHasBeenSet() const { return m_serverSideEncryptionHasBeenSet; }
    inline void SetServerSideEncryption(const ServerSideEncryption& value) { m_serverSideEncryptionHasBeenSet = true; m_serverSideEncryption = value; }
    inline void SetServerSideEncryption(ServerSideEncryption&& value) { m_serverSideEncryptionHasBeenSet = true; m_serverSideEncryption = std::move(value); }
    inline WriteGetObjectResponseRequest& WithServerSideEncryption(const ServerSideEncryption& value) { SetServerSideEncryption(value); return *this;}
    inline WriteGetObjectResponseRequest& WithServerSideEncryption(ServerSideEncryption&& value) { SetServerSideEncryption(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Encryption algorithm used if server-side encryption with a customer-provided
     * encryption key was specified for object stored in Amazon S3.</p>
     */
    inline const Aws::String& GetSSECustomerAlgorithm() const{ return m_sSECustomerAlgorithm; }
    inline bool SSECustomerAlgorithmHasBeenSet() const { return m_sSECustomerAlgorithmHasBeenSet; }
    inline void SetSSECustomerAlgorithm(const Aws::String& value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm = value; }
    inline void SetSSECustomerAlgorithm(Aws::String&& value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm = std::move(value); }
    inline void SetSSECustomerAlgorithm(const char* value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm.assign(value); }
    inline WriteGetObjectResponseRequest& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline WriteGetObjectResponseRequest& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> If present, specifies the ID (Key ID, Key ARN, or Key Alias) of the Amazon
     * Web Services Key Management Service (Amazon Web Services KMS) symmetric
     * encryption customer managed key that was used for stored in Amazon S3 object.
     * </p>
     */
    inline const Aws::String& GetSSEKMSKeyId() const{ return m_sSEKMSKeyId; }
    inline bool SSEKMSKeyIdHasBeenSet() const { return m_sSEKMSKeyIdHasBeenSet; }
    inline void SetSSEKMSKeyId(const Aws::String& value) { m_sSEKMSKeyIdHasBeenSet = true; m_sSEKMSKeyId = value; }
    inline void SetSSEKMSKeyId(Aws::String&& value) { m_sSEKMSKeyIdHasBeenSet = true; m_sSEKMSKeyId = std::move(value); }
    inline void SetSSEKMSKeyId(const char* value) { m_sSEKMSKeyIdHasBeenSet = true; m_sSEKMSKeyId.assign(value); }
    inline WriteGetObjectResponseRequest& WithSSEKMSKeyId(const Aws::String& value) { SetSSEKMSKeyId(value); return *this;}
    inline WriteGetObjectResponseRequest& WithSSEKMSKeyId(Aws::String&& value) { SetSSEKMSKeyId(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithSSEKMSKeyId(const char* value) { SetSSEKMSKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> 128-bit MD5 digest of customer-provided encryption key used in Amazon S3 to
     * encrypt data stored in S3. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/ServerSideEncryptionCustomerKeys.html">Protecting
     * data using server-side encryption with customer-provided encryption keys
     * (SSE-C)</a>.</p>
     */
    inline const Aws::String& GetSSECustomerKeyMD5() const{ return m_sSECustomerKeyMD5; }
    inline bool SSECustomerKeyMD5HasBeenSet() const { return m_sSECustomerKeyMD5HasBeenSet; }
    inline void SetSSECustomerKeyMD5(const Aws::String& value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5 = value; }
    inline void SetSSECustomerKeyMD5(Aws::String&& value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5 = std::move(value); }
    inline void SetSSECustomerKeyMD5(const char* value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5.assign(value); }
    inline WriteGetObjectResponseRequest& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline WriteGetObjectResponseRequest& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Provides storage class information of the object. Amazon S3 returns this
     * header for all objects except for S3 Standard storage class objects.</p> <p>For
     * more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/storage-class-intro.html">Storage
     * Classes</a>.</p>
     */
    inline const StorageClass& GetStorageClass() const{ return m_storageClass; }
    inline bool StorageClassHasBeenSet() const { return m_storageClassHasBeenSet; }
    inline void SetStorageClass(const StorageClass& value) { m_storageClassHasBeenSet = true; m_storageClass = value; }
    inline void SetStorageClass(StorageClass&& value) { m_storageClassHasBeenSet = true; m_storageClass = std::move(value); }
    inline WriteGetObjectResponseRequest& WithStorageClass(const StorageClass& value) { SetStorageClass(value); return *this;}
    inline WriteGetObjectResponseRequest& WithStorageClass(StorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of tags, if any, on the object.</p>
     */
    inline int GetTagCount() const{ return m_tagCount; }
    inline bool TagCountHasBeenSet() const { return m_tagCountHasBeenSet; }
    inline void SetTagCount(int value) { m_tagCountHasBeenSet = true; m_tagCount = value; }
    inline WriteGetObjectResponseRequest& WithTagCount(int value) { SetTagCount(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An ID used to reference a specific version of the object.</p>
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline bool VersionIdHasBeenSet() const { return m_versionIdHasBeenSet; }
    inline void SetVersionId(const Aws::String& value) { m_versionIdHasBeenSet = true; m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionIdHasBeenSet = true; m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionIdHasBeenSet = true; m_versionId.assign(value); }
    inline WriteGetObjectResponseRequest& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline WriteGetObjectResponseRequest& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> Indicates whether the object stored in Amazon S3 uses an S3 bucket key for
     * server-side encryption with Amazon Web Services KMS (SSE-KMS).</p>
     */
    inline bool GetBucketKeyEnabled() const{ return m_bucketKeyEnabled; }
    inline bool BucketKeyEnabledHasBeenSet() const { return m_bucketKeyEnabledHasBeenSet; }
    inline void SetBucketKeyEnabled(bool value) { m_bucketKeyEnabledHasBeenSet = true; m_bucketKeyEnabled = value; }
    inline WriteGetObjectResponseRequest& WithBucketKeyEnabled(bool value) { SetBucketKeyEnabled(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline WriteGetObjectResponseRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline WriteGetObjectResponseRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline WriteGetObjectResponseRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline WriteGetObjectResponseRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline WriteGetObjectResponseRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline WriteGetObjectResponseRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline WriteGetObjectResponseRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline WriteGetObjectResponseRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline WriteGetObjectResponseRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_requestRoute;
    bool m_requestRouteHasBeenSet = false;

    Aws::String m_requestToken;
    bool m_requestTokenHasBeenSet = false;


    int m_statusCode;
    bool m_statusCodeHasBeenSet = false;

    Aws::String m_errorCode;
    bool m_errorCodeHasBeenSet = false;

    Aws::String m_errorMessage;
    bool m_errorMessageHasBeenSet = false;

    Aws::String m_acceptRanges;
    bool m_acceptRangesHasBeenSet = false;

    Aws::String m_cacheControl;
    bool m_cacheControlHasBeenSet = false;

    Aws::String m_contentDisposition;
    bool m_contentDispositionHasBeenSet = false;

    Aws::String m_contentEncoding;
    bool m_contentEncodingHasBeenSet = false;

    Aws::String m_contentLanguage;
    bool m_contentLanguageHasBeenSet = false;

    long long m_contentLength;
    bool m_contentLengthHasBeenSet = false;

    Aws::String m_contentRange;
    bool m_contentRangeHasBeenSet = false;

    Aws::String m_checksumCRC32;
    bool m_checksumCRC32HasBeenSet = false;

    Aws::String m_checksumCRC32C;
    bool m_checksumCRC32CHasBeenSet = false;

    Aws::String m_checksumSHA1;
    bool m_checksumSHA1HasBeenSet = false;

    Aws::String m_checksumSHA256;
    bool m_checksumSHA256HasBeenSet = false;

    bool m_deleteMarker;
    bool m_deleteMarkerHasBeenSet = false;

    Aws::String m_eTag;
    bool m_eTagHasBeenSet = false;

    Aws::Utils::DateTime m_expires;
    bool m_expiresHasBeenSet = false;

    Aws::String m_expiration;
    bool m_expirationHasBeenSet = false;

    Aws::Utils::DateTime m_lastModified;
    bool m_lastModifiedHasBeenSet = false;

    int m_missingMeta;
    bool m_missingMetaHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_metadata;
    bool m_metadataHasBeenSet = false;

    ObjectLockMode m_objectLockMode;
    bool m_objectLockModeHasBeenSet = false;

    ObjectLockLegalHoldStatus m_objectLockLegalHoldStatus;
    bool m_objectLockLegalHoldStatusHasBeenSet = false;

    Aws::Utils::DateTime m_objectLockRetainUntilDate;
    bool m_objectLockRetainUntilDateHasBeenSet = false;

    int m_partsCount;
    bool m_partsCountHasBeenSet = false;

    ReplicationStatus m_replicationStatus;
    bool m_replicationStatusHasBeenSet = false;

    RequestCharged m_requestCharged;
    bool m_requestChargedHasBeenSet = false;

    Aws::String m_restore;
    bool m_restoreHasBeenSet = false;

    ServerSideEncryption m_serverSideEncryption;
    bool m_serverSideEncryptionHasBeenSet = false;

    Aws::String m_sSECustomerAlgorithm;
    bool m_sSECustomerAlgorithmHasBeenSet = false;

    Aws::String m_sSEKMSKeyId;
    bool m_sSEKMSKeyIdHasBeenSet = false;

    Aws::String m_sSECustomerKeyMD5;
    bool m_sSECustomerKeyMD5HasBeenSet = false;

    StorageClass m_storageClass;
    bool m_storageClassHasBeenSet = false;

    int m_tagCount;
    bool m_tagCountHasBeenSet = false;

    Aws::String m_versionId;
    bool m_versionIdHasBeenSet = false;

    bool m_bucketKeyEnabled;
    bool m_bucketKeyEnabledHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
