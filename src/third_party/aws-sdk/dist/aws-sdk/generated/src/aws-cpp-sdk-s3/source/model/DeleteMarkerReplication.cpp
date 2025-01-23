/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/DeleteMarkerReplication.h>
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

DeleteMarkerReplication::DeleteMarkerReplication() : 
    m_status(DeleteMarkerReplicationStatus::NOT_SET),
    m_statusHasBeenSet(false)
{
}

DeleteMarkerReplication::DeleteMarkerReplication(const XmlNode& xmlNode)
  : DeleteMarkerReplication()
{
  *this = xmlNode;
}

DeleteMarkerReplication& DeleteMarkerReplication::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = DeleteMarkerReplicationStatusMapper::GetDeleteMarkerReplicationStatusForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
      m_statusHasBeenSet = true;
    }
  }

  return *this;
}

void DeleteMarkerReplication::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_statusHasBeenSet)
  {
   XmlNode statusNode = parentNode.CreateChildElement("Status");
   statusNode.SetText(DeleteMarkerReplicationStatusMapper::GetNameForDeleteMarkerReplicationStatus(m_status));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
