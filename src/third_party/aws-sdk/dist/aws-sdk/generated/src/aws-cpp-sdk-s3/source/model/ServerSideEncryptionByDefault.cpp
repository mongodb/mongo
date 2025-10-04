/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ServerSideEncryptionByDefault.h>
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

ServerSideEncryptionByDefault::ServerSideEncryptionByDefault() : 
    m_sSEAlgorithm(ServerSideEncryption::NOT_SET),
    m_sSEAlgorithmHasBeenSet(false),
    m_kMSMasterKeyIDHasBeenSet(false)
{
}

ServerSideEncryptionByDefault::ServerSideEncryptionByDefault(const XmlNode& xmlNode)
  : ServerSideEncryptionByDefault()
{
  *this = xmlNode;
}

ServerSideEncryptionByDefault& ServerSideEncryptionByDefault::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode sSEAlgorithmNode = resultNode.FirstChild("SSEAlgorithm");
    if(!sSEAlgorithmNode.IsNull())
    {
      m_sSEAlgorithm = ServerSideEncryptionMapper::GetServerSideEncryptionForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(sSEAlgorithmNode.GetText()).c_str()).c_str());
      m_sSEAlgorithmHasBeenSet = true;
    }
    XmlNode kMSMasterKeyIDNode = resultNode.FirstChild("KMSMasterKeyID");
    if(!kMSMasterKeyIDNode.IsNull())
    {
      m_kMSMasterKeyID = Aws::Utils::Xml::DecodeEscapedXmlText(kMSMasterKeyIDNode.GetText());
      m_kMSMasterKeyIDHasBeenSet = true;
    }
  }

  return *this;
}

void ServerSideEncryptionByDefault::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_sSEAlgorithmHasBeenSet)
  {
   XmlNode sSEAlgorithmNode = parentNode.CreateChildElement("SSEAlgorithm");
   sSEAlgorithmNode.SetText(ServerSideEncryptionMapper::GetNameForServerSideEncryption(m_sSEAlgorithm));
  }

  if(m_kMSMasterKeyIDHasBeenSet)
  {
   XmlNode kMSMasterKeyIDNode = parentNode.CreateChildElement("KMSMasterKeyID");
   kMSMasterKeyIDNode.SetText(m_kMSMasterKeyID);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
