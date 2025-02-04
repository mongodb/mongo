/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
  class ListBucketsRequest : public S3Request
  {
  public:
    AWS_S3_API ListBucketsRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ListBuckets"; }

    AWS_S3_API Aws::String SerializePayload() const override;

    AWS_S3_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;

    AWS_S3_API bool HasEmbeddedError(IOStream &body, const Http::HeaderValueCollection &header) const override;

    ///@{
    /**
     * <p>Maximum number of buckets to be returned in response. When the number is more
     * than the count of buckets that are owned by an Amazon Web Services account,
     * return all the buckets in response.</p>
     */
    inline int GetMaxBuckets() const{ return m_maxBuckets; }
    inline bool MaxBucketsHasBeenSet() const { return m_maxBucketsHasBeenSet; }
    inline void SetMaxBuckets(int value) { m_maxBucketsHasBeenSet = true; m_maxBuckets = value; }
    inline ListBucketsRequest& WithMaxBuckets(int value) { SetMaxBuckets(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> <code>ContinuationToken</code> indicates to Amazon S3 that the list is being
     * continued on this bucket with a token. <code>ContinuationToken</code> is
     * obfuscated and is not a real key. You can use this
     * <code>ContinuationToken</code> for pagination of the list results. </p>
     * <p>Length Constraints: Minimum length of 0. Maximum length of 1024.</p>
     * <p>Required: No.</p>  <p>If you specify the <code>bucket-region</code>,
     * <code>prefix</code>, or <code>continuation-token</code> query parameters without
     * using <code>max-buckets</code> to set the maximum number of buckets returned in
     * the response, Amazon S3 applies a default page size of 10,000 and provides a
     * continuation token if there are more buckets.</p> 
     */
    inline const Aws::String& GetContinuationToken() const{ return m_continuationToken; }
    inline bool ContinuationTokenHasBeenSet() const { return m_continuationTokenHasBeenSet; }
    inline void SetContinuationToken(const Aws::String& value) { m_continuationTokenHasBeenSet = true; m_continuationToken = value; }
    inline void SetContinuationToken(Aws::String&& value) { m_continuationTokenHasBeenSet = true; m_continuationToken = std::move(value); }
    inline void SetContinuationToken(const char* value) { m_continuationTokenHasBeenSet = true; m_continuationToken.assign(value); }
    inline ListBucketsRequest& WithContinuationToken(const Aws::String& value) { SetContinuationToken(value); return *this;}
    inline ListBucketsRequest& WithContinuationToken(Aws::String&& value) { SetContinuationToken(std::move(value)); return *this;}
    inline ListBucketsRequest& WithContinuationToken(const char* value) { SetContinuationToken(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Limits the response to bucket names that begin with the specified bucket name
     * prefix.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline ListBucketsRequest& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline ListBucketsRequest& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline ListBucketsRequest& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Limits the response to buckets that are located in the specified Amazon Web
     * Services Region. The Amazon Web Services Region must be expressed according to
     * the Amazon Web Services Region code, such as <code>us-west-2</code> for the US
     * West (Oregon) Region. For a list of the valid values for all of the Amazon Web
     * Services Regions, see <a
     * href="https://docs.aws.amazon.com/general/latest/gr/rande.html#s3_region">Regions
     * and Endpoints</a>.</p>  <p>Requests made to a Regional endpoint that is
     * different from the <code>bucket-region</code> parameter are not supported. For
     * example, if you want to limit the response to your buckets in Region
     * <code>us-west-2</code>, the request must be made to an endpoint in Region
     * <code>us-west-2</code>.</p> 
     */
    inline const Aws::String& GetBucketRegion() const{ return m_bucketRegion; }
    inline bool BucketRegionHasBeenSet() const { return m_bucketRegionHasBeenSet; }
    inline void SetBucketRegion(const Aws::String& value) { m_bucketRegionHasBeenSet = true; m_bucketRegion = value; }
    inline void SetBucketRegion(Aws::String&& value) { m_bucketRegionHasBeenSet = true; m_bucketRegion = std::move(value); }
    inline void SetBucketRegion(const char* value) { m_bucketRegionHasBeenSet = true; m_bucketRegion.assign(value); }
    inline ListBucketsRequest& WithBucketRegion(const Aws::String& value) { SetBucketRegion(value); return *this;}
    inline ListBucketsRequest& WithBucketRegion(Aws::String&& value) { SetBucketRegion(std::move(value)); return *this;}
    inline ListBucketsRequest& WithBucketRegion(const char* value) { SetBucketRegion(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline ListBucketsRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline ListBucketsRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline ListBucketsRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline ListBucketsRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline ListBucketsRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline ListBucketsRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline ListBucketsRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline ListBucketsRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline ListBucketsRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    int m_maxBuckets;
    bool m_maxBucketsHasBeenSet = false;

    Aws::String m_continuationToken;
    bool m_continuationTokenHasBeenSet = false;

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;

    Aws::String m_bucketRegion;
    bool m_bucketRegionHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
