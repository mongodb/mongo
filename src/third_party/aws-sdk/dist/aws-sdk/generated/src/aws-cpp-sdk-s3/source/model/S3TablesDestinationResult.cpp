/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/S3TablesDestinationResult.h>
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

S3TablesDestinationResult::S3TablesDestinationResult() : 
    m_tableBucketArnHasBeenSet(false),
    m_tableNameHasBeenSet(false),
    m_tableArnHasBeenSet(false),
    m_tableNamespaceHasBeenSet(false)
{
}

S3TablesDestinationResult::S3TablesDestinationResult(const XmlNode& xmlNode)
  : S3TablesDestinationResult()
{
  *this = xmlNode;
}

S3TablesDestinationResult& S3TablesDestinationResult::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode tableBucketArnNode = resultNode.FirstChild("TableBucketArn");
    if(!tableBucketArnNode.IsNull())
    {
      m_tableBucketArn = Aws::Utils::Xml::DecodeEscapedXmlText(tableBucketArnNode.GetText());
      m_tableBucketArnHasBeenSet = true;
    }
    XmlNode tableNameNode = resultNode.FirstChild("TableName");
    if(!tableNameNode.IsNull())
    {
      m_tableName = Aws::Utils::Xml::DecodeEscapedXmlText(tableNameNode.GetText());
      m_tableNameHasBeenSet = true;
    }
    XmlNode tableArnNode = resultNode.FirstChild("TableArn");
    if(!tableArnNode.IsNull())
    {
      m_tableArn = Aws::Utils::Xml::DecodeEscapedXmlText(tableArnNode.GetText());
      m_tableArnHasBeenSet = true;
    }
    XmlNode tableNamespaceNode = resultNode.FirstChild("TableNamespace");
    if(!tableNamespaceNode.IsNull())
    {
      m_tableNamespace = Aws::Utils::Xml::DecodeEscapedXmlText(tableNamespaceNode.GetText());
      m_tableNamespaceHasBeenSet = true;
    }
  }

  return *this;
}

void S3TablesDestinationResult::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_tableBucketArnHasBeenSet)
  {
   XmlNode tableBucketArnNode = parentNode.CreateChildElement("TableBucketArn");
   tableBucketArnNode.SetText(m_tableBucketArn);
  }

  if(m_tableNameHasBeenSet)
  {
   XmlNode tableNameNode = parentNode.CreateChildElement("TableName");
   tableNameNode.SetText(m_tableName);
  }

  if(m_tableArnHasBeenSet)
  {
   XmlNode tableArnNode = parentNode.CreateChildElement("TableArn");
   tableArnNode.SetText(m_tableArn);
  }

  if(m_tableNamespaceHasBeenSet)
  {
   XmlNode tableNamespaceNode = parentNode.CreateChildElement("TableNamespace");
   tableNamespaceNode.SetText(m_tableNamespace);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
