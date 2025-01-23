/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/CompletedMultipartUpload.h>
#include <aws/s3/model/RequestPayer.h>
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
  class CompleteMultipartUploadRequest : public S3Request
  {
  public:
    AWS_S3_API CompleteMultipartUploadRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CompleteMultipartUpload"; }

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
     * <p>Name of the bucket to which the multipart upload was initiated.</p> <p>
     * <b>Directory buckets</b> - When you use this operation with a directory bucket,
     * you must use virtual-hosted-style requests in the format <code>
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
    inline CompleteMultipartUploadRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline CompleteMultipartUploadRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Object key for which the multipart upload was initiated.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline CompleteMultipartUploadRequest& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline CompleteMultipartUploadRequest& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The container for the multipart upload request information.</p>
     */
    inline const CompletedMultipartUpload& GetMultipartUpload() const{ return m_multipartUpload; }
    inline bool MultipartUploadHasBeenSet() const { return m_multipartUploadHasBeenSet; }
    inline void SetMultipartUpload(const CompletedMultipartUpload& value) { m_multipartUploadHasBeenSet = true; m_multipartUpload = value; }
    inline void SetMultipartUpload(CompletedMultipartUpload&& value) { m_multipartUploadHasBeenSet = true; m_multipartUpload = std::move(value); }
    inline CompleteMultipartUploadRequest& WithMultipartUpload(const CompletedMultipartUpload& value) { SetMultipartUpload(value); return *this;}
    inline CompleteMultipartUploadRequest& WithMultipartUpload(CompletedMultipartUpload&& value) { SetMultipartUpload(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>ID for the initiated multipart upload.</p>
     */
    inline const Aws::String& GetUploadId() const{ return m_uploadId; }
    inline bool UploadIdHasBeenSet() const { return m_uploadIdHasBeenSet; }
    inline void SetUploadId(const Aws::String& value) { m_uploadIdHasBeenSet = true; m_uploadId = value; }
    inline void SetUploadId(Aws::String&& value) { m_uploadIdHasBeenSet = true; m_uploadId = std::move(value); }
    inline void SetUploadId(const char* value) { m_uploadIdHasBeenSet = true; m_uploadId.assign(value); }
    inline CompleteMultipartUploadRequest& WithUploadId(const Aws::String& value) { SetUploadId(value); return *this;}
    inline CompleteMultipartUploadRequest& WithUploadId(Aws::String&& value) { SetUploadId(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithUploadId(const char* value) { SetUploadId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This header specifies the
     * base64-encoded, 32-bit CRC-32 checksum of the object. For more information, see
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32() const{ return m_checksumCRC32; }
    inline bool ChecksumCRC32HasBeenSet() const { return m_checksumCRC32HasBeenSet; }
    inline void SetChecksumCRC32(const Aws::String& value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32 = value; }
    inline void SetChecksumCRC32(Aws::String&& value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32 = std::move(value); }
    inline void SetChecksumCRC32(const char* value) { m_checksumCRC32HasBeenSet = true; m_checksumCRC32.assign(value); }
    inline CompleteMultipartUploadRequest& WithChecksumCRC32(const Aws::String& value) { SetChecksumCRC32(value); return *this;}
    inline CompleteMultipartUploadRequest& WithChecksumCRC32(Aws::String&& value) { SetChecksumCRC32(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithChecksumCRC32(const char* value) { SetChecksumCRC32(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This header specifies the
     * base64-encoded, 32-bit CRC-32C checksum of the object. For more information, see
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumCRC32C() const{ return m_checksumCRC32C; }
    inline bool ChecksumCRC32CHasBeenSet() const { return m_checksumCRC32CHasBeenSet; }
    inline void SetChecksumCRC32C(const Aws::String& value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C = value; }
    inline void SetChecksumCRC32C(Aws::String&& value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C = std::move(value); }
    inline void SetChecksumCRC32C(const char* value) { m_checksumCRC32CHasBeenSet = true; m_checksumCRC32C.assign(value); }
    inline CompleteMultipartUploadRequest& WithChecksumCRC32C(const Aws::String& value) { SetChecksumCRC32C(value); return *this;}
    inline CompleteMultipartUploadRequest& WithChecksumCRC32C(Aws::String&& value) { SetChecksumCRC32C(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithChecksumCRC32C(const char* value) { SetChecksumCRC32C(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This header specifies the
     * base64-encoded, 160-bit SHA-1 digest of the object. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA1() const{ return m_checksumSHA1; }
    inline bool ChecksumSHA1HasBeenSet() const { return m_checksumSHA1HasBeenSet; }
    inline void SetChecksumSHA1(const Aws::String& value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1 = value; }
    inline void SetChecksumSHA1(Aws::String&& value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1 = std::move(value); }
    inline void SetChecksumSHA1(const char* value) { m_checksumSHA1HasBeenSet = true; m_checksumSHA1.assign(value); }
    inline CompleteMultipartUploadRequest& WithChecksumSHA1(const Aws::String& value) { SetChecksumSHA1(value); return *this;}
    inline CompleteMultipartUploadRequest& WithChecksumSHA1(Aws::String&& value) { SetChecksumSHA1(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithChecksumSHA1(const char* value) { SetChecksumSHA1(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>This header can be used as a data integrity check to verify that the data
     * received is the same data that was originally sent. This header specifies the
     * base64-encoded, 256-bit SHA-256 digest of the object. For more information, see
     * <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetChecksumSHA256() const{ return m_checksumSHA256; }
    inline bool ChecksumSHA256HasBeenSet() const { return m_checksumSHA256HasBeenSet; }
    inline void SetChecksumSHA256(const Aws::String& value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256 = value; }
    inline void SetChecksumSHA256(Aws::String&& value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256 = std::move(value); }
    inline void SetChecksumSHA256(const char* value) { m_checksumSHA256HasBeenSet = true; m_checksumSHA256.assign(value); }
    inline CompleteMultipartUploadRequest& WithChecksumSHA256(const Aws::String& value) { SetChecksumSHA256(value); return *this;}
    inline CompleteMultipartUploadRequest& WithChecksumSHA256(Aws::String&& value) { SetChecksumSHA256(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithChecksumSHA256(const char* value) { SetChecksumSHA256(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestPayer& GetRequestPayer() const{ return m_requestPayer; }
    inline bool RequestPayerHasBeenSet() const { return m_requestPayerHasBeenSet; }
    inline void SetRequestPayer(const RequestPayer& value) { m_requestPayerHasBeenSet = true; m_requestPayer = value; }
    inline void SetRequestPayer(RequestPayer&& value) { m_requestPayerHasBeenSet = true; m_requestPayer = std::move(value); }
    inline CompleteMultipartUploadRequest& WithRequestPayer(const RequestPayer& value) { SetRequestPayer(value); return *this;}
    inline CompleteMultipartUploadRequest& WithRequestPayer(RequestPayer&& value) { SetRequestPayer(std::move(value)); return *this;}
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
    inline CompleteMultipartUploadRequest& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline CompleteMultipartUploadRequest& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Uploads the object only if the ETag (entity tag) value provided during the
     * WRITE operation matches the ETag of the object in S3. If the ETag values do not
     * match, the operation returns a <code>412 Precondition Failed</code> error.</p>
     * <p>If a conflicting operation occurs during the upload S3 returns a <code>409
     * ConditionalRequestConflict</code> response. On a 409 failure you should fetch
     * the object's ETag, re-initiate the multipart upload with
     * <code>CreateMultipartUpload</code>, and re-upload each part.</p> <p>Expects the
     * ETag value as a string.</p> <p>For more information about conditional requests,
     * see <a href="https://tools.ietf.org/html/rfc7232">RFC 7232</a>, or <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-requests.html">Conditional
     * requests</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetIfMatch() const{ return m_ifMatch; }
    inline bool IfMatchHasBeenSet() const { return m_ifMatchHasBeenSet; }
    inline void SetIfMatch(const Aws::String& value) { m_ifMatchHasBeenSet = true; m_ifMatch = value; }
    inline void SetIfMatch(Aws::String&& value) { m_ifMatchHasBeenSet = true; m_ifMatch = std::move(value); }
    inline void SetIfMatch(const char* value) { m_ifMatchHasBeenSet = true; m_ifMatch.assign(value); }
    inline CompleteMultipartUploadRequest& WithIfMatch(const Aws::String& value) { SetIfMatch(value); return *this;}
    inline CompleteMultipartUploadRequest& WithIfMatch(Aws::String&& value) { SetIfMatch(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithIfMatch(const char* value) { SetIfMatch(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Uploads the object only if the object key name does not already exist in the
     * bucket specified. Otherwise, Amazon S3 returns a <code>412 Precondition
     * Failed</code> error.</p> <p>If a conflicting operation occurs during the upload
     * S3 returns a <code>409 ConditionalRequestConflict</code> response. On a 409
     * failure you should re-initiate the multipart upload with
     * <code>CreateMultipartUpload</code> and re-upload each part.</p> <p>Expects the
     * '*' (asterisk) character.</p> <p>For more information about conditional
     * requests, see <a href="https://tools.ietf.org/html/rfc7232">RFC 7232</a>, or <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-requests.html">Conditional
     * requests</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::String& GetIfNoneMatch() const{ return m_ifNoneMatch; }
    inline bool IfNoneMatchHasBeenSet() const { return m_ifNoneMatchHasBeenSet; }
    inline void SetIfNoneMatch(const Aws::String& value) { m_ifNoneMatchHasBeenSet = true; m_ifNoneMatch = value; }
    inline void SetIfNoneMatch(Aws::String&& value) { m_ifNoneMatchHasBeenSet = true; m_ifNoneMatch = std::move(value); }
    inline void SetIfNoneMatch(const char* value) { m_ifNoneMatchHasBeenSet = true; m_ifNoneMatch.assign(value); }
    inline CompleteMultipartUploadRequest& WithIfNoneMatch(const Aws::String& value) { SetIfNoneMatch(value); return *this;}
    inline CompleteMultipartUploadRequest& WithIfNoneMatch(Aws::String&& value) { SetIfNoneMatch(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithIfNoneMatch(const char* value) { SetIfNoneMatch(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The server-side encryption (SSE) algorithm used to encrypt the object. This
     * parameter is required only when the object was created using a checksum
     * algorithm or if your bucket policy requires the use of SSE-C. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/ServerSideEncryptionCustomerKeys.html#ssec-require-condition-key">Protecting
     * data using SSE-C keys</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerAlgorithm() const{ return m_sSECustomerAlgorithm; }
    inline bool SSECustomerAlgorithmHasBeenSet() const { return m_sSECustomerAlgorithmHasBeenSet; }
    inline void SetSSECustomerAlgorithm(const Aws::String& value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm = value; }
    inline void SetSSECustomerAlgorithm(Aws::String&& value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm = std::move(value); }
    inline void SetSSECustomerAlgorithm(const char* value) { m_sSECustomerAlgorithmHasBeenSet = true; m_sSECustomerAlgorithm.assign(value); }
    inline CompleteMultipartUploadRequest& WithSSECustomerAlgorithm(const Aws::String& value) { SetSSECustomerAlgorithm(value); return *this;}
    inline CompleteMultipartUploadRequest& WithSSECustomerAlgorithm(Aws::String&& value) { SetSSECustomerAlgorithm(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithSSECustomerAlgorithm(const char* value) { SetSSECustomerAlgorithm(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The server-side encryption (SSE) customer managed key. This parameter is
     * needed only when the object was created using a checksum algorithm. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/ServerSideEncryptionCustomerKeys.html">Protecting
     * data using SSE-C keys</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerKey() const{ return m_sSECustomerKey; }
    inline bool SSECustomerKeyHasBeenSet() const { return m_sSECustomerKeyHasBeenSet; }
    inline void SetSSECustomerKey(const Aws::String& value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey = value; }
    inline void SetSSECustomerKey(Aws::String&& value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey = std::move(value); }
    inline void SetSSECustomerKey(const char* value) { m_sSECustomerKeyHasBeenSet = true; m_sSECustomerKey.assign(value); }
    inline CompleteMultipartUploadRequest& WithSSECustomerKey(const Aws::String& value) { SetSSECustomerKey(value); return *this;}
    inline CompleteMultipartUploadRequest& WithSSECustomerKey(Aws::String&& value) { SetSSECustomerKey(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithSSECustomerKey(const char* value) { SetSSECustomerKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The MD5 server-side encryption (SSE) customer managed key. This parameter is
     * needed only when the object was created using a checksum algorithm. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/ServerSideEncryptionCustomerKeys.html">Protecting
     * data using SSE-C keys</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetSSECustomerKeyMD5() const{ return m_sSECustomerKeyMD5; }
    inline bool SSECustomerKeyMD5HasBeenSet() const { return m_sSECustomerKeyMD5HasBeenSet; }
    inline void SetSSECustomerKeyMD5(const Aws::String& value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5 = value; }
    inline void SetSSECustomerKeyMD5(Aws::String&& value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5 = std::move(value); }
    inline void SetSSECustomerKeyMD5(const char* value) { m_sSECustomerKeyMD5HasBeenSet = true; m_sSECustomerKeyMD5.assign(value); }
    inline CompleteMultipartUploadRequest& WithSSECustomerKeyMD5(const Aws::String& value) { SetSSECustomerKeyMD5(value); return *this;}
    inline CompleteMultipartUploadRequest& WithSSECustomerKeyMD5(Aws::String&& value) { SetSSECustomerKeyMD5(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& WithSSECustomerKeyMD5(const char* value) { SetSSECustomerKeyMD5(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline CompleteMultipartUploadRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline CompleteMultipartUploadRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline CompleteMultipartUploadRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline CompleteMultipartUploadRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline CompleteMultipartUploadRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline CompleteMultipartUploadRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline CompleteMultipartUploadRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline CompleteMultipartUploadRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline CompleteMultipartUploadRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    CompletedMultipartUpload m_multipartUpload;
    bool m_multipartUploadHasBeenSet = false;

    Aws::String m_uploadId;
    bool m_uploadIdHasBeenSet = false;

    Aws::String m_checksumCRC32;
    bool m_checksumCRC32HasBeenSet = false;

    Aws::String m_checksumCRC32C;
    bool m_checksumCRC32CHasBeenSet = false;

    Aws::String m_checksumSHA1;
    bool m_checksumSHA1HasBeenSet = false;

    Aws::String m_checksumSHA256;
    bool m_checksumSHA256HasBeenSet = false;

    RequestPayer m_requestPayer;
    bool m_requestPayerHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    Aws::String m_ifMatch;
    bool m_ifMatchHasBeenSet = false;

    Aws::String m_ifNoneMatch;
    bool m_ifNoneMatchHasBeenSet = false;

    Aws::String m_sSECustomerAlgorithm;
    bool m_sSECustomerAlgorithmHasBeenSet = false;

    Aws::String m_sSECustomerKey;
    bool m_sSECustomerKeyHasBeenSet = false;

    Aws::String m_sSECustomerKeyMD5;
    bool m_sSECustomerKeyMD5HasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
