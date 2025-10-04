/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/S3Location.h>
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

S3Location::S3Location() : 
    m_bucketNameHasBeenSet(false),
    m_prefixHasBeenSet(false),
    m_encryptionHasBeenSet(false),
    m_cannedACL(ObjectCannedACL::NOT_SET),
    m_cannedACLHasBeenSet(false),
    m_accessControlListHasBeenSet(false),
    m_taggingHasBeenSet(false),
    m_userMetadataHasBeenSet(false),
    m_storageClass(StorageClass::NOT_SET),
    m_storageClassHasBeenSet(false)
{
}

S3Location::S3Location(const XmlNode& xmlNode)
  : S3Location()
{
  *this = xmlNode;
}

S3Location& S3Location::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode bucketNameNode = resultNode.FirstChild("BucketName");
    if(!bucketNameNode.IsNull())
    {
      m_bucketName = Aws::Utils::Xml::DecodeEscapedXmlText(bucketNameNode.GetText());
      m_bucketNameHasBeenSet = true;
    }
    XmlNode prefixNode = resultNode.FirstChild("Prefix");
    if(!prefixNode.IsNull())
    {
      m_prefix = Aws::Utils::Xml::DecodeEscapedXmlText(prefixNode.GetText());
      m_prefixHasBeenSet = true;
    }
    XmlNode encryptionNode = resultNode.FirstChild("Encryption");
    if(!encryptionNode.IsNull())
    {
      m_encryption = encryptionNode;
      m_encryptionHasBeenSet = true;
    }
    XmlNode cannedACLNode = resultNode.FirstChild("CannedACL");
    if(!cannedACLNode.IsNull())
    {
      m_cannedACL = ObjectCannedACLMapper::GetObjectCannedACLForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(cannedACLNode.GetText()).c_str()).c_str());
      m_cannedACLHasBeenSet = true;
    }
    XmlNode accessControlListNode = resultNode.FirstChild("AccessControlList");
    if(!accessControlListNode.IsNull())
    {
      XmlNode accessControlListMember = accessControlListNode.FirstChild("Grant");
      while(!accessControlListMember.IsNull())
      {
        m_accessControlList.push_back(accessControlListMember);
        accessControlListMember = accessControlListMember.NextNode("Grant");
      }

      m_accessControlListHasBeenSet = true;
    }
    XmlNode taggingNode = resultNode.FirstChild("Tagging");
    if(!taggingNode.IsNull())
    {
      m_tagging = taggingNode;
      m_taggingHasBeenSet = true;
    }
    XmlNode userMetadataNode = resultNode.FirstChild("UserMetadata");
    if(!userMetadataNode.IsNull())
    {
      XmlNode userMetadataMember = userMetadataNode.FirstChild("MetadataEntry");
      while(!userMetadataMember.IsNull())
      {
        m_userMetadata.push_back(userMetadataMember);
        userMetadataMember = userMetadataMember.NextNode("MetadataEntry");
      }

      m_userMetadataHasBeenSet = true;
    }
    XmlNode storageClassNode = resultNode.FirstChild("StorageClass");
    if(!storageClassNode.IsNull())
    {
      m_storageClass = StorageClassMapper::GetStorageClassForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(storageClassNode.GetText()).c_str()).c_str());
      m_storageClassHasBeenSet = true;
    }
  }

  return *this;
}

void S3Location::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_bucketNameHasBeenSet)
  {
   XmlNode bucketNameNode = parentNode.CreateChildElement("BucketName");
   bucketNameNode.SetText(m_bucketName);
  }

  if(m_prefixHasBeenSet)
  {
   XmlNode prefixNode = parentNode.CreateChildElement("Prefix");
   prefixNode.SetText(m_prefix);
  }

  if(m_encryptionHasBeenSet)
  {
   XmlNode encryptionNode = parentNode.CreateChildElement("Encryption");
   m_encryption.AddToNode(encryptionNode);
  }

  if(m_cannedACLHasBeenSet)
  {
   XmlNode cannedACLNode = parentNode.CreateChildElement("CannedACL");
   cannedACLNode.SetText(ObjectCannedACLMapper::GetNameForObjectCannedACL(m_cannedACL));
  }

  if(m_accessControlListHasBeenSet)
  {
   XmlNode accessControlListParentNode = parentNode.CreateChildElement("AccessControlList");
   for(const auto& item : m_accessControlList)
   {
     XmlNode accessControlListNode = accessControlListParentNode.CreateChildElement("Grant");
     item.AddToNode(accessControlListNode);
   }
  }

  if(m_taggingHasBeenSet)
  {
   XmlNode taggingNode = parentNode.CreateChildElement("Tagging");
   m_tagging.AddToNode(taggingNode);
  }

  if(m_userMetadataHasBeenSet)
  {
   XmlNode userMetadataParentNode = parentNode.CreateChildElement("UserMetadata");
   for(const auto& item : m_userMetadata)
   {
     XmlNode userMetadataNode = userMetadataParentNode.CreateChildElement("MetadataEntry");
     item.AddToNode(userMetadataNode);
   }
  }

  if(m_storageClassHasBeenSet)
  {
   XmlNode storageClassNode = parentNode.CreateChildElement("StorageClass");
   storageClassNode.SetText(StorageClassMapper::GetNameForStorageClass(m_storageClass));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
