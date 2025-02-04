/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetServiceLastAccessedDetailsWithEntitiesResult.h>
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

GetServiceLastAccessedDetailsWithEntitiesResult::GetServiceLastAccessedDetailsWithEntitiesResult() : 
    m_jobStatus(JobStatusType::NOT_SET),
    m_isTruncated(false)
{
}

GetServiceLastAccessedDetailsWithEntitiesResult::GetServiceLastAccessedDetailsWithEntitiesResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : GetServiceLastAccessedDetailsWithEntitiesResult()
{
  *this = result;
}

GetServiceLastAccessedDetailsWithEntitiesResult& GetServiceLastAccessedDetailsWithEntitiesResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetServiceLastAccessedDetailsWithEntitiesResult"))
  {
    resultNode = rootNode.FirstChild("GetServiceLastAccessedDetailsWithEntitiesResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode jobStatusNode = resultNode.FirstChild("JobStatus");
    if(!jobStatusNode.IsNull())
    {
      m_jobStatus = JobStatusTypeMapper::GetJobStatusTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(jobStatusNode.GetText()).c_str()).c_str());
    }
    XmlNode jobCreationDateNode = resultNode.FirstChild("JobCreationDate");
    if(!jobCreationDateNode.IsNull())
    {
      m_jobCreationDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(jobCreationDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
    }
    XmlNode jobCompletionDateNode = resultNode.FirstChild("JobCompletionDate");
    if(!jobCompletionDateNode.IsNull())
    {
      m_jobCompletionDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(jobCompletionDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
    }
    XmlNode entityDetailsListNode = resultNode.FirstChild("EntityDetailsList");
    if(!entityDetailsListNode.IsNull())
    {
      XmlNode entityDetailsListMember = entityDetailsListNode.FirstChild("member");
      while(!entityDetailsListMember.IsNull())
      {
        m_entityDetailsList.push_back(entityDetailsListMember);
        entityDetailsListMember = entityDetailsListMember.NextNode("member");
      }

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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetServiceLastAccessedDetailsWithEntitiesResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
