/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeletionTaskFailureReasonType.h>
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

DeletionTaskFailureReasonType::DeletionTaskFailureReasonType() : 
    m_reasonHasBeenSet(false),
    m_roleUsageListHasBeenSet(false)
{
}

DeletionTaskFailureReasonType::DeletionTaskFailureReasonType(const XmlNode& xmlNode)
  : DeletionTaskFailureReasonType()
{
  *this = xmlNode;
}

DeletionTaskFailureReasonType& DeletionTaskFailureReasonType::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode reasonNode = resultNode.FirstChild("Reason");
    if(!reasonNode.IsNull())
    {
      m_reason = Aws::Utils::Xml::DecodeEscapedXmlText(reasonNode.GetText());
      m_reasonHasBeenSet = true;
    }
    XmlNode roleUsageListNode = resultNode.FirstChild("RoleUsageList");
    if(!roleUsageListNode.IsNull())
    {
      XmlNode roleUsageListMember = roleUsageListNode.FirstChild("member");
      while(!roleUsageListMember.IsNull())
      {
        m_roleUsageList.push_back(roleUsageListMember);
        roleUsageListMember = roleUsageListMember.NextNode("member");
      }

      m_roleUsageListHasBeenSet = true;
    }
  }

  return *this;
}

void DeletionTaskFailureReasonType::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_reasonHasBeenSet)
  {
      oStream << location << index << locationValue << ".Reason=" << StringUtils::URLEncode(m_reason.c_str()) << "&";
  }

  if(m_roleUsageListHasBeenSet)
  {
      unsigned roleUsageListIdx = 1;
      for(auto& item : m_roleUsageList)
      {
        Aws::StringStream roleUsageListSs;
        roleUsageListSs << location << index << locationValue << ".RoleUsageList.member." << roleUsageListIdx++;
        item.OutputToStream(oStream, roleUsageListSs.str().c_str());
      }
  }

}

void DeletionTaskFailureReasonType::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_reasonHasBeenSet)
  {
      oStream << location << ".Reason=" << StringUtils::URLEncode(m_reason.c_str()) << "&";
  }
  if(m_roleUsageListHasBeenSet)
  {
      unsigned roleUsageListIdx = 1;
      for(auto& item : m_roleUsageList)
      {
        Aws::StringStream roleUsageListSs;
        roleUsageListSs << location <<  ".RoleUsageList.member." << roleUsageListIdx++;
        item.OutputToStream(oStream, roleUsageListSs.str().c_str());
      }
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
