/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ListObjectVersionsResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

ListObjectVersionsResult::ListObjectVersionsResult() : 
    m_isTruncated(false),
    m_maxKeys(0),
    m_encodingType(EncodingType::NOT_SET),
    m_requestCharged(RequestCharged::NOT_SET)
{
}

ListObjectVersionsResult::ListObjectVersionsResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListObjectVersionsResult()
{
  *this = result;
}

ListObjectVersionsResult& ListObjectVersionsResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
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
    XmlNode keyMarkerNode = resultNode.FirstChild("KeyMarker");
    if(!keyMarkerNode.IsNull())
    {
      m_keyMarker = Aws::Utils::Xml::DecodeEscapedXmlText(keyMarkerNode.GetText());
    }
    XmlNode versionIdMarkerNode = resultNode.FirstChild("VersionIdMarker");
    if(!versionIdMarkerNode.IsNull())
    {
      m_versionIdMarker = Aws::Utils::Xml::DecodeEscapedXmlText(versionIdMarkerNode.GetText());
    }
    XmlNode nextKeyMarkerNode = resultNode.FirstChild("NextKeyMarker");
    if(!nextKeyMarkerNode.IsNull())
    {
      m_nextKeyMarker = Aws::Utils::Xml::DecodeEscapedXmlText(nextKeyMarkerNode.GetText());
    }
    XmlNode nextVersionIdMarkerNode = resultNode.FirstChild("NextVersionIdMarker");
    if(!nextVersionIdMarkerNode.IsNull())
    {
      m_nextVersionIdMarker = Aws::Utils::Xml::DecodeEscapedXmlText(nextVersionIdMarkerNode.GetText());
    }
    XmlNode versionsNode = resultNode.FirstChild("Version");
    if(!versionsNode.IsNull())
    {
      XmlNode versionMember = versionsNode;
      while(!versionMember.IsNull())
      {
        m_versions.push_back(versionMember);
        versionMember = versionMember.NextNode("Version");
      }

    }
    XmlNode deleteMarkersNode = resultNode.FirstChild("DeleteMarker");
    if(!deleteMarkersNode.IsNull())
    {
      XmlNode deleteMarkerMember = deleteMarkersNode;
      while(!deleteMarkerMember.IsNull())
      {
        m_deleteMarkers.push_back(deleteMarkerMember);
        deleteMarkerMember = deleteMarkerMember.NextNode("DeleteMarker");
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
