/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ListObjectsV2Result.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

ListObjectsV2Result::ListObjectsV2Result() : 
    m_isTruncated(false),
    m_maxKeys(0),
    m_encodingType(EncodingType::NOT_SET),
    m_keyCount(0),
    m_requestCharged(RequestCharged::NOT_SET)
{
}

ListObjectsV2Result::ListObjectsV2Result(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListObjectsV2Result()
{
  *this = result;
}

ListObjectsV2Result& ListObjectsV2Result::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
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
    XmlNode contentsNode = resultNode.FirstChild("Contents");
    if(!contentsNode.IsNull())
    {
      XmlNode contentsMember = contentsNode;
      while(!contentsMember.IsNull())
      {
        m_contents.push_back(contentsMember);
        contentsMember = contentsMember.NextNode("Contents");
      }

    }
    XmlNode nameNode = resultNode.FirstChild("Name");
    if(!nameNode.IsNull())
    {
      m_name = Aws::Utils::Xml::DecodeEscapedXmlText(nameNode.GetText());
    }
    XmlNode prefixNode = resultNode.FirstChild("Prefix");
    if(!prefixNode.IsNull())
    {
      m_prefix = Aws::Utils::Xml::DecodeEscapedXmlText(prefixNode.GetText());
    }
    XmlNode delimiterNode = resultNode.FirstChild("Delimiter");
    if(!delimiterNode.IsNull())
    {
      m_delimiter = Aws::Utils::Xml::DecodeEscapedXmlText(delimiterNode.GetText());
    }
    XmlNode maxKeysNode = resultNode.FirstChild("MaxKeys");
    if(!maxKeysNode.IsNull())
    {
      m_maxKeys = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(maxKeysNode.GetText()).c_str()).c_str());
    }
    XmlNode commonPrefixesNode = resultNode.FirstChild("CommonPrefixes");
    if(!commonPrefixesNode.IsNull())
    {
      XmlNode commonPrefixesMember = commonPrefixesNode;
      while(!commonPrefixesMember.IsNull())
      {
        m_commonPrefixes.push_back(commonPrefixesMember);
        commonPrefixesMember = commonPrefixesMember.NextNode("CommonPrefixes");
      }

    }
    XmlNode encodingTypeNode = resultNode.FirstChild("EncodingType");
    if(!encodingTypeNode.IsNull())
    {
      m_encodingType = EncodingTypeMapper::GetEncodingTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(encodingTypeNode.GetText()).c_str()).c_str());
    }
    XmlNode keyCountNode = resultNode.FirstChild("KeyCount");
    if(!keyCountNode.IsNull())
    {
      m_keyCount = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(keyCountNode.GetText()).c_str()).c_str());
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
    XmlNode startAfterNode = resultNode.FirstChild("StartAfter");
    if(!startAfterNode.IsNull())
    {
      m_startAfter = Aws::Utils::Xml::DecodeEscapedXmlText(startAfterNode.GetText());
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
