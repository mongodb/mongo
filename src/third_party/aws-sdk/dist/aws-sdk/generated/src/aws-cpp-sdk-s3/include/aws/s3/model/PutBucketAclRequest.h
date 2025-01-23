/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/s3/model/BucketCannedACL.h>
#include <aws/s3/model/AccessControlPolicy.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
  class PutBucketAclRequest : public S3Request
  {
  public:
    AWS_S3_API PutBucketAclRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PutBucketAcl"; }

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
     * <p>The canned ACL to apply to the bucket.</p>
     */
    inline const BucketCannedACL& GetACL() const{ return m_aCL; }
    inline bool ACLHasBeenSet() const { return m_aCLHasBeenSet; }
    inline void SetACL(const BucketCannedACL& value) { m_aCLHasBeenSet = true; m_aCL = value; }
    inline void SetACL(BucketCannedACL&& value) { m_aCLHasBeenSet = true; m_aCL = std::move(value); }
    inline PutBucketAclRequest& WithACL(const BucketCannedACL& value) { SetACL(value); return *this;}
    inline PutBucketAclRequest& WithACL(BucketCannedACL&& value) { SetACL(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Contains the elements that set the ACL permissions for an object per
     * grantee.</p>
     */
    inline const AccessControlPolicy& GetAccessControlPolicy() const{ return m_accessControlPolicy; }
    inline bool AccessControlPolicyHasBeenSet() const { return m_accessControlPolicyHasBeenSet; }
    inline void SetAccessControlPolicy(const AccessControlPolicy& value) { m_accessControlPolicyHasBeenSet = true; m_accessControlPolicy = value; }
    inline void SetAccessControlPolicy(AccessControlPolicy&& value) { m_accessControlPolicyHasBeenSet = true; m_accessControlPolicy = std::move(value); }
    inline PutBucketAclRequest& WithAccessControlPolicy(const AccessControlPolicy& value) { SetAccessControlPolicy(value); return *this;}
    inline PutBucketAclRequest& WithAccessControlPolicy(AccessControlPolicy&& value) { SetAccessControlPolicy(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The bucket to which to apply the ACL.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline bool BucketHasBeenSet() const { return m_bucketHasBeenSet; }
    inline void SetBucket(const Aws::String& value) { m_bucketHasBeenSet = true; m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucketHasBeenSet = true; m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucketHasBeenSet = true; m_bucket.assign(value); }
    inline PutBucketAclRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline PutBucketAclRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline PutBucketAclRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The base64-encoded 128-bit MD5 digest of the data. This header must be used
     * as a message integrity check to verify that the request body was not corrupted
     * in transit. For more information, go to <a
     * href="http://www.ietf.org/rfc/rfc1864.txt">RFC 1864.</a> </p> <p>For requests
     * made using the Amazon Web Services Command Line Interface (CLI) or Amazon Web
     * Services SDKs, this field is calculated automatically.</p>
     */
    inline const Aws::String& GetContentMD5() const{ return m_contentMD5; }
    inline bool ContentMD5HasBeenSet() const { return m_contentMD5HasBeenSet; }
    inline void SetContentMD5(const Aws::String& value) { m_contentMD5HasBeenSet = true; m_contentMD5 = value; }
    inline void SetContentMD5(Aws::String&& value) { m_contentMD5HasBeenSet = true; m_contentMD5 = std::move(value); }
    inline void SetContentMD5(const char* value) { m_contentMD5HasBeenSet = true; m_contentMD5.assign(value); }
    inline PutBucketAclRequest& WithContentMD5(const Aws::String& value) { SetContentMD5(value); return *this;}
    inline PutBucketAclRequest& WithContentMD5(Aws::String&& value) { SetContentMD5(std::move(value)); return *this;}
    inline PutBucketAclRequest& WithContentMD5(const char* value) { SetContentMD5(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates the algorithm used to create the checksum for the object when you
     * use the SDK. This header will not provide any additional functionality if you
     * don't use the SDK. When you send this header, there must be a corresponding
     * <code>x-amz-checksum</code> or <code>x-amz-trailer</code> header sent.
     * Otherwise, Amazon S3 fails the request with the HTTP status code <code>400 Bad
     * Request</code>. For more information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/userguide/checking-object-integrity.html">Checking
     * object integrity</a> in the <i>Amazon S3 User Guide</i>.</p> <p>If you provide
     * an individual checksum, Amazon S3 ignores any provided
     * <code>ChecksumAlgorithm</code> parameter.</p>
     */
    inline const ChecksumAlgorithm& GetChecksumAlgorithm() const{ return m_checksumAlgorithm; }
    inline bool ChecksumAlgorithmHasBeenSet() const { return m_checksumAlgorithmHasBeenSet; }
    inline void SetChecksumAlgorithm(const ChecksumAlgorithm& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = value; }
    inline void SetChecksumAlgorithm(ChecksumAlgorithm&& value) { m_checksumAlgorithmHasBeenSet = true; m_checksumAlgorithm = std::move(value); }
    inline PutBucketAclRequest& WithChecksumAlgorithm(const ChecksumAlgorithm& value) { SetChecksumAlgorithm(value); return *this;}
    inline PutBucketAclRequest& WithChecksumAlgorithm(ChecksumAlgorithm&& value) { SetChecksumAlgorithm(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Allows grantee the read, write, read ACP, and write ACP permissions on the
     * bucket.</p>
     */
    inline const Aws::String& GetGrantFullControl() const{ return m_grantFullControl; }
    inline bool GrantFullControlHasBeenSet() const { return m_grantFullControlHasBeenSet; }
    inline void SetGrantFullControl(const Aws::String& value) { m_grantFullControlHasBeenSet = true; m_grantFullControl = value; }
    inline void SetGrantFullControl(Aws::String&& value) { m_grantFullControlHasBeenSet = true; m_grantFullControl = std::move(value); }
    inline void SetGrantFullControl(const char* value) { m_grantFullControlHasBeenSet = true; m_grantFullControl.assign(value); }
    inline PutBucketAclRequest& WithGrantFullControl(const Aws::String& value) { SetGrantFullControl(value); return *this;}
    inline PutBucketAclRequest& WithGrantFullControl(Aws::String&& value) { SetGrantFullControl(std::move(value)); return *this;}
    inline PutBucketAclRequest& WithGrantFullControl(const char* value) { SetGrantFullControl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Allows grantee to list the objects in the bucket.</p>
     */
    inline const Aws::String& GetGrantRead() const{ return m_grantRead; }
    inline bool GrantReadHasBeenSet() const { return m_grantReadHasBeenSet; }
    inline void SetGrantRead(const Aws::String& value) { m_grantReadHasBeenSet = true; m_grantRead = value; }
    inline void SetGrantRead(Aws::String&& value) { m_grantReadHasBeenSet = true; m_grantRead = std::move(value); }
    inline void SetGrantRead(const char* value) { m_grantReadHasBeenSet = true; m_grantRead.assign(value); }
    inline PutBucketAclRequest& WithGrantRead(const Aws::String& value) { SetGrantRead(value); return *this;}
    inline PutBucketAclRequest& WithGrantRead(Aws::String&& value) { SetGrantRead(std::move(value)); return *this;}
    inline PutBucketAclRequest& WithGrantRead(const char* value) { SetGrantRead(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Allows grantee to read the bucket ACL.</p>
     */
    inline const Aws::String& GetGrantReadACP() const{ return m_grantReadACP; }
    inline bool GrantReadACPHasBeenSet() const { return m_grantReadACPHasBeenSet; }
    inline void SetGrantReadACP(const Aws::String& value) { m_grantReadACPHasBeenSet = true; m_grantReadACP = value; }
    inline void SetGrantReadACP(Aws::String&& value) { m_grantReadACPHasBeenSet = true; m_grantReadACP = std::move(value); }
    inline void SetGrantReadACP(const char* value) { m_grantReadACPHasBeenSet = true; m_grantReadACP.assign(value); }
    inline PutBucketAclRequest& WithGrantReadACP(const Aws::String& value) { SetGrantReadACP(value); return *this;}
    inline PutBucketAclRequest& WithGrantReadACP(Aws::String&& value) { SetGrantReadACP(std::move(value)); return *this;}
    inline PutBucketAclRequest& WithGrantReadACP(const char* value) { SetGrantReadACP(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Allows grantee to create new objects in the bucket.</p> <p>For the bucket and
     * object owners of existing objects, also allows deletions and overwrites of those
     * objects.</p>
     */
    inline const Aws::String& GetGrantWrite() const{ return m_grantWrite; }
    inline bool GrantWriteHasBeenSet() const { return m_grantWriteHasBeenSet; }
    inline void SetGrantWrite(const Aws::String& value) { m_grantWriteHasBeenSet = true; m_grantWrite = value; }
    inline void SetGrantWrite(Aws::String&& value) { m_grantWriteHasBeenSet = true; m_grantWrite = std::move(value); }
    inline void SetGrantWrite(const char* value) { m_grantWriteHasBeenSet = true; m_grantWrite.assign(value); }
    inline PutBucketAclRequest& WithGrantWrite(const Aws::String& value) { SetGrantWrite(value); return *this;}
    inline PutBucketAclRequest& WithGrantWrite(Aws::String&& value) { SetGrantWrite(std::move(value)); return *this;}
    inline PutBucketAclRequest& WithGrantWrite(const char* value) { SetGrantWrite(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Allows grantee to write the ACL for the applicable bucket.</p>
     */
    inline const Aws::String& GetGrantWriteACP() const{ return m_grantWriteACP; }
    inline bool GrantWriteACPHasBeenSet() const { return m_grantWriteACPHasBeenSet; }
    inline void SetGrantWriteACP(const Aws::String& value) { m_grantWriteACPHasBeenSet = true; m_grantWriteACP = value; }
    inline void SetGrantWriteACP(Aws::String&& value) { m_grantWriteACPHasBeenSet = true; m_grantWriteACP = std::move(value); }
    inline void SetGrantWriteACP(const char* value) { m_grantWriteACPHasBeenSet = true; m_grantWriteACP.assign(value); }
    inline PutBucketAclRequest& WithGrantWriteACP(const Aws::String& value) { SetGrantWriteACP(value); return *this;}
    inline PutBucketAclRequest& WithGrantWriteACP(Aws::String&& value) { SetGrantWriteACP(std::move(value)); return *this;}
    inline PutBucketAclRequest& WithGrantWriteACP(const char* value) { SetGrantWriteACP(value); return *this;}
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
    inline PutBucketAclRequest& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline PutBucketAclRequest& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline PutBucketAclRequest& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline PutBucketAclRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline PutBucketAclRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline PutBucketAclRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline PutBucketAclRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline PutBucketAclRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline PutBucketAclRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline PutBucketAclRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline PutBucketAclRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline PutBucketAclRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    BucketCannedACL m_aCL;
    bool m_aCLHasBeenSet = false;

    AccessControlPolicy m_accessControlPolicy;
    bool m_accessControlPolicyHasBeenSet = false;

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_contentMD5;
    bool m_contentMD5HasBeenSet = false;

    ChecksumAlgorithm m_checksumAlgorithm;
    bool m_checksumAlgorithmHasBeenSet = false;

    Aws::String m_grantFullControl;
    bool m_grantFullControlHasBeenSet = false;

    Aws::String m_grantRead;
    bool m_grantReadHasBeenSet = false;

    Aws::String m_grantReadACP;
    bool m_grantReadACPHasBeenSet = false;

    Aws::String m_grantWrite;
    bool m_grantWriteHasBeenSet = false;

    Aws::String m_grantWriteACP;
    bool m_grantWriteACPHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
