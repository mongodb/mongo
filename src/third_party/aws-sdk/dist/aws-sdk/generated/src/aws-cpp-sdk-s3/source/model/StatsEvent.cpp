/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/StatsEvent.h>
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

StatsEvent::StatsEvent() : 
    m_detailsHasBeenSet(false)
{
}

StatsEvent::StatsEvent(const XmlNode& xmlNode)
  : StatsEvent()
{
  *this = xmlNode;
}

StatsEvent& StatsEvent::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode detailsNode = resultNode;
    if(!detailsNode.IsNull())
    {
      m_details = detailsNode;
      m_detailsHasBeenSet = true;
    }
  }

  return *this;
}

void StatsEvent::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_detailsHasBeenSet)
  {
   XmlNode detailsNode = parentNode.CreateChildElement("Details");
   m_details.AddToNode(detailsNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
