/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/AttachedPolicy.h>
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

AttachedPolicy::AttachedPolicy() : 
    m_policyNameHasBeenSet(false),
    m_policyArnHasBeenSet(false)
{
}

AttachedPolicy::AttachedPolicy(const XmlNode& xmlNode)
  : AttachedPolicy()
{
  *this = xmlNode;
}

AttachedPolicy& AttachedPolicy::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode policyNameNode = resultNode.FirstChild("PolicyName");
    if(!policyNameNode.IsNull())
    {
      m_policyName = Aws::Utils::Xml::DecodeEscapedXmlText(policyNameNode.GetText());
      m_policyNameHasBeenSet = true;
    }
    XmlNode policyArnNode = resultNode.FirstChild("PolicyArn");
    if(!policyArnNode.IsNull())
    {
      m_policyArn = Aws::Utils::Xml::DecodeEscapedXmlText(policyArnNode.GetText());
      m_policyArnHasBeenSet = true;
    }
  }

  return *this;
}

void AttachedPolicy::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_policyNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".PolicyName=" << StringUtils::URLEncode(m_policyName.c_str()) << "&";
  }

  if(m_policyArnHasBeenSet)
  {
      oStream << location << index << locationValue << ".PolicyArn=" << StringUtils::URLEncode(m_policyArn.c_str()) << "&";
  }

}

void AttachedPolicy::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_policyNameHasBeenSet)
  {
      oStream << location << ".PolicyName=" << StringUtils::URLEncode(m_policyName.c_str()) << "&";
  }
  if(m_policyArnHasBeenSet)
  {
      oStream << location << ".PolicyArn=" << StringUtils::URLEncode(m_policyArn.c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
