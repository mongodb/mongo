/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Destination.h>
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

Destination::Destination() : 
    m_bucketHasBeenSet(false),
    m_accountHasBeenSet(false),
    m_storageClass(StorageClass::NOT_SET),
    m_storageClassHasBeenSet(false),
    m_accessControlTranslationHasBeenSet(false),
    m_encryptionConfigurationHasBeenSet(false),
    m_replicationTimeHasBeenSet(false),
    m_metricsHasBeenSet(false)
{
}

Destination::Destination(const XmlNode& xmlNode)
  : Destination()
{
  *this = xmlNode;
}

Destination& Destination::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode bucketNode = resultNode.FirstChild("Bucket");
    if(!bucketNode.IsNull())
    {
      m_bucket = Aws::Utils::Xml::DecodeEscapedXmlText(bucketNode.GetText());
      m_bucketHasBeenSet = true;
    }
    XmlNode accountNode = resultNode.FirstChild("Account");
    if(!accountNode.IsNull())
    {
      m_account = Aws::Utils::Xml::DecodeEscapedXmlText(accountNode.GetText());
      m_accountHasBeenSet = true;
    }
    XmlNode storageClassNode = resultNode.FirstChild("StorageClass");
    if(!storageClassNode.IsNull())
    {
      m_storageClass = StorageClassMapper::GetStorageClassForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(storageClassNode.GetText()).c_str()).c_str());
      m_storageClassHasBeenSet = true;
    }
    XmlNode accessControlTranslationNode = resultNode.FirstChild("AccessControlTranslation");
    if(!accessControlTranslationNode.IsNull())
    {
      m_accessControlTranslation = accessControlTranslationNode;
      m_accessControlTranslationHasBeenSet = true;
    }
    XmlNode encryptionConfigurationNode = resultNode.FirstChild("EncryptionConfiguration");
    if(!encryptionConfigurationNode.IsNull())
    {
      m_encryptionConfiguration = encryptionConfigurationNode;
      m_encryptionConfigurationHasBeenSet = true;
    }
    XmlNode replicationTimeNode = resultNode.FirstChild("ReplicationTime");
    if(!replicationTimeNode.IsNull())
    {
      m_replicationTime = replicationTimeNode;
      m_replicationTimeHasBeenSet = true;
    }
    XmlNode metricsNode = resultNode.FirstChild("Metrics");
    if(!metricsNode.IsNull())
    {
      m_metrics = metricsNode;
      m_metricsHasBeenSet = true;
    }
  }

  return *this;
}

void Destination::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_bucketHasBeenSet)
  {
   XmlNode bucketNode = parentNode.CreateChildElement("Bucket");
   bucketNode.SetText(m_bucket);
  }

  if(m_accountHasBeenSet)
  {
   XmlNode accountNode = parentNode.CreateChildElement("Account");
   accountNode.SetText(m_account);
  }

  if(m_storageClassHasBeenSet)
  {
   XmlNode storageClassNode = parentNode.CreateChildElement("StorageClass");
   storageClassNode.SetText(StorageClassMapper::GetNameForStorageClass(m_storageClass));
  }

  if(m_accessControlTranslationHasBeenSet)
  {
   XmlNode accessControlTranslationNode = parentNode.CreateChildElement("AccessControlTranslation");
   m_accessControlTranslation.AddToNode(accessControlTranslationNode);
  }

  if(m_encryptionConfigurationHasBeenSet)
  {
   XmlNode encryptionConfigurationNode = parentNode.CreateChildElement("EncryptionConfiguration");
   m_encryptionConfiguration.AddToNode(encryptionConfigurationNode);
  }

  if(m_replicationTimeHasBeenSet)
  {
   XmlNode replicationTimeNode = parentNode.CreateChildElement("ReplicationTime");
   m_replicationTime.AddToNode(replicationTimeNode);
  }

  if(m_metricsHasBeenSet)
  {
   XmlNode metricsNode = parentNode.CreateChildElement("Metrics");
   m_metrics.AddToNode(metricsNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
