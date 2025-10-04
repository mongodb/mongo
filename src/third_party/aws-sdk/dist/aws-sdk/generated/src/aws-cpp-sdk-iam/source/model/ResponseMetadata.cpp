/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ResponseMetadata.h>
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

ResponseMetadata::ResponseMetadata() : 
    m_requestIdHasBeenSet(false)
{
}

ResponseMetadata::ResponseMetadata(const XmlNode& xmlNode)
  : ResponseMetadata()
{
  *this = xmlNode;
}

ResponseMetadata& ResponseMetadata::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode requestIdNode = resultNode.FirstChild("RequestId");
    if(!requestIdNode.IsNull())
    {
      m_requestId = Aws::Utils::Xml::DecodeEscapedXmlText(requestIdNode.GetText());
      m_requestIdHasBeenSet = true;
    }
  }

  return *this;
}

void ResponseMetadata::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_requestIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".RequestId=" << StringUtils::URLEncode(m_requestId.c_str()) << "&";
  }

}

void ResponseMetadata::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_requestIdHasBeenSet)
  {
      oStream << location << ".RequestId=" << StringUtils::URLEncode(m_requestId.c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
