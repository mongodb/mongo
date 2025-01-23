/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/KinesisRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <utility>

namespace Aws
{
namespace Kinesis
{
namespace Model
{

  /**
   * <p>Represents the input for <code>RemoveTagsFromStream</code>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/RemoveTagsFromStreamInput">AWS
   * API Reference</a></p>
   */
  class RemoveTagsFromStreamRequest : public KinesisRequest
  {
  public:
    AWS_KINESIS_API RemoveTagsFromStreamRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "RemoveTagsFromStream"; }

    AWS_KINESIS_API Aws::String SerializePayload() const override;

    AWS_KINESIS_API Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const override;

    /**
     * Helper function to collect parameters (configurable and static hardcoded) required for endpoint computation.
     */
    AWS_KINESIS_API EndpointParameters GetEndpointContextParams() const override;

    ///@{
    /**
     * <p>The name of the stream.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline bool StreamNameHasBeenSet() const { return m_streamNameHasBeenSet; }
    inline void SetStreamName(const Aws::String& value) { m_streamNameHasBeenSet = true; m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamNameHasBeenSet = true; m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamNameHasBeenSet = true; m_streamName.assign(value); }
    inline RemoveTagsFromStreamRequest& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline RemoveTagsFromStreamRequest& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline RemoveTagsFromStreamRequest& WithStreamName(const char* value) { SetStreamName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tag keys. Each corresponding tag is removed from the stream.</p>
     */
    inline const Aws::Vector<Aws::String>& GetTagKeys() const{ return m_tagKeys; }
    inline bool TagKeysHasBeenSet() const { return m_tagKeysHasBeenSet; }
    inline void SetTagKeys(const Aws::Vector<Aws::String>& value) { m_tagKeysHasBeenSet = true; m_tagKeys = value; }
    inline void SetTagKeys(Aws::Vector<Aws::String>&& value) { m_tagKeysHasBeenSet = true; m_tagKeys = std::move(value); }
    inline RemoveTagsFromStreamRequest& WithTagKeys(const Aws::Vector<Aws::String>& value) { SetTagKeys(value); return *this;}
    inline RemoveTagsFromStreamRequest& WithTagKeys(Aws::Vector<Aws::String>&& value) { SetTagKeys(std::move(value)); return *this;}
    inline RemoveTagsFromStreamRequest& AddTagKeys(const Aws::String& value) { m_tagKeysHasBeenSet = true; m_tagKeys.push_back(value); return *this; }
    inline RemoveTagsFromStreamRequest& AddTagKeys(Aws::String&& value) { m_tagKeysHasBeenSet = true; m_tagKeys.push_back(std::move(value)); return *this; }
    inline RemoveTagsFromStreamRequest& AddTagKeys(const char* value) { m_tagKeysHasBeenSet = true; m_tagKeys.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>The ARN of the stream.</p>
     */
    inline const Aws::String& GetStreamARN() const{ return m_streamARN; }
    inline bool StreamARNHasBeenSet() const { return m_streamARNHasBeenSet; }
    inline void SetStreamARN(const Aws::String& value) { m_streamARNHasBeenSet = true; m_streamARN = value; }
    inline void SetStreamARN(Aws::String&& value) { m_streamARNHasBeenSet = true; m_streamARN = std::move(value); }
    inline void SetStreamARN(const char* value) { m_streamARNHasBeenSet = true; m_streamARN.assign(value); }
    inline RemoveTagsFromStreamRequest& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline RemoveTagsFromStreamRequest& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline RemoveTagsFromStreamRequest& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}
  private:

    Aws::String m_streamName;
    bool m_streamNameHasBeenSet = false;

    Aws::Vector<Aws::String> m_tagKeys;
    bool m_tagKeysHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
