/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/SseKmsEncryptedObjects.h>
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

SseKmsEncryptedObjects::SseKmsEncryptedObjects() : 
    m_status(SseKmsEncryptedObjectsStatus::NOT_SET),
    m_statusHasBeenSet(false)
{
}

SseKmsEncryptedObjects::SseKmsEncryptedObjects(const XmlNode& xmlNode)
  : SseKmsEncryptedObjects()
{
  *this = xmlNode;
}

SseKmsEncryptedObjects& SseKmsEncryptedObjects::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = SseKmsEncryptedObjectsStatusMapper::GetSseKmsEncryptedObjectsStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
      m_statusHasBeenSet = true;
    }
  }

  return *this;
}

void SseKmsEncryptedObjects::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_statusHasBeenSet)
  {
   XmlNode statusNode = parentNode.CreateChildElement("Status");
   statusNode.SetText(SseKmsEncryptedObjectsStatusMapper::GetNameForSseKmsEncryptedObjectsStatus(m_status));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
