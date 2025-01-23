/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetBucketTaggingResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

GetBucketTaggingResult::GetBucketTaggingResult()
{
}

GetBucketTaggingResult::GetBucketTaggingResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

GetBucketTaggingResult& GetBucketTaggingResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode resultNode = xmlDocument.GetRootElement();

  if(!resultNode.IsNull())
  {
    XmlNode tagSetNode = resultNode.FirstChild("TagSet");
    if(!tagSetNode.IsNull())
    {
      XmlNode tagSetMember = tagSetNode.FirstChild("Tag");
      while(!tagSetMember.IsNull())
      {
        m_tagSet.push_back(tagSetMember);
        tagSetMember = tagSetMember.NextNode("Tag");
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
