/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetObjectTorrentResult.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Stream;
using namespace Aws::Utils;
using namespace Aws;

GetObjectTorrentResult::GetObjectTorrentResult() : 
    m_requestCharged(RequestCharged::NOT_SET)
{
}

GetObjectTorrentResult::GetObjectTorrentResult(GetObjectTorrentResult&& toMove) : 
    m_body(std::move(toMove.m_body)),
    m_requestCharged(toMove.m_requestCharged),
    m_requestId(std::move(toMove.m_requestId))
{
}

GetObjectTorrentResult& GetObjectTorrentResult::operator=(GetObjectTorrentResult&& toMove)
{
   if(this == &toMove)
   {
      return *this;
   }

   m_body = std::move(toMove.m_body);
   m_requestCharged = toMove.m_requestCharged;
   m_requestId = std::move(toMove.m_requestId);

   return *this;
}

GetObjectTorrentResult::GetObjectTorrentResult(Aws::AmazonWebServiceResult<ResponseStream>&& result)
  : GetObjectTorrentResult()
{
  *this = std::move(result);
}

GetObjectTorrentResult& GetObjectTorrentResult::operator =(Aws::AmazonWebServiceResult<ResponseStream>&& result)
{
  m_body = result.TakeOwnershipOfPayload();

  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestChargedIter = headers.find("x-amz-request-charged");
  if(requestChargedIter != headers.end())
  {
    m_requestCharged = RequestChargedMapper::GetRequestChargedForName(requestChargedIter->second);
  }

  const auto& requestIdIter = headers.find("x-amz-request-id");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }

   return *this;
}
