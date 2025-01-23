/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetBucketMetadataTableConfigurationResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace S3
{
namespace Model
{

GetBucketMetadataTableConfigurationResult::GetBucketMetadataTableConfigurationResult() : 
    m_metadataTableConfigurationResultHasBeenSet(false),
    m_statusHasBeenSet(false),
    m_errorHasBeenSet(false)
{
}

GetBucketMetadataTableConfigurationResult::GetBucketMetadataTableConfigurationResult(const XmlNode& xmlNode)
  : GetBucketMetadataTableConfigurationResult()
{
  *this = xmlNode;
}

GetBucketMetadataTableConfigurationResult& GetBucketMetadataTableConfigurationResult::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode metadataTableConfigurationResultNode = resultNode.FirstChild("MetadataTableConfigurationResult");
    if(!metadataTableConfigurationResultNode.IsNull())
    {
      m_metadataTableConfigurationResult = metadataTableConfigurationResultNode;
      m_metadataTableConfigurationResultHasBeenSet = true;
    }
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText());
      m_statusHasBeenSet = true;
    }
    XmlNode errorNode = resultNode.FirstChild("Error");
    if(!errorNode.IsNull())
    {
      m_error = errorNode;
      m_errorHasBeenSet = true;
    }
  }

  return *this;
}

void GetBucketMetadataTableConfigurationResult::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_metadataTableConfigurationResultHasBeenSet)
  {
   XmlNode metadataTableConfigurationResultNode = parentNode.CreateChildElement("MetadataTableConfigurationResult");
   m_metadataTableConfigurationResult.AddToNode(metadataTableConfigurationResultNode);
  }

  if(m_statusHasBeenSet)
  {
   XmlNode statusNode = parentNode.CreateChildElement("Status");
   statusNode.SetText(m_status);
  }

  if(m_errorHasBeenSet)
  {
   XmlNode errorNode = parentNode.CreateChildElement("Error");
   m_error.AddToNode(errorNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
