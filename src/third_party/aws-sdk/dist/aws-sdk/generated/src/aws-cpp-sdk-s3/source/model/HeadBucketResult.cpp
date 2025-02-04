/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/HeadBucketResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

HeadBucketResult::HeadBucketResult() : 
    m_bucketLocationType(LocationType::NOT_SET),
    m_accessPointAlias(false)
{
}

HeadBucketResult::HeadBucketResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : HeadBucketResult()
{
  *this = result;
}

HeadBucketResult& HeadBucketResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode resultNode = xmlDocument.GetRootElement();

  if(!resultNode.IsNull())
  {
  }

  const auto& headers = result.GetHeaderValueCollection();
  const auto& bucketLocationTypeIter = headers.find("x-amz-bucket-location-type");
  if(bucketLocationTypeIter != headers.end())
  {
    m_bucketLocationType = LocationTypeMapper::GetLocationTypeForName(bucketLocationTypeIter->second);
  }

  const auto& bucketLocationNameIter = headers.find("x-amz-bucket-location-name");
  if(bucketLocationNameIter != headers.end())
  {
    m_bucketLocationName = bucketLocationNameIter->second;
  }

  const auto& bucketRegionIter = headers.find("x-amz-bucket-region");
  if(bucketRegionIter != headers.end())
  {
    m_bucketRegion = bucketRegionIter->second;
  }

  const auto& accessPointAliasIter = headers.find("x-amz-access-point-alias");
  if(accessPointAliasIter != headers.end())
  {
     m_accessPointAlias = StringUtils::ConvertToBool(accessPointAliasIter->second.c_str());
  }

  const auto& requestIdIter = headers.find("x-amz-request-id");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }

  return *this;
}
