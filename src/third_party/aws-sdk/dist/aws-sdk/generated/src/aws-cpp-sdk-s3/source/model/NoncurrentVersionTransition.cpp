/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/NoncurrentVersionTransition.h>
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

NoncurrentVersionTransition::NoncurrentVersionTransition() : 
    m_noncurrentDays(0),
    m_noncurrentDaysHasBeenSet(false),
    m_storageClass(TransitionStorageClass::NOT_SET),
    m_storageClassHasBeenSet(false),
    m_newerNoncurrentVersions(0),
    m_newerNoncurrentVersionsHasBeenSet(false)
{
}

NoncurrentVersionTransition::NoncurrentVersionTransition(const XmlNode& xmlNode)
  : NoncurrentVersionTransition()
{
  *this = xmlNode;
}

NoncurrentVersionTransition& NoncurrentVersionTransition::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode noncurrentDaysNode = resultNode.FirstChild("NoncurrentDays");
    if(!noncurrentDaysNode.IsNull())
    {
      m_noncurrentDays = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(noncurrentDaysNode.GetText()).c_str()).c_str());
      m_noncurrentDaysHasBeenSet = true;
    }
    XmlNode storageClassNode = resultNode.FirstChild("StorageClass");
    if(!storageClassNode.IsNull())
    {
      m_storageClass = TransitionStorageClassMapper::GetTransitionStorageClassForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(storageClassNode.GetText()).c_str()).c_str());
      m_storageClassHasBeenSet = true;
    }
    XmlNode newerNoncurrentVersionsNode = resultNode.FirstChild("NewerNoncurrentVersions");
    if(!newerNoncurrentVersionsNode.IsNull())
    {
      m_newerNoncurrentVersions = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(newerNoncurrentVersionsNode.GetText()).c_str()).c_str());
      m_newerNoncurrentVersionsHasBeenSet = true;
    }
  }

  return *this;
}

void NoncurrentVersionTransition::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_noncurrentDaysHasBeenSet)
  {
   XmlNode noncurrentDaysNode = parentNode.CreateChildElement("NoncurrentDays");
   ss << m_noncurrentDays;
   noncurrentDaysNode.SetText(ss.str());
   ss.str("");
  }

  if(m_storageClassHasBeenSet)
  {
   XmlNode storageClassNode = parentNode.CreateChildElement("StorageClass");
   storageClassNode.SetText(TransitionStorageClassMapper::GetNameForTransitionStorageClass(m_storageClass));
  }

  if(m_newerNoncurrentVersionsHasBeenSet)
  {
   XmlNode newerNoncurrentVersionsNode = parentNode.CreateChildElement("NewerNoncurrentVersions");
   ss << m_newerNoncurrentVersions;
   newerNoncurrentVersionsNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
