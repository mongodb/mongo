/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/Delete.h>
#include <aws/s3/model/RequestPayer.h>
#include <aws/s3/model/ChecksumAlgorithm.h>
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
  class DeleteObjectsRequest : public S3Request
  {
  public:
    AWS_S3_API DeleteObjectsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DeleteObjects"; }

    AWS_S3_API Aws::String SerializePayload() const override;

    AWS_S3_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;

    AWS_S3_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    AWS_S3_API bool HasEmbeddedError(IOStream &body, const Http::HeaderValueCollection &header) const override;
    AWS_S3_API Aws::String GetChecksumAlgorithmName() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_S3_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The bucket name containing the objects to delete. </p> <p> <b>Directory
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
    inline DeleteObjectsRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline DeleteObjectsRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline DeleteObjectsRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for the request.</p>
     */
    inline const Delete& GetDelete() const{ return m_delete; }
    inline bool DeleteHasBeenSet() const { return m_deleteHasBeenSet; }
    inline void SetDelete(const Delete& value) { m_deleteHasBeenSet = true; m_delete = value; }
    inline void SetDelete(Delete&& value) { m_deleteHasBeenSet = true; m_delete = std::move(value); }
    inline DeleteObjectsRequest& WithDelete(const Delete& value) { SetDelete(value); return *this;}
    inline DeleteObjectsRequest& WithDelete(Delete&& value) { SetDelete(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The concatenation of the authentication device's serial number, a space, and
     * the value that is displayed on your authentication device. Required to
     * permanently delete a versioned object if versioning is configured with MFA
     * delete enabled.</p> <p>When performing the <code>DeleteObjects</code> operation
     * on an MFA delete enabled bucket, which attempts to delete the specified
     * versioned objects, you must include an MFA token. If you don't provide an MFA
     * token, the entire request will fail, even if there are non-versioned objects
     * that you are trying to delete. If you provide an invalid token, whether there
     * are versioned object keys in the request or not, the entire Multi-Object Delete
     * request will fail. For information about MFA Delete, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/Versioning.html#MultiFactorAuthenticationDelete">
     * MFA Delete</a> in the <i>Amazon S3 User Guide</i>.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline const Aws::String& GetMFA() const{ return m_mFA; }
    inline bool MFAHasBeenSet() const { return m_mFAHasBeenSet; }
    inline void SetMFA(const Aws::String& value) { m_mFAHasBeenSet = true; m_mFA = value; }
    inline void SetMFA(Aws::String&& value) { m_mFAHasBeenSet = true; m_mFA = std::move(value); }
    inline void SetMFA(const char* value) { m_mFAHasBeenSet = true; m_mFA.assign(value); }
    inline DeleteObjectsRequest& WithMFA(const Aws::String& value) { SetMFA(value); return *this;}
    inline DeleteObjectsRequest& WithMFA(Aws::String&& value) { SetMFA(std::move(value)); return *this;}
    inline DeleteObjectsRequest& WithMFA(const char* value) { SetMFA(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestPayer& GetRequestPayer() const{ return m_requestPayer; }
    inline bool RequestPayerHasBeenSet() const { return m_requestPayerHasBeenSet; }
    inline void SetRequestPayer(const RequestPayer& value) { m_requestPayerHasBeenSet = true; m_requestPayer = value; }
    inline void SetRequestPayer(RequestPayer&& value) { m_requestPayerHasBeenSet = true; m_requestPayer = std::move(value); }
    inline DeleteObjectsRequest& WithRequestPayer(const RequestPayer& value) { SetRequestPayer(value); return *this;}
    inline DeleteObjectsRequest& WithRequestPayer(RequestPayer&& value) { SetRequestPayer(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether you want to delete this object even if it has a
     * Governance-type Object Lock in place. To use this header, you must have the
     * <code>s3:BypassGovernanceRetention</code> permission.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline bool GetBypassGovernanceRetention() const{ return m_bypassGovernanceRetention; }
    inline bool BypassGovernanceRetentionHasBeenSet() const { return m_bypassGovernanceRetentionHasBeenSet; }
    inline void SetBypassGovernanceRetention(bool value) { m_bypassGovernanceRetentionHasBeenSet = true; m_bypassGovernanceRetention = value; }
    inline DeleteObjectsRequest& WithBypassGovernanceRetention(bool value) { SetBypassGovernanceRetention(value); return *this;}
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
    inline DeleteObjectsRequest& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline DeleteObjectsRequest& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline DeleteObjectsRequest& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates the algorithm used to create the checksum for the object when you
     * use the SDK. This header will not provide any additional functionality if you
     * don't use the SDK. When you send this header, there must be a corresponding
     * <code>x-amz-checksum-<i>algorithm</i> </code> or <code>x-amz-trailer</code>
     * header sent. Otherwise, Amazon S3 fails the request with the HTTP status code
     * <code>400 Bad Request</code>.</p> <p>For the
     * <code>x-amz-checksum-<i>algorithm</i> </code> header, replace <code>
     * <i>algorithm</i> </code> with the supported algorithm from the following list:
     * </p> <ul> <li> <p> <code>CRC32</code> </p> </li> <li> <p> <code>CRC32C</code>
     * </p> </li> <li> <p> <code>SHA1</code> </p> </li> <li> <p> <code>SHA256</code>
     * </p> </li> </ul> <p>For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p> <p>If the
     * individual checksum value you provide through
     * <code>x-amz-checksum-<i>algorithm</i> </code> doesn't match the checksum
     * algorithm you set through <code>x-amz-sdk-checksum-algorithm</code>, Amazon S3
     * ignores any provided <code>ChecksumAlgorithm</code> parameter and uses the
     * checksum algorithm that matches the provided value in
     * <code>x-amz-checksum-<i>algorithm</i> </code>.</p> <p>If you provide an
     * individual checksum, Amazon S3 ignores any provided
     * <code>ChecksumAlgorithm</code> parameter.</p>
     */
    inline const ChecksumAlgorithm& GetChecksumAlgorithm() const{ return m_checksumAlgorithm; }
    inline bool ChecksumAlgorithmHasBeenSet() const { return m_checksumAlgorithmHasBeenSet; }
    inline void SetChecksumAlgorithm(const ChecksumAlgorithm& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = value; }
    inline void SetChecksumAlgorithm(ChecksumAlgorithm&& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = std::move(value); }
    inline DeleteObjectsRequest& WithChecksumAlgorithm(const ChecksumAlgorithm& value) { SetChecksumAlgorithm(value); return *this;}
    inline DeleteObjectsRequest& WithChecksumAlgorithm(ChecksumAlgorithm&& value) { SetChecksumAlgorithm(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline DeleteObjectsRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline DeleteObjectsRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline DeleteObjectsRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline DeleteObjectsRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline DeleteObjectsRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline DeleteObjectsRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline DeleteObjectsRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline DeleteObjectsRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline DeleteObjectsRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Delete m_delete;
    bool m_deleteHasBeenSet = false;

    Aws::String m_mFA;
    bool m_mFAHasBeenSet = false;

    RequestPayer m_requestPayer;
    bool m_requestPayerHasBeenSet = false;

    bool m_bypassGovernanceRetention;
    bool m_bypassGovernanceRetentionHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    ChecksumAlgorithm m_checksumAlgorithm;
    bool m_checksumAlgorithmHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
