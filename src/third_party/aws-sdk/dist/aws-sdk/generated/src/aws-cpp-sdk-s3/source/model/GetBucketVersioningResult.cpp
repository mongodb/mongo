/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetBucketVersioningResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

GetBucketVersioningResult::GetBucketVersioningResult() : 
    m_status(BucketVersioningStatus::NOT_SET),
    m_mFADelete(MFADeleteStatus::NOT_SET)
{
}

GetBucketVersioningResult::GetBucketVersioningResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : GetBucketVersioningResult()
{
  *this = result;
}

GetBucketVersioningResult& GetBucketVersioningResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode resultNode = xmlDocument.GetRootElement();

  if(!resultNode.IsNull())
  {
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = BucketVersioningStatusMapper::GetBucketVersioningStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
    }
    XmlNode mFADeleteNode = resultNode.FirstChild("MfaDelete");
    if(!mFADeleteNode.IsNull())
    {
      m_mFADelete = MFADeleteStatusMapper::GetMFADeleteStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(mFADeleteNode.GetText()).c_str()).c_str());
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
