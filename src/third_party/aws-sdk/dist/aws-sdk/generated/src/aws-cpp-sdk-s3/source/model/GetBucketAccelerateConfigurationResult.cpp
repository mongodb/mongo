/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetBucketAccelerateConfigurationResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

GetBucketAccelerateConfigurationResult::GetBucketAccelerateConfigurationResult() : 
    m_status(BucketAccelerateStatus::NOT_SET),
    m_requestCharged(RequestCharged::NOT_SET)
{
}

GetBucketAccelerateConfigurationResult::GetBucketAccelerateConfigurationResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : GetBucketAccelerateConfigurationResult()
{
  *this = result;
}

GetBucketAccelerateConfigurationResult& GetBucketAccelerateConfigurationResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode resultNode = xmlDocument.GetRootElement();

  if(!resultNode.IsNull())
  {
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = BucketAccelerateStatusMapper::GetBucketAccelerateStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
    }
  }

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
