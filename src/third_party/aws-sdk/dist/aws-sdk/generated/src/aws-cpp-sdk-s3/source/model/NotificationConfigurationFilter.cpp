/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/NotificationConfigurationFilter.h>
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

NotificationConfigurationFilter::NotificationConfigurationFilter() : 
    m_keyHasBeenSet(false)
{
}

NotificationConfigurationFilter::NotificationConfigurationFilter(const XmlNode& xmlNode)
  : NotificationConfigurationFilter()
{
  *this = xmlNode;
}

NotificationConfigurationFilter& NotificationConfigurationFilter::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode keyNode = resultNode.FirstChild("S3Key");
    if(!keyNode.IsNull())
    {
      m_key = keyNode;
      m_keyHasBeenSet = true;
    }
  }

  return *this;
}

void NotificationConfigurationFilter::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_keyHasBeenSet)
  {
   XmlNode keyNode = parentNode.CreateChildElement("S3Key");
   m_key.AddToNode(keyNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
