/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Request.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/IntelligentTieringConfiguration.h>
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
  class PutBucketIntelligentTieringConfigurationRequest : public S3Request
  {
  public:
    AWS_S3_API PutBucketIntelligentTieringConfigurationRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PutBucketIntelligentTieringConfiguration"; }

    AWS_S3_API Aws::String SerializePayload() const override;

    AWS_S3_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;

    AWS_S3_API bool HasEmbeddedError(IOStream &body, const Http::HeaderValueCollection &header) const override;
    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_S3_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the Amazon S3 bucket whose configuration you want to modify or
     * retrieve.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline bool BucketHasBeenSet() const { return m_bucketHasBeenSet; }
    inline void SetBucket(const Aws::String& value) { m_bucketHasBeenSet = true; m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucketHasBeenSet = true; m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucketHasBeenSet = true; m_bucket.assign(value); }
    inline PutBucketIntelligentTieringConfigurationRequest& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline PutBucketIntelligentTieringConfigurationRequest& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline PutBucketIntelligentTieringConfigurationRequest& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ID used to identify the S3 Intelligent-Tiering configuration.</p>
     */
    inline const Aws::String& GetId() const{ return m_id; }
    inline bool IdHasBeenSet() const { return m_idHasBeenSet; }
    inline void SetId(const Aws::String& value) { m_idHasBeenSet = true; m_id = value; }
    inline void SetId(Aws::String&& value) { m_idHasBeenSet = true; m_id = std::move(value); }
    inline void SetId(const char* value) { m_idHasBeenSet = true; m_id.assign(value); }
    inline PutBucketIntelligentTieringConfigurationRequest& WithId(const Aws::String& value) { SetId(value); return *this;}
    inline PutBucketIntelligentTieringConfigurationRequest& WithId(Aws::String&& value) { SetId(std::move(value)); return *this;}
    inline PutBucketIntelligentTieringConfigurationRequest& WithId(const char* value) { SetId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for S3 Intelligent-Tiering configuration.</p>
     */
    inline const IntelligentTieringConfiguration& GetIntelligentTieringConfiguration() const{ return m_intelligentTieringConfiguration; }
    inline bool IntelligentTieringConfigurationHasBeenSet() const { return m_intelligentTieringConfigurationHasBeenSet; }
    inline void SetIntelligentTieringConfiguration(const IntelligentTieringConfiguration& value) { m_intelligentTieringConfigurationHasBeenSet = true; m_intelligentTieringConfiguration = value; }
    inline void SetIntelligentTieringConfiguration(IntelligentTieringConfiguration&& value) { m_intelligentTieringConfigurationHasBeenSet = true; m_intelligentTieringConfiguration = std::move(value); }
    inline PutBucketIntelligentTieringConfigurationRequest& WithIntelligentTieringConfiguration(const IntelligentTieringConfiguration& value) { SetIntelligentTieringConfiguration(value); return *this;}
    inline PutBucketIntelligentTieringConfigurationRequest& WithIntelligentTieringConfiguration(IntelligentTieringConfiguration&& value) { SetIntelligentTieringConfiguration(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::Map<Aws::String, Aws::String>& GetCustomizedAccessLogTag() const{ return m_customizedAccessLogTag; }
    inline bool CustomizedAccessLogTagHasBeenSet() const { return m_customizedAccessLogTagHasBeenSet; }
    inline void SetCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = value; }
    inline void SetCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag = std::move(value); }
    inline PutBucketIntelligentTieringConfigurationRequest& WithCustomizedAccessLogTag(const Aws::Map<Aws::String, Aws::String>& value) { SetCustomizedAccessLogTag(value); return *this;}
    inline PutBucketIntelligentTieringConfigurationRequest& WithCustomizedAccessLogTag(Aws::Map<Aws::String, Aws::String>&& value) { SetCustomizedAccessLogTag(std::move(value)); return *this;}
    inline PutBucketIntelligentTieringConfigurationRequest& AddCustomizedAccessLogTag(const Aws::String& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    inline PutBucketIntelligentTieringConfigurationRequest& AddCustomizedAccessLogTag(Aws::String&& key, const Aws::String& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline PutBucketIntelligentTieringConfigurationRequest& AddCustomizedAccessLogTag(const Aws::String& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline PutBucketIntelligentTieringConfigurationRequest& AddCustomizedAccessLogTag(Aws::String&& key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), std::move(value)); return *this; }
    inline PutBucketIntelligentTieringConfigurationRequest& AddCustomizedAccessLogTag(const char* key, Aws::String&& value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, std::move(value)); return *this; }
    inline PutBucketIntelligentTieringConfigurationRequest& AddCustomizedAccessLogTag(Aws::String&& key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(std::move(key), value); return *this; }
    inline PutBucketIntelligentTieringConfigurationRequest& AddCustomizedAccessLogTag(const char* key, const char* value) { m_customizedAccessLogTagHasBeenSet = true; m_customizedAccessLogTag.emplace(key, value); return *this; }
    ///@}
  private:

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_id;
    bool m_idHasBeenSet = false;

    IntelligentTieringConfiguration m_intelligentTieringConfiguration;
    bool m_intelligentTieringConfigurationHasBeenSet = false;

    Aws::Map<Aws::String, Aws::String> m_customizedAccessLogTag;
    bool m_customizedAccessLogTagHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
