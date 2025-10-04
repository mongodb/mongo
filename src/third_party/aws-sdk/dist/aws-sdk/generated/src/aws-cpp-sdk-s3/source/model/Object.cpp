/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Object.h>
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

Object::Object() : 
    m_keyHasBeenSet(false),
    m_lastModifiedHasBeenSet(false),
    m_eTagHasBeenSet(false),
    m_checksumAlgorithmHasBeenSet(false),
    m_size(0),
    m_sizeHasBeenSet(false),
    m_storageClass(ObjectStorageClass::NOT_SET),
    m_storageClassHasBeenSet(false),
    m_ownerHasBeenSet(false),
    m_restoreStatusHasBeenSet(false)
{
}

Object::Object(const XmlNode& xmlNode)
  : Object()
{
  *this = xmlNode;
}

Object& Object::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode keyNode = resultNode.FirstChild("Key");
    if(!keyNode.IsNull())
    {
      m_key = Aws::Utils::Xml::DecodeEscapedXmlText(keyNode.GetText());
      m_keyHasBeenSet = true;
    }
    XmlNode lastModifiedNode = resultNode.FirstChild("LastModified");
    if(!lastModifiedNode.IsNull())
    {
      m_lastModified = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(lastModifiedNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_lastModifiedHasBeenSet = true;
    }
    XmlNode eTagNode = resultNode.FirstChild("ETag");
    if(!eTagNode.IsNull())
    {
      m_eTag = Aws::Utils::Xml::DecodeEscapedXmlText(eTagNode.GetText());
      m_eTagHasBeenSet = true;
    }
    XmlNode checksumAlgorithmNode = resultNode.FirstChild("ChecksumAlgorithm");
    if(!checksumAlgorithmNode.IsNull())
    {
      XmlNode checksumAlgorithmMember = checksumAlgorithmNode;
      while(!checksumAlgorithmMember.IsNull())
      {
        m_checksumAlgorithm.push_back(ChecksumAlgorithmMapper::GetChecksumAlgorithmForName(StringUtils::Trim(checksumAlgorithmMember.GetText().c_str())));
        checksumAlgorithmMember = checksumAlgorithmMember.NextNode("ChecksumAlgorithm");
      }

      m_checksumAlgorithmHasBeenSet = true;
    }
    XmlNode sizeNode = resultNode.FirstChild("Size");
    if(!sizeNode.IsNull())
    {
      m_size = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(sizeNode.GetText()).c_str()).c_str());
      m_sizeHasBeenSet = true;
    }
    XmlNode storageClassNode = resultNode.FirstChild("StorageClass");
    if(!storageClassNode.IsNull())
    {
      m_storageClass = ObjectStorageClassMapper::GetObjectStorageClassForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(storageClassNode.GetText()).c_str()).c_str());
      m_storageClassHasBeenSet = true;
    }
    XmlNode ownerNode = resultNode.FirstChild("Owner");
    if(!ownerNode.IsNull())
    {
      m_owner = ownerNode;
      m_ownerHasBeenSet = true;
    }
    XmlNode restoreStatusNode = resultNode.FirstChild("RestoreStatus");
    if(!restoreStatusNode.IsNull())
    {
      m_restoreStatus = restoreStatusNode;
      m_restoreStatusHasBeenSet = true;
    }
  }

  return *this;
}

void Object::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_keyHasBeenSet)
  {
   XmlNode keyNode = parentNode.CreateChildElement("Key");
   keyNode.SetText(m_key);
  }

  if(m_lastModifiedHasBeenSet)
  {
   XmlNode lastModifiedNode = parentNode.CreateChildElement("LastModified");
   lastModifiedNode.SetText(m_lastModified.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

  if(m_eTagHasBeenSet)
  {
   XmlNode eTagNode = parentNode.CreateChildElement("ETag");
   eTagNode.SetText(m_eTag);
  }

  if(m_checksumAlgorithmHasBeenSet)
  {
   XmlNode checksumAlgorithmParentNode = parentNode.CreateChildElement("ChecksumAlgorithm");
   for(const auto& item : m_checksumAlgorithm)
   {
     XmlNode checksumAlgorithmNode = checksumAlgorithmParentNode.CreateChildElement("ChecksumAlgorithm");
     checksumAlgorithmNode.SetText(ChecksumAlgorithmMapper::GetNameForChecksumAlgorithm(item));
   }
  }

  if(m_sizeHasBeenSet)
  {
   XmlNode sizeNode = parentNode.CreateChildElement("Size");
   ss << m_size;
   sizeNode.SetText(ss.str());
   ss.str("");
  }

  if(m_storageClassHasBeenSet)
  {
   XmlNode storageClassNode = parentNode.CreateChildElement("StorageClass");
   storageClassNode.SetText(ObjectStorageClassMapper::GetNameForObjectStorageClass(m_storageClass));
  }

  if(m_ownerHasBeenSet)
  {
   XmlNode ownerNode = parentNode.CreateChildElement("Owner");
   m_owner.AddToNode(ownerNode);
  }

  if(m_restoreStatusHasBeenSet)
  {
   XmlNode restoreStatusNode = parentNode.CreateChildElement("RestoreStatus");
   m_restoreStatus.AddToNode(restoreStatusNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
