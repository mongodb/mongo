/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/RequestPayer.h>
#include <aws/core/utils/DateTime.h>
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
  class DeleteObjectRequest : public S3Request
  {
  public:
    AWS_S3_API DeleteObjectRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DeleteObject"; }

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
     * <p>The bucket name of the bucket containing the object. </p> <p> <b>Directory
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
    inline DeleteObjectRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline DeleteObjectRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline DeleteObjectRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Key name of the object to delete.</p>
     */
    inline const Aws::String& GetKey() const{ return m_key; }
    inline bool KeyHasBeenSet() const { return m_keyHasBeenSet; }
    inline void SetKey(const Aws::String& value) { m_keyHasBeenSet = true; m_key = value; }
    inline void SetKey(Aws::String&& value) { m_keyHasBeenSet = true; m_key = std::move(value); }
    inline void SetKey(const char* value) { m_keyHasBeenSet = true; m_key.assign(value); }
    inline DeleteObjectRequest& WithKey(const Aws::String& value) { SetKey(value); return *this;}
    inline DeleteObjectRequest& WithKey(Aws::String&& value) { SetKey(std::move(value)); return *this;}
    inline DeleteObjectRequest& WithKey(const char* value) { SetKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The concatenation of the authentication device's serial number, a space, and
     * the value that is displayed on your authentication device. Required to
     * permanently delete a versioned object if versioning is configured with MFA
     * delete enabled.</p>  <p>This functionality is not supported for directory
     * buckets.</p> 
     */
    inline const Aws::String& GetMFA() const{ return m_mFA; }
    inline bool MFAHasBeenSet() const { return m_mFAHasBeenSet; }
    inline void SetMFA(const Aws::String& value) { m_mFAHasBeenSet = true; m_mFA = value; }
    inline void SetMFA(Aws::String&& value) { m_mFAHasBeenSet = true; m_mFA = std::move(value); }
    inline void SetMFA(const char* value) { m_mFAHasBeenSet = true; m_mFA.assign(value); }
    inline DeleteObjectRequest& WithMFA(const Aws::String& value) { SetMFA(value); return *this;}
    inline DeleteObjectRequest& WithMFA(Aws::String&& value) { SetMFA(std::move(value)); return *this;}
    inline DeleteObjectRequest& WithMFA(const char* value) { SetMFA(value); return *this;}
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
    inline DeleteObjectRequest& WithVersionId(const Aws::String& value) { SetVersionId(value); return *this;}
    inline DeleteObjectRequest& WithVersionId(Aws::String&& value) { SetVersionId(std::move(value)); return *this;}
    inline DeleteObjectRequest& WithVersionId(const char* value) { SetVersionId(value); return *this;}
    ///@}

    ///@{
    
    inline const RequestPayer& GetRequestPayer() const{ return m_requestPayer; }
    inline bool RequestPayerHasBeenSet() const { return m_requestPayerHasBeenSet; }
    inline void SetRequestPayer(const RequestPayer& value) { m_requestPayerHasBeenSet = true; m_requestPayer = value; }
    inline void SetRequestPayer(RequestPayer&& value) { m_requestPayerHasBeenSet = true; m_requestPayer = std::move(value); }
    inline DeleteObjectRequest& WithRequestPayer(const RequestPayer& value) { SetRequestPayer(value); return *this;}
    inline DeleteObjectRequest& WithRequestPayer(RequestPayer&& value) { SetRequestPayer(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether S3 Object Lock should bypass Governance-mode restrictions
     * to process this operation. To use this header, you must have the
     * <code>s3:BypassGovernanceRetention</code> permission.</p>  <p>This
     * functionality is not supported for directory buckets.</p> 
     */
    inline bool GetBypassGovernanceRetention() const{ return m_bypassGovernanceRetention; }
    inline bool BypassGovernanceRetentionHasBeenSet() const { return m_bypassGovernanceRetentionHasBeenSet; }
    inline void SetBypassGovernanceRetention(bool value) { m_bypassGovernanceRetentionHasBeenSet = true; m_bypassGovernanceRetention = value; }
    inline DeleteObjectRequest& WithBypassGovernanceRetention(bool value) { SetBypassGovernanceRetention(value); return *this;}
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
    inline DeleteObjectRequest& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline DeleteObjectRequest& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline DeleteObjectRequest& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <code>If-Match</code> header field makes the request method conditional
     * on ETags. If the ETag value does not match, the operation returns a <code>412
     * Precondition Failed</code> error. If the ETag matches or if the object doesn't
     * exist, the operation will return a <code>204 Success (No Content)
     * response</code>.</p> <p>For more information about conditional requests, see <a
     * href="https://docs.aws.amazon.com/https:/tools.ietf.org/html/rfc7232">RFC
     * 7232</a>.</p>  <p>This functionality is only supported for directory
     * buckets.</p> 
     */
    inline const Aws::String& GetIfMatch() const{ return m_ifMatch; }
    inline bool IfMatchHasBeenSet() const { return m_ifMatchHasBeenSet; }
    inline void SetIfMatch(const Aws::String& value) { m_ifMatchHasBeenSet = true; m_ifMatch = value; }
    inline void SetIfMatch(Aws::String&& value) { m_ifMatchHasBeenSet = true; m_ifMatch = std::move(value); }
    inline void SetIfMatch(const char* value) { m_ifMatchHasBeenSet = true; m_ifMatch.assign(value); }
    inline DeleteObjectRequest& WithIfMatch(const Aws::String& value) { SetIfMatch(value); return *this;}
    inline DeleteObjectRequest& WithIfMatch(Aws::String&& value) { SetIfMatch(std::move(value)); return *this;}
    inline DeleteObjectRequest& WithIfMatch(const char* value) { SetIfMatch(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, the object is deleted only if its modification times matches the
     * provided <code>Timestamp</code>. If the <code>Timestamp</code> values do not
     * match, the operation returns a <code>412 Precondition Failed</code> error. If
     * the <code>Timestamp</code> matches or if the object doesn’t exist, the operation
     * returns a <code>204 Success (No Content)</code> response.</p>  <p>This
     * functionality is only supported for directory buckets.</p> 
     */
    inline const Aws::Utils::DateTime& GetIfMatchLastModifiedTime() const{ return m_ifMatchLastModifiedTime; }
    inline bool IfMatchLastModifiedTimeHasBeenSet() const { return m_ifMatchLastModifiedTimeHasBeenSet; }
    inline void SetIfMatchLastModifiedTime(const Aws::Utils::DateTime& value) { m_ifMatchLastModifiedTimeHasBeenSet = true; m_ifMatchLastModifiedTime = value; }
    inline void SetIfMatchLastModifiedTime(Aws::Utils::DateTime&& value) { m_ifMatchLastModifiedTimeHasBeenSet = true; m_ifMatchLastModifiedTime = std::move(value); }
    inline DeleteObjectRequest& WithIfMatchLastModifiedTime(const Aws::Utils::DateTime& value) { SetIfMatchLastModifiedTime(value); return *this;}
    inline DeleteObjectRequest& WithIfMatchLastModifiedTime(Aws::Utils::DateTime&& value) { SetIfMatchLastModifiedTime(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, the object is deleted only if its size matches the provided size
     * in bytes. If the <code>Size</code> value does not match, the operation returns a
     * <code>412 Precondition Failed</code> error. If the <code>Size</code> matches or
     * if the object doesn’t exist, the operation returns a <code>204 Success (No
     * Content)</code> response.</p>  <p>This functionality is only supported for
     * directory buckets.</p>   <p>You can use the
     * <code>If-Match</code>, <code>x-amz-if-match-last-modified-time</code> and
     * <code>x-amz-if-match-size</code> conditional headers in conjunction with
     * each-other or individually.</p> 
     */
    inline long long GetIfMatchSize() const{ return m_ifMatchSize; }
    inline bool IfMatchSizeHasBeenSet() const { return m_ifMatchSizeHasBeenSet; }
    inline void SetIfMatchSize(long long value) { m_ifMatchSizeHasBeenSet = true; m_ifMatchSize = value; }
    inline DeleteObjectRequest& WithIfMatchSize(long long value) { SetIfMatchSize(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline DeleteObjectRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline DeleteObjectRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline DeleteObjectRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline DeleteObjectRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline DeleteObjectRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline DeleteObjectRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline DeleteObjectRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline DeleteObjectRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline DeleteObjectRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_key;
    bool m_keyHasBeenSet = false;

    Aws::String m_mFA;
    bool m_mFAHasBeenSet = false;

    Aws::String m_versionId;
    bool m_versionIdHasBeenSet = false;

    RequestPayer m_requestPayer;
    bool m_requestPayerHasBeenSet = false;

    bool m_bypassGovernanceRetention;
    bool m_bypassGovernanceRetentionHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    Aws::String m_ifMatch;
    bool m_ifMatchHasBeenSet = false;

    Aws::Utils::DateTime m_ifMatchLastModifiedTime;
    bool m_ifMatchLastModifiedTimeHasBeenSet = false;

    long long m_ifMatchSize;
    bool m_ifMatchSizeHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
