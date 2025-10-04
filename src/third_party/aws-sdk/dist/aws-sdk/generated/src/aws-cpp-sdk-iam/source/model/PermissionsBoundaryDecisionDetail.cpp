/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PermissionsBoundaryDecisionDetail.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace IAM
{
namespace Model
{

PermissionsBoundaryDecisionDetail::PermissionsBoundaryDecisionDetail() : 
    m_allowedByPermissionsBoundary(false),
    m_allowedByPermissionsBoundaryHasBeenSet(false)
{
}

PermissionsBoundaryDecisionDetail::PermissionsBoundaryDecisionDetail(const XmlNode& xmlNode)
  : PermissionsBoundaryDecisionDetail()
{
  *this = xmlNode;
}

PermissionsBoundaryDecisionDetail& PermissionsBoundaryDecisionDetail::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode allowedByPermissionsBoundaryNode = resultNode.FirstChild("AllowedByPermissionsBoundary");
    if(!allowedByPermissionsBoundaryNode.IsNull())
    {
      m_allowedByPermissionsBoundary = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(allowedByPermissionsBoundaryNode.GetText()).c_str()).c_str());
      m_allowedByPermissionsBoundaryHasBeenSet = true;
    }
  }

  return *this;
}

void PermissionsBoundaryDecisionDetail::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_allowedByPermissionsBoundaryHasBeenSet)
  {
      oStream << location << index << locationValue << ".AllowedByPermissionsBoundary=" << std::boolalpha << m_allowedByPermissionsBoundary << "&";
  }

}

void PermissionsBoundaryDecisionDetail::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_allowedByPermissionsBoundaryHasBeenSet)
  {
      oStream << location << ".AllowedByPermissionsBoundary=" << std::boolalpha << m_allowedByPermissionsBoundary << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
