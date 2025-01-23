/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/ListStreamsRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Kinesis::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

ListStreamsRequest::ListStreamsRequest() : 
    m_limit(0),
    m_limitHasBeenSet(false),
    m_exclusiveStartStreamNameHasBeenSet(false),
    m_nextTokenHasBeenSet(false)
{
}

Aws::String ListStreamsRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_limitHasBeenSet)
  {
   payload.WithInteger("Limit", m_limit);

  }

  if(m_exclusiveStartStreamNameHasBeenSet)
  {
   payload.WithString("ExclusiveStartStreamName", m_exclusiveStartStreamName);

  }

  if(m_nextTokenHasBeenSet)
  {
   payload.WithString("NextToken", m_nextToken);

  }

  return payload.View().WriteReadable();
}

Aws::Http::HeaderValueCollection ListStreamsRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  headers.insert(Aws::Http::HeaderValuePair("X-Amz-Target", "Kinesis_20131202.ListStreams"));
  return headers;

}




