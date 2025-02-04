/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/kinesis/model/StreamStatus.h>
#include <aws/kinesis/model/StreamModeDetails.h>
#include <aws/core/utils/DateTime.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Kinesis
{
namespace Model
{

  /**
   * <p>The summary of a stream.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/StreamSummary">AWS
   * API Reference</a></p>
   */
  class StreamSummary
  {
  public:
    AWS_KINESIS_API StreamSummary();
    AWS_KINESIS_API StreamSummary(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API StreamSummary& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The name of a stream.</p>
     */
    inline const Aws::String& GetStreamName() const{ return m_streamName; }
    inline bool StreamNameHasBeenSet() const { return m_streamNameHasBeenSet; }
    inline void SetStreamName(const Aws::String& value) { m_streamNameHasBeenSet = true; m_streamName = value; }
    inline void SetStreamName(Aws::String&& value) { m_streamNameHasBeenSet = true; m_streamName = std::move(value); }
    inline void SetStreamName(const char* value) { m_streamNameHasBeenSet = true; m_streamName.assign(value); }
    inline StreamSummary& WithStreamName(const Aws::String& value) { SetStreamName(value); return *this;}
    inline StreamSummary& WithStreamName(Aws::String&& value) { SetStreamName(std::move(value)); return *this;}
    inline StreamSummary& WithStreamName(const char* value) { SetStreamName(value); return *this;}
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
    inline StreamSummary& WithStreamARN(const Aws::String& value) { SetStreamARN(value); return *this;}
    inline StreamSummary& WithStreamARN(Aws::String&& value) { SetStreamARN(std::move(value)); return *this;}
    inline StreamSummary& WithStreamARN(const char* value) { SetStreamARN(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The status of the stream.</p>
     */
    inline const StreamStatus& GetStreamStatus() const{ return m_streamStatus; }
    inline bool StreamStatusHasBeenSet() const { return m_streamStatusHasBeenSet; }
    inline void SetStreamStatus(const StreamStatus& value) { m_streamStatusHasBeenSet = true; m_streamStatus = value; }
    inline void SetStreamStatus(StreamStatus&& value) { m_streamStatusHasBeenSet = true; m_streamStatus = std::move(value); }
    inline StreamSummary& WithStreamStatus(const StreamStatus& value) { SetStreamStatus(value); return *this;}
    inline StreamSummary& WithStreamStatus(StreamStatus&& value) { SetStreamStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const StreamModeDetails& GetStreamModeDetails() const{ return m_streamModeDetails; }
    inline bool StreamModeDetailsHasBeenSet() const { return m_streamModeDetailsHasBeenSet; }
    inline void SetStreamModeDetails(const StreamModeDetails& value) { m_streamModeDetailsHasBeenSet = true; m_streamModeDetails = value; }
    inline void SetStreamModeDetails(StreamModeDetails&& value) { m_streamModeDetailsHasBeenSet = true; m_streamModeDetails = std::move(value); }
    inline StreamSummary& WithStreamModeDetails(const StreamModeDetails& value) { SetStreamModeDetails(value); return *this;}
    inline StreamSummary& WithStreamModeDetails(StreamModeDetails&& value) { SetStreamModeDetails(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The timestamp at which the stream was created.</p>
     */
    inline const Aws::Utils::DateTime& GetStreamCreationTimestamp() const{ return m_streamCreationTimestamp; }
    inline bool StreamCreationTimestampHasBeenSet() const { return m_streamCreationTimestampHasBeenSet; }
    inline void SetStreamCreationTimestamp(const Aws::Utils::DateTime& value) { m_streamCreationTimestampHasBeenSet = true; m_streamCreationTimestamp = value; }
    inline void SetStreamCreationTimestamp(Aws::Utils::DateTime&& value) { m_streamCreationTimestampHasBeenSet = true; m_streamCreationTimestamp = std::move(value); }
    inline StreamSummary& WithStreamCreationTimestamp(const Aws::Utils::DateTime& value) { SetStreamCreationTimestamp(value); return *this;}
    inline StreamSummary& WithStreamCreationTimestamp(Aws::Utils::DateTime&& value) { SetStreamCreationTimestamp(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_streamName;
    bool m_streamNameHasBeenSet = false;

    Aws::String m_streamARN;
    bool m_streamARNHasBeenSet = false;

    StreamStatus m_streamStatus;
    bool m_streamStatusHasBeenSet = false;

    StreamModeDetails m_streamModeDetails;
    bool m_streamModeDetailsHasBeenSet = false;

    Aws::Utils::DateTime m_streamCreationTimestamp;
    bool m_streamCreationTimestampHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
