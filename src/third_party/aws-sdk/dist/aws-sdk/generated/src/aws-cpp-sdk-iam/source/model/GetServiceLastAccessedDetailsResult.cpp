/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetServiceLastAccessedDetailsResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogMacros.h>

#include <utility>

using namespace Aws::IAM::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils::Logging;
using namespace Aws::Utils;
using namespace Aws;

GetServiceLastAccessedDetailsResult::GetServiceLastAccessedDetailsResult() : 
    m_jobStatus(JobStatusType::NOT_SET),
    m_jobType(AccessAdvisorUsageGranularityType::NOT_SET),
    m_isTruncated(false)
{
}

GetServiceLastAccessedDetailsResult::GetServiceLastAccessedDetailsResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : GetServiceLastAccessedDetailsResult()
{
  *this = result;
}

GetServiceLastAccessedDetailsResult& GetServiceLastAccessedDetailsResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetServiceLastAccessedDetailsResult"))
  {
    resultNode = rootNode.FirstChild("GetServiceLastAccessedDetailsResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode jobStatusNode = resultNode.FirstChild("JobStatus");
    if(!jobStatusNode.IsNull())
    {
      m_jobStatus = JobStatusTypeMapper::GetJobStatusTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(jobStatusNode.GetText()).c_str()).c_str());
    }
    XmlNode jobTypeNode = resultNode.FirstChild("JobType");
    if(!jobTypeNode.IsNull())
    {
      m_jobType = AccessAdvisorUsageGranularityTypeMapper::GetAccessAdvisorUsageGranularityTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(jobTypeNode.GetText()).c_str()).c_str());
    }
    XmlNode jobCreationDateNode = resultNode.FirstChild("JobCreationDate");
    if(!jobCreationDateNode.IsNull())
    {
      m_jobCreationDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(jobCreationDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
    }
    XmlNode servicesLastAccessedNode = resultNode.FirstChild("ServicesLastAccessed");
    if(!servicesLastAccessedNode.IsNull())
    {
      XmlNode servicesLastAccessedMember = servicesLastAccessedNode.FirstChild("member");
      while(!servicesLastAccessedMember.IsNull())
      {
        m_servicesLastAccessed.push_back(servicesLastAccessedMember);
        servicesLastAccessedMember = servicesLastAccessedMember.NextNode("member");
      }

    }
    XmlNode jobCompletionDateNode = resultNode.FirstChild("JobCompletionDate");
    if(!jobCompletionDateNode.IsNull())
    {
      m_jobCompletionDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(jobCompletionDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
    }
    XmlNode isTruncatedNode = resultNode.FirstChild("IsTruncated");
    if(!isTruncatedNode.IsNull())
    {
      m_isTruncated = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(isTruncatedNode.GetText()).c_str()).c_str());
    }
    XmlNode markerNode = resultNode.FirstChild("Marker");
    if(!markerNode.IsNull())
    {
      m_marker = Aws::Utils::Xml::DecodeEscapedXmlText(markerNode.GetText());
    }
    XmlNode errorNode = resultNode.FirstChild("Error");
    if(!errorNode.IsNull())
    {
      m_error = errorNode;
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetServiceLastAccessedDetailsResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
