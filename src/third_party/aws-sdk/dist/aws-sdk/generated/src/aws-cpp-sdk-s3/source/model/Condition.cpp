/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Condition.h>
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

Condition::Condition() : 
    m_httpErrorCodeReturnedEqualsHasBeenSet(false),
    m_keyPrefixEqualsHasBeenSet(false)
{
}

Condition::Condition(const XmlNode& xmlNode)
  : Condition()
{
  *this = xmlNode;
}

Condition& Condition::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode httpErrorCodeReturnedEqualsNode = resultNode.FirstChild("HttpErrorCodeReturnedEquals");
    if(!httpErrorCodeReturnedEqualsNode.IsNull())
    {
      m_httpErrorCodeReturnedEquals = Aws::Utils::Xml::DecodeEscapedXmlText(httpErrorCodeReturnedEqualsNode.GetText());
      m_httpErrorCodeReturnedEqualsHasBeenSet = true;
    }
    XmlNode keyPrefixEqualsNode = resultNode.FirstChild("KeyPrefixEquals");
    if(!keyPrefixEqualsNode.IsNull())
    {
      m_keyPrefixEquals = Aws::Utils::Xml::DecodeEscapedXmlText(keyPrefixEqualsNode.GetText());
      m_keyPrefixEqualsHasBeenSet = true;
    }
  }

  return *this;
}

void Condition::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_httpErrorCodeReturnedEqualsHasBeenSet)
  {
   XmlNode httpErrorCodeReturnedEqualsNode = parentNode.CreateChildElement("HttpErrorCodeReturnedEquals");
   httpErrorCodeReturnedEqualsNode.SetText(m_httpErrorCodeReturnedEquals);
  }

  if(m_keyPrefixEqualsHasBeenSet)
  {
   XmlNode keyPrefixEqualsNode = parentNode.CreateChildElement("KeyPrefixEquals");
   keyPrefixEqualsNode.SetText(m_keyPrefixEquals);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
