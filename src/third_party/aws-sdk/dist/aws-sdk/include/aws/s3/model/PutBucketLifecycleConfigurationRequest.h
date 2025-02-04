/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/ChecksumAlgorithm.h>
#include <aws/s3/model/BucketLifecycleConfiguration.h>
#include <aws/s3/model/TransitionDefaultMinimumObjectSize.h>
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
  class PutBucketLifecycleConfigurationRequest : public S3Request
  {
  public:
    AWS_S3_API PutBucketLifecycleConfigurationRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PutBucketLifecycleConfiguration"; }

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
     * <p>The name of the bucket for which to set the configuration.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline bool BucketHasBeenSet() const { return m_bucketHasBeenSet; }
    inline void SetBucket(const Aws::String& value) { m_bucketHasBeenSet = true; m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucketHasBeenSet = true; m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucketHasBeenSet = true; m_bucket.assign(value); }
    inline PutBucketLifecycleConfigurationRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline PutBucketLifecycleConfigurationRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline PutBucketLifecycleConfigurationRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
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
    inline PutBucketLifecycleConfigurationRequest& WithChecksumAlgorithm(const ChecksumAlgorithm& value) { SetChecksumAlgorithm(value); return *this;}
    inline PutBucketLifecycleConfigurationRequest& WithChecksumAlgorithm(ChecksumAlgorithm&& value) { SetChecksumAlgorithm(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for lifecycle rules. You can add as many as 1,000 rules.</p>
     */
    inline const BucketLifecycleConfiguration& GetLifecycleConfiguration() const{ return m_lifecycleConfiguration; }
    inline bool LifecycleConfigurationHasBeenSet() const { return m_lifecycleConfigurationHasBeenSet; }
    inline void SetLifecycleConfiguration(const BucketLifecycleConfiguration& value) { m_lifecycleConfigurationHasBeenSet = true; m_lifecycleConfiguration = value; }
    inline void SetLifecycleConfiguration(BucketLifecycleConfiguration&& value) { m_lifecycleConfigurationHasBeenSet = true; m_lifecycleConfiguration = std::move(value); }
    inline PutBucketLifecycleConfigurationRequest& WithLifecycleConfiguration(const BucketLifecycleConfiguration& value) { SetLifecycleConfiguration(value); return *this;}
    inline PutBucketLifecycleConfigurationRequest& WithLifecycleConfiguration(BucketLifecycleConfiguration&& value) { SetLifecycleConfiguration(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The account ID of the expected bucket owner. If the account ID that you
     * provide does not match the actual owner of the bucket, the request fails with
     * the HTTP status code <code>403 Forbidden</code> (access denied).</p> 
     * <p>This parameter applies to general purpose buckets only. It is not supported
     * for directory bucket lifecycle configurations.</p> 
     */
    inline const Aws::String& GetExpectedBucketOwner() const{ return m_expectedBucketOwner; }
    inline bool ExpectedBucketOwnerHasBeenSet() const { return m_expectedBucketOwnerHasBeenSet; }
    inline void SetExpectedBucketOwner(const Aws::String& value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner = value; }
    inline void SetExpectedBucketOwner(Aws::String&& value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner = std::move(value); }
    inline void SetExpectedBucketOwner(const char* value) { m_expectedBucketOwnerHasBeenSet = true; m_expectedBucketOwner.assign(value); }
    inline PutBucketLifecycleConfigurationRequest& WithExpectedBucketOwner(const Aws::String& value) { SetExpectedBucketOwner(value); return *this;}
    inline PutBucketLifecycleConfigurationRequest& WithExpectedBucketOwner(Aws::String&& value) { SetExpectedBucketOwner(std::move(value)); return *this;}
    inline PutBucketLifecycleConfigurationRequest& WithExpectedBucketOwner(const char* value) { SetExpectedBucketOwner(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates which default minimum object size behavior is applied to the
     * lifecycle configuration.</p>  <p>This parameter applies to general purpose
     * buckets only. It is not supported for directory bucket lifecycle
     * configurations.</p>  <ul> <li> <p> <code>all_storage_classes_128K</code>
     * - Objects smaller than 128 KB will not transition to any storage class by
     * default. </p> </li> <li> <p> <code>varies_by_storage_class</code> - Objects
     * smaller than 128 KB will transition to Glacier Flexible Retrieval or Glacier
     * Deep Archive storage classes. By default, all other storage classes will prevent
     * transitions smaller than 128 KB. </p> </li> </ul> <p>To customize the minimum
     * object size for any transition you can add a filter that specifies a custom
     * <code>ObjectSizeGreaterThan</code> or <code>ObjectSizeLessThan</code> in the
     * body of your transition rule. Custom filters always take precedence over the
     * default transition behavior.</p>
     */
    inline const TransitionDefaultMinimumObjectSize& GetTransitionDefaultMinimumObjectSize() const{ return m_transitionDefaultMinimumObjectSize; }
    inline bool TransitionDefaultMinimumObjectSizeHasBeenSet() const { return m_transitionDefaultMinimumObjectSizeHasBeenSet; }
    inline void SetTransitionDefaultMinimumObjectSize(const TransitionDefaultMinimumObjectSize& value) { m_transitionDefaultMinimumObjectSizeHasBeenSet = true; m_transitionDefaultMinimumObjectSize = value; }
    inline void SetTransitionDefaultMinimumObjectSize(TransitionDefaultMinimumObjectSize&& value) { m_transitionDefaultMinimumObjectSizeHasBeenSet = true; m_transitionDefaultMinimumObjectSize = std::move(value); }
    inline PutBucketLifecycleConfigurationRequest& WithTransitionDefaultMinimumObjectSize(const TransitionDefaultMinimumObjectSize& value) { SetTransitionDefaultMinimumObjectSize(value); return *this;}
    inline PutBucketLifecycleConfigurationRequest& WithTransitionDefaultMinimumObjectSize(TransitionDefaultMinimumObjectSize&& value) { SetTransitionDefaultMinimumObjectSize(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline PutBucketLifecycleConfigurationRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline PutBucketLifecycleConfigurationRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline PutBucketLifecycleConfigurationRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline PutBucketLifecycleConfigurationRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline PutBucketLifecycleConfigurationRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline PutBucketLifecycleConfigurationRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline PutBucketLifecycleConfigurationRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline PutBucketLifecycleConfigurationRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline PutBucketLifecycleConfigurationRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    ChecksumAlgorithm m_checksumAlgorithm;
    bool m_checksumAlgorithmHasBeenSet = false;

    BucketLifecycleConfiguration m_lifecycleConfiguration;
    bool m_lifecycleConfigurationHasBeenSet = false;

    Aws::String m_expectedBucketOwner;
    bool m_expectedBucketOwnerHasBeenSet = false;

    TransitionDefaultMinimumObjectSize m_transitionDefaultMinimumObjectSize;
    bool m_transitionDefaultMinimumObjectSizeHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
