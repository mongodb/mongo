/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/SessionCredentials.h>
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

SessionCredentials::SessionCredentials() : 
    m_accessKeyIdHasBeenSet(false),
    m_secretAccessKeyHasBeenSet(false),
    m_sessionTokenHasBeenSet(false),
    m_expirationHasBeenSet(false)
{
}

SessionCredentials::SessionCredentials(const XmlNode& xmlNode)
  : SessionCredentials()
{
  *this = xmlNode;
}

SessionCredentials& SessionCredentials::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode accessKeyIdNode = resultNode.FirstChild("AccessKeyId");
    if(!accessKeyIdNode.IsNull())
    {
      m_accessKeyId = Aws::Utils::Xml::DecodeEscapedXmlText(accessKeyIdNode.GetText());
      m_accessKeyIdHasBeenSet = true;
    }
    XmlNode secretAccessKeyNode = resultNode.FirstChild("SecretAccessKey");
    if(!secretAccessKeyNode.IsNull())
    {
      m_secretAccessKey = Aws::Utils::Xml::DecodeEscapedXmlText(secretAccessKeyNode.GetText());
      m_secretAccessKeyHasBeenSet = true;
    }
    XmlNode sessionTokenNode = resultNode.FirstChild("SessionToken");
    if(!sessionTokenNode.IsNull())
    {
      m_sessionToken = Aws::Utils::Xml::DecodeEscapedXmlText(sessionTokenNode.GetText());
      m_sessionTokenHasBeenSet = true;
    }
    XmlNode expirationNode = resultNode.FirstChild("Expiration");
    if(!expirationNode.IsNull())
    {
      m_expiration = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(expirationNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_expirationHasBeenSet = true;
    }
  }

  return *this;
}

void SessionCredentials::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_accessKeyIdHasBeenSet)
  {
   XmlNode accessKeyIdNode = parentNode.CreateChildElement("AccessKeyId");
   accessKeyIdNode.SetText(m_accessKeyId);
  }

  if(m_secretAccessKeyHasBeenSet)
  {
   XmlNode secretAccessKeyNode = parentNode.CreateChildElement("SecretAccessKey");
   secretAccessKeyNode.SetText(m_secretAccessKey);
  }

  if(m_sessionTokenHasBeenSet)
  {
   XmlNode sessionTokenNode = parentNode.CreateChildElement("SessionToken");
   sessionTokenNode.SetText(m_sessionToken);
  }

  if(m_expirationHasBeenSet)
  {
   XmlNode expirationNode = parentNode.CreateChildElement("Expiration");
   expirationNode.SetText(m_expiration.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
