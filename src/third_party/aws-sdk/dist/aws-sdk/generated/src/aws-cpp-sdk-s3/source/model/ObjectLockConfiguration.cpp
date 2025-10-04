/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectLockConfiguration.h>
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

ObjectLockConfiguration::ObjectLockConfiguration() : 
    m_objectLockEnabled(ObjectLockEnabled::NOT_SET),
    m_objectLockEnabledHasBeenSet(false),
    m_ruleHasBeenSet(false)
{
}

ObjectLockConfiguration::ObjectLockConfiguration(const XmlNode& xmlNode)
  : ObjectLockConfiguration()
{
  *this = xmlNode;
}

ObjectLockConfiguration& ObjectLockConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode objectLockEnabledNode = resultNode.FirstChild("ObjectLockEnabled");
    if(!objectLockEnabledNode.IsNull())
    {
      m_objectLockEnabled = ObjectLockEnabledMapper::GetObjectLockEnabledForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(objectLockEnabledNode.GetText()).c_str()).c_str());
      m_objectLockEnabledHasBeenSet = true;
    }
    XmlNode ruleNode = resultNode.FirstChild("Rule");
    if(!ruleNode.IsNull())
    {
      m_rule = ruleNode;
      m_ruleHasBeenSet = true;
    }
  }

  return *this;
}

void ObjectLockConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_objectLockEnabledHasBeenSet)
  {
   XmlNode objectLockEnabledNode = parentNode.CreateChildElement("ObjectLockEnabled");
   objectLockEnabledNode.SetText(ObjectLockEnabledMapper::GetNameForObjectLockEnabled(m_objectLockEnabled));
  }

  if(m_ruleHasBeenSet)
  {
   XmlNode ruleNode = parentNode.CreateChildElement("Rule");
   m_rule.AddToNode(ruleNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
