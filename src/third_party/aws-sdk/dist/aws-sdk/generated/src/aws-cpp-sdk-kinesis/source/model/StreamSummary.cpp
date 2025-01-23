/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/StreamSummary.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Kinesis
{
namespace Model
{

StreamSummary::StreamSummary() : 
    m_streamNameHasBeenSet(false),
    m_streamARNHasBeenSet(false),
    m_streamStatus(StreamStatus::NOT_SET),
    m_streamStatusHasBeenSet(false),
    m_streamModeDetailsHasBeenSet(false),
    m_streamCreationTimestampHasBeenSet(false)
{
}

StreamSummary::StreamSummary(JsonView jsonValue)
  : StreamSummary()
{
  *this = jsonValue;
}

StreamSummary& StreamSummary::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("StreamName"))
  {
    m_streamName = jsonValue.GetString("StreamName");

    m_streamNameHasBeenSet = true;
  }

  if(jsonValue.ValueExists("StreamARN"))
  {
    m_streamARN = jsonValue.GetString("StreamARN");

    m_streamARNHasBeenSet = true;
  }

  if(jsonValue.ValueExists("StreamStatus"))
  {
    m_streamStatus = StreamStatusMapper::GetStreamStatusForName(jsonValue.GetString("StreamStatus"));

    m_streamStatusHasBeenSet = true;
  }

  if(jsonValue.ValueExists("StreamModeDetails"))
  {
    m_streamModeDetails = jsonValue.GetObject("StreamModeDetails");

    m_streamModeDetailsHasBeenSet = true;
  }

  if(jsonValue.ValueExists("StreamCreationTimestamp"))
  {
    m_streamCreationTimestamp = jsonValue.GetDouble("StreamCreationTimestamp");

    m_streamCreationTimestampHasBeenSet = true;
  }

  return *this;
}

JsonValue StreamSummary::Jsonize() const
{
  JsonValue payload;

  if(m_streamNameHasBeenSet)
  {
   payload.WithString("StreamName", m_streamName);

  }

  if(m_streamARNHasBeenSet)
  {
   payload.WithString("StreamARN", m_streamARN);

  }

  if(m_streamStatusHasBeenSet)
  {
   payload.WithString("StreamStatus", StreamStatusMapper::GetNameForStreamStatus(m_streamStatus));
  }

  if(m_streamModeDetailsHasBeenSet)
  {
   payload.WithObject("StreamModeDetails", m_streamModeDetails.Jsonize());

  }

  if(m_streamCreationTimestampHasBeenSet)
  {
   payload.WithDouble("StreamCreationTimestamp", m_streamCreationTimestamp.SecondsWithMSPrecision());
  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
