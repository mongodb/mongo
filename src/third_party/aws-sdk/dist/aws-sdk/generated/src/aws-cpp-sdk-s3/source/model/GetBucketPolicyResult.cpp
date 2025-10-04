/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetBucketPolicyResult.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Stream;
using namespace Aws::Utils;
using namespace Aws;

GetBucketPolicyResult::GetBucketPolicyResult()
{
}

GetBucketPolicyResult::GetBucketPolicyResult(GetBucketPolicyResult&& toMove) : 
    m_policy(std::move(toMove.m_policy)),
    m_requestId(std::move(toMove.m_requestId))
{
}

GetBucketPolicyResult& GetBucketPolicyResult::operator=(GetBucketPolicyResult&& toMove)
{
   if(this == &toMove)
   {
      return *this;
   }

   m_policy = std::move(toMove.m_policy);
   m_requestId = std::move(toMove.m_requestId);

   return *this;
}

GetBucketPolicyResult::GetBucketPolicyResult(Aws::AmazonWebServiceResult<ResponseStream>&& result)
{
  *this = std::move(result);
}

GetBucketPolicyResult& GetBucketPolicyResult::operator =(Aws::AmazonWebServiceResult<ResponseStream>&& result)
{
  m_policy = result.TakeOwnershipOfPayload();

  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amz-request-id");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }

   return *this;
}
