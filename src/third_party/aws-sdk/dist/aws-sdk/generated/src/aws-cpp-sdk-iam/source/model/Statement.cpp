/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/Statement.h>
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

Statement::Statement() : 
    m_sourcePolicyIdHasBeenSet(false),
    m_sourcePolicyType(PolicySourceType::NOT_SET),
    m_sourcePolicyTypeHasBeenSet(false),
    m_startPositionHasBeenSet(false),
    m_endPositionHasBeenSet(false)
{
}

Statement::Statement(const XmlNode& xmlNode)
  : Statement()
{
  *this = xmlNode;
}

Statement& Statement::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode sourcePolicyIdNode = resultNode.FirstChild("SourcePolicyId");
    if(!sourcePolicyIdNode.IsNull())
    {
      m_sourcePolicyId = Aws::Utils::Xml::DecodeEscapedXmlText(sourcePolicyIdNode.GetText());
      m_sourcePolicyIdHasBeenSet = true;
    }
    XmlNode sourcePolicyTypeNode = resultNode.FirstChild("SourcePolicyType");
    if(!sourcePolicyTypeNode.IsNull())
    {
      m_sourcePolicyType = PolicySourceTypeMapper::GetPolicySourceTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(sourcePolicyTypeNode.GetText()).c_str()).c_str());
      m_sourcePolicyTypeHasBeenSet = true;
    }
    XmlNode startPositionNode = resultNode.FirstChild("StartPosition");
    if(!startPositionNode.IsNull())
    {
      m_startPosition = startPositionNode;
      m_startPositionHasBeenSet = true;
    }
    XmlNode endPositionNode = resultNode.FirstChild("EndPosition");
    if(!endPositionNode.IsNull())
    {
      m_endPosition = endPositionNode;
      m_endPositionHasBeenSet = true;
    }
  }

  return *this;
}

void Statement::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_sourcePolicyIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".SourcePolicyId=" << StringUtils::URLEncode(m_sourcePolicyId.c_str()) << "&";
  }

  if(m_sourcePolicyTypeHasBeenSet)
  {
      oStream << location << index << locationValue << ".SourcePolicyType=" << PolicySourceTypeMapper::GetNameForPolicySourceType(m_sourcePolicyType) << "&";
  }

  if(m_startPositionHasBeenSet)
  {
      Aws::StringStream startPositionLocationAndMemberSs;
      startPositionLocationAndMemberSs << location << index << locationValue << ".StartPosition";
      m_startPosition.OutputToStream(oStream, startPositionLocationAndMemberSs.str().c_str());
  }

  if(m_endPositionHasBeenSet)
  {
      Aws::StringStream endPositionLocationAndMemberSs;
      endPositionLocationAndMemberSs << location << index << locationValue << ".EndPosition";
      m_endPosition.OutputToStream(oStream, endPositionLocationAndMemberSs.str().c_str());
  }

}

void Statement::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_sourcePolicyIdHasBeenSet)
  {
      oStream << location << ".SourcePolicyId=" << StringUtils::URLEncode(m_sourcePolicyId.c_str()) << "&";
  }
  if(m_sourcePolicyTypeHasBeenSet)
  {
      oStream << location << ".SourcePolicyType=" << PolicySourceTypeMapper::GetNameForPolicySourceType(m_sourcePolicyType) << "&";
  }
  if(m_startPositionHasBeenSet)
  {
      Aws::String startPositionLocationAndMember(location);
      startPositionLocationAndMember += ".StartPosition";
      m_startPosition.OutputToStream(oStream, startPositionLocationAndMember.c_str());
  }
  if(m_endPositionHasBeenSet)
  {
      Aws::String endPositionLocationAndMember(location);
      endPositionLocationAndMember += ".EndPosition";
      m_endPosition.OutputToStream(oStream, endPositionLocationAndMember.c_str());
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
