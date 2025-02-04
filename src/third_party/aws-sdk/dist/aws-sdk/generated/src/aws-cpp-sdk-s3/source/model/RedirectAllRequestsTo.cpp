/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/RedirectAllRequestsTo.h>
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

RedirectAllRequestsTo::RedirectAllRequestsTo() : 
    m_hostNameHasBeenSet(false),
    m_protocol(Protocol::NOT_SET),
    m_protocolHasBeenSet(false)
{
}

RedirectAllRequestsTo::RedirectAllRequestsTo(const XmlNode& xmlNode)
  : RedirectAllRequestsTo()
{
  *this = xmlNode;
}

RedirectAllRequestsTo& RedirectAllRequestsTo::operator =(const XmlNode& xmlNode)
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
    XmlNode protocolNode = resultNode.FirstChild("Protocol");
    if(!protocolNode.IsNull())
    {
      m_protocol = ProtocolMapper::GetProtocolForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(protocolNode.GetText()).c_str()).c_str());
      m_protocolHasBeenSet = true;
    }
  }

  return *this;
}

void RedirectAllRequestsTo::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_hostNameHasBeenSet)
  {
   XmlNode hostNameNode = parentNode.CreateChildElement("HostName");
   hostNameNode.SetText(m_hostName);
  }

  if(m_protocolHasBeenSet)
  {
   XmlNode protocolNode = parentNode.CreateChildElement("Protocol");
   protocolNode.SetText(ProtocolMapper::GetNameForProtocol(m_protocol));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
