/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/OrganizationsDecisionDetail.h>
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

OrganizationsDecisionDetail::OrganizationsDecisionDetail() : 
    m_allowedByOrganizations(false),
    m_allowedByOrganizationsHasBeenSet(false)
{
}

OrganizationsDecisionDetail::OrganizationsDecisionDetail(const XmlNode& xmlNode)
  : OrganizationsDecisionDetail()
{
  *this = xmlNode;
}

OrganizationsDecisionDetail& OrganizationsDecisionDetail::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode allowedByOrganizationsNode = resultNode.FirstChild("AllowedByOrganizations");
    if(!allowedByOrganizationsNode.IsNull())
    {
      m_allowedByOrganizations = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(allowedByOrganizationsNode.GetText()).c_str()).c_str());
      m_allowedByOrganizationsHasBeenSet = true;
    }
  }

  return *this;
}

void OrganizationsDecisionDetail::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_allowedByOrganizationsHasBeenSet)
  {
      oStream << location << index << locationValue << ".AllowedByOrganizations=" << std::boolalpha << m_allowedByOrganizations << "&";
  }

}

void OrganizationsDecisionDetail::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_allowedByOrganizationsHasBeenSet)
  {
      oStream << location << ".AllowedByOrganizations=" << std::boolalpha << m_allowedByOrganizations << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
