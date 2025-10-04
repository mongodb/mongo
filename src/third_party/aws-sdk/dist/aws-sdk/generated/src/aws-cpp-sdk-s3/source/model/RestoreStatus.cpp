/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/RestoreStatus.h>
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

RestoreStatus::RestoreStatus() : 
    m_isRestoreInProgress(false),
    m_isRestoreInProgressHasBeenSet(false),
    m_restoreExpiryDateHasBeenSet(false)
{
}

RestoreStatus::RestoreStatus(const XmlNode& xmlNode)
  : RestoreStatus()
{
  *this = xmlNode;
}

RestoreStatus& RestoreStatus::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode isRestoreInProgressNode = resultNode.FirstChild("IsRestoreInProgress");
    if(!isRestoreInProgressNode.IsNull())
    {
      m_isRestoreInProgress = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(isRestoreInProgressNode.GetText()).c_str()).c_str());
      m_isRestoreInProgressHasBeenSet = true;
    }
    XmlNode restoreExpiryDateNode = resultNode.FirstChild("RestoreExpiryDate");
    if(!restoreExpiryDateNode.IsNull())
    {
      m_restoreExpiryDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(restoreExpiryDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_restoreExpiryDateHasBeenSet = true;
    }
  }

  return *this;
}

void RestoreStatus::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_isRestoreInProgressHasBeenSet)
  {
   XmlNode isRestoreInProgressNode = parentNode.CreateChildElement("IsRestoreInProgress");
   ss << std::boolalpha << m_isRestoreInProgress;
   isRestoreInProgressNode.SetText(ss.str());
   ss.str("");
  }

  if(m_restoreExpiryDateHasBeenSet)
  {
   XmlNode restoreExpiryDateNode = parentNode.CreateChildElement("RestoreExpiryDate");
   restoreExpiryDateNode.SetText(m_restoreExpiryDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
