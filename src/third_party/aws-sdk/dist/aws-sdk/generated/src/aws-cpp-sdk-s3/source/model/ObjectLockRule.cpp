/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectLockRule.h>
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

ObjectLockRule::ObjectLockRule() : 
    m_defaultRetentionHasBeenSet(false)
{
}

ObjectLockRule::ObjectLockRule(const XmlNode& xmlNode)
  : ObjectLockRule()
{
  *this = xmlNode;
}

ObjectLockRule& ObjectLockRule::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode defaultRetentionNode = resultNode.FirstChild("DefaultRetention");
    if(!defaultRetentionNode.IsNull())
    {
      m_defaultRetention = defaultRetentionNode;
      m_defaultRetentionHasBeenSet = true;
    }
  }

  return *this;
}

void ObjectLockRule::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_defaultRetentionHasBeenSet)
  {
   XmlNode defaultRetentionNode = parentNode.CreateChildElement("DefaultRetention");
   m_defaultRetention.AddToNode(defaultRetentionNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
