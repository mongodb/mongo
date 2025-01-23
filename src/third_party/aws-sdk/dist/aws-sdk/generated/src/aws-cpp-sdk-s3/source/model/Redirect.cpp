/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Redirect.h>
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

Redirect::Redirect() : 
    m_hostNameHasBeenSet(false),
    m_httpRedirectCodeHasBeenSet(false),
    m_protocol(Protocol::NOT_SET),
    m_protocolHasBeenSet(false),
    m_replaceKeyPrefixWithHasBeenSet(false),
    m_replaceKeyWithHasBeenSet(false)
{
}

Redirect::Redirect(const XmlNode& xmlNode)
  : Redirect()
{
  *this = xmlNode;
}

Redirect& Redirect::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode hostNameNode = resultNode.FirstChild("HostName");
    if(!hostNameNode.IsNull())
    {
      m_hostName = Aws::Utils::Xml::DecodeEscapedXmlText(hostNameNode.GetText());
      m_hostNameHasBeenSet = true;
    }
    XmlNode httpRedirectCodeNode = resultNode.FirstChild("HttpRedirectCode");
    if(!httpRedirectCodeNode.IsNull())
    {
      m_httpRedirectCode = Aws::Utils::Xml::DecodeEscapedXmlText(httpRedirectCodeNode.GetText());
      m_httpRedirectCodeHasBeenSet = true;
    }
    XmlNode protocolNode = resultNode.FirstChild("Protocol");
    if(!protocolNode.IsNull())
    {
      m_protocol = ProtocolMapper::GetProtocolForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(protocolNode.GetText()).c_str()).c_str());
      m_protocolHasBeenSet = true;
    }
    XmlNode replaceKeyPrefixWithNode = resultNode.FirstChild("ReplaceKeyPrefixWith");
    if(!replaceKeyPrefixWithNode.IsNull())
    {
      m_replaceKeyPrefixWith = Aws::Utils::Xml::DecodeEscapedXmlText(replaceKeyPrefixWithNode.GetText());
      m_replaceKeyPrefixWithHasBeenSet = true;
    }
    XmlNode replaceKeyWithNode = resultNode.FirstChild("ReplaceKeyWith");
    if(!replaceKeyWithNode.IsNull())
    {
      m_replaceKeyWith = Aws::Utils::Xml::DecodeEscapedXmlText(replaceKeyWithNode.GetText());
      m_replaceKeyWithHasBeenSet = true;
    }
  }

  return *this;
}

void Redirect::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_hostNameHasBeenSet)
  {
   XmlNode hostNameNode = parentNode.CreateChildElement("HostName");
   hostNameNode.SetText(m_hostName);
  }

  if(m_httpRedirectCodeHasBeenSet)
  {
   XmlNode httpRedirectCodeNode = parentNode.CreateChildElement("HttpRedirectCode");
   httpRedirectCodeNode.SetText(m_httpRedirectCode);
  }

  if(m_protocolHasBeenSet)
  {
   XmlNode protocolNode = parentNode.CreateChildElement("Protocol");
   protocolNode.SetText(ProtocolMapper::GetNameForProtocol(m_protocol));
  }

  if(m_replaceKeyPrefixWithHasBeenSet)
  {
   XmlNode replaceKeyPrefixWithNode = parentNode.CreateChildElement("ReplaceKeyPrefixWith");
   replaceKeyPrefixWithNode.SetText(m_replaceKeyPrefixWith);
  }

  if(m_replaceKeyWithHasBeenSet)
  {
   XmlNode replaceKeyWithNode = parentNode.CreateChildElement("ReplaceKeyWith");
   replaceKeyWithNode.SetText(m_replaceKeyWith);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
