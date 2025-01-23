/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/MetricsConfiguration.h>
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

MetricsConfiguration::MetricsConfiguration() : 
    m_idHasBeenSet(false),
    m_filterHasBeenSet(false)
{
}

MetricsConfiguration::MetricsConfiguration(const XmlNode& xmlNode)
  : MetricsConfiguration()
{
  *this = xmlNode;
}

MetricsConfiguration& MetricsConfiguration::operator =(const XmlNode& xmlNode)
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
  }

  return *this;
}

void MetricsConfiguration::AddToNode(XmlNode& parentNode) const
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

}

} // namespace Model
} // namespace S3
} // namespace Aws
