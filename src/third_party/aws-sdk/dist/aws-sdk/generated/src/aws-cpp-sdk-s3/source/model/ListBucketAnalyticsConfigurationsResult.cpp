/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ListBucketAnalyticsConfigurationsResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

ListBucketAnalyticsConfigurationsResult::ListBucketAnalyticsConfigurationsResult() : 
    m_isTruncated(false)
{
}

ListBucketAnalyticsConfigurationsResult::ListBucketAnalyticsConfigurationsResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListBucketAnalyticsConfigurationsResult()
{
  *this = result;
}

ListBucketAnalyticsConfigurationsResult& ListBucketAnalyticsConfigurationsResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode resultNode = xmlDocument.GetRootElement();

  if(!resultNode.IsNull())
  {
    XmlNode isTruncatedNode = resultNode.FirstChild("IsTruncated");
    if(!isTruncatedNode.IsNull())
    {
      m_isTruncated = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(isTruncatedNode.GetText()).c_str()).c_str());
    }
    XmlNode continuationTokenNode = resultNode.FirstChild("ContinuationToken");
    if(!continuationTokenNode.IsNull())
    {
      m_continuationToken = Aws::Utils::Xml::DecodeEscapedXmlText(continuationTokenNode.GetText());
    }
    XmlNode nextContinuationTokenNode = resultNode.FirstChild("NextContinuationToken");
    if(!nextContinuationTokenNode.IsNull())
    {
      m_nextContinuationToken = Aws::Utils::Xml::DecodeEscapedXmlText(nextContinuationTokenNode.GetText());
    }
    XmlNode analyticsConfigurationListNode = resultNode.FirstChild("AnalyticsConfiguration");
    if(!analyticsConfigurationListNode.IsNull())
    {
      XmlNode analyticsConfigurationMember = analyticsConfigurationListNode;
      while(!analyticsConfigurationMember.IsNull())
      {
        m_analyticsConfigurationList.push_back(analyticsConfigurationMember);
        analyticsConfigurationMember = analyticsConfigurationMember.NextNode("AnalyticsConfiguration");
      }

    }
  }

  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amz-request-id");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }

  return *this;
}
