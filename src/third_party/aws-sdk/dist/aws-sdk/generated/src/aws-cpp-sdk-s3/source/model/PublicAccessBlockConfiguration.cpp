/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/PublicAccessBlockConfiguration.h>
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

PublicAccessBlockConfiguration::PublicAccessBlockConfiguration() : 
    m_blockPublicAcls(false),
    m_blockPublicAclsHasBeenSet(false),
    m_ignorePublicAcls(false),
    m_ignorePublicAclsHasBeenSet(false),
    m_blockPublicPolicy(false),
    m_blockPublicPolicyHasBeenSet(false),
    m_restrictPublicBuckets(false),
    m_restrictPublicBucketsHasBeenSet(false)
{
}

PublicAccessBlockConfiguration::PublicAccessBlockConfiguration(const XmlNode& xmlNode)
  : PublicAccessBlockConfiguration()
{
  *this = xmlNode;
}

PublicAccessBlockConfiguration& PublicAccessBlockConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode blockPublicAclsNode = resultNode.FirstChild("BlockPublicAcls");
    if(!blockPublicAclsNode.IsNull())
    {
      m_blockPublicAcls = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(blockPublicAclsNode.GetText()).c_str()).c_str());
      m_blockPublicAclsHasBeenSet = true;
    }
    XmlNode ignorePublicAclsNode = resultNode.FirstChild("IgnorePublicAcls");
    if(!ignorePublicAclsNode.IsNull())
    {
      m_ignorePublicAcls = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(ignorePublicAclsNode.GetText()).c_str()).c_str());
      m_ignorePublicAclsHasBeenSet = true;
    }
    XmlNode blockPublicPolicyNode = resultNode.FirstChild("BlockPublicPolicy");
    if(!blockPublicPolicyNode.IsNull())
    {
      m_blockPublicPolicy = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(blockPublicPolicyNode.GetText()).c_str()).c_str());
      m_blockPublicPolicyHasBeenSet = true;
    }
    XmlNode restrictPublicBucketsNode = resultNode.FirstChild("RestrictPublicBuckets");
    if(!restrictPublicBucketsNode.IsNull())
    {
      m_restrictPublicBuckets = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(restrictPublicBucketsNode.GetText()).c_str()).c_str());
      m_restrictPublicBucketsHasBeenSet = true;
    }
  }

  return *this;
}

void PublicAccessBlockConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_blockPublicAclsHasBeenSet)
  {
   XmlNode blockPublicAclsNode = parentNode.CreateChildElement("BlockPublicAcls");
   ss << std::boolalpha << m_blockPublicAcls;
   blockPublicAclsNode.SetText(ss.str());
   ss.str("");
  }

  if(m_ignorePublicAclsHasBeenSet)
  {
   XmlNode ignorePublicAclsNode = parentNode.CreateChildElement("IgnorePublicAcls");
   ss << std::boolalpha << m_ignorePublicAcls;
   ignorePublicAclsNode.SetText(ss.str());
   ss.str("");
  }

  if(m_blockPublicPolicyHasBeenSet)
  {
   XmlNode blockPublicPolicyNode = parentNode.CreateChildElement("BlockPublicPolicy");
   ss << std::boolalpha << m_blockPublicPolicy;
   blockPublicPolicyNode.SetText(ss.str());
   ss.str("");
  }

  if(m_restrictPublicBucketsHasBeenSet)
  {
   XmlNode restrictPublicBucketsNode = parentNode.CreateChildElement("RestrictPublicBuckets");
   ss << std::boolalpha << m_restrictPublicBuckets;
   restrictPublicBucketsNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
