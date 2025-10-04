/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <aws/s3/model/RequestPayer.h>
#include <aws/s3/model/ChecksumMode.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
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
  class HeadObjectRequest : public S3Request
  {
  public:
    AWS_S3_API HeadObjectRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "HeadObject"; }

    AWS_S3_API Aws::String SerializePayload() const override;

    AWS_S3_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;

    AWS_S3_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    AWS_S3_API bool HasEmbeddedError(IOStream &body, const Http::HeaderValueCollection &header) const override;
    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_S3_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the bucket that contains the object.</p> <p> <b>Directory
     * buckets</b> - When you use this operation with a directory bucket, you must use
     * virtual-hosted-style requests in the format <code>
     * <i>Bucket-name</i>.s3express-<i>zone-id</i>.<i>region-code</i>.amazonaws.com</code>.
     * Path-style requests are not supported. Directory bucket names must be unique in
     * the chosen Zone (Availability Zone or Local Zone). Bucket names must follow the
     * format <code> <i>bucket-base-name</i>--<i>zone-id</i>--x-s3</code> (for example,
     * <code> <i>DOC-EXAMPLE-BUCKET</i>--<i>usw2-az1</i>--x-s3</code>). For information
     * about bucket naming restrictions, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/directory-bucket-naming-rules.html">Directory
     * bucket naming rules</a> in the <i>Amazon S3 User Guide</i>.</p> <p> <b>Access
     * points</b> - When you use this action with an access point, you must provide the
     * alias of the access point in place of the bucket name or specify the access
     * point ARN. When using the access point ARN, you must direct requests to the
     * access point hostname. The access point hostname takes the form
     * <i>AccessPointName</i>-<i>AccountId</i>.s3-accesspoint.<i>Region</i>.amazonaws.com.
     * When using this action with an access point through the Amazon Web Services
     * SDKs, you provide the access point ARN in place of the bucket name. For more
     * information about access point ARNs, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-access-points.html">Using
     * access points</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>Access
     * points and Object Lambda access points are not supported by directory
     * buckets.</p>  <p> <b>S3 on Outposts</b> - When you use this action with
     * Amazon S3 on Outposts, you must direct requests to the S3 on Outposts hostname.
     * The S3 on Outposts hostname takes the form <code>
     * <i>AccessPointName</i>-<i>AccountId</i>.<i>outpostID</i>.s3-outposts.<i>Region</i>.amazonaws.com</code>.
     * When you use this action with S3 on Outposts through the Amazon Web Services
     * SDKs, you provide the Outposts access point ARN in place of the bucket name. For
     * more information about S3 on Outposts ARNs, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/S3onOutposts.html">What
     * is S3 on Outposts?</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline bool BucketHasBeenSet() const { return m_bucketHasBeenSet; }
    inline void SetBucket(const Aws::String& value) { m_bucketHasBeenSet = true; m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucketHasBeenSet = true; m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucketHasBeenSet = true; m_bucket.assign(value); }
    inline HeadObjectRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline HeadObjectRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline HeadObjectRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Return the object only if its entity tag (ETag) is the same as the one
     * specified; otherwise, return a 412 (precondition failed) error.</p> <p>If both
     * of the <code>If-Match</code> and <code>If-Unmodified-Since</code> headers are
     * present in the request as follows:</p> <ul> <li> <p> <code>If-Match</code>
     * condition evaluates to <code>true</code>, and;</p> </li> <li> <p>
     * <code>If-Unmodified-Since</code> condition evaluates to <code>false</code>;</p>
     * </li> </ul> <p>Then Amazon S3 returns <code>200 OK</code> and the data
     * requested.</p> <p>For more information about conditional requests, see <a
     * href="https://tools.ietf.org/html/rfc7232">RFC 7232</a>.</p>
     */
    inline const Aws::String& GetIfMatch() const{ return m_ifMatch; }
    inline bool IfMatchHasBeenSet() const { return m_ifMatchHasBeenSet; }
    inline void SetIfMatch(const Aws::String& value) { m_ifMatchHasBeenSet = true; m_ifMatch = value; }
    inline void SetIfMatch(Aws::String&& value) { m_ifMatchHasBeenSet = true; m_ifMatch = std::move(value); }
    inline void SetIfMatch(const char* value) { m_ifMatchHasBeenSet = true; m_ifMatch.assign(value); }
    inline HeadObjectRequest& WithIfMatch(const Aws::String& value) { SetIfMatch(value); return *this;}
    inline HeadObjectRequest& WithIfMatch(Aws::String&& value) { SetIfMatch(std::move(value)); return *this;}
    inline HeadObjectRequest& WithIfMatch(const char* value) { SetIfMatch(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Return the object only if it has been modified since the specified time;
     * otherwise, return a 304 (not modified) error.</p> <p>If both of the
     * <code>If-None-Match</code> and <code>If-Modified-Since</code> headers are
     * present in the request as follows:</p> <ul> <li> <p> <code>If-None-Match</code>
     * condition evaluates to <code>false</code>, and;</p> </li> <li> <p>
     * <code>If-Modified-Since</code> condition evaluates to <code>true</code>;</p>
     * </li> </ul> <p>Then Amazon S3 returns the <code>304 Not Modified</code> response
     * code.</p> <p>For more information about conditional requests, see <a
     * href="https://tools.ietf.org/html/rfc7232">RFC 7232</a>.</p>
     */
    inline const Aws::Utils::DateTime& GetIfModifiedSince() const{ return m_ifModifiedSince; }
    inline bool IfModifiedSinceHasBeenSet() const { return m_ifModifiedSinceHasBeenSet; }
    inline void SetIfModifiedSince(const Aws::Utils::DateTime& value) { m_ifModifiedSinceHasBeenSet = true; m_ifModifiedSince = value; }
    inline void SetIfModifiedSince(Aws::Utils::DateTime&& value) { m_ifModifiedSinceHasBeenSet = true; m_ifModifiedSince = std::move(value); }
    inline HeadObjectRequest& WithIfModifiedSince(const Aws::Utils::DateTime& value) { SetIfModifiedSince(value); return *this;}
    inline HeadObjectRequest& WithIfModifiedSince(Aws::Utils::DateTime&& value) { SetIfModifiedSince(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Return the object only if its entity tag (ETag) is different from the one
     * specified; otherwise, return a 304 (not modified) error.</p> <p>If both of the
     * <code>If-None-Match</code> and <code>If-Modified-Since</code> headers are
     * present in the request as follows:</p> <ul> <li> <p> <code>If-None-Match</code>
     * condition evaluates to <code>false</code>, and;</p> </li> <li> <p>
     * <code>If-Modified-Since</code> condition evaluates to <code>true</code>;</p>
     * </li> </ul> <p>Then Amazon S3 returns the <code>304 Not Modified</code> response
     * code.</p> <p>For more information about conditional requests, see <a
     * href="https://tools.ietf.org/html/rfc7232">RFC 7232</a>.</p>
     */
    inline const Aws::String& GetIfNoneMatch() const{ return m_ifNoneMatch; }
    inline bool IfNoneMatchHasBeenSet() const { return m_ifNoneMatchHasBeenSet; }
    inline void SetIfNoneMatch(const Aws::String& value) { m_ifNoneMatchHasBeenSet = true; m_ifNoneMatch = value; }
    inline void SetIfNoneMatch(Aws::String&& value) { m_ifNoneMatchHasBeenSet = true; m_ifNoneMatch = std::move(value); }
    inline void SetIfNoneMatch(const char* value) { m_ifNoneMatchHasBeenSet = true; m_ifNoneMatch.assign(value); }
    inline HeadObjectRequest& WithIfNoneMatch(const Aws::String& value) { SetIfNoneMatch(value); return *this;}
    inline HeadObjectRequest& WithIfNoneMatch(Aws::String&& value) { SetIfNoneMatch(std::move(value)); return *this;}
    inline HeadObjectRequest& WithIfNoneMatch(const char* value) { SetIfNoneMatch(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Return the object only if it has not been modified since the specified time;
     * otherwise, return a 412 (precondition failed) error.</p> <p>If both of the
     * <code>If-Match</code> and <code>If-Unmodified-Since</code> headers are present
     * in the request as follows:</p> <ul> <li> <p> <code>If-Match</code> condition
     * evaluates to <code>true</code>, and;</p> </li> <li> <p>
     * <code>If-Unmodified-Since</code> condition evaluates to <code>false</code>;</p>
     * </li> </ul> <p>Then Amazon S3 returns <code>200 OK</code> and the data
     * requested.</p> <p>For more information about conditional requests, see <a
     * href="https://tools.ietf.org/html/rfc7232">RFC 7232</a>.</p>
     */
    inline const Aws::Utils::DateTime& GetIfUnmodifiedSince() const{ return m_ifUnmodifiedSince; }
    inline bool IfUnmodifiedSinceHasBeenSet() const { return m_ifUnmodifiedSinceHasBeenSet; }
    inline void SetIfUnmodifiedSince(const Aws::Utils::DateTime& value) { m_ifUnmodifiedSinceHasBeenSet = true; m_ifUnmodifiedSince = value; }
    inline void SetIfUnmodifiedSince(Aws::Utils::DateTime&& value) { m_ifUnmodifiedSinceHasBeenSet = true; m_ifUnmodifiedSince = std::move(value); }
    inline HeadObjectRequest& WithIfUnmodifiedSince(const Aws::Utils::DateTime& value) { SetIfUnmodifiedSince(value); return *this;}
    inline HeadObjectRequest& WithIfUnmodifiedSince(Aws::Utils::DateTime&& value) { SetIfUnmodifiedSince(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The object key.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline HeadObjectRequest& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline HeadObjectRequest& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline HeadObjectRequest& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>HeadObject returns only the metadata for an object. If the Range is
     * satisfiable, only the <code>ContentLength</code> is affected in the response. If
     * the Range is not satisfiable, S3 returns a <code>416 - Requested Range Not
     * Satisfiable</code> error.</p>
     */
    inline const Aws::String& GetRange() const{ return m_range; }
    inline bool RangeHasBeenSet() const { return m_rangeHasBeenSet; }
    inline void SetRange(const Aws::String& value) { m_rangeHasBeenSet = true; m_range = value; }
    inline void SetRange(Aws::String&& value) { m_rangeHasBeenSet = true; m_range = std::move(value); }
    inline void SetRange(const char* value) { m_rangeHasBeenSet = true; m_range.assign(value); }
    inline HeadObjectRequest& WithRange(const Aws::String& value) { SetRange(value); return *this;}
    inline HeadObjectRequest& WithRange(Aws::String&& value) { SetRange(std::move(value)); return *this;}
    inline HeadObjectRequest& WithRange(const char* value) { SetRange(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the <code>Cache-Control</code> header of the response.</p>
     */
    inline const Aws::String& GetResponseCacheControl() const{ return m_responseCacheControl; }
    inline bool ResponseCacheControlHasBeenSet() const { return m_responseCacheControlHasBeenSet; }
    inline void SetResponseCacheControl(const Aws::String& value) { m_responseCacheControlHasBeenSet = true; m_responseCacheControl = value; }
    inline void SetResponseCacheControl(Aws::String&& value) { m_responseCacheControlHasBeenSet = true; m_responseCacheControl = std::move(value); }
    inline void SetResponseCacheControl(const char* value) { m_responseCacheControlHasBeenSet = true; m_responseCacheControl.assign(value); }
    inline HeadObjectRequest& WithResponseCacheControl(const Aws::String& value) { SetResponseCacheControl(value); return *this;}
    inline HeadObjectRequest& WithResponseCacheControl(Aws::String&& value) { SetResponseCacheControl(std::move(value)); return *this;}
    inline HeadObjectRequest& WithResponseCacheControl(const char* value) { SetResponseCacheControl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the <code>Content-Disposition</code> header of the response.</p>
     */
    inline const Aws::String& GetResponseContentDisposition() const{ return m_responseContentDisposition; }
    inline bool ResponseContentDispositionHasBeenSet() const { return m_responseContentDispositionHasBeenSet; }
    inline void SetResponseContentDisposition(const Aws::String& value) { m_responseContentDispositionHasBeenSet = true; m_responseContentDisposition = value; }
    inline void SetResponseContentDisposition(Aws::String&& value) { m_responseContentDispositionHasBeenSet = true; m_responseContentDisposition = std::move(value); }
    inline void SetResponseContentDisposition(const char* value) { m_responseContentDispositionHasBeenSet = true; m_responseContentDisposition.assign(value); }
    inline HeadObjectRequest& WithResponseContentDisposition(const Aws::String& value) { SetResponseContentDisposition(value); return *this;}
    inline HeadObjectRequest& WithResponseContentDisposition(Aws::String&& value) { SetResponseContentDisposition(std::move(value)); return *this;}
    inline HeadObjectRequest& WithResponseContentDisposition(const char* value) { SetResponseContentDisposition(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the <code>Content-Encoding</code> header of the response.</p>
     */
    inline const Aws::String& GetResponseContentEncoding() const{ return m_responseContentEncoding; }
    inline bool ResponseContentEncodingHasBeenSet() const { return m_responseContentEncodingHasBeenSet; }
    inline void SetResponseContentEncoding(const Aws::String& value) { m_responseContentEncodingHasBeenSet = true; m_responseContentEncoding = value; }
    inline void SetResponseContentEncoding(Aws::String&& value) { m_responseContentEncodingHasBeenSet = true; m_responseContentEncoding = std::move(value); }
    inline void SetResponseContentEncoding(const char* value) { m_responseContentEncodingHasBeenSet = true; m_responseContentEncoding.assign(value); }
    inline HeadObjectRequest& WithResponseContentEncoding(const Aws::String& value) { SetResponseContentEncoding(value); return *this;}
    inline HeadObjectRequest& WithResponseContentEncoding(Aws::String&& value) { SetResponseContentEncoding(std::move(value)); return *this;}
    inline HeadObjectRequest& WithResponseContentEncoding(const char* value) { SetResponseContentEncoding(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the <code>Content-Language</code> header of the response.</p>
     */
    inline const Aws::String& GetResponseContentLanguage() const{ return m_responseContentLanguage; }
    inline bool ResponseContentLanguageHasBeenSet() const { return m_responseContentLanguageHasBeenSet; }
    inline void SetResponseContentLanguage(const Aws::String& value) { m_responseContentLanguageHasBeenSet = true; m_responseContentLanguage = value; }
    inline void SetResponseContentLanguage(Aws::String&& value) { m_responseContentLanguageHasBeenSet = true; m_responseContentLanguage = std::move(value); }
    inline void SetResponseContentLanguage(const char* value) { m_responseContentLanguageHasBeenSet = true; m_responseContentLanguage.assign(value); }
    inline HeadObjectRequest& WithResponseContentLanguage(const Aws::String& value) { SetResponseContentLanguage(value); return *this;}
    inline HeadObjectRequest& WithResponseContentLanguage(Aws::String&& value) { SetResponseContentLanguage(std::move(value)); return *this;}
    inline HeadObjectRequest& WithResponseContentLanguage(const char* value) { SetResponseContentLanguage(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the <code>Content-Type</code> header of the response.</p>
     */
    inline const Aws::String& GetResponseContentType() const{ return m_responseContentType; }
    inline bool ResponseContentTypeHasBeenSet() const { return m_responseContentTypeHasBeenSet; }
    inline void SetResponseContentType(const Aws::String& value) { m_responseContentTypeHasBeenSet = true; m_responseContentType = value; }
    inline void SetResponseContentType(Aws::String&& value) { m_responseContentTypeHasBeenSet = true; m_responseContentType = std::move(value); }
    inline void SetResponseContentType(const char* value) { m_responseContentTypeHasBeenSet = true; m_responseContentType.assign(value); }
    inline HeadObjectRequest& WithResponseContentType(const Aws::String& value) { SetResponseContentType(value); return *this;}
    inline HeadObjectRequest& WithResponseContentType(Aws::String&& value) { SetResponseContentType(std::move(value)); return *this;}
    inline HeadObjectRequest& WithResponseContentType(const char* value) { SetResponseContentType(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Sets the <code>Expires</code> header of the response.</p>
     */
    inline const Aws::Utils::DateTime& GetResponseExpires() const{ return m_responseExpires; }
    inline bool ResponseExpiresHasBeenSet() const { return m_responseExpiresHasBeenSet; }
    inline void SetResponseExpires(const Aws::Utils::DateTime& value) { m_responseExpiresHasBeenSet = true; m_responseExpires = value; }
    inline void SetResponseExpires(Aws::Utils::DateTime&& value) { m_responseExpiresHasBeenSet = true; m_responseExpires = std::move(value); }
    inline HeadObjectRequest& WithResponseExpires(const Aws::Utils::DateTime& value) { SetResponseExpires(value); return *this;}
    inline HeadObjectRequest& WithResponseExpires(Aws::Utils::DateTime&& value) { SetResponseExpires(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Version ID used to reference a specific version of the object.</p> 
     * <p>For directory buckets in this API operation, only the <code>null</code> value
     * of the version ID is supported.</p> 
     */
    inline const Aws::String& GetVersionId() const{ return m_versionId; }
    inline bool VersionIdHasBeenSet() const { return m_versionIdHasBeenSet; }
    inline void SetVersionId(const Aws::String& value) { m_versionIdHasBeenSet = true; m_versionId = value; }
    inline void SetVersionId(Aws::String&& value) { m_versionIdHasBeenSet = true; m_versionId = std::move(value); }
    inline void SetVersionId(const char* value) { m_versionIdHasBeenSet = true; m_versionId.assign(value); }
    inline HeadObjectRequest& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline HeadObjectRequest& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline HeadObjectRequest& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the algorithm to use when encrypting the object (for example,
     * AES256).</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerAlgorithm() const{ return m_sSECustomerAlgorithm; }
    inline bool SSECustomerAlgorithmHasBeenSet() const { return m_sSECustomerAlgorithmHasBeenSet; }
    inline void SetSSECustomerAlgorithm(const Aws::String& value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm = value; }
    inline void SetSSECustomerAlgorithm(Aws::String&& value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm = std::move(value); }
    inline void SetSSECustomerAlgorithm(const char* value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm.assign(value); }
    inline HeadObjectRequest& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline HeadObjectRequest& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline HeadObjectRequest& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the customer-provided encryption key for Amazon S3 to use in
     * encrypting data. This value is used to store the object and then it is
     * discarded; Amazon S3 does not store the encryption key. The key must be
     * appropriate for use with the algorithm specified in the
     * <code>x-amz-server-side-encryption-customer-algorithm</code> header.</p> 
     * <p>This functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerKey() const{ return m_sSECustomerKey; }
    inline bool SSECustomerKeyHasBeenSet() const { return m_sSECustomerKeyHasBeenSet; }
    inline void SetSSECustomerKey(const Aws::String& value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey = value; }
    inline void SetSSECustomerKey(Aws::String&& value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey = std::move(value); }
    inline void SetSSECustomerKey(const char* value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey.assign(value); }
    inline HeadObjectRequest& WithSSECustomerKey(const Aws::String& value) { SetSSECustomerKey(value); return *this;}
    inline HeadObjectRequest& WithSSECustomerKey(Aws::String&& value) { SetSSECustomerKey(std::move(value)); return *this;}
    inline HeadObjectRequest& WithSSECustomerKey(const char* value) { SetSSECustomerKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the 128-bit MD5 digest of the encryption key according to RFC 1321.
     * Amazon S3 uses this header for a message integrity check to ensure that the
     * encryption key was transmitted without error.</p>  <p>This functionality
     * is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerKeyMD5() const{ return m_sSECustomerKeyMD5; }
    inline bool SSECustomerKeyMD5HasBeenSet() const { return m_sSECustomerKeyMD5HasBeenSet; }
    inline void SetSSECustomerKeyMD5(const Aws::String& value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5 = value; }
    inline void SetSSECustomerKeyMD5(Aws::String&& value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5 = std::move(value); }
    inline void SetSSECustomerKeyMD5(const char* value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5.assign(value); }
    inline HeadObjectRequest& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline HeadObjectRequest& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline HeadObjectRequest& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestPayer& GetRequestPayer() const{ return m_requestPayer; }
    inline bool RequestPayerHasBeenSet() const { return m_requestPayerHasBeenSet; }
    inline void SetRequestPayer(const RequestPayer& value) { m_requestPayerHasBeenSet = true; m_requestPayer = value; }
    inline void SetRequestPayer(RequestPayer&& value) { m_requestPayerHasBeenSet = true; m_requestPayer = std::move(value); }
    inline HeadObjectRequest& WithRequestPayer(const RequestPayer& value) { SetRequestPayer(value); return *this;}
    inline HeadObjectRequest& WithRequestPayer(RequestPayer&& value) { SetRequestPayer(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Part number of the object being read. This is a positive integer between 1
     * and 10,000. Effectively performs a 'ranged' HEAD request for the part specified.
     * Useful querying about the size of the part and the number of parts in this
     * object.</p>
     */
    inline int GetPartNumber() const{ return m_partNumber; }
    inline bool PartNumberHasBeenSet() const { return m_partNumberHasBeenSet; }
    inline void SetPartNumber(int value) { m_partNumberHasBeenSet = true; m_partNumber = value; }
    inline HeadObjectRequest& WithPartNumber(int value) { SetPartNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The account ID of the expected bucket owner. If the account ID that you
     * provide does not match the actual owner of the bucket, the request fails with
     * the HTTP status code <code>403 Forbidden</code> (access denied).</p>
     */
    inline const Aws::String& GetExpectedBucketOwner() const{ return m_expectedBucketOwner; }
    inline bool ExpectedBucketOwnerHasBeenSet() const { return m_expectedBucketOwnerHasBeenSet; }
    inline void SetExpectedBucketOwner(const Aws::String& value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner = value; }
    inline void SetExpectedBucketOwner(Aws::String&& value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner = std::move(value); }
    inline void SetExpectedBucketOwner(const char* value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner.assign(value); }
    inline HeadObjectRequest& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline HeadObjectRequest& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline HeadObjectRequest& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>To retrieve the checksum, this parameter must be enabled.</p> <p> <b>General
     * purpose buckets</b> - If you enable checksum mode and the object is uploaded
     * with a <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_Checksum.html">checksum</a>
     * and encrypted with an Key Management Service (KMS) key, you must have permission
     * to use the <code>kms:Decrypt</code> action to retrieve the checksum.</p> <p>
     * <b>Directory buckets</b> - If you enable <code>ChecksumMode</code> and the
     * object is encrypted with Amazon Web Services Key Management Service (Amazon Web
     * Services KMS), you must also have the <code>kms:GenerateDataKey</code> and
     * <code>kms:Decrypt</code> permissions in IAM identity-based policies and KMS key
     * policies for the KMS key to retrieve the checksum of the object.</p>
     */
    inline const ChecksumMode& GetChecksumMode() const{ return m_checksumMode; }
    inline bool ChecksumModeHasBeenSet() const { return m_checksumModeHasBeenSet; }
    inline void SetChecksumMode(const ChecksumMode& value) { m_checksumModeHasBeenSet = true; m_checksumMode = value; }
    inline void SetChecksumMode(ChecksumMode&& value) { m_checksumModeHasBeenSet = true; m_checksumMode = std::move(value); }
    inline HeadObjectRequest& WithChecksumMode(const ChecksumMode& value) { SetChecksumMode(value); return *this;}
    inline HeadObjectRequest& WithChecksumMode(ChecksumMode&& value) { SetChecksumMode(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline HeadObjectRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline HeadObjectRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline HeadObjectRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline HeadObjectRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline HeadObjectRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline HeadObjectRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline HeadObjectRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline HeadObjectRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline HeadObjectRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_ifMatch;
    bool m_ifMatchHasBeenSet = false;

    Aws::Utils::DateTime m_ifModifiedSince;
    bool m_ifModifiedSinceHasBeenSet = false;

    Aws::String m_ifNoneMatch;
    bool m_ifNoneMatchHasBeenSet = false;

    Aws::Utils::DateTime m_ifUnmodifiedSince;
    bool m_ifUnmodifiedSinceHasBeenSet = false;

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    Aws::String m_range;
    bool m_rangeHasBeenSet = false;

    Aws::String m_responseCacheControl;
    bool m_responseCacheControlHasBeenSet = false;

    Aws::String m_responseContentDisposition;
    bool m_responseContentDispositionHasBeenSet = false;

    Aws::String m_responseContentEncoding;
    bool m_responseContentEncodingHasBeenSet = false;

    Aws::String m_responseContentLanguage;
    bool m_responseContentLanguageHasBeenSet = false;

    Aws::String m_responseContentType;
    bool m_responseContentTypeHasBeenSet = false;

    Aws::Utils::DateTime m_responseExpires;
    bool m_responseExpiresHasBeenSet = false;

    Aws::String m_versionId;
    bool m_versionIdHasBeenSet = false;

    Aws::String m_sSECustomerAlgorithm;
    bool m_sSECustomerAlgorithmHasBeenSet = false;

    Aws::String m_sSECustomerKey;
    bool m_sSECustomerKeyHasBeenSet = false;

    Aws::String m_sSECustomerKeyMD5;
    bool m_sSECustomerKeyMD5HasBeenSet = false;

    RequestPayer m_requestPayer;
    bool m_requestPayerHasBeenSet = false;

    int m_partNumber;
    bool m_partNumberHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    ChecksumMode m_checksumMode;
    bool m_checksumModeHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
