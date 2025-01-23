/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/S3TablesDestination.h>
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

S3TablesDestination::S3TablesDestination() : 
    m_tableBucketArnHasBeenSet(false),
    m_tableNameHasBeenSet(false)
{
}

S3TablesDestination::S3TablesDestination(const XmlNode& xmlNode)
  : S3TablesDestination()
{
  *this = xmlNode;
}

S3TablesDestination& S3TablesDestination::operator =(const XmlNode& xmlNode)
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
  }

  return *this;
}

void S3TablesDestination::AddToNode(XmlNode& parentNode) const
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

}

} // namespace Model
} // namespace S3
} // namespace Aws
