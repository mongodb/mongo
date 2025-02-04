/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/CreateStreamRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

CreateStreamRequest::CreateStreamRequest() : 
    m_streamNameHasBeenSet(false),
    m_shardCount(0),
    m_shardCountHasBeenSet(false),
    m_streamModeDetailsHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Aws::String CreateStreamRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_streamNameHasBeenSet)
  {
   payload.WithString("StreamName", m_streamName);

  }

  if(m_shardCountHasBeenSet)
  {
   payload.WithInteger("ShardCount", m_shardCount);

  }

  if(m_streamModeDetailsHasBeenSet)
  {
   payload.WithObject("StreamModeDetails", m_streamModeDetails.Jsonize());

  }

  if(m_tagsHasBeenSet)
  {
   JsonValue tagsJsonMap;
   for(auto& tagsItem : m_tags)
   {
     tagsJsonMap.WithString(tagsItem.first, tagsItem.second);
   }
   payload.WithObject("Tags", std::move(tagsJsonMap));

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection CreateStreamRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.CreateStream"));
  return headers;

}




