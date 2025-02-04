/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ContextEntry.h>
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

ContextEntry::ContextEntry() : 
    m_contextKeyNameHasBeenSet(false),
    m_contextKeyValuesHasBeenSet(false),
    m_contextKeyType(ContextKeyTypeEnum::NOT_SET),
    m_contextKeyTypeHasBeenSet(false)
{
}

ContextEntry::ContextEntry(const XmlNode& xmlNode)
  : ContextEntry()
{
  *this = xmlNode;
}

ContextEntry& ContextEntry::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode contextKeyNameNode = resultNode.FirstChild("ContextKeyName");
    if(!contextKeyNameNode.IsNull())
    {
      m_contextKeyName = Aws::Utils::Xml::DecodeEscapedXmlText(contextKeyNameNode.GetText());
      m_contextKeyNameHasBeenSet = true;
    }
    XmlNode contextKeyValuesNode = resultNode.FirstChild("ContextKeyValues");
    if(!contextKeyValuesNode.IsNull())
    {
      XmlNode contextKeyValuesMember = contextKeyValuesNode.FirstChild("member");
      while(!contextKeyValuesMember.IsNull())
      {
        m_contextKeyValues.push_back(contextKeyValuesMember.GetText());
        contextKeyValuesMember = contextKeyValuesMember.NextNode("member");
      }

      m_contextKeyValuesHasBeenSet = true;
    }
    XmlNode contextKeyTypeNode = resultNode.FirstChild("ContextKeyType");
    if(!contextKeyTypeNode.IsNull())
    {
      m_contextKeyType = ContextKeyTypeEnumMapper::GetContextKeyTypeEnumForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(contextKeyTypeNode.GetText()).c_str()).c_str());
      m_contextKeyTypeHasBeenSet = true;
    }
  }

  return *this;
}

void ContextEntry::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_contextKeyNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".ContextKeyName=" << StringUtils::URLEncode(m_contextKeyName.c_str()) << "&";
  }

  if(m_contextKeyValuesHasBeenSet)
  {
      unsigned contextKeyValuesIdx = 1;
      for(auto& item : m_contextKeyValues)
      {
        oStream << location << index << locationValue << ".ContextKeyValues.member." << contextKeyValuesIdx++ << "=" << StringUtils::URLEncode(item.c_str()) << "&";
      }
  }

  if(m_contextKeyTypeHasBeenSet)
  {
      oStream << location << index << locationValue << ".ContextKeyType=" << ContextKeyTypeEnumMapper::GetNameForContextKeyTypeEnum(m_contextKeyType) << "&";
  }

}

void ContextEntry::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_contextKeyNameHasBeenSet)
  {
      oStream << location << ".ContextKeyName=" << StringUtils::URLEncode(m_contextKeyName.c_str()) << "&";
  }
  if(m_contextKeyValuesHasBeenSet)
  {
      unsigned contextKeyValuesIdx = 1;
      for(auto& item : m_contextKeyValues)
      {
        oStream << location << ".ContextKeyValues.member." << contextKeyValuesIdx++ << "=" << StringUtils::URLEncode(item.c_str()) << "&";
      }
  }
  if(m_contextKeyTypeHasBeenSet)
  {
      oStream << location << ".ContextKeyType=" << ContextKeyTypeEnumMapper::GetNameForContextKeyTypeEnum(m_contextKeyType) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
