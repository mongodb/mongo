/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/TargetGrant.h>
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

TargetGrant::TargetGrant() : 
    m_granteeHasBeenSet(false),
    m_permission(BucketLogsPermission::NOT_SET),
    m_permissionHasBeenSet(false)
{
}

TargetGrant::TargetGrant(const XmlNode& xmlNode)
  : TargetGrant()
{
  *this = xmlNode;
}

TargetGrant& TargetGrant::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode granteeNode = resultNode.FirstChild("Grantee");
    if(!granteeNode.IsNull())
    {
      m_grantee = granteeNode;
      m_granteeHasBeenSet = true;
    }
    XmlNode permissionNode = resultNode.FirstChild("Permission");
    if(!permissionNode.IsNull())
    {
      m_permission = BucketLogsPermissionMapper::GetBucketLogsPermissionForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(permissionNode.GetText()).c_str()).c_str());
      m_permissionHasBeenSet = true;
    }
  }

  return *this;
}

void TargetGrant::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_granteeHasBeenSet)
  {
   XmlNode granteeNode = parentNode.CreateChildElement("Grantee");
   m_grantee.AddToNode(granteeNode);
  }

  if(m_permissionHasBeenSet)
  {
   XmlNode permissionNode = parentNode.CreateChildElement("Permission");
   permissionNode.SetText(BucketLogsPermissionMapper::GetNameForBucketLogsPermission(m_permission));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
