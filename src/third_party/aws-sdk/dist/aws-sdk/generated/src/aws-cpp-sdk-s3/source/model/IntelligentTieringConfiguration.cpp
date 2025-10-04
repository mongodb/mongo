/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/IntelligentTieringConfiguration.h>
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

IntelligentTieringConfiguration::IntelligentTieringConfiguration() : 
    m_idHasBeenSet(false),
    m_filterHasBeenSet(false),
    m_status(IntelligentTieringStatus::NOT_SET),
    m_statusHasBeenSet(false),
    m_tieringsHasBeenSet(false)
{
}

IntelligentTieringConfiguration::IntelligentTieringConfiguration(const XmlNode& xmlNode)
  : IntelligentTieringConfiguration()
{
  *this = xmlNode;
}

IntelligentTieringConfiguration& IntelligentTieringConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode idNode = resultNode.FirstChild("Id");
    if(!idNode.IsNull())
    {
      m_id = Aws::Utils::Xml::DecodeEscapedXmlText(idNode.GetText());
      m_idHasBeenSet = true;
    }
    XmlNode filterNode = resultNode.FirstChild("Filter");
    if(!filterNode.IsNull())
    {
      m_filter = filterNode;
      m_filterHasBeenSet = true;
    }
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = IntelligentTieringStatusMapper::GetIntelligentTieringStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
      m_statusHasBeenSet = true;
    }
    XmlNode tieringsNode = resultNode.FirstChild("Tiering");
    if(!tieringsNode.IsNull())
    {
      XmlNode tieringMember = tieringsNode;
      while(!tieringMember.IsNull())
      {
        m_tierings.push_back(tieringMember);
        tieringMember = tieringMember.NextNode("Tiering");
      }

      m_tieringsHasBeenSet = true;
    }
  }

  return *this;
}

void IntelligentTieringConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_idHasBeenSet)
  {
   XmlNode idNode = parentNode.CreateChildElement("Id");
   idNode.SetText(m_id);
  }

  if(m_filterHasBeenSet)
  {
   XmlNode filterNode = parentNode.CreateChildElement("Filter");
   m_filter.AddToNode(filterNode);
  }

  if(m_statusHasBeenSet)
  {
   XmlNode statusNode = parentNode.CreateChildElement("Status");
   statusNode.SetText(IntelligentTieringStatusMapper::GetNameForIntelligentTieringStatus(m_status));
  }

  if(m_tieringsHasBeenSet)
  {
   for(const auto& item : m_tierings)
   {
     XmlNode tieringsNode = parentNode.CreateChildElement("Tiering");
     item.AddToNode(tieringsNode);
   }
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
