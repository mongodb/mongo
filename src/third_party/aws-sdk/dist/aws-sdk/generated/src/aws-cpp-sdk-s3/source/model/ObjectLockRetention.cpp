/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectLockRetention.h>
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

ObjectLockRetention::ObjectLockRetention() : 
    m_mode(ObjectLockRetentionMode::NOT_SET),
    m_modeHasBeenSet(false),
    m_retainUntilDateHasBeenSet(false)
{
}

ObjectLockRetention::ObjectLockRetention(const XmlNode& xmlNode)
  : ObjectLockRetention()
{
  *this = xmlNode;
}

ObjectLockRetention& ObjectLockRetention::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode modeNode = resultNode.FirstChild("Mode");
    if(!modeNode.IsNull())
    {
      m_mode = ObjectLockRetentionModeMapper::GetObjectLockRetentionModeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(modeNode.GetText()).c_str()).c_str());
      m_modeHasBeenSet = true;
    }
    XmlNode retainUntilDateNode = resultNode.FirstChild("RetainUntilDate");
    if(!retainUntilDateNode.IsNull())
    {
      m_retainUntilDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(retainUntilDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_retainUntilDateHasBeenSet = true;
    }
  }

  return *this;
}

void ObjectLockRetention::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_modeHasBeenSet)
  {
   XmlNode modeNode = parentNode.CreateChildElement("Mode");
   modeNode.SetText(ObjectLockRetentionModeMapper::GetNameForObjectLockRetentionMode(m_mode));
  }

  if(m_retainUntilDateHasBeenSet)
  {
   XmlNode retainUntilDateNode = parentNode.CreateChildElement("RetainUntilDate");
   retainUntilDateNode.SetText(m_retainUntilDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
